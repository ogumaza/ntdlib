// SPDX-License-Identifier: MIT

// A game's Ubisoft Milan driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the
// mixer's setup, the driver's init with the sound bank, the start of a piece of music, and the two routines of each
// frame. The game's VBlank runs the sequencer's step, and its main loop runs the sound update. GameDriver then runs the
// mixer as the FIFOs' interrupts do, once for each 16 points of output that the frame's time holds. It reads what the
// driver leaves in memory: the track, the voices and the mixer's output. It's the reference for ntdlib's model of the
// driver (ubimilan::Output).

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::ubimilan
{

// The routines and the RAM of a game's driver. The routines are Thumb code, but for the mixer's.
struct GameRoutines
{
    uint32_t file_lookup = 0; // returns a file of the game's archive: r0 = its number
    uint32_t bank_file = 0;   // the archive's file that the game's start hands to the driver as its sound bank
    uint32_t mixer_setup = 0; // clears the voices and sets up the mixer and the FIFOs
    uint32_t init = 0;        // the driver's init: r0 = the sound bank
    uint32_t start = 0;       // starts a piece of music: r0 = its resource, r1 = 1 to start it at once
    uint32_t step = 0;        // the sequencer's step
    uint32_t update = 0;      // the sound update
    uint32_t mixer = 0;       // the mixer, in ARM code, in the ROM
    uint32_t mixer_size = 0;  // the address of the word that gives the mixer's size
    uint32_t mixer_ram = 0;   // where GameDriver puts the mixer: the mixer reads its variables from fixed addresses
    uint32_t track = 0;       // the sequencer's track
    uint32_t voices = 0;      // the mixer's voices
    uint32_t output = 0;      // the 16 bytes that each of the mixer's runs makes
};

class GameDriver
{
public:
    // Loads the cartridge and runs the game's start up to the driver's init. Throws std::runtime_error when a routine
    // fails.
    GameDriver(const Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts the piece of music at `piece`. Throws std::runtime_error when a routine fails. A game that resets makes a
    // BIOS call that the system doesn't model: routines fail there.
    void Play(uint32_t piece);

    // Runs the sequencer's step and the sound update of the next frame, then the mixer's runs up to the frame after it.
    // Adds the mixer's bytes to `bytes`. Each call but the first clears SoundWrites() first. The first keeps the stores
    // of the piece's start. Throws std::runtime_error when a routine fails.
    void Frame(std::vector<int8_t>& bytes);

    // The stores to the sound registers before the FIFOs since the last Frame() began, or since the start of a piece,
    // in order: the PSG's registers, the sound control registers and the wave RAM, from agb::kRegSound1CntL up to
    // agb::kRegFifoA.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // The same stores that the game's start and the driver's init made.
    const std::vector<agb::RegisterWrite>& InitWrites() const;

    // The sound bank that the game's start gave the driver.
    uint32_t Bank() const
    {
        return bank_;
    }

    // The track as the driver keeps it: the countdown, the next command, the next sequence, whether it plays and
    // whether an end command has been read.
    std::array<uint32_t, 5> TrackState();

    // Returns false if the mixer doesn't play voice `v`.
    bool VoicePlays(int v);

    // Voice `v` as the mixer keeps it: its next point, its loop's start, its end, its loop's end, its volume and
    // whether it loops.
    std::array<uint32_t, 6> VoiceState(int v);

    agb::System& Gba();

private:
    void Call(uint32_t address, std::span<const uint32_t> args = {});

    GameRoutines routines_;
    agb::System system_;
    uint32_t bank_ = 0;
    uint64_t frames_ = 0;
    uint64_t runs_ = 0;
    uint32_t run_cycles_ = 0;
    std::vector<agb::RegisterWrite> sound_writes_;
    std::vector<agb::RegisterWrite> init_writes_;
};

} // namespace ntdlib::ubimilan
