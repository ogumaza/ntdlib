// SPDX-License-Identifier: MIT

#include "enhanced_mix.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::rd1
{
namespace
{

// A level below this ends the echo's history.
constexpr float kSilence = 1.0f / 65536.0f;

// The coefficient at the mode's rate of a one-pole stage y[n] = c y[n - 1] + (1 - c) x[n] with coefficient `c` at the
// console's rate. `ratio` is the console's samples in each of the mode's. At low frequencies, the stage delays its
// input by c / (1 - c) samples. The coefficient keeps that delay the same length in time. A coefficient of c to the
// power of `ratio` would keep the time of the stage's decay instead. Through a fast high-pass of the console, such a
// stage passes about 5 dB more at 1 kHz.
float Coefficient(double c, double ratio)
{
    return float(c / (c + ratio * (1.0 - c)));
}

} // namespace

EnhancedMix::EnhancedMix(const Rom& rom, uint32_t period, uint64_t lead) : rom_(rom), period_(period)
{
    ratio_ = double(agb::kSecondCycles) / (double(period) * double(agb::kEnhancedRate));

    // The console's sample k plays from the timer's overflow k + 1.
    console_ = lead + 1;
    lead_ = size_t(Scaled(console_));
}

uint64_t EnhancedMix::Scaled(uint64_t console_samples) const
{
    return console_samples * period_ * uint64_t(agb::kEnhancedRate) / agb::kSecondCycles;
}

float EnhancedMix::Past(size_t side, uint64_t delay, size_t i) const
{
    const uint64_t at = written_ + i;
    const std::vector<float>& line = history_[side];
    if (at < delay || line.empty())
    {
        return 0.0f;
    }

    return line[size_t((at - delay) % line.size())];
}

void EnhancedMix::Frame(const std::vector<MixVoice>& voices, const MixSettings& settings, size_t samples, bool heard)
{
    const uint64_t first = Scaled(console_);
    console_ += samples;
    const size_t n = size_t(Scaled(console_) - first);
    for (std::vector<float>& side : out_)
    {
        side.assign(n, 0.0f);
    }
    parts_ = {};

    // The echo's delays: a ring's length on FIFO A, less the offset on FIFO B. A delay shorter than a frame reads the
    // frame before.
    const uint64_t ring = settings.ring_bytes;
    const uint64_t offset = settings.echo_offset < ring ? settings.echo_offset : 0;
    const uint64_t frame_max = Scaled(uint64_t(samples) + 64) + 1;
    const std::array<uint64_t, 2> delays = {std::max(Scaled(ring), frame_max),
                                            std::max(Scaled(ring - offset), frame_max)};
    const size_t length = size_t(std::max(delays[0], delays[1]) + frame_max);
    for (std::vector<float>& line : history_)
    {
        if (line.size() < length)
        {
            line.assign(length, 0.0f);
            written_ = 0;
        }
    }

    if (heard)
    {
        // The echo first, into the part that goes through the filter.
        std::array<std::vector<float>, 2> filtered = {std::vector<float>(n, 0.0f), std::vector<float>(n, 0.0f)};
        const uint32_t shift = std::min(settings.echo_shift, 31u);
        const uint32_t shift_2 = std::min(settings.echo_shift_2, 31u);
        const float low = Coefficient(1.0 - std::ldexp(1.0, -int(shift)), ratio_);
        const float band = Coefficient(1.0 - std::ldexp(1.0, -int(shift_2)), ratio_);
        const float level = float(uint64_t(settings.echo_level >> shift_2) << shift_2) / 128.0f;
        for (size_t side = 0; side < 2; side++)
        {
            for (size_t i = 0; i < n; i++)
            {
                const float x = Past(side, delays[side], i);
                echo_low_[side] = echo_low_[side] * low + x * (1.0f - low);
                const float high = x - echo_low_[side];
                echo_band_[side] = echo_band_[side] * band + high * (1.0f - band);
                filtered[side][i] = echo_band_[side] * level;
            }
        }

        // The voices.
        bool any_filtered = false;
        for (const MixVoice& voice : voices)
        {
            MixVoiceInto(voice, settings, samples, voice.filtered ? filtered : out_);
            any_filtered = any_filtered || voice.filtered;
        }

        // The filter of CC72. The console's state starts at 0 when the filter turns into a high-pass, and at the
        // output when it turns back.
        const bool high = settings.filter & 0x80;
        if (high != filter_high_)
        {
            for (size_t side = 0; side < 2; side++)
            {
                filter_state_[side] = high ? 0.0f : Past(side, 1, 0);
            }
            filter_high_ = high;
        }

        if (any_filtered)
        {
            const float coefficient = Coefficient(double((settings.filter & 0x7F) << 1) / 256.0, ratio_);
            for (size_t side = 0; side < 2; side++)
            {
                float& state = filter_state_[side];
                for (float& x : filtered[side])
                {
                    state = x * (1.0f - coefficient) + state * coefficient;
                    x = high ? x - state : state;
                }
            }
        }

        for (size_t side = 0; side < 2; side++)
        {
            for (size_t i = 0; i < n; i++)
            {
                out_[side][i] += filtered[side][i];
            }
        }
    }
    else
    {
        for (VoiceState& state : voices_)
        {
            state.known = false;
        }
    }

    for (size_t side = 0; side < 2; side++)
    {
        std::vector<float>& line = history_[side];
        for (size_t i = 0; i < n; i++)
        {
            const float x = out_[side][i];
            line[size_t((written_ + i) % line.size())] = std::fabs(x) < kSilence ? 0.0f : x;
        }
    }
    written_ += n;
}

void EnhancedMix::MixVoiceInto(const MixVoice& voice, const MixSettings& settings, size_t samples,
                               std::array<std::vector<float>, 2>& target)
{
    const std::array<uint8_t, kVoiceRecordBytes>& r = voice.record;
    const uint32_t volume = r[1];
    const uint32_t filter = settings.filter;
    const uint32_t gain =
        voice.filtered && (filter & 0x80) ? uint32_t(volume * (256 - filter) * settings.filter_gain) << 9 >> 16 : 0;
    const float level = float(volume + gain) / 16384.0f;
    const std::array<float, 2> to = {float(int8_t(r[3])) * level, float(int8_t(r[2])) * level};

    // The plain routine reads a sample for each of the console's, from the place's whole part.
    const bool stepped = r[0] & 2;
    const uint32_t base = RecordWord(r, 4);
    const uint32_t place = stepped ? RecordWord(r, 0xC) : RecordWord(r, 0xC) & ~0x3FFFu;
    const uint32_t loop = RecordWord(r, 0x10);
    const uint32_t end = RecordWord(r, 0x14);
    const uint32_t step = stepped ? RecordWord(r, 0x18) : 1u << 14;
    const uint64_t move = uint64_t(step) * samples;

    VoiceState& state = voices_[size_t(voice.voice) % voices_.size()];
    const bool goes_on = state.known && state.base == base && state.next == place;
    const std::array<float, 2> from = goes_on ? state.levels : to;
    uint64_t next = place + move;
    if (next >= end)
    {
        next = end > loop ? loop + (next - end) % (end - loop) : end;
    }
    state = {true, base, uint32_t(next), to};

    const size_t n = target[0].size();
    if (n == 0 || move == 0)
    {
        return;
    }

    const int64_t whole_end = int64_t(end >> 14);
    const int64_t whole_loop = int64_t(loop >> 14);
    const auto point_of = [&](int64_t at)
    {
        if (at < 0)
        {
            return 0.0f;
        }

        if (at >= whole_end)
        {
            if (whole_end <= whole_loop)
            {
                return 0.0f;
            }

            at = whole_loop + (at - whole_loop) % (whole_end - whole_loop);
        }

        return float(rom_.S8(base + uint32_t(at)));
    };

    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(n) * 16384.0));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(place >> 14) - (width - 1);
    const int64_t last = int64_t((uint64_t(place) + move) >> 14) + width;
    std::vector<float> input(size_t(last - first + 1));
    for (int64_t k = first; k <= last; k++)
    {
        input[size_t(k - first)] = point_of(k);
    }

    // Places in 2^-30ths of a sample.
    const uint64_t start = uint64_t(place) << 16;
    const uint64_t advance = (move << 16) / n;
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(n), (to[1] - from[1]) / float(n)};
    std::array<float, 2>& part = parts_[size_t(std::clamp(voice.track, 0, kMaxChannels - 1))];
    for (size_t i = 0; i < n; i++)
    {
        const uint64_t at = start + uint64_t(i) * advance;
        const int64_t whole = int64_t(at >> 30);
        const float value = kernel.At(&input[size_t(whole - (width - 1) - first)], uint32_t(at >> 14) & 0xFFFF);
        for (size_t side = 0; side < 2; side++)
        {
            const float x = value * (from[side] + slopes[side] * float(i));
            target[side][i] += x;
            if (voice.track >= 0)
            {
                part[side] = std::max(part[side], std::fabs(x));
            }
        }
    }
}

} // namespace ntdlib::rd1
