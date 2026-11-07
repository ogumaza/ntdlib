// SPDX-License-Identifier: MIT

// Plays a tune of a GBA ROM with Rare's driver on ntdlib's models: the driver's sequencer and mixer (rare::Sequencer)
// on the GBA's sound hardware (agb::Apu), frame by frame. The driver uses only FIFO A, on both sides at full volume,
// and no PSG channel.
//
// On the console, the driver mixes a frame's samples into one of two buffers. At the next VBlank, the game's interrupt
// swaps the buffers and points DMA 1 at the new mix, which timer 0 then plays to FIFO A, a byte every
// agb::kFrameCycles / DriverInfo::samples_per_frame cycles. A frame here runs the driver, queues its mix behind the
// mix of the frame before, and runs the hardware for the frame. Each mix therefore plays whole in the next frame, and
// the FIFO starts with a frame of silence. On the console, the FIFO still holds the bytes that the DMA has read ahead
// when the interrupt points it at the new mix. The player leaves that out, as it leaves out the byte of 0x80 that the
// driver's init stores in each FIFO before the sound is on.
//
// The driver's init sets the output's resolution in the sound bias register: 8 bits at 65,536 Hz. Rate() gives it.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate: agb::kEnhancedFrameSamples samples a frame. The same voices play as in the console's mode, by the
// rules of Sequencer::MixEnhanced(). The console's mixer runs on beside them. The mode has no anchor: no game ever ran
// it.
//
// A track's mute leaves out the voices of the notes that the track started. The voices still count towards the mixer's
// limit of DriverInfo::voice_limit, and a muted track's note still takes a slot from a note of another track. The
// driver's mix wraps in 16 bits and clips to 8. A mute takes the voice's parts out of the sum before both.

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
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/sequencer.h"

namespace ntdlib::rare
{

// The most tracks that the player mutes and meters.
constexpr int kPlayerTracks = 32;

// What the driver and the hardware start from, beyond the tune.
struct ArchivePlayerOptions
{
    // Plays in the enhanced mixing mode.
    bool enhanced = false;

    // For the tune that the game plays as two arrangements (DriverInfo::water): plays the arrangement under water.
    // Other tunes ignore it.
    bool under_water = false;
};

class ArchivePlayer
{
public:
    // Keeps references to `rom` and `info`. Both must outlive the player.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);

    // Starts tune `tune` from the state that the driver's init leaves, at the music volume at which the game starts it.
    // Throws std::invalid_argument when the tune can't be played.
    void Start(int tune, const ArchivePlayerOptions& options = {});

    // Starts an audition: one note of program `program` of tune `tune` at `key` and `velocity` (0 to 127), as
    // Sequencer::Audition() plays it. The tune's tracks stay idle. The note sounds until ReleaseNote() and its
    // release, or its sample's end. Throws std::invalid_argument when the tune can't be played.
    void StartNote(int tune, uint8_t program, uint8_t key, uint8_t velocity, const ArchivePlayerOptions& options = {});

    // Releases the audition's note from the next frame on. Throws std::logic_error before a start.
    void ReleaseNote();

    // Runs one frame of the driver and of the hardware, and adds the frame's samples to `out` at Rate(), left then
    // right. A frame takes agb::kFrameCycles of the CPU's cycles. Throws std::logic_error before a start, and in the
    // enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // In the enhanced mode, runs one frame and adds its samples, left then right, with full scale at 1. Nothing limits
    // them. Throws std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs one frame without its samples, to reach a place in a tune quickly. The console's mode ends in the state that
    // a frame with samples leaves. In the enhanced mode, the frame's samples are silent.
    void RunFrame();

    // Whether the tune still sounds: until its tracks have ended, its voices and its mix have fallen silent, and the
    // FIFO has played their last bytes. Once the tracks have ended, the driver stops the envelopes and goes on mixing.
    // A note that holds a looping sample then sounds for ever, and so does the tune. At a rate whose part of the buffer
    // the driver doesn't clear, the rest of the last mix plays again in each frame, for ever. An audition sounds until
    // its note and the mix have fallen silent.
    bool Busy() const;

    uint64_t Frames() const
    {
        return frames_;
    }

    // The tune's tick: the largest tick count of its tracks, which counts every pass through a loop.
    uint64_t Tick() const;

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
    const Sequencer& Driver() const;

    const agb::Apu& Hardware() const
    {
        return apu_;
    }

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
    // samples: on the output's scale at the FIFO's volume. 0 without meters, after a frame without samples, for a muted
    // track and for a track past the tune's. A voice's points reach the meter in the frame that plays them.
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The last frame's largest output on each side, left then right, on the same scale. 0 without meters and after a
    // frame without samples.
    std::array<int32_t, 2> OutputPeaks() const
    {
        return output_peaks_;
    }

    // What Restore() takes to go back to a place in the tune. A snapshot holds the driver's whole state and the
    // hardware's FIFOs: a few kilobytes.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::shared_ptr<const Sequencer> sequencer_;
        agb::Apu apu_;
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
    using TrackParts = std::vector<int32_t>;

    // Sets up the sequencer and the hardware for a start.
    void Begin(Sequencer sequencer, const ArchivePlayerOptions& options);
    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    uint32_t SilentTracks() const;
    bool Sounding() const;
    void TakePeaks(const TrackParts& parts);

    const Rom& rom_;
    const DriverInfo& info_;
    std::optional<Sequencer> sequencer_;
    agb::Apu apu_;
    uint64_t frames_ = 0;
    int tail_ = 0;
    bool enhanced_ = false;
    uint32_t muted_ = 0;
    uint32_t soloed_ = 0;
    bool metered_ = false;
    std::optional<TrackParts> mixed_; // the voices' parts of the mix that the FIFO plays next
    std::array<std::array<int32_t, 2>, kPlayerTracks> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::rare
