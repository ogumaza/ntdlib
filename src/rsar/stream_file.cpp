// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/stream_file.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "rsar/file_blocks.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar
{
namespace
{

constexpr uint16_t kVersion = 0x0100;
constexpr uint8_t kOffsetReference = 1; // a reference that holds an offset from HEAD's references
constexpr uint32_t kAlignment = 32;
constexpr uint32_t kDspAdpcmFrameBytes = 8;
constexpr uint32_t kDspAdpcmFrameSamples = 14;
constexpr uint32_t kHistoryEntryBytes = 4; // two 16-bit samples for each channel

// The layout that the writer gives a file: the header's size and its count of blocks, HEAD's references and settings,
// the volume and pan with their padding before an extended track's channels, a channel's DSP-ADPCM info, and DATA's
// bytes before the samples, with the field that gives their offset from the field's end.
constexpr std::size_t kStreamHeaderSize = 0x40;
constexpr uint16_t kHeaderBlocks = 2;
constexpr std::size_t kReferencesSize = 24;
constexpr std::size_t kReferenceSize = 8;
constexpr std::size_t kSettingsSize = 0x34;
constexpr std::size_t kExtendedTrackBytes = 8;
constexpr std::size_t kDspAdpcmInfoSize = 0x30;
constexpr std::size_t kDataHeader = 0x20;
constexpr uint32_t kSamplesOffset = 0x18;
constexpr uint16_t kOffsetType = uint16_t{kOffsetReference} << 8; // a reference's first two bytes, with data type 0

// Each channel's piece of a block, the most channels and tracks that the settings and tables count, and the highest
// rate that the settings' 24 bits hold.
constexpr uint32_t kBlockBytes = 0x2000;
constexpr std::size_t kMostCount = 255;
constexpr uint32_t kMostRate = 0xFFFFFF;

uint32_t RoundUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The bytes of `samples` samples of `encoding`, up to the nibble or byte of the last one.
uint32_t BytesOf(WaveEncoding encoding, uint32_t samples)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return samples;
    case WaveEncoding::kPcm16:
        return samples * 2;
    case WaveEncoding::kDspAdpcm:
        {
            if (samples == 0)
            {
                return 0;
            }
            // The frames before the one that holds the last sample, that frame's header, and its bytes up to the
            // last sample's nibble.
            const uint32_t last = samples - 1;
            return last / kDspAdpcmFrameSamples * kDspAdpcmFrameBytes + 2 + last % kDspAdpcmFrameSamples / 2;
        }
    }

    return 0;
}

// ADPC's entries for each channel, as the tools count them: for each block, its bytes over the interval, rounded up.
std::size_t HistoryCount(const StreamFile& stream)
{
    const uint32_t interval = stream.history_interval;
    const uint32_t blocks = stream.BlockCount();
    if (interval == 0 || blocks == 0)
    {
        return 0;
    }

    return std::size_t{blocks - 1} * ((stream.BlockSize() + interval - 1) / interval) +
           (stream.LastBlockSize() + interval - 1) / interval;
}

// The offset that the reference at `at` holds, from `base`. Throws for a reference of another kind.
std::size_t Follow(const xsflib::Reader& r, std::size_t at, std::size_t base, const char* what)
{
    if (r.U8(at) != kOffsetReference)
    {
        throw xsflib::FormatError(std::string("a stream file whose reference to ") + what + " isn't an offset");
    }

    return base + r.U32(at + 4);
}

// The bytes of each channel's pieces of DATA: every block's piece, the last one padded to 32 bytes.
std::size_t PiecesSize(const StreamFile& stream)
{
    const uint32_t blocks = stream.BlockCount();
    return blocks == 0 ? 0 : std::size_t{blocks - 1} * stream.BlockSize() + RoundUp(stream.LastBlockSize(), kAlignment);
}

DspAdpcmInfo ReadDspAdpcm(const xsflib::Reader& r, std::size_t at)
{
    DspAdpcmInfo info;
    for (std::size_t i = 0; i < info.coefs.size(); ++i)
    {
        info.coefs[i] = r.S16(at + i * 2);
    }
    info.gain = r.U16(at + 0x20);
    info.pred_scale = r.U16(at + 0x22);
    info.yn1 = r.S16(at + 0x24);
    info.yn2 = r.S16(at + 0x26);
    info.loop_pred_scale = r.U16(at + 0x28);
    info.loop_yn1 = r.S16(at + 0x2A);
    info.loop_yn2 = r.S16(at + 0x2C);

    return info;
}

} // namespace

