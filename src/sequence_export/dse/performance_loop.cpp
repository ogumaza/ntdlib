// SPDX-License-Identifier: MIT

// DSE's performer: the main loop's passes, the end of the performance and the runs (see performance.cpp).

#include "performance_internal.h"

namespace ntdlib::sequence_export::dse
{

namespace
{

namespace detail = ntdlib::sequence_export::detail;

FrameProbe Performer::PlayOn(uint32_t until, std::size_t enough)
{
    // The player and the performer's state go back to where they are once the copy has played (PlayProbe).
    const Player::Snapshot snapshot = player_->Save();
    State saved = s_;
    detail::PlayProbe(s_, until, enough, [this] { return Advance(); });

    std::vector<ntdlib::sequence_export::CountedPass> passes = detail::CountedPasses(s_);
    const std::size_t whole_span = WholeSpan();
    const double tolerance = s_.out.timebase * kSettledLengthShare;
    std::vector<seqlib::midi::PartSwitch> part_switches = s_.recorder.PartSwitches();
    seqlib::midi::Performance probe_out = std::move(s_.out);
    // The passes compare by the CC11 that the parts would take without their mutes (kUnmutedExpression).
    for (std::vector<TrackEvent>& events : probe_out.tracks)
    {
        std::erase_if(events, [](const TrackEvent& e) { return e.kind == EventKind::kExpression; });
        for (TrackEvent& e : events)
        {
            if (e.kind == kUnmutedExpression)
            {
                e.kind = EventKind::kExpression;
            }
        }
    }
    BendChanges probe_bends = std::move(s_.bend_changes);
    player_->Restore(snapshot);
    s_ = std::move(saved);

    // Two pitches that differ repeat only through the timer's departure from one bend (SamePitch). A pass can play a
    // note alone where the next doesn't. DSE's passes have no strikes to excuse (PassTest::kExcused): every note that
    // a pass plays gets a voice or none in every pass.
    auto repeats = [played = std::move(probe_out), switches = std::move(part_switches), bends = std::move(probe_bends),
                    tolerance](uint32_t start, uint32_t end, ntdlib::sequence_export::PassTest kind)
    {
        if (kind == ntdlib::sequence_export::PassTest::kExcused)
        {
            return false;
        }

        ntdlib::sequence_export::LoopRules rules = kFrameLoopRules;
        rules.same_pitch = [&bends](int track, const TrackEvent& a, const TrackEvent& b)
        {
            return ntdlib::sequence_export::SamePitch(bends, track, a, b);
        };

        return ntdlib::sequence_export::RepeatsAsTheGame(played, switches, start, end, rules) &&
               (kind != ntdlib::sequence_export::PassTest::kSettled ||
                ntdlib::sequence_export::LengthsRepeat(played, start, end, tolerance));
    };

    return {std::move(repeats), std::move(passes), whole_span};
}

// A track that ends leaves music that the passes before its end didn't play. The main loop's passes count again from
// there. Returns true if a track has ended since the last call and the passes start again.
bool Performer::RestartAtTrackEnds()
{
    bool restarted = false;
    for (int track = 0; track < Driver().TrackCount(); track++)
    {
        if (!Driver().TrackPlays(track) && s_.ended.insert(track).second && s_.main_loop.Owner())
        {
            s_.main_loop.Restart(s_.out, s_.main_loop.Owner());
            restarted = true;
        }
    }

    return restarted;
}

// Until each track that still plays has gone back to its loop's start, a track with a longer loop can still take the
// main loop, and a track without a loop can still play on.
bool Performer::EveryTrackLooped() const
{
    for (int track = 0; track < Driver().TrackCount(); track++)
    {
        if (Driver().TrackPlays(track) && !s_.looped.contains(track))
        {
            return false;
        }
    }

    return true;
}

std::size_t Performer::WholeSpan() const
{
    std::vector<uint32_t> lengths;
    for (int track = 0; track < Driver().TrackCount(); track++)
    {
        if (Driver().TrackPlays(track))
        {
            const auto length = s_.loop_lengths.find(track);
            lengths.push_back(length != s_.loop_lengths.end() ? length->second : 0);
        }
    }

    return detail::LastPassSpan(s_, lengths);
}

bool Performer::MarkSteadyPass()
{
    const auto play_on = [this](uint32_t until, std::size_t enough)
    {
        return PlayOn(until, enough);
    };

    return s_.main_loop.MarkSteadyPass(s_.out, std::nullopt, options_.loops, play_on, &detail::kFramePassSearch,
                                       WholeSpan());
}

void Performer::DecideLoop()
{
    const PendingLoop pending = *s_.pending;
    s_.pending.reset();
    if (RestartAtTrackEnds() || !EveryTrackLooped())
    {
        return;
    }

    // A pass that holds a sound ends the performance only once no note has started for kHoldSeconds.
    if (HoldsSound(pending.start) && s_.TickMs(s_.tick) - s_.last_note_ms <= kHoldSeconds * 1000.0)
    {
        return;
    }

    if (MarkSteadyPass())
    {
        stop_ = true;
        stop_tick_ = pending.end;
        held_sound_end_ = HeldSoundEnd(pending.start);
    }
}

// True if a note sounds and no note has started since tick `start`.
bool Performer::HoldsSound(uint32_t start) const
{
    return !seqlib::midi::StartsNote(s_.out, start) &&
           std::ranges::any_of(s_.notes, [](const auto& entry) { return entry.second.held; });
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    return HoldsSound(start) ? std::optional<double>(s_.HeldSoundEnd()) : std::nullopt;
}

void Performer::EndPerformance(bool finished)
{
    Performance& out = s_.out;

    // A performance that stops at the end of a pass leaves out what the next pass started in its last tick.
    const std::optional<uint32_t> end_tick = detail::PassEndTick(out, s_, held_sound_end_, stop_tick_);
    if (!finished && end_tick)
    {
        const std::set<uint32_t> late = seqlib::midi::EndPassAt(out, *end_tick);
        DropEventsFrom(out, *end_tick);

        for (auto& [number, note] : s_.notes)
        {
            if (late.contains(note.id))
            {
                note.held = false;
            }
        }

        for (auto& [number, note] : s_.notes)
        {
            if (note.held)
            {
                s_.recorder.NoteOff(out, static_cast<uint8_t>(note.track), *end_tick, note.part, note.key, note.id);
                note.held = false;
            }
        }

        detail::EndAtPass(out, s_, *end_tick, out.stretches, s_.channel_mix);
        return;
    }

    // A finished sequence plays its tails out.
    double end_ms = s_.FrameMs(s_.frame);
    if (finished)
    {
        const auto run_tick = [this]
        {
            player_->RunTick();
            EndFrame();
        };
        end_ms = detail::PlayTails(s_, [this] { return AnySounding(); }, run_tick);
    }

    // The notes still on end at the end.
    const double after = std::max((end_ms - s_.nominal_ms) / s_.ms_per_tick, 0.0);
    for (auto& [number, note] : s_.notes)
    {
        if (note.held)
        {
            s_.recorder.NoteOff(out, static_cast<uint8_t>(note.track), s_.tick, note.part, note.key, note.id, after);
            note.held = false;
        }
    }

    detail::EndAtLastEvent(out, s_, end_ms, after, finished, [&out](uint32_t end) { DropEventsFrom(out, end); });
}

Performance Performer::Run()
{
    // A sound effect plays on the first sequence of the pool that doesn't play.
    if (!player_->Start(files_, options_.song_start))
    {
        s_.out.warnings.push_back("the driver refuses the sound effect");
        return std::move(s_.out);
    }

    bool finished = false;
    for (;;)
    {
        if (!Advance())
        {
            finished = true;
            break;
        }

        if (s_.pending)
        {
            DecideLoop();
            if (stop_)
            {
                break;
            }
        }

        // A sequence that has been silent for longer than kHoldSeconds at the time limit has finished. It ends with its
        // last sound.
        const double now = s_.FrameMs(s_.frame);
        if (now >= kMaxSeconds * 1000.0)
        {
            finished = detail::FinishedAtTimeLimit(s_, now, AnySounding());
            s_.out.truncated = !finished;
            break;
        }
    }

    for (std::vector<TrackEvent>& events : s_.out.tracks)
    {
        std::erase_if(events, [](const TrackEvent& e) { return e.kind == kUnmutedExpression; });
    }

    const PassStart next_pass =
        s_.out.loop && stop_ && !held_sound_end_
            ? ntdlib::sequence_export::PassStartAt(s_.out, s_.recorder.PartSwitches(), s_.out.loop->end)
            : PassStart{};
    EndPerformance(finished);

    if (finished)
    {
        s_.out.DropLoop();
    }

    if (s_.out.loop)
    {
        ntdlib::sequence_export::RestoreAtLoopStart(s_.out, s_.recorder.PartSwitches(), next_pass, kFrameLoopRules);
    }

    detail::AddRecorderNotes(s_.out, s_.recorder);

    // Each track takes the changes of its channel.
    s_.out.stretches.resize(std::max(s_.out.stretches.size(), track_channels_.size()));
    s_.out.mix.resize(s_.out.stretches.size());
    for (std::size_t t = 0; t < track_channels_.size(); t++)
    {
        if (track_channels_[t] < s_.channel_mix.size())
        {
            s_.out.mix[t] = s_.channel_mix[track_channels_[t]];
        }
    }

    return std::move(s_.out);
}

} // namespace

Performance Perform(const SequenceFiles& files, const PerformOptions& options)
{
    if (files.song == nullptr && files.effects == nullptr)
    {
        throw std::invalid_argument("the files hold no song or sound effect");
    }

    // The runs settle the level scale and the parts (RunUntilSettled).
    const auto make = [&](double level_scale, const std::array<uint8_t, 16>& part_limits, const LoopReplay& replay)
    {
        return Performer(files, options, level_scale, part_limits, replay);
    };

    return ntdlib::sequence_export::detail::PerformUntilSettled<Performance>(make);
}

} // namespace ntdlib::sequence_export::dse
