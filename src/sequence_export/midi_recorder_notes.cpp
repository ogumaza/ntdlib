// SPDX-License-Identifier: MIT

// The MIDI recorder: the note-ons, the legatos and the commands (see midi_recorder.cpp).

#include "midi_recorder_internal.h"

namespace ntdlib::sequence_export
{

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
