// SPDX-License-Identifier: MIT

// The voices of the sound unit's enhanced mixing mode (see sound.h): the waves through a band-limited interpolator, the
// rectangular waves and the noise through band-limited steps, and the mix in floating point.

#include "sound_internal.h"

namespace ntdlib::ntr
{

void SoundUnit::StartVoice(int index)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    Voice fresh;
    fresh.steps = std::move(voice.steps);
    fresh.level = voice.level;
    fresh.idle = voice.idle;
    fresh.settled = voice.settled;
    fresh.run_gains = std::move(voice.run_gains);
    voice = std::move(fresh);

    const ChannelFormat format = FormatOf(Control(index));
    voice.psg = format == ChannelFormat::kPsg;
    voice.sounding = !voice.psg;
    voice.fresh = true;
    voice.delay = voice.psg ? kPsgDelay : format == ChannelFormat::kImaAdpcm ? kAdpcmDelay : kPcmDelay;
    if (!voice.psg)
    {
        voice.input.assign(static_cast<std::size_t>(kZeros), 0.0f);
        voice.first = -kZeros;
    }
}

void SoundUnit::StopVoice(int index, uint64_t at, uint64_t now)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    if (!voice.sounding || voice.stopped)
    {
        return;
    }

    // The samples after the one that plays go silent, or hold it with the hold flag. The voice rings out from there.
    voice.place = VoicePlace(index, at, now);
    voice.period = Period(index);
    voice.stopped = true;
    voice.stopped_at = at;
    voice.end = std::min(voice.end, static_cast<int64_t>(std::floor(voice.place)) + 1);
    voice.holds = (Control(index) & kHold) != 0;
    voice.held = channels_[static_cast<std::size_t>(index)].sample;
    for (int64_t sample = std::max(voice.end, voice.first); sample < voice.decoded; sample++)
    {
        voice.input[static_cast<std::size_t>(sample - voice.first)] = voice.holds ? voice.held : 0.0f;
    }
}

void SoundUnit::StepLevel(int index, uint64_t tick)
{
    // A rectangular wave's or noise's level: the channel's sample while it plays, or held.
    EnhancedState& enhanced = *enhanced_;
    Voice& voice = enhanced.voices[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    const bool held = !channel.playing && !channel.busy && (Control(index) & kHold) != 0;
    const float level = voice.psg && (channel.playing || held) ? static_cast<float>(channel.sample) : 0.0f;
    if (level == voice.level)
    {
        return;
    }

    if (!enhanced.stale)
    {
        // The steps start with the run's first sample, at the level before the change.
        if (!voice.steps)
        {
            voice.steps.emplace(kEnhancedFrameTicks, kEnhancedFrameSamples);
            voice.steps->Restart(enhanced.run_first, voice.level);
            voice.run_gains.assign(enhanced.run_waves.size(), {});
        }
        else if (voice.idle)
        {
            voice.steps->Restart(enhanced.run_first, voice.level);
            voice.idle = false;
        }
        voice.steps->Step(tick, level - voice.level);
        voice.settled = tick * kEnhancedFrameSamples / kEnhancedFrameTicks + 1 + 2 * kDelay;
    }
    voice.level = level;
}

void SoundUnit::Overflowed(int index, uint64_t tick, uint32_t late)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    voice.overflows++;
    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.busy && !channel.playing)
    {
        // A one-shot wave's last sample has ended.
        StopVoice(index, (tick - late) * kEnhancedFrameSamples, tick * kEnhancedFrameSamples);
    }
    StepLevel(index, tick - late);
}

uint32_t SoundUnit::Period(int index) const
{
    return kTimerOverflow - ChannelWord(index, 0x8) % 0x10000;
}

double SoundUnit::VoicePlace(int index, uint64_t at, uint64_t now) const
{
    // The sample that plays, and the share of the timer's period since it started. The channel's timer gives the
    // place at `now`.
    const Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    if (voice.stopped)
    {
        return voice.place +
               (static_cast<double>(at) - static_cast<double>(voice.stopped_at)) / kEnhancedFrameSamples / voice.period;
    }

    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t period = Period(index);
    const double since = static_cast<double>(channel.counter) - static_cast<double>(kTimerOverflow - period) -
                         (static_cast<double>(now) - static_cast<double>(at)) / kEnhancedFrameSamples;
    return static_cast<double>(static_cast<int64_t>(voice.overflows) - voice.delay) + since / period;
}

