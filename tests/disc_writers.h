// SPDX-License-Identifier: MIT

// What the GameCube and Wii disc tests share: the test game, and the writers. BuildGame builds the game's data, which
// fills a GameCube disc or a Wii disc's game partition. The writers store an ISO image as GCZ, CISO and WDF, compress a
// WIA or RVZ group with each method, pack RVZ's junk, encode WIA's PURGE, store a disc's raw data in WIA or RVZ groups
// and lay out a WIA or RVZ file. The writers follow the formats' descriptions. The lists of WIA's and RVZ's methods are
// here too.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha1.h"
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include "disc_files.h"
#include "ntdlib/disc/compression.h"
#include "ntdlib/disc/junk.h"

namespace disc_writers
{

using Bytes = std::vector<uint8_t>;
using ntdlib::disc::Method;

// A run of junk in a disc's data. The junk is generated like a disc's padding.
struct Junk
{
    uint64_t offset;
    uint64_t size;
    std::array<uint8_t, 68> seed;
};

// How the test game lies on a console's disc: the game's ID, the console's magic word and where boot.bin holds it, the
// shift of the offsets in boot.bin and the file system table, and the names of the sound files.
struct GameLayout
{
    std::string id;
    size_t magic_offset;
    uint32_t magic;
    int shift;                // the right shift of the offsets, and of boot.bin's sizes of the table: 2 on the Wii
    std::string sound_folder; // with sound_file in it, and sub_sound_file in its subfolder sub
    std::string sound_file;
    std::string sub_sound_file;
};

// The test game: its system files, its files, and the data that holds them, with runs of junk between the files. The
// data is a whole GameCube disc, or a Wii disc's game partition, decrypted and without hashes.
struct Game
{
    Bytes boot;
    Bytes bi2;
    Bytes apploader;
    Bytes dol;
    Bytes fst;
    std::map<std::string, Bytes> files; // by the path the reader gives, decoded from Shift-JIS
    Bytes data;
    std::vector<Junk> junk; // offsets in `data`
};

// The test game, laid out as `layout` gives.
inline Game BuildGame(const GameLayout& layout)
{
    Game game;
    game.data.assign(0x3A0000, 0);
    Bytes& data = game.data;

    // An offset or a size as boot.bin and the table store it.
    const auto shifted = [&layout](uint64_t value)
    {
        return static_cast<uint32_t>(value >> layout.shift);
    };

    // boot.bin, with the ID, the magic word, the title, and the offsets of main.dol and the file system table.
    const uint64_t kDol = 0x10000;
    const uint64_t kFst = 0x14000;
    game.boot.assign(0x440, 0);
    std::copy_n(layout.id.begin(), 6, game.boot.begin());
    xsflib::StoreBe32(game.boot.data() + layout.magic_offset, layout.magic);
    std::copy_n("TEST DISC", 9, game.boot.begin() + 0x20);
    xsflib::StoreBe32(game.boot.data() + 0x420, shifted(kDol));
    xsflib::StoreBe32(game.boot.data() + 0x424, shifted(kFst));

    game.bi2.assign(0x2000, 0);
    disc_files::Fill(game.bi2, 0, game.bi2.size(), 1);

    // The apploader: a header with its date, entry point, size and trailer size, then its code.
    game.apploader.assign(0x20 + 0x1234 + 0x100, 0);
    disc_files::Fill(game.apploader, 0x20, game.apploader.size() - 0x20, 2);
    std::copy_n("2026/10/07", 10, game.apploader.begin());
    xsflib::StoreBe32(game.apploader.data() + 0x10, 0x81200000);
    xsflib::StoreBe32(game.apploader.data() + 0x14, 0x1234);
    xsflib::StoreBe32(game.apploader.data() + 0x18, 0x100);

    // main.dol: one text and one data section.
    game.dol.assign(0x2600, 0);
    disc_files::Fill(game.dol, 0x100, 0x2500, 3);
    std::fill_n(game.dol.begin(), 0x100, uint8_t{0});
    xsflib::StoreBe32(game.dol.data() + 0x00, 0x100);
    xsflib::StoreBe32(game.dol.data() + 0x48, 0x80004000);
    xsflib::StoreBe32(game.dol.data() + 0x90, 0x2000);
    xsflib::StoreBe32(game.dol.data() + 0x1C, 0x2100);
    xsflib::StoreBe32(game.dol.data() + 0x64, 0x80006000);
    xsflib::StoreBe32(game.dol.data() + 0xAC, 0x500);
    xsflib::StoreBe32(game.dol.data() + 0xE0, 0x80004000);

    // The files: one across many sectors and two chunks, a small one in a subfolder, an empty one, and two whose names
    // aren't ASCII (half-width katakana, and two double-byte katakana of JIS X 0208).
    struct File
    {
        std::string disc_name;
        std::string path;
        uint64_t offset;
        uint32_t size;
    };

    const std::vector<File> files = {
        {"\x83R\x83s.bin", "\xE3\x82\xB3\xE3\x83\x94.bin", 0x18004, 0x1000},
        {layout.sound_file, layout.sound_folder + "/" + layout.sound_file, 0x20000, 0x30000},
        {layout.sub_sound_file, layout.sound_folder + "/sub/" + layout.sub_sound_file, 0x50010, 0x10},
        {"big.bin", "big.bin", 0x60000, 0x250000},
        {"empty.bin", "empty.bin", 0x2B0000, 0},
        {"\xB1\xB2.txt", "\xEF\xBD\xB1\xEF\xBD\xB2.txt", 0x2B0000, 0x7C04},
    };
    for (size_t i = 0; i < files.size(); ++i)
    {
        disc_files::Fill(data, files[i].offset, files[i].size, static_cast<uint32_t>(10 + i));
        game.files[files[i].path] = Bytes(data.begin() + static_cast<std::ptrdiff_t>(files[i].offset),
                                          data.begin() + static_cast<std::ptrdiff_t>(files[i].offset + files[i].size));
    }

    // The table: root, then each entry in order, with a directory's last entry + 1 as its size.
    struct Entry
    {
        std::string name; // Shift-JIS bytes, as on the disc
        bool directory;
        uint32_t parent_or_offset;
        uint32_t size_or_next;
    };

    const std::vector<Entry> entries = {
        {"", true, 0, 9},
        {files[0].disc_name, false, shifted(0x18004), 0x1000},
        {layout.sound_folder, true, 0, 6},
        {files[1].disc_name, false, shifted(0x20000), 0x30000},
        {"sub", true, 2, 6},
        {files[2].disc_name, false, shifted(0x50010), 0x10},
        {"big.bin", false, shifted(0x60000), 0x250000},
        {"empty.bin", false, shifted(0x2B0000), 0},
        {files[5].disc_name, false, shifted(0x2B0000), 0x7C04},
    };
    std::string names;
    game.fst.assign(entries.size() * 12, 0);
    for (size_t i = 0; i < entries.size(); ++i)
    {
        uint8_t* entry = game.fst.data() + i * 12;
        xsflib::StoreBe32(entry, static_cast<uint32_t>(names.size()));
        entry[0] = entries[i].directory ? 1 : 0;
        xsflib::StoreBe32(entry + 4, entries[i].parent_or_offset);
        xsflib::StoreBe32(entry + 8, entries[i].size_or_next);
        names += entries[i].name;
        names += '\0';
    }

    game.fst.insert(game.fst.end(), names.begin(), names.end());
    while (game.fst.size() % 4)
    {
        game.fst.push_back(0);
    }

    xsflib::StoreBe32(game.boot.data() + 0x428, shifted(game.fst.size()));
    xsflib::StoreBe32(game.boot.data() + 0x42C, shifted(game.fst.size()));

    // Junk between the files, as on a disc: one run inside a sector, one across several.
    const uint64_t kSector = 0x8000; // the period of the junk
    for (const auto& [offset, size, seed] : {std::tuple{0x51000u, 0x3000u, 7u}, std::tuple{0x2B8000u, 0x50000u, 8u}})
    {
        Junk junk = {offset, size, {}};
        for (size_t i = 0; i < junk.seed.size(); ++i)
        {
            junk.seed[i] = static_cast<uint8_t>(seed * 31 + i * 17);
        }

        ntdlib::disc::JunkGenerator generator(junk.seed.data());
        generator.Skip(offset % kSector);
        generator.Generate(data.data() + offset, size);
        game.junk.push_back(junk);
    }

    std::copy(game.boot.begin(), game.boot.end(), data.begin());
    std::copy(game.bi2.begin(), game.bi2.end(), data.begin() + 0x440);
    std::copy(game.apploader.begin(), game.apploader.end(), data.begin() + 0x2440);
    std::copy(game.dol.begin(), game.dol.end(), data.begin() + kDol);
    std::copy(game.fst.begin(), game.fst.end(), data.begin() + kFst);

    return game;
}

// GCZ: zlib-compressed blocks. A block that doesn't shrink is stored as it is.
inline Bytes Gcz(const Bytes& iso)
{
    const uint32_t kBlock = 0x4000;
    const uint32_t count = static_cast<uint32_t>((iso.size() + kBlock - 1) / kBlock);
    Bytes pointers(size_t{count} * 8);
    Bytes checksums(size_t{count} * 4);
    Bytes blocks;
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t size = std::min<size_t>(kBlock, iso.size() - size_t{i} * kBlock);
        Bytes block(iso.begin() + i * kBlock, iso.begin() + i * kBlock + static_cast<std::ptrdiff_t>(size));
        block.resize(kBlock, 0);
        uLongf compressed_size = compressBound(kBlock);
        Bytes compressed(compressed_size);
        compress2(compressed.data(), &compressed_size, block.data(), kBlock, 9);
        compressed.resize(compressed_size);
        const bool raw = compressed.size() >= kBlock;
        const Bytes& stored = raw ? block : compressed;
        xsflib::StoreLe64(pointers.data() + i * 8, blocks.size() | (raw ? 1ull << 63 : 0));
        xsflib::StoreLe32(checksums.data() + i * 4, ntdlib::disc::Adler32(stored));
        blocks.insert(blocks.end(), stored.begin(), stored.end());
    }