uint32_t StreamFile::BlockSize() const
{
    return BytesOf(encoding, block_samples);
}

uint32_t StreamFile::BlockCount() const
{
    return block_samples == 0 ? 0 : (sample_count + block_samples - 1) / block_samples;
}

uint32_t StreamFile::LastBlockSamples() const
{
    const uint32_t blocks = BlockCount();
    return blocks == 0 ? 0 : sample_count - (blocks - 1) * block_samples;
}

uint32_t StreamFile::LastBlockSize() const
{
    return BytesOf(encoding, LastBlockSamples());
}

StreamFile StreamFile::Read(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    if (!r.Magic(0, "RSTM") || r.U16(4) != 0xFEFF)
    {
        throw xsflib::FormatError("not a stream file of the Wii (RSTM)");
    }
    const uint16_t version = r.U16(6);
    if (version != kVersion)
    {
        throw xsflib::FormatError("a stream file of version " + std::to_string(version >> 8) + "." +
                                  std::to_string(version & 0xFF) + ", not 1.0");
    }
    const std::size_t head = r.U32(0x10);
    const std::size_t head_size = r.U32(0x14);
    const std::size_t adpc = r.U32(0x18);
    const std::size_t adpc_size = r.U32(0x1C);
    const std::size_t data = r.U32(0x20);
    const std::size_t data_size = r.U32(0x24);
    if (head == 0 || !r.Magic(head, "HEAD") || data == 0 || !r.Magic(data, "DATA"))
    {
        throw xsflib::FormatError("a stream file without HEAD or DATA");
    }
    r.Sub(head, head_size);
    r.Sub(data, data_size);

    // The settings.
    StreamFile s;
    const std::size_t base = head + 8;
    const std::size_t settings = Follow(r, base, base, "the settings");
    const uint8_t encoding = r.U8(settings);
    if (encoding > static_cast<uint8_t>(WaveEncoding::kDspAdpcm))
    {
        throw xsflib::FormatError("a stream of encoding " + std::to_string(encoding) + ", not PCM or DSP-ADPCM");
    }
    s.encoding = static_cast<WaveEncoding>(encoding);
    s.loop = r.U8(settings + 1) != 0;
    const uint8_t channel_count = r.U8(settings + 2);
    s.sample_rate = uint32_t{r.U8(settings + 3)} << 16 | r.U16(settings + 4);
    if (r.U16(settings + 6) != 0)
    {
        throw xsflib::FormatError("a stream whose blocks hold headers");
    }
    s.loop_start = r.U32(settings + 8);
    s.sample_count = r.U32(settings + 0x0C);
    const std::size_t samples = r.U32(settings + 0x10);
    const uint32_t block_count = r.U32(settings + 0x14);
    const uint32_t block_size = r.U32(settings + 0x18);
    s.block_samples = r.U32(settings + 0x1C);
    const uint32_t last_size = r.U32(settings + 0x20);
    const uint32_t last_samples = r.U32(settings + 0x24);
    const uint32_t last_padded = r.U32(settings + 0x28);
    if (s.block_samples == 0 || block_count == 0 || block_count != s.BlockCount() || block_size != s.BlockSize() ||
        last_samples != s.LastBlockSamples() || last_size != s.LastBlockSize() ||
        last_padded != RoundUp(last_size, kAlignment))
    {
        throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
    }
    const bool adpcm = s.encoding == WaveEncoding::kDspAdpcm;
    if (adpcm)
    {
        s.history_interval = r.U32(settings + 0x2C);
        if (s.history_interval == 0 || r.U32(settings + 0x30) != kHistoryEntryBytes)
        {
            throw xsflib::FormatError("a DSP-ADPCM stream without entries in ADPC");
        }
    }

    // The tracks.
    const std::size_t tracks = Follow(r, base + 8, base, "the tracks");
    const uint8_t track_count = r.U8(tracks);
    const uint8_t track_type = r.U8(tracks + 1);
    if (track_type > 1)
    {
        throw xsflib::FormatError("a stream whose tracks' infos are of type " + std::to_string(track_type));
    }
    s.extended_tracks = track_type == 1;
    for (uint8_t t = 0; t < track_count; ++t)
    {
        const std::size_t info = Follow(r, tracks + 4 + std::size_t{t} * 8, base, "a track");
        Track& track = s.tracks.emplace_back();
        std::size_t at = info;
        if (s.extended_tracks)
        {
            track.volume = r.U8(info);
            track.pan = r.U8(info + 1);
            at = info + 8;
        }
        const uint8_t count = r.U8(at);
        for (uint8_t c = 0; c < count; ++c)
        {
            const uint8_t channel = r.U8(at + 1 + c);
            if (channel >= channel_count)
            {
                throw xsflib::FormatError("a stream whose track plays a channel that the stream lacks");
            }
            track.channels.push_back(channel);
        }
    }

    // Each channel's info, its pieces of DATA and its entries of ADPC.
    const std::size_t channels = Follow(r, base + 16, base, "the channels");
    if (channel_count == 0 || r.U8(channels) != channel_count)
    {
        throw xsflib::FormatError("a stream whose table of channels has another count");
    }
    const std::size_t stride = std::size_t{block_size} * channel_count;
    const std::size_t all = (block_count - 1) * stride + std::size_t{last_padded} * channel_count;
    if (samples < data || samples + all > data + data_size)
    {
        throw xsflib::FormatError("a stream whose blocks run past DATA");
    }
    const std::size_t entries = HistoryCount(s);
    if (adpcm && (adpc == 0 || !r.Magic(adpc, "ADPC") || 8 + entries * channel_count * kHistoryEntryBytes > adpc_size))
    {
        throw xsflib::FormatError("a stream whose entries run past ADPC");
    }
    if (adpcm)
    {
        r.Sub(adpc, adpc_size);
    }
    for (uint8_t c = 0; c < channel_count; ++c)
    {
        const std::size_t info = Follow(r, channels + 4 + std::size_t{c} * 8, base, "a channel");
        Channel& channel = s.channels.emplace_back();
        if (adpcm)
        {
            channel.adpcm = ReadDspAdpcm(r, Follow(r, info, base, "a channel's DSP-ADPCM info"));
        }

        channel.samples.reserve(all / channel_count);
        for (uint32_t b = 0; b < block_count; ++b)
        {
            const bool final = b + 1 == block_count;
            const std::size_t piece = final ? last_padded : block_size;
            const std::span<const uint8_t> bytes = r.Sub(samples + b * stride + c * piece, piece);
            channel.samples.insert(channel.samples.end(), bytes.begin(), bytes.end());
        }

        for (std::size_t e = 0; adpcm && e < entries; ++e)
        {
            const std::size_t at = adpc + 8 + (e * channel_count + c) * kHistoryEntryBytes;
            channel.history.push_back({r.S16(at), r.S16(at + 2)});
        }
    }

    return s;
}

