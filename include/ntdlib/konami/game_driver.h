// SPDX-License-Identifier: MIT

// A game's Konami driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the init, the
// request and the start of a song, and each frame the per-frame entry. It then runs the mixer as the game's interrupts
// do: a block of 16 points at a time, with the fraction of a block that a frame leaves over carried into the next
// frame. A revision without a mixer runs its DMA interrupt routines from the FIFOs' timers instead. It reads what the
// driver leaves in memory: the output records, the voice records and the FIFOs' bytes. It's the reference for ntdlib's
// model of the driver (konami::Output).

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/konami/output.h"

namespace ntdlib::konami
{

using ntdlib::agb::Rom;

// The routines and the RAM of a game's driver. The addresses of the routines are of Thumb code, but for the mixer's.
struct GameRoutines
{
    // A routine that the game calls after the init, before any song.
    struct Call
    {
        uint32_t address = 0;
        std::vector<uint32_t> args;
    };

    uint32_t init = 0;
    std::vector<uint32_t> init_args;
    std::vector<Call> init_calls;
    uint32_t request_song = 0; // r0 = the song, then request_args
    std::vector<uint32_t> request_args;
    uint32_t start_song = 0;
    uint32_t frame = 0;        // the per-frame entry
    uint32_t output_stage = 0; // the output records lie at sp + record_offset there
    int tracks = 16;           // the tracks of a song, and the output records
    int record_offset = 8;
    int record_size = 12;
    int record_pan = 8; // the record's offset of its two pan bytes, or -1 in a record without pan

    uint32_t mixer = 0;          // ARM code, in IWRAM or in ROM, or 0 in a revision without a mixer
    uint32_t mixer_call = 0;     // a ring mixer: the routine through which the per-frame entry runs the mixer
    uint32_t mixer_tick = 0;     // a ring mixer: the DMA 1 interrupt's routine, for the position
    uint32_t mixer_position = 0; // a ring mixer's position
    int mixer_ring = 0;          // a ring mixer's buffer, in points
    uint32_t fifo_buffer = 0;    // FIFO A's buffer
    int fifo_b = 0x20;           // FIFO B's buffer, from FIFO A's
    uint32_t voices = 0;         // the voice records
    int voice_count = 12;
    int voice_size = 28;
    uint32_t echo_buses = 0;             // the echo buses' records of 20 bytes each, or 0 in a driver without echo
    uint32_t echo_mask = 0;              // the routing that the mixer reads
    std::array<uint32_t, 2> fifo_irqs{}; // the DMA 1 and DMA 2 interrupt routines that mix FIFO A's and FIFO B's points
                                         // in a revision without a mixer

    uint32_t live_request = 0; // the game's live stage's request: r0 = an entry of live_table, then live_args
    std::vector<uint32_t> live_args;
    uint32_t live_table = 0; // the live stage's songs
    int live_entry_size = 0;
    int live_entries = 0;
};

// A track's output record as the driver leaves it at the output stage.
struct GameRecord
{
    int16_t pitch = 0;
    uint8_t b2 = 0;
    uint8_t vol = 0;
    uint8_t trig = 0;
    uint8_t flags = 0;
    uint16_t key = 0;
    uint8_t pan_a = 0; // the record's first and second pan bytes, 0 in a record without pan
    uint8_t pan_b = 0;
};

class GameDriver
{
public:
    // Loads the cartridge and runs the init and the game's calls after it. Throws std::runtime_error when a routine
    // fails.
    GameDriver(const Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Requests `song` and starts it through the game's routines, or through the game's live stage. Throws
    // std::runtime_error when a routine fails, or the game has no live stage that plays the song.
    void Play(int song, bool live = false);

    // Runs the per-frame entry. Returns true if the driver reached its output stage. Records() then holds the output
    // records as it found them. Each call clears SoundWrites() first.
    bool Frame();

    // The output records of the last frame that reached the output stage.
    const std::vector<GameRecord>& Records() const;

    // The stores since the last Frame() began to the sound registers before the FIFOs, in order: the PSG's registers,
    // the sound control registers and the wave RAM, from agb::kRegSound1CntL up to agb::kRegFifoA.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // Runs the mixer for the frame that Frame() has just run, or a revision's DMA interrupt routines for its FIFOs.
    // Puts the bytes that the FIFOs take in `fifo_a` and `fifo_b`. The mixers of the Eternal Duelist and Captain
    // Tsubasa revisions fill rings, and the bytes are those of the frame's blocks. A revision without a mixer gives the
    // 16 bytes of each DMA interrupt, whose timers run at their rates. Its FIFOs needn't take the same number of bytes.
    void Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b);

    // The bytes that the last frame stored in FIFO `fifo` through its register. The game's note starts store such bytes
    // after a reset.
    std::vector<int8_t> FifoStores(int fifo) const;

    // The changes of the FIFOs' timers in the frame that Mix() last ran, in the order of their cycles, in a revision
    // without a mixer. A FIFO's changes count while the FIFO runs on the timer that's meant for it: FIFO A on timer 0,
    // and FIFO B on timer 1.
    const std::vector<FifoTimerChange>& TimerChanges() const;

    // Timer 0's overflows a block of 16 points takes, in CPU cycles, from the setting that the init started.
    uint32_t BlockCycles();

    // The rate of timer 0's overflows, in Hz.
    double Rate();

    // The bytes of voice record `voice`.
    std::vector<uint8_t> VoiceBytes(int voice);

    // The bytes of echo bus `bus`'s record, 20 bytes.
    std::vector<uint8_t> EchoBytes(int bus);

    // The routing that the mixer reads, or 0 in a driver without echo.
    uint32_t EchoMask();

    agb::System& Gba();

private:
    // A FIFO that a revision without a mixer fills: the bytes that it has yet to play, the sample that it plays, and
    // the frame's cycle of its timer's next overflow.
    struct Fifo
    {
        std::vector<int8_t> queue;
        bool ticking = false;
        int64_t next_tick = 0;
    };

    void Call(uint32_t address, std::span<const uint32_t> args = {});

    // Returns FIFO `index`'s timer period in CPU cycles, or 0 while the timer is stopped.
    uint32_t FifoPeriod(int index);

    // Plays FIFO `index`'s next sample. With 16 bytes or fewer left, its DMA takes the next 16 from its buffer, and the
    // DMA's interrupt routine runs. Appends the bytes that the DMA takes to `out`.
    void TickFifo(int index, std::vector<int8_t>& out);

    GameRoutines routines_;
    agb::System system_;
    bool recording_ = true;
    std::vector<agb::RegisterWrite> sound_writes_;
    std::vector<agb::RegisterWrite> fifo_writes_; // the frame's stores to the FIFOs and to SOUNDCNT_H
    std::array<uint32_t, 2> applied_periods_{};   // each timer's period in the changes that the host has had
    std::vector<FifoTimerChange> timer_changes_;
    std::vector<GameRecord> records_;
    uint64_t due_cycles_ = 0; // the cycles of a block that the last frames left over
    std::array<Fifo, 2> fifos_;
};

} // namespace ntdlib::konami
