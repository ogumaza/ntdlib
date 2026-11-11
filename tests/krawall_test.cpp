// SPDX-License-Identifier: MIT

// Unit tests for Krawall. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/krawall/archive_player.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/instruments.h"
#include "ntdlib/krawall/player.h"
#include "ntdlib/krawall/song.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/krawall/convert.h"
#include "ntdlib/sequence_export/krawall/performance.h"
#include "ntdlib/sequence_export/krawall/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/krawall_cart.h"

namespace ntdlib::sequence_export::krawall
{
namespace
{

using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::krawall::Build;
using ntdlib::krawall::DetectDriver;
using ntdlib::krawall::DriverInfo;
using ntdlib::krawall::DriverOverrides;
using ntdlib::krawall::DumpSong;
using ntdlib::krawall::kModeJingle;
using ntdlib::krawall::kModeLoop;
using ntdlib::krawall::kModeSong;
using ntdlib::krawall::kSampleEnd;
using ntdlib::krawall::kSampleLoop;
using ntdlib::krawall::kSampleLoopLength;
using ntdlib::krawall::kSamplePoints;
using ntdlib::krawall::MixChannel;
using ntdlib::krawall::ModuleSong;
using ntdlib::krawall::Player;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::LevelsToControllers;
using ntdlib::sequence_export::agb::Sf2File;
using ntdlib::sequence_export::agb::SongReport;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::krawall_cart::Cart;
using ntdlib::testing::krawall_cart::Cell;
using ntdlib::testing::krawall_cart::kBidiLoop;
using ntdlib::testing::krawall_cart::kBidiPoints;
using ntdlib::testing::krawall_cart::kBreak;
using ntdlib::testing::krawall_cart::kC4Rate;
using ntdlib::testing::krawall_cart::kForwardLoop;
using ntdlib::testing::krawall_cart::kForwardPoints;
using ntdlib::testing::krawall_cart::kGameInit;
using ntdlib::testing::krawall_cart::kInit;
using ntdlib::testing::krawall_cart::kInstrumentTable;
using ntdlib::testing::krawall_cart::kJump;
using ntdlib::testing::krawall_cart::kLinearTable;
using ntdlib::testing::krawall_cart::kMaster;
using ntdlib::testing::krawall_cart::kModuleTable;
using ntdlib::testing::krawall_cart::kOneShotPoints;
using ntdlib::testing::krawall_cart::kPan;
using ntdlib::testing::krawall_cart::kPeriodTable;
using ntdlib::testing::krawall_cart::kPlay;
using ntdlib::testing::krawall_cart::kRampTable;
using ntdlib::testing::krawall_cart::kSampleTable;
using ntdlib::testing::krawall_cart::kSetSpeed;
using ntdlib::testing::krawall_cart::kSineTable;
using ntdlib::testing::krawall_cart::kSpeed;
using ntdlib::testing::krawall_cart::kSquareTable;
using ntdlib::testing::krawall_cart::kTickSamples;
using ntdlib::testing::krawall_cart::kVersion;
using ntdlib::testing::krawall_cart::LoopingCells;
namespace cc = seqlib::midi::cc;
namespace sf2 = seqlib::sf2;

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, DriverOverrides(), info, error);

    return info;
}

// Detection finds the player by its code, its tables from the code and the values in them, the mixer's settings, the
// game's calls of the library's settings and the modules, in the order of the game's table and then the one it doesn't
// list. A count for the modules applies to the game's table, and leaves out the one it doesn't list. A game with the
// version string of another build gets an error that names the build.
void TestDetection()
{
    Cart cart;
    const uint32_t first = cart.Module(2, 5, LoopingCells());
    const uint32_t second = cart.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    const uint32_t unlisted = cart.Module(1, 4, {{0, 0, 37, 1, -1, -1, 0}}, false);
    const Rom rom = cart.ToRom();
    Cart other(false);
    other.PutString(kVersion, "$Id: Krawall $Id: version.h 8 2005-04-21 12:24:45Z seb $");
    const Rom other_rom = other.ToRom();
    const Rom blank = Cart(false).ToRom();
    DriverInfo info, counted_info, other_info, blank_info;
    std::string error, counted_error, other_error, blank_error;
    DriverOverrides one;
    one.module_count = 1;

    const bool found = DetectDriver(rom, DriverOverrides(), info, error);
    const bool counted = DetectDriver(rom, one, counted_info, counted_error);
    const bool found_other = DetectDriver(other_rom, DriverOverrides(), other_info, other_error);
    const bool found_blank = DetectDriver(blank, DriverOverrides(), blank_info, blank_error);

    SEQLIB_CHECK(found && error.empty());
    SEQLIB_CHECK_EQ(info.play, kPlay);
    SEQLIB_CHECK_EQ(info.samples, kSampleTable);
    SEQLIB_CHECK_EQ(info.instruments, kInstrumentTable);
    SEQLIB_CHECK_EQ(info.sine, kSineTable);
    SEQLIB_CHECK_EQ(info.periods, kPeriodTable);
    SEQLIB_CHECK_EQ(info.linear, kLinearTable);
    SEQLIB_CHECK_EQ(info.mix_rate, 16384);
    SEQLIB_CHECK_EQ(info.frame_samples, 276);
    SEQLIB_CHECK(info.stereo);
    SEQLIB_CHECK_EQ(info.quality, 0);
    SEQLIB_CHECK_EQ(info.master_volume, 0x40080);
    SEQLIB_CHECK_EQ(info.module_table, kModuleTable);
    SEQLIB_CHECK_EQ(info.table_count, 2);
    SEQLIB_CHECK(info.modules == std::vector<uint32_t>({first, second, unlisted}));
    SEQLIB_CHECK(counted && counted_info.modules == std::vector<uint32_t>({first}));
    SEQLIB_CHECK(!found_other && other_error ==
                                     "found Krawall, but not a build that ntdlib supports ($Id: Krawall $Id: "
                                     "version.h 8 2005-04-21 12:24:45Z seb $)");
    SEQLIB_CHECK(!found_blank && blank_error.empty());

    // Franklin's Great Adventures holds the ramp's first values in other data before the player's tables. The ramp
    // follows the sine. BFKE's ARM start-up code sets kragInit()'s argument after it loads the routine's address:
    // ldr r3, =kragInit|1; mov r0, #0; mov lr, pc; bx r3.
    Cart franklin;
    franklin.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    for (uint32_t i = 0; i < 16; i++)
    {
        franklin.Put16(kRomBase + 0x800 + 2 * i, uint16_t(8 * i));
    }
    for (uint32_t i = 0; i < 6; i += 2)
    {
        franklin.Put16(kGameInit + i, 0);
    }
    franklin.Put32(kRomBase + 0x900, 0xE59F3008);
    franklin.Put32(kRomBase + 0x904, 0xE3A00000);
    franklin.Put32(kRomBase + 0x908, 0xE1A0E00F);
    franklin.Put32(kRomBase + 0x90C, 0xE12FFF13);
    franklin.Put32(kRomBase + 0x910, kInit | 1);

    const DriverInfo franklin_info = Detect(franklin.ToRom());

    SEQLIB_CHECK_EQ(franklin_info.ramp, kRampTable);
    SEQLIB_CHECK_EQ(franklin_info.square, kSquareTable);
    SEQLIB_CHECK(!franklin_info.stereo);
    SEQLIB_CHECK(franklin_info.warnings.empty());

    // Harry Potter and the Prisoner of Azkaban gives the music 12 times its options' default level of 10.
    Cart potter;
    potter.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    potter.PutString(kRomBase + 0xAC, "BHTE");

    SEQLIB_CHECK_EQ(info.music_volume, 128);
    SEQLIB_CHECK_EQ(Detect(potter.ToRom()).music_volume, 120);

    // Shrek SuperSlam's pause calls kramSetMasterVol(0x40020), before its sound init in the ROM. The master volume is
    // the sound init's, from the call that follows the game's call of kragInit().
    Cart shrek;
    shrek.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}});
    shrek.Put16(kRomBase + 0xE0, 0x4803);
    shrek.PutBl(kRomBase + 0xE2, kMaster);
    shrek.Put32(kRomBase + 0xF0, 0x40020);

    SEQLIB_CHECK_EQ(Detect(shrek.ToRom()).master_volume, 0x40080);
}

