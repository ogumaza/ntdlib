// SPDX-License-Identifier: MIT

// A game's copy of Nintendo R&D1's driver, run on the GBA's CPU (agb::System). It calls the routines that the game
// calls: the driver's init, the game's call that sets the echo, the start of a song, the interrupt of the FIFOs' DMA
// and the per-frame routine. It reads what the driver leaves in memory: the bytes that the mixer leaves in its ring for
// the FIFOs, and the voices' records.
//
// The mixer keeps the bytes for each FIFO in a ring of words. Timer 0 takes a byte from each FIFO every TimerPeriod()
// cycles: 0xFFFED9 / rate, 1253 cycles at 13379 Hz. Each time a FIFO's bytes fall to 16, its DMA moves 4 words from the
// ring to it. DMA 2's interrupt follows, and its handler moves the ring's read place on by 4 words. Each frame, the
// mixer fills the ring from its write place up to a lead past the read place. Transfer() runs the interrupt's handler,
// and Frame() the per-frame routine. A caller runs the transfers that the FIFOs have asked for before each frame: 14 or
// 15 at 13379 Hz.
//
// The per-frame routine runs the players, then the voices' routine, then the echo's settings and the mixer. GameDriver
// puts an SWI in place of the call of the voices' routine in its copy of the cartridge. The SWI's handler calls the
// routine, then `before_mix`. A caller can change the voices' records there before the mixer reads them.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/rd1/driver.h"

namespace ntdlib::rd1
{

// The bytes that each transfer moves from a ring to its FIFO.
constexpr int kTransferBytes = 16;

// The bytes of a voice's record in the driver's RAM: a sample voice's, a mixer voice's and a PSG voice's.
constexpr int kVoiceRecordBytes = 32;

// The PSG's voices.
constexpr int kGamePsgVoices = 4;

// The SWI that GameDriver puts in place of the call of the voices' routine.
constexpr uint32_t kVoicesSwi = 0xF0;

// The routines and the RAM of a game's driver. The routines are Thumb code.
struct GameRoutines
{
    uint32_t init = 0;                                       // the driver's init
    uint32_t echo = 0;                                       // sets the echo's four settings: r0 to r3
    std::array<uint32_t, 4> echo_settings = {0x23, 2, 2, 4}; // what the game's init passes to it
    uint32_t play = 0;                                       // starts entry r0 of the song table
    uint32_t start = 0;                                      // starts song record r1 on player record r0
    uint32_t player_table = 0;                               // 12 bytes a player: its record in RAM
    uint32_t frame = 0;                                      // the per-frame routine
    uint32_t voices_call = 0;                                // the per-frame routine's call of the voices' routine
    uint32_t transfer = 0;                                   // the handler of DMA 2's interrupt
    uint32_t fifo_a = 0;                                     // a word: the address of FIFO A's ring
    uint32_t fifo_b = 0;                                     // a word: the address of FIFO B's ring
    uint32_t ring_words = 0;                                 // a word: the words of each ring
    uint32_t write_place = 0;                                // a word: the ring's word that the mixer writes next
    uint32_t read_place = 0;                                 // a word: the ring's word that the DMA reads next
    uint32_t sample_voices = 0;                              // a word: the address of the sample voices' records
    uint32_t mixer_voices = 0;                               // a word: the address of the mixer voices' records
    uint32_t mixer_voice_count = 0;                          // a halfword: the mixer's voices
    uint32_t psg_voices = 0;                                 // the PSG voices' records

