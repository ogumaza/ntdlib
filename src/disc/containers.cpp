// SPDX-License-Identifier: MIT

// The container formats that store a disc compressed or in sparse blocks: GCZ, CISO, WIA and RVZ. Dolphin's
// docs/WiaAndRvz.md describes WIA and RVZ. The page on CISO at https://wit.wiimm.de describes CISO. GCZ is Dolphin's
// format of zlib-compressed blocks.

#include "ntdlib/disc/containers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/sha1.h"

#include "disc/image_parts.h"
#include "ntdlib/disc/compression.h"
#include "ntdlib/disc/junk.h"

namespace ntdlib::disc
{
namespace
{

using detail::BlockImage;
using detail::kMaxBlockSize;
using detail::kSector;
using detail::kSectorData;

// Dolphin's GCZ: a little-endian header, a table of block offsets and a table of their Adler-32 checksums, then the
// zlib-compressed blocks. Bit 63 of an offset marks a block stored uncompressed.
class GczImage : public BlockImage
{
public:
    explicit GczImage(std::unique_ptr<Source> source) : source_(std::move(source))
    {
        uint8_t header[32];
        source_->Read(0, header, sizeof(header));
        compressed_size_ = xsflib::LoadLe64(header + 8);
        const uint64_t data_size = xsflib::LoadLe64(header + 16);
        block_size_ = xsflib::LoadLe32(header + 24);
        const uint32_t count = xsflib::LoadLe32(header + 28);
        if (!block_size_ || block_size_ > kMaxBlockSize || count != (data_size + block_size_ - 1) / block_size_ ||
            32 + uint64_t{count} * 12 > source_->Size())
        {
            throw Error("the GCZ header is invalid");
        }

        const std::vector<uint8_t> tables = source_->Read(32, size_t{count} * 12);
        for (uint32_t i = 0; i < count; ++i)
        {
            pointers_.push_back(xsflib::LoadLe64(tables.data() + i * 8));
            checksums_.push_back(xsflib::LoadLe32(tables.data() + uint64_t{count} * 8 + i * 4));
        }

        data_start_ = 32 + uint64_t{count} * 12;
    }

    std::string Format() const override
    {
        return "GCZ";
    }

protected:
    bool FetchBlock(uint64_t index, std::vector<uint8_t>& out) override
    {
        if (index >= pointers_.size())
        {
            return false;
        }

        const uint64_t kRaw = 1ull << 63;
        const uint64_t start = pointers_[index] & ~kRaw;
        const uint64_t end = index + 1 < pointers_.size() ? pointers_[index + 1] & ~kRaw : compressed_size_;
        if (end < start || end - start > kMaxBlockSize)
        {
            throw Error("the GCZ block table is invalid");
        }

        const std::vector<uint8_t> stored = source_->Read(data_start_ + start, static_cast<size_t>(end - start));
        if (Adler32(stored) != checksums_[index])
        {
            throw Error("GCZ block " + std::to_string(index) + " doesn't match its checksum");
        }

        out.assign(static_cast<size_t>(block_size_), 0);
        if (pointers_[index] & kRaw)
        {
            std::copy_n(stored.begin(), std::min(stored.size(), out.size()), out.begin());
        }
        else
        {
            Inflate(stored, out);
        }

        return true;
    }

private:
    std::unique_ptr<Source> source_;
    std::vector<uint64_t> pointers_;
    std::vector<uint32_t> checksums_;
    uint64_t data_start_ = 0;
    uint64_t compressed_size_ = 0;
};

// CISO, also called WBI: "CISO", a little-endian block size and a byte for each block, then the stored blocks in order
// from offset 0x8000. A block's byte is 1 if the file stores the block.
class CisoImage : public BlockImage
{
public:
    explicit CisoImage(std::unique_ptr<Source> source) : source_(std::move(source))
    {
        const std::vector<uint8_t> header = source_->Read(0, kHeaderSize);
        block_size_ = xsflib::LoadLe32(header.data() + 4);
        if (!block_size_ || block_size_ > kMaxBlockSize)
        {
            throw Error("the CISO header is invalid");
        }

        int64_t stored = 0;
        for (size_t i = 8; i < kHeaderSize; ++i)
        {
            slots_.push_back(header[i] ? stored++ : -1);
        }
    }

