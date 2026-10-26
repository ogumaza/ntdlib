// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/wave_system.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "jaudio/reader.h"

namespace ntdlib::jaudio
{

using detail::HasTag;
using detail::IsBigEndian;
using detail::kOffsetBytes;
using detail::kReaderOptions;
using detail::kWideOffsetBytes;
using detail::OffsetAt;
using detail::StartsWithReversed;

namespace
{

// The header: the tag, the size, the number, a word, then the offsets of the WINF and WBCT chunks.
constexpr std::size_t kWinfAt = 0x10;

// Whether `file` holds a wave system with 64-bit offsets: a tag in the big-endian order, and a WINF chunk whose tag is
// reversed at the offset that the 64 bits at kWinfAt give. The Switch's release of Pikmin keeps its wave systems that
// way. Each system's tag and size keep the big-endian order. Its other fields are little-endian.
bool HasWideOffsets(std::span<const uint8_t> file)
{
    if (file.size() < kWinfAt + kWideOffsetBytes || StartsWithReversed(file, "WSYS"))
    {
        return false;
    }

    const uint32_t winf = xsflib::LoadLe32(file.data() + kWinfAt);
    return xsflib::LoadLe32(file.data() + kWinfAt + 4) == 0 && winf <= file.size() &&
           StartsWithReversed(file.subspan(winf), "WINF");
}

// The information at `at` of a wave of the archive `archive`. Throws FormatError if the wave's format is unknown.
WaveInfo ReadWaveInfo(const xsflib::Reader& r, std::size_t at, const std::string& archive)
{
    const uint8_t format = r.U8(at + 1);
    if (format > static_cast<uint8_t>(WaveFormat::kPcm16))
    {
        throw xsflib::FormatError("wave system has a wave of unknown format");
    }

    WaveInfo info;
    info.format = static_cast<WaveFormat>(format);
    info.base_key = r.U8(at + 2);
    info.sample_rate = r.F32(at + 4);
    info.offset = r.U32(at + 8);
    info.size = r.U32(at + 12);
    info.loop = r.U32(at + 16) != 0;
    info.loop_start = r.U32(at + 20);
    info.loop_end = r.U32(at + 24);
    info.sample_count = r.U32(at + 28);
    info.loop_last = r.S16(at + 32);
    info.loop_penult = r.S16(at + 34);
    info.archive = archive;

    return info;
}

// The words of the list `tag` of a scene, whose offset is at `offset_at`, each offset `w` bytes wide. The list has no
// value if its offset is null or doesn't point at the tag. The list holds its tag, the number of its waves, then an
// offset of each wave's word.
std::optional<std::vector<uint32_t>> ReadSceneList(const xsflib::Reader& r, std::size_t offset_at, uint32_t w,
                                                   const char* tag)
{
    const uint32_t list = OffsetAt(r, offset_at, w);
    if (list == 0 || !HasTag(r, list, tag))
    {
        return std::nullopt;
    }

    std::vector<uint32_t> words;
    const uint32_t count = r.U32(std::size_t{list} + 4);
    for (uint32_t i = 0; i < count; i++)
    {
        words.push_back(r.U32(OffsetAt(r, std::size_t{list} + 8 + std::size_t{w} * i, w)));
    }

    return words;
}

// The scene at `at`, each offset `w` bytes wide. A scene holds its tag, its external mode, the number of its
// dependencies, the offsets of its lists C-DF, C-EX and C-ST, then the numbers of its dependencies in 32 bits each.
WaveScene ReadScene(const xsflib::Reader& r, std::size_t at, uint32_t w)
{
    WaveScene scene;
    scene.external_mode = r.U32(at + 4);
    scene.cdf = ReadSceneList(r, at + 12, w, "C-DF");
    scene.cex = ReadSceneList(r, at + 12 + w, w, "C-EX");
    scene.cst = ReadSceneList(r, at + 12 + 2 * std::size_t{w}, w, "C-ST");

    const uint32_t dependencies = r.U32(at + 8);
    const std::size_t numbers = at + 12 + 3 * std::size_t{w};
    for (uint32_t i = 0; i < dependencies; i++)
    {
        scene.dependencies.push_back(r.U32(numbers + 4 * std::size_t{i}));
    }

    return scene;
}

} // namespace

// Reads a wave system: its archives (WINF) and the wave ids that its scenes give their waves (WBCT). JAudio2 gives each
// wave the id in the low half of its scene's entry (JASWSParser). A little-endian system stores the entry as a 32-bit
// word too.
WaveSystem ReadWaveSystem(std::span<const uint8_t> file, uint32_t number, std::size_t wave_count_at)
{
    const bool wide_offsets = HasWideOffsets(file);
    const xsflib::Reader r(file, !wide_offsets && IsBigEndian(file, "WSYS"), kReaderOptions);
    if (!HasTag(r, 0, "WSYS"))
    {
        throw xsflib::FormatError("wave system has no WSYS header");
    }

    // Each offset takes `w` bytes.
    const uint32_t w = wide_offsets ? kWideOffsetBytes : kOffsetBytes;
    WaveSystem ws;
    ws.number = number;
    const uint32_t winf = OffsetAt(r, kWinfAt, w);
    const uint32_t wbct = OffsetAt(r, kWinfAt + w, w);
    const uint32_t archives = r.U32(winf + 4);
    const uint32_t groups = r.U32(wbct + 8);
    for (uint32_t g = 0; g < std::min(archives, groups); g++)
    {
        const uint32_t archive = OffsetAt(r, winf + 8 + w * g, w);
        WaveGroup& group = ws.groups.emplace_back();
        group.archive = r.CString(archive, 0x70);
        group.scene = ReadScene(r, OffsetAt(r, wbct + 12 + w * g, w), w);

        const uint32_t count = r.U32(archive + wave_count_at);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t at = OffsetAt(r, archive + wave_count_at + 4 + w * i, w);
            group.archive_waves.push_back(ReadWaveInfo(r, at, group.archive));
        }

        // C-DF gives the archive's first waves their ids.
        const std::vector<uint32_t> no_words;
        const std::vector<uint32_t>& cdf = group.scene.cdf ? *group.scene.cdf : no_words;
        for (std::size_t i = 0; i < std::min(group.archive_waves.size(), cdf.size()); i++)
        {
            const auto id = static_cast<uint16_t>(cdf[i] & 0xffff);
            ws.waves[id] = group.archive_waves[i];
            group.waves.push_back(id);
        }
    }