// The player starts a note at the rate its period gives and the volume its sample or the volume column gives, a volume
// slide changes the mixer's volume on the ticks between rows, an offset starts a note within its sample, a pan sets the
// sides' volumes, and a portamento up lowers the period on the ticks between rows.
void TestPlayer()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    std::vector<MixChannel> first, second;
    std::vector<uint64_t> times;
    std::vector<int> periods;
    const auto record = [&](uint64_t time)
    {
        times.push_back(time);
        first.push_back(player.MixerOf(player.Channels()[0]));
        second.push_back(player.MixerOf(player.Channels()[1]));
        periods.push_back(player.Channels()[0].period);
    };
    player.OnTick(record);

    while (times.size() < 13)
    {
        player.Frame();
    }

    // The first tick comes a tick's samples after the start, and the rest a tick apart.
    SEQLIB_CHECK(player.Valid());
    SEQLIB_CHECK_EQ(times[0], kTickSamples);
    SEQLIB_CHECK_EQ(times[12] - times[0], 12 * kTickSamples);

    // Row 0: C-4 at volume 64, and E-4 at its sample's volume, 48, with a pan of -32: the left side gets three times
    // the right's level.
    SEQLIB_CHECK_EQ(first[0].inc, int32_t(kC4Rate << 2));
    SEQLIB_CHECK_EQ(first[0].volume, 64);
    SEQLIB_CHECK_EQ(second[0].volume, 48);
    SEQLIB_CHECK_EQ(second[0].pan, -32);
    SEQLIB_CHECK_EQ(second[0].left, (48 * 64 * 96) >> 13);
    SEQLIB_CHECK_EQ(second[0].right, (48 * 64 * 32) >> 13);

    // Row 1 slides the volume down by 4 on its second and third ticks.
    SEQLIB_CHECK_EQ(first[3].volume, 64);
    SEQLIB_CHECK_EQ(first[4].volume, 60);
    SEQLIB_CHECK_EQ(first[5].volume, 56);

    // Row 2 starts E-4 again 256 points into its sample.
    SEQLIB_CHECK(second[6].handle != second[5].handle);
    SEQLIB_CHECK_EQ(second[6].pos - second[6].start, 256);

    // Row 3 lowers the period by 64 on each of its ticks between rows, and the rate goes up with it.
    SEQLIB_CHECK_EQ(periods[9], 1712);
    SEQLIB_CHECK_EQ(periods[10], 1712 - 64);
    SEQLIB_CHECK_EQ(periods[11], 1712 - 128);
    SEQLIB_CHECK_EQ(first[11].inc, int32_t((14317456 / (1712 - 128)) << 2));
}

// The mixer plays a sample to its end, or to its loop's end and back to the loop's start, or with a loop back and
// forth, back to the loop's start and forwards again, and a silent channel moves on by its guess, which for a step of
// less than a point is a point a sample.
void TestMixer()
{
    Cart cart;
    cart.Module(4, 4,
                {{0, 0, 49, 1, -1, -1, 0}, {0, 1, 85, 2, -1, -1, 0}, {0, 2, 49, 3, -1, -1, 0}, {0, 3, 49, 2, 0x10}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    bool forward_in_loop = true, bidi_turned = false;
    uint32_t one_shot_status = 1;
    std::vector<uint32_t> silent_points;
    const auto check = [&](uint64_t)
    {
        const MixChannel& f = player.MixerOf(player.Channels()[0]);
        const MixChannel& b = player.MixerOf(player.Channels()[2]);
        const MixChannel& s = player.MixerOf(player.Channels()[3]);
        forward_in_loop = forward_in_loop && f.status == 1 && f.pos < f.end && f.pos >= f.start;
        bidi_turned = bidi_turned || b.inc < 0;
        one_shot_status = player.MixerOf(player.Channels()[1]).status;
        if (s.status == 1)
        {
            silent_points.push_back(s.pos - s.start);
        }
    };
    player.OnTick(check);

    for (int f = 0; f < 40; f++)
    {
        player.Frame();
    }

    SEQLIB_CHECK(forward_in_loop);
    SEQLIB_CHECK(bidi_turned);
    SEQLIB_CHECK_EQ(one_shot_status, 0);
    SEQLIB_CHECK(silent_points.size() >= 2 && silent_points[1] - silent_points[0] == kTickSamples);
}

// In the enhanced mode, a sample that loops back and forth plays its attack: the points before the loop sound as they
// are, and the loop turns at its start only once it plays backwards.
void TestEnhancedAttack()
{
    Cart cart;
    cart.Module(4, 4, {{0, 0, 49, 3, -1, -1, 0}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Player player(rom, info, info.modules[0]);
    player.MixEnhanced(true);
    std::vector<float> mix;
    for (int f = 0; f < 4; f++)
    {
        player.Frame();
        const std::vector<float>& left = player.EnhancedOutput(0);
        const std::vector<float>& right = player.EnhancedOutput(1);
        for (size_t i = 0; i < left.size(); i++)
        {
            mix.push_back(left[i] + (i < right.size() ? right[i] : 0.0f));
        }
    }

    // The sample's first points rise from the middle, as a sine does. The loop's last point is below the middle.
    const auto first = std::find_if(mix.begin(), mix.end(), [](float s) { return s != 0; });
    SEQLIB_CHECK(mix.end() - first >= 20);
    SEQLIB_CHECK(std::all_of(first, first + 20, [](float s) { return s > 0; }));
}

// A module holds two songs separated by +++: the first plays pattern 0, with a note on C-4, and the second pattern 1,
// with a note an octave up. Each converts on its own in the player's song mode. At its end it goes back to its start,
// and it never plays the other song.
void TestSongs()
{
    Cart cart;
    const uint32_t module = cart.Module(1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 61, 1}}}});
    cart.Songs(module, {{0}, {1}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_songs";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{module, 0}, {module, 1}}));
    SEQLIB_CHECK(first.ok && second.ok && !first.silent && !second.silent);
    std::vector<std::set<int>> keys;
    for (const SongReport* s : {&first, &second})
    {
        std::set<int> played;
        for (const auto& e : ReadMidi(s->midi_path).Find(0x90))
        {
            played.insert(e.bytes[1]);
        }
        keys.push_back(played);
    }
    SEQLIB_CHECK(keys[0].size() == 1 && keys[1].size() == 1);
    SEQLIB_CHECK(!keys[0].empty() && !keys[1].empty() && *keys[0].begin() + 12 == *keys[1].begin());
}

// Eight rows per quarter note give a tempo of 126 BPM, within the target range of 105 to 210 BPM. Each player tick is
// one MIDI tick, and the tempo comes from the sample count for eight rows. Each channel's notes are on its MIDI
// channel, on the keys their rates give, with a program for each sample and start offset. The five-row loop, from the
// first row back to it, plays twice; the volume slide becomes CC11 and the portamento bends upwards. SoundFont presets
// preserve sample offsets and C-4 rates, and bidirectional loops contain both the forward and reverse samples.
void TestConversion()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    cart.Module(1, 4, {{0, 0, 49, 3, -1, -1, 0}, {3, 0, -1, 0, -1, kJump, 0x00}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport bidi = ConvertSong(rom, info, 1, opt, nullptr);
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(sum.midi_path);
    const ntdlib::testing::agb_files::Sf2Records sf2 = ReadSf2(sum.sf2_path);
    const ntdlib::testing::agb_files::Sf2Records bidi_sf2 = ReadSf2(bidi.sf2_path);

    // The plan: a loop of 15 ticks from the start, played twice.
    constexpr int kBeatTicks = 8 * kSpeed;
    const double kTickSeconds = double(kTickSamples) / 16384;
    SEQLIB_CHECK(sum.ok && bidi.ok);
    SEQLIB_CHECK_EQ(sum.tracks, 2);
    SEQLIB_CHECK(std::fabs(sum.loop_start) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 15 * kTickSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 30 * kTickSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.bpm - 60.0 / (kBeatTicks * kTickSeconds)) < 1e-3);
    SEQLIB_CHECK_EQ(midi.division, kBeatTicks);
    const auto metas = midi.Find(0xFF);
    bool tempo = false, loop_start = false, loop_end = false;
    for (const auto& [tick, bytes] : metas)
    {
        tempo = tempo ||
                (bytes[1] == 0x51 && tick == 0 &&
                 ((bytes[3] << 16) | (bytes[4] << 8) | bytes[5]) == int(std::lround(kBeatTicks * kTickSeconds * 1e6)));
        loop_start = loop_start || (bytes[1] == 0x06 && tick == 0);
        loop_end = loop_end || (bytes[1] == 0x06 && tick == 15);
    }
    SEQLIB_CHECK(tempo && loop_start && loop_end);

    // The notes: C-4 on key 60 at the start of each pass, and E-4 on key 64, again 2 rows on from its point.
    const auto first_on = midi.Find(0x90);
    const auto second_on = midi.Find(0x91);
    SEQLIB_REQUIRE_EQ(first_on.size(), 2u);
    SEQLIB_CHECK(first_on[0].tick == 0 && first_on[0].bytes[1] == 60 && first_on[1].tick == 15);
    SEQLIB_REQUIRE_EQ(second_on.size(), 4u);
    SEQLIB_CHECK(second_on[0].bytes[1] == 64 && second_on[1].tick == 6);
    const auto programs = midi.Find(0xC1);
    SEQLIB_CHECK(programs.size() >= 2 && programs[0].bytes[1] != programs[1].bytes[1]);

    // The volume slide: CC11 for volumes 64, 60 and 56 on its channel.
    std::vector<int> expression;
    for (const auto& [tick, bytes] : midi.Find(0xB0))
    {
        if (bytes[1] == cc::kExpression && tick > 0 && tick < 6)
        {
            expression.push_back(bytes[2]);
        }
    }
    int cc10 = 0, cc11_at_60 = 0, cc11_at_56 = 0;
    LevelsToControllers((60 * 64 * 64) >> 13, (60 * 64 * 64) >> 13, cc10, cc11_at_60);
    LevelsToControllers((56 * 64 * 64) >> 13, (56 * 64 * 64) >> 13, cc10, cc11_at_56);
    SEQLIB_CHECK(expression == std::vector<int>({cc11_at_60, cc11_at_56}));

    // The portamento: a bend up on the first channel in row 3.
    bool bent_up = false;
    for (const auto& [tick, bytes] : midi.Find(0xE0))
    {
        bent_up = bent_up || (tick >= 9 && tick < 12 && (bytes[1] | (bytes[2] << 7)) > 8192);
    }
    SEQLIB_CHECK(bent_up);

    // The SoundFont: a preset for each sample and point, and the note from point 256 starting there.
    SEQLIB_CHECK_EQ(sf2.Count("phdr", 38), 4u);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf2));
    bool offset_zone = false;
    for (size_t z = 0; z + 1 < sf2.Count("ibag", 4); z++)
    {
        const auto gens = sf2.ZoneGens(z);
        offset_zone = offset_zone || (gens.count(sf2::kStartAddrsOffset) && gens.at(sf2::kStartAddrsOffset) == 256);
    }
    SEQLIB_CHECK(offset_zone);
    SEQLIB_CHECK_EQ(Le32(sf2.Record("shdr", 46, 0) + 36), kC4Rate);
    SEQLIB_CHECK_EQ(Le32(sf2.Record("shdr", 46, 0) + 28) - Le32(sf2.Record("shdr", 46, 0) + 20),
                    kForwardPoints - kForwardLoop);

    // The sample that loops back and forth plays its loop forwards and then backwards.
    const uint8_t* s = bidi_sf2.Record("shdr", 46, 0);
    SEQLIB_CHECK_EQ(Le32(s + 32) - Le32(s + 28), 2 * kBidiLoop);
    SEQLIB_CHECK_EQ(Le32(s + 32) - Le32(s + 20), kBidiPoints + kBidiLoop);
}

