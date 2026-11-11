// SPDX-License-Identifier: MIT

// The audio lists of libultra's synthesizer for the standard microcode.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "al_synth_constants.h"
#include "equal_power.h"
#include "ntdlib/n64/al_synth.h"
#include "round_toward_zero.h"

namespace ntdlib::n64
{

namespace
{

using al_synth_constants::kAdpcmFrameBytes;
using al_synth_constants::kAdpcmFrameSamples;
using al_synth_constants::kAdpcmFrameShift;
using al_synth_constants::kForever;
using al_synth_constants::kMaxRatio;
using al_synth_constants::kUnityPitch;

// The rate that takes a level from `volume` to `target` over `count` samples. The microcode updates the level on every
// 8th sample. The rate is therefore the ratio per 8 samples, in a whole part and a 16-bit fraction.
int16_t GetRate(double volume, double target, int32_t count, uint16_t& rate_low)
{
    if (count == 0)
    {
        if (target >= volume)
        {
            rate_low = 0xFFFF;

            return 0x7FFF;
        }

        rate_low = 0;

        return 0;
    }

    if (target < 1.0)
    {
        target = 1.0;
    }

    if (volume <= 0)
    {
        volume = 1;
    }

    // ln(target / volume) from a table of the logarithm of the mantissa, like the game's code.
    constexpr std::array<double, 8> kLogarithms = {-0.912537, -0.752072, -0.607683, -0.476438,
                                                   -0.356144, -0.245112, -0.142019, -0.045804};
    constexpr double kLn2 = 0.69314718055994530942;
    constexpr int kFractionBits = 30;
    constexpr int kTableBits = 3;

    const double inverse = 1.0 / count;
    auto steps = static_cast<int32_t>(inverse * static_cast<double>(1 << kFractionBits));
    const double value = target / volume;
    int exponent = 0;
    double mantissa = value;
    if (mantissa != 0.0)
    {
        double magnitude = mantissa > 0.0 ? mantissa : -mantissa;
        for (; magnitude >= 1.0; magnitude *= 0.5)
        {
            exponent++;
        }

        for (; magnitude < 0.5; magnitude += magnitude)
        {
            exponent--;
        }

        mantissa = mantissa > 0.0 ? magnitude : -magnitude;
    }

    const auto index = static_cast<int32_t>(mantissa * static_cast<double>(1 << (kTableBits + 1)));
    double epsilon = (kLogarithms[static_cast<std::size_t>(index - (1 << kTableBits))] + exponent) * kLn2;
    epsilon /= static_cast<double>(1 << kFractionBits);
    double factor = 1.0 + epsilon;
    double rate = 1.0;
    while (steps != 0)
    {
        if ((steps & 1) != 0)
        {
            rate = rate * factor;
        }

        factor *= factor;
        steps >>= 1;
    }

    rate = rate * rate;
    rate = rate * rate;
    rate = rate * rate;

    const auto whole = static_cast<int16_t>(static_cast<int32_t>(rate));
    rate_low =
        static_cast<uint16_t>(static_cast<int32_t>(0xFFFF * (rate - static_cast<double>(static_cast<float>(whole)))));

    return whole;
}

// GoldenEye 007's rate: a straight line in 8-sample steps. The code at 0x7001AAF4 leaves a target of 0 as it is.
int16_t LinearRate(float volume, float target, int32_t count, uint16_t& rate_low)
{
    if (count == 0)
    {
        if (volume <= target)
        {
            rate_low = 0xFFFF;

            return 0x7FFF;
        }

        rate_low = 0;

        return -0x8000;
    }

    const float each = (target - volume) / static_cast<float>(count);
    const auto step = static_cast<float>(static_cast<double>(each) * 8.0);
    const float below = step < 0.0F ? step - 1.0F : step;
    const auto whole = static_cast<int16_t>(static_cast<int32_t>(below));
    rate_low = static_cast<uint16_t>(static_cast<int32_t>((below - static_cast<float>(whole)) * 65535.0F));

    return whole;
}

// Where GoldenEye 007's ramp has reached: the code at 0x7001B150.
int16_t LinearVolume(int16_t volume, int32_t samples, int16_t rate_high, uint16_t rate_low)
{
    const float rate = static_cast<float>(static_cast<int32_t>(rate_high) * 65536) + static_cast<float>(rate_low);
    const auto per_step = static_cast<float>(static_cast<double>(rate) / 65536.0);
    const float moved = per_step * static_cast<float>(samples);
    const auto result = static_cast<float>(static_cast<double>(volume) + (static_cast<double>(moved) * 0.125));

    return static_cast<int16_t>(static_cast<int32_t>(result));
}

float GetVolume(float volume, int32_t samples, int16_t rate_high, uint16_t rate_low)
{
    samples >>= 3;
    if (samples == 0)
    {
        return volume;
    }

    float rate = (static_cast<float>(static_cast<int32_t>(rate_high) << 16) + static_cast<float>(rate_low)) / 65536;
    float result = 1.0F;
    for (int i = 0; i < 32; i++)
    {
        if ((samples & 1) != 0)
        {
            result *= rate;
        }

        samples >>= 1;
        if (samples == 0)
        {
            break;
        }

        rate *= rate;
    }

    return volume * result;
}

} // namespace

void AlSynth::BuildCommands(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    const RoundTowardZero rounding(round_toward_zero_);
    if (form_ == AlSynthForm::kCompact)
    {
        BuildCompactCommands(list, memory, output, samples);

        return;
    }

    uint32_t place = output;
    int left = samples;
    while (left > 0)
    {
        const int count = std::min(kAlMaxChunkSamples, left);
        list.Segment(0, 0);

        // The save filter pulls the main bus. The main bus pulls the effect and then the auxiliary bus.
        list.ClearBuffer(kAlMainLeft, static_cast<uint32_t>(count) << 1);
        list.ClearBuffer(kAlMainRight, static_cast<uint32_t>(count) << 1);
        if (fx_.present)
        {
            PullFx(list, memory, count, current_samples_);
        }
        else
        {
            PullAux(list, memory, count, current_samples_);
        }

        list.MarkChunkEnd();
        list.SetBuffer(0, 0, 0, static_cast<uint32_t>(count) << 1);
        list.Mixer(0, 0x7FFF, kAlAuxLeft, kAlMainLeft);
        list.Mixer(0, 0x7FFF, kAlAuxRight, kAlMainRight);

        list.SetBuffer(0, 0, 0, static_cast<uint32_t>(count) << 1);
        list.Interleave(kAlMainLeft, kAlMainRight);
        list.SetBuffer(0, 0, 0, static_cast<uint32_t>(count) << 2);
        list.SaveBuffer(place);

        left -= count;
        place += static_cast<uint32_t>(count) << 2;
        current_samples_ += count;
    }

    // The voices that are done become free.
    while (!lame_list_.empty())
    {
        const int physical = lame_list_.front();
        lame_list_.erase(lame_list_.begin());
        free_list_.insert(free_list_.begin(), physical);
    }
}

void AlSynth::PullAux(AudioList& list, AudioMemory& memory, int count, int32_t offset)
{
    list.ClearBuffer(kAlAuxLeft, static_cast<uint32_t>(count) << 1);
    list.ClearBuffer(kAlAuxRight, static_cast<uint32_t>(count) << 1);
    for (int physical = 0; physical < static_cast<int>(physical_.size()); physical++)
    {
        list.MarkVoice(physical_[static_cast<std::size_t>(physical)].tag);
        PullMixer(list, memory, physical, count, offset);
    }

    list.MarkVoicesEnd();
}

void AlSynth::PullMixer(AudioList& list, AudioMemory& memory, int physical, int count, int32_t offset)
{
    PhysicalVoiceState& voice = physical_[static_cast<std::size_t>(physical)];
    int16_t in = kAlResamplerOut;
    int16_t out = 0;
    int32_t this_offset = offset;
    int left = count;
    while (!voice.updates.empty())
    {
        Param& update = voice.updates.front();
        const int32_t last_offset = this_offset;
        this_offset = update.delta;
        const int32_t samples = this_offset - last_offset;
        if (samples > left)
        {
            break;
        }

        switch (update.type)
        {
        case ParamType::kStartVoiceAlt:
            StartMixer(memory, voice, update);
            break;

        case ParamType::kSetFxMix:
        case ParamType::kSetPan:
        case ParamType::kSetVolume:
            PullSubFrame(list, voice, in, out, samples, offset);
            ChangeMixer(voice, update);
            break;

        case ParamType::kStartVoice:
            if (update.unity)
            {
                voice.unity_pitch = true;
            }

            ApplySetWave(memory, voice, update.wave);
            voice.playing = true;
            break;

        case ParamType::kStopVoice:
            PullSubFrame(list, voice, in, out, samples, offset);
            ResetFilters(voice);
            break;

        case ParamType::kFreeVoice:
            voice.offset = 0;
            MoveToLame(update.physical);
            break;

        case ParamType::kSetPitch:
            PullSubFrame(list, voice, in, out, samples, offset);
            voice.ratio = update.pitch;
            break;
        }

        out = static_cast<int16_t>(out + (samples << 1));
        left -= samples;
        voice.updates.pop_front();
        free_updates_++;
    }

    PullSubFrame(list, voice, in, out, left, offset);
    if (voice.mixer_delta > voice.segment_end)
    {
        voice.mixer_delta = voice.segment_end;
    }
}

// Starts a voice with its wave, volume, pan, effect mix and pitch.
void AlSynth::StartMixer(AudioMemory& memory, PhysicalVoiceState& voice, const Param& update)
{
    if (update.unity)
    {
        voice.unity_pitch = true;
    }

    ApplySetWave(memory, voice, update.wave);
    voice.playing = true;
    voice.mixer_first = true;
    voice.mixer_delta = 0;
    voice.segment_end = update.samples;
    voice.volume = static_cast<int16_t>((static_cast<int32_t>(update.volume) * update.volume) >> 15);
    voice.pan = update.pan;
    voice.dry = kEqualPower[update.fx_mix];
    voice.wet = kEqualPower[static_cast<std::size_t>(127 - update.fx_mix)];
    if (update.samples != 0)
    {
        voice.current_left = 1;
        voice.current_right = 1;
    }
    else
    {
        voice.current_left =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(voice.pan)]) >> 15);
        voice.current_right =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(127 - voice.pan)]) >> 15);
    }

    voice.ratio = update.pitch;
}

