// SPDX-License-Identifier: MIT

#include "ntdlib/dol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib
{
namespace
{

// The header holds three tables with a word for each section: the sections' file offsets, their load addresses and
// their sizes. The entry point follows the tables.
constexpr std::size_t kHeaderSize = 0x100;
constexpr int kSectionCount = 18; // 7 text, then 11 data
constexpr std::size_t kSectionOffsets = 0x00;
constexpr std::size_t kSectionAddresses = 0x48;
constexpr std::size_t kSectionSizes = 0x90;
constexpr std::size_t kEntryPoint = 0xe0;

// Section `i`'s word in the header's table at `table`.
uint32_t SectionWord(std::span<const uint8_t> header, std::size_t table, int i)
{
    return xsflib::LoadBe32(&header[table + static_cast<std::size_t>(i) * 4]);
}

} // namespace

std::optional<Dol> Dol::Parse(std::span<const uint8_t> file, bool skip_damaged_sections)
{
    if (file.size() < kHeaderSize)
    {
        return std::nullopt;
    }

    Dol dol;
    for (int i = 0; i < kSectionCount; i++)
    {
        const uint32_t offset = SectionWord(file, kSectionOffsets, i);
        const uint32_t address = SectionWord(file, kSectionAddresses, i);
        const uint32_t size = SectionWord(file, kSectionSizes, i);
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

    dol.entry_ = xsflib::LoadBe32(&file[kEntryPoint]);

    return dol;
}

Dol Dol::ParseMain(std::span<const uint8_t> file)
{
    std::optional<Dol> dol = Parse(file);
    if (!dol)
    {
        throw xsflib::FormatError("main.dol isn't a DOL");
    }

    return std::move(*dol);
}

std::span<const uint8_t> Dol::At(uint32_t address) const
{
    for (const Section& section : sections_)
    {
        if (address >= section.address && address - section.address < section.data.size())
        {
            return section.data.subspan(address - section.address);
        }
    }

    return {};
}

uint64_t Dol::Size(std::span<const uint8_t> header)
{
    uint64_t size = kHeaderSize;
    for (int i = 0; i < kSectionCount; i++)
    {
        size = std::max<uint64_t>(
            size, uint64_t{SectionWord(header, kSectionOffsets, i)} + SectionWord(header, kSectionSizes, i));
    }

    return size;
}

} // namespace ntdlib
