// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/compact_image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/rounding.h"

namespace ntdlib::ntr
{

namespace
{

constexpr std::size_t kAlignment = 0x200;
constexpr uint8_t kFill = 0xFF;

// The header's fields that the layout changes.
constexpr std::size_t kCapacityField = 0x14;
constexpr std::size_t kArm9SizeField = 0x2C;
constexpr std::size_t kArm7OffsetField = 0x30;
constexpr std::size_t kFntOffsetField = 0x40;
constexpr std::size_t kFatOffsetField = 0x48;
constexpr std::size_t kArm9OverlaysField = 0x50;
constexpr std::size_t kArm7OverlaysField = 0x58;
constexpr std::size_t kBannerField = 0x68;
constexpr std::size_t kUsedSizeField = 0x80;
constexpr std::size_t kHeaderBytes = 0x200;
constexpr std::size_t kCrcBytes = 0x15E;

// The chip's capacity is 128 KB shifted left by the byte at 0x14.
constexpr uint64_t kSmallestCapacity = 0x20000;

// The ARM9 binary's footer: 0xDEC00621, the module parameters' offset and 0.
constexpr uint32_t kFooterMagic = 0xDEC00621;
constexpr std::size_t kFooterBytes = 12;

constexpr std::size_t kFatEntryBytes = 8;

class Layout
{
public:
    explicit Layout(std::span<const uint8_t> start) : out_(start.begin(), start.end())
    {
    }

    // Puts `data` on the next multiple of 0x200 bytes, and returns its offset.
    uint32_t Place(std::span<const uint8_t> data)
    {
        out_.resize(AlignUp(out_.size(), kAlignment), kFill);
        const auto at = static_cast<uint32_t>(out_.size());
        out_.insert(out_.end(), data.begin(), data.end());

        return at;
    }

    // Puts `data` right after the bytes so far.
    void Append(std::span<const uint8_t> data)
    {
        out_.insert(out_.end(), data.begin(), data.end());
    }

    void Store(std::size_t at, uint32_t value)
    {
        xsflib::StoreLe32(&out_[at], value);
    }

