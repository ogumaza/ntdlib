// SPDX-License-Identifier: MIT

#include "ntdlib/sar/wave_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kWaveInfoBlock = 0x7000;
constexpr uint16_t kWaveDataBlock = 0x7001;
constexpr uint16_t kArchiveInfoBlock = 0x6800;
constexpr uint16_t kArchiveFileBlock = 0x6801;
constexpr uint16_t kChannelInfoReference = 0x7100; // from the table of channels to a channel's info
constexpr uint16_t kDataReference = 0x1F00;        // to a channel's samples in DATA, or to a wave in FILE
constexpr uint16_t kDspAdpcmReference = 0x0300;    // from a channel's info to its DSP-ADPCM info
constexpr uint32_t kNoOffset = 0xFFFFFFFF;         // a reference to nothing
constexpr std::size_t kBlockAlignment = 32;

// The parts of a wave file's INFO block: the settings, a channel's info and a DSP-ADPCM info. A channel's info holds a
// reference to its samples, a reference to its DSP-ADPCM info and 4 reserved bytes. A DSP-ADPCM info holds the
// coefficients, the context at the start and at the loop, and 2 bytes of padding.
constexpr std::size_t kSettingsSize = 0x14;
constexpr std::size_t kChannelInfoSize = 0x14;
constexpr std::size_t kDspAdpcmInfoSize = 46;

using detail::Out;

// The boundary of a file that a wave's samples and an archive's waves start on.
std::size_t SampleAlignment(Flavor flavor)
{
    return flavor == Flavor::kNx ? 64 : 32;
}

// The flavor of a file of `kind` (WAV or WAR), from its magic and its byte order mark, and a reader in its byte order.
// Throws xsflib::FormatError for another file. `name` names the file in the message.
std::pair<Flavor, xsflib::Reader> Open(std::span<const uint8_t> file, const char* kind, const std::string& name)
{
    if (file.size() < 0x14 || (file[0] != 'C' && file[0] != 'F') || std::memcmp(file.data() + 1, kind, 3) != 0)
    {
        throw xsflib::FormatError("not a " + name + " (C" + kind + " or F" + kind + ")");
    }

    const bool big_endian = file[4] == 0xFE && file[5] == 0xFF;
    if (!big_endian && (file[4] != 0xFF || file[5] != 0xFE))
    {
        throw xsflib::FormatError("a " + name + " without a byte order mark");
    }
    const Flavor flavor = file[0] == 'C' ? Flavor::kCtr : big_endian ? Flavor::kCafe : Flavor::kNx;
    if (flavor == Flavor::kCtr && big_endian)
    {
        throw xsflib::FormatError(std::string("a big-endian C") + kind);
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

    throw xsflib::FormatError(std::string("no ") + name + " block");
}

// The header of a file with two blocks: its magic, its byte order mark, its size, its version and the table of blocks,
// padded. SetBlock fills the table, and the caller sets the file's size.
void AppendHeader(Out& out, const char* magic, uint32_t version)
{
    out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(0x14 + 2 * 12 - 4);
    out.Align(kBlockAlignment);
    out.U16(4, 0xFEFF);
    out.U16(6, static_cast<uint16_t>(out.Size()));
    out.U32(8, version);
    out.U16(0x10, 2);
}

// Appends a block's magic and room for its size. Returns where the block starts.
std::size_t StartBlock(Out& out, const char* magic)
{
    const std::size_t start = out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(4);

    return start;
}

// Ends the block that starts at `start` at the end of `out`: its size, and entry `index` of the table of blocks.
void SetBlock(Out& out, std::size_t index, uint16_t type, std::size_t start)
{
    const auto size = static_cast<uint32_t>(out.Size() - start);
    const std::size_t entry = 0x14 + index * 12;
    out.U32(start + 4, size);
    out.Reference(entry, type, static_cast<uint32_t>(start));
    out.U32(entry + 8, size);
}

} // namespace

WaveFile WaveFile::Read(std::span<const uint8_t> file)
{
    const auto [flavor, r] = Open(file, "WAV", "wave file");
    const auto [info, info_size] = BlockOf(r, kWaveInfoBlock, "INFO");
    const auto [data, data_size] = BlockOf(r, kWaveDataBlock, "DATA");

    // The settings.
    WaveFile w;
    w.flavor = flavor;
    w.version = r.U32(8);
    const std::size_t settings = info + 8;
    const uint8_t encoding = r.U8(settings);
    if (encoding > static_cast<uint8_t>(WaveEncoding::kDspAdpcm))
    {
        throw xsflib::FormatError("a wave of encoding " + std::to_string(encoding) + ", not PCM or DSP-ADPCM");
    }
    w.encoding = static_cast<WaveEncoding>(encoding);
    w.loop = r.U8(settings + 1) != 0;
    w.sample_rate = r.U32(settings + 4);
    w.loop_start = r.U32(settings + 8);
    w.loop_end = r.U32(settings + 12);
    w.original_loop_start = r.U32(settings + 16);

    // Each channel's info, and where its samples start.
    const std::size_t table = settings + kSettingsSize;
    const uint32_t count = r.U32(table);
    std::vector<std::size_t> starts;
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t channel = table + r.U32(table + 4 + std::size_t{i} * 8 + 4);
        starts.push_back(data + 8 + r.U32(channel + 4));
        Channel c;
        if (w.encoding == WaveEncoding::kDspAdpcm)
        {
            if (r.U16(channel + 8) != kDspAdpcmReference)
            {
                throw xsflib::FormatError("a DSP-ADPCM channel without its DSP-ADPCM info");
            }

            const std::size_t a = channel + r.U32(channel + 12);
            DspAdpcmInfo adpcm;
            for (std::size_t k = 0; k < adpcm.coefs.size(); ++k)
            {
                adpcm.coefs[k] = r.S16(a + k * 2);
            }
            adpcm.pred_scale = r.U16(a + 32);
            adpcm.yn1 = r.S16(a + 34);
            adpcm.yn2 = r.S16(a + 36);
            adpcm.loop_pred_scale = r.U16(a + 38);
            adpcm.loop_yn1 = r.S16(a + 40);
            adpcm.loop_yn2 = r.S16(a + 42);
            c.adpcm = adpcm;
        }
        w.channels.push_back(c);
    }

    // The samples. A channel's run up to the next channel's.
    const std::size_t end = data + data_size;
    for (std::size_t i = 0; i < starts.size(); ++i)
    {
        const std::size_t next = i + 1 < starts.size() ? starts[i + 1] : end;
        if (next < starts[i])
        {
            throw xsflib::FormatError("a wave file whose channels' samples don't follow one another");
        }

        const std::span<const uint8_t> samples = r.Sub(starts[i], next - starts[i]);
        w.channels[i].samples.assign(samples.begin(), samples.end());
    }

    return w;
}

