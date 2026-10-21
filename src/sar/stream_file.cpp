// SPDX-License-Identifier: MIT

#include "ntdlib/sar/stream_file.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "ntdlib/wave/encode.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kInfoBlock = 0x4000;
constexpr uint16_t kSeekBlock = 0x4001;
constexpr uint16_t kDataBlock = 0x4002;
constexpr uint16_t kSettingsReference = 0x4100;     // from INFO to the stream's settings
constexpr uint16_t kTableReference = 0x0101;        // from INFO to the table of tracks or of channels
constexpr uint16_t kTrackInfoReference = 0x4101;    // from the table of tracks to a track's info
constexpr uint16_t kChannelInfoReference = 0x4102;  // from the table of channels to a channel's info
constexpr uint16_t kChannelIndexReference = 0x0100; // from a track's info to its channels
constexpr uint16_t kDspAdpcmReference = 0x0300;     // from a channel's info to its DSP-ADPCM info
constexpr uint16_t kDataReference = 0x1F00;         // from the settings to the samples in DATA
constexpr uint32_t kNoOffset = 0xFFFFFFFF;          // a reference to nothing
constexpr std::size_t kAlignment = 32;
constexpr uint32_t kBlockBytes = 0x2000;

// The settings: the encoding, the loop, the counts, the rate, the loop's start, the samples, the blocks' sizes and
// samples, the seek's sizes, the reference to the samples, and the regions' size and reference. From version
// 0x00040000 on, the loop's original start and end follow them.
constexpr std::size_t kSettingsSize = 0x44;
constexpr std::size_t kOriginalLoopSize = 8;
constexpr std::size_t kDspAdpcmInfoSize = 46;
constexpr uint32_t kRegionInfoSize = 0x100;

// The versions that the writer lays out.
constexpr uint32_t kOldestVersion = 0x00030000;
constexpr uint32_t kOriginalLoopVersion = 0x00040000;

using detail::Out;

// The flavor of a stream file, from its magic and its byte order mark, and a reader in its byte order. Throws
// xsflib::FormatError for another file.
std::pair<Flavor, xsflib::Reader> Open(std::span<const uint8_t> file)
{
    if (file.size() < 0x14 || (file[0] != 'C' && file[0] != 'F') || std::memcmp(file.data() + 1, "STM", 3) != 0)
    {
        throw xsflib::FormatError("not a stream file (CSTM or FSTM)");
    }

    const bool big_endian = file[4] == 0xFE && file[5] == 0xFF;
    if (!big_endian && (file[4] != 0xFF || file[5] != 0xFE))
    {
        throw xsflib::FormatError("a stream file without a byte order mark");
    }
    const Flavor flavor = file[0] == 'C' ? Flavor::kCtr : big_endian ? Flavor::kCafe : Flavor::kNx;
    if (flavor == Flavor::kCtr && big_endian)
    {
        throw xsflib::FormatError("a big-endian CSTM");
    }

    return {flavor, xsflib::Reader(file, big_endian)};
}

// The offset and the size of block `type`, named `name`. Throws xsflib::FormatError for a file without it.
std::pair<std::size_t, std::size_t> BlockOf(const xsflib::Reader& r, uint16_t type, const char* name)
{
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        if (r.U16(entry) == type)
        {
            return {r.U32(entry + 4), r.U32(entry + 8)};
        }
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
    if (found == 0 && offset == kNoOffset)
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

// The bytes that `samples` samples of `encoding` take, up to the last sample's nibble or byte.
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
            const uint32_t rest = samples % 14;
            return samples / 14 * 8 + (rest == 0 ? 0 : 1 + (rest + 1) / 2);
        }
    case WaveEncoding::kImaAdpcm:
        break;
    }

    throw std::invalid_argument("a stream of PCM or DSP-ADPCM");
}

// `size` rounded up to a multiple of kAlignment.
uint32_t Aligned(uint32_t size)
{
    return static_cast<uint32_t>((uint64_t{size} + kAlignment - 1) / kAlignment * kAlignment);
}