    Bytes gcz(32);
    xsflib::StoreLe32(gcz.data(), 0xB10BC001);
    xsflib::StoreLe32(gcz.data() + 4, 1);
    xsflib::StoreLe64(gcz.data() + 8, blocks.size());
    xsflib::StoreLe64(gcz.data() + 16, size_t{count} * kBlock);
    xsflib::StoreLe32(gcz.data() + 24, kBlock);
    xsflib::StoreLe32(gcz.data() + 28, count);
    gcz.insert(gcz.end(), pointers.begin(), pointers.end());
    gcz.insert(gcz.end(), checksums.begin(), checksums.end());
    gcz.insert(gcz.end(), blocks.begin(), blocks.end());

    return gcz;
}

inline bool AllZero(const Bytes& data, size_t offset, size_t size)
{
    const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);

    return std::all_of(begin, begin + static_cast<std::ptrdiff_t>(size), [](uint8_t b) { return b == 0; });
}

// CISO: the map of stored blocks, then the blocks that aren't all zeros.
inline Bytes Ciso(const Bytes& iso)
{
    const uint32_t kBlock = 0x10000;
    Bytes ciso(0x8000, 0);
    std::copy_n("CISO", 4, ciso.begin());
    xsflib::StoreLe32(ciso.data() + 4, kBlock);
    for (size_t i = 0; i * kBlock < iso.size(); ++i)
    {
        const size_t size = std::min<size_t>(kBlock, iso.size() - i * kBlock);
        if (AllZero(iso, i * kBlock, size))
        {
            continue;
        }

        ciso[8 + i] = 1;
        Bytes block(iso.begin() + static_cast<std::ptrdiff_t>(i * kBlock),
                    iso.begin() + static_cast<std::ptrdiff_t>(i * kBlock + size));
        block.resize(kBlock, 0);
        ciso.insert(ciso.end(), block.begin(), block.end());
    }

    return ciso;
}

