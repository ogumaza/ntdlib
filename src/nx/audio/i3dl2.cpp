// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/i3dl2.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ntdlib/aarch64.h"

#include "nx/audio/arithmetic.h"
#include "nx/audio/fixed_point.h"
#include "nx/audio/tables.h"

namespace ntdlib::nx::audio
{
namespace
{

// The channels the early reflections' taps go to, in the order the code adds them up (ApplyI3dl2ReverbEffect2ch).
constexpr std::array<int, 10> kLeftTaps = {0, 1, 2, 7, 8, 9, 13, 14, 15, 16};
constexpr std::array<int, 10> kRightTaps = {3, 4, 5, 6, 10, 11, 12, 17, 18, 19};

// AArch64's fmin and fmax.
using ntdlib::aarch64::Fmax;
using ntdlib::aarch64::Fmin;

// A float in Q14, towards zero and held to 32 bits (fcvtzs with 14 fraction bits).
int32_t ToQ14(float x)
{
    return ToFixed(x, 14);
}

// The sample rate in whole kHz, in Q14. The code works the rate out with a multiply by 2^38 / 1000.
int64_t KilohertzQ14(int32_t sample_rate)
{
    const uint64_t product = static_cast<uint64_t>(static_cast<uint32_t>(sample_rate)) * 0x10624DD3u;
    return static_cast<int64_t>(static_cast<int32_t>((product >> 38) << 14));
}

// 10 to the power of an exponent in Q14, as a float: 1 from 0 up, 0 below the table's end.
float Power(int32_t exponent)
{
    return static_cast<float>(PowerQ14(exponent)) * 0x1p-14f;
}

// An output sample: the float times 65536, held to 24 bits, towards zero (a NaN gives 0).
int32_t ToSample(float x)
{
    return ToInt(Fmin(Fmax(x * 65536.0f, -8388608.0f), 8388607.0f));
}

} // namespace

AuxI3dl2Reverb::AuxI3dl2Reverb(int32_t sample_rate) : sample_rate_(sample_rate)
{
}

void AuxI3dl2Reverb::SetInputOutput(const std::array<int8_t, 2>& inputs, const std::array<int8_t, 2>& outputs)
{
    inputs_ = inputs;
    outputs_ = outputs;
    parameter_state_ = 0;
}

void AuxI3dl2Reverb::SetParameters(const I3dl2ReverbParameterSet& parameters)
{
    if (parameter_state_ != 0)
    {
        parameter_state_ = 1;
    }
    parameters_ = parameters;
}

void AuxI3dl2Reverb::Process(int32_t* output, const int32_t* input, uint32_t sample_count)
{
    const auto block = [sample_count](int8_t index)
    {
        return static_cast<std::size_t>(static_cast<uint32_t>(index) * sample_count);
    };

    if (parameter_state_ == 0)
    {
        Initialize();
    }
    else if (parameter_state_ == 1)
    {
        Update(false);
    }
    parameter_state_ = 2;

    Apply2ch(input + block(inputs_[0]), input + block(inputs_[1]), output + block(outputs_[0]),
             output + block(outputs_[1]), sample_count);
}

AuxI3dl2Reverb::DelayLine AuxI3dl2Reverb::MakeLine(std::size_t& cursor, int32_t length)
{
    DelayLine line;
    line.start = cursor;
    line.length = length;
    line.write = length;
    line.delay = length;
    cursor += static_cast<std::size_t>(length) + 1;

    return line;
}

// The code clears every cell of a line but the last. A line writes the last cell before reading it.
void AuxI3dl2Reverb::Clear(const DelayLine& line)
{
    const auto first = lines_.begin() + static_cast<std::ptrdiff_t>(line.start);
    std::fill(first, first + line.length, 0.0f);
}

void AuxI3dl2Reverb::Initialize()
{
    state_ = State{};
    const int64_t k = KilohertzQ14(sample_rate_);

    // Each feedback line at its longest, its two all-passes, the centre line of 5 ms, and the input's line of 400 ms,
    // one after the other. The input's line starts writing at the line's last cell, or at the first cell if the line's
    // length is odd.
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        state_.fdn[i] = MakeLine(cursor, Samples(ToQ14(tables::kI3dl2FdnMaxDelay[i]), k));
        state_.decay1[i] = MakeLine(cursor, Samples(ToQ14(tables::kI3dl2DecayDelay1[i]), k));
        state_.decay2[i] = MakeLine(cursor, Samples(ToQ14(tables::kI3dl2DecayDelay2[i]), k));
    }
    state_.center = MakeLine(cursor, static_cast<int32_t>(5 * k) >> 14);
    const int32_t early = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(k * 0x640000) >> 14)) >> 14;
    state_.early = MakeLine(cursor, early);
    state_.early.write = early % (early | 1);
    if (lines_.size() < cursor)
    {
        lines_.resize(cursor);
    }

    for (std::size_t i = 0; i < 4; ++i)
    {
        Clear(state_.fdn[i]);
        Clear(state_.decay1[i]);
        Clear(state_.decay2[i]);
    }
    Clear(state_.center);
    Clear(state_.early);
    Update(true);
}

