// SPDX-License-Identifier: MIT

// Sequence execution (see performer.h). A core of nw::snd (nwsnd/) plays the sequence. The performer follows the core's
// hooks and records the notes and controllers. Addresses without another source come from the Wii U's nw::snd in Animal
// Crossing: amiibo Festival's executable. The Switch's nn::atk in WarioWare: Get It Together!'s executable is the same
// player with 5 ms frames, arguments in the file's byte order and the Switch's audio renderer. Settings selects the
// console.
//
// This file contains the performer's set-up, its notes and its tracks. performer_passes.cpp, performer_tick.cpp,
// performer_core.cpp and performer_run.cpp contain the rest.

#include "performer_internal.h"

namespace ntdlib::sequence_export
{

Performer::Performer(const Settings& settings, const SoundInfo& sound, const Sequence& seq, Regions& banks,
                     const PerformOptions& options, const PassFindings& earlier, double level_scale,
                     const std::array<uint8_t, kTrackCount>& part_limits, const std::vector<LoopChoice>& loop_choices)
    : settings_(settings), sound_(sound), seq_(seq), banks_(banks), options_(options), main_loop_(loop_choices)
{
    midi_.rec_.SetLevelScale(level_scale);
    midi_.earlier_ = earlier;
    midi_.rec_.SetPartLimits(part_limits);
    for (Track& tr : tracks_)
    {
        tr.engine = settings_.engine;
    }

    // The game's changes come in the order of their frames, and so do its writes in user procedures.
    std::ranges::stable_sort(options_.changes, {}, &VariableChange::frame);
    std::ranges::stable_sort(options_.procedure_changes, {}, &ProcedureChange::frame);
    procedure_taken_.assign(options_.procedure_changes.size(), false);

    main_volume_.Init(127);

    // The Wii's stereo waves play in balance mode. The build of October 2006 has only dual mode.
    if (Wii())
    {
        balance_ = !WiiOctober2006() && sound.pan_mode == 1;
        midi_.out_.since_march_2008 = settings_.wii_build.SinceMarch2008();
        midi_.out_.balance = balance_;
    }

    // The core plays the sequence (MakeCore). On the Wii U and the Switch, each channel has a voice. With voice
    // drops, the cores of the Wii U, the Switch and the Wii have the game's voices. Without, the cores have room
    // for kEveryNote notes at once. With voice drops, the Wii's core has NW4R SND's channels: one per voice, and
    // one more. The 3DS's core has room for kEveryNote channels. A channel there plays only while it holds a voice.
    // Only the voices run out. With voice drops, the 3DS's core has the game's voices. A new note then takes the
    // voice of the lowest priority.
    const int count = VoiceLimit() > 0 ? VoiceLimit() : kEveryNote;
    if (settings_.engine == Engine::kCafe)
    {
        MakeCore<CafeCore>(banks, count, count);
    }
    else if (settings_.engine == Engine::kSwitch)
    {
        MakeCore<NxCore>(banks, count, count);
    }
    else if (settings_.engine == Engine::kCtr)
    {
        MakeCore<CtrCore>(banks, kEveryNote, VoiceLimit() > 0 ? VoiceLimit() : kEveryNote);
    }
    else if (Wii())
    {
        MakeCore<RevolutionCore>(banks, VoiceLimit() > 0 ? nwsnd::RevolutionPolicy::kChannelCount : count, count);
    }
}

int16_t* Performer::Variable(int t, uint8_t index)
{
    // Player variables 0-15, global 16-31 and track 32-47 (MmlParser::GetVariablePtr, 0x022CB600).
    if (index < 16)
    {
        return &player_vars_[index];
    }
    if (index < 32)
    {
        return &global_vars_[index - 16];
    }
    if (index < 48)
    {
        return &tracks_[t].vars[index - 32];
    }

    return nullptr;
}

template <typename Core>
void Performer::SetTrackVariables(Core& core, uint8_t index, int16_t value)
{
    core.GetPlayer().SetVariable(core, index, value);
    for (int t = 0; t < kTrackCount; t++)
    {
        if (core.GetPlayer().Track(t))
        {
            tracks_[t].vars[index - 32] = value;
        }
    }
}

PresetKey Performer::NoteKey(int t, const Note& n) const
{
    PresetKey pk = ChannelPresetKey(tracks_[t], n);
    pk.attack_timecents = n.midi.attack_timecents;
    pk.lift = n.midi.lift;
    pk.lfo_swing = n.midi.lfo_fit.swing;
    pk.lfo_cut = n.midi.lfo_fit.cut;
    pk.at_pitch_limits = n.midi.at_limits;
    if (midi_.earlier_.filter_moves[t])
    {
        pk.filter_controller = true;
    }
    else
    {
        pk.filter = midi_.tracks_[t].filter;
    }

    return pk;
}

void Performer::SetFilter(int t)
{
    Track& tr = tracks_[t];
    const SoundFontFilter filter =
        ToSoundFont(TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value, settings_), settings_);
    if (filter != midi_.tracks_[t].filter && TrackBusy(t))
    {
        midi_.filter_moves_[t] = true;
    }