// WDF: the extents of 64 KiB that aren't all zeros, then a magic and the table of extents.
inline Bytes Wdf(const Bytes& iso, int version)
{
    const uint64_t kPiece = 0x10000;
    Bytes wdf(0x38, 0);
    Bytes table;
    uint32_t count = 0;
    for (uint64_t offset = 0; offset < iso.size(); offset += kPiece)
    {
        const uint64_t size = std::min<uint64_t>(kPiece, iso.size() - offset);
        if (AllZero(iso, offset, size))
        {
            continue;
        }

        Bytes entry(version == 1 ? 28 : 24, 0);
        uint8_t* p = entry.data() + (version == 1 ? 4 : 0);
        xsflib::StoreBe64(p, offset);
        xsflib::StoreBe64(p + 8, wdf.size());
        xsflib::StoreBe64(p + 16, size);
        table.insert(table.end(), entry.begin(), entry.end());
        wdf.insert(wdf.end(), iso.begin() + static_cast<std::ptrdiff_t>(offset),
                   iso.begin() + static_cast<std::ptrdiff_t>(offset + size));
        ++count;
    }

    std::copy_n(
        "WII\x01"
        "DISC",
        8, wdf.begin());
    xsflib::StoreBe32(wdf.data() + 8, version);
    xsflib::StoreBe64(wdf.data() + 0x18, iso.size());
    xsflib::StoreBe32(wdf.data() + 0x2C, count);
    xsflib::StoreBe64(wdf.data() + 0x30, wdf.size());
    wdf.insert(wdf.end(), {'W', 'I', 'I', 1, 'D', 'I', 'S', 'C'});
    wdf.insert(wdf.end(), table.begin(), table.end());

    return wdf;
}

