// SPDX-License-Identifier: MIT

// The MIDI recording of the performer (see performer.h): the MIDI track and the parts of each track, the
// controllers, the presets of the notes and the times of the ticks.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// The weights that rule `rule` gives a JAudio1 note's bank value, effects' value and track value, with the pan powers
// `powers` of the note's track (JASystem::TChannel::calcPan and calcEffect). setPanPower (0x8028bdbc) divides each
// power by their sum, or by 1 when they sum to 0. A rule past the table adds all three.
std::array<float, 3> CalcWeights(uint8_t rule, const std::array<uint16_t, 3>& powers)
{
    if (rule >= kCalcRules.size())
    {
        return {1.0f, 1.0f, 1.0f};
    }

    float sum = 0.0f;
    for (const uint16_t power : powers)
    {
        sum += static_cast<float>(power);
    }
    if (sum == 0.0f)
    {
        sum = 1.0f;
    }

    std::array<float, 3> weights{};
    for (std::size_t i = 0; i < weights.size(); i++)
    {
        switch (kCalcRules[rule][i])
        {
        case 1:
            weights[i] = 1.0f;
            break;
        case 2:
            weights[i] = static_cast<float>(powers[i]) / sum;
            break;
        default:
            break;
        }
    }

    return weights;
}

} // namespace

int Performer::SlotOf(int t)
{
    // The first 16 tracks that record something get MIDI tracks: the first track's children by number, others in the
    // order they first record. Tracks beyond that share the last one.
    Track& tr = TrackAt(t);
    if (tr.slot >= 0)
    {
        return tr.slot;
    }

    int wanted = -1;
    if (tr.parent == 0)
    {
        for (int i = 0; i < kChildren; i++)
        {
            if (tracks_[0].children[static_cast<std::size_t>(i)] == t)
            {
                wanted = i;
            }
        }
    }
    if (wanted < 0 || slot_tracks_[static_cast<std::size_t>(wanted)] >= 0)
    {
        wanted = -1;
        for (int s = 0; s < 16 && wanted < 0; s++)
        {
            // Leave the slots that the first track's children would take for them.
            const bool reserved = tracks_[0].children[static_cast<std::size_t>(s)] >= 0 &&
                                  tracks_[0].children[static_cast<std::size_t>(s)] != t;
            if (slot_tracks_[static_cast<std::size_t>(s)] < 0 && !reserved)
            {
                wanted = s;
            }
        }
        for (int s = 0; s < 16 && wanted < 0; s++)
        {
            if (slot_tracks_[static_cast<std::size_t>(s)] < 0)
            {
                wanted = s;
            }
        }
    }
    if (wanted < 0)
    {
        Approximate("tracks beyond 16 share the last MIDI track");
        wanted = 15;
    }
    else
    {
        slot_tracks_[static_cast<std::size_t>(wanted)] = t;
        out_.track_names[static_cast<std::size_t>(wanted)] = tr.name;
    }
    TrackAt(t).slot = wanted;
    TrackAt(t).output.midi_track = static_cast<uint8_t>(wanted);

    return wanted;
}

seqlib::midi::TrackOutput& Performer::Output(int t)
{
    // The track's output, with the MIDI track it records into (SlotOf).
    SlotOf(t);

    return TrackAt(t).output;
}

void Performer::EmitTempo()
{
    // The MIDI tempo is the tempo times the timebase over the file's timebase.
    if (!ticked_)
    {
        out_.timebase = std::max<uint16_t>(timebase_, 1);
    }
    recorder_.SetTempo(out_, tick_, static_cast<double>(tempo_) * timebase_ / out_.timebase * tempo_rate_);

    // In the later JAudio1, setTempo and setTimebase run updateTempo at once (0x800a259c and 0x800a25d8 in Pikmin 2's
    // main.dol). The first track works out its tick rate, and each open child copies its parent's (0x800a06d0).
    if (sound_.later_jaudio1)
    {
        for (Track& tr : tracks_)
        {
            tr.tick_rate = tr.parent < 0 ? TicksPerSubFrame() : TrackAt(tr.parent).tick_rate;
        }
    }
}

bool Performer::Audible(const Channel& ch) const
{
    if (ch.ended)
    {
        return false;
    }
    if (!ch.started || !ch.released)
    {
        return true;
    }

    return !Inaudible(ch.level);
}