    midi_.tracks_[t].filter = filter;
}

void Performer::OpenTrack(int t, uint32_t offset)
{
    // The rest of the track's state comes from the core (TakeCoreTrack). Open (0x022B69B8) clears the wait, the call
    // stack and the wait for notes to end on every console but the Wii (kOpenClearsState in nwsnd's policies).
    Track& tr = tracks_[t];
    tr.current = offset;
    tr.open = true;
    tr.opened = tick_;
    tr.next_stretch = Entry::kOpen;
}

void Performer::ReleaseNote(int t, Note& n, int release_value)
{
    // Channel::Release (0x022B6028) applies a new rate even to channels already releasing. Key groups depend on that.
    //
    // Envelopes advance Settings::env_step_ms per frame, from the note-on frame on. A channel stops in the frame it
    // falls below -90.4 dB and is gone before the next frame's ticks. While the core still has the channel, the core's
    // channel gives the envelope's level.
    const int64_t frame = static_cast<int64_t>(frame_);

    std::optional<float> core_level;
    const auto level_on_core = [&](auto& core)
    {
        if (const auto* channel = CoreChannelOf(core, n))
        {
            core_level = channel->envelope.Level();
        }
    };
    WithCore(level_on_core);

    n.release_pending = false;
    if (n.released)
    {
        if (release_value >= 0 && n.release_end_ms > now_ms_)
        {
            const int64_t steps = frame - static_cast<int64_t>(n.released_frame);
            const float level =
                core_level ? *core_level : ReleasedLevel(n.release_level, n.release_rate, steps, settings_);
            n.release_level = level;
            n.released_frame = frame_;
            n.release_rate = DecayRate(release_value, settings_.engine);
            n.release_end_ms =
                static_cast<double>(frame + ReleaseFrames(level, n.release_rate, settings_) + 1) * settings_.frame_ms;
        }

        return;
    }

    n.released = true;
    n.released_frame = frame_;
    n.release_level = core_level ? *core_level : HeldLevel(n.env, frame - static_cast<int64_t>(n.on_frame), settings_);
    n.release_rate = DecayRate(release_value >= 0 ? release_value : n.env.release, settings_.engine);
    n.release_end_ms =
        static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate, settings_) + 1) * settings_.frame_ms;
    midi_.OnRelease(*this, t, n);
}

bool Performer::Unheard(const Note& n) const
{
    return frame_ == n.on_frame;
}

bool Performer::StillAudible(const Note& n) const
{
    // A note-off that waits for the channel's first update starts the release in the frame after the tick's frame
    // (ReleasePending). Before the next tick, the release has taken no step.
    const int64_t frames = static_cast<int64_t>(frame_) - static_cast<int64_t>(n.released_frame);
    const double steps = static_cast<double>(std::max<int64_t>(frames, 0));
    const bool double_rate = settings_.engine == Engine::kCtr || settings_.engine == Engine::kRevolution;
    const double level = double_rate
                             ? n.release_level - static_cast<double>(n.release_rate) * settings_.env_step_ms * steps
                             : n.release_level - n.release_rate * settings_.env_step_ms * steps;
    return n.EndMs() > now_ms_ && level > -kInaudibleSustain;
}

bool Performer::Heard(const Note& n) const
{
    return n.released ? StillAudible(n) : n.EndMs() > now_ms_;
}

bool Performer::Alone(int t, const Note& n) const
{
    const Track& tr = tracks_[t];
    auto other_heard = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.midi.part == midi_.tracks_[t].output.part && Heard(other);
    };

    return std::ranges::none_of(tr.notes, other_heard) && std::ranges::none_of(tr.detached, other_heard);
}

