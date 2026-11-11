// SPDX-License-Identifier: MIT

// The audio lists of libultra's synthesizer for the compact microcode.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "al_synth_constants.h"
#include "equal_power.h"
#include "ntdlib/n64/al_synth.h"

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

// The compact form's constants, from the code at 0x800490E0 and 0x80050DE0 in Pokémon Stadium.
constexpr int kCompactChunkBytes = kAlCompactChunkSamples * 2;
constexpr int kCompactDecoderOut = kCompactChunkBytes;
constexpr int kCompactFxTemp = kCompactChunkBytes * 2;
constexpr uint32_t kCompactMainLeft = 0x4E0;
constexpr uint32_t kCompactMainRight = 0x650;
constexpr uint32_t kCompactAuxLeft = 0x7C0;
constexpr uint32_t kCompactAuxRight = 0x930;
constexpr uint32_t kOpLoad = 0x04000000;
constexpr uint32_t kOpSave = 0x06000000;
constexpr uint32_t kOpMix = 0x0C000000;
constexpr uint32_t kOpMove = 0x0A000000;
constexpr uint32_t kAddressMask = 0xFFFFFF;
constexpr uint32_t kDmemMask = 0xFFF;

// The compact form's rate: a straight line in 8-sample steps. The game's code is at 0x80047910 in Pokémon Stadium.
// Most games have the arithmetic in double precision. Mario Tennis has it in single precision, at 0x802F26B8.
template <typename Real>
int16_t CompactRate(Real volume, Real target, int32_t count, uint16_t& rate_low, bool holds_without_time)
{
    if (count == 0)
    {
        if (volume <= target)
        {
            rate_low = 0xFFFF;

            return 0x7FFF;
        }

        rate_low = 0;

        return holds_without_time ? int16_t{0} : int16_t{-0x8000};
    }

    constexpr auto kStep = static_cast<Real>(8.0);
    constexpr auto kLowScale = static_cast<Real>(65535.0);
    const Real inverse = static_cast<Real>(1.0) / static_cast<Real>(count);
    if (target < static_cast<Real>(1.0))
    {
        target = static_cast<Real>(1.0);
    }

    if (volume <= static_cast<Real>(0.0))
    {
        volume = static_cast<Real>(1.0);
    }

    const Real step = (target - volume) * inverse * kStep;
    const auto whole = static_cast<int16_t>(static_cast<int32_t>(step));
    const Real above = step - static_cast<Real>(whole) + static_cast<Real>(1.0);
    const auto carry = static_cast<int16_t>(static_cast<int32_t>(above));
    rate_low = static_cast<uint16_t>(static_cast<uint32_t>(kLowScale * (above - static_cast<Real>(carry))));

    return static_cast<int16_t>(whole - 1 + carry);
}

// Where a compact form's ramp has reached. The game's code is at 0x80047A98 in Pokémon Stadium.
int16_t CompactRamp(int16_t volume, int32_t samples, int16_t rate_high, uint16_t rate_low)
{
    const int32_t steps = samples >> 3;
    if (steps == 0)
    {
        return volume;
    }

    const auto low = static_cast<int32_t>(static_cast<uint32_t>(rate_low) * static_cast<uint32_t>(steps));

    return static_cast<int16_t>(volume + (low >> 16) + rate_high * steps);
}

// A count of samples, rounded to whole chunks of the compact form.
int32_t RoundToCompactChunk(int32_t samples)
{
    return (samples + kAlCompactChunkSamples / 2) / kAlCompactChunkSamples * kAlCompactChunkSamples;
}

} // namespace

void AlSynth::BuildCompactCommands(AudioList& list, AudioMemory& memory, uint32_t output, int samples)
{
    constexpr uint32_t kOutputBytes = kCompactChunkBytes * 2;
    uint32_t place = output;
    for (int left = samples; left > 0; left -= kAlCompactChunkSamples)
    {
        list.ClearBuffer(kCompactMainLeft, kOutputBytes);
        if (fx_.present)
        {
            PullCompactFx(list, memory, current_samples_);
        }
        else
        {
            PullCompactAux(list, memory, current_samples_);
        }

        list.MarkChunkEnd();
        list.commands.push_back({kOpMix | 0x7FFF, (kCompactAuxLeft << 16) | kCompactMainLeft});
        list.commands.push_back({kOpMix | 0x7FFF, (kCompactAuxRight << 16) | kCompactMainRight});

        // The game's code doesn't write this command's second word. The microcode doesn't read it.
        list.commands.push_back({static_cast<uint32_t>(kAudioInterleave) << 24, 0});
        list.commands.push_back({kOpSave | (kOutputBytes << 12), place});
        place += kOutputBytes;
        current_samples_ += kAlCompactChunkSamples;
    }

    // The voices that are done become free.
    while (!lame_list_.empty())
    {
        const int physical = lame_list_.front();
        lame_list_.erase(lame_list_.begin());
        free_list_.insert(free_list_.begin(), physical);
    }
}

