// SPDX-License-Identifier: MIT

// Plays a song of a GBA ROM with AlphaDream's driver: the game's copy of the driver on the GBA's CPU (GameDriver), and
// its stores and the bytes of its buffers on the model of the GBA's sound hardware (agb::Apu), frame by frame.
//
// A frame starts with VBlank's swap of the halves. It runs the hardware up to the cycle of the per-frame routine and
// the mix, runs them and makes their stores, and runs the hardware to the frame's end. Before each run of the
// hardware, the FIFOs' DMA moves the bytes that the timer's samples have asked for: 16 bytes from each source in
// memory as it stands then, each time a FIFO's bytes fall to 16. The swap that marks the first half starts both DMAs
// again at the buffers' start. A DMA's transfer at the cycle of a call of the game's code comes before the call.
//
// The player starts in the steady state of the halves: the driver's init and one frame have run, and the next swap
// starts the DMAs again. The FIFOs then hold ArchivePlayerOptions::fifo_bytes bytes of 0. A frame's mix plays from
// that many samples after the next swap on.
//
// Track t of the song's player plays sample channel t below kSampleTracks, and PSG channel t - kSampleTracks from
// there. A mute leaves the track's channel out of the output. The driver and its channels go on as before:
//
// - A muted sample channel adds nothing to the mix: the player sets its volumes to 0 after the per-frame routine and
//   puts them back after the mix. Its envelope and its place go on.
// - A muted PSG channel leaves the output (agb::Apu::SetMuted). Its stores go on.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. A model of the mixer mixes the sample channels that
// each frame's mix played in floating point, by the rules in src/alphadream/enhanced_mix.h: through wave::SincKernel
// at the mode's rate, without the console's truncations and wrapping. The channels stay the game's. The PSG's
// channels go through the hardware model's band-limited steps (agb/apu.h). The console's mixer and FIFOs run on
// beside them. Busy() stays the console's. The mode has no anchor: no game ever ran it.

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
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/alphadream/game_driver.h"

namespace ntdlib::alphadream
{

class EnhancedMix;
struct MixChannel;

// The player that the games start their music on, and that the player plays songs on.
constexpr int kMusicPlayer = 1;

// What the driver and the hardware start from, beyond the song.
struct ArchivePlayerOptions
{
    // The cycle of each frame at which the per-frame routine and the mix run, after the swap at the frame's start. It
    // lies below agb::kFrameCycles.
    uint32_t store_cycle = agb::kFrameCycles / 16;

    // The bytes that each FIFO holds when the first swap starts the DMAs again: 17 to 32. The console's phase between
    // the DMA and the swaps depends on when the game started them.
    int fifo_bytes = 24;

    // The frame sequencer's phase at the song's start: see agb::Apu::SetSequencer.
    int sequencer_step = 0;
    uint32_t sequencer_cycles = agb::kSequencerCycles;

    // Plays in the enhanced mixing mode.
    bool enhanced = false;
};

class ArchivePlayer
{
public:
    // What Restore() takes to go back to a place in the song: the system's memories and registers, the hardware, and
    // the DMA's state. A snapshot shares the system's pages that hold the same bytes with the snapshot that it was
    // saved after.
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
        std::array<uint32_t, 2> sources_ = {};
        uint64_t queued_ = 0;
        std::optional<uint64_t> last_sound_;
        bool sounding_ = false;
        bool audition_ = false;
        uint32_t release_ = 0;
        std::vector<uint8_t> release_bytes_;
        std::shared_ptr<const EnhancedMix> enhanced_;
    };

    // Keeps references to `rom` and `info`. Both must outlive the player. Throws std::invalid_argument when the
    // driver's code isn't laid out as FindGameRoutines() requires, and std::runtime_error when the code fails on the
    // CPU.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);
    ArchivePlayer(const ArchivePlayer&) = delete;
    ArchivePlayer& operator=(const ArchivePlayer&) = delete;
    ~ArchivePlayer();

    // Starts song `song` on kMusicPlayer from the steady state after the init. The game's command 0 does the same.
    // Throws std::invalid_argument when the song table holds no song there, or an option is out of range.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Starts an audition: one note of instrument `instrument` at `key` and `velocity` (0 to 127) on track `track` of
    // kMusicPlayer. A sample track plays the instrument's samples, and a PSG track its PSG channel. The player plays a
    // song of the note at the track's volume of 255 * velocity / 127. The note sounds until ReleaseNote() and its
    // release, or its sample's end. Throws std::invalid_argument for an instrument outside the table, or a value or an
    // option out of range.
    void StartNote(int track, int instrument, uint8_t key, uint8_t velocity, const ArchivePlayerOptions& options = {});

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
    // that a frame with samples leaves. In the enhanced mode, the frame's samples are silent.
    void RunFrame();

    // Whether the song still sounds: until its tracks have ended, its sample channels and its PSG channels have
    // fallen silent, and the FIFOs have moved on from the last byte that wasn't 0.
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

    // The driver on the CPU.
    GameDriver& Driver();

    const agb::Apu& Hardware() const
    {
        return apu_;
    }

    // The bytes that the DMA has moved to each FIFO and the timer hasn't taken.
    int FifoBytes() const
    {
        return fifo_bytes_;
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
    // samples, at the FIFO's volume. A sample channel's part is the largest sample that it adds to the mix at its
    // volumes and its envelope's level: in the enhanced mode, the enhanced mix's largest sample. A PSG channel's part
    // is its channel's (agb::Apu). 0 without meters, after a frame without samples and for a muted track. A sample
    // channel's part reaches the meter in the frame that plays it.
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
    using TrackParts = std::array<std::array<int32_t, 2>, kTracks>;

    // Goes back to the steady state after the init. Throws std::invalid_argument for an option out of range.
    void Begin(const ArchivePlayerOptions& options);

    // Sets the hardware and the player up after a start.
    void Arm(const ArchivePlayerOptions& options);

    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    void RunTransfers(uint64_t until);
    void Queue(const std::array<std::vector<int8_t>, 2>& bytes);
    uint32_t SilentTracks() const;
    void ApplyPsgMutes();
    void BeforeMix();
    void AfterMix();
    TrackParts ChannelParts() const;
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
    uint64_t samples_ = 0;                 // the samples that the timer has taken from each FIFO
    int fifo_bytes_ = 0;                   // the bytes that the DMA has moved to each FIFO and the timer hasn't taken
    std::array<uint32_t, 2> sources_ = {}; // where each FIFO's DMA reads next
    uint64_t queued_ = 0;                  // the bytes queued in each FIFO since the start
    std::optional<uint64_t> last_sound_;   // the place among them of the last byte that wasn't 0
    bool sounding_ = false;
    bool audition_ = false;
    uint32_t release_ = 0;               // where ReleaseNote() writes the audition's last note
    std::vector<uint8_t> release_bytes_; // the bytes that it writes
    uint32_t muted_ = 0;
    uint32_t soloed_ = 0;
    bool metered_ = false;
    bool metering_ = false;                               // whether the frame keeps peaks
    std::vector<std::pair<uint32_t, uint16_t>> silenced_; // the channels' volumes that the frame set to 0
    std::array<std::array<uint8_t, kChannelBytes>, kSampleTracks> before_ = {}; // the channels' records before the mix
    std::vector<MixChannel> mixed_channels_; // the sample channels that the frame's mix played
    std::shared_ptr<EnhancedMix> enhanced_;  // the enhanced mode's mixer, or nothing
    std::vector<TrackParts> mixed_;          // the channels' parts that the FIFOs have yet to play
    TrackParts peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::alphadream
