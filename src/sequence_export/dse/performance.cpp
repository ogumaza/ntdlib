// SPDX-License-Identifier: MIT

// DSE's performer (see performance.h). dse::Player plays the song or the sound effect, and the driver's views follow
// its sequence. The performer takes the sequence's notes, releases, ticks and loops through the driver's hooks, and
// reads the channels' settings and the voices' states from the driver. It records each tick's settings when the next
// tick starts, and when the driver's tick ends. A MIDI note ends when its voices release, or when each of its voices
// has ended or gone to another note.
//
// A voice whose hardware volume is 0 is silent on the console (VoiceView::volume). The driver squares the voice's
// product of volumes, and a quiet channel or the end of a release gives 0 well before the envelope's level does. A part
// all of whose notes are silent and released stops with CC120, and so does a part whose last voice ends. A part whose
// notes are silent but held takes a CC11 of 0 until one of its voices sounds again.
//
// The driver runs a tick about every 10 ms, or once a video frame where the game's settings give the tick of 16.715
// ms (PerformOptions::tick_microseconds). A sequence's tempo adds to a counter each tick, and a tick of the sequence
// runs each time the counter passes the length of a quarter note. MIDI's tempo puts each of the sequence's ticks at its
// nominal time, up to a driver's tick before the driver's tick that runs it. The events of a sequence's tick go at the
// start of that driver's tick (Performance::frame_ticks). The ends of voices that a driver's tick finds go at its start
// too (TrackEvent::after).
//
// A part that sounds one note follows the note's voices more closely. The driver drops the fraction of the timer's
// division, and a voice departs from equal semitones. A bent voice departs by another amount than its zone's tuning
// gives its key. The part's pitch is therefore the bend at which the voices' timers play the note through their zones
// (TimerBend). Notes that overlap on a part play the channel's bend. A note whose timer plays it more than 5 cents from
// that bend at its start takes a part to itself while a MIDI channel is free. No other note joins that part while the
// note sounds.

#include "ntdlib/sequence_export/dse/performance.h"

#include "performance_internal.h"