// The compression methods of WIA, with the names that the tests give them.
inline const std::vector<std::pair<Method, std::string>> kWiaMethods = {
    {Method::kNone, "NONE"}, {Method::kPurge, "PURGE"}, {Method::kBzip2, "BZIP2"},
    {Method::kLzma, "LZMA"}, {Method::kLzma2, "LZMA2"},
};

// The compression methods of RVZ, with the names that the tests give them.
inline const std::vector<std::pair<Method, std::string>> kRvzMethods = {
    {Method::kNone, "NONE"},   {Method::kBzip2, "BZIP2"}, {Method::kLzma, "LZMA"},
    {Method::kLzma2, "LZMA2"}, {Method::kZstd, "ZSTD"},
};

// Compresses a WIA or RVZ group's data with `method`. `props` receives the compressor data for the header.
inline Bytes Compress(Method method, const Bytes& data, Bytes& props)
{
    Bytes out(data.size() * 2 + 0x10000);
    size_t size = out.size();
    switch (method)
    {
    case Method::kBzip2:
        {
            unsigned bzip2_size = static_cast<unsigned>(out.size());
            BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(out.data()), &bzip2_size,
                                     const_cast<char*>(reinterpret_cast<const char*>(data.data())),
                                     static_cast<unsigned>(data.size()), 9, 0, 30);
            size = bzip2_size;
            break;
        }

    case Method::kLzma:
    case Method::kLzma2:
        {
            lzma_options_lzma options;
            lzma_lzma_preset(&options, 6);
            options.dict_size = 1 << 20;
            options.lc = 3;
            options.lp = 0;
            options.pb = 2;
            const lzma_filter filters[] = {
                {method == Method::kLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &options},
                {LZMA_VLI_UNKNOWN, nullptr},
            };
            size_t written = 0;
            lzma_raw_buffer_encode(filters, nullptr, data.data(), data.size(), out.data(), &written, out.size());
            size = written;

            // The 7-Zip SDK's properties: (pb * 5 + lp) * 9 + lc and the dictionary size. For LZMA2 they're a byte for
            // the dictionary size. The dictionary size here is 2 << 19.
            props = method == Method::kLzma ? Bytes{(2 * 5 + 0) * 9 + 3, 0, 0, 0x10, 0} : Bytes{16};
            break;
        }

    case Method::kZstd:
        size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
        break;

    default:
        size = 0;
        break;
    }

    out.resize(size);

    return out;
}

