// SPDX-License-Identifier: MIT

// The MIDI recorder: the outputs of the tracks, such as their controllers, sends, filters, pitches, sweeps and
// levels (see midi_recorder.cpp).

#include "midi_recorder_internal.h"

namespace ntdlib::sequence_export
{

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

} // namespace ntdlib::sequence_export
