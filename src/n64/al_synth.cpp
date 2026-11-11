// SPDX-License-Identifier: MIT

// The voices of libultra's synthesizer and their parameters, and the setup of the effects. al_synth_standard.cpp and
// al_synth_compact.cpp build the audio lists.

#include "ntdlib/n64/al_synth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "al_synth_constants.h"
#include "round_toward_zero.h"

namespace ntdlib::n64
{

namespace
{

using al_synth_constants::kAdpcmFrameBytes;

constexpr int kStateSize = 32;
constexpr int kMixerStateSize = 80;
constexpr int kPoleStateSize = 32;
constexpr int kStolenRampSamples = 64;
constexpr int kStolenOffset = 512;

// The compact form's constants, from the code at 0x800490E0 and 0x80050DE0 in Pokémon Stadium.
constexpr int kCompactStolenOffset = 552;
constexpr int kCompactStolenRamp = 368;

// The presets of libultra's effects: the sections and the length, then for each section the input, the output, the
// feedback, the feed-forward, the gain, the chorus's rate and depth, and the low-pass filter's coefficient. The times
// are in units of 40 samples. The library calls that unit a millisecond.
constexpr int32_t kMillisecond = 40;

// The compact form's presets, from the table at 0x800794B0 in Pokémon Stadium.
const std::vector<int32_t>& CompactEffectParameters(AlEffect effect, const std::vector<int32_t>& custom)
{
    static const std::vector<int32_t> kSmallRoom = {3, 4000, 0,    2160, 9830,  -9830, 0, 0,    0,
                                                    0, 760,  1520, 3276, -3276, 16383, 0, 0,    0,
                                                    0, 2400, 5000, 0,    0,     0,     0, 20480};
    static const std::vector<int32_t> kBigRoom = {
        4, 4000, 0,    2640, 9830,  -9830, 0, 0, 0, 0, 880,  2160, 3276, -3276, 16383, 0, 0,
        0, 2640, 3640, 3276, -3276, 16383, 0, 0, 0, 0, 3760, 8000, 0,    0,     0,     0, 20480};
    static const std::vector<int32_t> kChorus = {1, 8000, 0, 7160, 12000, 0, 32767, 0, 0, 0};
    static const std::vector<int32_t> kFlange = {1, 800, 0, 200, 16384, 0, 32767, 7600, 700, 0};
    static const std::vector<int32_t> kEcho = {1, 800, 0, 200, 0, 24575, 32767, 380, 500, 0};
    static const std::vector<int32_t> kNull = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    switch (effect)
    {
    case AlEffect::kSmallRoom:
        return kSmallRoom;
    case AlEffect::kBigRoom:
        return kBigRoom;
    case AlEffect::kChorus:
        return kChorus;
    case AlEffect::kFlange:
        return kFlange;
    case AlEffect::kEcho:
        return kEcho;
    case AlEffect::kCustom:
        return custom;
    case AlEffect::kNone:
        break;
    }

    return kNull;
}

const std::vector<int32_t>& EffectParameters(AlEffect effect, const std::vector<int32_t>& custom)
{
    static const std::vector<int32_t> kSmallRoom = {3,
                                                    100 * kMillisecond,
                                                    0,
                                                    54 * kMillisecond,
                                                    9830,
                                                    -9830,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    19 * kMillisecond,
                                                    38 * kMillisecond,
                                                    3276,
                                                    -3276,
                                                    0x3FFF,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    60 * kMillisecond,
                                                    5000,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    0x5000};
    static const std::vector<int32_t> kBigRoom = {4,
                                                  100 * kMillisecond,
                                                  0,
                                                  66 * kMillisecond,
                                                  9830,
                                                  -9830,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  22 * kMillisecond,
                                                  54 * kMillisecond,
                                                  3276,
                                                  -3276,
                                                  0x3FFF,
                                                  0,
                                                  0,
                                                  0,
                                                  66 * kMillisecond,
                                                  91 * kMillisecond,
                                                  3276,
                                                  -3276,
                                                  0x3FFF,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  94 * kMillisecond,
                                                  8000,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  0x5000};
    static const std::vector<int32_t> kEcho = {1, 200 * kMillisecond, 0, 179 * kMillisecond, 12000, 0, 0x7FFF, 0, 0, 0};
    static const std::vector<int32_t> kChorus = {
        1, 20 * kMillisecond, 0, 5 * kMillisecond, 0x4000, 0, 0x7FFF, 7600, 700, 0};
    static const std::vector<int32_t> kFlange = {1, 20 * kMillisecond, 0, 5 * kMillisecond, 0, 0x5FFF, 0x7FFF, 380, 500,
                                                 0};
    static const std::vector<int32_t> kNull = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    switch (effect)
    {
    case AlEffect::kSmallRoom:
        return kSmallRoom;
    case AlEffect::kBigRoom:
        return kBigRoom;
    case AlEffect::kEcho:
        return kEcho;
    case AlEffect::kChorus:
        return kChorus;
    case AlEffect::kFlange:
        return kFlange;
    case AlEffect::kCustom:
        return custom;
    case AlEffect::kNone:
        break;
    }

    return kNull;
}

void InitLowPass(AudioMemory& memory, int16_t cutoff, int16_t& gain, std::array<int16_t, 16>& coefficients, bool& first,
                 uint32_t address)
{
    constexpr int kScale = 16384;
    const int32_t scaled = cutoff * kScale;
    const auto fc = static_cast<int16_t>(scaled >> 15);
    gain = static_cast<int16_t>(kScale - fc);
    first = true;
    coefficients.fill(0);
    coefficients[8] = fc;
    double running = static_cast<double>(fc) / kScale;
    const double step = running;
    for (std::size_t i = 9; i < coefficients.size(); i++)
    {
        running *= step;
        coefficients[i] = static_cast<int16_t>(static_cast<int32_t>(running * kScale));
    }

    std::array<uint8_t, 32> bytes = {};
    for (std::size_t i = 0; i < coefficients.size(); i++)
    {
        xsflib::StoreBe16(bytes.data() + i * 2, static_cast<uint16_t>(coefficients[i]));
    }

    memory.Write(address, bytes.data(), bytes.size());
}
} // namespace

AlSynth::AlSynth(const AlSynthConfig& config, AudioMemory& memory)
    : form_(config.form),
      round_toward_zero_(config.round_toward_zero),
      single_precision_ramps_(config.single_precision_ramps),
      later_compact_(config.later_compact),
      linear_ramps_(config.linear_ramps),
      output_rate_(config.output_rate)
{
    const RoundTowardZero rounding(round_toward_zero_);
    free_updates_ = config.updates;
    InitFx(config, memory);

    // The free list takes each new voice at its head. The last voice therefore comes first.
    physical_.resize(static_cast<std::size_t>(config.voices));
    for (int i = 0; i < config.voices; i++)
    {
        PhysicalVoiceState& voice = physical_[static_cast<std::size_t>(i)];
        free_list_.insert(free_list_.begin(), i);
        voice.decoder_state = memory.Allocate(kStateSize);
        voice.decoder_loop_state = memory.Allocate(kStateSize);
        voice.resampler_state = memory.Allocate(kStateSize);
        voice.mixer_state = memory.Allocate(kMixerStateSize);
    }
}

void AlSynth::InitFx(const AlSynthConfig& config, AudioMemory& memory)
{
    if (config.effect == AlEffect::kNone)
    {
        return;
    }

    const std::vector<int32_t>& parameters = form_ == AlSynthForm::kCompact
                                                 ? CompactEffectParameters(config.effect, config.custom_effect)
                                                 : EffectParameters(config.effect, config.custom_effect);
    std::size_t at = 0;
    const auto next = [&]
    {
        return at < parameters.size() ? parameters[at++] : 0;
    };

    const auto sections = static_cast<uint8_t>(next());
    fx_.present = true;
    fx_.length = static_cast<uint32_t>(next());
    fx_.base = memory.Allocate(fx_.length * 2);
    fx_.delays.resize(sections);
    for (Delay& delay : fx_.delays)
    {
        delay.input = static_cast<uint32_t>(next());
        delay.output = static_cast<uint32_t>(next());
        delay.feedback = static_cast<int16_t>(next());
        delay.feed_forward = static_cast<int16_t>(next());
        delay.gain = static_cast<int16_t>(next());
        const int32_t rate = next();
        const int32_t depth = next();
        if (rate != 0)
        {
            constexpr double kRange = 2.0;
            constexpr double kConvert = 173123.404906676;
            delay.mod_increment =
                static_cast<float>(static_cast<double>(static_cast<float>(rate) / 1000) * kRange / config.output_rate);
            delay.mod_gain = static_cast<float>(static_cast<double>(static_cast<float>(depth)) / kConvert *
                                                static_cast<double>(delay.output - delay.input));
            delay.mod_value = 1.0F;
            delay.has_resampler = true;
            delay.resampler_state = memory.Allocate(kStateSize);
        }

        const int32_t cutoff = next();
        if (cutoff != 0)
        {
            delay.has_low_pass = true;
            delay.low_pass.state = memory.Allocate(kPoleStateSize);
            delay.low_pass.coefficients_address = memory.Allocate(kStateSize);
            delay.low_pass.cutoff = static_cast<int16_t>(cutoff);
            if (config.filters_stay_clear)
            {
                delay.low_pass.first = false;
            }
            else
            {
                InitLowPass(memory, delay.low_pass.cutoff, delay.low_pass.gain, delay.low_pass.coefficients,
                            delay.low_pass.first, delay.low_pass.coefficients_address);
            }
        }
    }
}

int AlSynth::AddVoices(int count)
{
    const int first = static_cast<int>(voices_.size());
    voices_.resize(voices_.size() + static_cast<std::size_t>(count));

    return first;
}

int AlSynth::VoiceCount() const
{
    return static_cast<int>(voices_.size());
}

int AlSynth::PhysicalVoiceCount() const
{
    return static_cast<int>(physical_.size());
}

int AlSynth::PhysicalVoice(int voice) const
{
    return voices_.at(static_cast<std::size_t>(voice)).physical;
}

bool AlSynth::VoiceFinished(int voice) const
{
    return voices_.at(static_cast<std::size_t>(voice)).finished;
}

void AlSynth::SetVoiceTag(int voice, int tag)
{
    const int physical = PhysicalVoice(voice);
    if (physical >= 0)
    {
        physical_[static_cast<std::size_t>(physical)].tag = tag;
    }
}

int AlSynth::PhysicalVoiceTag(int physical) const
{
    return physical_.at(static_cast<std::size_t>(physical)).tag;
}

bool AlSynth::AnyVoicePlaying() const
{
    return std::ranges::any_of(physical_, &PhysicalVoiceState::playing);
}

int32_t AlSynth::TimeToSamplesNoRound(int32_t microseconds) const
{
    const auto samples = static_cast<float>(
        static_cast<double>(static_cast<float>(microseconds) * static_cast<float>(output_rate_)) / 1000000.0 + 0.5);

    return static_cast<int32_t>(samples);
}

int32_t AlSynth::TimeToSamples(int32_t microseconds) const
{
    return TimeToSamplesNoRound(microseconds) & ~0xF;
}

void AlSynth::SetParamSamples(int32_t samples)
{
    param_samples_ = samples;
}

int32_t AlSynth::ParamSamples() const
{
    return param_samples_;
}

int32_t AlSynth::CurrentSamples() const
{
    return current_samples_;
}

bool AlSynth::AllocParam()
{
    if (free_updates_ == 0)
    {
        return false;
    }

    free_updates_--;

    return true;
}

void AlSynth::AddUpdate(int physical, Param param)
{
    physical_[static_cast<std::size_t>(physical)].updates.push_back(param);
}

int AlSynth::AllocatePhysical(int16_t priority, bool& stolen)
{
    stolen = false;
    if (!lame_list_.empty())
    {
        const int physical = lame_list_.front();
        lame_list_.erase(lame_list_.begin());
        alloc_list_.insert(alloc_list_.begin(), physical);

        return physical;
    }

    if (!free_list_.empty())
    {
        const int physical = free_list_.front();
        free_list_.erase(free_list_.begin());
        alloc_list_.insert(alloc_list_.begin(), physical);

        return physical;
    }

    int found = -1;
    for (const int physical : alloc_list_)
    {
        const PhysicalVoiceState& candidate = physical_[static_cast<std::size_t>(physical)];
        if (candidate.voice >= 0 && voices_[static_cast<std::size_t>(candidate.voice)].priority <= priority &&
            candidate.offset == 0)
        {
            found = physical;
            priority = voices_[static_cast<std::size_t>(candidate.voice)].priority;
            stolen = true;
        }
    }

    return found;
}

bool AlSynth::AllocVoice(int voice, int16_t priority, bool unity_pitch)
{
    Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    virtual_voice.priority = priority;
    virtual_voice.unity_pitch = unity_pitch;
    virtual_voice.has_wave = false;
    virtual_voice.physical = -1;
    virtual_voice.finished = false;

    bool stolen = false;
    const int physical = AllocatePhysical(priority, stolen);
    if (physical < 0)
    {
        return false;
    }

    PhysicalVoiceState& state = physical_[static_cast<std::size_t>(physical)];
    if (stolen)
    {
        state.offset = form_ == AlSynthForm::kCompact ? kCompactStolenOffset : kStolenOffset;
        voices_[static_cast<std::size_t>(state.voice)].physical = -1;

        // The stolen voice ramps down, then stops.
        if (AllocParam())
        {
            Param ramp;
            ramp.delta = param_samples_;
            ramp.type = ParamType::kSetVolume;
            ramp.data = 0;
            ramp.more = form_ == AlSynthForm::kCompact ? kCompactStolenRamp : state.offset - kStolenRampSamples;
            AddUpdate(physical, ramp);
        }

        if (AllocParam())
        {
            Param stop;
            stop.delta = param_samples_ + state.offset;
            stop.type = ParamType::kStopVoice;
            AddUpdate(physical, stop);
        }
    }
    else
    {
        state.offset = 0;
    }

    state.voice = voice;
    virtual_voice.physical = physical;

    return true;
}

void AlSynth::MoveToLame(int physical)
{
    alloc_list_.erase(std::find(alloc_list_.begin(), alloc_list_.end(), physical));
    lame_list_.insert(lame_list_.begin(), physical);
}

void AlSynth::FreeVoice(int voice)
{
    Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0)
    {
        return;
    }