bool Performer::TailsOnPart(int slot, uint8_t part) const
{
    // A note on the part that its track has detached, and that can still be heard. Its MIDI note has started. The
    // release's note-off has usually ended it before the track detaches the channel.
    for (const int c : voices_)
    {
        if (c >= 0)
        {
            const Channel& ch = channels_[static_cast<std::size_t>(c)];
            if (ch.midi_id != 0 && ch.track < 0 && ch.slot == slot && ch.part == part && Audible(ch))
            {
                return true;
            }
        }
    }

    return false;
}

void Performer::SwitchPart(int t)
{
    // Tails keep the settings they had when their track detached them. A change the track makes while they still sound
    // on its part would reach them. The track therefore moves to a part where none of its tails sound, and sends that
    // part every setting first.
    const int slot = SlotOf(t);
    if (!TailsOnPart(slot, TrackAt(t).output.part))
    {
        return;
    }

    // A note that the track still holds keeps the part. It follows the track's settings, as in the game. The tails
    // there follow them too.
    if (HoldsOnPart(t))
    {
        Approximate("released notes take later settings of their track while it holds a note on their part");
        return;
    }

    if (recorder_.Switch(out_, Output(t), tick_, [&](uint8_t part) { return TailsOnPart(slot, part); }))
    {
        Changed(t);
    }
}

void Performer::SendSetting(int t, EventKind kind, int value)
{
    // A setting that changes the part's last value moves the track off a part where its tails sound (SwitchPart).
    const seqlib::midi::TrackOutput& output = TrackAt(t).output;
    const auto it = output.sent.find({output.part, kind});
    if (it != output.sent.end() && it->second != value)
    {
        SwitchPart(t);
    }
    if (recorder_.SendSetting(out_, Output(t), tick_, kind, static_cast<uint8_t>(value)))
    {
        Changed(t);
    }
}

void Performer::SendPitch(int t, float semitones)
{
    // Any pitch that goes out moves the track off a part where its tails sound (SwitchPart).
    const seqlib::midi::TrackOutput& output = TrackAt(t).output;
    const auto it = output.pitch.find(output.part);
    if ((it == output.pitch.end() && semitones == 0.0f) || (it != output.pitch.end() && it->second == semitones))
    {
        return;
    }

    SwitchPart(t);
    if (recorder_.SendPitch(out_, Output(t), tick_, semitones))
    {
        Changed(t);
    }
}

float Performer::GatePitch(int t) const
{
    // The bend of the track's gate note on top of the track's: its key since its MIDI note started, and its sweep.
    if (!GateBends(t))
    {
        return 0.0f;
    }

    const Channel& ch = channels_[static_cast<std::size_t>(TrackAt(t).slots[0])];
    return ch.key_offset - ch.midi_key_offset + ch.key_sweep;
}

bool Performer::GateBends(int t) const
{
    const Track& tr = TrackAt(t);
    if (tr.slots[0] < 0)
    {
        return false;
    }

    const Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
    return ch.midi_on && ch.bends && ch.part == tr.output.part;
}

bool Performer::HoldsOnPart(int t) const
{
    // A note of the track that it hasn't released, on the track's part.
    const Track& tr = TrackAt(t);
    for (const int c : tr.slots)
    {
        if (c >= 0 && channels_[static_cast<std::size_t>(c)].midi_on &&
            channels_[static_cast<std::size_t>(c)].part == tr.output.part)
        {
            return true;
        }
    }

    return false;
}

void Performer::SweepBetweenTicks()
{
    // A sweep moves on in every sub-frame, between ticks too. Send its pitch where the sub-frame's events go: at the
    // start of the sub-frame before (RecordTickTime), as a fraction of the way from the last tick to the next.
    if (!ticked_ || tick_ >= tick_seconds_.size() || TicksPerSubFrame() <= 0.0f)
    {
        return;
    }

    const double per_tick = kSubFrameSeconds / static_cast<double>(TicksPerSubFrame());
    const double at = static_cast<double>(subframe_ > 0 ? subframe_ - 1 : 0) * kSubFrameSeconds;
    const double after = std::clamp((at - tick_seconds_[tick_]) / per_tick, 1e-6, 1.0 - 1e-6);
    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        const int i = static_cast<int>(t);
        if (!tracks_[t].open || !GateBends(i))
        {
            continue;
        }

        // A pitch of exactly 0 goes out on a part that has had no pitch yet. The part is at 0 already. The recorder's
        // SendPitch leaves such a pitch out.
        const float semitones = 12.0f * tracks_[t].params.bend + GatePitch(i);
        seqlib::midi::TrackOutput& output = Output(i);
        if (semitones == 0.0f && !output.pitch.contains(output.part))
        {
            output.pitch[output.part] = semitones;
            TrackEvent e;
            e.kind = EventKind::kPitch;
            e.semitones = semitones;
            e.after = after;
            recorder_.Emit(out_, output, tick_, e);
            continue;
        }

        recorder_.SendPitch(out_, output, tick_, semitones, -1, after);
    }
}

