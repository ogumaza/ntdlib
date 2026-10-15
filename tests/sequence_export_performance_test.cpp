// SPDX-License-Identifier: MIT

// Tests of sequence_export's performance types: the envelope overrides, the LFO units of each engine, the Performance
// that seqlib's MIDI writer takes, and how FitParts fits a Performance's parts into MIDI's channels.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "seqlib/midi/performance_writer.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::sequence_export;

namespace
{

// The 3DS's envelope time scale: the DSP's frame over the 5 ms envelope step.
constexpr double kCtrFrameMs = 160.0 * 8192.0 * 1000.0 / 268111856.0;
constexpr double kCtrEnvTimeScale = kCtrFrameMs / 5;

Settings SettingsFor(Engine engine)
{
    Settings settings;
    settings.engine = engine;
    if (engine == Engine::kCtr)
    {
        settings.env_time_scale = kCtrEnvTimeScale;
    }

    return settings;
}

void TestOverridesReplaceOnlyTheSetValues()
{
    PresetKey key;
    key.attack = 10;
    key.release = 127;
    const EnvelopeValues kRegion{1, 2, 3, 4, 5};

    const EnvelopeValues env = OverrideEnvelope(kRegion, key);

    XSFLIB_CHECK(env.attack == 10);
    XSFLIB_CHECK(env.decay == 2);
    XSFLIB_CHECK(env.sustain == 3);
    XSFLIB_CHECK(env.hold == 4);
    XSFLIB_CHECK(env.release == 127);
}

// The Wii U and the Switch keep Hz and milliseconds.
void TestLfoOnTheWiiUAndTheSwitchIsInHzAndMilliseconds()
{
    PresetKey key;
    key.lfo_speed = 7.5f;
    key.lfo_delay = 1234;

    for (const Engine engine : {Engine::kCafe, Engine::kSwitch})
    {
        const Settings settings = SettingsFor(engine);

        XSFLIB_CHECK(LfoHz(key, settings) == 7.5);
        XSFLIB_CHECK(LfoDelayMs(key, settings) == 1234.0);
    }
}

// The 3DS and the Wii keep the bytecode's units. The expected values convert them as each console does.
void TestLfoOnThe3dsAndTheWiiIsInTheBytecodesUnits()
{
    for (const uint8_t speed : {uint8_t{1}, uint8_t{16}, uint8_t{37}, uint8_t{255}})
    {
        for (const uint16_t delay : {uint16_t{0}, uint16_t{7}, uint16_t{65535}})
        {
            PresetKey key;
            key.lfo_speed = speed;
            key.lfo_delay = delay;

            const double ctr_hz = LfoHz(key, SettingsFor(Engine::kCtr));
            const double ctr_delay = LfoDelayMs(key, SettingsFor(Engine::kCtr));
            const double wii_hz = LfoHz(key, SettingsFor(Engine::kRevolution));
            const double wii_delay = LfoDelayMs(key, SettingsFor(Engine::kRevolution));

            const uint8_t ctr_speed = speed;
            const uint16_t ctr_delay_units = delay;
            constexpr int kEnvStepMs = 5;
            XSFLIB_CHECK(ctr_hz == ctr_speed * 100.0 / 256.0 / kCtrEnvTimeScale);
            XSFLIB_CHECK(ctr_delay == ctr_delay_units * kEnvStepMs * kCtrEnvTimeScale);
            XSFLIB_CHECK(wii_hz == speed * 100.0 / 256.0);
            XSFLIB_CHECK(wii_delay == delay * 5.0);
        }
    }
}

// seqlib's MIDI writer takes sequence_export's Performance as it is.
void TestPerformanceWritesAsMidi()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    perf.tracks[0].push_back({0, EventKind::kNoteOn, 60, 100, 0, 0.0f, 1});
    perf.tracks[0].push_back({48, kFilterEvent, 0, 64});
    perf.tracks[0].push_back({96, EventKind::kNoteOff, 60, 0, 0, 0.0f, 1});
    perf.end_tick = 96;
    perf.presets.push_back(PresetKey{});

