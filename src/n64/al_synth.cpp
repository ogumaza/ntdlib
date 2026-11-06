// SPDX-License-Identifier: MIT

#include "ntdlib/n64/al_synth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "round_toward_zero.h"

namespace ntdlib::n64
{

namespace
{

constexpr int kAdpcmFrameBytes = 9;
constexpr int kAdpcmFrameSamples = 16;
constexpr int kAdpcmFrameShift = 4;
constexpr double kMaxRatio = 1.99996;
constexpr int32_t kUnityPitch = 0x8000;
constexpr uint32_t kForever = 0xFFFFFFFF;
constexpr int kStateSize = 32;
constexpr int kMixerStateSize = 80;
constexpr int kPoleStateSize = 32;
constexpr int kStolenRampSamples = 64;
constexpr int kStolenOffset = 512;

// The compact form's constants, from the code at 0x800490E0 and 0x80050DE0 in Pokémon Stadium.
constexpr int kCompactStolenOffset = 552;
constexpr int kCompactStolenRamp = 368;
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

// The volume's gain for each pan, in the table of the game's code. The table runs from full level to silence along a
// quarter of a cosine.
constexpr std::array<int16_t, 128> kEqualPower = {
    32767, 32764, 32757, 32744, 32727, 32704, 32677, 32644, 32607, 32564, 32517, 32464, 32407, 32344, 32277, 32205,
    32127, 32045, 31958, 31866, 31770, 31668, 31561, 31450, 31334, 31213, 31087, 30957, 30822, 30682, 30537, 30388,
    30234, 30075, 29912, 29744, 29572, 29395, 29214, 29028, 28838, 28643, 28444, 28241, 28033, 27821, 27605, 27385,
    27160, 26931, 26698, 26461, 26220, 25975, 25726, 25473, 25216, 24956, 24691, 24423, 24151, 23875, 23596, 23313,
    23026, 22736, 22442, 22145, 21845, 21541, 21234, 20924, 20610, 20294, 19974, 19651, 19325, 18997, 18665, 18331,
    17993, 17653, 17310, 16965, 16617, 16266, 15913, 15558, 15200, 14840, 14477, 14113, 13746, 13377, 13006, 12633,
    12258, 11881, 11503, 11122, 10740, 10357, 9971,  9584,  9196,  8806,  8415,  8023,  7630,  7235,  6839,  6442,
    6044,  5646,  5246,  4845,  4444,  4042,  3640,  3237,  2833,  2429,  2025,  1620,  1216,  810,   405,   0};

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
    physical_[static_cast<std::size_t>(physical)].updates.push_back(std::move(param));
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
            AddUpdate(physical, std::move(ramp));
        }

        if (AllocParam())
        {
            Param stop;
            stop.delta = param_samples_ + state.offset;
            stop.type = ParamType::kStopVoice;
            AddUpdate(physical, std::move(stop));
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
        AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
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
    AddUpdate(virtual_voice.physical, std::move(update));
}

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
            break;