    std::string Format() const override
    {
        return "CISO";
    }

protected:
    bool FetchBlock(uint64_t index, std::vector<uint8_t>& out) override
    {
        if (index >= slots_.size() || slots_[index] < 0)
        {
            return false;
        }

        out = source_->Read(kHeaderSize + static_cast<uint64_t>(slots_[index]) * block_size_,
                            static_cast<size_t>(block_size_));

        return true;
    }

private:
    static constexpr size_t kHeaderSize = 0x8000;

    std::unique_ptr<Source> source_;
    std::vector<int64_t> slots_; // each block's index among the stored blocks, or -1
};

// WIA and RVZ (Dolphin's format based on WIA). The disc is divided into chunks. Each group entry stores one compressed
// chunk. Raw data entries cover the disc outside Wii partitions and store it as on the disc. A partition's data is
// stored decrypted and without its hashes: 0x7C00 bytes for each 0x8000 on the disc. It's stored in two segments: the
// system files, then the rest. Each chunk of partition data starts with lists of the hashes that differ from recomputed
// ones. Readers that only read the data skip those lists.
class WiaImage : public Image
{
public:
    // A wia_part_data_t: a run of a partition's sectors and the groups that store them.
    struct Segment
    {
        uint32_t first_sector;
        uint32_t sectors;
        uint32_t first_group;
        uint32_t groups;
    };

    // With `wii`, the reader also reads the table of the Wii partitions that the image stores apart. A GameCube disc
    // has none.
    WiaImage(std::unique_ptr<Source> source, bool wii);

    std::string Format() const override
    {
        return rvz_ ? "RVZ" : "WIA";
    }

    void Read(uint64_t offset, uint8_t* out, size_t size) override;

    std::unique_ptr<PartitionData> OpenPartitionData(uint64_t data_offset) override;

    // Returns the data of a group that holds `size` bytes after `exception_lists` lists of hash exceptions. The bytes
    // are disc data, or partition data without hashes. In RVZ, the junk starts `junk_offset` bytes into the disc or
    // into the partition's data. The result stays valid until the next call.
    const std::vector<uint8_t>& Group(uint32_t index, size_t size, size_t exception_lists, uint64_t junk_offset);

    uint64_t ChunkSize() const
    {
        return chunk_size_;
    }

private:
    struct Raw
    {
        uint64_t offset;
        uint64_t size;
        uint32_t first_group;
        uint32_t groups;
    };

    struct GroupEntry
    {
        uint64_t offset;
        uint32_t stored_size;
        bool compressed;
        uint32_t packed_size; // RVZ: the size before unpacking, or 0 if the group isn't packed
    };

    // The newest version of the formats that this reader supports: 1.00 for both (encoded as 0xAABBCCDD for A.BB.CC).
    static constexpr uint32_t kVersion = 0x01000000;

    // These read the tables that `disc`, the wia_disc_t, points to: the Wii partitions into partitions_, `raw_count`
    // raw data entries into raw_ and `group_count` group entries into groups_.
    void ReadPartitions(std::span<const uint8_t> disc);
    void ReadRawEntries(std::span<const uint8_t> disc, uint32_t raw_count);
    void ReadGroupEntries(std::span<const uint8_t> disc, uint32_t group_count);

    // Reads one of the tables. Tables are compressed like the groups. `size` is the size after decompression.
    std::vector<uint8_t> Table(uint64_t offset, uint32_t stored_size, size_t size);

    // Decodes the data of `group` into `out` (see Group). `out` holds zeros on entry. A group that stores nothing
    // leaves `out` unchanged.
    void DecodeGroup(const GroupEntry& group, size_t exception_lists, uint64_t junk_offset, std::span<uint8_t> out);

    // Decodes WIA's PURGE method. The data holds `exception_lists` lists and padding to 4 bytes, then segments of data
    // over zeros, then a SHA-1. Each segment gives an offset, a size and the bytes.
    static void Purge(std::span<const uint8_t> stored, size_t exception_lists, std::span<uint8_t> out);

    // Decodes RVZ's packing: runs of data, and runs of junk given as seeds.
    static void Unpack(std::span<const uint8_t> packed, std::span<uint8_t> out, uint64_t junk_offset);

