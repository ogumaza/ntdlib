// SPDX-License-Identifier: MIT

// Unit tests of the levels that conversions of Nintendo R&D2's songs write: the PSG tables and tuning, the levels, pans
// and envelopes of the PSG and sample voices, and the order of the levels. They build small synthetic cartridges in
// memory. No game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/rd2/convert.h"
#include "ntdlib/sequence_export/rd2/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/rd2_cart.h"

namespace ntdlib::sequence_export::rd2
{
namespace
{

using namespace ntdlib::testing::rd2_cart;

namespace cc = seqlib::midi::cc;
using ntdlib::agb::kFrameRate;
using ntdlib::agb::Rom;
using ntdlib::rd2::DetectDriver;
using ntdlib::rd2::DriverInfo;
using ntdlib::rd2::DriverOverrides;
using ntdlib::rd2::Revision;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::LevelsToControllers;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempSettings;

// Returns the driver's tables in `rom`. The ROM must have them.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

void TestPsgTables()
{
    // A square that steps through a table of duties, a frame each, holds the last one, 75%. A noise holds the last of
    // its widths, the 15-bit noise. Each SoundFont zone plays what its voice holds.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kSquareSteps})
                         .Note(0x30, 12, 127)
                         .Wait(12)
                         .Bytes({0xC2, kNoiseSteps})
                         .Note(0x30, 12, 127)
                         .Wait(12)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_psg_tables");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    std::vector<std::string> squares;
    std::vector<int> noises;
    for (size_t s = 0; s + 1 < sf.Count("shdr", 46); s++)
    {
        const std::string name(reinterpret_cast<const char*>(sf.Record("shdr", 46, s)), 20);
        const std::string trimmed = name.substr(0, name.find('\0'));
        if (trimmed.rfind("Square", 0) == 0)
        {
            squares.push_back(trimmed);
        }
        if (trimmed.rfind("Noise ", 0) == 0)
        {
            noises.push_back(std::stoi(trimmed.substr(6), nullptr, 16));
        }
    }
    SEQLIB_CHECK(squares == std::vector<std::string>{"Square 75%"});
    SEQLIB_CHECK(noises.size() == 1 && (noises[0] & 8) == 0);
}

// Returns the zones after the global zone of each instrument of `sf` whose name ends in `suffix`.
std::vector<size_t> ZonesOf(const Sf2Records& sf, const std::string& suffix)
{
    std::vector<size_t> zones;
    for (size_t i = 0; i + 1 < sf.Count("inst", 22); i++)
    {
        const std::string name(reinterpret_cast<const char*>(sf.Record("inst", 22, i)));
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            const size_t first = Le16(sf.Record("inst", 22, i) + 20) + 1;
            for (size_t z = first; z < Le16(sf.Record("inst", 22, i + 1) + 20); z++)
            {
                zones.push_back(z);
            }
        }
    }

    return zones;
}

void TestPsgTuning()
{
    // The driver plays a square at the setting of its pitch index in the frequency table, and the wave an octave lower.
    // Each setting here is about 40 cents above equal temperament. The zones of a square and of the wave are then tuned
    // up by the cents of their entry.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kSquare})
                         .Note(0x30, 4, 127)
                         .Wait(4)
                         .Bytes({0xC2, kWave})
                         .Note(48, 4, 127)
                         .Wait(4)
                         .End()}});
    const auto setting = [](int index)
    {
        const double hz = 261.6255653005986 * std::pow(2.0, (index - 0x30 + 40 / 100.0) / 12.0);
        return int(std::lround(2048 - 131072 / hz));
    };
    for (int i = 0x18; i <= 0x78; i++)
    {
        cart.Put16(kFrequencyTable + 2 * uint32_t(i), uint16_t(setting(i)));
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_psg_tuning");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    for (const auto& [suffix, index_of_key] : {std::pair{" instrument 2", -12}, std::pair{" instrument 9", 0}})
    {
        const std::vector<size_t> zones = ZonesOf(sf, suffix);
        SEQLIB_REQUIRE_EQ(zones.size(), 1);
        const auto gens = sf.ZoneGens(zones[0]);
        const int key = gens.at(sf2::kKeyRange) & 0xFF;
        const int index = key + index_of_key;
        const double hz = 131072.0 / (2048 - setting(index));
        const int cents =
            int(std::lround(1200 * std::log2(hz / (261.6255653005986 * std::pow(2.0, (index - 0x30) / 12.0)))));
        SEQLIB_CHECK(cents >= 36 && cents <= 44); // a setting moves a pitch near middle C by about 3.5 cents
        SEQLIB_CHECK(!gens.count(sf2::kCoarseTune));
        SEQLIB_CHECK(gens.count(sf2::kFineTune) && int16_t(gens.at(sf2::kFineTune)) == cents);
    }
}

