// SPDX-License-Identifier: MIT

// A builder of sound archives of the Wii (RSAR) for the tests, laid out as NintendoWare for Revolution's tools lay them
// out: the header, SYMB with the sounds' names, INFO with the tables, and FILE. FILE holds the groups one after
// another. A group holds its files one after another, then their wave data. Each file and each wave data's size is a
// multiple of 32 bytes. The last group is empty and ends the archive. The file table holds each file's size and its
// wave data's.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/testing/rsar_files.h"

namespace ntdlib::testing::rsar_tools
{

using Bytes = std::vector<uint8_t>;

// A file of the archive: its bytes, and its wave data. Each size has to be a multiple of 32 bytes.
struct File
{
    Bytes bytes;
    Bytes wave_data = {};
};

// A sequence sound: its name, its file, and its start in the file's bytecode.
struct SequenceSound
{
    std::string name;
    uint32_t file = 0;
    uint32_t start = 0;
};

// An archive of version `version` (at least 1.1) with the sequence sounds `sounds`. `groups` lists the files that each
// group holds, in order. The builder adds the empty group at the end.
inline Bytes MakeArchive(uint16_t version, const std::vector<File>& files, const std::vector<SequenceSound>& sounds,
                         const std::vector<std::vector<uint32_t>>& groups)
{
    using rsar_files::Block;
    using rsar_files::EndBlock;
    using rsar_files::Grow;
    using rsar_files::InfoTable;
    using rsar_files::PutRef;

    Bytes archive = rsar_files::Header("RSAR", version, 3, 0x40);
    std::vector<const char*> names;
    for (const SequenceSound& sound : sounds)
    {
        names.push_back(sound.name.c_str());
    }
    rsar_files::PutSymbols(archive, names);

    // INFO: the tables of sounds, banks, players, files and groups.
    const size_t info = Block(archive, "INFO", 1);
    const size_t base = archive.size();
    const size_t tables = Grow(archive, 0x30);
    const size_t sound_refs = InfoTable(archive, tables, base, sounds.size());
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const size_t sound = Grow(archive, 0x2C);
        PutRef(archive, sound_refs + i * 8, 0, static_cast<uint32_t>(sound - base));
        xsflib::StoreBe32(&archive[sound], static_cast<uint32_t>(i));
        xsflib::StoreBe32(&archive[sound + 4], sounds[i].file);
        archive[sound + 0x14] = 127;
        archive[sound + 0x15] = 64;
        archive[sound + 0x16] = 1;
        const size_t details = Grow(archive, 0x14);
        PutRef(archive, sound + 0x18, 1, static_cast<uint32_t>(details - base));
        xsflib::StoreBe32(&archive[details], sounds[i].start);
        xsflib::StoreBe32(&archive[details + 8], 1);
        archive[details + 0xC] = 64;
    }
    InfoTable(archive, tables + 8, base, 0);
    InfoTable(archive, tables + 0x10, base, 0);

    // Each file's entry lists the places where groups hold it: a group and an item of the group.
    const std::vector<std::vector<uint32_t>> all_groups = [&]
    {
        std::vector<std::vector<uint32_t>> all = groups;
        all.emplace_back();
        return all;
    }();
    const size_t file_refs = InfoTable(archive, tables + 0x18, base, files.size());
    std::vector<size_t> file_entries;
    for (uint32_t f = 0; f < files.size(); ++f)
    {
        std::vector<std::pair<uint32_t, uint32_t>> places;
        for (uint32_t g = 0; g < all_groups.size(); ++g)
        {
            for (uint32_t i = 0; i < all_groups[g].size(); ++i)
            {
                if (all_groups[g][i] == f)
                {
                    places.emplace_back(g, i);
                }
            }
        }

        const size_t entry = Grow(archive, 0x1C);
        file_entries.push_back(entry);
        PutRef(archive, file_refs + f * 8, 0, static_cast<uint32_t>(entry - base));
        xsflib::StoreBe32(&archive[entry], static_cast<uint32_t>(files[f].bytes.size()));
        xsflib::StoreBe32(&archive[entry + 4], static_cast<uint32_t>(files[f].wave_data.size()));
        const size_t place_refs = InfoTable(archive, entry + 0x14, base, places.size());
        for (size_t p = 0; p < places.size(); ++p)
        {
            const size_t place = Grow(archive, 8);
            PutRef(archive, place_refs + p * 8, 0, static_cast<uint32_t>(place - base));
            xsflib::StoreBe32(&archive[place], places[p].first);
            xsflib::StoreBe32(&archive[place + 4], places[p].second);
        }
    }

