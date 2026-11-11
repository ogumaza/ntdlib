// SPDX-License-Identifier: MIT

// Definitions that the parts of jaudio1's tracks share (track.cpp, track_registers.cpp, track_notes.cpp and
// track_parser.cpp): the game's tables of points and settings of oscillators, the registers' numbers, the flags of the
// levels' updates and the timed levels. The four files build as one translation unit. The definitions therefore keep
// internal linkage. Only those files include this header.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/jaudio1/oscillator.h"
#include "ntdlib/jaudio1/track.h"

namespace ntdlib::jaudio1
{
namespace
{

// JASPlayer's tables of points, from main's data (0xb63be0 to 0xb63c4c): a simple ADSR, the release of the default
// envelope, a vibrato and a tremolo. Each point is a mode, a time and a value.
constexpr std::array<OscPoint, 4> kAdsTable = {{{0, 0, 0x7fff}, {0, 0, 0x7fff}, {0, 0, 0}, {kPointHold, 0, 0}}};
constexpr std::array<OscPoint, 2> kRelTable = {{{0, 10, 0}, {kPointStop, 1, 0}}};
constexpr std::array<OscPoint, 6> kVibTable = {
    {{0, 0, 0}, {0, 12, 0x7fff}, {0, 12, 0}, {0, 12, -0x4000}, {0, 12, 0}, {kPointJump, 0, 1}}};
constexpr std::array<OscPoint, 6> kTreTable = {
    {{0, 0, 0x7fff}, {0, 20, 0}, {0, 20, -0x7fff}, {0, 20, 0}, {0, 20, 0x7fff}, {kPointJump, 0, 1}}};

// JASBankMgr's release of a note on the DSP's oscillator (OSC_RELEASE_TABLE, 0x94ab1e).
constexpr std::array<OscPoint, 2> kOscReleaseTable = {{{1, 10, 0}, {kPointStop, 0, 0}}};

template <std::size_t N>
constexpr OscTable TableOf(const std::array<OscPoint, N>& points)
{
    return {points.data(), N};
}

// JASPlayer's settings of the oscillators (sEnvelopeDef, sVibratoDef, sTremoroDef and sAdsrDef), and JASBankMgr's for a
// note on the DSP's oscillator (OSC_ENV, 0xb62fc0).
constexpr OscData kEnvelopeDef{0, 1.0f, {}, TableOf(kRelTable), 1.0f, 0.0f};
constexpr OscData kVibratoDef{1, 0.5f, TableOf(kVibTable), TableOf(kVibTable), 0.0f, 1.0f};
constexpr OscData kTremoroDef{0, 0.5f, TableOf(kTreTable), TableOf(kTreTable), 0.0f, 1.0f};
constexpr OscData kAdsrDef{0, 1.0f, {}, {}, 1.0f, 0.0f};
constexpr OscData kOscEnv{0, 1.0f, {}, TableOf(kOscReleaseTable), 1.0f, 0.0f};

// The register numbers (Track::kRegisters).
constexpr uint32_t kRegResult = 3;
constexpr uint32_t kRegProduct = 4; // the high half of a product, then the low half in register 5
constexpr uint32_t kRegBankProgram = 6;
constexpr uint32_t kRegBendRange = 7;
constexpr uint32_t kRegPanPower = 8;
constexpr uint32_t kRegOuterWeight = 11;
constexpr uint32_t kRegParentWeight = 12;
constexpr uint32_t kRegPriority = 13;
constexpr uint32_t kRegLong = 16;
constexpr uint32_t kRegAfter = 24; // the first of the words after JASRegisterParam

// The flags of the levels' updates (JASOuterParamFlag).
constexpr uint32_t kUpdateVolume = OuterParam::kVolume;
constexpr uint32_t kUpdatePitch = OuterParam::kPitch;
constexpr uint32_t kUpdateFxmix = OuterParam::kFxmix;
constexpr uint32_t kUpdatePan = OuterParam::kPan;
constexpr uint32_t kUpdateDolby = OuterParam::kDolby;
constexpr uint32_t kUpdateDistortion = 0x20;
constexpr uint32_t kUpdateTempo = OuterParam::kTempo;
constexpr uint32_t kUpdateFir = OuterParam::kFir;
constexpr uint32_t kUpdateIir = 0xf000;
constexpr uint32_t kUpdateDelay = 0x20000;

// The timed levels by index.
constexpr std::size_t kTimedVolume = 0;
constexpr std::size_t kTimedPitch = 1;
constexpr std::size_t kTimedFxmix = 2;
constexpr std::size_t kTimedPan = 3;
constexpr std::size_t kTimedDolby = 4;
constexpr std::size_t kTimedDistortion = 5;
constexpr std::size_t kTimedIir = 12;
constexpr std::size_t kTimedPause = 16;
constexpr std::size_t kTimedDelay = 17;

// 32767 and 1/32768 in single precision.
constexpr float kShortMax = 32767.0f;
constexpr float kShortScale = 1.0f / 32768.0f;

} // namespace
} // namespace ntdlib::jaudio1
