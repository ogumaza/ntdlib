// SPDX-License-Identifier: MIT

// What the channels of Quintet's driver play their notes with. The driver has no table of instruments. A channel's
// commands select the duty of a square (2F), the wave of the wave channel (DA), the noise macro of the noise channel
// (DB) and the sample of a FIFO (DC). A note gives only a pitch: no key selects another instrument.

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/driver.h"

namespace ntdlib::quintet
{

using ntdlib::agb::Rom;

// An instrument that a channel's commands select.
struct Instrument
{
    enum class Kind : uint8_t
    {
        kSquare, // a duty: 12.5%, 25%, 50% or 75%
        kWave,   // a wave of 32 steps from the game's wave table
        kNoise,  // a noise macro: the noise channel's settings, frame by frame
        kPcm,    // a sample that a FIFO plays
    };

    Kind kind = Kind::kSquare;
    int number = 0;       // the duty, the wave, the noise macro or the sample
    int bank = 0;         // the bank of a noise macro or a sample: 0 for music, 1 for sound effects
    uint32_t address = 0; // the wave's 16 bytes, the macro's settings or the sample's header, or 0 for none
};

// The instrument that number `number` of bank `bank` selects on channel `channel` (0 to 5). Its address is 0 where the
// game's tables don't hold it, or for a noise macro of number 0, which selects none. On a square, `number` is the duty.
// Throws std::out_of_range for a channel past kChannels.
Instrument LookUpInstrument(const Rom& rom, const DriverInfo& info, int channel, int number, int bank = 0);

// The 16 bytes of wave `wave` of the game's wave table, two steps of 4 bits to a byte, high nibble first. Bit 7 of the
// number picks the wave RAM's other bank, not another wave. Gives nothing for a game without a wave table.
std::optional<std::array<uint8_t, 16>> ReadWave(const Rom& rom, const DriverInfo& info, int wave);

// A PCM sample. The driver plays it as signed 8-bit bytes at `rate` Hz for a note at index 24 of the pitch table. A
// channel's E7 sets where each note starts, ends and loops, in percent of `played`.
struct Sample
{
    uint32_t header = 0; // its header: a word of its size with the header, then a halfword of its rate
    uint32_t data = 0;   // its first byte
    uint32_t size = 0;   // the bytes after the header
    uint32_t played = 0; // the bytes that the driver plays: the size with the header, less 64 (256 in the S revision),
                         // less 0.6%
    uint32_t rate = 0;
};

// Reads sample `number` of bank `bank` of the game's samples. Gives nothing for one that the bank doesn't hold.
std::optional<Sample> ReadSample(const Rom& rom, const DriverInfo& info, int bank, int number);

} // namespace ntdlib::quintet
