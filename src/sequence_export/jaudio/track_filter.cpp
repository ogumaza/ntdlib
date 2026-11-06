// SPDX-License-Identifier: MIT

// The zones of a track's IIR filter (see track_filter.h).

#include "ntdlib/sequence_export/jaudio/track_filter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// The table's first and last entries.
constexpr std::array<double, 4> kFirstEntry{3932.0, 2621.0, 18021.0, 14745.0};
constexpr std::array<double, 4> kLastEntry{32767.0, 0.0, 0.0, 0.0};

// The SoundFont players' Q at a resonance of 0, and FluidSynth's usual rate. SFZ's Q at a resonance of 0, and the rate
// of 48 kHz at which sfizz usually plays.
constexpr double kSoundFontQ = 0.7071312006813011;
constexpr double kSoundFontRate = 44100.0;
constexpr double kSfzQ = 1.0;
constexpr double kSfzRate = 48000.0;

// The frequency of 0 absolute cents.
constexpr double kCentsBaseHz = 8.176;

// The cutoffs that the fit tries, in absolute cents.
constexpr int kLowestCents = 7000;
constexpr int kHighestCents = 13500;
constexpr int kCentsStep = 25;

// The bands that the fit compares: thirds of an octave from 125 Hz, up to 16 kHz and 0.4 times the game's rate.
constexpr double kLowestBandHz = 125.0;
constexpr double kHighestBandHz = 16000.0;
constexpr double kHighestBandShare = 0.4;

// The share of the sound in the unfiltered zone, in hundredths, up to 98.
constexpr int kShareSteps = 100;
constexpr int kMostShareSteps = 98;

// The smallest change that a band can hear, in dB.
constexpr double kHeardDb = 0.3;

using Complex = std::complex<double>;

// Entry `entry`'s coefficients.
std::array<double, 4> Coefficients(uint8_t entry)
{
    std::array<double, 4> c{};
    for (std::size_t i = 0; i < c.size(); i++)
    {
        c[i] = std::trunc(kFirstEntry[i] + (kLastEntry[i] - kFirstEntry[i]) * entry / kOpenTrackFilter);
    }

    return c;
}

// The filter's response at `hz`.
Complex GameResponse(const std::array<double, 4>& c, double hz, double rate)
{
    const Complex delay = std::polar(1.0, -2.0 * std::numbers::pi * hz / rate);

    return (c[0] + c[1] * delay) / (32768.0 - c[2] * delay - c[3] * delay * delay);
}

// The response at `hz` of a player's two-pole low-pass with `q` at `synth_rate`. FluidSynth gives every voice a gain of
// 1 / sqrt(Q) on top. That gain is left out.
Complex LowPassResponse(double cutoff_hz, double hz, double q, double synth_rate)
{
    const double w = 2.0 * std::numbers::pi * cutoff_hz / synth_rate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double cosine = std::cos(w);
    const double b0 = (1.0 - cosine) / 2.0;
    const Complex delay = std::polar(1.0, -2.0 * std::numbers::pi * hz / synth_rate);

    return (b0 + (1.0 - cosine) * delay + b0 * delay * delay) /
           (1.0 + alpha - 2.0 * cosine * delay + (1.0 - alpha) * delay * delay);
}

double Db(double amplitude)
{
    return 20.0 * std::log10(amplitude);
}

std::vector<double> Bands(double rate)
{
    std::vector<double> bands;
    const double top = std::min(kHighestBandShare * rate, kHighestBandHz);
    for (int k = 0; kLowestBandHz * std::pow(2.0, k / 3.0) <= top * 1.0001; k++)
    {
        bands.push_back(kLowestBandHz * std::pow(2.0, k / 3.0));
    }

    return bands;
}

} // namespace

bool TrackFilterHeard(uint8_t entry, double rate)
{
    if (entry >= kOpenTrackFilter)
    {
        return false;
    }

    const std::array<double, 4> c = Coefficients(entry);
    for (const double hz : Bands(rate))
    {
        if (std::abs(Db(std::abs(GameResponse(c, hz, rate)))) >= kHeardDb)
        {
            return true;
        }
    }

    return false;
}

std::vector<FilterLayer> TrackFilterLayers(uint8_t entry, double rate, FilterFormat format)
{
    if (entry >= kOpenTrackFilter)
    {
        return {FilterLayer{}};
    }

    const std::array<double, 4> c = Coefficients(entry);
    const std::vector<double> bands = Bands(rate);
    std::vector<double> target;
    for (const double hz : bands)
    {
        target.push_back(Db(std::abs(GameResponse(c, hz, rate))));
    }

    // The zones play at the filter's gain in the lowest band, where the low-pass passes everything.
    const double low = std::abs(GameResponse(c, bands[0], rate));

    double best = std::numeric_limits<double>::infinity();
    int best_steps = 0;
    int best_cents = kHighestCents;
    std::vector<Complex> low_pass(bands.size());
    for (int cents = kLowestCents; cents <= kHighestCents; cents += kCentsStep)
    {
        for (std::size_t b = 0; b < bands.size(); b++)
        {
            low_pass[b] = format == FilterFormat::kSoundFont
                              ? LowPassResponse(CutoffHz(cents), bands[b], kSoundFontQ, kSoundFontRate)
                              : LowPassResponse(CutoffHz(cents), bands[b], kSfzQ, kSfzRate);
        }
        for (int steps = 0; steps <= kMostShareSteps; steps++)
        {
            const double share = static_cast<double>(steps) / kShareSteps;
            double error = 0.0;
            for (std::size_t b = 0; b < bands.size(); b++)
            {
                const double d = Db(low * std::abs(share + (1.0 - share) * low_pass[b])) - target[b];
                error += d * d;
            }
            if (error < best)
            {
                best = error;
                best_steps = steps;
                best_cents = cents;
            }
        }
    }

    const double share = static_cast<double>(best_steps) / kShareSteps;
    std::vector<FilterLayer> layers;
    if (best_steps > 0)
    {
        layers.push_back({.gain = low * share, .cutoff_cents = std::nullopt});
    }
    layers.push_back({.gain = low * (1.0 - share), .cutoff_cents = best_cents});

    return layers;
}

double CutoffHz(int cents)
{
    return kCentsBaseHz * std::pow(2.0, cents / 1200.0);
}

} // namespace ntdlib::sequence_export::jaudio
