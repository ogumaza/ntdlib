// SPDX-License-Identifier: MIT

#include "ntdlib/sar/sarc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::sar
{

std::vector<SarcFile> SarcFiles(std::span<const uint8_t> sarc)
{
    std::vector<SarcFile> files;
    if (sarc.size() < 0x14 || std::memcmp(sarc.data(), "SARC", 4) != 0)
    {
        return files;
    }

    const xsflib::Reader r(sarc, sarc[6] == 0xFE && sarc[7] == 0xFF);
    const std::size_t sfat = r.U16(4);
    if (sfat + 0xC > sarc.size() || std::memcmp(&sarc[sfat], "SFAT", 4) != 0)
    {
        return files;
    }

    // The data's start is 64-bit. The sum with a damaged node's end then can't wrap round in a 32-bit build.
    const uint64_t data = r.U32(0xC);
    const std::size_t nodes = sfat + r.U16(sfat + 4);
    const std::size_t count = r.U16(sfat + 6);

    // The names lie between the SFNT block's header and the data.
    std::span<const uint8_t> names;
    const std::size_t sfnt = nodes + count * 16;
    if (sfnt + 8 <= sarc.size() && std::memcmp(&sarc[sfnt], "SFNT", 4) == 0)
    {
        const std::size_t first = sfnt + r.U16(sfnt + 4);
        const std::size_t end = static_cast<std::size_t>(std::min<uint64_t>(data, sarc.size()));
        if (first < end)
        {
            names = sarc.subspan(first, end - first);
        }
    }

    for (std::size_t i = 0; i < count && nodes + i * 16 + 16 <= sarc.size(); ++i)
    {
        const uint32_t attributes = r.U32(nodes + i * 16 + 4);
        const uint32_t start = r.U32(nodes + i * 16 + 8);
        const uint32_t end = r.U32(nodes + i * 16 + 12);
        if (start > end || data + end > sarc.size())
        {
            continue;
        }

        SarcFile file{{}, sarc.subspan(static_cast<std::size_t>(data + start), end - start)};
        const std::size_t offset = static_cast<std::size_t>(attributes & 0xFFFFFF) * 4;
        if (attributes >> 24 != 0 && offset < names.size())
        {
            const auto first = names.begin() + static_cast<std::ptrdiff_t>(offset);
            const auto nul = std::find(first, names.end(), uint8_t{0});
            if (nul != names.end())
            {
                file.name.assign(first, nul);
            }
        }

        files.push_back(std::move(file));
    }

    return files;
}

namespace
{

// Whether `file` is the file whose data `old` spans.
bool IsFile(const SarcFile& file, std::span<const uint8_t> old)
{
    return file.data.data() == old.data() && file.data.size() == old.size();
}

// The place of a file's data from the SARC's start, the start of the next file's data, and the end of the last file's
// data.
struct FilePlace
{
    uint64_t start = 0;
    uint64_t end = 0;
    std::optional<uint64_t> next;
    uint64_t last_end = 0;
};

// The place of the data that `old` spans, the data of one of `files` of `sarc`.
FilePlace PlaceOf(std::span<const uint8_t> sarc, const std::vector<SarcFile>& files, std::span<const uint8_t> old)
{
    const auto offset_of = [&sarc](const SarcFile& file)
    {
        return static_cast<uint64_t>(file.data.data() - sarc.data());
    };
    FilePlace place;
    place.start = offset_of(*std::ranges::find_if(files, [&old](const SarcFile& file) { return IsFile(file, old); }));
    place.end = place.start + old.size();
    for (const SarcFile& file : files)
    {
        place.last_end = std::max<uint64_t>(place.last_end, offset_of(file) + file.data.size());
        if (!IsFile(file, old) && offset_of(file) >= place.start)
        {
            place.next = std::min(place.next.value_or(UINT64_MAX), offset_of(file));
        }
    }

    return place;
}

// The bytes of a SARC with a file's new data, and where the data starts.
struct NewSarc
{
    std::vector<uint8_t> bytes;
    uint64_t place = 0;
};

// The bytes of `sarc` with `data` in place of the data at `old`. The data takes the old place where it fits before the
// next file's data. A place after the last file's data keeps the old place's alignment: the largest power of two that
// divides its offset, up to kMaxAlignment.
NewSarc LaidBytes(std::span<const uint8_t> sarc, const FilePlace& old, std::span<const uint8_t> data)
{
    constexpr uint64_t kMaxAlignment = 0x2000;
    uint64_t place = old.start;
    std::vector<uint8_t> bytes(sarc.begin(), sarc.begin() + static_cast<std::ptrdiff_t>(old.start));
    if (!old.next)
    {
        bytes.insert(bytes.end(), data.begin(), data.end());
        bytes.insert(bytes.end(), sarc.begin() + static_cast<std::ptrdiff_t>(old.end), sarc.end());
    }
    else if (data.size() <= *old.next - old.start)
    {
        bytes.insert(bytes.end(), data.begin(), data.end());
        bytes.resize(static_cast<std::size_t>(*old.next), 0);
        bytes.insert(bytes.end(), sarc.begin() + static_cast<std::ptrdiff_t>(*old.next), sarc.end());
    }
    else
    {
        const uint64_t alignment = std::min(old.start & (~old.start + 1), kMaxAlignment);
        place = (old.last_end + alignment - 1) / alignment * alignment;
        bytes.resize(static_cast<std::size_t>(old.end), 0);
        bytes.insert(bytes.end(), sarc.begin() + static_cast<std::ptrdiff_t>(old.end),
                     sarc.begin() + static_cast<std::ptrdiff_t>(old.last_end));
        bytes.resize(static_cast<std::size_t>(place), 0);
        bytes.insert(bytes.end(), data.begin(), data.end());
    }

    return NewSarc{std::move(bytes), place};
}

// Stores the low 32 bits of `value` at `at` in `bytes`, in the SARC's byte order.
void Store32(std::vector<uint8_t>& bytes, bool big_endian, std::size_t at, uint64_t value)
{
    big_endian ? xsflib::StoreBe32(&bytes[at], static_cast<uint32_t>(value))
               : xsflib::StoreLe32(&bytes[at], static_cast<uint32_t>(value));
}

} // namespace

std::vector<uint8_t> WithSarcFile(std::span<const uint8_t> sarc, std::span<const uint8_t> old,
                                  std::span<const uint8_t> data)
{
    const std::vector<SarcFile> files = SarcFiles(sarc);
    if (std::ranges::none_of(files, [&old](const SarcFile& file) { return IsFile(file, old); }))
    {
        throw std::invalid_argument("the data isn't one of the SARC's files");
    }

    const FilePlace old_place = PlaceOf(sarc, files, old);
    NewSarc laid = LaidBytes(sarc, old_place, data);
    if (laid.bytes.size() > UINT32_MAX)
    {
        throw std::invalid_argument("the new SARC passes 4 GB");
    }

    const bool big_endian = sarc[6] == 0xFE && sarc[7] == 0xFF;
    Store32(laid.bytes, big_endian, 8, laid.bytes.size());

    // Every node that gives the old place, read as SarcFiles reads the nodes.
    const xsflib::Reader r(sarc, big_endian);
    const std::size_t sfat = r.U16(4);
    const std::size_t nodes = sfat + r.U16(sfat + 4);
    const std::size_t count = r.U16(sfat + 6);
    const uint64_t data_start = r.U32(0xC);
    for (std::size_t i = 0; i < count && nodes + i * 16 + 16 <= sarc.size(); ++i)
    {
        const std::size_t node = nodes + i * 16;
        const uint32_t node_start = r.U32(node + 8);
        const uint32_t node_end = r.U32(node + 12);
        if (data_start + node_start == old_place.start && node_start <= node_end && node_end - node_start == old.size())
        {
            Store32(laid.bytes, big_endian, node + 8, laid.place - data_start);
            Store32(laid.bytes, big_endian, node + 12, laid.place - data_start + data.size());
        }
    }

    return std::move(laid.bytes);
}

} // namespace ntdlib::sar
