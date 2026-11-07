// SPDX-License-Identifier: MIT

// A game's Brownie Brown driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls: the
// init and the request of a sound, and each frame the per-frame routine. In the Sword of Mana revision, it then runs
// the mixer as the game's DMA interrupts do, once for each 16 points of the FIFOs, counted from the frame in which the
// DMA and Timer 0 started. In the Magical Vacation revision, each FIFO plays at its timer's rate, and a FIFO that holds
// 16 points or fewer after playing one runs its DMA interrupt's routine. It reads what the driver leaves in memory: the
// channel records, the voices, the driver's variables and the FIFOs' bytes. It's the reference for ntdlib's model of
// the driver (brownie::Sequencer).
//
// Every call starts with r0 to r12 at 0. A channel's rest writes the per-frame routine's r5 to a frequency register. In
// the game, r5 holds what the code that the VBlank interrupted left there. The reference takes 0, as the model does.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/brownie/sequencer.h"

namespace ntdlib::brownie
{

// The routines and the RAM of a game's driver. The routines are ARM code.
struct GameRoutines
{
    // Code that the game copies to IWRAM after the driver's init.
    struct Copy
    {
        uint32_t from = 0;
        uint32_t to = 0;
        uint32_t size = 0;
    };

    uint32_t init = 0;
    uint32_t request = 0; // r0 = the sound
    uint32_t frame = 0;
    std::vector<Copy> copies;

    uint32_t mixer = 0;                  // the routine of both DMA interrupts, or 0 in a revision without a mixer
    uint32_t fifo_points = 0;            // the 16 points that the mixer makes for FIFO A, then the 16 for FIFO B
    std::array<uint32_t, 2> fifo_irqs{}; // the DMA 1 and DMA 2 interrupt routines, in a revision without a mixer

    uint32_t channels = 0; // the channel records, 0x38 bytes each
    int channel_count = 0;
    uint32_t voices = 0; // the voices, or a revision's FIFOs' samples: 16 bytes each
    int voice_count = 0;
    std::vector<std::pair<uint32_t, uint32_t>> variables; // the driver's other variables: each range's start and end
};

class GameDriver
{
public:
    // Loads the cartridge, runs the init and copies the game's code. Throws std::runtime_error when a routine fails.
    GameDriver(const Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Requests `sound`. The driver starts it at the end of the next frame.
    void Play(int sound);

    // Runs the per-frame routine. Each call clears SoundWrites() first. Throws std::runtime_error when it fails.
    void Frame();

    // The stores since the last Frame() began to the sound registers before the FIFOs, in order: the PSG's registers,
    // the sound control registers and the wave RAM.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // Runs the hardware for the frame that Frame() has just run. Puts the bytes that the frame gave each FIFO, in
    // order, in `fifo_a` and `fifo_b`: in the Sword of Mana revision, the mixer's points, or in the Magical Vacation
    // revision, what the note's stores and the interrupts put in the FIFO.
    void Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b);

    // The changes of the FIFOs' timers in the frame that Mix() last ran, in the order of their cycles.
    const std::vector<FifoTimerChange>& TimerChanges() const;

    // Whether the DMA and Timer 0 ran through the mixer's last frame, in the Sword of Mana revision.
    bool Mixing() const;

    // The bytes of channel record `channel`, 0x38 of them.
    std::vector<uint8_t> ChannelBytes(int channel);

    // The bytes of voice `voice`, 16 of them. Mix() moves on the voices. `before_mix` gives them as Frame() left them.
    std::vector<uint8_t> VoiceBytes(int voice, bool before_mix = false);

    // The bytes of the driver's other variables.
    std::vector<uint8_t> VariableBytes();

    // Whether a channel is in use, or the mixer or a FIFO's timer runs.
    bool Playing();

    agb::System& Gba();

private:
    // A FIFO and the timer that plays it, in a revision without a mixer.
    struct HardwareFifo
    {
        bool running = false;
        uint64_t next = 0; // the CPU cycle of the timer's next overflow
        std::deque<int8_t> points;
    };

    void Call(uint32_t address, std::span<const uint32_t> args = {});
    uint32_t Period(int fifo);
    void OnFifoStore(int fifo, int size, uint32_t value);
    void OnTimerStore(int timer, uint32_t value);
    void PlayFifo(int fifo, uint64_t end);

    GameRoutines routines_;
    agb::System system_;
    std::vector<agb::RegisterWrite> sound_writes_;
    std::array<HardwareFifo, 2> fifos_;
    std::array<std::vector<int8_t>, 2> fifo_bytes_; // what the FIFOs took since the last Mix()
    std::vector<FifoTimerChange> timer_changes_;    // the timers' changes since the last Mix()
    std::vector<FifoTimerChange> frame_changes_;
    std::vector<std::vector<uint8_t>> voices_before_mix_;
    uint64_t now_ = 0;       // the CPU cycle that the code runs at
    uint64_t frames_ = 0;    // the frames that Frame() has run
    int64_t mix_start_ = -1; // the frame in which the DMA and Timer 0 started, or -1 while they're stopped
    uint64_t mixed_ = 0;     // the mixer's runs since then
};

} // namespace ntdlib::brownie