void AlSynth::PullCompactAux(AudioList& list, AudioMemory& memory, int32_t offset)
{
    list.ClearBuffer(kCompactAuxLeft, kCompactChunkBytes * 2);
    for (int physical = 0; physical < static_cast<int>(physical_.size()); physical++)
    {
        list.MarkVoice(physical_[static_cast<std::size_t>(physical)].tag);
        PullCompactMixer(list, memory, physical, offset);
    }

    list.MarkVoicesEnd();
}

void AlSynth::PullCompactMixer(AudioList& list, AudioMemory& memory, int physical, int32_t offset)
{
    PhysicalVoiceState& voice = physical_[static_cast<std::size_t>(physical)];
    int16_t in = 0;
    int32_t this_offset = offset;
    int left = kAlCompactChunkSamples;
    while (!voice.updates.empty())
    {
        Param& update = voice.updates.front();
        const int32_t last_offset = this_offset;
        const int32_t samples = CompactUpdateSamples(update.delta, last_offset, this_offset);
        if (samples > left)
        {
            break;
        }

        switch (update.type)
        {
        case ParamType::kStartVoiceAlt:
            StartCompactMixer(memory, voice, update);
            break;

        case ParamType::kSetFxMix:
        case ParamType::kSetPan:
        case ParamType::kSetVolume:
            PullCompactSubFrame(list, voice, in, samples);
            ChangeCompactMixer(voice, update, samples);
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
            PullCompactSubFrame(list, voice, in, samples);
            ResetFilters(voice);
            voice.segment_end = 0;
            break;

        case ParamType::kFreeVoice:
            voice.offset = 0;
            MoveToLame(update.physical);
            break;

        case ParamType::kSetPitch:
            PullCompactSubFrame(list, voice, in, samples);
            voice.ratio = update.pitch;
            break;
        }

        left -= samples;
        voice.updates.pop_front();
        free_updates_++;
    }

    PullCompactSubFrame(list, voice, in, left);
    if (voice.mixer_delta > voice.segment_end)
    {
        voice.mixer_delta = voice.segment_end;
    }
}

// The samples from the last update to an update at `delta`, in whole chunks, and the update's offset. The later compact
// library rounds the offset. The earlier one rounds the count of samples.
int32_t AlSynth::CompactUpdateSamples(int32_t delta, int32_t last_offset, int32_t& this_offset) const
{
    if (later_compact_)
    {
        this_offset = RoundToCompactChunk(delta);

        return this_offset - last_offset;
    }

    this_offset = delta;
    const int32_t samples = RoundToCompactChunk(this_offset - last_offset);
    if (samples == 0)
    {
        this_offset = last_offset;
    }

    return samples;
}

// The target level of each side, from the volume and the pan of a voice.
void AlSynth::SetCompactTargets(PhysicalVoiceState& voice)
{
    voice.target_left = static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(voice.pan)]) >> 15);
    voice.target_right =
        static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(127 - voice.pan)]) >> 15);
}

// Starts a voice with its wave, volume, pan, effect mix and pitch.
void AlSynth::StartCompactMixer(AudioMemory& memory, PhysicalVoiceState& voice, const Param& update)
{
    if (update.unity)
    {
        voice.unity_pitch = true;
    }

    ApplySetWave(memory, voice, update.wave);
    voice.playing = true;
    voice.mixer_first = true;
    voice.mixer_delta = 0;
    voice.segment_end = RoundToCompactChunk(update.samples);
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
        SetCompactTargets(voice);
        voice.current_left = voice.target_left;
        voice.current_right = voice.target_right;
    }

    voice.ratio = update.pitch;
}

