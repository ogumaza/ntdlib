// SPDX-License-Identifier: MIT

// Tests of sequence_export on the Wii's engine (Engine::kRevolution), with the Wii's settings (Wii() in
// sequence_export_test.h) and synthetic RSAR archives (sequence_export_revolution_test.h): the LFOs, envelopes and
// filters, and broken files.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_build.h"
#include "sequence_export/wii_pan.h"
#include "sequence_export_revolution_test.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export::wii_tests;

namespace
{
void TestBuildsFrom2008PlayTheBiquad()
{
    // A biquad filter (type 1, value 64) plays in builds from 2008: the SoundFont preset takes the biquad low-pass's
    // row 55, and the SFZ controller the value. Older builds ignore the commands. An envelope reset puts back the
    // region's release after the track's release 10.
    const std::vector<uint8_t> kCommands = {0xb4, 0x01, 0xb5, 0x40, 0xd3, 0x0a, 0xfb, 0x3c, 0x64, 0x30, 0xff};

    const Performance late = PerformCommands(kCommands);
    const Performance early = PerformCommands(kCommands, {.version = 0x0103});

    auto biquad_controller = [](const Performance& p)
    {
        int value = 0;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == kSfzBiquadEvent)
            {
                value = e.value;
            }
        }

        return value;
    };
    XSFLIB_CHECK(late.presets.size() == 1 && late.presets[0].release == 0xff);
    XSFLIB_CHECK(late.presets[0].filter == ToSoundFont(0x40, 1, 64) && late.presets[0].filter.resonance == 17);
    XSFLIB_CHECK(late.biquad_types[0] == 1 && biquad_controller(late) == 64);
    XSFLIB_CHECK(early.presets.size() == 1 && early.presets[0].release == 10);
    XSFLIB_CHECK(early.presets[0].filter == SoundFontFilter{} && early.biquad_types[0] == 0);
}
void TestVolumeLfoAboveFullLevelIsReported()
{
    // A volume LFO at depth 127 and range 1 swings almost 6 dB. At sustain 127, the envelope's gain is at full level.
    // The LFO's upper half lifts the gain above full level. The game's voice holds the gain at full level there
    // (Voice::SetVeVolume). Keys 60 and 62 are each reported once. Both get a fit
    // (TestVolumeLfoAboveFullLevelIsFitted). At sustain 64 (-11.9 dB), with the LFO 500 ms late (lfo_delay 100), the
    // gain stays below full level. A pitch LFO doesn't change the gain.
    const SoundArchive archive =
        SequenceArchive({{0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0xff},
                         {0xd2, 0x40, 0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0xe0, 0x00, 0x64, 0x3c, 0x64, 0x60, 0xff},
                         {0xcc, 0x00, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}});

    const Performance lifted = PerformSound(archive, 0);
    const Performance below = PerformSound(archive, 1);
    const Performance vibrato = PerformSound(archive, 2);

    const char* const kReport =
        "volume LFO above full level (limited in the game; the SoundFont and SFZ swing less, from a lower level)";
    XSFLIB_CHECK(lifted.approximations.count(kReport) == 1 && lifted.approximations.at(kReport) == 2);
    XSFLIB_CHECK(below.approximations.empty() && vibrato.approximations.empty());
}

void TestLaterBuildsLetTheVolumeLfoPassFullLevel()
{
    // As in TestVolumeLfoAboveFullLevelIsReported, a volume LFO lifts a note at full level by up to 5.95 dB. Builds
    // from September 2009 hold the gain only at 0. The game then plays the LFO above full level, as the SoundFont and
    // the SFZ do. Nothing is reported, and the note keeps the LFO's full swing at its level.
    const SoundArchive archive = SequenceArchive({{0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}});
    PerformOptions options;
    options.build = SndBuild{.date = 20090924};

    const Conversion c = ConvertSequence(archive, 0, options);

    const std::vector<PresetKey>& presets = c.performance.presets;
    XSFLIB_CHECK(c.performance.approximations.empty());
    XSFLIB_CHECK(presets.size() == 1 && presets[0].lfo_swing == 16 && presets[0].lfo_cut == 0);
}

