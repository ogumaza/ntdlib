// SPDX-License-Identifier: MIT

#include "ntdlib/bezel/sheet.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::bezel
{
namespace
{

constexpr uint32_t kSymbolTable = 2;  // SHT_SYMTAB
constexpr uint32_t kRelocations = 4;  // SHT_RELA
constexpr uint32_t kNoBits = 8;       // SHT_NOBITS: a section with no bytes in the file
constexpr uint32_t kAbsolute64 = 257; // R_AARCH64_ABS64
constexpr uint16_t kAarch64 = 183;    // EM_AARCH64

bool Fits(std::size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

} // namespace

std::optional<std::string> Sheet::Parse(std::vector<uint8_t> data, Sheet& out)
{
    out = Sheet{};
    if (data.size() < 0x40 || data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' || data[4] != 2 ||
        data[5] != 1 || xsflib::LoadLe16(&data[0x12]) != kAarch64)
    {
        return std::string("not a little-endian 64-bit AArch64 ELF object");
    }

    std::vector<Header> headers;
    if (std::optional<std::string> error = out.ReadSections(data, headers))
    {
        return error;
    }

    out.ReadPointers(data, headers, out.ReadSymbols(data, headers));
    out.data_ = std::move(data);

    return std::nullopt;
}

std::optional<std::string> Sheet::ReadSections(const std::vector<uint8_t>& data, std::vector<Header>& headers)
{
    const std::size_t size = data.size();
    const uint64_t table = xsflib::LoadLe64(&data[0x28]);
    const uint16_t entry = xsflib::LoadLe16(&data[0x3A]);
    const uint16_t count = xsflib::LoadLe16(&data[0x3C]);
    if (entry < 0x40 || !Fits(size, table, uint64_t{entry} * count))
    {
        return std::string("its section headers run past its end");
    }

    for (uint16_t i = 0; i < count; ++i)
    {
        const uint8_t* h = &data[table + uint64_t{entry} * i];
        const Header header{xsflib::LoadLe32(h + 4), xsflib::LoadLe64(h + 0x18), xsflib::LoadLe64(h + 0x20),
                            xsflib::LoadLe32(h + 0x28), xsflib::LoadLe32(h + 0x2C)};
        const bool has_bytes = header.type != kNoBits;
        if (has_bytes && !Fits(size, header.offset, header.size))
        {
            return "section " + std::to_string(i) + " runs past its end";
        }

        sections_.push_back({header.offset, has_bytes ? header.size : 0});
        headers.push_back(header);
    }

    return std::nullopt;
}

std::vector<Sheet::Target> Sheet::ReadSymbols(const std::vector<uint8_t>& data, const std::vector<Header>& headers)
{
    std::vector<Target> places;
    for (const Header& h : headers)
    {
        if (h.type != kSymbolTable || h.link >= headers.size())
        {
            continue;
        }

        const Header& strings = headers[h.link];
        for (uint64_t at = h.offset; at + 24 <= h.offset + h.size; at += 24)
        {
            const uint32_t name = xsflib::LoadLe32(&data[at]);
            const uint16_t section = xsflib::LoadLe16(&data[at + 6]);
            const uint64_t value = xsflib::LoadLe64(&data[at + 8]);
            places.push_back({section, value});
            if (name == 0 || name >= strings.size)
            {
                continue;
            }

            const auto* text = reinterpret_cast<const char*>(&data[strings.offset + name]);
            const std::size_t length = std::string(text, static_cast<std::size_t>(strings.size - name)).find('\0');
            if (length != std::string::npos)
            {
                symbols_[std::string(text, length)] = {section, value, xsflib::LoadLe64(&data[at + 16])};
            }
        }
    }

    return places;
}

void Sheet::ReadPointers(const std::vector<uint8_t>& data, const std::vector<Header>& headers,
                         const std::vector<Target>& symbols)
{
    for (const Header& h : headers)
    {
        if (h.type != kRelocations || h.info >= headers.size())
        {
            continue;
        }

        for (uint64_t at = h.offset; at + 24 <= h.offset + h.size; at += 24)
        {
            const uint64_t info = xsflib::LoadLe64(&data[at + 8]);
            const uint64_t symbol = info >> 32;
            if ((info & 0xFFFFFFFF) != kAbsolute64 || symbol >= symbols.size())
            {
                continue;
            }

            const Target& place = symbols[symbol];
            const auto addend = static_cast<uint64_t>(static_cast<int64_t>(xsflib::LoadLe64(&data[at + 16])));
            pointers_[{static_cast<uint16_t>(h.info), xsflib::LoadLe64(&data[at])}] = {place.section,
                                                                                       place.offset + addend};
        }
    }
}

std::span<const uint8_t> Sheet::Symbol(const std::string& name) const
{
    const auto found = symbols_.find(name);
    if (found == symbols_.end() || found->second.section >= sections_.size())
    {
        return {};
    }

    const Section& section = sections_[found->second.section];
    if (!Fits(static_cast<std::size_t>(section.size), found->second.value, found->second.size))
    {
        return {};
    }

    return std::span<const uint8_t>(data_).subspan(static_cast<std::size_t>(section.offset + found->second.value),
                                                   static_cast<std::size_t>(found->second.size));
}

std::optional<std::string> Sheet::StringAt(const std::string& name, std::size_t offset) const
{
    const auto symbol = symbols_.find(name);
    if (symbol == symbols_.end())
    {
        return std::nullopt;
    }

    const auto pointer = pointers_.find({symbol->second.section, symbol->second.value + offset});
    if (pointer == pointers_.end() || pointer->second.section >= sections_.size())
    {
        return std::nullopt;
    }

    const Section& section = sections_[pointer->second.section];
    if (pointer->second.offset >= section.size)
    {
        return std::nullopt;
    }

    const auto* text =
        reinterpret_cast<const char*>(&data_[static_cast<std::size_t>(section.offset + pointer->second.offset)]);
    const std::string_view rest(text, static_cast<std::size_t>(section.size - pointer->second.offset));
    return std::string(rest.substr(0, rest.find('\0')));
}

std::vector<std::string> Sheet::SymbolsStartingWith(std::string_view prefix) const
{
    std::vector<std::string> names;
    for (auto it = symbols_.lower_bound(std::string(prefix)); it != symbols_.end() && it->first.starts_with(prefix);
         ++it)
    {
        names.push_back(it->first);
    }

    return names;
}

} // namespace ntdlib::bezel
