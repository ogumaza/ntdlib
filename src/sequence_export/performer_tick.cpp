// SPDX-License-Identifier: MIT

// The performer: the core's set-up, and a tick's variables, procedures, commands and notes (see performer.cpp).

#include "performer_internal.h"

namespace ntdlib::sequence_export
{

void Performer::AllocateTracks()
{
    // A track's low-pass starts at 0. The sound's low-pass applies from the track's first note, before any 0xd8
    // command.
    for (int t = 0; t < kTrackCount; t++)
    {
        tracks_[t].Init();
        tracks_[t].allocated = (sound_.allocate_track_flags >> t) & 1;
        if (options_.lpf != 0.0f)
        {
            tracks_[t].low_pass = LowPassArgument(0.0f, options_.lpf, settings_);
            SetFilter(t);
        }
    }

    // The Wii allocates only the tracks that the sound's flags select (SeqPlayer::Setup, main.dol 0x8003e1f4).
    if (Wii())
    {
        return;
    }

    tracks_[0].allocated = true;

    // A leading alloctrack (0xfe, 16-bit mask) also allocates tracks. The start offset can be 0xffffffff. Guard against
    // addition wrapping.
    const std::size_t start = sound_.start_offset;
    if (start < seq_.data.size() && seq_.data.size() - start > 2 && seq_.data[start] == 0xfe)
    {
        const uint32_t first = seq_.data[start + 1], second = seq_.data[start + 2];
        const uint32_t mask = ArgumentsBigEndian() ? first << 8 | second : second << 8 | first;
        for (int t = 0; t < kTrackCount; t++)
        {
            if ((mask >> t) & 1)
            {
                tracks_[t].allocated = true;
            }
        }
    }
}

template <typename Core>
void Performer::MakeCore(Regions& banks, int channels, int voices)
{
    Core& core = core_.template emplace<Core>(banks, channels, voices);

    // The seed is the random state when the game starts the sound. On the Switch, a sound starts in the second sound
    // frame after that: the first takes the start to the sound thread. The state steps once a frame
    // (SoundThread::FrameProcess, 0xc18590). The first tick draws from the state one step on.
    core.Rng() = nwsnd::Random(options_.seed);
    for (int i = 0; i < settings_.random_steps_before_start; i++)
    {
        core.Rng().Next();
    }

    core.GlobalVariables().fill(Wii() ? 0 : -1);
    if constexpr (std::is_same_v<Core, RevolutionCore>)
    {
        core.build_ = settings_.wii_build;
    }
}

void Performer::StartTick()
{
    ChangeVariables();

    // The core begins its tick: SequenceSoundPlayer::ParseNextTick (0x022B70AC) moves the main volume a tick on before
    // the tracks' ticks. The performer takes the main volume from the core.
    const auto begin_tick = [&](auto& core)
    {
        core.now_ms_ = now_ms_;
        core.GetPlayer().BeginTick();
        core_open_ = false;
        main_volume_ = core.GetPlayer().MainVolume();
    };
    WithCore(begin_tick);
}

void Performer::ChangeVariables()
{
    // A write in a user procedure waits from its frame for a call. A pass that started before the frame plays without
    // the write, and the passes after it can take the write. As after a change, the earlier pass doesn't count.
    const std::vector<ProcedureChange>& waits = options_.procedure_changes;
    if (armed_procedure_changes_ < waits.size() && waits[armed_procedure_changes_].frame <= frame_)
    {
        while (armed_procedure_changes_ < waits.size() && waits[armed_procedure_changes_].frame <= frame_)
        {
            ++armed_procedure_changes_;
        }
        changed_at_ = tick_;
        main_loop_.Restart(midi_.out_, std::nullopt);
    }

    const std::vector<VariableChange>& changes = options_.changes;
    if (next_change_ == changes.size() || changes[next_change_].frame > frame_)
    {
        return;
    }

    // The core and the performer take each change of a player or a global variable.
    while (next_change_ < changes.size() && changes[next_change_].frame <= frame_)
    {
        const VariableChange& change = changes[next_change_++];
        if (change.index < 32)
        {
            *Variable(0, change.index) = change.value;
            WithCore([&](auto& core) { core.GetPlayer().SetVariable(core, change.index, change.value); });
        }
        else if (change.index < 48)
        {
            WithCore([&](auto& core) { SetTrackVariables(core, change.index, change.value); });
        }
    }

    // The passes of the main loop so far can play otherwise than the passes after the change. The search for the main
    // loop starts again with the first pass that starts after it (OnLoop).
    changed_at_ = tick_;
    main_loop_.Restart(midi_.out_, std::nullopt);
}

void Performer::ChangeInProcedure(uint16_t procedure)
{
    const std::vector<ProcedureChange>& changes = options_.procedure_changes;
    bool changed = false;
    for (std::size_t i = 0; i < armed_procedure_changes_; ++i)
    {
        const ProcedureChange& change = changes[i];
        if (procedure_taken_[i] || change.procedure != procedure)
        {
            continue;
        }

        procedure_taken_[i] = true;
        changed = true;
        if (change.index < 32)
        {
            *Variable(0, change.index) = change.value;
            WithCore([&](auto& core) { core.GetPlayer().SetVariable(core, change.index, change.value); });
        }
    }

    // As after a change before a frame (ChangeVariables).
    if (changed)
    {
        changed_at_ = tick_;
        main_loop_.Restart(midi_.out_, std::nullopt);
    }
}

void Performer::RunUserProcedure(int t, uint16_t procedure)
{
    if (!options_.user_procedures)
    {
        return;
    }

    // The core holds the variables as the track's commands left them. The performer takes them from it after each
    // command.
    std::array<int16_t, 16> local = player_vars_;
    std::array<int16_t, 16> global = global_vars_;
    std::array<int16_t, 16> track = tracks_[t].vars;
    const auto take_variables = [&](auto& core)
    {
        local = core.GetPlayer().LocalVariables();
        global = core.GlobalVariables();
        if (const auto* core_track = core.GetPlayer().Track(t))
        {
            track = core_track->Variables();
        }
    };
    WithCore(take_variables);

    const std::array<std::array<int16_t, 16>, 3> before = {local, global, track};
    if (options_.user_procedures(procedure, local, global, track))
    {
        last_draw_ = tick_;
        last_draw_order_ = commands_run_;
    }

    const std::array<const std::array<int16_t, 16>*, 3> after = {&local, &global, &track};
    for (std::size_t set = 0; set < after.size(); ++set)
    {
        for (std::size_t i = 0; i < 16; ++i)
        {
            const int16_t value = (*after[set])[i];
            if (value == before[set][i])
            {
                continue;
            }

            const auto index = static_cast<uint8_t>(set * 16 + i);
            *Variable(t, index) = value;
            const auto set_variable = [&](auto& core)
            {
                if (index < 32)
                {
                    core.GetPlayer().SetVariable(core, index, value);
                }
                else
                {
                    core.GetPlayer().SetTrackVariable(t, index - 32, value);
                }
            };
            WithCore(set_variable);
            NoteVariableChange(t, index);
        }
    }
}

bool Performer::ParseTracks(int first)
{
    bool any_open = false;
    WithCore([&](auto& core) { any_open = ParseCoreTracks(core, first); });

    return any_open;
}

template <typename Core>
bool Performer::ParseCoreTracks(Core& core, int first)
{
    core.performer_ = this;
    bool any_open = false;
    for (int t = first; t < kTrackCount && !stop_; t++)
    {
        // The core's tick of the track counts its channels' lengths down (FollowLengths), moves the track's ramps on
        // and runs the commands. The performer takes the ramps from the core (FollowCommandStart).
        Track& tr = tracks_[t];
        const bool was_open = tr.open;
        const uint64_t commands = commands_run_;
        counting_lengths_ = true;
        const bool open = core.GetPlayer().RunTrackTick(core, t);
        counting_lengths_ = false;
        core_open_ = open || core_open_;

        // A pass that stops the performance stops in the middle of the track's tick, at a jump (Run). The core's track
        // stops at the same command (StopsTick). The performer's track keeps its state.
        if (stop_)
        {
            break;
        }

        any_open = FinishCoreTrackTick(core, t, was_open, open, commands_run_ != commands) || any_open;
    }

    return any_open;
}

template <typename Core>
void Performer::ContinueCoreTrack(Core& core, int t)
{
    core.performer_ = this;
    const uint64_t commands = commands_run_;
    const bool open = core.GetPlayer().ContinueTrackTick(core, t);
    core_open_ = open || core_open_;
    FinishCoreTrackTick(core, t, true, open, commands_run_ != commands);
}

template <typename Core>
bool Performer::FinishCoreTrackTick(Core& core, int t, bool was_open, bool open, bool ran)
{
    SettleNote(false);
    Track& tr = tracks_[t];
    if (was_open && !open)
    {
        CloseTrack(t);
        tr.allocated = false; // the player frees a finished track
    }
    else if (ran)
    {
        // A track that ran its commands up to a wait waits again. One that stopped at the parse limit reads on in the
        // next tick.
        const auto* track = core.GetPlayer().Track(t);
        if (track && (track->Wait() != 0 || track->NoteFinishWait()))
        {
            tr.last_wait = tick_;
        }
    }

    TakeCoreTrack(core, t);
    TakeCorePlayer(core);

    return was_open && open;
}

void Performer::FollowCommandStart(int t, uint32_t offset, uint32_t command, bool conditional)
{
    SettleNote(false);
    if (stop_)
    {
        return;
    }

    Track& tr = tracks_[t];
    NumberCalls(t);
    last_run_[t][offset] = {tick_, commands_run_++, InnermostCall(t)};

    // The command runs on in the track's stretch of commands, or begins the next stretch (Performance::stretches).
    std::vector<Stretch>& stretches = midi_.out_.stretches[t];
    tr.command = offset;
    if (tr.next_stretch)
    {
        stretches.push_back({.tick = tick_,
                             .first = offset,
                             .last = offset,
                             .entry = *tr.next_stretch,
                             .depth = static_cast<uint8_t>(tr.call_depth),
                             .note_wait = tr.note_wait});
        tr.next_stretch.reset();
    }
    else
    {
        stretches.back().last = offset;
    }

    // A conditional jump, call, return or fin can leave a loop (see OnLoop).
    if (conditional && (command == 0x89 || command == 0x8a || command == 0xfd || command == 0xff))
    {
        tr.leave_at = commands_run_;
    }

    // The core moved the track's ramps on at the start of its tick. The note before the command set the track's waits,
    // and the command's start has ended the wait for a held note.
    const auto take_ramps_and_waits = [&](auto& core)
    {
        TakeCoreRamps(core, t);
        TakeCoreWaits(core, t);
    };
    WithCore(take_ramps_and_waits);
}

void Performer::FollowCommand(int t, uint32_t command, int32_t arg1, int32_t arg2, uint32_t next)
{
    // The extended command 0xe0 calls into the game. The performer records nothing for it. The model of the game's
    // procedure runs (RunUserProcedure), and then the game's writes in the procedure take effect (ChangeInProcedure).
    if (!stop_ && command == 0xf0e0)
    {
        RunUserProcedure(t, static_cast<uint16_t>(arg1));
        ChangeInProcedure(static_cast<uint16_t>(arg1));
    }
    if (stop_ || command == 0xf0e0)
    {
        return;
    }

    tracks_[t].current = next;
    WithCore([&](auto& core) { RecordCoreCommand(core, t, command, arg1, arg2); });

    // A command that moved the track elsewhere ends the track's stretch of commands.
    if (tracks_[t].current != next)
    {
        tracks_[t].next_stretch = EntryAfter(command);
    }
}

void Performer::ClosePass(int t, uint32_t cmd, int32_t arg1)
{
    const Track& tr = tracks_[t];
    if (cmd == 0x89 && static_cast<uint32_t>(arg1) < tr.current)
    {
        OnLoop(t, static_cast<uint32_t>(arg1), true);
    }
    if (cmd == 0xfc && tr.call_depth != 0)
    {
        // The Wii takes the top of the call stack as the loop, whether a loop start or a call put it there
        // (kLoopEndIgnoresCalls in nwsnd's policies). A counted loop in endless_loops_ closes a pass like a loop of
        // count 0.
        const nwsnd::CallStackEntry& e = tr.calls[tr.call_depth - 1];
        if ((e.is_loop || Wii()) && (e.loop_count == 0 || endless_loops_.contains({t, e.address})))
        {
            OnLoop(t, e.address, false);
        }
    }
}

void Performer::RecordVariableCommand(int t, uint32_t op, uint8_t index, bool changed)
{
    if (op >= 0x90)
    {
        NoteVariableRead(t, index);
    }
    if (changed)
    {
        NoteVariableChange(t, index);
    }
}

template <typename Core>
void Performer::RecordCoreCommand(Core& core, int t, uint32_t cmd, int32_t arg1, int32_t arg2)
{
    // A jump back, and the end of an endless loop, close a pass of a loop (OnLoop). The analysis takes the track's
    // position and loops from before the command.
    Track& tr = tracks_[t];
    ClosePass(t, cmd, arg1);

    // A variable command's variable before the command, and the track's LFOs.
    const uint32_t op = cmd & 0xff;
    const bool lfo_command = (op >= 0xa0 && op <= 0xb1) || (op >= 0xe1 && op <= 0xe6);
    const uint8_t index = static_cast<uint8_t>(arg1);
    const int16_t* variable = cmd > 0xff && !lfo_command ? Variable(t, index) : nullptr;
    const int16_t before = variable ? *variable : 0;
    const PresetKey preset_before = TrackPresetKey(tr);

    TakeCoreTrack(core, t);
    TakeCorePlayer(core);
    if (cmd > 0xff)
    {
        if (variable)
        {
            RecordVariableCommand(t, op, index, *variable != before);
        }

        return;
    }

    switch (cmd)
    {
    case 0x88: // opentrack: a track that reached FIN was freed and can't be opened again
        if (arg1 < kTrackCount && arg1 != t && tracks_[arg1].allocated)
        {
            CloseTrack(arg1);
            OpenTrack(arg1, static_cast<uint32_t>(arg2));
            TakeCoreTrack(core, arg1);
        }
        break;

    case 0xb0: // timebase: the Wii's build of October 2006 has no such command
        if (!WiiOctober2006())
        {
            midi_.OnTimebase(*this);
        }
        break;

    case 0xb2:
        if (tr.mono)
        {
            ReleaseAndDetach(t);
        }
        break;

    case 0xc8:
        ReleaseAndDetach(t);
        break;

    case 0xb4:
    case 0xb5:
    case 0xd8:
        SetFilter(t);
        break;

    case 0xbd:
    case 0xbe:
    case 0xcb:
    case 0xcc:
    case 0xcd:
    case 0xe0:
    case 0xe4:
        if (!SameLfo(TrackPresetKey(tr), preset_before))
        {
            midi_.GiveStruckNotesLfo(*this, t);
        }
        break;

    case 0xdd:
        MuteNotes(t, static_cast<uint8_t>(arg1));
        break;

    case 0xe1:
        midi_.OnTempo(*this);
        break;

    default:
        break;
    }

    midi_.OnCommand(*this, t, cmd, arg1);
}

void Performer::FollowNoteOn(int t, nwsnd::ChannelId channel, int key)
{
    if (pending_note_ && pending_note_->t == t)
    {
        settling_channel_ = channel;
        SettleNote(false);
        settling_channel_ = nwsnd::kNoChannel;
    }

    if (note_to_pair_ == t && !tracks_[t].notes.empty())
    {
        tracks_[t].notes.back().core_channel = channel;
        tracks_[t].notes.back().core_key = key;
    }
    note_to_pair_ = -1;
}

void Performer::FollowLegato(int t, nwsnd::ChannelId channel)
{
    if (pending_note_ && pending_note_->t == t)
    {
        settling_channel_ = channel;
        SettleNote(false);
        settling_channel_ = nwsnd::kNoChannel;
    }
}

void Performer::FollowNoChannel(int t)
{
    if (pending_note_ && pending_note_->t == t)
    {
        SettleNote(VoiceLimit() > 0);
    }
}

void Performer::FollowChannelDrop(int t, nwsnd::ChannelId channel)
{
    if (stop_ || VoiceLimit() == 0)
    {
        return;
    }

    // A note past its track's cap for one tick takes a voice from a note that the performer keeps. NoteOn omits the
    // newer note: the cap keeps a tick's first notes. Where a new note takes the voice of the lowest priority, the game
    // keeps the newest (TickCapReport).
    if (pending_note_)
    {
        const Track& taker = tracks_[pending_note_->t];
        const int started = taker.note_tick == tick_ ? taker.tick_notes : 0;
        if (started >= TickNoteCap())
        {
            return;
        }
    }

    // The track's newest note on the channel. An older note's channel has ended, and a newer channel can take its
    // place.
    auto& notes = tracks_[t].notes;
    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    const auto found = std::find_if(notes.rbegin(), notes.rend(), on_channel);
    if (found != notes.rend())
    {
        StopNote(t, *found);
        found->core_channel = nwsnd::kNoChannel;
    }
}

void Performer::FollowGainAboveFull(int t, nwsnd::ChannelId channel)
{
    Note* note = NewestNoteOn(t, channel);
    if (note == nullptr || note->midi.lfo_above_full)
    {
        return;
    }

    note->midi.lfo_above_full = true;
    midi_.Approximate(note->midi.lfo_fit != VolumeLfoFit{}
                          ? "volume LFO above full level (limited in the game; the SoundFont and SFZ swing less, from "
                            "a lower level)"
                          : "volume LFO above full level (limited in the game; the SoundFont and SFZ swing above it)");
}

void Performer::FollowVolumeLfo(int t, nwsnd::ChannelId channel, float envelope_db, float lfo_db)
{
    // A note without a MIDI note has id 0.
    const Note* note = NewestNoteOn(t, channel);
    if (note == nullptr || note->midi.id == 0)
    {
        return;
    }

    const Track& tr = tracks_[t];
    const int depth = LfoDepth(tr.lfos[0]);
    const double scale = depth != 0 ? 2.0 * ModulationValue(tr.lfos[0], tr.lfo_types[0]) / depth : 0.0;
    midi_.OnVolumeLfo(note->midi.id, envelope_db, lfo_db, scale);
}

} // namespace ntdlib::sequence_export
