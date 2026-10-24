// SPDX-License-Identifier: MIT

// The reader settings of the jaudio module's files, and the reads of their tags in either byte order.
//
// The GameCube's and the Wii's formats are big-endian. The Switch's releases of some of those games, such as Super
// Mario Galaxy's, keep the same files with each field stored little-endian in the width of its type. A tag of four
// characters is stored as a 32-bit word there. Its characters come in reverse order: "RARC" becomes "CRAR". A few tags
// keep their order.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "xsflib/binary.h"

namespace ntdlib::jaudio::detail
{

// A string's offset has to lie inside the data.
inline constexpr xsflib::ReaderOptions kReaderOptions{.check_string_offset = true};

// Whether `data` starts with `tag` in reverse order. A little-endian file starts that way.
inline bool StartsWithReversed(std::span<const uint8_t> data, const char* tag)
{
    return data.size() >= 4 && data[0] == static_cast<uint8_t>(tag[3]) && data[1] == static_cast<uint8_t>(tag[2]) &&
           data[2] == static_cast<uint8_t>(tag[1]) && data[3] == static_cast<uint8_t>(tag[0]);
}

// The byte order of a file whose first tag is `tag`: little-endian if the file starts with the tag in reverse order,
// and big-endian otherwise.
inline bool IsBigEndian(std::span<const uint8_t> data, const char* tag)
{
    return !StartsWithReversed(data, tag);
}

// The tag at `at` as a big-endian file holds it: the four bytes in order, or in reverse order in a little-endian file.
inline std::string TagAt(const xsflib::Reader& r, std::size_t at)
{
    const std::span<const uint8_t> bytes = r.Sub(at, 4);
    std::string tag(reinterpret_cast<const char*>(bytes.data()), 4);
    if (!r.BigEndian())
    {
        tag = {tag[3], tag[2], tag[1], tag[0]};
    }

    return tag;
}

// Whether the four bytes at `at` hold `tag`. A little-endian file can hold it in either order.
inline bool HasTag(const xsflib::Reader& r, std::size_t at, const char* tag)
{
    if (r.Magic(at, tag))
    {
        return true;
    }

    const char reversed[4] = {tag[3], tag[2], tag[1], tag[0]};
    return !r.BigEndian() && r.Magic(at, reversed);
}

} // namespace ntdlib::jaudio::detail
