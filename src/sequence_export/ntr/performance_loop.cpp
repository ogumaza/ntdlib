// SPDX-License-Identifier: MIT

// The DS's performer: the main loop's passes, the end of the performance and the runs (see performance.cpp).

#include "performance_internal.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

namespace detail = ntdlib::sequence_export::detail;

FrameProbe Performer::PlayOn(uint32_t until, std::size_t enough)
{
    // The player and the performer's state go back to where they are once the copy has played (PlayProbe).
    const ArchivePlayer::Snapshot snapshot = player_.Save(nullptr);
    State saved = s_;
    probing_ = true;
    detail::PlayProbe(s_, until, enough, [this] { return Advance(); });
    probing_ = false;

    std::vector<ntdlib::sequence_export::CountedPass> passes = detail::CountedPasses(s_);
    const std::size_t whole_span = WholeSpan();

    std::vector<seqlib::midi::PartSwitch> probe_switches = s_.recorder.PartSwitches();
    seqlib::midi::Performance probe = std::move(s_.out);
    BendChanges probe_bends = std::move(s_.bend_changes);
    StandLists probe_stands = std::move(s_.stands);
    Strikes probe_strikes = std::move(s_.strikes);
    player_.Restore(snapshot);
    s_ = std::move(saved);

    // The test reads what CC10, CC11 and CC1 stand for (TestList), then the values that a repeat keeps
    // (KeptValuesRepeat). Two pitches that differ repeat only through the timer's rounding of one bend (SamePitch). A
    // bent note plays the timer's pitch on a part that plays it alone (TimerPitch), and the bend itself on a part that
    // plays another note too. The two are a few cents apart, and a pass can play a note alone where the next doesn't.
    // Two notes play alike only where a bend that moves while they sound has the same range (Strikes::slides).
    seqlib::midi::Performance probe_test = TestList(probe, probe_stands);
    auto repeats = [played = std::move(probe), test = std::move(probe_test), switches = std::move(probe_switches),
                    bends = std::move(probe_bends), strikes = std::move(probe_strikes),
                    excused = &excused_](uint32_t start, uint32_t end, ntdlib::sequence_export::PassTest kind)
    {
        const bool excuse = kind == ntdlib::sequence_export::PassTest::kExcused;
        ntdlib::sequence_export::LoopRules rules = kFrameLoopRules;
        std::size_t left_out = 0;
        if (excuse)
        {
            rules.unstarted = [&strikes, &left_out](int track, uint32_t tick, uint8_t key)
            {
                const bool dropped = strikes.unstarted.contains({track, tick, key});
                left_out += dropped ? 1 : 0;
                return dropped;
            };
        }
        rules.same_pitch = [&bends](int track, const TrackEvent& a, const TrackEvent& b)
        {
            return SamePitch(bends, track, a, b);
        };
        rules.plays_alike = [&strikes](int track, uint32_t repeat_tick, uint32_t game_tick)
        {
            const auto range = [&strikes, track](uint32_t tick)
            {
                const auto slide = strikes.slides.find({track, tick});
                return slide != strikes.slides.end() ? slide->second : 0;
            };
            return range(repeat_tick) == range(game_tick);
        };

        const bool repeat = ntdlib::sequence_export::RepeatsAsTheGame(test, switches, start, end, rules) &&
                            KeptValuesRepeat(played, test, switches, strikes, start, end, excuse) &&
                            (kind != ntdlib::sequence_export::PassTest::kSettled ||
                             ntdlib::sequence_export::LengthsRepeat(played, start, end, kSettledLengthTicks));
        if (repeat)
        {
            (*excused)[{start, end}] = left_out;
        }
        return repeat;
    };

    return {std::move(repeats), std::move(passes), whole_span};
}

bool Performer::MarkSteadyPass()
{
    const auto play_on = [this](uint32_t until, std::size_t enough)
    {
        return PlayOn(until, enough);
    };
    const bool stop = s_.main_loop.MarkSteadyPass(s_.out, s_.last_draw, options_.loops, play_on,
                                                  &detail::kFramePassSearch, WholeSpan());

    // A pass that this run tested gives its count. A choice that this run makes again without a test keeps the count
    // of the earlier run.
    if (s_.out.loop)
    {
        if (const auto tested = excused_.find({s_.out.loop->start, s_.out.loop->end}); tested != excused_.end())
        {
            s_.main_loop.SetLeftOut(tested->second);
        }
    }

    return stop;
}

