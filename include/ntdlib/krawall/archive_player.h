// SPDX-License-Identifier: MIT

// Plays a piece of a GBA ROM with Krawall on ntdlib's models: the player and its mixer (krawall::Player) on the GBA's
// sound hardware (agb::Apu), frame by frame. In stereo, FIFO A plays the left side's mix and FIFO B the right side's,
// each on its side alone. In mono, FIFO A plays the left side's mix on both sides. Timer 0 plays a byte of each at the
// mixer's rate (DriverInfo::mix_rate).
//
// The game mixes DriverInfo::frame_samples samples at a time, into a ring of buffers that the DMA plays. A frame mixes
// once while the DMA has caught up with the mix, as the game's worker does when getDmaAddress() hands it a buffer. The
// player queues each mix behind the ones before it, and mixes in a frame while the FIFOs hold less than two mixes. In
// the other frames, the worker returns at once, and the player runs no tick (Player::SkipFrame()). The FIFOs start with
// a mix of silence. The output's resolution is the sound bias register's default: 9 bits at 32,768 Hz. Rate() gives it.
//
// In the enhanced mixing mode (ArchivePlayerOptions::enhanced), the output comes in floating point at
// agb::kEnhancedRate. Each mixer channel's sample plays at that rate, interpolated between its points, at its volume,
// without the mix's limits: no wrap of the sums, no table, no 8 bits. The console's mixer runs on beside it. The mode
// has no anchor: no game ever ran it.
//
// The tracks are the module's channels. A track's mute leaves out the mixer channels that play its notes: their points
// and their part of the middle that the mixer takes off. The mix's sums wrap, so a mute changes the carries between
// the other channels' samples where the sums are that loud.

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
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"

namespace ntdlib::krawall
{

// The most tracks that the player mutes and meters: the module's channels.
constexpr int kPlayerTracks = kPlayerChannels;

// What the player and the hardware start from, beyond the piece.
struct ArchivePlayerOptions
{
    // Plays in the enhanced mixing mode.
    bool enhanced = false;
};

class ArchivePlayer
{
public:
    // Keeps references to `rom` and `info`. Both must outlive the player.
    ArchivePlayer(const Rom& rom, const DriverInfo& info);

    // Starts piece `song` of DriverInfo::songs as the game's call to krapPlay() does, at the music volume that the
    // game sets. Throws std::invalid_argument when the piece can't be played.
    void Start(int song, const ArchivePlayerOptions& options = {});

    // Starts an audition: one note on the first channel of piece `song`'s module, with `note` (from 1 for C-0) and
    // `instrument` (from 1), as Player::Audition() plays it. The module's rows don't play. The note sounds until
    // ReleaseNote(), its sample's end or its envelope's. Throws std::invalid_argument when the piece can't be played.
    void StartNote(int song, uint16_t instrument, uint8_t note, const ArchivePlayerOptions& options = {});

    // Plays a note off on the audition's channel from the next frame on. Throws std::logic_error before a start.
    void ReleaseNote();

    // Runs one frame of the player and of the hardware, and adds the frame's samples to `out` at Rate(), left then
    // right. A frame takes agb::kFrameCycles of the CPU's cycles. Throws std::logic_error before a start, and in the
    // enhanced mode.
    void RunFrame(std::vector<int16_t>& out);

    // In the enhanced mode, runs one frame and adds its samples, left then right, with full scale at 1. Nothing limits
    // them. Throws std::logic_error before a start, and outside the enhanced mode.
    void RunFrame(std::vector<float>& out);

    // Runs one frame without its samples, to reach a place in a piece quickly. It ends in the state that a frame with
    // samples leaves.
    void RunFrame();

    // Whether the piece still sounds: while the player plays its rows, while a mixer channel plays, and for the mixes
    // that the FIFOs hold after that. A module that loops plays for ever. An audition sounds while its note plays.
    bool Busy() const;

    uint64_t Frames() const
    {
        return frames_;
    }

    // The player's ticks so far, which count every pass through a loop.
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

    // The model of the player. Throws std::logic_error before a start.
    const Player& Driver() const;

    const agb::Apu& Hardware() const
    {
        return apu_;
    }

    // Leaves track `track`'s notes out of the output from the next mix on. The track and its notes go on as before.
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

    // The largest part of track `track`'s notes on each side, left then right, on the scale of 16-bit samples at the
    // FIFOs' full volume, in the mixes of the last frame that mixed. 0 without meters, after a frame without samples,
    // for a muted track and for a track past the module's channels.
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The last frame's largest output on each side, left then right, on the same scale. 0 without meters and after a
    // frame without samples.
    std::array<int32_t, 2> OutputPeaks() const
    {
        return output_peaks_;
    }

    // What Restore() takes to go back to a place in the piece: the player's whole state and the hardware's.
    class Snapshot
    {
        friend class ArchivePlayer;

        const ArchivePlayer* owner_ = nullptr;
        std::shared_ptr<const Player> player_;
        agb::Apu apu_;
        uint64_t frames_ = 0;
        int quiet_mixes_ = 0;
        bool enhanced_ = false;
    };

    // Takes a snapshot. `previous` is there for the interface that ntdlib's archive players share: a snapshot stands
    // alone. Throws std::logic_error before a start.
    Snapshot Save(const Snapshot* previous = nullptr) const;

    // Goes back to `snapshot`. The next frames give what they gave after the save. The last frame's meters go. Throws
    // std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    // Makes the player for piece `song`, or throws std::invalid_argument.
    Player Load(int song) const;

    // Sets up the hardware and starts `player`.
    void Begin(Player player, const ArchivePlayerOptions& options);

    void Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out);
    uint32_t SilentTracks() const;

    const Rom& rom_;
    const DriverInfo& info_;
    std::optional<Player> player_;
    agb::Apu apu_;
    uint64_t frames_ = 0;
    int quiet_mixes_ = 0; // the mixes in a row in which neither the player nor a mixer channel played
    bool enhanced_ = false;
    uint32_t muted_ = 0;
    uint32_t soloed_ = 0;
    bool metered_ = false;
    std::array<std::array<int32_t, 2>, kPlayerTracks> peaks_ = {};
    std::array<int32_t, 2> output_peaks_ = {};
};

} // namespace ntdlib::krawall
