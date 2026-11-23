// SPDX-License-Identifier: MIT

// The DS's performer: the recording of the tracks' settings, the channels' parts, pitches and pans, and the end of each
// frame (see performance.cpp).

#include "performance_internal.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

void Performer::RecordCommand(int number, const snd::Track& track)
{
    // A command that follows the one before it goes on with the stretch.
    const auto t = static_cast<std::size_t>(number);
    const uint32_t offset = track.position - track.base;
    LastCommand& last = s_.last_commands[t];
    std::vector<Stretch>& stretches = s_.out.stretches[t];
    if (last.ran && offset == last.next && !stretches.empty())
    {
        stretches.back().last = offset;
    }
    else
    {
        Stretch stretch;
        stretch.tick = s_.tick;
        stretch.first = offset;
        stretch.last = offset;
        stretch.depth = track.call_depth;
        stretch.note_wait = track.note_wait;
        if (last.ran)
        {
            switch (last.kind)
            {
            case ntdlib::commands::Kind::kCall:
                stretch.entry = Entry::kCall;
                break;
            case ntdlib::commands::Kind::kReturn:
                stretch.entry = Entry::kReturn;
                break;
            case ntdlib::commands::Kind::kLoopEnd:
                stretch.entry = Entry::kLoop;
                break;
            default:
                stretch.entry = Entry::kJump;
                break;
            }
        }
        stretches.push_back(stretch);
    }

    // The command as the driver reads it, from main memory.
    const std::span<const uint8_t> memory = player_.MainMemory();
    const auto byte_at = [memory](uint32_t address)
    {
        return memory[(address - ArchivePlayer::kMainMemory) % memory.size()];
    };
    const snd::PlayedCommand played = snd::ReadPlayedCommand(byte_at, track.position);
    last = {true, played.next - track.base, played.command.kind};
}

void Performer::RecordMix(int number, const snd::Track& track)
{
    const auto t = static_cast<std::size_t>(number);
    const std::array<std::pair<Mix, int16_t>, 2> values = {
        {{Mix::kVolume, static_cast<int16_t>(track.volume)}, {Mix::kPan, static_cast<int16_t>(track.pan)}}};
    ntdlib::sequence_export::detail::RecordMixValues(s_.out.mix[t], s_.mixed[t], values, s_.tick);
}

void Performer::SyncTempo()
{
    const snd::Player& player = Driver().GetPlayer(0);
    const uint32_t step = (static_cast<uint32_t>(player.tempo) * player.tempo_ratio) >> 8;
    if (step == 0)
    {
        return;
    }

    s_.ms_per_tick = kFrameMs * kTickCounter / step;
    s_.recorder.SetTempo(s_.out, s_.tick, 60000.0 / s_.ms_per_tick / kTimebase);
}

void Performer::SyncTracks()
{
    SyncTempo();
    for (int number = 0; number < snd::kTracksPerPlayer; number++)
    {
        if (const snd::Track* track = TrackOf(number); track != nullptr && track->active)
        {
            SyncTrack(number, *track);
            RecordMix(number, *track);
        }
    }
}

