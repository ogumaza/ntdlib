// SPDX-License-Identifier: MIT

// The length analysis of a song or a sound effect of DSE on dse::Player: xsflib's analysis (xsflib/rip/length.h) with
// a model of the sequence that the player's driver follows. A frame is one of DSE's ticks, about 10 ms. A tick's
// commands sound in the next tick's samples: the ARM7's driver runs them in its next frame, and a channel starts in the
// frame after that. The DS's sound unit has no effects, and nothing rings.
//
// A track of DSE loops when its end (0x98) goes back to its loop's start (0x99). The analysis counts the loops of the
// first track that loops and plays. The other tracks that loop hold the fade back until each of them has looped
// kLengthLoops times too (OthersLooped). No wait of the driver's tracks lasts for ever. A track's end without a loop
// stops the voices of its channel. A sequence is therefore settled, and stuck, only once none of its tracks plays, and
// nothing of it sounds a few ticks later. The sound changes when a note starts, when a channel's settings change, and
// while a voice's envelope moves.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "xsflib/rip/length.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::dse
{

// One of DSE's ticks in milliseconds.
constexpr double kTickMs = 1000.0 * Player::kTickClocks / ntr::kSystemClockHz;

// The analysis's rules are those of the later consoles: at most 600 s, and a sequence that stays busy without a sound
// for 180 s has ended. A tick's commands, a loop's point and a steady sequence's point sound a tick on.
xsflib::rip::LengthRules LengthRules();

// The model that xsflib's length analysis (xsflib::rip::MeasureLength) reads: the sequence that `player`'s driver
// follows (Driver::Follow). The model sets the driver's hooks. It also counts each track's loops.
class LengthModel
{
public:
    explicit LengthModel(Player& player);

    bool RunFrame();

    // The frames run so far.
    uint64_t Frames() const;

    bool Sounded() const;
    bool Ringing() const;
    std::size_t VoiceCount() const;
    xsflib::rip::VoicePlace Voice(std::size_t index) const;

    // The sequence plays, or a voice of its channels does.
    bool Active() const;

    // None of the sequence's tracks plays.
    bool Settled() const;
    bool Stuck() const;

    // The fades and slides change the channels' settings: ChangedAt counts them.
    bool Moving() const;

    uint64_t ChangedAt() const;
    bool Busy() const;
    bool ChannelsChanging() const;
    int ActiveChannels() const;

    // Sets the driver's hooks: `hook` for the loops of the first track that loops and plays, with a target of 0, and a
    // count of every track's loops and of the notes.
    template <typename Hook>
    void OnLoop(Hook hook)
    {
        SequenceHooks hooks;
        hooks.on_note = [this](const NoteEvent&)
        {
            ++changes_;
        };
        hooks.on_loop_start = [this](int track)
        {
            Follow(track).loops = true;
        };
        hooks.on_loop = [this, on_loop = std::move(hook)](int track) mutable
        {
            ++Follow(track).passes;
            if (track == MainTrack())
            {
                on_loop(track, 0u);
            }
        };
        player_.Driver().SetHooks(std::move(hooks));
    }

    // Whether every track other than `track` that loops and plays has looped kLengthLoops times.
    bool OthersLooped(int track) const;

private:
    // What the model follows of a track.
    struct TrackFollow
    {
        bool loops = false; // the track has passed its loop's start
        int passes = 0;     // the track's loops
    };

    TrackFollow& Follow(int track);

    // The first track that has passed its loop's start and plays, or -1.
    int MainTrack() const;

    Player& player_;
    uint64_t frames_ = 0;
    uint64_t changes_ = 0; // the notes and the changes of the channels' settings so far
    bool envelopes_moved_ = false;
    std::vector<ntr::SoundOutput> samples_;
    std::vector<int16_t> output_;
    std::vector<ChannelView> channels_;
    std::array<VoiceView, Driver::kVoices> voices_{};
    std::vector<TrackFollow> tracks_;
};

// Measures the song or the sound effect of `files` on a player of `build` (Player::Start). Nothing when DSE refuses the
// sound effect. The start's exceptions pass through: xsflib::rip::TryMeasure turns them into nothing. So does a Hang
// when a track hangs the driver.
std::optional<xsflib::rip::Timing> MeasureSequence(const SequenceFiles& files, Build build = Build::kLater);

} // namespace ntdlib::dse