void AuxI3dl2Reverb::Update(bool reset)
{
    const I3dl2ReverbParameterSet& p = parameters_;
    State& s = state_;
    s.dry_gain = p.dry_gain;
    s.shelf_state = {};
    s.lowpass = 0.0f;

    // The early reflections' and the late reverb's gains: the room's gain plus the reflections' gain or the reverb
    // gain, held to 5000 mB.
    s.early_gain = Power(ToQ14(Fmin(p.room_gain + p.reflections_gain, 5000.0f) / 2000.0f));
    s.late_gain = Power(ToQ14(Fmin(p.room_gain + p.reverb_gain, 5000.0f) / 2000.0f));

    // The input's low-pass, a one-pole filter for the room's loss of high frequencies, from the root of a quadratic.
    const int32_t room_hf = ToQ14(p.room_hf_gain / 2000.0f);
    const float q = Power(room_hf);
    if (room_hf >= 0 || q >= 1.0f)
    {
        s.lowpass_feedback = 0.0f;
        s.lowpass_input = 1.0f;
    }
    else
    {
        const auto rate = static_cast<float>(static_cast<uint32_t>(sample_rate_));
        const int32_t angle = ToQ14(p.hf_reference * 256.0f / rate);
        const float negative_cosine = static_cast<float>(Sine(WrapAdd(angle, kQuarterTurn))) * -0x1p-14f;
        const float one_minus = 1.0f - q;
        const float b = negative_cosine * (q + q) + 2.0f;
        const float discriminant = b * b + one_minus * (one_minus * -4.0f);
        const float root = static_cast<float>(SquareRoot(ToQ14(discriminant))) * -0x1p-14f;
        const float feedback = Fmin((b + root) / (one_minus + one_minus), 0x1.fe9456p-1f);
        s.lowpass_feedback = feedback;
        s.lowpass_input = 1.0f - feedback;
    }

    // Where the late reverb takes the input: after the reflections' delay and then the late reverb's delay.
    const int64_t k = KilohertzQ14(sample_rate_);
    s.late_tap = Samples(ToQ14((p.reflections_delay + p.late_reverb_delay) * 1000.0f), k);

    // Each feedback line's delay, between the line's shortest and longest delays by the density, and the line's shelf
    // filter. The filter makes the line's loop decay by 60 dB in the decay time, and faster above the reference
    // frequency by the ratio.
    const float echo = p.late_reverb_diffusion * 0.6f * 0.01f;
    const auto rate = static_cast<float>(static_cast<uint32_t>(sample_rate_));
    for (std::size_t i = 0; i < 4; ++i)
    {
        DelayLine& fdn = s.fdn[i];
        const float least = tables::kI3dl2FdnMinDelay[i];
        const float time = least + p.late_reverb_density / 100.0f * (tables::kI3dl2FdnMaxDelay[i] - least);
        // A delay below 0, from a density below 0, would put the line's write position before its first cell. A line
        // keeps its delay when the new one is below 0 or too long for the line.
        int32_t delay = Samples(ToQ14(time), k);
        if (delay >= 0 && fdn.length >= delay)
        {
            const int64_t position = static_cast<int64_t>(fdn.read) + delay;
            fdn.write = static_cast<int32_t>(position % (static_cast<int64_t>(fdn.length) + 1));
            fdn.delay = delay;
        }
        else
        {
            delay = fdn.delay;
        }

        const float half_angle = p.hf_reference * 0.5f * 128.0f / rate;
        const int32_t total = WrapAdd(WrapAdd(delay, s.decay1[i].delay), s.decay2[i].delay);
        const float a = static_cast<float>(static_cast<uint32_t>(total)) * -60.0f / (p.late_reverb_decay_time * rate);
        const int32_t angle = ToQ14(half_angle);
        const float cosine = static_cast<float>(Sine(WrapAdd(angle, kQuarterTurn))) * 0x1p-14f;
        const float b = a / p.late_reverb_hf_decay_ratio;
        const float sine = static_cast<float>(Sine(angle)) * 0x1p-14f;
        const float d = Power(ToQ14((b - a) / 40.0f));
        const float e = Power(ToQ14((a + b) / 40.0f)) * 0.7071f;
        const float c = cosine / sine;
        const float cd = c * d;
        s.shelf_feedback[i] = (c - d) / (c + d);
        s.shelf_gain[i] = (cd + 1.0f) * e / (c + d);
        s.shelf_feedforward[i] = (1.0f - cd) * e / (c + d);
        s.decay1[i].gain = echo;
        s.decay2[i].gain = echo * -0.9f;
    }

    if (reset)
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            Clear(s.fdn[i]);
            Clear(s.decay1[i]);
            Clear(s.decay2[i]);
        }
        Clear(s.early);
        Clear(s.center);
    }

    // The early reflections' taps, from the reflections' delay over most of the late reverb's delay, held to the line.
    const float late_time = (p.late_reverb_delay * 0.9998f + 0.02f) * 1000.0f;
    for (std::size_t i = 0; i < s.early_taps.size(); ++i)
    {
        const float time = p.reflections_delay * 1000.0f + late_time * tables::kI3dl2EarlyTapTime[i];
        const int32_t tap = Samples(ToQ14(time), k);
        s.early_taps[i] = static_cast<uint32_t>(tap) < static_cast<uint32_t>(s.early.length) ? tap : s.early.length;
    }
}

