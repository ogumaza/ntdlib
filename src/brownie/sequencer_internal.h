// SPDX-License-Identifier: MIT

// Definitions that the parts of the Brownie Brown sequencer share (sequencer.cpp, sequencer_channels.cpp and
// sequencer_mix.cpp): the names they use, the hardware registers, the limits, and the helpers of the channels and
// voices. The three files build as one translation unit. The definitions therefore keep internal linkage. Only those
// files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/brownie/song.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::brownie
{

using ntdlib::agb::kEnhancedFrameSamples;
using ntdlib::agb::kFrameCycles;
using ntdlib::agb::WarnOnce;

namespace
{

// The hardware registers the driver writes besides the sound registers.
constexpr uint32_t kIoFifoA = 0x040000A0; // then FIFO B at 0x040000A4
constexpr uint32_t kIoDma1Control = 0x040000C6;
constexpr uint32_t kIoDma2Control = 0x040000D2;
constexpr uint32_t kIoTimer0Reload = 0x04000100;
constexpr uint32_t kIoTimer0Control = 0x04000102;
constexpr uint32_t kIoTimer1Reload = 0x04000104;
constexpr uint32_t kIoTimer1Control = 0x04000106;
constexpr uint32_t kIoSoundBias = 0x04000088;

// The setting the driver gives both FIFOs' DMA: on, with an interrupt, at the FIFO's request, repeating, a word at a
// time to a fixed address. Bit 10, a word at a time, is the one the driver checks.
constexpr uint16_t kDmaOn = 0xF640;
constexpr uint16_t kDmaWords = 0x0400;
constexpr uint16_t kTimerOn = 0x0080;

// The DMA 1 and DMA 2 interrupts, which run the mixer, or in the Magical Vacation revision feed FIFO A and FIFO B.
constexpr uint16_t kMixerInterrupts = 0x0600;
constexpr uint16_t kFifoAInterrupt = 0x0200;

// The setting the Magical Vacation revision gives a FIFO's DMA, which only makes the FIFO's requests interrupt, since
// the driver never gives the DMA addresses: on, with an interrupt, at the FIFO's request, repeating, a word at a time.
constexpr uint16_t kFifoDmaOn = 0xF600;

// The NRx2 that a PSG note starts with when its envelope's first level is below the channel's volume: volume 0 with the
// direction bit set. It's silent, and it keeps the channel's DAC on. A later step below the volume writes 0.
constexpr uint32_t kSilentNrx2 = 0x08;

// The bytes a FIFO holds, and the most it can hold when it asks for more after playing a point.
constexpr int kFifoBytes = 32;
constexpr int kFifoRequest = 16;

// The sounds that start only when the noise channel is free.
constexpr int kNoiseSound1 = 0x3B;
constexpr int kNoiseSound2 = 0x3D;

// The sound the driver queues when E3 brings the sound back.
constexpr int kResumeSound = 0x36;

// The most commands a channel reads in a frame, so that data that never reaches a note can't hang the model, and the
// most channels a sound starts.
constexpr int kMaxCommands = 10000;
constexpr int kMaxSoundChannels = 64;

std::string HexByte(int v)
{
    char b[8];
    std::snprintf(b, sizeof b, "%02X", unsigned(v & 0xFF));

    return b;
}

// Returns a sample channel's volume with its envelope's level: (volume + 1) × level / 256, rounded down. The record
// holds it at 0x25.
uint8_t VolumeAtLevel(const Channel& ch)
{
    return uint8_t(((ch.volume + 1u) * ch.level) >> 8);
}

// Sets voice `v`'s sides as a note and a fade's step set them: from sample channel `ch`'s volume with its envelope's
// level, the master level `master` and the pan, rounded down once. SetVoiceLevels() rounds twice.
void SetNoteSides(Voice& v, const Channel& ch, uint32_t master)
{
    const uint32_t scaled = ch.EnvelopeVolume() * (master + 1);
    v.right = uint8_t(((ch.RightLevel() + 1u) * scaled) >> 16);
    v.left = uint8_t(((ch.LeftLevel() + 1u) * scaled) >> 16);
}

} // namespace

} // namespace ntdlib::brownie
