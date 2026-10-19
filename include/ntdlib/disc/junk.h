// SPDX-License-Identifier: MIT

// The pseudorandom data that pads GameCube and Wii discs: a lagged Fibonacci generator (f = xor, j = 32, k = 521)
// seeded with 17 words. RVZ stores runs of this data as their seeds. Dolphin's docs/WiaAndRvz.md describes the
// algorithm.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ntdlib::disc
{

class JunkGenerator
{
public:
    // The seed is 68 bytes: 17 big-endian words.
    explicit JunkGenerator(const uint8_t* seed);

    // Skips `size` bytes of output.
    void Skip(size_t size);

    // Writes the next `size` bytes of output to `out`.
    void Generate(uint8_t* out, size_t size);

private:
    // The words in the buffer (k), the lag (j), the words of the seed, and the bytes of output that the buffer holds.
    static constexpr size_t kWords = 521;
    static constexpr size_t kLag = 32;
    static constexpr size_t kSeedWords = 17;
    static constexpr size_t kBufferBytes = kWords * 4;

    // Advances the whole buffer by one generation.
    void Advance();

    std::array<uint32_t, kWords> buffer_;
    size_t position_ = 0; // the next byte to output, within the kBufferBytes bytes of the buffer
};

} // namespace ntdlib::disc