// A change of a voice's volume, pan or effect mix. The levels first move to where the ramp stands at the change.
void AlSynth::ChangeCompactMixer(PhysicalVoiceState& voice, const Param& update, int32_t samples)
{
    if (voice.mixer_delta >= voice.segment_end || (later_compact_ && samples == 0))
    {
        SetCompactTargets(voice);
        voice.mixer_delta = voice.segment_end;
        voice.current_left = voice.target_left;
        voice.current_right = voice.target_right;
    }
    else
    {
        voice.current_left =
            CompactRamp(voice.current_left, voice.mixer_delta, voice.rate_high_left, voice.rate_low_left);
        voice.current_right =
            CompactRamp(voice.current_right, voice.mixer_delta, voice.rate_high_right, voice.rate_low_right);
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
        voice.segment_end = RoundToCompactChunk(update.more);
    }
    else
    {
        voice.dry = kEqualPower[static_cast<std::size_t>(update.data)];
        voice.wet = kEqualPower[static_cast<std::size_t>(127 - update.data)];
    }

    voice.mixer_first = true;
}

void AlSynth::PullCompactSubFrame(AudioList& list, PhysicalVoiceState& voice, int16_t& in, int count)
{
    if (!voice.playing || count == 0)
    {
        return;
    }

    PullCompactResampler(list, voice, in);
    if (voice.mixer_first)
    {
        constexpr uint32_t kSetTargetLeft = 0x09000000;
        constexpr uint32_t kSetCurrentLeft = 0x09060000;
        constexpr uint32_t kSetTargetRight = 0x09040000;
        constexpr uint32_t kEnvMixerInit = 0x03010000;
        const auto pair = [](int16_t high, uint16_t low)
        {
            return (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16) | low;
        };

        voice.mixer_first = false;
        voice.target_left =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(voice.pan)]) >> 15);
        voice.rate_high_left = single_precision_ramps_
                                   ? CompactRate<float>(voice.current_left, voice.target_left, voice.segment_end,
                                                        voice.rate_low_left, later_compact_)
                                   : CompactRate<double>(voice.current_left, voice.target_left, voice.segment_end,
                                                         voice.rate_low_left, later_compact_);
        voice.target_right =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(127 - voice.pan)]) >> 15);
        voice.rate_high_right = single_precision_ramps_
                                    ? CompactRate<float>(voice.current_right, voice.target_right, voice.segment_end,
                                                         voice.rate_low_right, later_compact_)
                                    : CompactRate<double>(voice.current_right, voice.target_right, voice.segment_end,
                                                          voice.rate_low_right, later_compact_);
        list.commands.push_back({kSetTargetLeft | static_cast<uint16_t>(voice.target_left),
                                 pair(voice.rate_high_left, voice.rate_low_left)});
        list.commands.push_back({kSetCurrentLeft | static_cast<uint16_t>(voice.current_left),
                                 pair(voice.dry, static_cast<uint16_t>(voice.wet))});
        list.commands.push_back({kSetTargetRight | static_cast<uint16_t>(voice.target_right),
                                 pair(voice.rate_high_right, voice.rate_low_right)});
        list.commands.push_back({kEnvMixerInit | static_cast<uint16_t>(voice.current_right), voice.mixer_state});
    }
    else
    {
        list.EnvMixer(0, voice.mixer_state);
    }

    in = static_cast<int16_t>(in + kCompactChunkBytes);
    voice.mixer_delta += kAlCompactChunkSamples;
}

void AlSynth::PullCompactResampler(AudioList& list, PhysicalVoiceState& voice, int16_t in)
{
    int16_t out = kCompactDecoderOut;
    if (voice.unity_pitch)
    {
        PullCompactAdpcm(list, voice, out, kAlCompactChunkSamples);
        list.commands.push_back({kOpMove | (static_cast<uint32_t>(out) & kAddressMask),
                                 (static_cast<uint32_t>(static_cast<uint16_t>(in)) << 16) | kCompactChunkBytes});

        return;
    }

    if (voice.ratio > kMaxRatio)
    {
        voice.ratio = static_cast<float>(kMaxRatio);
    }

    voice.ratio = static_cast<float>(static_cast<int32_t>(voice.ratio * kUnityPitch));
    voice.ratio = voice.ratio / kUnityPitch;

    const float exact = voice.delta + voice.ratio * static_cast<float>(kAlCompactChunkSamples);
    const auto count = static_cast<int32_t>(exact);
    voice.delta = exact - static_cast<float>(count);
    PullCompactAdpcm(list, voice, out, count);

    const auto increment = static_cast<int32_t>(voice.ratio * kUnityPitch);
    list.commands.push_back({static_cast<uint32_t>(kAudioResample) << 24 | (voice.resampler_state & kAddressMask),
                             (voice.resampler_first ? 1U << 30 : 0U) |
                                 ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                                 ((static_cast<uint32_t>(out) & kDmemMask) << 2)});
    voice.resampler_first = false;
}