    std::unique_ptr<Source> source_;
    bool rvz_ = false;
    Method method_ = Method::kNone;
    std::vector<uint8_t> props_;
    uint64_t chunk_size_ = 0;
    std::array<uint8_t, 0x80> disc_head_{};
    std::vector<std::array<Segment, 2>> partitions_;
    std::vector<Raw> raw_;
    std::vector<GroupEntry> groups_;
    uint32_t cached_group_ = std::numeric_limits<uint32_t>::max();
    std::vector<uint8_t> cached_;
};

// Whether `data` hashes to the SHA-1 at `hash`.
bool MatchesHash(std::span<const uint8_t> data, const uint8_t* hash)
{
    const xsflib::Sha1::Digest digest = xsflib::Sha1::Of(data);

    return std::equal(digest.begin(), digest.end(), hash);
}

// The number of bytes that `exception_lists` lists of hash exceptions take at the start of `data`.
size_t SkipExceptionLists(std::span<const uint8_t> data, size_t exception_lists)
{
    size_t position = 0;
    for (size_t i = 0; i < exception_lists; ++i)
    {
        if (position + 2 > data.size())
        {
            throw Error("a group's hash exceptions are truncated");
        }

        position += 2 + size_t{xsflib::LoadBe16(data.data() + position)} * 22;
    }
    if (position > data.size())
    {
        throw Error("a group's hash exceptions are truncated");
    }

    return position;
}

WiaImage::WiaImage(std::unique_ptr<Source> source, bool wii) : source_(std::move(source))
{
    uint8_t head[0x48];
    source_->Read(0, head, sizeof(head));
    rvz_ = head[0] == 'R';
    const std::string name = Format();
    if (!MatchesHash({head, 0x34}, head + 0x34))
    {
        throw Error("the " + name + " header doesn't match its hash");
    }
    if (xsflib::LoadBe32(head + 8) > kVersion)
    {
        throw Error("this " + name + " file requires a newer reader");
    }

    // wia_disc_t. It may be shorter or longer than the version this reader supports.
    const uint32_t disc_size = xsflib::LoadBe32(head + 0x0C);
    if (disc_size > 0x1000)
    {
        throw Error("the " + name + " header is invalid");
    }

    std::vector<uint8_t> disc = source_->Read(0x48, disc_size);
    if (!MatchesHash(disc, head + 0x10))
    {
        throw Error("the " + name + " disc header doesn't match its hash");
    }

    disc.resize(std::max<size_t>(disc.size(), 0xDC));
    const uint32_t method = xsflib::LoadBe32(disc.data() + 4);
    chunk_size_ = xsflib::LoadBe32(disc.data() + 0x0C);
    std::copy_n(disc.begin() + 0x10, disc_head_.size(), disc_head_.begin());
    if (method > (rvz_ ? 5u : 4u) || (rvz_ && method == 1))
    {
        throw Error(name + " compression method " + std::to_string(method) + " isn't supported");
    }

    method_ = static_cast<Method>(method);
    props_.assign(disc.begin() + 0xD5, disc.begin() + 0xD5 + std::min<uint8_t>(disc[0xD4], 7));
    const bool power_of_two = (chunk_size_ & (chunk_size_ - 1)) == 0;
    if (!chunk_size_ || chunk_size_ > kMaxBlockSize ||
        (chunk_size_ % 0x200000 && !(rvz_ && chunk_size_ >= kSector && power_of_two)))
    {
        throw Error("the " + name + " chunk size is invalid");
    }

    // A Wii disc's partitions. A GameCube disc has none.
    if (wii)
    {
        ReadPartitions(disc);
    }

    // The raw data and group tables. Both counts are checked before either table is read.
    const uint32_t raw_count = xsflib::LoadBe32(disc.data() + 0xB4);
    const uint32_t group_count = xsflib::LoadBe32(disc.data() + 0xC4);
    if (raw_count > (1u << 20) || group_count > (1u << 24))
    {
        throw Error("the " + name + " tables are invalid");
    }

    ReadRawEntries(disc, raw_count);
    ReadGroupEntries(disc, group_count);
}

void WiaImage::ReadPartitions(std::span<const uint8_t> disc)
{
    const uint32_t partition_count = xsflib::LoadBe32(disc.data() + 0x90);
    const uint32_t partition_size = xsflib::LoadBe32(disc.data() + 0x94);
    if (partition_count > 64 || partition_size > 0x1000)
    {
        throw Error("the " + Format() + " partition table is invalid");
    }

    const std::vector<uint8_t> partitions =
        source_->Read(xsflib::LoadBe64(disc.data() + 0x98), size_t{partition_count} * partition_size);
    if (!MatchesHash(partitions, disc.data() + 0xA0))
    {
        throw Error("the " + Format() + " partition table doesn't match its hash");
    }

    for (uint32_t i = 0; i < partition_count; ++i)
    {
        std::vector<uint8_t> entry(partitions.begin() + i * partition_size,
                                   partitions.begin() + (i + 1) * partition_size);
        entry.resize(std::max<size_t>(entry.size(), 0x30));
        std::array<Segment, 2> segments;
        for (int s = 0; s < 2; ++s)
        {
            const uint8_t* p = entry.data() + 0x10 + s * 16;
            segments[s] = {xsflib::LoadBe32(p), xsflib::LoadBe32(p + 4), xsflib::LoadBe32(p + 8),
                           xsflib::LoadBe32(p + 12)};
        }

        partitions_.push_back(segments);
    }
}

void WiaImage::ReadRawEntries(std::span<const uint8_t> disc, uint32_t raw_count)
{
    // The first raw data entry starts at 0x80, after the header stored in wia_disc_t, but the entry's data starts at 0.
    // Each entry is therefore rounded down to a sector.
    const std::vector<uint8_t> raw =
        Table(xsflib::LoadBe64(disc.data() + 0xB8), xsflib::LoadBe32(disc.data() + 0xC0), size_t{raw_count} * 24);
    for (uint32_t i = 0; i < raw_count; ++i)
    {
        const uint8_t* p = raw.data() + i * 24;
        const uint64_t offset = xsflib::LoadBe64(p);
        const uint64_t aligned = offset - offset % kSector;
        raw_.push_back(
            {aligned, xsflib::LoadBe64(p + 8) + offset - aligned, xsflib::LoadBe32(p + 16), xsflib::LoadBe32(p + 20)});
    }
}

void WiaImage::ReadGroupEntries(std::span<const uint8_t> disc, uint32_t group_count)
{
    const size_t group_entry = rvz_ ? 12 : 8;
    const std::vector<uint8_t> groups =
        Table(xsflib::LoadBe64(disc.data() + 0xC8), xsflib::LoadBe32(disc.data() + 0xD0), group_count * group_entry);
    for (uint32_t i = 0; i < group_count; ++i)
    {
        const uint8_t* p = groups.data() + i * group_entry;
        const uint32_t stored = xsflib::LoadBe32(p + 4);
        if (rvz_)
        {
            // The top bit marks a group as compressed with the file's method. The method NONE leaves the data
            // unchanged.
            const bool compressed = (stored >> 31) != 0 && method_ != Method::kNone;
            groups_.push_back(
                {uint64_t{xsflib::LoadBe32(p)} * 4, stored & 0x7FFFFFFF, compressed, xsflib::LoadBe32(p + 8)});
        }
        else
        {
            groups_.push_back({uint64_t{xsflib::LoadBe32(p)} * 4, stored, method_ > Method::kPurge, 0});
        }
    }
}

std::vector<uint8_t> WiaImage::Table(uint64_t offset, uint32_t stored_size, size_t size)
{
    std::vector<uint8_t> stored = source_->Read(offset, stored_size);
    std::vector<uint8_t> table;
    if (method_ == Method::kNone)
    {
        table = std::move(stored);
    }
    else if (method_ == Method::kPurge)
    {
        table.resize(size);
        Purge(stored, 0, table);
    }
    else
    {
        table = Decompress(method_, props_, stored, size);
    }

    if (table.size() < size)
    {
        throw Error("the " + Format() + " tables are truncated");
    }

    return table;
}

void WiaImage::Purge(std::span<const uint8_t> stored, size_t exception_lists, std::span<uint8_t> out)
{
    size_t position = SkipExceptionLists(stored, exception_lists);
    position = (position + 3) & ~size_t{3};
    if (stored.size() < 20)
    {
        throw Error("a PURGE group is truncated");
    }

    const size_t end = stored.size() - 20;
    std::fill(out.begin(), out.end(), uint8_t{0});
    while (position < end)
    {
        if (end - position < 8)
        {
            throw Error("a PURGE group is damaged");
        }

        const uint32_t offset = xsflib::LoadBe32(stored.data() + position);
        const uint32_t size = xsflib::LoadBe32(stored.data() + position + 4);
        position += 8;
        if (size > end - position || offset > out.size() || size > out.size() - offset)
        {
            throw Error("a PURGE group is damaged");
        }

        std::copy_n(stored.begin() + static_cast<std::ptrdiff_t>(position), size, out.begin() + offset);
        position += size;
    }
}

void WiaImage::Unpack(std::span<const uint8_t> packed, std::span<uint8_t> out, uint64_t junk_offset)
{
    size_t position = 0;
    size_t written = 0;
    while (written < out.size())
    {
        if (packed.size() - position < 4)
        {
            throw Error("an RVZ group's packed data is truncated");
        }

        uint32_t run = xsflib::LoadBe32(packed.data() + position);
        position += 4;
        const bool junk = (run & 0x80000000) != 0;
        run &= 0x7FFFFFFF;
        const size_t n = std::min<size_t>(run, out.size() - written);
        if (junk)
        {
            if (packed.size() - position < 68)
            {
                throw Error("an RVZ group's packed data is truncated");
            }

            JunkGenerator generator(packed.data() + position);
            position += 68;
            generator.Skip(static_cast<size_t>((junk_offset + written) % kSector));
            generator.Generate(out.data() + written, n);
        }
        else
        {
            if (packed.size() - position < run)
            {
                throw Error("an RVZ group's packed data is truncated");
            }

            std::copy_n(packed.begin() + static_cast<std::ptrdiff_t>(position), n, out.begin() + written);
            position += run;
        }

        written += n;
    }
}

void WiaImage::DecodeGroup(const GroupEntry& group, size_t exception_lists, uint64_t junk_offset,
                           std::span<uint8_t> out)
{
    if (!group.stored_size)
    {
        return;
    }

    std::vector<uint8_t> stored = source_->Read(group.offset, group.stored_size);
    if (method_ == Method::kPurge)
    {
        Purge(stored, exception_lists, out);

        return;
    }

    // A list holds at most 3328 exceptions. That's enough for every hash and padding byte of 2 MiB.
    const size_t limit = std::max<size_t>(out.size(), group.packed_size) + exception_lists * (2 + 3328 * 22);
    const std::vector<uint8_t> body = group.compressed ? Decompress(method_, props_, stored, limit) : std::move(stored);
    size_t position = SkipExceptionLists(body, exception_lists);
    if (!group.compressed && exception_lists)
    {
        position = (position + 3) & ~size_t{3};
    }

    const std::span<const uint8_t> data = std::span(body).subspan(std::min(position, body.size()));
    if (group.packed_size)
    {
        Unpack(data, out, junk_offset);

        return;
    }

    if (data.size() < out.size())
    {
        throw Error("a group in the " + Format() + " file holds too little data");
    }

    std::copy_n(data.begin(), out.size(), out.begin());
}

const std::vector<uint8_t>& WiaImage::Group(uint32_t index, size_t size, size_t exception_lists, uint64_t junk_offset)
{
    if (index == cached_group_ && cached_.size() == size)
    {
        return cached_;
    }
    if (index >= groups_.size())
    {
        throw Error("the " + Format() + " file is missing a group");
    }

    cached_group_ = std::numeric_limits<uint32_t>::max();
    cached_.assign(size, 0);
    DecodeGroup(groups_[index], exception_lists, junk_offset, cached_);
    cached_group_ = index;

    return cached_;
}

void WiaImage::Read(uint64_t offset, uint8_t* out, size_t size)
{
    std::fill_n(out, size, uint8_t{0});
    if (offset < disc_head_.size())
    {
        std::copy_n(disc_head_.begin() + static_cast<std::ptrdiff_t>(offset),
                    std::min<uint64_t>(size, disc_head_.size() - offset), out);
    }

    for (const Raw& raw : raw_)
    {
        // The part of the read that the entry holds, after the disc's head.
        const uint64_t start = std::max({offset, raw.offset, uint64_t{disc_head_.size()}});
        const uint64_t end = std::min(offset + size, raw.offset + raw.size);
        if (start >= end)
        {
            continue;
        }

        // Each group holds one chunk of the entry's data. The last group may hold less. The entry's data past its
        // last group reads as 0.
        const auto read = [&](uint64_t g, size_t within, uint8_t* piece, size_t n)
        {
            if (g >= raw.groups)
            {
                return;
            }

            const uint64_t group_start = raw.offset + g * chunk_size_;
            const uint64_t group_size = std::min(chunk_size_, raw.offset + raw.size - group_start);
            const std::vector<uint8_t>& data =
                Group(static_cast<uint32_t>(raw.first_group + g), static_cast<size_t>(group_size), 0, group_start);
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(within), n, piece);
        };
        ReadByBlocks(start - raw.offset, out + (start - offset), static_cast<size_t>(end - start), chunk_size_, read);
    }
}

// The data of one partition of a WIA or RVZ file.
class WiaPartitionData : public PartitionData
{
public:
    WiaPartitionData(WiaImage& image, const std::array<WiaImage::Segment, 2>& segments)
        : image_(image),
          segments_(segments),
          sectors_per_group_(std::max<uint64_t>(1, image.ChunkSize() / kSector)),
          exception_lists_(static_cast<size_t>(std::max<uint64_t>(1, image.ChunkSize() / 0x200000)))
    {
    }