    // The mixer's settings: see ArchivePlayer's enhanced mode.
    uint32_t filter = 0;       // the CC72 filter: a byte of its mode and coefficient, then a word of state a side
    uint32_t filter_gain = 0;  // a byte: the gain of the voices through the filter while it's a high-pass
    uint32_t filter_all = 0;   // a byte: while it isn't 0, every voice goes through the filter. 0 without it.
    uint32_t echo_level = 0;   // a word: the echo's level
    uint32_t echo_shift = 0;   // a word: the shift of the echo's first one-pole stage
    uint32_t echo_shift_2 = 0; // a word: the shift of its second stage, and of its level
    uint32_t echo_offset = 0;  // a word: the words by which FIFO B's echo reads ahead of FIFO A's
};

// The mixer's settings for its echo and its filter of CC72: the values that the voices' routine leaves before the mix.
struct MixSettings
{
    uint8_t filter = 0;        // the filter's mode (bit 7 for the high-pass) and coefficient
    uint8_t filter_gain = 0;   // the gain of the voices through the high-pass
    bool filter_all = false;   // whether every voice goes through the filter
    uint32_t echo_level = 0;   // the echo's level
    uint32_t echo_shift = 0;   // the shift of its first stage
    uint32_t echo_shift_2 = 0; // the shift of its second stage and of its level
    uint32_t ring_bytes = 0;   // the bytes of each ring
    uint32_t echo_offset = 0;  // the bytes by which FIFO B's echo reads ahead of FIFO A's
};

// A mixer voice's record before the mix, and the track that it plays.
struct MixVoice
{
    std::array<uint8_t, kVoiceRecordBytes> record = {};
    int voice = 0;         // the mixer's voice
    int track = -1;        // the track that it plays, or -1
    bool filtered = false; // whether it goes through the filter of CC72
};

// Finds the routines and the RAM of the driver in `rom`, from the code of the routines that `info` gives. Returns
// nothing when the code isn't laid out as the known revisions lay it out.
std::optional<GameRoutines> FindGameRoutines(const agb::Rom& rom, const DriverInfo& info);

class GameDriver
{
public:
    // Loads the cartridge, runs the driver's init and sets the echo as the game does. Throws std::runtime_error when a
    // routine fails, and std::invalid_argument when `routines.voices_call` holds no call.
    GameDriver(const agb::Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts entry `song` of the song table. The game's play routine does the same.
    void Play(int song);

    // Starts the song record at `record` on player `player`. The play routine does the same with an entry of the song
    // table.
    void Start(int player, uint32_t record);

    // Runs DMA 2's interrupt: the ring's read place moves on by 4 words.
    void Transfer();

    // Runs the per-frame routine. Each call clears SoundWrites() first.
    void Frame();

    // The cycles between the FIFOs' samples: the period of the timer that FIFO A plays at. The init sets it.
    uint32_t TimerPeriod();

    // The stores to the sound registers before the FIFOs of the construction or the last call of Play(), Start() or
    // Frame(), in order.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // The bytes that the mixer wrote to FIFO A's ring and FIFO B's ring in the last frame, in order.
    const std::vector<int8_t>& FifoA() const;
    const std::vector<int8_t>& FifoB() const;

    // The bytes that lie in each ring between its read place and its write place: what the FIFOs play before the bytes
    // of the next frame.
    std::vector<int8_t> PendingA();
    std::vector<int8_t> PendingB();

    // The bytes of sample voice `voice`'s record, of mixer voice `voice`'s record and of PSG voice `voice`'s record.
    std::vector<uint8_t> SampleVoiceBytes(int voice);
    std::vector<uint8_t> MixerVoiceBytes(int voice);
    std::vector<uint8_t> PsgVoiceBytes(int voice);

    // The mixer's settings now.
    MixSettings Settings();

    const GameRoutines& Routines() const;
    agb::System& Gba();
    const agb::System& Gba() const;

    // Called in each frame after the voices' routine and before the mixer.
    std::function<void()> before_mix;

private:
    // Appends the ring's bytes from word `from` up to word `to` to `out`.
    void ReadRing(uint32_t ring, uint32_t from, uint32_t to, std::vector<int8_t>& out);

    std::vector<uint8_t> RecordBytes(uint32_t at);

    GameRoutines routines_;
    agb::System system_;
    uint32_t voices_routine_ = 0;
    std::vector<agb::RegisterWrite> sound_writes_;
    std::vector<int8_t> fifo_a_;
    std::vector<int8_t> fifo_b_;
};

} // namespace ntdlib::rd1