// The bytes of a channel's piece of the last block in DATA: the last block's size rounded up. The piece holds a
// DSP-ADPCM stream's last frame whole: 32 is a multiple of a frame's 8 bytes.
uint32_t LastPieceSize(const StreamFile& stream)
{
    return Aligned(stream.LastBlockSize());
}

// The header of a file with three blocks: its magic, its byte order mark, its size, its version and the table of
// blocks, padded. SetBlock fills the table, and the caller sets the file's size.
void AppendHeader(Out& out, const char* magic, uint32_t version)
{
    out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(0x14 + 3 * 12 - 4);
    out.Align(kAlignment);
    out.U16(4, 0xFEFF);
    out.U16(6, static_cast<uint16_t>(out.Size()));
    out.U32(8, version);
    out.U16(0x10, 3);
}

// Appends a block's magic and room for its size. Returns where the block starts.
std::size_t StartBlock(Out& out, const char* magic)
{
    const std::size_t start = out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(4);

    return start;
}

// Ends the block that starts at `start` at the end of `out`, padded: its size, and entry `index` of the table of
// blocks.
void SetBlock(Out& out, std::size_t index, uint16_t type, std::size_t start)
{
    out.Align(kAlignment);
    const auto size = static_cast<uint32_t>(out.Size() - start);
    const std::size_t entry = 0x14 + index * 12;
    out.U32(start + 4, size);
    out.Reference(entry, type, static_cast<uint32_t>(start));
    out.U32(entry + 8, size);
}

// Reads a DSP-ADPCM info at `at`.
DspAdpcmInfo ReadDspAdpcm(const xsflib::Reader& r, std::size_t at)
{
    DspAdpcmInfo adpcm;
    for (std::size_t k = 0; k < adpcm.coefs.size(); ++k)
    {
        adpcm.coefs[k] = r.S16(at + k * 2);
    }
    adpcm.pred_scale = r.U16(at + 32);
    adpcm.yn1 = r.S16(at + 34);
    adpcm.yn2 = r.S16(at + 36);
    adpcm.loop_pred_scale = r.U16(at + 38);
    adpcm.loop_yn1 = r.S16(at + 40);
    adpcm.loop_yn2 = r.S16(at + 42);

    return adpcm;
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
        const std::optional<std::size_t> indices = Follow(r, at + 4, at, kChannelIndexReference, "a track's channels");
        if (indices)
        {
            const std::span<const uint8_t> channels = r.Sub(*indices + 4, r.U32(*indices));
            track.channels.assign(channels.begin(), channels.end());
        }
    }

    return tracks;
}

} // namespace

uint32_t StreamFile::BlockSize() const
{
    return BytesOf(encoding, block_samples);
}