float Performer::SoundVolume() const
{
    return sound_.volume * fade_;
}

void Performer::MoveFade()
{
    // JAInter::SequenceMgr::checkReadSeq (0x8029706c in The Wind Waker's main.dol) sets the sound's inter volume 7 to 0
    // once the sequence's data is in, then moves it to 1 over the fade's frames. Each of the game's frames moves it by
    // the same step. The game's frames come 30 a second. The level reaches the tracks as the sound's volume does.
    constexpr double kGameFramesPerSecond = 30.0;
    if (fade_ >= 1.0f)
    {
        return;
    }

    const double frames = std::floor(static_cast<double>(subframe_) * kSubFrameSeconds * kGameFramesPerSecond);
    const auto level = static_cast<float>(std::min(frames / static_cast<double>(sound_.fade_in), 1.0));
    if (level != fade_)
    {
        fade_ = level;
        TrackAt(0).dirty = true;
    }
}

uint8_t Performer::ZoneFilter(int t) const
{
    const uint8_t entry = TrackAt(t).filter_entry;
    if (!sound_.zone_filters || swept_filters_in_.contains(t) || !TrackFilterHeard(entry, sound_.filter_rate))
    {
        return kOpenTrackFilter;
    }

    return entry;
}

double Performer::TrackLevel(int t) const
{
    // CC11 holds the square root of the parents' volumes and the sound's volume, of the track's volume through the
    // sound's handle, its external volume and the game's level of the mix, and of kAutoMixerLevel for notes on the
    // DSP's automatic mixer. CC11 acts squared.
    const Track& tr = TrackAt(t);
    float above = (tr.parent < 0 ? SoundVolume() : TrackAt(tr.parent).params.volume) * tr.handle_volume *
                  tr.ext_volume * sound_.mix_level;
    if (TrackAt(ChannelsTrack(t)).auto_mixer)
    {
        above *= kAutoMixerLevel;
    }

    return 127.0 * std::sqrt(std::max(static_cast<double>(above), 0.0));
}

std::array<float, 3> Performer::NoteWeights(int t, std::size_t value) const
{
    // TTrack::noteOn: a note takes its rule from the channels' track and its pan powers from its track
    // (TChannel::setPanPower). `value` is 0 for the pan and 1 for the effect send.
    const Track& tr = TrackAt(t);
    return CalcWeights(TrackAt(ChannelsTrack(t)).channel_calc[value], {tr.regs[8], tr.regs[9], tr.regs[10]});
}

int Performer::ChannelsTrack(int t) const
{
    // TTrack::noteOn: a track opened in mode 4 starts its notes on its parent's channels, and takes the parent's mix
    // lines with them. Any other track starts them on its channels.
    const Track& tr = TrackAt(t);
    return tr.open_mode == 4 && tr.parent >= 0 ? tr.parent : t;
}

