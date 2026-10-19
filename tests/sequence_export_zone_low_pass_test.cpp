// SPDX-License-Identifier: MIT

// Tests of the zones' fitted track low-pass (zone_low_pass.h): the spectrum of a wave, the cutoffs that match the
// game's level, the tracking of the key, the keys that each preset plays, and the choice between the fitted low-pass
// and the resampling low-pass (AddZoneLowPass).
//
// The expected cutoffs come from the filters' exact responses to a sine. AX's one-pole is a0 / (1 - b0 z^-1), with
// a0 and b0 from AXGetLpfCoefs or AXComputeLpfCoefs. FluidSynth's low-pass is the cookbook two-pole at 44.1 kHz with
// Q = 10^(-3.01 / 20). The cutoff at which the two-pole passes as much of a sine at f as the one-pole at fc lies near
// sqrt(fc f).

#include <algorithm>
#include <bitset>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/ax/lpf.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "seqlib/sf2/soundfont.h"
#include "xsflib/testing/check.h"

#include "sequence_export/sf2_build.h"
#include "sequence_export/zone_low_pass.h"

using namespace ntdlib::sequence_export;

namespace sf2 = seqlib::sf2;

namespace
{

// A one-channel wave of `length` samples at 32 kHz that loops: a sine at `hz` with an amplitude of 10000. The loop
// holds a whole number of cycles.
ntdlib::wave::Pcm Sine(double hz, uint32_t length)
{
    ntdlib::wave::Pcm wave;
    wave.loop = true;
    wave.loop_end = length;
    wave.channels.emplace_back();
    for (uint32_t n = 0; n < length; n++)
    {
        const double value = 10000.0 * std::sin(2.0 * std::numbers::pi * hz * n / 32000.0);
        wave.channels[0].push_back(static_cast<int16_t>(std::lround(value)));
    }

    return wave;
}

// The keys from `first` to `last`.
std::vector<int> Keys(int first, int last)
{
    std::vector<int> keys;
    for (int k = first; k <= last; k++)
    {
        keys.push_back(k);
    }

    return keys;
}

// The Wii, played as a build of 2008.
Settings Wii()
{
    return RevolutionSettings(BuildOfRsarVersion(0x0104));
}

// A sine at 1000 Hz, bin 128 of the transform at 32 kHz, puts all but a trace of its power in the bands from 900 to
// 1100 Hz, with their mean at 1000 Hz. The trace comes from the wave's start.
void TestSpectrumFindsASine()
{
    const ntdlib::wave::Pcm wave = Sine(1000.0, 8192);

    const WaveSpectrum spectrum = MeasureSpectrum(wave);

    double total = 0.0;
    double near = 0.0;
    double moment = 0.0;
    for (std::size_t b = 0; b < spectrum.power.size(); b++)
    {
        total += spectrum.power[b];
        if (spectrum.hz[b] > 900.0 && spectrum.hz[b] < 1100.0)
        {
            near += spectrum.power[b];
            moment += spectrum.power[b] * spectrum.hz[b];
        }
    }
    XSFLIB_CHECK(spectrum.hz.size() == spectrum.power.size() && near > 0.995 * total);
    XSFLIB_CHECK(std::abs(moment / near - 1000.0) < 0.5);
}

// AX's one-pole at 92 Hz (argument 10 on the Wii) passes a sine at 3 kHz 30.2 dB down. FluidSynth's two-pole passes
// as much at 7244 cents (537 Hz). The table's cutoff, 5770 cents (229 Hz), fits the response's shape and passes
// 14.8 dB less. Near 7244 cents, 100 cents move the level by about 1 dB. A table's cutoff of 7300 cents would come
// within 1 dB and stay. One of 7400 cents would miss by 1.6 dB.
void TestFitMatchesTheGamesLevelAtOneKey()
{
    const WaveSpectrum spectrum = MeasureSpectrum(Sine(3000.0, 4096));

    const std::optional<FittedCutoff> cutoff = FitZoneCutoff(spectrum, 60, 1.0, 92, 5770, {60}, Wii());
    const std::optional<FittedCutoff> close = FitZoneCutoff(spectrum, 60, 1.0, 92, 7300, {60}, Wii());
    const std::optional<FittedCutoff> far = FitZoneCutoff(spectrum, 60, 1.0, 92, 7400, {60}, Wii());

    XSFLIB_CHECK(Wii().filters->low_pass_cutoff[10] == 5770);
    XSFLIB_CHECK(cutoff && std::abs(cutoff->base - 7244) <= 3 && cutoff->slope == 0);
    XSFLIB_CHECK(!close && far && *far == *cutoff);
}

// The level of AX's one-pole low-pass with `coefs` at `hz`, in dB.
double OnePoleDb(double hz, const ntdlib::ax::LpfCoefs& coefs)
{
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * hz / 32000.0);
    const std::complex<double> h = (coefs.a0 / 32768.0) / (1.0 - coefs.b0 / 32768.0 * z);

