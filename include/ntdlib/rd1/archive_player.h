// SPDX-License-Identifier: MIT

// Plays a song of a GBA ROM with Nintendo R&D1's driver: the game's copy of the driver on the GBA's CPU (GameDriver),
// and its stores and bytes on the model of the GBA's sound hardware (agb::Apu), frame by frame.
//
// A frame runs the hardware up to the per-frame routine's cycle, runs the transfers of the FIFOs' DMA that the timer's
// samples have asked for, runs the routine and makes its stores, queues the bytes that the mixer wrote, and runs the
// hardware to the frame's end. The FIFOs start with the bytes that lie in the rings after the init: the lead of the
// mixer. A frame's mix therefore plays from about two frames later.
//
// Track t's controllers set channel t, and the driver plays its notes on channel t & 15. A mute of track t leaves out
// the voices that play channel t. Below track 16, a mute therefore leaves out the notes of track t + 16 too. From track
// 16 on, a mute leaves out no voice. The driver and its voices go on as before:
//
// - A muted sample voice adds nothing to the mix: the player sets its gains to 0 after the voices' routine has run and
//   puts them back after the mixer. The echo's input leaves the voice out too, and what it has put there rings on.
// - A muted PSG voice's channel leaves the output (agb::Apu::SetMuted). Its stores go on.
// - A note of a muted track still takes a voice from the note that it replaces.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. A model of the mixer mixes the voices' records in
// floating point before each frame's mix, by the rules in src/rd1/enhanced_mix.h: through wave::SincKernel, with the
// echo and the filter of CC72 at the mode's rate. The voices stay the game's. The PSG's channels go through the
// hardware model's band-limited steps (agb/apu.h). The console's mixer and FIFOs run on beside them. Busy() stays the
// console's. The mode has no anchor: no game ever ran it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/game_driver.h"

namespace ntdlib::rd1
{

class EnhancedMix;

// What the driver and the hardware start from, beyond the song.
struct ArchivePlayerOptions
{
    // The cycle of each frame at which the per-frame routine runs. It lies below agb::kFrameCycles.
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
    // What Restore() takes to go back to a place in the song: the system's memories and registers, and the hardware.
    // A snapshot shares the system's pages that hold the same bytes with the snapshot that it was saved after.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        agb::System::Snapshot system_;
        agb::Apu apu_;
        uint32_t store_cycle_ = 0;
        uint64_t frames_ = 0;
        uint64_t samples_ = 0;
        int fifo_bytes_ = 0;
        uint64_t queued_ = 0;
        std::optional<uint64_t> last_sound_;
        bool sounding_ = false;
        int tracks_ = 0;
        uint32_t channels_ = 0;
        uint32_t track_records_ = 0;
        bool audition_ = false;
        std::shared_ptr<const EnhancedMix> enhanced_;
    };

    // Keeps references to `rom` and `info`. Both must outlive the player. Throws std::invalid_argument when the
    // driver's code isn't laid out as FindGameRoutines() requires.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;
    ~ArchivePlayer();

    // Starts entry `song` of the song table from the state that the driver's init leaves. The game's call of the play
    // routine does the same. Throws std::invalid_argument when the entry holds no song, or an option is out of range.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Starts an audition: one note of program `program` of song `song`'s bank, at `key` and `velocity` (0 to 127), on
    // the song's player. The player plays a MIDI file of the note, on track 0. The note sounds until ReleaseNote() and
    // its release, or its sample's end. Throws std::invalid_argument when the entry holds no song, or a value or an
    // option is out of range.
    void StartNote(int song, uint8_t program, uint8_t key, uint8_t velocity, const ArchivePlayerOptions& options = {});

    // Plays the audition's note-off in the next frame. Throws std::logic_error before an audition.
    void ReleaseNote();

    // Runs one frame of the driver and of the hardware, and adds the frame's samples to `out` at Rate(), left then
    // right. A frame takes agb::kFrameCycles of the CPU's cycles. Throws std::logic_error before a start, and in the
    // enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // In the enhanced mode, runs one frame and adds its samples, left then right, with full scale at 1. Nothing limits
    // them. Throws std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs one frame without its samples, to reach a place in a song quickly. The player ends the frame in the state
    // that a frame with samples leaves. In the enhanced mode, the frame's samples are silent, and the echo's input is
    // silent too.
    void RunFrame();

