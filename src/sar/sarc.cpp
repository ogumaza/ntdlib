// SPDX-License-Identifier: MIT

#include "ntdlib/sar/sarc.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

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

} // namespace ntdlib::sar
