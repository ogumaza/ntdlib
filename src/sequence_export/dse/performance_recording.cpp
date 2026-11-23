// SPDX-License-Identifier: MIT

// DSE's performer: the recording of the channels' settings, parts, pitches and pans, and the end of each tick (see
// performance.cpp).

#include "performance_internal.h"

namespace ntdlib::sequence_export::dse
{

namespace
{

void Performer::SyncTempo()
{
    const int32_t tempo = Driver().SequenceTempo();
    if (tempo <= 0 || s_.out.timebase == 0)
    {
        return;
    }

    // A tick of the sequence takes tempo / timebase microseconds of the driver's nominal ticks.
    s_.ms_per_tick = tempo / 1000.0 / s_.out.timebase * (s_.frame_ms / nominal_tick_ms_);
    s_.recorder.SetTempo(s_.out, s_.tick, 60000.0 / s_.ms_per_tick / s_.out.timebase);
}

void Performer::SyncChannels(double after)
{
    const int count = std::min(Driver().ChannelCount(), kMidiTracks);
    for (int c = 0; c < count; c++)
    {
        SyncChannel(c, after);
    }
}

void Performer::SyncChannel(int number, double after)
{
    using seqlib::midi::EventKind;
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(number)];
    const ChannelView channel = Driver().GetChannel(number);

    // CC7 and CC11 take the square of their value. So do the volumes of the driver: the volume of a voice is the square
    // of the product of the fade, expression and synth volume of the channel, the volume of the note and the level of
    // the envelope. CC11 contains the expression, the synth volume and the level of the channel pan (PanGain). The
    // zones contain the rest of the level of the pan. CC10 contains the position of the pan. The driver gives the sound
    // unit the hardware pan of its pan. For a mixer that pans linearly (PerformOptions::linear_pans), CC10 contains the
    // pan, and CC11 contains no level of the pan.
    RecordMix(number, channel);
    const int channel_pan = ChannelPan(number);

    // The part that new notes go to, and every part on which a note of the channel sounds. The voices that release
    // follow the channel's settings, as MIDI's notes do. The oldest build's voices keep the pan that they start with: a
    // part that sounds notes keeps their pan (PartPan).
    std::set<uint8_t> parts = {output.part};
    for (const auto& [number_of_note, note] : s_.notes)
    {
        if (note.track == number)
        {
            parts.insert(note.part);
        }
    }