void Performer::DecideLoop()
{
    const PendingLoop pending = *s_.pending;
    s_.pending.reset();
    if (GameChangesDone() && MarkSteadyPass())
    {
        stop_ = true;
        stop_tick_ = pending.end;
        held_sound_end_ = HeldSoundEnd(pending.start);
    }
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    if (seqlib::midi::StartsNote(s_.out, start))
    {
        return std::nullopt;
    }

    const bool held = std::ranges::any_of(s_.voices, [](const Voice& v) { return v.sounding && v.held; });
    return held ? std::optional<double>(s_.HeldSoundEnd()) : std::nullopt;
}

void Performer::EndPerformance(bool finished)
{
    Performance& out = s_.out;

    // A performance that stops at the end of a pass leaves out what the next pass started in its last tick.
    const std::optional<uint32_t> end_tick = detail::PassEndTick(out, s_, held_sound_end_, stop_tick_);
    if (!finished && end_tick)
    {
        const std::set<uint32_t> late = seqlib::midi::EndPassAt(out, *end_tick);
        std::erase_if(out.unstarted, [&end_tick](const UnstartedNote& note) { return note.tick >= *end_tick; });
        for (Voice& v : s_.voices)
        {
            if (late.contains(v.note))
            {
                v.held = false;
            }
        }
        for (int c = 0; c < snd::kChannelCount; c++)
        {
            Voice& v = s_.voices[static_cast<std::size_t>(c)];
            if (v.held)
            {
                s_.recorder.NoteOff(out, static_cast<uint8_t>(v.track), *end_tick, v.part, v.key, v.note);
                v.held = false;
            }
        }
        detail::EndAtPass(out, s_, *end_tick, out.stretches, out.mix);
        return;
    }

    // A finished sequence plays its tails out.
    double end_ms = s_.FrameMs(s_.frame);
    if (finished)
    {
        const auto run_frame = [this]
        {
            player_.RunFrame();
            EndFrame();
        };
        end_ms = detail::PlayTails(s_, [this] { return AnySounding(); }, run_frame);
    }

    // The notes still on end at the end.
    const double after = std::max((end_ms - s_.nominal_ms) / s_.ms_per_tick, 0.0);
    for (int c = 0; c < snd::kChannelCount; c++)
    {
        EndVoice(c, after);
    }

    const auto drop_from = [&out](uint32_t end)
    {
        std::erase_if(out.unstarted, [end](const UnstartedNote& note) { return note.tick >= end; });
    };
    detail::EndAtLastEvent(out, s_, end_ms, after, finished, drop_from);
}

