// SPDX-License-Identifier: MIT

#include "ntdlib/dol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "xsflib/bytes.h"

namespace ntdlib
{
namespace
{

constexpr std::size_t kHeaderSize = 0x100;
constexpr int kSectionCount = 18; // 7 text, then 11 data

} // namespace

std::optional<Dol> Dol::Parse(std::span<const uint8_t> file, bool skip_damaged_sections)
{
    // The header holds the file offsets of the sections at 0x00, their load addresses at 0x48 and their sizes at 0x90.
    // The entry point is at 0xe0.
    if (file.size() < kHeaderSize)
    {
        return std::nullopt;
    }

    Dol dol;
    for (int i = 0; i < kSectionCount; i++)
    {
        const uint32_t offset = xsflib::LoadBe32(&file[i * 4]);
        const uint32_t address = xsflib::LoadBe32(&file[0x48 + i * 4]);
        const uint32_t size = xsflib::LoadBe32(&file[0x90 + i * 4]);
        if (size == 0)
        {
            continue;
        }
        if (offset < kHeaderSize || offset > file.size() || size > file.size() - offset)
        {
            if (skip_damaged_sections)
            {
                continue;
            }

            return std::nullopt;
        }

        dol.sections_.push_back({address, file.subspan(offset, size)});
    }

    dol.entry_ = xsflib::LoadBe32(&file[0xe0]);

    return dol;
}

uint64_t Dol::Size(std::span<const uint8_t> header)
{
    uint64_t size = kHeaderSize;
    for (int i = 0; i < kSectionCount; i++)
    {
        size = std::max<uint64_t>(size,
                                  uint64_t{xsflib::LoadBe32(&header[i * 4])} + xsflib::LoadBe32(&header[0x90 + i * 4]));
    }

    return size;
}

} // namespace ntdlib
