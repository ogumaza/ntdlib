// SPDX-License-Identifier: MIT

#include "ntdlib/sdat/files.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/sound.h"
#include "ntdlib/rounding.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/wave/encode_ima.h"
#include "ntdlib/wave/pcm.h"
#include "sdat/writer.h"

namespace ntdlib::sdat
{

namespace
{

using detail::Writer;

constexpr uint16_t kByteOrderMark = 0xFEFF;
constexpr uint16_t kVersion = 0x0100;
constexpr std::size_t kHeaderBytes = 0x10;
constexpr std::size_t kTableStart = 0x3C;   // a bank's or a wave archive's table: after 32 bytes that a game fills in
constexpr std::size_t kSplitKeys = 8;       // a key split's keys
constexpr uint32_t kMaxOffset = 0xFFFFFF;   // the 24 bits of a bank record's offset
constexpr uint32_t kMaxLoopStart = 0xFFFF;  // the 16 bits of a wave's loop start
constexpr uint32_t kMaxTimer = 0xFFFF;      // the 16 bits of a wave's timer
constexpr uint32_t kStreamHeadBytes = 0x50; // a stream's HEAD block
constexpr uint32_t kBlockHeaderBytes = 8;   // a block's magic and size
constexpr uint32_t kImaStateBytes = 4;      // the decoder's state before a part's nibbles of IMA-ADPCM
constexpr uint16_t kLeastStreamRate = 256;  // the lowest rate of a stream: that of a wave

// Starts a file: its header, then the DATA block's header. FinishFile fills in the sizes.
Writer StartFile(const char* magic)
{
    Writer w;
    w.Magic(magic);
    w.U16(kByteOrderMark);
    w.U16(kVersion);
    w.U32(0);
    w.U16(static_cast<uint16_t>(kHeaderBytes));
    w.U16(1);
    w.Magic("DATA");
    w.U32(0);

    return w;
}

// Ends a file on a multiple of 4 bytes, and gives the file's and the block's sizes.
std::vector<uint8_t> FinishFile(Writer& w)
{
    w.PadTo(AlignUp(w.Size(), 4));
    w.Put32(8, w.Size());
    w.Put32(kHeaderBytes + 4, w.Size() - kHeaderBytes);

    return w.Take();
}

// A region's parameters (SNDInstParam).
void WriteRegion(Writer& w, const Region& region)
{
    w.U16(region.wave);
    w.U16(region.wave_archive);
    w.U8(region.root_key);
    w.U8(region.attack);
    w.U8(region.decay);
    w.U8(region.sustain);
    w.U8(region.release);
    w.U8(region.pan);
}

// A region of a drum set or a key split: its type, a byte of padding and its parameters.
void WriteTypedRegion(Writer& w, const Region& region)
{
    w.U8(static_cast<uint8_t>(region.type));
    w.U8(region.padding);
    WriteRegion(w, region);
}

// An instrument's data in the layout that Bank::Parse reads.
std::vector<uint8_t> InstrumentData(const Instrument& instrument)
{
    Writer w;
    switch (instrument.type)
    {
    case InstrumentType::kNone:
        break;

    case InstrumentType::kDrumSet:
        {
            if (instrument.max_key < instrument.min_key ||
                instrument.regions.size() != std::size_t{instrument.max_key} - instrument.min_key + 1)
            {
                throw std::invalid_argument("a drum set's regions don't cover its keys");
            }
            w.U8(instrument.min_key);
            w.U8(instrument.max_key);
            for (const Region& region : instrument.regions)
            {
                WriteTypedRegion(w, region);
            }
            break;
        }

    case InstrumentType::kKeySplit:
        {
            const auto keys =
                static_cast<std::size_t>(std::ranges::find(instrument.split_keys, 0) - instrument.split_keys.begin());
            if (instrument.regions.size() != keys)
            {
                throw std::invalid_argument("a key split's regions don't match its keys");
            }
            for (std::size_t k = 0; k < kSplitKeys; k++)
            {
                w.U8(instrument.split_keys[k]);
            }
            for (const Region& region : instrument.regions)
            {
                WriteTypedRegion(w, region);
            }
            break;
        }

    default:
        WriteRegion(w, instrument.region);
        break;
    }

    return w.Take();
}

// `count` samples: `samples`, then the samples from `loop_start` to the end again, as many times as `count` requires.
std::vector<int16_t> Repeated(std::span<const int16_t> samples, std::size_t loop_start, std::size_t count)
{
    const std::size_t length = samples.size() - loop_start;
    std::vector<int16_t> out(count);
    for (std::size_t i = 0; i < out.size(); i++)
    {
        out[i] = i < samples.size() ? samples[i] : samples[loop_start + (i - loop_start) % length];
    }

    return out;
}

// `samples` laid out in whole words of `per_word` samples (see EncodeWave). Gives the loop's start in samples.
std::vector<int16_t> InWords(std::span<const int16_t> samples, uint32_t per_word, std::optional<uint32_t>& loop_start)
{
    if (!loop_start)
    {
        std::vector<int16_t> out(samples.begin(), samples.end());
        out.resize(AlignUp(out.size(), per_word), 0);
        return out;
    }

    // The loop moves forward to a word's start, and repeats until its length is a whole number of words.
    const std::size_t old_start = *loop_start;
    const std::size_t length = samples.size() - old_start;
    const std::size_t start = AlignUp(old_start, per_word);
    const std::size_t passes = per_word / std::gcd(length, std::size_t{per_word});
    std::vector<int16_t> out = Repeated(samples, old_start, start + passes * length);
    loop_start = static_cast<uint32_t>(start);

    return out;
}

// The bytes of a part of `samples` of 16-bit PCM or PCM8.
void AppendPcm(std::vector<uint8_t>& part, std::span<const int16_t> samples, WaveEncoding encoding)
{
    for (const int16_t sample : samples)
    {
        if (encoding == WaveEncoding::kPcm16)
        {
            xsflib::AppendLe16(part, static_cast<uint16_t>(sample));
        }
        else
        {
            part.push_back(wave::EncodePcm8(sample));
        }
    }
}

// Each block's part of `samples`, one channel of a stream.
std::vector<std::vector<uint8_t>> PartsOf(std::span<const int16_t> samples, WaveEncoding encoding,
                                          uint32_t block_samples)
{
    std::vector<std::vector<uint8_t>> parts;
    if (encoding != WaveEncoding::kImaAdpcm)
    {
        for (std::size_t first = 0; first < samples.size(); first += block_samples)
        {
            AppendPcm(parts.emplace_back(),
                      samples.subspan(first, std::min<std::size_t>(block_samples, samples.size() - first)), encoding);
        }

        return parts;
    }

    // The whole channel's encoding. A block's part starts with the state where the nibbles of the channel reach it.
    const wave::ImaAdpcmChannel channel = wave::EncodeImaAdpcm(samples, 0, ntr::AdpcmStep);
    wave::ImaAdpcmState state = channel.start;
    for (std::size_t first = 0; first < samples.size(); first += block_samples)
    {
        const std::size_t count = std::min<std::size_t>(block_samples, samples.size() - first);
        std::vector<uint8_t>& part = parts.emplace_back();
        const auto sample = static_cast<uint16_t>(state.sample);
        part.insert(part.end(),
                    {static_cast<uint8_t>(sample & 0xFF), static_cast<uint8_t>(sample >> 8), state.index, 0});
        const auto nibbles = std::span(channel.data).subspan(first / 2, wave::ImaAdpcmBytes(count));
        part.insert(part.end(), nibbles.begin(), nibbles.end());
        for (std::size_t i = first; i < first + count; i++)
        {
            const uint8_t byte = channel.data[i / 2];
            state = ntr::AdpcmStep(state, static_cast<uint8_t>(i % 2 == 0 ? byte & 0x0F : byte >> 4));
        }
    }

    return parts;
}

} // namespace

uint32_t SamplesPerWord(WaveEncoding encoding)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return 4;
    case WaveEncoding::kPcm16:
        return 2;
    case WaveEncoding::kImaAdpcm:
        return 8;
    }

    throw std::invalid_argument("unknown wave encoding");
}

