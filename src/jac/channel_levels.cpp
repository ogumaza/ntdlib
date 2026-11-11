// SPDX-License-Identifier: MIT

// The channels' levels: the pans, the effects of the instruments and the oscillators, and the mixers' volumes and
// the pitch that go to the DSP (see channel.cpp).

#include "ntdlib/jac/channel.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "jac/channel_internal.h"
#include "ntdlib/jaudio1/channel.h"
#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jac
{

namespace
{

// The ways that the three sources of a pan, an effect send or a surround level add up (0x5bf478 in main's ro): for
// each source, 0 leaves it out, 1 adds its value, and 2 adds its value times its weight. The table holds 27 types.
// A higher type reads the data that follows the table. Of those bytes, only the ones below hold a 1 or a 2.
using PanModes = std::array<uint8_t, 3>;

constexpr const std::array<PanModes, 27>& kPanTypes = jaudio1::kCalcTable; // JAudio1's table holds the same types

struct PanByte
{
    uint8_t type;
    uint8_t source;
    uint8_t mode;
};

constexpr std::array<PanByte, 15> kPanBytesPastTable = {{
    {80, 1, 2},
    {83, 0, 2},
    {85, 2, 1},
    {88, 1, 1},
    {90, 2, 1},
    {91, 0, 1},
    {98, 0, 2},
    {162, 2, 2},
    {170, 0, 1},
    {170, 1, 1},
    {173, 0, 1},
    {175, 2, 1},
    {178, 1, 2},
    {181, 0, 2},
    {248, 0, 1},
}};

constexpr std::array<PanModes, 256> MakePanTable()
{
    std::array<PanModes, 256> table{};
    for (std::size_t i = 0; i < kPanTypes.size(); i++)
    {
        table[i] = kPanTypes[i];
    }
    for (const PanByte& b : kPanBytesPastTable)
    {
        table[b.type][b.source] = b.mode;
    }

    return table;
}

constexpr std::array<PanModes, 256> kPanTable = MakePanTable();

// DSP_SetMixerInitDelayMax.
void SetMixerInitDelayMax(DspBlock& block, uint8_t delay)
{
    block.SetU16(DspChannelBlock::kDelayCount, delay);
}

// DSP_SetMixerInitVolume: the volume and the target, and the delay in both bytes of the delays' word.
void SetMixerInitVolume(DspBlock& block, uint32_t mixer, uint16_t volume, uint8_t delay)
{
    const std::size_t offset = MixerOffset(mixer);
    block.SetU16(offset + 4, volume);
    block.SetU16(offset + 2, volume);
    block.SetU16(offset + 6, uint32_t{delay} * 0x101);
}

// DSP_SetMixerVolume: the target and the delay in the delays' high byte, unless the channel is stopping.
void SetMixerVolume(DspMemory& memory, DspBlock& block, uint32_t mixer, uint16_t volume, uint8_t delay)
{
    if (block.U16(DspChannelBlock::kForcedStop) != 0)
    {
        return;
    }

    const std::size_t offset = MixerOffset(mixer);
    block.SetU16(offset + 2, volume);
    memory.SetByte(block.Address() + static_cast<uint32_t>(offset + 7), delay);
}

// DSP_SetMixerTarget: the target alone, unless the channel is stopping.
void SetMixerTarget(DspBlock& block, uint32_t mixer, uint16_t volume)
{
    if (block.U16(DspChannelBlock::kForcedStop) != 0)
    {
        return;
    }

    block.SetU16(MixerOffset(mixer) + 2, volume);
}

// DSP_SetIIR4: the IIR filter's 4 coefficients.
void SetIir4(DspBlock& block, const std::array<int16_t, 4>& iir)
{
    for (std::size_t i = 0; i < iir.size(); i++)
    {
        block.SetU16(DspChannelBlock::kIirCoefficients + 2 * i, static_cast<uint16_t>(iir[i]));
    }
}

void SetFir8(DspBlock& block, const std::array<int16_t, 8>& fir)
{
    std::array<uint16_t, 8> values{};
    for (std::size_t i = 0; i < fir.size(); i++)
    {
        values[i] = static_cast<uint16_t>(fir[i]);
    }
    block.SetFir8FilterParam(values);
}

// The pan, the effect send and the surround level of a channel, each held to 0 to 1.
struct ChannelPans
{
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
};

// Sets each mixer's volume of `c` (see the second half of UpdateEffecterParam). `volume_level` is the channel's volume
// before the clamp, `volume` the volume that the channel plays at, and `max` the mixers' full level.
void SetMixerLevels(Channel& c, const ChannelPans& pans, float volume_level, float volume, float max)
{
    const float pan = pans.pan;
    const float fxmix = pans.fxmix;
    const float dolby = pans.dolby;

    // Each mixer's config: its bus in the high byte, then two steps of 4 bits. Each step multiplies the level by a
    // curve of a value: a quarter sine (Sinf3), the value itself, or the surround curve (the second step's surround
    // level). A step of 4 or above 7 takes the value of the last step before it, in this mixer or an earlier one,
    // or the volume before any step. The first mixer's first step takes the pan there.
    const float pan_rest = 1.0f - pan;
    const float fxmix_rest = 1.0f - fxmix;
    const float dolby_rest = 1.0f - dolby;
    float value = volume_level;

    // A step's value: 1 to 3 take the pan, the effect send and the surround level, 5 to 7 what each leaves of 1. The
    // step gives the level that it took, or kKept for a step that keeps the value.
    enum Source
    {
        kKept,
        kPanLevel,
        kFxmixLevel,
        kDolbyLevel,
    };
    const auto take = [&](uint32_t step)
    {
        switch (step)
        {
        case 1:
            value = pan;
            return kPanLevel;

        case 2:
            value = fxmix;
            return kFxmixLevel;

        case 3:
            value = dolby;
            return kDolbyLevel;

        case 5:
            value = pan_rest;
            return kPanLevel;

        case 6:
            value = fxmix_rest;
            return kFxmixLevel;

        case 7:
            value = dolby_rest;
            return kDolbyLevel;

        default:
            return kKept;
        }
    };

    for (std::size_t i = 0; i < c.mix_configs.size(); i++)
    {
        const uint16_t config = c.mix_configs[i];
        if (config < 0x100)
        {
            c.mixer_levels[i] = 0;
            continue;
        }

        // The first step's curve is a quarter sine, or the value itself for the effect send. The first mixer takes
        // the pan for a step that would keep the value.
        float level = volume;
        const uint32_t first = (config >> 4) & 0xf;
        if (first != 0)
        {
            const Source source = take(first);
            if (source == kKept && i == 0)
            {
                value = pan;
            }
            level = volume * (source == kFxmixLevel ? value : Sinf3(value));
        }

        // The second step's curve is the surround curve for the surround level.
        const uint32_t second = config & 0xf;
        if (second != 0)
        {
            const Source source = take(second);
            float factor = value;
            if (source == kDolbyLevel)
            {
                factor = SurroundCurve(value);
            }
            else if (source != kFxmixLevel)
            {
                factor = Sinf3(value);
            }
            level = level * factor;
        }

        c.mixer_levels[i] = static_cast<uint16_t>(ToInt32(level * max));
    }
}

} // namespace

float Channels::PanCalc(const Channel& c, std::size_t matrix, uint8_t type) const
{
    // PanCalc (in UpdateEffecterParam, 0x274ba0 in main): the sources' values in turn, from 0.
    const PanModes& modes = kPanTable[type];
    const std::array<float, 3>& values = c.pans[matrix];
    const std::array<float, 3>& weights = c.pans[Channel::kPower];
    float sum = 0.0f;
    if (modes[0] == 2)
    {
        sum = values[0] * weights[0] + 0.0f;
    }
    else if (modes[0] == 1)
    {
        sum = values[0] + 0.0f;
    }
    for (std::size_t k = 1; k < 3; k++)
    {
        if (modes[k] == 2)
        {
            sum = sum + values[k] * weights[k];
        }
        else if (modes[k] == 1)
        {
            sum = sum + values[k];
        }
    }

    return sum;
}

void Channels::UpdateEffecterParam(uint32_t channel)
{
    // UpdateEffecterParam (0x274ba0 in main): the manager's levels when the channel took them last from the same
    // manager, then the pan, the effect send and the surround level, the volume and the pitch, and each mixer's
    // volume.
    Channel& c = channels_[channel];
    const ChannelManager& m = managers_[c.manager];
    if (c.last_manager == c.manager)
    {
        c.manager_pitch = m.pitch;
        c.manager_volume = m.volume;
        c.pans[Channel::kPan][Channel::kManager] = m.pan;
        c.pans[Channel::kFxmix][Channel::kManager] = m.fxmix;
        c.pans[Channel::kDolby][Channel::kManager] = m.dolby;
        c.pan_calcs = m.pan_calcs;
    }

    // Stereo gives each level its sum. Mono gives the pan's middle, the effect send's sum and no surround. In any
    // other mode, the game leaves the pan and the effect send unset. The model gives the middle and no send there.
    float pan = 0.5f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
    if (kOutputMode == 1)
    {
        pan = c.pan_calcs[0] == 0 ? 0.5f : PanCalc(c, Channel::kPan, c.pan_calcs[0]);
        fxmix = PanCalc(c, Channel::kFxmix, c.pan_calcs[1]);
        dolby = PanCalc(c, Channel::kDolby, c.pan_calcs[2]);
    }
    else if (kOutputMode == 0)
    {
        fxmix = PanCalc(c, Channel::kFxmix, c.pan_calcs[1]);
    }

    const float volume_level = (c.volume * c.volume_modifier) * c.manager_volume;
    pan = ClampUnit(pan);
    fxmix = ClampUnit(fxmix);
    dolby = ClampUnit(dolby);
    c.final_pitch = static_cast<uint16_t>(ToInt32(((c.pitch * c.pitch_modifier) * c.manager_pitch) * 4096.0f));
    const float volume = c.unheard ? 0.0f : ClampUnit(volume_level);
    const auto max = static_cast<float>(mixer_level_);

    if (c.mix_configs[0] == 0xffff)
    {
        // The auto mixer.
        if (c.dsp_channel != kNone)
        {
            DspBlock block = Block(dsp_channels_[c.dsp_channel].block);
            block.SetAutoMixer(ToInt32(volume * max), ToInt32(pan * 127.5f), ToInt32(dolby * 127.5f),
                               ToInt32(fxmix * 127.5f));
        }
        return;
    }

    SetMixerLevels(c, {pan, fxmix, dolby}, volume_level, volume, max);
}

void Channels::UpdateToDsp(uint32_t channel)
{
    // UpdateJcToDSP (0x274a00 in main). A moved channel keeps its block's delays and filters.
    const Channel& c = channels_[channel];
    DspBlock block = Block(dsp_channels_[c.dsp_channel].block);
    if (c.moved)
    {
        for (uint32_t i = 0; i < c.mixer_levels.size(); i++)
        {
            SetMixerTarget(block, i, c.mixer_levels[i]);
        }
        block.SetPitch(c.final_pitch);
    }
    else
    {
        const ChannelManager& m = managers_[c.manager];
        for (uint32_t i = 0; i < c.mixer_levels.size(); i++)
        {
            SetMixerVolume(memory_, block, i, c.mixer_levels[i], m.delays[i]);
        }
        block.SetPitch(c.final_pitch);
        if ((m.filter_mode & 0x20) != 0)
        {
            SetIir4(block, m.iir);
        }
        if ((m.filter_mode & 0x1f) != 0)
        {
            SetFir8(block, m.fir);
        }
        block.SetFilterMode(m.filter_mode);
        block.SetDistFilter(static_cast<uint16_t>(m.distortion));
    }
    block.SetPauseFlag(c.pause ? 1 : 0);
}

void Channels::UpdateToDspInit(uint32_t channel)
{
    // UpdateJcToDSPInit (0x276210 in main): the mixers' start volumes, or the auto mixer's start, then the pitch and
    // the filters. The distortion keeps its value.
    const Channel& c = channels_[channel];
    const ChannelManager& m = managers_[c.manager];
    DspBlock block = Block(dsp_channels_[c.dsp_channel].block);
    if (static_cast<int16_t>(c.mix_configs[0]) == -1)
    {
        block.InitAutoMixer();
    }
    else
    {
        SetMixerInitDelayMax(block, m.delay_max);
        for (uint32_t i = 0; i < c.mixer_levels.size(); i++)
        {
            SetMixerInitVolume(block, i, c.mixer_levels[i], m.delays[i]);
        }
    }
    block.SetPitch(c.final_pitch);
    if ((m.filter_mode & 0x20) != 0)
    {
        SetIir4(block, m.iir);
    }
    if ((m.filter_mode & 0x1f) != 0)
    {
        SetFir8(block, m.fir);
    }
    block.SetFilterMode(m.filter_mode);
    block.SetPauseFlag(c.pause ? 1 : 0);
}

void Channels::DoEffectOsc(uint32_t channel, uint8_t target, float value)
{
    // DoEffectOsc (0x275780 in main).
    Channel& c = channels_[channel];
    switch (target)
    {
    case OscData::kVolume:
        c.volume_modifier = c.volume_modifier * value;
        break;
    case OscData::kPitch:
        c.pitch_modifier = c.pitch_modifier * value;
        break;
    case OscData::kPan:
        c.pans[Channel::kPan][Channel::kEffect] = value;
        break;
    case OscData::kFxmix:
        c.pans[Channel::kFxmix][Channel::kEffect] = value;
        break;
    case OscData::kDolby:
        c.pans[Channel::kDolby][Channel::kEffect] = value;
        break;
    default:
        break;
    }
}

void Channels::DoEffect(uint32_t channel, uint8_t target, float value)
{
    // An instrument's or a drum's effect at a note's start (in Play_1shot and Play_1shot_Perc).
    Channel& c = channels_[channel];
    switch (target)
    {
    case OscData::kVolume:
        c.volume = value * c.volume;
        break;
    case OscData::kPitch:
        c.pitch = value * c.pitch;
        break;
    case OscData::kPan:
        c.pans[Channel::kPan][Channel::kEffect] = value;
        break;
    case OscData::kFxmix:
        c.pans[Channel::kFxmix][Channel::kEffect] = value;
        break;
    case OscData::kDolby:
        c.pans[Channel::kDolby][Channel::kEffect] = value;
        break;
    default:
        break;
    }
}

} // namespace ntdlib::jac
