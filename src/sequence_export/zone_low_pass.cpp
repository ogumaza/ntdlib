// SPDX-License-Identifier: MIT

// The zones' fitted track low-pass (see zone_low_pass.h).

#include "sequence_export/zone_low_pass.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/ax/lpf.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::sequence_export
{
namespace
{

// The spectrum's transforms: 4096 samples, a hop of half that, and a sine window. At that hop, the squares of the
// windows that cover a sample add up to 1.
constexpr std::size_t kFftSize = 4096;
constexpr std::size_t kHop = kFftSize / 2;

// The samples of a wave that the spectrum measures: about a second at 32 kHz.
constexpr std::size_t kMeasuredSamples = std::size_t{1} << 15;

constexpr double kBandsPerOctave = 24.0;

// FluidSynth's usual rate, and its highest cutoff: 0.45 times the rate.
constexpr double kSynthRate = 44100.0;
constexpr double kHighestHz = 0.45 * kSynthRate;

// The range of FluidSynth's cutoff, in absolute cents. FluidSynth clamps the sum of the generator and the modulators to
// it.
constexpr double kLowestCents = 1500.0;
constexpr double kHighestCents = 13500.0;

// The frequency of 0 absolute cents, and FluidSynth's Q at a resonance of 0: 10^(-3.01 / 20).
constexpr double kCentsBaseHz = 8.176;
constexpr double kQ = 0.7071312006813011;

// The lowest frequency that counts toward the level.
constexpr double kLowestHz = 20.0;

// A key takes a weight in the fit where 100 cents of cutoff move its level by at least 0.01 dB. The value is in dB a
// cent.
constexpr double kLeastSensitivity = 0.01 / 100.0;

// The most that a fixed cutoff may miss the game's level by at a key, in dB: the table's before the zone takes a fitted
// cutoff, and a fitted one before it tracks the key.
constexpr double kFixedTolerance = 1.0;

// The steepest tracking, in cents a key: the pitch's.
constexpr int kSteepestSlope = 100;

// e^(-2 pi i k / kFftSize) for k below kFftSize / 2: the cosines, then the sines.
const std::array<std::vector<double>, 2>& Twiddles()
{
    static const std::array<std::vector<double>, 2> kTwiddles = []
    {
        std::array<std::vector<double>, 2> t;
        for (std::size_t k = 0; k < kFftSize / 2; k++)
        {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(kFftSize);
            t[0].push_back(std::cos(angle));
            t[1].push_back(-std::sin(angle));
        }

        return t;
    }();

    return kTwiddles;
}

// Transforms `re` and `im`, of kFftSize values, in place: a radix-2 FFT.
void Fft(std::vector<double>& re, std::vector<double>& im)
{
    // Bit-reversed order.
    for (std::size_t i = 1, j = 0; i < kFftSize; i++)
    {
        std::size_t bit = kFftSize >> 1;
        for (; (j & bit) != 0; bit >>= 1)
        {
            j ^= bit;
        }
        j ^= bit;

        if (i < j)
        {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // The butterflies, stage by stage.
    const std::array<std::vector<double>, 2>& twiddles = Twiddles();
    for (std::size_t length = 2; length <= kFftSize; length <<= 1)
    {
        const std::size_t step = kFftSize / length;
        for (std::size_t start = 0; start < kFftSize; start += length)
        {
            for (std::size_t k = 0; k < length / 2; k++)
            {
                const double wr = twiddles[0][k * step];
                const double wi = twiddles[1][k * step];
                const std::size_t a = start + k;
                const std::size_t b = a + length / 2;
                const double tr = re[b] * wr - im[b] * wi;
                const double ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

// A key's bands: the power that the game's low-pass passes, and each band's power and angle at FluidSynth's rate.
struct KeyBands
{
    int key = 0;
    double game = 0.0;
    std::vector<double> power;
    std::vector<double> cosine;
};

// The level that FluidSynth's low-pass at `cents` gives `bands`, in dB above the game's.
double SynthLevel(const KeyBands& bands, double cents)
{
    // FluidSynth's cookbook low-pass: b0 = b2 = b1 / 2.
    const double hz =
        std::min(kCentsBaseHz * std::exp2(std::clamp(cents, kLowestCents, kHighestCents) / 1200.0), kHighestHz);
    const double w = 2.0 * std::numbers::pi * hz / kSynthRate;
    const double cos_w = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * kQ);
    const double b1 = (1.0 - cos_w) / (1.0 + alpha);
    const double a1 = -2.0 * cos_w / (1.0 + alpha);
    const double a2 = (1.0 - alpha) / (1.0 + alpha);

    // |H|^2 = b1^2 (1 + cos t)^2 / (1 + a1^2 + a2^2 + 2 a1 (1 + a2) cos t + 2 a2 cos 2t) at angle t.
    double passed = 0.0;
    for (std::size_t b = 0; b < bands.power.size(); b++)
    {
        const double c = bands.cosine[b];
        const double numerator = b1 * b1 * (1.0 + c) * (1.0 + c);
        const double denominator = 1.0 + a1 * a1 + a2 * a2 + 2.0 * a1 * (1.0 + a2) * c + 2.0 * a2 * (2.0 * c * c - 1.0);
        passed += bands.power[b] * numerator / denominator;
    }

    return 10.0 * std::log10(std::max(passed, 1e-300) / bands.game);
}

// The cutoff, in absolute cents within FluidSynth's range, at which FluidSynth passes the game's level of `bands`.
double MatchingCutoff(const KeyBands& bands)
{
    if (SynthLevel(bands, kHighestCents) <= 0.0)
    {
        return kHighestCents;
    }
    if (SynthLevel(bands, kLowestCents) >= 0.0)
    {
        return kLowestCents;
    }

    // The level rises with the cutoff. 20 halvings of the range come within 0.02 cents.
    double low = kLowestCents;
    double high = kHighestCents;
    for (int i = 0; i < 20; i++)
    {
        const double middle = (low + high) / 2.0;
        (SynthLevel(bands, middle) < 0.0 ? low : high) = middle;
    }

    return (low + high) / 2.0;
}

// A key's bands, its matching cutoff, and the weight of the key in the fit: the square of the dB by which a cent moves
// the key's level there.
struct KeyFit
{
    KeyBands bands;
    double cutoff = 0.0;
    double weight = 0.0;
};

// The most by which the cutoff `base` + `slope` * k misses the game's level at the keys of `fits`, in dB.
double WorstMiss(const std::vector<KeyFit>& fits, int base, int slope)
{
    double worst = 0.0;
    for (const KeyFit& fit : fits)
    {
        worst = std::max(worst, std::abs(SynthLevel(fit.bands, base + slope * fit.bands.key)));
    }

    return worst;
}

} // namespace

WaveSpectrum MeasureSpectrum(const wave::Pcm& wave)
{
    // The windows start half a window before the first sample. Every sample then falls in two windows. A one-shot wave
    // that ends within kMeasuredSamples plays out into silence, and the windows run past its end. The windows of
    // another wave stop at kMeasuredSamples. A looping wave plays its loop again after its end.
    const bool ends = !wave.loop && wave.Length() <= kMeasuredSamples;
    const auto last_start = ends ? static_cast<std::ptrdiff_t>(wave.Length()) - 1
                                 : static_cast<std::ptrdiff_t>(kMeasuredSamples - kFftSize);
    std::vector<std::pair<const std::vector<int16_t>*, std::ptrdiff_t>> windows;
    for (const std::vector<int16_t>& channel : wave.channels)
    {
        for (auto start = -static_cast<std::ptrdiff_t>(kHop); start <= last_start;
             start += static_cast<std::ptrdiff_t>(kHop))
        {
            windows.emplace_back(&channel, start);
        }
    }

    // Each transform takes two windows: one in its real part and one in its imaginary part. Their powers at bin k add
    // up to (|X(k)|^2 + |X(N - k)|^2) / 2.
    std::vector<double> sine(kFftSize);
    for (std::size_t n = 0; n < kFftSize; n++)
    {
        sine[n] = std::sin(std::numbers::pi * (static_cast<double>(n) + 0.5) / static_cast<double>(kFftSize));
    }

    const auto fill =
        [&sine, &wave](std::vector<double>& out, const std::vector<int16_t>& samples, std::ptrdiff_t start)
    {
        const auto end = static_cast<std::ptrdiff_t>(std::min<std::size_t>(wave.Length(), samples.size()));
        const auto loop_start = static_cast<std::ptrdiff_t>(wave.loop_start);
        const bool loops = wave.loop && loop_start < end;
        for (std::size_t n = 0; n < kFftSize; n++)
        {
            std::ptrdiff_t i = start + static_cast<std::ptrdiff_t>(n);
            if (i >= end && loops)
            {
                i = loop_start + (i - loop_start) % (end - loop_start);
            }
            out[n] = i >= 0 && i < end ? samples[static_cast<std::size_t>(i)] * sine[n] : 0.0;
        }
    };

    std::vector<double> power(kFftSize / 2 + 1, 0.0);
    std::vector<double> re(kFftSize);
    std::vector<double> im(kFftSize);
    for (std::size_t w = 0; w < windows.size(); w += 2)
    {
        fill(re, *windows[w].first, windows[w].second);
        if (w + 1 < windows.size())
        {
            fill(im, *windows[w + 1].first, windows[w + 1].second);
        }
        else
        {
            std::fill(im.begin(), im.end(), 0.0);
        }

        Fft(re, im);
        for (std::size_t k = 0; k <= kFftSize / 2; k++)
        {
            const std::size_t mirror = (kFftSize - k) % kFftSize;
            power[k] += (re[k] * re[k] + im[k] * im[k] + re[mirror] * re[mirror] + im[mirror] * im[mirror]) / 2.0;
        }
    }

    // The bins from 1 up, in bands of 1/24 octave. Each band's frequency is its bins' mean, weighted by power. The bins
    // up to 34 each make a band. A band without power is left out.
    WaveSpectrum spectrum;
    const double bin_hz = wave.sample_rate / static_cast<double>(kFftSize);
    int band = -1;
    double band_power = 0.0;
    double band_moment = 0.0;
    const auto flush = [&]
    {
        if (band_power > 0.0)
        {
            spectrum.hz.push_back(bin_hz * band_moment / band_power);
            spectrum.power.push_back(band_power);
        }
        band_power = 0.0;
        band_moment = 0.0;
    };

    for (std::size_t k = 1; k <= kFftSize / 2; k++)
    {
        const auto k_band = static_cast<int>(std::floor(kBandsPerOctave * std::log2(static_cast<double>(k))));
        if (k_band != band)
        {
            flush();
            band = k_band;
        }

        band_power += power[k];
        band_moment += power[k] * static_cast<double>(k);
    }
    flush();

    return spectrum;
}

std::optional<FittedCutoff> FitZoneCutoff(const WaveSpectrum& spectrum, int original_key, double tune, uint32_t game_hz,
                                          int table_cutoff, const std::vector<int>& keys, const Settings& settings)
{
    // The game's one-pole low-pass, y = a0 x + b0 y1 in Q15, at AX's rate.
    const ax::LpfSettings& ax_settings = *settings.zone_low_pass;
    const ax::LpfCoefs coefs = ax::ComputeLpfCoefs(game_hz, ax_settings);
    const double a0 = coefs.a0 / 32768.0;
    const double b0 = coefs.b0 / 32768.0;
    const double game_rate = ax_settings.rate;

    // Each key's bands, its matching cutoff and its weight.
    std::vector<KeyFit> fits;
    for (const int key : keys)
    {
        const double ratio = tune * std::exp2((key - original_key) / 12.0);
        KeyFit fit;
        fit.bands.key = key;
        for (std::size_t b = 0; b < spectrum.hz.size(); b++)
        {
            const double hz = spectrum.hz[b] * ratio;
            if (hz >= kLowestHz && hz < game_rate / 2.0)
            {
                const double angle = 2.0 * std::numbers::pi * hz / game_rate;
                fit.bands.game += spectrum.power[b] * a0 * a0 / (1.0 - 2.0 * b0 * std::cos(angle) + b0 * b0);
            }
            if (hz >= kLowestHz && hz < kSynthRate / 2.0)
            {
                fit.bands.power.push_back(spectrum.power[b]);
                fit.bands.cosine.push_back(std::cos(2.0 * std::numbers::pi * hz / kSynthRate));
            }
        }

        if (fit.bands.game <= 0.0)
        {
            continue;
        }

        fit.cutoff = MatchingCutoff(fit.bands);
        const double sensitivity =
            (SynthLevel(fit.bands, fit.cutoff + 50.0) - SynthLevel(fit.bands, fit.cutoff - 50.0)) / 100.0;
        fit.weight = sensitivity >= kLeastSensitivity ? sensitivity * sensitivity : 0.0;
        fits.push_back(std::move(fit));
    }

    // The table's cutoff stays where it comes close enough to the game's level.
    if (WorstMiss(fits, table_cutoff, 0) <= kFixedTolerance)
    {
        return std::nullopt;
    }

    // The weighted sums of the keys and their cutoffs.
    double weight = 0.0;
    double sum_c = 0.0;
    double sum_k = 0.0;
    double sum_kk = 0.0;
    double sum_kc = 0.0;
    int weighted = 0;
    for (const KeyFit& fit : fits)
    {
        const double k = fit.bands.key;
        weight += fit.weight;
        sum_c += fit.weight * fit.cutoff;
        sum_k += fit.weight * k;
        sum_kk += fit.weight * k * k;
        sum_kc += fit.weight * k * fit.cutoff;
        weighted += fit.weight > 0.0 ? 1 : 0;
    }

    if (weighted == 0)
    {
        return std::nullopt;
    }

    // A fixed cutoff: the weighted mean of the keys' cutoffs. The keys without weight count toward its miss too.
    const auto mean_c = std::clamp(sum_c / weight, kLowestCents, kHighestCents);
    const FittedCutoff fixed = {static_cast<int>(std::lround(mean_c)), 0};
    const double fixed_miss = WorstMiss(fits, fixed.base, 0);
    if (fixed_miss <= kFixedTolerance || weighted < 2)
    {
        return fixed;
    }

    // A cutoff that tracks the key: the weighted least-squares line through the keys' cutoffs, in whole cents a key.
    const double mean_k = sum_k / weight;
    const double spread = sum_kk / weight - mean_k * mean_k;
    const double line = spread > 0.0 ? (sum_kc / weight - mean_k * sum_c / weight) / spread : 0.0;
    const int slope = std::clamp(static_cast<int>(std::lround(line)), 0, kSteepestSlope);
    const FittedCutoff tracking = {static_cast<int>(std::lround(sum_c / weight - slope * mean_k)), slope};

    return WorstMiss(fits, tracking.base, tracking.slope) < fixed_miss ? tracking : fixed;
}

std::vector<std::bitset<128>> PlayedKeys(const Performance& perf)
{
    std::vector<std::bitset<128>> keys(perf.presets.size());
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        std::array<int64_t, 256> preset; // per part, or -1 before the part's first preset
        preset.fill(-1);
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kPreset)
            {
                preset[e.part] = e.preset;
            }
            else if (e.kind == EventKind::kNoteOn && preset[e.part] >= 0)
            {
                keys[static_cast<std::size_t>(preset[e.part])].set(static_cast<std::size_t>(e.key & 0x7f));
            }
        }
    }

    return keys;
}

bool FitsTrackLowPass(const PresetKey& key, const Settings& settings)
{
    return settings.zone_low_pass && key.filter.low_pass_hz != 0 && !key.filter_controller;
}

ZoneLowPass::ZoneLowPass(const Performance& perf, const Settings& settings)
    : played_(settings.zone_low_pass ? PlayedKeys(perf) : std::vector<std::bitset<128>>()), settings_(settings)
{
}

std::optional<FittedCutoff> ZoneLowPass::Fit(uint32_t index, const PresetKey& key, std::pair<int, int> keys,
                                             int original_key, double tune, std::pair<uint32_t, uint32_t> wave_key,
                                             const wave::Pcm& wave)
{
    if (!FitsTrackLowPass(key, settings_) || index >= played_.size())
    {
        return std::nullopt;
    }

    // The keys of the zones that the performance plays.
    std::vector<int> played;
    for (int k = std::max(keys.first, 0); k <= std::min(keys.second, 127); k++)
    {
        if (played_[index].test(static_cast<std::size_t>(k)))
        {
            played.push_back(k);
        }
    }

    if (played.empty())
    {
        return std::nullopt;
    }

    auto spectrum = spectra_.find(wave_key);
    if (spectrum == spectra_.end())
    {
        spectrum = spectra_.emplace(wave_key, MeasureSpectrum(wave)).first;
    }

    return FitZoneCutoff(spectrum->second, original_key, tune, key.filter.low_pass_hz, key.filter.cutoff, played,
                         settings_);
}

} // namespace ntdlib::sequence_export
