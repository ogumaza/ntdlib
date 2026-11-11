// SPDX-License-Identifier: MIT

#include "ntdlib/sar/stream_file.h"

#include <algorithm>
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

#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/decode.h"
#include "sar/file_bytes.h"
#include "sar/file_parts.h"
#include "sar/format_ids.h"
#include "sar/sample_steps.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kInfoBlock = 0x4000;
constexpr uint16_t kSeekBlock = 0x4001;
constexpr uint16_t kDataBlock = 0x4002;
constexpr uint16_t kSettingsReference = 0x4100;    // from INFO to the stream's settings
constexpr uint16_t kTrackInfoReference = 0x4101;   // from the table of tracks to a track's info
constexpr uint16_t kChannelInfoReference = 0x4102; // from the table of channels to a channel's info
constexpr std::size_t kAlignment = 32;

// The settings: the encoding, the loop, the counts, the rate, the loop's start, the samples, the blocks' sizes and
// samples, the seek's sizes, the reference to the samples, and the regions' size and reference. From version
// 0x00040000 on, the loop's original start and end follow them. The 3DS's settings of version 0x02000000 end before
// the regions' size.
constexpr std::size_t kSettingsSize = 0x44;
constexpr std::size_t kSettingsWithoutRegions = 0x38;
constexpr std::size_t kOriginalLoopSize = 8;
constexpr uint32_t kRegionInfoSize = 0x100;

// The versions that the writer lays out: the Wii U's and the Switch's, then the 3DS's.
constexpr uint32_t kOldestVersion = 0x00030000;
constexpr uint32_t kOriginalLoopVersion = 0x00040000;
constexpr uint32_t kCtrOldestVersion = 0x02000000;
constexpr uint32_t kCtrRegionsVersion = 0x02010000;
constexpr uint32_t kCtrOriginalLoopVersion = 0x02030000;
constexpr uint32_t kCtrNewestVersion = 0x02030100;

// The samples past a loop's end that the 3DS's tools encode, and those past the end of a stream without a loop in
// version 0x02000000. That version's count of the last block's samples leaves out the last 127.
constexpr uint32_t kCtrPastLoopEnd = 128;
constexpr uint32_t kCtrPastEnd = 1;
constexpr uint32_t kCtrUncounted = 127;

// The layout of a version's settings: their size, and whether they hold the regions' size and reference and the
// loop's original start and end.
struct Layout
{
    std::size_t settings = kSettingsSize;
    bool regions = true;
    bool original_loop = true;
};

// The layout of version `version` of `flavor`'s files. Throws std::invalid_argument for a version that the writer has
// no layout for.
Layout LayoutOf(Flavor flavor, uint32_t version)
{
    if (flavor == Flavor::kCtr)
    {
        switch (version)
        {
        case kCtrOldestVersion:
            return {kSettingsWithoutRegions, false, false};
        case kCtrRegionsVersion:
            return {kSettingsSize, true, false};
        case kCtrOriginalLoopVersion:
        case kCtrNewestVersion:
            return {kSettingsSize, true, true};
        default:
            throw std::invalid_argument(
                "a stream of the 3DS of version 0x02000000, 0x02010000, 0x02030000 or 0x02030100: the writer has no "
                "layout for another");
        }
    }
    if (version != kOldestVersion && version != kOriginalLoopVersion)
    {
        throw std::invalid_argument(
            "a stream of version 0x00030000 or 0x00040000: the writer has no layout for another");
    }

    return {kSettingsSize, true, version == kOriginalLoopVersion};
}

using detail::AppendHeader;
using detail::kDataReference;
using detail::kDspAdpcmInfoSize;
using detail::kDspAdpcmReference;
using detail::kListReference;
using detail::kNone;
using detail::kTableReference;
using detail::OpenFile;
using detail::Out;
using detail::PaddedPieceSize;
using detail::SetBlock;
using detail::StartBlock;
using detail::StreamBytes;

