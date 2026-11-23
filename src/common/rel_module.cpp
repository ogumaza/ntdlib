// SPDX-License-Identifier: MIT

// Modules of a GameCube game's code (see rel_module.h).

#include "ntdlib/rel_module.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib
{
namespace
{

constexpr std::size_t kHeaderSize = 0x30;
constexpr std::size_t kSectionEntrySize = 8;
constexpr std::size_t kImportSize = 8;

// The type of relocation that writes the address of its target into a word.
constexpr uint8_t kAddress32 = 1;

} // namespace

RelModule::RelModule(std::span<const uint8_t> file, std::string name) : file_(file), name_(std::move(name))
{
    const xsflib::Reader r(file, true);
    r.Sub(0, kHeaderSize);
    const uint32_t id = r.U32(0);
    const uint32_t count = r.U32(0xc);
    const std::span<const uint8_t> entries = r.Sub(r.U32(0x10), std::size_t{count} * kSectionEntrySize);
    const xsflib::Reader e(entries, true);
    for (uint32_t s = 0; s < count; s++)
    {
        Section section;
        section.offset = e.U32(s * kSectionEntrySize) & ~1u;
        if (section.offset != 0)
        {
            section.size = e.U32(s * kSectionEntrySize + 4);
            r.Sub(section.offset, section.size);
        }

        sections_.push_back(std::move(section));
    }

    const std::span<const uint8_t> imports = r.Sub(r.U32(0x28), r.U32(0x2c));
    const xsflib::Reader i(imports, true);
    for (std::size_t at = 0; at + kImportSize <= imports.size(); at += kImportSize)
    {
        if (i.U32(at) == id)
        {
            ReadRelocations(i.U32(at + 4));
        }
    }
}

std::size_t RelModule::At(uint32_t section, uint32_t offset) const
{
    if (section >= sections_.size() || sections_[section].offset == 0 || offset >= sections_[section].size)
    {
        throw xsflib::FormatError(name_ + "'s section " + std::to_string(section) + " has no data at offset " +
                                  std::to_string(offset));
    }

    return sections_[section].offset + offset;
}

std::optional<std::size_t> RelModule::Pointer(std::size_t at) const
{
    const auto found = pointers_.find(at);
    if (found == pointers_.end())
    {
        return std::nullopt;
    }

    return found->second;
}

std::span<const uint8_t> RelModule::Object(std::size_t at) const
{
    for (const Section& section : sections_)
    {
        if (section.offset == 0 || at < section.offset || at - section.offset >= section.size)
        {
            continue;
        }

        const auto offset = static_cast<uint32_t>(at - section.offset);
        const auto next = section.targets.upper_bound(offset);
        const std::size_t end =
            next == section.targets.end() ? section.size : std::min<std::size_t>(*next, section.size);

        return file_.subspan(at, end - offset);
    }

    throw xsflib::FormatError("an object lies outside " + name_ + "'s sections");
}

std::vector<std::pair<uint16_t, std::size_t>> RelModule::TablePointers(std::size_t at) const
{
    std::vector<std::pair<uint16_t, std::size_t>> pointers;
    const std::size_t words = Object(at).size() / 4;
    for (std::size_t i = 0; i < words; i++)
    {
        if (const std::optional<std::size_t> pointer = Pointer(at + i * 4))
        {
            pointers.emplace_back(static_cast<uint16_t>(i), *pointer);
        }
    }

    return pointers;
}

void RelModule::ReadRelocations(std::size_t at)
{
    const auto visit = [this](const RelRelocation& relocation)
    {
        if (relocation.target_section >= sections_.size())
        {
            throw xsflib::FormatError(name_ + " has a relocation into section " +
                                      std::to_string(relocation.target_section) + ", past its sections");
        }

        // Every target starts an object. A word that the relocation sets points at its target.
        Section& into = sections_[relocation.target_section];
        into.targets.insert(relocation.addend);
        const bool in_file = relocation.section < sections_.size() && sections_[relocation.section].offset != 0 &&
                             std::size_t{relocation.offset} + 4 <= sections_[relocation.section].size &&
                             into.offset != 0 && relocation.addend < into.size;
        if (relocation.type == kAddress32 && in_file)
        {
            pointers_[sections_[relocation.section].offset + relocation.offset] = into.offset + relocation.addend;
        }
    };
    ForEachRelocation(file_, at, visit);
}

} // namespace ntdlib