void TestVolumeLfoAboveFullLevelIsFitted()
{
    // As in TestVolumeLfoAboveFullLevelIsReported, a volume LFO at depth 127 and range 1 swings 5.95 dB on a note at
    // full level. The game holds the LFO's upper half at full level. The least-squares fit to the game's level halves
    // the swing and lowers the note by a third of the swing (5.95 dB / pi, 1.9 dB). The SoundFont's modulator from CC1
    // to the volume LFO has half the amount, 60 cB. The zone's generator holds 2.5 times the cut of 20 cB: 50 cB
    // (seqlib's ToZoneAttenuation). The SFZ's LFO swings half as far, and the region's volume takes the cut. At
    // sustain 64 with the LFO 500 ms late, the gain stays below full level, and the note keeps the full swing.
    const SoundArchive archive =
        SequenceArchive({{0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff},
                         {0xd2, 0x40, 0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0xe0, 0x00, 0x64, 0x3c, 0x64, 0x60, 0xff}});

    const Conversion lifted = ConvertSequence(archive, 0);
    const Conversion below = ConvertSequence(archive, 1);

    const std::vector<PresetKey>& presets = lifted.performance.presets;
    XSFLIB_CHECK(presets.size() == 1 && presets[0].lfo_swing == 8 && presets[0].lfo_cut == 20);
    XSFLIB_CHECK(below.performance.presets.size() == 1 && below.performance.presets[0].lfo_swing == 16 &&
                 below.performance.presets[0].lfo_cut == 0);

    // The SoundFont: the CC1 modulator's amount, and the attenuation of the zone, whose region is at full volume.
    const sf2::Records v = sf2::Read(lifted.sf2);
    int lfo_amount = -1;
    for (std::size_t m = 0; m < v.Count("imod", 10); m++)
    {
        const uint8_t* r = v.Record("imod", 10, m);
        if (xsflib::LoadLe16(r) == sf2::kSrcModWheel && xsflib::LoadLe16(r + 2) == sf2::kModLfoToVolume)
        {
            lfo_amount = static_cast<int16_t>(xsflib::LoadLe16(r + 4));
        }
    }
    int attenuation = -1;
    for (std::size_t z = 0; z + 1 < v.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> gens = v.ZoneGens(z);
        if (gens.contains(sf2::kSampleId))
        {
            const auto found = gens.find(sf2::kInitialAttenuation);
            attenuation = found == gens.end() ? 0 : found->second;
        }
    }
    XSFLIB_CHECK(seqlib::sf2::Check(lifted.sf2).empty() && lfo_amount == 60 && attenuation == 50);

    // The SFZ: 6 dB * 2 * 127 / 128 at full CC1, halved, and the region 2 dB lower.
    XSFLIB_CHECK(lifted.sfz.files.size() == 1);
    for (const SfzFile& f : lifted.sfz.files)
    {
        XSFLIB_CHECK(f.text.find(" lfo1_volume_oncc1=5.953125") != std::string::npos);
        XSFLIB_CHECK(f.text.find(" volume=-2 ") != std::string::npos);
    }
}