std::vector<uint8_t> WaveFile::Write() const
{
    if (encoding != WaveEncoding::kPcm8 && encoding != WaveEncoding::kPcm16 && encoding != WaveEncoding::kDspAdpcm)
    {
        throw std::invalid_argument("a wave file of PCM or DSP-ADPCM");
    }
    for (const Channel& channel : channels)
    {
        if (channel.adpcm.has_value() != (encoding == WaveEncoding::kDspAdpcm))
        {
            throw std::invalid_argument(
                "a DSP-ADPCM info for each channel of a DSP-ADPCM wave, and for no PCM channel");
        }
    }

    Out out(flavor == Flavor::kCafe);
    AppendHeader(out, flavor == Flavor::kCtr ? "CWAV" : "FWAV", version);

    // INFO: the settings, the table of channels, the channels' infos, then the DSP-ADPCM infos.
    const std::size_t info = StartBlock(out, "INFO");
    const std::size_t settings = out.Zeros(kSettingsSize);
    out.U8(settings, static_cast<uint8_t>(encoding));
    out.U8(settings + 1, loop ? 1 : 0);
    out.U32(settings + 4, sample_rate);
    out.U32(settings + 8, loop_start);
    out.U32(settings + 12, loop_end);
    out.U32(settings + 16, original_loop_start);
    const std::size_t table = out.Zeros(4 + channels.size() * 8);
    out.U32(table, static_cast<uint32_t>(channels.size()));
    std::vector<std::size_t> infos;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        infos.push_back(out.Zeros(kChannelInfoSize));
        out.Reference(table + 4 + i * 8, kChannelInfoReference, static_cast<uint32_t>(infos[i] - table));
    }
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        if (!channels[i].adpcm)
        {
            out.Reference(infos[i] + 8, 0, kNoOffset);
            continue;
        }

        const DspAdpcmInfo& adpcm = *channels[i].adpcm;
        const std::size_t a = out.Zeros(kDspAdpcmInfoSize);
        out.Reference(infos[i] + 8, kDspAdpcmReference, static_cast<uint32_t>(a - infos[i]));
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
    out.Align(kBlockAlignment);
    SetBlock(out, 0, kWaveInfoBlock, info);

    // DATA: each channel's samples, on the boundary.
    const std::size_t data = StartBlock(out, "DATA");
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        out.Align(SampleAlignment(flavor));
        const std::size_t samples = out.Append(channels[i].samples);
        out.Reference(infos[i], kDataReference, static_cast<uint32_t>(samples - (data + 8)));
    }
    SetBlock(out, 1, kWaveDataBlock, data);
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

WaveArchiveFile WaveArchiveFile::Read(std::span<const uint8_t> file)
{
    const auto [flavor, r] = Open(file, "WAR", "wave archive");
    const auto [info, info_size] = BlockOf(r, kArchiveInfoBlock, "INFO");
    const auto [files, files_size] = BlockOf(r, kArchiveFileBlock, "FILE");

    WaveArchiveFile archive;
    archive.flavor = flavor;
    archive.version = r.U32(8);
    const uint32_t count = r.U32(info + 8);
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t entry = info + 12 + std::size_t{i} * 12;
        const std::span<const uint8_t> wave = r.Sub(files + 8 + r.U32(entry + 4), r.U32(entry + 8));
        archive.waves.emplace_back(wave.begin(), wave.end());
    }

    return archive;
}

std::vector<uint8_t> WaveArchiveFile::Write() const
{
    Out out(flavor == Flavor::kCafe);
    AppendHeader(out, flavor == Flavor::kCtr ? "CWAR" : "FWAR", version);

    // INFO: each wave's offset and size, which FILE fills in.
    const std::size_t info = StartBlock(out, "INFO");
    const std::size_t table = out.Zeros(4 + waves.size() * 12);
    out.U32(table, static_cast<uint32_t>(waves.size()));
    out.Align(kBlockAlignment);
    SetBlock(out, 0, kArchiveInfoBlock, info);

    // FILE: each wave, on the boundary.
    const std::size_t files = StartBlock(out, "FILE");
    out.Align(kBlockAlignment);
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        out.Align(SampleAlignment(flavor));
        const std::size_t wave = out.Append(waves[i]);
        const std::size_t entry = table + 4 + i * 12;
        out.Reference(entry, kDataReference, static_cast<uint32_t>(wave - (files + 8)));
        out.U32(entry + 8, static_cast<uint32_t>(waves[i].size()));
    }
    SetBlock(out, 1, kArchiveFileBlock, files);
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

} // namespace ntdlib::sar