// Returns a MIDI file's time signatures, as the row each starts on, its numerator and its denominator's power of 2.
std::vector<std::vector<uint32_t>> TimeSignatures(const std::string& path)
{
    std::vector<std::vector<uint32_t>> signatures;
    for (const auto& [tick, bytes] : ReadMidi(path).Find(0xFF))
    {
        if (bytes[1] == 0x58)
        {
            signatures.push_back({uint32_t(tick / uint32_t(kSpeed)), bytes[3], bytes[4]});
        }
    }

    return signatures;
}

// Bars follow pattern boundaries, with a short bar when an order ends partway through one. At eight rows per quarter
// note, a 4/4 bar has 32 rows. A break ends the first order after 33 rows, so each pass starts with a one-row pickup.
// The 48-row patterns use 12/8: two bars per pattern, at two rows per eighth note.
void TestBars()
{
    Cart cart;
    cart.Module(1, {{64, {{1, 0, 49, 1}, {32, 0, -1, 0, -1, kBreak, 0x00}}},
                    {64, {{0, 0, 49, 1}, {32, 0, 49, 1}}},
                    {40, {{0, 0, 49, 1}, {32, 0, 49, 1}}}});
    cart.Module(1, 64, {{0, 0, 49, 1}});
    cart.Module(1, {{48, {{0, 0, 49, 1}}}, {48, {{0, 0, 49, 1}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_bars";

    const SongReport cut = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport full = ConvertSong(rom, info, 1, opt, nullptr);
    const SongReport compound = ConvertSong(rom, info, 2, opt, nullptr);

    // Each pass starts with a one-row pickup, followed by full bars and a one-quarter-note bar at the end of the third
    // pattern. The unbroken 64-row pattern stays in 4/4. The 48-row patterns use 12/8, with four rows per quarter note.
    const std::vector<std::vector<uint32_t>> kPasses = {{0, 1, 5},   {1, 4, 2},   {129, 1, 2},
                                                        {137, 1, 5}, {138, 4, 2}, {266, 1, 2}};
    SEQLIB_CHECK(cut.ok && full.ok && compound.ok);
    SEQLIB_CHECK(TimeSignatures(cut.midi_path) == kPasses);
    SEQLIB_CHECK(TimeSignatures(full.midi_path) == std::vector<std::vector<uint32_t>>({{0, 4, 2}}));
    SEQLIB_CHECK(TimeSignatures(compound.midi_path) == std::vector<std::vector<uint32_t>>({{0, 12, 3}}));
    SEQLIB_CHECK_EQ(ReadMidi(compound.midi_path).division, 4 * kSpeed);
}

// The intro's row pans the channel to the left, and the loop plays a note on its first row and then pans the channel to
// the right, which it keeps when the module jumps back. The first pass's note plays on the left and the later passes'
// on the right, so the marked loop starts at the second pass, and the module plays the first pass once before it.
void TestLoopStartsAtSecondPass()
{
    Cart cart;
    cart.Module(1, {{1, {{0, 0, -1, 0, -1, kPan, 0x00}}},
                    {4, {{0, 0, 49, 1, -1, -1, 0}, {2, 0, -1, 0, -1, kPan, 0x80}, {3, 0, -1, 0, -1, kJump, 0x01}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_second";

    const SongReport sum = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop runs from the second pass, after the intro's row and the first pass's four, for four rows. The file
    // plays the note in the first pass and in the loop's two passes.
    const double kRowSeconds = kSpeed * double(kTickSamples) / 16384;
    SEQLIB_CHECK(sum.ok);
    SEQLIB_CHECK(std::fabs(sum.loop_start - 5 * kRowSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.loop_end - 9 * kRowSeconds) < 1e-9);
    SEQLIB_CHECK(std::fabs(sum.seconds - 13 * kRowSeconds) < 1e-9);
    SEQLIB_REQUIRE_EQ(ReadMidi(sum.midi_path).Find(0x90).size(), 3u);
}

// A module can have up to 32 channels, and --tracks can name each of them. In a module with more than 15, channels
// share MIDI channels, and channels whose notes sound at once get MIDI channels of their own.
void TestManyChannels()
{
    Cart cart;
    cart.Module(18, 4, {{0, 0, 49, 1}, {0, 17, 53, 2}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_channels";
    ConvertSettings first_only = opt;
    first_only.track_mask = 1;
    ConvertSettings last_only = opt;
    last_only.track_mask = 1u << 17;
    ConvertSettings first_sixteen = opt;
    first_sixteen.track_mask = 0xFFFF;

    const SongReport every = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport chosen = InspectSong(rom, info, 0, first_only);
    const SongReport last = InspectSong(rom, info, 0, last_only);
    const SongReport sixteen = InspectSong(rom, info, 0, first_sixteen);

    SEQLIB_CHECK(every.ok && chosen.ok && last.ok && sixteen.ok);
    SEQLIB_CHECK_EQ(every.tracks, 2);
    SEQLIB_CHECK_EQ(chosen.tracks, 1);
    SEQLIB_CHECK_EQ(last.tracks, 1);
    SEQLIB_CHECK_EQ(sixteen.tracks, 1);

    // The 18th channel's E-4 sounds at once with the first channel's note. The 18th channel therefore takes a MIDI
    // channel of its own: channel 16, after those of the first 14 channels. Its note is on key 64 in each of the
    // loop's two passes.
    const auto notes = ReadMidi(every.midi_path).Find(0x9F);
    SEQLIB_CHECK(notes.size() == 2 && notes[0].bytes[1] == 64 && notes[1].bytes[1] == 64);
    SEQLIB_CHECK_EQ(ReadMidi(every.midi_path).Find(0x90).size(), 2u);
}

// Two channels whose notes never sound at once share a MIDI channel. A MIDI channel's program and controllers act on
// all its notes. Each note of a shared MIDI channel therefore sets them again, and the tracks don't set them at the
// start.
void TestSharedMidiChannel()
{
    // 14 channels hold looping notes. Channels 15 and 16 play the sample that doesn't loop, an octave up, at rows 0 and
    // 3. Each note ends before the other starts.
    std::vector<Cell> cells;
    for (int c = 0; c < 14; c++)
    {
        cells.push_back({0, c, 49, 1});
    }
    cells.push_back({0, 14, 61, 2});
    cells.push_back({3, 15, 61, 2, -1, kPan, 0x20});
    Cart cart;
    cart.Module(16, 6, cells);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_shared";

    const SongReport report = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(report.ok);
    SEQLIB_CHECK(report.warnings.empty());
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
    SEQLIB_CHECK_EQ(midi.Find(0x9F).size(), 4u);
    for (const char* name : {"Channel 15", "Channel 16"})
    {
        const size_t t = size_t(std::find(names.begin(), names.end(), name) - names.begin());
        SEQLIB_REQUIRE(t < midi.tracks.size());
        uint64_t note = 0;
        uint64_t program = 0;
        uint64_t expression = 0;
        int programs = 0;
        for (const seqlib::midi::Event& e : midi.tracks[t].events)
        {
            note = (e.bytes[0] == 0x9F && !note) ? e.tick : note;
            programs += e.bytes[0] == 0xCF ? 1 : 0;
            program = (e.bytes[0] == 0xCF && !program) ? e.tick : program;
            expression = (e.bytes[0] == 0xBF && e.bytes[1] == 11 && !expression) ? e.tick : expression;
        }
        SEQLIB_CHECK(note > 0 && program == note && expression == note);
        SEQLIB_CHECK_EQ(programs, 2);
    }
}

// With more than 15 channels that play notes, the channels whose settings clash least share a MIDI channel, even if
// their notes sound at once. A note ends where the mixer holds its levels at 0. Another track's expression could make
// it sound again. A note whose levels stay 0 throughout goes. With fewer channels, a note runs on through that silence.
void TestMidiChannelsFollowSettings()
{
    // Channels 1 to 14 hold notes on one key at full volume. Channels 15 and 16 hold notes on other keys at half
    // volume. Channel 4's levels go to 0 at row 2 and channel 15's at row 4. Channel 17's stay at 0.
    std::vector<Cell> cells;
    for (int c = 0; c < 14; c++)
    {
        cells.push_back({0, c, 49, 1, 0x50});
    }
    cells.push_back({0, 14, 53, 1, 0x30});
    cells.push_back({0, 15, 56, 1, 0x30});
    cells.push_back({0, 16, 49, 1, 0x10});
    cells.push_back({2, 3, -1, 0, 0x10});
    cells.push_back({4, 14, -1, 0, 0x10});
    Cart cart;
    cart.Module(17, 6, cells);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_settings";
    Cart few;
    few.Module(2, 6, {{0, 0, 49, 1}, {0, 1, 53, 1}, {2, 1, -1, 0, 0x10}});
    const Rom few_rom = few.ToRom();
    ConvertSettings few_opt = opt;
    few_opt.base_name = "krawall_settings_few";

    const SongReport report = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport few_report = ConvertSong(few_rom, Detect(few_rom), 0, few_opt, nullptr);

    SEQLIB_REQUIRE(report.ok && few_report.ok);
    SEQLIB_CHECK(report.warnings.empty());
    SEQLIB_CHECK_EQ(report.tracks, 16);
    const auto note_of = [](const ntdlib::testing::agb_files::MidiEvents& midi, const std::string& name)
    {
        const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
        const size_t t = size_t(std::find(names.begin(), names.end(), name) - names.begin());
        std::array<uint64_t, 3> note{0, 0, 0}; // the first note on's status, and the end of the first note
        if (t >= midi.tracks.size())
        {
            return note;
        }
        for (const seqlib::midi::Event& e : midi.tracks[t].events)
        {
            if ((e.bytes[0] & 0xF0) == 0x90 && !note[0])
            {
                note[0] = e.bytes[0];
            }
            if ((e.bytes[0] & 0xF0) == 0x80 && !note[1])
            {
                note[1] = 1;
                note[2] = e.tick;
            }
        }
        return note;
    };
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const auto first = note_of(midi, "Channel 1");
    const auto faded = note_of(midi, "Channel 4");
    const auto fifteenth = note_of(midi, "Channel 15");
    const auto sixteenth = note_of(midi, "Channel 16");
    SEQLIB_CHECK(fifteenth[0] != 0 && fifteenth[0] == sixteenth[0]);
    SEQLIB_CHECK(first[0] != fifteenth[0]);
    SEQLIB_CHECK_EQ(note_of(midi, "Channel 17")[0], 0u);
    SEQLIB_CHECK(faded[2] > 0 && faded[2] < first[2]);

    // Where channel 15's note ends, channel 16's note sets its expression again.
    const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
    const size_t sharer = size_t(std::find(names.begin(), names.end(), "Channel 16") - names.begin());
    SEQLIB_REQUIRE(sharer < midi.tracks.size());
    int set_again = 0;
    for (const seqlib::midi::Event& e : midi.tracks[sharer].events)
    {
        set_again += (e.bytes[0] & 0xF0) == 0xB0 && e.bytes[1] == 11 && e.tick == fifteenth[2] ? 1 : 0;
    }
    SEQLIB_CHECK(fifteenth[2] > 0 && fifteenth[2] < sixteenth[2]);
    SEQLIB_CHECK_EQ(set_again, 1);

    const ntdlib::testing::agb_files::MidiEvents few_midi = ReadMidi(few_report.midi_path);
    SEQLIB_CHECK_EQ(note_of(few_midi, "Channel 2")[2], note_of(few_midi, "Channel 1")[2]);
}

// DumpSong lists the header, the orders and the rows.
void TestDump()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    DriverInfo info;
    std::string error;
    const bool found = DetectDriver(rom, DriverOverrides(), info, error);
    const std::string dump = (ntdlib::testing::agb_files::TempFolderPath() / "krawall_dump.txt").string();

    const bool dumped = found && DumpSong(rom, info, 0, dump, error);
    const std::vector<uint8_t> dump_bytes = ReadAll(dump);
    const std::string dump_text(dump_bytes.begin(), dump_bytes.end());

    SEQLIB_CHECK(found && info.songs.size() == 1);
    SEQLIB_CHECK(dumped);
    SEQLIB_CHECK(dump_text.find("2 channels, 1 orders, restart at order 0, speed 3, tempo 125") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  0 | c0 C-4 i1 v64 | c1 E-4 i2 pan 20\n") != std::string::npos);
    SEQLIB_CHECK(dump_text.find("  4 | c0 jump 00\n") != std::string::npos);
}

// A whole module that the game plays once, through its routine for jingles, converts once and without loop markers. In
// loop mode, the same module plays its pattern twice. The log names the routine and the jingle. A call of the routine
// whose entry detection can't read gets a warning.
void TestJingles()
{
    Cart cart;
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t jingle = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.Jingle(1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_jingles";

    const SongReport loop = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport once = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{looped, -1, false}, {jingle, -1, true}}));
    const auto logged = [&info](const std::string& line)
    {
        return std::find(info.log.begin(), info.log.end(), line) != info.log.end();
    };
    SEQLIB_CHECK(logged("the game's routine that plays a module once, as a jingle: 0x08000300"));
    SEQLIB_CHECK(logged("jingles, which play once: 1"));
    SEQLIB_CHECK(info.warnings == std::vector<std::string>({"the game plays a module once from 0x08000348, by a number "
                                                            "that detection can't read. That module converts as a "
                                                            "loop"}));
    SEQLIB_REQUIRE(loop.ok && once.ok);
    SEQLIB_CHECK(loop.loop_end > 0 && once.loop_start < 0 && once.loop_end < 0);
    SEQLIB_CHECK_EQ(ReadMidi(loop.midi_path).Find(0x90).size(), 2u);
    SEQLIB_CHECK_EQ(ReadMidi(once.midi_path).Find(0x90).size(), 1u);
    bool marker = false;
    for (const auto& [tick, bytes] : ReadMidi(once.midi_path).Find(0xFF))
    {
        marker = marker || bytes[1] == 0x06;
    }
    SEQLIB_CHECK(!marker);
}

// In the jingle mode with the loop bit, krapPlay() loops the module. An entry that a routine plays in that mode isn't
// a jingle. The entry's module converts as a loop, without a warning for the call that passes r5.
void TestLoopingJingleMode()
{
    Cart cart;
    const uint32_t module = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.Jingle(0, kModeJingle | kModeLoop);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{module, -1, false}}));
    SEQLIB_CHECK(info.warnings.empty());
    SEQLIB_CHECK(std::none_of(info.log.begin(), info.log.end(),
                              [](const std::string& line) { return line.find("jingle") != std::string::npos; }));
}

// The entries of the game's table of what it plays come first, each in its mode, without the entry that repeats one.
// The module that no entry plays follows. The entry in song mode without the loop bit plays its module once and
// converts without loop markers. The log names the table and the entry whose mode doesn't loop.
void TestPlayTable()
{
    Cart cart;
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t once = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t unplayed = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.PlayTable({{once, kModeSong, 0}, {looped, kModeLoop, 0}, {once, kModeSong, 0}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_play_table";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(
        info.songs ==
        (std::vector<ModuleSong>{{once, -1, false, kModeSong}, {looped, -1, false, kModeLoop}, {unplayed, -1, false}}));
    const auto logged = [&info](const std::string& line)
    {
        return std::find(info.log.begin(), info.log.end(), line) != info.log.end();
    };
    SEQLIB_CHECK(logged("the game's table of what it plays: 0x08002800 (3 entries)"));
    SEQLIB_CHECK(logged("songs whose mode doesn't loop: 0"));
    SEQLIB_REQUIRE(first.ok && second.ok);
    SEQLIB_CHECK(first.loop_start < 0 && first.loop_end < 0 && second.loop_end > 0);
    SEQLIB_CHECK_EQ(ReadMidi(first.midi_path).Find(0x90).size(), 1u);
}

// krapPlay()'s mode 0 plays a module once. An entry in that mode doesn't end the game's table of what it plays.
void TestPlayTableModeZero()
{
    Cart cart;
    const uint32_t once = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.PlayTable({{once, 0, 0}, {looped, kModeLoop, 0}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_mode_zero";

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{once, -1, false, 0}, {looped, -1, false, kModeLoop}}));
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(), "songs whose mode doesn't loop: 0") != info.log.end());
    SEQLIB_REQUIRE(first.ok && second.ok);
    SEQLIB_CHECK(first.loop_start < 0 && first.loop_end < 0 && second.loop_end > 0);
    SEQLIB_CHECK_EQ(ReadMidi(first.midi_path).Find(0x90).size(), 1u);
}

// Franklin's Great Adventures plays each entry of its table of modules in the mode and at the music volume of its
// tables. Its start-up code copies the table of modules to RAM. The entry that repeats one is left out. The player
// sets the music volume after it starts the module, as krapSetMusicVol() does.
void TestModuleSettings()
{
    Cart cart;
    const uint32_t looped = cart.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t once = cart.Module(1, 4, {{0, 0, 49, 1}});
    cart.Put32(kModuleTable + 8, looped);
    cart.ModuleSettings(0x03006050, {kModeLoop, 0, kModeLoop}, {90, 70, 90});
    Cart bad;
    bad.Module(1, 4, {{0, 0, 49, 1}});
    bad.ModuleSettings(kModuleTable, {8}, {90});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const DriverInfo bad_info = Detect(bad.ToRom());
    SEQLIB_REQUIRE(info.songs.size() == 2);
    const Player player(rom, info, info.songs[0]);
    const Player full(rom, info, looped);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{looped, -1, false, kModeLoop, 90}, {once, -1, false, 0, 70}}));
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's routine at 0x08000380 plays each module in the mode of the table at "
                           "0x08002800 and at the music volume of the table at 0x08002900") != info.log.end());
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(), "songs whose mode doesn't loop: 1") != info.log.end());
    SEQLIB_CHECK_EQ(player.Globals()[8], 90);
    SEQLIB_CHECK_EQ(full.Globals()[8], 128);
    SEQLIB_CHECK_EQ(player.Globals()[0] * 128, full.Globals()[0] * 90);
    SEQLIB_CHECK(bad_info.songs == (std::vector<ModuleSong>{{bad_info.modules[0], -1, false}}));
}

