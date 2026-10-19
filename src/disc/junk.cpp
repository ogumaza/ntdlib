// SPDX-License-Identifier: MIT

#include "ntdlib/disc/junk.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "xsflib/bytes.h"

namespace ntdlib::disc
{

JunkGenerator::JunkGenerator(const uint8_t* seed)
{
    for (size_t i = 0; i < kSeedWords; ++i)
    {
        buffer_[i] = xsflib::LoadBe32(seed + i * 4);
    }

    for (size_t i = kSeedWords; i < kWords; ++i)
    {
        buffer_[i] = (buffer_[i - 17] << 23) ^ (buffer_[i - 16] >> 9) ^ buffer_[i - 1];
    }

    // The output starts after four generations.
    for (int i = 0; i < 4; ++i)
    {
        Advance();
    }
}

void JunkGenerator::Skip(size_t size)
{
    size += position_;
    for (; size >= kBufferBytes; size -= kBufferBytes)
    {
        Advance();
    }

    position_ = size;
}

void JunkGenerator::Generate(uint8_t* out, size_t size)
{
    while (size)
    {
        if (position_ == kBufferBytes)
        {
            Advance();
            position_ = 0;
        }

        // Each word gives its bytes from shifts of 24, 18 (not 16), 8 and 0.
        const uint32_t word = buffer_[position_ / 4];
        const uint8_t bytes[4] = {static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 18),
                                  static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
        const size_t n = std::min(size, 4 - position_ % 4);
        std::copy_n(bytes + position_ % 4, n, out);
        out += n;
        size -= n;
        position_ += n;
    }
}

void JunkGenerator::Advance()
{
    for (size_t i = 0; i < kLag; ++i)
    {
        buffer_[i] ^= buffer_[i + kWords - kLag];
    }

    for (size_t i = kLag; i < kWords; ++i)
    {
        buffer_[i] ^= buffer_[i - kLag];
    }
}

} // namespace ntdlib::disc