void TestTrackLowPassFitsEachZone()
{
    // A looping wave of a sine at 3 kHz plays through the low-pass at argument 10, AX's 92 Hz. The preset's global zone
    // takes the table's cutoff, 5770 cents. At key 60 alone, the region's zone takes the cutoff at which FluidSynth
    // passes as much of the sine as AX does: 7244 cents (sequence_export_zone_low_pass_test). Keys 48 and 72 need
    // 6615 and 7965 cents. Their zone's cutoff tracks the key, through a modulator from the note-on key.
    std::vector<int16_t> sine;
    for (int n = 0; n < 4096; n++)
    {
        const double value = 10000.0 * std::sin(2.0 * std::numbers::pi * 3000.0 * n / 32000.0);
        sine.push_back(static_cast<int16_t>(std::lround(value)));
    }
    const SoundArchive archive =
        SequenceArchive({{0xd8, 0x0a, 0x3c, 0x64, 0x30, 0xff}, {0xd8, 0x0a, 0x30, 0x64, 0x30, 0x48, 0x64, 0x30, 0xff}},
                        {.wave = Rwav(4096, sine, 1, true)});

    const Conversion one = ConvertSequence(archive, 0);
    const Conversion two = ConvertSequence(archive, 1);

    // The cutoffs of each SoundFont: in the global zone, in the region's zone, and the modulator from the key.
    const auto cutoffs = [](const Conversion& c)
    {
        const sf2::Records v = sf2::Read(c.sf2);
        std::vector<int> found(2, -1);
        for (std::size_t z = 0; z + 1 < v.Count("ibag", 4); z++)
        {
            const std::map<uint16_t, uint16_t> gens = v.ZoneGens(z);
            if (const auto fc = gens.find(sf2::kInitialFilterFc); fc != gens.end())
            {
                found[gens.contains(sf2::kSampleId) ? 1 : 0] = static_cast<int16_t>(fc->second);
            }
        }
        for (std::size_t m = 0; m < v.Count("imod", 10); m++)
        {
            const uint8_t* r = v.Record("imod", 10, m);
            if (xsflib::LoadLe16(r) == sf2::kNoteOnKeyNumber && xsflib::LoadLe16(r + 2) == sf2::kInitialFilterFc)
            {
                found.push_back(static_cast<int16_t>(xsflib::LoadLe16(r + 4)));
            }
        }

        return found;
    };
    const std::vector<int> fixed = cutoffs(one);
    const std::vector<int> tracking = cutoffs(two);
    XSFLIB_CHECK(seqlib::sf2::Check(one.sf2).empty() && seqlib::sf2::Check(two.sf2).empty());
    XSFLIB_CHECK(fixed.size() == 2 && fixed[0] == 5770 && std::abs(fixed[1] - 7244) <= 3);
    XSFLIB_CHECK(tracking.size() == 3 && tracking[0] == 5770 && tracking[2] % 128 == 0);
    if (tracking.size() == 3)
    {
        const int slope = tracking[2] / 128;
        XSFLIB_CHECK(std::abs(tracking[1] + 48 * slope - 6615) <= 15 &&
                     std::abs(tracking[1] + 72 * slope - 7965) <= 15);
    }
}
void TestTrackWaitingForeverEnds()
{
    // After a note, a wait takes its length from player variable 0. The variable is -1, and a negative wait waits for
    // good. Once nothing sounds, the sequence is over. It doesn't run to the time limit.
    const Performance p = PerformCommands({0x3c, 0x64, 0x30, 0xa1, 0x80, 0x00, 0xff});

    XSFLIB_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}
void TestLongestHoldMatchesTheGame()
{
    // The shared TestEnvelopeValuesMatchTheGame checks the other values of the envelope.
    XSFLIB_CHECK(HoldMs(127) == 4096);
}

void TestRegionsHoldComesWith2008()
{
    // Builds before 2008 have no hold, whatever the region gives.
    Region region;
    region.hold = 50;

    XSFLIB_CHECK(RegionEnvelope(region, true).hold == 50 && RegionEnvelope(region, false).hold == 0);
}

void TestEnvelopeSimulationSustainsAndReleases()
{
    // Reach the sustain, then release until silent. Before its first update, a note releases from -90.4 dB, even with
    // an instant attack.
    XSFLIB_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 10) == DecibelSquare(64));
    XSFLIB_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 0) == -904.0f);
    XSFLIB_CHECK(ReleaseFrames(0.0f, DecayRate(127)) == 1);
    XSFLIB_CHECK(ReleaseFrames(-500.0f, 1.0) == 135); // 134 steps of 3 end at -902, and the 135th is silent
    XSFLIB_CHECK(FramesToLevel(EnvelopeValues{127, 127, 127, 0, 127}, -400.0f) == -1);
    XSFLIB_CHECK(FramesToLevel(EnvelopeValues{127, 100, 0, 0, 127}, -400.0f) > 0);

    // Release 114 falls exactly 3 tenths of a dB a frame. From sustain 2 (-72.1 dB), the 61st frame reaches -90.4 dB.
    // That level is already silent.
    const float kSustain2 = DecibelSquare(2);
    XSFLIB_CHECK(ReleaseFrames(kSustain2, DecayRate(114)) == 61);
}