// Care Bears: Care Quest plays song 1 of its module at 0x08360B0C at a music volume of 64, and song 2 once, in the song
// mode. Franklin the Turtle plays its module at 0x08335F60 only as a jingle. The Koala Brothers plays its music at 64,
// The Lord of the Rings: The Third Age at 153, Crash of the Titans at 96 and Digimon Racing at 180. A piece that no
// row names keeps the mode that the song and jingle give and the game's music volume.
void TestSongSettings()
{
    Cart bears;
    bears.PutString(kRomBase + 0xAC, "BEAE");
    const uint32_t module =
        bears.Module(1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}}, true, 0x08360B0C);
    bears.Songs(module, {{0}, {1}, {2}});
    Cart franklin;
    franklin.PutString(kRomBase + 0xAC, "BFKE");
    const uint32_t piece = franklin.Module(1, 4, {{0, 0, 49, 1}}, true, 0x08334368);
    const uint32_t jingle = franklin.Module(1, 4, {{0, 0, 49, 1}}, true, 0x08335F60);
    const uint32_t other = franklin.Module(1, 4, {{0, 0, 49, 1}});
    Cart koalas;
    koalas.PutString(kRomBase + 0xAC, "BAKE");
    koalas.Module(1, 4, {{0, 0, 49, 1}});
    Cart third_age;
    third_age.PutString(kRomBase + 0xAC, "B3AE");
    third_age.Module(1, 4, {{0, 0, 49, 1}});
    Cart crash;
    crash.PutString(kRomBase + 0xAC, "BQCE");
    crash.Module(1, 4, {{0, 0, 49, 1}});
    Cart digimon;
    digimon.PutString(kRomBase + 0xAC, "BDGE");
    digimon.Module(1, 4, {{0, 0, 49, 1}});
    Cart digimon_japan;
    digimon_japan.PutString(kRomBase + 0xAC, "BDJJ");
    digimon_japan.Module(1, 4, {{0, 0, 49, 1}});
    const Rom rom = bears.ToRom();
    const DriverInfo info = Detect(rom);
    const DriverInfo franklin_info = Detect(franklin.ToRom());
    SEQLIB_REQUIRE(info.songs.size() == 3);
    const Player first(rom, info, info.songs[0]);
    const Player second(rom, info, info.songs[1]);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_song_settings";

    const SongReport once = ConvertSong(rom, info, 2, opt, nullptr);

    SEQLIB_CHECK(info.songs ==
                 (std::vector<ModuleSong>{{module, 0}, {module, 1, false, -1, 64}, {module, 2, false, kModeSong}}));
    const auto logged = [&info](const std::string& line)
    {
        return std::find(info.log.begin(), info.log.end(), line) != info.log.end();
    };
    SEQLIB_CHECK(logged("the game plays song 1 at a music volume of 64"));
    SEQLIB_CHECK(logged("the game plays song 2 in mode 2"));
    SEQLIB_CHECK_EQ(first.Globals()[8], 128);
    SEQLIB_CHECK_EQ(second.Globals()[8], 64);
    SEQLIB_CHECK_EQ(second.Globals()[0] * 128, first.Globals()[0] * 64);
    SEQLIB_REQUIRE(once.ok);
    SEQLIB_CHECK(once.loop_start < 0 && once.loop_end < 0);
    SEQLIB_CHECK(
        franklin_info.songs ==
        (std::vector<ModuleSong>{{piece, -1, false, -1, 64}, {jingle, -1, false, kModeJingle}, {other, -1, false}}));
    SEQLIB_CHECK_EQ(Detect(koalas.ToRom()).music_volume, 64);
    SEQLIB_CHECK_EQ(Detect(third_age.ToRom()).music_volume, 153);
    SEQLIB_CHECK_EQ(Detect(crash.ToRom()).music_volume, 96);
    SEQLIB_CHECK_EQ(Detect(digimon.ToRom()).music_volume, 180);
    SEQLIB_CHECK_EQ(Detect(digimon_japan.ToRom()).music_volume, 180);
}

