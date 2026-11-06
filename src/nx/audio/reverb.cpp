// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/reverb.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "nx/audio/arithmetic.h"
#include "nx/audio/fixed_point.h"
#include "nx/audio/tables.h"

namespace ntdlib::nx::audio
{
namespace
{

// The longest delays in ms in Q14 that the buffer holds for the feedback lines and their all-passes. The first
// feedback line's is 1/16 ms longer than any mode gives it.
constexpr std::array<int32_t, kReverbLines> kMostFdnDelay = {884994, 1297491, 1904456, 2795361};
constexpr std::array<int32_t, kReverbLines> kMostAllPassDelay = {7 << 14, 9 << 14, 13 << 14, 17 << 14};

// The pre-delay line's length in ms in Q14, from the renderer's revision 3 on and before it, and the centre line's.
constexpr int32_t kLongPredelay = 350 << 14;
constexpr int32_t kShortPredelay = 150 << 14;
constexpr int32_t kCenterDelay = 5 << 14;

// Each early mode's entries in the table of delays: the taps, then the late reverb's tap.
constexpr int kEarlyDelays = kReverbTaps + 1;

// The all-passes' gain without coloration (0.6), the largest high-frequency decay ratio that still filters (a little
// under 0.995), the feedback lines' gain (1 / sqrt 2), the LFE's share of the early reflections (0.2), and the
// centre's share of the third and fourth lines (0.5), all in Q14.
constexpr int32_t kAllPassScale = 9830;
constexpr int32_t kMostFilteredRatio = 16301;
constexpr int32_t kFdnScale = 11585;
constexpr int32_t kLfeScale = 3276;
constexpr int32_t kCenterScale = 8192;

// The angle whose cosine the filters take: 1280 over the sample rate in kHz, in 256ths of a turn, in Q14.
constexpr int32_t kAngle = 1280 << 14;

// The channel that each early reflection's tap goes to, for 1, 2, 4 and 6 channels. With 6, every tap goes to the LFE
// too.
constexpr std::array<int, kReverbTaps> kTaps1 = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<int, kReverbTaps> kTaps2 = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1};
constexpr std::array<int, kReverbTaps> kTaps4 = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3};
constexpr std::array<int, kReverbTaps> kTaps6 = {0, 0, 1, 1, 2, 2, 4, 4, 5, 5};

// The channels that the early reflections' taps go to, for a reverb of `channels` channels.
const std::array<int, kReverbTaps>& TapChannels(int channels)
{
    switch (channels)
    {
    case 1:
        return kTaps1;
    case 2:
        return kTaps2;
    case 4:
        return kTaps4;
    default:
        return kTaps6;
    }
}

// The low 32 bits of a * gain shifted right by 14, the product in 64 bits.
int32_t MulQ14(int64_t a, int64_t gain)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(a * gain) >> 14));
}

// A sample on the reverb's scale: shifted left by 6, wrapping.
int32_t Scaled(int32_t sample)
{
    return static_cast<int32_t>(static_cast<uint32_t>(sample) << 6);
}

// A line of `count` samples at `cursor` in the buffer. The next line follows it.
ReverbLine MakeLine(int64_t& cursor, int32_t count)
{
    ReverbLine line;
    line.most = static_cast<uint32_t>(count);
    line.length = line.most;
    line.start = cursor;
    line.cursor = cursor;
    cursor += count;

    return line;
}

// Gives a line a new length, at most its longest, and starts it again at its first cell.
void SetLength(ReverbLine& line, int32_t count)
{
    line.length = std::min(static_cast<uint32_t>(count), line.most);
    line.cursor = line.start;
}

// The cell at `index`, or 0 outside the buffer. Nintendo's code reads whatever memory is there. No parameters that a
// client gives reach it.
int32_t At(const ReverbState& state, int64_t index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= state.buffer.size())
    {
        return 0;
    }

    return state.buffer[static_cast<std::size_t>(index)];
}