bool Performer::PartSounds(int t, uint8_t part, const Note* except) const
{
    const Track& tr = tracks_[t];
    auto sounds = [&](const Note& n)
    {
        return &n != except && n.HasMidiNote() && n.midi.part == part && n.EndMs() > now_ms_;
    };

    return std::ranges::any_of(tr.notes, sounds) || std::ranges::any_of(tr.detached, sounds);
}

void Performer::ReleaseAndDetach(int t)
{
    // SequenceTrack::ReleaseAllChannel (0x022B608C) gives the channels the track's settings before it releases them.
    // The end of a tick does the same. So the MIDI part the notes sound on gets the tick's changes so far before the
    // track leaves it.
    midi_.OnReleaseAll(*this, t);
    for (Note& n : tracks_[t].notes)
    {
        ReleaseNote(t, n);
    }
    midi_.OnReleasedAll(*this, t);
}

bool Performer::OthersSounding(int t, const Note* except) const
{
    const Track& tr = tracks_[t];
    for (const Note& n : tr.notes)
    {
        if (&n != except && n.EndMs() > now_ms_ && !BelowHearing(n))
        {
            return true;
        }
    }

    for (const Note& n : tr.detached)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

void Performer::StopNote(int t, Note& n)
{
    // Channel::Stop (0x022C4278) silences immediately.
    midi_.OnStop(*this, t, n);
    n.released = true;
    n.release_end_ms = now_ms_;
}

void Performer::CloseTrack(int t)
{
    // Like the end of a tick, SequenceTrack::Close (0x022B6158) first gives the channels the track's settings. The
    // release tails start from the closing tick's volume, pan, pitch and filter. Close then releases every channel,
    // even one that ignores note-off.
    Track& tr = tracks_[t];
    midi_.OnTrackClosing(*this, t);
    for (Note& n : tr.notes)
    {
        ReleaseNote(t, n);
    }

    midi_.OnTrackClosed(*this, t);
    tr.open = false;
}

bool Performer::TrackBusy(int t) const
{
    for (const Note& n : tracks_[t].notes)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

bool Performer::Quiet(int t) const
{
    const Track& tr = tracks_[t];
    const uint32_t since = std::max(tr.last_change.value_or(0), tr.opened);
    return !OthersSounding(t, nullptr) && now_ms_ - tick_ms_[since] > kHoldSeconds * 1000.0;
}

bool Performer::WaitsForever(int t) const
{
    // A track that waits a negative number of ticks never reads on. That happens to a note or wait whose length comes
    // from a variable left at -1. A tick counts down only a positive wait. The track reads commands only while its
    // wait is 0 (SequenceTrack::ParseNextTick, 0x022B6F14).
    const Track& tr = tracks_[t];
    if (tr.held_wait || tr.wait < 0)
    {
        return true;
    }
    if (!tr.note_finish_wait || tr.notes.empty())
    {
        return false;
    }

    for (const Note& n : tr.notes)
    {
        if (n.length > 0 || n.EndMs() < kNever)
        {
            return false;
        }
    }

    return true;
}

bool Performer::Stuck() const
{
    // A note that hasn't been released sounds here until it has faded (MidiNote::faded_ms). The MIDI note of a tie ends
    // at the same point. The sequence's sound then ends where the note's MIDI note ends (EndPerformance).
    const auto sounding = [this](const Note& n)
    {
        return n.EndMs() > now_ms_ && (n.released || now_ms_ < n.midi.faded_ms);
    };
    const auto ends_later = [this](const Note& n)
    {
        return n.EndMs() > now_ms_;
    };
    for (int t = 0; t < kTrackCount; t++)
    {
        const Track& tr = tracks_[t];
        if ((tr.open && tr.wait >= 0 && !tr.held_wait) || std::ranges::any_of(tr.notes, sounding) ||
            std::ranges::any_of(tr.detached, ends_later))
        {
            return false;
        }
    }

    return true;
}

bool Performer::Steady() const
{
    // The main volume and volume2 ramp on the Wii U and the Switch (SequenceSoundPlayer::ParseNextTick, 0x022B70AC, and
    // SequenceTrack::ParseNextTick, 0x022B6F14).
    if (main_volume_.Moving())
    {
        return false;
    }

    bool any = false;
    for (int t = 0; t < kTrackCount; t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }

        any = true;
        if (!WaitsForever(t) || tr.vol.Moving() || tr.volume2.Moving() || tr.pan.Moving() || tr.bend.Moving())
        {
            return false;
        }

        for (const Note& n : tr.notes)
        {
            if (n.SweepAt(frame_) != 0.0f)
            {
                return false;
            }
        }
    }

    return any;
}

void Performer::PruneNotes(int t)
{
    // Remove channels when their one-shot wave or release finishes (SequenceTrack::ChannelCallbackFunc, 0x022CB06C).
    auto ended = [&](Note& n)
    {
        if (n.EndMs() > now_ms_)
        {
            return false;
        }

        midi_.OnEnd(*this, t, n);
        Ended(n);

        return true;
    };
    std::erase_if(tracks_[t].notes, ended);
    std::erase_if(tracks_[t].detached, ended);
}

void Performer::FollowRelease(nwsnd::ChannelId channel)
{
    if (counting_lengths_)
    {
        length_releases_.push_back(channel);
    }
}

void Performer::FollowLengths(int t)
{
    // The count runs a note-off on a channel whose length has run out if the track's damper doesn't hold it. A region
    // that ignores note-offs plays on. Its MIDI note ends on the channel's release or stop, the wave's end, or where it
    // falls below hearing. In the game, silent channels keep their voices until the voice allocator reuses them.
    //
    // A note can be without a channel in the core. A one-shot wave can end in the frames before the tick in which the
    // note's length runs out. The core frees the channel at the wave's end. The performer also keeps some notes that
    // have no voice in the core (FollowNoChannel, FollowChannelDrop). FollowLengths counts the length and the sweep's
    // ticks of such a note itself. The note then ends in its place among the tick's note-offs. Without that count, a
    // note whose wave has ended would end after them (PruneNotes).
    counting_lengths_ = false;
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        bool has_channel = false;
        bool ran_out = false;
        const auto take_length = [&](auto& core)
        {
            if (const auto* channel = CoreChannelOf(core, n))
            {
                has_channel = true;
                n.length = channel->length;
                n.sweep_ticks = channel->sweep_counter;
                ran_out = std::ranges::find(length_releases_, n.core_channel) != length_releases_.end();
            }
        };
        WithCore(take_length);
        if (!has_channel)
        {
            if (n.length > 0)
            {
                n.length--;
            }
            ran_out = n.length == 0 && !tr.damper;

            if (!n.sweep_in_ms)
            {
                n.sweep_ticks++;
            }
        }

        if (ran_out && !n.released && !n.release_pending && !n.ignore_note_off)
        {
            if (settings_.note_off_waits_for_update && frame_ == n.on_frame)
            {
                n.release_pending = true;
            }
            else
            {
                ReleaseNote(t, n);
            }
        }

        midi_.OnLengthUpdated(*this, t, n);
    }

    length_releases_.clear();
    PruneNotes(t);
}

