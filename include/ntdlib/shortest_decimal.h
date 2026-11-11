// SPDX-License-Identifier: MIT

// The text of a float for a tag or a message: the shortest decimal that reads back as the same float.

#pragma once

#include <charconv>
#include <string>
#include <system_error>

namespace ntdlib
{

// The shortest decimal that reads back as the same float, in every locale: 1.3f gives "1.3". The text has no exponent.
// It's empty when it doesn't fit in 48 characters.
inline std::string ShortestDecimal(float value)
{
    char text[48];
    const auto [end, error] = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed);

    return error == std::errc() ? std::string(text, end) : std::string();
}

} // namespace ntdlib
