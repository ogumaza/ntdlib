// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/wave_system.h"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "jaudio/reader.h"
#include "xsflib/binary.h"

namespace ntdlib::jaudio
{

using detail::kReaderOptions;

// Reads a wave system: its archives (WINF) and the wave ids that its scenes give their waves (WBCT). JAudio2 gives each
// wave the id in the low half of its scene's entry (JASWSParser).
WaveSystem ReadWaveSystem(std::span<const uint8_t> file, uint32_t number)
{
    const xsflib::Reader r(file, true, kReaderOptions);
    if (!r.Magic(0, "WSYS"))
    {
        throw xsflib::FormatError("wave system has no WSYS header");
    }

    WaveSystem ws;
    ws.number = number;
    const uint32_t winf = r.U32(0x10);
    const uint32_t wbct = r.U32(0x14);
    const uint32_t archives = r.U32(winf + 4);
    const uint32_t groups = r.U32(wbct + 8);
    for (uint32_t g = 0; g < std::min(archives, groups); g++)
    {
        const uint32_t archive = r.U32(winf + 8 + 4 * g);
        const std::string name = r.CString(archive, 0x70);
        const uint32_t scene = r.U32(wbct + 12 + 4 * g);
        const uint32_t ids = r.U32(scene + 12);
        const uint32_t waves = std::min(r.U32(archive + 0x70), r.U32(ids + 4));

        WaveGroup& group = ws.groups.emplace_back();
        group.archive = name;
        for (uint32_t w = 0; w < waves; w++)
        {
            const uint32_t at = r.U32(archive + 0x74 + 4 * w);
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
            info.archive = name;

            const auto id = static_cast<uint16_t>(r.U32(r.U32(ids + 8 + 4 * w)) & 0xffff);
            ws.waves[id] = std::move(info);
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