std::vector<uint8_t> SequenceFile(std::span<const uint8_t> bytecode)
{
    Writer w = StartFile("SSEQ");
    w.U32(static_cast<uint32_t>(w.Size() + 4));
    w.Bytes(bytecode);

    return FinishFile(w);
}

std::vector<uint8_t> SequenceArchiveFile(std::span<const ArchivedSequence> sequences, std::span<const uint8_t> bytecode)
{
    Writer w = StartFile("SSAR");
    w.U32(static_cast<uint32_t>(w.Size() + 8 + 12 * sequences.size()));
    w.U32(static_cast<uint32_t>(sequences.size()));
    for (const ArchivedSequence& s : sequences)
    {
        w.U32(s.offset);
        w.U16(s.bank);
        w.U8(s.volume);
        w.U8(s.channel_priority);
        w.U8(s.player_priority);
        w.U8(s.player);
        w.U16(s.reserved);
    }
    w.Bytes(bytecode);

    return FinishFile(w);
}

std::vector<uint8_t> BankFile(const Bank& bank)
{
    const std::vector<Instrument>& instruments = bank.instruments;
    std::vector<std::vector<uint8_t>> data;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        data.push_back(InstrumentData(instruments[i]));
        if (instruments[i].type != InstrumentType::kNone)
        {
            order.push_back(i);
        }
    }

    // The data in the order of the offsets, the new instruments last.
    const auto place = [&instruments](std::size_t i)
    {
        return instruments[i].offset == 0 ? std::numeric_limits<uint32_t>::max() : instruments[i].offset;
    };
    std::ranges::stable_sort(order, {}, place);

    // The table, then the data. An instrument shares the data of an earlier one with its offset and its bytes.
    Writer w = StartFile("SBNK");
    w.PadTo(kTableStart - 4);
    w.U32(static_cast<uint32_t>(instruments.size()));
    w.PadTo(kTableStart + 4 * instruments.size());
    std::vector<std::size_t> written(instruments.size());
    for (std::size_t n = 0; n < order.size(); n++)
    {
        const std::size_t i = order[n];
        std::optional<std::size_t> shared;
        for (std::size_t m = 0; m < n && !shared; m++)
        {
            const std::size_t j = order[m];
            if (instruments[j].offset != 0 && instruments[j].offset == instruments[i].offset && data[j] == data[i])
            {
                shared = j;
            }
        }
        if (shared)
        {
            written[i] = written[*shared];
        }
        else
        {
            written[i] = w.Size();
            w.Bytes(data[i]);
        }
        if (written[i] > kMaxOffset)
        {
            throw std::invalid_argument("a bank's data lies past the 24 bits of an offset");
        }
        w.Put32(kTableStart + 4 * i, static_cast<uint32_t>(instruments[i].type) | written[i] << 8);
    }

    return FinishFile(w);
}