    PhysicalVoiceState& state = physical_[static_cast<std::size_t>(virtual_voice.physical)];
    if (state.offset != 0)
    {
        if (!AllocParam())
        {
            return;
        }

        Param update;
        update.delta = param_samples_ + state.offset;
        update.type = ParamType::kFreeVoice;
        update.physical = virtual_voice.physical;
        AddUpdate(virtual_voice.physical, update);
    }
    else
    {
        MoveToLame(virtual_voice.physical);
    }

    virtual_voice.physical = -1;
}

void AlSynth::SetPriority(int voice, int16_t priority)
{
    voices_.at(static_cast<std::size_t>(voice)).priority = priority;
}

void AlSynth::StartVoiceParams(int voice, const AlWave& wave, float pitch, int16_t volume, uint8_t pan, uint8_t fx_mix,
                               int32_t microseconds)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kStartVoiceAlt;
    update.unity = virtual_voice.unity_pitch;
    update.pan = pan;
    update.volume = volume;
    update.fx_mix = std::min<uint8_t>(fx_mix, 127);
    update.pitch = pitch;
    update.samples = TimeToSamples(microseconds);
    update.wave = wave;
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::StartVoice(int voice, const AlWave& wave)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kStartVoice;
    update.wave = wave;
    update.unity = virtual_voice.unity_pitch;
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::StopVoice(int voice)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kStopVoice;
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::SetVolume(int voice, int16_t volume, int32_t microseconds)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kSetVolume;
    update.data = volume;
    update.more = TimeToSamples(microseconds);
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::SetPan(int voice, uint8_t pan)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kSetPan;
    update.data = pan;
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::SetPitch(int voice, float pitch)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kSetPitch;
    update.pitch = pitch;
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::SetFxMix(int voice, uint8_t fx_mix)
{
    const Voice& virtual_voice = voices_.at(static_cast<std::size_t>(voice));
    if (virtual_voice.physical < 0 || !AllocParam())
    {
        return;
    }

    Param update;
    update.delta = param_samples_ + physical_[static_cast<std::size_t>(virtual_voice.physical)].offset;
    update.type = ParamType::kSetFxMix;
    update.data = std::min<uint8_t>(fx_mix, 127);
    AddUpdate(virtual_voice.physical, update);
}

