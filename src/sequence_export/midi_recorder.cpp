// SPDX-License-Identifier: MIT

// The recording of a performance's MIDI events (MidiRecorder in performer_engine.h). The recorder turns the channels
// and tracks that the performer follows into notes, controllers and presets. It keeps what a pass finds for the next
// pass. Addresses without another source come from the Wii U's nw::snd in Animal Crossing: amiibo Festival's
// executable.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"
#include "seqlib/sf2/units.h"

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/performer_engine.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

using seqlib::sf2::SoundFontPan;
using seqlib::sf2::ToTimecents;

namespace
{

// The performer's rules for recording MIDI (seqlib/midi/recorder.h). nw::snd gives a frame's notes the settings that
// the frame ends with. A setting therefore goes before every event of its tick but a sound off. A part switch drops the
// new part's last values. A pitch that moves 0.0005 semitones or less isn't sent.
seqlib::midi::RecorderOptions RecordingRules()
{
    seqlib::midi::RecorderOptions options;
    options.settings_before_all_events = true;
    options.pitch_tolerance = 0.0005f;

    return options;
}

// The last value of setting `kind` sent to the part that `output`'s track plays on, or -1 if none was.
int LastSent(const seqlib::midi::TrackOutput& output, EventKind kind)
{
    const auto sent = output.sent.find({output.part, kind});
    return sent != output.sent.end() ? sent->second : -1;
}

// The first of `events` at tick `tick`, or their end. The events of a tick come last.
std::vector<TrackEvent>::iterator TickStart(std::vector<TrackEvent>& events, uint32_t tick)
{
    auto first = events.end();
    while (first != events.begin() && std::prev(first)->tick == tick)
    {
        --first;
    }

    return first;
}

// The notes that the note-ons from `first` to `last` strike.
std::set<uint32_t> NotesStruck(std::vector<TrackEvent>::const_iterator first,
                               std::vector<TrackEvent>::const_iterator last)
{
    std::set<uint32_t> struck;
    for (auto it = first; it != last; ++it)
    {
        if (it->kind == EventKind::kNoteOn)
        {
            struck.insert(it->note);
        }
    }

    return struck;
}

// The preset of each part that a note-on of `events` from `first` on strikes, before `first`: its last preset event's
// preset, or -1 for none.
std::map<uint8_t, int> PresetsBefore(const std::vector<TrackEvent>& events,
                                     std::vector<TrackEvent>::const_iterator first)
{
    std::map<uint8_t, int> sent;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kNoteOn && !sent.contains(it->part))
        {
            const uint8_t part = it->part;
            auto is_part_preset = [part](const TrackEvent& e)
            {
                return e.kind == EventKind::kPreset && e.part == part;
            };
            const auto back = std::find_if(std::make_reverse_iterator(first), events.rend(), is_part_preset);
            sent[part] = back != events.rend() ? static_cast<int>(back->preset) : -1;
        }
    }

    return sent;
}

} // namespace

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
    early_releases_[n.midi.id] = static_cast<int16_t>(ToTimecents(attack_ms, -12000, 8000));
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

void MidiRecorder::UpdateOutputs(Performer& engine, int t)
{
    // The values SequenceTrack::UpdateChannelParam (0x022B55B4) gives the track's channels.
    Track& tr = engine.tracks_[t];
    if (!tracks_[t].played)
    {
        return;
    }

    if (!tr.open)
    {
        SweepClosedTrack(engine, t);
        return;
    }

    // Channel gain is (vol * volume2 * main volume / 127^3)^2 * sound volume / 127 * (velocity / 127)^2. Each volume
    // can reach 255. SoundFont CC7, CC11 and velocity all act squared: put up to 127 of track volume in CC7, and the
    // excess, other volumes and legato velocity change in CC11. If CC11 exceeds 127, rerun with all CC11 values scaled
    // equally (Perform).
    PartControllers c;
    c.volume = std::clamp<int>(tr.vol.Get(), 0, 127);
    c.level = TrackLevel(engine, t);
    const int change_filter = FilterControllerValue(tracks_[t].filter.cutoff);
    NoteLevelChange(engine, t, c.level, change_filter);

    // nw::snd pan is value / 63, and the SoundFont CC10 modulator is (value - 64) / 64, on the synth's pan curve (see
    // seqlib/sf2/units.h).
    c.pan = std::clamp(static_cast<int>(std::lround(64.0 + SoundFontPan(tr.pan.Get() / 63.0) * 64.0)), 0, 127);
    ReportBalancePan(engine, t, c.pan);
    c.modulation = ModulationValue(tr.lfos[0], tr.lfo_types[0]);
    SetSends(engine, t, c);
    if (earlier_.filter_moves[t])
    {
        c.filter = change_filter;
    }
    SetSfzFilters(engine, t, c);

    // Detached notes keep their old settings in the game. In MIDI, those the track couldn't leave on another part
    // (LeaveTails) still share its channel and receive later controller changes.
    const bool tails = TailsOnPart(engine, t);
    auto control = [&](int value, EventKind kind)
    {
        if (SendSetting(engine, t, kind, value) && tails)
        {
            Approximate("detached notes (still receive track controller changes in MIDI)");
        }
    };
    SendControllers(c, rec_.Expression(c.level * tracks_[t].legato_gain), control);
    SendToLeftParts(engine, t, c);
    NotePitchChange(engine, t);
    SendPartPitches(engine, t, tails);
}

