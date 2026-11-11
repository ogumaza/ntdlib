// SPDX-License-Identifier: MIT

// Definitions that the parts of the R&D2 sequencer share (sequencer.cpp, sequencer_voices.cpp, sequencer_levels.cpp and
// sequencer_output.cpp): the sound registers, the limits, the arithmetic of the driver and the events. The four files
// build as one translation unit. The definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/sequencer.h"

namespace ntdlib::rd2
{

namespace
{

using ntdlib::agb::Hex;
using ntdlib::agb::WarnOnce;

// The sound registers the driver writes.
constexpr uint32_t kRegisters = 0x04000060;
constexpr uint32_t kNr10 = 0x04000060;
constexpr uint32_t kNr11 = 0x04000062;
constexpr uint32_t kNr12 = 0x04000063;
constexpr uint32_t kNr13 = 0x04000064;
constexpr uint32_t kNr14 = 0x04000065;
constexpr uint32_t kNr21 = 0x04000068;
constexpr uint32_t kNr22 = 0x04000069;
constexpr uint32_t kNr23 = 0x0400006C;
constexpr uint32_t kNr24 = 0x0400006D;
constexpr uint32_t kNr30 = 0x04000070;
constexpr uint32_t kNr31 = 0x04000072;
constexpr uint32_t kNr32 = 0x04000073;
constexpr uint32_t kNr33 = 0x04000074;
constexpr uint32_t kNr41 = 0x04000078;
constexpr uint32_t kNr42 = 0x04000079;
constexpr uint32_t kNr43 = 0x0400007C;
constexpr uint32_t kNr44 = 0x0400007D;
constexpr uint32_t kNr51 = 0x04000081;
constexpr uint32_t kWaveRam = 0x04000090;

// The NRx2 value with which the PSG voices leave a channel's envelope as it is.
constexpr uint8_t kNoEnvelope = 8;

// Each PSG channel's NRx4. A write that sets its bit 7 starts the channel again.
constexpr uint32_t kRestartRegisters[kPsgVoices] = {0x04000065, 0x0400006D, 0x04000075, 0x0400007D};
constexpr uint8_t kRestartBit = 0x80;

// The time of a track that F8 has started and that hasn't run yet.
constexpr uint64_t kFresh = ~uint64_t(0);

// The most commands a track reads in a frame before the model gives up on it. A loop without a wait then can't hang the
// conversion. The driver would hang there.
constexpr int kMaxCommands = 100000;

// The tuning of the region the driver keeps in RAM for instruments with a sample for each key. The region plays a note
// of 0x30 at the sample's rate. It's a sample voice's, with no flags and no release.
constexpr uint8_t kKeySampleTuning = 0x30;

// Returns a / b as the driver's division does. libgcc's division gives 0 for a divisor of 0.
uint32_t UnsignedDiv(uint32_t a, uint32_t b)
{
    return b ? a / b : 0;
}

// Returns a / b, rounded towards 0, as the driver's signed divisions do. A divisor of 0 gives 0, as in libgcc's
// division. The driver's mixer uses the BIOS's Div instead. Div never returns for a divisor of 0.
int32_t SignedDiv(int32_t a, int32_t b)
{
    return b ? int32_t(int64_t(a) / int64_t(b)) : 0;
}

// Returns a released voice's volume for the frame: its volume in the last frame, times (release + 0xE6) / 512.
uint32_t ReleaseFade(const Voice& v)
{
    return ((uint32_t(v.release) + 0xE6) * v.volume) >> 9;
}

// Moves a voice's slide on by a frame. The slide waits out its delay, then adds its step each frame up to its end.
void AdvanceSlide(Voice& v)
{
    if (v.slide_delay != 0)
    {
        v.slide_delay--;
    }
    else if (v.slide_frames != 0)
    {
        v.slide += v.slide_step;
        if (--v.slide_frames == 0)
        {
            v.slide = v.slide_end;
        }
    }
}

// Returns an event without a note's settings.
Event MakeEvent(Event::Kind kind, int track, int voice, uint32_t id, uint64_t units, uint32_t value,
                Event::Phase phase = Event::kCommands)
{
    Event e;
    e.kind = kind;
    e.phase = phase;
    e.track = track;
    e.voice = voice;
    e.id = id;
    e.units = units;
    e.value = value;

    return e;
}

} // namespace

} // namespace ntdlib::rd2
