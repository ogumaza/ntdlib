// SPDX-License-Identifier: MIT

// Definitions that the parts of the DS's sound unit share (sound.cpp and sound_enhanced.cpp): the registers, the
// channels' formats and delays, and the constants of the enhanced mixing mode. The two files build as one translation
// unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/main_memory.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/blep.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::ntr
{

namespace
{

constexpr int16_t kAdpcmMinimum = -0x7FFF;
constexpr uint8_t kAdpcmLastIndex = 88;
constexpr uint32_t kTimerOverflow = 0x10000;
constexpr int16_t kPsgHigh = 0x7FFF;
constexpr int16_t kPsgLow = -0x7FFF;
constexpr uint16_t kNoiseStart = 0x7FFF;
constexpr uint16_t kNoiseTaps = 0x6000;

// The channels' timer overflows before their first sample: the delays that GBATEK gives for a start.
constexpr int kPcmDelay = 3;
constexpr int kAdpcmDelay = 11;
constexpr int kPsgDelay = 1;

// A sound shorter than 4 words hangs: it stays busy and plays nothing.
constexpr uint32_t kMinimumWords = 4;

// The offsets of the registers past the channels', from kSoundRegisters. Capture 1's registers follow capture 0's: its
// SNDCAPxCNT 1 byte on, and its SNDCAPxDAD and SNDCAPxLEN 8 bytes on.
constexpr uint32_t kSoundControl = 0x100;
constexpr uint32_t kSoundBias = 0x104;
constexpr uint32_t kCaptureControl = 0x108;
constexpr uint32_t kCaptureAddress = 0x110;
constexpr uint32_t kCaptureLength = 0x114;
constexpr uint32_t kCaptureBytes = 8;
constexpr uint32_t kChannelBytes = 0x10;
constexpr uint32_t kChannels = 16;
constexpr uint32_t kCaptures = 2;
constexpr uint32_t kMostGrownChannels = snd::kMostChannels - kChannels;

// The enhanced mode's delay, and the samples of the zeros before a wave and of the samples kept behind its place.
constexpr std::size_t kDelay = wave::BandLimitedSteps::kHalfWidth;
constexpr int64_t kZeros = 2 * wave::SincKernel::kMaxHalfWidth;
constexpr int64_t kKept = wave::SincKernel::kMaxHalfWidth;
constexpr int64_t kMostKept = 8192;

// The mixer's sum, on the scale of a 16-bit sample times 256, times the master volume, on the output's scale of 1.
constexpr float kOutputScale = 1.0f / (1 << 30);

// SOUNDxCNT's fields.
constexpr uint32_t kBusy = 1u << 31;
constexpr uint32_t kHold = 1u << 15;

// SOUNDCNT's fields.
constexpr uint32_t kMasterEnable = 1u << 15;
constexpr uint32_t kChannel1NotToMixer = 1u << 12;
constexpr uint32_t kChannel3NotToMixer = 1u << 13;

// SNDCAPxCNT's fields.
constexpr uint8_t kCaptureBusy = 0x80;
constexpr uint8_t kCapturePcm8 = 0x08;
constexpr uint8_t kCaptureOneShot = 0x04;

// The limits of a capture's input: 16.8 bits.
constexpr int32_t kCaptureMinimum = -0x800000;
constexpr int32_t kCaptureMaximum = 0x7FFFFF;

// Which bits of each register's bytes a write sets, and which a read gives.
struct Masks
{
    std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters> write{};
    std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters> read{};
};

constexpr void SetMask(std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters>& masks, uint32_t offset, uint32_t mask,
                       int size)
{
    for (int i = 0; i < size; i++)
    {
        masks[offset + static_cast<uint32_t>(i)] = static_cast<uint8_t>(mask >> (8 * i));
    }
}

// The masks of GBATEK's registers. The reserved bits read as 0, and so do the write-only registers.
constexpr Masks MakeMasks()
{
    Masks masks;
    for (uint32_t channel = 0; channel < kChannels; channel++)
    {
        const uint32_t base = channel * kChannelBytes;
        SetMask(masks.write, base, 0xFF7F837F, 4); // SOUNDxCNT
        SetMask(masks.read, base, 0xFF7F837F, 4);
        SetMask(masks.write, base + 0x4, 0x07FFFFFC, 4); // SOUNDxSAD
        SetMask(masks.write, base + 0x8, 0xFFFF, 2);     // SOUNDxTMR
        SetMask(masks.write, base + 0xA, 0xFFFF, 2);     // SOUNDxPNT
        SetMask(masks.write, base + 0xC, 0x003FFFFF, 4); // SOUNDxLEN
    }
    for (const auto [offset, write, read, size] : {std::array<uint32_t, 4>{kSoundControl, 0xBF7F, 0xBF7F, 4},
                                                   {kSoundBias, 0x3FF, 0x3FF, 4},
                                                   {0x108, 0x8F, 0x8F, 1},             // SNDCAP0CNT
                                                   {0x109, 0x8F, 0x8F, 1},             // SNDCAP1CNT
                                                   {0x110, 0x07FFFFFC, 0x07FFFFFC, 4}, // SNDCAP0DAD
                                                   {0x114, 0xFFFF, 0, 4},              // SNDCAP0LEN
                                                   {0x118, 0x07FFFFFC, 0x07FFFFFC, 4}, // SNDCAP1DAD
                                                   {0x11C, 0xFFFF, 0, 4}})             // SNDCAP1LEN
    {
        SetMask(masks.write, offset, write, static_cast<int>(size));
        SetMask(masks.read, offset, read, static_cast<int>(size));
    }

    return masks;
}

constexpr Masks kMasks = MakeMasks();

// A volume or pan of 127 scales by 128/128.
int32_t Factor(uint32_t value)
{
    return value == 127 ? 128 : static_cast<int32_t>(value);
}

// The divider's shifts by SOUNDxCNT's bits 8 and 9: 1, 2, 4 and 16.
constexpr std::array<int, 4> kDivisorShifts = {0, 1, 2, 4};

ChannelFormat FormatOf(uint32_t control)
{
    return static_cast<ChannelFormat>((control >> 29) & 3);
}

uint32_t RepeatOf(uint32_t control)
{
    return (control >> 27) & 3;
}

// The samples that a wave's word holds.
uint32_t SamplesPerWord(ChannelFormat format)
{
    switch (format)
    {
    case ChannelFormat::kPcm8:
        return 4;
    case ChannelFormat::kPcm16:
        return 2;
    case ChannelFormat::kImaAdpcm:
        return 8;
    case ChannelFormat::kPsg:
        break;
    }

    return 1;
}

// A captured value from the mixer's 16.8 bits, with `shift` bits of fraction: 8 for PCM16, and 16 for PCM8, whose
// value is the 16.8 bits over 0x100. GBATEK: a fraction whose top bit is set rounds toward zero. Then the fraction
// goes. A negative value with that bit therefore rounds up, and every other value rounds down.
int32_t CapturedValue(int32_t value, int shift)
{
    const int32_t whole = value >> shift;
    return value < 0 && ((value >> (shift - 1)) & 1) != 0 ? whole + 1 : whole;
}

// The samples in `words` words of a wave, and the sample at the start of word `words`.
uint32_t SamplesIn(ChannelFormat format, uint32_t words)
{
    switch (format)
    {
    case ChannelFormat::kPcm8:
        return words * 4;
    case ChannelFormat::kPcm16:
        return words * 2;
    case ChannelFormat::kImaAdpcm:
        return words == 0 ? 0 : (words - 1) * 8; // the first word holds the decoder's state
    case ChannelFormat::kPsg:
        break;
    }

    return 0;
}

} // namespace

} // namespace ntdlib::ntr
