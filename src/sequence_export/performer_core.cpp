// SPDX-License-Identifier: MIT

// The performer: what it takes from the core after a tick, such as its channels, waves, LFOs and tails, and the move
// to the next tick (see performer.cpp).

#include "performer_internal.h"

namespace ntdlib::sequence_export
{

Note* Performer::NewestNoteOn(int t, nwsnd::ChannelId channel)
{
    if (t < 0 || static_cast<std::size_t>(t) >= tracks_.size())
    {
        return nullptr;
    }

    const auto on_channel = [channel](const Note& n)
    {
        return n.core_channel == channel;
    };
    for (std::vector<Note>* list : {&tracks_[t].notes, &tracks_[t].detached})
    {
        const auto found = std::find_if(list->rbegin(), list->rend(), on_channel);
        if (found != list->rend())
        {
            return &*found;
        }
    }

    return nullptr;
}

template <typename Core>
const typename Core::Channel* Performer::CoreChannelOf(Core& core, const Note& n)
{
    if (n.core_channel == nwsnd::kNoChannel || n.core_key < 0)
    {
        return nullptr;
    }

    // A freed channel's place can hold a newer channel. The note's channel still plays the voice with the note's key.
    const auto& channels = core.Channels();
    const auto& in_use = channels.InUse();
    if (std::find(in_use.begin(), in_use.end(), n.core_channel) == in_use.end())
    {
        return nullptr;
    }

    const auto& channel = channels.At(n.core_channel);
    if (channel.voice == nwsnd::kNoVoice)
    {
        return nullptr;
    }

    return core.PositionKey(channel.voice) == n.core_key ? &channel : nullptr;
}

template <typename Core>
void Performer::TakeCoreWaves(Core& core)
{
    for (Track& tr : tracks_)
    {
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                const auto end = n.core_key >= 0 ? core.PositionOf(n.core_key) : std::nullopt;
                if (end)
                {
                    n.one_shot = end->one_shot;
                    n.core_end = end->frame;
                }
            }
        }
    }
}

template <typename Core>
void Performer::TakeCoreLfos(Core& core, uint64_t frame)
{
    if (midi_.earlier_.lfo_changes.empty())
    {
        return;
    }

    for (Track& tr : tracks_)
    {
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                const auto* channel = n.midi.lfo_bends ? CoreChannelOf(core, n) : nullptr;
                if (!channel)
                {
                    continue;
                }

                TakeNoteLfo(core, n, *channel, frame);
            }
        }
    }
}

template <typename Core>
void Performer::TakeNoteLfo(Core& core, Note& n, const typename Core::Channel& channel, uint64_t frame)
{
    // Channel::Update adds the pitch LFO's value to the frame's pitch before the LFO steps on. A copy of
    // the LFO gives the value. The copy draws from a copy of the random generator. The core's generator
    // stays as it was. A random curve's numbers come from that copy. They can differ from those in the
    // game. A note reports such a curve once, when the curve first moves the note's pitch.
    float value = 0.0f;
    if (channel.lfo_types[0] == 0)
    {
        auto lfo = channel.lfos[0];
        nwsnd::Random random = core.Rng();
        value = lfo.GetValue(random);
        if (lfo.param.curve == 4 && value != 0.0f && !n.midi.lfo_random)
        {
            n.midi.lfo_random = true;
            midi_.Approximate("random LFO curve in pitch bends (its numbers can differ from those in the game)");
        }
    }

    // The note keeps the values from frame_ - 1 on.
    const uint64_t kept = frame_ > 0 ? frame_ - 1 : 0;
    if (n.lfo_pitch.empty() || n.lfo_frame + n.lfo_pitch.size() != frame)
    {
        n.lfo_pitch.clear();
        n.lfo_frame = frame;
    }
    else if (n.lfo_frame < kept)
    {
        const uint64_t gone = std::min<uint64_t>(kept - n.lfo_frame, n.lfo_pitch.size());
        n.lfo_pitch.erase(n.lfo_pitch.begin(), n.lfo_pitch.begin() + static_cast<std::ptrdiff_t>(gone));
        n.lfo_frame += gone;
    }
    n.lfo_pitch.push_back(value);
}

