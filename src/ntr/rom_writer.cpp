// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/rom_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "ntdlib/ntr/rom.h"

namespace ntdlib::ntr
{

namespace
{

constexpr int64_t kFileAlignment = 0x200;
constexpr uint64_t kTableAlignment = 0x200; // the digest's tables and the used size
constexpr uint64_t kDsiAlignment = 0x80000; // the DSi's part of the image
constexpr std::size_t kHashBytes = 20;
constexpr std::size_t kSignatureBytes = 0x88; // the signature for download play
constexpr std::size_t kCrcBytes = 0x15E;
constexpr std::size_t kWhitelistBytes = 0x160; // the header's bytes that the whitelist's HMAC covers
constexpr std::size_t kSignedBytes = 0xE00;    // the header's bytes that its RSA signature covers
constexpr std::size_t kRsaSignature = 0xF80;
constexpr std::size_t kRsaBytes = 0x80;
constexpr std::size_t kMasterHash = 0x328;
constexpr std::size_t kWhitelistHeaderHash = 0x378;
constexpr std::size_t kWhitelistOverlaysHash = 0x38C;
constexpr uint32_t kMinCapacity = 0x20000; // the chip's capacity is 128 KB shifted left by the byte at 0x14
constexpr std::size_t kCapacityField = 0x14;

uint32_t Load32(std::span<const uint8_t> bytes, std::size_t at)
{
    return xsflib::LoadLe32(&bytes[at]);
}

void Store32(std::vector<uint8_t>& bytes, std::size_t at, uint64_t value)
{
    xsflib::StoreLe32(&bytes[at], static_cast<uint32_t>(value));
}

uint64_t Align(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// `value` rounded up to a multiple of `alignment`, below 0 as well.
int64_t RoundUp(int64_t value, int64_t alignment)
{
    return value >= 0 ? (value + alignment - 1) / alignment * alignment : -(-value / alignment * alignment);
}

bool Same(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    return std::ranges::equal(a, b);
}

bool AllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t b) { return b == 0; });
}

// The banner's size by its version.
uint32_t BannerBytes(std::span<const uint8_t> image, uint32_t offset)
{
    switch (xsflib::LoadLe16(&image[offset]))
    {
    case 2:
        return 0x940;
    case 3:
        return 0xA40;
    case 0x103:
        return 0x23C0;
    default:
        return 0x840;
    }
}

// A file with new bytes: its old place, the start of the next item in the old image, and how far the items from there
// on move.
struct Change
{
    uint32_t id = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t next = 0;
    const std::vector<uint8_t>* bytes = nullptr;
    int64_t delta = 0;
};

[[noreturn]] void RefuseDigest()
{
    throw std::invalid_argument("the DSi image's digest isn't laid out as the SDK's tools lay it out");
}

// The DSi's digest of the old image, from the header's fields.
struct Digest
{
    uint32_t sector = 0;
    uint32_t count = 0; // sectors in a block
    uint32_t ntr_start = 0;
    uint32_t ntr_end = 0;
    uint32_t twl_bytes = 0;
    uint32_t sectors_at = 0;
    uint32_t sectors_bytes = 0;
    uint32_t blocks_at = 0;
    uint32_t blocks_bytes = 0;
    uint32_t dsi_start = 0; // the DSi's part
    uint32_t dsi_end = 0;
};