namespace ntdlib::sequence_export::dse
{

namespace
{

Performer::Performer(const SequenceFiles& files, const PerformOptions& options, double level_scale,
                     const std::array<uint8_t, 16>& part_limits, const LoopReplay& replay)
    : files_(files),
      options_(options),
      player_(std::make_unique<Player>(
          ntdlib::dse::PlayerOptions{.build = options.build, .tick_microseconds = options.tick_microseconds})),
      nominal_tick_ms_(options.tick_microseconds / 1000.0),
      s_(replay, 1000.0 * static_cast<double>(player_->TickClocks()) / ntdlib::ntr::kSystemClockHz, level_scale,
         part_limits)
{
    s_.out.tick_microseconds = options.tick_microseconds;
    const std::vector<ntdlib::dse::Track>* tracks = nullptr;
    if (files.song != nullptr)
    {
        tracks = &files.song->tracks;
    }
    else if (files.effect < files.effects->sequences.size() && files.effects->sequences[files.effect])
    {
        tracks = &files.effects->sequences[files.effect]->tracks;
    }
    if (tracks != nullptr)
    {
        for (const ntdlib::dse::Track& track : *tracks)
        {
            track_channels_.push_back(track.channel);
        }
    }

    // The game's loads: the banks, then the waves of each bank without samples that the main bank holds.
    for (const uint16_t id : player_->LoadBanks(files))
    {
        s_.out.warnings.push_back("a bank's id " + std::to_string(id) +
                                  " is taken: the driver plays the bank loaded first");
    }

    ntdlib::dse::SequenceHooks hooks;
    hooks.on_tick = [this]
    {
        OnTick();
    };
    hooks.on_event = [this](const EventStart& e)
    {
        OnEvent(e);
    };
    hooks.on_note = [this](const NoteEvent& e)
    {
        OnNote(e);
    };
    hooks.on_release = [this](int voice)
    {
        OnRelease(voice);
    };
    hooks.on_loop_start = [this](int track)
    {
        OnLoopStart(track);
    };
    hooks.on_loop = [this](int track)
    {
        OnLoop(track);
    };
    player_->Driver().SetHooks(std::move(hooks));
}

// --- The driver's hooks ---

void Performer::OnTick()
{
    if (!s_.ticked)
    {
        s_.StartTicks(s_.out.frame_ticks);
        s_.out.timebase = Driver().SequenceTimebase();
        SyncTempo();
        return;
    }

    RestartAtTrackEnds();

    // The settings of the last tick, then the next tick at MIDI's tempo.
    SyncChannels(0.0);
    SyncTempo();
    s_.NextTick(s_.out.frame_ticks);
}

void Performer::OnEvent(const EventStart& e)
{
    // An event that follows the one before it goes on with the stretch. So does an event that runs again.
    if (e.track < 0)
    {
        return;
    }

    const auto t = static_cast<std::size_t>(e.track);
    if (t >= s_.out.stretches.size())
    {
        s_.out.stretches.resize(t + 1);
        s_.last_events.resize(t + 1);
    }

    LastEvent& last = s_.last_events[t];
    std::vector<Stretch>& stretches = s_.out.stretches[t];
    if (last.ran && (e.offset == last.next || e.offset == last.offset) && !stretches.empty())
    {
        stretches.back().last = e.offset;
    }
    else
    {
        Stretch stretch;
        stretch.tick = s_.tick;
        stretch.first = e.offset;
        stretch.last = e.offset;
        stretch.depth = e.repeats;
        if (last.ran)
        {
            stretch.entry = last.code == kRepeatEnd ? Entry::kLoop : Entry::kJump;
        }
        stretches.push_back(stretch);
    }

    last = {true, e.offset, e.next, e.code};
}

void Performer::OnLoopStart(int track)
{
    s_.pass_starts[track] = s_.tick;
}

void Performer::OnLoop(int track)
{
    // The pass started when the track last passed its loop's start. A pass within one tick waited for nothing.
    const auto found = s_.pass_starts.find(track);
    if (found == s_.pass_starts.end() || found->second >= s_.tick)
    {
        return;
    }

    const uint32_t start = found->second;
    found->second = s_.tick;
    s_.looped.insert(track);
    s_.loop_lengths[track] = s_.tick - start;

    // The longest loop of the sequence's tracks owns the main loop. A song's tracks usually loop over the same ticks.
    const std::pair<int, uint32_t> owner{track, 0};
    const uint32_t length = s_.tick - start;
    if (!s_.main_loop.Owner() || (*s_.main_loop.Owner() != owner && length > *s_.owner_length))
    {
        s_.main_loop.Restart(s_.out, owner);
        s_.owner_length = length;
    }
    if (*s_.main_loop.Owner() != owner)
    {
        return;
    }

    s_.CountPass(s_.out, owner, track, start, options_.loops);
}

void Performer::OnNote(const NoteEvent& e)
{
    if (!s_.ticked)
    {
        return;
    }
    if (e.channel >= kMidiTracks)
    {
        if (!s_.reported_channels)
        {
            s_.reported_channels = true;
            s_.out.warnings.push_back("notes on channels past 15 are left out");
        }
        return;
    }

    // A legato goes on in the voices of the previous key, and moves them to the new key (NotePitch). The voices keep
    // the velocity of the note that started them.
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(e.channel)];
    if (e.legato)
    {
        std::set<uint32_t> moved;
        for (int v = 0; v < kVoices; v++)
        {
            const uint32_t number = s_.voices[static_cast<std::size_t>(v)];
            const VoiceView view = Driver().GetVoice(v);
            if (number != 0 && view.channel == e.channel && view.key == e.key && s_.notes.at(number).held)
            {
                moved.insert(number);
            }
        }

        for (const uint32_t number : moved)
        {
            const MidiNote& note = s_.notes.at(number);
            if (note.key != e.key && NewestOn(note.track, note.part, true) != number)
            {
                Approximate("legato to another key on a part with newer notes (they keep its pitch)");
            }
        }

        SyncChannel(e.channel, 0.0);
        return;
    }