void MidiRecorder::SweepClosedTrack(Performer& engine, int t)
{
    for (uint8_t part = 0; part < kPartCount; part++)
    {
        if (engine.frame_ > 0 && engine.SweepMoves(t, part, engine.frame_ - 1, engine.now_ms_))
        {
            SendPitch(engine, t, part, engine.frame_);
        }
    }
}

void MidiRecorder::NoteLevelChange(Performer& engine, int t, double level, int change_filter)
{
    Track& tr = engine.tracks_[t];
    if (level != tr.change_level || change_filter != tr.change_filter)
    {
        if (engine.Wii() && level != tr.change_level)
        {
            CheckFullLevel(engine, t);
        }

        tr.change_level = level;
        tr.change_filter = change_filter;
        tr.last_change = engine.tick_;
    }
}

void MidiRecorder::ReportBalancePan(const Performer& engine, int t, int pan)
{
    const Track& tr = engine.tracks_[t];
    auto stereo_on_part = [&](const Note& n)
    {
        return n.stereo && n.midi.part == tracks_[t].output.part && n.EndMs() > engine.now_ms_;
    };
    if (pan != LastSent(tracks_[t].output, EventKind::kPan) && pan != 64 && engine.balance_ &&
        std::ranges::any_of(tr.notes, stereo_on_part))
    {
        Approximate("track pan on stereo waves in balance mode (they pan as in dual mode)");
    }
}

void MidiRecorder::SetSends(const Performer& engine, int t, PartControllers& c) const
{
    // The game's code sets up the effects on the aux buses, and a send to a bus without an effect is lost (sndcore2).
    // The games put an effect on bus A. Bus A becomes the synth's reverb. Bus B usually has none, and CC93 stays at 0.
    // A synth's default chorus send therefore adds nothing either.
    c.reverb = engine.settings_.sends == Sends::kNone ? 0 : engine.EffectSend(t, 0);
    if (engine.settings_.sends == Sends::kReverbDry &&
        (engine.options_.bus_reverbs[1] || engine.options_.bus_reverbs[2]))
    {
        // A reverb on bus B or C takes that bus's send too (PerformOptions::bus_reverbs). CC91 stands in for every
        // reverb. A delay on bus A then leaves CC91 (PerformOptions::bus_a_delay).
        if (engine.options_.bus_a_delay)
        {
            c.reverb = 0;
        }
        for (int bus = 1; bus < 3; ++bus)
        {
            if (engine.options_.bus_reverbs[static_cast<std::size_t>(bus)])
            {
                c.reverb = std::min(127, c.reverb + engine.EffectSend(t, bus));
            }
        }
    }
    if (engine.settings_.sends == Sends::kReverb || engine.settings_.sends == Sends::kNone)
    {
        c.chorus = 0;
    }
    else if (engine.settings_.sends == Sends::kReverbChorus)
    {
        c.chorus = engine.EffectSend(t, 1);
    }
}

void MidiRecorder::SetSfzFilters(const Performer& engine, int t, PartControllers& c)
{
    // SFZ controllers hold the original filter settings. Each track file uses its first enabled biquad type; omit later
    // types.
    const Track& tr = engine.tracks_[t];
    c.sfz_low_pass = SfzLowPassValue(tr.low_pass);
    c.sfz_biquad = SfzBiquadValue(tr.biquad_type, tr.biquad_value, engine.settings_);
    if (c.sfz_biquad != 0 && out_.biquad_types[t] == 0)
    {
        out_.biquad_types[t] = static_cast<uint8_t>(tr.biquad_type);
    }
    if (tr.biquad_type != out_.biquad_types[t])
    {
        c.sfz_biquad = 0;
    }
}

void MidiRecorder::SendToLeftParts(Performer& engine, int t, const PartControllers& c)
{
    // As in the game, a part that the track has left but whose notes it still holds follows the track's changes. The
    // part takes them at the level its notes have (their legato gain). A part that holds only notes that the track
    // detached keeps their settings.
    const Track& tr = engine.tracks_[t];
    for (uint8_t part = 0; part < kPartCount; part++)
    {
        auto held = [&](const Note& n)
        {
            return n.HasMidiNote() && n.midi.part == part && engine.Heard(n);
        };
        if (part == tracks_[t].output.part || !std::ranges::any_of(tr.notes, held))
        {
            continue;
        }

        const double gain = tracks_[t].left[part].gain;
        auto follow = [&](int value, EventKind kind)
        {
            SendSetting(engine, t, kind, value, part);
        };
        SendControllers(c, rec_.Expression(c.level * gain), follow);
        rec_.RaiseLevelPeak(c.level * gain);
    }
}

void MidiRecorder::NotePitchChange(Performer& engine, int t)
{
    // MIDI has one pitch per channel; nw::snd has one per note. For change detection, take the track's bend and the
    // newest sweep of any part, without the legato key offset. The game's notes have no parts (Track::last_change). A
    // note below hearing changes nothing that can be heard.
    Track& tr = engine.tracks_[t];
    float game_pitch = tr.Bend();
    for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
    {
        const float s = it->SweepAt(engine.frame_);
        if (s != 0.0f && !engine.BelowHearing(*it))
        {
            game_pitch += s;
            break;
        }
    }
    if (std::fabs(game_pitch - tr.change_pitch) > 0.0005f || (game_pitch != tr.change_pitch && game_pitch == 0.0f))
    {
        tr.change_pitch = game_pitch;
        tr.last_change = engine.tick_;
    }
}

