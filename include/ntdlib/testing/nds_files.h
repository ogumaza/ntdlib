// SPDX-License-Identifier: MIT

// A builder of small DS cartridge images for the tests: a header, an ARM9 and an ARM7 binary, an overlay of each, and a
// file system with directories. The layout follows ntdlib/ntr/rom.h: the header's 0x4000 bytes, the ARM9 binary, its
// overlay table, the ARM7 binary, its overlay table, the FNT, the FAT, the overlays' files and the named files, each
// on a multiple of 0x200 bytes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/testing/byte_writer.h"

namespace ntdlib::testing::nds_files
{

using Bytes = std::vector<uint8_t>;

struct Image
{
    std::string title = "TEST";
    std::string game_code = "ATST";
    uint8_t unit_code = 0;
    uint8_t version = 0;
    Bytes arm9 = Bytes(0x100, 0x99);
    Bytes arm7 = Bytes(0x80, 0x77);
    Bytes arm9_overlay = Bytes(0x40, 0x91);
    Bytes arm7_overlay = Bytes(0x20, 0x71);
    std::vector<std::pair<std::string, Bytes>> files; // paths with '/' between directories
};

namespace detail
{

inline void Put32(Bytes& file, std::size_t at, uint32_t value)
{
    xsflib::StoreLe32(&file[at], value);
}

inline std::size_t Place(Bytes& image, const Bytes& data)
{
    image.resize(AlignUp(image.size(), 0x200), 0xFF);
    const std::size_t at = image.size();
    image.insert(image.end(), data.begin(), data.end());

    return at;
}

// A directory of the file system: its subdirectories by name and its files' indices into Image::files.
struct Directory
{
    std::map<std::string, Directory> directories;
    std::vector<std::pair<std::string, std::size_t>> files;
    uint16_t id = 0;
    uint16_t parent = 0;
};

inline void Number(Directory& directory, uint16_t& next, std::vector<Directory*>& order)
{
    directory.id = next++;
    order.push_back(&directory);
    for (auto& [name, child] : directory.directories)
    {
        child.parent = directory.id;
        Number(child, next, order);
    }
}

// The header's 0x4000 bytes, with the title, the game code, the maker code, the unit code and the version.
inline Bytes HeaderOf(const Image& spec)
{
    Bytes image(0x4000, 0);
    for (std::size_t i = 0; i < spec.title.size() && i < 12; i++)
    {
        image[i] = static_cast<uint8_t>(spec.title[i]);
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        image[12 + i] = static_cast<uint8_t>(spec.game_code[i]);
    }
    image[16] = '0';
    image[17] = '1';
    image[0x12] = spec.unit_code;
    image[0x1E] = spec.version;

    return image;
}

// The binaries and their overlay tables. Each table holds one overlay.
inline void AppendBinaries(Bytes& image, const Image& spec)
{
    const auto binary = [&](std::size_t field, const Bytes& data, uint32_t ram)
    {
        const std::size_t at = Place(image, data);
        Put32(image, field, static_cast<uint32_t>(at));
        Put32(image, field + 4, ram);
        Put32(image, field + 8, ram);
        Put32(image, field + 12, static_cast<uint32_t>(data.size()));
    };
    const auto overlay_table = [&](std::size_t field, uint32_t ram, uint32_t file_id, std::size_t size)
    {
        Bytes table(32, 0);
        Put32(table, 4, ram);
        Put32(table, 8, static_cast<uint32_t>(size));
        Put32(table, 24, file_id);
        const std::size_t at = Place(image, table);
        Put32(image, field, static_cast<uint32_t>(at));
        Put32(image, field + 4, 32);
    };
    binary(0x20, spec.arm9, 0x02000000);
    overlay_table(0x50, 0x02100000, 0, spec.arm9_overlay.size());
    binary(0x30, spec.arm7, 0x02380000);
    overlay_table(0x58, 0x02390000, 1, spec.arm7_overlay.size());
}

// The FNT of the files of `spec`, and the file ID of each of Image::files in order.
inline std::pair<Bytes, std::vector<uint32_t>> FntOf(const Image& spec)
{
    // The directory tree, numbered from the root in depth-first order.
    Directory root;
    for (std::size_t i = 0; i < spec.files.size(); i++)
    {
        Directory* directory = &root;
        std::string path = spec.files[i].first;
        for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/'))
        {
            directory = &directory->directories[path.substr(0, slash)];
            path = path.substr(slash + 1);
        }
        directory->files.emplace_back(path, i);
    }
    uint16_t next = 0;
    std::vector<Directory*> order;
    Number(root, next, order);

    // The files take IDs from 2, a directory's files together.
    std::vector<uint32_t> ids(spec.files.size());
    std::vector<uint32_t> first(order.size());
    uint32_t id = 2;
    for (std::size_t d = 0; d < order.size(); d++)
    {
        first[d] = id;
        for (const auto& [name, index] : order[d]->files)
        {
            ids[index] = id++;
        }
    }

    // The FNT: the main table, then each directory's sub-table.
    Bytes fnt(order.size() * 8, 0);
    for (std::size_t d = 0; d < order.size(); d++)
    {
        Put32(fnt, d * 8, static_cast<uint32_t>(fnt.size()));
        xsflib::StoreLe16(&fnt[d * 8 + 4], static_cast<uint16_t>(first[d]));
        xsflib::StoreLe16(&fnt[d * 8 + 6], static_cast<uint16_t>(d == 0 ? order.size() : 0xF000 + order[d]->parent));
        for (const auto& [name, index] : order[d]->files)
        {
            fnt.push_back(static_cast<uint8_t>(name.size()));
            fnt.insert(fnt.end(), name.begin(), name.end());
        }
        for (const auto& [name, child] : order[d]->directories)
        {
            fnt.push_back(static_cast<uint8_t>(0x80 | name.size()));
            fnt.insert(fnt.end(), name.begin(), name.end());
            fnt.push_back(static_cast<uint8_t>(child.id));
            fnt.push_back(0xF0);
        }
        fnt.push_back(0);
    }

    return {fnt, ids};
}

// The FAT, then the files' bytes. `ids` gives the file ID of each of Image::files.
inline void AppendFat(Bytes& image, const Image& spec, const std::vector<uint32_t>& ids)
{
    const std::size_t count = 2 + spec.files.size();
    const std::size_t fat_at = Place(image, Bytes(count * 8, 0));
    Put32(image, 0x48, static_cast<uint32_t>(fat_at));
    Put32(image, 0x4C, static_cast<uint32_t>(count * 8));
    const auto place_file = [&](uint32_t file_id, const Bytes& data)
    {
        const std::size_t at = Place(image, data);
        Put32(image, fat_at + file_id * 8, static_cast<uint32_t>(at));
        Put32(image, fat_at + file_id * 8 + 4, static_cast<uint32_t>(at + data.size()));
    };
    place_file(0, spec.arm9_overlay);
    place_file(1, spec.arm7_overlay);
    for (std::size_t i = 0; i < spec.files.size(); i++)
    {
        place_file(ids[i], spec.files[i].second);
    }
}

} // namespace detail

// The image, and the file ID of each of Image::files in order. The overlays take IDs 0 and 1.
inline std::pair<Bytes, std::vector<uint32_t>> BuildImage(const Image& spec)
{
    using detail::Put32;

    Bytes image = detail::HeaderOf(spec);
    detail::AppendBinaries(image, spec);
    const auto [fnt, ids] = detail::FntOf(spec);
    const std::size_t fnt_at = detail::Place(image, fnt);
    Put32(image, 0x40, static_cast<uint32_t>(fnt_at));
    Put32(image, 0x44, static_cast<uint32_t>(fnt.size()));
    detail::AppendFat(image, spec, ids);
    Put32(image, 0x80, static_cast<uint32_t>(image.size()));
    Put32(image, 0x84, 0x4000);

    return {image, ids};
}

} // namespace ntdlib::testing::nds_files
