// SPDX-License-Identifier: MIT

// Plays a sound of a GBA ROM with Brownie Brown's driver on ntdlib's models: the driver (brownie::Sequencer) on the
// GBA's sound hardware (agb::Apu), frame by frame. The driver's stores go to the PSG's registers, and its bytes to the
// FIFOs.
//
// A frame runs the hardware up to the driver's routine, makes the routine's stores, queues what the frame gives the
// FIFOs, and runs the hardware to the frame's end. The driver's interrupts work by frames here, not at the times that
// they take on the console. What a frame's stores change therefore waits in the FIFOs for about as long as it waits on
// the console:
//
// - The Sword of Mana revision runs its mixer from the interrupts of both FIFOs' DMA, at 1,024 cycles a point. Each run
//   makes 16 points of each FIFO behind the 32 that the FIFO holds. A frame's first block therefore lies 32 to 48
//   points from the front of the queue, and the FIFOs start with 48 points of silence. Timer 0 runs from the first note
//   on, and stops when the driver stops it. What the FIFOs hold at that stop plays when the timer starts again.
// - The Magical Vacation revision runs each FIFO on a timer of its own, whose period changes with the FIFO's notes. The
//   timers follow the changes that Sequencer::TimerChanges() gives, at their cycles. A note gives its FIFO the sample's
//   first 4 points at the stores.
//
// The driver leaves the sound bias register as the BIOS does. The output's rate is 32,768 Hz.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. The model of the driver mixes its voices in the mode
// too, by the rules in brownie/sequencer.h. The PSG's channels go through the hardware model's band-limited steps
// (agb/apu.h). The console's mixer and FIFOs run on beside them. Busy() stays the console's. No console or emulator
// shows what the mode should give.
//
// Each track plays one sound channel: track c is the driver's channel c (driver.h). Some things in a track reach
// further than its channel, and a mute leaves them as they are:
//
// - A track runs its commands, stores and timers while it's muted. A wave that it loads still reaches the channel, and
//   its E3 still brings back a sound after a fade.
// - A sound effect's channel takes its PSG channel, or its FIFO, from the music's channel. The PSG channel, or the
//   FIFO, then counts as the sound effect's track for mutes and meters. The music's notes there stay lost. The console
//   loses them too.
// - In the Magical Vacation revision, a muted track's FIFO takes zeros in place of the track's bytes.
// - The Sword of Mana revision's mixer rounds the sum of its voices, and the sum wraps in 8 bits. A mute takes the
//   voice's points out of the sum, with any wrap that they caused. A sum of two muted parts can lie a step from the
//   whole.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/sequencer.h"

namespace ntdlib::brownie
{

// What the driver and the hardware start from, beyond the sound.
struct ArchivePlayerOptions
{
    // The cycle of each frame at which the driver's routine makes its stores. It lies below agb::kFrameCycles.
    uint32_t store_cycle = agb::kFrameCycles / 6;

    // The frame sequencer's phase at the sound's start: see agb::Apu::SetSequencer.
    int sequencer_step = 0;
    uint32_t sequencer_cycles = agb::kSequencerCycles;

    // Plays in the enhanced mixing mode.
    bool enhanced = false;
};

// The points that the Sword of Mana revision's FIFOs start with, and the points that a frame's first block waits behind
// on average once the mixer runs: a block of 16 lies 32 to 48 points from the front of the queue.
inline constexpr uint32_t kFifoLeadPoints = 48;
inline constexpr uint32_t kMeanWaitPoints = 40;

class ArchivePlayer
{
public:
    // Keeps a reference to `rom` and a copy of `info`. `rom` must outlive the player. The player's parts refer to the
    // copy. The player therefore can't be copied.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;

    // Starts sound `song` from the state that the game's init leaves. Throws std::invalid_argument when the sound can't
    // be played or an option lies out of range.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Runs one frame of the driver and of the hardware, and adds the frame's samples to `out` at Rate(), left then
    // right. A frame takes agb::kFrameCycles of the CPU's cycles. Throws std::logic_error before a start, and in the
    // enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // In the enhanced mode, runs one frame and adds its samples, left then right, with full scale at 1. Nothing limits
    // them. Throws std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs one frame without its samples, to reach a place in a sound quickly. The console's mode ends in the state
    // that a frame with samples leaves. In the enhanced mode, the frame's samples are silent, and the next frame with
    // samples starts the PSG's band-limited steps again at the channels' levels.
    void RunFrame();