    return ws;
}

std::vector<uint32_t> WaveGroups(const std::vector<WaveSystem>& systems, const std::vector<WaveUse>& waves)
{
    std::vector<uint32_t> chosen;
    for (const WaveSystem& system : systems)
    {
        std::set<uint16_t> missing;
        for (const WaveUse& use : waves)
        {
            if (use.system == system.number)
            {
                missing.insert(use.id);
            }
        }

        while (!missing.empty())
        {
            std::size_t best = 0;
            std::size_t most = 0;
            for (std::size_t g = 0; g < system.groups.size(); g++)
            {
                std::set<uint16_t> held;
                for (const uint16_t id : system.groups[g].waves)
                {
                    if (missing.contains(id))
                    {
                        held.insert(id);
                    }
                }

                if (held.size() > most)
                {
                    best = g;
                    most = held.size();
                }
            }

            if (most == 0)
            {
                break;
            }

            chosen.push_back(system.number << 16 | static_cast<uint32_t>(best));
            for (const uint16_t id : system.groups[best].waves)
            {
                missing.erase(id);
            }
        }
    }

    std::sort(chosen.begin(), chosen.end());

    return chosen;
}

bool GroupsHold(const std::vector<WaveSystem>& systems, std::span<const uint32_t> groups, WaveUse wave)
{
    for (const uint32_t entry : groups)
    {
        for (const WaveSystem& system : systems)
        {
            const std::size_t group = entry & 0xffff;
            if (system.number == entry >> 16 && entry >> 16 == wave.system && group < system.groups.size())
            {
                const std::vector<uint16_t>& waves = system.groups[group].waves;
                if (std::find(waves.begin(), waves.end(), wave.id) != waves.end())
                {
                    return true;
                }
            }
        }
    }

    return false;
}

std::size_t MissingWaves(const std::vector<WaveSystem>& systems, std::span<const uint32_t> groups,
                         std::span<const WaveUse> played)
{
    // Whether a group of the wave's system holds it.
    const auto in_a_group = [&systems](WaveUse wave)
    {
        for (const WaveSystem& system : systems)
        {
            if (system.number != wave.system)
            {
                continue;
            }

            for (const WaveGroup& group : system.groups)
            {
                if (std::find(group.waves.begin(), group.waves.end(), wave.id) != group.waves.end())
                {
                    return true;
                }
            }
        }

        return false;
    };

    // Whether the chosen groups lack a wave that a group holds.
    const auto lacking = [&](WaveUse wave)
    {
        return in_a_group(wave) && !GroupsHold(systems, groups, wave);
    };
    return static_cast<std::size_t>(std::count_if(played.begin(), played.end(), lacking));
}

std::vector<std::string> GroupArchives(const std::vector<WaveSystem>& systems, std::span<const uint32_t> groups)
{
    std::set<std::string> names;
    for (const uint32_t entry : groups)
    {
        for (const WaveSystem& system : systems)
        {
            if (system.number == entry >> 16 && (entry & 0xffff) < system.groups.size())
            {
                names.insert(system.groups[entry & 0xffff].archive);
            }
        }
    }

    return std::vector<std::string>(names.begin(), names.end());
}

} // namespace ntdlib::jaudio