float SoundUnit::WaveAt(int index, double place)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    const uint32_t period = voice.stopped ? voice.period : Period(index);
    if (voice.kernel == nullptr || period != voice.kernel_period)
    {
        voice.kernel =
            &wave::SincKernel::ForStep(static_cast<double>(kEnhancedFrameTicks) / kEnhancedFrameSamples / period);
        voice.kernel_period = period;
    }
    const wave::SincKernel& kernel = *voice.kernel;
    const int64_t sample = static_cast<int64_t>(std::floor(place));
    const auto fraction = static_cast<uint32_t>(std::min(65535.0, (place - static_cast<double>(sample)) * 65536.0));
    const int64_t half = kernel.HalfWidth();

    // A place far past the samples decoded, after a run without output, skips the samples between.
    const int64_t keep = sample - kKept;
    if (voice.decoded < keep)
    {
        while (voice.decoded < keep)
        {
            DecodeNext(index);
        }
        voice.input.clear();
        voice.first = voice.decoded;
    }
    while (voice.decoded <= sample + half)
    {
        voice.input.push_back(DecodeNext(index));
    }
    if (keep - voice.first > kMostKept)
    {
        voice.input.erase(voice.input.begin(), voice.input.begin() + static_cast<std::ptrdiff_t>(keep - voice.first));
        voice.first = keep;
    }

    const int64_t from = sample - half + 1;
    if (from < voice.first)
    {
        return 0.0f;
    }

    return kernel.At(&voice.input[static_cast<std::size_t>(from - voice.first)], fraction);
}

float SoundUnit::DecodeNext(int index)
{
    // The wave's samples in the order the channel plays them. StepWave and WaveSample give the same samples.
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    const int64_t sample = voice.decoded++;
    if (sample < 0)
    {
        return 0.0f;
    }
    if (sample >= voice.end)
    {
        return voice.holds ? voice.held : 0.0f;
    }

    const uint32_t control = Control(index);
    const ChannelFormat format = FormatOf(control);
    const uint32_t loop_words = ChannelWord(index, 0x8) >> 16;
    const uint32_t words = loop_words + ChannelWord(index, 0xC);
    if (format == ChannelFormat::kPsg || words < kMinimumWords)
    {
        return 0.0f;
    }

    const uint32_t end = SamplesIn(format, words);
    const uint32_t loop_start = SamplesIn(format, loop_words);
    const uint32_t repeat = RepeatOf(control);
    uint32_t at = voice.wave_sample;
    if (at >= end && repeat == 1)
    {
        at = loop_start;
        voice.adpcm = voice.loop_adpcm;
    }
    else if (at >= end && repeat == 2)
    {
        return (control & kHold) ? voice.last : 0.0f;
    }

    const uint32_t address = ChannelWord(index, 0x4);
    float value = 0.0f;
    switch (format)
    {
    case ChannelFormat::kPcm8:
        value = static_cast<float>(static_cast<int8_t>(Byte(address + at))) * 256.0f;
        break;
    case ChannelFormat::kPcm16:
        value = static_cast<float>(
            static_cast<int16_t>(Byte(address + 2 * at) | static_cast<uint32_t>(Byte(address + 2 * at + 1)) << 8));
        break;
    case ChannelFormat::kImaAdpcm:
        {
            if (at == 0)
            {
                voice.adpcm = AdpcmHeader(MemoryWord(address));
            }
            if (at == loop_start)
            {
                voice.loop_adpcm = voice.adpcm;
            }
            const uint32_t word = MemoryWord(address + 4 + (at / 8) * 4);
            voice.adpcm = AdpcmStep(voice.adpcm, static_cast<uint8_t>((word >> (4 * (at % 8))) & 0x0F));
            value = voice.adpcm.sample;
            break;
        }
    case ChannelFormat::kPsg:
        break;
    }
    voice.wave_sample = at + 1;
    voice.last = value;

    return value;
}

std::array<float, 2> SoundUnit::Gains(int index) const
{
    // The volume, the divider and the pan of Sides, on the scale of the mixer's 16.8 bits.
    const uint32_t control = Control(index);
    const float scale =
        static_cast<float>(Factor(control & 0x7F)) / static_cast<float>(64 << kDivisorShifts[(control >> 8) & 3]);
    const int32_t pan = Factor((control >> 16) & 0x7F);
    return {scale * static_cast<float>(128 - pan), scale * static_cast<float>(pan)};
}