    const ChannelView channel = Driver().GetChannel(e.channel);

    // A voice that another note takes stops at once on the console. A note of a key group that allows one voice takes
    // the voice of the group's last note on the channel. Its exclusive class cuts that note where both share a part
    // and a preset (PresetZone::exclusive_class).
    std::vector<MidiNote> exclusive_cuts;
    for (int v = 0; v < kVoices; v++)
    {
        const auto index = static_cast<std::size_t>(v);
        if ((e.voices & (1u << v)) == 0 || s_.voices[index] == 0)
        {
            continue;
        }

        const auto found = s_.notes.find(s_.voices[index]);
        const bool exclusive = s_.exclusive[index] != 0 && Driver().GetVoice(v).key_group == s_.exclusive[index] &&
                               found != s_.notes.end() && found->second.track == e.channel;
        if (const std::optional<MidiNote> rings = LeaveVoice(v, true, 0.0); rings && exclusive)
        {
            exclusive_cuts.push_back(*rings);
        }
        else if (rings)
        {
            Approximate("note cut short by another note's voice (rings on in its release)");
        }
    }

    // A note that gets no voice plays nothing on the console. It gets no MIDI note.
    if (e.voices == 0)
    {
        s_.out.unstarted.push_back({.tick = s_.tick,
                                    .length = e.length,
                                    .track = static_cast<uint8_t>(e.track),
                                    .key = e.key,
                                    .velocity = e.velocity,
                                    .offset = e.offset});
        return;
    }

    int first_voice = 0;
    while ((e.voices & (1u << first_voice)) == 0)
    {
        first_voice++;
    }

    // The note's key is its voices' key: a channel with random keys gives the note another key than the event's. The
    // settings that the tick's events have changed so far come first. The note then goes to another part when a note
    // held there plays at another pitch. The pitches of a channel's notes differ by their random changes of pitch, the
    // ranges of their key bends and the keys that legatos have moved them to.
    //
    // A note whose timer plays it more than kOwnPartDeparture from its bend takes a part to itself. No other note joins
    // a part that sounds such a note.
    const uint8_t midi_key = Driver().GetVoice(first_voice).key;
    SyncChannel(e.channel, 0.0);
    const float pitch = VoicePitch(first_voice, midi_key);
    const bool other_pitch = std::ranges::any_of(s_.notes,
                                                 [&](const auto& entry)
                                                 {
                                                     const MidiNote& n = entry.second;
                                                     return n.held && n.track == e.channel && n.part == output.part &&
                                                            NotePitch(entry.first) != pitch;
                                                 });
    const std::optional<double> timer = TimerPitch(e.voices, e.number, midi_key);
    const bool own_part = timer && std::abs(*timer - pitch) > kOwnPartDeparture;
    const bool taken = PartOwned(e.channel, output.part) || (own_part && PartSounds(e.channel, output.part));

    // In the oldest build, a note that starts at another pan than the notes that sound on its part takes another part.
    // They keep their pan.
    const int channel_pan = ChannelPan(e.channel);
    const bool other_pan = PartPan(e.channel, output.part, channel_pan) != channel_pan;
    if ((other_pitch || taken || other_pan) &&
        !s_.recorder.Switch(s_.out, output, s_.tick, [this, &e](uint8_t part) { return PartSounds(e.channel, part); }))
    {
        Approximate(other_pitch ? "note on a part whose pitch another note moves (no MIDI channel free)"
                    : taken     ? "note off its timer's pitch on a shared part (no MIDI channel free)"
                                : "note at another pan than the notes on its part (no MIDI channel free)");
    }
    if (options_.build == ntdlib::dse::Build::kOldest)
    {
        s_.part_pans[{e.channel, output.part}] = channel_pan;
        SyncChannel(e.channel, 0.0);
    }

