// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "driver_shared.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr::snd
{

void Driver::FollowLengths(LengthHooks hooks, std::set<LoopPlace> endless_loops)
{
    hooks_ = std::move(hooks);
    endless_loops_ = std::move(endless_loops);
    following_ = static_cast<bool>(hooks_.on_loop);
}

bool Driver::IsSettled(int player) const
{
    const Player& state = players_[static_cast<std::size_t>(player)];
    const auto settled = [this](int track)
    {
        return WaitsForNotes(track) || (IsStill(track) && !HasEndingNote(track));
    };
    return TempoStep(state.tempo, state.tempo_ratio) == 0 || EveryOpenTrack(player, settled);
}

bool Driver::IsHolding(int player) const
{
    const Player& state = players_[static_cast<std::size_t>(player)];
    const auto holding = [this](int track)
    {
        return WaitsForEver(track) || (IsStill(track) && !HasEndingNote(track));
    };
    return TempoStep(state.tempo, state.tempo_ratio) == 0 || EveryOpenTrack(player, holding);
}

bool Driver::IsStuck(int player) const
{
    const Player& state = players_[static_cast<std::size_t>(player)];
    const auto stuck = [this](int track)
    {
        return WaitsForEver(track) || (IsStill(track) && track_follows_[static_cast<std::size_t>(track)].quiet);
    };
    return TempoStep(state.tempo, state.tempo_ratio) == 0 || EveryOpenTrack(player, stuck);
}

uint64_t Driver::ChangedAt(int player) const
{
    return player_follows_[static_cast<std::size_t>(player)].changed_at;
}

bool Driver::IsTrackOpen(int player, int track) const
{
    const Track* open = TrackOf(player, track);
    return open && open->position != 0;
}

bool Driver::IsTrackIdle(int player, int track) const
{
    const Track* open = TrackOf(player, track);
    return open && IsIdle(static_cast<int>(open - tracks_.data()));
}

bool Driver::WaitsForEver(int player, int track) const
{
    const Track* open = TrackOf(player, track);
    return open && WaitsForEver(static_cast<int>(open - tracks_.data()));
}

std::set<LoopPlace> Driver::OutermostCountedLoops(int player) const
{
    std::set<LoopPlace> loops;
    for (int number = 0; number < kTracksPerPlayer; number++)
    {
        if (!IsTrackOpen(player, number))
        {
            continue;
        }

        const Track& track = *TrackOf(player, number);
        const TrackFollow& follow = track_follows_[static_cast<std::size_t>(&track - tracks_.data())];
        for (int depth = 0; depth < track.call_depth; depth++)
        {
            if (follow.loops[static_cast<std::size_t>(depth)])
            {
                if (track.loop_counts[static_cast<std::size_t>(depth)] != 0)
                {
                    loops.insert({player, number, track.calls[static_cast<std::size_t>(depth)] - track.base});
                }
                break;
            }
        }
    }

    return loops;
}

bool Driver::AnyChannelChanging() const
{
    return std::ranges::any_of(
        channels_, [](const Channel& channel) { return channel.active && channel.envelope_stage != kSustain; });
}

int Driver::ActiveChannels() const
{
    return static_cast<int>(std::ranges::count_if(channels_, [](const Channel& channel) { return channel.active; }));
}

Driver::SoundState Driver::StateOf(const Track& track, const Player& player) const
{
    return {
        .program = track.program,
        .transpose = track.transpose,
        .bend_range = track.bend_range,
        .priority = track.priority,
        .portamento_key = track.portamento_key,
        .portamento_time = track.portamento_time,
        .envelope = {track.attack, track.decay, track.sustain, track.release},
        .sweep_pitch = track.sweep_pitch,
        .modulation = track.modulation,
        .muted = track.muted,
        .tie = track.tie,
        .portamento = track.portamento,
        .note_wait = track.note_wait,
        .volume = track.volume,
        .expression = track.expression,
        .pitch_bend = track.pitch_bend,
        .pan = track.pan,
        .player_volume = player.volume,
        .tempo = player.tempo,
        .channels = track.channels,
    };
}

// While the driver follows lengths: the command's place in the order that the player's tracks run them, and whether it
// changes the track's sound.
int Driver::FollowCommand(int index, int number, Player& player, bool play_notes)
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const uint64_t now = ++player_follows_[player.id].commands_run;
    follow.run[track.position - track.base] = now;
    follow.held_wait = false;
    if (hooks_.on_command_start)
    {
        hooks_.on_command_start(player.id, number);
    }

    const SoundState before = StateOf(track, player);
    const int result = RunCommand(index, number, player, play_notes);
    if (StateOf(track, player) != before)
    {
        MarkChange(index, player);
    }

    return result;
}