void SoundUnit::BeginEnhanced()
{
    EnhancedState& enhanced = *enhanced_;
    enhanced.run_first = enhanced.samples;
    enhanced.run_waves.clear();
    enhanced.run_master.clear();
    enhanced.run_meters.clear();
    for (Voice& voice : enhanced.voices)
    {
        voice.run_gains.clear();
    }
    if (!enhanced.stale)
    {
        return;
    }

    // After a run without output, the steps start again at the levels, the delay starts empty, and the gains take
    // their values at once. A voice that the run gave a level gets steps then: a level can hold without a change.
    for (Voice& voice : enhanced.voices)
    {
        if (!voice.steps && voice.level != 0.0f)
        {
            voice.steps.emplace(kEnhancedFrameTicks, kEnhancedFrameSamples);
        }
        if (voice.steps)
        {
            voice.steps->Restart(enhanced.samples, voice.level);
            voice.idle = false;
            voice.settled = enhanced.samples;
        }
        voice.fresh = true;
    }
    enhanced.master_fresh = true;
    for (std::vector<float>& line : enhanced.delayed)
    {
        line.assign(kDelay, 0.0f);
    }
    enhanced.stale = false;
}

void SoundUnit::MixDue(bool output)
{
    // The samples whose times are before the present tick. A run's samples are those whose times are in it.
    EnhancedState& enhanced = *enhanced_;
    while (enhanced.samples * kEnhancedFrameTicks < enhanced.ticks * kEnhancedFrameSamples)
    {
        if (output)
        {
            MixEnhanced();
        }
        enhanced.samples++;
    }
}

void SoundUnit::MixEnhanced()
{
    EnhancedState& enhanced = *enhanced_;
    const uint64_t at = enhanced.samples * kEnhancedFrameTicks;
    const uint64_t now = enhanced.ticks * kEnhancedFrameSamples;
    const uint32_t sound_control = Word(kSoundControl);
    const auto ramp = [](std::array<float, 2>& gains, std::array<float, 2>& targets, std::array<float, 2>& slopes,
                         uint32_t& left, bool& fresh, const std::array<float, 2>& target)
    {
        if (fresh)
        {
            gains = target;
            targets = target;
            left = 0;
            fresh = false;
        }
        else if (target != targets)
        {
            targets = target;
            for (std::size_t side = 0; side < 2; side++)
            {
                slopes[side] = (target[side] - gains[side]) / kEnhancedFrameSamples;
            }
            left = kEnhancedFrameSamples;
        }
        if (left > 0 && --left == 0)
        {
            gains = targets;
        }
        else if (left > 0)
        {
            gains[0] += slopes[0];
            gains[1] += slopes[1];
        }
    };

    std::array<float, 6> waves{};
    std::array<float, 32> meters{};
    for (int index = 0; index < ChannelCount(); index++)
    {
        Voice& voice = enhanced.voices[static_cast<std::size_t>(index)];
        if (!voice.sounding && !voice.steps)
        {
            continue;
        }

        ramp(voice.gains, voice.targets, voice.slopes, voice.ramp, voice.fresh, Gains(index));
        const bool muted = Muted(index);
        if (voice.steps)
        {
            voice.run_gains.push_back(muted ? std::array<float, 2>{} : voice.gains);
        }
        if (!voice.sounding)
        {
            continue;
        }

        const double place = VoicePlace(index, at, now);
        if (voice.stopped && !voice.holds && place - static_cast<double>(kZeros) > static_cast<double>(voice.end))
        {
            // The voice has rung out.
            voice.sounding = false;
            voice.input = {};
            continue;
        }
        const float value = WaveAt(index, place);
        if (muted)
        {
            continue;
        }

        const std::array<float, 2> sides = {value * voice.gains[0], value * voice.gains[1]};
        bool mixed = true;
        if (index == 1)
        {
            waves[2] = sides[0];
            waves[3] = sides[1];
            mixed = (sound_control & kChannel1NotToMixer) == 0;
        }
        else if (index == 3)
        {
            waves[4] = sides[0];
            waves[5] = sides[1];
            mixed = (sound_control & kChannel3NotToMixer) == 0;
        }
        if (!mixed)
        {
            continue;
        }
        waves[0] += sides[0];
        waves[1] += sides[1];
        if (const int meter = meters_[static_cast<std::size_t>(index)]; metered_ && meter >= 0)
        {
            meters[static_cast<std::size_t>(meter) * 2] += sides[0];
            meters[static_cast<std::size_t>(meter) * 2 + 1] += sides[1];
        }
    }

    const float master_target =
        (sound_control & kMasterEnable) ? static_cast<float>(Factor(sound_control & 0x7F)) : 0.0f;
    std::array<float, 2> master = {enhanced.master, enhanced.master};
    std::array<float, 2> master_targets = {enhanced.master_target, enhanced.master_target};
    std::array<float, 2> master_slopes = {enhanced.master_slope, enhanced.master_slope};
    ramp(master, master_targets, master_slopes, enhanced.master_ramp, enhanced.master_fresh,
         {master_target, master_target});
    enhanced.master = master[0];
    enhanced.master_target = master_targets[0];
    enhanced.master_slope = master_slopes[0];

    enhanced.run_waves.push_back(waves);
    enhanced.run_master.push_back(enhanced.master);
    if (metered_)
    {
        enhanced.run_meters.push_back(meters);
    }
}

