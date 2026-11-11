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
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rounding.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "rsar/file_blocks.h"
#include "sar/file_bytes.h"
#include "sar/sample_steps.h"

namespace ntdlib::rsar
{
namespace
{

constexpr uint16_t kVersion = 0x0100;
constexpr uint8_t kOffsetReference = 1;    // a reference that holds an offset from HEAD's references
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

using detail::kMostRate;
using detail::kMostStreamCount;
using detail::ReadDspAdpcm;
using detail::WriteDspAdpcm;
using sar::detail::PaddedPieceSize;
using sar::detail::StreamBytes;

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
    return blocks == 0 ? 0 : std::size_t{blocks - 1} * stream.BlockSize() + PaddedPieceSize(stream.LastBlockSize());
}

// Throws std::invalid_argument for a stream that the writer can't lay out (see StreamFile::Write).
void CheckWritable(const StreamFile& stream)
{
    if (stream.encoding != WaveEncoding::kPcm8 && stream.encoding != WaveEncoding::kPcm16 &&
        stream.encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
    }
    if (stream.channels.empty() || stream.channels.size() > kMostStreamCount || stream.sample_count == 0 ||
        stream.block_samples != StreamBlockSamples(stream.encoding))
    {
        throw std::invalid_argument("a stream with 1 to 255 channels, samples and blocks of 8 KiB");
    }
    if (stream.sample_rate == 0 || stream.sample_rate > kMostRate)
    {
        throw std::invalid_argument("a stream whose rate lies between 1 and 24 bits");
    }
    if (stream.loop && stream.loop_start >= stream.sample_count)
    {
        throw std::invalid_argument("a stream whose loop starts before its end");
    }
    const bool adpcm = stream.encoding == WaveEncoding::kDspAdpcm;
    if (adpcm != (stream.history_interval != 0))
    {
        throw std::invalid_argument("a DSP-ADPCM stream without an interval of ADPC, or a PCM stream with one");
    }
    const std::size_t pieces = PiecesSize(stream);
    const std::size_t entries = HistoryCount(stream);
    for (const StreamFile::Channel& channel : stream.channels)
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
    if (stream.tracks.size() > kMostStreamCount)
    {
        throw std::invalid_argument("a stream with more than 255 tracks");
    }
    for (const StreamFile::Track& track : stream.tracks)
    {
        if (track.channels.size() > kMostStreamCount)
        {
            throw std::invalid_argument("a track that plays more than 255 channels");
        }
        for (const uint8_t channel : track.channels)
        {
            if (channel >= stream.channels.size())
            {
                throw std::invalid_argument("a track that plays a channel that the stream lacks");
            }
        }
    }
}

// Appends the HEAD block of `stream`: its references, the settings, the tracks and the channels. Returns where the
// settings start. AppendData gives them the offset of the samples.
std::size_t AppendHead(sar::detail::Out& out, const StreamFile& stream)
{
    // The references, then the settings.
    const bool adpcm = stream.encoding == WaveEncoding::kDspAdpcm;
    const std::size_t head = detail::OpenBlock(out, "HEAD");
    const std::size_t base = out.Size();
    out.Zeros(kReferencesSize);
    const std::size_t settings = out.Zeros(kSettingsSize);
    const uint32_t blocks = stream.BlockCount();
    out.Reference(base, kOffsetType, static_cast<uint32_t>(settings - base));
    out.U8(settings, static_cast<uint8_t>(stream.encoding));
    out.U8(settings + 1, stream.loop ? 1 : 0);
    out.U8(settings + 2, static_cast<uint8_t>(stream.channels.size()));
    detail::WriteRate(out, settings + 3, stream.sample_rate);
    out.U32(settings + 8, stream.loop_start);
    out.U32(settings + 0x0C, stream.sample_count);
    out.U32(settings + 0x14, blocks);
    out.U32(settings + 0x18, stream.BlockSize());
    out.U32(settings + 0x1C, stream.block_samples);
    out.U32(settings + 0x20, stream.LastBlockSize());
    out.U32(settings + 0x24, stream.LastBlockSamples());
    out.U32(settings + 0x28, PaddedPieceSize(stream.LastBlockSize()));
    out.U32(settings + 0x2C, stream.history_interval);
    out.U32(settings + 0x30, adpcm ? kHistoryEntryBytes : 0);

    // The tracks' table, then each track's info: an extended one's volume and pan, then the channels that it plays.
    const uint8_t track_type = stream.extended_tracks ? 1 : 0;
    const std::size_t track_table = out.Zeros(4 + stream.tracks.size() * kReferenceSize);
    out.Reference(base + 8, kOffsetType, static_cast<uint32_t>(track_table - base));
    out.U8(track_table, static_cast<uint8_t>(stream.tracks.size()));
    out.U8(track_table + 1, track_type);
    for (std::size_t t = 0; t < stream.tracks.size(); ++t)
    {
        const StreamFile::Track& track = stream.tracks[t];
        const std::size_t played = AlignUp(1 + track.channels.size(), 4);
        const std::size_t info = out.Zeros((stream.extended_tracks ? kExtendedTrackBytes : 0) + played);
        out.Reference(track_table + 4 + t * kReferenceSize, kOffsetType | track_type,
                      static_cast<uint32_t>(info - base));
        std::size_t at = info;
        if (stream.extended_tracks)
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
    const std::size_t channel_table = out.Zeros(4 + stream.channels.size() * kReferenceSize);
    out.Reference(base + 16, kOffsetType, static_cast<uint32_t>(channel_table - base));
    out.U8(channel_table, static_cast<uint8_t>(stream.channels.size()));
    for (std::size_t c = 0; c < stream.channels.size(); ++c)
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
        WriteDspAdpcm(out, at, *stream.channels[c].adpcm);
    }
    detail::CloseBlock(out, head, 0);

    return settings;
}

// Appends the ADPC block of a DSP-ADPCM `stream`: for each point, each channel's two samples before it.
void AppendAdpc(sar::detail::Out& out, const StreamFile& stream)
{
    const std::size_t entries = HistoryCount(stream);
    const std::size_t adpc = detail::OpenBlock(out, "ADPC");
    const std::size_t at = out.Zeros(entries * stream.channels.size() * kHistoryEntryBytes);
    for (std::size_t e = 0; e < entries; ++e)
    {
        for (std::size_t c = 0; c < stream.channels.size(); ++c)
        {
            const std::size_t entry = at + (e * stream.channels.size() + c) * kHistoryEntryBytes;
            out.U16(entry, static_cast<uint16_t>(stream.channels[c].history[e][0]));
            out.U16(entry + 2, static_cast<uint16_t>(stream.channels[c].history[e][1]));
        }
    }
    detail::CloseBlock(out, adpc, 1);
}

// Appends the DATA block of `stream`: each block's piece of each channel. Gives the samples' offset in the settings at
// `settings`.
void AppendData(sar::detail::Out& out, const StreamFile& stream, std::size_t settings)
{
    const std::size_t pieces = PiecesSize(stream);
    const uint32_t blocks = stream.BlockCount();
    if (uint64_t{out.Size()} + kDataHeader + uint64_t{pieces} * stream.channels.size() >
        std::numeric_limits<uint32_t>::max())
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
        const std::size_t piece = final ? pieces - std::size_t{b} * stream.BlockSize() : stream.BlockSize();
        for (const StreamFile::Channel& channel : stream.channels)
        {
            out.Append(std::span(channel.samples).subspan(std::size_t{b} * stream.BlockSize(), piece));
        }
    }
    detail::CloseBlock(out, data, 2);
}