void Performer::SyncTrack(int number, const snd::Track& track)
{
    using seqlib::midi::EventKind;
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const snd::Player& player = Driver().GetPlayer(0);

    // CC7 and CC11 take the square of their value. So do the driver's volumes (SND_CalcDecibelSquare). CC11 holds the
    // expression, the player's volume, the faders and the level of the track's pan (PanGain). A fader of f gives the
    // channels f / 10 dB, and CC11 the square root of that gain. The zones hold the rest of the pan's level. CC10 holds
    // the pan's position.
    const int pan = TrackPan(track);
    const double level = std::min<int>(track.expression, 127) * std::min<int>(player.volume, 127) / 127.0 *
                         std::sqrt(PanGain(pan)) * std::pow(10.0, TrackFader(track, player) / 400.0);
    s_.recorder.RaiseLevelPeak(level);
    if (track.volume > 127 || track.expression > 127 || player.volume > 127)
    {
        Approximate("volume above 127 (played at 127)");
    }
    if (track.modulation.depth > 127)
    {
        Approximate("LFO depth above 127 (CC1 holds 127)");
    }

    // A game's reverb gives every part a reverb send of the reverb's volume. Without a reverb, every part gets a send
    // of 0. A synth that follows General MIDI 2 gives a channel without CC91 a send of 40.
    const std::array<std::pair<EventKind, uint8_t>, 5> settings = {{
        {EventKind::kVolume, static_cast<uint8_t>(std::min<int>(track.volume, 127))},
        {EventKind::kExpression, s_.recorder.Expression(level)},
        {EventKind::kPan, PanController(pan)},
        {EventKind::kModulation, static_cast<uint8_t>(std::min<int>(track.modulation.depth, 127))},
        {EventKind::kReverb, static_cast<uint8_t>(options_.reverb ? std::clamp(options_.reverb->volume, 0, 127) : 0)},
    }};
    const float bend = static_cast<float>(BendUnits(track)) / 64.0f;

    // A held note that the track's bend moves after the frame of its strike slides at the bend range of its strike
    // (Strikes::slides). In the strike's frame, the channel first sounds with the bend that the tick's commands leave.
    // A bend counts up to the tick where the note's length runs out. In that tick the driver sets the channel's pitch
    // and then releases the channel. The note still counts as held in the next tick when both ticks run in one frame. A
    // bend in that tick reaches only the release, and only in a pass where the two ticks share a frame. Which passes
    // those are depends on the frames, not on the sequence.
    for (Voice& v : s_.voices)
    {
        if (!v.sounding || !v.held || v.track != number || v.last_bend == BendUnits(track))
        {
            continue;
        }

        v.last_bend = BendUnits(track);
        if (s_.tick <= v.ends && (s_.tick > v.struck || s_.FrameAfter() > 0.0))
        {
            int& range = s_.strikes.slides[{number, v.struck}];
            range = std::max(range, v.strike_range);
        }
    }

    // The settings that SendPanLfos gives a part whose pan a channel's LFO moves.
    const auto follows_lfo = [&](uint8_t part, EventKind kind)
    {
        return (kind == EventKind::kPan || kind == EventKind::kExpression || kind == EventKind::kModulation) &&
               PanLfoChannel(number, part) >= 0;
    };

    const auto changes = [&](uint8_t part)
    {
        for (const auto& [kind, value] : settings)
        {
            if (follows_lfo(part, kind))
            {
                continue;
            }

            const auto sent = output.sent.find({part, kind});
            if (sent == output.sent.end() || sent->second != value)
            {
                return true;
            }
        }

        return !PartHolds(number, part) && !PlaysBend(number, part, bend);
    };

    // The channels that release keep their settings. A part with only release tails gives way to a free part before a
    // change. A part that also holds a note can't.
    if (PartSounds(number, output.part) && !PartHolds(number, output.part) && changes(output.part))
    {
        SwitchPart(number, output);
    }

    // The part that new notes go to, and every part that holds a note of the track.
    std::set<uint8_t> parts = {output.part};
    for (const Voice& v : s_.voices)
    {
        if (v.sounding && v.held && v.track == number)
        {
            parts.insert(v.part);
        }
    }

    bool tails_follow = false;
    // A part that stops following its pan LFO in a later frame of a tick takes the track's values at that frame. At
    // the tick's start, they would go before the LFO's values of the tick's earlier frames.
    for (const uint8_t part : parts)
    {
        bool changed = false;
        for (const auto& [kind, value] : settings)
        {
            // The part stands for the track's values, whether it follows a pan LFO or not.
            if (IsLfoSetting(kind))
            {
                SetStand(number, part, kind, value);
            }
            if (follows_lfo(part, kind))
            {
                continue;
            }

            // SendSettingAfter marks the stand as sent itself.
            if (SentLaterInTick(number, part, kind))
            {
                changed = SendSettingAfter(number, part, kind, value, s_.FrameAfter()) || changed;
                continue;
            }
            const bool sent = s_.recorder.SendSetting(s_.out, output, s_.tick, kind, value, part);
            if (sent && IsLfoSetting(kind))
            {
                MarkStandSent(number, part, kind, 0.0);
            }
            changed = sent || changed;
        }
        if (!PartHolds(number, part) && !PlaysBend(number, part, bend))
        {
            changed = s_.recorder.SendPitch(s_.out, output, s_.tick, bend, part) || changed;
            SetPartBend(number, part, bend, 0.0, 0.0);
        }
        if (changed)
        {
            s_.last_change_ms = s_.TickMs(s_.tick);
            for (const Voice& v : s_.voices)
            {
                tails_follow = tails_follow || (v.sounding && !v.held && v.track == number && v.part == part);
            }
        }
    }
    if (tails_follow)
    {
        Approximate("release tail follows its track's later settings");
    }

    // A channel takes the track's LFO every frame. The note's preset keeps the LFO's speed, range and delay at its
    // start.
    for (Voice& v : s_.voices)
    {
        if (v.sounding && v.held && v.track == number && !v.lfo_reported && track.modulation.depth != 0 &&
            (v.preset.lfo_target != track.modulation.target || v.preset.lfo_speed != track.modulation.speed ||
             v.preset.lfo_range != track.modulation.range || v.preset.lfo_delay != track.modulation.delay))
        {
            v.lfo_reported = true;
            Approximate("LFO parameters changed during a note (using its note-on settings)");
        }
    }
}

