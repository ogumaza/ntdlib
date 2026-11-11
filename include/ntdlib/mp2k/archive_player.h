// SPDX-License-Identifier: MIT

// Plays MP2K's songs on the model of the driver (mp2k::Sequencer) and on the model of the GBA's sound hardware
// (agb::Apu). Each frame, the driver's routine runs once. Its writes to the sound registers take effect at one cycle of
// the frame. Its mixer writes the part of the buffer that the sound DMA plays from the next frame on.
//
// The DMA moves the buffer's points into the FIFOs. GBATEK gives each FIFO 32 bytes, and has it ask the DMA for 16 more
// once it holds 16. A point therefore plays 17 to 32 of the timer's overflows after the DMA reads it, depending on the
// DMA's phase. The player keeps the FIFOs 24 points ahead of the timer.
//
// The SDK starts timer 0 so that a frame holds a whole number of its overflows: one for each of the mixer's points.
// Each frame therefore plays the points of one part of the buffer.
//
// In the enhanced mixing mode, the output runs at about 48,022 Hz in floats (agb::Apu's enhanced mode). The mixer adds
// each frame's channels into 804 samples (Sequencer::StartEnhancedMixing), and the player queues them into the
// hardware's float FIFOs. The FIFOs start with the silence of the console's FIFOs at the enhanced rate: a frame, and
// the 24 points ahead of the timer. The console's mixer and FIFOs run on beside them. Their buffer, the channels'
// progress and Busy() stay the console's. The driver grows its channels in the enhanced mode (Sequencer::GrowChannels):
// a note takes a new channel in place of a note's that hasn't reached its release, and no priority leaves a note out. A
// note in its release gives way to a new note, as on the console. The hardware's model plays the grown PSG channels in
// its grown units.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"

namespace ntdlib::mp2k
{

// The points that the player keeps in each FIFO ahead of the timer.
constexpr int kFifoLead = 24;

struct ArchivePlayerOptions
{
    // The cycle of each frame at which the driver's writes to the sound registers take effect, below
    // agb::kFrameCycles. A game calls the driver's routine from its vertical blank interrupt or from its main loop. The
    // writes come after the music players' work. The default lies a sixth of a frame in.
    uint32_t store_cycle = uint32_t(agb::kFrameCycles / 6);

    // The PSG envelopes' frame counter at the song's start (Sequencer::SetEnvelopeCounter).
    int envelope_counter = 0;

    // The frame sequencer's next step at the song's start, and the cycles until it (agb::Apu::SetSequencer). The
    // default follows a switch-on at the start.
    int sequencer_step = 0;
    uint32_t sequencer_cycles = agb::kSequencerCycles;

    // Whether the song plays in the enhanced mixing mode. Its frames then take RunFrame(std::vector<float>&).
    bool enhanced = false;
};

class ArchivePlayer
{
public:
    // A player of the songs of `rom`, whose driver `info` describes. The player keeps a copy of `info`. `rom` has to
    // outlive the player.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;

    // Starts song `song` as the game does after the driver's init. The hardware starts as the init leaves it: the
    // PSG's channels off, the FIFOs silent and timer 0 at the mixer's rate. Throws std::invalid_argument when the
    // song's header can't be read, its music player plays none of its tracks, or an option lies out of range.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Writes `value` to byte `index` of the memory that the song's memacc commands read and write (Sequencer::Memory),
    // before the next frame. A game's code writes it the same way. Throws std::out_of_range for an index past
    // kMemAccSize.
    void WriteMemory(int index, uint8_t value);

    // Runs a frame, and adds its samples to `out` at Rate(), left then right. Throws std::logic_error before a start,
    // and in the enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // Runs a frame of the enhanced mode, and adds its samples to `out` at Rate(), left then right, at a full scale of
    // 1. Throws std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs the frame in the same way, without the samples. The player ends the frame in the state that RunFrame(out)
    // leaves, but in the enhanced mode. There the mixer leaves the frame's enhanced samples silent, and the reverb's
    // history with them. The meters then give 0. A host runs such frames to reach a place in a song quickly. In the
    // enhanced mode, it then runs frames with samples before the place, for the reverb to fill again.
    void RunFrame();

