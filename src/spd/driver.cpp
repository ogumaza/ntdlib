// SPDX-License-Identifier: MIT

#include "ntdlib/spd/driver.h"

#include <cstdio>

#include "xsflib/binary.h"

namespace ntdlib::spd
{

std::optional<DriverInfo> FindDriver(const ntdlib::ntr::Rom& rom)
{
    for (const ntdlib::ntr::RomFile& file : rom.Files())
    {
        const std::span<const uint8_t> data = rom.FileData(file.id);
        if (!IsSoundData(data))
        {
            continue;
        }
        DriverInfo info;
        info.file_id = file.id;
        info.path = file.path;
        try
        {
            info.blocks = ReadBlocks(data);
        }
        catch (const xsflib::FormatError&)
        {
            continue;
        }
        if (info.blocks.empty())
        {
            continue;
        }
        for (std::size_t index = 0; index < info.blocks.size(); ++index)
        {
            const BlockPlace& place = info.blocks[index];
            try
            {
                const Block block(data.subspan(place.offset, place.size));
                for (int sound = 0; sound < block.SoundCount(); ++sound)
                {
                    if (block.SoundAt(sound))
                    {
                        info.songs.push_back({static_cast<int>(index), sound});
                    }
                }
            }
            catch (const xsflib::FormatError&)
            {
                // A block that doesn't read gives no songs. ReadBlockData() throws for it.
            }
        }
        return info;
    }
    return std::nullopt;
}

std::span<const uint8_t> SoundData(const ntdlib::ntr::Rom& rom, const DriverInfo& info)
{
    return rom.FileData(info.file_id);
}

std::string SongName(const DriverInfo& info, int song)
{
    const SongEntry& entry = info.songs.at(static_cast<std::size_t>(song));
    char name[16];
    std::snprintf(name, sizeof(name), "%02d_%03d", info.blocks[static_cast<std::size_t>(entry.block)].slot,
                  entry.sound);
    return name;
}

std::vector<Block> ReadBlockData(std::span<const uint8_t> data, const DriverInfo& info)
{
    std::vector<Block> blocks;
    blocks.reserve(info.blocks.size());
    for (const BlockPlace& place : info.blocks)
    {
        if (place.offset > data.size() || place.size > data.size() - place.offset)
        {
            throw xsflib::FormatError("a block runs past the end of the sound data");
        }
        blocks.emplace_back(data.subspan(place.offset, place.size));
    }
    return blocks;
}

Sets SetsOf(const std::vector<Block>& blocks, const DriverInfo& info, int song)
{
    const SongEntry& entry = info.songs.at(static_cast<std::size_t>(song));
    Sets sets;
    sets.a = &blocks.at(0);
    if (entry.block != 0)
    {
        sets.b = &blocks.at(static_cast<std::size_t>(entry.block));
    }
    return sets;
}

} // namespace ntdlib::spd
