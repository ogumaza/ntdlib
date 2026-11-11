// SPDX-License-Identifier: MIT

// Two roundings that several modules share: a size or an offset rounded up to a multiple of an alignment, and a float
// rounded to the nearest 32-bit integer.

#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace ntdlib
{

// `value` rounded up to a multiple of `alignment`, in the type of `value`. A negative value rounds up toward 0.
template <std::integral T>
constexpr T AlignUp(T value, std::type_identity_t<T> alignment)
{
    if constexpr (std::is_signed_v<T>)
    {
        if (value < 0)
        {
            return -(-value / alignment * alignment);
        }
    }

    return (value + alignment - 1) / alignment * alignment;
}

// A float rounded to the nearest integer, halves away from 0, and held to 32 bits. The enhanced modes of the nx, ctr
// and cafe renderers use it for their 32-bit samples.
inline int32_t RoundToInt32(float x)
{
    return static_cast<int32_t>(std::clamp(std::round(static_cast<double>(x)), -2147483648.0, 2147483647.0));
}

} // namespace ntdlib