    return 20.0 * std::log10(std::abs(h));
}

// The level of FluidSynth's low-pass at `cents` and a resonance of 0, at 44.1 kHz, at `hz`, in dB.
double TwoPoleDb(double hz, double cents)
{
    const double w = 2.0 * std::numbers::pi * 8.176 * std::exp2(cents / 1200.0) / 44100.0;
    const double alpha = std::sin(w) / (2.0 * std::pow(10.0, -3.01 / 20.0));
    const double b1 = (1.0 - std::cos(w)) / (1.0 + alpha);
    const double a1 = -2.0 * std::cos(w) / (1.0 + alpha);
    const double a2 = (1.0 - alpha) / (1.0 + alpha);
    const std::complex<double> z = std::polar(1.0, -2.0 * std::numbers::pi * hz / 44100.0);
    const std::complex<double> h = (b1 / 2.0 + b1 * z + b1 / 2.0 * z * z) / (1.0 + a1 * z + a2 * z * z);

    return 20.0 * std::log10(std::abs(h));
}

// Over keys 48 to 72, the sine moves from 1500 Hz to 6 kHz. The keys' cutoffs rise from 6615 to 7965 cents. Their
// mean misses the game's level by up to 7.1 dB. A line through them rises by about 56 cents a key. It misses by less
// than 0.5 dB.
void TestFitTracksTheKeyWhereOneCutoffMisses()
{
    const WaveSpectrum spectrum = MeasureSpectrum(Sine(3000.0, 4096));
    const ntdlib::ax::LpfCoefs game = ntdlib::ax::ComputeLpfCoefs(92, {.double_precision = true});

    const std::optional<FittedCutoff> cutoff = FitZoneCutoff(spectrum, 60, 1.0, 92, 5770, Keys(48, 72), Wii());

    XSFLIB_CHECK(cutoff && cutoff->slope >= 54 && cutoff->slope <= 57);
    double worst = 0.0;
    for (int k = 48; cutoff && k <= 72; k++)
    {
        const double hz = 3000.0 * std::exp2((k - 60) / 12.0);
        worst = std::max(worst, std::abs(TwoPoleDb(hz, cutoff->base + cutoff->slope * k) - OnePoleDb(hz, game)));
    }
    XSFLIB_CHECK(worst < 0.5);
}

// A sine at 100 Hz lies far below AX's 7025 Hz (argument 50). No cutoff in FluidSynth's range moves its level by
// 0.01 dB in 100 cents. The zone keeps the preset's cutoff.
void TestFitLeavesAWaveThatTheCutoffDoesntMove()
{
    const WaveSpectrum spectrum = MeasureSpectrum(Sine(100.0, 3200));

    const std::optional<FittedCutoff> cutoff =
        FitZoneCutoff(spectrum, 60, 1.0, 7025, Wii().filters->low_pass_cutoff[50], {60}, Wii());

    XSFLIB_CHECK(!cutoff);
}

// On the Wii U, step 3 is kLpfFreq's 160 Hz. AXComputeLpfCoefs gives b0 = 31754. FluidSynth passes as much of a sine
// at 3 kHz at 7723 cents.
void TestFitOnTheWiiUTakesItsTable()
{
    const WaveSpectrum spectrum = MeasureSpectrum(Sine(3000.0, 4096));

    const std::optional<FittedCutoff> cutoff =
        FitZoneCutoff(spectrum, 60, 1.0, 160, CafeSettings().filters->low_pass_cutoff[3], {60}, CafeSettings());

    XSFLIB_CHECK(cutoff && std::abs(cutoff->base - 7723) <= 2 && cutoff->slope == 0);
}