int32_t Performer::SweepApplied(int channel) const
{
    const snd::Channel& ch = Driver().GetChannel(channel);
    const Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    if (ch.sweep_pitch == 0)
    {
        return 0;
    }

    // An automatic sweep steps its counter in each frame's update, once the update has applied the sweep. In the
    // frame's ticks, the counter gives the sweep that the update is to apply. The performer puts that pitch at the
    // frame's start, with the ticks' events. After the update, the counter has moved on since the last frame's end only
    // when the update applied the sweep, and the sweep has otherwise ended. A sweep over the note's length steps in the
    // ticks, before the update.
    int32_t counter = ch.sweep_counter;
    if (ch.auto_sweep && v.sounding && v.held && counter != v.sweep_counter)
    {
        counter--;
    }

    // The driver applies a sweep only while its counter is below its length (Driver::StepSweep). The division below
    // then never has a length of 0.
    if (counter < 0 || counter >= ch.sweep_length)
    {
        return 0;
    }

    return static_cast<int32_t>(static_cast<int64_t>(ch.sweep_pitch) * (ch.sweep_length - counter) / ch.sweep_length);
}

float Performer::VoicePitch(int channel) const
{
    const snd::Channel& ch = Driver().GetChannel(channel);
    const Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    return static_cast<float>(ch.user_pitch + SweepApplied(channel)) / 64.0f + static_cast<float>(ch.key - v.key);
}

double Performer::TimerPitchRounding(int channel) const
{
    if (VoicePitch(channel) == 0.0f)
    {
        return 0.0;
    }

    const snd::Channel& ch = Driver().GetChannel(channel);
    const Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    const int pitch = (ch.key - ch.root_key) * 64 + ch.user_pitch + SweepApplied(channel);
    return TimerRounding(ch.type, ch.wave.timer, ch.root_key, v.key, pitch);
}

float Performer::TimerPitch(int channel) const
{
    const float nominal = VoicePitch(channel);
    if (nominal == 0.0f)
    {
        return nominal;
    }

    const snd::Channel& ch = Driver().GetChannel(channel);
    const Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    const int pitch = (ch.key - ch.root_key) * 64 + ch.user_pitch + SweepApplied(channel);
    return static_cast<float>(TimerBend(ch.type, ch.wave.timer, ch.root_key, v.key, pitch));
}

int Performer::PanLfoChannel(int number, uint8_t part) const
{
    int channel = -1;
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        const Voice& v = s_.voices[c];
        if (v.sounding && v.track == number && v.part == part)
        {
            if (channel >= 0)
            {
                return -1;
            }
            channel = static_cast<int>(c);
        }
    }
    if (channel < 0)
    {
        return -1;
    }

    const snd::LfoParameters& lfo = Driver().GetChannel(channel).lfo.parameters;
    return lfo.target == kPanTarget && lfo.depth != 0 ? channel : -1;
}

int Performer::LfoPan(int channel) const
{
    // The channel's pan without the LFO, as Driver::StepChannels works it out. The difference from the channel's pan
    // is the LFO's.
    const snd::Channel& ch = Driver().GetChannel(channel);
    int32_t pan = ch.initial_pan;
    if (ch.pan_range != 127)
    {
        pan = (pan * ch.pan_range + 0x40) >> 7;
    }
    const int still = std::clamp(pan + ch.user_pan + 0x40, 0, 127);

    return std::clamp(64 + ch.user_pan + ch.pan - still, 0, 127);
}