// What a stream file's settings give for its blocks beside the values that StreamFile keeps: the count of channels,
// where the samples start, the blocks' count and size, and the last block's padded size.
struct BlockSettings
{
    uint8_t channel_count = 0;
    std::size_t samples = 0;
    uint32_t block_count = 0;
    uint32_t block_size = 0;
    uint32_t last_padded = 0;
};

// Where a stream file's ADPC and DATA start, with their sizes, and where its table of channels and HEAD's references
// start.
struct ChannelPlaces
{
    std::size_t adpc = 0;
    std::size_t adpc_size = 0;
    std::size_t data = 0;
    std::size_t data_size = 0;
    std::size_t channels = 0;
    std::size_t base = 0;
};

// Reads the settings at `settings` into `s`, and returns the values of the blocks. Throws xsflib::FormatError for an
// encoding other than PCM and DSP-ADPCM, blocks with headers, blocks that don't add up to the samples, and a DSP-ADPCM
// stream without entries in ADPC.
BlockSettings ReadSettings(const xsflib::Reader& r, std::size_t settings, StreamFile& s)
{
    const uint8_t encoding = r.U8(settings);
    if (encoding > static_cast<uint8_t>(WaveEncoding::kDspAdpcm))
    {
        throw xsflib::FormatError("a stream of encoding " + std::to_string(encoding) + ", not PCM or DSP-ADPCM");
    }
    s.encoding = static_cast<WaveEncoding>(encoding);
    s.loop = r.U8(settings + 1) != 0;
    BlockSettings blocks;
    blocks.channel_count = r.U8(settings + 2);
    s.sample_rate = detail::ReadRate(r, settings + 3);
    if (r.U16(settings + 6) != 0)
    {
        throw xsflib::FormatError("a stream whose blocks hold headers");
    }

    s.loop_start = r.U32(settings + 8);
    s.sample_count = r.U32(settings + 0x0C);
    blocks.samples = r.U32(settings + 0x10);
    blocks.block_count = r.U32(settings + 0x14);
    blocks.block_size = r.U32(settings + 0x18);
    s.block_samples = r.U32(settings + 0x1C);
    const uint32_t last_size = r.U32(settings + 0x20);
    const uint32_t last_samples = r.U32(settings + 0x24);
    blocks.last_padded = r.U32(settings + 0x28);
    if (s.block_samples == 0 || blocks.block_count == 0 || blocks.block_count != s.BlockCount() ||
        blocks.block_size != s.BlockSize() || last_samples != s.LastBlockSamples() || last_size != s.LastBlockSize() ||
        blocks.last_padded != PaddedPieceSize(last_size))
    {
        throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
    }

    if (s.encoding == WaveEncoding::kDspAdpcm)
    {
        s.history_interval = r.U32(settings + 0x2C);
        if (s.history_interval == 0 || r.U32(settings + 0x30) != kHistoryEntryBytes)
        {
            throw xsflib::FormatError("a DSP-ADPCM stream without entries in ADPC");
        }
    }

    return blocks;
}