// The other releases of the games with rows: their music volumes and mixers, and the songs of the European Care Bears
// and of each revision of the European Franklin.
void TestSongSettingsOfReleases()
{
    const auto release = [](const char* game_code)
    {
        Cart cart;
        cart.PutString(kRomBase + 0xAC, game_code);
        cart.Module(1, 4, {{0, 0, 49, 1}});
        return Detect(cart.ToRom());
    };
    SEQLIB_CHECK_EQ(release("BENP").music_volume, 161);
    SEQLIB_CHECK_EQ(release("BHTJ").music_volume, 120);
    SEQLIB_CHECK_EQ(release("BJKP").music_volume, 128);
    SEQLIB_CHECK_EQ(release("BJKP").music_fade_in, 2);
    SEQLIB_CHECK_EQ(release("BAKP").music_volume, 64);
    SEQLIB_CHECK_EQ(release("B3AJ").music_volume, 153);
    SEQLIB_CHECK_EQ(release("BQCP").music_volume, 96);
    SEQLIB_CHECK_EQ(release("BDGP").music_volume, 180);
    for (const char* game_code : {"B63P", "BCZE"})
    {
        const DriverInfo modified = release(game_code);
        SEQLIB_CHECK(modified.stereo);
        SEQLIB_CHECK_EQ(modified.quality, 0x21);
    }
    SEQLIB_CHECK_EQ(release("B63P").music_volume, 100);
    SEQLIB_CHECK_EQ(release("BCZE").music_volume, 64);
    const std::pair<const char*, uint32_t> kEurope[] = {
        {"B4MP", 64}, {"BBAP", 60}, {"BKFX", 64}, {"BU5P", 80}, {"BU7P", 85}, {"BUIP", 48}, {"BUQP", 80},
    };
    for (const auto& [code, volume] : kEurope)
    {
        SEQLIB_CHECK_EQ(release(code).music_volume, volume);
    }
    SEQLIB_CHECK_EQ(int(release("BU7P").quality), 1);

    Cart bears_europe;
    bears_europe.PutString(kRomBase + 0xAC, "BEAP");
    const uint32_t bears_module = bears_europe.Module(
        1, {{4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}, {4, {{0, 0, 49, 1}}}}, true, 0x08361B28);
    bears_europe.Songs(bears_module, {{0}, {1}, {2}, {3}});
    SEQLIB_CHECK(Detect(bears_europe.ToRom()).songs == (std::vector<ModuleSong>{{bears_module, 0},
                                                                                {bears_module, 1, false, -1, 64},
                                                                                {bears_module, 2},
                                                                                {bears_module, 3, false, kModeSong}}));
    for (const uint32_t revision_jingle : {0x08331178u, 0x08336D64u, 0x08335F60u})
    {
        Cart franklin_europe;
        franklin_europe.PutString(kRomBase + 0xAC, "BFKP");
        const uint32_t jingle_europe = franklin_europe.Module(1, 4, {{0, 0, 49, 1}}, true, revision_jingle);
        SEQLIB_CHECK(Detect(franklin_europe.ToRom()).songs ==
                     (std::vector<ModuleSong>{{jingle_europe, -1, false, kModeJingle}}));
    }
    for (const uint32_t revision_piece : {0x0832F580u, 0x0833516Cu, 0x08334368u})
    {
        Cart franklin_europe;
        franklin_europe.PutString(kRomBase + 0xAC, "BFKP");
        const uint32_t piece_europe = franklin_europe.Module(1, 4, {{0, 0, 49, 1}}, true, revision_piece);
        SEQLIB_CHECK(Detect(franklin_europe.ToRom()).songs ==
                     (std::vector<ModuleSong>{{piece_europe, -1, false, -1, 64}}));
    }
}