Digest ReadDigest(std::span<const uint8_t> image, uint64_t data_end, uint32_t used)
{
    Digest d;
    d.sector = Load32(image, 0x200);
    d.count = Load32(image, 0x204);
    d.ntr_start = Load32(image, 0x1E0);
    d.ntr_end = d.ntr_start + Load32(image, 0x1E4);
    d.twl_bytes = Load32(image, 0x1EC);
    d.sectors_at = Load32(image, 0x1F0);
    d.sectors_bytes = Load32(image, 0x1F4);
    d.blocks_at = Load32(image, 0x1F8);
    d.blocks_bytes = Load32(image, 0x1FC);
    d.dsi_start = uint32_t{xsflib::LoadLe16(&image[0x92])} * kDsiAlignment;
    d.dsi_end = Load32(image, 0x210);
    if (d.sector == 0 || d.count == 0 || d.ntr_end != Align(data_end, d.sector) || d.ntr_start > d.ntr_end ||
        (d.ntr_end - d.ntr_start) % d.sector != 0 || d.twl_bytes % d.sector != 0)
    {
        RefuseDigest();
    }
    const uint64_t sectors = (d.ntr_end - d.ntr_start) / d.sector + d.twl_bytes / d.sector;
    const uint64_t entries = Align(sectors, d.count);
    if (d.sectors_at != d.ntr_end || d.sectors_bytes != entries * kHashBytes ||
        d.blocks_at != Align(uint64_t{d.sectors_at} + d.sectors_bytes, kTableAlignment) ||
        d.blocks_bytes != entries / d.count * kHashBytes ||
        used != Align(uint64_t{d.blocks_at} + d.blocks_bytes, kTableAlignment) ||
        d.dsi_start != Align(used, kDsiAlignment) || xsflib::LoadLe16(&image[0x90]) != xsflib::LoadLe16(&image[0x92]) ||
        Load32(image, 0x1E8) < d.dsi_start || uint64_t{Load32(image, 0x1E8)} + d.twl_bytes != d.dsi_end ||
        d.dsi_end > image.size())
    {
        RefuseDigest();
    }

    return d;
}

} // namespace

xsflib::Sha1::Digest HmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> data)
{
    std::array<uint8_t, 64> block{};
    if (key.size() > block.size())
    {
        const xsflib::Sha1::Digest hashed = xsflib::Sha1::Of(key);
        std::ranges::copy(hashed, block.begin());
    }
    else
    {
        std::ranges::copy(key, block.begin());
    }

    std::array<uint8_t, 64> pad{};
    std::ranges::transform(block, pad.begin(), [](uint8_t b) { return static_cast<uint8_t>(b ^ 0x36); });
    xsflib::Sha1 inner;
    inner.Update(pad);
    inner.Update(data);
    const xsflib::Sha1::Digest inner_hash = inner.Finish();

    std::ranges::transform(block, pad.begin(), [](uint8_t b) { return static_cast<uint8_t>(b ^ 0x5C); });
    xsflib::Sha1 outer;
    outer.Update(pad);
    outer.Update(inner_hash);

    return outer.Finish();
}

uint16_t Crc16(std::span<const uint8_t> data)
{
    uint16_t crc = 0xFFFF;
    for (const uint8_t byte : data)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
        }
    }

    return crc;
}