// A change of a voice's volume, pan or effect mix. The levels first move to where the ramp stands at the change.
void AlSynth::ChangeMixer(PhysicalVoiceState& voice, const Param& update)
{
    if (voice.mixer_delta >= voice.segment_end)
    {
        voice.target_left =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(voice.pan)]) >> 15);
        voice.target_right =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(127 - voice.pan)]) >> 15);
        voice.mixer_delta = voice.segment_end;
        voice.current_left = voice.target_left;
        voice.current_right = voice.target_right;
    }
    else
    {
        voice.current_left =
            linear_ramps_
                ? LinearVolume(voice.current_left, voice.mixer_delta, voice.rate_high_left, voice.rate_low_left)
                : static_cast<int16_t>(static_cast<int32_t>(
                      GetVolume(voice.current_left, voice.mixer_delta, voice.rate_high_left, voice.rate_low_left)));
        voice.current_right =
            linear_ramps_
                ? LinearVolume(voice.current_right, voice.mixer_delta, voice.rate_high_right, voice.rate_low_right)
                : static_cast<int16_t>(static_cast<int32_t>(
                      GetVolume(voice.current_right, voice.mixer_delta, voice.rate_high_right, voice.rate_low_right)));
    }

    if (voice.current_left == 0)
    {
        voice.current_left = 1;
    }

    if (voice.current_right == 0)
    {
        voice.current_right = 1;
    }

    if (update.type == ParamType::kSetPan)
    {
        voice.pan = static_cast<int16_t>(update.data);
    }
    else if (update.type == ParamType::kSetVolume)
    {
        voice.mixer_delta = 0;
        voice.volume = static_cast<int16_t>((update.data * update.data) >> 15);
        voice.segment_end = update.more;
    }
    else
    {
        voice.dry = kEqualPower[static_cast<std::size_t>(update.data)];
        voice.wet = kEqualPower[static_cast<std::size_t>(127 - update.data)];
    }

    voice.mixer_first = true;
}