        case ParamType::kSetFxMix:
        case ParamType::kSetPan:
        case ParamType::kSetVolume:
            PullSubFrame(list, voice, in, out, samples, offset);
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
                        : static_cast<int16_t>(static_cast<int32_t>(GetVolume(
                              voice.current_left, voice.mixer_delta, voice.rate_high_left, voice.rate_low_left)));
                voice.current_right =
                    linear_ramps_
                        ? LinearVolume(voice.current_right, voice.mixer_delta, voice.rate_high_right,
                                       voice.rate_low_right)
                        : static_cast<int16_t>(static_cast<int32_t>(GetVolume(
                              voice.current_right, voice.mixer_delta, voice.rate_high_right, voice.rate_low_right)));
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
        voice.last_sample = static_cast<int32_t>(voice.loop_start & 0xF);
        voice.memory_in = static_cast<int64_t>(voice.wave.base) +
                          kAdpcmFrameBytes * (static_cast<int64_t>(voice.loop_start >> kAdpcmFrameShift) + 1);
        voice.sample = static_cast<int32_t>(voice.loop_start);

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
            tsam = std::max(samples - kAdpcmFrameSamples + voice.last_sample, 0);
            frames = (tsam + kAdpcmFrameSamples - 1) >> kAdpcmFrameShift;
            bytes = frames * kAdpcmFrameBytes;
            DecodeChunk(list, voice, tsam, bytes, static_cast<int16_t>(place), in,
                        static_cast<uint8_t>((voice.decoder_first ? 1 : 0) | kAudioFlagLoop));
            list.DmemMove(static_cast<uint16_t>(place + (voice.last_sample << 1)), static_cast<uint16_t>(buffer_end),
                          static_cast<uint32_t>(samples) << 1);
        }

        voice.last_sample = (left + voice.last_sample) & 0xF;
        voice.sample += left;
        voice.memory_in += kAdpcmFrameBytes * frames;

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
            bytes = samples << 1;
            const auto location = static_cast<uint32_t>(voice.memory_in);
            const int dram_align = static_cast<int>(location & 7);
            bytes += dram_align;
            const int dmem_align = (place & 7) != 0 ? 8 - (place & 7) : 0;
            list.SetBuffer(0, static_cast<uint16_t>(place + dmem_align), 0,
                           static_cast<uint32_t>(bytes + 8 - (bytes & 7)));
            list.LoadBuffer(location - static_cast<uint32_t>(dram_align));
            if (dram_align != 0 || dmem_align != 0)
            {
                list.DmemMove(static_cast<uint16_t>(place + dram_align + dmem_align), static_cast<uint16_t>(place),
                              static_cast<uint32_t>(samples) << 1);
            }
        }

        voice.sample += left;
        voice.memory_in += static_cast<int64_t>(left) << 1;

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
    const auto targets = [&]
    {
        voice.target_left =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(voice.pan)]) >> 15);
        voice.target_right =
            static_cast<int16_t>((voice.volume * kEqualPower[static_cast<std::size_t>(127 - voice.pan)]) >> 15);
    };

    while (!voice.updates.empty())
    {
        Param& update = voice.updates.front();
        const int32_t last_offset = this_offset;
        int32_t samples = 0;
        if (later_compact_)
        {
            this_offset = RoundToCompactChunk(update.delta);
            samples = this_offset - last_offset;
        }
        else
        {
            this_offset = update.delta;
            samples = RoundToCompactChunk(this_offset - last_offset);
            if (samples == 0)
            {
                this_offset = last_offset;
            }
        }

        if (samples > left)
        {
            break;
        }

        switch (update.type)
        {
        case ParamType::kStartVoiceAlt:
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
                targets();
                voice.current_left = voice.target_left;
                voice.current_right = voice.target_right;
            }

            voice.ratio = update.pitch;
            break;

        case ParamType::kSetFxMix:
        case ParamType::kSetPan:
        case ParamType::kSetVolume:
            PullCompactSubFrame(list, voice, in, samples);
            if (voice.mixer_delta >= voice.segment_end || (later_compact_ && samples == 0))
            {
                targets();
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
            tsam = std::max(samples + voice.last_sample - kAdpcmFrameSamples, 0);
            frames = (tsam + kAdpcmFrameSamples - 1) >> kAdpcmFrameShift;
            bytes = frames * kAdpcmFrameBytes;
            CompactDecodeChunk(list, voice, tsam, bytes, static_cast<int16_t>(place),
                               (voice.decoder_first ? 1U : 0U) | kAudioFlagLoop);
            list.commands.push_back(
                {kOpMove | (static_cast<uint32_t>(place + (voice.last_sample << 1)) & kAddressMask),
                 (static_cast<uint32_t>(buffer_end) << 16) | (static_cast<uint32_t>(samples << 1) & 0xFFFF)});
        }

        voice.last_sample = (left + voice.last_sample) & 0xF;
        voice.sample += left;
        voice.memory_in += kAdpcmFrameBytes * frames;

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