void TestTrackFilterFollowsTheGame()
{
    // Low-pass arguments of 0 and below give the lowest cutoff. Arguments of 58 and above reach 16 kHz and turn AX's
    // filter off. AX's cutoffs are 32000 * 2^(10 (argument - 64) / 64) Hz.
    const std::pair<int, int> kIndex[] = {{-5, 0}, {0, 0}, {30, 30}, {57, 57}, {58, -1}, {0x40, -1}, {255, -1}};
    for (const auto& [arg, index] : kIndex)
    {
        XSFLIB_CHECK(LowPassIndex(arg) == index);
    }

    int off_formula = 0;
    for (int i = 0; i <= 57; i++)
    {
        off_formula += LowPassHz(i, SndBuild{}) != static_cast<int>(32000.0 * std::exp2(10.0 * (i - 64) / 64.0));
    }

    XSFLIB_CHECK(off_formula == 0 && LowPassHz(30, SndBuild{}) == 805);
}

void TestLaterBuildsTakeTheLowPassFromTheTable()
{
    // Builds from November 2008 take the low-pass's cutoff from kLpfFreq, as the Wii U does. Argument 30 gives the
    // value (30 - 64) / 64 + 1 = 0.46875, the Wii U's step 9. The SoundFont and the SFZ take the Wii U's fits for that
    // step. Older builds work the cutoff out: 805 Hz. Arguments 0 to 57 leave the low-pass on in every build.
    const SndBuild kOldBuild{.date = 20080919};
    const SndBuild kNewBuild{.date = 20081120};
    const Settings new_settings = Wii(kNewBuild).settings;
    const ntdlib::sequence_export::FilterTables& cafe = ntdlib::sequence_export::kCafeFilters;

    const GameFilters filters = TrackFilters(30, 0, 0, new_settings);
    const SoundFontFilter sf = ntdlib::sequence_export::ToSoundFont(filters, new_settings);
    XSFLIB_CHECK(filters.low_pass == 30 && sf.cutoff == cafe.low_pass_cutoff[9] &&
                 sf.low_pass_hz == ntdlib::nwsnd::tables::kLpfFreq[9]);
    XSFLIB_CHECK(ntdlib::sequence_export::SfzLowPassCutoff(30, new_settings) == cafe.sfz_low_pass_cutoff[9]);
    XSFLIB_CHECK(LowPassHz(30, kOldBuild) == 805 && LowPassHz(30, kNewBuild) == ntdlib::nwsnd::tables::kLpfFreq[9]);
    XSFLIB_CHECK(LowPassHz(0, kNewBuild) == 80 && TrackFilters(57, 0, 0, new_settings).low_pass == 57 &&
                 TrackFilters(58, 0, 0, new_settings).low_pass == -1);
    XSFLIB_CHECK(&ntdlib::sequence_export::RevolutionFilters(kOldBuild) ==
                 &ntdlib::sequence_export::kRevolutionFilters);
}