// The offset and the size of block `type`, named `name`. Throws xsflib::FormatError for a file without it.
std::pair<std::size_t, std::size_t> BlockOf(const xsflib::Reader& r, uint16_t type, const char* name)
{
    if (const std::optional<std::pair<std::size_t, std::size_t>> block = detail::FindBlock(r, type))
    {
        return *block;
    }

    throw xsflib::FormatError(std::string("a stream file without its ") + name + " block");
}

// Where the reference at `at` leads, counted from `base`, or nothing for a reference to nothing. Throws
// xsflib::FormatError for a reference of another type than `type`.
std::optional<std::size_t> Follow(const xsflib::Reader& r, std::size_t at, std::size_t base, uint16_t type,
                                  const char* what)
{
    const uint16_t found = r.U16(at);
    const uint32_t offset = r.U32(at + 4);
    if (found == 0 && offset == kNone)
    {
        return std::nullopt;
    }
    if (found != type)
    {
        throw xsflib::FormatError(std::string("a stream file whose reference to ") + what + " has another type");
    }

    return base + offset;
}

// Where the reference at `at` leads, as Follow gives it. Throws xsflib::FormatError for a reference to nothing.
std::size_t Required(const xsflib::Reader& r, std::size_t at, std::size_t base, uint16_t type, const char* what)
{
    const std::optional<std::size_t> place = Follow(r, at, base, type, what);
    if (!place)
    {
        throw xsflib::FormatError(std::string("a stream file without ") + what);
    }

    return *place;
}

// The bytes of a channel's piece of the last block in DATA: the last block's size rounded up. The piece holds a
// DSP-ADPCM stream's last frame whole: 32 is a multiple of a frame's 8 bytes.
uint32_t LastPieceSize(const StreamFile& stream)
{
    return PaddedPieceSize(stream.LastBlockSize());
}

// The samples of the last block up to the stream's end, the uncounted ones included.
uint32_t LastSamples(const StreamFile& stream)
{
    return detail::StreamLastBlockSamples(stream.sample_count, stream.block_samples);
}

// The samples past the end that the last block holds: as many as it has room for.
uint32_t PastEndInBlock(const StreamFile& stream)
{
    return std::min(stream.samples_past_end,
                    stream.block_samples - std::min(stream.block_samples, LastSamples(stream)));
}

// The bytes of each channel's pieces in DATA, the piece after the last block included.
std::size_t PiecesSize(const StreamFile& stream)
{
    const uint32_t blocks = stream.BlockCount();
    return blocks == 0 ? 0
                       : std::size_t{blocks - 1} * stream.BlockSize() + LastPieceSize(stream) + stream.OverflowSize();
}

// The samples past the end that the last block holds, from its size. The 3DS's tools encode 128 after a loop, so a
// count of 128 comes first when it gives the size. Otherwise the fewest that give it. Throws xsflib::FormatError for a
// size that no count gives.
uint32_t PastEndOfSize(WaveEncoding encoding, uint32_t last_samples, uint32_t room, uint32_t size)
{
    if (kCtrPastLoopEnd <= room && StreamBytes(encoding, last_samples + kCtrPastLoopEnd) == size)
    {
        return kCtrPastLoopEnd;
    }
    for (uint32_t past = 0; past <= room; ++past)
    {
        if (StreamBytes(encoding, last_samples + past) == size)
        {
            return past;
        }
    }

    throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
}

// The samples in a piece of `size` bytes after a full last block that holds `in_block` samples past the end. The
// 3DS's tools encode 128 past the end in all, so the rest of them come first when they fill the piece. Otherwise the
// fewest that fill it. Returns nothing for a size that no count fills.
std::optional<uint32_t> OverflowOfSize(WaveEncoding encoding, uint32_t in_block, std::size_t size)
{
    if (in_block < kCtrPastLoopEnd && PaddedPieceSize(StreamBytes(encoding, kCtrPastLoopEnd - in_block)) == size)
    {
        return kCtrPastLoopEnd - in_block;
    }
    for (uint32_t samples = 1; StreamBytes(encoding, samples) <= size; ++samples)
    {
        if (PaddedPieceSize(StreamBytes(encoding, samples)) == size)
        {
            return samples;
        }
    }

    return std::nullopt;
}

