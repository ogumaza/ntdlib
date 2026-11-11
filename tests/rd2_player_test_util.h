// SPDX-License-Identifier: MIT

// What the tests of rd2::ArchivePlayer share: the names they use, the first sample of the test cartridge, the detection
// of a cartridge, and the tracks and frames that the tests play.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/archive_player.h"
#include "ntdlib/rd2/archive_source.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/output.h"
#include "ntdlib/rd2/sequencer.h"
#include "ntdlib/testing/rd2_cart.h"
#include "ntdlib/wave/blep.h"

namespace
{

using namespace ntdlib::testing::rd2_cart;
using ntdlib::agb::kEnhancedFrameSamples;
using ntdlib::agb::kEnhancedRate;
using ntdlib::agb::kFrameCycles;
using ntdlib::agb::Rom;
using ntdlib::rd2::ArchivePlayer;
using ntdlib::rd2::ArchivePlayerOptions;
using ntdlib::rd2::DriverInfo;
using ntdlib::rd2::DriverOverrides;
using ntdlib::rd2::kFifoCycles;
using ntdlib::rd2::kFifoLead;
using ntdlib::rd2::kFrameSamples;
using ntdlib::rd2::kPlayerTracks;
using ntdlib::rd2::VoiceMix;

constexpr uint32_t kFirstPoints = kSamples + 8 + 16;
constexpr uint32_t kFirstLength = 64;
constexpr uint32_t kFirstLoopStart = 16;
constexpr int kEchoFrames = 18; // the frames of the driver's echo line

// The cycle of the timer's overflow that takes the first mix's first byte.
constexpr uint32_t kFirstByteCycle = uint32_t(kFifoLead + 1) * kFifoCycles;

template <typename E, typename F>
bool Throws(F f)
{
    try
    {
        f();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

// A ROM and its driver, for the players' references.
struct Game
{
    explicit Game(const Cart& cart) : rom(cart.ToRom())
    {
        std::string error;
        const bool found = ntdlib::rd2::DetectDriver(rom, DriverOverrides(), info, error);
        XSFLIB_CHECK(found);
    }

    Rom rom;
    DriverInfo info;
};

// Fills the first sample with one point.
inline void FillFirst(Cart& cart, int point)
{
    for (uint32_t i = 0; i < kFirstLength; i++)
    {
        cart.Put8(kFirstPoints + i, uint8_t(int8_t(point)));
    }
}

// Fills the first sample's loop with a whole sine.
inline void FillSine(Cart& cart)
{
    const uint32_t length = kFirstLength - kFirstLoopStart;
    for (uint32_t i = 0; i < kFirstLength; i++)
    {
        const double phase = 2.0 * 3.14159265358979 * double(i % length) / double(length);
        cart.Put8(kFirstPoints + i, uint8_t(int8_t(std::lround(100.0 * std::sin(phase)))));
    }
}

// A track of the first sample, held, at note 60: a point of the sample for each of the mixer's points.
inline std::vector<uint8_t> HeldTrack(uint8_t pan, uint16_t frames = 1000)
{
    return TrackData().Bytes({0xC3, pan, 0xC2, kHeld}).Note(60, frames, 127).Wait(frames).End();
}

inline std::vector<uint8_t> PsgTrack(uint8_t instrument, uint8_t note, uint16_t frames)
{
    return TrackData().Bytes({0xC2, instrument}).Note(note, frames, 127).Wait(frames).End();
}

inline std::vector<int16_t> Frames(ArchivePlayer& player, int frames)
{
    std::vector<int16_t> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

} // namespace
