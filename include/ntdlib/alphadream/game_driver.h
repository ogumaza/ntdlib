// SPDX-License-Identifier: MIT

// A game's copy of AlphaDream's driver, run on the GBA's CPU (agb::System). It calls the routines that the game calls:
// the mixer's setup and the driver's init, the start of a song, VBlank's swap of the mixer's halves, the per-frame
// routine and the mix. It reads what the driver leaves in memory: the halves of the FIFOs' buffers, and the sample
// channels' records.
//
// The mixer keeps the bytes for each FIFO in a buffer of two halves of kFrameSamples bytes, FIFO B's after FIFO A's.
// Timer 0 takes a byte from each FIFO every TimerPeriod() cycles: 1064 cycles, 15768 Hz, and kFrameSamples bytes a
// frame. Each time a FIFO's bytes fall to 16, its DMA moves 4 words from the buffer to it. The swap marks the half that
// the DMA plays next, and starts both DMAs again at the buffers' start when that half is the first. The mix fills the
// other half when the last swap marked it free. A frame's mix therefore plays from the next swap on.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/alphadream/driver.h"

namespace ntdlib::alphadream
{

// The samples that each half of a FIFO's buffer holds: a frame's.
constexpr int kFrameSamples = 264;

// The bytes that each transfer moves from a buffer to its FIFO.
constexpr int kTransferBytes = 16;

// The bytes of a sample channel's record: the status, the envelope's level, the sample, the position in 1024ths of a
// point, the right and the left volume, the pitch's fine tune and key, and the region's 4 envelope bytes.
constexpr int kChannelBytes = 16;

// The bytes of a track's record, and of a player's 12 records in the driver's state.
constexpr uint32_t kTrackBytes = 32;
constexpr uint32_t kPlayerBytes = kTrackBytes * kTracks;

// The routines and the RAM of a game's copy of the driver.
struct GameRoutines
{
    uint32_t setup = 0;      // the mixer's setup: decompresses the mixer, sets up timer 0 and the FIFOs' DMA
    uint32_t init = 0;       // the driver's init: r0 is the driver's state
    uint32_t state = 0;      // the state of the driver that the game passes to the init
    bool init_first = false; // whether the game calls the init before the setup
    uint32_t start = 0;      // starts song r1 on player r0
    uint32_t frame = 0;      // the per-frame routine: the commands, the players and the PSG's output
    uint32_t mix = 0;        // mixes a frame into the free half
    uint32_t swap = 0;       // VBlank's swap of the halves
    uint32_t step = 0;       // the mixer's step from a sample's rate and a pitch
    uint32_t step_scale = 0; // the step routine's last factor less 1, in 2^32nds
    uint32_t halves = 0;     // 3 bytes: whether half 0 and half 1 hold a mix, then the half that the DMA plays
    uint32_t buffer = 0;     // FIFO A's buffer: half 0, then half 1. FIFO B's follows it.
    uint32_t channels = 0;   // the sample channels' records: kSampleTracks of kChannelBytes
};

// Finds the routines and the RAM of the driver in `rom`, from the code of the mixer and of the game's calls. Returns
// nothing when the code isn't laid out as in the three known versions.
std::optional<GameRoutines> FindGameRoutines(const Rom& rom, const DriverInfo& info);

// The mixer's step in 1024ths of a point a sample, for a sample at `rate` (1024ths of a hertz at key 60), `key` and
// `fine` (256ths of a semitone, 0 to 255): what the routine at `routines.step` works out, from the driver's tables. A
// fixed-pitch sample plays at kRootKey and a fine tune of 0.
uint32_t MixStep(const Rom& rom, const DriverInfo& info, const GameRoutines& routines, uint32_t rate, int key,
                 int fine);

class GameDriver
{
public:
    // Runs the mixer's setup and the driver's init in the game's order. Throws std::runtime_error when the code fails
    // on the CPU.
    GameDriver(const Rom& rom, const GameRoutines& routines);
    GameDriver(const GameDriver&) = delete;
    GameDriver& operator=(const GameDriver&) = delete;

    // Starts song `song` on player `player`. The game's command 0 does the same.
    void Start(int player, int song);

    // Runs VBlank's swap of the halves. Each call clears SoundWrites() and DmaRestarted() first.
    void Swap();

    // Runs the per-frame routine, then before_mix, then the mix. Each call clears SoundWrites() first.
    void Frame();

    // The cycles between the FIFOs' samples: the period that the setup gives timer 0.
    uint32_t TimerPeriod();

    // The stores to the sound registers before the FIFOs of the construction or the last call of Start(), Swap() or
    // Frame(), in order.
    const std::vector<agb::RegisterWrite>& SoundWrites() const;

    // Whether the last Swap() started the DMAs again at the buffers' start.
    bool DmaRestarted() const;

    // The half that the last Frame() mixed, or -1.
    int MixedHalf() const;

    // The address of half `half` of FIFO `fifo`'s buffer.
    uint32_t Half(int fifo, int half) const;

    // The record of sample channel `channel` now.
    std::array<uint8_t, kChannelBytes> ChannelBytes(int channel);

    // The track record of track `track` of player `player`.
    uint32_t TrackRecord(int player, int track) const;

    // Calls the mixer's step routine: the step in 1024ths of a point a sample of a sample at `rate` (1024ths of a
    // hertz at key 60) for `key` and `fine` (256ths of a semitone).
    uint32_t Step(uint32_t rate, int key, int fine);

    const GameRoutines& Routines() const;
    agb::System& Gba();
    const agb::System& Gba() const;

    // Called in each frame after the per-frame routine and before the mix.
    std::function<void()> before_mix;

private:
    GameRoutines routines_;
    agb::System system_;
    std::vector<agb::RegisterWrite> sound_writes_;
    bool dma_off_ = false;
    bool restarted_ = false;
    int mixed_half_ = -1;
};

} // namespace ntdlib::alphadream