    PresetKey key;
    key.bank = channel.bank;
    key.program = channel.program;
    key.envelope = channel.envelope;
    key.lfos = channel.lfos;
    key.lfo_level = channel.lfo_level;
    key.build = options_.build;

    // SoundFont holds a pitch LFO and a volume LFO, each a triangle without a fade (convert.h).
    std::set<uint8_t> outputs;
    for (const LfoShape& lfo : PresetLfos(key, options_.tick_microseconds))
    {
        if (lfo.output == kPanOutput)
        {
            Approximate("pan LFO (omitted from SoundFont)");
        }
        else if (!outputs.insert(lfo.output).second)
        {
            Approximate("second LFO of the pitch or the volume (omitted from SoundFont)");
        }
        else
        {
            if (!lfo.triangle)
            {
                Approximate("LFO waveform other than a triangle (a triangle in SoundFont)");
            }
            if (lfo.fade > 0.0)
            {
                Approximate("LFO fade-in (omitted from SoundFont)");
            }
        }
    }

    // A part that sounds the note alone plays it at its voices' timer's rate.
    const uint32_t preset = seqlib::midi::PresetIndex(s_.out.presets, key);
    s_.out.preset_keys.resize(s_.out.presets.size());
    s_.out.preset_keys[preset].insert(midi_key);
    MidiNote note{
        .held = true, .track = e.channel, .part = output.part, .key = midi_key, .voices = e.voices, .preset = preset};
    note.own_part = own_part && !PartSounds(e.channel, output.part);
    const PartSend send =
        PartSounds(e.channel, output.part) ? PartSend{pitch, pitch, 0.0} : AlonePitch(e.number, note, pitch);
    s_.recorder.SendPreset(s_.out, output, s_.tick, preset);
    s_.recorder.SendPitch(s_.out, output, s_.tick, send.pitch);
    SetPartBend(e.channel, output.part, send, 0.0);

    for (const MidiNote& cut : exclusive_cuts)
    {
        if (cut.part != output.part || cut.preset != preset)
        {
            Approximate("note cut short by another note's voice (rings on in its release)");
        }
    }

    const auto velocity = static_cast<uint8_t>(std::clamp<int>(e.velocity, 1, 127));
    note.id = s_.recorder.NoteOn(s_.out, output, s_.tick, midi_key, velocity);
    s_.notes[e.number] = note;
    RecordNote(note.id, e);

    for (int v = 0; v < kVoices; v++)
    {
        if ((e.voices & (1u << v)) != 0)
        {
            s_.voices[static_cast<std::size_t>(v)] = e.number;
            s_.started[static_cast<std::size_t>(v)] = false;
            s_.levels[static_cast<std::size_t>(v)] = kFullLevel;
            s_.volumes[static_cast<std::size_t>(v)] = kFullVolume;
            const VoiceView view = Driver().GetVoice(v);
            s_.release_ticks[static_cast<std::size_t>(v)] = view.release_ticks;
            s_.exclusive[static_cast<std::size_t>(v)] = view.key_group_voices == 1 ? view.key_group : 0;
        }
    }

    s_.last_change_ms = s_.TickMs(s_.tick);
    s_.last_note_ms = s_.last_change_ms;
}

void Performer::OnRelease(int voice)
{
    const uint32_t number = s_.voices[static_cast<std::size_t>(voice)];
    const auto found = s_.notes.find(number);
    if (number == 0 || found == s_.notes.end() || !found->second.held)
    {
        return;
    }

    MidiNote& note = found->second;
    s_.recorder.NoteOff(s_.out, static_cast<uint8_t>(note.track), s_.tick, note.part, note.key, note.id);
    note.held = false;
}

} // namespace

} // namespace ntdlib::sequence_export::dse
