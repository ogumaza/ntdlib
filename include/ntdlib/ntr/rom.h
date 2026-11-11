// SPDX-License-Identifier: MIT

// The DS's cartridge image (an NDS ROM): its header, its ARM9 and ARM7 binaries, their overlays and its file system.
// The layouts follow GBATEK's description of the cartridge header and of the file system: a file name table (FNT) of
// directories, and a file allocation table (FAT) of each file's start and end in the image. A DSi-enhanced or DSi-only
// image adds the ARM9i and ARM7i binaries in the header's extended part.
//
// The parsed data points into the image. The image must outlive it.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ntdlib::ntr
{

// Where a binary lies in the image and where the console loads it.
struct Binary
{
    friend bool operator==(const Binary&, const Binary&) = default;

    uint32_t rom_offset = 0;
    uint32_t entry = 0;
    uint32_t ram_address = 0;
    uint32_t size = 0;
};

// The header's unit code: which consoles run the image.
enum class UnitCode : uint8_t
{
    kDs = 0,
    kDsiEnhanced = 2,
    kDsiOnly = 3,
};

// The fields of the cartridge header that the file system and the binaries use.
struct RomHeader
{
    std::string title;      // 12 bytes, without the zeros at the end
    std::string game_code;  // 4 characters, such as "AMCE"
    std::string maker_code; // 2 characters
    uint8_t unit_code = 0;
    uint8_t version = 0; // the ROM's version: 0 for the first release, then 1, 2 and so on
    Binary arm9;
    Binary arm7;
    uint32_t fnt_offset = 0;
    uint32_t fnt_size = 0;
    uint32_t fat_offset = 0;
    uint32_t fat_size = 0;
    uint32_t arm9_overlay_offset = 0;
    uint32_t arm9_overlay_size = 0;
    uint32_t arm7_overlay_offset = 0;
    uint32_t arm7_overlay_size = 0;
    uint32_t banner_offset = 0;
    uint32_t used_size = 0;   // the bytes of the image that the cartridge's data takes, from the start
    uint32_t header_size = 0; // 0x4000

    // The DSi's binaries, in a DSi-enhanced or DSi-only image.
    std::optional<Binary> arm9i;
    std::optional<Binary> arm7i;
};

// An overlay: a part of a binary that the game loads at run time. Each one is a file of the file system.
struct Overlay
{
    friend bool operator==(const Overlay&, const Overlay&) = default;

    uint32_t id = 0;
    uint32_t ram_address = 0;
    uint32_t ram_size = 0;
    uint32_t bss_size = 0;
    uint32_t static_init_start = 0;
    uint32_t static_init_end = 0;
    uint32_t file_id = 0;
    uint32_t flags = 0; // bit 24: compressed. Bits 0 to 23: the compressed size
};

// A file of the file system. A file without a name, such as an overlay's, has an empty path.
struct RomFile
{
    uint32_t id = 0;
    std::string path; // the directories and the name, separated by '/', from the root
    uint32_t start = 0;
    uint32_t end = 0; // one past the last byte
};

class Rom
{
public:
    // Reads an image. Throws xsflib::FormatError when the header, the FNT or the FAT doesn't fit in the image, or when
    // a file's bytes don't.
    static Rom Parse(std::span<const uint8_t> image);

    const RomHeader& Header() const;

    std::span<const uint8_t> Image() const;

    // The binaries as the image holds them. An ARM9 binary may hold compressed parts.
    std::span<const uint8_t> Arm9() const;
    std::span<const uint8_t> Arm7() const;

    // The DSi's binaries. Empty in an image for the DS alone.
    // Empty too where a trimmed image ends before the binary.
    std::span<const uint8_t> Arm9i() const;
    std::span<const uint8_t> Arm7i() const;

    const std::vector<Overlay>& Arm9Overlays() const;
    const std::vector<Overlay>& Arm7Overlays() const;

    // Every file, by ID: the FAT's order.
    const std::vector<RomFile>& Files() const;

    std::span<const uint8_t> FileData(uint32_t id) const;

    // The file at `path`, with '/' between the directories and the name. The match is exact. Null when no file has the
    // path.
    const RomFile* Find(std::string_view path) const;

private:
    std::span<const uint8_t> image_;
    RomHeader header_;
    std::vector<Overlay> arm9_overlays_;
    std::vector<Overlay> arm7_overlays_;
    std::vector<RomFile> files_;
};

} // namespace ntdlib::ntr
