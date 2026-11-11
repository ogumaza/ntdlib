// SPDX-License-Identifier: MIT

#include "ntdlib/sar/wave_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"
#include "sar/file_parts.h"
#include "sar/format_ids.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kWaveInfoBlock = 0x7000;
constexpr uint16_t kWaveDataBlock = 0x7001;
constexpr uint16_t kArchiveInfoBlock = 0x6800;
constexpr uint16_t kArchiveFileBlock = 0x6801;
constexpr uint16_t kChannelInfoReference = 0x7100; // from the table of channels to a channel's info

// The parts of a wave file's INFO block: the settings and a channel's info. A channel's info holds a reference to its
// samples, a reference to its DSP-ADPCM info and 4 reserved bytes. file_parts.h gives the size of a DSP-ADPCM info.
constexpr std::size_t kSettingsSize = 0x14;
constexpr std::size_t kChannelInfoSize = 0x14;

using detail::AppendHeader;
using detail::kBankInfoBlock;
using detail::kBlockAlignment;
using detail::kDataReference;
using detail::kDspAdpcmInfoSize;
using detail::kDspAdpcmReference;
using detail::kListReference;
using detail::kNone;
using detail::kWaveSoundInfoBlock;
using detail::OpenFile;
using detail::Out;
using detail::SetBlock;
using detail::StartBlock;

// The offset and the size of block `type`, named `name`. Throws xsflib::FormatError for a file without it.
std::pair<std::size_t, std::size_t> BlockOf(const xsflib::Reader& r, uint16_t type, const char* name)
{
    if (const std::optional<std::pair<std::size_t, std::size_t>> block = detail::FindBlock(r, type))
    {
        return *block;
    }

    throw xsflib::FormatError(std::string("no ") + name + " block");
}

// The start of the table of waves of `file`, a bank file or a wave sound file of `flavor`, and a reader of the file in
// its byte order. The table holds a count, then a wave archive's item ID and a wave's index for each wave. Throws
// xsflib::FormatError for bytes that aren't such a file, a file of another console, a file without a table of waves,
// and a table past the file's end.
std::pair<std::size_t, xsflib::Reader> WaveTableOf(std::span<const uint8_t> file, Flavor flavor)
{
    // The file's kind and console. INFO's first two references lead to its tables.
    const bool bank = file.size() >= 4 && std::memcmp(file.data() + 1, "BNK", 3) == 0;
    if (!bank && (file.size() < 4 || std::memcmp(file.data() + 1, "WSD", 3) != 0))
    {
        throw xsflib::FormatError("not a bank file or a wave sound file");
    }
    const auto [read_flavor, r] = OpenFile(file, bank ? "BNK" : "WSD", bank ? "bank file" : "wave sound file");
    if (read_flavor != flavor)
    {
        throw xsflib::FormatError("a file of another console");
    }
    const std::size_t info = BlockOf(r, bank ? kBankInfoBlock : kWaveSoundInfoBlock, "INFO").first + 8;
    std::optional<std::size_t> table;
    for (std::size_t i = 0; i < 2; ++i)
    {
        const int32_t offset = r.S32(info + i * 8 + 4);
        if (r.U16(info + i * 8) == kListReference && offset >= 0)
        {
            table = info + static_cast<std::size_t>(offset);
        }
    }
    if (!table)
    {
        throw xsflib::FormatError("a file without a table of waves");
    }
    r.Sub(*table + 4, std::size_t{r.U32(*table)} * 8);

    return {*table, r};
}

} // namespace

WaveFile WaveFile::Read(std::span<const uint8_t> file)
{
    const auto [flavor, r] = OpenFile(file, "WAV", "wave file");
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

            c.adpcm = detail::ReadDspAdpcm(r, channel + r.U32(channel + 12));
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
    AppendHeader(out, flavor == Flavor::kCtr ? "CWAV" : "FWAV", version, 2);

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
            out.Reference(infos[i] + 8, 0, kNone);
            continue;
        }

        const std::size_t a = out.Zeros(kDspAdpcmInfoSize);
        out.Reference(infos[i] + 8, kDspAdpcmReference, static_cast<uint32_t>(a - infos[i]));
        detail::WriteDspAdpcm(out, a, *channels[i].adpcm);
    }
    out.Align(kBlockAlignment);
    SetBlock(out, 0, kWaveInfoBlock, info);

    // DATA: each channel's samples, on the boundary.
    const std::size_t data = StartBlock(out, "DATA");
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        out.Align(detail::FileAlignment(flavor));
        const std::size_t samples = out.Append(channels[i].samples);
        out.Reference(infos[i], kDataReference, static_cast<uint32_t>(samples - (data + 8)));
    }
    SetBlock(out, 1, kWaveDataBlock, data);
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

WaveArchiveFile WaveArchiveFile::Read(std::span<const uint8_t> file)
{
    const auto [flavor, r] = OpenFile(file, "WAR", "wave archive");
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
    AppendHeader(out, flavor == Flavor::kCtr ? "CWAR" : "FWAR", version, 2);

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
        out.Align(detail::FileAlignment(flavor));
        const std::size_t wave = out.Append(waves[i]);
        const std::size_t entry = table + 4 + i * 12;
        out.Reference(entry, kDataReference, static_cast<uint32_t>(wave - (files + 8)));
        out.U32(entry + 8, static_cast<uint32_t>(waves[i].size()));
    }
    SetBlock(out, 1, kArchiveFileBlock, files);
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));

    return out.Take();
}

std::vector<uint8_t> WithWaveArchives(std::span<const uint8_t> file, Flavor flavor,
                                      const std::map<uint32_t, uint32_t>& wave_archives)
{
    // Each wave's wave archive, mapped.
    std::vector<WaveId> waves = WaveTable(file, flavor);
    for (WaveId& wave : waves)
    {
        const auto mapped = wave_archives.find(wave.wave_archive_item);
        if (mapped != wave_archives.end())
        {
            wave.wave_archive_item = mapped->second;
        }
    }

    return WithWaveTable(file, flavor, waves);
}

std::vector<WaveId> WaveTable(std::span<const uint8_t> file, Flavor flavor)
{
    const auto [table, r] = WaveTableOf(file, flavor);
    std::vector<WaveId> waves(r.U32(table));
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        waves[i] = {.wave_archive_item = r.U32(table + 4 + i * 8), .index = r.U32(table + 8 + i * 8)};
    }

    return waves;
}

std::vector<uint8_t> WithWaveTable(std::span<const uint8_t> file, Flavor flavor, std::span<const WaveId> waves)
{
    const auto [table, r] = WaveTableOf(file, flavor);
    if (r.U32(table) != waves.size())
    {
        throw std::invalid_argument("a table of waves of another length");
    }

    std::vector<uint8_t> bytes(file.begin(), file.end());
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        uint8_t* entry = &bytes[table + 4 + i * 8];
        if (r.BigEndian())
        {
            xsflib::StoreBe32(entry, waves[i].wave_archive_item);
            xsflib::StoreBe32(entry + 4, waves[i].index);
        }
        else
        {
            xsflib::StoreLe32(entry, waves[i].wave_archive_item);
            xsflib::StoreLe32(entry + 4, waves[i].index);
        }
    }

    return bytes;
}

} // namespace ntdlib::sar