    for (const uint8_t part : parts)
    {
        const int pan = PartPan(number, part, channel_pan);
        const double pan_level = options_.linear_pans ? 1.0 : std::sqrt(PanGain(pan));
        const double level = channel.expression * std::clamp<int>(channel.synth_volume, 0, 127) / 127.0 * pan_level;
        s_.recorder.RaiseLevelPeak(level);
        const uint8_t unmuted = s_.recorder.Expression(level);
        const uint8_t expression = PartMuted(number, part) ? 0 : unmuted;

        // The driver has no reverb send. CC91 is 0: a synth that follows General MIDI 2 gives a channel without CC91 a
        // send of 40.
        const std::array<std::pair<EventKind, uint8_t>, 5> settings = {{
            {EventKind::kVolume, static_cast<uint8_t>(std::min<int>(channel.volume, 127))},
            {EventKind::kExpression, expression},
            {kUnmutedExpression, unmuted},
            {EventKind::kPan,
             options_.linear_pans ? static_cast<uint8_t>(std::clamp(pan, 0, 127)) : PanController(pan)},
            {EventKind::kReverb, 0},
        }};

        bool changed = false;
        for (const auto& [kind, value] : settings)
        {
            changed = s_.recorder.SendSetting(s_.out, output, s_.tick, kind, value, part) || changed;
        }
        if (const std::optional<PartSend> send = PartPitchToSend(number, part))
        {
            changed = s_.recorder.SendPitch(s_.out, output, s_.tick, send->pitch, part, after) || changed;
            SetPartBend(number, part, *send, after);
        }
        if (changed)
        {
            s_.last_change_ms = s_.TickMs(s_.tick);
        }
    }
}

void Performer::RecordNote(uint32_t id, const NoteEvent& e)
{
    // The note's event ran in its track's last stretch.
    std::vector<NoteCommand>& commands = s_.out.note_commands;
    if (commands.size() < id)
    {
        commands.resize(id);
        s_.out.note_tracks.resize(id);
        s_.out.note_channels.resize(id);
    }

    const auto t = static_cast<std::size_t>(e.track);
    const std::size_t stretches = t < s_.out.stretches.size() ? s_.out.stretches[t].size() : 0;
    commands[id - 1] = {e.offset, static_cast<uint32_t>(stretches > 0 ? stretches - 1 : 0)};
    s_.out.note_tracks[id - 1] = static_cast<uint8_t>(e.track);
}

void Performer::RecordMix(int number, const ChannelView& channel)
{
    const auto c = static_cast<std::size_t>(number);
    const std::array<std::pair<Mix, int16_t>, 2> values = {
        {{Mix::kVolume, static_cast<int16_t>(std::min<int>(channel.volume, 127))},
         {Mix::kPan, static_cast<int16_t>(std::clamp<int>(channel.pan - 0x40, -64, 63))}}};
    ntdlib::sequence_export::detail::RecordMixValues(s_.channel_mix[c], s_.mixed[c], values, s_.tick);
}

float Performer::VoicePitch(int voice, uint8_t key) const
{
    const VoiceView view = Driver().GetVoice(voice);
    const ChannelView channel = Driver().GetChannel(view.channel);
    const int32_t bend = static_cast<int16_t>(channel.bend) + view.key_bend + view.jitter;
    return static_cast<float>(bend) / 256.0f + static_cast<float>(view.key - key);
}

float Performer::NotePitch(uint32_t number) const
{
    // A voice that the driver has freed in this tick still counts among the note's until the tick ends (EndFrame).
    const MidiNote& note = s_.notes.at(number);
    for (int v = 0; v < kVoices; v++)
    {
        if ((note.voices & (1u << v)) == 0)
        {
            continue;
        }

        const VoiceView view = Driver().GetVoice(v);
        if (view.allocated && view.followed && view.note == number)
        {
            return VoicePitch(v, note.key);
        }
    }

    return static_cast<float>(static_cast<int16_t>(Driver().GetChannel(note.track).bend)) / 256.0f;
}

bool Performer::PartSounds(int track, uint8_t part) const
{
    return std::ranges::any_of(
        s_.notes, [&](const auto& entry) { return entry.second.track == track && entry.second.part == part; });
}

int Performer::ChannelPan(int track) const
{
    return ntdlib::dse::tables::kPanCurve[std::clamp<int>(Driver().GetChannel(track).pan, 0, 127)];
}

int Performer::PartPan(int track, uint8_t part, int channel_pan) const
{
    if (options_.build != ntdlib::dse::Build::kOldest || !PartSounds(track, part))
    {
        return channel_pan;
    }

    const auto found = s_.part_pans.find({track, part});
    return found != s_.part_pans.end() ? found->second : channel_pan;
}

bool Performer::PartOwned(int track, uint8_t part) const
{
    return std::ranges::any_of(s_.notes,
                               [&](const auto& entry)
                               {
                                   const MidiNote& note = entry.second;
                                   return note.track == track && note.part == part && note.own_part;
                               });
}

bool Performer::VoiceAudible(int voice) const
{
    const auto index = static_cast<std::size_t>(voice);
    return !s_.started[index] || s_.volumes[index] > 0;
}

bool Performer::PartAudible(int track, uint8_t part) const
{
    const auto audible = [&](const auto& entry)
    {
        const MidiNote& note = entry.second;
        if (note.track != track || note.part != part)
        {
            return false;
        }

        for (int v = 0; v < kVoices; v++)
        {
            if ((note.voices & (1u << v)) != 0 && VoiceAudible(v))
            {
                return true;
            }
        }

        return false;
    };
    return std::ranges::any_of(s_.notes, audible);
}

bool Performer::PartHolds(int track, uint8_t part) const
{
    const auto held = [&](const auto& entry)
    {
        return entry.second.track == track && entry.second.part == part && entry.second.held;
    };
    return std::ranges::any_of(s_.notes, held);
}

bool Performer::PartMuted(int track, uint8_t part) const
{
    return PartHolds(track, part) && !PartAudible(track, part);
}

void Performer::StopSilentParts(double after)
{
    std::set<std::pair<int, uint8_t>> parts;
    for (const auto& [number, note] : s_.notes)
    {
        parts.insert({note.track, note.part});
    }

    for (const auto& [track, part] : parts)
    {
        const auto on_part = [&](const auto& entry)
        {
            return entry.second.track == track && entry.second.part == part;
        };
        const auto stopped = [&](const auto& entry)
        {
            return !on_part(entry) || entry.second.stopped;
        };
        if (PartHolds(track, part) || PartAudible(track, part) || std::ranges::all_of(s_.notes, stopped))
        {
            continue;
        }

        for (auto& entry : s_.notes)
        {
            if (on_part(entry))
            {
                entry.second.stopped = true;
            }
        }

        s_.recorder.Emit(s_.out, s_.outputs[static_cast<std::size_t>(track)], s_.tick,
                         {.kind = EventKind::kSoundOff, .after = after}, part);
    }
}

uint32_t Performer::NewestOn(int track, uint8_t part, bool held) const
{
    uint32_t newest = 0;
    for (const auto& [number, note] : s_.notes)
    {
        if (note.track == track && note.part == part && (note.held || !held))
        {
            newest = std::max(newest, number);
        }
    }

    return newest;
}

float Performer::PartPitch(int track, uint8_t part) const
{
    if (const uint32_t held = NewestOn(track, part, true); held != 0)
    {
        return NotePitch(held);
    }
    if (const uint32_t newest = NewestOn(track, part, false); newest != 0)
    {
        return NotePitch(newest);
    }

    return static_cast<float>(static_cast<int16_t>(Driver().GetChannel(track).bend)) / 256.0f;
}

std::optional<double> Performer::VoiceTimerPitch(int voice, uint8_t key) const
{
    if (VoicePitch(voice, key) == 0.0f)
    {
        return 0.0;
    }

    // The driver's pitch: the channel's bend, the split's tuning, the random pitch, the key and the key bend.
    const VoiceView view = Driver().GetVoice(voice);
    const ChannelView channel = Driver().GetChannel(view.channel);
    const auto pitch =
        static_cast<uint16_t>(channel.bend + view.tuning + view.jitter + (view.key << 8) + view.key_bend);
    PresetZone zone;
    zone.tuning = view.tuning;
    zone.format = view.format;

    return TimerBend(zone, SampleRateOf(view.format, view.rate), key, pitch);
}

std::optional<double> Performer::TimerPitch(uint16_t voices, uint32_t number, uint8_t key) const
{
    std::optional<double> pitch;
    for (int v = 0; v < kVoices; v++)
    {
        const VoiceView view = Driver().GetVoice(v);
        if ((voices & (1u << v)) == 0 || !view.allocated || !view.followed || view.note != number)
        {
            continue;
        }

        const std::optional<double> voice = VoiceTimerPitch(v, key);
        if (!voice || (pitch && std::abs(*voice - *pitch) > kHalfCent))
        {
            return std::nullopt;
        }
        pitch = pitch.value_or(*voice);
    }

    return pitch;
}

PartSend Performer::AlonePitch(uint32_t number, const MidiNote& note, float plain) const
{
    const std::optional<double> timer = TimerPitch(note.voices, number, note.key);
    if (!timer)
    {
        return {plain, plain, 0.0};
    }

    return {static_cast<float>(*timer), plain, std::abs(*timer - plain) + kHalfCent};
}

std::optional<PartSend> Performer::PartPitchToSend(int track, uint8_t part) const
{
    const float plain = PartPitch(track, part);
    std::optional<uint32_t> alone;
    for (const auto& [number, note] : s_.notes)
    {
        if (note.track != track || note.part != part)
        {
            continue;
        }
        if (alone)
        {
            return PartSend{plain, plain, 0.0};
        }
        alone = number;
    }

    if (alone)
    {
        return AlonePitch(*alone, s_.notes.at(*alone), plain);
    }

    // A part without a note keeps a timer's pitch for the same bend: nothing sounds there.
    const auto changes = s_.bend_changes.find({track, part});
    if (changes != s_.bend_changes.end() && !changes->second.empty() && changes->second.back().bend == plain)
    {
        return std::nullopt;
    }

    return PartSend{plain, plain, 0.0};
}

void Performer::SetPartBend(int track, uint8_t part, const PartSend& send, double after)
{
    // A record that repeats the last one changes nothing that BendOf finds.
    const auto found = s_.bend_changes.find({track, part});
    if (found != s_.bend_changes.end() && !found->second.empty())
    {
        const ntdlib::sequence_export::BendChange& last = found->second.back();
        if (last.bend == send.bend && last.rounding == send.rounding &&
            std::pair(last.tick, last.after) <= std::pair(s_.tick, after))
        {
            return;
        }
    }

    AddBendChange(s_.bend_changes, track, part, {s_.tick, after, send.bend, send.rounding});
}

std::optional<MidiNote> Performer::LeaveVoice(int voice, bool cut, double after)
{
    uint32_t& number = s_.voices[static_cast<std::size_t>(voice)];
    const auto found = s_.notes.find(number);
    number = 0;
    if (found == s_.notes.end())
    {
        return std::nullopt;
    }

    MidiNote& note = found->second;
    note.voices = static_cast<uint16_t>(note.voices & ~(1u << voice));
    if (note.voices != 0)
    {
        return std::nullopt;
    }

    const MidiNote ended = note;
    s_.notes.erase(found);

    if (ended.held)
    {
        s_.recorder.NoteOff(s_.out, static_cast<uint8_t>(ended.track), s_.tick, ended.part, ended.key, ended.id, after);
    }
    if (cut && ended.held && ended.id >= 1 && ended.id <= s_.out.note_channels.size())
    {
        s_.out.note_channels[ended.id - 1].cut_tick = s_.tick;
    }

    // The note's voices no longer sound. CC120 stops its part where no other note that can be heard sounds there and
    // no note is held there, and stops those below hearing with it. A held note below hearing can sound again, and
    // CC120 would end it.
    if (!PartAudible(ended.track, ended.part) && !PartHolds(ended.track, ended.part))
    {
        if (ended.stopped)
        {
            return std::nullopt;
        }
        s_.recorder.Emit(s_.out, s_.outputs[static_cast<std::size_t>(ended.track)], s_.tick,
                         {.kind = EventKind::kSoundOff, .after = after}, ended.part);
        return std::nullopt;
    }

    // Otherwise a cut note plays its release in MIDI, where it stops at once on the console. A release below hearing
    // doesn't count, and neither does one that falls below hearing within a tick: the level falls in a straight line.
    if (cut && VoiceAudible(voice))
    {
        const uint8_t level = s_.levels[static_cast<std::size_t>(voice)];
        const std::optional<int32_t> release = s_.release_ticks[static_cast<std::size_t>(voice)];
        if (!release || int64_t{*release} * (level - kInaudibleLevel) > level)
        {
            return ended;
        }
    }

    return std::nullopt;
}

void Performer::EndFrame()
{
    if (!s_.ticked)
    {
        s_.frame++;
        return;
    }

    // A voice that the driver has freed, stopped or given to another note no longer plays its note.
    const double after = s_.FrameAfter();
    for (int v = 0; v < kVoices; v++)
    {
        const auto index = static_cast<std::size_t>(v);
        if (s_.voices[index] == 0)
        {
            continue;
        }

        const VoiceView view = Driver().GetVoice(v);
        if (!view.allocated || !view.followed || view.note != s_.voices[index] || (s_.started[index] && !view.started))
        {
            LeaveVoice(v, false, after);
            continue;
        }

        s_.started[index] = s_.started[index] || view.started;
        s_.levels[index] = view.level;
        s_.volumes[index] = view.volume;
        s_.release_ticks[index] = view.release_ticks;
    }

    StopSilentParts(after);
    SyncChannels(after);

    s_.FinishFrame(AnySounding());
}

bool Performer::Advance()
{
    player_->RunTick();
    EndFrame();
    return Driver().SequencePlays();
}

bool Performer::AnySounding() const
{
    return player_->BusyChannels() != 0;
}

} // namespace

} // namespace ntdlib::sequence_export::dse