void Driver::MarkChange(int track, const Player& player)
{
    PlayerFollow& follow = player_follows_[player.id];
    track_follows_[static_cast<std::size_t>(track)].changed_at = follow.commands_run;
    follow.changed_at = follow.commands_run;
}

void Driver::MarkRead(const Player& player, int variable)
{
    if (variable >= 0 && variable < 32)
    {
        player_follows_[player.id].variables_read |= 1u << variable;
    }
}

// A pass of a loop of track `number` back to `target`. The pass began when the track last ran `target`. A still pass
// also left the variables that the tracks read with the values that the pass before left. The first pass compares
// them with 0. The pass played a note if the track has played one since the pass began.
void Driver::CountLoop(int index, int number, const Player& player, uint32_t target)
{
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const PlayerFollow& player_follow = player_follows_[player.id];
    const auto ran = follow.run.find(target);
    const uint64_t begun = ran != follow.run.end() ? ran->second : 0;
    std::array<int16_t, 32> variables{};
    for (int i = 0; i < static_cast<int>(variables.size()); i++)
    {
        const uint32_t address = (player_follow.variables_read >> i & 1) != 0 ? VariableAddress(player, i) : 0;
        variables[static_cast<std::size_t>(i)] = address != 0 ? static_cast<int16_t>(Read16(address)) : 0;
    }
    follow.idle = follow.changed_at < begun;
    follow.still = follow.idle && follow.drew_at < begun && variables == follow.loop_variables;
    follow.loop_variables = variables;
    follow.looped_at = player_follow.commands_run;
    follow.polled_at = 0;

    const bool noted = follow.noted_at != 0 && follow.noted_at >= begun;
    const bool alone = HoldsSustainedNote(index) && OthersRest(player, number);
    if (!follow.idle && (number == 0 || (noted && IsMainTrack(player, number)) || alone))
    {
        hooks_.on_loop(player.id, number, target);
    }

    follow.quiet = !noted;
    if (hooks_.on_track_loop)
    {
        hooks_.on_track_loop(player.id, number, target);
    }
}

// A jump of track `index` back to `target` that isn't a loop: a pass of a poll inside a subroutine (`next` is the
// offset after the jump). The pass began when the track last ran `target`. It's idle when it changed nothing in the
// sound, changed no variable and drew no random number. The track then does the same on every pass until another track
// or the game changes a variable. A track that polls the game's variables once a tick, while later tracks play the
// music, makes such passes. A countdown between a track's notes changes its counter on each pass. The pass is never a
// loop. nw::snd's model has the same rule (nwsnd::SequenceTrack::CountPoll).
void Driver::CountPoll(int index, const Player& player, uint32_t target, uint32_t next)
{
    TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const auto ran = follow.run.find(target);
    if (target >= next || ran == follow.run.end())
    {
        return;
    }

    // After a pass that isn't idle, IsIdle follows the track's loops again.
    const uint64_t begun = ran->second;
    const bool idle = follow.changed_at < begun && follow.set_at < begun && follow.drew_at < begun;
    follow.polled_at = idle ? player_follows_[player.id].commands_run : 0;
}

// A jump back to a command that the track has run is a loop, outside a subroutine. Inside a subroutine, the jump is a
// loop only when it's unconditional and the track holds a note that sounds until the game stops it, or the track has
// run no conditional command since the jump's target.
bool Driver::FollowsJumpAsLoop(int index, uint32_t target, uint32_t next, bool conditional) const
{
    const TrackFollow& follow = track_follows_[static_cast<std::size_t>(index)];
    const auto ran = follow.run.find(target);
    if (target >= next || ran == follow.run.end())
    {
        return false;
    }

    const Track& track = tracks_[static_cast<std::size_t>(index)];
    bool in_call = false;
    for (int depth = 0; depth < track.call_depth; depth++)
    {
        in_call = in_call || !follow.loops[static_cast<std::size_t>(depth)];
    }
    const bool unconditioned = follow.conditional_at < ran->second;
    return !in_call || (!conditional && (HoldsSustainedNote(index) || unconditioned));
}