// One group's data before compression, and the RVZ packing or WIA PURGE encoding of it.
struct GroupData
{
    Bytes lists; // the hash exception lists (each empty here)
    Bytes data;
    std::vector<Junk> junk; // runs of junk in `data`, offsets relative to it, for RVZ packing
};

// Packs `data` for RVZ: data runs, and junk runs as their seeds.
inline Bytes Pack(const Bytes& data, const std::vector<Junk>& junk)
{
    Bytes packed;
    const auto run = [&packed](uint32_t header)
    {
        packed.resize(packed.size() + 4);
        xsflib::StoreBe32(packed.data() + packed.size() - 4, header);
    };

    uint64_t position = 0;
    for (const Junk& j : junk)
    {
        if (j.offset > position)
        {
            run(static_cast<uint32_t>(j.offset - position));
            packed.insert(packed.end(), data.begin() + static_cast<std::ptrdiff_t>(position),
                          data.begin() + static_cast<std::ptrdiff_t>(j.offset));
        }

        run(static_cast<uint32_t>(j.size) | 0x80000000);
        packed.insert(packed.end(), j.seed.begin(), j.seed.end());
        position = j.offset + j.size;
    }
    if (position < data.size())
    {
        run(static_cast<uint32_t>(data.size() - position));
        packed.insert(packed.end(), data.begin() + static_cast<std::ptrdiff_t>(position), data.end());
    }

    return packed;
}

// WIA's PURGE: the lists, padding, then the data that isn't zeros as segments, then a SHA-1.
inline Bytes Purge(const GroupData& group)
{
    Bytes out = group.lists;
    out.resize((out.size() + 3) & ~size_t{3}, 0);
    for (size_t offset = 0; offset < group.data.size();)
    {
        if (!group.data[offset])
        {
            ++offset;
            continue;
        }

        size_t end = offset;
        while (end < group.data.size() && (group.data[end] || (end + 8 < group.data.size() && group.data[end + 8])))
        {
            ++end;
        }

        Bytes header(8);
        xsflib::StoreBe32(header.data(), static_cast<uint32_t>(offset));
        xsflib::StoreBe32(header.data() + 4, static_cast<uint32_t>(end - offset));
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), group.data.begin() + static_cast<std::ptrdiff_t>(offset),
                   group.data.begin() + static_cast<std::ptrdiff_t>(end));
        offset = end;
    }

    const xsflib::Sha1::Digest hash = xsflib::Sha1::Of(out);
    out.insert(out.end(), hash.begin(), hash.end());

    return out;
}

// What a test stores in a WIA or RVZ file. `disc_type` is wia_disc_t's: 1 for a GameCube disc, 2 for a Wii disc. `head`
// holds the disc's first 0x80 bytes, and `disc_size` gives the disc's size. The partition table holds `partitions`
// entries of 0x30 bytes. The tables number `groups` in order.
struct WiaContents
{
    bool rvz = false;
    Method method = Method::kNone;
    uint32_t chunk_size = 0;
    uint32_t disc_type = 0;
    Bytes head;
    uint64_t disc_size = 0;
    Bytes raw_table;
    Bytes partition_table;
    uint32_t partitions = 0;
    std::vector<GroupData> groups;
};