void MidiRecorder::SendPartPitches(Performer& engine, int t, bool tails)
{
    // Each part gets the pitch of its notes, the track's first.
    if (SendPitch(engine, t, tracks_[t].output.part, engine.frame_) && tails)
    {
        Approximate("detached notes (still receive track controller changes in MIDI)");
    }
    for (uint8_t part = 0; part < kPartCount; part++)
    {
        if (part != tracks_[t].output.part)
        {
            SendPitch(engine, t, part, engine.frame_);
        }
    }
}

std::optional<float> MidiRecorder::PartPitch(const Performer& engine, int t, uint8_t part, uint64_t frame) const
{
    const Track& tr = engine.tracks_[t];
    if (part == tracks_[t].output.part && tr.open)
    {
        const float pitch = tr.Bend() + tracks_[t].legato_bend;
        for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
        {
            const float s = it->SweepAt(frame);
            if ((s != 0.0f || it->midi.lfo_bends) && it->midi.part == part && it->HasMidiNote() && engine.Heard(*it))
            {
                return it->midi.lfo_bends ? pitch + s + it->LfoPitchAt(frame) : pitch + s;
            }
        }

        return pitch;
    }

    const Note* newest = nullptr;
    bool held = false;
    float sweep = 0.0f;
    auto look = [&](const Note& n, bool holds)
    {
        if (!n.HasMidiNote() || n.midi.part != part || !engine.Heard(n))
        {
            return;
        }

        if (!newest)
        {
            newest = &n;
            held = holds;
        }
        sweep = sweep != 0.0f ? sweep : n.SweepAt(frame);
    };
    std::for_each(tr.notes.rbegin(), tr.notes.rend(), [&](const Note& n) { look(n, true); });
    std::for_each(tr.detached.rbegin(), tr.detached.rend(), [&](const Note& n) { look(n, false); });
    if (!newest)
    {
        return std::nullopt;
    }

    // The game gives the notes a track holds its bend in the frame of the tick that changes it, after this tick's
    // events (SequenceTrack::UpdateChannelParam). Detached notes keep theirs.
    const float bend = held ? tr.Bend() : newest->bend;
    const float pitch = bend + static_cast<float>(newest->key - newest->midi.key) + sweep;
    return newest->midi.lfo_bends ? pitch + newest->LfoPitchAt(frame) : pitch;
}

bool MidiRecorder::SendPitch(Performer& engine, int t, uint8_t part, uint64_t frame, double after)
{
    const std::optional<float> pitch = PartPitch(engine, t, part, frame);
    if (pitch && *pitch != 0.0f)
    {
        // The notes past a pitch limit on the part move away from their keys' pitch (PassFindings::pitch_moves).
        const Track& tr = engine.tracks_[t];
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.midi.past_limit && n.HasMidiNote() && n.midi.part == part)
                {
                    pitch_moves_.insert(n.midi.id);
                }
            }
        }
    }

    return pitch && rec_.SendPitch(out_, tracks_[t].output, engine.tick_, *pitch, part, after);
}

bool MidiRecorder::TailsOnPart(const Performer& engine, int t) const
{
    const Track& tr = engine.tracks_[t];
    auto shares_part = [&](const Note& n)
    {
        return n.HasMidiNote() && n.midi.part == tracks_[t].output.part && engine.StillAudible(n);
    };

    return std::ranges::any_of(tr.detached, shares_part);
}

void MidiRecorder::SweepBetweenTicks(Performer& engine, const TickTime& next)
{
    // Channel::Update moves a timed sweep and an LFO on every sound frame. That includes the frames between ticks and
    // the frames after the track has closed. The track's settings hold still between ticks. Find the parts with a sweep
    // under way, and the parts whose pitch bends add a note's pitch LFO.
    std::array<std::array<bool, kPartCount>, kTrackCount> sweeping{};
    std::array<std::array<bool, kPartCount>, kTrackCount> lfo{};
    bool any = false;
    for (int t = 0; t < kTrackCount; t++)
    {
        const Track& tr = engine.tracks_[t];
        if (!tracks_[t].played)
        {
            continue;
        }

        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.HasMidiNote() && n.sweep_in_ms && n.SweepAt(engine.frame_) != 0.0f)
                {
                    sweeping[t][n.midi.part] = true;
                    any = true;
                }
                if (n.HasMidiNote() && n.midi.lfo_bends && engine.Heard(n))
                {
                    sweeping[t][n.midi.part] = true;
                    lfo[t][n.midi.part] = true;
                    any = true;
                }
            }
        }
    }
    if (!any)
    {
        return;
    }

    // The core has run the current tick's frame. The LFO's value in that frame goes at the tick, in place of the latest
    // value that the tick sent.
    for (int t = 0; t < kTrackCount; t++)
    {
        for (uint8_t part = 0; part < kPartCount; part++)
        {
            if (lfo[t][part] && SendPitch(engine, t, part, engine.frame_) && engine.tracks_[t].open &&
                part == tracks_[t].output.part && TailsOnPart(engine, t))
            {
                Approximate("detached notes (still receive track controller changes in MIDI)");
            }
        }
    }

    // Send each frame's pitch where the frame starts. Give its time as a fraction of the way to the next tick.
    for (uint64_t f = engine.frame_ + 1; f < next.frame; f++)
    {
        // On the Wii's build of October 2006, the fraction counts at MIDI's tempo from the tick's nominal time
        // (RecordTickTime).
        const double start = static_cast<double>(f) * engine.settings_.frame_ms;
        const double after = engine.WiiOctober2006() ? (start - engine.nominal_ms_) / engine.MidiMsPerTick()
                                                     : (start - engine.now_ms_) / (next.ms - engine.now_ms_);
        for (int t = 0; t < kTrackCount; t++)
        {
            for (uint8_t part = 0; part < kPartCount; part++)
            {
                const Track& tr = engine.tracks_[t];
                if (sweeping[t][part] && SendPitch(engine, t, part, f, after) && tr.open &&
                    part == tracks_[t].output.part && TailsOnPart(engine, t))
                {
                    Approximate("detached notes (still receive track controller changes in MIDI)");
                }
            }
        }
    }
}