void AlSynth::PullSubFrame(AudioList& list, PhysicalVoiceState& voice, int16_t& in, int16_t& out, int count,
                           int32_t offset)
{
    if (!voice.playing || count == 0)
    {
        return;
    }

    PullResampler(list, voice, in, count, offset);
    list.SetBuffer(0, static_cast<uint32_t>(in), static_cast<uint32_t>(kAlMainLeft + out),
                   static_cast<uint32_t>(count) << 1);
    list.SetBuffer(kAudioFlagAux, static_cast<uint32_t>(kAlMainRight + out), static_cast<uint32_t>(kAlAuxLeft + out),
                   static_cast<uint32_t>(kAlAuxRight + out));
    if (voice.mixer_first)
    {
        voice.mixer_first = false;
        voice.target_left =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(voice.pan)]) >> 15);
        voice.rate_high_left =
            linear_ramps_ ? LinearRate(voice.current_left, voice.target_left, voice.segment_end, voice.rate_low_left)
                          : GetRate(voice.current_left, voice.target_left, voice.segment_end, voice.rate_low_left);
        voice.target_right =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(127 - voice.pan)]) >> 15);
        voice.rate_high_right =
            linear_ramps_ ? LinearRate(voice.current_right, voice.target_right, voice.segment_end, voice.rate_low_right)
                          : GetRate(voice.current_right, voice.target_right, voice.segment_end, voice.rate_low_right);
        list.SetVolume(kAudioFlagLeft | kAudioFlagVolume, static_cast<uint16_t>(voice.current_left), 0, 0);
        list.SetVolume(kAudioFlagVolume, static_cast<uint16_t>(voice.current_right), 0, 0);
        list.SetVolume(kAudioFlagLeft, static_cast<uint16_t>(voice.target_left),
                       static_cast<uint16_t>(voice.rate_high_left), voice.rate_low_left);
        list.SetVolume(0, static_cast<uint16_t>(voice.target_right), static_cast<uint16_t>(voice.rate_high_right),
                       voice.rate_low_right);
        list.SetVolume(kAudioFlagAux, static_cast<uint16_t>(voice.dry), 0, static_cast<uint16_t>(voice.wet));
        list.EnvMixer(kAudioFlagInit | kAudioFlagAux, voice.mixer_state);
    }
    else
    {
        list.EnvMixer(kAudioFlagAux, voice.mixer_state);
    }

    in = static_cast<int16_t>(in + (count << 1));
    voice.mixer_delta += count;
}

