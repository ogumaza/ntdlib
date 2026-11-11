// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/rom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::ntr
{

namespace
{

constexpr std::size_t kHeaderBytes = 0x200;
constexpr std::size_t kOverlayEntryBytes = 32;
constexpr std::size_t kFatEntryBytes = 8;
constexpr std::size_t kFntEntryBytes = 8;
constexpr uint16_t kRootDirectory = 0xF000;
constexpr uint16_t kMaxDirectories = 0x1000;

Binary ReadBinary(const xsflib::Reader& r, std::size_t at)
{
    return {r.U32(at), r.U32(at + 4), r.U32(at + 8), r.U32(at + 12)};
}

std::string Text(const xsflib::Reader& r, std::size_t at, std::size_t size)
{
    std::string s;
    for (std::size_t i = 0; i < size; i++)
    {
        s.push_back(static_cast<char>(r.U8(at + i)));
    }
    while (!s.empty() && s.back() == '\0')
    {
        s.pop_back();
    }

    return s;
}

bool Fits(const xsflib::Reader& r, uint32_t offset, uint32_t size)
{
    return offset <= r.Size() && size <= r.Size() - offset;
}

void CheckSpan(const xsflib::Reader& r, uint32_t offset, uint32_t size, const char* what)
{
    if (!Fits(r, offset, size))
    {
        throw xsflib::FormatError(std::string(what) + " lies past the end of the image");
    }
}

std::vector<Overlay> ReadOverlays(const xsflib::Reader& r, uint32_t offset, uint32_t size)
{
    CheckSpan(r, offset, size, "an overlay table");

    std::vector<Overlay> overlays;
    for (std::size_t at = offset; at + kOverlayEntryBytes <= std::size_t{offset} + size; at += kOverlayEntryBytes)
    {
        overlays.push_back({r.U32(at), r.U32(at + 4), r.U32(at + 8), r.U32(at + 12), r.U32(at + 16), r.U32(at + 20),
                            r.U32(at + 24), r.U32(at + 28)});
    }

    return overlays;
}

// Gives each named file its path. A directory's sub-table lists its entries in order: a byte with the name's length
// (bit 7 set for a directory), the name, and a directory's ID. A zero byte ends the list. The files of a directory
// take consecutive IDs from the directory's first file ID.
void NameFiles(const xsflib::Reader& r, uint32_t fnt, uint32_t fnt_size, std::vector<RomFile>& files)
{
    const uint16_t directories = r.U16(fnt + 6);
    if (directories == 0 || directories > kMaxDirectories || std::size_t{directories} * kFntEntryBytes > fnt_size)
    {
        throw xsflib::FormatError("the file name table's directory count is out of range");
    }

    const xsflib::Reader table(r.Sub(fnt, fnt_size));
    std::vector<bool> visited(directories, false);
    std::vector<std::pair<uint16_t, std::string>> pending = {{kRootDirectory, ""}};
    while (!pending.empty())
    {
        const auto [directory, prefix] = pending.back();
        pending.pop_back();
        const uint16_t index = directory & 0x0FFF;
        if ((directory & 0xF000) != kRootDirectory || index >= directories || visited[index])
        {
            throw xsflib::FormatError("the file name table names a directory twice or out of range");
        }
        visited[index] = true;

        std::size_t at = table.U32(std::size_t{index} * kFntEntryBytes);
        uint32_t id = table.U16(std::size_t{index} * kFntEntryBytes + 4);
        for (uint8_t kind = table.U8(at++); kind != 0; kind = table.U8(at++))
        {
            const std::size_t length = kind & 0x7F;
            std::string name = Text(table, at, length);
            at += length;
            if (kind & 0x80)
            {
                pending.emplace_back(table.U16(at), prefix + name + '/');
                at += 2;
                continue;
            }

            if (id >= files.size())
            {
                throw xsflib::FormatError("the file name table names a file past the file allocation table");
            }
            files[id++].path = prefix + name;
        }
    }
}

} // namespace

Rom Rom::Parse(std::span<const uint8_t> image)
{
    const xsflib::Reader r(image);
    if (image.size() < kHeaderBytes)
    {
        throw xsflib::FormatError("the image is shorter than a cartridge header");
    }

    Rom rom;
    rom.image_ = image;
    RomHeader& h = rom.header_;
    h.title = Text(r, 0, 12);
    h.game_code = Text(r, 12, 4);
    h.maker_code = Text(r, 16, 2);
    h.unit_code = r.U8(0x12);
    h.version = r.U8(0x1E);
    h.arm9 = ReadBinary(r, 0x20);
    h.arm7 = ReadBinary(r, 0x30);
    h.fnt_offset = r.U32(0x40);
    h.fnt_size = r.U32(0x44);
    h.fat_offset = r.U32(0x48);
    h.fat_size = r.U32(0x4C);
    h.arm9_overlay_offset = r.U32(0x50);
    h.arm9_overlay_size = r.U32(0x54);
    h.arm7_overlay_offset = r.U32(0x58);
    h.arm7_overlay_size = r.U32(0x5C);
    h.banner_offset = r.U32(0x68);
    h.used_size = r.U32(0x80);
    h.header_size = r.U32(0x84);
    if (h.unit_code & 2)
    {
        // The extended header holds each DSi binary's ROM offset, a word, its RAM address and its size.
        // A trimmed image may end before a DSi binary. The header then has no entry for it. The rest of the image
        // parses as before.
        const Binary arm9i = ReadBinary(r, 0x1C0);
        const Binary arm7i = ReadBinary(r, 0x1D0);
        if (Fits(r, arm9i.rom_offset, arm9i.size))
        {
            h.arm9i = Binary{arm9i.rom_offset, 0, arm9i.ram_address, arm9i.size};
        }
        if (Fits(r, arm7i.rom_offset, arm7i.size))
        {
            h.arm7i = Binary{arm7i.rom_offset, 0, arm7i.ram_address, arm7i.size};
        }
    }

    CheckSpan(r, h.arm9.rom_offset, h.arm9.size, "the ARM9 binary");
    CheckSpan(r, h.arm7.rom_offset, h.arm7.size, "the ARM7 binary");
    rom.arm9_overlays_ = ReadOverlays(r, h.arm9_overlay_offset, h.arm9_overlay_size);
    rom.arm7_overlays_ = ReadOverlays(r, h.arm7_overlay_offset, h.arm7_overlay_size);

    // The FAT gives each file's start and end.
    CheckSpan(r, h.fat_offset, h.fat_size, "the file allocation table");
    for (uint32_t id = 0; id < h.fat_size / kFatEntryBytes; id++)
    {
        const std::size_t at = h.fat_offset + std::size_t{id} * kFatEntryBytes;
        RomFile file{id, "", r.U32(at), r.U32(at + 4)};
        if (file.end < file.start || file.end > image.size())
        {
            throw xsflib::FormatError("file " + std::to_string(id) + " lies past the end of the image");
        }
        rom.files_.push_back(std::move(file));
    }

    CheckSpan(r, h.fnt_offset, h.fnt_size, "the file name table");
    NameFiles(r, h.fnt_offset, h.fnt_size, rom.files_);

    return rom;
}

const RomHeader& Rom::Header() const
{
    return header_;
}

std::span<const uint8_t> Rom::Image() const
{
    return image_;
}

std::span<const uint8_t> Rom::Arm9() const
{
    return image_.subspan(header_.arm9.rom_offset, header_.arm9.size);
}

std::span<const uint8_t> Rom::Arm7() const
{
    return image_.subspan(header_.arm7.rom_offset, header_.arm7.size);
}

std::span<const uint8_t> Rom::Arm9i() const
{
    return header_.arm9i ? image_.subspan(header_.arm9i->rom_offset, header_.arm9i->size) : std::span<const uint8_t>{};
}

std::span<const uint8_t> Rom::Arm7i() const
{
    return header_.arm7i ? image_.subspan(header_.arm7i->rom_offset, header_.arm7i->size) : std::span<const uint8_t>{};
}

const std::vector<Overlay>& Rom::Arm9Overlays() const
{
    return arm9_overlays_;
}

const std::vector<Overlay>& Rom::Arm7Overlays() const
{
    return arm7_overlays_;
}

const std::vector<RomFile>& Rom::Files() const
{
    return files_;
}

std::span<const uint8_t> Rom::FileData(uint32_t id) const
{
    const RomFile& file = files_.at(id);

    return image_.subspan(file.start, file.end - file.start);
}

const RomFile* Rom::Find(std::string_view path) const
{
    for (const RomFile& file : files_)
    {
        if (!file.path.empty() && file.path == path)
        {
            return &file;
        }
    }

    return nullptr;
}

} // namespace ntdlib::ntr