template <typename Core>
void Performer::RunCoreTail(Core& core, uint64_t until)
{
    const auto playing = [this, &core](uint64_t frame)
    {
        for (const Track& tr : tracks_)
        {
            for (const auto* list : {&tr.notes, &tr.detached})
            {
                for (const Note& n : *list)
                {
                    if (n.core_key >= 0 && n.one_shot && !n.WaveEndedBy(frame))
                    {
                        return true;
                    }
                    if (n.midi.lfo_bends && static_cast<double>(frame) * settings_.frame_ms < n.EndMs() &&
                        CoreChannelOf(core, n))
                    {
                        return true;
                    }
                }
            }
        }

        return false;
    };

    for (uint64_t frame = frame_; frame < until && playing(frame); frame++)
    {
        if (frame > frame_)
        {
            core.BeginFrame();
        }
        if (core.GetPlayer().IsActive())
        {
            core.GetPlayer().UpdateTrackParams(core);
        }
        TakeCoreLfos(core, frame);
        core.FinishFrame();
        TakeCoreWaves(core);
    }
}

void Performer::FollowRandomDraw()
{
    if (stop_)
    {
        return;
    }

    last_draw_ = tick_;
    midi_.out_.draws = true;
    last_draw_order_ = commands_run_;
}

void Performer::FollowVariableRead(int t, int index)
{
    if (stop_)
    {
        return;
    }

    NoteVariableRead(t, static_cast<uint8_t>(index));
}

void Performer::FollowNoteCommand(int t, int key, int velocity, int32_t length)
{
    if (stop_)
    {
        return;
    }

    // The note waits for the core's verdict on its channel (SettleNote). The performer takes the track's waits after
    // the note from the core (TakeCoreWaits).
    const Track& tr = tracks_[t];
    note_to_pair_ = -1;
    if (!tr.mute)
    {
        const auto stretch = static_cast<uint32_t>(midi_.out_.stretches[t].size() - 1);
        pending_note_ = PendingNote{t, key, velocity, length, {tr.command, stretch}};
    }
}

void Performer::SettleNote(bool drop)
{
    if (!pending_note_)
    {
        return;
    }

    // A note that NoteOn starts waits for the core's channel (FollowNoteOn).
    const PendingNote note = *pending_note_;
    pending_note_.reset();
    settling_command_ = note.command;
    const uint64_t started = notes_started_;
    if (NoteOn(note.t, note.key, note.velocity, note.length > 0 ? note.length : -1, drop))
    {
        tracks_[note.t].note_at = commands_run_;
    }
    if (notes_started_ != started)
    {
        note_to_pair_ = note.t;
    }
}

template <typename Core>
void Performer::TakeCoreTrack(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.open = track->IsOpen();
    tr.current = track->Current();
    TakeCoreWaits(core, t);
    const auto calls = track->CallStack();
    tr.call_depth = static_cast<int>(calls.size());
    std::ranges::copy(calls, tr.calls.begin());

    tr.tie = track->Tie();
    tr.mono = track->Mono();
    tr.bank_slot = track->BankIndex();
    tr.program = track->ProgramNumber();
    TakeCoreRamps(core, t);
    tr.bend_range = track->BendRange();
    tr.init_pan = track->InitPan();
    tr.lfos = track->LfoParams();
    tr.lfo_types = track->LfoTypes();

    tr.attack = track->Attack();
    tr.decay = track->Decay();
    tr.sustain = track->Sustain();
    tr.release = track->Release();
    tr.hold = static_cast<uint8_t>(track->Hold());
    tr.main_send = track->MainSend();
    tr.fx_send_a = track->FxSends()[0];
    tr.fx_send_b = track->FxSends()[1];
    tr.fx_send_c = track->FxSends()[2];
    tr.handle_volume = track->HandleVolume();

    // The core keeps the filters' values as nw::snd uses them. The performer keeps the commands' arguments. The player
    // adds the sound's low-pass to the track's low-pass. The argument therefore stands for the sum.
    tr.low_pass = LowPassArgument(track->Lpf(), options_.lpf, settings_);
    tr.biquad_type = static_cast<uint8_t>(track->BiquadType());
    tr.biquad_value = static_cast<int32_t>(std::lround(track->BiquadValue() * 127.0f));

    tr.note_wait = track->NoteWait();
    tr.mute = track->IsMuted();
    tr.damper = track->Damper();
    tr.velocity_range = track->VelocityRange();
    tr.vars = track->Variables();
}