double MidiRecorder::TrackLevel(const Performer& engine, int t) const
{
    // On the Switch, a voice's main bus takes the main send, and its aux buses B and C the effect sends. Sub-mix 0
    // passes every bus to the final mix at 1.0. A bus with no effect reaches the mix dry, in phase with the main bus
    // (MultiVoice::CalcMixVolumeForBuses, 0xc25fc0). Those sends therefore add up to the note's level. Bus A's send
    // goes through the game's reverb. CC91 stands in for that reverb. The sound's sends on buses B and C add to the
    // track's sends (PerformOptions::fx_sends).
    //
    // The volume that the game sets through the sound's handle multiplies the channel's volume (Policy::TrackVolume).
    // A track that PerformOptions::tracks leaves out has volume 0 there. The sound's volume from its handle
    // (PerformOptions::volume) multiplies its volume from the archive.
    const Track& tr = engine.tracks_[t];
    const double handle = std::sqrt(std::max(0.0f, tr.handle_volume));
    if (engine.settings_.sends == Sends::kReverbDry)
    {
        const std::array<float, 3>& sends = engine.options_.fx_sends;
        const std::array<std::optional<float>, 3>& reverbs = engine.options_.bus_reverbs;
        double dry = (tr.main_send + tr.fx_send_b + tr.fx_send_c) / 127.0 + sends[1] + sends[2];
        if (reverbs[1] || reverbs[2])
        {
            // A bus with a reverb passes on only its dry gain's share of the send (PerformOptions::bus_reverbs). The
            // reverb's part goes to CC91 (EffectSend). A bus that gets no send leaves the level exactly as it was.
            dry -= (1.0 - reverbs[1].value_or(1.0f)) * (tr.fx_send_b / 127.0 + sends[1]) +
                   (1.0 - reverbs[2].value_or(1.0f)) * (tr.fx_send_c / 127.0 + sends[2]);
            dry = std::max(dry, 0.0);
        }
        return std::max<int>(tr.vol.Get(), 127) * (tr.volume2.Get() / 127.0) * (engine.main_volume_.Get() / 127.0) *
               std::sqrt(engine.sound_.volume / 127.0 * engine.options_.volume * dry) * handle;
    }

    // The Wii's voice clamps a channel's volume to 1 (Voice::SetVolume), and the sound's volume to 1 too. The track's
    // volumes never take a note above full level. Limit them to full level, the most CC7 and CC11 can give together. A
    // note's zones take the rest (ZoneLift).
    if (engine.Wii())
    {
        const double level =
            std::max<int>(tr.vol.Get(), 127) * (tr.volume2.Get() / 127.0) * (engine.main_volume_.Get() / 127.0) *
            std::sqrt(std::min<int>(engine.sound_.volume, 127) / 127.0 * engine.options_.volume) * handle;
        return std::min(level, 127.0 * 127.0 / std::clamp<int>(tr.vol.Get(), 1, 127));
    }

    return std::max<int>(tr.vol.Get(), 127) * (tr.volume2.Get() / 127.0) * (engine.main_volume_.Get() / 127.0) *
           std::sqrt(engine.sound_.volume / 127.0 * engine.options_.volume) * handle;
}

int16_t MidiRecorder::ZoneLift(const Performer& engine, int t, int velocity) const
{
    // A channel's volume is the velocity squared, the region's volume and the track's volumes (UserVolume) multiplied
    // together. MIDI's velocities stop at 127. Its track volumes stop at full level (TrackLevel). The zones take the
    // rest.
    const double above = std::max(velocity, 127) / 127.0;
    const double rest = above * above * std::max(1.0f, engine.UserVolume(t));
    return static_cast<int16_t>(std::lround(200.0 * std::log10(rest)));
}

void MidiRecorder::CheckFullLevel(Performer& engine, int t)
{
    // The game limits a channel's volume to full level (Note::init_volume). MIDI plays a note at its level before the
    // track's volumes (MidiNote::level), times the track's volumes up to full level (TrackLevel). Report a note whose
    // level differs from its level in the game by more than 0.1 dB. That happens to a note below velocity 127 whose
    // zone reaches full level before the note does (ZoneLift). It also happens to a note whose track's volumes change
    // while it plays. After a legato, CC11 sets the level. A note with lifted zones then plays at another level. So
    // does a quieter note while the track's volumes are above full level.
    constexpr double kTenthDb = 1.0115794542598986; // 10^(0.1 / 20)
    Track& tr = engine.tracks_[t];
    const float user = engine.UserVolume(t);
    for (Note& n : tr.notes)
    {
        if (n.midi.over_full_level || !n.midi.on)
        {
            continue;
        }

        const double game = std::min(1.0, static_cast<double>(n.init_volume) * user);
        const double midi = static_cast<double>(n.midi.level) * std::min(1.0f, user);
        const bool differs = n.midi.level >= 0.0f ? midi > game * kTenthDb || game > midi * kTenthDb
                                                  : n.midi.lift > 0 || (user > 1.0f && n.init_volume < 1.0f);
        if (differs)
        {
            n.midi.over_full_level = true;
            Approximate("channel volume above full level (limited in the game; MIDI plays the note at another level)");
        }
    }
}

