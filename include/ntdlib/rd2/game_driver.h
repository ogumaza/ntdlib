// SPDX-License-Identifier: MIT

// A game's R&D2 driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the game's sound
// init, the requests for a song, and each frame the driver's VBlank routine and its per-frame routine. It reads what
// the driver leaves in memory: the bytes that the mixer leaves for the FIFOs and the voices' records. It's the
// reference for ntdlib's model of the driver (rd2::Output).

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/rd2/driver.h"

namespace ntdlib::rd2
{

// The bytes of a voice's record in the driver's RAM.
constexpr int kVoiceRecordBytes = 0x78;

// The byte of a track's record that mutes it.
constexpr int kTrackMuteByte = 0x4A;

// The routines and the RAM of a game's driver. The routines are Thumb code.
struct GameRoutines
{
    uint32_t sound_init = 0;     // the game's sound init: it calls the driver's init with the game's settings
    uint32_t request = 0;        // the play request: r0 = the player, r1 = the sequence
    uint32_t effect_request = 0; // a sound effect's request: r0 = the player, r1 = the sequence, r2 = the track
    uint32_t commit = 0;         // hands the requests made since its last call to the next frame
    uint32_t vblank = 0;         // restarts timer 0 and the output DMAs
    uint32_t frame = 0;          // the per-frame routine: requests, sequencer, PSG voices and mixer
    uint32_t voices = 0;         // the sample voices' records, then the PSG voices'
    uint32_t tracks = 0;         // the tracks' records
    uint32_t players = 0;        // the players' records
    uint32_t outputs = 0;        // the addresses of the two output buffers, and the same again past a buffer's FIFO A
    uint32_t output_index = 0;   // the byte that gives the buffer the mixer writes in this frame
    uint32_t echo_level = 0;     // the byte that gives the echo's level
    uint32_t echo_goal = 0;      // the byte that gives the level that the game asks for
    int music_player = 0x12;     // the player that the game plays its music on
    int effect_player = 0x10;    // the first of the players that the game plays its sound effects on
    int player_bytes = 0x48;     // a player's record
    int player_playing = 0x42;   // the record's byte that is 1 while the player plays
    uint32_t play_music = 0;     // the game's play-music routine, or 0: r0 = the game's music number
    uint32_t yoshi_mute = 0;     // the game's routine for the tracks that play only with Yoshi, or 0: r0 = 1 with him
};

class GameDriver
{
public:
    // Loads the cartridge and runs the game's sound init. Throws std::runtime_error when a routine fails.
    GameDriver(const agb::Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Requests version `version` of song `song` of `info` as the game does. The driver starts it in the next frame.
    void Play(const DriverInfo& info, int song, int version = 0);

    // Asks the driver for the echo's level `level`, as the game's request does. The level moves towards it by 1 a
    // frame.
    void SetEchoGoal(uint8_t level);

    // Mutes or unmutes the player's track `track` with its mute byte: the track starts no more notes.
    void MuteTrack(int track, bool mute);

    // Runs a frame: the VBlank routine, then the per-frame routine. Each call clears SoundWrites() first.
    void Frame();

    // The stores of the last frame to the sound registers before the FIFOs, in order.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // The bytes that the mixer left in the last frame: 176 for FIFO A, then 176 for FIFO B.
    std::vector<int8_t> OutputBytes();

    // The bytes of voice `voice`'s record.
    std::vector<uint8_t> VoiceBytes(int voice);

    // Whether the player that plays the song still plays.
    bool Playing();

    agb::System& Gba();

private:
    uint32_t PlayerAddress();

    GameRoutines routines_;
    agb::System system_;
    int player_ = 0;
    std::vector<agb::RegisterWrite> sound_writes_;
};

} // namespace ntdlib::rd2
