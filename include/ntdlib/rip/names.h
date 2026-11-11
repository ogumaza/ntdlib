// SPDX-License-Identifier: MIT

// The names that the rippers' messages give the items of a sound archive.

#pragma once

#include <cstddef>
#include <string>

namespace ntdlib::rip
{

// An item's name in a message: its name, or "#" and its number if it has none.
inline std::string NameOrNumber(const std::string& name, std::size_t index)
{
    return name.empty() ? "#" + std::to_string(index) : name;
}

} // namespace ntdlib::rip
