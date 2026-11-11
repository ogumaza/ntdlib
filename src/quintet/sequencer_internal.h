// SPDX-License-Identifier: MIT

// Definitions that the parts of the quintet sequencer share (sequencer.cpp, sequencer_effects.cpp and
// sequencer_output.cpp): the hardware registers, the limits on a frame, and the arithmetic of the driver. The three
// files build as one translation unit. The definitions therefore keep internal linkage. Only those files include this
// header.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/sequencer.h"
#include "ntdlib/quintet/song.h"

namespace ntdlib::quintet
{

namespace
{

// The registers outside the image that the driver writes: SOUNDBIAS, and the wave RAM's 16 bytes.
constexpr uint32_t kSoundbias = 0x04000088;
constexpr uint32_t kWaveRam = 0x04000090;

// The registers that play a FIFO's samples: the timer that sets its rate, and the DMA channel that feeds it.
struct FifoRegisters
{
    uint32_t timer;       // TMxCNT_L: a word written there sets TMxCNT_H too
    uint32_t dma_source;  // DMAxSAD
    uint32_t dma_dest;    // DMAxDAD
    uint32_t dma_count;   // DMAxCNT_L
    uint32_t dma_control; // DMAxCNT_H
    uint32_t fifo;        // FIFO_A or FIFO_B: the DMA's destination
    uint16_t reset;       // the bit of SOUNDCNT_H that resets the FIFO
};

// FIFO A plays through timer 0 and DMA 1. FIFO B plays through timer 1 and DMA 2.
constexpr FifoRegisters kFifoRegisters[2] = {
    {0x04000100, 0x040000BC, 0x040000C0, 0x040000C4, 0x040000C6, 0x040000A0, 0x0800},
    {0x04000104, 0x040000C8, 0x040000CC, 0x040000D0, 0x040000D2, 0x040000A4, 0x8000}};

// The bits that the hardware returns when its SOUNDCNT_H or NR30 is read.
constexpr uint16_t kSoundcntHRead = 0x770F;
constexpr uint8_t kNr30Read = 0xE0;

// The driver's figure for the CPU clock, from which it works out a FIFO's timer setting.
constexpr uint32_t kTimerClock = 16780000;

// The most commands a channel reads in a frame, so that data that never reaches a note can't hang the model.
constexpr int kMaxCommands = 10000;

// The most bytes of a noise macro read in a frame, so that a macro that never reaches a setting can't hang the model.
constexpr int kMaxMacroBytes = 10000;

// Divides as the game's library does: rounding towards 0, and giving 0 for a division by 0.
int32_t Divide(int32_t a, int32_t b)
{
    return b == 0 ? 0 : a / b;
}

uint32_t DivideUnsigned(uint32_t a, uint32_t b)
{
    return b == 0 ? 0 : a / b;
}

int32_t Remainder(int32_t a, int32_t b)
{
    return b == 0 ? 0 : a % b;
}

// Returns half of `x`, rounded towards 0, as the driver's (x + sign) >> 1 does.
int32_t Half(int32_t x)
{
    return (x + (x < 0 ? 1 : 0)) >> 1;
}

// Returns the timer setting for a FIFO that plays at `rate` Hz: the reload value, and the timer's enable bit.
uint32_t TimerSetting(uint32_t rate)
{
    return (0x10000 - SampleCycles(rate)) | 0x800000;
}

// Returns the rate at which a FIFO plays a sample of `base_rate` Hz at PCM pitch `pitch`: the sample's rate plus the
// rate × pitch / 1000. The division rounds towards 0.
uint32_t PlayRate(uint32_t base_rate, uint16_t pitch)
{
    return base_rate + uint32_t(Divide(int32_t(base_rate) * int16_t(pitch), 1000));
}

// Returns the index of note `note` (0-11, or 12 to play again or 13 to rest) at the channel's octave and transposes.
// The index stays within a pitch table of `entries` entries.
int NoteIndex(const Channel& ch, int note, int entries)
{
    const int index = ch.octave * 12 + note + ch.transpose + ch.transpose2;
    return index < 0 ? 0 : index > entries - 1 ? entries - 1 : index;
}

// Starts the channel's effects again for a note or a "play again". Stops them for a rest (note 13).
void RestartEffects(Channel& ch, int note)
{
    if (note != 13)
    {
        ch.macro = ch.macro_start;
        ch.macro_count = 0;
        ch.macro_hold = 0;
        ch.envelope_time = 0;
        ch.bend_time = 0;
        ch.lfo_time = 0;
        ch.switch_count = ch.switch_delay;
    }
    else
    {
        ch.macro = 0;
        ch.envelope_time = kAfterRest;
        ch.bend_time = kAfterRest;
        ch.lfo_time = kAfterRest;
        ch.switch_count = 0;
    }
}

// Sets the channel's countdown to the frames until its next note. The next note comes `ticks` ticks after this one. The
// channel counts the ticks it has played and the frames they take. A tick takes 37 / tempo frames. Working the frames
// out from the whole count keeps the rounding from adding up. A note lasts at least a frame.
void WaitTicks(Channel& ch, uint16_t ticks)
{
    ch.ticks += uint32_t(int32_t(int16_t(ticks)));
    uint32_t frames = DivideUnsigned(ch.ticks * 37, ch.tempo);
    uint32_t wait = frames - ch.frames;
    if (wait == 0)
    {
        frames++;
        wait = 1;
    }

    ch.frames = frames;
    ch.countdown = uint16_t(wait);
}

} // namespace

} // namespace ntdlib::quintet
