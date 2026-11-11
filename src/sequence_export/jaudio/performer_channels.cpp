// SPDX-License-Identifier: MIT

// The performer's channels (see performer.h): the voices that play notes, their oscillators and LFOs, and the
// game's echo.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sequence_export/jaudio/performer.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// A vibrato's counter moves on by this much each sub-frame (JASChannel::updateDSPChannel, with 32028.5 at 0x8053b090).
constexpr float kLfoStep = 32028.5f / kDacRate;

// The game's echo counts as silent once its output falls below this level, in 16-bit samples. The game's output moves
// in steps of 4. Below 12, it has fallen to 8 or below (-72 dBFS). A sequence that ends sounds on until then.
constexpr float kEchoAudible = 12.0f;

// The largest magnitude among the samples of `pcm` that a voice plays from position `from` up to `to`. A looping wave
// goes back to its loop's start at the loop's end.
float WavePeak(const Pcm& pcm, double from, double to)
{
    if (pcm.channels.empty())
    {
        return 0.0f;
    }

    const std::vector<int16_t>& samples = pcm.channels[0];
    const uint64_t loop_length = pcm.loop ? pcm.loop_end - pcm.loop_start : 0;
    auto i = static_cast<uint64_t>(from);
    uint64_t count = static_cast<uint64_t>(std::ceil(to)) - i;
    int peak = 0;
    while (count > 0)
    {
        if (loop_length != 0 && i >= pcm.loop_end)
        {
            i = pcm.loop_start + (i - pcm.loop_start) % loop_length;
        }
        const uint64_t stop = std::min<uint64_t>(i + count, loop_length != 0 ? pcm.loop_end : samples.size());
        if (i >= stop)
        {
            break;
        }

        count -= stop - i;
        for (; i < stop; i++)
        {
            peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        }
    }

    return static_cast<float>(peak);
}

// Start `semitones` of sweep on `ch` over `subframes` (JASChannel::setKeySweepTarget, 0x8002ae78). A sweep of no
// sub-frames moves at once.
void SetSweep(Channel& ch, float semitones, uint32_t subframes)
{
    ch.sweep_count = subframes;
    if (subframes == 0)
    {
        ch.key_sweep = semitones;
    }
    else
    {
        ch.key_sweep = 0.0f;
        ch.sweep_target = semitones;
    }
}

} // namespace

std::optional<std::size_t> Performer::AllocateVoice(uint8_t priority)
{
    // JASDSPChannel::getLowestChannel: a free voice, or the oldest voice of the lowest priority at or below this one.
    int best = -1;
    int best_priority = 0xff;
    uint32_t best_age = 0;
    for (std::size_t v = 0; v < voices_.size(); v++)
    {
        const int c = voices_[v];
        if (c < 0)
        {
            return v;
        }

        const Channel& ch = channels_[static_cast<std::size_t>(c)];
        const int p = ch.released ? ch.release_priority : ch.priority;
        if (p <= priority && p <= best_priority && (p != best_priority || ch.age > best_age))
        {
            best = static_cast<int>(v);
            best_priority = p;
            best_age = ch.age;
        }
    }

    // Without a voice limit, a note that finds every voice busy gets a new one.
    if (options_.voice_limit == 0)
    {
        voices_.push_back(-1);
        return voices_.size() - 1;
    }
    if (best < 0)
    {
        return std::nullopt;
    }

    // The voice's channel stops at once.
    const auto stolen = static_cast<std::size_t>(voices_[static_cast<std::size_t>(best)]);
    Approximate("notes cut when the game's " + std::to_string(options_.voice_limit) + " voices ran out");
    EndChannel(stolen);

    return static_cast<std::size_t>(best);
}