void AlSynth::CompactDecodeChunk(AudioList& list, PhysicalVoiceState& voice, int tsam, int bytes, int16_t out,
                                 uint32_t flags)
{
    uint32_t align = 0;
    if (bytes > 0)
    {
        const auto location = static_cast<uint32_t>(voice.memory_in);
        align = location & 7;
        const uint32_t total = static_cast<uint32_t>(bytes) + align;
        list.commands.push_back({kOpLoad | (((total - (total & 7) + 8) & kDmemMask) << 12), location - align});
    }

    if ((flags & kAudioFlagLoop) != 0)
    {
        list.SetLoop(voice.decoder_loop_state);
    }

    list.commands.push_back({static_cast<uint32_t>(kAudioAdpcm) << 24 | (voice.decoder_state & kAddressMask),
                             (flags << 28) | ((static_cast<uint32_t>(tsam * 2) & kDmemMask) << 16) |
                                 ((align & 0xF) << 12) | (static_cast<uint32_t>(out) & kDmemMask)});
    voice.decoder_first = false;
}

void AlSynth::PullCompactAdpcm(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count)
{
    if (count == 0)
    {
        return;
    }

    list.LoadAdpcm(static_cast<uint32_t>(voice.bytes_book), voice.wave.book);

    const bool looped = count + voice.sample > static_cast<int32_t>(voice.loop_end) && voice.loop_count != 0;
    int samples = looped ? static_cast<int>(voice.loop_end) - voice.sample : count;
    const int left_in_frame = voice.last_sample != 0 ? kAdpcmFrameSamples - voice.last_sample : 0;
    int tsam = std::max(samples - left_in_frame, 0);
    int frames = (tsam + kAdpcmFrameSamples - 1) >> kAdpcmFrameShift;
    int bytes = frames * kAdpcmFrameBytes;
    const uint32_t first_flag = voice.decoder_first ? 1 : 0;

    if (looped)
    {
        CompactDecodeChunk(list, voice, tsam, bytes, out, first_flag);
        out = static_cast<int16_t>(out + (voice.last_sample != 0 ? voice.last_sample << 1 : kAdpcmFrameSamples << 1));
        PullCompactAdpcmLoops(list, voice, out, count, samples, frames);

        return;
    }

    samples = frames << kAdpcmFrameShift;
    int over_flow =
        static_cast<int>(voice.memory_in + bytes - (static_cast<int64_t>(voice.wave.base) + voice.wave.length));
    over_flow = std::max(over_flow, 0);
    int over = (over_flow / kAdpcmFrameBytes) << kAdpcmFrameShift;
    over = std::min(over, samples + left_in_frame);

    bool decoded = false;
    if (over - (over & 0xF) < count)
    {
        decoded = true;
        CompactDecodeChunk(list, voice, samples - over, bytes - over_flow, out, first_flag);
        out = static_cast<int16_t>(out + (voice.last_sample != 0 ? voice.last_sample << 1 : kAdpcmFrameSamples << 1));
        voice.last_sample = (count + voice.last_sample) & 0xF;
        voice.sample += count;
        voice.memory_in += kAdpcmFrameBytes * frames;
    }
    else
    {
        voice.last_sample = 0;
        voice.memory_in += kAdpcmFrameBytes * frames;
        if (voice.voice >= 0)
        {
            voices_[static_cast<std::size_t>(voice.voice)].finished = true;
        }
    }

    if (over != 0)
    {
        voice.last_sample = 0;
        const int start_zero = decoded ? (left_in_frame + samples - over) << 1 : 0;
        list.ClearBuffer(static_cast<uint32_t>(start_zero + out), static_cast<uint32_t>(over) << 1);
    }
}