// Reads the table of tracks at `table`.
std::vector<StreamFile::Track> ReadTracks(const xsflib::Reader& r, std::size_t table)
{
    std::vector<StreamFile::Track> tracks;
    const uint32_t count = r.U32(table);
    r.Sub(table + 4, std::size_t{count} * 8);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t at = Required(r, table + 4 + std::size_t{i} * 8, table, kTrackInfoReference, "a track");
        StreamFile::Track& track = tracks.emplace_back();
        track.volume = r.U8(at);
        track.pan = r.U8(at + 1);
        track.span = r.U8(at + 2);
        track.flags = r.U8(at + 3);
        const std::optional<std::size_t> indices = Follow(r, at + 4, at, kListReference, "a track's channels");
        if (indices)
        {
            const std::span<const uint8_t> channels = r.Sub(*indices + 4, r.U32(*indices));
            track.channels.assign(channels.begin(), channels.end());
        }
    }

    return tracks;
}

// The count of entries of the SEEK block for each channel of `stream`: one for each block, and one more when pieces
// follow the last block.
std::size_t SeekEntries(const StreamFile& stream)
{
    return std::size_t{stream.BlockCount()} + (stream.OverflowSize() != 0 ? 1 : 0);
}

// Checks that the writer can lay `stream` out, and returns the layout of its version. Throws std::invalid_argument for
// a stream that the writer can't lay out.
Layout CheckedLayout(const StreamFile& stream)
{
    if (stream.encoding != WaveEncoding::kPcm8 && stream.encoding != WaveEncoding::kPcm16 &&
        stream.encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
    }
    const Layout layout = LayoutOf(stream.flavor, stream.version);
    if (stream.channels.empty() || stream.channels.size() > std::numeric_limits<uint8_t>::max() ||
        stream.sample_count == 0 || stream.block_samples != StreamBlockSamples(stream.encoding))
    {
        throw std::invalid_argument("a stream with channels, samples and blocks of 8 KiB");
    }
    if (stream.uncounted_samples >= LastSamples(stream))
    {
        throw std::invalid_argument("a stream whose last block's count leaves out all of its samples");
    }
    const std::size_t pieces = PiecesSize(stream);
    const std::size_t entries = SeekEntries(stream);
    for (const StreamFile::Channel& channel : stream.channels)
    {
        if (channel.adpcm.has_value() != (stream.encoding == WaveEncoding::kDspAdpcm))
        {
            throw std::invalid_argument(
                "a DSP-ADPCM info for each channel of a DSP-ADPCM stream, and for no PCM channel");
        }
        if (channel.samples.size() != pieces || channel.history.size() != entries)
        {
            throw std::invalid_argument("a stream whose channels' samples or history don't fit its settings");
        }
    }
    for (const StreamFile::Track& track : stream.tracks.value_or(std::vector<StreamFile::Track>()))
    {
        for (const uint8_t index : track.channels)
        {
            if (index >= stream.channels.size())
            {
                throw std::invalid_argument("a track that plays a channel that the stream lacks");
            }
        }
    }

    return layout;
}

// Appends the settings of `stream` in the layout `layout`, and sets the reference to them at `base`, the start of
// INFO's references.
void AppendSettings(Out& out, std::size_t base, const StreamFile& stream, const Layout& layout)
{
    const std::size_t settings = out.Zeros(layout.settings + (layout.original_loop ? kOriginalLoopSize : 0));
    out.Reference(base, kSettingsReference, static_cast<uint32_t>(settings - base));
    out.U8(settings, static_cast<uint8_t>(stream.encoding));
    out.U8(settings + 1, stream.loop ? 1 : 0);
    out.U8(settings + 2, static_cast<uint8_t>(stream.channels.size()));
    out.U32(settings + 4, stream.sample_rate);
    out.U32(settings + 8, stream.loop_start);
    out.U32(settings + 12, stream.sample_count);
    out.U32(settings + 16, stream.BlockCount());
    out.U32(settings + 20, stream.BlockSize());
    out.U32(settings + 24, stream.block_samples);
    out.U32(settings + 28, stream.LastBlockSize());
    out.U32(settings + 32, stream.LastBlockSamples());
    out.U32(settings + 36, LastPieceSize(stream));
    out.U32(settings + 40, 4);
    out.U32(settings + 44, stream.block_samples);
    out.Reference(settings + 48, kDataReference, kAlignment - 8);
    if (layout.regions)
    {
        out.U16(settings + 56, kRegionInfoSize);
        out.Reference(settings + 60, 0, kNone);
    }
    if (layout.original_loop)
    {
        out.U32(settings + layout.settings, stream.original_loop_start);
        out.U32(settings + layout.settings + 4, stream.original_loop_end);
    }
}