bool Performer::SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const
{
    const Track& tr = tracks_[t];
    auto moves = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == part &&
               ((n.sweep_in_ms && n.SweepAt(frame) != 0.0f) || n.midi.lfo_bends) && n.EndMs() > ms;
    };

    return std::ranges::any_of(tr.notes, moves) || std::ranges::any_of(tr.detached, moves);
}

void Performer::SweepAfterEnd()
{
    // As MsToTick counts, MIDI goes on at the last tick's tempo. Follow the sweeps for as long as the release tails
    // last (EndPerformance).
    const double per_tick = MsPerTick();
    if (per_tick <= 0)
    {
        return;
    }

    for (uint64_t f = frame_ + 1; static_cast<double>(f) * settings_.frame_ms < now_ms_ + kMaxTailMs; f++)
    {
        const double ms = static_cast<double>(f) * settings_.frame_ms;
        bool any = false;
        for (int t = 0; t < kTrackCount; t++)
        {
            for (uint8_t part = 0; part < kPartCount; part++)
            {
                if (SweepMoves(t, part, f - 1, ms))
                {
                    midi_.SendPitch(
                        *this, t, part, f,
                        WiiOctober2006() ? (ms - nominal_ms_) / MidiMsPerTick() : (ms - now_ms_) / per_tick);
                    any = true;
                }
            }
        }
        if (!any)
        {
            break;
        }
    }
}

float Performer::UserVolume(int t) const
{
    const Track& tr = tracks_[t];
    const float sound = std::clamp(static_cast<float>(sound_.volume) / 127.0f, 0.0f, 1.0f);

    return nwsnd::RevolutionPolicy::TrackVolume(tr.vol.Get(), tr.volume2.Get(), main_volume_.Get(), tr.handle_volume,
                                                sound) *
           options_.volume;
}

