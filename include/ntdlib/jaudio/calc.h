// SPDX-License-Identifier: MIT

// The random generators and the table of pan rules that JAudio1's and JAudio2's code share. They are inline, so code
// that reads them requires no link to ntdlib::jaudio.

#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace ntdlib::jaudio
{

// JMath::TRandom_fast_: the generator's next value after `value`, x × 0x19660d + 0x3c6ef35f. JAudio2's instruments,
// sequences and conductors each draw from a generator of this kind.
constexpr uint32_t NextFastRandom(uint32_t value)
{
    return value * 0x19660du + 0x3c6ef35fu;
}

// TRandom_fast_'s number from 0 to 1 for the generator's `value`: the float from 1 to 2 that the top 23 bits make, less
// 1.
inline float FastRandomUnit(uint32_t value)
{
    return std::bit_cast<float>(0x3f800000u | value >> 9) - 1.0f;
}

// JAudio1's generator with a state of two words (JASystem::Player::getRandomS32, 0x8027e250 in The Wind Waker's USA
// main.dol). The words start at 0x1000 and 0x5555 in every game that has it: The Wind Waker, Pikmin 2 (0x8009c6a4) and
// Pac-Man Vs. (0x8002a0a4).
class PlayerRandom
{
public:
    PlayerRandom() = default;

    // A generator whose words are `first` and `second`, as a model's saved state gives them.
    PlayerRandom(uint32_t first, uint32_t second) : first_(first), second_(second)
    {
    }

    // The next number: the second word times 0x98765432, plus the first word times 0x13579bde shifted right by 4 as a
    // signed number, plus 1. The second word then moves to the first, and the number becomes the second.
    uint32_t Next()
    {
        const auto shifted = static_cast<uint32_t>(static_cast<int32_t>(first_ * 0x13579bdeu) >> 4);
        const uint32_t next = second_ * 0x98765432u + shifted + 1u;
        first_ = second_;
        second_ = next;

        return next;
    }

    uint32_t First() const
    {
        return first_;
    }

    uint32_t Second() const
    {
        return second_;
    }

private:
    uint32_t first_ = 0x1000u;
    uint32_t second_ = 0x5555u;
};

// JASystem::Driver::calc_sw_table (0x80398c28 in The Wind Waker's main.dol): how each of cmdPanSwSet's 27 rules makes
// a note's pan or effect send from the bank's value, the instrument effects' value and the track's value. 0 leaves a
// value out, 1 adds it, and 2 weights it by its pan power. Pikmin 2's main.dol holds the same table (0x80478e18).
inline constexpr std::array<std::array<uint8_t, 3>, 27> kCalcRules{{
    {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 1}, {0, 1, 2}, {0, 1, 0}, {0, 2, 1}, {0, 2, 2},
    {1, 0, 0}, {1, 0, 1}, {1, 0, 2}, {1, 1, 0}, {1, 1, 1}, {1, 1, 2}, {1, 2, 0}, {1, 2, 1}, {1, 2, 2},
    {1, 0, 0}, {2, 0, 1}, {2, 0, 2}, {2, 1, 0}, {2, 1, 1}, {2, 1, 2}, {2, 2, 0}, {2, 2, 1}, {2, 2, 2},
}};

} // namespace ntdlib::jaudio