// The music volumes and mixers that the games' code sets, The Bee Game's song mode and Postman Pat's table of music
// volumes.
void TestGameCodeSettings()
{
    const std::pair<const char*, uint32_t> volumes[] = {
        {"B2WE", 254}, {"B3UE", 96}, {"B46E", 180}, {"B4ME", 64}, {"B8QE", 140}, {"BBAE", 60}, {"BC4E", 96},
        {"BJXE", 80},  {"BKFE", 64}, {"BU5E", 80},  {"BU7E", 85}, {"BUIE", 48},  {"BUQE", 80}, {"BXPE", 77},
    };
    for (const auto& [code, volume] : volumes)
    {
        Cart game;
        game.PutString(kRomBase + 0xAC, code);
        game.Module(1, 4, {{0, 0, 49, 1}});
        SEQLIB_CHECK_EQ(Detect(game.ToRom()).music_volume, volume);
    }
    Cart spyro;
    spyro.PutString(kRomBase + 0xAC, "BU7E");
    spyro.Module(1, 4, {{0, 0, 49, 1}});
    SEQLIB_CHECK_EQ(int(Detect(spyro.ToRom()).quality), 1);

    // The Bee Game plays its modules in the song mode.
    Cart bee;
    bee.PutString(kRomBase + 0xAC, "BKFE");
    const uint32_t bee_module = bee.Module(1, 4, {{0, 0, 49, 1}}, true, 0x08380B3C);
    SEQLIB_CHECK(Detect(bee.ToRom()).songs == (std::vector<ModuleSong>{{bee_module, -1, false, kModeSong}}));
    Cart bee_europe;
    bee_europe.PutString(kRomBase + 0xAC, "BKFX");
    const uint32_t bee_europe_module = bee_europe.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0837B0D4);
    SEQLIB_CHECK(Detect(bee_europe.ToRom()).songs ==
                 (std::vector<ModuleSong>{{bee_europe_module, -1, false, kModeSong}}));

    // Postman Pat's table gives each entry of the game's table of modules a music volume.
    Cart pat;
    pat.PutString(kRomBase + 0xAC, "BROP");
    const uint32_t pat_first = pat.Module(1, 4, {{0, 0, 49, 1}});
    const uint32_t pat_second = pat.Module(1, 4, {{0, 0, 49, 1}});
    pat.Resize(0x40000);
    pat.Put8(0x08032C48, 128);
    pat.Put8(0x08032C49, 90);
    SEQLIB_CHECK(Detect(pat.ToRom()).songs ==
                 (std::vector<ModuleSong>{{pat_first, -1, false, -1, 128}, {pat_second, -1, false, -1, 90}}));
}