bool Performer::SendSettingAfter(int number, uint8_t part, EventKind kind, uint8_t value, double after)
{
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const auto [sent, added] = output.sent.try_emplace({part, kind}, value);
    if (!added && sent->second == value)
    {
        return false;
    }

    sent->second = value;
    s_.recorder.Emit(s_.out, output, s_.tick, {.kind = kind, .value = value, .after = after}, part);
    if (IsLfoSetting(kind))
    {
        MarkStandSent(number, part, kind, after);
    }

    return true;
}

bool Performer::SentLaterInTick(int number, uint8_t part, EventKind kind) const
{
    const TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const std::vector<TrackEvent>& events = s_.out.tracks[output.midi_track];
    for (auto it = events.rbegin(); it != events.rend() && it->tick == s_.tick; ++it)
    {
        if (it->kind == kind && it->part == part)
        {
            return it->after > 0.0;
        }
    }

    return false;
}

void Performer::SetStand(int number, uint8_t part, EventKind kind, uint8_t value)
{
    const auto [stand, added] = s_.stood.try_emplace({number, part, kind}, value);
    if (!added && stand->second == value)
    {
        return;
    }

    stand->second = value;
    s_.stands[static_cast<std::size_t>(number)].push_back(
        {.tick = s_.tick, .kind = kind, .value = value, .part = part, .after = 0.0});
}

void Performer::MarkStandSent(int number, uint8_t part, EventKind kind, double after)
{
    if (const auto stand = s_.stood.find({number, part, kind}); stand != s_.stood.end())
    {
        s_.stands[static_cast<std::size_t>(number)].push_back(
            {.tick = s_.tick, .kind = kind, .value = stand->second, .part = part, .after = after});
    }
}

void Performer::SendPanLfos(double after)
{
    const snd::Player& player = Driver().GetPlayer(0);
    std::set<std::pair<int, uint8_t>> done;
    for (const Voice& v : s_.voices)
    {
        if (!v.sounding || !done.insert({v.track, v.part}).second)
        {
            continue;
        }
        const int channel = PanLfoChannel(v.track, v.part);
        const snd::Track* track = TrackOf(v.track);
        if (channel < 0 || track == nullptr)
        {
            continue;
        }

        // CC11 holds the level of the pan, as SyncTrack gives it. A release tail keeps the expression and the player's
        // volume from its release. The faders still apply.
        const Voice& voice = s_.voices[static_cast<std::size_t>(channel)];

        // A note that strikes on a part that follows: the LFO's course is written around the pan and the level of the
        // strike. Both count as sent there (Strikes::follows).
        if (voice.held && voice.struck == s_.tick && after == 0.0)
        {
            s_.strikes.follows.insert({v.track, s_.tick, v.part});
            MarkStandSent(v.track, v.part, EventKind::kPan, 0.0);
            MarkStandSent(v.track, v.part, EventKind::kExpression, 0.0);
        }

        const int expression = voice.held ? track->expression : voice.released_expression;
        const int volume = voice.held ? player.volume : voice.released_volume;
        const int pan = LfoPan(channel);
        const double level = std::min(expression, 127) * std::min(volume, 127) / 127.0 * std::sqrt(PanGain(pan)) *
                             std::pow(10.0, TrackFader(*track, player) / 400.0);
        s_.recorder.RaiseLevelPeak(level);
        SendSettingAfter(v.track, v.part, EventKind::kPan, PanController(pan), after);
        SendSettingAfter(v.track, v.part, EventKind::kExpression, s_.recorder.Expression(level), after);
        SendSettingAfter(v.track, v.part, EventKind::kModulation, 0, after);
    }
}

bool Performer::PartSounds(int number, uint8_t part) const
{
    return std::ranges::any_of(s_.voices,
                               [&](const Voice& v) { return v.sounding && v.track == number && v.part == part; });
}

bool Performer::PartHolds(int number, uint8_t part) const
{
    return std::ranges::any_of(
        s_.voices, [&](const Voice& v) { return v.sounding && v.held && v.track == number && v.part == part; });
}

bool Performer::NeedsOtherPart(int number, uint8_t part, bool sweeps) const
{
    const snd::Track* track = TrackOf(number);
    const float bend = track != nullptr ? static_cast<float>(BendUnits(*track)) / 64.0f : 0.0f;
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        const Voice& v = s_.voices[c];
        if (!v.sounding || v.track != number || v.part != part)
        {
            continue;
        }
        if (sweeps || (v.held && VoicePitch(static_cast<int>(c)) != bend))
        {
            return true;
        }
    }

    return false;
}