float AuxI3dl2Reverb::AllPassTick(DelayLine& first, DelayLine& second, DelayLine& fdn, float x)
{
    // A line's all-pass: the line takes x less the delayed sample times the gain, and gives that times the gain plus
    // the delayed sample, read after the write.
    const auto tick = [this](DelayLine& line, float input)
    {
        float* cells = lines_.data() + line.start;
        const float u = input - line.gain * cells[line.read];
        cells[line.write] = u;
        const float delayed = cells[line.read];
        line.write = line.write >= line.length ? 0 : line.write + 1;
        line.read = line.read >= line.length ? 0 : line.read + 1;

        return line.gain * u + delayed;
    };

    const float y = tick(second, tick(first, x));
    lines_[fdn.start + static_cast<std::size_t>(fdn.write)] = y;
    fdn.write = fdn.write >= fdn.length ? 0 : fdn.write + 1;
    fdn.read = fdn.read >= fdn.length ? 0 : fdn.read + 1;

    return y;
}

void AuxI3dl2Reverb::Apply2ch(const int32_t* in0, const int32_t* in1, int32_t* out0, int32_t* out1, uint32_t count)
{
    State& s = state_;

    // A sample of the input's line, `delay` + 1 samples before the line's write position. The line wraps once. A delay
    // longer than the line would read outside the line. No parameters give such a delay. Here it is held to the buffer.
    const auto tap = [&](uint32_t delay)
    {
        int64_t index = static_cast<int64_t>(s.early.start) + s.early.write - (static_cast<int64_t>(delay) + 1);
        if (index < static_cast<int64_t>(s.early.start))
        {
            index += static_cast<int64_t>(s.early.length) + 1;
        }
        index = std::clamp<int64_t>(index, 0, static_cast<int64_t>(lines_.size()) - 1);

        return lines_[static_cast<std::size_t>(index)];
    };

    for (uint32_t n = 0; n < count; ++n)
    {
        // The early reflections, before the sample goes in.
        const float late_input = tap(static_cast<uint32_t>(s.late_tap));
        float left = 0.0f;
        for (const int t : kLeftTaps)
        {
            left = left + tap(static_cast<uint32_t>(s.early_taps[t])) * tables::kI3dl2EarlyGain2ch[t];
        }
        float right = 0.0f;
        for (const int t : kRightTaps)
        {
            right = right + tap(static_cast<uint32_t>(s.early_taps[t])) * tables::kI3dl2EarlyGain2ch[t];
        }

        // The two channels' sum through the low-pass, into the input's line.
        const int32_t sum = WrapAdd(in0[n], in1[n]);
        s.lowpass = static_cast<float>(sum) * 0x1p-16f * s.lowpass_input + s.lowpass * s.lowpass_feedback;
        lines_[s.early.start + static_cast<std::size_t>(s.early.write)] = s.lowpass;
        s.early.write = s.early.write >= s.early.length ? 0 : s.early.write + 1;
        s.early.read = s.early.read >= s.early.length ? 0 : s.early.read + 1;
        const float early_left = s.early_gain * left;
        const float early_right = s.early_gain * right;

        // The late reverb: each feedback line through its shelf filter, mixed by a Hadamard matrix with the input, then
        // through the line's two all-passes and back into it.
        std::array<float, 4> y{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            const float r = lines_[s.fdn[i].start + static_cast<std::size_t>(s.fdn[i].read)];
            y[i] = r * s.shelf_gain[i] + s.shelf_state[i];
            s.shelf_state[i] = r * s.shelf_feedforward[i] + y[i] * s.shelf_feedback[i];
        }
        const float late = late_input * s.late_gain;
        const float a0 = AllPassTick(s.decay1[0], s.decay2[0], s.fdn[0], late + (y[1] + y[2]));
        const float a1 = AllPassTick(s.decay1[1], s.decay2[1], s.fdn[1], late + (-y[0] - y[3]));
        AllPassTick(s.decay1[2], s.decay2[2], s.fdn[2], late + (y[0] - y[3]));
        AllPassTick(s.decay1[3], s.decay2[3], s.fdn[3], late + (y[1] - y[2]));

        // Each channel: its lines and reflections, and its input at the dry gain.
        out0[n] = ToSample(a0 + early_left + s.dry_gain * (static_cast<float>(in0[n]) * 0x1p-16f));
        out1[n] = ToSample(a1 + early_right + s.dry_gain * (static_cast<float>(in1[n]) * 0x1p-16f));
    }
}

} // namespace ntdlib::nx::audio
