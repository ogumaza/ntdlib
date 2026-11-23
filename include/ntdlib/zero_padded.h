// SPDX-License-Identifier: MIT

// A number with leading zeros, as a game's sprintf writes it with a format such as "%03d" in a file name.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ntdlib
{

// The decimal digits of `number` with leading zeros up to `digits` digits. A number with more digits keeps them all.
inline std::string ZeroPadded(uint32_t number, std::size_t digits)
{
    std::string text = std::to_string(number);
    text.insert(0, digits > text.size() ? digits - text.size() : 0, '0');

    return text;
}

} // namespace ntdlib