// The rest of a chunk that runs past the loop's end: the passes of the loop from its start.
void AlSynth::PullCompactAdpcmLoops(AudioList& list, PhysicalVoiceState& voice, int16_t out, int count, int samples,
                                    int frames)
{
    voice.last_sample = static_cast<int32_t>(voice.loop_start & 0xF);
    voice.memory_in = static_cast<int64_t>(voice.wave.base) +
                      kAdpcmFrameBytes * (static_cast<int64_t>(voice.loop_start >> kAdpcmFrameShift) + 1);
    voice.sample = static_cast<int32_t>(voice.loop_start);

    int buffer_end = out;
    int left = count;
    while (samples < left)
    {
        const int place = (buffer_end + ((frames + 1) << (kAdpcmFrameShift + 1)) + kAdpcmFrameSamples) & ~0x1F;
        left -= samples;
        buffer_end += samples << 1;
        if (voice.loop_count != kForever && voice.loop_count != 0)
        {
            voice.loop_count--;
        }

        samples = std::min(left, static_cast<int>(voice.loop_end - voice.loop_start));
        const int tsam = std::max(samples + voice.last_sample - kAdpcmFrameSamples, 0);
        frames = (tsam + kAdpcmFrameSamples - 1) >> kAdpcmFrameShift;
        const int bytes = frames * kAdpcmFrameBytes;
        CompactDecodeChunk(list, voice, tsam, bytes, static_cast<int16_t>(place),
                           (voice.decoder_first ? 1U : 0U) | kAudioFlagLoop);
        list.commands.push_back(
            {kOpMove | (static_cast<uint32_t>(place + (voice.last_sample << 1)) & kAddressMask),
             (static_cast<uint32_t>(buffer_end) << 16) | (static_cast<uint32_t>(samples << 1) & 0xFFFF)});
    }

    voice.last_sample = (left + voice.last_sample) & 0xF;
    voice.sample += left;
    voice.memory_in += kAdpcmFrameBytes * frames;
}

void AlSynth::PullCompactFx(AudioList& list, AudioMemory& memory, int32_t offset)
{
    PullCompactAux(list, memory, offset);
    list.commands.push_back({kOpMix | 0xDA83, (kCompactAuxLeft << 16) | kCompactAuxLeft});
    list.commands.push_back({kOpMix | 0x5A82, (kCompactAuxRight << 16) | kCompactAuxLeft});

    int buffer1 = 0;
    int buffer2 = kCompactChunkBytes;
    CompactSaveFx(list, fx_.input, kCompactAuxLeft);
    list.ClearBuffer(kCompactAuxRight, kCompactChunkBytes);

    const auto mix = [&](int16_t gain, int in, uint32_t out)
    {
        list.commands.push_back(
            {kOpMix | static_cast<uint16_t>(gain), (static_cast<uint32_t>(in) << 16) | (out & 0xFFFF)});
    };

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
            CompactLoadFx(list, in_place, buffer1, kAlCompactChunkSamples);
        }

        CompactLoadFxOutput(list, delay, buffer2);
        if (delay.feed_forward != 0)
        {
            mix(delay.feed_forward, buffer1, static_cast<uint32_t>(buffer2));
            if (!delay.has_resampler && !delay.has_low_pass)
            {
                CompactSaveFx(list, out_place, buffer2);
            }
        }

        if (delay.feedback != 0)
        {
            mix(delay.feedback, buffer2, static_cast<uint32_t>(buffer1));
            CompactSaveFx(list, in_place, buffer1);
        }

        if (delay.has_low_pass)
        {
            CompactFilter(list, delay.low_pass, buffer2);
        }

        if (!delay.has_resampler)
        {
            CompactSaveFx(list, out_place, buffer2);
        }

        if (delay.gain != 0)
        {
            mix(delay.gain, buffer2, kCompactAuxRight);
        }

        // The game's code takes this place with the output added, not taken away.
        previous_out = fx_.input + static_cast<int64_t>(static_cast<int32_t>(delay.output));
    }

    fx_.input += kAlCompactChunkSamples;
    if (fx_.input > static_cast<int32_t>(fx_.length))
    {
        fx_.input -= static_cast<int32_t>(fx_.length);
    }

    list.commands.push_back({kOpMove | kCompactAuxRight, (kCompactAuxLeft << 16) | kCompactChunkBytes});
}