// Appends the info of each of `tracks` with its channels, and sets the references to them in the table at
// `track_table`.
void AppendTrackInfos(Out& out, std::size_t track_table, const std::vector<StreamFile::Track>& tracks)
{
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        const StreamFile::Track& track = tracks[i];
        const std::size_t at = out.Zeros(12);
        out.Reference(track_table + 4 + i * 8, kTrackInfoReference, static_cast<uint32_t>(at - track_table));
        out.U8(at, track.volume);
        out.U8(at + 1, track.pan);
        out.U8(at + 2, track.span);
        out.U8(at + 3, track.flags);
        const std::size_t indices = out.Zeros(4);
        out.Reference(at + 4, kListReference, static_cast<uint32_t>(indices - at));
        out.U32(indices, static_cast<uint32_t>(track.channels.size()));
        out.Append(track.channels);
        out.Align(4);
    }
}

// Appends the info of each of `channels`, then their DSP-ADPCM infos, and sets the references to the infos in the
// table at `table`.
void AppendChannelInfos(Out& out, std::size_t table, const std::vector<StreamFile::Channel>& channels)
{
    std::vector<std::size_t> infos;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        infos.push_back(out.Zeros(8));
        out.Reference(table + 4 + i * 8, kChannelInfoReference, static_cast<uint32_t>(infos[i] - table));
    }
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        if (!channels[i].adpcm)
        {
            out.Reference(infos[i], 0, kNone);
            continue;
        }

        const std::size_t a = out.Zeros(kDspAdpcmInfoSize);
        out.Reference(infos[i], kDspAdpcmReference, static_cast<uint32_t>(a - infos[i]));
        detail::WriteDspAdpcm(out, a, *channels[i].adpcm);
    }
}

// Appends the INFO block of `stream` in the layout `layout`.
void AppendInfo(Out& out, const StreamFile& stream, const Layout& layout)
{
    // INFO: the three references, the settings, the tracks, the channels' table and infos, then the DSP-ADPCM infos.
    const std::size_t info = StartBlock(out, "INFO");
    const std::size_t base = out.Zeros(3 * 8);
    AppendSettings(out, base, stream, layout);

    // The tables of tracks and of channels come first, then each track's info with its channels, as the 3DS's tools
    // lay them out.
    std::optional<std::size_t> track_table;
    if (stream.tracks)
    {
        track_table = out.Zeros(4 + stream.tracks->size() * 8);
        out.Reference(base + 8, kTableReference, static_cast<uint32_t>(*track_table - base));
        out.U32(*track_table, static_cast<uint32_t>(stream.tracks->size()));
    }
    else
    {
        out.Reference(base + 8, 0, kNone);
    }
    const std::size_t table = out.Zeros(4 + stream.channels.size() * 8);
    out.Reference(base + 16, kTableReference, static_cast<uint32_t>(table - base));
    out.U32(table, static_cast<uint32_t>(stream.channels.size()));
    if (track_table)
    {
        AppendTrackInfos(out, *track_table, *stream.tracks);
    }
    AppendChannelInfos(out, table, stream.channels);
    out.Align(kAlignment);
    SetBlock(out, 0, kInfoBlock, info);
}