void TestPsgSampleLevels()
{
    // On the GBA, a square at full volume, the noise and a wave from 0 to 15 all swing between the same two levels.
    // Their samples therefore span the same range: 2 × PsgShare() × 32767. A full-scale sample spans 2 × 32767.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kSquare})
                         .Note(0x30, 4, 127)
                         .Wait(4)
                         .Bytes({0xC2, kNoise})
                         .Note(0x30, 4, 127)
                         .Wait(4)
                         .Bytes({0xC2, kWave})
                         .Note(48, 4, 127)
                         .Wait(4)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_psg_levels");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    std::vector<std::pair<std::string, int>> spans;
    for (size_t s = 0; s + 1 < sf.Count("shdr", 46); s++)
    {
        const uint8_t* header = sf.Record("shdr", 46, s);
        const std::string name(reinterpret_cast<const char*>(header), 20);
        int low = 0;
        int high = 0;
        for (uint32_t i = Le32(header + 20); i < Le32(header + 24); i++)
        {
            const int point = int16_t(Le16(&pcm[2 * i]));
            low = std::min(low, point);
            high = std::max(high, point);
        }
        spans.emplace_back(name.substr(0, name.find(' ')), high - low);
    }

    const double expected = 2 * PsgShare(Revision::kLinkToThePast) * 32767;
    for (const char* kind : {"Square", "Noise", "Wave"})
    {
        const auto it = std::find_if(spans.begin(), spans.end(), [&](const auto& s) { return s.first == kind; });
        SEQLIB_REQUIRE(it != spans.end());
        SEQLIB_CHECK(std::fabs(it->second - expected) <= 1);
    }
}

void TestDrumPanAndWave()
{
    // A drum keeps its pan whatever its track's pan is. So the drum kit's zones replace the default modulator from CC10
    // with one of no amount, and the plain instrument's zone keeps the default. The wave voice leaves its region's
    // envelope out, and its notes' CC11 follows its release (TestWaveVolume()). Its zone has no envelope.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x60, 0xC2, kDrums})
                         .Note(36, 4, 100)
                         .Note(37, 4, 100)
                         .Wait(4)
                         .Note(38, 4, 100)
                         .Wait(4)
                         .Bytes({0xC2, kWave})
                         .Note(48, 8, 100)
                         .Wait(8)
                         .Bytes({0xC2, kPlain})
                         .Note(60, 4, 100)
                         .Wait(4)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_drum_wave");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));

    auto keeps_pan = [&](size_t zone)
    {
        bool found = false;
        for (size_t m = Le16(sf.Record("ibag", 4, zone) + 2); m < Le16(sf.Record("ibag", 4, zone + 1) + 2); m++)
        {
            const uint8_t* mod = sf.Record("imod", 10, m);
            found = found || (Le16(mod) == 0x028A && Le16(mod + 2) == sf2::kPan && Le16(mod + 4) == 0);
        }

        return found;
    };
    const std::vector<size_t> drums = ZonesOf(sf, " instrument 3"), plain = ZonesOf(sf, " instrument 1");
    const std::vector<size_t> wave = ZonesOf(sf, " instrument 9");
    SEQLIB_REQUIRE_EQ(drums.size(), 3);
    SEQLIB_CHECK(std::all_of(drums.begin(), drums.end(), keeps_pan));

    // The sample drum's pan of 0x20 gives it 95/63 of the left level at the centre and 32/64 of the right level. Pan
    // -296 gives the sides that ratio under a sin/cos pan law. The square drum at the centre plays in the middle. The
    // one at 0x50 plays on the right alone.
    const auto sample_drum = sf.ZoneGens(drums[0]), right_drum = sf.ZoneGens(drums[2]);
    SEQLIB_CHECK(sample_drum.count(sf2::kPan) && int16_t(sample_drum.at(sf2::kPan)) == -296);
    SEQLIB_CHECK(!sf.ZoneGens(drums[1]).count(sf2::kPan));
    SEQLIB_CHECK(right_drum.count(sf2::kPan) && int16_t(right_drum.at(sf2::kPan)) == 500);
    SEQLIB_CHECK(plain.size() == 1 && !keeps_pan(plain[0]));
    SEQLIB_REQUIRE_EQ(wave.size(), 1);
    const auto gens = sf.ZoneGens(wave[0]);
    for (uint16_t op : {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv,
                        sf2::kSustainVolEnv, sf2::kReleaseVolEnv})
    {
        SEQLIB_CHECK(!gens.count(op));
    }
}