std::vector<uint8_t> WaveBytes(const Wave& wave)
{
    Writer w;
    w.U8(static_cast<uint8_t>(wave.encoding));
    w.U8(wave.loop);
    w.U16(wave.sample_rate);
    w.U16(wave.timer);
    w.U16(wave.loop_start);
    w.U32(wave.loop_length);
    w.Bytes(wave.data);

    return w.Take();
}

std::vector<uint8_t> WaveFile(std::span<const uint8_t> wave)
{
    Writer w = StartFile("SWAV");
    w.Bytes(wave);

    return FinishFile(w);
}

std::vector<uint8_t> WaveArchiveFile(std::span<const std::vector<uint8_t>> waves)
{
    Writer w = StartFile("SWAR");
    w.PadTo(kTableStart - 4);
    w.U32(static_cast<uint32_t>(waves.size()));
    w.PadTo(kTableStart + 4 * waves.size());
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        w.Put32(kTableStart + 4 * i, w.Size());
        w.Bytes(waves[i]);
        w.PadTo(AlignUp(w.Size(), 4));
    }

    return FinishFile(w);
}

std::vector<uint8_t> EncodeWave(std::span<const int16_t> samples, uint16_t sample_rate, WaveEncoding encoding,
                                std::optional<uint32_t> loop_start)
{
    const uint32_t per_word = SamplesPerWord(encoding);
    if (sample_rate == 0 || kTimerClockHz / sample_rate > kMaxTimer)
    {
        throw std::invalid_argument("a wave's sample rate is below 256 Hz");
    }
    if (loop_start && *loop_start >= samples.size())
    {
        throw std::invalid_argument("a wave's loop starts at or past its end");
    }

    // The samples in whole words. An IMA-ADPCM wave's first word holds the decoder's state.
    const uint32_t header_words = HeaderWords(encoding);
    if (loop_start && header_words + AlignUp(std::size_t{*loop_start}, per_word) / per_word > kMaxLoopStart)
    {
        throw std::invalid_argument("a wave's loop starts past the 65,535 words of its field");
    }
    const std::vector<int16_t> laid_out = InWords(samples, per_word, loop_start);
    if (laid_out.size() / per_word + header_words > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("too many samples for a wave");
    }
    const uint32_t start_words = header_words + loop_start.value_or(0) / per_word;
    const auto words = static_cast<uint32_t>(header_words + laid_out.size() / per_word);

    Writer w;
    w.U8(static_cast<uint8_t>(encoding));
    w.U8(loop_start ? 1 : 0);
    w.U16(sample_rate);
    w.U16(static_cast<uint16_t>(kTimerClockHz / sample_rate));
    w.U16(static_cast<uint16_t>(start_words));
    w.U32(words - start_words);
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        for (const int16_t sample : laid_out)
        {
            w.U8(wave::EncodePcm8(sample));
        }
        break;

    case WaveEncoding::kPcm16:
        for (const int16_t sample : laid_out)
        {
            w.U16(static_cast<uint16_t>(sample));
        }
        break;

    case WaveEncoding::kImaAdpcm:
        {
            const wave::ImaAdpcmChannel channel =
                wave::EncodeImaAdpcm(laid_out, loop_start.value_or(0), ntr::AdpcmStep);
            w.U32(static_cast<uint16_t>(channel.start.sample) | uint32_t{channel.start.index} << 16);
            w.Bytes(channel.data);
            break;
        }
    }

    return w.Take();
}

