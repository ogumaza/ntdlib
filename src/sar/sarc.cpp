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

std::vector<uint8_t> WithSarcFile(std::span<const uint8_t> sarc, std::span<const uint8_t> old,
                                  std::span<const uint8_t> data)
{
    const std::vector<SarcFile> files = SarcFiles(sarc);
    const auto is_old = [&old](const SarcFile& file)
    {
        return file.data.data() == old.data() && file.data.size() == old.size();
    };
    if (std::ranges::none_of(files, is_old))
    {
        throw std::invalid_argument("the data isn't one of the SARC's files");
    }

    // The old place, the next file's data and the end of the last file's data, from the SARC's start.
    const auto offset_of = [&sarc](const SarcFile& file)
    {
        return static_cast<uint64_t>(file.data.data() - sarc.data());
    };
    const uint64_t start = offset_of(*std::ranges::find_if(files, is_old));
    const uint64_t end = start + old.size();
    std::optional<uint64_t> next;
    uint64_t last_end = 0;
    for (const SarcFile& file : files)
    {
        last_end = std::max<uint64_t>(last_end, offset_of(file) + file.data.size());
        if (!is_old(file) && offset_of(file) >= start)
        {
            next = std::min(next.value_or(UINT64_MAX), offset_of(file));
        }
    }

    // A place after the last file's data keeps the old place's alignment: the largest power of two that divides its
    // offset, up to kMaxAlignment.
    constexpr uint64_t kMaxAlignment = 0x2000;
    uint64_t place = start;
    std::vector<uint8_t> bytes(sarc.begin(), sarc.begin() + static_cast<std::ptrdiff_t>(start));
    if (!next)
    {
        bytes.insert(bytes.end(), data.begin(), data.end());
        bytes.insert(bytes.end(), sarc.begin() + static_cast<std::ptrdiff_t>(end), sarc.end());
    }
    else if (data.size() <= *next - start)
    {
        bytes.insert(bytes.end(), data.begin(), data.end());
        bytes.resize(static_cast<std::size_t>(*next), 0);
        bytes.insert(bytes.end(), sarc.begin() + static_cast<std::ptrdiff_t>(*next), sarc.end());
    }
    else
    {
        const uint64_t alignment = std::min(start & (~start + 1), kMaxAlignment);
        place = (last_end + alignment - 1) / alignment * alignment;
        bytes.resize(static_cast<std::size_t>(end), 0);
        bytes.insert(bytes.end(), sarc.begin() + static_cast<std::ptrdiff_t>(end),
                     sarc.begin() + static_cast<std::ptrdiff_t>(last_end));
        bytes.resize(static_cast<std::size_t>(place), 0);
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    if (bytes.size() > UINT32_MAX)
    {
        throw std::invalid_argument("the new SARC passes 4 GB");
    }

    const bool big_endian = sarc[6] == 0xFE && sarc[7] == 0xFF;
    const auto store = [&bytes, big_endian](std::size_t at, uint64_t value)
    {
        big_endian ? xsflib::StoreBe32(&bytes[at], static_cast<uint32_t>(value))
                   : xsflib::StoreLe32(&bytes[at], static_cast<uint32_t>(value));
    };
    store(8, bytes.size());

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
        if (data_start + node_start == start && node_start <= node_end && node_end - node_start == old.size())
        {
            store(node + 8, place - data_start);
            store(node + 12, place - data_start + data.size());
        }
    }

    return bytes;
}

} // namespace ntdlib::sar
