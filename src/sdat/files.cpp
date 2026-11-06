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

#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/wave/encode_ima.h"
#include "sdat/writer.h"

namespace ntdlib::sdat
{

namespace
{

using detail::Align;
using detail::Writer;

constexpr uint16_t kByteOrderMark = 0xFEFF;
constexpr uint16_t kVersion = 0x0100;
constexpr std::size_t kHeaderBytes = 0x10;
constexpr std::size_t kTableStart = 0x3C;  // a bank's or a wave archive's table: after 32 bytes that a game fills in
constexpr std::size_t kSplitKeys = 8;      // a key split's keys
constexpr uint32_t kMaxOffset = 0xFFFFFF;  // the 24 bits of a bank record's offset
constexpr uint32_t kMaxLoopStart = 0xFFFF; // the 16 bits of a wave's loop start
constexpr uint32_t kMaxTimer = 0xFFFF;     // the 16 bits of a wave's timer
constexpr uint32_t kTimerClockHz = ntr::kSystemClockHz / 2;

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
    w.PadTo(Align(w.Size(), 4));
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

// The samples in a 32-bit word of `encoding`.
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

// `samples` laid out in whole words of `per_word` samples (see EncodeWave). Gives the loop's start in samples.
std::vector<int16_t> InWords(std::span<const int16_t> samples, uint32_t per_word, std::optional<uint32_t>& loop_start)
{
    if (!loop_start)
    {
        std::vector<int16_t> out(samples.begin(), samples.end());
        out.resize(Align(out.size(), per_word), 0);
        return out;
    }

    // The loop moves forward to a word's start, and repeats until its length is a whole number of words.
    const std::size_t old_start = *loop_start;
    const std::size_t length = samples.size() - old_start;
    const std::size_t start = Align(old_start, per_word);
    const std::size_t passes = per_word / std::gcd(length, std::size_t{per_word});
    std::vector<int16_t> out(start + passes * length);
    for (std::size_t i = 0; i < out.size(); i++)
    {
        out[i] = i < samples.size() ? samples[i] : samples[old_start + (i - old_start) % length];
    }
    loop_start = static_cast<uint32_t>(start);

    return out;
}

} // namespace

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
        w.PadTo(Align(w.Size(), 4));
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
    const uint32_t header_words = encoding == WaveEncoding::kImaAdpcm ? 1 : 0;
    if (loop_start && header_words + Align(*loop_start, per_word) / per_word > kMaxLoopStart)
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
            w.U8(static_cast<uint8_t>(std::clamp((sample + 0x80) >> 8, -128, 127)));
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

} // namespace ntdlib::sdat