// Cocoto Kart Racer plays its module at 0x0826181C at a music volume of 100. Cocoto Platform Jumper plays its module at
// 0x0802DE90 at 73. A piece that no row names keeps the game's music volume, and a row counts only for its game.
void TestCocotoSongSettings()
{
    Cart kart;
    kart.PutString(kRomBase + 0xAC, "BC5P");
    const uint32_t race = kart.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0826181C);
    const uint32_t menu = kart.Module(1, 4, {{0, 0, 49, 1}});
    Cart jumper;
    jumper.PutString(kRomBase + 0xAC, "BC8P");
    const uint32_t level = jumper.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0802DE90);
    Cart other;
    other.PutString(kRomBase + 0xAC, "ZZZZ");
    const uint32_t same_place = other.Module(1, 4, {{0, 0, 49, 1}}, true, 0x0802DE90);
    const Rom rom = kart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.songs.size() == 2);

    SEQLIB_CHECK(info.songs == (std::vector<ModuleSong>{{race, -1, false, -1, 100}, {menu, -1, false}}));
    SEQLIB_CHECK(Detect(jumper.ToRom()).songs == (std::vector<ModuleSong>{{level, -1, false, -1, 73}}));
    SEQLIB_CHECK(Detect(other.ToRom()).songs == (std::vector<ModuleSong>{{same_place, -1, false}}));
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(), "the game plays song 0 at a music volume of 100") !=
                 info.log.end());
    const Player raced(rom, info, info.songs[0]);
    const Player unlisted(rom, info, info.songs[1]);
    SEQLIB_CHECK_EQ(raced.Globals()[8], 100);
    SEQLIB_CHECK_EQ(unlisted.Globals()[8], 128);
    SEQLIB_CHECK_EQ(raced.Globals()[0] * 128, unlisted.Globals()[0] * 100);
}