bool Driver::LoopsForEver(const Player& player, int number, uint32_t offset) const
{
    return following_ && endless_loops_.contains({player.id, number, offset});
}

const Track* Driver::TrackOf(int player, int number) const
{
    const uint8_t track = players_[static_cast<std::size_t>(player)].tracks[static_cast<std::size_t>(number)];
    return track == kNoTrack ? nullptr : &tracks_[track];
}

bool Driver::IsIdle(int track) const
{
    // A track that polls in a subroutine is idle too after an idle pass of its poll (CountPoll).
    const TrackFollow& follow = track_follows_[static_cast<std::size_t>(track)];
    return (follow.idle && follow.changed_at < follow.looped_at) ||
           (follow.polled_at != 0 && follow.changed_at < follow.polled_at);
}

bool Driver::IsStill(int track) const
{
    // A poll's passes don't count.
    const TrackFollow& follow = track_follows_[static_cast<std::size_t>(track)];
    return follow.idle && follow.changed_at < follow.looped_at && follow.still && follow.drew_at < follow.looped_at;
}

// A note or wait whose length comes from a variable left at -1 makes a negative wait.
bool Driver::WaitsForEver(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    if (track.wait < 0 || track_follows_[static_cast<std::size_t>(index)].held_wait)
    {
        return true;
    }
    if (!track.note_finish_wait)
    {
        return false;
    }

    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(at)];
        if (channel.length < 0 && ChannelWaveLoops(channel))
        {
            return true;
        }
    }

    return false;
}

bool Driver::WaitsForNotes(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    return (track.note_finish_wait && track.channels >= 0) || WaitsForEver(index);
}

// A held note's length doesn't count: the game stops it first.
bool Driver::HasEndingNote(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    if (track_follows_[static_cast<std::size_t>(index)].holds)
    {
        return false;
    }

    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        if (channels_[static_cast<std::size_t>(at)].length > 0)
        {
            return true;
        }
    }

    return false;
}

bool Driver::HoldsSustainedNote(int index) const
{
    const Track& track = tracks_[static_cast<std::size_t>(index)];
    const bool holds = track_follows_[static_cast<std::size_t>(index)].holds;
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        const Channel& channel = channels_[static_cast<std::size_t>(at)];
        if ((holds || channel.length < 0) && channel.envelope_stage == kSustain && ChannelWaveLoops(channel))
        {
            return true;
        }
    }

    return false;
}

// A PCM wave loops when its settings say so. A rectangular wave and noise play on for ever.
bool Driver::ChannelWaveLoops(const Channel& channel) const
{
    return channel.type != kPcm || channel.wave.loop != 0;
}

// A track after track 0 is the main track when every track before it is idle, waits for ever, or passes without
// notes.
bool Driver::IsMainTrack(const Player& player, int number) const
{
    for (int i = 0; i < number; i++)
    {
        const Track* track = TrackOf(player.id, i);
        if (!track || track->position == 0)
        {
            continue;
        }

        const int index = static_cast<int>(track - tracks_.data());
        if (!IsIdle(index) && !WaitsForEver(index) &&
            (i == 0 || !track_follows_[static_cast<std::size_t>(index)].quiet))
        {
            return false;
        }
    }

    return true;
}

bool Driver::OthersRest(const Player& player, int number) const
{
    for (int i = 0; i < kTracksPerPlayer; i++)
    {
        const Track* track = TrackOf(player.id, i);
        if (i == number || !track || track->position == 0)
        {
            continue;
        }

        const int index = static_cast<int>(track - tracks_.data());
        if (!IsIdle(index) && !WaitsForEver(index))
        {
            return false;
        }
    }

    return true;
}

// Whether `test` passes for the driver's index of each open track of player `player`. It passes when no track is open.
template <typename Test>
bool Driver::EveryOpenTrack(int player, const Test& test) const
{
    for (int number = 0; number < kTracksPerPlayer; number++)
    {
        const Track* track = TrackOf(player, number);
        if (track && track->position != 0 && !test(static_cast<int>(track - tracks_.data())))
        {
            return false;
        }
    }

    return true;
}

} // namespace ntdlib::ntr::snd
