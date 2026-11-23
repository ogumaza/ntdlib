// SPDX-License-Identifier: MIT

// Builds the parts of GameCube and Wii discs that tests write byte by byte: a disc header with a console's magic word,
// a Wii image's partition table with one game partition and the partition's header, and the system files of an
// extracted disc. YAGCD ("Yet Another GameCube Documentation") documents the disc header. WiiBrew's "Wii disc" page
// documents the partition table and the partition's header.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/disc/header.h"
#include "ntdlib/disc/image.h"
#include "ntdlib/testing/write_file.h"

namespace ntdlib::testing::disc_files
{

// Where a test Wii disc keeps its partition table and its game partition, and where the partition's header places the
// TMD, from the partition's start.
inline constexpr uint32_t kPartitionTable = 0x40000;
inline constexpr uint32_t kPartition = 0x50000;
inline constexpr uint32_t kTmdOffset = 0x2C0;

// A disc header (sys/boot.bin) with the magic word of `console` and zeros elsewhere.
inline std::vector<uint8_t> DiscHeader(ntdlib::disc::Console console)
{
    std::vector<uint8_t> header(ntdlib::disc::kHeaderSize, 0);
    if (console == ntdlib::disc::Console::kWii)
    {
        xsflib::StoreBe32(header.data() + ntdlib::disc::kWiiMagicOffset, ntdlib::disc::kWiiMagic);
    }
    else
    {
        xsflib::StoreBe32(header.data() + ntdlib::disc::kGameCubeMagicOffset, ntdlib::disc::kGameCubeMagic);
    }

    return header;
}

// Writes the partition table and the game partition's header into the Wii disc image `image`. The table's first group
// lists one partition, at kPartition, of type 0: the game partition. Its entries follow the four groups. The header
// gives the TMD's size, the TMD's offset (kTmdOffset), and the offset of the partition's data from kPartition and the
// data's size. The table and the header store each offset and the data's size divided by 4. Throws
// std::invalid_argument when `image` ends before the end of the header.
inline void WriteGamePartition(std::vector<uint8_t>& image, uint32_t tmd_size, uint32_t data_offset, uint64_t data_size)
{
    if (image.size() < kPartition + kTmdOffset)
    {
        throw std::invalid_argument("the image ends before the partition's header");
    }

    xsflib::StoreBe32(image.data() + kPartitionTable, 1);
    xsflib::StoreBe32(image.data() + kPartitionTable + 4, (kPartitionTable + 0x20) >> 2);
    xsflib::StoreBe32(image.data() + kPartitionTable + 0x20, kPartition >> 2);
    xsflib::StoreBe32(image.data() + kPartitionTable + 0x24, 0);
    xsflib::StoreBe32(image.data() + kPartition + 0x2A4, tmd_size);
    xsflib::StoreBe32(image.data() + kPartition + 0x2A8, kTmdOffset >> 2);
    xsflib::StoreBe32(image.data() + kPartition + 0x2B8, data_offset >> 2);
    xsflib::StoreBe32(image.data() + kPartition + 0x2BC, static_cast<uint32_t>(data_size >> 2));
}

// The system files of an extracted disc: the files in its sys folder.
struct SystemFiles
{
    std::vector<uint8_t> boot;      // boot.bin: the disc header, or a Wii partition's header
    std::vector<uint8_t> bi2;       // bi2.bin
    std::vector<uint8_t> apploader; // apploader.img
    std::vector<uint8_t> dol;       // main.dol
    std::vector<uint8_t> fst;       // fst.bin: the file system table
};

// Writes each file of `files` that isn't empty into `root`/sys. Makes the folders first. Throws std::runtime_error when
// a file can't be written.
inline void WriteSystemFiles(const std::filesystem::path& root, const SystemFiles& files)
{
    const std::filesystem::path sys = root / "sys";
    std::filesystem::create_directories(sys);
    const auto write = [&sys](const char* name, const std::vector<uint8_t>& data)
    {
        if (!data.empty())
        {
            WriteFile(sys / name, data);
        }
    };

    write("boot.bin", files.boot);
    write("bi2.bin", files.bi2);
    write("apploader.img", files.apploader);
    write("main.dol", files.dol);
    write("fst.bin", files.fst);
}

} // namespace ntdlib::testing::disc_files