std::vector<uint8_t> StreamFile::Write() const
{
    if (encoding != WaveEncoding::kPcm8 && encoding != WaveEncoding::kPcm16 && encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
    }
    if (channels.empty() || channels.size() > kMostCount || sample_count == 0 ||
        block_samples != StreamBlockSamples(encoding))
    {
        throw std::invalid_argument("a stream with 1 to 255 channels, samples and blocks of 8 KiB");
    }
    if (sample_rate == 0 || sample_rate > kMostRate)
    {
        throw std::invalid_argument("a stream whose rate lies between 1 and 24 bits");
    }
    if (loop && loop_start >= sample_count)
    {
        throw std::invalid_argument("a stream whose loop starts before its end");
    }
    const bool adpcm = encoding == WaveEncoding::kDspAdpcm;
    if (adpcm != (history_interval != 0))
    {
        throw std::invalid_argument("a DSP-ADPCM stream without an interval of ADPC, or a PCM stream with one");
    }
    const std::size_t pieces = PiecesSize(*this);
    const std::size_t entries = HistoryCount(*this);
    for (const Channel& channel : channels)
    {
        if (channel.adpcm.has_value() != adpcm)
        {
            throw std::invalid_argument("a DSP-ADPCM channel without its DSP-ADPCM info, or a PCM channel with one");
        }
        if (channel.samples.size() != pieces || channel.history.size() != entries)
        {
            throw std::invalid_argument("a channel whose samples or entries of ADPC don't fit the stream's settings");
        }
    }
    if (tracks.size() > kMostCount)
    {
        throw std::invalid_argument("a stream with more than 255 tracks");
    }
    for (const Track& track : tracks)
    {
        if (track.channels.size() > kMostCount)
        {
            throw std::invalid_argument("a track that plays more than 255 channels");
        }
        for (const uint8_t channel : track.channels)
        {
            if (channel >= channels.size())
            {
                throw std::invalid_argument("a track that plays a channel that the stream lacks");
            }
        }
    }

    // The header. Its count of blocks holds 2 in every file that the tools write.
    sar::detail::Out out(true);
    out.Zeros(kStreamHeaderSize);
    const std::string magic = "RSTM";
    for (std::size_t i = 0; i < magic.size(); ++i)
    {
        out.U8(i, static_cast<uint8_t>(magic[i]));
    }
    out.U16(4, 0xFEFF);
    out.U16(6, kVersion);
    out.U16(0xC, static_cast<uint16_t>(kStreamHeaderSize));
    out.U16(0xE, kHeaderBlocks);

    // HEAD: its references, then the settings.
    const std::size_t head = detail::OpenBlock(out, "HEAD");
    const std::size_t base = out.Size();
    out.Zeros(kReferencesSize);
    const std::size_t settings = out.Zeros(kSettingsSize);
    const uint32_t blocks = BlockCount();
    out.Reference(base, kOffsetType, static_cast<uint32_t>(settings - base));
    out.U8(settings, static_cast<uint8_t>(encoding));
    out.U8(settings + 1, loop ? 1 : 0);
    out.U8(settings + 2, static_cast<uint8_t>(channels.size()));
    out.U8(settings + 3, static_cast<uint8_t>(sample_rate >> 16));
    out.U16(settings + 4, static_cast<uint16_t>(sample_rate & 0xFFFF));
    out.U32(settings + 8, loop_start);
    out.U32(settings + 0x0C, sample_count);
    out.U32(settings + 0x14, blocks);
    out.U32(settings + 0x18, BlockSize());
    out.U32(settings + 0x1C, block_samples);
    out.U32(settings + 0x20, LastBlockSize());
    out.U32(settings + 0x24, LastBlockSamples());
    out.U32(settings + 0x28, RoundUp(LastBlockSize(), kAlignment));
    out.U32(settings + 0x2C, history_interval);
    out.U32(settings + 0x30, adpcm ? kHistoryEntryBytes : 0);

    // The tracks' table, then each track's info: an extended one's volume and pan, then the channels that it plays.
    const uint8_t track_type = extended_tracks ? 1 : 0;
    const std::size_t track_table = out.Zeros(4 + tracks.size() * kReferenceSize);
    out.Reference(base + 8, kOffsetType, static_cast<uint32_t>(track_table - base));
    out.U8(track_table, static_cast<uint8_t>(tracks.size()));
    out.U8(track_table + 1, track_type);
    for (std::size_t t = 0; t < tracks.size(); ++t)
    {
        const Track& track = tracks[t];
        const std::size_t played = RoundUp(static_cast<uint32_t>(1 + track.channels.size()), 4);
        const std::size_t info = out.Zeros((extended_tracks ? kExtendedTrackBytes : 0) + played);
        out.Reference(track_table + 4 + t * kReferenceSize, kOffsetType | track_type,
                      static_cast<uint32_t>(info - base));
        std::size_t at = info;
        if (extended_tracks)
        {
            out.U8(info, track.volume);
            out.U8(info + 1, track.pan);
            at = info + kExtendedTrackBytes;
        }
        out.U8(at, static_cast<uint8_t>(track.channels.size()));
        for (std::size_t c = 0; c < track.channels.size(); ++c)
        {
            out.U8(at + 1 + c, track.channels[c]);
        }
    }

    // The channels' table, then each channel's info with its DSP-ADPCM info after it. A PCM channel's info refers to
    // nothing.
    const std::size_t channel_table = out.Zeros(4 + channels.size() * kReferenceSize);
    out.Reference(base + 16, kOffsetType, static_cast<uint32_t>(channel_table - base));
    out.U8(channel_table, static_cast<uint8_t>(channels.size()));
    for (std::size_t c = 0; c < channels.size(); ++c)
    {
        const std::size_t info = out.Zeros(kReferenceSize);
        out.Reference(channel_table + 4 + c * kReferenceSize, kOffsetType, static_cast<uint32_t>(info - base));
        if (!adpcm)
        {
            out.Reference(info, kOffsetType, 0);
            continue;
        }

        const std::size_t at = out.Zeros(kDspAdpcmInfoSize);
        out.Reference(info, kOffsetType, static_cast<uint32_t>(at - base));
        const DspAdpcmInfo& dsp = *channels[c].adpcm;
        for (std::size_t i = 0; i < dsp.coefs.size(); ++i)
        {
            out.U16(at + i * 2, static_cast<uint16_t>(dsp.coefs[i]));
        }
        out.U16(at + 0x20, dsp.gain);
        out.U16(at + 0x22, dsp.pred_scale);
        out.U16(at + 0x24, static_cast<uint16_t>(dsp.yn1));
        out.U16(at + 0x26, static_cast<uint16_t>(dsp.yn2));
        out.U16(at + 0x28, dsp.loop_pred_scale);
        out.U16(at + 0x2A, static_cast<uint16_t>(dsp.loop_yn1));
        out.U16(at + 0x2C, static_cast<uint16_t>(dsp.loop_yn2));
    }
    detail::CloseBlock(out, head, 0);

    // ADPC: for each point, each channel's two samples before it.
    if (adpcm)
    {
        const std::size_t adpc = detail::OpenBlock(out, "ADPC");
        const std::size_t at = out.Zeros(entries * channels.size() * kHistoryEntryBytes);
        for (std::size_t e = 0; e < entries; ++e)
        {
            for (std::size_t c = 0; c < channels.size(); ++c)
            {
                const std::size_t entry = at + (e * channels.size() + c) * kHistoryEntryBytes;
                out.U16(entry, static_cast<uint16_t>(channels[c].history[e][0]));
                out.U16(entry + 2, static_cast<uint16_t>(channels[c].history[e][1]));
            }
        }
        detail::CloseBlock(out, adpc, 1);
    }

    // DATA: each block's piece of each channel.
    if (uint64_t{out.Size()} + kDataHeader + uint64_t{pieces} * channels.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("a stream file past 4 GB");
    }
    const std::size_t data = detail::OpenBlock(out, "DATA");
    out.Zeros(kDataHeader - detail::kBlockHeader);
    out.U32(data + 8, kSamplesOffset);
    out.U32(settings + 0x10, static_cast<uint32_t>(data + kDataHeader));
    for (uint32_t b = 0; b < blocks; ++b)
    {
        const bool final = b + 1 == blocks;
        const std::size_t piece = final ? pieces - std::size_t{b} * BlockSize() : BlockSize();
        for (const Channel& channel : channels)
        {
            out.Append(std::span(channel.samples).subspan(std::size_t{b} * BlockSize(), piece));
        }
    }
    detail::CloseBlock(out, data, 2);

    return detail::CloseFile(out);
}