void Performer::RouteOscillators1(Track& tr, Channel& ch)
{
    // JASystem::TTrack's routes (0x802814ac in The Wind Waker's main.dol). Each of the track's oscillators that has a
    // route plays in place of one of the note's oscillators: bits 0 and 1 pick which. With bit 3, the track's
    // oscillator first takes a copy of the note's (0x8028bb98). With bit 2 alone, it takes the copy but keeps its
    // release table. Either bit leaves the note as it is if the note has no such oscillator (0x8028bb14). The note then
    // plays the track's (0x8028bc78). The channel reads the track's oscillator as it plays. Pac-Man Vs. and Pikmin 2
    // route the track's oscillator even when the note has none to copy (0x8002d3bc in Pac-Man Vs.'s main.dol). None of
    // the three games' sequences routes with bit 2 or 3. A note keeps its oscillator when the track's has changed in a
    // way that the performer leaves out (Track::osc1_known), unless bit 3 replaces the track's with a copy.
    for (std::size_t k = 0; k < tr.osc_routes.size(); k++)
    {
        const uint8_t route = tr.osc_routes[k];
        if (route == kNoRoute)
        {
            continue;
        }

        const std::size_t slot = route & 3u;
        if (slot >= ch.osc_data.size())
        {
            Approximate("oscillators that tracks route to a note's third or fourth oscillator omitted");
            continue;
        }
        if (!tr.osc1_known[k] && (route & 8) == 0)
        {
            continue;
        }
        if ((route & 0xc) != 0)
        {
            if (ch.osc_data[slot] == nullptr)
            {
                continue;
            }

            Oscillator copy = *ch.osc_data[slot];
            if ((route & 8) == 0)
            {
                copy.release = tr.osc1[k].release;
                copy.release_is_attack = tr.osc1[k].release_is_attack;
            }
            tr.osc1[k] = std::move(copy);
            tr.osc1_known[k] = true;
        }
        ch.osc_data[slot] = &tr.osc1[k];
        ch.track_osc[slot] = &tr.osc1[k];
    }
}

std::optional<std::size_t> Performer::StartChannel(int t, int key, int velocity, uint32_t timer, float sweep,
                                                   uint32_t sweep_subframes, bool bends)
{
    // JASTrack::channelStart and JASBank::noteOn: find what the program plays, and start a channel on a voice.
    Track& tr = TrackAt(t);
    const std::optional<NoteSound> found = FindProgramSound(tr, key, velocity);
    if (!found)
    {
        return std::nullopt;
    }

    const Pcm* pcm = banks_.GetWave(found->wave_system, found->wave_id);
    const WaveInfo* wave = FindWave(banks_.Archive(), found->wave_system, found->wave_id);
    if (pcm == nullptr || wave == nullptr)
    {
        return std::nullopt;
    }

    if (found->random)
    {
        Approximate("random variation of instruments omitted");
    }
    if (found->velocity_sense)
    {
        Approximate("instrument effects that follow the velocity kept at each velocity region's top");
    }

    const std::optional<std::size_t> voice = AllocateVoice(tr.note_on_prio);
    if (!voice)
    {
        Approximate("notes lost when the game's " + std::to_string(options_.voice_limit) + " voices ran out");
        return std::nullopt;
    }

    Channel ch;
    ch.track = t;
    ch.sound = *found;
    ch.sound.wave = wave;
    ch.key = static_cast<uint8_t>(std::clamp(key, 0, 127));
    ch.velocity = static_cast<uint8_t>(std::clamp(velocity, 0, 127));
    ch.init_pitch = found->pitch * (ch.sound.wave->sample_rate / kDacRate);
    ch.base_key = found->drum ? key : ch.sound.wave->base_key;
    ch.key_offset = static_cast<float>(key - ch.base_key);
    ch.update_timer = timer;
    ch.priority = tr.note_on_prio;
    ch.release_priority = tr.release_prio;
    ch.direct_release = found->direct_release;
    if (tr.direct_release != 0)
    {
        ch.direct_release = tr.direct_release;
    }
    ch.pcm = pcm;
    ch.wave_loops = pcm->loop;
    ch.wave_length = pcm->Length();
    ch.params = tr.params;
    ch.auto_mixer = TrackAt(ChannelsTrack(t)).auto_mixer;
    ch.vibrato.SetDelay(tr.vib_delay);
    ch.tremolo.SetDelay(tr.trem_delay);
    ch.vibrato.Set(tr.vib_depth, tr.vib_pitch);
    ch.tremolo.Set(tr.trem_depth, tr.trem_pitch);
    if (tr.skip_samples != 0)
    {
        Approximate("sample skips omitted");
    }

    StartOscillators(tr, *found, ch);

    if (sweep != 0.0f || sweep_subframes != 0)
    {
        SetSweep(ch, sweep, sweep_subframes);
    }
    ch.bends = bends;
    channels_.push_back(std::move(ch));
    const std::size_t c = channels_.size() - 1;
    voices_[*voice] = static_cast<int>(c);
    MidiNoteOn(c);

    return c;
}