void AlSynth::PullResampler(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count, int32_t offset)
{
    (void)offset;
    int16_t in = kAlDecoderOut;
    if (count == 0)
    {
        return;
    }

    if (voice.unity_pitch)
    {
        PullDecoder(list, voice, in, count);
        list.DmemMove(static_cast<uint16_t>(in), static_cast<uint16_t>(out), static_cast<uint32_t>(count) << 1);

        return;
    }

    if (voice.ratio > kMaxRatio)
    {
        voice.ratio = static_cast<float>(kMaxRatio);
    }

    voice.ratio = static_cast<float>(static_cast<int32_t>(voice.ratio * kUnityPitch));
    voice.ratio = voice.ratio / kUnityPitch;

    const float exact = voice.delta + voice.ratio * static_cast<float>(count);
    const auto in_count = static_cast<int32_t>(exact);
    voice.delta = exact - static_cast<float>(in_count);

    PullDecoder(list, voice, in, in_count);

    const auto increment = static_cast<int32_t>(voice.ratio * kUnityPitch);
    list.SetBuffer(0, static_cast<uint16_t>(in), static_cast<uint16_t>(out), static_cast<uint32_t>(count) << 1);
    list.Resample(voice.resampler_first ? 1 : 0, static_cast<uint32_t>(increment), voice.resampler_state);
    voice.resampler_first = false;
}

void AlSynth::PullDecoder(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count)
{
    if (voice.wave.raw)
    {
        PullRaw(list, voice, out, count);
    }
    else
    {
        PullAdpcm(list, voice, out, count);
    }
}