uint32_t StreamBlockSamples(WaveEncoding encoding, uint32_t block_bytes)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return block_bytes;
    case WaveEncoding::kPcm16:
        return block_bytes / 2;
    case WaveEncoding::kImaAdpcm:
        return (block_bytes - kImaStateBytes) * 2;
    }

    throw std::invalid_argument("unknown wave encoding");
}

std::vector<uint8_t> StreamFile(const Stream& stream)
{
    Writer w;
    w.Magic("STRM");
    w.U16(kByteOrderMark);
    w.U16(stream.header.version);
    w.U32(0);
    w.U16(static_cast<uint16_t>(kHeaderBytes));
    w.U16(2);

    w.Magic("HEAD");
    w.U32(kStreamHeadBytes);
    w.U8(static_cast<uint8_t>(stream.encoding));
    w.U8(stream.loop);
    w.U8(stream.channels);
    w.U8(stream.head_padding);
    w.U16(stream.sample_rate);
    w.U16(stream.timer);
    w.U32(stream.loop_start);
    w.U32(stream.samples);
    w.U32(kHeaderBytes + kStreamHeadBytes + kBlockHeaderBytes);
    w.U32(stream.blocks);
    w.U32(stream.block_bytes);
    w.U32(stream.block_samples);
    w.U32(stream.last_block_bytes);
    w.U32(stream.last_block_samples);
    w.Bytes(stream.reserved);

    w.Magic("DATA");
    w.U32(0);
    w.Bytes(stream.data);
    w.PadTo(AlignUp(w.Size(), 4));
    w.Put32(8, w.Size());
    w.Put32(kHeaderBytes + kStreamHeadBytes + 4, w.Size() - kHeaderBytes - kStreamHeadBytes);

    return w.Take();
}

std::vector<uint8_t> EncodeStream(std::span<const std::vector<int16_t>> channels, uint16_t sample_rate,
                                  WaveEncoding encoding, std::optional<uint32_t> loop_start)
{
    if (sample_rate < kLeastStreamRate)
    {
        throw std::invalid_argument("a stream's sample rate is below 256 Hz");
    }
    if (channels.empty() || channels.size() > 2)
    {
        throw std::invalid_argument("a stream has one or two channels");
    }
    const std::size_t length = channels[0].size();
    if (length == 0 || std::ranges::any_of(channels, [length](const auto& each) { return each.size() != length; }))
    {
        throw std::invalid_argument("a stream's channels have other lengths or no samples");
    }
    const uint32_t block_samples = StreamBlockSamples(encoding);
    if (loop_start && *loop_start >= length)
    {
        throw std::invalid_argument("a stream's loop starts at or past its end");
    }

    // A loop moves forward to a block. The samples between repeat after the end, and the loop keeps its length.
    std::size_t total = length;
    uint32_t start = 0;
    if (loop_start)
    {
        start = (*loop_start + block_samples - 1) / block_samples * block_samples;
        total = start + (length - *loop_start);
    }
    if (total > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("too many samples for a stream");
    }

    // The channels' parts, each block's in turn.
    std::vector<std::vector<std::vector<uint8_t>>> parts;
    for (const std::vector<int16_t>& channel : channels)
    {
        parts.push_back(PartsOf(Repeated(channel, loop_start.value_or(0), total), encoding, block_samples));
    }

    const std::size_t blocks = parts[0].size();
    std::vector<uint8_t> data;
    for (std::size_t block = 0; block < blocks; block++)
    {
        for (const auto& channel : parts)
        {
            data.insert(data.end(), channel[block].begin(), channel[block].end());
        }
    }

    Stream stream;
    stream.header.version = kVersion;
    stream.encoding = encoding;
    stream.loop = loop_start ? 1 : 0;
    stream.channels = static_cast<uint8_t>(channels.size());
    stream.sample_rate = sample_rate;
    stream.timer = static_cast<uint16_t>(kTimerClockHz / sample_rate / kStreamTimerDivider);
    stream.loop_start = start;
    stream.samples = static_cast<uint32_t>(total);
    stream.blocks = static_cast<uint32_t>(blocks);
    stream.block_bytes = kStreamBlockBytes;
    stream.block_samples = block_samples;
    stream.last_block_bytes = static_cast<uint32_t>(parts[0].back().size());
    stream.last_block_samples = static_cast<uint32_t>(total - (blocks - 1) * block_samples);
    stream.data = data;

    return StreamFile(stream);
}

} // namespace ntdlib::sdat