int Performer::EffectSend(int t, int bus) const
{
    const Track& tr = tracks_[t];
    const std::array<uint8_t, 3> track_sends = {tr.fx_send_a, tr.fx_send_b, tr.fx_send_c};
    const double send =
        track_sends[static_cast<std::size_t>(bus)] / 127.0 + options_.fx_sends[static_cast<std::size_t>(bus)];

    return static_cast<int>(std::lround(127.0 * std::clamp(send, 0.0, 1.0)));
}

void Performer::StartSweep(Note& n)
{
    // SequenceTrack::NoteOn has given the channel its sweep (Channel::SetSweepParam, 0x022C487C): the track's sweep
    // pitch, plus the distance from the porta key with portamento, over the note's length or over the porta time.
    n.sweep = 0.0f;
    n.sweep_length = 0;
    n.sweep_in_ms = false;
    n.sweep_ticks = 0;
    n.sweep_frame = frame_;

    const auto take_sweep = [&](auto& core)
    {
        if (settling_channel_ != nwsnd::kNoChannel)
        {
            const auto& channel = core.Channels().At(settling_channel_);
            n.sweep = channel.sweep_pitch;
            n.sweep_length = channel.sweep_length;
            n.sweep_in_ms = channel.auto_sweep;
        }
    };
    WithCore(take_sweep);
}

bool Performer::NoteOn(int t, int key, int velocity, int32_t length, bool drop)
{
    // SequenceTrack::NoteOn (0x022C9A4C).
    Track& tr = tracks_[t];

    // At most TickNoteCap() new channels can sound in one tick, fewer with stereo waves. The cap avoids thousands of
    // MIDI notes from loops without waits. Legatos past Settings::voices in one tick continue their existing MIDI note,
    // with or without --voices.
    if (tr.note_tick != tick_)
    {
        tr.note_tick = tick_;
        tr.tick_notes = 0;
        tr.tick_legatos = 0;
    }

    const int vel = velocity * tr.velocity_range / 127;
    midi_.OnNoteOnStart(*this, t);

    // Tie/mono continue the newest channel without another attack (see Legato for MIDI handling).
    if (!tr.notes.empty() && (tr.tie || tr.mono))
    {
        Note& head = tr.notes.back();
        if (!tr.mono || !head.released)
        {
            return Legato(t, head, key, vel, length, ++tr.tick_legatos > settings_.voices);
        }

        StopNote(t, head); // nw::snd stops a releasing channel and starts a new one
        Ended(head);
        tr.notes.pop_back();
    }

    // The game found no free channel for the new note (FollowNoChannel).
    if (drop)
    {
        return false;
    }

    // Bank::NoteOn (0x022C4A0C) finds the new channel's region and wave, or leaves it silent.
    const VelocityRegion* region = banks_.FindRegion(tr.bank_slot, tr.program, key, vel);
    if (!region)
    {
        return true;
    }

    const Pcm* pcm = banks_.GetWave(tr.bank_slot, region->wave_id_index);
    if (!pcm)
    {
        return true;
    }

    if (++tr.tick_notes > TickNoteCap())
    {
        const std::string what = "more than " + std::to_string(TickNoteCap()) + TickCapReport(settings_.engine);
        midi_.Approximate(what.c_str());
        return true;
    }

    const Note n = NewNote(t, key, vel, length, *region, *pcm);

    // Release other channels in the key group at rate 126. SoundFont uses exclusiveClass for this. From 2008, the Wii
    // releases each of them at its release rate (Mario Kart Wii, 0x8009bfcc).
    if (n.key_group != 0)
    {
        for (Note& other : tr.notes)
        {
            if (other.key_group == n.key_group)
            {
                ReleaseNote(t, other, Wii() ? -1 : kKeyGroupRelease);
            }
        }
    }

    tr.notes.push_back(n);
    notes_started_++;
    midi_.OnNoteOn(*this, t, tr.notes.back(), vel, region, n.stereo);

    return true;
}