void AlSynth::DecodeChunk(AudioList& list, PhysicalVoiceState& voice, int tsam, int bytes, int16_t out, int16_t in,
                          uint8_t flags)
{
    int align = 0;
    if (bytes > 0)
    {
        const auto location = static_cast<uint32_t>(voice.memory_in);
        align = static_cast<int>(location & 7);
        bytes += align;
        list.SetBuffer(0, static_cast<uint16_t>(in), 0, static_cast<uint32_t>(bytes + 8 - (bytes & 7)));
        list.LoadBuffer(location - static_cast<uint32_t>(align));
    }

    if ((flags & kAudioFlagLoop) != 0)
    {
        list.SetLoop(voice.decoder_loop_state);
    }

    list.SetBuffer(0, static_cast<uint16_t>(in + align), static_cast<uint16_t>(out), static_cast<uint32_t>(tsam) << 1);
    list.Adpcm(flags, voice.decoder_state);
    voice.decoder_first = false;
}

void AlSynth::PullAdpcm(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count)
{
    if (count == 0)
    {
        return;
    }

    const int16_t in = kAlDecoderIn;
    list.LoadAdpcm(static_cast<uint32_t>(voice.bytes_book), voice.wave.book);

    const bool looped = count + voice.sample > static_cast<int32_t>(voice.loop_end) && voice.loop_count != 0;
    int samples = looped ? static_cast<int>(voice.loop_end) - voice.sample : count;
    const int left_in_frame = voice.last_sample != 0 ? kAdpcmFrameSamples - voice.last_sample : 0;
    int tsam = std::max(samples - left_in_frame, 0);
    int frames = (tsam + kAdpcmFrameSamples - 1) >> kAdpcmFrameShift;
    int bytes = frames * kAdpcmFrameBytes;
    const uint8_t first_flag = voice.decoder_first ? 1 : 0;

    if (looped)
    {
        DecodeChunk(list, voice, tsam, bytes, out, in, first_flag);
        out = static_cast<int16_t>(out + (voice.last_sample != 0 ? voice.last_sample << 1 : kAdpcmFrameSamples << 1));
        PullAdpcmLoops(list, voice, out, count, samples, frames);

        return;
    }

    samples = frames << kAdpcmFrameShift;
    int over_flow =
        static_cast<int>(voice.memory_in + bytes - (static_cast<int64_t>(voice.wave.base) + voice.wave.length));
    over_flow = std::max(over_flow, 0);
    int over = (over_flow / kAdpcmFrameBytes) << kAdpcmFrameShift;
    over = std::min(over, samples + left_in_frame);
    bytes -= over_flow;

    bool decoded = false;
    if (over - (over & 0xF) < count)
    {
        decoded = true;
        DecodeChunk(list, voice, samples - over, bytes, out, in, first_flag);
        out = static_cast<int16_t>(out + (voice.last_sample != 0 ? voice.last_sample << 1 : kAdpcmFrameSamples << 1));
        voice.last_sample = (count + voice.last_sample) & 0xF;
        voice.sample += count;
        voice.memory_in += kAdpcmFrameBytes * frames;
    }
    else
    {
        voice.last_sample = 0;
        voice.memory_in += kAdpcmFrameBytes * frames;
    }

    if (over != 0)
    {
        voice.last_sample = 0;
        const int start_zero = decoded ? (left_in_frame + samples - over) << 1 : 0;
        list.ClearBuffer(static_cast<uint32_t>(start_zero + out), static_cast<uint32_t>(over) << 1);
    }
}

