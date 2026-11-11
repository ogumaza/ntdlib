// SPDX-License-Identifier: MIT

// JMap tables (BCSV), the tables of values that the Super Mario Galaxy games keep in RARC archives. A table starts with
// its count of rows, its count of fields, the offset of its first row and the size of a row. 12 bytes then describe
// each field: the hash of its name, a mask, its offset in a row, a shift and its type. Type 0 is a 32-bit integer, and
// type 6 the offset of a string in the strings after the rows. The fields are big-endian.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ntdlib::jaudio
{

// The hash of a field's name: each character, as a signed char, added to 31 times the hash so far.
uint32_t JMapHash(std::string_view name);

// A row's string and integer.
struct JMapNamedValue
{
    std::string name;
    uint32_t value = 0;
};

// The rows of the table named `file` in the RARC archive `archive`, compressed with Yaz0 or not: each row's string in
// the field `name_field` and its integer in the field `value_field`, masked and shifted, in the table's order. The
// table has the byte order of the archive: big-endian in an "RARC" archive and little-endian in a "CRAR" one. The
// table's name matches whatever its case. Throws FormatError if the archive has no such table, if the fields aren't a
// string and an integer, or if the table is damaged.
std::vector<JMapNamedValue> ReadJMapNamedValues(std::span<const uint8_t> archive, std::string_view file,
                                                std::string_view name_field, std::string_view value_field);

} // namespace ntdlib::jaudio
