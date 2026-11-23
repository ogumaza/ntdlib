// SPDX-License-Identifier: MIT

// The recording of a performance's MIDI events (MidiRecorder in midi_recorder.h). The recorder turns the channels and
// tracks that the performer follows into notes, controllers and presets. It keeps what a pass finds for the next pass.
// Addresses without another source come from the Wii U's nw::snd in Animal Crossing: amiibo Festival's executable.
//
// This file contains the settings, the LFOs, the releases and the stops. midi_recorder_outputs.cpp contains the outputs
// of the tracks, and midi_recorder_notes.cpp the notes and the commands.

#include "midi_recorder_internal.h"

namespace ntdlib::sequence_export
{

MidiRecorder::MidiRecorder() : rec_(RecordingRules())
{
    // A track's first part starts with the SFZ filters off, at their controllers' default of 0.
    for (int t = 0; t < kTrackCount; t++)
    {
        tracks_[t].output.midi_track = static_cast<uint8_t>(t);
        tracks_[t].output.sent[{0, kSfzLowPassEvent}] = 0;
        tracks_[t].output.sent[{0, kSfzBiquadEvent}] = 0;
    }
}

void MidiRecorder::OnTempo(Performer& engine)
{
    const double tempo = static_cast<double>(engine.tempo_) * engine.options_.tempo_ratio;
    const double bpm = engine.WiiOctober2006()
                           ? tempo * (60000.0 / engine.settings_.frame_ms / kTickUnits) / out_.timebase
                           : tempo * engine.timebase_ / out_.timebase;
    rec_.SetTempo(out_, engine.tick_, bpm);
}

void MidiRecorder::Emit(Performer& engine, int t, TrackEvent e, int part)
{
    e.tick = engine.tick_;
    e.part = static_cast<uint8_t>(part >= 0 ? part : tracks_[t].output.part);
    NoteChange(engine, t, e.kind);
    rec_.Emit(out_, tracks_[t].output, engine.tick_, e, e.part);
    if (IsTrackSetting(e.kind))
    {
        GiveTheFrameTheBiquad(engine, t, e);
    }
}

bool MidiRecorder::SendSetting(Performer& engine, int t, EventKind kind, int value, int part)
{
    if (!rec_.SendSetting(out_, tracks_[t].output, engine.tick_, kind, static_cast<uint8_t>(value), part))
    {
        return false;
    }

    TrackEvent e{engine.tick_, kind, 0, static_cast<uint8_t>(value)};
    e.part = static_cast<uint8_t>(part >= 0 ? part : tracks_[t].output.part);
    NoteChange(engine, t, kind);
    GiveTheFrameTheBiquad(engine, t, e);

    return true;
}

void MidiRecorder::NoteChange(Performer& engine, int t, EventKind kind)
{
    const bool counts = kind != EventKind::kReverb || engine.settings_.reverb_moves_loops;
    if (counts && kind != EventKind::kExpression && kind != kFilterEvent && kind != EventKind::kPitch)
    {
        engine.tracks_[t].last_change = engine.tick_;
    }
}

void MidiRecorder::GiveTheFrameTheBiquad(Performer& engine, int t, const TrackEvent& e)
{
    // The ticks played out after a sequence stops advancing have no recorded time (PlayOutStopped). No command switches
    // the biquad then.
    if (e.kind != kSfzBiquadEvent || engine.tick_ >= engine.tick_ms_.size())
    {
        return;
    }

    // Find the events of the frame's earlier ticks, just before the current tick's events.
    std::vector<TrackEvent>& events = out_.tracks[t];
    std::size_t end = events.size();
    while (end > 0 && events[end - 1].tick == engine.tick_)
    {
        end--;
    }

    const uint64_t frame = engine.FrameOf(engine.tick_ms_[engine.tick_]);
    std::size_t begin = end;
    while (begin > 0 && events[begin - 1].after == 0 &&
           engine.FrameOf(engine.tick_ms_[events[begin - 1].tick]) == frame)
    {
        begin--;
    }

    // The controller's value before the frame, 0 (off) if it was never sent.
    int value = 0;
    for (std::size_t k = begin; k > 0; k--)
    {
        if (events[k - 1].kind == kSfzBiquadEvent && events[k - 1].part == e.part)
        {
            value = events[k - 1].value;
            break;
        }
    }

    // Each of those notes on the part that would get the other choice gets a copy of the controller first.
    for (std::size_t k = begin; k < end; k++)
    {
        const TrackEvent& ev = events[k];
        if (ev.part != e.part)
        {
            continue;
        }

        if (ev.kind == kSfzBiquadEvent)
        {
            value = ev.value;
        }
        else if (ev.kind == EventKind::kNoteOn && (value != 0) != (e.value != 0))
        {
            TrackEvent copy = e;
            copy.tick = ev.tick;
            events.insert(events.begin() + static_cast<std::ptrdiff_t>(k), copy);
            k++;
            end++;
            value = e.value;
        }
    }
}

void MidiRecorder::Approximate(const char* what)
{
    rec_.Approximate(what);
}

void MidiRecorder::RecordTickTime(Performer& engine)
{
    engine.tick_ms_.push_back(engine.now_ms_);

    // The Wii's build of October 2006 runs a tick at the start of its frame. MIDI's tempo puts the tick at
    // `nominal_ms_`, up to a frame earlier.
    if (engine.WiiOctober2006())
    {
        out_.frame_ticks.push_back(static_cast<double>(engine.tick_) +
                                   (engine.now_ms_ - engine.nominal_ms_) / engine.MidiMsPerTick());
        return;
    }

    const double start = static_cast<double>(engine.frame_) * engine.settings_.frame_ms;
    rec_.RecordFrameStart(out_, engine.tick_ms_, engine.tick_, start);
}

void MidiRecorder::GiveStruckNotesLfo(Performer& engine, int t)
{
    // This tick's events, and the notes struck in them.
    Track& tr = engine.tracks_[t];
    std::vector<TrackEvent>& events = out_.tracks[t];
    const auto first = TickStart(events, engine.tick_);
    const std::set<uint32_t> struck = NotesStruck(first, events.end());
    if (struck.empty())
    {
        return;
    }

    // The preset each part had before this tick.
    std::map<uint8_t, int> sent = PresetsBefore(events, first);

    // Each note's preset with the track's LFO settings goes in front of it, in place of this tick's preset events. A
    // note with lfo_bends keeps its preset without the LFO.
    const PresetKey lfo = TrackPresetKey(tr);
    const std::vector<TrackEvent> tick_events = TickEventsWithLfo(engine, t, first, lfo, sent);
    events.erase(first, events.end());
    events.insert(events.end(), tick_events.begin(), tick_events.end());

    if (sent.contains(tracks_[t].output.part))
    {
        tracks_[t].output.sent[{tracks_[t].output.part, EventKind::kPreset}] = sent[tracks_[t].output.part];
    }

    for (Note& n : tr.notes)
    {
        if (struck.contains(n.midi.id))
        {
            n.midi.preset = WithLfo(n.midi.preset, lfo);
        }
    }
}

std::vector<TrackEvent> MidiRecorder::TickEventsWithLfo(const Performer& engine, int t,
                                                        std::vector<TrackEvent>::const_iterator first,
                                                        const PresetKey& lfo, std::map<uint8_t, int>& sent)
{
    std::set<uint32_t> bends;
    for (const Note& n : engine.tracks_[t].notes)
    {
        if (n.midi.lfo_bends)
        {
            bends.insert(n.midi.id);
        }
    }

    std::map<uint8_t, int> struck_with = sent;
    std::vector<TrackEvent> tick_events;
    for (auto it = first; it != out_.tracks[t].cend(); ++it)
    {
        if (it->kind == EventKind::kPreset)
        {
            struck_with[it->part] = static_cast<int>(it->preset);
            continue;
        }

        const int with = it->kind == EventKind::kNoteOn ? struck_with[it->part] : -1;
        if (with >= 0)
        {
            const int preset = bends.contains(it->note)
                                   ? with
                                   : static_cast<int>(seqlib::midi::PresetIndex(
                                         out_.presets, WithLfo(out_.presets[static_cast<std::size_t>(with)], lfo)));
            if (preset != sent[it->part])
            {
                TrackEvent ev{engine.tick_, EventKind::kPreset};
                ev.part = it->part;
                ev.preset = static_cast<uint32_t>(preset);
                tick_events.push_back(ev);
                sent[it->part] = preset;
            }
        }
        tick_events.push_back(*it);
    }

    return tick_events;
}

void MidiRecorder::CheckLfo(Performer& engine, int t)
{
    Track& tr = engine.tracks_[t];
    const PresetKey lfo = TrackPresetKey(tr);
    if (LfoDepth(tr.lfos[0]) == 0 || lfo.lfo_type > 2)
    {
        return;
    }

    for (Note& n : tr.notes)
    {
        if (n.midi.on && !n.midi.lfo_moved && n.EndMs() > engine.now_ms_ && !SameLfo(n.midi.preset, lfo))
        {
            // A pitch LFO that changes, without a volume or pan LFO before it, can go out as pitch bends.
            n.midi.lfo_moved = true;
            if (lfo.lfo_type == 0 && (n.midi.preset.lfo_type == 0 || n.midi.preset.lfo_type == 3))
            {
                lfo_changes_.insert(n.midi.id);
            }
            if (n.midi.lfo_crowded)
            {
                Approximate(
                    "LFO parameters changed during a note, with no MIDI channel free for its pitch bends (using "
                    "its note-on settings)");
            }
            else if (!n.midi.lfo_bends)
            {
                Approximate("LFO parameters changed during a note (using its note-on settings)");
            }
        }
    }
}

void MidiRecorder::MidiNoteOff(Performer& engine, int t, Note& n)
{
    if (!n.midi.on)
    {
        return;
    }

    n.midi.on = false;
    rec_.NoteOff(out_, static_cast<uint8_t>(t), engine.tick_, n.midi.part, n.midi.key, n.midi.id);
    NoteChange(engine, t, EventKind::kNoteOff);
}

void MidiRecorder::OnRelease(Performer& engine, int t, Note& n)
{
    RecordEarlyRelease(engine, n);
    EndMidiNote(engine, t, n);
}

void MidiRecorder::RecordEarlyRelease(Performer& engine, const Note& n)
{
    // The SoundFont attack starts with the MIDI note and reaches full level after Sf2AttackMs. A linear ramp to full
    // level over T milliseconds reaches the game's level L dB after t milliseconds when T = t * 10^(-L / 20).
    const uint64_t released_frame = engine.settings_.note_off_waits_for_update ? n.released_frame : engine.frame_;
    const double held_ms = static_cast<double>(released_frame - n.midi.frame) * engine.settings_.frame_ms;
    const double ramp_ms = Sf2AttackMs(n.env, engine.settings_);
    if (!n.midi.on || held_ms <= 0.0 || held_ms >= ramp_ms)
    {
        return;
    }

    const double level_db = n.release_level / 10.0;
    if (20.0 * std::log10(held_ms / ramp_ms) - level_db <= 1.0)
    {
        return;
    }

    const double attack_ms = held_ms * std::pow(10.0, -level_db / 20.0);
    early_releases_[n.midi.id] =
        static_cast<int16_t>(ToTimecents(attack_ms, seqlib::sf2::kShortestTimecents, seqlib::sf2::kLongestTimecents));
}

void MidiRecorder::OnVolumeLfo(uint32_t id, float envelope_db, float lfo_db, double scale)
{
    // The SoundFont and SFZ play the LFO at `x` decibels. The game raises a level by 6 dB at most (CalcVolumeRatio).
    // The voice holds the envelope's level plus the LFO's at full level. The game then plays the LFO at `y`. A frame
    // counts by the envelope's power. The note's quiet frames then count for little.
    const double envelope = envelope_db;
    const double game = std::min(static_cast<double>(lfo_db), 6.0);
    const double x = scale * lfo_db;
    const double y = std::min(0.0, envelope + game) - envelope;
    const double w = std::pow(10.0, envelope / 10.0);
    VolumeLfoSums& sums = lfo_sums_[id];
    sums.w += w;
    sums.x += w * x;
    sums.xx += w * x * x;
    sums.y += w * y;
    sums.xy += w * x * y;
    sums.held = sums.held || envelope + game > 0.0;
}

std::map<uint32_t, VolumeLfoFit> MidiRecorder::VolumeLfoFits() const
{
    // The SoundFont and SFZ play a fitted note's LFO at `swing` times `x`, `offset` decibels lower: the weighted
    // least-squares fit to the game's `y` (OnVolumeLfo). The swing stays between 0 and 1, and the offset at 0 or
    // below. A note whose envelope stays at full level then swings half as far, a third of the LFO's depth lower. The
    // fit's steps are 16ths of the swing and 5 centibels. Only the notes that the voice holds at full level get a fit.
    std::map<uint32_t, VolumeLfoFit> fits;
    for (const auto& [id, sums] : lfo_sums_)
    {
        const double det = sums.w * sums.xx - sums.x * sums.x;
        if (!sums.held || sums.w <= 0.0 || det <= 1e-12 * sums.w * sums.xx)
        {
            continue;
        }

        double swing = std::clamp((sums.w * sums.xy - sums.x * sums.y) / det, 0.0, 1.0);
        double offset = (sums.y - swing * sums.x) / sums.w;
        if (offset > 0.0)
        {
            offset = 0.0;
            swing = std::clamp(sums.xy / sums.xx, 0.0, 1.0);
        }

        VolumeLfoFit fit;
        fit.swing = static_cast<uint8_t>(std::lround(16.0 * swing));
        fit.cut = static_cast<int16_t>(5 * std::lround(-2.0 * offset));
        if (fit != VolumeLfoFit{})
        {
            fits[id] = fit;
        }
    }

    return fits;
}

void MidiRecorder::EndMidiNote(Performer& engine, int t, Note& n)
{
    if (!n.midi.on || !engine.Unheard(n))
    {
        MidiNoteOff(engine, t, n);
        return;
    }

    std::vector<TrackEvent>& events = out_.tracks[t];
    auto is_note_on = [&](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn && e.note == n.midi.id;
    };
    const auto on = std::find_if(events.rbegin(), events.rend(), is_note_on);
    if (on != events.rend())
    {
        events.erase(std::next(on).base());
    }

    n.midi.on = false;
}

bool MidiRecorder::Switch(Performer& engine, int t, const Note* except)
{
    // The new part's channel may hold another part's values from earlier. Its last values are dropped, and every
    // controller goes out again.
    const uint8_t old = tracks_[t].output.part;
    auto sounds = [&](uint8_t part)
    {
        return engine.PartSounds(t, part, except);
    };
    if (!rec_.Switch(out_, tracks_[t].output, engine.tick_, sounds))
    {
        return false;
    }

    tracks_[t].left[old].gain = tracks_[t].legato_gain;

    return true;
}

void MidiRecorder::OnStopAll(Performer& engine, int t)
{
    // CC120 stops the parts that the track's notes sounded on. Tails that the track detached on other parts go on.
    std::set<uint8_t> sounding;
    for (Note& n : engine.tracks_[t].notes)
    {
        if (n.HasMidiNote() && n.EndMs() > engine.now_ms_ && !engine.Unheard(n))
        {
            sounding.insert(n.midi.part);
        }
        EndMidiNote(engine, t, n);
    }

    for (const uint8_t part : sounding)
    {
        Emit(engine, t, {0, EventKind::kSoundOff}, part);
    }
}

void MidiRecorder::OnReleaseAll(Performer& engine, int t)
{
    UpdateOutputs(engine, t);
}

void MidiRecorder::OnReleasedAll(Performer& engine, int t)
{
    DetachAll(engine, t);
    LeaveTails(engine, t);
}

void MidiRecorder::LeaveTails(Performer& engine, int t)
{
    const Track& tr = engine.tracks_[t];
    auto tail = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == tracks_[t].output.part && engine.StillAudible(n);
    };
    if (std::ranges::any_of(tr.detached, tail))
    {
        Switch(engine, t);
    }
}