void Performer::RecordOutputs(int t)
{
    // The MIDI controllers for a track's settings: CC7 its volume, CC11 the rest of the volume, CC10 the pan, the bend,
    // CC1 the vibrato's depth and CC91 the effect send.
    const Track& tr = TrackAt(t);
    if (!tr.open || tr.slot < 0)
    {
        return;
    }

    // A tied note's later velocities scale CC11. The game squares the velocity, and CC11 acts squared.
    double level = TrackLevel(t);
    if (GateBends(t))
    {
        const Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
        level *= static_cast<double>(ch.cc11_velocity) / static_cast<double>(ch.midi_velocity);
    }
    recorder_.RaiseLevelPeak(level);
    // CC7 acts squared. The game squares the track's volume in volume mode 0 too (SquaresVolume). Otherwise it plays
    // the volume as it is, and CC7 holds the volume's square root.
    const double track_volume = std::abs(static_cast<double>(tr.move[0].value));
    const double cc7 = SquaresVolume(tr) ? track_volume : std::sqrt(track_volume);
    const int volume = std::clamp(static_cast<int>(std::lround(127.0 * cc7)), 0, 127);
    const int expression = recorder_.Expression(level);
    // A JAudio1 note weights the track's pan and effect send by its rules (NoteWeights).
    double track_pan = static_cast<double>(tr.params.pan);
    double send = static_cast<double>(tr.params.fx_mix);
    if (sound_.jaudio1)
    {
        track_pan = 0.5 + static_cast<double>(NoteWeights(t, 0)[2]) * (track_pan - 0.5);
        send *= static_cast<double>(NoteWeights(t, 1)[2]);
    }
    const int pan = std::clamp(static_cast<int>(std::lround(127.0 * std::clamp(track_pan, 0.0, 1.0))), 0, 127);
    const double vibrato_cents = 1200.0 * std::abs(tr.vib_depth);
    if (vibrato_cents > kVibratoRangeCents)
    {
        Approximate("vibrato deeper than a semitone kept to a semitone");
    }
    // CC1 holds 128 times the depth over kVibratoRangeCents. The SoundFont's and SFZ's modulators read it that way.
    const int modulation =
        std::clamp(static_cast<int>(std::lround(128.0 * vibrato_cents / kVibratoRangeCents)), 0, 127);
    const int reverb = std::clamp(static_cast<int>(std::lround(127.0 * std::clamp(send, 0.0, 1.0))), 0, 127);
    if (tr.trem_depth != 0.0f)
    {
        Approximate("tremolo omitted");
    }
    if (tr.iir)
    {
        Approximate(ZoneFilter(t) != kOpenTrackFilter ? "track filters played by zones" : "track filters omitted");
    }

    SendSetting(t, EventKind::kVolume, volume);
    SendSetting(t, EventKind::kExpression, expression);
    SendSetting(t, EventKind::kPan, pan);
    SendSetting(t, EventKind::kModulation, modulation);
    SendSetting(t, EventKind::kReverb, reverb);
    SendPitch(t, 12.0f * tr.params.bend + GatePitch(t));
}

void Performer::MidiNoteOn(std::size_t c)
{
    Channel& ch = channels_[c];
    const int t = ch.track;
    if (ch.velocity == 0)
    {
        return; // the game plays it silently
    }

    const uint32_t preset = NotePreset(ch);

    // A note whose key starts it past the DSP's pitch limit plays at the limit. The preset then plays that key and the
    // keys above it at the limit (PresetZones).
    if (PastPitchLimit(DspPitch(ch.init_pitch, 1.0f, ch.key_offset / 12.0f, 1.0f)))
    {
        out_.pitch_limited_presets.insert(preset);
    }

    // A note that bends goes on a part where no tail of its track sounds. The tails keep their pitch there.
    if (ch.bends)
    {
        SwitchPart(t);
    }

    // The settings go before the note. The note then starts with them.
    RecordOutputs(t);
    recorder_.SendPreset(out_, Output(t), tick_, preset);

    ch.midi_on = true;
    ch.midi_key_offset = ch.key_offset;
    ch.midi_velocity = ch.velocity;
    ch.cc11_velocity = ch.velocity;
    ch.slot = SlotOf(t);
    ch.midi_id = recorder_.NextNote();

    PlanTieVelocity(ch);
    ch.part = TrackAt(t).output.part;
    recorder_.NoteOn(out_, Output(t), tick_, ch.key, ch.midi_velocity);
    Changed(t);
}