    // Each group's entry, with its items' entries.
    const size_t group_refs = InfoTable(archive, tables + 0x20, base, all_groups.size());
    std::vector<size_t> group_entries;
    std::vector<std::vector<size_t>> item_entries(all_groups.size());
    for (size_t g = 0; g < all_groups.size(); ++g)
    {
        const size_t entry = Grow(archive, 0x28);
        group_entries.push_back(entry);
        PutRef(archive, group_refs + g * 8, 0, static_cast<uint32_t>(entry - base));
        xsflib::StoreBe32(&archive[entry + 4], 0xFFFFFFFF);
        const size_t item_refs = InfoTable(archive, entry + 0x20, base, all_groups[g].size());
        for (size_t i = 0; i < all_groups[g].size(); ++i)
        {
            const size_t item = Grow(archive, 0x18);
            item_entries[g].push_back(item);
            PutRef(archive, item_refs + i * 8, 0, static_cast<uint32_t>(item - base));
            xsflib::StoreBe32(&archive[item], all_groups[g][i]);
        }
    }
    archive.resize((archive.size() + 31) & ~size_t{31}, 0);
    EndBlock(archive, info, 1);

    // FILE: a header of 0x20 bytes, then each group's files and wave data.
    const size_t data = Block(archive, "FILE", 2);
    Grow(archive, 0x18);
    for (size_t g = 0; g < all_groups.size(); ++g)
    {
        const size_t group_files = archive.size();
        for (size_t i = 0; i < all_groups[g].size(); ++i)
        {
            const Bytes& bytes = files[all_groups[g][i]].bytes;
            xsflib::StoreBe32(&archive[item_entries[g][i] + 4], static_cast<uint32_t>(archive.size() - group_files));
            xsflib::StoreBe32(&archive[item_entries[g][i] + 8], static_cast<uint32_t>(bytes.size()));
            archive.insert(archive.end(), bytes.begin(), bytes.end());
        }

        const size_t group_waves = archive.size();
        for (size_t i = 0; i < all_groups[g].size(); ++i)
        {
            const Bytes& wave_data = files[all_groups[g][i]].wave_data;
            if (!wave_data.empty())
            {
                xsflib::StoreBe32(&archive[item_entries[g][i] + 0xC],
                                  static_cast<uint32_t>(archive.size() - group_waves));
                xsflib::StoreBe32(&archive[item_entries[g][i] + 0x10], static_cast<uint32_t>(wave_data.size()));
                archive.insert(archive.end(), wave_data.begin(), wave_data.end());
            }
        }

        const size_t entry = group_entries[g];
        xsflib::StoreBe32(&archive[entry + 0x10], static_cast<uint32_t>(group_files));
        xsflib::StoreBe32(&archive[entry + 0x14], static_cast<uint32_t>(group_waves - group_files));
        xsflib::StoreBe32(&archive[entry + 0x18], static_cast<uint32_t>(group_waves));
        xsflib::StoreBe32(&archive[entry + 0x1C], static_cast<uint32_t>(archive.size() - group_waves));
    }
    EndBlock(archive, data, 2);

    return archive;
}

} // namespace ntdlib::testing::rsar_tools
