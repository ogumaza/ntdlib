// SPDX-License-Identifier: MIT

// A game's Rare driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the driver's
// init, the request for a tune, the setter of the music volume and, each frame, the driver's per-frame routine. It
// reads what the driver leaves in memory: the bytes that the mixer leaves for FIFO A and the records of the note
// slots. It's the reference for ntdlib's model of the driver (rare::Sequencer). The routine that swaps the output
// buffers at the vertical blank doesn't run, so the mixer writes the same buffer each frame.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/rare/driver.h"

namespace ntdlib::rare
{

// The bytes of a note slot's record in the driver's RAM.
constexpr int kSlotRecordBytes = 0x28;

// The routines and the RAM of a game's driver. The routines are Thumb code.
struct GameRoutines
{
    uint32_t init = 0;           // the driver's init
    uint32_t request = 0;        // the request for a tune: r0 = the tune, which starts in the next frame
    uint32_t frame = 0;          // the per-frame routine: tracks, envelopes, mixer and the move of the notes
    uint32_t volume_setter = 0;  // sets the music volume: r0 = the volume, out of 128
    uint32_t buffer_flag = 0;    // the word that picks the buffer that the mixer writes in this frame
    uint32_t buffers[2] = {};    // the words in ROM that give the mixer's buffer at a flag of 0, and at others
    uint32_t samples = 0;        // the word that holds the samples a frame
    uint32_t note_slots = 0;     // the note slots' records
    uint32_t channel_levels = 0; // each channel's level, a byte, in Donkey Kong Country 3's revision; or 0
    uint32_t volume_routine = 0; // the game's routine that gives the tune's music volume in r0, or 0
    uint32_t volume_option = 0;  // the halfword of the options' music level that the routine reads, or 0
    uint16_t volume_level = 0;   // the options' music level when the game's first tune starts
};

class GameDriver
{
public:
    // Loads the cartridge and runs the driver's init. Throws std::runtime_error when a routine fails.
    GameDriver(const agb::Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Requests tune `tune`, and sets its music volume as the game's code does: through the game's routine where
    // `routines` gives one, or else MusicVolume(info, tune). The driver starts the tune in the next frame. The game
    // then holds the channels of `silenced` at a level of 0, each frame (SilencedChannels).
    void Play(const DriverInfo& info, int tune, uint16_t silenced = 0);

    // Runs the per-frame routine. Each call clears SoundWrites() first.
    void Frame();

    // The stores of the last frame to the sound registers before the FIFOs, in order.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // The bytes that the mixer left in the last frame for FIFO A.
    std::vector<int8_t> OutputBytes();

    // The bytes of note slot `slot`'s record.
    std::vector<uint8_t> SlotBytes(int slot);

    // The music volume that the last Play() set.
    uint32_t MusicVolume() const
    {
        return volume_;
    }

    agb::System& Gba();

private:
    GameRoutines routines_;
    agb::System system_;
    uint16_t silenced_ = 0;
    uint32_t volume_ = 128;
    std::vector<agb::RegisterWrite> sound_writes_;
};

} // namespace ntdlib::rare
