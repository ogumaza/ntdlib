// SPDX-License-Identifier: MIT

// Definitions that the parts of jac's tracks share (track.cpp, track_registers.cpp, track_notes.cpp and
// track_parser.cpp): the game's tables of points and settings of oscillators, the registers' numbers, the flags of
// the levels' updates and the timed levels. The four files build as one translation unit. The definitions therefore
// keep internal linkage. Only those files include this header.

#pragma once

#include <cstddef>
#include <cstdint>

#include "ntdlib/jac/track.h"
#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jac
{
namespace
{

// The commands by their place in the table (CMDP_LIST, 0x6afb68 in main's data): 0xc0 plus the place.
enum CommandIndex : uint8_t
{
    kOpenTrack = 1,
    kOpenTrackBros = 2,
    kCall = 3,
    kCallF = 4,
    kRet = 5,
    kRetF = 6,
    kJmp = 7,
    kJmpF = 8,
    kLoopS = 9,
    kLoopE = 10,
    kReadPort = 11,
    kWritePort = 12,
    kCheckPortImport = 13,
    kCheckPortExport = 14,
    kWaitReg = 15,
    kConnectName = 16,
    kParentWritePort = 17,
    kChildWritePort = 18,
    kSetLastNote = 20,
    kTimeRelate = 21,
    kSimpleOsc = 22,
    kSimpleEnv = 23,
    kSimpleAdsr = 24,
    kTranspose = 25,
    kCloseTrack = 26,
    kOutSwitch = 27,
    kUpdateSync = 28,
    kBusConnect = 29,
    kPauseStatus = 30,
    kSetInterrupt = 31,
    kDisInterrupt = 32,
    kClrI = 33,
    kSetI = 34,
    kRetI = 35,
    kIntTimer = 36,
    kConnectOpen = 37,
    kConnectClose = 38,
    kSyncCpu = 39,
    kFlushAll = 40,
    kFlushRelease = 41,
    kWait3 = 42,
    kPanPowSet = 43,
    kIirSet = 44,
    kFirSet = 45,
    kExtSet = 46,
    kPanSwSet = 47,
    kOscRoute = 48,
    kIirCutOff = 49,
    kOscFull = 50,
    kCheckWave = 58,
    kPrintf = 59,
    kNop = 60,
    kTempo = 61,
    kTimeBase = 62,
    kFinish = 63,
};

// The registers by number.
constexpr uint32_t kRegValue = 3;
constexpr uint32_t kRegX = 4;
constexpr uint32_t kRegY = 5;
constexpr uint32_t kRegBank = 6;
constexpr uint32_t kRegBendRange = 7;
constexpr uint32_t kRegArguments = 8; // the five pan weights (arguments), 8 to 12
constexpr uint32_t kRegPriority = 13;
constexpr uint32_t kRegLong = 16;

// The update flags: the levels of the timed parameters by their index, and the outer parameters' levels.
constexpr uint32_t kUpdateVolume = 0x1;
constexpr uint32_t kUpdatePitch = 0x2;
constexpr uint32_t kUpdateFxmix = 0x4;
constexpr uint32_t kUpdatePan = 0x8;
constexpr uint32_t kUpdateDolby = 0x10;
constexpr uint32_t kUpdateDistortion = 0x20;
constexpr uint32_t kUpdateTempo = 0x40;
constexpr uint32_t kUpdateFir = 0x80;
constexpr uint32_t kUpdateIir = 0xf000;
constexpr uint32_t kUpdateDelay = 0x20000;

// The timed parameters by index.
constexpr std::size_t kTimedVolume = 0;
constexpr std::size_t kTimedPitch = 1;
constexpr std::size_t kTimedFxmix = 2;
constexpr std::size_t kTimedPan = 3;
constexpr std::size_t kTimedDolby = 4;
constexpr std::size_t kTimedDistortion = 5;
constexpr std::size_t kTimedIir = 12;
constexpr std::size_t kTimedPause = 16;
constexpr std::size_t kTimedDelay = 17;

constexpr float kShortMax = 32767.0f;
constexpr uint8_t kNoNote = 0xff;

// A float's conversion to an integer as the build does it (fcvtzs): toward 0, saturated to 32 bits.
using jaudio2::ToInt32;

// A timed level that takes its target at once: no step, and one tick left.
[[maybe_unused]] void TakeTarget(MoveParam& move)
{
    move.value = move.target;
    move.step = 0.0f;
    move.time = 1.0f;
}

} // namespace

} // namespace ntdlib::jac
