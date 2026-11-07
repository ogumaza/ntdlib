// SPDX-License-Identifier: MIT

// Plays a song of a GBA ROM with Nintendo R&D2's driver on ntdlib's models: the driver's sequencer and mixer
// (rd2::Output) on the GBA's sound hardware (agb::Apu), frame by frame. The driver's stores go to the PSG's registers,
// and its bytes to the FIFOs.
//
// A frame runs the hardware up to the driver's routine, makes the routine's stores, queues the frame's mix in the
// FIFOs, and runs the hardware to the frame's end. On the console, timer 0 takes a byte from each FIFO every
// kFifoCycles. A frame's mix plays from the next VBlank on, once the FIFO's last 17 bytes have played. The FIFOs
// therefore start with kFifoLead bytes of silence.
//
// The driver's init sets the output's resolution in the sound bias register: 8 bits at 65,536 Hz. Rate() gives it.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. The model of the driver mixes its voices in the mode
// too, by the rules in rd2/output.h. A note that finds every voice busy takes a new voice. The PSG's channels go
// through the hardware model's band-limited steps (agb/apu.h), in as many units as the voices require. The console's
// mixer and FIFOs run on beside them. Busy() stays the console's: a voice past the console's counts only through its
// mix there. The mode has no anchor: no game ever ran it.
//
// A track plays any of the voices. A mute follows the player's track behind each voice's last note. Two things in a
// track reach further than its voices, and a mute leaves them as they are:
//
// - A track runs its commands and its stores while it's muted. A wave that it loads still reaches the track that plays
//   the wave channel next.
// - A voice's note takes a channel from the note that it replaces, whichever track played that note. A note of a muted
//   track still takes the channel, and the other track's note then falls silent.
//
// The echo's history follows the mute: a muted voice leaves the echo's input, and what it has put there rings on.

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
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/output.h"
#include "ntdlib/rd2/sequencer.h"

namespace ntdlib::rd2
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

    // The version of the song: an entry of the sequence's DriverInfo::music_settings.
    int version = 0;

    // The echo's level that the game asks for, up to 31. Without it, the level stays where the driver's init leaves it:
    // the echo stays off, and in the Super Mario Advance 2 revision it shifts everything away.
    std::optional<uint8_t> echo_goal;
};

class ArchivePlayer
{
public:
    // Keeps references to `rom` and `info`. Both must outlive the player.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);

    // Starts song `song` from the state that the game's init leaves. Throws std::invalid_argument when the song can't
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
    // samples starts the PSG's band-limited steps again at the channels' levels. The echo's input is silent there too.
    void RunFrame();

    // Whether the song still sounds: until its player has stopped, its voices, its PSG channels and the echo's history
    // have fallen silent, and the FIFOs have played their last bytes. An echo at a level of 0 keeps its history for
    // ever. So does the Super Mario Advance 2 revision's at a level of 1, whose rounding keeps a sum of 1.
    bool Busy() const;

    uint64_t Frames() const
    {
        return frames_;
    }

    // The output's rate in Hz: agb::kEnhancedRate in the enhanced mode, or else from the resolution in the sound bias
    // register.
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

    // Asks for the echo's level `level`, up to 31, as the game's request does. The level moves towards it by 1 a frame.
    // Throws std::logic_error before a start.
    void SetEchoGoal(uint8_t level);

    // Leaves track `track`'s voices out of the output from the next frame on. The track and its voices go on as before.
    // Mutes aren't part of a snapshot, and stay through a restore. Throws std::out_of_range for a track past
    // kPlayerTracks.
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

    // The last frame's largest part of track `track`'s voices on each side, left then right, on the scale of 16-bit
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

    // What Restore() takes to go back to a place in the song. A snapshot holds the driver's whole state, its echo's
    // history and the hardware's FIFOs: tens of kilobytes, or a few hundred with the enhanced mode's line.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::shared_ptr<const Output> output_;
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
    using TrackParts = std::array<std::array<float, 2>, kPlayerTracks>;

    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    uint16_t SilentTracks() const;
    void ApplyPsgMutes();
    void TakePeaks(const TrackParts& parts);

    const Rom& rom_;
    const DriverInfo& info_;
    std::optional<Output> output_;
    agb::Apu apu_;
    uint32_t store_cycle_ = 0;
    uint64_t frames_ = 0;
    int tail_ = 0;
    bool enhanced_ = false;
    uint16_t muted_ = 0;
    uint16_t soloed_ = 0;
    bool metered_ = false;
    std::vector<TrackParts> mixed_; // the voices' parts that the FIFOs have yet to play
    std::array<std::array<int32_t, 2>, kPlayerTracks> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::rd2