// Reads the table of tracks at `tracks` into `s`. Its references count from `base`. Throws xsflib::FormatError for
// tracks' infos of an unknown type, and for a track that plays a channel past `channel_count`.
void ReadTracks(const xsflib::Reader& r, std::size_t tracks, std::size_t base, uint8_t channel_count, StreamFile& s)
{
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
        StreamFile::Track& track = s.tracks.emplace_back();
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
}

// Reads each channel of `s`: its info, its pieces of DATA and its entries of ADPC. Throws xsflib::FormatError for a
// table of channels with another count than the settings, and for blocks or entries that run past DATA or ADPC.
void ReadChannels(const xsflib::Reader& r, const ChannelPlaces& places, const BlockSettings& blocks, StreamFile& s)
{
    const uint8_t channel_count = blocks.channel_count;
    if (channel_count == 0 || r.U8(places.channels) != channel_count)
    {
        throw xsflib::FormatError("a stream whose table of channels has another count");
    }
    const std::size_t stride = std::size_t{blocks.block_size} * channel_count;
    const std::size_t all = (blocks.block_count - 1) * stride + std::size_t{blocks.last_padded} * channel_count;
    if (blocks.samples < places.data || blocks.samples + all > places.data + places.data_size)
    {
        throw xsflib::FormatError("a stream whose blocks run past DATA");
    }
    const bool adpcm = s.encoding == WaveEncoding::kDspAdpcm;
    const std::size_t entries = HistoryCount(s);
    if (adpcm && (places.adpc == 0 || !r.Magic(places.adpc, "ADPC") ||
                  8 + entries * channel_count * kHistoryEntryBytes > places.adpc_size))
    {
        throw xsflib::FormatError("a stream whose entries run past ADPC");
    }
    if (adpcm)
    {
        r.Sub(places.adpc, places.adpc_size);
    }

    for (uint8_t c = 0; c < channel_count; ++c)
    {
        const std::size_t info = Follow(r, places.channels + 4 + std::size_t{c} * 8, places.base, "a channel");
        StreamFile::Channel& channel = s.channels.emplace_back();
        if (adpcm)
        {
            channel.adpcm = ReadDspAdpcm(r, Follow(r, info, places.base, "a channel's DSP-ADPCM info"));
        }

        channel.samples.reserve(all / channel_count);
        for (uint32_t b = 0; b < blocks.block_count; ++b)
        {
            const bool final = b + 1 == blocks.block_count;
            const std::size_t piece = final ? blocks.last_padded : blocks.block_size;
            const std::span<const uint8_t> bytes = r.Sub(blocks.samples + b * stride + c * piece, piece);
            channel.samples.insert(channel.samples.end(), bytes.begin(), bytes.end());
        }

        for (std::size_t e = 0; adpcm && e < entries; ++e)
        {
            const std::size_t at = places.adpc + 8 + (e * channel_count + c) * kHistoryEntryBytes;
            channel.history.push_back({r.S16(at), r.S16(at + 2)});
        }
    }
}

} // namespace