// The rest of a chunk that runs past the loop's end: the passes of the loop from its start.
void AlSynth::PullAdpcmLoops(AudioList& list, PhysicalVoiceState& voice, int16_t out, int count, int samples,
                             int frames)
{
    voice.last_sample = static_cast<int32_t>(voice.loop_start & 0xF);
    voice.memory_in = static_cast<int64_t>(voice.wave.base) +
                      kAdpcmFrameBytes * (static_cast<int64_t>(voice.loop_start >> kAdpcmFrameShift) + 1);
    voice.sample = static_cast<int32_t>(voice.loop_start);

    const int16_t in = kAlDecoderIn;
    int buffer_end = out;
    int left = count;
    while (left > samples)
    {
        left -= samples;
        const int place = (buffer_end + ((frames + 1) << (kAdpcmFrameShift + 1))) & ~0x1F;
        buffer_end += samples << 1;
        if (voice.loop_count != kForever && voice.loop_count != 0)
        {
            voice.loop_count--;
        }

        samples = std::min(left, static_cast<int>(voice.loop_end - voice.loop_start));
        const int tsam = std::max(samples - kAdpcmFrameSamples + voice.last_sample, 0);
        frames = (tsam + kAdpcmFrameSamples - 1) >> kAdpcmFrameShift;
        const int bytes = frames * kAdpcmFrameBytes;
        DecodeChunk(list, voice, tsam, bytes, static_cast<int16_t>(place), in,
                    static_cast<uint8_t>((voice.decoder_first ? 1 : 0) | kAudioFlagLoop));
        list.DmemMove(static_cast<uint16_t>(place + (voice.last_sample << 1)), static_cast<uint16_t>(buffer_end),
                      static_cast<uint32_t>(samples) << 1);
    }

    voice.last_sample = (left + voice.last_sample) & 0xF;
    voice.sample += left;
    voice.memory_in += kAdpcmFrameBytes * frames;
}

void AlSynth::PullRaw(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count)
{
    if (count == 0)
    {
        return;
    }

    if (count + voice.sample > static_cast<int32_t>(voice.loop_end) && voice.loop_count != 0)
    {
        int samples = static_cast<int>(voice.loop_end) - voice.sample;
        int bytes = samples << 1;
        int align = 0;
        if (samples > 0)
        {
            const auto location = static_cast<uint32_t>(voice.memory_in);
            align = static_cast<int>(location & 7);
            bytes += align;
            list.SetBuffer(0, static_cast<uint16_t>(out), 0, static_cast<uint32_t>(bytes + 8 - (bytes & 7)));
            list.LoadBuffer(location - static_cast<uint32_t>(align));
        }

        out = static_cast<int16_t>(out + align);
        PullRawLoops(list, voice, out, count, samples);

        return;
    }

    int bytes = count << 1;
    int over_flow =
        static_cast<int>(voice.memory_in + bytes - (static_cast<int64_t>(voice.wave.base) + voice.wave.length));
    over_flow = std::clamp(over_flow, 0, bytes);
    if (over_flow < bytes)
    {
        int align = 0;
        bytes -= over_flow;
        const auto location = static_cast<uint32_t>(voice.memory_in);
        align = static_cast<int>(location & 7);
        bytes += align;
        list.SetBuffer(0, static_cast<uint16_t>(out), 0, static_cast<uint32_t>(bytes + 8 - (bytes & 7)));
        list.LoadBuffer(location - static_cast<uint32_t>(align));
        out = static_cast<int16_t>(out + align);
        voice.sample += count;
        voice.memory_in += static_cast<int64_t>(count) << 1;
    }
    else
    {
        voice.memory_in += static_cast<int64_t>(count) << 1;
    }

    if (over_flow != 0)
    {
        const int start_zero = std::max((count << 1) - over_flow, 0);
        list.ClearBuffer(static_cast<uint32_t>(start_zero + out), static_cast<uint32_t>(over_flow));
    }
}