bool Performer::PlaysBend(int number, uint8_t part, float bend) const
{
    if (const auto stands_for = s_.part_bends.find({number, part}); stands_for != s_.part_bends.end())
    {
        return stands_for->second == bend;
    }

    const TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const auto pitch = output.pitch.find(part);
    return pitch == output.pitch.end() ? bend == 0.0f : pitch->second == bend;
}

bool Performer::SwitchPart(int number, TrackOutput& output)
{
    const uint8_t old = output.part;
    if (!s_.recorder.Switch(s_.out, output, s_.tick, [this, number](uint8_t part) { return PartSounds(number, part); }))
    {
        return false;
    }

    // The new part takes the old part's pitch, with the bend and the rounding that it stands for.
    const auto stands_for = s_.part_bends.find({number, old});
    const auto changes = s_.bend_changes.find({number, old});
    const double rounding =
        changes != s_.bend_changes.end() && !changes->second.empty() ? changes->second.back().rounding : 0.0;
    SetPartBend(number, output.part,
                stands_for != s_.part_bends.end() ? std::optional(stands_for->second) : std::nullopt, 0.0, rounding);

    // The new part takes the old part's stands too. Recorder::Switch sends it the old part's values.
    for (const EventKind kind : {EventKind::kPan, EventKind::kExpression, EventKind::kModulation})
    {
        if (const auto stand = s_.stood.find({number, old, kind}); stand != s_.stood.end())
        {
            s_.stood[{number, output.part, kind}] = stand->second;
            MarkStandSent(number, output.part, kind, 0.0);
        }
    }

    return true;
}

void Performer::SetPartBend(int number, uint8_t part, std::optional<float> bend, double after, double rounding)
{
    if (bend)
    {
        s_.part_bends[{number, part}] = *bend;
    }
    else
    {
        s_.part_bends.erase({number, part});
    }

    AddBendChange(s_.bend_changes, number, part, {s_.tick, after, bend, rounding});
}

void Performer::EndVoice(int channel, double after)
{
    Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    if (v.held)
    {
        s_.recorder.NoteOff(s_.out, static_cast<uint8_t>(v.track), s_.tick, v.part, v.key, v.note, after);
        v.held = false;
        const snd::Track* track = TrackOf(v.track);
        v.released_expression = track != nullptr ? track->expression : 0;
        v.released_volume = Driver().GetPlayer(0).volume;
    }
}

void Performer::EndFrame()
{
    if (!s_.ticked)
    {
        s_.frame++;
        return;
    }

    SyncTracks();

    // A channel that releases ends its MIDI note. A channel that stops ends its voice. The release comes at the end of
    // the frame whose ticks end the note's length.
    const double after = s_.FrameAfter();
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        Voice& v = s_.voices[c];
        if (!v.sounding)
        {
            continue;
        }

        const snd::Channel& ch = Driver().GetChannel(static_cast<int>(c));
        if (!ch.active || ch.envelope_stage == kReleaseStage)
        {
            EndVoice(static_cast<int>(c), after);
        }
        v.sounding = ch.active;
    }

    // Each part that holds a note takes the pitch of its newest held note. A part that holds one note takes the
    // pitch of its channel's timer.
    struct Held
    {
        std::size_t newest = 0; // the voice of the newest note
        int notes = 0;
    };
    std::map<std::pair<int, uint8_t>, Held> parts;
    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        const Voice& v = s_.voices[c];
        if (!v.sounding || !v.held)
        {
            continue;
        }

        const auto [it, added] = parts.try_emplace({v.track, v.part}, Held{.newest = c});
        if (!added && s_.voices[it->second.newest].note < v.note)
        {
            it->second.newest = c;
        }
        it->second.notes++;
    }
    for (const auto& [place, held] : parts)
    {
        TrackOutput& output = s_.outputs[static_cast<std::size_t>(place.first)];
        const auto c = static_cast<int>(held.newest);
        const bool alone = held.notes == 1;
        const float pitch = alone ? TimerPitch(c) : VoicePitch(c);
        if (s_.recorder.SendPitch(s_.out, output, s_.tick, pitch, place.second, after))
        {
            s_.last_change_ms = s_.FrameMs(s_.frame);
        }
        SetPartBend(place.first, place.second, VoicePitch(c), after, alone ? TimerPitchRounding(c) : 0.0);
    }
    SendPanLfos(after);

    for (std::size_t c = 0; c < s_.voices.size(); c++)
    {
        s_.voices[c].sweep_counter = Driver().GetChannel(static_cast<int>(c)).sweep_counter;
    }
    s_.FinishFrame(AnySounding());
}