void AlSynth::CompactLoadFx(AudioList& list, int64_t position, int buffer, int count)
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
        const auto before = static_cast<uint32_t>(length - position) << 1;
        const auto after = static_cast<uint32_t>(updated - length) << 1;
        list.commands.push_back(
            {kOpLoad | ((before & kDmemMask) << 12) | (static_cast<uint32_t>(buffer) & kDmemMask), address});
        list.commands.push_back(
            {kOpLoad | ((after & kDmemMask) << 12) | ((static_cast<uint32_t>(buffer) + before) & kDmemMask), fx_.base});
    }
    else
    {
        list.commands.push_back({kOpLoad | ((static_cast<uint32_t>(count << 1) & kDmemMask) << 12) |
                                     (static_cast<uint32_t>(buffer) & kDmemMask),
                                 address});
    }
}

void AlSynth::CompactSaveFx(AudioList& list, int64_t position, int buffer)
{
    if (position < 0)
    {
        position += fx_.length;
    }

    const int64_t updated = position + kAlCompactChunkSamples;
    const auto length = static_cast<int64_t>(fx_.length);
    const auto address = static_cast<uint32_t>(fx_.base + position * 2);
    if (updated > length)
    {
        const auto before = static_cast<uint32_t>(length - position) << 1;
        const auto after = static_cast<uint32_t>(updated - length) << 1;
        list.commands.push_back(
            {kOpSave | ((before & kDmemMask) << 12) | (static_cast<uint32_t>(buffer) & kDmemMask), address});
        list.commands.push_back(
            {kOpSave | ((after & kDmemMask) << 12) | ((static_cast<uint32_t>(buffer) + before) & kDmemMask), fx_.base});
    }
    else
    {
        list.commands.push_back(
            {kOpSave | (kCompactChunkBytes << 12) | (static_cast<uint32_t>(buffer) & kDmemMask), address});
    }
}

void AlSynth::CompactLoadFxOutput(AudioList& list, Delay& delay, int buffer)
{
    if (!delay.has_resampler)
    {
        CompactLoadFx(list, fx_.input - static_cast<int32_t>(delay.output), buffer, kAlCompactChunkSamples);

        return;
    }

    constexpr double kRange = 2.0;
    const int32_t length = static_cast<int32_t>(delay.output - delay.input);

    // The chorus's modulation: a triangle from -1 to 1 at the chorus's rate.
    delay.mod_value += delay.mod_increment * static_cast<float>(kAlCompactChunkSamples);
    delay.mod_value = delay.mod_value > kRange ? static_cast<float>(delay.mod_value - kRange * 2) : delay.mod_value;
    float wave = delay.mod_value < 0 ? -delay.mod_value : delay.mod_value;
    wave = static_cast<float>(wave - kRange / 2);
    float delta = delay.mod_gain * wave;

    delta /= static_cast<float>(length);
    delta = static_cast<float>(static_cast<int32_t>(delta * kUnityPitch));
    delta = delta / kUnityPitch;
    const float ratio = static_cast<float>(1.0 - delta);

    const float exact = delay.resampler_delta + ratio * static_cast<float>(kAlCompactChunkSamples);
    const auto samples = static_cast<int32_t>(exact);
    delay.resampler_delta = exact - static_cast<float>(samples);

    const int64_t out_place = fx_.input - (static_cast<int64_t>(delay.output) - delay.mod_delta);
    const int align = static_cast<int>((fx_.base + out_place * 2) & 7) >> 1;
    CompactLoadFx(list, out_place - align, kCompactFxTemp, samples + align);
    const auto increment = static_cast<int32_t>(ratio * kUnityPitch);
    list.commands.push_back({static_cast<uint32_t>(kAudioResample) << 24 | (delay.resampler_state & kAddressMask),
                             (delay.resampler_first ? 1U << 30 : 0U) |
                                 ((static_cast<uint32_t>(increment) & 0xFFFF) << 14) |
                                 ((static_cast<uint32_t>((align << 1) + kCompactFxTemp) & kDmemMask) << 2) |
                                 ((static_cast<uint32_t>(buffer) >> 8) & 3)});
    delay.resampler_first = false;
    delay.mod_delta += samples - kAlCompactChunkSamples;
}

void AlSynth::CompactFilter(AudioList& list, LowPass& low_pass, int buffer)
{
    list.LoadAdpcm(32, low_pass.coefficients_address);
    list.commands.push_back({static_cast<uint32_t>(kAudioPoleFilter) << 24 | (low_pass.first ? 1U << 16 : 0U) |
                                 static_cast<uint16_t>(low_pass.gain),
                             ((static_cast<uint32_t>(buffer) >> 8) << 24) | (low_pass.state & kAddressMask)});
    low_pass.first = false;
}

} // namespace ntdlib::n64