std::optional<NoteSound> Performer::FindProgramSound(const Track& tr, int key, int velocity)
{
    std::optional<NoteSound> found;
    if (tr.program >= kFirstOscillatorProgram)
    {
        found = FindOscillatorSound(tr.bank, tr.program);
        if (!found)
        {
            Approximate("notes on the DSP's noise, ramp and other oscillators omitted");
            return std::nullopt;
        }
    }
    else
    {
        const Bank* bank = banks_.Archive().FindBank(tr.bank);
        if (bank == nullptr)
        {
            Approximate("notes on a missing bank omitted");
            return std::nullopt;
        }

        found = FindSound(*bank, tr.program, key, velocity);
        if (!found)
        {
            return std::nullopt;
        }
    }

    return found;
}

void Performer::StartOscillators(Track& tr, const NoteSound& found, Channel& ch)
{
    // The instrument's oscillators, then the track's tables in their place (JASTrack::overwriteOsc).
    for (std::size_t i = 0; i < 2 && i < found.oscillators.size(); i++)
    {
        ch.osc_data[i] = found.oscillators[i];
    }
    if (found.drum)
    {
        static const Oscillator kDrumOscillator;
        ch.osc_data[0] = &kDrumOscillator;
        ch.osc_data[1] = nullptr;
    }
    if (sound_.jaudio1)
    {
        if (sound_.track_oscillator_routes)
        {
            RouteOscillators1(tr, ch);
        }
    }
    else
    {
        for (std::size_t i = 0; i < 2; i++)
        {
            if (tr.osc[i])
            {
                ch.osc_data[i] = &*tr.osc[i];
                ch.track_osc[i] = ch.osc_data[i];
            }
        }
    }
    for (std::size_t i = 0; i < 2; i++)
    {
        if (ch.osc_data[i] != nullptr)
        {
            ch.osc[i].Start(*ch.osc_data[i]);
        }
    }
}

void Performer::NoteOn(int t, uint32_t note_id, int key, int velocity)
{
    // JASTrack::noteOn: a muted track starts no note. Otherwise the note id's channel is released for the new one.
    if (note_id == 0 || note_id >= kNoteIds)
    {
        return;
    }

    key += TransposeTotal(t);
    Changed(t);
    if (Muted(t))
    {
        return;
    }
    NoteOff(t, note_id, 0);
    const std::optional<std::size_t> c = StartChannel(t, key, velocity, 0);
    TrackAt(t).slots[note_id] = c ? static_cast<int>(*c) : -1;
    if (c)
    {
        channels_[*c].note_id = static_cast<int>(note_id);
    }
}

