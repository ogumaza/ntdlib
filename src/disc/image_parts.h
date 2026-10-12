// SPDX-License-Identifier: MIT

// The parts of the image readers that ntdlib::disc and ntdlib::disc_containers share: an image file's pieces, the magic
// words of WBFS and WDF, and the base of the formats that store a disc in blocks.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

#include "ntdlib/disc/image.h"
#include "ntdlib/disc/source.h"

namespace ntdlib::disc::detail
{

namespace fs = std::filesystem;

constexpr uint64_t kSector = 0x8000;     // a Wii disc sector, with its hashes
constexpr uint64_t kSectorData = 0x7C00; // the data in a sector, without the hashes

// The largest block that any of the formats may use. The limit stops a damaged header from requesting absurd amounts of
// memory.
constexpr uint64_t kMaxBlockSize = 1ull << 30;

// The first bytes of a WBFS file and of a WDF file.
constexpr std::string_view kWbfsMagic = "WBFS";
constexpr std::string_view kWdfMagic{
    "WII\x01"
    "DISC",
    8};

// The files of a possibly split image: the first piece, then any continuation files. Only the Wii's WBFS files continue
// in .wbf1 to .wbf9.
std::vector<fs::path> Pieces(const fs::path& path, Console console);

// An image stored in blocks of one size. FetchBlock fetches each block. The last block read is cached.
class BlockImage : public Image
{
public:
    void Read(uint64_t offset, uint8_t* out, size_t size) override
    {
        ReadByBlocks(offset, out, size, block_size_,
                     [this](uint64_t index, size_t within, uint8_t* piece, size_t n)
                     {
                         if (index != cached_index_)
                         {
                             cached_index_ = std::numeric_limits<uint64_t>::max();
                             cached_present_ = FetchBlock(index, cached_);
                             cached_index_ = index;
                         }

                         if (cached_present_)
                         {
                             std::copy_n(cached_.begin() + static_cast<std::ptrdiff_t>(within), n, piece);
                         }
                         else
                         {
                             std::fill_n(piece, n, uint8_t{0});
                         }
                     });
    }

protected:
    // Fills `out` with block `index` and resizes it to the block size. Returns false if the block is all zeros.
    virtual bool FetchBlock(uint64_t index, std::vector<uint8_t>& out) = 0;

    uint64_t block_size_ = 0;

private:
    uint64_t cached_index_ = std::numeric_limits<uint64_t>::max();
    bool cached_present_ = false;
    std::vector<uint8_t> cached_;
};

} // namespace ntdlib::disc::detail
