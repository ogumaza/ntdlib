// SPDX-License-Identifier: MIT

// The performer: the run, the search for the main loop and the end of the performance (see performer.cpp).

#include "performer_internal.h"

namespace ntdlib::sequence_export
{

Performance Performer::Run()
{
    // The game's player opens track 0, if the sound allocates it, at the sound's start offset (on the Wii,
    // SeqPlayer::Start, main.dol 0x8003e504). The others always allocate it.
    AllocateTracks();
    if (tracks_[0].allocated)
    {
        OpenTrack(0, sound_.start_offset);
    }

    midi_.OnTempo(*this);
    midi_.RecordTickTime(*this);
    StartCore();

    bool finished = false;
    double steady_since = -1.0;
    PassStart next_pass;
    for (;;)
    {
        StartTick();
        const std::array<int16_t, 16> globals = global_vars_;
        const bool any_open = ParseTracks(0);
        RecordGlobalChanges(globals);
        if (stop_)
        {
            finished = EndAtStoppingPass(next_pass);
            break;
        }

        FinishTick();
        if (const std::optional<bool> ended = EndAfterTick(any_open, steady_since))
        {
            finished = *ended;
            break;
        }

        const std::optional<TickTime> next = NextTick();
        if (!next)
        {
            midi_.rec_.Warn(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                           : "tempo 0: the sequence stops advancing");
            PlayOutStopped();
            break;
        }

        AdvanceTo(*next);
    }

    EndPerformance(finished);
    if (midi_.out_.loop)
    {
        RestoreAtLoopStart(midi_.out_, midi_.rec_.PartSwitches(), next_pass, LoopRulesOf(settings_));
    }

    midi_.out_.approximations = midi_.rec_.Approximations();
    midi_.out_.warnings = midi_.rec_.Warnings();
    for (int i = 0; i < 16; i++)
    {
        midi_.out_.globals_read |= static_cast<uint16_t>(variable_readers_[16 + i] != 0 ? 1u << i : 0u);
    }

    return midi_.out_;
}

bool Performer::EndAtStoppingPass(PassStart& next_pass)
{
    // A main loop that sounds nothing is no loop. The sequence ends with its last sound, like one that ends.
    if (midi_.out_.loop && !held_sound_end_ms_ && SilentLoop())
    {
        silent_loop_ = true;
        midi_.out_.DropLoop();
        return true;
    }
    if (midi_.out_.loop && !held_sound_end_ms_)
    {
        next_pass = tick_ > midi_.out_.loop->end ? PassStartAt(midi_.out_.loop->end)
                                                 : NextPassStart(stopped_by_, tracks_[stopped_by_].current);
    }

    return false;
}

std::optional<bool> Performer::EndAfterTick(bool any_open, double& steady_since)
{
    // Every track has ended. The game repeats none of the passes before the end, such as a state machine's jumps back
    // to its dispatcher. Drop the loop marked on the way. On the Wii, a sequence also ends when no track will read on
    // and no note sounds (Stuck).
    if (!any_open || (Wii() && Stuck()))
    {
        midi_.out_.DropLoop();
        return true;
    }

    // A performance with a stop plays to it (SetStop).
    if (steady_since < 0 && !stop_ms_ && Steady())
    {
        steady_since = now_ms_;
    }
    if (steady_since >= 0 && now_ms_ - steady_since >= kHoldSeconds * 1000.0)
    {
        midi_.out_.holds = true;
        return false;
    }

    if (now_ms_ >= kMaxSeconds * 1000.0)
    {
        return EndAtTimeLimit();
    }
    if (stop_ms_ && now_ms_ >= *stop_ms_)
    {
        midi_.out_.DropLoop();
        return false;
    }

    return std::nullopt;
}

void Performer::RecordGlobalChanges(const std::array<int16_t, 16>& before)
{
    for (int i = 0; i < 16; i++)
    {
        if (global_vars_[i] != before[i])
        {
            midi_.out_.global_changes.push_back(
                {.frame = frame_, .index = static_cast<uint8_t>(16 + i), .value = global_vars_[i]});
        }
    }
}

void Performer::StartCore()
{
    // The core's sound starts in the first tick's frame. The core's clock gives the first tick at the frame's start.
    // The core allocates the tracks that AllocateTracks does. The core alone would allocate only those that the sound's
    // flags select. A sound archive's flags give the tracks of the sequence's alloctrack.
    SoundInfo info = sound_;
    info.allocate_track_flags = 0;
    for (int t = 0; t < kTrackCount; t++)
    {
        info.allocate_track_flags |= tracks_[t].allocated ? 1u << t : 0u;
    }

    const auto start = [&](auto& core)
    {
        core.Play(info, seq_.data, 0);
        core.GetPlayer().SetBuild(core.Build());
        core.GetPlayer().SetTempoRatio(options_.tempo_ratio);
        core.GetPlayer().SetPitch(options_.pitch);
        for (const auto& [index, value] : options_.variables)
        {
            if (index < 32)
            {
                core.GetPlayer().SetVariable(core, static_cast<int>(index), value);
            }
        }
        for (const TrackVariable& variable : options_.track_variables)
        {
            core.GetPlayer().SetTrackVariable(variable.track, variable.index, variable.value);
        }

        // The frame starts the sound, and the player allocates its tracks. The game's code then silences the tracks
        // that PerformOptions::tracks leaves out, gives the others their volumes, and sets the tracks' variables.
        core.BeginFrame();
        core.GetPlayer().SetTrackVolume(~static_cast<uint32_t>(options_.tracks) & 0xffffu, 0.0f);
        for (std::size_t t = 0; t < kSequenceTracks; ++t)
        {
            const uint32_t bit = 1u << t;
            if ((options_.tracks & bit) != 0 && options_.track_volumes[t] != 1.0f)
            {
                core.GetPlayer().SetTrackVolume(bit, options_.track_volumes[t]);
            }
        }
        for (const auto& [index, value] : options_.variables)
        {
            if (index >= 32 && index < 48)
            {
                SetTrackVariables(core, index, value);
            }
        }
        core.GetPlayer().StartClock(1);
        core.GetPlayer().NextTickTime();
        player_vars_ = core.GetPlayer().LocalVariables();
        global_vars_ = core.GlobalVariables();
    };
    WithCore(start);

    // The Wii's build of October 2006 starts its count at a whole tick, and its first frame runs one tick.
    if (WiiOctober2006())
    {
        frame_ticks_left_ = std::get<RevolutionCore>(core_).GetPlayer().ClockTicksLeft();
    }
}

bool Performer::EndAtTimeLimit()
{
    // Treat a sequence that has been silent for longer than kHoldSeconds at the time limit as finished. Such a
    // sequence ends with its last sound and, like a sequence that ends, has no main loop.
    bool sounding = false;
    for (int t = 0; t < kTrackCount; t++)
    {
        sounding = sounding || OthersSounding(t, nullptr);
    }
    const bool finished = !sounding && now_ms_ - sound_end_ms_ > kHoldSeconds * 1000.0;
    midi_.out_.truncated = !finished;

    // When the performance is cut here, the outermost loop that each open track is in, where that loop is a counted one
    // (LimitLoops). The length analysis takes the same loops (nwsnd::OutermostCountedLoops). calls[0] is the outermost
    // entry. A track whose outermost loop is endless loops already, and keeps its inner loops. An inner loop still
    // ends, and repeats in each pass of the outer loop. A performance that counts as finished ends with its last sound.
    for (int t = 0; midi_.out_.truncated && t < kTrackCount; t++)
    {
        const Track& tr = tracks_[t];
        for (int i = 0; tr.open && i < tr.call_depth; i++)
        {
            if (tr.calls[i].is_loop)
            {
                if (tr.calls[i].loop_count != 0)
                {
                    limit_loops_.insert({t, tr.calls[i].address});
                }
                break;
            }
        }
    }

    // A sequence that finishes drops its loop.
    if (finished)
    {
        midi_.out_.DropLoop();
    }

    return finished;
}

Performer::PassStart Performer::PassStartAt(uint32_t tick) const
{
    return sequence_export::PassStartAt(midi_.out_, midi_.rec_.PartSwitches(), tick);
}

Performer Performer::ResumeAfterJump(int t, uint32_t target) const
{
    // Track `t` has jumped back, and the tracks after it haven't run this tick. The copy runs them with no limit on the
    // loops. No pass then stops the copy again.
    Performer probe = *this;
    probe.stop_ = false;
    probe.options_.loops = std::numeric_limits<int>::max();
    probe.tracks_[t].current = target;
    probe.WithCore([&](auto& core) { probe.ContinueCoreTrack(core, t); });
    probe.ParseTracks(t + 1);
    probe.FinishTick();

    return probe;
}

bool Performer::PlayNextTick()
{
    const std::optional<TickTime> next = NextTick();
    if (!next)
    {
        return false;
    }

    AdvanceTo(*next);
    StartTick();
    const bool any_open = ParseTracks(0);
    FinishTick();

    return any_open;
}

Performer Performer::PlayOn(int t, uint32_t target, uint32_t until) const
{
    Performer probe = ResumeAfterJump(t, target);
    while (probe.tick_ + 1 < until)
    {
        if (!probe.PlayNextTick())
        {
            break;
        }
    }

    return probe;
}

bool Performer::TurnsInner(int t, uint32_t target) const
{
    // The copy stops at the first tick that shows the loop inner.
    Performer probe = ResumeAfterJump(t, target);
    const double end_ms = now_ms_ + kMinLoopSeconds * 1000.0;
    while (probe.now_ms_ < end_ms && !probe.tracks_[t].inner_loops.contains(target))
    {
        if (!probe.PlayNextTick())
        {
            break;
        }
    }

    return probe.tracks_[t].inner_loops.contains(target);
}

Performer::PassStart Performer::NextPassStart(int t, uint32_t target) const
{
    return PlayOn(t, target, tick_ + 1).PassStartAt(tick_);
}

bool Performer::RepeatsAsTheGame(uint32_t start, uint32_t end) const
{
    return sequence_export::RepeatsAsTheGame(midi_.out_, midi_.rec_.PartSwitches(), start, end, LoopRulesOf(settings_));
}

bool Performer::MarkSteadyPass(int t, uint32_t target)
{
    // A copy of this performer plays on from the jump back. Its passes are compared. A later run of the sequence makes
    // the choices of the first (Perform). The first run compared its passes without the details that later runs give
    // the notes it played, such as the attacks of early releases (PassFindings). Notes that only a copy played have
    // none of those details.
    auto play_on = [&](uint32_t until)
    {
        return [probe = PlayOn(t, target, until)](uint32_t start, uint32_t end)
        {
            return probe.RepeatsAsTheGame(start, end);
        };
    };
    return main_loop_.MarkSteadyPass(midi_.out_, last_draw_, options_.loops, play_on);
}

std::optional<TickTime> Performer::NextTick()
{
    std::optional<TickTime> next;
    WithCore([&](auto& core) { next = NextCoreTick(core); });

    return next;
}

template <typename Core>
std::optional<TickTime> Performer::NextCoreTick(Core& core)
{
    // The clock goes on from the last tick through the frames after it, a frame at a time, until a tick falls in one.
    auto& player = core.GetPlayer();
    TickTime next{frame_};
    std::optional<uint64_t> left = player.NextTickTime();
    while (!left)
    {
        if (player.ClockStopped())
        {
            return std::nullopt;
        }

        next.frame++;
        player.StartClock(1);
        left = player.NextTickTime();
    }

    // On the Wii, the tick's time comes from the milliseconds left in its frame, in the clock's floats. The build of
    // October 2006 runs its ticks at the frame's start.
    if constexpr (std::is_same_v<Core, RevolutionCore>)
    {
        next.ms = static_cast<double>(next.frame) * settings_.frame_ms +
                  (settings_.frame_ms - static_cast<double>(player.ClockRestMs()));
        if (WiiOctober2006())
        {
            next.ticks_left = player.ClockTicksLeft();
            next.nominal_ms = nominal_ms_ + MidiMsPerTick();
        }
    }
    else
    {
        const uint64_t frame_units = FrameUnits(settings_);
        next.ms = UnitsToMs(next.frame * frame_units + (frame_units - *left), settings_);
    }

    return next;
}

void Performer::PlayOutStopped()
{
    // The frame's channel update makes the note-offs still pending. Their MIDI note-offs come at most a tick after the
    // last, at the MIDI tempo. As in EndPerformance, the core's frames finish the one-shot waves.
    const double ticks_per_second = midi_.out_.tempo.back().bpm * midi_.out_.timebase / 60.0;
    const double next_frame_ms = static_cast<double>(frame_ + 1) * settings_.frame_ms;
    ReleasePending({frame_ + 1, std::max(now_ms_ + 1000.0 / ticks_per_second, next_frame_ms)});
    WithCore([&](auto& core) { RunCoreTail(core, FrameOf(now_ms_ + kMaxTailMs) + 1); });

    double end_ms = now_ms_;
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                midi_.out_.holds = midi_.out_.holds || n.EndMs() >= kNever;
                end_ms = std::max(end_ms, std::min(n.EndMs(), now_ms_ + kMaxTailMs));
            }
        }
    }
    if (midi_.out_.holds)
    {
        end_ms = now_ms_ + kHoldSeconds * 1000.0;
    }

    // MIDI ticks continue after sequence timing stops. Millisecond sweeps keep moving; note-length sweeps remain frozen
    // with the sequence's ticks (UpdateOutputs).
    const auto ticks = static_cast<uint32_t>(std::ceil((end_ms - now_ms_) / 1000.0 * ticks_per_second));
    const double start_ms = now_ms_;
    for (uint32_t i = 1; i <= ticks; i++)
    {
        const double ms = start_ms + i / ticks_per_second * 1000.0;
        midi_.SweepBetweenTicks(*this, {FrameOf(ms), ms});

        tick_++;
        now_ms_ = ms;
        if (Wii())
        {
            nominal_ms_ += 1000.0 / ticks_per_second;
        }
        frame_ = FrameOf(now_ms_);

        for (int t = 0; t < kTrackCount; t++)
        {
            PruneNotes(t); // finished notes have no active sweep
            midi_.UpdateOutputs(*this, t);
        }
    }
}

