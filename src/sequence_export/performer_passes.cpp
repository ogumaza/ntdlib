// SPDX-License-Identifier: MIT

// The performer: the passes of the tracks' loops and the main loop's marks (see performer.cpp).

#include "performer_internal.h"

namespace ntdlib::sequence_export
{

void Performer::OnLoop(int t, uint32_t target, bool jump)
{
    if (const std::optional<uint32_t> start = CountedPassStart(t, target, jump))
    {
        MarkPass(t, target, *start);
    }
}

std::optional<uint32_t> Performer::CountedPassStart(int t, uint32_t target, bool jump)
{
    // A backward jump to unvisited commands is not a loop; shared code may precede its callers. Also ignore passes
    // without waits, such as random retries or variable polling. A jump back to commands that last ran in a call that
    // the track has since returned from doesn't end a pass either. A random retry can jump into another subroutine's
    // copy of its draw. That copy last ran in an earlier pass.
    const auto run = last_run_[t].find(target);
    Track& tr = tracks_[t];
    const std::optional<uint32_t> last_wait = tr.last_wait;
    if (run == last_run_[t].end() || !last_wait || *last_wait < run->second.tick || ReturnedFrom(t, run->second.call))
    {
        return std::nullopt;
    }

    // A loop that the track jumps back to during a pass of another loop is an inner loop, such as a countdown of waits
    // between the notes of an outer loop. The outer loop repeats the inner loop with everything else. Later passes of
    // an inner loop therefore don't count. The first pass of the outer loop shows which loops are inner. These passes,
    // and passes that can leave their loop (below), don't set Track::idle. It keeps the value from the other passes of
    // the track. An inner loop counts again once the track has played no pass of another loop for kMinLoopSeconds. The
    // outer loop may then take minutes to repeat. A countdown can compare a counter that no command resets with a
    // constant. Its later passes then run until the counter wraps.
    const uint32_t start = run->second.tick;
    const uint64_t first_command = run->second.order;
    if (InnerLoopPass(t, target, first_command))
    {
        return std::nullopt;
    }

    // A pass that changed nothing is idle: the track only waited, polled or counted (ChangedSince).

    // A jump back inside a subroutine isn't a loop when its pass ran a conditional command that can leave the loop and
    // played no note. A subroutine that polls a variable after a one-shot sound does that. A stricter rule would leave
    // out every pass that runs a conditional command. But a game may loop a melody, or move the pitch of a note it has
    // played, in such a subroutine until it stops the sound. And a conditional setvar can't leave the loop.
    // Such a pass is still idle when it changed nothing, changed no variable and drew no random number. The track then
    // does the same on every pass until another track or the game changes a variable. A track that polls the game's
    // variables once a tick, while later tracks play the music, does that for as long as the sound plays. A countdown
    // between a track's notes changes its counter. Its passes keep Track::idle as the track's other passes set it.
    if (jump && Leavable(t, first_command))
    {
        if (!ChangedSince(t, start, first_command) && tr.last_variable_change <= first_command &&
            last_draw_order_ <= first_command)
        {
            tr.idle = true;
        }

        return std::nullopt;
    }

    tr.last_pass_ms = now_ms_;
    tr.idle = !ChangedSince(t, start, first_command);

    // Use the lowest-numbered track that can still advance: track 0 while active, or a later track for continuing sound
    // effects. A pass counts only if no earlier track contributed during it. The marked section then contains only
    // repeating material. A track whose last pass was idle doesn't count as advancing. An idle pass of this track
    // counts only if no later track advances or contributed either. A later track that has been quiet for kHoldSeconds
    // doesn't count as advancing: it may wait minutes in silence before it loops or closes.
    auto plays_on = [&](int i)
    {
        return tracks_[i].open && !WaitsForever(i) && !tracks_[i].idle && (i < t || !Quiet(i));
    };
    for (int i = 0; i < kTrackCount; i++)
    {
        if (i != t && (i < t || tr.idle) && (plays_on(i) || ChangedSince(i, start, first_command)))
        {
            return std::nullopt;
        }
    }

    // A pass that started before the game's last change of a variable can play otherwise than the passes after it
    // (ChangeVariables). It doesn't count.
    if (start < changed_at_)
    {
        return std::nullopt;
    }

    return start;
}

bool Performer::InnerLoopPass(int t, uint32_t target, uint64_t first_command)
{
    Track& tr = tracks_[t];
    for (const auto& [other, at] : tr.jump_backs)
    {
        if (other != target && at > first_command)
        {
            tr.inner_loops.insert(other);
        }
    }

    tr.jump_backs[target] = commands_run_;
    if (tr.inner_loops.contains(target))
    {
        if (now_ms_ - tr.last_pass_ms < kMinLoopSeconds * 1000.0)
        {
            return true;
        }

        tr.inner_loops.erase(target);
    }

    return false;
}

bool Performer::ChangedSince(int t, uint32_t start, uint64_t first_command) const
{
    // Changing a variable that another open track reads counts as a change (Track::last_signal). It belongs to the
    // pass if it came after the pass's first command. The tick doesn't show that: a loop's last commands run in the
    // tick that its next pass starts in.
    const Track& tr = tracks_[t];
    return (tr.last_change && *tr.last_change >= start) || (tr.last_signal && *tr.last_signal > first_command);
}

void Performer::MarkPass(int t, uint32_t target, uint32_t start)
{
    Track& tr = tracks_[t];

    // A pass that changed something, of another loop than the marked one, means the marked pass doesn't repeat what
    // plays now. That happens when an earlier track has gone idle while a later track's loop plays on. Mark this pass
    // instead. An idle pass counts only while nothing else changes. Any idle pass then repeats the sound. Such a pass
    // therefore keeps the marked loop. It doesn't when the pass plays on the track of the marked loop, or when that
    // track has closed. The marked loop then no longer plays.
    const std::pair<int, uint32_t> owner{t, target};
    const std::optional<std::pair<int, uint32_t>>& marked = main_loop_.Owner();
    const int marked_track = marked ? marked->first : t;
    if (!marked || (*marked != owner && (!tr.idle || marked_track == t || !tracks_[marked_track].open)))
    {
        main_loop_.Restart(midi_.out_, owner);
    }

    main_loop_.CountPass(midi_.out_, owner, start, tick_);

    // The performance stops at a pass that repeats, once it has played the passes asked for and kMinLoopSeconds. It
    // plays on while a change of a variable is still to come. The change can change what the loop plays.
    const bool may_stop = midi_.out_.loops >= options_.loops &&
                          now_ms_ >= kMinLoopSeconds * 1000.0 - settings_.early_ticks_ms &&
                          next_change_ == options_.changes.size() && ProcedureChangesArmed();

    // A silent pass that ran a conditional command that can leave its loop can be a tick of a countdown between the
    // notes of an outer loop. The outer loop's first pass shows such a loop as inner (CountedPassStart). When that pass
    // ends after kMinLoopSeconds, the countdown's passes count first. A copy of the performer therefore plays on for
    // kMinLoopSeconds to see whether the loop turns inner. If it does, it's inner from now on, and the performance
    // plays on.
    const auto run = last_run_[t].find(target);
    if (may_stop && run != last_run_[t].end() && tr.leave_at > run->second.order && tr.note_at <= run->second.order &&
        TurnsInner(t, target))
    {
        tr.inner_loops.insert(target);
        return;
    }

    if (may_stop && MarkSteadyPass(t, target))
    {
        stop_ = true;
        stopped_by_ = t;
        held_sound_end_ms_ = HeldSoundEnd(start);
    }
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    if (now_ms_ - tick_ms_[start] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // The last change has to be more than kHoldSeconds back.
    const uint32_t last = LastChangeTick();
    if (now_ms_ - tick_ms_[last] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // Every sounding note must be endless, unreleased and the newest note of an open track with tie or mono on.
    bool sounding = false;
    for (const Track& tr : tracks_)
    {
        for (const Note& n : tr.detached)
        {
            if (n.EndMs() > now_ms_)
            {
                return std::nullopt;
            }
        }

        for (std::size_t k = 0; k < tr.notes.size(); k++)
        {
            const Note& n = tr.notes[k];
            if (n.EndMs() <= now_ms_ || BelowHearing(n))
            {
                continue;
            }
            if (!tr.open || !(tr.tie || tr.mono) || n.released || n.EndMs() < kNever || k + 1 != tr.notes.size())
            {
                return std::nullopt;
            }

            sounding = true;
        }
    }
    if (!sounding)
    {
        return std::nullopt;
    }

    return tick_ms_[last] + kHoldSeconds * 1000.0;
}

uint32_t Performer::LastChangeTick() const
{
    if (settings_.reverb_moves_loops)
    {
        return midi_.out_.LastEventTick();
    }

    // The last tempo change, and each track's last event that isn't CC91.
    const auto is_change = [](const TrackEvent& e)
    {
        return e.kind != EventKind::kReverb;
    };
    uint32_t last = midi_.out_.tempo.empty() ? 0 : midi_.out_.tempo.back().tick;
    for (const std::vector<TrackEvent>& events : midi_.out_.tracks)
    {
        const auto change = std::find_if(events.rbegin(), events.rend(), is_change);
        if (change != events.rend())
        {
            last = std::max(last, change->tick);
        }
    }

    return last;
}

bool Performer::SilentLoop() const
{
    if (seqlib::midi::StartsNote(midi_.out_, midi_.out_.loop->start))
    {
        return false;
    }

    // A sunk channel (MidiNote::sunk) has ended its MIDI note, and its sound ends at its fade.
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.midi.on || n.SoundEndMs() >= kNever)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

} // namespace ntdlib::sequence_export
