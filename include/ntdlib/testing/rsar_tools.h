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

#include "ntdlib/testing/byte_writer.h"
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

// Appends the table of file references to INFO, with the reference to it at `ref`, then the entry of each file. Each
// entry lists the places where `all_groups` hold the file. The references in INFO count from `base`.
inline void PutFileEntries(Bytes& archive, size_t ref, size_t base, const std::vector<File>& files,
                           const std::vector<std::vector<uint32_t>>& all_groups)
{
    using rsar_files::Grow;
    using rsar_files::InfoTable;
    using rsar_files::PutRef;

    const size_t file_refs = InfoTable(archive, ref, base, files.size());
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
}

// Appends the table of group references to INFO, with the reference to it at `ref`, then the entry of each group and
// the entries of its items. Returns the groups and their files for rsar_files::AppendFileBlock.
inline std::vector<rsar_files::FileGroup> PutGroupEntries(Bytes& archive, size_t ref, size_t base,
                                                          const std::vector<File>& files,
                                                          const std::vector<std::vector<uint32_t>>& all_groups)
{
    using rsar_files::Grow;
    using rsar_files::InfoTable;
    using rsar_files::PutRef;

    const size_t group_refs = InfoTable(archive, ref, base, all_groups.size());
    std::vector<rsar_files::FileGroup> file_groups;
    for (size_t g = 0; g < all_groups.size(); ++g)
    {
        const size_t entry = Grow(archive, 0x28);
        rsar_files::FileGroup& file_group = file_groups.emplace_back();
        file_group.entry = entry;
        PutRef(archive, group_refs + g * 8, 0, static_cast<uint32_t>(entry - base));
        xsflib::StoreBe32(&archive[entry + 4], 0xFFFFFFFF);
        const size_t item_refs = InfoTable(archive, entry + 0x20, base, all_groups[g].size());
        for (size_t i = 0; i < all_groups[g].size(); ++i)
        {
            const size_t item = Grow(archive, 0x18);
            const File& file = files[all_groups[g][i]];
            file_group.items.push_back({item, file.bytes, file.wave_data});
            PutRef(archive, item_refs + i * 8, 0, static_cast<uint32_t>(item - base));
            xsflib::StoreBe32(&archive[item], all_groups[g][i]);
        }
    }

    return file_groups;
}

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
    std::vector<std::vector<uint32_t>> all_groups = groups;
    all_groups.emplace_back();
    PutFileEntries(archive, tables + 0x18, base, files, all_groups);

    // Each group's entry, with its items' entries.
    const std::vector<rsar_files::FileGroup> file_groups =
        PutGroupEntries(archive, tables + 0x20, base, files, all_groups);
    archive.resize(AlignUp(archive.size(), 32), 0);
    EndBlock(archive, info, 1);

    // FILE: a header of 0x20 bytes, then each group's files and wave data.
    rsar_files::AppendFileBlock(archive, file_groups);

    return archive;
}

} // namespace ntdlib::testing::rsar_tools
