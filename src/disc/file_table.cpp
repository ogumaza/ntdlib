// SPDX-License-Identifier: MIT

#include "disc/file_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "disc/encodings.h"
#include "ntdlib/disc/source.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"

namespace ntdlib::disc::detail
{

bool ReadHostFile(const std::filesystem::path& path, std::vector<uint8_t>& data)
{
    std::error_code error;

    return std::filesystem::is_regular_file(path, error) && xsflib::ReadFile(path, data, kMaxFileSize);
}

std::map<std::string, FileEntry> ParseFileTable(std::span<const uint8_t> table, int offset_shift,
                                                const std::string& damaged)
{
    if (table.size() < 12 || table[0] != 1)
    {
        throw Error(damaged);
    }

    const uint32_t count = xsflib::LoadBe32(table.data() + 8);
    if (!count || count > table.size() / 12)
    {
        throw Error(damaged);
    }

    const size_t names = size_t{count} * 12;
    const auto name = [&](uint32_t index)
    {
        const size_t start = names + (xsflib::LoadBe32(table.data() + size_t{index} * 12) & 0xFFFFFF);
        const auto end =
            std::find(table.begin() + static_cast<std::ptrdiff_t>(std::min(start, table.size())), table.end(), 0);
        if (start >= table.size() || end == table.end())
        {
            throw Error(damaged);
        }

        return DecodeShiftJis(table.subspan(start, static_cast<size_t>(end - table.begin()) - start));
    };

    // Each directory ends at the index in its entry. The stack holds the open directories and their paths.
    std::map<std::string, FileEntry> files;
    std::vector<std::pair<uint32_t, std::string>> open = {{count, ""}};
    for (uint32_t i = 1; i < count; ++i)
    {
        while (i >= open.back().first)
        {
            open.pop_back();
        }

        const uint8_t* entry = table.data() + size_t{i} * 12;
        if (entry[0])
        {
            const uint32_t end = xsflib::LoadBe32(entry + 8);
            if (end <= i || end > open.back().first)
            {
                throw Error(damaged);
            }

            const std::string folder = name(i);
            open.push_back({end, folder == "." ? open.back().second : open.back().second + folder + "/"});
        }
        else
        {
            files[open.back().second + name(i)] = {uint64_t{xsflib::LoadBe32(entry + 4)} << offset_shift,
                                                   xsflib::LoadBe32(entry + 8), i};
        }
    }

    return files;
}

} // namespace ntdlib::disc::detail