void Performer::EndPerformance(bool finished)
{
    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers. Nothing changed in
    // the ticks after that.
    if (held_sound_end_ms_)
    {
        tick_ = std::min(MsToTick(*held_sound_end_ms_), tick_);
        now_ms_ = tick_ms_[tick_];
        midi_.out_.holds = true;
        midi_.out_.DropLoop();
    }

    // Stop immediately at a loop boundary, time limit or held-sound limit. A naturally finished sequence ends with its
    // final sound, including release tails; nw::snd can leave finished tracks waiting long after silence. The core's
    // frames finish the one-shot waves.
    if (finished)
    {
        WithCore([&](auto& core) { RunCoreTail(core, FrameOf(now_ms_ + kMaxTailMs) + 1); });
    }

    EndNotes(finished);
    if (finished)
    {
        SetFinishedEnd();
    }
    else
    {
        midi_.out_.end_tick = tick_;
        midi_.out_.seconds = now_ms_ / 1000.0;
    }

    // Without Settings::reverb_moves_loops, a change of CC91 doesn't move the end. A MIDI track ends after its last
    // event. The changes of CC91 after the end are therefore dropped. Nothing sounds there.
    if (!settings_.reverb_moves_loops)
    {
        const uint32_t end = midi_.out_.end_tick;
        const auto after_end = [end](const TrackEvent& e)
        {
            return e.kind == EventKind::kReverb && e.tick > end;
        };
        for (std::vector<TrackEvent>& events : midi_.out_.tracks)
        {
            std::erase_if(events, after_end);
        }
    }
}

