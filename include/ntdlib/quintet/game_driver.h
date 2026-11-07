// SPDX-License-Identifier: MIT

// A game's Quintet driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the sound
// init and the routines that the game calls after it, the loader of a music file where the game has one, the play
// routine of a song or of a sound effect, and each frame the per-frame routine. It records the stores to the sound
// registers, to DMA 1 and DMA 2, to timers 0 and 1, and to the FIFOs. It's the reference for ntdlib's model of the
// driver (quintet::Sequencer). Neither the DMAs nor the timers run.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/quintet/driver.h"

namespace ntdlib::quintet
{

// The routines and the RAM of a game's driver, or of a program that the game sends to another console with a copy of
// the driver. The routines are Thumb code.
struct GameRoutines
{
    // A routine that the game calls after the sound init, before any song.
    struct Call
    {
        uint32_t address = 0;
        std::vector<uint32_t> args;
    };

    uint32_t image = 0;      // a program's copy in the ROM, which runs from the start of EWRAM, or 0 for the game
    uint32_t sound_init = 0; // the game's sound init, or the driver's init in a game with a loader
    std::vector<Call> init_calls;
    uint32_t loader = 0;          // the loader of a music file: r0 = the file's first word. 0 if the game has none
    uint32_t play = 0;            // r0 = the song
    uint32_t play_effect = 0;     // r0 = the sound effect
    uint32_t frame = 0;           // the per-frame routine
    uint32_t channels = 0;        // the table of pointers to the 6 records of the music channels
    uint32_t effect_channels = 0; // the table of pointers to the 6 records of the sound effect channels
    std::vector<uint32_t> files;  // the music files that the loader reads: each file's first word in the ROM
};

class GameDriver
{
public:
    // Loads the cartridge, copies a program to EWRAM where `routines` names one, and runs the sound init and the calls
    // after it. Throws std::runtime_error when a routine fails.
    GameDriver(const agb::Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Reads the music file whose first word is at `file` in the ROM, where the game has a loader. Then plays song
    // `index` of it, or sound effect `index` when `effect` is true. Each call clears Writes() first.
    void Play(uint32_t file, int index, bool effect);

    // Runs the per-frame routine. Each call clears Writes() first.
    void Frame();

    // The stores since the last Play() or Frame() began, in order: to the sound registers from kRegSound1CntL up to the
    // FIFOs, to DMA 1's and DMA 2's registers, and to timer 0's and timer 1's. A DMA's source in a program that runs
    // from EWRAM is given as the ROM's address of the same byte.
    const std::vector<agb::RegisterWrite>& Writes() const
    {
        return writes_;
    }

    // The stores of the sound init and the calls after it, in the same way.
    const std::vector<agb::RegisterWrite>& InitWrites() const
    {
        return init_writes_;
    }

    // The stores of Writes() with the stores to the FIFOs among them, in order.
    const std::vector<agb::RegisterWrite>& Stream() const
    {
        return stream_;
    }

    // The read position of channel `channel` (0 to 5) of the channels that the last Play() started, or 0 once the
    // channel has ended.
    uint32_t Position(int channel);

    // Returns true while a channel has a read position.
    bool Playing();

    agb::System& Gba()
    {
        return system_;
    }

private:
    void Call(uint32_t address, std::span<const uint32_t> args = {});

    // Returns the ROM's address of a byte of a program that runs from EWRAM, or `address` itself.
    uint32_t RomAddress(uint32_t address) const;

    // Returns the address in EWRAM of a byte of the ROM's copy of a program, or `address` itself.
    uint32_t RamAddress(uint32_t address) const;

    // Stores again, without recording, the bits of a register that the hardware returns when the driver reads it.
    void MaskReadBack(uint32_t address, int size);

    GameRoutines routines_;
    agb::System system_;
    bool recording_ = false;
    uint32_t records_ = 0; // the table of channel records that the last Play() started
    std::vector<agb::RegisterWrite> writes_;
    std::vector<agb::RegisterWrite> init_writes_;
    std::vector<agb::RegisterWrite> stream_;
};

} // namespace ntdlib::quintet
