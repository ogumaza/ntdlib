// SPDX-License-Identifier: MIT

// The constants and the vector arithmetic that the source files of the audio microcodes' model share. audio_rsp.cpp
// contains the standard microcode's commands. The other microcodes reuse many of them. audio_rsp_ead.cpp contains the
// commands of EAD's microcodes, and audio_rsp_compact.cpp the compact microcode's commands.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/n64/audio_rsp.h"

namespace ntdlib::n64::audio_rsp_common
{

// The 8 lanes of a vector register.
using Vector = std::array<int16_t, 8>;

inline constexpr uint32_t kDmemMask = 0xFFF;

// The offsets of each microcode's fields in DMEM from the start of those fields.
inline constexpr uint32_t kIn = 0;
inline constexpr uint32_t kOut = 2;
inline constexpr uint32_t kCount = 4;
inline constexpr uint32_t kVolumeLeft = 6;
inline constexpr uint32_t kVolumeRight = 8;
inline constexpr uint32_t kDryRight = 10;
inline constexpr uint32_t kWetLeft = 12;
inline constexpr uint32_t kWetRight = 14;
inline constexpr uint32_t kRamps = 16; // shares its first word with the loop's address
inline constexpr uint32_t kAuxMix = 28;

inline constexpr uint8_t kFlagInit = 0x01;
inline constexpr uint8_t kFlagLoop = 0x02;
inline constexpr uint8_t kFlagLeft = 0x02;
inline constexpr uint8_t kFlagVolume = 0x04;
inline constexpr uint8_t kFlagAux = 0x08;
inline constexpr uint8_t kFlagSmall = 0x04;

// A ramp's share of the change from one group of 8 samples to the next, at each of the group's samples.
inline constexpr std::array<uint16_t, 8> kRampSteps = {0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000, 0xFFFF};

constexpr int16_t Clamp16(int64_t value)
{
    return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
}

// The accumulator's 48 bits, sign-extended.
constexpr int64_t Wrap48(int64_t value)
{
    return static_cast<int64_t>(static_cast<uint64_t>(value) << 16) >> 16;
}

// The low 16 bits of the accumulator, when its upper bits fit in 16 bits. Otherwise 0 or 0xFFFF.
constexpr uint16_t SaturateLow(int64_t accumulator)
{
    const int64_t upper = accumulator >> 16;
    if (upper < -32768)
    {
        return 0;
    }

    if (upper > 32767)
    {
        return 0xFFFF;
    }

    return static_cast<uint16_t>(accumulator);
}

// A multiply of two fractions, rounded.
constexpr int16_t MultiplyFraction(int16_t a, int16_t b)
{
    return Clamp16((int64_t{a} * b * 2 + 0x8000) >> 16);
}

// A sample times an unsigned 16-bit gain: the product's upper half.
constexpr int16_t MultiplyHigh(int16_t sample, uint16_t gain)
{
    return static_cast<int16_t>((int64_t{sample} * gain) >> 16);
}

// A sample with its gain, added to a buffer's sample that keeps nearly all its level.
constexpr int16_t MixSample(int16_t out, int16_t in, int16_t gain)
{
    return Clamp16((int64_t{out} * 0x7FFF * 2 + 0x8000 + int64_t{in} * gain * 2) >> 16);
}

// The bytes that a command runs on. The command runs on `group` bytes at a time and always runs once.
constexpr uint32_t RoundedUp(int32_t bytes, uint32_t group)
{
    return bytes <= 0 ? group : ((static_cast<uint32_t>(bytes) + group - 1) / group) * group;
}

// A DMA's record. A DMA runs on 8-byte multiples of both addresses.
inline void RecordDma(AudioOps* ops, AudioOpKind kind, uint32_t dmem, uint32_t address, uint32_t bytes)
{
    if (ops != nullptr)
    {
        AudioOp op;
        op.kind = kind;
        op.in = dmem & kDmemMask & ~uint32_t{7};
        op.address = address & ~uint32_t{7};
        op.bytes = std::min<uint32_t>(((bytes - 1) | 7) + 1, AudioRsp::kDmemSize);
        ops->ops.push_back(op);
    }
}

// One step of a ramp that grows in a straight line, in each of the 8 lanes: the rate added to the value, with the
// fractions' carry.
inline void LinearStepLanes(Vector& whole, Vector& fraction, int16_t rate, uint16_t rate_fraction)
{
    for (std::size_t i = 0; i < 8; i++)
    {
        const uint32_t sum = uint32_t{static_cast<uint16_t>(fraction[i])} + rate_fraction;
        fraction[i] = static_cast<int16_t>(sum);
        whole[i] = Clamp16(int32_t{whole[i]} + rate + static_cast<int32_t>(sum >> 16));
    }
}

// Keeps the values of a ramp that grows in a straight line at its target, in each of the 8 lanes.
inline void LinearLimitLanes(Vector& values, int16_t target, int16_t rate)
{
    for (int16_t& value : values)
    {
        value = rate >= 0 ? std::min(value, target) : std::max(value, target);
    }
}

// The values of a ramp that grows in a straight line, in the 8 lanes of the first group, from `volume`.
inline void LinearFirstRamp(int16_t volume, int16_t rate, uint16_t rate_fraction, Vector& whole, Vector& fraction)
{
    for (std::size_t i = 0; i < 8; i++)
    {
        int64_t accumulator = (int64_t{kRampSteps[i]} * rate_fraction) >> 16;
        accumulator = Wrap48(accumulator + int64_t{kRampSteps[i]} * rate);
        accumulator = Wrap48(accumulator + int64_t{volume} * 65536);
        whole[i] = Clamp16(accumulator >> 16);
        fraction[i] = static_cast<int16_t>(SaturateLow(accumulator));
    }
}

} // namespace ntdlib::n64::audio_rsp_common
