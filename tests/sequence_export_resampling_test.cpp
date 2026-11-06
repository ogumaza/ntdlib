// SPDX-License-Identifier: MIT

// Tests of the low-passes that stand in for the consoles' resampling filters (resampling.h): which notes take them on
// each console, the spans of keys that a region's zones split into, the generators and modulators that a zone takes,
// and the zones, SFZ regions and reports of conversions on the Wii U, the Switch and the 3DS. The Wii's conversion test
// is in sequence_export_revolution_test.cpp.

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/sf2/reader.h"
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

// A region with original key 60 and no tuning, for a wave of `sample_rate` Hz, with `interpolation`. Its notes hold
// their pitch, and the keys past a limit play at the limit (ResamplingRegion::at_limits).
ResamplingRegion RegionAt(uint32_t sample_rate, uint8_t interpolation = 0)
{
    return {60, 1.0f, sample_rate, interpolation, true};
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

// A span as (keys, low-pass, tuning at a limit).
using SpanView = std::tuple<std::pair<int, int>, const ResamplingFilter*, std::optional<int>>;

// The spans as SpanViews, in order.
std::vector<SpanView> ViewsOf(const std::vector<ResamplingSpan>& spans)
{
    std::vector<SpanView> got;
    for (const ResamplingSpan& span : spans)
    {
        got.emplace_back(span.keys, span.filter, span.limit_cents);
    }

    return got;
}

// On the Wii U, a 32 kHz wave's keys split at 60 and 64, and at the rate limit. Key 96 plays at a rate of exactly 8.
// From key 97 (8.48), the notes play at the limit, 3600 cents above the wave's rate, without a low-pass.
void TestSpansSplitWhereTheRateCrossesAThreshold()
{
    const Settings settings = CafeSettings();

    const std::vector<ResamplingSpan> spans = ResamplingSpans({0, 127}, RegionAt(32000), settings);

    const std::vector<SpanView> expected = {
        {{0, 60}, nullptr, std::nullopt},
        {{61, 64}, Above1(settings), std::nullopt},
        {{65, 96}, Above4Thirds(settings), std::nullopt},
        {{97, 127}, nullptr, 3600},
    };
    XSFLIB_CHECK((ViewsOf(spans) == expected));
}

// The Wii U plays a voice at a rate of 8 at most: 256 kHz of wave at its 32 kHz output. A 32 kHz wave plays there at a
// pitch of 8, 3600 cents up, and a 48 kHz wave at 5.33, 2898 cents up. A tuning of 1.5 at original key 60 takes a
// 32 kHz wave past the limit at key 89 (a pitch of 8.01). Key 88 plays at 7.56. A range that stays below the limit
// keeps one span. The Wii U has no lowest pitch. The 3DS has no limits.
void TestKeysPastTheWiiUsRateLimitPlayAtIt()
{
    const Settings settings = CafeSettings();
    const ResamplingRegion tuned = {60, 1.5f, 32000, 0, true};

    const std::vector<ResamplingSpan> spans = PitchLimitSpans({0, 127}, tuned, settings);
    const std::vector<ResamplingSpan> below = PitchLimitSpans({0, 88}, tuned, settings);
    const std::vector<ResamplingSpan> above = PitchLimitSpans({100, 127}, tuned, settings);

    const std::vector<SpanView> expected = {{{0, 88}, nullptr, std::nullopt}, {{89, 127}, nullptr, 3600}};
    XSFLIB_CHECK((PitchLimits(RegionAt(32000), settings) == std::pair{0.0, 8.0}));
    XSFLIB_CHECK(PitchLimits(RegionAt(48000), settings).second == 8.0 * 32000 / 48000);
    XSFLIB_CHECK((ViewsOf(spans) == expected));
    XSFLIB_CHECK((ViewsOf(below) == std::vector<SpanView>{{{0, 88}, nullptr, std::nullopt}}));
    XSFLIB_CHECK((ViewsOf(above) == std::vector<SpanView>{{{100, 127}, nullptr, 3600}}));
    XSFLIB_CHECK(
        (ViewsOf(PitchLimitSpans({0, 127}, RegionAt(48000), settings)).back() == SpanView{{89, 127}, nullptr, 2898}));
    XSFLIB_CHECK((ViewsOf(PitchLimitSpans({0, 127}, tuned, CtrSettings())) ==
                  std::vector<SpanView>{{{0, 127}, nullptr, std::nullopt}}));
}

// The Switch holds a voice's pitch ratio between 1/16 and 16, whatever the wave's rate. A wave at original key 60 plays
// below 1/16 up to key 11, and above 16 from key 109. Those keys play at 1/16 and 16: 4800 cents below and above the
// wave's rate. Between them, the low-passes split the keys at 67 and 72 (TestTheSwitchPicksAt48kHz). A tuning of 0.5
// moves both limits up an octave.
void TestTheSwitchHoldsThePitchBetween1_16And16()
{
    const Settings settings = SwitchSettings();
    const ResamplingRegion tuned = {60, 0.5f, 32000, 0, true};

    const std::vector<ResamplingSpan> spans = ResamplingSpans({0, 127}, RegionAt(32000), settings);
    const std::vector<ResamplingSpan> octave_up = PitchLimitSpans({0, 127}, tuned, settings);

    const std::vector<SpanView> expected = {
        {{0, 11}, nullptr, -4800},
        {{12, 67}, nullptr, std::nullopt},
        {{68, 72}, Above1(settings), std::nullopt},
        {{73, 108}, Above4Thirds(settings), std::nullopt},
        {{109, 127}, nullptr, 4800},
    };
    const std::vector<SpanView> expected_up = {
        {{0, 23}, nullptr, -4800}, {{24, 120}, nullptr, std::nullopt}, {{121, 127}, nullptr, 4800}};
    XSFLIB_CHECK((PitchLimits(RegionAt(44100), settings) == std::pair{1.0 / 16.0, 16.0}));
    XSFLIB_CHECK((ViewsOf(spans) == expected));
    XSFLIB_CHECK((ViewsOf(octave_up) == expected_up));
}

// A region whose notes don't hold their pitch keeps its keys past the limits in its spans. A bend or a sweep moves them
// as in MIDI. PastPitchLimit finds the keys past a limit all the same: on the Wii U from key 97, and on the Switch up
// to key 11 and from key 109. The 3DS has no limits.
void TestARegionWhoseNotesMoveKeepsItsKeys()
{
    const ResamplingRegion moving = {60, 1.0f, 32000, 0, false};

    XSFLIB_CHECK((ViewsOf(PitchLimitSpans({0, 127}, moving, CafeSettings())) ==
                  std::vector<SpanView>{{{0, 127}, nullptr, std::nullopt}}));
    XSFLIB_CHECK(ViewsOf(ResamplingSpans({0, 127}, moving, SwitchSettings())).size() == 3);
    XSFLIB_CHECK(!PastPitchLimit(96, moving, CafeSettings()) && PastPitchLimit(97, moving, CafeSettings()));
    XSFLIB_CHECK(PastPitchLimit(11, moving, SwitchSettings()) && !PastPitchLimit(12, moving, SwitchSettings()));
    XSFLIB_CHECK(!PastPitchLimit(108, moving, SwitchSettings()) && PastPitchLimit(109, moving, SwitchSettings()));
    XSFLIB_CHECK(!PastPitchLimit(0, moving, CtrSettings()) && !PastPitchLimit(127, moving, CtrSettings()));
}

// A track's fixed filter at 13000 cents (14.9 kHz) keeps the lower cutoff. The 12 kHz set's low-pass for a 32 kHz wave
// at original key 60 is 0.3899 of the rate: 6691 cents at key 0, so 12991 at key 63 and 13091 at key 64. The 8 kHz
// set's is 0.2518 of the rate: 5934 cents at key 0, so 12934 at key 70 and 13034 at key 71. A preset whose filter
// changes during notes takes no low-pass. Both keep the span at the rate limit, from key 97.
void TestZonesKeepTheLowerCutoff()
{
    const Settings settings = CafeSettings();
    PresetKey key;
    key.filter.cutoff = 13000;
    PresetKey controlled;
    controlled.filter_controller = true;

    const std::vector<ResamplingSpan> spans = ZoneSpans({0, 127}, RegionAt(32000), key, settings);
    const std::vector<ResamplingSpan> none = ZoneSpans({0, 127}, RegionAt(32000), controlled, settings);

    const std::vector<SpanView> expected = {
        {{0, 60}, nullptr, std::nullopt},  {{61, 63}, Above1(settings), std::nullopt},
        {{64, 64}, nullptr, std::nullopt}, {{65, 70}, Above4Thirds(settings), std::nullopt},
        {{71, 96}, nullptr, std::nullopt}, {{97, 127}, nullptr, 3600},
    };
    const std::vector<SpanView> expected_none = {{{0, 96}, nullptr, std::nullopt}, {{97, 127}, nullptr, 3600}};
    XSFLIB_CHECK((ViewsOf(spans) == expected));
    XSFLIB_CHECK((ViewsOf(none) == expected_none));
}

// The operators and amounts of a zone's generators, in order.
std::vector<std::pair<int, int>> GeneratorsOf(const sf2::Zone& zone)
{
    std::vector<std::pair<int, int>> got;
    for (const sf2::Generator& g : zone.generators)
    {
        got.emplace_back(g.oper, static_cast<int16_t>(g.amount));
    }

    return got;
}

// A zone at a pitch limit drops its coarse and fine tuning and takes the limit's, with a scale tuning of 0. 3600 and
// -4800 cents need no fine tuning. The sample's generator stays last.
void TestAZoneAtAPitchLimitPlaysEveryKeyThere()
{
    sf2::Zone whole;
    whole.generators = {sf2::Generator::Value(sf2::kOverridingRootKey, 60), sf2::Generator::Value(sf2::kCoarseTune, 5),
                        sf2::Generator::Value(sf2::kFineTune, -3), sf2::Generator::Value(sf2::kSampleId, 7)};
    sf2::Zone fine = whole;
    sf2::Zone down = whole;

    PlayAtFixedPitch(whole, 3600);
    PlayAtFixedPitch(fine, 2898);
    PlayAtFixedPitch(down, -4800);

    const std::vector<std::pair<int, int>> expected_whole = {
        {sf2::kOverridingRootKey, 60}, {sf2::kCoarseTune, 36}, {sf2::kScaleTuning, 0}, {sf2::kSampleId, 7}};
    const std::vector<std::pair<int, int>> expected_fine = {{sf2::kOverridingRootKey, 60},
                                                            {sf2::kCoarseTune, 28},
                                                            {sf2::kFineTune, 98},
                                                            {sf2::kScaleTuning, 0},
                                                            {sf2::kSampleId, 7}};
    const std::vector<std::pair<int, int>> expected_down = {
        {sf2::kOverridingRootKey, 60}, {sf2::kCoarseTune, -48}, {sf2::kScaleTuning, 0}, {sf2::kSampleId, 7}};
    XSFLIB_CHECK((GeneratorsOf(whole) == expected_whole));
    XSFLIB_CHECK((GeneratorsOf(fine) == expected_fine));
    XSFLIB_CHECK((GeneratorsOf(down) == expected_down));
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

// The coarse tuning and the scale tuning of the sample zone for `keys` in the SoundFont `d`, or -1s.
std::pair<int, int> ZoneTuning(const std::vector<uint8_t>& d, std::pair<int, int> keys)
{
    const sf2::Records v = sf2::Read(d);
    for (std::size_t z = 0; z + 1 < v.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> gens = v.ZoneGens(z);
        const auto range = gens.find(sf2::kKeyRange);
        if (gens.contains(sf2::kSampleId) && range != gens.end() && range->second == (keys.second << 8 | keys.first))
        {
            const auto value = [&gens](uint16_t oper)
            {
                return gens.contains(oper) ? static_cast<int>(static_cast<int16_t>(gens.at(oper))) : -1;
            };
            return {value(sf2::kCoarseTune), value(sf2::kScaleTuning)};
        }
    }

    return {-1, -1};
}

// The line of the first SFZ region in `text` for keys `keys`, from its keys on, or "no region".
std::string RegionLine(const std::string& text, std::pair<int, int> keys)
{
    const std::string at = " lokey=" + std::to_string(keys.first) + " hikey=" + std::to_string(keys.second) + " ";
    const std::size_t found = text.find(at);

    return found == std::string::npos ? "no region" : text.substr(found, text.find('\n', found) - found);
}

// The zones that a region of keys 0 to 127 splits into at the console's pitch limits, with the zones `between` between
// the keys `limits` and the zones at the limits around them, which have no cutoff.
std::vector<ZoneCutoff> WithLimitZones(const std::vector<ZoneCutoff>& between, std::pair<int, int> limits)
{
    std::vector<ZoneCutoff> zones;
    if (limits.first > 0)
    {
        zones.push_back({{0, limits.first - 1}});
    }
    zones.insert(zones.end(), between.begin(), between.end());
    if (limits.second < 127)
    {
        zones.push_back({{limits.second + 1, 127}});
    }

    return zones;
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

// A note past a pitch limit that holds its pitch gives its preset zones and regions at the limits: key 100 on the Wii U
// and the 3DS, and key 5 on the Switch. The region of a 32 kHz wave at original key 60 splits at the thresholds as
// above. On the Wii U, the keys from 97 play at the rate limit, 3600 cents above the wave's rate at every key. On the
// Switch, the keys up to 11 play at the pitch floor, 4800 cents below, and the keys from 109 at the ceiling, 4800
// cents above. Their zones and regions take no low-pass. The 3DS has no limits. A preset whose filter changes during
// notes splits at the limits too.
void TestAStillNotePastALimitPlaysAtIt()
{
    const uint8_t key = ByConsole<uint8_t>(100, 5, 100);
    const SoundArchive archive = OneRegionArchive({key, 0x7f, 0x30, 0xff});

    const Conversion c = ConvertSequence(archive, 0);

    const int above_1 = ByConsole(61, 68, 61);
    const int above_4_thirds = ByConsole(65, 73, 66);
    const std::pair<int, int> limits = ByConsole(std::pair{0, 96}, std::pair{12, 108}, std::pair{0, 127});
    const std::vector<ZoneCutoff> zones = WithLimitZones(
        {
            {{limits.first, above_1 - 1}},
            {{above_1, above_4_thirds - 1}, ByConsole(6691, 6686, 6704), 0x0003, 12800},
            {{above_4_thirds, limits.second}, ByConsole(5934, 6085, 5955), 0x0003, 12800},
        },
        limits);
    XSFLIB_CHECK(Sf2Problem(c.sf2).empty() && ZoneCutoffs(c.sf2) == zones);

    const std::string text = c.sfz.files.size() == 1 ? c.sfz.files[0].text : "";
    XSFLIB_CHECK(RegionLowPass(text, {limits.first, above_1 - 1}).empty());
    for (const auto& [keys, coarse] : ByConsole<std::vector<std::pair<std::pair<int, int>, int>>>(
             {{{97, 127}, 36}}, {{{0, 11}, -48}, {{109, 127}, 48}}, {}))
    {
        const std::string line = RegionLine(text, keys);
        XSFLIB_CHECK(line.find(" pitch_keytrack=0 transpose=" + std::to_string(coarse) + " ") != std::string::npos &&
                     line.find("pitch_keycenter") == std::string::npos && RegionLowPass(text, keys).empty());
        XSFLIB_CHECK((ZoneTuning(c.sf2, keys) == std::pair{coarse, 0}));
    }
    XSFLIB_CHECK(c.performance.approximations.empty());

    const Conversion moving_filter =
        ConvertSequence(OneRegionArchive({0xc7, 0x00, key, 0x7f, 0x30, 0x80, 0x0c, 0xd8, 0x10, 0x80, 0x24, 0xff}), 0);
    XSFLIB_CHECK(Sf2Problem(moving_filter.sf2).empty() &&
                 ZoneCutoffs(moving_filter.sf2) == WithLimitZones({{limits}}, limits));
}

// A note past a pitch limit whose pitch moves plays every key at its pitch, as before the limits: with a sweep from 4
// semitones up (sweep pitch 256, in 64ths), and with a bend of half the range. In the game the note stays at the
// limit while its pitch would pass it. A zone at the limit would move it away from there.
void TestANotePastALimitThatMovesKeepsItsPitch()
{
    const uint8_t key = ByConsole<uint8_t>(100, 5, 100);
    const int above_1 = ByConsole(61, 68, 61);
    const int above_4_thirds = ByConsole(65, 73, 66);
    const std::vector<ZoneCutoff> zones = {
        {{0, above_1 - 1}},
        {{above_1, above_4_thirds - 1}, ByConsole(6691, 6686, 6704), 0x0003, 12800},
        {{above_4_thirds, 127}, ByConsole(5934, 6085, 5955), 0x0003, 12800},
    };
    for (const std::vector<uint8_t>& commands :
         {Code({0xe3, U16(0x0100), key, 0x7f, 0x30, 0xff}), std::vector<uint8_t>{0xc4, 0x40, key, 0x7f, 0x30, 0xff}})
    {
        const Conversion c = ConvertSequence(OneRegionArchive(commands), 0);

        const std::string text = c.sfz.files.size() == 1 ? c.sfz.files[0].text : "";
        XSFLIB_CHECK(Sf2Problem(c.sf2).empty() && ZoneCutoffs(c.sf2) == zones);
        XSFLIB_CHECK(!text.empty() && text.find("pitch_keytrack=0") == std::string::npos);
    }
}

// The conversion tests, on the Wii U, the Switch and the 3DS.
constexpr Test kConversionTests[] = {
    TestZonesTakeTheLowPassesAboveTheThresholds,
    TestAMovingFilterLeavesTheLowPassOutOfTheSoundFont,
    TestAStillNotePastALimitPlaysAtIt,
    TestANotePastALimitThatMovesKeepsItsPitch,
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
    TestKeysPastTheWiiUsRateLimitPlayAtIt();
    TestTheSwitchHoldsThePitchBetween1_16And16();
    TestARegionWhoseNotesMoveKeepsItsKeys();
    TestAZoneAtAPitchLimitPlaysEveryKeyThere();
    TestTrackingCutoffsStayInRange();
    TestALowPassClearsTheTracksResonance();
    RunOn(WiiU(), kConversionTests);
    RunOn(Switch(), kConversionTests);
    RunOn(Ctr(), kConversionTests);

    return xsflib::testing::Summary("sequence_export_resampling");
}