RomWrite WithFiles(std::span<const uint8_t> image, const std::map<uint32_t, std::vector<uint8_t>>& files,
                   const std::optional<DsiKeys>& keys)
{
    const Rom rom = Rom::Parse(image);
    if (image.size() < kRsaSignature + kRsaBytes)
    {
        throw std::invalid_argument("the image's header is cut short");
    }
    const RomHeader& h = rom.Header();

    // The places of the header, the binaries, the tables, the banner and the files with bytes.
    struct Item
    {
        uint32_t start = 0;
        uint32_t end = 0;
        std::optional<uint32_t> file;
    };
    std::vector<Item> items = {{0, h.header_size, {}},
                               {h.arm9.rom_offset, h.arm9.rom_offset + h.arm9.size, {}},
                               {h.arm7.rom_offset, h.arm7.rom_offset + h.arm7.size, {}},
                               {h.fnt_offset, h.fnt_offset + h.fnt_size, {}},
                               {h.fat_offset, h.fat_offset + h.fat_size, {}}};
    for (const auto [offset, size] :
         {std::pair{h.arm9_overlay_offset, h.arm9_overlay_size}, std::pair{h.arm7_overlay_offset, h.arm7_overlay_size}})
    {
        if (size != 0)
        {
            items.push_back({offset, offset + size, {}});
        }
    }
    if (h.banner_offset != 0)
    {
        items.push_back({h.banner_offset, h.banner_offset + BannerBytes(image, h.banner_offset), {}});
    }
    for (const RomFile& file : rom.Files())
    {
        if (file.end > file.start)
        {
            items.push_back({file.start, file.end, file.id});
        }
    }
    uint64_t data_end = 0;
    for (const Item& item : items)
    {
        data_end = std::max<uint64_t>(data_end, item.end);
    }
    const bool dsi = (h.unit_code & 2) != 0;
    if (!dsi)
    {
        data_end = std::max<uint64_t>(data_end, std::min<uint64_t>(h.used_size, image.size()));
    }

    std::set<uint32_t> overlay_files;
    for (const std::vector<Overlay>* overlays : {&rom.Arm9Overlays(), &rom.Arm7Overlays()})
    {
        for (const Overlay& overlay : *overlays)
        {
            overlay_files.insert(overlay.file_id);
        }
    }

    std::vector<Change> changes;
    for (const auto& [id, bytes] : files)
    {
        const std::string name = "file " + std::to_string(id);
        if (id >= rom.Files().size())
        {
            throw std::invalid_argument(name + " lies past the file allocation table");
        }
        if (overlay_files.contains(id))
        {
            throw std::invalid_argument(name + " is an overlay's: its table describes it");
        }
        if (Same(rom.FileData(id), bytes))
        {
            continue;
        }
        const RomFile& file = rom.Files()[id];
        Change change{id, file.start, file.end, static_cast<uint32_t>(data_end), &bytes, 0};
        for (const Item& item : items)
        {
            if (item.file == id)
            {
                continue;
            }
            const bool shares =
                file.end > file.start ? item.start < file.end && item.end > file.start : item.start == file.start;
            if (shares)
            {
                throw std::invalid_argument(name + " shares its place with another item");
            }
            if (item.start >= file.end)
            {
                change.next = std::min(change.next, item.start);
            }
        }
        changes.push_back(change);
    }
    if (changes.empty())
    {
        return {std::vector<uint8_t>(image.begin(), image.end()), {}};
    }
    std::ranges::sort(changes, {}, &Change::start);

    // The NTR part: each new file at its start, and the items after it moved. The last item moves by the change in its
    // size alone.
    std::vector<uint8_t> out;
    out.reserve(image.size());
    std::size_t from = 0;
    for (Change& c : changes)
    {
        out.insert(out.end(), image.begin() + from, image.begin() + c.start);
        out.insert(out.end(), c.bytes->begin(), c.bytes->end());
        const int64_t grown = int64_t{c.start} + static_cast<int64_t>(c.bytes->size()) - c.next;
        c.delta = c.next == data_end ? static_cast<int64_t>(c.bytes->size()) - (c.end - c.start)
                                     : RoundUp(grown, kFileAlignment);
        out.resize(static_cast<std::size_t>(static_cast<int64_t>(out.size()) - grown + c.delta), 0xFF);
        from = c.next;
    }
    out.insert(out.end(), image.begin() + from, image.begin() + static_cast<std::ptrdiff_t>(data_end));
    const auto moved = [&changes](uint64_t offset)
    {
        int64_t to = static_cast<int64_t>(offset);
        for (const Change& c : changes)
        {
            if (c.next <= offset)
            {
                to += c.delta;
            }
        }
        return static_cast<uint64_t>(to);
    };

    // The header's offsets and the FAT.
    for (const std::size_t field : {0x20, 0x30, 0x40, 0x48})
    {
        Store32(out, field, moved(Load32(image, field)));
    }
    for (const std::size_t field : {0x50, 0x58})
    {
        if (Load32(image, field + 4) != 0)
        {
            Store32(out, field, moved(Load32(image, field)));
        }
    }
    if (h.banner_offset != 0)
    {
        Store32(out, 0x68, moved(h.banner_offset));
    }
    const uint64_t fat = moved(h.fat_offset);
    for (const RomFile& file : rom.Files())
    {
        const auto change = std::ranges::find(changes, file.id, &Change::id);
        const uint64_t start = moved(file.start);
        const uint64_t size = change != changes.end() ? change->bytes->size() : file.end - file.start;
        Store32(out, fat + 8 * std::size_t{file.id}, start);
        Store32(out, fat + 8 * std::size_t{file.id} + 4, start + size);
    }

    RomWrite result;
    std::size_t old_tail = 0; // where the old image's data ends
    if (!dsi)
    {
        // The signature for download play follows the data.
        const auto used = static_cast<std::size_t>(data_end);
        Store32(out, 0x80, out.size());
        old_tail = used;
        if (used + kSignatureBytes <= image.size() && image[used] == 'a' && image[used + 1] == 'c')
        {
            old_tail += kSignatureBytes;
            if (!Same(std::span(out).first(kWhitelistBytes), image.first(kWhitelistBytes)))
            {
                result.stale.push_back({"the signature for download play", static_cast<uint32_t>(out.size()), 1});
            }
            out.insert(out.end(), image.begin() + used, image.begin() + static_cast<std::ptrdiff_t>(old_tail));
        }
    }
    else
    {
        const Digest old = ReadDigest(image, data_end, h.used_size);
        const uint32_t ntr_end = static_cast<uint32_t>(Align(out.size(), old.sector));
        out.resize(ntr_end, 0xFF);
        const uint32_t ntr_sectors = (ntr_end - old.ntr_start) / old.sector;
        const uint32_t old_ntr_sectors = (old.ntr_end - old.ntr_start) / old.sector;
        const uint32_t twl_sectors = old.twl_bytes / old.sector;
        const auto entries = static_cast<uint32_t>(Align(uint64_t{ntr_sectors} + twl_sectors, old.count));
        const uint32_t blocks = entries / old.count;
        const std::span<const uint8_t> old_sectors = image.subspan(old.sectors_at, old.sectors_bytes);
        const std::span<const uint8_t> old_blocks = image.subspan(old.blocks_at, old.blocks_bytes);

        // A hash stays where its bytes stay the same. Otherwise it's written again with the key, or left at zeros.
        StaleHash stale_sectors{"the digest's sector hashes", 0, 0};
        StaleHash stale_blocks{"the digest's block hashes", 0, 0};
        const auto rehash =
            [&keys](std::span<uint8_t> hash, std::span<const uint8_t> bytes, StaleHash& stale, uint32_t at)
        {
            if (keys)
            {
                std::ranges::copy(HmacSha1(keys->digest, bytes), hash.begin());
                return;
            }
            if (stale.count++ == 0)
            {
                stale.offset = at;
            }
        };
        std::vector<uint8_t> sectors(std::size_t{entries} * kHashBytes, 0);
        for (uint32_t i = 0; i < ntr_sectors; i++)
        {
            const std::span<const uint8_t> bytes =
                std::span(out).subspan(old.ntr_start + std::size_t{i} * old.sector, old.sector);
            const std::span<uint8_t> hash = std::span(sectors).subspan(std::size_t{i} * kHashBytes, kHashBytes);
            if (i < old_ntr_sectors &&
                Same(bytes, image.subspan(old.ntr_start + std::size_t{i} * old.sector, old.sector)))
            {
                std::ranges::copy(old_sectors.subspan(std::size_t{i} * kHashBytes, kHashBytes), hash.begin());
                continue;
            }
            rehash(hash, bytes, stale_sectors, static_cast<uint32_t>(ntr_end + std::size_t{i} * kHashBytes));
        }
        for (uint32_t j = 0; j < twl_sectors; j++)
        {
            std::ranges::copy(
                old_sectors.subspan((std::size_t{old_ntr_sectors} + j) * kHashBytes, kHashBytes),
                sectors.begin() + static_cast<std::ptrdiff_t>((std::size_t{ntr_sectors} + j) * kHashBytes));
        }

        const auto blocks_at = static_cast<uint32_t>(Align(uint64_t{ntr_end} + sectors.size(), kTableAlignment));
        const std::size_t block_input = std::size_t{old.count} * kHashBytes;
        std::vector<uint8_t> block_hashes(std::size_t{blocks} * kHashBytes, 0);
        for (uint32_t b = 0; b < blocks; b++)
        {
            const std::span<const uint8_t> input = std::span(sectors).subspan(b * block_input, block_input);
            const std::span<uint8_t> hash = std::span(block_hashes).subspan(std::size_t{b} * kHashBytes, kHashBytes);
            if ((b + 1) * block_input <= old_sectors.size() &&
                Same(input, old_sectors.subspan(b * block_input, block_input)))
            {
                std::ranges::copy(old_blocks.subspan(std::size_t{b} * kHashBytes, kHashBytes), hash.begin());
                continue;
            }
            rehash(hash, input, stale_blocks, static_cast<uint32_t>(blocks_at + std::size_t{b} * kHashBytes));
        }
        if (!Same(block_hashes, old_blocks))
        {
            if (keys)
            {
                const xsflib::Sha1::Digest master = HmacSha1(keys->digest, block_hashes);
                std::ranges::copy(master, out.begin() + kMasterHash);
            }
            else
            {
                std::fill_n(out.begin() + kMasterHash, kHashBytes, 0);
                result.stale.push_back({"the digest's master hash", kMasterHash, 1});
            }
        }
        for (const StaleHash& stale : {stale_sectors, stale_blocks})
        {
            if (stale.count != 0)
            {
                result.stale.push_back(stale);
            }
        }

        // The tables, the used size, then the DSi's part on the next multiple of 0x80000 bytes.
        out.insert(out.end(), sectors.begin(), sectors.end());
        out.resize(blocks_at, 0xFF);
        out.insert(out.end(), block_hashes.begin(), block_hashes.end());
        const uint64_t used = Align(out.size(), kTableAlignment);
        const uint64_t dsi_start = Align(used, kDsiAlignment);
        out.resize(dsi_start, 0xFF);
        out.insert(out.end(), image.begin() + old.dsi_start, image.begin() + old.dsi_end);
        old_tail = old.dsi_end;

        const int64_t shift = static_cast<int64_t>(dsi_start) - old.dsi_start;
        const auto in_dsi_part = [&old](uint32_t offset)
        {
            return offset >= old.dsi_start && offset < old.dsi_end;
        };
        for (const std::size_t field : {0x1C0, 0x1D0, 0x1E8, 0x210, 0x220, 0x228})
        {
            const uint32_t offset = Load32(image, field);
            if (in_dsi_part(offset) || (field == 0x210 && offset == old.dsi_end))
            {
                Store32(out, field, static_cast<uint64_t>(offset + shift));
            }
        }
        Store32(out, 0x80, used);
        xsflib::StoreLe16(&out[0x90], static_cast<uint16_t>(dsi_start / kDsiAlignment));
        xsflib::StoreLe16(&out[0x92], static_cast<uint16_t>(dsi_start / kDsiAlignment));
        Store32(out, 0x1E4, ntr_end - old.ntr_start);
        Store32(out, 0x1F0, ntr_end);
        Store32(out, 0x1F4, sectors.size());
        Store32(out, 0x1F8, blocks_at);
        Store32(out, 0x1FC, block_hashes.size());
    }

    // The chip's capacity, then the bytes after the data. A full image fills its chip.
    uint8_t capacity = image[kCapacityField];
    const auto capacity_bytes = [](uint8_t shift)
    {
        return shift < 32 ? uint64_t{kMinCapacity} << shift : 0;
    };
    while (capacity < 32 && out.size() > capacity_bytes(capacity))
    {
        capacity++;
    }
    out[kCapacityField] = capacity;
    const uint64_t size = image.size() == capacity_bytes(image[kCapacityField])
                              ? capacity_bytes(capacity)
                              : std::max<uint64_t>(out.size(), image.size() - old_tail + out.size());
    const std::size_t trailing = std::min<std::size_t>(image.size() - old_tail, size - out.size());
    out.insert(out.end(), image.begin() + static_cast<std::ptrdiff_t>(old_tail),
               image.begin() + static_cast<std::ptrdiff_t>(old_tail + trailing));
    out.resize(size, 0xFF);

    xsflib::StoreLe16(&out[kCrcBytes], Crc16(std::span(out).first(kCrcBytes)));

    // The hashes that no key here can redo.
    const std::span<const uint8_t> header = std::span(out).first(h.header_size);
    if (!AllZero(image.subspan(kRsaSignature, kRsaBytes)) &&
        !Same(header.first(kSignedBytes), image.first(kSignedBytes)))
    {
        result.stale.push_back({"the header's RSA signature", kRsaSignature, 1});
    }
    if (!AllZero(image.subspan(kWhitelistHeaderHash, kHashBytes)) &&
        !Same(header.first(kWhitelistBytes), image.first(kWhitelistBytes)))
    {
        result.stale.push_back({"the whitelist's hash of the header and the binaries", kWhitelistHeaderHash, 1});
    }
    if (!AllZero(image.subspan(kWhitelistOverlaysHash, kHashBytes)) &&
        !Same(std::span(out).subspan(fat, h.fat_size), image.subspan(h.fat_offset, h.fat_size)))
    {
        result.stale.push_back({"the whitelist's hash of the ARM9 overlays and the FAT", kWhitelistOverlaysHash, 1});
    }
    result.image = std::move(out);

    return result;
}

} // namespace ntdlib::ntr