uint32_t StreamBlockSamples(WaveEncoding encoding)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return kBlockBytes;
    case WaveEncoding::kPcm16:
        return kBlockBytes / 2;
    case WaveEncoding::kDspAdpcm:
        return kBlockBytes / kDspAdpcmFrameBytes * kDspAdpcmFrameSamples;
    }

    throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
}

wave::Pcm DecodeStream(const StreamFile& stream)
{
    wave::Pcm pcm;
    pcm.sample_rate = stream.sample_rate;
    pcm.loop = stream.loop && stream.loop_start < stream.sample_count;
    pcm.loop_start = pcm.loop ? stream.loop_start : 0;
    pcm.loop_end = stream.sample_count;
    for (const StreamFile::Channel& channel : stream.channels)
    {
        // Each channel decodes by itself: the wave decoder decodes only the first two channels.
        wave::EncodedWave encoded;
        encoded.encoding = stream.encoding == WaveEncoding::kPcm8    ? wave::Encoding::kPcm8
                           : stream.encoding == WaveEncoding::kPcm16 ? wave::Encoding::kPcm16
                                                                     : wave::Encoding::kDspAdpcm;
        encoded.big_endian = true;
        encoded.sum = wave::DspAdpcmSum::kWide;
        encoded.sample_rate = stream.sample_rate;
        encoded.loop_end = stream.sample_count;
        wave::EncodedChannel& one = encoded.channels.emplace_back();
        one.data = channel.samples;
        if (channel.adpcm)
        {
            one.coefs = &channel.adpcm->coefs;
            one.yn1 = channel.adpcm->yn1;
            one.yn2 = channel.adpcm->yn2;
        }
        else if (stream.encoding == WaveEncoding::kDspAdpcm)
        {
            throw xsflib::FormatError("a DSP-ADPCM channel without its DSP-ADPCM info");
        }
        pcm.channels.push_back(std::move(wave::DecodeWave(encoded).channels.front()));
        if (pcm.channels.back().size() < stream.sample_count)
        {
            throw xsflib::FormatError("a stream whose channel is shorter than its samples");
        }
    }

    return pcm;
}

