// SPDX-License-Identifier: MIT

// The handlers of the sequence commands. They build with driver_sequences.cpp as one translation unit.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "driver_state.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/tables.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::dse
{

// The handlers of the commands, from the table at 0x020B0B90. `at` points past the code, and each handler moves it past
// what it reads.
void Driver::State::RunEvent(Sequence& sequence, TrackState& track, uint8_t code, uint32_t& at)
{
    Channel& channel = *track.channel;
    Synth& synth = sequence.synth;
    const auto next = [&]()
    {
        return Byte(sequence, track, at++);
    };
    const auto next16 = [&]()
    {
        const uint8_t low = next();
        return static_cast<uint16_t>(low | next() << 8);
    };
    const auto update_bend = [&]()
    {
        channel.bend =
            static_cast<uint16_t>(channel.tuning + static_cast<int16_t>(channel.bend_fade.current >> 16) + synth.bend);
        FlagVoices(channel, 0x10);
    };
    const auto update_volume = [&](int32_t volume)
    {
        channel.volume =
            static_cast<uint16_t>(Wrap(int64_t{synth.combined_volume} * (volume * channel.expression)) / (127 * 127));
        FlagVoices(channel, 0x20);
    };
    const auto set_fade = [&](Fade& fade)
    {
        const uint16_t ticks = next16();
        const int32_t target = static_cast<int8_t>(next()) << 16;
        fade.target = target;
        uint16_t stored = ticks;
        if (ticks == 0)
        {
            fade.current = target;
        }
        else
        {
            const int32_t difference = Wrap(int64_t{target} - fade.current);
            if (difference == 0)
            {
                stored = 0;
            }
            else
            {
                fade.delta = difference / ticks;
            }
        }
        fade.ticks = stored;
    };
    const auto lfo_setup = [&](LfoSettings& lfo)
    {
        const uint8_t b0 = next();
        const uint8_t b1 = next();
        const uint8_t b2 = next();
        const uint8_t b3 = next();
        lfo.waveform = next();
        lfo.amplitude = static_cast<int16_t>(b0 + (b1 << 8));
        lfo.phase_ms = static_cast<uint16_t>(b2 + (b3 << 8));
        lfo.delay_ms = 0;
        lfo.envelope_ms = 0;
        lfo.unknown14 = 0;
    };
    const auto lfo_envelope = [&](LfoSettings& lfo)
    {
        lfo.delay_ms = next16();
        lfo.envelope_ms = next16();
    };
    const auto use_lfo = [&](LfoSettings& lfo, uint8_t output)
    {
        uint8_t type = next();
        if (type == 2)
        {
            type = 1;
        }
        lfo.type = type;
        lfo.output = type == 0 ? 0 : output;
    };
    // The LFO that events 0xF0 to 0xF3 set. The model keeps a selection past the 4 LFOs off the channel's settings.
    LfoSettings spare;
    const auto selected_lfo = [&]() -> LfoSettings&
    {
        const uint8_t index = channel.unknown5a[7];
        return index < channel.lfos.size() ? channel.lfos[index] : spare;
    };
    SubLoop spare_loop;
    const auto sub_loop = [&](int index) -> SubLoop&
    {
        return index >= 0 && index < static_cast<int>(track.sub_loops.size())
                   ? track.sub_loops[static_cast<std::size_t>(index)]
                   : spare_loop;
    };

    switch (code)
    {
    case kWaitAgain:
        track.wait = track.previous_wait;
        break;
    case kWaitChange:
        track.previous_wait += static_cast<uint32_t>(static_cast<int8_t>(next()));
        track.wait = track.previous_wait;
        break;
    case kWait1Byte:
        track.previous_wait = next();
        track.wait = track.previous_wait;
        break;
    case kWait2Bytes:
        track.previous_wait = next16();
        track.wait = track.previous_wait;
        break;
    case kWait3Bytes:
        {
            const uint32_t low = next16();
            track.previous_wait = low + (uint32_t{next()} << 16);
            track.wait = track.previous_wait;
            break;
        }
    case kWaitForVoices:
        if (CountActive(channel) > 0)
        {
            track.wait = Byte(sequence, track, at);
            at--;
        }
        else
        {
            at++;
        }
        break;
    case kEndTrack:
        if (track.loop_start == 0)
        {
            track.playing = 0;
            at--;
            FreeVoices(channel);
        }
        else
        {
            track.loops++;
            sequence.looped = 1;
            at = track.loop_start;
            if (hooks.on_loop && &sequence == Followed())
            {
                hooks.on_loop(static_cast<int>(&track - sequence.tracks.data()));
            }
        }
        break;
    case kLoopPoint:
        track.loop_start = at;
        if (hooks.on_loop_start && &sequence == Followed())
        {
            hooks.on_loop_start(static_cast<int>(&track - sequence.tracks.data()));
        }
        break;
    case kRepeatStart:
        {
            const uint8_t count = next();
            SubLoop& loop = sub_loop(track.loop_index);
            loop.start = at;
            loop.end = 0;
            loop.count = count;
            loop.octave = track.octave;
            track.loop_index++;
            break;
        }
    case kRepeatEnd:
        {
            const int index = track.loop_index - 1;
            SubLoop& loop = sub_loop(index);
            const int count = loop.count - 1;
            if (count != 0)
            {
                loop.end = at;
                loop.count = static_cast<uint8_t>(count);
                // The earlier build keeps the octave of the repeat's end (0x02063098 in Explorers of Time's ARM9
                // binary).
                if (build != Build::kEarlier)
                {
                    track.octave = loop.octave;
                }
                at = loop.start;
            }
            else
            {
                track.loop_index = static_cast<uint8_t>(index);
            }
            break;
        }
    case kRepeatExit:
        {
            const uint8_t n = track.loop_index;
            SubLoop& loop = sub_loop(n - 1);
            if (loop.count - 1 == 0)
            {
                at = loop.end;
                track.loop_index = static_cast<uint8_t>(n - 1);
            }
            break;
        }
    case kOctave:
        track.octave = next();
        break;
    case kOctaveChange:
        track.octave = static_cast<uint8_t>(track.octave + static_cast<int8_t>(next()));
        break;
    case kTempo:
    case kTempoAgain:
        {
            const uint8_t bpm = next();
            uint32_t beat = static_cast<uint32_t>((sequence.tempo.current >> 16) * bpm) >> 8;
            if (beat == 0)
            {
                beat = 1;
            }
            sequence.microseconds_per_beat = static_cast<int32_t>(60'000'000u / beat);
            sequence.bpm = bpm;
            break;
        }
    case kBank:
        {
            const uint8_t high = next();
            SelectBank(channel, static_cast<uint16_t>((high << 8) + next()));
            break;
        }
    case kBankHigh:
        SelectBank(channel, static_cast<uint16_t>((next() << 8) + (channel.bank_id & 0xFF)));
        break;
    case kBankLow:
        SelectBank(channel, static_cast<uint16_t>((channel.bank_id & ~0xFF) + next()));
        break;
    case kSkipByte:
        at++;
        break;
    case kProgram:
        SelectProgram(channel, next());
        break;
    case kSongFade:
        {
            const uint16_t ms = next16();
            uint16_t ticks = 0;
            if (build == Build::kEarlier)
            {
                // 0x0206326C in Explorers of Time's ARM9 binary: the earlier build multiplies only the high byte of
                // the time by 1000, and a fade can take 0 ticks.
                const auto earlier = static_cast<int32_t>((ms & 0xFF00) * 1000 + (ms & 0xFF));
                ticks = static_cast<uint16_t>(Divide(earlier, tick));
            }
            else if (ms != 0)
            {
                ticks = static_cast<uint16_t>(DivideUnsigned(ms * 1000u, static_cast<uint32_t>(int32_t{tick})));
                if (ticks == 0)
                {
                    ticks = 1;
                }
            }
            const int32_t target = next() << 16;
            sequence.volume.target = target;
            if (ticks == 0)
            {
                sequence.volume.current = target;
            }
            else
            {
                const int32_t difference = Wrap(int64_t{target} - sequence.volume.current);
                if (difference == 0)
                {
                    ticks = 0;
                }
                else
                {
                    sequence.volume.delta = Divide(difference, ticks);
                }
            }
            sequence.volume.ticks = ticks;
            break;
        }
    case kEnvelopeDefaults:
        channel.envelope.Reset();
        break;
    case kAttackBegin:
        channel.envelope.attack_begin = next();
        channel.envelope.SetUse();
        break;
    case kAttackTime:
        channel.envelope.attack_time = next();
        channel.envelope.SetUse();
        break;
    case kHoldTime:
        channel.envelope.hold_time = next();
        channel.envelope.SetUse();
        break;
    case kDecayAndSustain:
        {
            const uint8_t decay = next();
            const uint8_t sustain = next();
            if (decay != 0xFF)
            {
                channel.envelope.decay_time = decay;
            }
            if (sustain != 0xFF)
            {
                channel.envelope.sustain_level = sustain;
            }
            channel.envelope.SetUse();
            break;
        }
    case kSustainTime:
        channel.envelope.sustain_time = next();
        channel.envelope.SetUse();
        break;
    case kReleaseTime:
        channel.envelope.release_time = next();
        channel.envelope.SetUse();
        break;
    case kLengthMultiplier:
        track.multiplier = next();
        break;
    case kLfoLevel:
        {
            const auto level = static_cast<int8_t>(next());
            channel.lfo_level = level;
            // The earlier build also marks the channel's pitch (0x02064894 in Explorers of Time's ARM9 binary).
            if (build == Build::kEarlier)
            {
                FlagVoices(channel, 0x10);
            }
            ForEachVoice(channel, [level](Voice& voice) { SetConstantLevels(voice.lfos, level); });
            break;
        }
    case kDamper:
        if (next() >= 0x40)
        {
            track.channel->flags |= 1;
        }
        else
        {
            track.channel->flags &= static_cast<uint8_t>(~1);
            if (build == Build::kEarlier)
            {
                ReleaseDampedVoices(*track.channel);
            }
            else
            {
                ReleaseEndedNotes(*track.channel);
            }
        }
        break;
    case kLegato:
        channel.flags |= 2;
        break;
    case kOptionalVolume:
        channel.optional_volume = next();
        break;
    case kSkipTwo:
    case kSkipTwoAgain:
        at += 2;
        break;
    case kTuning:
        channel.tuning = static_cast<int16_t>(static_cast<int8_t>(next()) << 8);
        update_bend();
        break;
    case kTuningSemitones:
        channel.tuning = static_cast<int16_t>((static_cast<int8_t>(next()) << 8) + channel.tuning);
        update_bend();
        break;
    case kTuningSteps:
        channel.tuning = static_cast<int16_t>((static_cast<int8_t>(next()) << 2) + channel.tuning);
        update_bend();
        break;
    case kTuningWord:
        channel.tuning = static_cast<int16_t>(channel.tuning + next16());
        update_bend();
        break;
    case kTuningSlide:
        {
            uint16_t ticks = next16();
            const uint8_t semitones = next();
            if (!(channel.flags & 2))
            {
                channel.bend_fade.current = 0;
            }
            const auto target = static_cast<int32_t>(
                uint32_t{static_cast<uint16_t>((channel.bend_fade.current >> 16) + (semitones << 8))} << 16);
            channel.bend_fade.target = target;
            if (ticks == 0)
            {
                channel.bend_fade.current = target;
            }
            else
            {
                const int32_t difference = Wrap(int64_t{target} - channel.bend_fade.current);
                if (difference == 0)
                {
                    ticks = 0;
                }
                else
                {
                    channel.bend_fade.delta = Divide(difference, ticks);
                }
            }
            channel.bend_fade.ticks = ticks;
            channel.bend = static_cast<uint16_t>(synth.bend + channel.tuning +
                                                 static_cast<uint16_t>(channel.bend_fade.current >> 16));
            FlagVoices(channel, 0x10);
            break;
        }
    case kRandomKeys:
        {
            const uint8_t a = next();
            const uint8_t b = next();
            channel.random_low = std::min(a, b);
            channel.random_high = std::max(a, b);
            break;
        }
    case kJitter:
        channel.jitter = static_cast<int16_t>(next16());
        break;
    case kKeyBend:
        {
            const uint8_t high = next();
            ApplyKeyBend(channel, static_cast<int16_t>((high << 8) + next()));
            break;
        }
    case kUnusedWord:
        {
            const uint8_t high = next();
            channel.unknown56 = static_cast<int16_t>((high << 8) + next());
            break;
        }
    case kBendRange:
        channel.bend_range = next();
        break;
    case kBendLfo:
        {
            LfoSettings& lfo = channel.lfos[0];
            const uint8_t b0 = next();
            const uint8_t b1 = next();
            const uint8_t b2 = next();
            const uint8_t b3 = next();
            const uint8_t b4 = next();
            lfo.type = 1;
            lfo.output = 1;
            lfo.waveform = b4;
            lfo.amplitude = static_cast<int16_t>(b0 + (b1 << 8));
            lfo.phase_ms = static_cast<uint16_t>(b2 + (b3 << 8));
            lfo.delay_ms = 0;
            lfo.envelope_ms = 0;
            lfo.unknown14 = 0;
            break;
        }
    case kBendLfoEnvelope:
        lfo_envelope(channel.lfos[0]);
        break;
    case kBendLfoSwitch:
        use_lfo(channel.lfos[0], 1);
        break;
    case kVolume:
        {
            // The earlier build reads the volume unsigned (0x020636C8 in Explorers of Time's ARM9 binary).
            const uint8_t byte = next();
            const int32_t volume = build == Build::kEarlier ? int32_t{byte} : int32_t{static_cast<int8_t>(byte)};
            channel.volume_fade.target = volume << 16;
            channel.volume_fade.current = volume << 16;
            channel.volume_fade.ticks = 0;
            update_volume(volume);
            break;
        }
    case kVolumeChange:
        {
            const int32_t volume =
                std::clamp(static_cast<int8_t>(next()) + (channel.volume_fade.current >> 16), 0, 0x7F);
            channel.volume_fade.target = volume << 16;
            channel.volume_fade.current = volume << 16;
            channel.volume_fade.ticks = 0;
            update_volume(volume);
            break;
        }
    case kVolumeSlide:
        set_fade(channel.volume_fade);
        if (build == Build::kEarlier)
        {
            // The earlier build updates the channel's volume at once (0x02063738 in Explorers of Time's ARM9 binary).
            update_volume(channel.volume_fade.current >> 16);
        }
        break;
    case kExpression:
        channel.expression = next();
        update_volume(channel.volume_fade.current >> 16);
        break;
    case kVolumeLfo:
        lfo_setup(channel.lfos[1]);
        channel.lfos[1].type = 1;
        channel.lfos[1].output = 2;
        break;
    case kVolumeLfoEnvelope:
        lfo_envelope(channel.lfos[1]);
        break;
    case kVolumeLfoSwitch:
        use_lfo(channel.lfos[1], 2);
        break;
    case kPan:
        {
            const int32_t pan = std::min<int32_t>(next(), 0x7F);
            channel.pan_fade.target = pan << 16;
            channel.pan_fade.current = pan << 16;
            channel.pan_fade.ticks = 0;
            channel.pan = static_cast<int16_t>(pan + (synth.pan - 0x40));
            FlagVoices(channel, 0x40);
            break;
        }
    case kPanChange:
        {
            const int32_t pan = std::clamp(static_cast<int8_t>(next()) + (channel.pan_fade.current >> 16), 0, 0x7F);
            channel.pan_fade.target = pan << 16;
            channel.pan_fade.current = pan << 16;
            channel.pan_fade.ticks = 0;
            channel.pan = static_cast<int16_t>(pan + (synth.pan - 0x40));
            FlagVoices(channel, 0x40);
            break;
        }
    case kPanSlide:
        set_fade(channel.pan_fade);
        if (build == Build::kEarlier)
        {
            // The earlier build updates the channel's pan at once (0x020638E0 in Explorers of Time's ARM9 binary).
            channel.pan = static_cast<int16_t>((channel.pan_fade.current >> 16) + (synth.pan - 0x40));
            FlagVoices(channel, 0x40);
        }
        break;
    case kPanLfo:
        lfo_setup(channel.lfos[2]);
        channel.lfos[2].type = 1;
        channel.lfos[2].output = 3;
        break;
    case kPanLfoEnvelope:
        lfo_envelope(channel.lfos[2]);
        break;
    case kPanLfoSwitch:
        use_lfo(channel.lfos[2], 3);
        break;
    case kLfo:
        lfo_setup(selected_lfo());
        break;
    case kLfoEnvelope:
        lfo_envelope(selected_lfo());
        break;
    case kLfoParameter:
        {
            const uint8_t parameter = next();
            const uint8_t value = next();
            LfoSettings& lfo = selected_lfo();
            switch (parameter)
            {
            case 1:
                channel.unknown5a[7] = value;
                break;
            case 2:
                lfo.type = value;
                break;
            case 3:
                lfo.output = value;
                break;
            case 4:
                lfo.waveform = value;
                break;
            case 5:
                switch (lfo.output)
                {
                case 1:
                case 4:
                    lfo.amplitude = value * 10;
                    break;
                case 2:
                    lfo.amplitude = value * -20;
                    break;
                default:
                    lfo.amplitude = value * 20;
                    break;
                }
                break;
            case 6:
                lfo.phase_ms = static_cast<uint16_t>(value * 5);
                break;
            case 7:
                lfo.delay_ms = static_cast<uint16_t>(value * 20);
                break;
            case 8:
                lfo.delay_ms = static_cast<uint16_t>((lfo.delay_ms & 0xFF00) | value);
                break;
            case 9:
                lfo.delay_ms = static_cast<uint16_t>((lfo.delay_ms & 0xFF) | value << 8);
                break;
            case 10:
                lfo.envelope_ms = static_cast<uint16_t>(value * 20);
                break;
            default:
                break;
            }
            break;
        }
    case kLfoSwitch:
        {
            const uint8_t index = next();
            uint8_t type = next();
            const uint8_t output = next();
            channel.unknown5a[7] = index;
            LfoSettings& lfo = selected_lfo();
            if (type == 2)
            {
                type = 1;
            }
            lfo.type = type;
            lfo.output = output;
            break;
        }
    case kSignalGame:
        sequence.signal = next();
        break;
    default:
        track.playing = 0;
        break;
    }
}

} // namespace ntdlib::dse
