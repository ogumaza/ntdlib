// SPDX-License-Identifier: MIT

// The test of a magic word at the start of a file or a chunk, such as DSE's smdl, a CPK table's @UTF or Universomo's
// UNISAMBA.

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace ntdlib::detail
{

// Whether `bytes` start with the characters of `magic`.
inline bool HasMagic(std::span<const uint8_t> bytes, std::string_view magic)
{
    return bytes.size() >= magic.size() && std::memcmp(bytes.data(), magic.data(), magic.size()) == 0;
}

} // namespace ntdlib::detail
