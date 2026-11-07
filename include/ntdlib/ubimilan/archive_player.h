// SPDX-License-Identifier: MIT

// Plays a piece of music of a GBA ROM with Ubisoft Milan's driver on ntdlib's models: the driver and its mixer
// (ubimilan::Output) on the GBA's sound hardware (agb::Apu), frame by frame. The driver's stores go to the PSG's
// registers, and its bytes to both FIFOs.
//
// A frame runs the hardware up to the driver's routines, makes their stores, queues what the frame's runs of the mixer
// give the FIFOs, and runs the hardware to the frame's end. The driver's mixer works by frames here, not by the
// interrupts that run it on the console. The bytes that a frame's stores change therefore wait in the FIFOs for the
// time that they wait on the console. On the console, the FIFOs' interrupt runs the mixer in the block after the
// stores. Its bytes go through a second buffer, and join the FIFO's other 16 bytes: 3 blocks of 16 points in all
// (kFifoLeadPoints).
//
// The driver sets the output's rate through the sound bias register: 65,536 Hz. Rate() gives it. FIFO A plays on the
// right, and FIFO B on the left. Both take the same bytes: the music is mono.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. The model of the driver mixes its voices in the mode
// too, by the rules in ubimilan/output.h. The PSG's channels go through the hardware model's band-limited steps
// (agb/apu.h). The console's mixer and FIFOs run on beside them. The mode has no anchor.
//
// A track is one of the sequence's 16 channels. Tracks 0 to 2 play the PSG's square 1, square 2 and wave channels.
// Track 9 plays the kit's samples on the mixer's voices, and the PSG's noise. The other tracks play nothing. Some
// things in a track reach further than its channel, and a mute leaves them as they are:
//
// - The sequence runs its commands and stores while a track is muted. A wave that a program loads still reaches the
//   wave channel, and a voice that a kit note takes stays taken.
// - The mixer's bytes clip after the voices' sum. A mute takes the voices' points out of the sum, and any clipping that
//   they caused.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/output.h"

namespace ntdlib::ubimilan
{

// The tracks: the sequence's channels.
inline constexpr int kTracks = 16;

// The points of output that a store's bytes wait for in the FIFOs: 3 blocks of 16.
inline constexpr uint32_t kFifoLeadPoints = 48;

// What the driver and the hardware start from, beyond the song.
struct ArchivePlayerOptions
{
    // The cycle of each frame at which the driver's routines make their stores. It lies below agb::kFrameCycles.
    uint32_t store_cycle = agb::kFrameCycles / 6;

    // The frame sequencer's phase at the song's start: see agb::Apu::SetSequencer.
    int sequencer_step = 0;
    uint32_t sequencer_cycles = agb::kSequencerCycles;

    // Plays in the enhanced mixing mode.
    bool enhanced = false;
};

class ArchivePlayer
{
public:
    // Keeps a reference to `rom` and a copy of `info`. `rom` must outlive the player. The player's parts refer to the
    // copy. The player therefore can't be copied.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;

    // Starts song `song` from the state that the game's start leaves. Throws std::invalid_argument when the song can't
    // be played or an option lies out of range.
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
    // samples starts the PSG's band-limited steps again at the channels' levels.
    void RunFrame();

    // Whether the song still sounds: until its track has ended, its voices have fallen silent, and the FIFOs have
    // played their last bytes. A PSG note that the song leaves sounding doesn't count. A piece on which the game resets
    // ends there.
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

    // The model of the driver. Throws std::logic_error before a start.
    const Output& Driver() const;

    const agb::Apu& Hardware() const
    {
        return apu_;
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

    // The last frame's largest part of track `track`'s channels on each side, left then right, on the scale of 16-bit
    // samples: on the output's scale at the FIFOs' volume and the PSG's. 0 without meters and after a frame without
    // samples. The FIFOs' lead lasts less than a frame, and a frame's samples hold points of the frame before. Track
    // 9's peak therefore takes in the points that both frames mixed, and falls to 0 a frame after a mute.
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

    // What Restore() takes to go back to a place in the song. A snapshot holds the driver's whole state and the
    // hardware's FIFOs: a few kilobytes.
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
    void TakePeaks();

    const Rom& rom_;
    DriverInfo info_;
    std::optional<Output> output_;
    agb::Apu apu_;
    uint32_t store_cycle_ = 0;
    uint64_t frames_ = 0;
    int tail_ = 0;
    bool enhanced_ = false;
    uint16_t muted_ = 0;
    uint16_t soloed_ = 0;
    bool metered_ = false;
    float last_voices_peak_ = 0.0f; // what the frame before mixed
    std::array<std::array<int32_t, 2>, kTracks> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::ubimilan