uint32_t StreamFile::BlockSize() const
{
    return StreamBytes(encoding, block_samples);
}

uint32_t StreamFile::BlockCount() const
{
    return sar::detail::StreamBlockCount(sample_count, block_samples);
}

uint32_t StreamFile::LastBlockSamples() const
{
    return sar::detail::StreamLastBlockSamples(sample_count, block_samples);
}

uint32_t StreamFile::LastBlockSize() const
{
    return StreamBytes(encoding, LastBlockSamples());
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

    // The settings, the tracks and the channels.
    StreamFile s;
    const std::size_t base = head + 8;
    const std::size_t settings = Follow(r, base, base, "the settings");
    const BlockSettings blocks = ReadSettings(r, settings, s);
    ReadTracks(r, Follow(r, base + 8, base, "the tracks"), base, blocks.channel_count, s);
    const std::size_t channels = Follow(r, base + 16, base, "the channels");
    ReadChannels(r, {adpc, adpc_size, data, data_size, channels, base}, blocks, s);

    return s;
}

std::vector<uint8_t> StreamFile::Write() const
{
    CheckWritable(*this);

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

    // HEAD, ADPC for DSP-ADPCM, then DATA.
    const std::size_t settings = AppendHead(out, *this);
    if (encoding == WaveEncoding::kDspAdpcm)
    {
        AppendAdpc(out, *this);
    }
    AppendData(out, *this, settings);

    return detail::CloseFile(out);
}

uint32_t StreamBlockSamples(WaveEncoding encoding)
{
    return sar::detail::StreamBlockSamplesOf(encoding);
}

wave::Pcm DecodeStream(const StreamFile& stream)
{
    // The Wii's 16-bit samples are big-endian. DSP-ADPCM sums in 64 bits. The wave decoder throws for a channel shorter
    // than the stream's samples.
    sar::detail::StreamCoding coding;
    coding.encoding = sar::detail::DecoderEncoding(stream.encoding);
    coding.big_endian = true;
    coding.sum = wave::DspAdpcmSum::kWide;
    const char* missing_info =
        stream.encoding == WaveEncoding::kDspAdpcm ? "a DSP-ADPCM channel without its DSP-ADPCM info" : nullptr;

    return sar::detail::DecodeStreamChannels(stream, coding, missing_info);
}

StreamFile MakeStream(WaveEncoding encoding, uint32_t sample_rate, std::span<const std::vector<int16_t>> channels,
                      std::optional<uint32_t> loop_start)
{
    if (encoding != WaveEncoding::kPcm16 && encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of 16-bit PCM or DSP-ADPCM");
    }
    sar::detail::CheckedLength(channels, loop_start);
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
        s.loop_start = sar::detail::MoveLoop(samples, *loop_start, moved);
    }
    s.sample_count = sar::detail::SampleCount(samples);

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
    sar::detail::ChannelLayout layout;
    layout.adpcm = encoding == WaveEncoding::kDspAdpcm;
    layout.big_endian = true;
    layout.sum = wave::DspAdpcmSum::kWide;
    layout.loop_start = s.loop ? s.loop_start : 0;
    layout.size = PiecesSize(s);
    layout.points = HistoryCount(s);
    layout.interval = s.history_interval;
    for (const std::vector<int16_t>& input : samples)
    {
        s.channels.push_back(sar::detail::EncodeChannel<StreamFile::Channel>(input, layout));
    }

    return s;
}

} // namespace ntdlib::rsar
