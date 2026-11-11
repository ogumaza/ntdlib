// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/wave_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/reference.h"
#include "rsar/file_blocks.h"
#include "rsar/wave_infos.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar
{
namespace detail
{

std::size_t LastSamplesEnd(std::span<const uint8_t> bytes, std::size_t start, std::size_t stop, const WaveFile& wave)
{
    const std::size_t cut = start + SamplesSize(wave.encoding, wave.loop_end);
    const bool padding = cut < stop && (cut + kBlockAlignment - 1) / kBlockAlignment * kBlockAlignment == stop &&
                         std::all_of(
                             bytes.begin() + static_cast<std::ptrdiff_t>(cut),
                             bytes.begin() + static_cast<std::ptrdiff_t>(stop), [](uint8_t b) { return b == 0; });

    return padding ? cut : stop;
}

std::size_t WaveInfoSize(WaveEncoding encoding, std::size_t channels)
{
    return kWaveInfoSize + channels * (4 + kChannelInfoSize) +
           (encoding == WaveEncoding::kDspAdpcm ? channels * kAdpcmInfoSize : 0);
}

void PutWaveInfo(sar::detail::Out& out, std::size_t at, const WaveFile& wave, uint32_t place,
                 std::span<const uint32_t> channel_offsets)
{
    const std::size_t channels = wave.channels.size();
    if (channels == 0 || channels > 0xFF)
    {
        throw std::invalid_argument("a wave holds " + std::to_string(channels) + " channels");
    }
    if (wave.sample_rate > kMostRate)
    {
        throw std::invalid_argument("a wave's rate passes 24 bits");
    }
    if (wave.loop_end == 0 || wave.loop_start > wave.loop_end ||
        AddressOf(wave.loop_end - 1, wave.encoding) < wave.loop_end - 1)
    {
        throw std::invalid_argument("a wave's loop ends before it starts, or past what its addresses hold");
    }

    const bool adpcm = wave.encoding == WaveEncoding::kDspAdpcm;
    out.U8(at, static_cast<uint8_t>(wave.encoding));
    out.U8(at + 1, wave.loop ? 1 : 0);
    out.U8(at + 2, static_cast<uint8_t>(channels));
    WriteRate(out, at + 3, wave.sample_rate);
    out.U32(at + 8, AddressOf(wave.loop_start, wave.encoding));
    out.U32(at + 0xC, AddressOf(wave.loop_end - 1, wave.encoding));
    out.U32(at + 0x10, static_cast<uint32_t>(kWaveInfoSize));
    out.U32(at + 0x14, place);

    const std::size_t infos = kWaveInfoSize + channels * 4;
    const std::size_t adpcm_infos = infos + channels * kChannelInfoSize;
    for (std::size_t c = 0; c < channels; ++c)
    {
        const WaveFile::Channel& channel = wave.channels[c];
        if (channel.adpcm.has_value() != adpcm)
        {
            throw std::invalid_argument(adpcm ? "a DSP-ADPCM channel has no DSP-ADPCM info"
                                              : "a PCM channel has a DSP-ADPCM info");
        }

        const std::size_t info = infos + c * kChannelInfoSize;
        out.U32(at + kWaveInfoSize + c * 4, static_cast<uint32_t>(info));
        out.U32(at + info, channel_offsets[c]);
        for (std::size_t v = 0; v < channel.volumes.size(); ++v)
        {
            out.U32(at + info + 8 + v * 4, channel.volumes[v]);
        }
        if (!adpcm)
        {
            continue;
        }

        const std::size_t a = adpcm_infos + c * kAdpcmInfoSize;
        out.U32(at + info + 4, static_cast<uint32_t>(a));
        WriteDspAdpcm(out, at + a, *channel.adpcm);
    }
}

ReadInfo ReadWaveInfo(const xsflib::Reader& r, std::size_t offset)
{
    ReadInfo read;
    WaveFile& wave = read.wave;
    const uint8_t encoding = r.U8(offset);
    if (encoding > static_cast<uint8_t>(WaveEncoding::kDspAdpcm))
    {
        throw xsflib::FormatError("a wave has an unknown encoding (" + std::to_string(encoding) + ")");
    }

    wave.encoding = static_cast<WaveEncoding>(encoding);
    wave.loop = r.U8(offset + 1) != 0;
    const uint8_t channels = r.U8(offset + 2);
    wave.sample_rate = ReadRate(r, offset + 3);
    wave.loop_start = SampleAt(r.U32(offset + 8), wave.encoding);
    wave.loop_end = SampleAt(r.U32(offset + 0xC), wave.encoding) + 1;
    read.place = r.U32(offset + 0x14);

    const std::size_t table = offset + r.U32(offset + 0x10);
    for (uint8_t c = 0; c < channels; ++c)
    {
        const std::size_t info = offset + r.U32(table + c * std::size_t{4});
        WaveFile::Channel& channel = wave.channels.emplace_back();
        read.channel_offsets.push_back(r.U32(info));
        for (std::size_t v = 0; v < channel.volumes.size(); ++v)
        {
            channel.volumes[v] = r.U32(info + 8 + v * 4);
        }

        const uint32_t adpcm = r.U32(info + 4);
        if (adpcm == 0)
        {
            continue;
        }

        channel.adpcm = ReadDspAdpcm(r, offset + adpcm);
    }

    return read;
}

std::vector<WaveFile> ReadRawWaves(const xsflib::Reader& r, std::span<const std::size_t> offsets,
                                   std::span<const uint8_t> wave_data)
{
    std::vector<ReadInfo> infos;
    std::map<std::size_t, std::size_t> ends; // where each channel's samples start, to where the next ones start
    for (const std::size_t offset : offsets)
    {
        ReadInfo& info = infos.emplace_back(ReadWaveInfo(r, offset));
        for (const uint32_t channel : info.channel_offsets)
        {
            const std::size_t start = std::size_t{info.place} + channel;
            if (start > wave_data.size() || !ends.emplace(start, wave_data.size()).second)
            {
                throw xsflib::FormatError(start > wave_data.size()
                                              ? "a wave's samples start past the wave data"
                                              : "two channels' samples start at one place of the wave data");
            }
        }
    }
    for (auto it = ends.begin(); it != ends.end(); ++it)
    {
        const auto next = std::next(it);
        it->second = next == ends.end() ? wave_data.size() : next->first;
    }

    std::vector<WaveFile> waves;
    for (ReadInfo& info : infos)
    {
        for (std::size_t c = 0; c < info.channel_offsets.size(); ++c)
        {
            const std::size_t start = std::size_t{info.place} + info.channel_offsets[c];
            std::size_t stop = ends.at(start);
            if (c + 1 == info.channel_offsets.size())
            {
                stop = LastSamplesEnd(wave_data, start, stop, info.wave);
            }
            const std::span<const uint8_t> samples = wave_data.subspan(start, stop - start);
            info.wave.channels[c].samples.assign(samples.begin(), samples.end());
        }
        waves.push_back(std::move(info.wave));
    }

    return waves;
}

RawWaves LayRawWaves(std::span<const WaveFile> waves)
{
    RawWaves raw;
    sar::detail::Out out(true);
    for (const WaveFile& wave : waves)
    {
        out.Align(kBlockAlignment);
        const auto place = static_cast<uint32_t>(out.Size());
        std::vector<uint32_t>& offsets = raw.channel_offsets.emplace_back();
        for (const WaveFile::Channel& channel : wave.channels)
        {
            offsets.push_back(static_cast<uint32_t>(out.Size() - place));
            out.Append(channel.samples);
        }
        raw.places.push_back(place);
    }
    out.Align(kBlockAlignment);
    raw.wave_data = out.Take();

    return raw;
}

std::vector<WaveFile> ReadArchivedWaves(std::span<const uint8_t> wave_data)
{
    std::vector<WaveFile> waves;
    for (const std::vector<uint8_t>& rwav : WaveArchiveFile::Read(wave_data).waves)
    {
        waves.push_back(WaveFile::Read(rwav));
    }

    return waves;
}

std::vector<uint8_t> ArchiveWaves(std::span<const WaveFile> waves)
{
    WaveArchiveFile archive;
    for (const WaveFile& wave : waves)
    {
        archive.waves.push_back(wave.Write());
    }

    return archive.Write();
}

} // namespace detail

namespace
{

constexpr std::size_t kFirstWave = 0x20; // where an RWAR's waves start in its DATA block
constexpr std::size_t kTableEntry = 12;

using detail::BlockOf;
using detail::CloseBlock;
using detail::CloseFile;
using detail::Header;
using detail::kBlockAlignment;
using detail::kBlockHeader;
using detail::OpenBlock;
using detail::VersionOf;
using sar::detail::Out;

} // namespace

WaveFile WaveFile::Read(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    const uint16_t version = VersionOf(r, "RWAV");
    const auto [info, info_size] = BlockOf(r, 0, "INFO", "RWAV");
    const auto [data, data_size] = BlockOf(r, 1, "DATA", "RWAV");
    detail::ReadInfo read = detail::ReadWaveInfo(r, info + kBlockHeader);
    WaveFile wave = std::move(read.wave);
    wave.version = version;

    // Each channel's samples run to the next channel's. The last channel's run to DATA's end, without the zeros that
    // pad DATA to 32 bytes past the wave's last sample.
    const std::size_t samples = data + kBlockHeader;
    const std::size_t end = std::min(r.Size(), data + std::max(data_size, kBlockHeader));
    for (std::size_t c = 0; c < wave.channels.size(); ++c)
    {
        const std::size_t start = samples + read.channel_offsets[c];
        std::size_t stop = c + 1 < wave.channels.size() ? samples + read.channel_offsets[c + 1] : end;
        if (start > stop || stop > end)
        {
            throw xsflib::FormatError("an RWAV file's channels don't follow one another");
        }
        if (c + 1 == wave.channels.size())
        {
            stop = detail::LastSamplesEnd(file, start, stop, wave);
        }
        wave.channels[c].samples.assign(file.begin() + static_cast<std::ptrdiff_t>(start),
                                        file.begin() + static_cast<std::ptrdiff_t>(stop));
    }

    return wave;
}

std::vector<uint8_t> WaveFile::Write() const
{
    Out out = Header("RWAV", version, 2);

    // INFO: the wave's info, which DATA follows.
    const std::size_t info = OpenBlock(out, "INFO");
    const std::size_t wave_info = out.Zeros(detail::WaveInfoSize(encoding, channels.size()));
    out.Align(kBlockAlignment);
    const std::size_t data = out.Size();

    // DATA: the channels' samples one after another, from 8 bytes into the block.
    std::vector<uint32_t> offsets;
    Out samples(true);
    for (const Channel& channel : channels)
    {
        offsets.push_back(static_cast<uint32_t>(samples.Size()));
        samples.Append(channel.samples);
    }
    detail::PutWaveInfo(out, wave_info, *this, static_cast<uint32_t>(data - wave_info), offsets);
    CloseBlock(out, info, 0);
    const std::size_t block = OpenBlock(out, "DATA");
    const std::vector<uint8_t> bytes = samples.Take();
    out.Append(bytes);
    CloseBlock(out, block, 1);

    return CloseFile(out);
}

WaveArchiveFile WaveArchiveFile::Read(std::span<const uint8_t> file)
{
    const xsflib::Reader r(file, true);
    WaveArchiveFile archive;
    archive.version = VersionOf(r, "RWAR");
    const auto [table, table_size] = BlockOf(r, 0, "TABL", "RWAR");
    const auto [data, data_size] = BlockOf(r, 1, "DATA", "RWAR");
    const uint32_t count = r.U32(table + kBlockHeader);
    if (count > file.size() / kTableEntry)
    {
        throw xsflib::FormatError("an RWAR file lists more waves than it can hold");
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const std::size_t entry = table + kBlockHeader + 4 + i * kTableEntry;
        const std::span<const uint8_t> wave = r.Sub(data + std::size_t{ReadRef(r, entry).offset}, r.U32(entry + 8));
        archive.waves.emplace_back(wave.begin(), wave.end());
    }

    return archive;
}

std::vector<uint8_t> WaveArchiveFile::Write() const
{
    Out out = Header("RWAR", version, 2);
    const std::size_t table = OpenBlock(out, "TABL");
    out.Zeros(4);
    out.U32(table + kBlockHeader, static_cast<uint32_t>(waves.size()));
    const std::size_t entries = out.Zeros(waves.size() * kTableEntry);
    CloseBlock(out, table, 0);

    const std::size_t data = OpenBlock(out, "DATA");
    out.Zeros(kFirstWave - kBlockHeader);
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        if (waves[i].size() % kBlockAlignment != 0)
        {
            throw std::invalid_argument("wave " + std::to_string(i) + "'s size isn't a multiple of 32 bytes");
        }
        out.Reference(entries + i * kTableEntry, 0x0100, static_cast<uint32_t>(out.Size() - data));
        out.U32(entries + i * kTableEntry + 8, static_cast<uint32_t>(waves[i].size()));
        out.Append(waves[i]);
    }
    CloseBlock(out, data, 1);

    return CloseFile(out);
}

std::vector<uint8_t> RawWaveData(std::span<const WaveFile> waves)
{
    return detail::LayRawWaves(waves).wave_data;
}

} // namespace ntdlib::rsar
