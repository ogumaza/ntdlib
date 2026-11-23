// SPDX-License-Identifier: MIT

// Definitions that the parts of the model of the SPD driver share (player.cpp, player_tracks.cpp and
// player_channels.cpp): the driver's addresses and registers, its tables and its arithmetic. The three files build as
// one translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/spd/player.h"
#include "ntdlib/spd/song.h"

namespace ntdlib::spd
{

namespace
{

constexpr uint32_t kArea = 0x0214A140;
constexpr uint32_t kSlotTable = 0x14;
constexpr std::size_t kHeaderChannelGroups = 0xE;
constexpr std::size_t kHeaderReverb = 6;
constexpr uint32_t kCaptureLeft = 0x83C00; // the capture buffers, from the sound area's start
constexpr uint32_t kCaptureRight = 0x85C00;
constexpr int kMaxMessages = 64;
constexpr int kSetBSound = 64;

// The sound unit's registers.
constexpr uint32_t kChannelRegisters = 0x04000400;
constexpr uint32_t kCnt = 0x0;
constexpr uint32_t kPanByte = 0x2;
constexpr uint32_t kStartByte = 0x3;
constexpr uint32_t kSad = 0x4;
constexpr uint32_t kTmr = 0x8;
constexpr uint32_t kPnt = 0xA;
constexpr uint32_t kLen = 0xC;
constexpr uint32_t kCaptureControl = 0x04000508;
constexpr uint32_t kCapture0Address = 0x04000510;
constexpr uint32_t kCapture0Length = 0x04000514;
constexpr uint32_t kCapture1Address = 0x04000518;
constexpr uint32_t kCapture1Length = 0x0400051C;
constexpr uint32_t kReverbLength = 1536; // words
constexpr uint32_t kReverbTimer = 0xFC00;
constexpr uint32_t kReverbControl = 0xA8; // loop, PCM16, start
constexpr uint32_t kCaptureStart = 0x8080;
constexpr int kReverbLeft = 1; // the hardware channels of the reverb
constexpr int kReverbRight = 3;

// The timer of a square wave or noise at key 64.
constexpr uint32_t kToneTimer = 0x18DA;

// The tracks that each player keeps (0x037FABF0).
constexpr std::array<uint8_t, kPlayers> kMaxTracks = {14, 10, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0, 0, 0};

// The wobble of the pitch mode 2 (0x038046D0).
constexpr std::array<int8_t, 8> kWobble = {0, 6, 12, 6, 0, -6, -12, -6};

constexpr int kKindSquare = 1;
constexpr int kKindNoise = 2;
constexpr int kKindSample = 3;
constexpr int kKindKit = 7;
constexpr uint32_t kLowPriority = 0x8000;

// The pitch table at 0x03804718: entry n is 65536 * 2^(-n/96), rounded, and entry 0 is 65535.
const std::array<uint16_t, 97>& PitchTable()
{
    static const std::array<uint16_t, 97> kTable = []
    {
        std::array<uint16_t, 97> table{};
        table[0] = 0xFFFF;
        for (int n = 1; n <= 96; ++n)
        {
            table[n] = static_cast<uint16_t>(std::lround(65536.0 * std::exp2(-n / 96.0)));
        }
        return table;
    }();
    return kTable;
}

// The driver's division (0x03804178): truncating, and a division by 0 gives the dividend.
int32_t Divide(int32_t a, int32_t b)
{
    return b == 0 ? a : a / b;
}

int32_t Remainder(int32_t a, int32_t b)
{
    return b == 0 ? 0 : a % b;
}

int SignedBits(uint32_t word, int low, int count)
{
    return static_cast<int>(static_cast<int32_t>(word << (32 - low - count)) >> (32 - count));
}

uint32_t Bits(uint32_t word, int low, int count)
{
    return (word >> low) & ((1u << count) - 1);
}

} // namespace

} // namespace ntdlib::spd