void MidiRecorder::MidiNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region,
                              double gain)
{
    n.midi.key = n.key;
    n.midi.lfo_bends = false;
    n.midi.lfo_crowded = false;
    n.midi.past_limit = false;
    n.midi.at_limits = false;
    if (velocity <= 0)
    {
        return; // silent in nw::snd; MIDI velocity 0 would be a note-off
    }

    // On the Wii, the voice limits the channel's volume to full level (Note::init_volume). The note's zones take the
    // part of the volume above what MIDI's velocity and track controllers reach (ZoneLift). A new note that a legato
    // starts takes its level from CC11 instead (OnLegato).
    n.midi.lift = engine.Wii() && gain == 1.0 ? ZoneLift(engine, t, velocity) : 0;
    PlaceNote(engine, t, n, velocity, gain);

    // A note that legatos raise later (PassFindings::rises) starts at the highest velocity that they reach and that its
    // region still plays. That way the synth selects the same zone, and CC11 brings the note down to the velocity it
    // plays at. The legatos then continue the note (Legato). That works only for a note alone on its part (Alone).
    const int capped = std::min(velocity, 127);
    n.midi.liftable = engine.Alone(t, n);
    const int start = RaisedVelocity(engine, n, capped, region);

    // A note above velocity 127 plays at 127. If the note is alone on its part, CC11 raises it the rest of the way. On
    // the Wii, the zones do.
    double above = 1.0;
    if (velocity > 127 && n.midi.lift == 0)
    {
        if (n.midi.liftable)
        {
            above = static_cast<double>(velocity) / 127.0;
        }
        else
        {
            Approximate("velocity above 127 (played at 127)");
        }
    }

    tracks_[t].legato_gain = gain * capped / start * above;
    tracks_[t].legato_bend = 0.0f;
    UpdateOutputs(engine, t); // the controllers, and the pitch of a sweeping note, come first
    ReportNoteApproximations(engine, t, n);
    SendNotePreset(engine, t, n, region);
    StartMidiNote(engine, t, n, velocity, start, region, gain);
}

void MidiRecorder::PlaceNote(Performer& engine, int t, Note& n, int velocity, double gain)
{
    // A note that requires a pitch or level that the other notes sounding on the track's part don't share moves the
    // track to another part (Switch), and the note plays there. Each part has a MIDI channel of its own. A note that
    // sweeps, or that starts beside one still sweeping, moves the track. So does a note above velocity 127 whose zones
    // don't take the rest. A note that later legatos raise moves the track too. Such a note starts higher (below). A
    // note moves the track if its CC11, or the CC11 a legato lowered, would change the level of the others. Last, a
    // note whose pitch LFO an earlier pass found changing (PassFindings::lfo_changes) moves the track. So does a note
    // that starts beside one. Alone on its part, the note plays without the LFO in its preset. The pitch bends of the
    // part add the core's LFO (lfo_bends).
    const Track& tr = engine.tracks_[t];
    const bool rises = earlier_.rises.contains(rec_.NextNote());
    const bool lfo_changes_here = earlier_.lfo_changes.contains(rec_.NextNote());
    auto sweeps_otherwise = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.midi.part == tracks_[t].output.part &&
               engine.Heard(other) && (!n.SweepsLike(other, engine.frame_) || other.midi.lfo_bends);
    };
    const bool own_settings = std::ranges::any_of(tr.notes, sweeps_otherwise) ||
                              std::ranges::any_of(tr.detached, sweeps_otherwise) || rises ||
                              (velocity > 127 && n.midi.lift == 0) || gain != 1.0 || tracks_[t].legato_gain != 1.0 ||
                              lfo_changes_here;
    if (own_settings && !engine.Alone(t, n))
    {
        Switch(engine, t, &n);
    }
    n.midi.part = tracks_[t].output.part;
    n.midi.lfo_bends = lfo_changes_here && engine.Alone(t, n);
    n.midi.lfo_crowded = lfo_changes_here && !n.midi.lfo_bends;
}

int MidiRecorder::RaisedVelocity(const Performer& engine, const Note& n, int capped, const VelocityRegion* region) const
{
    const auto rise = earlier_.rises.find(rec_.NextNote());
    int start = capped;
    if (n.midi.liftable && rise != earlier_.rises.end() && region)
    {
        for (int v = std::min(rise->second, 127); v > capped; v--)
        {
            if (engine.banks_.FindRegion(n.bank_slot, n.program, n.key, v) == region)
            {
                start = v;
                break;
            }
        }
    }

    return start;
}