// Stores the disc in `iso` up to `end` as raw data in `wia`: the raw data table's one entry, from 0x80 to `end`, and a
// group for each chunk from the disc's start. `junk` gives runs of junk by their offsets on the disc. Each group lists
// the runs that lie inside it, for RVZ packing.
inline void AddRawData(WiaContents& wia, const Bytes& iso, uint64_t end, const std::vector<Junk>& junk)
{
    const uint64_t groups = (end + wia.chunk_size - 1) / wia.chunk_size;
    wia.raw_table.resize(24);
    xsflib::StoreBe64(wia.raw_table.data(), 0x80);
    xsflib::StoreBe64(wia.raw_table.data() + 8, end - 0x80);
    xsflib::StoreBe32(wia.raw_table.data() + 16, 0);
    xsflib::StoreBe32(wia.raw_table.data() + 20, static_cast<uint32_t>(groups));
    for (uint64_t g = 0; g < groups; ++g)
    {
        const uint64_t start = g * wia.chunk_size;
        const uint64_t size = std::min<uint64_t>(wia.chunk_size, end - start);
        GroupData group;
        group.data.assign(iso.begin() + static_cast<std::ptrdiff_t>(start),
                          iso.begin() + static_cast<std::ptrdiff_t>(start + size));
        for (const Junk& j : junk)
        {
            if (j.offset >= start && j.offset + j.size <= start + size)
            {
                group.junk.push_back({j.offset - start, j.size, j.seed});
            }
        }

        wia.groups.push_back(group);
    }
}