    // Whether the sound still sounds: until its channels have ended, the mixer or the FIFOs' timers have stopped, and
    // the FIFOs have played their last bytes.
    bool Busy() const;

    uint64_t Frames() const
    {
        return frames_;
    }

    // The output's rate in Hz: agb::kEnhancedRate in the enhanced mode, or else from the sound bias register.
    int Rate() const
    {
        return enhanced_ ? agb::kEnhancedRate : apu_.Rate();
    }

    bool Enhanced() const
    {
        return enhanced_;
    }

    const DriverInfo& Info() const
    {
        return info_;
    }

    // The model of the driver. Throws std::logic_error before a start.
    const Sequencer& Driver() const;

    const agb::Apu& Hardware() const
    {
        return apu_;
    }

    // The bytes that the last frame gave FIFO `fifo` (0 for A, 1 for B).
    const std::vector<int8_t>& FifoBytes(int fifo) const;

    // Leaves track `track`'s channel out of the output from the next frame on. The track and its channel go on as
    // before. Mutes aren't part of a snapshot, and stay through a restore. Throws std::out_of_range for a track past
    // kChannelCount.
    void MuteTrack(int track, bool mute);
    bool IsTrackMuted(int track) const
    {
        return muted_ >> track & 1;
    }

    // While any track is soloed, leaves every track that isn't out of the output, in the same way as a mute. A mute
    // still holds on a soloed track.
    void SoloTrack(int track, bool solo);
    bool IsTrackSoloed(int track) const
    {
        return soloed_ >> track & 1;
    }

    // From the next frame with samples on, keeps the tracks' and the output's peaks when `on`.
    void MeterTracks(bool on);

    // The last frame's largest part of track `track`'s channel on each side, left then right, on the scale of 16-bit
    // samples: on the output's scale at the FIFO's volume and the PSG's. 0 without meters, after a frame without
    // samples and for a muted track. A voice's points reach the meter in the frame that mixes them.
    std::array<int32_t, 2> TrackPeaks(int track) const
    {
        return peaks_[size_t(track)];
    }

    // The last frame's largest output on each side, left then right, on the same scale. 0 without meters and after a
    // frame without samples.
    std::array<int32_t, 2> OutputPeaks() const
    {
        return output_peaks_;
    }

    // What Restore() takes to go back to a place in the sound. A snapshot holds the driver's whole state and the
    // hardware's FIFOs: tens of kilobytes.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::shared_ptr<const Sequencer> sequencer_;
        agb::Apu apu_;
        uint32_t store_cycle_ = 0;
        uint64_t frames_ = 0;
        int tail_ = 0;
        bool enhanced_ = false;
        std::vector<FifoTimerChange> changes_;
        size_t next_change_ = 0;
        uint64_t mixed_cycles_ = 0;
    };

    // Takes a snapshot. `previous` is there for the interface that ntdlib's archive players share: a snapshot stands
    // alone. Throws std::logic_error before a start.
    Snapshot Save(const Snapshot* previous = nullptr) const;

    // Goes back to `snapshot`. The next frames give what they gave after the save. The last frame's meters go. Throws
    // std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    uint16_t SilentTracks() const;
    void ApplyPsgMutes();
    void TakePeaks();

    const Rom& rom_;
    DriverInfo info_;
    std::optional<Sequencer> sequencer_;
    agb::Apu apu_;
    uint32_t store_cycle_ = 0;
    uint64_t frames_ = 0;
    int tail_ = 0;
    bool enhanced_ = false;
    std::vector<FifoTimerChange> changes_; // the timers' changes of the frame that the driver last ran
    size_t next_change_ = 0;
    uint64_t mixed_cycles_ = 0; // the cycles since the driver last ran
    uint16_t muted_ = 0;
    uint16_t soloed_ = 0;
    bool metered_ = false;
    std::array<std::array<int32_t, 2>, kChannelCount> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::brownie