// A sample of 20 points that loops forwards over its last 6 gets its loop repeated 6 times: 36 points, the first length
// of kMinLoop points or more.
void TestShortLoop()
{
    Cart cart;
    constexpr uint32_t kHeader = kRomBase + 0xF000;
    cart.Put32(kHeader + kSampleLoopLength, 6);
    cart.Put32(kHeader + kSampleEnd, kHeader + kSamplePoints + 20);
    cart.Put32(kHeader + 8, 8363);
    cart.Put8(kHeader + kSampleLoop, 1);
    for (uint32_t i = 0; i < 20; i++)
    {
        cart.Put8(kHeader + kSamplePoints + i, uint8_t(i * 9));
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    InstrumentSet set;
    set.Program(kHeader, 0, 0);
    const Sf2File sf = set.Build(rom, info);
    SEQLIB_REQUIRE_EQ(sf.samples.size(), 1);
    SEQLIB_CHECK(ntdlib::testing::agb_files::LengthenedLoop(sf.samples[0], 14, 6));
}

// A module without instruments plays the game's samples by number: instrument 3 is sample 2, over every note. A
// sample's C-4 plays at 8363 Hz. The lookup adds the sample's relative note to the note's key.
void TestInstruments()
{
    Cart cart;
    cart.Module(1, 4, {{0, 0, 49, 1}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ntdlib::krawall::ModuleInfo module;
    SEQLIB_REQUIRE(ntdlib::krawall::ReadModule(rom, info.build, info.modules[0], module));

    const auto forward = ntdlib::krawall::ReadSample(rom, info, 0);
    const auto one_shot = ntdlib::krawall::ReadSample(rom, info, 1);
    const auto instrument = ntdlib::krawall::ReadInstrument(rom, info, module, 3);
    const auto lookup = ntdlib::krawall::LookUpInstrument(rom, info, module, 2, 49);
    SEQLIB_REQUIRE(forward && one_shot && instrument && lookup);
    SEQLIB_CHECK(forward->loop == 1 && forward->loop_length == kForwardLoop &&
                 forward->end - forward->data == kForwardPoints);
    SEQLIB_CHECK(forward->rate == kC4Rate && forward->volume == 64 && forward->header == rom.U32(kSampleTable));
    SEQLIB_CHECK(one_shot->loop == 0 && one_shot->volume == 48 && one_shot->end - one_shot->data == kOneShotPoints);
    SEQLIB_CHECK(instrument->address == 0 && instrument->regions == std::vector<ntdlib::krawall::Region>(
                                                                        {{1, ntdlib::krawall::kInstrumentNotes, 2}}));
    SEQLIB_CHECK(lookup->instrument == 0 && lookup->sample == 1 && lookup->header == rom.U32(kSampleTable + 4) &&
                 lookup->key == 48);
    SEQLIB_CHECK(!ntdlib::krawall::LookUpInstrument(rom, info, module, 2, ntdlib::krawall::kNoteOff));
    SEQLIB_CHECK(!ntdlib::krawall::ReadInstrument(rom, info, module, 0));
}

// An audition plays a note of an instrument on the module's first channel. The forward loop holds the note, and the
// rows don't play. The release stops the note: without an envelope, a note off stops the channel.
void TestAudition()
{
    Cart cart;
    cart.Module(1, 4, {{0, 0, 49, 2}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ntdlib::krawall::ArchivePlayer player(rom, info);
    player.StartNote(0, 1, 61);
    std::vector<int16_t> out;
    for (int f = 0; f < 30; f++)
    {
        player.RunFrame(out);
    }
    const bool sounds = std::any_of(out.end() - 1000, out.end(), [&out](int16_t v) { return v != out.back(); });
    const bool busy = player.Busy();
    const auto& mixer = player.Driver().MixerOf(player.Driver().Channels()[0]);
    const bool forward = mixer.sample == rom.U32(kSampleTable) && player.Driver().RowNumber() == 0;
    player.ReleaseNote();
    int frames = 0;
    for (; frames < 20 && player.Busy(); frames++)
    {
        player.RunFrame(out);
    }
    std::vector<int16_t> after;
    player.RunFrame(after);
    const bool silent = std::all_of(after.begin(), after.end(), [&after](int16_t v) { return v == after.front(); });

    SEQLIB_CHECK(sounds && busy && forward);
    SEQLIB_CHECK(frames > 0 && frames < 20);
    SEQLIB_CHECK(!after.empty() && silent && player.Driver().MixerOf(player.Driver().Channels()[0]).status == 0);
}

// ConvertSongFiles gives the bytes of the files that ConvertSong writes, and names no paths.
void TestSongFiles()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.out_dir = ntdlib::testing::agb_files::TempFolderPath().string();
    opt.base_name = "krawall_files";

    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);

    SEQLIB_REQUIRE(written.ok && files.report.ok && !files.report.silent);
    SEQLIB_CHECK(files.report.midi_path.empty() && files.report.sf2_path.empty());
    SEQLIB_CHECK(!files.midi.empty() && files.midi == ReadAll(written.midi_path));
    SEQLIB_CHECK(!files.sf2.empty() && files.sf2 == ReadAll(written.sf2_path));
    SEQLIB_CHECK(files.report.warnings == written.warnings && files.report.seconds == written.seconds);
}

// The performance holds the notes of the MIDI file, each with its row's entry. The module plays its 5 rows, and the
// jump on row 4 goes back to row 0. Each row is a stretch, every 3 ticks: the row that the jump goes back to has
// Entry::kLoop, and the others Entry::kOpen. Channel 0's notes start at its entry on row 0, and channel 1's at its
// entries on rows 0 and 2. Channel 0's volume starts at 64, and the slide on row 1 takes it to 60 and 56 on the row's
// second and third ticks. Each tick is a MIDI tick.
void TestPerformance()
{
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 2;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const ntdlib::krawall::Decoded decoded = ntdlib::krawall::DecodeRomSequence(rom, info, 0);
    const std::vector<uint32_t> offsets =
        ntdlib::krawall::ItemOffsets(decoded.sequence, ntdlib::krawall::TraitsOf(info));

    std::vector<std::tuple<uint32_t, int, uint32_t>> notes;
    std::array<std::vector<uint32_t>, 2> entries;
    std::map<uint32_t, uint32_t> ons;
    for (size_t t = 0; t < 2; t++)
    {
        for (const TrackEvent& e : p.tracks[t])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                ons[e.note] = e.tick;
                entries[t].push_back(p.note_commands[e.note - 1].offset);
            }
            else if (e.kind == EventKind::kNoteOff)
            {
                notes.emplace_back(ons[e.note], e.key, e.tick);
            }
        }
    }
    std::vector<std::tuple<uint32_t, int, uint32_t>> midi;
    for (const seqlib::midi::Note& n : seqlib::midi::Notes(seqlib::midi::Read(files.midi)))
    {
        midi.emplace_back(uint32_t(n.on), int(n.key), uint32_t(n.off));
    }
    std::sort(notes.begin(), notes.end());
    std::sort(midi.begin(), midi.end());

    const std::vector<Stretch>& stretches = p.stretches[0];
    const std::vector<MixChange>& mix = p.mix[0];
    const uint32_t row_0 = offsets[1];
    const uint32_t end = offsets[decoded.sequence.items.size() - 1];
    SEQLIB_REQUIRE(entries[0].size() >= 2 && entries[1].size() >= 4 && stretches.size() >= 6 && mix.size() >= 4);
    SEQLIB_CHECK(p.base == ntdlib::krawall::SequenceAddress(rom, info, 0) && p.loop && p.loops == 2);
    SEQLIB_CHECK(notes == midi);
    SEQLIB_CHECK(entries[0][0] == offsets[1] && entries[0][1] == offsets[1]);
    SEQLIB_CHECK(entries[1][0] == offsets[3] && entries[1][1] != offsets[3] && entries[1][2] == offsets[3]);
    for (size_t i = 0; i < 5; i++)
    {
        SEQLIB_CHECK(stretches[i].tick == 3 * i && stretches[i].entry == Entry::kOpen &&
                     stretches[i].first <= stretches[i].last);
        SEQLIB_CHECK(i == 0 || stretches[i].first > stretches[i - 1].last);
    }
    SEQLIB_CHECK(stretches[0].first == row_0 && stretches[4].last == end);
    SEQLIB_CHECK(stretches[5] == (Stretch{15, row_0, stretches[0].last, Entry::kLoop, 0, false}));
    SEQLIB_CHECK(p.note_commands[0].stretch == 0 && p.stretches[1] == stretches && p.stretches[2].empty());
    SEQLIB_CHECK(mix[0] == (MixChange{0, Mix::kVolume, 64}) && mix[1] == (MixChange{0, Mix::kPan, 0}));
    SEQLIB_CHECK(mix[2] == (MixChange{4, Mix::kVolume, 60}) && mix[3] == (MixChange{5, Mix::kVolume, 56}));
}

// Each stretch keeps the contract of a stretch: its entries play at its tick, and the wait at its end gives the next
// row's tick. Speed effects set speed 3 on row 0 and speed 6 on row 2, and the jump on row 5 goes back to row 0. Each
// row is a stretch. The rows at speed 6 last twice as many MIDI ticks as the rows at speed 3, and every note starts at
// its row's tick.
void TestPerformanceAtTwoSpeeds()
{
    Cart cart;
    cart.Module(1, 6,
                {{0, 0, 49, 1, -1, kSetSpeed, 3},
                 {1, 0, 51, 1, -1, -1, 0},
                 {2, 0, 53, 1, -1, kSetSpeed, 6},
                 {3, 0, 55, 1, -1, -1, 0},
                 {4, 0, 56, 1, -1, -1, 0},
                 {5, 0, 58, 1, -1, kJump, 0x00}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 2;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);

    const std::vector<Stretch>& stretches = p.stretches[0];
    SEQLIB_REQUIRE(stretches.size() >= 12);
    int notes = 0;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            const NoteCommand& command = p.note_commands[e.note - 1];
            SEQLIB_REQUIRE(command.stretch < stretches.size());
            const Stretch& s = stretches[command.stretch];
            SEQLIB_CHECK_EQ(s.tick, e.tick);
            SEQLIB_CHECK(s.first <= command.offset && command.offset <= s.last);
            notes++;
        }
    }
    const size_t midi_notes = seqlib::midi::Notes(seqlib::midi::Read(files.midi)).size();

    const uint32_t fast = stretches[1].tick - stretches[0].tick;
    const uint32_t slow = stretches[3].tick - stretches[2].tick;
    SEQLIB_CHECK(notes >= 12 && size_t(notes) == midi_notes);
    SEQLIB_CHECK(fast > 0 && slow == 2 * fast && stretches[2].tick - stretches[1].tick == fast);
    SEQLIB_CHECK(stretches[4].tick - stretches[3].tick == slow && stretches[5].tick - stretches[4].tick == slow);
    SEQLIB_CHECK(stretches[6].tick - stretches[5].tick == slow && stretches[7].tick - stretches[6].tick == fast);
    for (size_t i = 0; i < 12; i++)
    {
        SEQLIB_CHECK(stretches[i].entry == (i == 6 ? Entry::kLoop : Entry::kOpen) && stretches[i].depth == 0);
        SEQLIB_CHECK(stretches[i].first <= stretches[i].last);
    }
}

} // namespace

int RunTests()
{
    TestDetection();
    TestPlayer();
    TestMixer();
    TestEnhancedAttack();
    TestSongs();
    TestConversion();
    TestBars();
    TestLoopStartsAtSecondPass();
    TestManyChannels();
    TestSharedMidiChannel();
    TestMidiChannelsFollowSettings();
    TestDump();
    TestJingles();
    TestLoopingJingleMode();
    TestPlayTable();
    TestPlayTableModeZero();
    TestModuleSettings();
    TestSongSettings();
    TestSongSettingsOfReleases();
    TestGameCodeSettings();
    TestCocotoSongSettings();
    TestShortLoop();
    TestInstruments();
    TestAudition();
    TestSongFiles();
    TestPerformance();
    TestPerformanceAtTwoSpeeds();

    return seqlib::testing::Summary("krawall");
}

} // namespace ntdlib::sequence_export::krawall

int main()
{
    return ntdlib::sequence_export::krawall::RunTests();
}