void Performer::GateOn(int t, int key, int velocity, float time, uint32_t flags)
{
    // JASTrack::gateOn (0x80037214): a note on slot 0 with a length. Flag 1 sweeps from the latest key to the key over
    // the length, flag 2 ties the next note to this one, and flag 4 waits for the note to end. A muted track releases
    // the note on slot 0 but starts no new one.
    Changed(t);
    Track& tr = TrackAt(t);
    key += TransposeTotal(t);
    if (tr.gate_rate != 100)
    {
        time *= static_cast<float>(tr.gate_rate) / 100.0f;
    }

    // JASTrack::seqTimeToDspTime (0x80037bc8) divides the length by the track's tick rate. A track opens with the
    // default rate (tempo 120, timebase 48). updateTrack sets the rate at the end of each sub-frame. So a note in the
    // tick that opens its track, or in a tick that changes the tempo, keeps the earlier rate. The later JAudio1 gives
    // the track the current rate at once (OpenChild and EmitTempo).
    const auto subframes = static_cast<uint32_t>(time / tr.tick_rate);
    const uint32_t timer = (flags & 6) ? 0 : subframes;
    const bool sweeps = (flags & 1) != 0;
    const int from = sweeps ? tr.latest_key : key;
    const auto sweep = static_cast<float>(key - from);
    if (!tr.tied)
    {
        NoteOff(t, 0, 0);
        const std::optional<std::size_t> c = Muted(t)
                                                 ? std::nullopt
                                                 : StartChannel(t, from, velocity, timer, sweeps ? sweep : 0.0f,
                                                                sweeps ? subframes : 0, sweeps || (flags & 2) != 0);
        TrackAt(t).slots[0] = c ? static_cast<int>(*c) : -1;
        if (c)
        {
            channels_[*c].note_id = 0;
        }
    }
    else if (tr.slots[0] >= 0)
    {
        // The sounding note goes on at the new key and velocity. Its MIDI note bends to the key, and CC11 follows the
        // velocity. A velocity above the MIDI note's goes in TieVelocities for the next run. The game's tie doesn't
        // start the envelope again. A note below hearing stays there. Its tie keeps CC11 at or below the note's start
        // and adds nothing to TieVelocities. A rise past CC11's limit would lower the whole sequence for a note nobody
        // hears.
        Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
        const bool heard = !BelowHearing(ch);
        const float old_offset = ch.key_offset;
        ch.key_offset = static_cast<float>(from - ch.base_key);
        if (sound_.jaudio1)
        {
            // JASystem::TChannel sets a tied note's pitch at its key (0x80288e44 in The Wind Waker's main.dol). A sweep
            // keeps its target as a pitch (0x8028bd10). A sweep under way goes on from the new key toward that target.
            // A sweep that has ended adds nothing. JAudio2's JASChannel keeps the sweep's semitones on top of the key.
            ch.sweep_target = ch.sweep_count != 0 ? old_offset + ch.sweep_target - ch.key_offset : 0.0f;
            ch.key_sweep = 0.0f;
        }
        ch.velocity = static_cast<uint8_t>(std::clamp(velocity, 0, 127));
        ch.cc11_velocity = heard ? ch.velocity : std::min(ch.velocity, ch.midi_velocity);
        if (ch.midi_on && heard && ch.velocity > ch.midi_velocity)
        {
            uint8_t& peak = tie_velocities_[ch.midi_id];
            peak = std::max(peak, ch.velocity);
        }
        ch.update_timer = timer;
        if (sweeps)
        {
            SetSweep(ch, sweep, subframes);
        }
    }

    Track& again = TrackAt(t);
    again.tied = (flags & 2) != 0;
    again.latest_key = static_cast<uint8_t>(std::clamp(key, 0, 127));
}

void Performer::NoteOff(int t, uint32_t note_id, uint16_t release)
{
    // TChannelMgr::noteOff: release the note id's channel and detach it.
    if (note_id >= kNoteIds)
    {
        return;
    }

    int& slot = TrackAt(t).slots[note_id];
    if (slot < 0)
    {
        return;
    }

    const auto c = static_cast<std::size_t>(slot);
    slot = -1;
    Changed(t);
    ReleaseChannel(c, release);
    Detach(c);
}

void Performer::ReleaseChannel(std::size_t c, uint16_t direct_release)
{
    // JASChannel::release: the oscillators release, and the voice drops to the release priority.
    Channel& ch = channels_[c];
    if (ch.ended || ch.released)
    {
        return;
    }

    if (direct_release != 0)
    {
        ch.direct_release = direct_release;
    }
    for (std::size_t i = 0; i < 2; i++)
    {
        if (ch.osc[i].Valid())
        {
            ch.osc[i].Release(i == 0 ? ch.direct_release : 0, sound_.default_release);
        }
    }
    ch.released = true;
    MidiNoteOff(c);
}

void Performer::Detach(std::size_t c)
{
    // JASChannel::free: the channel no longer takes its track's settings.
    channels_[c].track = -1;
}