    void Read(uint64_t offset, uint8_t* out, size_t size) override
    {
        ReadByBlocks(offset, out, size, kSectorData, [this](uint64_t index, size_t within, uint8_t* piece, size_t n)
                     { ReadSector(segments_[0].first_sector + index, within, piece, n); });
    }

private:
    // Reads `n` bytes at `within` in the data of the partition's sector `sector`, counted from the disc's start.
    void ReadSector(uint64_t sector, size_t within, uint8_t* out, size_t n)
    {
        const auto contains = [sector](const WiaImage::Segment& s)
        {
            return s.first_sector <= sector && sector < uint64_t{s.first_sector} + s.sectors;
        };

        const auto segment = std::find_if(segments_.begin(), segments_.end(), contains);
        if (segment == segments_.end())
        {
            throw Error("the image has no data for a read of the game partition");
        }

        const uint64_t group = (sector - segment->first_sector) / sectors_per_group_;
        if (group >= segment->groups)
        {
            throw Error("the image is missing data of the game partition");
        }

        const uint64_t data_sector = segments_[0].first_sector;
        const uint64_t group_sectors = std::min(sectors_per_group_, segment->sectors - group * sectors_per_group_);
        const uint64_t junk_offset = (segment->first_sector - data_sector + group * sectors_per_group_) * kSectorData;
        const std::vector<uint8_t>& data =
            image_.Group(static_cast<uint32_t>(segment->first_group + group),
                         static_cast<size_t>(group_sectors * kSectorData), exception_lists_, junk_offset);
        const uint64_t start = (sector - segment->first_sector) % sectors_per_group_ * kSectorData + within;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(start), n, out);
    }