// A part's notes count for the preset that the part last selected. A note before the part's first preset counts for
// none.
void TestPlayedKeysFollowEachPartsPreset()
{
    Performance perf;
    perf.presets.resize(2);
    perf.tracks[0] = {
        {.kind = EventKind::kNoteOn, .key = 40, .part = 0}, {.kind = EventKind::kPreset, .preset = 1, .part = 0},
        {.kind = EventKind::kNoteOn, .key = 60, .part = 0}, {.kind = EventKind::kPreset, .preset = 0, .part = 1},
        {.kind = EventKind::kNoteOn, .key = 62, .part = 1}, {.kind = EventKind::kNoteOn, .key = 64, .part = 0},
    };

    const std::vector<std::bitset<128>> keys = PlayedKeys(perf);

    XSFLIB_CHECK(keys.size() == 2 && keys[0].count() == 1 && keys[0].test(62));
    XSFLIB_CHECK(keys[1].count() == 2 && keys[1].test(60) && keys[1].test(64));
}

// A zone with only its sample generator, sample 7.
sf2::Zone SampleZone()
{
    sf2::Zone zone;
    zone.generators.push_back(sf2::Generator::Value(sf2::kSampleId, 7));

    return zone;
}

// The zone's cutoff generator and its modulator from the key, as (cutoff, amount), or -1s.
std::pair<int, int> CutoffOf(const sf2::Zone& zone)
{
    std::pair<int, int> cutoff = {-1, -1};
    for (const sf2::Generator& g : zone.generators)
    {
        if (g.oper == sf2::kInitialFilterFc)
        {
            cutoff.first = static_cast<int16_t>(g.amount);
        }
    }
    for (const sf2::Modulator& m : zone.modulators)
    {
        if (m.dest == sf2::kInitialFilterFc)
        {
            cutoff.second = m.amount;
        }
    }

    return cutoff;
}

// On the Wii U, a 32 kHz wave at original key 60 plays above a rate of 4/3 from key 65. The resampling low-pass's
// cutoff is 5934 cents at key 0 and rises by 100 cents a key: 11934 cents at the original key. A zone of a preset with
// the one-pole's cutoff takes the lower of the two low-passes at the original key. A fitted 7000 cents beats it, and a
// fitted 12000 cents loses to it. Without a fitted cutoff, the zone compares the preset's 11000 cents and keeps the
// preset's low-pass. A preset with a biquad's cutoff takes the resampling low-pass, as ZoneSpans gives it. The spans of
// a preset with the one-pole's cutoff aren't split by the track's cutoff.
void TestZonesTakeTheLowerLowPassAtTheOriginalKey()
{
    const Settings settings = CafeSettings();
    const ResamplingRegion region = {60, 1.0f, 32000, 0};
    const ResamplingFilter* above_4_thirds = &settings.resampling->filters[0];
    PresetKey one_pole;
    one_pole.filter = {11000, 0, 160};
    PresetKey biquad;
    biquad.filter = {11000, 0, 0};
    sf2::Zone low = SampleZone();
    sf2::Zone high = SampleZone();
    sf2::Zone kept = SampleZone();
    sf2::Zone other = SampleZone();

    AddZoneLowPass(low, above_4_thirds, FittedCutoff{7000, 0}, region, one_pole, settings);
    AddZoneLowPass(high, above_4_thirds, FittedCutoff{12000, 0}, region, one_pole, settings);
    AddZoneLowPass(kept, above_4_thirds, std::nullopt, region, one_pole, settings);
    AddZoneLowPass(other, above_4_thirds, std::nullopt, region, biquad, settings);
    const std::vector<ResamplingSpan> spans = ZoneSpans({0, 127}, region, one_pole, settings);

    XSFLIB_CHECK((CutoffOf(low) == std::pair{7000, -1}));
    XSFLIB_CHECK((CutoffOf(high) == std::pair{5934, 12800}));
    XSFLIB_CHECK((CutoffOf(kept) == std::pair{-1, -1}));
    XSFLIB_CHECK((CutoffOf(other) == std::pair{5934, 12800}));
    XSFLIB_CHECK((spans.size() == 3 && spans[2].keys == std::pair{65, 127} && spans[2].filter == above_4_thirds));
}

} // namespace

int main()
{
    TestSpectrumFindsASine();
    TestFitMatchesTheGamesLevelAtOneKey();
    TestFitTracksTheKeyWhereOneCutoffMisses();
    TestFitLeavesAWaveThatTheCutoffDoesntMove();
    TestFitOnTheWiiUTakesItsTable();
    TestPlayedKeysFollowEachPartsPreset();
    TestZonesTakeTheLowerLowPassAtTheOriginalKey();

    return xsflib::testing::Summary("sequence_export_zone_low_pass");
}
