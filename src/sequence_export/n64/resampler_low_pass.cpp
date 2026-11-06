// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/n64/resampler_low_pass.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "ntdlib/n64/audio_rsp.h"

namespace ntdlib::sequence_export::n64
{

namespace
{

constexpr std::size_t kPhases = 64;
constexpr std::size_t kPhaseTaps = 4;
constexpr double kOne = 32768.0; // 1.0 in 1.15 format

// The points over half the rate at which the design reads the response.
constexpr int kDesignPoints = 256;

// The mean power response of the resampler's 64 filters is r[0] + 2 (r[1] cos w + r[2] cos 2w + r[3] cos 3w), where
// r[k] is the filters' mean autocorrelation at lag k. The symmetric filter's response is the root of that. Its taps
// are the cosine coefficients of the root, by the midpoint rule. They're rounded to 1.15, and the centre tap takes
// what makes the sum 1.0, so that the filter passes a constant level unchanged.
std::array<int16_t, kResamplerLowPassTaps> DesignTaps()
{
    const std::array<int16_t, kPhases * kPhaseTaps>& table = ntdlib::n64::AudioResampleTable();
    std::array<double, kPhaseTaps> correlation = {};
    for (std::size_t phase = 0; phase < kPhases; phase++)
    {
        for (std::size_t lag = 0; lag < kPhaseTaps; lag++)
        {
            for (std::size_t tap = 0; tap + lag < kPhaseTaps; tap++)
            {
                correlation[lag] += table[phase * kPhaseTaps + tap] / kOne *
                                    (table[phase * kPhaseTaps + tap + lag] / kOne) / static_cast<double>(kPhases);
            }
        }
    }

    std::array<double, kResamplerLowPassTaps> taps = {};
    for (int point = 0; point < kDesignPoints; point++)
    {
        const double w = (point + 0.5) * std::numbers::pi / kDesignPoints;
        double power = correlation[0];
        for (std::size_t lag = 1; lag < kPhaseTaps; lag++)
        {
            power += 2.0 * correlation[lag] * std::cos(static_cast<double>(lag) * w);
        }

        const double magnitude = std::sqrt(std::max(power, 0.0));
        for (std::size_t tap = 0; tap < kResamplerLowPassTaps; tap++)
        {
            taps[tap] += magnitude * std::cos(static_cast<double>(tap) * w) / kDesignPoints;
        }
    }

    std::array<int16_t, kResamplerLowPassTaps> out = {};
    int32_t sides = 0;
    for (std::size_t tap = 1; tap < kResamplerLowPassTaps; tap++)
    {
        out[tap] = static_cast<int16_t>(std::lround(taps[tap] * kOne));
        sides += out[tap];
    }

    out[0] = static_cast<int16_t>(static_cast<int32_t>(kOne) - 2 * sides);

    return out;
}

// Reads one channel of a wave as the filter sees it around one sample.
class Reader
{
public:
    Reader(const std::vector<int16_t>& samples, const seqlib::Pcm& pcm)
        : samples_(samples),
          size_(static_cast<int64_t>(samples.size())),
          loops_(pcm.loop && pcm.loop_start < pcm.loop_end && pcm.loop_end <= samples.size()),
          loop_start_(static_cast<int64_t>(pcm.loop_start)),
          loop_end_(static_cast<int64_t>(pcm.loop_end))
    {
    }

    // The sample at `place`, as the filter reads it for the sample at `at`.
    int32_t At(int64_t at, int64_t place) const
    {
        if (loops_ && at >= loop_start_ && at < loop_end_)
        {
            return samples_[static_cast<std::size_t>(InLoop(place))];
        }

        if (loops_ && at < loop_start_ && place >= loop_end_)
        {
            return samples_[static_cast<std::size_t>(InLoop(place))];
        }

        return place >= 0 && place < size_ ? samples_[static_cast<std::size_t>(place)] : 0;
    }

private:
    // The place in the loop that a repeat of it puts at `place`.
    int64_t InLoop(int64_t place) const
    {
        const int64_t length = loop_end_ - loop_start_;
        const int64_t offset = (place - loop_start_) % length;

        return loop_start_ + (offset < 0 ? offset + length : offset);
    }

    const std::vector<int16_t>& samples_;
    int64_t size_;
    bool loops_;
    int64_t loop_start_;
    int64_t loop_end_;
};

} // namespace

const std::array<int16_t, kResamplerLowPassTaps>& ResamplerLowPass()
{
    static const std::array<int16_t, kResamplerLowPassTaps> kTaps = DesignTaps();

    return kTaps;
}

seqlib::Pcm ApplyResamplerLowPass(const seqlib::Pcm& pcm, std::size_t* clipped)
{
    const std::array<int16_t, kResamplerLowPassTaps>& taps = ResamplerLowPass();
    seqlib::Pcm out = pcm;
    for (std::size_t channel = 0; channel < pcm.channels.size(); channel++)
    {
        const std::vector<int16_t>& samples = pcm.channels[channel];
        const Reader reader(samples, pcm);
        for (std::size_t i = 0; i < samples.size(); i++)
        {
            const auto at = static_cast<int64_t>(i);
            int64_t sum = int64_t{taps[0]} * reader.At(at, at);
            for (std::size_t tap = 1; tap < kResamplerLowPassTaps; tap++)
            {
                const auto step = static_cast<int64_t>(tap);
                sum += int64_t{taps[tap]} * (reader.At(at, at - step) + reader.At(at, at + step));
            }

            // Rounded to the nearest step, halves upwards.
            const int64_t value = (sum + static_cast<int64_t>(kOne) / 2) >> 15;
            const int64_t limited =
                std::clamp<int64_t>(value, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max());
            if (limited != value && clipped != nullptr)
            {
                (*clipped)++;
            }

            out.channels[channel][i] = static_cast<int16_t>(limited);
        }
    }

    return out;
}

} // namespace ntdlib::sequence_export::n64
