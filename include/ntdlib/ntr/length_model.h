// SPDX-License-Identifier: MIT

// The length analysis of a sequence of a DS sound archive (SDAT) on the model of the DS's sound driver: xsflib's
// analysis (xsflib/rip/length.h) with a model that counts each track's passes. nwsnd/length_model.h does the same for
// the later consoles. The driver follows the loops by nwsnd's rules (snd::Driver::FollowLengths). The model is an
// ArchivePlayer, whose frames last snd::kFrameClocks system clocks, about 5.21 ms. A frame's commands sound two frames
// later: the player plays a frame's samples before the driver's frame, and a channel starts in the frame after the one
// that sets it up. The DS's sound unit has no effects, and nothing rings.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "xsflib/rip/length.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::ntr
{

// A frame of the driver in milliseconds.
constexpr double kFrameMs = 1000.0 * snd::kFrameClocks / kSystemClockHz;

// The analysis's rules are those of the later consoles: at most 600 s, and a sequence that stays busy without a sound
// for 180 s has ended. A frame's commands, a loop's point and a steady sequence's point sound two frames on.
xsflib::rip::LengthRules LengthRules();

// The model that xsflib's length analysis (xsflib::rip::MeasureLength) reads: an ArchivePlayer that plays a sequence on
// the driver's player `sequence_player`. The model also counts each track's passes. A looping sequence's fade then
// waits for the tracks that loop at other lengths than the main track (OthersLooped). An open of a track that a track
// opened before (0x93) is a pass of the opened track: the track starts its part again. The first open isn't a pass. The
// open keeps the track's wait. The pass therefore counts at the track's next command. Two opens during one wait then
// make one pass.
class LengthModel
{
public:
    // The counted loops in `endless_loops` loop for ever (snd::Driver::FollowLengths).
    LengthModel(ArchivePlayer& player, int sequence_player = 0, std::set<snd::LoopPlace> endless_loops = {});

    bool RunFrame();

    // The frames run so far.
    uint64_t Frames() const;

    bool Sounded() const;
    bool Ringing() const;
    std::size_t VoiceCount() const;
    xsflib::rip::VoicePlace Voice(std::size_t index) const;
    bool Active() const;
    bool Settled() const;
    bool Stuck() const;

    // The driver has no ramps of a track's volume, pan or pitch.
    bool Moving() const;

    uint64_t ChangedAt() const;
    bool Busy() const;
    bool ChannelsChanging() const;
    int ActiveChannels() const;

    // Sets the driver's hooks: `hook` for the sequence's loops, a count of every track's passes, and of each open of a
    // track. Only the passes that changed the sound count.
    template <typename Hook>
    void OnLoop(Hook hook)
    {
        snd::LengthHooks hooks;
        hooks.on_loop = [this, on_loop = std::move(hook)](int player, int track, uint32_t target) mutable
        {
            if (player == sequence_player_)
            {
                on_loop(track, target);
            }
        };
        hooks.on_track_loop = [this](int player, int track, uint32_t)
        {
            if (player == sequence_player_ && !player_.Driver().IsTrackIdle(player, track))
            {
                ++passes_[static_cast<std::size_t>(track)];
            }
        };
        hooks.on_open_track = [this](int player, int, int opened)
        {
            if (player != sequence_player_)
            {
                return;
            }

            const auto index = static_cast<std::size_t>(opened);
            reopened_[index] = opened_[index];
            opened_[index] = true;
        };
        hooks.on_command_start = [this](int player, int track)
        {
            const auto index = static_cast<std::size_t>(track);
            if (player == sequence_player_ && reopened_[index])
            {
                ++passes_[index];
                reopened_[index] = false;
            }
        };
        player_.FollowLengths(std::move(hooks), endless_loops_);
    }

    // Whether every open track other than `track` has made kLengthLoops passes that changed the sound. A track whose
    // last pass changed nothing, or that waits for ever, doesn't hold the fade back.
    bool OthersLooped(int track) const;

private:
    ArchivePlayer& player_;
    int sequence_player_;
    std::set<snd::LoopPlace> endless_loops_;
    uint64_t frames_ = 0;
    std::vector<SoundOutput> samples_;
    std::vector<int16_t> output_;
    std::array<int, snd::kTracksPerPlayer> passes_{};    // each track's passes that changed the sound
    std::array<bool, snd::kTracksPerPlayer> opened_{};   // whether another track has opened each track
    std::array<bool, snd::kTracksPerPlayer> reopened_{}; // whether each track's next command starts its part again
};

// Measures `sequence` of `archive`, started with `settings` on the driver's player 0 of a driver of `build`. The
// counted loops in `endless_loops` loop for ever. A sequence can repeat its music in a counted loop that the game plays
// for longer than the limit. When the measure reaches the limit inside counted loops, it runs again on a new player
// with each track's outermost counted loop endless (snd::Driver::OutermostCountedLoops). Their passes then count as
// loops of the sequence. The start's exceptions pass through (ArchivePlayer::Start): xsflib::rip::TryMeasure turns them
// into nothing.
std::optional<xsflib::rip::Timing> MeasureSequence(const sdat::Archive& archive, const SequenceRef& sequence,
                                                   const GameSettings& settings = {}, const snd::Build& build = {},
                                                   const std::set<snd::LoopPlace>& endless_loops = {});

} // namespace ntdlib::ntr