uint32_t StreamFile::BlockCount() const
{
    return block_samples == 0 ? 0 : static_cast<uint32_t>((uint64_t{sample_count} + block_samples - 1) / block_samples);
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
    const auto [flavor, r] = Open(file);
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
    const uint8_t encoding = r.U8(settings);
    if (encoding > static_cast<uint8_t>(WaveEncoding::kDspAdpcm))
    {
        throw xsflib::FormatError("a stream of encoding " + std::to_string(encoding) + ", not PCM or DSP-ADPCM");
    }
    s.encoding = static_cast<WaveEncoding>(encoding);
    s.loop = r.U8(settings + 1) != 0;
    const uint8_t channel_count = r.U8(settings + 2);
    if (r.U8(settings + 3) != 0)
    {
        throw xsflib::FormatError("a stream with regions");
    }
    s.sample_rate = r.U32(settings + 4);
    s.loop_start = r.U32(settings + 8);
    s.sample_count = r.U32(settings + 12);
    const uint32_t block_count = r.U32(settings + 16);
    const uint32_t block_size = r.U32(settings + 20);
    s.block_samples = r.U32(settings + 24);
    const uint32_t last_size = r.U32(settings + 28);
    const uint32_t last_samples = r.U32(settings + 32);
    const uint32_t last_padded = r.U32(settings + 36);
    const uint32_t seek_entry = r.U32(settings + 40);
    const std::size_t samples = Required(r, settings + 48, data + 8, kDataReference, "the samples");

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
    if (block_count != s.BlockCount() || block_size != s.BlockSize() || last_samples != s.LastBlockSamples() ||
        last_size != s.LastBlockSize() || seek_entry != 4)
    {
        throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
    }
    if (tracks)
    {
        s.tracks = ReadTracks(r, *tracks);
    }

    // Each channel's info, its pieces of DATA and its history in SEEK.
    if (r.U32(channels) != channel_count)
    {
        throw xsflib::FormatError("a stream whose table of channels has another count");
    }
    const std::size_t piece = block_size;
    const std::size_t stride = piece * channel_count;
    const std::size_t last = last_padded;
    if (block_count == 0 || last < last_size)
    {
        throw xsflib::FormatError("a stream whose blocks don't add up to its samples");
    }
    const std::size_t all = (block_count - 1) * stride + last * channel_count;
    if (samples + all > data + data_size || seek + 8 + std::size_t{block_count} * channel_count * 4 > seek + seek_size)
    {
        throw xsflib::FormatError("a stream whose blocks run past DATA or SEEK");
    }
    for (uint8_t c = 0; c < channel_count; ++c)
    {
        const std::size_t channel_info =
            Required(r, channels + 4 + std::size_t{c} * 8, channels, kChannelInfoReference, "a channel");
        Channel& channel = s.channels.emplace_back();
        const std::optional<std::size_t> adpcm =
            Follow(r, channel_info, channel_info, kDspAdpcmReference, "a channel's DSP-ADPCM info");
        if (adpcm)
        {
            channel.adpcm = ReadDspAdpcm(r, *adpcm);
        }
        else if (s.encoding == WaveEncoding::kDspAdpcm)
        {
            throw xsflib::FormatError("a DSP-ADPCM channel without its DSP-ADPCM info");
        }

        channel.samples.reserve((block_count - 1) * piece + last);
        for (uint32_t b = 0; b < block_count; ++b)
        {
            const bool final = b + 1 == block_count;
            const std::span<const uint8_t> bytes =
                r.Sub(samples + b * stride + c * (final ? last : piece), final ? last : piece);
            channel.samples.insert(channel.samples.end(), bytes.begin(), bytes.end());

            // SEEK holds its samples little-endian in both byte orders.
            const std::size_t entry = seek + 8 + (std::size_t{b} * channel_count + c) * 4;
            channel.history.push_back({static_cast<int16_t>(xsflib::LoadLe16(&file[entry])),
                                       static_cast<int16_t>(xsflib::LoadLe16(&file[entry + 2]))});
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
    if (version != kOldestVersion && version != kOriginalLoopVersion)
    {
        throw std::invalid_argument("a stream of version 0x00030000 or 0x00040000: the writer knows no other layout");
    }
    if (channels.empty() || channels.size() > std::numeric_limits<uint8_t>::max() || sample_count == 0 ||
        block_samples != StreamBlockSamples(encoding))
    {
        throw std::invalid_argument("a stream with channels, samples and blocks of 8 KiB");
    }
    const uint32_t blocks = BlockCount();
    const std::size_t pieces = std::size_t{blocks - 1} * BlockSize() + LastPieceSize(*this);
    for (const Channel& channel : channels)
    {
        if (channel.adpcm.has_value() != (encoding == WaveEncoding::kDspAdpcm))
        {
            throw std::invalid_argument(
                "a DSP-ADPCM info for each channel of a DSP-ADPCM stream, and for no PCM channel");
        }
        if (channel.samples.size() != pieces || channel.history.size() != blocks)
        {
            throw std::invalid_argument("a stream whose channels' samples or history don't fit its settings");
        }
    }
    for (const Track& track : tracks.value_or(std::vector<Track>()))
    {
        for (const uint8_t index : track.channels)
        {
            if (index >= channels.size())
            {
                throw std::invalid_argument("a track that plays a channel that the stream lacks");
            }
        }
    }

    Out out(flavor == Flavor::kCafe);
    AppendHeader(out, flavor == Flavor::kCtr ? "CSTM" : "FSTM", version);

    // INFO: the three references, the settings, the tracks, the channels' table and infos, then the DSP-ADPCM infos.
    const std::size_t info = StartBlock(out, "INFO");
    const std::size_t base = out.Zeros(3 * 8);
    const bool original_loop = version == kOriginalLoopVersion;
    const std::size_t settings = out.Zeros(kSettingsSize + (original_loop ? kOriginalLoopSize : 0));
    out.Reference(base, kSettingsReference, static_cast<uint32_t>(settings - base));
    out.U8(settings, static_cast<uint8_t>(encoding));
    out.U8(settings + 1, loop ? 1 : 0);
    out.U8(settings + 2, static_cast<uint8_t>(channels.size()));
    out.U32(settings + 4, sample_rate);
    out.U32(settings + 8, loop_start);
    out.U32(settings + 12, sample_count);
    out.U32(settings + 16, blocks);
    out.U32(settings + 20, BlockSize());
    out.U32(settings + 24, block_samples);
    out.U32(settings + 28, LastBlockSize());
    out.U32(settings + 32, LastBlockSamples());
    out.U32(settings + 36, LastPieceSize(*this));
    out.U32(settings + 40, 4);
    out.U32(settings + 44, block_samples);
    out.Reference(settings + 48, kDataReference, kAlignment - 8);
    out.U16(settings + 56, kRegionInfoSize);
    out.Reference(settings + 60, 0, kNoOffset);
    if (original_loop)
    {
        out.U32(settings + kSettingsSize, original_loop_start);
        out.U32(settings + kSettingsSize + 4, original_loop_end);
    }

    if (tracks)
    {
        const std::size_t table = out.Zeros(4 + tracks->size() * 8);
        out.Reference(base + 8, kTableReference, static_cast<uint32_t>(table - base));
        out.U32(table, static_cast<uint32_t>(tracks->size()));
        for (std::size_t i = 0; i < tracks->size(); ++i)
        {
            const Track& track = (*tracks)[i];
            const std::size_t at = out.Zeros(12);
            out.Reference(table + 4 + i * 8, kTrackInfoReference, static_cast<uint32_t>(at - table));
            out.U8(at, track.volume);
            out.U8(at + 1, track.pan);
            out.U8(at + 2, track.span);
            out.U8(at + 3, track.flags);
            const std::size_t indices = out.Zeros(4);
            out.Reference(at + 4, kChannelIndexReference, static_cast<uint32_t>(indices - at));
            out.U32(indices, static_cast<uint32_t>(track.channels.size()));
            out.Append(track.channels);
            out.Align(4);
        }
    }
    else
    {
        out.Reference(base + 8, 0, kNoOffset);
    }

    const std::size_t table = out.Zeros(4 + channels.size() * 8);
    out.Reference(base + 16, kTableReference, static_cast<uint32_t>(table - base));
    out.U32(table, static_cast<uint32_t>(channels.size()));
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
            out.Reference(infos[i], 0, kNoOffset);
            continue;
        }

        const DspAdpcmInfo& adpcm = *channels[i].adpcm;
        const std::size_t a = out.Zeros(kDspAdpcmInfoSize);
        out.Reference(infos[i], kDspAdpcmReference, static_cast<uint32_t>(a - infos[i]));
        for (std::size_t k = 0; k < adpcm.coefs.size(); ++k)
        {
            out.U16(a + k * 2, static_cast<uint16_t>(adpcm.coefs[k]));
        }
        out.U16(a + 32, adpcm.pred_scale);
        out.U16(a + 34, static_cast<uint16_t>(adpcm.yn1));
        out.U16(a + 36, static_cast<uint16_t>(adpcm.yn2));
        out.U16(a + 38, adpcm.loop_pred_scale);
        out.U16(a + 40, static_cast<uint16_t>(adpcm.loop_yn1));
        out.U16(a + 42, static_cast<uint16_t>(adpcm.loop_yn2));
    }
    SetBlock(out, 0, kInfoBlock, info);

    // SEEK: the two samples before each block of each channel, little-endian.
    const std::size_t seek = StartBlock(out, "SEEK");
    std::vector<uint8_t> history(std::size_t{blocks} * channels.size() * 4);
    for (uint32_t b = 0; b < blocks; ++b)
    {
        for (std::size_t c = 0; c < channels.size(); ++c)
        {
            uint8_t* entry = &history[(b * channels.size() + c) * 4];
            xsflib::StoreLe16(entry, static_cast<uint16_t>(channels[c].history[b][0]));
            xsflib::StoreLe16(entry + 2, static_cast<uint16_t>(channels[c].history[b][1]));
        }
    }
    out.Append(history);
    SetBlock(out, 1, kSeekBlock, seek);

    // DATA: each block's pieces, from 32 bytes after the block's start.
    const std::size_t data = StartBlock(out, "DATA");
    out.Align(kAlignment);
    const std::size_t piece = BlockSize();
    const std::size_t last = LastPieceSize(*this);
    for (uint32_t b = 0; b < blocks; ++b)
    {
        const bool final = b + 1 == blocks;
        for (const Channel& channel : channels)
        {
            out.Append(std::span(channel.samples).subspan(b * piece, final ? last : piece));
        }
    }
    SetBlock(out, 2, kDataBlock, data);
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
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
        return kBlockBytes / 8 * 14;
    case WaveEncoding::kImaAdpcm:
        break;
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
        // Each channel decodes by itself: the wave decoder decodes the first two channels.
        wave::EncodedWave encoded;
        encoded.encoding = stream.encoding == WaveEncoding::kPcm8       ? wave::Encoding::kPcm8
                           : stream.encoding == WaveEncoding::kPcm16    ? wave::Encoding::kPcm16
                           : stream.encoding == WaveEncoding::kDspAdpcm ? wave::Encoding::kDspAdpcm
                                                                        : wave::Encoding::kUnknown;
        encoded.big_endian = stream.flavor == Flavor::kCafe;
        encoded.sum = stream.flavor == Flavor::kCtr ? wave::DspAdpcmSum::kWide : wave::DspAdpcmSum::kWrap;
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
        pcm.channels.push_back(std::move(wave::DecodeWave(encoded).channels.front()));
    }

    return pcm;
}

