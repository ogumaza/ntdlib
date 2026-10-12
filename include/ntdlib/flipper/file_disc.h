// SPDX-License-Identifier: MIT

// A disc or a Wii game partition made of files: its 0x440-byte header (sys/boot.bin), a file system table generated
// from the files' paths, and the files' data after the table. Game mode builds one from a rip's FILE chunks. The game
// then finds the files it requires, such as its sound archive, by their paths.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/flipper/disc.h"

namespace ntdlib::flipper
{

class FileDisc final : public Disc
{
public:
    // `header` is the disc's or the partition's header. The table's offset and size in it are filled in. Each file is a
    // path on the disc and its data. Paths use '/' between names and have no leading '/' ("Sound/sound.brsar"). The
    // data must outlive the disc. The table and the header give offsets and sizes in units of `offset_unit` bytes: 4 on
    // the Wii and 1 on the GameCube.
    FileDisc(std::vector<uint8_t> header, const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files,
             uint32_t offset_unit);

    // Reads the header, the table and the files. Each file starts on a 32-byte boundary. Zeros fill the space between
    // the files and the rest of the last file's last 32-byte block. The disc interface transfers whole 32-byte blocks.
    // A read of a file's last block therefore takes up to 31 bytes past the file's end. The data ends with the last
    // file's last block. A disc without files ends with the table. A read past the end returns false.
    bool Read(uint64_t offset, void* out, std::size_t size) override;

private:
    struct Placed
    {
        uint64_t offset;
        std::span<const uint8_t> data;
    };

    std::vector<uint8_t> header_;
    std::vector<uint8_t> fst_;
    uint64_t fst_offset_ = 0;
    std::vector<Placed> files_; // in order of their offsets
};

} // namespace ntdlib::flipper