void Performer::EndChannel(std::size_t c)
{
    Channel& ch = channels_[c];
    if (ch.ended)
    {
        return;
    }

    ch.ended = true;
    MidiNoteOff(c);
    for (int& v : voices_)
    {
        if (v == static_cast<int>(c))
        {
            v = -1;
        }
    }
    if (ch.track >= 0)
    {
        for (int& slot : TrackAt(ch.track).slots)
        {
            if (slot == static_cast<int>(c))
            {
                slot = -1;
            }
        }
    }
    sound_end_ = std::max(sound_end_, static_cast<double>(subframe_) * kSubFrameSeconds);
}

float Performer::ChannelVolume(const Channel& ch, float osc_volume) const
{
    // JASChannel::updateEffectorParam: velocity squared, the instrument's volume, the oscillators, the track's volume
    // and the tremolo. The game's level of the mix applies to every channel.
    const float v = static_cast<float>(ch.velocity) / 127.0f;
    const float volume = ch.sound.volume * osc_volume * ch.params.volume * (ch.tremolo.Value() + 1.0f) * (v * v) *
                         (ch.auto_mixer ? kAutoMixerLevel : 1.0f) * sound_.mix_level;
    return std::max(volume, 0.0f);
}

float Performer::ChannelRatio(const Channel& ch, float osc_pitch) const
{
    // The pitch the channel gives the DSP, as a ratio of samples per output sample. The game truncates the pitch to a
    // whole number of 4096ths (0x8002b688). The DSP's limit applies after that.
    const float octaves = ch.params.bend + (ch.key_offset + ch.key_sweep) / 12.0f + ch.vibrato.Value();
    const float pitch = DspPitch(ch.init_pitch, osc_pitch, octaves, ch.params.pitch);
    return std::min(std::trunc(std::max(pitch, 0.0f)), kPitchLimit) / 4096.0f;
}

void Performer::UpdateChannel(Channel& ch)
{
    // JASChannel::updateDSPChannel, or initialUpdateDSPChannel in its first sub-frame.
    if (ch.track >= 0)
    {
        const Track& tr = TrackAt(ch.track);
        ch.params = tr.params;
        ch.vibrato.Set(tr.vib_depth, tr.vib_pitch);
        ch.tremolo.Set(tr.trem_depth, tr.trem_pitch);
    }

    if (!ch.started)
    {
        ch.started = true;
        ch.vibrato.Reset();
        ch.tremolo.Reset();
    }
    else if (!AdvanceChannel(ch))
    {
        return;
    }

    float osc_volume = 1.0f, osc_pitch = 1.0f;
    bool silenced = false;
    for (std::size_t i = 0; i < 2; i++)
    {
        if (!ch.osc[i].Valid())
        {
            continue;
        }

        const float value = ch.osc[i].Value();
        if (ch.osc_data[i]->target == 0)
        {
            osc_volume *= value;
            silenced = silenced || ch.osc[i].HoldsSilent();
        }
        else if (ch.osc_data[i]->target == 1)
        {
            osc_pitch *= value;
        }
    }

    // A volume oscillator that holds at 0 silences the channel for good. The game keeps the channel, but the MIDI note
    // ends here. The zone's release then ends its sound. The zone's decay alone would let it ring on.
    if (silenced)
    {
        MidiNoteOff(static_cast<std::size_t>(&ch - channels_.data()));
    }

    ch.envelope = osc_volume;
    ch.envelope_heard = ch.envelope_heard || !Inaudible(osc_volume);
    ch.level = ChannelVolume(ch, osc_volume);
    if (osc_pitch != 1.0f && !ch.pitch_moved)
    {
        ch.pitch_moved = true;
        Approximate("pitch envelopes omitted");
    }

    // The DSP plays the sub-frame at this pitch. A wave that doesn't loop ends when it runs out.
    const double from = ch.position;
    ch.position += kSubFrameSamples * static_cast<double>(ChannelRatio(ch, osc_pitch));
    SendToEcho(ch, from);
    if (ch.sweep_count != 0)
    {
        ch.key_sweep += (ch.sweep_target - ch.key_sweep) / static_cast<float>(ch.sweep_count);
        ch.sweep_count--;
    }
    if (!ch.wave_loops && ch.position >= static_cast<double>(ch.wave_length))
    {
        EndChannel(static_cast<std::size_t>(&ch - channels_.data()));
    }
}

