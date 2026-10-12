// SPDX-License-Identifier: MIT

// What the Wii's and the Wii U's AX libraries share: the voices and their priorities, the sample formats, the voices'
// states and types, the sample rate converters, and the bits of a voice's sync word. The sync word records what the
// game has changed in the voice's parameter block since the last frame. The Wii U's AX keeps the Wii's bits. Where the
// two consoles' code names a bit otherwise, both names are given, with the Wii's name first.

#pragma once

#include <cstdint>

namespace ntdlib::ax
{

// AX has 96 voices and 32 priorities: 0 marks a free voice, and 31 is the highest.
constexpr int kVoiceCount = 96;
constexpr uint32_t kPriorityFree = 0;
constexpr uint32_t kPriorityMin = 1;
constexpr uint32_t kPriorityMax = 31;

enum class SampleFormat : uint16_t
{
    kAdpcm = 0x00,
    kPcm16 = 0x0a,
    kPcm8 = 0x19,
};

enum class VoiceState : uint16_t
{
    kStop = 0,
    kRun = 1,
};

// A streaming voice's buffer is refilled as it plays. Its ADPCM history isn't reloaded at its loop start.
enum class VoiceType : uint16_t
{
    kNormal = 0,
    kStream = 1,
};

// How a voice converts its sample rate: the 4-tap filters (8, 12 and 16 kHz), linear interpolation, or none.
enum class SrcMode : uint16_t
{
    kFourTap = 0,
    kLinear = 1,
    kNone = 2,
};

namespace sync
{

constexpr uint32_t kSelect = 1u << 0, kSrcType = kSelect;
constexpr uint32_t kMixerCtrl = 1u << 1, kMixMasks = kMixerCtrl;
constexpr uint32_t kState = 1u << 2;
constexpr uint32_t kType = 1u << 3;
constexpr uint32_t kMix = 1u << 4;
constexpr uint32_t kItd = 1u << 5;
constexpr uint32_t kItdShift = 1u << 6, kItdTarget = kItdShift;
constexpr uint32_t kDpop = 1u << 7;
constexpr uint32_t kVe = 1u << 8;
constexpr uint32_t kVeDelta = 1u << 9;
constexpr uint32_t kAddr = 1u << 10, kAddresses = kAddr;
constexpr uint32_t kLoopFlag = 1u << 11;
constexpr uint32_t kLoopAddr = 1u << 12, kLoopAddress = kLoopAddr;
constexpr uint32_t kEndAddr = 1u << 13, kEndAddress = kEndAddr;
constexpr uint32_t kCurrAddr = 1u << 14, kCurrentAddress = kCurrAddr;
constexpr uint32_t kAdpcm = 1u << 15;
constexpr uint32_t kSrc = 1u << 16;
constexpr uint32_t kSrcRatio = 1u << 17;
constexpr uint32_t kAdpcmLoop = 1u << 18;
constexpr uint32_t kLpf = 1u << 19;
constexpr uint32_t kLpfCoefs = 1u << 20;
constexpr uint32_t kBiquad = 1u << 21;
constexpr uint32_t kBiquadCoefs = 1u << 22;
constexpr uint32_t kRemote = 1u << 23, kRmtOn = kRemote;
constexpr uint32_t kRmtMixerCtrl = 1u << 24;
constexpr uint32_t kRmtMix = 1u << 25;
constexpr uint32_t kRmtDpop = 1u << 26;
constexpr uint32_t kRmtSrc = 1u << 27;
constexpr uint32_t kRmtIir = 1u << 28;
constexpr uint32_t kRmtIirLpfCoefs = 1u << 29;
constexpr uint32_t kRmtIirBiquadCoefs = 1u << 30;
constexpr uint32_t kAll = 1u << 31;

} // namespace sync
} // namespace ntdlib::ax
