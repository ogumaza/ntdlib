// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/jmap.h"

#include <algorithm>
#include <cstddef>

#include "xsflib/ascii.h"
#include "xsflib/binary.h"

#include "jaudio/reader.h"
#include "ntdlib/jaudio/rarc.h"

namespace ntdlib::jaudio
{
namespace
{

// A field of a table: a mask, its offset in a row, a shift and its type.
struct JMapField
{
    uint32_t mask = 0;
    uint16_t offset = 0;
    uint8_t shift = 0;
    uint8_t type = 0;
};

// The field of table `r` named `name`. Throws FormatError if the table has none.
JMapField FindJMapField(const xsflib::Reader& r, std::string_view name)
{
    const uint32_t fields = r.U32(4);
    for (uint32_t i = 0; i < fields; i++)
    {
        const std::size_t at = 16 + 12 * std::size_t{i};
        if (r.U32(at) == JMapHash(name))
        {
            return {.mask = r.U32(at + 4), .offset = r.U16(at + 8), .shift = r.U8(at + 10), .type = r.U8(at + 11)};
        }
    }

    throw xsflib::FormatError("the table has no field " + std::string(name));
}

} // namespace

uint32_t JMapHash(std::string_view name)
{
    uint32_t hash = 0;
    for (const char c : name)
    {
        hash = hash * 31 + static_cast<uint32_t>(static_cast<signed char>(c));
    }

    return hash;
}

std::vector<JMapNamedValue> ReadJMapNamedValues(std::span<const uint8_t> archive, std::string_view file,
                                                std::string_view name_field, std::string_view value_field)
{
    constexpr uint8_t kInteger = 0;
    constexpr uint8_t kString = 6;

    std::vector<uint8_t> decoded;
    const std::span<const uint8_t> rarc = DecodeIfYaz0(archive, decoded);
    const std::vector<ArchiveFile> files = ReadRarc(rarc);
    const auto table =
        std::ranges::find_if(files, [&](const ArchiveFile& f)
                             { return xsflib::AsciiLower(f.name) == xsflib::AsciiLower(std::string(file)); });
    if (table == files.end())
    {
        throw xsflib::FormatError("the archive holds no " + std::string(file));
    }

    // The table has the byte order of its archive. A Switch release stores the archive little-endian ("CRAR").
    const xsflib::Reader r(table->data, detail::IsBigEndian(rarc, "RARC"), detail::kReaderOptions);
    const uint32_t rows = r.U32(0);
    const std::size_t first = r.U32(8);
    const std::size_t size = r.U32(12);
    const JMapField name = FindJMapField(r, name_field);
    const JMapField value = FindJMapField(r, value_field);
    if (name.type != kString || value.type != kInteger || value.shift >= 32)
    {
        throw xsflib::FormatError("the table's fields aren't a name and an integer");
    }

    std::vector<JMapNamedValue> values;
    const std::size_t strings = first + rows * size;
    for (uint32_t i = 0; i < rows; i++)
    {
        const std::size_t row = first + i * size;
        values.push_back({r.CString(strings + r.U32(row + name.offset), 0x100),
                          (r.U32(row + value.offset) & value.mask) >> value.shift});
    }

    return values;
}

} // namespace ntdlib::jaudio