    // Whether the song plays, or its sound has yet to end: a track that hasn't ended, a channel that sounds, a reverb
    // that still echoes, or points that the FIFOs haven't played. A reverb keeps the song busy until the whole buffer
    // holds 0 (Sequencer::MixerSilent): up to a pass of the buffer after its last sound.
    bool Busy() const;

    // The frames run since the start.
    uint64_t Frames() const
    {
        return frames_;
    }

    // The output's rate in Hz, from the resolution that the driver gives the sound bias register, or
    // agb::kEnhancedRate in the enhanced mode.
    int Rate() const
    {
        return enhanced_ ? agb::kEnhancedRate : apu_.Rate();
    }

    // Whether the song plays in the enhanced mixing mode.
    bool Enhanced() const
    {
        return enhanced_;
    }

    const DriverInfo& Info() const
    {
        return info_;
    }

    // The driver's model, and the hardware's. Throws std::logic_error before a start.
    const Sequencer& Driver() const;
    const agb::Apu& Hardware() const
    {
        return apu_;
    }

    // What the tracks and channels did in the last frame (Sequencer::Step).
    const std::vector<Action>& Actions() const
    {
        return actions_;
    }

    // Called with each note that a track starts on a channel (Action::kNoteOn), in the frame that starts it.
    std::function<void(const Action&)> on_note;

    // A host's mute of track `track` (0 to 15). The driver has no such mute. A muted track plays on as before, and
    // none of its channels reach the output, their releases included. A channel belongs to the track whose note last
    // started it (Sequencer::ChannelTrack). A change takes effect at the next frame. The mutes aren't part of a
    // snapshot. They stay through a restore.
    void MuteTrack(int track, bool mute);
    bool IsTrackMuted(int track) const
    {
        return (muted_ >> track & 1) != 0;
    }

    // A host's meters of the tracks, when `on`. The driver has no such meters. A track's meter adds up the points of
    // its DirectSound channels as the mixer adds them, and takes the largest part of one of its PSG channels. A
    // DirectSound channel's points reach the meter in the frame that plays them, a frame after the mixer's. The mixer's
    // reverb counts in no track's meter. TrackPeaks gives track `track`'s peaks of the last frame, left then right, on
    // the scale of the output's samples. Without meters, after a frame without samples, and for a muted track, it gives
    // 0. The meters change nothing in the output. They aren't part of a snapshot, and stay through a restore.
    void MeterTracks(bool on);
    std::array<int32_t, 2> TrackPeaks(int track) const
    {
        return peaks_[size_t(track)];
    }

    // The player's state between frames.
    class Snapshot
    {
    private:
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::optional<Sequencer> sequencer_;
        agb::Apu apu_;
        uint32_t store_cycle_ = 0;
        uint64_t frames_ = 0;
        int tail_ = 0;
        bool enhanced_ = false;
        std::vector<std::pair<int, uint8_t>> writes_;
    };

    // Takes a snapshot between frames, after a start. `previous` is null or an earlier snapshot of this player. The
    // snapshot shares nothing with it. Throws std::logic_error before a start.
    Snapshot Save(const Snapshot* previous = nullptr) const;

    // Returns the player to `snapshot`. Throws std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void ApplySettings();

    // Keeps the PSG's channels of muted tracks out of the output, those of the grown units too.
    void MutePsg();

    // Sets the tracks' peaks of the frame from the mixer's and the PSG's, or clears them without `metering`.
    void UpdatePeaks(bool metering);

    const Rom& rom_;
    const DriverInfo info_;
    std::optional<Sequencer> sequencer_;
    agb::Apu apu_;
    uint32_t store_cycle_ = 0;
    uint64_t frames_ = 0;
    int tail_ = 0; // frames left for the FIFOs' last points
    bool enhanced_ = false;
    std::vector<std::pair<int, uint8_t>> writes_; // the memory's writes before the next frame
    std::vector<Action> actions_;

    uint16_t muted_ = 0;
    bool metered_ = false;
    std::array<std::array<int32_t, 2>, kMaxTracks> mixed_ = {}; // the mixer's peaks of the frame that plays next
    std::array<std::array<int32_t, 2>, kMaxTracks> peaks_ = {};
};

} // namespace ntdlib::mp2k
