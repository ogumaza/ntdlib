// SPDX-License-Identifier: MIT

// Tests of the low-passes that stand in for the consoles' resampling filters (resampling.h): which notes take them on
// each console, the spans of keys that a region's zones split into, the generators and modulators that a zone takes,
// and the zones, SFZ regions and reports of conversions on the Wii U, the Switch and the 3DS. The Wii's conversion test
// is in sequence_export_revolution_test.cpp.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/sf2/soundfont.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/resampling.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/sf2_build.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;

namespace sf2 = seqlib::sf2;

namespace
{

// A region with original key 60 and no tuning, for a wave of `sample_rate` Hz, with `interpolation`.
ResamplingRegion RegionAt(uint32_t sample_rate, uint8_t interpolation = 0)
{
    return {60, 1.0f, sample_rate, interpolation};
}

// The rule's low-pass above 4/3 (the 8 kHz set or kernel 0) and above 1 (the 12 kHz set or kernel 1).
const ResamplingFilter* Above4Thirds(const Settings& settings)
{
    return &settings.resampling->filters[0];
}

const ResamplingFilter* Above1(const Settings& settings)
{
    return &settings.resampling->filters[1];
}

// On the Wii U, a 32 kHz wave plays at a rate of 1 at its original key. 4 semitones up, the rate is 1.26. 5 semitones
// up, it's 1.335, above 4/3. Linear interpolation (1) and none (2) leave the filters out.
void TestTheWiiUPicksByTheRateAtTheKey()
{
    const Settings settings = CafeSettings();

    XSFLIB_CHECK(ResamplingFilterOf(60, RegionAt(32000), settings) == nullptr);
    XSFLIB_CHECK(ResamplingFilterOf(61, RegionAt(32000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(64, RegionAt(32000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(65, RegionAt(32000), settings) == Above4Thirds(settings));
    XSFLIB_CHECK(ResamplingFilterOf(72, RegionAt(32000, 1), settings) == nullptr);
    XSFLIB_CHECK(ResamplingFilterOf(72, RegionAt(32000, 2), settings) == nullptr);
}

// On the Switch, the renderer mixes at 48 kHz, and every region plays through the filters. A 32 kHz wave passes a rate
// of 1 at key 68 (1.058). At key 72 it plays at exactly 4/3: the renderer's step is 0xAAAA, below 0xAAAB. Key 73 is
// above (1.413).
void TestTheSwitchPicksAt48kHz()
{
    const Settings settings = SwitchSettings();

    XSFLIB_CHECK(ResamplingFilterOf(67, RegionAt(32000), settings) == nullptr);
    XSFLIB_CHECK(ResamplingFilterOf(68, RegionAt(32000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(72, RegionAt(32000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(73, RegionAt(32000), settings) == Above4Thirds(settings));
    XSFLIB_CHECK(ResamplingFilterOf(68, RegionAt(32000, 1), settings) == Above1(settings));
}

// On the 3DS, the DSP mixes at 32728 Hz. A wave at that rate plays unfiltered at its original key, a rate of exactly
// 1. A 32 kHz wave passes 1 at key 61 (1.036) and 4/3 at key 66 (1.383).
void TestThe3dsPicksAt32728Hz()
{
    const Settings settings = CtrSettings();

    XSFLIB_CHECK(ResamplingFilterOf(60, RegionAt(32728), settings) == nullptr);
    XSFLIB_CHECK(ResamplingFilterOf(61, RegionAt(32728), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(60, RegionAt(32000), settings) == nullptr);
    XSFLIB_CHECK(ResamplingFilterOf(61, RegionAt(32000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(65, RegionAt(32000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(66, RegionAt(32000), settings) == Above4Thirds(settings));
    XSFLIB_CHECK(ResamplingFilterOf(66, RegionAt(32000, 2), settings) == nullptr);
}

// On the Wii, NW4R SND picks before the voice has its pitch. A 44.1 kHz wave takes the 8 kHz set at every key, a
// 36 kHz wave the 12 kHz set, and a 32 kHz wave none.
void TestTheWiiPicksByTheWavesRateAlone()
{
    const Settings settings = RevolutionSettings(BuildOfRsarVersion(0x0104));

    XSFLIB_CHECK(ResamplingFilterOf(0, RegionAt(44100), settings) == Above4Thirds(settings));
    XSFLIB_CHECK(ResamplingFilterOf(127, RegionAt(44100), settings) == Above4Thirds(settings));
    XSFLIB_CHECK(ResamplingFilterOf(0, RegionAt(36000), settings) == Above1(settings));
    XSFLIB_CHECK(ResamplingFilterOf(127, RegionAt(32000), settings) == nullptr);
}

// The filters' cutoffs follow the note's pitch: a fraction of the wave's rate times the pitch ratio. The tuning counts.
void TestCutoffsFollowThePitch()
{
    const ResamplingRegion kTuned = {60, 1.5f, 32000, 0};

    XSFLIB_CHECK(ResamplingCutoffHz(0.25, 60, RegionAt(32000)) == 8000.0);
    XSFLIB_CHECK(ResamplingCutoffHz(0.25, 72, RegionAt(32000)) == 16000.0);
    XSFLIB_CHECK(ResamplingCutoffHz(0.25, 48, kTuned) == 6000.0);
}

// On the Wii U, a 32 kHz wave's keys split at 60 and 64.
void TestSpansSplitWhereTheRateCrossesAThreshold()
{
    const Settings settings = CafeSettings();

    const std::vector<ResamplingSpan> spans = ResamplingSpans({0, 127}, RegionAt(32000), settings);

    XSFLIB_CHECK((spans.size() == 3));
    XSFLIB_CHECK((spans.size() == 3 && spans[0].keys == std::pair{0, 60} && spans[0].filter == nullptr));
    XSFLIB_CHECK((spans.size() == 3 && spans[1].keys == std::pair{61, 64} && spans[1].filter == Above1(settings)));
    XSFLIB_CHECK(
        (spans.size() == 3 && spans[2].keys == std::pair{65, 127} && spans[2].filter == Above4Thirds(settings)));
}

// A track's fixed filter at 13000 cents (14.9 kHz) keeps the lower cutoff. The 12 kHz set's low-pass for a 32 kHz wave
// at original key 60 is 0.3899 of the rate: 6691 cents at key 0, so 12991 at key 63 and 13091 at key 64. The 8 kHz
// set's is 0.2518 of the rate: 5934 cents at key 0, so 12934 at key 70 and 13034 at key 71. A preset whose filter
// changes during notes takes no low-pass.
void TestZonesKeepTheLowerCutoff()
{
    const Settings settings = CafeSettings();
    PresetKey key;
    key.filter.cutoff = 13000;
    PresetKey controlled;
    controlled.filter_controller = true;

    const std::vector<ResamplingSpan> spans = ZoneSpans({0, 127}, RegionAt(32000), key, settings);
    const std::vector<ResamplingSpan> none = ZoneSpans({0, 127}, RegionAt(32000), controlled, settings);

    const std::vector<std::pair<std::pair<int, int>, const ResamplingFilter*>> expected = {
        {{0, 60}, nullptr},   {{61, 63}, Above1(settings)}, {{64, 64}, nullptr}, {{65, 70}, Above4Thirds(settings)},
        {{71, 127}, nullptr},
    };
    std::vector<std::pair<std::pair<int, int>, const ResamplingFilter*>> got;
    for (const ResamplingSpan& span : spans)
    {
        got.emplace_back(span.keys, span.filter);
    }
    XSFLIB_CHECK((got == expected));
    XSFLIB_CHECK((none.size() == 1 && none[0].keys == std::pair{0, 127} && none[0].filter == nullptr));
}

// A zone with only its sample generator, sample 7.
sf2::Zone SampleZone()
{
    sf2::Zone zone;
    zone.generators.push_back(sf2::Generator::Value(sf2::kSampleId, 7));

    return zone;
}

// The zone's cutoff generator and its modulator, as (cutoff, source, amount), or -1s.
std::tuple<int, int, int> CutoffOf(const sf2::Zone& zone)
{
    int cutoff = -1;
    for (const sf2::Generator& g : zone.generators)
    {
        if (g.oper == sf2::kInitialFilterFc)
        {
            cutoff = static_cast<int16_t>(g.amount);
        }
    }

    int source = -1;
    int amount = -1;
    for (const sf2::Modulator& m : zone.modulators)
    {
        if (m.dest == sf2::kInitialFilterFc)
        {
            source = m.source;
            amount = m.amount;
        }
    }

    return {cutoff, source, amount};
}

// The generator holds the cutoff at key 0, 64 or 128, the first that keeps it within 1500 to 13500 cents. The sample's
// generator stays last. From key 0, the modulator takes the key as it is (source 3). From key 64, it takes it bipolar
// (0x200). From key 128, it takes it negative (0x100).
void TestTrackingCutoffsStayInRange()
{
    sf2::Zone from_0 = SampleZone();
    sf2::Zone from_64 = SampleZone();
    sf2::Zone from_128 = SampleZone();
    sf2::Zone fixed = SampleZone();

    AddTrackingCutoff(from_0, 5781, 100);
    AddTrackingCutoff(from_64, 1090, 100);
    AddTrackingCutoff(from_128, -5000, 100);
    AddTrackingCutoff(fixed, 9000, 0);

    XSFLIB_CHECK((CutoffOf(from_0) == std::tuple{5781, 0x0003, 12800}));
    XSFLIB_CHECK((CutoffOf(from_64) == std::tuple{7490, 0x0203, 6400}));
    XSFLIB_CHECK((CutoffOf(from_128) == std::tuple{7800, 0x0103, -12800}));
    XSFLIB_CHECK((CutoffOf(fixed) == std::tuple{9000, -1, -1}));
    for (const sf2::Zone* zone : {&from_0, &from_64, &from_128, &fixed})
    {
        XSFLIB_CHECK(zone->generators.size() == 2 && zone->generators.back().oper == sf2::kSampleId);
    }
}

// A zone of a preset whose track filter has a resonance takes a resonance of 0 with the low-pass.
void TestALowPassClearsTheTracksResonance()
{
    const Settings settings = CafeSettings();
    PresetKey resonant;
    resonant.filter = {13000, 60};
    sf2::Zone zone = SampleZone();

    AddResamplingFilter(zone, *Above4Thirds(settings), RegionAt(32000), resonant);

    XSFLIB_CHECK(zone.generators.size() == 3 && zone.generators[0].oper == sf2::kInitialFilterFc &&
                 static_cast<int16_t>(zone.generators[0].amount) == 5934 &&
                 zone.generators[1].oper == sf2::kInitialFilterQ && zone.generators[1].amount == 0 &&
                 zone.generators[2].oper == sf2::kSampleId);
}

// A sound archive whose one sequence plays `commands` on a bank with one region for every key, of original key 60, and
// a looping 32 kHz wave.
SoundArchive OneRegionArchive(const std::vector<uint8_t>& commands)
{
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(commands);
    files[1].data = OneRegionFbnk({{0x05000000, 0}});
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});

    return SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                              console->settings.archive);
}

// The region of a 32 kHz wave at original key 60 splits where the rate passes 1 and 4/3: after keys 60 and 64 on the
// Wii U, 67 and 72 on the Switch, and 60 and 65 on the 3DS. The zones above each threshold take that filter's
// low-pass: the cutoff at key 0 and a modulator that raises it by 100 cents a key. At key 0 the wave plays at 1000 Hz.
// The cutoffs of AX's sets are 0.3899 and 0.2518 of that on the Wii U, 6691 and 5934 cents, and 0.3889 and 0.2748 on
// the Switch, 6686 and 6085 cents. The 3DS's kernels take 0.3928 and 0.2549 of it, 6704 and 5955 cents. The SFZ
// regions take lpf_1p at the original key: at 0.3014 and 0.1784 of 32 kHz on the Wii U, 0.2640 and 0.1663 on the
// Switch, and 0.3007 and 0.1779 on the 3DS.
void TestZonesTakeTheLowPassesAboveTheThresholds()
{
    const SoundArchive archive = OneRegionArchive({0x48, 0x7f, 0x30, 0xff});

    const Conversion c = ConvertSequence(archive, 0);

    const int above_1 = ByConsole(61, 68, 61);
    const int above_4_thirds = ByConsole(65, 73, 66);
    const std::vector<ZoneCutoff> zones = {
        {{0, above_1 - 1}},
        {{above_1, above_4_thirds - 1}, ByConsole(6691, 6686, 6704), 0x0003, 12800},
        {{above_4_thirds, 127}, ByConsole(5934, 6085, 5955), 0x0003, 12800},
    };
    XSFLIB_CHECK(Sf2Problem(c.sf2).empty() && ZoneCutoffs(c.sf2) == zones);

    const std::string above_1_cutoff = ByConsole<std::string>("9644.8", "8448", "9622.4");
    const std::string above_4_thirds_cutoff = ByConsole<std::string>("5708.8", "5321.6", "5692.8");
    const std::string text = c.sfz.files.size() == 1 ? c.sfz.files[0].text : "";
    XSFLIB_CHECK(RegionLowPass(text, {0, above_1 - 1}).empty());
    XSFLIB_CHECK(RegionLowPass(text, {above_1, above_4_thirds - 1}) ==
                 " fil_type=lpf_1p cutoff=" + above_1_cutoff + " fil_keytrack=100 fil_keycenter=60");
    XSFLIB_CHECK(RegionLowPass(text, {above_4_thirds, 127}) ==
                 " fil_type=lpf_1p cutoff=" + above_4_thirds_cutoff + " fil_keytrack=100 fil_keycenter=60");
    XSFLIB_CHECK(c.performance.approximations.empty());
}

// A track that changes its low-pass during a note sends its filter as a controller (kFilterController). Key 72 plays
// above a rate of 1 on every console. The SoundFont leaves the resampling low-pass out of the zones, and the note
// reports it. The SFZ region keeps the low-pass, in filter slot 2. The track's low-pass takes slot 1.
void TestAMovingFilterLeavesTheLowPassOutOfTheSoundFont()
{
    const SoundArchive archive =
        OneRegionArchive({0xc7, 0x00, 0x48, 0x7f, 0x30, 0x80, 0x0c, 0xd8, 0x10, 0x80, 0x24, 0xff});

    const Conversion c = ConvertSequence(archive, 0);

    const std::vector<ZoneCutoff> kZones = {{{0, 127}}};
    const auto reports = c.performance.approximations.find(kResamplingWithController);
    XSFLIB_CHECK(Sf2Problem(c.sf2).empty() && ZoneCutoffs(c.sf2) == kZones);
    XSFLIB_CHECK(reports != c.performance.approximations.end() && reports->second == 1);

    const int above_4_thirds = ByConsole(65, 73, 66);
    const std::string text = c.sfz.files.size() == 1 ? c.sfz.files[0].text : "";
    XSFLIB_CHECK(RegionLowPass(text, {above_4_thirds, 127}).starts_with(" fil2_type=lpf_1p cutoff2="));
}

// The conversion tests, on the Wii U, the Switch and the 3DS.
constexpr Test kConversionTests[] = {
    TestZonesTakeTheLowPassesAboveTheThresholds,
    TestAMovingFilterLeavesTheLowPassOutOfTheSoundFont,
};

} // namespace

int main()
{
    TestTheWiiUPicksByTheRateAtTheKey();
    TestTheSwitchPicksAt48kHz();
    TestThe3dsPicksAt32728Hz();
    TestTheWiiPicksByTheWavesRateAlone();
    TestCutoffsFollowThePitch();
    TestSpansSplitWhereTheRateCrossesAThreshold();
    TestZonesKeepTheLowerCutoff();
    TestTrackingCutoffsStayInRange();
    TestALowPassClearsTheTracksResonance();
    RunOn(WiiU(), kConversionTests);
    RunOn(Switch(), kConversionTests);
    RunOn(Ctr(), kConversionTests);

    return xsflib::testing::Summary("sequence_export_resampling");
}