// The rest of a chunk that runs past the loop's end: the passes of the loop from its start.
void AlSynth::PullRawLoops(AudioList& list, PhysicalVoiceState& voice, int16_t out, int count, int samples)
{
    voice.memory_in = static_cast<int64_t>(voice.wave.base) + (static_cast<int64_t>(voice.loop_start) << 1);
    voice.sample = static_cast<int32_t>(voice.loop_start);
    int place = out;
    int left = count;
    while (left > samples)
    {
        place += samples << 1;
        left -= samples;
        if (voice.loop_count != kForever && voice.loop_count != 0)
        {
            voice.loop_count--;
        }

        samples = std::min(left, static_cast<int>(voice.loop_end - voice.loop_start));
        int bytes = samples << 1;
        const auto location = static_cast<uint32_t>(voice.memory_in);
        const int dram_align = static_cast<int>(location & 7);
        bytes += dram_align;
        const int dmem_align = (place & 7) != 0 ? 8 - (place & 7) : 0;
        list.SetBuffer(0, static_cast<uint16_t>(place + dmem_align), 0, static_cast<uint32_t>(bytes + 8 - (bytes & 7)));
        list.LoadBuffer(location - static_cast<uint32_t>(dram_align));
        if (dram_align != 0 || dmem_align != 0)
        {
            list.DmemMove(static_cast<uint16_t>(place + dram_align + dmem_align), static_cast<uint16_t>(place),
                          static_cast<uint32_t>(samples) << 1);
        }
    }

    voice.sample += left;
    voice.memory_in += static_cast<int64_t>(left) << 1;
}

void AlSynth::PullFx(AudioList& list, AudioMemory& memory, int count, int32_t offset)
{
    PullAux(list, memory, count, offset);

    const int input = kAlAuxLeft;
    const int output = kAlAuxRight;
    int buffer1 = kAlTemp0;
    int buffer2 = kAlTemp1;
    const auto bytes = static_cast<uint32_t>(count) << 1;

    list.SetBuffer(0, 0, 0, bytes);
    list.Mixer(0, 0xDA83, kAlAuxLeft, input);
    list.Mixer(0, 0x5A82, kAlAuxRight, input);
    SaveFxBuffer(list, fx_.input, input, count);
    list.ClearBuffer(output, bytes);

    std::optional<int64_t> previous_out;
    for (Delay& delay : fx_.delays)
    {
        const int64_t in_place = fx_.input - static_cast<int32_t>(delay.input);
        const int64_t out_place = fx_.input - static_cast<int32_t>(delay.output);
        if (previous_out == in_place)
        {
            std::swap(buffer1, buffer2);
        }
        else
        {
            LoadFxBuffer(list, in_place, buffer1, count);
        }

        LoadFxOutput(list, delay, buffer2, count);
        if (delay.feed_forward != 0)
        {
            list.Mixer(0, static_cast<uint16_t>(delay.feed_forward), static_cast<uint32_t>(buffer1),
                       static_cast<uint32_t>(buffer2));
            if (!delay.has_resampler && !delay.has_low_pass)
            {
                SaveFxBuffer(list, out_place, buffer2, count);
            }
        }

        if (delay.feedback != 0)
        {
            list.Mixer(0, static_cast<uint16_t>(delay.feedback), static_cast<uint32_t>(buffer2),
                       static_cast<uint32_t>(buffer1));
            SaveFxBuffer(list, in_place, buffer1, count);
        }

        if (delay.has_low_pass)
        {
            FilterBuffer(list, delay.low_pass, buffer2, count);
        }

        if (!delay.has_resampler)
        {
            SaveFxBuffer(list, out_place, buffer2, count);
        }

        if (delay.gain != 0)
        {
            list.Mixer(0, static_cast<uint16_t>(delay.gain), static_cast<uint32_t>(buffer2), output);
        }

        // The game's code takes this place with the output added, not taken away.
        previous_out = fx_.input + static_cast<int64_t>(static_cast<int32_t>(delay.output));
    }

    fx_.input += count;
    if (fx_.input > static_cast<int32_t>(fx_.length))
    {
        fx_.input -= static_cast<int32_t>(fx_.length);
    }

    list.DmemMove(output, kAlAuxLeft, bytes);
}

void AlSynth::LoadFxBuffer(AudioList& list, int64_t position, int buffer, int count)
{
    if (position < 0)
    {
        position += fx_.length;
    }

    const int64_t updated = position + count;
    const auto length = static_cast<int64_t>(fx_.length);
    const auto address = static_cast<uint32_t>(fx_.base + position * 2);
    if (updated > length)
    {
        const auto after = static_cast<int32_t>(updated - length);
        const auto before = static_cast<int32_t>(length - position);
        list.SetBuffer(0, static_cast<uint16_t>(buffer), 0, static_cast<uint32_t>(before) << 1);
        list.LoadBuffer(address);
        list.SetBuffer(0, static_cast<uint16_t>(buffer + (before << 1)), 0, static_cast<uint32_t>(after) << 1);
        list.LoadBuffer(fx_.base);
    }
    else
    {
        list.SetBuffer(0, static_cast<uint16_t>(buffer), 0, static_cast<uint32_t>(count) << 1);
        list.LoadBuffer(address);
    }

    list.SetBuffer(0, 0, 0, static_cast<uint32_t>(count) << 1);
}