StreamFile MakeStream(Flavor flavor, uint32_t version, WaveEncoding encoding, uint32_t sample_rate,
                      std::span<const std::vector<int16_t>> channels, std::optional<uint32_t> loop_start)
{
    if (encoding != WaveEncoding::kPcm16 && encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a stream of 16-bit PCM or DSP-ADPCM");
    }
    if (channels.empty() || channels.size() > std::numeric_limits<uint8_t>::max())
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
        const uint64_t moved = (uint64_t{*loop_start} + s.block_samples - 1) / s.block_samples * s.block_samples;
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

    // Each channel's samples, padded to the last block's piece, and the samples before each block.
    const uint32_t blocks = s.BlockCount();
    const std::size_t pieces = std::size_t{blocks - 1} * s.BlockSize() + LastPieceSize(s);
    const wave::DspAdpcmSum sum = flavor == Flavor::kCtr ? wave::DspAdpcmSum::kWide : wave::DspAdpcmSum::kWrap;
    for (const std::vector<int16_t>& input : samples)
    {
        StreamFile::Channel& channel = s.channels.emplace_back();
        if (encoding == WaveEncoding::kDspAdpcm)
        {
            const wave::DspAdpcmChannel encoded = wave::EncodeDspAdpcm(input, s.loop ? s.loop_start : 0, sum);
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
                flavor == Flavor::kCafe ? xsflib::StoreBe16(&channel.samples[i * 2], static_cast<uint16_t>(input[i]))
                                        : xsflib::StoreLe16(&channel.samples[i * 2], static_cast<uint16_t>(input[i]));
            }
        }
        channel.samples.resize(pieces);
        for (uint32_t b = 0; b < blocks; ++b)
        {
            const std::size_t first = std::size_t{b} * s.block_samples;
            channel.history.push_back(
                {first >= 1 ? input[first - 1] : int16_t{0}, first >= 2 ? input[first - 2] : int16_t{0}});
        }
    }

    return s;
}

} // namespace ntdlib::sar