// Writes the line's next cell and moves on, back to the first cell after the last.
void Write(ReverbState& state, ReverbLine& line, int32_t value)
{
    if (line.cursor >= 0 && static_cast<std::size_t>(line.cursor) < state.buffer.size())
    {
        state.buffer[static_cast<std::size_t>(line.cursor)] = value;
    }
    ++line.cursor;
    if (line.cursor > line.start + static_cast<int64_t>(line.length) - 1)
    {
        line.cursor = line.start;
    }
}

// The cell `delay` before the line's next cell, wrapping once.
int32_t Tap(const ReverbState& state, const ReverbLine& line, int32_t delay)
{
    int64_t index = line.cursor - delay;
    if (index < line.start)
    {
        index += line.length;
    }

    return At(state, index);
}

// Clears the cells that a line uses. The cells beyond its length keep what they held.
void Clear(ReverbState& state, const ReverbLine& line)
{
    const auto first = static_cast<std::size_t>(line.start);
    const std::size_t last = std::min(first + line.length, state.buffer.size());
    std::fill(state.buffer.begin() + static_cast<std::ptrdiff_t>(first),
              state.buffer.begin() + static_cast<std::ptrdiff_t>(last), 0);
}

// A mode as an index into the tables. Nintendo's code takes the mode as it is. A client gives 0 to 4.
std::size_t Mode(int32_t mode)
{
    return static_cast<std::size_t>(std::clamp(mode, 0, kReverbModes - 1));
}

// The gains of a feedback line whose loop, through the line and its all-pass, lasts `samples`: the gain that makes the
// loop die away by 60 dB in the decay time, and the one-pole filter that makes it die away faster above about 5 / 256
// of the sample rate, by the high-frequency decay ratio. Gives the line's gain and the filter's.
struct LineGains
{
    int32_t fdn;
    int32_t filter;
};

LineGains Gains(const ReverbParameter& parameter, int32_t samples, int32_t cosine)
{
    // The loop's decay in dB, over 20, per decay time: -60 dB across the loop's samples.
    const int64_t k = parameter.sample_rate;
    const int32_t decay = MulQ14(parameter.decay_time, k);
    const int32_t loop = Divide(WrapMul(samples, -0xC000), decay);

    // Without filtering, the filter passes everything. Otherwise the filter's pole is the root of a quadratic.
    int32_t filter = 0;
    if (parameter.high_frequency_decay_ratio <= kMostFilteredRatio)
    {
        const int32_t tenths = Divide(loop, 10 << 14);
        const int32_t inverse = Divide(0x4000, parameter.high_frequency_decay_ratio);
        const int32_t excess = WrapSub(WrapMul(inverse, 2), 0x8000);
        const int32_t e = PowerQ14(MulQ14(Divide(excess, 100 << 14), tenths));
        const int32_t f = WrapSub(0x4000, e);
        const int32_t g = WrapSub(0x8000, MulQ14(cosine, WrapMul(e, 2)));
        const auto four_f_squared =
            static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(int64_t{f} * f) >> 12) & ~3u);
        const int32_t root = SquareRoot(WrapSub(MulQ14(g, g), four_f_squared));
        const int32_t numerator = WrapSub(g, root);
        if (f != 0 && numerator != 0)
        {
            filter = Divide(numerator, WrapMul(f, 2));
        }
    }

    const int32_t gain = MulQ14(WrapSub(0x4000, filter), PowerQ14(Divide(loop, 1000 << 14)));

    return {MulQ14(gain, kFdnScale), filter};
}

} // namespace

void InitializeReverb(const ReverbParameter& parameter, ReverbState& state, bool long_predelay)
{
    std::vector<int32_t> buffer = std::move(state.buffer);
    state = ReverbState{};
    state.buffer = std::move(buffer);

    // Each feedback line and its all-pass at their longest, then the pre-delay line and the centre line, one after
    // the other.
    const int64_t k = parameter.sample_rate;
    int64_t cursor = 0;
    for (std::size_t i = 0; i < kReverbLines; ++i)
    {
        state.fdn[i] = MakeLine(cursor, Samples(kMostFdnDelay[i], k));
        state.all_pass[i] = MakeLine(cursor, Samples(kMostAllPassDelay[i], k));
    }
    state.predelay = MakeLine(cursor, Samples(long_predelay ? kLongPredelay : kShortPredelay, k));
    state.center = MakeLine(cursor, Samples(kCenterDelay, k));
    if (cursor > 0 && state.buffer.size() < static_cast<std::size_t>(cursor))
    {
        state.buffer.resize(static_cast<std::size_t>(cursor), 0);
    }

    // The lengths that the modes give, then the cells the lines use, cleared. Only 6 channels use the centre line.
    UpdateReverb(parameter, state);
    Clear(state, state.predelay);
    if (parameter.channel_count == 6)
    {
        Clear(state, state.center);
    }
    for (std::size_t i = 0; i < kReverbLines; ++i)
    {
        Clear(state, state.fdn[i]);
        Clear(state, state.all_pass[i]);
    }
}