void MidiRecorder::SendNotePreset(Performer& engine, int t, Note& n, const VelocityRegion* region)
{
    const Track& tr = engine.tracks_[t];
    const auto attack = earlier_.early_releases.find(rec_.NextNote());
    n.midi.attack_timecents =
        attack != earlier_.early_releases.end() ? std::optional<int16_t>(attack->second) : std::nullopt;
    const auto fit = earlier_.lfo_fits.find(rec_.NextNote());
    n.midi.lfo_fit = fit != earlier_.lfo_fits.end() ? fit->second : VolumeLfoFit{};
    // AX on the Wii U and nn::atk on the Switch hold a voice's pitch within the console's limits (resampling.h). A note
    // past a limit whose pitch holds still plays at the limit (PresetKey::at_pitch_limits). A note that starts with a
    // sweep, an LFO in its part's bends or a bend moves. An earlier pass found the notes that a later bend or LFO moves
    // (PassFindings::pitch_moves).
    const Pcm* limited = region ? engine.banks_.GetWave(n.bank_slot, region->wave_id_index) : nullptr;
    n.midi.past_limit =
        limited &&
        PastPitchLimit(n.midi.key, {region->original_key, region->pitch, limited->sample_rate, region->interpolation},
                       engine.settings_);
    const bool moves = n.SweepAt(engine.frame_) != 0.0f || n.midi.lfo_bends ||
                       tr.Bend() + tracks_[t].legato_bend != 0.0f || earlier_.pitch_moves.contains(rec_.NextNote());
    n.midi.at_limits = n.midi.past_limit && !moves;
    n.midi.preset = engine.NoteKey(t, n);
    n.midi.lfo_moved = false;

    // The SoundFont leaves the zone's low-pass for the console's resampling filter out of a preset whose filter changes
    // during notes (ZoneSpans).
    const Pcm* wave =
        region && n.midi.preset.filter_controller ? engine.banks_.GetWave(n.bank_slot, region->wave_id_index) : nullptr;
    if (wave &&
        ResamplingFilterOf(n.midi.key, {region->original_key, region->pitch, wave->sample_rate, region->interpolation},
                           engine.settings_))
    {
        Approximate("resampling filter on a preset whose filter changes during notes (omitted from SoundFont)");
    }
    const PresetKey sent = n.midi.lfo_bends ? WithoutLfo(n.midi.preset) : n.midi.preset;
    if (rec_.SendPreset(out_, tracks_[t].output, engine.tick_, seqlib::midi::PresetIndex(out_.presets, sent)))
    {
        NoteChange(engine, t, EventKind::kPreset);
    }
}

void MidiRecorder::StartMidiNote(Performer& engine, int t, Note& n, int velocity, int start,
                                 const VelocityRegion* region, double gain)
{
    // Assign IDs to overlapping notes of the same key so the MIDI writer can distribute them across channels.
    n.midi.on = true;
    n.midi.velocity = static_cast<uint8_t>(start);
    n.midi.start_velocity = velocity;
    n.midi.frame = engine.frame_;
    n.midi.id = rec_.NoteOn(out_, tracks_[t].output, engine.tick_, n.midi.key, n.midi.velocity);
    out_.note_commands.resize(n.midi.id);
    out_.note_commands[n.midi.id - 1] = engine.settling_command_;
    NoteChange(engine, t, EventKind::kNoteOn);
    if (engine.Wii())
    {
        // The note's level before the track's volumes: the velocity's level times the zone's level, up to full level.
        // CC11 sets the level of a note that a legato starts.
        const int capped = std::min(velocity, 127);
        const double zone = region ? region->volume / 127.0 * std::pow(10.0, n.midi.lift / 200.0) : 0.0;
        n.midi.level =
            gain == 1.0 ? static_cast<float>(capped * capped / (127.0 * 127.0) * std::min(1.0, zone)) : -1.0f;
        CheckFullLevel(engine, t);
    }
}

void MidiRecorder::ReportNoteApproximations(Performer& engine, int t, const Note& n)
{
    const Track& tr = engine.tracks_[t];
    const nwsnd::CurveLfoParam& lfo = tr.lfos[0];
    const uint8_t type = tr.lfo_types[0];
    const int depth = LfoDepth(lfo);
    if (type == 2 && depth != 0 && LfoStarts(lfo))
    {
        Approximate("pan LFO (omitted from SoundFont)");
    }
    if (type == 1 && depth * lfo.range > 128 && LfoStarts(lfo))
    {
        Approximate("volume LFO deeper than 6 dB (limited to 6 dB: it dips less than in the game)");
    }
    // The pitch bends of a note with lfo_bends follow the curve and the phase of the core's LFO. TakeCoreLfos reports a
    // random curve there.
    if (depth != 0 && LfoStarts(lfo) && (lfo.curve != 0 || lfo.phase != 0) && !n.midi.lfo_bends)
    {
        Approximate("LFO curve other than a sine, or a phase (using a sine from the cycle's start)");
    }
    if (std::any_of(tr.lfos.begin() + 1, tr.lfos.end(), [](const nwsnd::CurveLfoParam& s) { return LfoDepth(s) != 0; }))
    {
        Approximate("second to fourth LFOs (omitted)");
    }
    if (engine.settings_.sends == Sends::kReverb && engine.EffectSend(t, 1) != 0)
    {
        Approximate("effect send B (omitted)");
    }
    if ((engine.settings_.sends == Sends::kReverb || engine.settings_.sends == Sends::kReverbChorus) &&
        engine.EffectSend(t, 2) != 0)
    {
        Approximate("effect send C (omitted)");
    }

    const GameFilters filters = TrackFilters(tr.low_pass, tr.biquad_type, tr.biquad_value, engine.settings_);
    if (filters.low_pass >= 0)
    {
        Approximate("low-pass filter (one-pole in the game, two-pole in the SoundFont)");
    }
    if (BiquadShape(filters.biquad_type, engine.settings_) >= 2)
    {
        Approximate("biquad high-pass or band-pass (omitted from SoundFont)");
    }
    if (filters.low_pass >= 0 && BiquadShape(filters.biquad_type, engine.settings_) == 1)
    {
        Approximate("combined low-pass filters (SoundFont uses the lower cutoff)");
    }
    if (filters.biquad_type != 0 && filters.biquad_type != out_.biquad_types[t])
    {
        Approximate("biquad type change (SFZ supports one type per track)");
    }
}