// The WIA or RVZ file of `wia`.
inline Bytes WiaFile(const WiaContents& wia)
{
    // The tables are compressed like the groups.
    Bytes props;
    const auto table = [&](const Bytes& plain)
    {
        if (wia.method == Method::kNone)
        {
            return plain;
        }
        if (wia.method == Method::kPurge)
        {
            return Purge({{}, plain, {}});
        }

        return Compress(wia.method, plain, props);
    };

    // The layout: the two headers, the partition table, the raw data table, the groups' data and then the group table.
    // The data is aligned to 4 bytes because the group table stores offsets divided by 4.
    Bytes disc_header(0xDC, 0);
    const Bytes raw_stored = table(wia.raw_table);
    const uint64_t partition_offset = 0x48 + disc_header.size();
    const uint64_t raw_offset = partition_offset + wia.partition_table.size();
    const uint64_t data_offset = (raw_offset + raw_stored.size() + 3) & ~uint64_t{3};
    Bytes data;
    Bytes group_table;
    for (const GroupData& group : wia.groups)
    {
        uint32_t stored_size = 0;
        bool compressed = false;
        uint32_t packed_size = 0;
        const uint64_t offset = data_offset + data.size();
        if (!AllZero(group.data, 0, group.data.size()))
        {
            Bytes body = group.data;
            if (wia.rvz && !group.junk.empty())
            {
                body = Pack(group.data, group.junk);
                packed_size = static_cast<uint32_t>(body.size());
            }

            Bytes stored;
            if (wia.method == Method::kPurge)
            {
                stored = Purge(group);
            }
            else if (wia.method == Method::kNone)
            {
                stored = group.lists;
                stored.resize((stored.size() + 3) & ~size_t{3}, 0);
                stored.insert(stored.end(), body.begin(), body.end());
            }
            else
            {
                Bytes plain = group.lists;
                plain.insert(plain.end(), body.begin(), body.end());
                stored = Compress(wia.method, plain, props);
                compressed = true;
            }

            data.insert(data.end(), stored.begin(), stored.end());
            stored_size = static_cast<uint32_t>(stored.size());
            data.resize((data.size() + 3) & ~size_t{3}, 0);
        }

        Bytes entry(wia.rvz ? 12 : 8, 0);
        xsflib::StoreBe32(entry.data(), static_cast<uint32_t>(offset / 4));
        xsflib::StoreBe32(entry.data() + 4, stored_size | (wia.rvz && compressed ? 0x80000000 : 0));
        if (wia.rvz)
        {
            xsflib::StoreBe32(entry.data() + 8, packed_size);
        }

        group_table.insert(group_table.end(), entry.begin(), entry.end());
    }

    const uint64_t group_offset = data_offset + data.size();
    const Bytes groups_stored = table(group_table);

    // wia_disc_t: the disc's type, the method, the chunk size, the first 0x80 bytes of the disc, and where the tables
    // are.
    xsflib::StoreBe32(disc_header.data(), wia.disc_type);
    xsflib::StoreBe32(disc_header.data() + 4, static_cast<uint32_t>(wia.method));
    xsflib::StoreBe32(disc_header.data() + 0x0C, wia.chunk_size);
    std::copy_n(wia.head.begin(), 0x80, disc_header.begin() + 0x10);
    xsflib::StoreBe32(disc_header.data() + 0x90, wia.partitions);
    xsflib::StoreBe32(disc_header.data() + 0x94, 0x30);
    xsflib::StoreBe64(disc_header.data() + 0x98, partition_offset);
    const xsflib::Sha1::Digest partition_hash = xsflib::Sha1::Of(wia.partition_table);
    std::copy(partition_hash.begin(), partition_hash.end(), disc_header.begin() + 0xA0);
    xsflib::StoreBe32(disc_header.data() + 0xB4, 1);
    xsflib::StoreBe64(disc_header.data() + 0xB8, raw_offset);
    xsflib::StoreBe32(disc_header.data() + 0xC0, static_cast<uint32_t>(raw_stored.size()));
    xsflib::StoreBe32(disc_header.data() + 0xC4, static_cast<uint32_t>(wia.groups.size()));
    xsflib::StoreBe64(disc_header.data() + 0xC8, group_offset);
    xsflib::StoreBe32(disc_header.data() + 0xD0, static_cast<uint32_t>(groups_stored.size()));
    disc_header[0xD4] = static_cast<uint8_t>(props.size());
    std::copy(props.begin(), props.end(), disc_header.begin() + 0xD5);

    // wia_file_head_t, with the hashes of wia_disc_t and of itself.
    Bytes file(0x48, 0);
    std::copy_n(wia.rvz ? "RVZ\x01" : "WIA\x01", 4, file.begin());
    xsflib::StoreBe32(file.data() + 4, 0x01000000);
    xsflib::StoreBe32(file.data() + 8, 0x00090000);
    xsflib::StoreBe32(file.data() + 0x0C, static_cast<uint32_t>(disc_header.size()));
    const xsflib::Sha1::Digest disc_hash = xsflib::Sha1::Of(disc_header);
    std::copy(disc_hash.begin(), disc_hash.end(), file.begin() + 0x10);
    xsflib::StoreBe64(file.data() + 0x24, wia.disc_size);
    file.insert(file.end(), disc_header.begin(), disc_header.end());
    file.insert(file.end(), wia.partition_table.begin(), wia.partition_table.end());
    file.insert(file.end(), raw_stored.begin(), raw_stored.end());
    file.resize(data_offset, 0);
    file.insert(file.end(), data.begin(), data.end());
    file.insert(file.end(), groups_stored.begin(), groups_stored.end());
    xsflib::StoreBe64(file.data() + 0x2C, file.size());
    const xsflib::Sha1::Digest head_hash = xsflib::Sha1::Of({file.data(), 0x34});
    std::copy(head_hash.begin(), head_hash.end(), file.begin() + 0x34);

    return file;
}

} // namespace disc_writers