    // Whether the song still sounds: until its tracks have ended, its voices and its PSG channels have fallen silent,
    // and the FIFOs have moved on from the last byte that wasn't 0. The echo's history is in the bytes.
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
        return enhanced_ != nullptr;
    }

    const DriverInfo& Info() const
    {
        return info_;
    }

    // The tracks of the song's player: the channels that its init sets it up with. 0 before a start.
    int Tracks() const
    {
        return tracks_;
    }

    // The driver on the CPU.
    GameDriver& Driver();

    const agb::Apu& Hardware() const
    {
        return apu_;
    }

    // Leaves track `track`'s voices out of the output from the next frame on. The track and its voices go on as before.
    // Mutes aren't part of a snapshot, and stay through a restore. Throws std::out_of_range for a track past
    // kMaxChannels.
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
    // samples, at the FIFO's volume. A sample voice's part is the largest sample that it adds to the mix at its gains.
    // In the console's mode, it leaves out the filter of CC72 and its gain. A voice's part leaves out the echo. A PSG
    // voice's part is its channel's (agb::Apu). 0 without meters, after a frame without samples and for a muted track.
    // A sample voice's part reaches the meter in the frame that plays it.
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

    // Takes a snapshot. Pass the last snapshot as `previous` to share its pages. Throws std::logic_error before a
    // start.
    Snapshot Save(const Snapshot* previous = nullptr) const;

    // Goes back to `snapshot`. The next frames give what they gave after the save. The last frame's meters go. Throws
    // std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    using TrackParts = std::array<std::array<int32_t, 2>, kMaxChannels>;

    // Goes back to the state after the init. Throws std::invalid_argument for an option out of range.
    void Begin(const ArchivePlayerOptions& options);

    // Sets the hardware and the player up after a start on player `player`.
    void Arm(int player, const ArchivePlayerOptions& options);

    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunTransfers();
    void Queue(const std::vector<int8_t>& a, const std::vector<int8_t>& b);
    uint32_t SilentTracks() const;

    // The track that the voice's record at `record` plays, or -1.
    int VoiceTrack(uint32_t record);

    void ApplyPsgMutes();
    void BeforeMix();
    TrackParts VoiceParts(size_t samples) const;
    TrackParts EnhancedParts() const;
    void TakePeaks(const TrackParts& parts);
    bool Sounding();

    const Rom& rom_;
    const DriverInfo& info_;
    GameRoutines routines_;
    GameDriver driver_;
    agb::System::Snapshot init_;
    std::vector<agb::RegisterWrite> init_writes_;
    agb::Apu apu_;
    bool started_ = false;
    uint32_t store_cycle_ = 0;
    uint32_t period_ = 0;
    uint64_t frames_ = 0;
    uint64_t samples_ = 0;               // the samples that the timer has taken from each FIFO
    int fifo_bytes_ = 0;                 // the bytes that the DMA has moved to each FIFO and the timer hasn't taken
    uint64_t queued_ = 0;                // the bytes queued in each FIFO since the start
    std::optional<uint64_t> last_sound_; // the place among them of the last byte that wasn't 0
    bool sounding_ = false;
    int tracks_ = 0;
    uint32_t channels_ = 0;      // the song's player's channel records
    uint32_t track_records_ = 0; // the song's player's track records
    bool audition_ = false;
    uint32_t muted_ = 0;
    uint32_t soloed_ = 0;
    bool metered_ = false;
    bool metering_ = false;                                         // whether the frame keeps peaks
    std::array<int, kGamePsgVoices> psg_tracks_ = {-1, -1, -1, -1}; // the track of each PSG voice
    std::vector<std::pair<uint32_t, uint16_t>> silenced_;           // the mixer records whose gains the frame set to 0
    std::vector<MixVoice> mixed_voices_;    // the frame's sample voices, a silenced one with gains of 0
    MixSettings settings_;                  // the mixer's settings in the frame's mix
    std::shared_ptr<EnhancedMix> enhanced_; // the enhanced mode's mixer, or nothing
    std::vector<TrackParts> mixed_;         // the voices' parts that the FIFOs have yet to play
    TrackParts peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::rd1