// Appends the SEEK block of `stream`.
void AppendSeek(Out& out, const StreamFile& stream)
{
    const std::size_t entries = SeekEntries(stream);
    // SEEK: the two samples before each block of each channel, and before each piece after the last block,
    // little-endian.
    const std::size_t seek = StartBlock(out, "SEEK");
    std::vector<uint8_t> history(entries * stream.channels.size() * 4);
    for (std::size_t b = 0; b < entries; ++b)
    {
        for (std::size_t c = 0; c < stream.channels.size(); ++c)
        {
            uint8_t* entry = &history[(b * stream.channels.size() + c) * 4];
            xsflib::StoreLe16(entry, static_cast<uint16_t>(stream.channels[c].history[b][0]));
            xsflib::StoreLe16(entry + 2, static_cast<uint16_t>(stream.channels[c].history[b][1]));
        }
    }
    out.Append(history);
    out.Align(kAlignment);
    SetBlock(out, 1, kSeekBlock, seek);
}

// Appends the DATA block of `stream`.
void AppendData(Out& out, const StreamFile& stream)
{
    const uint32_t blocks = stream.BlockCount();
    const std::size_t pieces = PiecesSize(stream);
    // DATA: each block's pieces, from 32 bytes after the block's start, then the pieces after the last block.
    const std::size_t data = StartBlock(out, "DATA");
    out.Align(kAlignment);
    const std::size_t piece = stream.BlockSize();
    const std::size_t last = LastPieceSize(stream);
    for (uint32_t b = 0; b < blocks; ++b)
    {
        const bool final = b + 1 == blocks;
        for (const StreamFile::Channel& channel : stream.channels)
        {
            out.Append(std::span(channel.samples).subspan(b * piece, final ? last : piece));
        }
    }
    const std::size_t overflow = stream.OverflowSize();
    for (const StreamFile::Channel& channel : stream.channels)
    {
        out.Append(std::span(channel.samples).subspan(pieces - overflow, overflow));
    }
    out.Align(kAlignment);
    SetBlock(out, 2, kDataBlock, data);
}

// What a stream file's settings give for its blocks beside the values that StreamFile keeps: the count of channels,
// the blocks' count and size, the last block's size, samples and padded size, the size of an entry of SEEK, and where
// the samples start.
struct BlockSettings
{
    uint8_t channel_count = 0;
    uint32_t block_count = 0;
    uint32_t block_size = 0;
    uint32_t last_size = 0;
    uint32_t last_samples = 0;
    uint32_t last_padded = 0;
    uint32_t seek_entry = 0;
    std::size_t samples = 0;
};

// Where a stream file's table of channels, SEEK and DATA start, and the sizes of SEEK and DATA.
struct ChannelPlaces
{
    std::size_t channels = 0;
    std::size_t seek = 0;
    std::size_t seek_size = 0;
    std::size_t data = 0;
    std::size_t data_size = 0;
};

// Reads the settings at `settings` into `s`, and returns the values of the blocks. DATA starts at `data`. Throws
// xsflib::FormatError for an encoding other than PCM and DSP-ADPCM, a stream with regions, and a reference to the
// samples of another type.
BlockSettings ReadSettings(const xsflib::Reader& r, std::size_t settings, std::size_t data, StreamFile& s)
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
    if (r.U8(settings + 3) != 0)
    {
        throw xsflib::FormatError("a stream with regions");
    }

    s.sample_rate = r.U32(settings + 4);
    s.loop_start = r.U32(settings + 8);
    s.sample_count = r.U32(settings + 12);
    blocks.block_count = r.U32(settings + 16);
    blocks.block_size = r.U32(settings + 20);
    s.block_samples = r.U32(settings + 24);
    blocks.last_size = r.U32(settings + 28);
    blocks.last_samples = r.U32(settings + 32);
    blocks.last_padded = r.U32(settings + 36);
    blocks.seek_entry = r.U32(settings + 40);
    blocks.samples = Required(r, settings + 48, data + 8, kDataReference, "the samples");

    return blocks;
}