void Performer::ChangeVariables()
{
    const std::vector<VariableChange>& changes = options_.changes;
    if (s_.next_change == changes.size() || changes[s_.next_change].frame > s_.frame)
    {
        return;
    }

    ntdlib::ntr::Variables variables;
    for (; s_.next_change < changes.size() && changes[s_.next_change].frame <= s_.frame; s_.next_change++)
    {
        const VariableChange& change = changes[s_.next_change];
        if (change.index < 16)
        {
            variables.local[change.index] = change.value;
        }
        else if (change.index < 32)
        {
            variables.global[change.index - 16u] = change.value;
        }
    }
    player_.Send(ntdlib::ntr::VariableCommands(variables));
    RestartSearch();
}

void Performer::RestartSearch()
{
    // The search starts again with the first pass that starts after the change (OnLoop).
    s_.changed_at = s_.tick;
    s_.changed_ms = s_.FrameMs(s_.frame);
    s_.main_loop.Restart(s_.out, std::nullopt);
}

void Performer::CutVoice(int channel, double after, const std::string& what)
{
    Voice& v = s_.voices[static_cast<std::size_t>(channel)];
    if (v.held && s_.tick < v.ends && v.note >= 1 && v.note <= s_.out.note_channels.size())
    {
        s_.out.note_channels[v.note - 1].cut_tick = s_.tick;
    }
    EndVoice(channel, after);
    v.sounding = false;

    if (PartSounds(v.track, v.part))
    {
        Approximate(what);
    }
    else
    {
        s_.recorder.Emit(s_.out, s_.outputs[static_cast<std::size_t>(v.track)], s_.tick,
                         {.kind = EventKind::kSoundOff, .after = after}, v.part);
    }
}

void Performer::LockChannels()
{
    const std::vector<ntdlib::ntr::ChannelLock>& locks = options_.channel_locks;
    if (s_.next_lock == locks.size() || locks[s_.next_lock].frame > s_.frame)
    {
        return;
    }

    // The driver stops the channels of a lock before the frame's ticks, at once, as a steal does. The stop comes where
    // the last frame ends.
    const double after = s_.FrameAfter();
    for (; s_.next_lock < locks.size() && locks[s_.next_lock].frame <= s_.frame; s_.next_lock++)
    {
        const ntdlib::ntr::ChannelLock& lock = locks[s_.next_lock];
        player_.Send(ntdlib::ntr::ChannelLockCommands(lock));
        for (int c = 0; c < snd::kChannelCount && !lock.unlock; c++)
        {
            if ((lock.channels >> c & 1) != 0 && s_.voices[static_cast<std::size_t>(c)].sounding)
            {
                CutVoice(c, after, "note cut short by a lock of its channel (rings on in its release)");
            }
        }
    }

    RestartSearch();
}

void Performer::SendTickCommands()
{
    // The ticks that the player has run: the count that NNS_SndPlayerGetTick reads. A frame of the game's code that
    // starts by the start of the driver's frame that runs next reads the ticks of the frames before it.
    const uint32_t ticks = s_.ticked ? s_.tick + 1 : 0;
    const bool finished = s_.tick_progress.SendDue(options_.tick_commands, s_.frame * snd::kFrameClocks, ticks,
                                                   [this](const snd::CommandList& list) { player_.Send(list); });
    if (finished)
    {
        RestartSearch();
    }
}

bool Performer::GameChangesDone() const
{
    return s_.tick_progress.Done(options_.tick_commands) && s_.next_change == options_.changes.size() &&
           s_.next_lock == options_.channel_locks.size();
}

bool Performer::Advance()
{
    ChangeVariables();
    LockChannels();
    SendTickCommands();
    player_.RunFrame();
    EndFrame();
    return Driver().GetPlayer(0).active;
}

// A note that its track struck silent doesn't sound (snd::Driver::IsChannelSilent).
bool Performer::AnySounding() const
{
    for (int c = 0; c < snd::kChannelCount; c++)
    {
        if (Driver().GetChannel(c).active && !Driver().IsChannelSilent(c))
        {
            return true;
        }
    }

    return false;
}

} // namespace

} // namespace ntdlib::sequence_export::ntr