void AlSynth::ApplySetWave(AudioMemory& memory, PhysicalVoiceState& voice, const AlWave& wave)
{
    voice.wave = wave;
    voice.has_wave = true;
    voice.memory_in = wave.base;
    voice.sample = 0;
    if (!wave.raw)
    {
        voice.wave.length = kAdpcmFrameBytes * (wave.length / kAdpcmFrameBytes);
        voice.bytes_book = wave.book_size;
    }

    if (wave.has_loop)
    {
        voice.loop_start = wave.loop_start;
        voice.loop_end = wave.loop_end;
        voice.loop_count = wave.loop_count;
        if (!wave.raw)
        {
            std::array<uint8_t, kStateSize> state = {};
            memory.Read(wave.loop_state, state.data(), state.size());
            memory.Write(voice.decoder_loop_state, state.data(), state.size());
        }
    }
    else
    {
        voice.loop_start = 0;
        voice.loop_end = 0;
        voice.loop_count = 0;
    }
}

void AlSynth::ResetFilters(PhysicalVoiceState& voice)
{
    voice.mixer_first = true;
    voice.playing = false;
    voice.volume = 1;

    voice.delta = 0.0F;
    voice.resampler_first = true;
    voice.unity_pitch = false;

    voice.last_sample = 0;
    voice.decoder_first = true;
    voice.sample = 0;
    if (voice.has_wave)
    {
        voice.memory_in = voice.wave.base;
        if (voice.wave.has_loop)
        {
            voice.loop_count = voice.wave.loop_count;
        }
    }
}

} // namespace ntdlib::n64