void Performer::EndNotes(bool finished)
{
    for (int t = 0; t < kTrackCount; t++)
    {
        Track& tr = tracks_[t];
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                // On the Wii, a note below hearing ends the sequence's sound where it has faded (MidiNote::faded_ms).
                // Stuck can end a sequence there while its tracks wait out held notes.
                if (finished)
                {
                    const double end = Wii() && BelowHearing(n) ? n.midi.faded_ms : n.SoundEndMs();
                    sound_end_ms_ = std::max(sound_end_ms_, std::min(end, now_ms_ + kMaxTailMs));
                }
                midi_.MidiNoteOff(*this, t, n);
            }
        }
    }

    // The tails' timed sweeps go on after the last tick, and their pitch events follow its note-offs.
    if (finished)
    {
        SweepAfterEnd();
    }

    for (Track& tr : tracks_)
    {
        tr.notes.clear();
        tr.detached.clear();
    }
}

void Performer::SetFinishedEnd()
{
    // After a main loop that sounded nothing (SilentLoop), what comes after the last sound can't be heard. Changes that
    // come more than kHoldSeconds after the last sound can't be heard either (EndsAtLastSound). The performance then
    // ends with the sound. A note below hearing that sounds on keeps its note-off, at the end.
    if (silent_loop_ || EndsAtLastSound(sound_end_ms_ / 1000.0, tick_ms_[LastChangeTick()] / 1000.0))
    {
        seqlib::midi::EndAt(midi_.out_, MsToTick(sound_end_ms_));
    }

    const uint32_t last_change = LastChangeTick();
    midi_.out_.end_tick = std::max(last_change, MsToTick(sound_end_ms_));
    midi_.out_.seconds = std::max(sound_end_ms_, tick_ms_[last_change]) / 1000.0;
}

uint32_t Performer::MsToTick(double ms) const
{
    const auto it = std::lower_bound(tick_ms_.begin(), tick_ms_.end(), ms);
    if (it != tick_ms_.end())
    {
        return static_cast<uint32_t>(it - tick_ms_.begin());
    }

    // Extrapolate at the last tempo, or the MIDI tempo if sequence timing stopped.
    const double per_tick = MsPerTick() > 0 ? MsPerTick() : MidiMsPerTick();
    return static_cast<uint32_t>(tick_ms_.size() - 1 + std::ceil((ms - tick_ms_.back()) / per_tick));
}

} // namespace ntdlib::sequence_export