bool Performer::AdvanceChannel(Channel& ch)
{
    ch.vibrato.Advance(kLfoStep);
    ch.tremolo.Advance(kLfoStep);
    if (ch.update_timer != 0)
    {
        ch.update_timer--;
        if (ch.update_timer == 0 && ch.track >= 0)
        {
            // A gate note's time is up: its track releases and detaches it (channelUpdateCallback's timer case).
            Track& tr = TrackAt(ch.track);
            const auto c = static_cast<std::size_t>(&ch - channels_.data());
            if (tr.slots[0] == static_cast<int>(c))
            {
                tr.slots[0] = -1;
            }
            ReleaseChannel(c, 0);
            Detach(c);
        }
    }
    for (std::size_t i = 0; i < 2; i++)
    {
        if (ch.osc[i].Valid())
        {
            ch.osc[i].Advance(kOscStep);
            if (i == 0 && ch.osc[0].Stopped())
            {
                EndChannel(static_cast<std::size_t>(&ch - channels_.data()));
                return false;
            }
        }
    }

    return true;
}

void Performer::UpdateChannels()
{
    for (std::size_t v = 0; v < voices_.size(); v++)
    {
        const int c = voices_[v];
        if (c >= 0)
        {
            Channel& ch = channels_[static_cast<std::size_t>(c)];
            ch.age++;
            UpdateChannel(ch);
            last_sounding_ = subframe_;
        }
    }
    StepEcho(subframe_);
}

void Performer::SendToEcho(const Channel& ch, double from)
{
    const float fx_mix = std::clamp(ch.params.fx_mix, 0.0f, 1.0f);
    if (!sound_.echo || fx_mix <= 0.0f || ch.level <= 0.0f || ch.pcm == nullptr)
    {
        return;
    }

    // JASChannel::updateMixer with JASTrack's default mix (0x352 and 0x412, set at 0x80036650): each side's send is the
    // volume times the side's pan gain times the effect mix. The pan adds the instrument's pan to the track's
    // (calcPan). A voice's level is the largest sample that it plays in the sub-frame. The voices' levels add up.
    constexpr float kHalfPi = 1.57079633f;
    const float pan = std::clamp(ch.sound.pan + ch.params.pan - 0.5f, 0.0f, 1.0f);
    const std::array<float, 2> gains{std::sin((1.0f - pan) * kHalfPi), std::sin(pan * kHalfPi)};
    const float peak = WavePeak(*ch.pcm, from, ch.position);
    for (std::size_t side = 0; side < 2; side++)
    {
        echo_sends_[side] += std::min(ch.level * gains[side] * fx_mix, 1.0f) * sound_.echo->voice_level * peak;
    }
}

void Performer::StepEcho(uint64_t subframe)
{
    if (!sound_.echo)
    {
        return;
    }

    // Each line plays what it took in a delay ago, through its filter. It takes in the sends and that output. The
    // output repeats the wave from a delay earlier, at a phase unrelated to the sends'. The two therefore add by power.
    // Adding their levels would make a long sound's echo up to twice as loud as the game's.
    for (std::size_t side = 0; side < 2; side++)
    {
        float& delayed = echo_lines_[side][echo_next_];
        const float filtered = sound_.echo->feedback * delayed;
        if (sound_.echo->volume * filtered >= kEchoAudible)
        {
            echo_heard_ = subframe;
        }
        delayed = std::hypot(echo_sends_[side], filtered);
    }
    echo_next_ = (echo_next_ + 1) % echo_lines_[0].size();
    echo_sends_ = {};
}

bool Performer::EchoSounds() const
{
    if (!sound_.echo)
    {
        return false;
    }

    const float scale = sound_.echo->volume * sound_.echo->feedback;
    for (const std::vector<float>& line : echo_lines_)
    {
        if (std::ranges::any_of(line, [&](float level) { return scale * level >= kEchoAudible; }))
        {
            return true;
        }
    }

    return false;
}

} // namespace ntdlib::sequence_export::jaudio
