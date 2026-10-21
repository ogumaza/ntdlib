// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/loop_finder.h"

#include <algorithm>
#include <cstdint>
#include <functional>

namespace ntdlib::jaudio
{

long long FramesToMs(uint64_t frames, uint32_t sample_rate)
{
    return static_cast<long long>(frames * 1000 / sample_rate);
}

bool ChangesSound(uint8_t command)
{
    return command < 0xa0 || (command >= 0xb0 && command <= 0xbf) || command == 0xc1 || command == 0xc2 ||
           (command >= 0xe0 && command <= 0xef) || command == kFinishCommand;
}

bool JAudio1ChangesSound(uint8_t command)
{
    // Notes, the ends of notes (with 0xf9) and the track's parameters (0x90 to 0x9f). Waits (0x80 and 0x88) and the
    // commands on registers (0xa0 to 0xaf) don't change the sound.
    if (command < 0xc0)
    {
        return command < 0x80 || (command < 0xa0 && command != 0x80 && command != 0x88);
    }

    // Tracks opened (0xc1 and 0xc2) or closed (0xda), the oscillators and envelopes, transposition, the outputs,
    // vibrato, the release of the track's notes, pan, the filters, the volume mode, the tempo and timebase, and the
    // track's end.
    return command == 0xc1 || command == 0xc2 || (command >= 0xd6 && command <= 0xdb) || command == 0xdd ||
           command == 0xe5 || command == 0xe6 || command == 0xe8 || command == 0xe9 ||
           (command >= 0xeb && command <= 0xf4) || command == 0xf9 || command >= 0xfd;
}

LoopFinder::LoopFinder(uint32_t root, uint32_t sample_rate, bool (*changes_sound)(uint8_t command), bool idle_in_calls)
    : sample_rate_(sample_rate), changes_sound_(changes_sound), idle_in_calls_(idle_in_calls)
{
    order_.push_back(root);
    tracks_[root] = {};
}

LoopFinder::Verdict LoopFinder::Take(const TrackEvent& event, const Sounded& sounded)
{
    taken_++;
    Track& track = Find(event.track, event.frame);
    switch (event.kind)
    {
    case TrackEvent::Kind::kCommand:
        Command(track, event);
        break;
    case TrackEvent::Kind::kJump:
        return Jump(track, event, sounded);
    }

    return Verdict::kNone;
}

void LoopFinder::TakeWaits(const std::function<TrackWait(uint32_t track)>& read)
{
    for (auto& [id, track] : tracks_)
    {
        track.wait = read(id);
    }
}

bool LoopFinder::Holds(uint64_t now) const
{
    const bool idles =
        std::any_of(tracks_.begin(), tracks_.end(), [](const auto& entry) { return entry.second.idle.has_value(); });
    const bool settled = std::all_of(tracks_.begin(), tracks_.end(),
                                     [this, now](const auto& entry) { return Settled(entry.second, now); });

    uint64_t last_move = 0;
    for (const auto& [id, track] : tracks_)
    {
        last_move = std::max(last_move, LastMove(track));
    }

    return idles && settled && FramesToMs(now - last_move, sample_rate_) >= kStillMs;
}

LoopFinder::Track& LoopFinder::Find(uint32_t track, uint64_t frame)
{
    const auto [it, added] = tracks_.try_emplace(track);
    if (added)
    {
        order_.push_back(track);
        it->second.first_command = frame;
    }

    return it->second;
}

// A command inside the loop that the track idles in, or in a call from it, isn't a move. A move in a call counts at the
// call.
void LoopFinder::Command(Track& track, const TrackEvent& event)
{
    track.last_command = event.frame;
    if (event.depth == 0)
    {
        track.call_site = event.at;
        track.reached[event.at] = {event.frame, taken_};
    }
    else if (idle_in_calls_)
    {
        track.reached_in_calls[event.at] = {event.frame, taken_};
    }

    if (changes_sound_(event.command))
    {
        track.changed = taken_;
    }

    const auto& idle = track.idle;
    if (idle &&
        (event.depth > idle->depth || (event.depth == idle->depth && event.at >= idle->start && event.at < idle->end)))
    {
        return;
    }

    track.idle.reset();
    track.moves[track.call_site] = event.frame;
    track.finished = event.command == kFinishCommand && event.depth == 0;
}

uint64_t LoopFinder::LastMove(const Track& track)
{
    uint64_t last = track.judged_move;
    for (const auto& [at, frame] : track.moves)
    {
        last = std::max(last, frame);
    }

    return last;
}

// A jump back to a command that the track has played, outside any call or counted loop. The pass started when the track
// last reached that command. A pass that changes nothing waits, or polls the game. The track then idles in that loop,
// and its commands in the loop weren't moves. A jump back judges only the moves inside its loop. The others wait for
// the jump back of a loop around them. Any other pass counts towards a loop unless it comes before the first sound or a
// track before this one holds it back.
LoopFinder::Verdict LoopFinder::Jump(Track& track, const TrackEvent& event, const Sounded& sounded)
{
    if (event.to >= event.at)
    {
        return Verdict::kNone;
    }

    if (event.depth != 0)
    {
        if (idle_in_calls_)
        {
            JumpInCall(track, event);
        }

        return Verdict::kNone;
    }

    // Event orders start at 1. A track that never reached the target ran the pass from its first command.
    const auto start = track.reached.find(event.to);
    const bool reached = start != track.reached.end();
    track.pass_start = reached ? start->second.frame : track.first_command;
    const bool idles = track.changed < (reached ? start->second.order : 1);

    const auto first = track.moves.lower_bound(event.to);
    const auto last = track.moves.lower_bound(event.at);
    if (idles)
    {
        track.idle = Loop{event.to, event.at};
    }
    else
    {
        for (auto move = first; move != last; ++move)
        {
            track.judged_move = std::max(track.judged_move, move->second);
        }
    }

    track.moves.erase(first, last);

    // A pass that doesn't count starts the count again. Such a pass keeps the count when its loop lies inside the loop
    // being counted.
    if (idles || !sounded.any || HeldBack(event.track, track, event.frame))
    {
        const bool inside = track.counted && event.to > track.counted->start && event.at <= track.counted->end;
        if (!inside)
        {
            track.loops = 0;
            track.counted.reset();
        }

        return Verdict::kNone;
    }

    if (track.loops > 0 && sounded.last < track.pass_start)
    {
        return Verdict::kEnded;
    }

    track.loops = track.counted && track.counted->start == event.to ? track.loops + 1 : 1;
    track.counted = Loop{event.to, event.at};

    return track.loops == 2 ? Verdict::kLoops : Verdict::kNone;
}

// With idle_in_calls: a jump back inside a call. The pass started when the track last reached the jump's target. A pass
// that changes nothing waits, or polls the game. The track then idles in that loop. The pass's commands weren't moves,
// and neither are its later commands in the loop and in calls from it. The commands in the call before the pass were.
// They count at the call, as at most the pass's start.
void LoopFinder::JumpInCall(Track& track, const TrackEvent& event)
{
    const auto start = track.reached_in_calls.find(event.to);
    if (start == track.reached_in_calls.end() || track.changed >= start->second.order)
    {
        return;
    }

    track.idle = Loop{event.to, event.at, event.depth};
    const auto call = track.moves.find(track.call_site);
    if (call != track.moves.end())
    {
        call->second = std::min(call->second, start->second.frame);
    }
}

bool LoopFinder::Settled(const Track& track, uint64_t now) const
{
    const bool counting = track.wait.ticks > 0 && !track.wait.notes;
    const bool still = FramesToMs(now - track.last_command, sample_rate_) >= kStillMs;
    return track.idle || track.finished || (still && !counting);
}

bool LoopFinder::HeldBack(uint32_t id, const Track& track, uint64_t now) const
{
    for (const uint32_t earlier : order_)
    {
        if (earlier == id)
        {
            return false;
        }

        const Track& other = tracks_.at(earlier);
        if (LastMove(other) > track.pass_start || !Settled(other, now))
        {
            return true;
        }
    }

    return false;
}

} // namespace ntdlib::jaudio