// Returns the values of controller `controller` on MIDI channel `channel`, with their ticks.
std::vector<std::pair<uint32_t, int>> ControllerValues(const MidiEvents& midi, uint8_t channel, uint8_t controller)
{
    std::vector<std::pair<uint32_t, int>> found;
    for (const auto& [tick, bytes] : midi.Find(uint8_t(0xB0 | channel)))
    {
        if (bytes[1] == controller)
        {
            found.emplace_back(uint32_t(tick), bytes[2]);
        }
    }

    return found;
}

// Returns the value that a list of ControllerValues() sets at `tick`, or -1 before the first.
int ValueAt(const std::vector<std::pair<uint32_t, int>>& values, uint32_t tick)
{
    int value = -1;
    for (const auto& [at, v] : values)
    {
        if (at <= tick)
        {
            value = v;
        }
    }

    return value;
}

// Returns CC11 for a note at full velocity on both sides of the PSG at volume `volume`, or on one side alone with
// `one_side`. Volume 15 plays at PsgShare() of a full-scale sample (TestPsgSampleLevels()).
int VolumeExpression(double volume, bool one_side)
{
    const double level = volume / PsgShare(Revision::kLinkToThePast);
    int cc10 = 64, cc11 = 0;
    LevelsToControllers(level, one_side ? 0 : level, cc10, cc11);

    return cc11;
}

// Returns the number of zones of the SoundFont's instruments after their global zones. Checks that each of them plays
// at a constant level, with no envelope and no attenuation.
size_t ConstantZones(const Sf2Records& sf)
{
    size_t zones = 0;
    for (size_t i = 0; i + 1 < sf.Count("inst", 22); i++)
    {
        for (size_t z = Le16(sf.Record("inst", 22, i) + 20) + 1; z < Le16(sf.Record("inst", 22, i + 1) + 20); z++)
        {
            const auto gens = sf.ZoneGens(z);
            for (uint16_t op : {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv,
                                sf2::kSustainVolEnv, sf2::kReleaseVolEnv})
            {
                SEQLIB_CHECK(!gens.count(op));
            }
            zones++;
        }
    }

    return zones;
}

void TestPsgPan()
{
    // The PSG plays a note on both sides at the centre pan, 0x40. At any other pan, the driver doubles the note's
    // level and plays it on one side alone. The volume register holds 15 at most. Track 0 plays the square at full
    // velocity at 0x20, 0x40 and 0x50, at volume 15 on each side it plays on. Its CC10 goes to 0, 64 and 127. Each note
    // starts with CC11 for volume 15 on its sides, and lasts until the next takes square 2. The square of CC11 sets the
    // level. A sin/cos player plays each side of a centred note at 1/√2 of the level. The square of CC11 on one side is
    // then 1/√2 of the square at the centre. Track 1's sample and track 2's square drum keep the rule of the sample
    // voices. Pan 0x20 gives them CC10 26. The drum comes after track 0's notes. Otherwise it would take square 2 from
    // them.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x20, 0xC2, kSquare})
                         .Note(0x30, 4, 127)
                         .Wait(8)
                         .Bytes({0xC3, 0x40})
                         .Note(0x30, 4, 127)
                         .Wait(8)
                         .Bytes({0xC3, 0x50})
                         .Note(0x30, 4, 127)
                         .Wait(8)
                         .End(),
                     TrackData().Bytes({0xC3, 0x20, 0xC2, kPlain}).Note(60, 4, 127).Wait(24).End(),
                     TrackData().Bytes({0xC3, 0x20, 0xC2, kDrums}).Wait(30).Note(37, 4, 127).Wait(4).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_pan"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<int> pans;
    for (const auto& value : ControllerValues(midi, 0, cc::kPan))
    {
        pans.push_back(value.second);
    }
    SEQLIB_CHECK((pans == std::vector<int>{0, 64, 127}));
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);
    std::vector<int> starts;
    std::vector<uint32_t> ons, offs;
    for (const auto& note_on : midi.Find(0x90))
    {
        starts.push_back(ValueAt(loudness, uint32_t(note_on.tick)));
        ons.push_back(uint32_t(note_on.tick));
    }
    for (const auto& note_off : midi.Find(0x80))
    {
        offs.push_back(uint32_t(note_off.tick));
    }
    SEQLIB_REQUIRE_EQ(starts.size(), 3);
    SEQLIB_CHECK((starts == std::vector<int>{VolumeExpression(15, true), VolumeExpression(15, false),
                                             VolumeExpression(15, true)}));
    const double ratio = std::pow(double(starts[0]) / starts[1], 2);
    SEQLIB_CHECK(std::fabs(ratio * std::sqrt(2.0) - 1) < 0.025);
    SEQLIB_REQUIRE_EQ(offs.size(), 3);
    SEQLIB_CHECK(offs[0] == ons[1] && offs[1] == ons[2]);
    SEQLIB_CHECK((ControllerValues(midi, 1, cc::kPan) == std::vector<std::pair<uint32_t, int>>{{0, 26}}));
    SEQLIB_CHECK((ControllerValues(midi, 2, cc::kPan) == std::vector<std::pair<uint32_t, int>>{{0, 26}}));
}