void AlSynth::SaveFxBuffer(AudioList& list, int64_t position, int buffer, int count)
{
    if (position < 0)
    {
        position += fx_.length;
    }

    const int64_t updated = position + count;
    const auto length = static_cast<int64_t>(fx_.length);
    const auto address = static_cast<uint32_t>(fx_.base + position * 2);
    if (updated > length)
    {
        const auto after = static_cast<int32_t>(updated - length);
        const auto before = static_cast<int32_t>(length - position);
        list.SetBuffer(0, 0, static_cast<uint16_t>(buffer), static_cast<uint32_t>(before) << 1);
        list.SaveBuffer(address);
        list.SetBuffer(0, 0, static_cast<uint16_t>(buffer + (before << 1)), static_cast<uint32_t>(after) << 1);
        list.SaveBuffer(fx_.base);
        list.SetBuffer(0, 0, 0, static_cast<uint32_t>(count) << 1);
    }
    else
    {
        list.SetBuffer(0, 0, static_cast<uint16_t>(buffer), static_cast<uint32_t>(count) << 1);
        list.SaveBuffer(address);
    }
}

void AlSynth::LoadFxOutput(AudioList& list, Delay& delay, int buffer, int count)
{
    if (!delay.has_resampler)
    {
        LoadFxBuffer(list, fx_.input - static_cast<int32_t>(delay.output), buffer, count);

        return;
    }

    constexpr double kRange = 2.0;
    const int32_t length = static_cast<int32_t>(delay.output - delay.input);

    // The chorus's modulation: a triangle from -1 to 1 at the chorus's rate.
    delay.mod_value += delay.mod_increment * static_cast<float>(count);
    delay.mod_value = delay.mod_value > kRange ? static_cast<float>(delay.mod_value - kRange * 2) : delay.mod_value;
    float wave = delay.mod_value < 0 ? -delay.mod_value : delay.mod_value;
    wave = static_cast<float>(wave - kRange / 2);
    float delta = delay.mod_gain * wave;

    delta /= static_cast<float>(length);
    delta = static_cast<float>(static_cast<int32_t>(delta * kUnityPitch));
    delta = delta / kUnityPitch;
    const float ratio = static_cast<float>(1.0 - delta);

    const float exact = delay.resampler_delta + ratio * static_cast<float>(count);
    const auto samples = static_cast<int32_t>(exact);
    delay.resampler_delta = exact - static_cast<float>(samples);

    const int64_t out_place = fx_.input - (static_cast<int64_t>(delay.output) - delay.mod_delta);
    const int align = static_cast<int>((fx_.base + out_place * 2) & 7) >> 1;
    LoadFxBuffer(list, out_place - align, kAlTemp2, samples + align);
    const auto increment = static_cast<int32_t>(ratio * kUnityPitch);
    list.SetBuffer(0, static_cast<uint16_t>(kAlTemp2 + (align << 1)), static_cast<uint16_t>(buffer),
                   static_cast<uint32_t>(count) << 1);
    list.Resample(delay.resampler_first ? 1 : 0, static_cast<uint32_t>(increment), delay.resampler_state);
    delay.resampler_first = false;
    delay.mod_delta += samples - count;
}

void AlSynth::FilterBuffer(AudioList& list, LowPass& low_pass, int buffer, int count)
{
    list.SetBuffer(0, static_cast<uint16_t>(buffer), static_cast<uint16_t>(buffer), static_cast<uint32_t>(count) << 1);
    list.LoadAdpcm(32, low_pass.coefficients_address);
    list.PoleFilter(low_pass.first ? 1 : 0, static_cast<uint16_t>(low_pass.gain), low_pass.state);
    low_pass.first = false;
}

} // namespace ntdlib::n64