uint32_t Performer::NotePreset(const Channel& ch)
{
    // The note's preset: the bank and program, with the track's oscillators and settings.
    const int t = ch.track;
    const Track& tr = TrackAt(t);
    PresetKey key;
    key.bank = ch.sound.bank;
    key.program = ch.sound.program;
    for (std::size_t i = 0; i < 2; i++)
    {
        // A JAudio1 note plays the track's oscillators that its routes give (RouteOscillators1). A JAudio2 note plays
        // the track's tables.
        const Oscillator* osc = sound_.jaudio1 ? ch.track_osc[i] : (tr.osc[i] ? &*tr.osc[i] : nullptr);
        if (osc == nullptr)
        {
            continue;
        }
        if (sound_.jaudio1 && osc->target > 1)
        {
            Approximate("oscillators that tracks set for a note's pan, effect send or surround omitted");
            continue;
        }

        (i == 0 ? key.volume_oscillator : key.pitch_oscillator) = OscillatorIndex(*osc);
    }
    if (sound_.jaudio1)
    {
        const std::array<float, 3> weights = NoteWeights(t, 0);
        key.bank_pan_weight = weights[0];
        key.effect_pan_weight = weights[1];
    }
    key.direct_release = tr.direct_release;
    key.vibrato_rate = tr.vib_pitch;
    key.vibrato_delay = tr.vib_delay;
    key.track_filter = ZoneFilter(t);
    if (key.track_filter != kOpenTrackFilter &&
        filter_seen_.try_emplace(t, key.track_filter).first->second != key.track_filter)
    {
        swept_filters_.insert(t);
    }

    return seqlib::midi::PresetIndex(out_.presets, key);
}

int Performer::OscillatorIndex(const Oscillator& osc)
{
    // The track oscillators of the performance: an oscillator that matches one in the list takes its index.
    const auto same_points = [](const std::vector<OscPoint>& a, const std::vector<OscPoint>& b)
    {
        return std::ranges::equal(a, b, [](const OscPoint& x, const OscPoint& y)
                                  { return x.mode == y.mode && x.time == y.time && x.value == y.value; });
    };
    const auto same = [&](const Oscillator& o)
    {
        return o.target == osc.target && o.rate == osc.rate && o.scale == osc.scale && o.vertex == osc.vertex &&
               o.release_is_attack == osc.release_is_attack && same_points(o.attack, osc.attack) &&
               same_points(o.release, osc.release);
    };
    if (const auto it = std::ranges::find_if(out_.oscillators, same); it != out_.oscillators.end())
    {
        return static_cast<int>(it - out_.oscillators.begin());
    }
    out_.oscillators.push_back(osc);
    return static_cast<int>(out_.oscillators.size() - 1);
}

void Performer::PlanTieVelocity(Channel& ch)
{
    // A note that ties take above its velocity starts at the highest they reach. CC11 lowers the note until then. The
    // instrument has to play the same wave at that velocity. An oscillator plays the same waveform at every velocity.
    const auto planned = tie_velocities_in_.find(ch.midi_id);
    if (planned != tie_velocities_in_.end())
    {
        std::optional<NoteSound> higher;
        if (ch.sound.program >= kFirstOscillatorProgram)
        {
            higher = FindOscillatorSound(ch.sound.bank, ch.sound.program);
        }
        else if (const Bank* bank = banks_.Archive().FindBank(ch.sound.bank))
        {
            higher = FindSound(*bank, ch.sound.program, ch.key, planned->second);
        }
        if (higher && higher->wave_system == ch.sound.wave_system && higher->wave_id == ch.sound.wave_id)
        {
            ch.midi_velocity = planned->second;
        }
    }
}

void Performer::MidiNoteOff(std::size_t c)
{
    Channel& ch = channels_[c];
    if (!ch.midi_on)
    {
        return;
    }

    ch.midi_on = false;
    recorder_.NoteOff(out_, static_cast<uint8_t>(ch.slot), tick_, ch.part, ch.key, ch.midi_id);
}

void Performer::Approximate(const std::string& what)
{
    out_.approximations[what]++;
}

void Performer::RecordTickTime()
{
    // A tick's nominal time, and the start of the sub-frame before the one it runs in as a fractional tick: where the
    // MIDI writer puts its events (Performance::frame_ticks). The game runs a tick in the first sub-frame after its
    // time, and the DSP plays its changes from the next one.
    const double per_tick = kSubFrameSeconds / static_cast<double>(TicksPerSubFrame());
    tick_seconds_.push_back(tick_seconds_.empty() ? 0.0 : tick_seconds_.back() + per_tick);
    tick_subframes_.push_back(subframe_);
    const double start = static_cast<double>(subframe_ > 0 ? subframe_ - 1 : 0) * kSubFrameSeconds;
    recorder_.RecordFrameStart(out_, tick_seconds_, tick_, start);
}

} // namespace ntdlib::sequence_export::jaudio