    std::vector<uint8_t>& Bytes()
    {
        return out_;
    }

private:
    std::vector<uint8_t> out_;
};

// The ARM9 binary's footer in the image, or nothing.
std::span<const uint8_t> Arm9Footer(const Rom& rom)
{
    const std::span<const uint8_t> image = rom.Image();
    const Binary& arm9 = rom.Header().arm9;
    const uint64_t at = uint64_t{arm9.rom_offset} + arm9.size;
    if (at + kFooterBytes > image.size() || xsflib::LoadLe32(&image[at]) != kFooterMagic)
    {
        return {};
    }

    return image.subspan(static_cast<std::size_t>(at), kFooterBytes);
}

// Places an overlay table and writes its offset, or 0 for an empty table.
void PlaceTable(Layout& layout, const Rom& rom, std::size_t field)
{
    const std::span<const uint8_t> image = rom.Image();
    const uint32_t offset = xsflib::LoadLe32(&image[field]);
    const uint32_t size = xsflib::LoadLe32(&image[field + 4]);
    layout.Store(field, size == 0 ? 0 : layout.Place(image.subspan(offset, size)));
}

// The kept files' IDs in the order of their offsets in the image.
std::vector<uint32_t> KeptInOrder(const Rom& rom, const CompactParts& parts)
{
    std::vector<uint32_t> ids;
    for (const auto& [id, bytes] : parts.files)
    {
        if (id >= rom.Files().size())
        {
            throw std::invalid_argument("file " + std::to_string(id) + " lies past the FAT");
        }
        ids.push_back(id);
    }
    std::ranges::stable_sort(ids,
                             [&rom](uint32_t a, uint32_t b) { return rom.Files()[a].start < rom.Files()[b].start; });

    return ids;
}

// The image of a game that runs only on a DSi, in its layout.
std::vector<uint8_t> WriteDsiImage(const Rom& rom, const CompactParts& parts, const std::vector<uint32_t>& kept)
{
    const RomHeader& header = rom.Header();
    if (parts.arm9.size() > header.arm9.size)
    {
        throw std::invalid_argument("the new ARM9 binary is larger than the image's");
    }

    std::vector<uint8_t> out(rom.Image().begin(), rom.Image().end());
    std::ranges::copy(parts.arm9, out.begin() + header.arm9.rom_offset);

    // Each file's bytes outside the sectors of the kept files become zeros. The kept files then get their bytes.
    const DigestSectors sectors = DigestSectorsOf(rom);
    std::vector<std::pair<uint32_t, uint32_t>> kept_sectors;
    for (const uint32_t id : kept)
    {
        kept_sectors.push_back(sectors.Around(rom.Files()[id].start, rom.Files()[id].end));
    }
    std::ranges::sort(kept_sectors);
    const auto zero = [&out](uint32_t first, uint32_t end)
    {
        std::fill(out.begin() + first, out.begin() + end, uint8_t{0});
    };
    for (const RomFile& file : rom.Files())
    {
        uint32_t next = file.start;
        for (const auto& [first, end] : kept_sectors)
        {
            if (first >= file.end)
            {
                break;
            }
            if (end > next)
            {
                zero(next, std::max(next, first));
                next = end;
            }
        }
        if (next < file.end)
        {
            zero(next, file.end);
        }
    }

    for (const uint32_t id : kept)
    {
        const RomFile& file = rom.Files()[id];
        const std::vector<uint8_t>& bytes = parts.files.at(id);
        if (bytes.size() != file.end - file.start)
        {
            throw std::invalid_argument("file " + std::to_string(id) + " changes its size in a DSi image");
        }
        std::ranges::copy(bytes, out.begin() + file.start);
    }

    return out;
}

void SetCapacity(std::vector<uint8_t>& out)
{
    uint8_t capacity = 0;
    while ((kSmallestCapacity << capacity) < out.size())
    {
        capacity++;
    }

    out[kCapacityField] = capacity;
}

} // namespace

std::pair<uint32_t, uint32_t> DigestSectors::Around(uint32_t first, uint32_t end) const
{
    if (size == 0 || first < start || end <= first)
    {
        return {first, end};
    }

    const uint64_t from = start + uint64_t{first - start} / size * size;
    const uint64_t to = start + AlignUp(uint64_t{end} - start, size);
    return {static_cast<uint32_t>(from),
            static_cast<uint32_t>(std::min<uint64_t>(to, std::numeric_limits<uint32_t>::max()))};
}

DigestSectors DigestSectorsOf(const Rom& rom)
{
    constexpr std::size_t kStartField = 0x1E0;
    constexpr std::size_t kSizeField = 0x200;
    const std::span<const uint8_t> image = rom.Image();
    if (rom.Header().unit_code == static_cast<uint8_t>(UnitCode::kDs) || image.size() < kSizeField + 4)
    {
        return {};
    }

    return {.start = xsflib::LoadLe32(&image[kStartField]), .size = xsflib::LoadLe32(&image[kSizeField])};
}

std::vector<uint8_t> WriteCompactImage(const Rom& rom, const CompactParts& parts)
{
    if (rom.Partial())
    {
        throw std::invalid_argument("the image is partial");
    }
    const RomHeader& header = rom.Header();
    if (header.unit_code != static_cast<uint8_t>(UnitCode::kDs) && !RunsInDsiMode(header))
    {
        throw std::invalid_argument("the image is for the DSi");
    }
    if (header.arm9.rom_offset < kHeaderBytes)
    {
        throw std::invalid_argument("the ARM9 binary lies inside the header");
    }
    const std::vector<uint32_t> kept = KeptInOrder(rom, parts);
    if (RunsInDsiMode(header))
    {
        return WriteDsiImage(rom, parts, kept);
    }

    // The header and the space up to the ARM9 binary, the binary and its footer.
    const std::span<const uint8_t> image = rom.Image();
    Layout layout(image.first(header.arm9.rom_offset));
    layout.Append(parts.arm9);
    layout.Append(Arm9Footer(rom));
    layout.Store(kArm9SizeField, static_cast<uint32_t>(parts.arm9.size()));

    // The tables and the ARM7 binary.
    PlaceTable(layout, rom, kArm9OverlaysField);
    layout.Store(kArm7OffsetField, layout.Place(rom.Arm7()));
    PlaceTable(layout, rom, kArm7OverlaysField);
    layout.Store(kFntOffsetField, layout.Place(image.subspan(header.fnt_offset, header.fnt_size)));
    const uint32_t fat = layout.Place(std::vector<uint8_t>(header.fat_size, 0));
    layout.Store(kFatOffsetField, fat);

    // The kept files. Every other file has no bytes.
    for (const uint32_t id : kept)
    {
        const std::vector<uint8_t>& bytes = parts.files.at(id);
        const uint32_t at = layout.Place(bytes);
        layout.Store(fat + id * kFatEntryBytes, at);
        layout.Store(fat + id * kFatEntryBytes + 4, at + static_cast<uint32_t>(bytes.size()));
    }

    // The header's sizes, its capacity and its CRC.
    std::vector<uint8_t>& out = layout.Bytes();
    layout.Store(kBannerField, 0);
    layout.Store(kUsedSizeField, static_cast<uint32_t>(out.size()));
    SetCapacity(out);
    xsflib::StoreLe16(&out[kCrcBytes], Crc16(std::span(out).first(kCrcBytes)));

    return std::move(out);
}

} // namespace ntdlib::ntr
