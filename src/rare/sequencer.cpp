// SPDX-License-Identifier: MIT

#include "ntdlib/rare/sequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/song.h"

namespace ntdlib::rare
{

using ntdlib::agb::WarnOnce;

namespace
{

// Maximum number of commands a track may run per frame. A loop without a delay in it would hang the driver.
constexpr int kMaxCommandsPerFrame = 100000;

// A quotient and remainder from the driver's 16-bit division.
struct Quotient
{
    uint32_t quotient;
    uint32_t remainder;
};

// Returns `dividend` / `divisor` and its remainder, as the driver's 16-bit division works them out.
Quotient Divide(uint32_t dividend, uint32_t divisor)
{
    return {(dividend / divisor) & 0xFFFF, dividend % divisor};
}

// Frees a slot whose envelope has ended. The driver leaves a level of 0x10 in the slot.
void Stop(Slot& s)
{
    s.state = kSlotFree;
    s.level = 0x10;
}

// Takes a frame's step off a releasing note's level. Ends the note where the level would reach 0.
void FallRelease(Slot& s)
{
    if (s.level - s.level_step > 0)
    {
        s.level -= s.level_step;
    }
    else
    {
        Stop(s);
    }
}

} // namespace

uint32_t AttackSteps(uint32_t attack, uint8_t setting)
{
    return setting < 0x80 ? uint32_t(setting) : Divide((99 - std::min<uint32_t>(attack, 99)) << 5, 99).quotient;
}

uint32_t SustainLevel(uint32_t sustain, uint8_t setting)
{
    if (setting < 0x80)
    {
        return uint32_t(setting) << 8;
    }

    const Quotient whole = Divide(sustain << 7, 99);
    return (whole.quotient << 8) | Divide(whole.remainder << 8, 99).quotient;
}

Sequencer::Sequencer(const Rom& rom, const DriverInfo& info, int tune, uint16_t silenced)
    : rom_(rom), info_(info), silenced_(silenced), music_volume_(uint32_t(MusicVolume(info, tune)))
{
    valid_ = ReadTuneHeader(rom, TuneAddress(rom, info, tune), header_);
    if (!valid_)
    {
        return;
    }

    for (uint32_t address : header_.tracks)
    {
        Track t;
        t.position = address;
        tracks_.push_back(t);
    }

    slots_.resize(size_t(kChannels * info.slots_per_channel));
    for (size_t i = 0; i < slots_.size(); i++)
    {
        slots_[i].channel = int8_t(i / size_t(info.slots_per_channel));
    }
}

const std::vector<Action>& Sequencer::Step()
{
    actions_.clear();
    if (!valid_)
    {
        return actions_;
    }

    // Once the tune has ended, the driver stops running the tracks and the envelopes, but goes on mixing.
    if (!ended_)
    {
        int at_end = 0;
        for (size_t t = 0; t < tracks_.size() && !audition_; t++)
        {
            if (RunTrack(int(t)))
            {
                at_end++;
            }
        }
        ended_ = !audition_ && at_end == int(tracks_.size());

        UpdateVibrato();
        for (size_t i = 0; i < slots_.size(); i++)
        {
            if (slots_[i].state == kSlotOn || slots_[i].state == kSlotReleased)
            {
                UpdateEnvelope(int(i));
            }
        }
    }

    for (Action& a : actions_)
    {
        a.frame = frame_;
    }

    Mix();

    for (Slot& s : slots_)
    {
        if (s.state == kSlotOn || s.state == kSlotReleased)
        {
            Advance(s);
        }
    }

    frame_++;

    return actions_;
}

bool Sequencer::RunTrack(int t)
{
    Track& track = tracks_[size_t(t)];
    track.counter -= frame_time_;
    if (track.counter >= 0)
    {
        return false;
    }
    if (track.done)
    {
        return true;
    }

    for (int n = 0; n < kMaxCommandsPerFrame; n++)
    {
        Event e;
        if (!DecodeEvent(rom_, track.position, info_.format, e))
        {
            char b[96];
            std::snprintf(b, sizeof b, "track %d has an unreadable command at 0x%08X; stopping the track", t,
                          unsigned(track.position));
            Warn(b);
            track.done = true;
            return true;
        }

        if (record_stretches_)
        {
            RecordRead(t, track.position, e.size);
        }
        track.position += e.size;
        switch (e.command)
        {
        case kCmdTempo:
            {
                tempo_ = e.value;
                frame_time_ = kFrameTime;
                Action a = TrackAction(Action::kTempo, t, e);
                a.value = e.value;
                actions_.push_back(a);
            }
            break;

        case kCmdDelay1:
        case kCmdDelay2:
        case kCmdDelay3:
            track.counter += DelayTime(e.command, e.value);
            track.tick += e.value;
            if (track.counter >= 0)
            {
                return false;
            }
            break;

        case kCmdNoteOnB:
        case kCmdNoteOn:
            NoteOn(t, e);
            break;

        case kCmdNoteOff:
            NoteOff(t, e);
            break;

        case kCmdControl:
            Control(t, e, track);
            break;

        case kCmdProgram:
            Program(t, e);
            break;

        case kCmdBend:
            channels_[e.channel].bend = e.value;
            actions_.push_back(TrackAction(Action::kBend, t, e));
            break;

        case kCmdEnd:
            // The driver reads the end again every frame, and counts the track as ended each time.
            track.position -= e.size;
            track.done = true;
            return true;

        default:
            break;
        }
    }

    Warn("track " + std::to_string(t) + " loops without a delay; it stops there");
    track.done = true;

    return true;
}

Action Sequencer::TrackAction(Action::Kind kind, int t, const Event& e) const
{
    Action a;
    a.kind = kind;
    a.track = uint8_t(t);
    a.channel = e.channel;
    a.tick = tracks_[size_t(t)].tick;
    a.address = tracks_[size_t(t)].position - e.size;

    return a;
}

void Sequencer::RecordRead(int t, uint32_t address, uint32_t size)
{
    Track& track = tracks_[size_t(t)];
    if (track.stretch >= 0 && stretches_[size_t(track.stretch)].last == address)
    {
        // A loop with no commands in it goes back to its end, and the track reads the end again. The read stays in the
        // stretch.
        return;
    }

    if (track.stretch >= 0 && address == track.next)
    {
        stretches_[size_t(track.stretch)].last = address;
    }
    else
    {
        TrackStretch stretch;
        stretch.track = uint8_t(t);
        stretch.tick = track.tick;
        stretch.first = address;
        stretch.last = address;
        stretch.loop = track.stretch >= 0;
        track.stretch = int(stretches_.size());
        stretches_.push_back(stretch);
    }
    track.next = address + size;
}

int64_t Sequencer::DelayTime(uint32_t command, uint32_t ticks) const
{
    // The delay in microseconds, a quotient and a remainder of ticks * tempo / ticks per quarter, with 8 bits of
    // fraction. A one-byte delay is multiplied in 32 bits.
    const uint32_t per_quarter = header_.ticks_per_quarter;
    const uint64_t product = command == kCmdDelay1 ? uint64_t(uint32_t(ticks * tempo_)) : uint64_t(tempo_) * ticks;
    const uint32_t quotient = uint32_t(product / per_quarter);
    const uint32_t remainder = uint32_t(product % per_quarter);
    const uint32_t fraction = Divide(remainder << 16, per_quarter).quotient;
    const uint32_t low = (quotient << 8) | (fraction >> 8);
    const uint32_t high = command == kCmdDelay1 ? 0 : quotient >> 24;
    return int64_t((uint64_t(high) << 32) | low);
}

void Sequencer::NoteOn(int t, const Event& e)
{
    Action a = TrackAction(Action::kNoteOn, t, e);
    a.a = e.a;
    a.b = e.b;

    // A mono channel always plays in its first slot. Otherwise the note takes the first free slot, or failing that the
    // first one whose note is releasing.
    const int base = e.channel * info_.slots_per_channel;
    const int end = base + info_.slots_per_channel;
    int index = -1;
    if (channels_[e.channel].mono)
    {
        index = base;
    }
    for (uint8_t want : {kSlotFree, kSlotReleased})
    {
        for (int i = base; i < end && index < 0; i++)
        {
            if (slots_[size_t(i)].state == want)
            {
                index = i;
            }
        }
    }
    if (index < 0)
    {
        a.kind = Action::kNoteDropped;
        actions_.push_back(a);
        return;
    }

    // A drum kit plays the instrument for the key at its root key, and a key split at the key. Any type of instrument
    // but a sample or a key split is read as a drum kit. The driver reads these words even where they aren't
    // instruments, as it does for a channel without a program, which reads zeros here.
    const uint32_t top = channels_[e.channel].instrument;
    const Instrument chosen = InstrumentAt(rom_, top);
    uint32_t voice = top;
    if (!chosen.IsSample())
    {
        const uint8_t index_in_map = rom_.U8(chosen.key_map + e.a);
        voice = rom_.U32(chosen.key_table + 4u * index_in_map);
    }
    const Instrument played = InstrumentAt(rom_, voice);
    const bool drum_kit = !chosen.IsSample() && chosen.type != kInstKeySplit;

    Instrument inst;
    a.playable = ReadInstrument(rom_, voice, inst) && SampleValid(rom_, inst) && rom_.Contains(top);
    a.program = channels_[e.channel].program;
    a.instrument = top;
    if (!a.playable)
    {
        char b[160];
        std::snprintf(b, sizeof b,
                      top ? "track %d plays key %d on channel %d, whose instrument has no sample for it"
                          : "track %d plays key %d on channel %d before giving the channel a program; the game plays "
                            "it with the instrument the channel had in the song before",
                      t, e.a, e.channel);
        Warn(b);
    }

    Slot& s = slots_[size_t(index)];
    s.velocity = uint8_t(e.b + 1);
    s.loop_mode = uint8_t(played.loop_mode);
    s.key = e.a;
    s.pitch_key = drum_kit ? uint8_t(played.root_key) : e.a;
    s.position = played.start;
    s.fraction = 0;
    s.phase = kPhaseAttackStart;
    s.level = 0;
    s.instrument = voice;
    s.state = kSlotOn;
    s.track = uint8_t(t);
    a.slot = index;
    actions_.push_back(a);
}

void Sequencer::NoteOff(int t, const Event& e)
{
    // Only the channel's first slot playing the key is released: the driver goes back to the track once it has found
    // one.
    const int base = e.channel * info_.slots_per_channel;
    int released = -1;
    for (int i = base; i < base + info_.slots_per_channel; i++)
    {
        Slot& s = slots_[size_t(i)];
        if (s.state == kSlotOn && s.key == e.a)
        {
            s.state = kSlotReleased;
            released = i;
            break;
        }
    }

    Action a = TrackAction(Action::kNoteOff, t, e);
    a.slot = released;
    a.a = e.a;
    actions_.push_back(a);
}

void Sequencer::Control(int t, const Event& e, Track& track)
{
    ChannelState& c = channels_[e.channel];
    switch (e.a)
    {
    case kCtrlModulation:
        c.modulation = e.b;
        break;

    case kCtrlVolume:
        {
            // The model works out a voice's level only to find whether it's 0, and passes the volume on to the
            // conversion.
            c.volume = e.b;
            Action a = TrackAction(Action::kVolume, t, e);
            a.a = e.b;
            actions_.push_back(a);
        }
        break;

    case kCtrlLoopStart:
        track.loop = track.position;
        break;

    case kCtrlLoopEnd:
        if (track.loop == 0)
        {
            Warn("track " + std::to_string(t) + " ends a loop that it didn't start; the end is ignored");
            break;
        }

        track.position = track.loop;
        break;

    case kCtrlMonoOn:
    case kCtrlPolyOn:
        c.mono = e.a == kCtrlMonoOn;
        break;

    case kCtrlAttack:
    case kCtrlDecay:
    case kCtrlSustain:
    case kCtrlRelease:
        if (info_.envelope_controllers)
        {
            uint8_t* settings[] = {&c.envelope.attack, &c.envelope.decay, &c.envelope.sustain, &c.envelope.release};
            *settings[e.a - kCtrlAttack] = e.b;
        }
        break;

    default:
        break;
    }
}

void Sequencer::Program(int t, const Event& e)
{
    const uint8_t index = rom_.U8(header_.program_map + (e.a & 0x7Fu));
    if (index == 0xFF)
    {
        Warn("track " + std::to_string(t) + " chooses program " + std::to_string(e.a) +
             ", which the tune's program map doesn't have");
    }

    ChannelState& c = channels_[e.channel];
    c.instrument = rom_.U32(header_.instruments + 4u * index);
    c.program = e.a;
    c.envelope = EnvelopeSettings();

    Action a = TrackAction(Action::kProgram, t, e);
    a.a = e.a;
    a.instrument = c.instrument;
    actions_.push_back(a);
}

void Sequencer::UpdateVibrato()
{
    // A channel's vibrato phase runs while its modulation is on, at its instrument's rate, and goes back to 0 when the
    // modulation is turned off.
    for (ChannelState& c : channels_)
    {
        if (c.modulation == 0)
        {
            c.vibrato_phase = 0;
            continue;
        }

        Instrument inst;
        const uint32_t rate = ReadInstrument(rom_, c.instrument, inst) ? inst.vibrato_rate : 0;
        c.vibrato_phase = (c.vibrato_phase + rate) & 0xFFFFFF;
    }
}

void Sequencer::UpdateEnvelope(int index)
{
    Slot& s = slots_[size_t(index)];
    Instrument inst;
    ReadInstrument(rom_, s.instrument, inst);

    // The channel's settings from controllers 20-23 count where each phase starts.
    const EnvelopeSettings& settings = channels_[size_t(s.channel)].envelope;

    // Envelope phases advance within the same frame wherever the driver's code falls through. An update of a released
    // note in kPhaseAttack, kPhaseDecay or kPhaseSustain starts the release instead.
    switch (s.phase)
    {
    case kPhaseAttackStart:
        {
            // The attack takes `steps` + 1 frames, from 0 to the full level.
            AddEnvelopeAction(index, {.attack = settings.attack});
            const uint32_t steps = AttackSteps(inst.attack, settings.attack);
            if (steps == 0)
            {
                s.phase = kPhaseDecayStart;
                s.level = kFullLevel;
                return;
            }

            const Quotient q = Divide(kFullSustain, steps + 1);
            s.level = int32_t(q.remainder << 4);
            s.level_step = int32_t(q.quotient << 4);
            s.phase = kPhaseAttack;
        }
        [[fallthrough]];

    case kPhaseAttack:
        if (s.state == kSlotReleased)
        {
            StartRelease(index, inst);
        }
        else if (s.level + s.level_step >= kFullLevel)
        {
            s.phase = kPhaseDecayStart;
            s.level = kFullLevel;
        }
        else
        {
            s.level += s.level_step;
        }
        return;

    case kPhaseDecayStart:
        {
            // The decay falls from the full level to the sustain level in the fade table's number of frames. Where a
            // channel sets the sustain level but not the decay, the driver reads the decay from a register that holds
            // whatever the code before left there, so the model takes the instrument's.
            AddEnvelopeAction(index, {.decay = settings.decay, .sustain = settings.sustain});
            const uint32_t sustain = SustainLevel(inst.sustain, settings.sustain);
            s.sustain = int32_t(sustain << 4);
            const int frames = FadeFrames(info_, inst.decay, settings.decay);
            if (frames == 0)
            {
                s.phase = kPhaseSustain;
                if (s.sustain < kAudibleLevel)
                {
                    Stop(s);
                }
                else
                {
                    s.level = s.sustain;
                }
                return;
            }

            const Quotient q = Divide(kFullSustain - sustain, uint32_t(frames));
            s.level_step = int32_t(q.quotient << 4);
            s.level -= int32_t(q.remainder << 4);
            s.phase = kPhaseDecay;
        }
        [[fallthrough]];

    case kPhaseDecay:
        {
            if (s.state == kSlotReleased)
            {
                StartRelease(index, inst);
                return;
            }

            int32_t level = s.level - s.level_step;
            if (level <= s.sustain)
            {
                s.phase = kPhaseSustain;
                level = s.sustain;
            }
            if (level < kAudibleLevel)
            {
                Stop(s);
            }
            else
            {
                s.level = level;
            }
            return;
        }

    case kPhaseSustain:
        if (s.state == kSlotReleased)
        {
            StartRelease(index, inst);
        }
        return;

    case kPhaseRelease:
        FallRelease(s);
        return;

    case kPhaseReleaseStart:
    default:
        StartRelease(index, inst);
        return;
    }
}

void Sequencer::StartRelease(int index, const Instrument& inst)
{
    // The release falls from the note's level to 0 in the fade table's number of frames.
    Slot& s = slots_[size_t(index)];
    const EnvelopeSettings& settings = channels_[size_t(s.channel)].envelope;
    AddEnvelopeAction(index, {.release = settings.release});
    const int frames = FadeFrames(info_, inst.release, settings.release);
    if (frames == 0)
    {
        Stop(s);
        return;
    }

    const Quotient q = Divide(uint32_t(s.level) >> 4, uint32_t(frames));
    if (q.quotient == 0)
    {
        Stop(s);
        return;
    }

    s.level_step = int32_t(q.quotient << 4);
    s.level -= int32_t(q.remainder << 4);
    s.phase = kPhaseRelease;
    FallRelease(s);
}

void Sequencer::AddEnvelopeAction(int index, const EnvelopeSettings& used)
{
    if (used.None())
    {
        return;
    }

    Action a;
    a.kind = Action::kEnvelope;
    a.channel = uint8_t(slots_[size_t(index)].channel);
    a.slot = index;
    a.envelope = used;
    actions_.push_back(a);
}

void Sequencer::Mix()
{
    // The mixer takes the voices whose notes are on, then those that are releasing, each in slot order, up to its
    // limit. The others are silent for the frame.
    for (Slot& s : slots_)
    {
        s.mix = s.state == kSlotOn || s.state == kSlotReleased ? MixResult::kSkipped : MixResult::kIdle;
    }

    // The routine before the mixer clears the mix buffer: all of it at 224, 304 and 352 samples a frame, and 88 of
    // its values at any other count.
    const auto samples = size_t(info_.samples_per_frame);
    if (enhance_)
    {
        std::fill(enhanced_.begin(), enhanced_.end(), 0.0f);
        std::fill(peaks_.begin(), peaks_.end(), 0);
    }
    if (render_)
    {
        const size_t cleared =
            samples == 224 || samples == 304 || samples == 352 ? samples : std::min<size_t>(88, samples);
        std::fill(mix_.begin(), mix_.begin() + std::ptrdiff_t(cleared), int16_t(0));
        std::fill(peaks_.begin(), peaks_.end(), 0);
    }

    // The mixer stops once it has taken as many voices as its limit allows. A limit of 0 never stops it.
    int left = info_.voice_limit;
    bool full = false;
    bool first = true;
    for (uint8_t state : {kSlotOn, kSlotReleased})
    {
        for (Slot& s : slots_)
        {
            if (full)
            {
                break;
            }
            if (s.state != state)
            {
                continue;
            }

            // A note's level before its velocity and envelope count is the music volume × (the channel's volume + 1) /
            // 128, × the channel's level / 128. Only the game's code turns a channel's level down, to 0 for the
            // channels in silenced_. In Donkey Kong Country 3's revision, a note at a level of 0 isn't mixed and
            // doesn't count towards the limit. Below a music volume of 128, that also leaves out the notes of a
            // channel at a volume of 0. A lower music volume leaves out those of more channel volumes.
            const bool silenced = Silenced(s.channel);
            if (info_.skips_silent_notes &&
                (silenced || (music_volume_ * (channels_[size_t(s.channel)].volume + 1)) >> 7 == 0))
            {
                s.mix = MixResult::kSilent;
                continue;
            }

            s.mix = MixResult::kMixed;
            MixVoice(s, first);
            first = false;
            full = --left == 0;
        }
    }

    // The mix, clipped to 8 bits, for FIFO A.
    if (render_)
    {
        for (size_t i = 0; i < samples; i++)
        {
            samples_[i] = int8_t(std::clamp<int>(mix_[i], -128, 127));
        }
    }
}

void Sequencer::MixVoice(Slot& s, bool first)
{
    s.step = PitchStep(s);

    // The mixer stops a sample without a loop at its end. It doesn't keep the position it reaches: Advance() moves it.
    Instrument inst;
    ReadInstrument(rom_, s.instrument, inst);
    if (render_)
    {
        RenderVoice(s, inst, first);
    }
    if (enhance_)
    {
        RenderEnhanced(s, inst);
    }

    const uint64_t moved = s.fraction + uint64_t(s.step) * uint32_t(info_.samples_per_frame);
    const uint32_t position = s.position + uint32_t(moved >> 23);
    const bool loops = LoopsForward(s.loop_mode);
    if (int32_t(position) >= int32_t(inst.end) && !loops)
    {
        s.state = kSlotFree;
    }
}

void Sequencer::RenderVoice(const Slot& s, const Instrument& inst, bool first)
{
    // The voice's level, out of 128: the music volume, scaled by the channel's volume, the velocity and the envelope,
    // each step truncated.
    uint32_t level = (music_volume_ * (channels_[size_t(s.channel)].volume + 1)) >> 7;
    level = Silenced(s.channel) ? 0 : level;
    level = (uint32_t(s.velocity) * level) >> 7;
    level = (uint32_t(s.level) * level) >> 19;

    // The bytes on either side of the position, interpolated by its 23-bit fraction. A sample past its end loops back
    // by the loop's length, a sample at a time, or else stops the voice for the rest of the frame.
    //
    // Banjo-Pilot's mixer writes the first voice's samples in pairs where the voice doesn't reach its sample's end in
    // the frame: the first sample, plus the second shifted up by 16 bits, as one word. The first sample's sign borrows
    // 1 from the second where it's negative.
    const bool muted = muted_ >> s.track & 1;
    const uint32_t reached = s.position + uint32_t((uint64_t(mix_.size()) * s.step + s.fraction) >> 23);
    const bool pairs = info_.writes_pairs && first && int32_t(reached) < int32_t(inst.end);
    int32_t peak = 0;
    uint32_t position = s.position;
    uint32_t fraction = s.fraction;
    for (size_t i = 0; i < mix_.size(); i++)
    {
        const int32_t a = rom_.S8(position);
        const int32_t b = rom_.S8(position + 1);
        const int32_t point = a + (int32_t(fraction * uint32_t(b - a)) >> 23);
        const int32_t part = muted ? 0 : int32_t(uint32_t(point) * level) >> 7;
        const int32_t borrow = pairs && i % 2 == 1 && mix_[i - 1] < 0 ? 1 : 0;
        mix_[i] = int16_t(first ? part - borrow : mix_[i] + part);
        peak = std::max(peak, part < 0 ? -part : part);

        fraction += s.step;
        position += fraction >> 23;
        fraction &= 0x7FFFFF;
        if (int32_t(position) >= int32_t(inst.end))
        {
            if (!LoopsForward(s.loop_mode))
            {
                break;
            }
            position -= inst.loop_length;
        }
    }

    if (!enhance_)
    {
        int32_t& track = peaks_[s.track];
        track = std::max(track, peak);
    }
}

void Sequencer::RenderEnhanced(const Slot& s, const Instrument& inst)
{
    if ((muted_ >> s.track & 1) || Silenced(s.channel))
    {
        return;
    }

    // The level without the driver's truncations: the music volume, the channel's volume, the velocity and the
    // envelope, each out of its full scale.
    const double level = music_volume_ / 128.0 * (channels_[size_t(s.channel)].volume + 1) / 128.0 * s.velocity /
                         128.0 * double(s.level) / double(int64_t(1) << 19);

    // The driver's step moves the position by 1/2^23 bytes for each of its samples. The output's samples come at
    // agb::kEnhancedRate, over the same frame.
    const double step = double(s.step) / double(1 << 23) * double(info_.samples_per_frame) / double(enhanced_.size());
    const double end = double(int32_t(inst.end));
    const double loop = double(inst.loop_length);
    double position = double(s.position) + double(s.fraction) / double(1 << 23);
    double peak = 0;
    for (float& out : enhanced_)
    {
        const auto at = uint32_t(position);
        const double fraction = position - double(at);
        const double a = rom_.S8(at);
        const double b = rom_.S8(at + 1);
        const double part = (a + (b - a) * fraction) * level;
        out += float(part);
        peak = std::max(peak, std::abs(part));

        position += step;
        if (position >= end)
        {
            if (!LoopsForward(s.loop_mode) || loop <= 0)
            {
                break;
            }
            position -= loop * std::ceil((position - end + 1e-9) / loop);
        }
    }

    int32_t& track = peaks_[s.track];
    track = std::max(track, int32_t(std::lround(peak)));
}

void Sequencer::Advance(Slot& s)
{
    // Each frame moves every voice on by the frame's samples, whether it was mixed or not.
    const uint32_t step = s.step ? s.step : PitchStep(s);
    s.step = 0;
    const uint64_t moved =
        ((uint64_t(s.position) << 23) | s.fraction) + uint64_t(step) * uint32_t(info_.samples_per_frame);
    s.fraction = uint32_t(moved & 0x7FFFFF);
    s.position = uint32_t(moved >> 23);

    // Past the sample's end, a loop goes back by its length. In loop mode 1 the older revisions store the free state
    // through a register that doesn't hold the slot's address, so the slot stays in use until the mixer next takes the
    // voice.
    Instrument inst;
    ReadInstrument(rom_, s.instrument, inst);
    if (int32_t(s.position) < int32_t(inst.end) || (s.loop_mode == kLoopOnce && !info_.frees_loop_once))
    {
        return;
    }
    if (s.loop_mode == kLoopOnce || s.loop_mode == kLoopOnceB)
    {
        s.state = kSlotFree;
        return;
    }
    if (inst.loop_length == 0)
    {
        s.state = kSlotFree;
        Warn("an instrument loops with a loop length of 0");
        return;
    }

    while (int32_t(s.position) >= int32_t(inst.end))
    {
        s.position -= inst.loop_length;
    }
}

uint32_t Sequencer::PitchStep(const Slot& s) const
{
    Instrument voice;
    ReadInstrument(rom_, s.instrument, voice);

    // The pitch in semitones from the root key, as 32.32 fixed point: the fine tune (cents), the bend and the vibrato.
    const int64_t total = kCent * voice.fine_tune + PitchOffset(s.channel);
    const uint32_t fraction = uint32_t(total);
    const int32_t semitones = int32_t(s.pitch_key) - int32_t(voice.root_key) + int32_t(total >> 32);

    // The pitch table gives 2^(n/12) in 9.23 fixed point, and the driver interpolates between its entries.
    uint32_t ratio = 0;
    if (info_.pitch_table)
    {
        const uint32_t at = info_.pitch_table + uint32_t(semitones) * 4;
        const uint32_t low = rom_.U32(at);
        const uint32_t high = rom_.U32(at + 4);
        ratio = low + uint32_t((uint64_t(fraction) * (high - low)) >> 32);
    }
    else
    {
        const double semis = semitones + fraction / 4294967296.0;
        ratio = uint32_t(std::lround(8388608.0 * std::pow(2.0, semis / 12.0)));
    }

    const uint32_t rate = uint32_t((uint64_t(info_.rate_scale) * voice.rate) >> 14);
    return uint32_t((uint64_t(ratio) * rate) >> 24);
}

int64_t Sequencer::PitchOffset(int channel) const
{
    if (channel < 0 || channel >= kChannels)
    {
        return 0;
    }

    // The bend and the vibrato's depth come from the channel's instrument, which may not be the one a voice plays.
    const ChannelState& c = channels_[size_t(channel)];
    Instrument inst;
    ReadInstrument(rom_, c.instrument, inst);
    const int32_t range = int32_t(uint32_t(inst.bend_range) * 0x80000u);
    const int64_t bend = int64_t(int32_t(c.bend - 0x2000)) * range;
    if (c.modulation == 0)
    {
        return bend;
    }

    // The vibrato's sine, interpolated between the table's 257 entries.
    const uint32_t index = c.vibrato_phase >> 16;
    const int32_t part = int32_t(c.vibrato_phase & 0xFFFF);
    int32_t sine = 0;
    if (info_.sine_table)
    {
        const int32_t a = rom_.S32(info_.sine_table + index * 4);
        const int32_t b = rom_.S32(info_.sine_table + index * 4 + 4);
        sine = a + int32_t((int64_t(b - a) * part) >> 16);
    }
    else
    {
        sine =
            int32_t(std::lround(4294967296.0 / 12800.0 * std::sin(c.vibrato_phase / 16777216.0 * 6.283185307179586)));
    }

    const int32_t scaled = int32_t(uint32_t(c.modulation + 1) * uint32_t(sine));
    return bend + int64_t(scaled) * inst.vibrato_depth;
}

void Sequencer::Audition(uint8_t program, uint8_t key, uint8_t velocity)
{
    if (!valid_)
    {
        throw std::logic_error("the tune's header can't be read");
    }

    audition_ = true;
    audition_key_ = uint8_t(key & 0x7F);
    Event e;
    e.channel = 0;
    e.a = uint8_t(program & 0x7F);
    Program(0, e);
    e.a = audition_key_;
    e.b = uint8_t(velocity & 0x7F);
    NoteOn(0, e);
}

void Sequencer::ReleaseAudition()
{
    if (!audition_)
    {
        return;
    }

    Event e;
    e.channel = 0;
    e.a = audition_key_;
    NoteOff(0, e);
}

void Sequencer::MixSamples(bool on)
{
    render_ = on;
    const auto samples = size_t(std::max(info_.samples_per_frame, 0));
    if (on && mix_.size() != samples)
    {
        mix_.assign(samples, 0);
        samples_.assign(samples, 0);
    }
    if (on)
    {
        peaks_.assign(tracks_.size(), 0);
    }
}

void Sequencer::MixEnhanced(bool on)
{
    enhance_ = on;
    enhanced_.assign(on ? size_t(agb::kEnhancedFrameSamples) : 0, 0.0f);
    peaks_.assign(tracks_.size(), 0);
}

bool Sequencer::Silenced(int channel) const
{
    // The driver gives every channel a level of 128 as it starts the tune, in the tune's first frame. The game's code
    // turns the levels down between frames.
    return frame_ > 0 && (silenced_ >> channel & 1);
}

void Sequencer::Warn(const std::string& message)
{
    WarnOnce(warnings_, message);
}

} // namespace ntdlib::rare