StreamFile MakeStream(WaveEncoding encoding, uint32_t sample_rate, std::span<const std::vector<int16_t>> channels,
                      std::optional<uint32_t> loop_start)
{
    if (encoding != WaveEncoding::kPcm16 && encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of 16-bit PCM or DSP-ADPCM");
    }
    if (channels.empty() || channels.size() > kMostCount)
    {
        throw std::invalid_argument("a stream with 1 to 255 channels");
    }
    const std::size_t length = channels.front().size();
    for (const std::vector<int16_t>& channel : channels)
    {
        if (channel.size() != length)
        {
            throw std::invalid_argument("a stream whose channels have different lengths");
        }
    }
    if (length == 0 || (loop_start && *loop_start >= length))
    {
        throw std::invalid_argument("a stream with samples, and a loop that starts before their end");
    }
    if (sample_rate == 0 || sample_rate > kMostRate)
    {
        throw std::invalid_argument("a stream whose rate lies between 1 and 24 bits");
    }

    // The loop moves forward to the first block's start at or after it, and the samples from its start follow the
    // end. A sample past the old end repeats the loop's samples.
    StreamFile s;
    s.encoding = encoding;
    s.sample_rate = sample_rate;
    s.block_samples = StreamBlockSamples(encoding);
    s.history_interval = encoding == WaveEncoding::kDspAdpcm ? s.block_samples : 0;
    s.extended_tracks = true;
    std::vector<std::vector<int16_t>> samples(channels.begin(), channels.end());
    s.loop = loop_start.has_value();
    if (loop_start)
    {
        const uint64_t block = s.block_samples;
        const uint64_t moved = (uint64_t{*loop_start} + block - 1) / block * block;
        for (std::vector<int16_t>& channel : samples)
        {
            for (uint64_t i = *loop_start; i < moved; ++i)
            {
                const int16_t sample = channel[i];
                channel.push_back(sample);
            }
        }
        if (moved > std::numeric_limits<uint32_t>::max())
        {
            throw std::invalid_argument("a stream with more samples than its 32-bit sizes hold");
        }
        s.loop_start = static_cast<uint32_t>(moved);
    }
    if (samples.front().size() > std::numeric_limits<uint32_t>::max() / 2)
    {
        throw std::invalid_argument("a stream with more samples than its 32-bit sizes hold");
    }
    s.sample_count = static_cast<uint32_t>(samples.front().size());

    // A track for each pair of channels.
    for (std::size_t c = 0; c < samples.size(); c += 2)
    {
        StreamFile::Track& track = s.tracks.emplace_back();
        track.channels.push_back(static_cast<uint8_t>(c));
        if (c + 1 < samples.size())
        {
            track.channels.push_back(static_cast<uint8_t>(c + 1));
        }
    }

    // Each channel's samples, padded to the last block's piece, and the samples before each point of ADPC.
    const std::size_t pieces = PiecesSize(s);
    const std::size_t entries = HistoryCount(s);
    for (const std::vector<int16_t>& input : samples)
    {
        StreamFile::Channel& channel = s.channels.emplace_back();
        if (encoding == WaveEncoding::kDspAdpcm)
        {
            const wave::DspAdpcmChannel encoded =
                wave::EncodeDspAdpcm(input, s.loop ? s.loop_start : 0, wave::DspAdpcmSum::kWide);
            channel.samples = encoded.data;
            DspAdpcmInfo adpcm;
            adpcm.coefs = encoded.coefs;
            adpcm.pred_scale = encoded.pred_scale;
            adpcm.loop_pred_scale = encoded.loop_pred_scale;
            adpcm.loop_yn1 = encoded.loop_yn1;
            adpcm.loop_yn2 = encoded.loop_yn2;
            channel.adpcm = adpcm;
        }
        else
        {
            channel.samples.resize(input.size() * 2);
            for (std::size_t i = 0; i < input.size(); ++i)
            {
                xsflib::StoreBe16(&channel.samples[i * 2], static_cast<uint16_t>(input[i]));
            }
        }
        channel.samples.resize(pieces);
        for (std::size_t e = 0; e < entries; ++e)
        {
            const std::size_t point = e * s.history_interval;
            channel.history.push_back(
                {point >= 1 ? input[point - 1] : int16_t{0}, point >= 2 ? input[point - 2] : int16_t{0}});
        }
    }

    return s;
}

} // namespace ntdlib::rsar