void MidiRecorder::OnNoteOnStart(Performer& engine, int t)
{
    if (!tracks_[t].played)
    {
        tracks_[t].played = true;
        UpdateOutputs(engine, t);
    }
}

void MidiRecorder::OnNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region, bool stereo)
{
    // Use nw::snd's default pan curve and dual mode for stereo waves. On the Wii, the default pan curve, with the
    // sound's pan mode.
    if (engine.Wii())
    {
        if (velocity > 0 && engine.sound_.pan_curve != 0)
        {
            Approximate("custom pan curve (using the default)");
        }
    }
    else if (velocity > 0 && (engine.sound_.pan_curve != 0 || (engine.sound_.pan_mode != 0 && stereo)))
    {
        Approximate("custom pan curve or mode (using defaults)");
    }

    // nw::snd adds region pan, track initial pan and track pan before applying its pan law. The synth receives the
    // first two as zone pan and the third as CC10, then combines them by the synth's pan law.
    if (velocity > 0 && region->pan + engine.tracks_[t].init_pan != 64 && engine.tracks_[t].pan.Get() != 0)
    {
        Approximate("combined region and track pan (using the synth pan law)");
    }

    // In the Wii's balance mode a stereo wave's channels keep to their sides, but CC10 pans them as in dual mode.
    if (velocity > 0 && stereo && engine.balance_ && engine.tracks_[t].pan.Get() != 0)
    {
        Approximate("track pan on stereo waves in balance mode (they pan as in dual mode)");
    }

    // AX uses the region's interpolation mode. The SFZ files follow linear interpolation. SoundFont synths use their
    // usual interpolation. That interpolation most closely matches the default mode.
    if ((engine.settings_.engine == Engine::kCafe || engine.settings_.engine == Engine::kCtr) && velocity > 0 &&
        region->interpolation == 1)
    {
        Approximate("linear sample interpolation (SoundFont uses the synth's interpolation)");
    }
    else if (engine.settings_.engine == Engine::kCafe && velocity > 0 && region->interpolation != 0)
    {
        Approximate("uninterpolated sample playback (using synth interpolation)");
    }
    else if (engine.settings_.engine == Engine::kCtr && velocity > 0 && region->interpolation == 2)
    {
        Approximate("uninterpolated sample playback (SoundFont uses the synth's interpolation)");
    }

    MidiNoteOn(engine, t, n, velocity, region);
}

bool MidiRecorder::OnLegato(Performer& engine, int t, Note& n, int key, int velocity, bool past_cap, bool heard)
{
    if (heard)
    {
        n.midi.quiet_ms = n.QuietMs();
        n.midi.faded_ms = n.FadedMs();
    }

    // A channel that has faded far enough goes on silent. Its MIDI note has ended (MidiRecorder::OnLengthUpdated).
    // Nothing starts another.
    if (!heard && !n.midi.on)
    {
        return false;
    }

    // The tie path of SequenceTrack::NoteOn (0x022C9A4C) sets only the channel's velocity volume. The channel keeps the
    // volume of the region it started on. The 3DS's and the Wii's nw::snd drop that volume. The synth zone applies the
    // volume of the region the MIDI note started on. Once a legato has started a new MIDI note on another region, CC11
    // makes up the difference. A zero-volume zone can't be made audible this way.
    //
    // The 3DS's and the Wii's SequenceTrack::NoteOn set the gain from the new velocity without the region's volume
    // (Settings::keep_first_region_volume). CC11 then takes out the volume of the region that the synth plays
    // (WithFirstRegionVolume).

    // Continue the MIDI note if its preset matches. CC11 handles the velocity and pitch bend the key (UpdateOutputs).
    // The note keeps its sample position and original region. At velocity 0, keep the note silent through CC11
    // regardless of preset; ending it would leave a release tail.
    //
    // MIDI can't raise a note's velocity. A legato above the velocity that a MIDI note alone on its track started at
    // therefore raises CC11 above the note's start. Record it. A later pass then starts the note higher (MidiNoteOn);
    // CC11 above 127, past the top velocity of the note's region, lowers the whole sequence (Perform). A note that
    // isn't alone requires a new MIDI note. Lowering CC11 for it would lower the others too.
    //
    // After Settings::voices legatos in a tick, continue regardless of preset, capped at the starting velocity. Count
    // only legatos that would otherwise require a new MIDI note as approximations.
    //
    // A legato on a channel below hearing continues its MIDI note the same way. The game's legato doesn't start the
    // envelope again. A new MIDI note, or a later pass that starts the note higher, would change what can be heard. The
    // gain of such a legato therefore stays at or below the note's start, and the legato records no rise.
    const bool carries_on =
        n.midi.on && (velocity <= n.midi.velocity || n.midi.liftable) && (velocity <= 0 || SamePreset(engine, t, n));
    if (carries_on || (n.midi.on && (past_cap || !heard)))
    {
        ContinueMidiNote(engine, t, n, key, velocity, carries_on, heard);
        return heard;
    }

    // Start a new MIDI note. Select its region by the new key and velocity. A legato at velocity 0 starts none.
    const VelocityRegion* played = engine.banks_.FindRegion(n.bank_slot, n.program, key, velocity);
    n.midi.volume = played ? played->volume : 127;
    EndMidiNote(engine, t, n);
    MidiNoteOn(engine, t, n, velocity, played, WithFirstRegionVolume(engine, n, velocity, 1.0));
    if (n.midi.on)
    {
        Approximate("legato (tie or mono) played as a new note");
    }

    return heard;
}