void TestNovember2008LowPassSteps()
{
    // The build of November 20 2008 reads the low-pass's cutoff from a table. That build's Util::CalcLpfFreq (main.dol
    // 0x802d9828) gives 80 Hz below 0.13561438 and 16000 Hz, the filter off, from 0.9. Between them, each thirtieth
    // moves one step up the table. Arguments 0 to 10 therefore give 80 Hz, 11 gives 100 Hz, 30 gives 640 Hz, 32 gives
    // 800 Hz, 33 gives 1000 Hz and 57 gives 12800 Hz. From 58 on, the filter is off. The build of September 19 2008
    // works out 1000 Hz at 32. AX then gets the same cutoff from both builds. So do the SoundFont and the SFZ.
    const Settings november = Wii(SndBuild{.date = 20081120}).settings;
    const Settings september = Wii(SndBuild{.date = 20080919}).settings;

    const std::span<const uint16_t> hz = november.filters->low_pass_hz;
    const GameFilters november_33 = TrackFilters(33, 0, 0, november);
    const GameFilters september_32 = TrackFilters(32, 0, 0, september);

    XSFLIB_CHECK(hz.size() == 58 && hz[0] == 80 && hz[10] == 80 && hz[11] == 100 && hz[30] == 640 && hz[32] == 800 &&
                 hz[33] == 1000 && hz[57] == 12800);
    XSFLIB_CHECK(september.filters->low_pass_hz[32] == 1000);
    XSFLIB_CHECK(ntdlib::sequence_export::ToSoundFont(november_33, november) ==
                 ntdlib::sequence_export::ToSoundFont(september_32, september));
    XSFLIB_CHECK(ntdlib::sequence_export::SfzLowPassCutoff(33, november) ==
                 ntdlib::sequence_export::SfzLowPassCutoff(32, september));
}

void TestLaterBuildsMoveTheBendOverATimePrefix()
{
    // A time prefix of 4 ticks before a bend of 64: a semitone up at the default range of 2. Builds from September 2009
    // move the bend there over the 4 ticks, a quarter of the way each tick. Older builds set it at once.
    const std::vector<uint8_t> kCommands = {0xa3, 0xc4, 0x40, 0x00, 0x04, 0x3c, 0x7f, 0x18, 0xff};

    const Performance moving = PerformCommands(kCommands, {.build = SndBuild{.date = 20090924}});
    const Performance jumping = PerformCommands(kCommands, {.build = SndBuild{.date = 20081120}});

    const std::vector<std::pair<uint32_t, float>> kMoving = {{1, 0.25f}, {2, 0.5f}, {3, 0.75f}, {4, 1.0f}};
    const std::vector<std::pair<uint32_t, float>> kJumping = {{0, 1.0f}};
    XSFLIB_CHECK(Pitches(moving) == kMoving && Pitches(jumping) == kJumping);
}

void TestEachLowPassStepMovesTheSoundFontCutoff()
{
    // The shared TestSoundFontFilterStandsInForTheGames checks the rest. On the Wii, low-pass 58 leaves the SoundFont's
    // filter open, without resonance. Each lower setting gives a cutoff strictly lower than the one above it.
    XSFLIB_CHECK(ToSoundFont(58, 0, 0) == SoundFontFilter{});
    XSFLIB_CHECK(ToSoundFont(30, 0, 0).cutoff < kFilterOpen && ToSoundFont(30, 0, 0).resonance == 0);
    int rising = 0;
    for (int arg = 1; arg < 58; arg++)
    {
        rising += ToSoundFont(arg, 0, 0).cutoff <= ToSoundFont(arg - 1, 0, 0).cutoff;
    }

    XSFLIB_CHECK(rising == 0);
}

void TestSfzFilterFollowsTheGame()
{
    // The SFZ controllers: 64 minus the low-pass argument, and the biquad value (0 without a preset). At 6 or less, the
    // low-pass is off.
    XSFLIB_CHECK(SfzLowPassValue(0x40) == 0 && SfzLowPassValue(200) == 0 && SfzLowPassValue(58) == 6);
    XSFLIB_CHECK(SfzLowPassValue(57) == 7 && SfzLowPassValue(0) == 64 && SfzLowPassValue(-100) == 127);
    XSFLIB_CHECK(SfzBiquadValue(1, 64) == 64 && SfzBiquadValue(2, 300) == 127 && SfzBiquadValue(0, 64) == 0);
    XSFLIB_CHECK(SfzBiquadValue(6, 64) == 0 && SfzBiquadValue(3, -5) == 0);

    // A biquad type with a preset and a positive value select a row. Value 64 selects the low-pass's row 55, and 127 or
    // more each table's last. The band-passes scale value * (2 - value).
    XSFLIB_CHECK(BiquadStep(1, 64) == 55 && BiquadStep(1, 255) == 111 && BiquadStep(2, 127) == 96);
    XSFLIB_CHECK(BiquadStep(3, 127) == 121 && BiquadStep(5, 127) == 92 && BiquadStep(4, 64) == 69);
    XSFLIB_CHECK(BiquadStep(1, 0) == -1 && BiquadStep(0, 64) == -1 && BiquadStep(6, 64) == -1);

    // AX's pole for 31 Hz is sfizz's one-pole at 31.2 Hz, 11190 cents below 20 kHz, and lower settings still give lower
    // cutoffs.
    XSFLIB_CHECK(SfzLowPassCutoff(0) == -11190);
    int falling = 0;
    for (int i = 1; i <= 57; i++)
    {
        falling += SfzLowPassCutoff(i) <= SfzLowPassCutoff(i - 1);
    }

    XSFLIB_CHECK(falling == 0);
}

