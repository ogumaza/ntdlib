// SPDX-License-Identifier: MIT

// Plays a song of a GBA ROM with Konami's driver on ntdlib's models: the driver's sequencer, output stage and mixer
// (konami::Output) on the GBA's sound hardware (agb::Apu), frame by frame. The driver's stores go to the PSG's
// registers, and its bytes to the FIFOs.
//
// A frame runs the hardware up to the driver's routine, makes the routine's stores, queues what the frame's blocks give
// the FIFOs, and runs the hardware to the frame's end. The driver's mixer works by frames here, not by the interrupts
// that run it on the console. The bytes that a frame's stores change therefore wait in the FIFOs for the time that
// they wait on the console (Output::FifoLead()):
//
// - A mixer that an interrupt runs for each block mixes the block after the stores within a block's time. The block
//   joins the FIFO a block later, behind the FIFO's other 16 bytes.
// - A ring mixer fills the ring behind the DMA's place. What it mixes at the stores plays once the DMA comes round.
// - The revision without a mixer runs each FIFO on a timer of its own. The timer's period changes with the FIFO's
//   notes. The timers follow the changes that Output::TimerChanges() gives, at their cycles. A note that restarts a
//   FIFO empties it, and stores 4 bytes of 0.
//
// The driver sets the output's rate through the sound bias register: 65,536 Hz for the revisions with a mixer, and
// 32,768 Hz without one. Rate() gives it.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. The model of the driver mixes its voices in the mode
// too, by the rules in konami/output.h. The PSG's channels go through the hardware model's band-limited steps
// (agb/apu.h). The console's mixer and FIFOs run on beside them. Busy() stays the console's. The mode lifts the
// arithmetic's limits and adds no voices: Konami's driver gives each track one channel, and a song's tracks past the
// voices stay silent.
//
// Each track plays one sound channel. Tracks 0 to 3 play the PSG's channels, and track t + 4 the mixer's voice t. Some
// things in a track reach further than its channel, and a mute leaves them as they are:
//
// - A track runs its commands, stores and timers while it's muted. A wave that it loads and a master volume that it
//   sets still reach the channels that use them.
// - In the revisions with pan bytes in their records, a PSG track off centre plays the right side on its channel and
//   the left side on the next, in place of the next track. The next channel then counts as the track's for mutes and
//   meters. The notes of the next track stay lost. The console loses them too.
// - In the revision without a mixer, a FIFO's two voices share the FIFO's timer. A note of one sets the pitch of both,
//   and a note can refuse or stop the other's.
// - A voice leaves an echo bus's input when it's muted. What the voice has already put in the bus's line rings on.
// - The FIFOs' sums wrap in 8 bits. A mute takes the voice's points out of the sum, and any wrap that they caused.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/sequencer.h"

namespace ntdlib::konami
{

// What the driver and the hardware start from, beyond the song.
struct ArchivePlayerOptions
{
    // The cycle of each frame at which the driver's routine makes its stores. It lies below agb::kFrameCycles.
    uint32_t store_cycle = agb::kFrameCycles / 6;

    // The frame sequencer's phase at the song's start: see agb::Apu::SetSequencer.
    int sequencer_step = 0;
    uint32_t sequencer_cycles = agb::kSequencerCycles;

    // Plays in the enhanced mixing mode.
    bool enhanced = false;

    // Requests the song as the game's live stage does (DriverInfo::live_songs). The request sets what the F6 and FA
    // commands read. Other songs ignore it.
    bool live = false;
};

class ArchivePlayer
{
public:
    // Keeps a reference to `rom`. `rom` must outlive the player.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);

    // Starts song `song` from the state that the game's init leaves. Throws std::invalid_argument when the song can't
    // be played or an option lies out of range, and std::runtime_error when the ROM holds no mixer that the revision's
    // code would use.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Runs one frame of the driver and of the hardware, and adds the frame's samples to `out` at Rate(), left then
    // right. A frame takes agb::kFrameCycles of the CPU's cycles. Throws std::logic_error before a start, and in the
    // enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // In the enhanced mode, runs one frame and adds its samples, left then right, with full scale at 1. Nothing limits
    // them. Throws std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs one frame without its samples, to reach a place in a song quickly. The console's mode ends in the state that
    // a frame with samples leaves. In the enhanced mode, the frame's samples are silent, and the next frame with
    // samples starts the PSG's band-limited steps again at the channels' levels. An echo bus's input is silent there
    // too.
    void RunFrame();

    // Whether the song still sounds: until its tracks have ended, its voices and its echo lines have fallen silent, and
    // the FIFOs have played their last bytes.
    bool Busy() const;

    uint64_t Frames() const
    {
        return frames_;
    }

    // The output's rate in Hz: agb::kEnhancedRate in the enhanced mode, or else from the resolution that the driver
    // gives the sound bias register.
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

    // The model of the driver. Throws std::logic_error before a start. Driver().Records() holds what the tracks asked
    // of their channels in the last frame.
    const Output& Driver() const;

    const agb::Apu& Hardware() const
    {
        return apu_;
    }

    // The bytes that the last frame gave FIFO `fifo` (0 for A, 1 for B).
    const std::vector<int8_t>& FifoBytes(int fifo) const
    {
        return fifo_bytes_[size_t(fifo)];
    }

    // Leaves track `track`'s channel out of the output from the next frame on. The track and its channel go on as
    // before. Mutes aren't part of a snapshot, and stay through a restore. Throws std::out_of_range for a track past
    // kTracks.
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
    // samples: on the output's scale at the FIFO's volume and the PSG's. A voice's part leaves out the echo. 0 without
    // meters, after a frame without samples and for a muted track. A voice's points reach the meter in the frame that
    // plays them.
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

    // What Restore() takes to go back to a place in the song. A snapshot holds the driver's whole state, its echo lines
    // and the hardware's FIFOs: tens of kilobytes, or a few hundred with the enhanced mode's lines.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::optional<Output> output_;
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
    using TrackParts = std::array<std::array<float, 2>, kTracks>;

    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    uint16_t SilentTracks() const;
    void ApplyPsgMutes();
    void TakePeaks(const TrackParts& parts);

    const Rom& rom_;
    DriverInfo info_;
    std::optional<Output> output_;
    agb::Apu apu_;
    uint32_t store_cycle_ = 0;
    uint64_t frames_ = 0;
    int tail_ = 0;
    int tail_frames_ = 2;
    bool enhanced_ = false;
    std::vector<FifoTimerChange> changes_; // the timers' changes of the frame that the driver last mixed
    size_t next_change_ = 0;
    uint64_t mixed_cycles_ = 0; // the cycles since the driver last mixed
    std::array<std::vector<int8_t>, 2> fifo_bytes_;
    uint16_t muted_ = 0;
    uint16_t soloed_ = 0;
    bool metered_ = false;
    size_t meter_frames_ = 0;       // the frames between a voice's mixing and its playing
    std::vector<TrackParts> mixed_; // the voices' parts that the FIFOs have yet to play
    std::array<std::array<int32_t, 2>, kTracks> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::konami