void SoundUnit::FinishEnhanced(std::vector<float>& out)
{
    EnhancedState& enhanced = *enhanced_;
    const std::size_t count = enhanced.run_waves.size();

    // The rectangular waves and the noise, kDelay samples late, at their gains.
    std::vector<float> psg(2 * count, 0.0f);
    std::vector<float> levels(count);
    for (int index = 0; index < ChannelCount(); index++)
    {
        Voice& voice = enhanced.voices[static_cast<std::size_t>(index)];
        if (!voice.steps || voice.idle)
        {
            continue;
        }
        voice.steps->Read(levels.data(), count);
        const int meter = meters_[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < count; i++)
        {
            for (std::size_t side = 0; side < 2; side++)
            {
                const float part = levels[i] * voice.run_gains[i][side];
                psg[2 * i + side] += part;
                if (metered_ && meter >= 0)
                {
                    enhanced.run_meters[i][static_cast<std::size_t>(meter) * 2 + side] += part;
                }
            }
        }
        if (voice.level == 0.0f && voice.steps->Level() == 0.0 && enhanced.samples >= voice.settled)
        {
            voice.idle = true;
        }
    }

    // The waves through the delay.
    std::array<std::vector<float>, 6> delayed;
    for (std::size_t line = 0; line < 6; line++)
    {
        std::vector<float>& held = enhanced.delayed[line];
        for (const std::array<float, 6>& waves : enhanced.run_waves)
        {
            held.push_back(waves[line]);
        }
        delayed[line].assign(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(count));
        held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(count));
    }

    // The outputs that SOUNDCNT chooses, at the master volume.
    const uint32_t sound_control = Word(kSoundControl);
    out.reserve(out.size() + 2 * count);
    for (std::size_t i = 0; i < count; i++)
    {
        for (std::size_t side = 0; side < 2; side++)
        {
            float value = delayed[side][i] + psg[2 * i + side];
            const float channel1 = delayed[2 + side][i];
            const float channel3 = delayed[4 + side][i];
            switch ((sound_control >> (8 + 2 * side)) & 3)
            {
            case 1:
                value = channel1;
                break;
            case 2:
                value = channel3;
                break;
            case 3:
                value = channel1 + channel3;
                break;
            default:
                break;
            }
            out.push_back(value * enhanced.run_master[i] * kOutputScale);
        }
    }

    // The meters, on the scale of 16-bit samples.
    if (metered_)
    {
        for (std::size_t i = 0; i < count; i++)
        {
            for (std::size_t meter = 0; meter < peaks_.size(); meter++)
            {
                for (std::size_t side = 0; side < 2; side++)
                {
                    const float level =
                        std::abs(enhanced.run_meters[i][meter * 2 + side] * enhanced.run_master[i]) / 32768.0f;
                    peaks_[meter][side] = std::max(peaks_[meter][side], static_cast<int32_t>(std::lround(level)));
                }
            }
        }
    }
}

} // namespace ntdlib::ntr