void TestStereoWavesKeepTheirSidesInBalanceMode()
{
    // In balance mode a stereo wave's channels stay on their sides, at the level the pan law gives the side; in dual
    // mode each pans from its side at full level. A mono wave plays at the pan in both.
    const ChannelPan centre = WaveChannelPan(0.0, 0, 2, true);
    const ChannelPan right = WaveChannelPan(0.5, 1, 2, true);
    const ChannelPan dual = WaveChannelPan(0.5, 0, 2, false);
    const ChannelPan mono = WaveChannelPan(0.3, 0, 1, true);

    XSFLIB_CHECK(centre.pan == -1.0 && std::abs(centre.gain_db - 10.0 * std::log10(0.5)) < 1e-9);
    XSFLIB_CHECK(right.pan == 1.0 && std::abs(right.gain_db - 10.0 * std::log10(0.75)) < 1e-9);
    XSFLIB_CHECK(dual.pan == -0.5 && dual.gain_db == 0.0 && mono.pan == 0.3 && mono.gain_db == 0.0);
}

void TestMissingBlockIsReported()
{
    // An RWAV whose header has an INFO block but no DATA block.
    std::vector<uint8_t> rwav = Header("RWAV", 0x0102, 2);
    const std::size_t info = Block(rwav, "INFO", 0);
    Grow(rwav, WaveInfoSize(1));
    EndBlock(rwav, info, 0);

    const std::string message = ErrorOf<xsflib::FormatError>([&] { Wave::ParseFile(rwav); });

    XSFLIB_CHECK(message == "no INFO or DATA block");
}

void TestShortFileIsReported()
{
    const std::string message =
        ErrorOf<xsflib::FormatError>([&] { Wave::ParseFile(std::vector<uint8_t>{'R', 'W', 'A', 'V'}); });

    XSFLIB_CHECK(message == "too short to be an RWAV file");
}

// The tests on the Wii.
const Test kTests[] = {
    TestMissingBlockIsReported,
    TestShortFileIsReported,
    TestLongestHoldMatchesTheGame,
    TestRegionsHoldComesWith2008,
    TestEnvelopeSimulationSustainsAndReleases,
    TestTrackFilterFollowsTheGame,
    TestLaterBuildsTakeTheLowPassFromTheTable,
    TestNovember2008LowPassSteps,
    TestLaterBuildsMoveTheBendOverATimePrefix,
    TestEachLowPassStepMovesTheSoundFontCutoff,
    TestSfzFilterFollowsTheGame,
    TestStereoWavesKeepTheirSidesInBalanceMode,
    TestBuildsFrom2008PlayTheBiquad,
    TestVolumeLfoAboveFullLevelIsReported,
    TestVolumeLfoAboveFullLevelIsFitted,
    TestLaterBuildsLetTheVolumeLfoPassFullLevel,
    TestTrackLowPassFitsEachZone,
    TestTrackWaitingForeverEnds,
};

} // namespace

int main()
{
    RunOn(Wii(), kTests);

    return xsflib::testing::Summary("sequence_export_revolution_filters");
}