void UpdateReverb(const ReverbParameter& parameter, ReverbState& state)
{
    const int64_t k = parameter.sample_rate;
    const std::size_t early = Mode(parameter.early_mode) * kEarlyDelays;
    const std::size_t late = Mode(parameter.late_mode) * kReverbLines;
    const auto most = static_cast<int32_t>(state.predelay.most);

    // The early reflections: each tap's delay after the pre-delay, at most the pre-delay line's length, and its gain.
    // Two channels take half the fifth and sixth taps.
    for (std::size_t i = 0; i < kReverbTaps; ++i)
    {
        const int32_t delay = Samples(int64_t{tables::kReverbEarlyDelay[early + i]} + parameter.predelay, k);
        state.early_delays[i] = std::min(delay, most) + 1;
        state.early_gains[i] =
            MulQ14(parameter.early_gain, tables::kReverbEarlyGain[Mode(parameter.early_mode) * kReverbTaps + i]);
    }
    if (parameter.channel_count == 2)
    {
        state.early_gains[4] >>= 1;
        state.early_gains[5] >>= 1;
    }

    // The late reverb's tap, the all-passes' gain, and the cosine that the filters take. Nintendo's code works the
    // cosine out once, from the first reverb that it updates, and keeps it. Every renderer of a game has the same rate.
    const int32_t delay = Samples(int64_t{tables::kReverbEarlyDelay[early + kReverbTaps]} + parameter.predelay, k);
    state.late_delay = std::min(delay, most);
    state.all_pass_gain = MulQ14(0x4000 - int64_t{parameter.coloration}, kAllPassScale);
    const int32_t cosine = Sine(WrapAdd(Divide(kAngle, parameter.sample_rate), kQuarterTurn));

    // Each feedback line's and all-pass's length by the late mode, from its first cell, and the line's gains.
    for (std::size_t i = 0; i < kReverbLines; ++i)
    {
        SetLength(state.fdn[i], Samples(tables::kReverbFdnDelay[late + i], k));
        SetLength(state.all_pass[i], Samples(tables::kReverbAllPassDelay[late + i], k));
        const auto samples = static_cast<int32_t>(state.fdn[i].length + state.all_pass[i].length);
        const LineGains gains = Gains(parameter, samples, cosine);
        state.fdn_gains[i] = gains.fdn;
        state.filter_gains[i] = gains.filter;
        state.filtered[i] = 0;
    }
}

void PassReverb(const ReverbParameter& parameter, std::span<const int32_t* const> inputs,
                std::span<int32_t* const> outputs, int count)
{
    const auto channels =
        std::min({static_cast<std::size_t>(std::max(parameter.channel_count, 0)), inputs.size(), outputs.size()});
    for (std::size_t k = 0; k < channels; ++k)
    {
        if (inputs[k] != outputs[k])
        {
            std::copy(inputs[k], inputs[k] + count, outputs[k]);
        }
    }
}