void MidiRecorder::DetachAll(Performer& engine, int t)
{
    // FreeAllChannel (0x022B6114). SequenceTrack::UpdateChannelParam (0x022B55B4) gives the channels the track's bend
    // before it detaches them. A detached note keeps that bend (PartPitch).
    Track& tr = engine.tracks_[t];
    for (Note& n : tr.notes)
    {
        n.bend = tr.Bend();
        tr.detached.push_back(n);
    }
    tr.notes.clear();
}

void MidiRecorder::OnStop(Performer& engine, int t, Note& n)
{
    // MIDI CC120 cuts the whole channel. Use it only for an audible note with no other sounds remaining on its part.
    const bool heard = n.HasMidiNote() && !engine.Unheard(n);
    EndMidiNote(engine, t, n);
    if (heard && n.EndMs() > engine.now_ms_)
    {
        if (!engine.PartSounds(t, n.midi.part, &n))
        {
            Emit(engine, t, {0, EventKind::kSoundOff}, n.midi.part);
        }
        else
        {
            Approximate("immediate channel stop (MIDI release tail remains)");
        }
    }
}

void MidiRecorder::OnTrackClosing(Performer& engine, int t)
{
    if (engine.TrackBusy(t))
    {
        UpdateOutputs(engine, t);
        rec_.RaiseLevelPeak(TrackLevel(engine, t) * tracks_[t].legato_gain);
    }
}

void MidiRecorder::OnTrackClosed(Performer& engine, int t)
{
    DetachAll(engine, t);
}

void MidiRecorder::OnEnd(Performer& engine, int t, Note& n)
{
    MidiNoteOff(engine, t, n); // the wave ended by itself
}

void MidiRecorder::OnLengthUpdated(Performer& engine, int t, Note& n)
{
    // The MIDI note of a channel that its length can't release, such as a tie, ends where the channel has faded to
    // kFadedAttenuation. The channel's sound ends there.
    if (!n.midi.sunk && (n.ignore_note_off || n.length < 0) && !n.released && engine.now_ms_ >= n.midi.faded_ms)
    {
        MidiNoteOff(engine, t, n);
        n.midi.sunk = true;
        engine.sound_end_ms_ = std::max(engine.sound_end_ms_, n.midi.faded_ms);
    }
}

} // namespace ntdlib::sequence_export
