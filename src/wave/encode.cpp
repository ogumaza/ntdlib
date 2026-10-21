// SPDX-License-Identifier: MIT

#include "ntdlib/wave/encode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/wave/adpcm.h"

namespace ntdlib::wave
{
namespace
{

constexpr std::size_t kFrameSamples = 14;
constexpr std::size_t kFrameBytes = 8;
constexpr std::size_t kPairs = 8;
constexpr double kUnit = 2048.0; // a coefficient of 1 in the file's units
constexpr int kMaxScale = 15;    // the largest power of 2 that a header's low nibble holds
constexpr int kRounds = 32;      // the most rounds of the k-means method after each new pair

// The energy that a frame's error counts relative to: the frame's energy, plus that of a frame whose samples all lie 2
// from 0. A frame much quieter than that counts less.
constexpr double kQuietEnergy = 14.0 * 4.0;

// The sums of a frame's samples from which any pair's squared error on the frame follows: the products of each sample
// with itself and with the two samples before it, and those of the two samples before it.
struct FrameSums
{
    double r00 = 0.0; // x[n] x[n]
    double r01 = 0.0; // x[n] x[n - 1]
    double r02 = 0.0; // x[n] x[n - 2]
    double r11 = 0.0; // x[n - 1] x[n - 1]
    double r12 = 0.0; // x[n - 1] x[n - 2]
    double r22 = 0.0; // x[n - 2] x[n - 2]
};

// A pair of coefficients: the prediction of x[n] is c1 x[n - 1] + c2 x[n - 2].
struct Pair
{
    double c1 = 0.0;
    double c2 = 0.0;
};

// The squared error of the prediction of a frame by `pair`.
double ErrorOf(const FrameSums& sums, const Pair& pair)
{
    return sums.r00 - 2.0 * (pair.c1 * sums.r01 + pair.c2 * sums.r02) + pair.c1 * pair.c1 * sums.r11 +
           2.0 * pair.c1 * pair.c2 * sums.r12 + pair.c2 * pair.c2 * sums.r22;
}

// Adds `sums` times `weight` to `total`.
void AddSums(FrameSums& total, const FrameSums& sums, double weight)
{
    total.r00 += weight * sums.r00;
    total.r01 += weight * sums.r01;
    total.r02 += weight * sums.r02;
    total.r11 += weight * sums.r11;
    total.r12 += weight * sums.r12;
    total.r22 += weight * sums.r22;
}

// Whether the decoder's prediction with `pair` is stable: both roots of z^2 - c1 z - c2 lie in the unit circle or on
// it. Those pairs form the triangle with corners (0, 1), (2, -1) and (-2, -1).
bool Stable(const Pair& pair)
{
    return pair.c2 >= -1.0 && pair.c2 <= 1.0 && std::abs(pair.c1) <= 1.0 - pair.c2;
}

// The stable pair with the least squared error over `sums`. The error is a quadratic function of the pair. Its least
// value lies where its gradient is 0, or on an edge of the triangle when that place lies outside.
Pair BestPair(const FrameSums& sums)
{
    const double determinant = sums.r11 * sums.r22 - sums.r12 * sums.r12;
    if (determinant > 1e-9 * sums.r11 * sums.r22 && determinant > 0.0)
    {
        const Pair inside = {(sums.r01 * sums.r22 - sums.r02 * sums.r12) / determinant,
                             (sums.r02 * sums.r11 - sums.r01 * sums.r12) / determinant};
        if (Stable(inside))
        {
            return inside;
        }
    }

    // Along each edge from corner a to corner b, the error is e(a) + 2 t g + t^2 h at a + t (b - a).
    constexpr std::array<Pair, 3> kCorners = {{{0.0, 1.0}, {2.0, -1.0}, {-2.0, -1.0}}};
    Pair best;
    double least = ErrorOf(sums, best);
    for (std::size_t i = 0; i < kCorners.size(); i++)
    {
        const Pair& a = kCorners[i];
        const Pair& b = kCorners[(i + 1) % kCorners.size()];
        const double d1 = b.c1 - a.c1;
        const double d2 = b.c2 - a.c2;
        const double g =
            d1 * (sums.r11 * a.c1 + sums.r12 * a.c2 - sums.r01) + d2 * (sums.r12 * a.c1 + sums.r22 * a.c2 - sums.r02);
        const double h = d1 * d1 * sums.r11 + 2.0 * d1 * d2 * sums.r12 + d2 * d2 * sums.r22;
        const double t = h > 0.0 ? std::clamp(-g / h, 0.0, 1.0) : (g < 0.0 ? 1.0 : 0.0);
        const Pair pair = {a.c1 + t * d1, a.c2 + t * d2};
        const double error = ErrorOf(sums, pair);
        if (error < least)
        {
            best = pair;
            least = error;
        }
    }

    return best;
}

// The sums of each frame of `samples`, and the weight of each frame's error. The samples before the first are 0.
std::vector<FrameSums> FramesOf(std::span<const int16_t> samples, std::vector<double>& weights)
{
    const std::size_t frames = (samples.size() + kFrameSamples - 1) / kFrameSamples;
    std::vector<FrameSums> sums(frames);
    weights.assign(frames, 0.0);
    for (std::size_t f = 0; f < frames; f++)
    {
        FrameSums& frame = sums[f];
        const std::size_t end = std::min(samples.size(), (f + 1) * kFrameSamples);
        for (std::size_t n = f * kFrameSamples; n < end; n++)
        {
            const double x0 = samples[n];
            const double x1 = n >= 1 ? samples[n - 1] : 0.0;
            const double x2 = n >= 2 ? samples[n - 2] : 0.0;
            frame.r00 += x0 * x0;
            frame.r01 += x0 * x1;
            frame.r02 += x0 * x2;
            frame.r11 += x1 * x1;
            frame.r12 += x1 * x2;
            frame.r22 += x2 * x2;
        }
        weights[f] = 1.0 / (frame.r00 + kQuietEnergy);
    }

    return sums;
}

// Gives each frame the pair in `pairs` with the least weighted error on it. Returns whether a frame changed its pair.
// `errors` takes each frame's weighted error.
bool Assign(const std::vector<FrameSums>& frames, const std::vector<double>& weights, const std::vector<Pair>& pairs,
            std::vector<std::size_t>& groups, std::vector<double>& errors)
{
    bool changed = false;
    for (std::size_t f = 0; f < frames.size(); f++)
    {
        std::size_t best = 0;
        double least = std::numeric_limits<double>::infinity();
        for (std::size_t p = 0; p < pairs.size(); p++)
        {
            const double error = weights[f] * ErrorOf(frames[f], pairs[p]);
            if (error < least)
            {
                least = error;
                best = p;
            }
        }
        changed = changed || groups[f] != best;
        groups[f] = best;
        errors[f] = least;
    }

    return changed;
}

// The channel's 8 pairs, in the file's units. Each new pair starts as the best pair of the frame with the largest
// weighted error under the pairs so far. Rounds of the k-means method follow each new pair.
std::array<int16_t, 16> PairsOf(std::span<const int16_t> samples)
{
    std::vector<double> weights;
    const std::vector<FrameSums> frames = FramesOf(samples, weights);
    FrameSums total;
    for (std::size_t f = 0; f < frames.size(); f++)
    {
        AddSums(total, frames[f], weights[f]);
    }

    std::vector<Pair> pairs = {BestPair(total)};
    std::vector<std::size_t> groups(frames.size(), 0);
    std::vector<double> errors(frames.size(), 0.0);
    Assign(frames, weights, pairs, groups, errors);
    while (pairs.size() < kPairs)
    {
        // The frame that the pairs so far predict worst.
        const auto worst = std::max_element(errors.begin(), errors.end());
        if (worst == errors.end() || *worst <= 0.0)
        {
            break;
        }

        pairs.push_back(BestPair(frames[static_cast<std::size_t>(worst - errors.begin())]));

        // The rounds: each frame goes to its best pair, then each pair becomes the best pair for its group.
        for (int round = 0; round < kRounds; round++)
        {
            if (!Assign(frames, weights, pairs, groups, errors) && round > 0)
            {
                break;
            }

            std::vector<FrameSums> group_sums(pairs.size());
            std::vector<bool> used(pairs.size(), false);
            for (std::size_t f = 0; f < frames.size(); f++)
            {
                AddSums(group_sums[groups[f]], frames[f], weights[f]);
                used[groups[f]] = true;
            }

            for (std::size_t p = 0; p < pairs.size(); p++)
            {
                if (used[p])
                {
                    pairs[p] = BestPair(group_sums[p]);
                }
            }
        }

        Assign(frames, weights, pairs, groups, errors);
    }

    // The file's units. A channel with fewer distinct frames than pairs leaves the other pairs at 0.
    const auto unit = [](double value)
    {
        return static_cast<int16_t>(std::clamp<long>(std::lround(value * kUnit), -32768, 32767));
    };
    std::array<int16_t, 16> coefs{};
    for (std::size_t p = 0; p < pairs.size(); p++)
    {
        coefs[2 * p] = unit(pairs[p].c1);
        coefs[2 * p + 1] = unit(pairs[p].c2);
    }

    return coefs;
}

// A frame encoded with one pair and one scale: its nibbles, its decoded samples and their squared error.
struct EncodedFrame
{
    std::array<int8_t, kFrameSamples> nibbles{};
    std::array<int16_t, kFrameSamples> decoded{};
    double error = 0.0;
};

// Encodes `frame`, the samples of a frame, with the pair (c1, c2) and scale `shift`, from the decoded samples `hist1`
// and `hist2` before it. In every form of DspAdpcmSum, a decoded sample is the prediction, (c1 hist1 + c2 hist2 +
// 1024) / 2048 rounded down, plus the nibble times the scale, clamped to 16 bits, while the sums stay within 32 bits.
// The nibble nearest to the sample's difference from the prediction then gives the closest decoded sample. Of two
// decoded samples equally close, it gives the higher. Samples past the frame's input count as 0 and add nothing to the
// error.
EncodedFrame EncodeFrame(std::span<const int16_t> frame, int32_t c1, int32_t c2, int32_t shift, int32_t hist1,
                         int32_t hist2, DspAdpcmSum sum)
{
    EncodedFrame encoded;
    const int64_t half = shift > 0 ? int64_t{1} << (shift - 1) : 0;
    for (std::size_t n = 0; n < kFrameSamples; n++)
    {
        const int64_t target = n < frame.size() ? frame[n] : 0;
        const int64_t prediction = (int64_t{c1} * hist1 + int64_t{c2} * hist2 + 1024) >> 11;
        const auto nibble = static_cast<int32_t>(std::clamp<int64_t>((target - prediction + half) >> shift, -8, 7));
        const int16_t sample = DspAdpcmSample(nibble, shift, c1, c2, hist1, hist2, sum);

        encoded.nibbles[n] = static_cast<int8_t>(nibble);
        encoded.decoded[n] = sample;
        if (n < frame.size())
        {
            const auto difference = static_cast<double>(target - sample);
            encoded.error += difference * difference;
        }
        hist2 = hist1;
        hist1 = sample;
    }

    return encoded;
}

// The smallest scale at which the nibbles reach the frame's largest difference from its prediction by the input's
// samples, with the pair (c1, c2) and the decoded samples `hist1` and `hist2` before the frame.
int32_t ScaleOf(std::span<const int16_t> frame, int32_t c1, int32_t c2, int32_t hist1, int32_t hist2)
{
    double largest = 0.0;
    double before1 = hist1;
    double before2 = hist2;
    for (const int16_t sample : frame)
    {
        const double prediction = (c1 * before1 + c2 * before2) / kUnit;
        largest = std::max(largest, std::abs(sample - prediction));
        before2 = before1;
        before1 = sample;
    }

    int32_t shift = 0;
    while (shift < kMaxScale && largest > 7.5 * std::ldexp(1.0, shift))
    {
        shift++;
    }

    return shift;
}

} // namespace

std::size_t DspAdpcmBytes(std::size_t samples)
{
    return (samples + kFrameSamples - 1) / kFrameSamples * kFrameBytes;
}

DspAdpcmChannel EncodeDspAdpcm(std::span<const int16_t> samples, uint32_t loop_start, DspAdpcmSum sum)
{
    if (samples.size() > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("too many samples for a wave");
    }

    // The pairs, then each frame with the pair and the scale that come closest.
    DspAdpcmChannel channel;
    channel.coefs = PairsOf(samples);
    channel.data.assign(DspAdpcmBytes(samples.size()), 0);
    std::vector<int16_t> decoded(samples.size());
    int32_t hist1 = 0;
    int32_t hist2 = 0;
    for (std::size_t f = 0; f * kFrameSamples < samples.size(); f++)
    {
        const std::span<const int16_t> frame =
            samples.subspan(f * kFrameSamples, std::min(kFrameSamples, samples.size() - f * kFrameSamples));
        EncodedFrame best;
        best.error = std::numeric_limits<double>::infinity();
        uint8_t header = 0;
        for (std::size_t p = 0; p < kPairs; p++)
        {
            const int32_t c1 = channel.coefs[2 * p];
            const int32_t c2 = channel.coefs[2 * p + 1];
            const int32_t scale = ScaleOf(frame, c1, c2, hist1, hist2);
            for (int32_t shift = std::max(0, scale - 1); shift <= std::min(kMaxScale, scale + 1); shift++)
            {
                const EncodedFrame encoded = EncodeFrame(frame, c1, c2, shift, hist1, hist2, sum);
                if (encoded.error < best.error)
                {
                    best = encoded;
                    header = static_cast<uint8_t>(p << 4 | static_cast<std::size_t>(shift));
                }
            }
        }

        // The frame's bytes, and the decoded samples that the next frame predicts from.
        uint8_t* bytes = &channel.data[f * kFrameBytes];
        bytes[0] = header;
        for (std::size_t n = 0; n < kFrameSamples; n += 2)
        {
            bytes[1 + n / 2] = static_cast<uint8_t>((best.nibbles[n] & 0xF) << 4 | (best.nibbles[n + 1] & 0xF));
        }
        std::copy(best.decoded.begin(), best.decoded.begin() + static_cast<std::ptrdiff_t>(frame.size()),
                  decoded.begin() + static_cast<std::ptrdiff_t>(f * kFrameSamples));
        hist1 = best.decoded[kFrameSamples - 1];
        hist2 = best.decoded[kFrameSamples - 2];
    }

    // The decoder's state at the start and at the loop's start. A loop's start past the last sample takes the last
    // frame's header.
    const std::size_t frames = channel.data.size() / kFrameBytes;
    const std::size_t start = std::min<std::size_t>(loop_start, samples.size());
    if (frames > 0)
    {
        channel.pred_scale = channel.data[0];
        channel.loop_pred_scale = channel.data[std::min(start / kFrameSamples, frames - 1) * kFrameBytes];
    }
    channel.loop_yn1 = start >= 1 ? decoded[start - 1] : 0;
    channel.loop_yn2 = start >= 2 ? decoded[start - 2] : 0;

    return channel;
}

uint32_t AlignDspAdpcmLoop(std::vector<std::vector<int16_t>>& channels, uint32_t loop_start)
{
    for (const std::vector<int16_t>& channel : channels)
    {
        if (loop_start >= channel.size())
        {
            throw std::invalid_argument("a loop that starts at or past the end of its samples");
        }
    }

    // The samples from the loop's start up to the next frame repeat after the loop's end. Each sample copied comes
    // from before the end of the channel as it grows: a loop shorter than the move repeats as often as it takes.
    const uint64_t aligned = (uint64_t{loop_start} + kFrameSamples - 1) / kFrameSamples * kFrameSamples;
    if (aligned > std::numeric_limits<uint32_t>::max())
    {
        throw std::invalid_argument("a loop that starts too late in a wave for a frame after it");
    }

    for (std::vector<int16_t>& channel : channels)
    {
        for (std::size_t i = loop_start; i < aligned; i++)
        {
            const int16_t sample = channel[i];
            channel.push_back(sample);
        }
    }

    return static_cast<uint32_t>(aligned);
}

} // namespace ntdlib::wave
