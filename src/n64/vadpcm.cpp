// SPDX-License-Identifier: MIT

#include "ntdlib/n64/vadpcm.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::n64
{

namespace
{

constexpr int kMaxScale = 12;
constexpr int kResidualMin = -8;
constexpr int kResidualMax = 7;

// The most samples that a search for exact residuals visits for a predictor and a scale. Runs of saturated samples
// allow many residuals. Only a clipped wave reaches this limit.
constexpr int kExactSearchSteps = 65536;

int16_t Saturate(int32_t value)
{
    return static_cast<int16_t>(
        std::clamp<int32_t>(value, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

// The part of a group's sample at `index` that doesn't depend on its residual.
int32_t Prediction(const VadpcmBook& book, int predictor, int index, int16_t second_last, int16_t last,
                   std::span<const int32_t> shifted)
{
    int32_t sum = book.Coefficient(predictor, 0, index) * second_last + book.Coefficient(predictor, 1, index) * last;
    for (int earlier = 0; earlier < index; earlier++)
    {
        sum += book.Coefficient(predictor, 1, index - earlier - 1) * shifted[static_cast<std::size_t>(earlier)];
    }

    return sum;
}

// The outcome of trying a predictor and a scale on a frame's samples.
struct Attempt
{
    std::array<int8_t, kVadpcmFrameSamples> residuals = {};
    int64_t squared_error = 0;
};

// The residuals nearest to the samples, one sample after another.
Attempt TryFrame(std::span<const int16_t, kVadpcmFrameSamples> samples, const VadpcmBook& book,
                 const VadpcmState& previous, int predictor, int scale)
{
    Attempt attempt;
    int16_t second_last = previous[kVadpcmFrameSamples - 2];
    int16_t last = previous[kVadpcmFrameSamples - 1];
    for (std::size_t group = 0; group < 2; group++)
    {
        std::array<int32_t, kVadpcmGroupSamples> shifted = {};
        std::array<int16_t, kVadpcmGroupSamples> decoded = {};
        for (int i = 0; i < static_cast<int>(kVadpcmGroupSamples); i++)
        {
            const std::size_t slot = group * kVadpcmGroupSamples + static_cast<std::size_t>(i);
            const int32_t prediction = Prediction(book, predictor, i, second_last, last, shifted);

            // The residual that gives the sample, rounded to the nearest multiple of the scale.
            const int32_t wanted = samples[slot] - (prediction >> kVadpcmCoefficientBits);
            const int32_t half = (1 << scale) >> 1;
            const int residual = std::clamp((wanted + half) >> scale, kResidualMin, kResidualMax);
            shifted[static_cast<std::size_t>(i)] = residual * (1 << scale);
            decoded[static_cast<std::size_t>(i)] =
                Saturate((prediction + shifted[static_cast<std::size_t>(i)] * (1 << kVadpcmCoefficientBits)) >>
                         kVadpcmCoefficientBits);
            attempt.residuals[slot] = static_cast<int8_t>(residual);

            const int64_t error = decoded[static_cast<std::size_t>(i)] - samples[slot];
            attempt.squared_error += error * error;
        }

        second_last = decoded[kVadpcmGroupSamples - 2];
        last = decoded[kVadpcmGroupSamples - 1];
    }

    return attempt;
}

// The frame of a predictor that gives the samples exactly, if none of them lies at a limit of 16 bits. The residuals'
// values follow from the samples. `energy` receives the sum of their squares. Of the scales that fit, a frame takes the
// highest, like the SDK's encoder. A frame of zeros takes scale 0.
bool ExactFrame(std::span<const int16_t, kVadpcmFrameSamples> samples, const VadpcmBook& book,
                const VadpcmState& previous, int predictor, VadpcmFrame& frame, int64_t& energy)
{
    std::array<int32_t, kVadpcmFrameSamples> values = {};
    int16_t second_last = previous[kVadpcmFrameSamples - 2];
    int16_t last = previous[kVadpcmFrameSamples - 1];
    int32_t any_bits = 0;
    energy = 0;
    for (std::size_t group = 0; group < 2; group++)
    {
        std::array<int32_t, kVadpcmGroupSamples> shifted = {};
        for (int i = 0; i < static_cast<int>(kVadpcmGroupSamples); i++)
        {
            const std::size_t slot = group * kVadpcmGroupSamples + static_cast<std::size_t>(i);
            if (samples[slot] == std::numeric_limits<int16_t>::max() ||
                samples[slot] == std::numeric_limits<int16_t>::min())
            {
                return false;
            }

            const int32_t prediction = Prediction(book, predictor, i, second_last, last, shifted);
            const int32_t value = samples[slot] - (prediction >> kVadpcmCoefficientBits);
            shifted[static_cast<std::size_t>(i)] = value;
            values[slot] = value;
            any_bits |= value;
            energy += static_cast<int64_t>(value) * value;
        }

        second_last = samples[group * kVadpcmGroupSamples + kVadpcmGroupSamples - 2];
        last = samples[group * kVadpcmGroupSamples + kVadpcmGroupSamples - 1];
    }

    // The lowest set bit of any value limits the scale.
    int scale = 0;
    while (any_bits != 0 && scale < kMaxScale && (any_bits & (1 << scale)) == 0)
    {
        scale++;
    }

    frame.scale = static_cast<uint8_t>(scale);
    frame.predictor = static_cast<uint8_t>(predictor);
    for (std::size_t slot = 0; slot < kVadpcmFrameSamples; slot++)
    {
        const int32_t residual = values[slot] >> scale;
        if (residual < kResidualMin || residual > kResidualMax)
        {
            return false;
        }

        frame.residuals[slot] = static_cast<int8_t>(residual);
    }

    return true;
}

// A search for residuals that give the samples exactly. A sample that isn't at a limit of 16 bits fixes its residual. A
// sample at a limit allows every residual that saturates there, and each one changes the samples after it.
class ExactSearch
{
public:
    ExactSearch(std::span<const int16_t, kVadpcmFrameSamples> samples, const VadpcmBook& book,
                const VadpcmState& previous, int predictor, int scale)
        : samples_(samples), book_(book), previous_(previous), predictor_(predictor), scale_(scale)
    {
    }

    bool Run()
    {
        std::array<int32_t, kVadpcmGroupSamples> shifted = {};

        return Step(0, shifted, previous_[kVadpcmFrameSamples - 2], previous_[kVadpcmFrameSamples - 1]);
    }

    const std::array<int8_t, kVadpcmFrameSamples>& Residuals() const
    {
        return residuals_;
    }

private:
    bool Step(std::size_t slot, std::array<int32_t, kVadpcmGroupSamples>& shifted, int16_t second_last, int16_t last)
    {
        if (slot == kVadpcmFrameSamples)
        {
            return true;
        }

        if (++steps_ > kExactSearchSteps)
        {
            return false;
        }

        // A new group takes its history from the group before and starts its residuals again.
        const int i = static_cast<int>(slot % kVadpcmGroupSamples);
        if (i == 0 && slot != 0)
        {
            shifted = {};
            second_last = samples_[slot - 2];
            last = samples_[slot - 1];
        }

        const int32_t prediction = Prediction(book_, predictor_, i, second_last, last, shifted);
        const int32_t wanted = samples_[slot] - (prediction >> kVadpcmCoefficientBits);
        const bool at_top = samples_[slot] == std::numeric_limits<int16_t>::max();
        const bool at_bottom = samples_[slot] == std::numeric_limits<int16_t>::min();
        const std::array<int32_t, kVadpcmGroupSamples> kept = shifted;
        // A sample at the bottom tries the residuals from the highest down. The nearest ones are the likeliest.
        for (int step = 0; step <= kResidualMax - kResidualMin; step++)
        {
            const int residual = at_bottom ? kResidualMax - step : kResidualMin + step;
            const int32_t value = residual * (1 << scale_);
            const bool fits = (at_top && value >= wanted) || (at_bottom && value <= wanted) || value == wanted;
            if (!fits)
            {
                continue;
            }

            shifted[static_cast<std::size_t>(i)] = value;
            residuals_[slot] = static_cast<int8_t>(residual);
            if (Step(slot + 1, shifted, second_last, last))
            {
                return true;
            }

            shifted = kept;
        }

        return false;
    }

    std::span<const int16_t, kVadpcmFrameSamples> samples_;
    const VadpcmBook& book_;
    const VadpcmState& previous_;
    int predictor_;
    int scale_;
    std::array<int8_t, kVadpcmFrameSamples> residuals_ = {};
    int steps_ = 0;
};

} // namespace

int16_t VadpcmBook::Coefficient(int predictor, int row, int sample) const
{
    const auto index =
        static_cast<std::size_t>((predictor * order + row) * static_cast<int>(kVadpcmGroupSamples) + sample);

    return coefficients[index];
}

bool VadpcmBook::Usable() const
{
    if (order != 2 || predictor_count <= 0 || predictor_count > 16)
    {
        return false;
    }

    const auto wanted =
        static_cast<std::size_t>(order) * static_cast<std::size_t>(predictor_count) * kVadpcmGroupSamples;

    return coefficients.size() == wanted;
}

namespace
{

constexpr uint8_t kFrameMaxScale = 12;
constexpr uint8_t kSmallFrameMaxScale = 14;

void DecodeResiduals(uint8_t scale, uint8_t predictor, std::span<const int8_t, kVadpcmFrameSamples> residuals,
                     const VadpcmBook& book, VadpcmState& state)
{
    if (predictor >= book.predictor_count)
    {
        throw xsflib::FormatError("a VADPCM frame names predictor " + std::to_string(predictor) + " of a book of " +
                                  std::to_string(book.predictor_count));
    }

    int16_t second_last = state[kVadpcmFrameSamples - 2];
    int16_t last = state[kVadpcmFrameSamples - 1];
    for (std::size_t group = 0; group < 2; group++)
    {
        std::array<int32_t, kVadpcmGroupSamples> shifted = {};
        std::array<int16_t, kVadpcmGroupSamples> decoded = {};
        for (int i = 0; i < static_cast<int>(kVadpcmGroupSamples); i++)
        {
            const std::size_t slot = group * kVadpcmGroupSamples + static_cast<std::size_t>(i);
            const int32_t prediction = Prediction(book, predictor, i, second_last, last, shifted);
            shifted[static_cast<std::size_t>(i)] = residuals[slot] * (1 << scale);
            decoded[static_cast<std::size_t>(i)] =
                Saturate((prediction + shifted[static_cast<std::size_t>(i)] * (1 << kVadpcmCoefficientBits)) >>
                         kVadpcmCoefficientBits);
            state[slot] = decoded[static_cast<std::size_t>(i)];
        }

        second_last = decoded[kVadpcmGroupSamples - 2];
        last = decoded[kVadpcmGroupSamples - 1];
    }
}

template <std::size_t kSize, typename Decode>
std::vector<int16_t> DecodeFrames(std::span<const uint8_t> frames, const VadpcmBook& book, VadpcmState& state,
                                  Decode decode)
{
    if (!book.Usable())
    {
        throw xsflib::FormatError("a VADPCM book that doesn't have an order of 2 and whole predictors");
    }

    const std::size_t frame_count = frames.size() / kSize;
    std::vector<int16_t> samples;
    samples.reserve(frame_count * kVadpcmFrameSamples);
    for (std::size_t frame = 0; frame < frame_count; frame++)
    {
        decode(std::span<const uint8_t, kSize>(frames.data() + frame * kSize, kSize), book, state);
        samples.insert(samples.end(), state.begin(), state.end());
    }

    return samples;
}

} // namespace

VadpcmFrame VadpcmFrame::Parse(std::span<const uint8_t, kVadpcmFrameSize> bytes)
{
    VadpcmFrame frame;
    frame.scale = static_cast<uint8_t>(bytes[0] >> 4);
    frame.predictor = static_cast<uint8_t>(bytes[0] & 0x0F);
    for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
    {
        const uint8_t byte = bytes[1 + i / 2];
        const int nibble = (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
        frame.residuals[i] = static_cast<int8_t>(nibble >= 8 ? nibble - 16 : nibble);
    }

    return frame;
}

std::array<uint8_t, kVadpcmFrameSize> VadpcmFrame::Write() const
{
    std::array<uint8_t, kVadpcmFrameSize> bytes = {};
    bytes[0] = static_cast<uint8_t>((scale << 4) | (predictor & 0x0F));
    for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
    {
        const auto nibble = static_cast<uint8_t>(residuals[i] & 0x0F);
        bytes[1 + i / 2] = static_cast<uint8_t>(bytes[1 + i / 2] | ((i % 2 == 0) ? (nibble << 4) : nibble));
    }

    return bytes;
}

void DecodeVadpcmFrame(std::span<const uint8_t, kVadpcmFrameSize> bytes, const VadpcmBook& book, VadpcmState& state)
{
    const VadpcmFrame frame = VadpcmFrame::Parse(bytes);
    DecodeResiduals(std::min(frame.scale, kFrameMaxScale), frame.predictor, frame.residuals, book, state);
}

void DecodeVadpcmSmallFrame(std::span<const uint8_t, kVadpcmSmallFrameSize> bytes, const VadpcmBook& book,
                            VadpcmState& state)
{
    std::array<int8_t, kVadpcmFrameSamples> residuals = {};
    for (std::size_t i = 0; i < kVadpcmFrameSamples; i++)
    {
        const int bits = (bytes[1 + i / 4] >> (6 - (i % 4) * 2)) & 3;
        residuals[i] = static_cast<int8_t>(bits >= 2 ? bits - 4 : bits);
    }

    const auto scale = static_cast<uint8_t>(bytes[0] >> 4);
    DecodeResiduals(std::min(scale, kSmallFrameMaxScale), static_cast<uint8_t>(bytes[0] & 0x0F), residuals, book,
                    state);
}

std::vector<int16_t> DecodeVadpcm(std::span<const uint8_t> frames, const VadpcmBook& book, VadpcmState& state)
{
    return DecodeFrames<kVadpcmFrameSize>(frames, book, state, DecodeVadpcmFrame);
}

std::vector<int16_t> DecodeVadpcm(std::span<const uint8_t> frames, const VadpcmBook& book)
{
    VadpcmState state = {};

    return DecodeVadpcm(frames, book, state);
}

std::vector<int16_t> DecodeVadpcmSmall(std::span<const uint8_t> frames, const VadpcmBook& book, VadpcmState& state)
{
    return DecodeFrames<kVadpcmSmallFrameSize>(frames, book, state, DecodeVadpcmSmallFrame);
}

std::vector<int16_t> DecodeVadpcmSmall(std::span<const uint8_t> frames, const VadpcmBook& book)
{
    VadpcmState state = {};

    return DecodeVadpcmSmall(frames, book, state);
}

std::vector<uint8_t> EncodeVadpcm(std::span<const int16_t> samples, const VadpcmBook& book)
{
    if (!book.Usable())
    {
        throw xsflib::FormatError("a VADPCM book that doesn't have an order of 2 and whole predictors");
    }

    const std::size_t frame_count = (samples.size() + kVadpcmFrameSamples - 1) / kVadpcmFrameSamples;
    std::vector<uint8_t> frames;
    frames.reserve(frame_count * kVadpcmFrameSize);
    VadpcmState state = {};
    for (std::size_t frame = 0; frame < frame_count; frame++)
    {
        VadpcmState wanted = {};
        const std::size_t count = std::min(kVadpcmFrameSamples, samples.size() - frame * kVadpcmFrameSamples);
        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(frame * kVadpcmFrameSamples), count, wanted.begin());

        VadpcmFrame best;
        bool exact = false;
        int64_t best_energy = std::numeric_limits<int64_t>::max();
        for (int predictor = 0; predictor < book.predictor_count; predictor++)
        {
            VadpcmFrame candidate;
            int64_t energy = 0;
            if (ExactFrame(wanted, book, state, predictor, candidate, energy) && energy < best_energy)
            {
                exact = true;
                best = candidate;
                best_energy = energy;
            }
        }

        // Samples at a limit of 16 bits allow several residuals, and the search takes the highest scale that fits.
        for (int scale = kMaxScale; scale >= 0 && !exact; scale--)
        {
            for (int predictor = 0; predictor < book.predictor_count && !exact; predictor++)
            {
                ExactSearch search(wanted, book, state, predictor, scale);
                exact = search.Run();
                if (exact)
                {
                    best.scale = static_cast<uint8_t>(scale);
                    best.predictor = static_cast<uint8_t>(predictor);
                    best.residuals = search.Residuals();
                }
            }
        }

        int64_t best_error = std::numeric_limits<int64_t>::max();
        for (int scale = 0; scale <= kMaxScale && !exact; scale++)
        {
            for (int predictor = 0; predictor < book.predictor_count; predictor++)
            {
                const Attempt attempt = TryFrame(wanted, book, state, predictor, scale);
                if (attempt.squared_error < best_error)
                {
                    best_error = attempt.squared_error;
                    best.scale = static_cast<uint8_t>(scale);
                    best.predictor = static_cast<uint8_t>(predictor);
                    best.residuals = attempt.residuals;
                }
            }
        }

        const std::array<uint8_t, kVadpcmFrameSize> bytes = best.Write();
        frames.insert(frames.end(), bytes.begin(), bytes.end());
        DecodeVadpcmFrame(bytes, book, state);
    }

    return frames;
}

} // namespace ntdlib::n64