Note Performer::NewNote(int t, int key, int vel, int32_t length, const VelocityRegion& region, const Pcm& pcm)
{
    const Track& tr = tracks_[t];
    Note n;
    n.settings = &settings_;
    n.key = static_cast<uint8_t>(key);
    n.length = tr.tie ? -1 : length;
    n.on_frame = frame_;
    n.ignore_note_off = region.ignore_note_off;
    n.key_group = region.key_group;
    n.bank_slot = tr.bank_slot;
    n.program = tr.program;
    n.init_pan = tr.init_pan;
    n.stereo = pcm.channels.size() > 1;
    const float init_volume = static_cast<float>(vel) / 127.0f;
    n.init_volume = init_volume * init_volume * (static_cast<float>(region.volume) / 127.0f);
    n.env = OverrideEnvelope(RegionEnvelope(region), TrackPresetKey(tr));

    n.midi.quiet_ms = n.QuietMs();
    n.midi.faded_ms = n.FadedMs();

    n.midi.volume = region.volume;
    n.region_volume = region.volume;
    if (!pcm.loop)
    {
        const float ratio = nwsnd::CalcPitchRatio((key - region.original_key) * 256) * region.pitch * options_.pitch;
        n.one_shot = true;
        WithCore([&](auto& core) { n.core_end = core.PredictEnd(frame_, pcm.Length(), ratio, pcm.sample_rate); });
    }

    StartSweep(n);

    return n;
}

bool Performer::Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap)
{
    Track& tr = tracks_[t];
    const bool heard = !BelowHearing(n);
    n.key = static_cast<uint8_t>(key);
    if (tr.mono)
    {
        n.length = length;
    }

    // The channel takes the velocity squared as its volume (the Wii's Note::init_volume).
    const float init_volume = static_cast<float>(velocity) / 127.0f;
    n.init_volume = init_volume * init_volume;
    n.env = OverrideEnvelope(n.env, TrackPresetKey(tr));
    StartSweep(n);

    // A legato on a channel below hearing changes nothing that can be heard. Count every other legato as a change,
    // regardless of MIDI representation: that can differ between passes.
    if (heard)
    {
        tr.last_change = tick_;
    }

    return midi_.OnLegato(*this, t, n, key, velocity, past_cap, heard);
}

void Performer::NoteVariableRead(int t, uint8_t index)
{
    if (index < 32)
    {
        variable_readers_[index] |= static_cast<uint16_t>(1u << t);
    }
}

void Performer::NoteVariableChange(int t, uint8_t index)
{
    tracks_[t].last_variable_change = commands_run_;
    if (index >= 32)
    {
        return;
    }

    for (int i = 0; i < kTrackCount; i++)
    {
        if (i != t && (variable_readers_[index] >> i & 1) && tracks_[i].open && !WaitsForever(i))
        {
            tracks_[t].last_signal = commands_run_;
            return;
        }
    }
}

void Performer::MuteNotes(int t, uint8_t mode)
{
    Track& tr = tracks_[t];
    if (mode == 2)
    {
        ReleaseAndDetach(t);
    }
    else if (mode == 3)
    {
        midi_.OnStopAll(*this, t);
        for (Note& n : tr.notes)
        {
            n.released = true;
            n.release_end_ms = now_ms_;
            Ended(n);
        }

        tr.notes.clear();
    }
}

bool Performer::Leavable(int t, uint64_t first_command) const
{
    const Track& tr = tracks_[t];
    const auto call = [](const nwsnd::CallStackEntry& e)
    {
        return !e.is_loop;
    };
    if (std::none_of(tr.calls.begin(), tr.calls.begin() + tr.call_depth, call) || tr.leave_at <= first_command ||
        tr.note_at > first_command)
    {
        return false;
    }

    // A note of kHeldTicks or more on a looping wave sounds until the game stops it. A note below hearing doesn't
    // count.
    const auto sustained = [&](const Note& n)
    {
        return !n.released && !n.one_shot && !BelowHearing(n);
    };
    return !tr.holds || std::ranges::none_of(tr.notes, sustained);
}

void Performer::NumberCalls(int t)
{
    Track& tr = tracks_[t];
    tr.numbered_calls = std::min(tr.numbered_calls, tr.call_depth);
    while (tr.numbered_calls < tr.call_depth)
    {
        tr.call_ids[tr.numbered_calls++] = ++call_serial_;
    }
}

uint64_t Performer::InnermostCall(int t) const
{
    const Track& tr = tracks_[t];
    for (int i = tr.call_depth - 1; i >= 0; i--)
    {
        if (!tr.calls[i].is_loop)
        {
            return tr.call_ids[i];
        }
    }

    return 0;
}

bool Performer::ReturnedFrom(int t, uint64_t call) const
{
    const Track& tr = tracks_[t];
    return call != 0 && std::find(tr.call_ids.begin(), tr.call_ids.begin() + tr.call_depth, call) ==
                            tr.call_ids.begin() + tr.call_depth;
}

} // namespace ntdlib::sequence_export