    WiaImage& image_;
    std::array<WiaImage::Segment, 2> segments_;
    uint64_t sectors_per_group_;
    size_t exception_lists_;
};

std::unique_ptr<PartitionData> WiaImage::OpenPartitionData(uint64_t data_offset)
{
    for (const std::array<Segment, 2>& segments : partitions_)
    {
        if (uint64_t{segments[0].first_sector} * kSector == data_offset && segments[0].sectors)
        {
            return std::make_unique<WiaPartitionData>(*this, segments);
        }
    }

    return nullptr;
}

} // namespace

std::unique_ptr<Image> OpenContainer(const std::filesystem::path& path, Console console)
{
    auto source = std::make_unique<Source>(detail::Pieces(path, console), "image");
    uint8_t magic[8] = {};
    const size_t stored = static_cast<size_t>(std::min<uint64_t>(sizeof(magic), source->Size()));
    source->Read(0, magic, stored);
    const std::string format = ContainerFormat({magic, stored});
    if (format == "GCZ")
    {
        return std::make_unique<GczImage>(std::move(source));
    }
    if (format == "WIA" || format == "RVZ")
    {
        return std::make_unique<WiaImage>(std::move(source), console == Console::kWii);
    }
    if (format == "CISO")
    {
        return std::make_unique<CisoImage>(std::move(source));
    }

    return nullptr;
}

} // namespace ntdlib::disc
