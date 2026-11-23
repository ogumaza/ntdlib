// SPDX-License-Identifier: MIT

// The 64-bit FNV-1a hash, a byte at a time. The tests fingerprint long outputs with it and compare the result with a
// recorded value.

#pragma once

#include <cstdint>
#include <string_view>

namespace ntdlib::testing
{

class Fnv1a
{
public:
    // Adds the low `bytes` bytes of `value`, the low byte first.
    void Add(uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; i++)
        {
            value_ = (value_ ^ ((value >> (8 * i)) & 0xff)) * kPrime;
        }
    }

    // Adds each character of `text`.
    void Add(std::string_view text)
    {
        for (const char c : text)
        {
            Add(static_cast<uint8_t>(c), 1);
        }
    }

    uint64_t Value() const
    {
        return value_;
    }

private:
    static constexpr uint64_t kOffsetBasis = 0xcbf29ce484222325;
    static constexpr uint64_t kPrime = 0x100000001b3;

    uint64_t value_ = kOffsetBasis;
};

} // namespace ntdlib::testing