    seqlib::midi::MidiReport report;
    const std::vector<uint8_t> midi = seqlib::midi::WriteMidi(perf, "Test", report);

    // The filter event becomes controller 102 on the track's channel.
    const std::vector<uint8_t> kController = {0xb0, kFilterController, 64};
    bool found = false;
    for (std::size_t i = 0; i + kController.size() <= midi.size() && !found; i++)
    {
        found = std::equal(kController.begin(), kController.end(), midi.begin() + static_cast<std::ptrdiff_t>(i));
    }

    XSFLIB_CHECK(found);
    XSFLIB_CHECK(report.channels == 1);
}

// A performance in which the first `tracks` tracks play a note each. Track 0 also plays two notes of key 60 at once on
// its second part.
Performance PartsWithALayer(int tracks)
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    uint32_t note = 1;
    for (int t = 0; t < tracks; t++)
    {
        perf.tracks[t].push_back({0, EventKind::kNoteOn, 48, 100, 0, 0.0f, note});
        perf.tracks[t].push_back({96, EventKind::kNoteOff, 48, 0, 0, 0.0f, note});
        note++;
    }
    perf.tracks[0].push_back({0, EventKind::kNoteOn, 60, 100, 0, 0.0f, note, 1});
    perf.tracks[0].push_back({48, EventKind::kNoteOn, 60, 100, 0, 0.0f, note + 1, 1});
    perf.tracks[0].push_back({96, EventKind::kNoteOff, 60, 0, 0, 0.0f, note, 1});
    perf.tracks[0].push_back({96, EventKind::kNoteOff, 60, 0, 0, 0.0f, note + 1, 1});
    perf.end_tick = 96;
    perf.presets.push_back(PresetKey{});

    return perf;
}

// The parts that PartsWithALayer's tracks use.
std::array<uint8_t, 16> PartsUsed(int tracks)
{
    std::array<uint8_t, 16> used{};
    std::fill_n(used.begin(), tracks, 1);
    used[0] = 2;

    return used;
}

// With one channel spare, FitParts gives it to track 0's second part by default. With SpareChannels::kForLayers, the
// second note of key 60 takes it instead, and track 0 keeps one part.
void TestFitPartsKeepsAChannelForALayer()
{
    const Performance perf = PartsWithALayer(15);

    const std::array<uint8_t, 16> for_parts = FitParts(perf, PartsUsed(15));
    const std::array<uint8_t, 16> for_layers = FitParts(perf, PartsUsed(15), SpareChannels::kForLayers);

    XSFLIB_CHECK(for_parts[0] == 2);
    XSFLIB_CHECK(for_layers[0] == 1);
    XSFLIB_CHECK(std::count(for_layers.begin(), for_layers.end(), 1) == 16);
}

// With channels to spare for both, track 0 keeps both of its parts.
void TestFitPartsGivesPartsTheChannelsThatLayersLeave()
{
    const Performance perf = PartsWithALayer(14);

    const std::array<uint8_t, 16> limits = FitParts(perf, PartsUsed(14), SpareChannels::kForLayers);

    XSFLIB_CHECK(limits[0] == 2);
}

} // namespace

int main()
{
    TestOverridesReplaceOnlyTheSetValues();
    TestLfoOnTheWiiUAndTheSwitchIsInHzAndMilliseconds();
    TestLfoOnThe3dsAndTheWiiIsInTheBytecodesUnits();
    TestPerformanceWritesAsMidi();
    TestFitPartsKeepsAChannelForALayer();
    TestFitPartsGivesPartsTheChannelsThatLayersLeave();

    if (xsflib::testing::failures != 0)
    {
        std::printf("%d checks failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("All checks passed\n");

    return 0;
}