void ApplyReverb(const ReverbParameter& parameter, ReverbState& state, std::span<const int32_t* const> inputs,
                 std::span<int32_t* const> outputs, int count)
{
    const int channels = parameter.channel_count;
    if (channels != 1 && channels != 2 && channels != 4 && channels != 6)
    {
        PassReverb(parameter, inputs, outputs, count);
        return;
    }
    if (inputs.size() < static_cast<std::size_t>(channels) || outputs.size() < static_cast<std::size_t>(channels) ||
        state.buffer.empty())
    {
        return;
    }

    const std::array<int, kReverbTaps>& taps = TapChannels(channels);
    const auto c = static_cast<std::size_t>(channels);
    for (int t = 0; t < count; ++t)
    {
        // The early reflections, from the pre-delay line before the input goes in.
        std::array<int32_t, kReverbChannels> early{};
        for (std::size_t i = 0; i < kReverbTaps; ++i)
        {
            const int32_t v = MulQ14(Tap(state, state.predelay, state.early_delays[i]), state.early_gains[i]);
            const auto to = static_cast<std::size_t>(taps[i]);
            early[to] = WrapAdd(early[to], v);
            if (channels == 6)
            {
                early[3] = WrapAdd(early[3], v);
            }
        }
        if (channels == 6)
        {
            early[3] = MulQ14(early[3], kLfeScale);
        }

        // The input into the pre-delay line. Two and four channels scale each channel. Six scale their sum.
        std::array<int32_t, kReverbChannels> x{};
        int32_t sum = 0;
        int32_t in = 0;
        for (std::size_t k = 0; k < c; ++k)
        {
            x[k] = inputs[k][t];
            sum = WrapAdd(sum, x[k]);
            in = WrapAdd(in, MulQ14(Scaled(x[k]), parameter.reverb_gain));
        }
        if (channels == 6)
        {
            in = MulQ14(Scaled(sum), parameter.reverb_gain);
        }
        Write(state, state.predelay, in);

        // The late reverb's tap, just after the input went in, and each feedback line through its filter.
        const uint32_t length = state.predelay.length;
        const int32_t late_delay =
            length > static_cast<uint32_t>(state.late_delay) ? state.late_delay : static_cast<int32_t>(length - 1);
        const int32_t late = MulQ14(Tap(state, state.predelay, late_delay + 1), parameter.late_gain);
        std::array<int32_t, kReverbLines>& o = state.filtered;
        for (std::size_t i = 0; i < kReverbLines; ++i)
        {
            o[i] = WrapAdd(MulQ14(o[i], state.filter_gains[i]),
                           MulQ14(At(state, state.fdn[i].cursor), state.fdn_gains[i]));
        }

        // The lines mixed, each through its all-pass and back into its feedback line.
        const std::array<int32_t, kReverbLines> mixed = {
            WrapAdd(WrapAdd(o[1], late), o[2]),
            WrapSub(WrapSub(late, o[0]), o[3]),
            WrapSub(WrapAdd(o[0], late), o[3]),
            WrapSub(WrapAdd(o[1], late), o[2]),
        };
        std::array<int32_t, kReverbLines> a{};
        for (std::size_t i = 0; i < kReverbLines; ++i)
        {
            const int32_t r = At(state, state.all_pass[i].cursor);
            const int32_t u = WrapSub(mixed[i], MulQ14(r, state.all_pass_gain));
            Write(state, state.all_pass[i], u);
            a[i] = WrapAdd(r, MulQ14(u, state.all_pass_gain));
            Write(state, state.fdn[i], a[i]);
        }

        // The lines that each channel takes: all four for one channel, two each for two. Six channels give the centre
        // the third and fourth lines' difference, 5 ms late.
        std::array<int32_t, kReverbChannels> wet{};
        switch (channels)
        {
        case 1:
            wet[0] = WrapAdd(WrapAdd(WrapAdd(a[0], a[1]), a[2]), a[3]);
            break;
        case 2:
            wet = {WrapAdd(a[2], a[0]), WrapAdd(a[3], a[1])};
            break;
        case 4:
            wet = {a[0], a[1], a[2], a[3]};
            break;
        default:
            {
                const int32_t center = At(state, state.center.cursor);
                Write(state, state.center, MulQ14(WrapSub(a[2], a[3]), kCenterScale));
                wet = {a[0], a[1], center, a[3], a[2], a[3]};
                break;
            }
        }

        for (std::size_t k = 0; k < c; ++k)
        {
            const int32_t reverb = MulQ14(WrapAdd(early[k], wet[k]), parameter.out_gain) >> 6;
            outputs[k][t] = WrapAdd(MulQ14(x[k], parameter.dry_gain), reverb);
        }
    }
}

} // namespace ntdlib::nx::audio