void TestPsgVolume()
{
    // A square's or the noise's note plays at full velocity, and its CC11 follows its channel's volume, frame by frame.
    // The square's region holds volume 15 until the release. The release writes the volume to NR22 with a period of 2,
    // and the noise's to NR42 with a period of 1. The PSG's envelope then steps the volume down to 0, a step every
    // 2/64 s or 1/64 s: 1 or 2 frames apart for the square, and 1 frame apart, or now and then 2 steps in a frame, for
    // the noise. Each note ends at the start of the frame whose volume is 0. Their zones play at a constant level, with
    // no envelope and no attenuation.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(40).End(),
                     TrackData().Bytes({0xC2, kNoise}).Note(0x30, 4, 127).Wait(40).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_volume"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    for (uint8_t channel : {uint8_t(0), uint8_t(1)})
    {
        const auto ons = midi.Find(uint8_t(0x90 | channel)), offs = midi.Find(uint8_t(0x80 | channel));
        SEQLIB_REQUIRE(ons.size() == 1 && offs.size() == 1);
        SEQLIB_CHECK_EQ(int(ons[0].bytes[2]), 127);

        // CC11 takes the volumes from 15 down to 0. Each of the square's lasts a frame or two. CC11 therefore takes
        // every one. A frame can take two of the noise's. The release comes 4 frames after the note on. At this tempo,
        // a frame lasts 150 ticks. The last 14 steps take 14 of the envelope's periods, to within a frame.
        const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, channel, cc::kExpression);
        std::vector<int> volumes;
        for (const auto& value : loudness)
        {
            int volume = 15;
            while (volume > 0 && VolumeExpression(volume, false) != value.second)
            {
                volume--;
            }
            volumes.push_back(volume);
        }
        SEQLIB_REQUIRE(volumes.size() >= 2);
        SEQLIB_CHECK(volumes.front() == 15 && volumes.back() == 0 && loudness.back().second == 0);
        for (size_t i = 1; i < volumes.size(); i++)
        {
            const int step = volumes[i - 1] - volumes[i];
            SEQLIB_CHECK(step == 1 || (channel == 1 && step == 2));
        }
        SEQLIB_CHECK_EQ(loudness[0].first, uint32_t(ons[0].tick));
        SEQLIB_CHECK(loudness[1].first >= ons[0].tick + 4 * 150);
        for (size_t i = 2; i < loudness.size(); i++)
        {
            SEQLIB_CHECK(loudness[i].first - loudness[i - 1].first <= 2 * 150);
        }
        const double period = (channel == 0 ? 2 : 1) * kFrameRate / 64;
        SEQLIB_CHECK(std::fabs((loudness.back().first - loudness[1].first) / 150.0 - 14 * period) <= 1);
        SEQLIB_CHECK_EQ(uint32_t(offs[0].tick), loudness.back().first);
    }

    // The square's and the noise's zones.
    SEQLIB_CHECK_EQ(ConstantZones(ReadSf2(sum.sf2_path)), size_t(2));
}