double MidiRecorder::WithFirstRegionVolume(const Performer& engine, const Note& n, int velocity, double gain)
{
    const int first_volume = engine.settings_.keep_first_region_volume ? n.region_volume : 127;
    if (n.midi.volume == 0 && first_volume > 0 && velocity > 0)
    {
        Approximate("legato on a zero-volume region (audible in the game, silent in the synth)");
    }

    return n.midi.volume > 0 ? gain * std::sqrt(static_cast<double>(first_volume) / n.midi.volume) : gain;
}

bool MidiRecorder::SamePreset(const Performer& engine, int t, const Note& n) const
{
    // Envelope settings that the track leaves to the bank (0xff), such as those after an envelope reset, leave the
    // channel's settings as they were (SequenceTrack::NoteOn). They therefore count as the MIDI note's settings.
    const PresetKey& sent = out_.presets[static_cast<std::size_t>(LastSent(tracks_[t].output, EventKind::kPreset))];
    PresetKey current = n.midi.lfo_bends ? WithoutLfo(engine.NoteKey(t, n)) : engine.NoteKey(t, n);
    for (auto [kept, from] : {std::pair{&current.attack, &sent.attack}, std::pair{&current.decay, &sent.decay},
                              std::pair{&current.sustain, &sent.sustain}, std::pair{&current.release, &sent.release},
                              std::pair{&current.hold, &sent.hold}})
    {
        if (*kept == 0xff)
        {
            *kept = *from;
        }
    }

    return current == sent;
}

void MidiRecorder::ContinueMidiNote(Performer& engine, int t, Note& n, int key, int velocity, bool carries_on,
                                    bool heard)
{
    if (!carries_on && heard)
    {
        const std::string what = "more than " + std::to_string(engine.settings_.voices) +
                                 " legatos in a tick (keeping the MIDI preset and limiting gain to the starting level)";
        Approximate(what.c_str());
    }
    else if (carries_on && heard && velocity > n.midi.start_velocity)
    {
        int& top = rises_[n.midi.id];
        top = std::max(top, velocity);
    }

    const int kept = carries_on && heard ? std::max(velocity, 0) : std::clamp<int>(velocity, 0, n.midi.velocity);
    tracks_[t].legato_gain = WithFirstRegionVolume(engine, n, velocity, static_cast<double>(kept) / n.midi.velocity);
    tracks_[t].legato_bend = static_cast<float>(key - n.midi.key);
    if (engine.Wii())
    {
        n.midi.level = -1.0f; // CC11 sets the level from here (CheckFullLevel)
        CheckFullLevel(engine, t);
    }
}

void MidiRecorder::OnCommand(Performer& engine, int /*t*/, uint32_t cmd, int32_t arg1)
{
    const uint8_t a8 = static_cast<uint8_t>(arg1);
    switch (cmd)
    {
    case 0xd7: // surround pan: the Wii's stereo mix leaves it out
        if (!engine.settings_.surround_pan && arg1 != 0)
        {
            Approximate("surround pan (omitted)");
        }
        break;

    case 0xd9:
        if (engine.settings_.sends == Sends::kNone && a8 != 0)
        {
            Approximate("effect send A (omitted)");
        }
        break;

    case 0xda:
        if (engine.settings_.sends == Sends::kNone && a8 != 0)
        {
            Approximate("effect send B (omitted)");
        }
        break;

    case 0xdb:
        if (engine.settings_.sends != Sends::kReverbDry && a8 != 127)
        {
            Approximate("main send (omitted)");
        }
        break;

    default:
        break;
    }
}

void MidiRecorder::OnTimebase(Performer& engine)
{
    if (engine.tick_ == 0 && engine.timebase_ > 0)
    {
        out_.timebase = engine.timebase_;
    }

    OnTempo(engine);
}

void MidiRecorder::OnRunaway(Performer& /*engine*/, int t)
{
    rec_.Warn("track " + std::to_string(t) + " stopped after running without waits");
}

void MidiRecorder::OnTickEnd(Performer& engine, int t)
{
    RecordMix(engine, t);
    UpdateOutputs(engine, t);
    CheckLfo(engine, t);
    if (tracks_[t].played && engine.tracks_[t].open && engine.TrackBusy(t))
    {
        rec_.RaiseLevelPeak(TrackLevel(engine, t) * tracks_[t].legato_gain);
    }
}

void MidiRecorder::RecordMix(const Performer& engine, int t)
{
    const Track& tr = engine.tracks_[t];
    const std::array<int16_t, kMixSettingCount> values = {static_cast<int16_t>(tr.vol.Get()),
                                                          static_cast<int16_t>(tr.pan.Get()), tr.fx_send_a,
                                                          tr.fx_send_b, tr.fx_send_c};
    std::array<int16_t, kMixSettingCount>& last = tracks_[t].mix;
    for (int setting = 0; setting < kMixSettingCount; setting++)
    {
        if (values[setting] != last[setting])
        {
            out_.mix[t].push_back({engine.tick_, static_cast<Mix>(setting), values[setting]});
            last[setting] = values[setting];
        }
    }
}

void MidiRecorder::OnDeferredRelease(Performer& engine, int t, Note& n, double after)
{
    RecordEarlyRelease(engine, n);
    if (n.midi.on)
    {
        n.midi.on = false;
        rec_.NoteOff(out_, static_cast<uint8_t>(t), engine.tick_, n.midi.part, n.midi.key, n.midi.id, after);
    }
}

void MidiRecorder::OnBetweenTicks(Performer& engine, const TickTime& next)
{
    SweepBetweenTicks(engine, next);
}

} // namespace ntdlib::sequence_export