Performance Performer::Run()
{
    ntdlib::ntr::GameSettings settings = {
        .channels = options_.player_channels, .track_channels = options_.track_channels, .reverb = options_.reverb};
    for (const auto& [index, value] : options_.variables)
    {
        if (index < 16)
        {
            settings.variables.local[index] = value;
        }
        else if (index < 32)
        {
            settings.variables.global[index - 16u] = value;
        }
    }
    player_.Start(sequence_, settings);
    if (!options_.commands.empty())
    {
        player_.Send(options_.commands);
    }

    // The game's changes come in the order of their frames.
    std::ranges::stable_sort(options_.changes, {}, &VariableChange::frame);
    std::ranges::stable_sort(options_.channel_locks, {}, &ntdlib::ntr::ChannelLock::frame);
    s_.tick_progress = ntdlib::ntr::TickListProgress(options_.tick_commands.size());

    bool finished = false;
    std::optional<double> steady_since;
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

        // A sequence whose tracks only wait has ended once nothing sounds. So has one whose tracks pass still. A held
        // sound ends kHoldSeconds after it stops changing, without loop markers. A track that waits for ever holds such
        // a sound. So does a track that passes still while its notes don't end by themselves, such as a tie struck
        // again in each pass. A player that loops on a pass would strike the notes again. A game's change that is still
        // to come can set such a track going. So can the last change, once the track's pass reads it. The driver's
        // rules count that pass as still until it ends, and it gets kHoldSeconds.
        const double now = s_.FrameMs(s_.frame);
        const bool changes_settled =
            GameChangesDone() && (options_.changes.empty() || now - s_.changed_ms >= kHoldSeconds * 1000.0);
        if (changes_settled && (Driver().IsStuck(0) || Driver().IsHolding(0)))
        {
            if (!AnySounding())
            {
                finished = true;
                break;
            }
            if (!steady_since)
            {
                steady_since = std::max(now, s_.last_change_ms);
            }
            if (now - *steady_since >= kHoldSeconds * 1000.0)
            {
                s_.out.holds = true;
                s_.out.DropLoop();
                break;
            }
        }
        else
        {
            steady_since.reset();
        }

        // A sequence that has been silent for longer than kHoldSeconds at the time limit has finished. It ends with
        // its last sound.
        if (now >= kMaxSeconds * 1000.0)
        {
            finished = detail::FinishedAtTimeLimit(s_, now, AnySounding());
            s_.out.truncated = !finished;
            break;
        }

        const snd::Player& player = Driver().GetPlayer(0);
        if (s_.ticked && ((static_cast<uint32_t>(player.tempo) * player.tempo_ratio) >> 8) == 0)
        {
            s_.out.warnings.push_back("tempo 0: the sequence stops advancing");
            s_.out.holds = AnySounding();
            break;
        }
    }

    const bool has_next = s_.out.loop && stop_ && !held_sound_end_;
    const PassStart next_pass =
        has_next ? ntdlib::sequence_export::PassStartAt(s_.out, s_.recorder.PartSwitches(), s_.out.loop->end)
                 : PassStart{};

    // CC10, CC11 and CC1 come back at the loop's start where their stand does (StandReturns).
    std::set<ntdlib::sequence_export::SettingKey> stands_return;
    if (s_.out.loop)
    {
        const seqlib::midi::Performance test = TestList(s_.out, s_.stands);
        const PassStart test_start =
            ntdlib::sequence_export::PassStartAt(test, s_.recorder.PartSwitches(), s_.out.loop->start);
        const PassStart test_next =
            ntdlib::sequence_export::PassStartAt(test, s_.recorder.PartSwitches(), s_.out.loop->end);
        for (const auto& [key, at_start] : test_start.settings)
        {
            const auto [t, part, kind] = key;
            if (IsLfoSetting(kind) &&
                StandReturns(test, test_start, has_next ? &test_next : nullptr, s_.out.loop->end, t, part, kind))
            {
                stands_return.insert(key);
            }
        }
    }
    EndPerformance(finished);
    if (finished)
    {
        s_.out.DropLoop();
    }
    if (s_.out.loop)
    {
        ntdlib::sequence_export::LoopRules rules = kFrameLoopRules;
        rules.restores = [&stands_return](int track, uint8_t part, EventKind kind) -> std::optional<bool>
        {
            if (!IsLfoSetting(kind))
            {
                return std::nullopt;
            }
            return stands_return.contains({track, part, kind});
        };
        ntdlib::sequence_export::RestoreAtLoopStart(s_.out, s_.recorder.PartSwitches(), next_pass, rules);

        // Each repeat plays the notes that the loop's pass started. The game drops others in later passes.
        if (const std::size_t left_out = s_.main_loop.LeftOut(); left_out > 0)
        {
            const std::string kWhat =
                "note that gets no channel in one pass and plays in the next (repeats play the loop's)";
            s_.out.approximations[kWhat] += left_out;
        }
    }
    detail::AddRecorderNotes(s_.out, s_.recorder);

    return std::move(s_.out);
}

} // namespace

Performance Perform(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                    const PerformOptions& options)
{
    // The runs settle the level scale and the parts (RunUntilSettled).
    const auto make = [&](double level_scale, const std::array<uint8_t, 16>& part_limits, const LoopReplay& replay)
    {
        return Performer(archive, sequence, options, level_scale, part_limits, replay);
    };

    return ntdlib::sequence_export::detail::PerformUntilSettled<Performance>(make);
}

} // namespace ntdlib::sequence_export::ntr