void TestSampleEnvelope()
{
    // A sample note's CC11 follows its voice's envelope frame by frame. The envelope rises to its peak in 4 frames and
    // falls to half of it over the next 8. The zone has only the release. The driver releases the note after 16 frames.
    // The released voice keeps half of the peak's level. E0 then lowers the track's volume from 0x80 to 0x20 at
    // tick 24. CC11 keeps the half and follows the volume: it falls to half of what it was, the square root of a
    // quarter.
    Cart cart;
    cart.Sequences(
        {{TrackData().Bytes({0xC2, kLooped}).Note(60, 16, 127).Wait(24).Bytes({0xE0, 0x20}).Wait(40).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_sample_envelope"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x90), offs = midi.Find(0x80);
    SEQLIB_REQUIRE(ons.size() == 1 && offs.size() == 1);
    SEQLIB_CHECK(offs[0].tick >= 15 * 150 && offs[0].tick <= 17 * 150);

    // CC11 starts at a quarter of the peak's level and reaches the peak at frame 3. It then falls to half the level.
    // CC11 holds the square root of the level.
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);
    const int first = ValueAt(loudness, uint32_t(ons[0].tick));
    const int peak = ValueAt(loudness, uint32_t(ons[0].tick + 3 * 150));
    const int held = ValueAt(loudness, uint32_t(offs[0].tick));
    SEQLIB_CHECK(std::abs(first - int(std::lround(peak * std::sqrt(0.25)))) <= 1);
    SEQLIB_CHECK(std::abs(held - int(std::lround(peak * std::sqrt(0.5)))) <= 1);
    const int after = ValueAt(loudness, 24 * 150);
    SEQLIB_CHECK(std::abs(after - int(std::lround(held * std::sqrt(0.25)))) <= 1);

    // The zones play at a constant level until the release.
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    size_t zones = 0;
    for (size_t z = Le16(sf.Record("inst", 22, 0) + 20) + 1; z < Le16(sf.Record("inst", 22, 1) + 20); z++)
    {
        const auto gens = sf.ZoneGens(z);
        for (uint16_t op :
             {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv, sf2::kSustainVolEnv})
        {
            SEQLIB_CHECK(!gens.count(op));
        }
        SEQLIB_CHECK(gens.count(sf2::kReleaseVolEnv));
        zones++;
    }
    SEQLIB_CHECK(zones > 0);
}

void TestSampleLegato()
{
    // A track in legato plays its notes on one voice. The voice keeps its envelope from note to note. The envelope
    // rises to its peak in 4 frames and falls to half of it over the next 8. Both notes play on a zone whose envelope
    // has only the release. The track's CC11 follows the voice's envelope frame by frame. The second note starts in
    // frame 6, where the envelope falls. The first note ends there. C6 releases the voice in frame 12, and the second
    // note ends there.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kLooped, 0xC5})
                         .Note(60, 6, 127)
                         .Wait(6)
                         .Note(64, 6, 127)
                         .Wait(6)
                         .Bytes({0xC6})
                         .Wait(40)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_legato"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x90), offs = midi.Find(0x80);
    SEQLIB_REQUIRE(ons.size() == 2 && offs.size() == 2);
    SEQLIB_CHECK_EQ(ons[1].tick, 6 * 150);
    SEQLIB_CHECK_EQ(offs[0].tick, ons[1].tick);
    SEQLIB_CHECK_EQ(offs[1].tick, 12 * 150);

    // CC11 starts at a quarter of the peak's, rises to the peak's, and falls from there. The second note doesn't start
    // it again. The release keeps the last value.
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);
    SEQLIB_REQUIRE(loudness.size() >= 4);
    int peak = 0;
    for (const auto& value : loudness)
    {
        peak = std::max(peak, value.second);
    }
    const int first = ValueAt(loudness, uint32_t(ons[0].tick));
    const int second = ValueAt(loudness, uint32_t(ons[1].tick));
    SEQLIB_CHECK(first > 0 && first < peak && second > first && second < peak);
    SEQLIB_CHECK(loudness.back().second > first && loudness.back().second < second);

    // The SoundFont holds the one preset. Its zones play at a constant level until the release.
    const Sf2Records sf = ReadSf2(sum.sf2_path);
    SEQLIB_REQUIRE_EQ(sf.Count("phdr", 38), 2);
    SEQLIB_CHECK(std::string(reinterpret_cast<const char*>(sf.Record("phdr", 38, 0))) == "Bank 0 instrument 0");
    size_t zones = 0;
    for (size_t z = Le16(sf.Record("inst", 22, 0) + 20) + 1; z < Le16(sf.Record("inst", 22, 1) + 20); z++)
    {
        const auto gens = sf.ZoneGens(z);
        for (uint16_t op :
             {sf2::kInitialAttenuation, sf2::kAttackVolEnv, sf2::kHoldVolEnv, sf2::kDecayVolEnv, sf2::kSustainVolEnv})
        {
            SEQLIB_CHECK(!gens.count(op));
        }
        SEQLIB_CHECK(gens.count(sf2::kReleaseVolEnv));
        zones++;
    }
    SEQLIB_CHECK(zones > 0);
}

void TestWaveVolume()
{
    // The wave voice's note plays at full velocity, and its CC11 follows the wave channel's level, frame by frame. At
    // the start of each segment of the voice's envelope, the driver turns the velocity times the volumes into one of
    // the channel's five levels (NR32): 0, 25, 50, 75 or 100%. Velocities 127, 100, 64 and 40 give 100, 75, 50 and
    // 25%. The release comes 6 frames after each note on, partway through the envelope's second segment. Each frame
    // from the release on takes (0x46 + 230) / 512 of the level, and NR32 steps down with it. The driver turns the
    // channel off once the level's step is 0. The note ends at the start of that frame. Its zone plays at a constant
    // level.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kWave})
                         .Note(48, 6, 127)
                         .Wait(12)
                         .Note(48, 6, 100)
                         .Wait(12)
                         .Note(48, 6, 64)
                         .Wait(12)
                         .Note(48, 6, 40)
                         .Wait(12)
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_wave_volume"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto ons = midi.Find(0x90), offs = midi.Find(0x80);
    SEQLIB_REQUIRE(ons.size() == 4 && offs.size() == 4);
    const std::vector<std::pair<uint32_t, int>> loudness = ControllerValues(midi, 0, cc::kExpression);

    // Each note's shares of the full level: the one it holds until its release, then a frame of each of the release's
    // steps, down to 0. At this tempo, a frame lasts 150 ticks.
    const std::vector<std::vector<double>> kShares = {
        {1, 0.5, 0.25, 0}, {0.75, 0.5, 0.25, 0}, {0.5, 0.25, 0}, {0.25, 0}};
    for (size_t i = 0; i < ons.size(); i++)
    {
        SEQLIB_CHECK_EQ(int(ons[i].bytes[2]), 127);
        std::vector<int> values, expected;
        for (const auto& [tick, value] : loudness)
        {
            if (tick >= ons[i].tick && tick <= offs[i].tick)
            {
                values.push_back(value);
            }
        }
        for (double share : kShares[i])
        {
            expected.push_back(VolumeExpression(15 * share, false));
        }
        SEQLIB_CHECK(values == expected);
        SEQLIB_CHECK_EQ(offs[i].tick - ons[i].tick, uint64_t(150 * (6 + kShares[i].size() - 2)));
    }
    SEQLIB_CHECK_EQ(ConstantZones(ReadSf2(sum.sf2_path)), size_t(1));
}

void TestPsgRunOut()
{
    // A sequence without a loop ends with its last track, but the PSG can still sound then. The square's track ends 2
    // frames after the square's release. The release's envelope steps the volume down from 15 to 0, a step every
    // 2/64 s (TestPsgVolume()), and runs on for about 26 frames after the end. The wave's track ends a frame after the
    // wave's release, with a frame of the release's fade left: 50% in the release's frame, then 25% (TestWaveVolume()).
    // The model runs on until both channels fall silent. Each note's CC11 follows its channel's volume down to 0, and
    // the MIDI file ends where the square falls silent.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kWave}).Note(48, 6, 127).Wait(7).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_run_out"), nullptr);

    SEQLIB_CHECK(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    std::vector<uint32_t> ends;
    for (const auto& [tick, bytes] : midi.Find(0xFF))
    {
        if (bytes[1] == 0x2F)
        {
            ends.push_back(uint32_t(tick));
        }
    }
    SEQLIB_REQUIRE(!ends.empty());
    const uint32_t end = *std::max_element(ends.begin(), ends.end());

    // The square's CC11 takes every volume from 15 down to 1. Volume 1 lasts a frame or two. The note then ends at the
    // start of the frame whose volume is 0. The file ends there too. At this tempo, a frame lasts 150 ticks.
    const auto square_ons = midi.Find(0x90), square_offs = midi.Find(0x80);
    SEQLIB_REQUIRE(square_ons.size() == 1 && square_offs.size() == 1);
    const std::vector<std::pair<uint32_t, int>> square = ControllerValues(midi, 0, cc::kExpression);
    std::vector<int> values, expected;
    for (const auto& [tick, value] : square)
    {
        values.push_back(value);
    }
    for (int volume = 15; volume >= 1; volume--)
    {
        expected.push_back(VolumeExpression(volume, false));
    }
    SEQLIB_CHECK(values == expected);
    SEQLIB_REQUIRE(!square.empty());
    const uint32_t last_step = uint32_t(square_offs[0].tick) - square.back().first;
    SEQLIB_CHECK(last_step == 150 || last_step == 300);
    SEQLIB_CHECK(square_offs[0].tick > 6 * 150 + 20 * 150);
    SEQLIB_CHECK_EQ(uint32_t(square_offs[0].tick), end);

    // The wave's CC11 takes the release's two steps. The wave's track ends in frame 7. The note ends 8 frames after
    // the note on.
    const auto wave_ons = midi.Find(0x91), wave_offs = midi.Find(0x81);
    SEQLIB_REQUIRE(wave_ons.size() == 1 && wave_offs.size() == 1);
    values.clear();
    for (const auto& [tick, value] : ControllerValues(midi, 1, cc::kExpression))
    {
        values.push_back(value);
    }
    expected.clear();
    for (double share : {1.0, 0.5, 0.25, 0.0})
    {
        expected.push_back(VolumeExpression(15 * share, false));
    }
    SEQLIB_CHECK(values == expected);
    SEQLIB_CHECK_EQ(wave_offs[0].tick - wave_ons[0].tick, uint64_t(8 * 150));
}

void TestSilentEndAfterLastSound()
{
    // A square's note of 4 ticks at a tick a frame, a wait, a volume change and the end. The release's envelope runs
    // on for about 26 frames (TestPsgRunOut()). After a wait of 600 ticks, the change comes about 9.5 s after the
    // square falls silent, and the sequence ends at the change. After a wait of 720 ticks, it comes about 11.5 s after,
    // more than kMaxSilentEndSeconds. The sequence then ends two frames after the last frame that sounds, and its MIDI
    // file holds nothing later.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(600).Bytes({0xE0, 0x40}).End()},
                    {TrackData().Bytes({0xC2, kSquare}).Note(0x30, 4, 127).Wait(720).Bytes({0xE0, 0x40}).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport near = ConvertSong(rom, info, 0, TempSettings("rd2_silent_end_near"), nullptr);
    const SongReport far = ConvertSong(rom, info, 1, TempSettings("rd2_silent_end_far"), nullptr);

    SEQLIB_CHECK(near.ok && std::fabs(near.seconds - 600 / kFrameRate) < 0.02);
    SEQLIB_CHECK(far.ok && far.seconds > 20 / kFrameRate && far.seconds < 40 / kFrameRate);
    const MidiEvents midi = ReadMidi(far.midi_path);
    uint64_t last = 0;
    for (const auto& track : midi.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }
    SEQLIB_CHECK(midi.Find(0x90).size() == 1 && last < 40 * 150);
}

void TestPsgPanPasses()
{
    // The sequence loops back to its start. The first pass plays the square at 0x20 and then at 0x10. The second pass
    // keeps 0x10. The PSG plays both passes on the left alone at the same level. The passes then repeat each other. The
    // MIDI file's loop therefore starts with the first pass. A sample voice would play the first note of each pass at
    // another level.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x20, 0xC2, kSquare})
                         .Note(0x30, 6, 127)
                         .Wait(6)
                         .Bytes({0xC3, 0x10})
                         .Note(0x30, 6, 127)
                         .Wait(6)
                         .Bytes({0xF0, 8, 0})
                         .End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_psg_pan_passes"), nullptr);

    SEQLIB_CHECK(sum.ok);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& [tick, bytes] : ReadMidi(sum.midi_path).Find(0xFF))
    {
        if (bytes[1] == 0x06)
        {
            markers.emplace_back(uint32_t(tick), std::string(bytes.begin() + 3, bytes.end()));
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{0, "loopStart"}, {1800, "loopEnd"}};
    SEQLIB_CHECK(markers == kMarkers);
}

void TestLevelOrder()
{
    // At a tempo of 300, a frame lasts two ticks. Track 0 sets its pan at tick 4, and then, in the same frame, track 1
    // sets the player's volume to 0 at tick 3. Track 0 plays with both from that frame on. So the MIDI file gives them
    // both at tick 4, and track 0's last CC11 silences it.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xE4, 0x81, 0x2C, 0xC2, kPlain})
                         .Note(60, 20, 127)
                         .Wait(4)
                         .Bytes({0xC3, 0x10})
                         .Wait(16)
                         .End(),
                     TrackData().Wait(3).Bytes({0xEA, 0}).Wait(17).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("rd2_level_order");

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(sum.ok);
    std::vector<std::pair<uint32_t, int>> expression;
    for (const auto& e : ReadMidi(sum.midi_path).Find(0xB0))
    {
        if (e.bytes[1] == 11)
        {
            expression.emplace_back(uint32_t(e.tick), e.bytes[2]);
        }
    }
    SEQLIB_CHECK(!expression.empty() && expression.back() == std::make_pair(600u, 0));
}

void TestHighVelocities()
{
    // The driver plays a sample note's level in proportion to its velocity, up to 255. Track 0's note at 255 plays at
    // velocity 127. Its note at 127 plays at 127 × √(127 / 255). The track's CC11 raises its levels by 255 / 127 to
    // make up for it. CC11 sets the level by its square. Track 1 plays at velocity 127 and at its levels. Track 2's
    // volume of 0xFF leaves CC11 too little room. Its CC11 reaches 127. Its note at 100 then plays between its velocity
    // at no scale and at the full one.
    Cart cart;
    cart.Sequences(
        {{TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 255).Wait(6).Note(62, 6, 127).Wait(6).End(),
          TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(12).End(),
          TrackData().Bytes({0xE0, 0xFF, 0xC2, kPlain}).Note(67, 6, 255).Wait(6).Note(69, 6, 100).Wait(6).End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport sum = ConvertSong(rom, info, 0, TempSettings("rd2_high_velocities"), nullptr);

    SEQLIB_REQUIRE(sum.ok);
    const MidiEvents midi = ReadMidi(sum.midi_path);
    const auto velocities = [&midi](uint8_t channel)
    {
        std::vector<int> found;
        for (const auto& e : midi.Find(uint8_t(0x90 | channel)))
        {
            if (e.bytes[2] > 0)
            {
                found.push_back(e.bytes[2]);
            }
        }
        return found;
    };
    SEQLIB_CHECK(velocities(0) == std::vector<int>({127, int(std::lround(127 * std::sqrt(127 / 255.0)))}));
    SEQLIB_CHECK(velocities(1) == std::vector<int>({127}));
    const auto scaled = ControllerValues(midi, 0, cc::kExpression), plain = ControllerValues(midi, 1, cc::kExpression);
    const auto capped = ControllerValues(midi, 2, cc::kExpression);
    SEQLIB_REQUIRE(!scaled.empty() && !plain.empty() && !capped.empty());
    SEQLIB_CHECK(std::abs(scaled[0].second - plain[0].second * std::sqrt(255 / 127.0)) <= 1);
    SEQLIB_CHECK_EQ(capped[0].second, 127);
    const std::vector<int> loud = velocities(2);
    SEQLIB_REQUIRE_EQ(loud.size(), 2);
    SEQLIB_CHECK_EQ(loud[0], 127);
    SEQLIB_CHECK(loud[1] > std::lround(127 * std::sqrt(100 / 255.0)) &&
                 loud[1] < std::lround(127 * std::sqrt(100 / 127.0)));
}

} // namespace

int RunTests()
{
    TestPsgTables();
    TestPsgTuning();
    TestPsgSampleLevels();
    TestDrumPanAndWave();
    TestPsgPan();
    TestPsgVolume();
    TestSampleEnvelope();
    TestSampleLegato();
    TestWaveVolume();
    TestPsgRunOut();
    TestSilentEndAfterLastSound();
    TestPsgPanPasses();
    TestLevelOrder();
    TestHighVelocities();

    return seqlib::testing::Summary("rd2_levels");
}

} // namespace ntdlib::sequence_export::rd2

int main()
{
    return ntdlib::sequence_export::rd2::RunTests();
}