template <typename Core>
void Performer::TakeCoreRamps(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.vol = track->Volume();
    tr.volume2 = track->Volume2();
    tr.pan = track->Pan();
    tr.bend = track->Bend();
}

template <typename Core>
void Performer::TakeCoreWaits(Core& core, int t)
{
    const auto* track = core.GetPlayer().Track(t);
    if (!track)
    {
        return;
    }

    Track& tr = tracks_[t];
    tr.wait = track->Wait();
    tr.note_finish_wait = track->NoteFinishWait();
    tr.holds = track->Holds();
    tr.held_wait = track->HeldWait();
}

template <typename Core>
void Performer::TakeCorePlayer(Core& core)
{
    const auto& player = core.GetPlayer();
    tempo_ = player.Tempo();
    timebase_ = player.Timebase();
    main_volume_ = player.MainVolume();
    player_vars_ = player.LocalVariables();
    global_vars_ = core.GlobalVariables();
}

void Performer::FinishTick()
{
    WithCore([&](auto& core) { core.GetPlayer().EndTick(core, core_open_); });

    for (int t = 0; t < kTrackCount; t++)
    {
        midi_.OnTickEnd(*this, t);
        if (!tracks_[t].open)
        {
            PruneNotes(t);
        }
    }

    // The Wii's build of October 2006 stops a released note once its volume times its envelope is 0, whether a track
    // owns it or not (Channel::Update, 0x800e039c in Smooth Moves' main.dol). The later builds stop a note that a track
    // owns only when its envelope is silent. The channels update after the frame's last tick. The performer stops only
    // the notes that a track owns.
    if (WiiOctober2006() && frame_ticks_left_ == 0)
    {
        for (int t = 0; t < kTrackCount; t++)
        {
            const float user = UserVolume(t);
            for (Note& n : tracks_[t].notes)
            {
                if (n.released && n.EndMs() > now_ms_ && n.init_volume * user == 0.0f)
                {
                    StopNote(t, n);
                }
            }
        }
    }
}

void Performer::ReleasePending(const TickTime& next)
{
    if (!settings_.note_off_waits_for_update || next.frame == frame_)
    {
        return;
    }

    const uint64_t frame = frame_ + 1;
    const double after = (static_cast<double>(frame) * settings_.frame_ms - now_ms_) / (next.ms - now_ms_);
    for (int t = 0; t < kTrackCount; t++)
    {
        for (Note& n : tracks_[t].notes)
        {
            if (!n.release_pending)
            {
                continue;
            }

            n.release_pending = false;
            n.released = true;
            n.released_frame = frame;
            n.release_level = HeldLevel(n.env, 1, settings_);
            n.release_rate = DecayRate(n.env.release, settings_.engine);
            n.release_end_ms =
                static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate, settings_) + 1) *
                settings_.frame_ms;
            tracks_[t].last_change = tick_;
            midi_.OnDeferredRelease(*this, t, n, after);
        }
    }
}

void Performer::AdvanceTo(const TickTime& next)
{
    // The core ends the last tick's frame, plays the frames without a tick, and begins the next tick's frame.
    const auto play_frames = [&](auto& core)
    {
        for (uint64_t frame = frame_; frame < next.frame; frame++)
        {
            if (frame > frame_)
            {
                core.BeginFrame();
            }
            if (core.GetPlayer().IsActive())
            {
                core.GetPlayer().UpdateTrackParams(core);
            }
            TakeCoreLfos(core, frame);
            core.FinishFrame();
        }

        TakeCoreWaves(core);
        core.BeginFrame();
    };
    if (next.frame > frame_)
    {
        WithCore(play_frames);
    }

    ReleasePending(next);
    midi_.OnBetweenTicks(*this, next);

    frame_ = next.frame;
    now_ms_ = next.ms;
    if (WiiOctober2006())
    {
        frame_ticks_left_ = next.ticks_left;
        nominal_ms_ = next.nominal_ms;
    }

    tick_++;
    midi_.RecordTickTime(*this);
}

} // namespace ntdlib::sequence_export
