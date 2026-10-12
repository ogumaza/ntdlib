// SPDX-License-Identifier: MIT

// Synthetic inputs for the wave decoders' tests, from a fixed generator. wave_test.cpp checks the decoders' output on
// them against fixed hashes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::wave::synthetic
{

// SplitMix64. It gives the same numbers on every platform.
class Random
{
public:
    explicit Random(uint64_t seed) : state_(seed)
    {
    }

    uint64_t Next()
    {
        state_ += 0x9e3779b97f4a7c15;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;

        return z ^ (z >> 31);
    }

    // A number from 0 to `n` - 1.
    uint32_t Below(uint32_t n)
    {
        return static_cast<uint32_t>(Next() % n);
    }

    // A coefficient or a sample of one of three kinds: 0 is moderate, 1 is anywhere in 16 bits, and 2 is one of the
    // extremes that make a DSP-ADPCM sum pass 32 bits.
    int16_t Value(uint32_t kind)
    {
        constexpr int16_t kExtremes[6] = {-32768, -32767, -2048, 0, 2048, 32767};
        if (kind == 0)
        {
            return static_cast<int16_t>(static_cast<int32_t>(Below(8192)) - 4096);
        }
        if (kind == 1)
        {
            return static_cast<int16_t>(static_cast<uint16_t>(Next()));
        }

        return kExtremes[Below(6)];
    }

private:
    uint64_t state_;
};

// The input of a block decoder.
struct BlockCase
{
    std::vector<uint8_t> data;
    uint32_t samples = 0;
    std::array<int16_t, 16> coefs{};
    int16_t hist1 = 0; // IMA-ADPCM's first sample
    int16_t hist2 = 0;
    uint8_t index = 0; // IMA-ADPCM's step index
};

// Inputs for the block decoders. The data ends inside frames and between them, and the sample count can exceed the
// samples that the data holds. Coefficients and history range from moderate values to the extremes.
inline std::vector<BlockCase> BlockCases(uint64_t seed, std::size_t count)
{
    Random random(seed);
    std::vector<BlockCase> cases(count);
    for (BlockCase& c : cases)
    {
        c.data.resize(random.Below(130));
        for (uint8_t& byte : c.data)
        {
            byte = static_cast<uint8_t>(random.Next());
        }

        c.samples = random.Below(8) == 0 ? 0 : random.Below(260);
        const uint32_t kind = random.Below(3);
        for (int16_t& coef : c.coefs)
        {
            coef = random.Value(kind);
        }
        c.hist1 = random.Value(kind);
        c.hist2 = random.Value(kind);
        c.index = static_cast<uint8_t>(random.Next());
    }

    return cases;
}

// The input of DspAdpcmSample.
struct StepCase
{
    int32_t nibble = 0;
    int32_t shift = 0;
    int32_t c1 = 0;
    int32_t c2 = 0;
    int32_t hist1 = 0;
    int32_t hist2 = 0;
};

// Inputs for DspAdpcmSample: every nibble and shift, with coefficients and history of the three kinds.
inline std::vector<StepCase> StepCases(uint64_t seed, std::size_t count)
{
    Random random(seed);
    std::vector<StepCase> cases(count);
    for (StepCase& c : cases)
    {
        const uint32_t kind = random.Below(3);
        c.nibble = static_cast<int32_t>(random.Below(16)) - 8;
        c.shift = static_cast<int32_t>(random.Below(16));
        c.c1 = random.Value(kind);
        c.c2 = random.Value(kind);
        c.hist1 = random.Value(kind);
        c.hist2 = random.Value(kind);
    }

    return cases;
}

// FNV-1a over 64 bits: a fingerprint of many decoders' outputs.
class Hash
{
public:
    void Add(uint64_t value, int bytes)
    {
        for (int i = 0; i < bytes; i++)
        {
            value_ = (value_ ^ ((value >> (8 * i)) & 0xff)) * 0x100000001b3;
        }
    }

    // Adds a decoder's output: its length, then each sample.
    void Add(const std::vector<int16_t>& samples)
    {
        Add(samples.size(), 4);
        for (const int16_t s : samples)
        {
            Add(static_cast<uint16_t>(s), 2);
        }
    }

    uint64_t Value() const
    {
        return value_;
    }

private:
    uint64_t value_ = 0xcbf29ce484222325;
};

} // namespace ntdlib::wave::synthetic