// Reads each channel of `s`: its info, its pieces of DATA and its history in SEEK. `file` is the file that `r` reads.
// A full last block leaves the rest of the samples past the end to a piece of each channel after it, and SEEK holds an
// entry for each piece. Throws xsflib::FormatError for a table of channels with another count than the settings,
// blocks that don't add up to the samples or run past DATA or SEEK, and a DSP-ADPCM channel without its DSP-ADPCM info.
void ReadChannels(const xsflib::Reader& r, std::span<const uint8_t> file, const ChannelPlaces& places,
                  const BlockSettings& blocks, StreamFile& s)
{
    const uint8_t channel_count = blocks.channel_count;
    const uint32_t block_count = blocks.block_count;
    if (r.U32(places.channels) != channel_count)
    {
        throw xsflib::FormatError("a stream whose table of channels has another count");
    }
    const std::size_t piece = blocks.block_size;
    const std::size_t stride = piece * channel_count;
    const std::size_t last = blocks.last_padded;
    if (block_count == 0 || last < blocks.last_size)
    {
        throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
    }
    const std::size_t all = (block_count - 1) * stride + last * channel_count;
    if (blocks.samples + all > places.data + places.data_size)
    {
        throw xsflib::FormatError("a stream whose blocks run past DATA or SEEK");
    }

    std::size_t overflow = 0;
    const uint32_t counted = LastSamples(s);
    const uint32_t room = s.block_samples - counted;
    const std::size_t rest = places.data + places.data_size - (blocks.samples + all);
    if (rest != 0 && rest % (std::size_t{channel_count} * kAlignment) == 0 &&
        StreamBytes(s.encoding, counted + room) == blocks.last_size)
    {
        const std::optional<uint32_t> more = OverflowOfSize(s.encoding, room, rest / channel_count);
        if (more)
        {
            overflow = rest / channel_count;
            s.samples_past_end = room + *more;
        }
    }
    const std::size_t entries = std::size_t{block_count} + (overflow != 0 ? 1 : 0);
    if (places.seek + 8 + entries * channel_count * 4 > places.seek + places.seek_size)
    {
        throw xsflib::FormatError("a stream whose blocks run past DATA or SEEK");
    }

    for (uint8_t c = 0; c < channel_count; ++c)
    {
        const std::size_t channel_info =
            Required(r, places.channels + 4 + std::size_t{c} * 8, places.channels, kChannelInfoReference, "a channel");
        StreamFile::Channel& channel = s.channels.emplace_back();
        const std::optional<std::size_t> adpcm =
            Follow(r, channel_info, channel_info, kDspAdpcmReference, "a channel's DSP-ADPCM info");
        if (adpcm)
        {
            channel.adpcm = detail::ReadDspAdpcm(r, *adpcm);
        }
        else if (s.encoding == WaveEncoding::kDspAdpcm)
        {
            throw xsflib::FormatError("a DSP-ADPCM channel without its DSP-ADPCM info");
        }

        channel.samples.reserve((block_count - 1) * piece + last + overflow);
        for (uint32_t b = 0; b < block_count; ++b)
        {
            const bool final = b + 1 == block_count;
            const std::span<const uint8_t> bytes =
                r.Sub(blocks.samples + b * stride + c * (final ? last : piece), final ? last : piece);
            channel.samples.insert(channel.samples.end(), bytes.begin(), bytes.end());
        }
        if (overflow != 0)
        {
            const std::span<const uint8_t> bytes = r.Sub(blocks.samples + all + c * overflow, overflow);
            channel.samples.insert(channel.samples.end(), bytes.begin(), bytes.end());
        }

        // SEEK holds its samples little-endian in both byte orders.
        for (std::size_t b = 0; b < entries; ++b)
        {
            const std::size_t entry = places.seek + 8 + (b * channel_count + c) * 4;
            channel.history.push_back({static_cast<int16_t>(xsflib::LoadLe16(&file[entry])),
                                       static_cast<int16_t>(xsflib::LoadLe16(&file[entry + 2]))});
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
    return detail::StreamBlockCount(sample_count, block_samples);
}

uint32_t StreamFile::LastBlockSamples() const
{
    const uint32_t last = LastSamples(*this);
    return last - std::min(last, uncounted_samples);
}

uint32_t StreamFile::LastBlockSize() const
{
    return StreamBytes(encoding, LastSamples(*this) + PastEndInBlock(*this));
}

uint32_t StreamFile::OverflowSamples() const
{
    return samples_past_end - PastEndInBlock(*this);
}

uint32_t StreamFile::OverflowSize() const
{
    const uint32_t overflow = OverflowSamples();
    return overflow == 0 ? 0 : PaddedPieceSize(StreamBytes(encoding, overflow));
}

StreamFile StreamFile::Read(std::span<const uint8_t> file)
{
    const auto [flavor, r] = OpenFile(file, "STM", "stream file");
    const auto [info, info_size] = BlockOf(r, kInfoBlock, "INFO");
    const auto [seek, seek_size] = BlockOf(r, kSeekBlock, "SEEK");
    const auto [data, data_size] = BlockOf(r, kDataBlock, "DATA");
    r.Sub(info, info_size);
    r.Sub(seek, seek_size);
    r.Sub(data, data_size);

    // The settings.
    StreamFile s;
    s.flavor = flavor;
    s.version = r.U32(8);
    const std::size_t base = info + 8;
    const std::size_t settings = Required(r, base, base, kSettingsReference, "the settings");
    const BlockSettings blocks = ReadSettings(r, settings, data, s);

    // The tables follow the settings. Settings that reach the first of them hold the loop's original start and end.
    // Settings without them give the loop's start and the stream's length: the tools write those for a loop that they
    // didn't move.
    const std::optional<std::size_t> tracks = Follow(r, base + 8, base, kTableReference, "the tracks");
    const std::size_t channels = Required(r, base + 16, base, kTableReference, "the channels");
    const std::size_t settings_end = std::min(tracks.value_or(channels), channels);
    if (settings_end >= settings + kSettingsSize + kOriginalLoopSize)
    {
        s.original_loop_start = r.U32(settings + kSettingsSize);
        s.original_loop_end = r.U32(settings + kSettingsSize + 4);
    }
    else
    {
        s.original_loop_start = s.loop_start;
        s.original_loop_end = s.sample_count;
    }
    // The last block's samples that its count leaves out, and the samples past the end that the block holds.
    const uint32_t counted = LastSamples(s);
    if (s.block_samples == 0 || blocks.block_count != s.BlockCount() || blocks.block_size != s.BlockSize() ||
        blocks.last_samples > counted || blocks.seek_entry != 4)
    {
        throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
    }
    s.uncounted_samples = counted - blocks.last_samples;
    const uint32_t room = s.block_samples - counted;
    s.samples_past_end = PastEndOfSize(s.encoding, counted, room, blocks.last_size);
    if (tracks)
    {
        s.tracks = ReadTracks(r, *tracks);
    }

    ReadChannels(r, file, {channels, seek, seek_size, data, data_size}, blocks, s);

    return s;
}

std::vector<uint8_t> StreamFile::Write() const
{
    const Layout layout = CheckedLayout(*this);
    Out out(flavor == Flavor::kCafe);
    AppendHeader(out, flavor == Flavor::kCtr ? "CSTM" : "FSTM", version, 3);
    AppendInfo(out, *this, layout);
    AppendSeek(out, *this);
    AppendData(out, *this);
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

uint32_t StreamBlockSamples(WaveEncoding encoding)
{
    return detail::StreamBlockSamplesOf(encoding);
}

wave::Pcm DecodeStream(const StreamFile& stream)
{
    // The wave decoder throws for a DSP-ADPCM channel without its info, and for an IMA-ADPCM stream.
    detail::StreamCoding coding;
    coding.encoding = detail::DecoderEncoding(stream.encoding);
    coding.big_endian = stream.flavor == Flavor::kCafe;
    coding.sum = detail::SumOf(stream.flavor);

    return detail::DecodeStreamChannels(stream, coding, nullptr);
}

StreamFile WithoutSamplesPastEnd(const StreamFile& stream)
{
    StreamFile s = stream;
    s.samples_past_end = 0;
    s.uncounted_samples = 0;
    const uint32_t blocks = s.BlockCount();
    if (blocks == 0)
    {
        return s;
    }

    // The bytes up to the end of the last sample's frame or sample stay.
    std::size_t kept = StreamBytes(s.encoding, LastSamples(s));
    if (s.encoding == WaveEncoding::kDspAdpcm)
    {
        kept = (kept + 7) / 8 * 8;
    }
    kept += std::size_t{blocks - 1} * s.BlockSize();
    for (StreamFile::Channel& channel : s.channels)
    {
        channel.samples.resize(std::min(channel.samples.size(), kept));
        channel.samples.resize(PiecesSize(s), 0);
        channel.history.resize(blocks);
    }

    return s;
}

StreamFile MakeStream(Flavor flavor, uint32_t version, WaveEncoding encoding, uint32_t sample_rate,
                      std::span<const std::vector<int16_t>> channels, std::optional<uint32_t> loop_start)
{
    if (encoding != WaveEncoding::kPcm16 && encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of 16-bit PCM or DSP-ADPCM");
    }
    const std::size_t length = detail::CheckedLength(channels, loop_start);

    // The loop moves forward to a block's start, and the samples from its start follow the end.
    StreamFile s;
    s.flavor = flavor;
    s.version = version;
    s.encoding = encoding;
    s.sample_rate = sample_rate;
    s.block_samples = StreamBlockSamples(encoding);
    std::vector<std::vector<int16_t>> samples(channels.begin(), channels.end());
    s.loop = loop_start.has_value();
    s.original_loop_start = loop_start.value_or(0);
    s.original_loop_end = static_cast<uint32_t>(std::min<std::size_t>(length, std::numeric_limits<uint32_t>::max()));
    if (loop_start)
    {
        // The 3DS's tools move a loop's start to the start of the block after the one that holds it. A loop from the
        // first sample then starts at the second block. The other consoles' tools move it to the first block's start
        // at or after it.
        const uint64_t block = s.block_samples;
        const uint64_t moved = flavor == Flavor::kCtr ? (uint64_t{*loop_start} / block + 1) * block
                                                      : (uint64_t{*loop_start} + block - 1) / block * block;
        s.loop_start = detail::MoveLoop(samples, *loop_start, moved);
    }
    s.sample_count = detail::SampleCount(samples);

    // The samples past the end that the 3DS's tools encode: the loop's first samples, or a sample of 0 after a stream
    // without a loop in version 0x02000000. That version's count of the last block's samples leaves out the last 127.
    if (flavor == Flavor::kCtr && s.loop)
    {
        s.samples_past_end = kCtrPastLoopEnd;
    }
    else if (flavor == Flavor::kCtr && version == kCtrOldestVersion)
    {
        s.samples_past_end = kCtrPastEnd;
        s.uncounted_samples = std::min(kCtrUncounted, LastSamples(s) - 1);
    }
    for (std::vector<int16_t>& channel : samples)
    {
        for (uint32_t i = 0; i < s.samples_past_end; ++i)
        {
            const int16_t sample = s.loop ? channel[s.loop_start + i % (s.sample_count - s.loop_start)] : int16_t{0};
            channel.push_back(sample);
        }
    }

    // Each channel's samples, padded to the last block's piece, and the samples before each block and before the piece
    // after the last block.
    detail::ChannelLayout layout;
    layout.adpcm = encoding == WaveEncoding::kDspAdpcm;
    layout.big_endian = flavor == Flavor::kCafe;
    layout.sum = detail::SumOf(flavor);
    layout.loop_start = s.loop ? s.loop_start : 0;
    layout.size = PiecesSize(s);
    layout.points = SeekEntries(s);
    layout.interval = s.block_samples;
    for (const std::vector<int16_t>& input : samples)
    {
        s.channels.push_back(detail::EncodeChannel<StreamFile::Channel>(input, layout));
    }

    return s;
}

} // namespace ntdlib::sar
