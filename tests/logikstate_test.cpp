// SPDX-License-Identifier: MIT

// Unit tests for Logik State's driver. They build small synthetic cartridges in memory. A test reads no game's ROM.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/player.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/logikstate/convert.h"
#include "ntdlib/sequence_export/logikstate/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/logikstate_cart.h"

namespace ntdlib::sequence_export::logikstate
{
namespace
{

using ntdlib::logikstate::Build;
using ntdlib::logikstate::Cell;
using ntdlib::logikstate::Channel;
using ntdlib::logikstate::DetectDriver;
using ntdlib::logikstate::FrameSamples;
using ntdlib::logikstate::kBankPoints;
using ntdlib::logikstate::kFullVolume;
using ntdlib::logikstate::kMixChannels;
using ntdlib::logikstate::kModuleOrderList;
using ntdlib::logikstate::kModulePatternBytes;
using ntdlib::logikstate::kModuleSize;
using ntdlib::logikstate::kNoLoopRow;
using ntdlib::logikstate::kNoPitch;
using ntdlib::logikstate::kPeriodClock;
using ntdlib::logikstate::kSamples;
using ntdlib::logikstate::Player;
using ntdlib::logikstate::ReadCell;
using ntdlib::logikstate::ReadModule;
using ntdlib::logikstate::Sample;
using ntdlib::logikstate::SamplePoints;
using ntdlib::logikstate::Song;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempSettings;
using namespace ntdlib::testing::logikstate_cart;

// The lengths of a row and of a tick of effects at the test modules' rate, speed 6 and tempo 125.
constexpr uint32_t kRow = 2522;
constexpr uint32_t kTick = 420;

// The MIDI ticks of a row, and of a quarter note at 4 rows a beat.
constexpr uint32_t kRowTicks = 120;
constexpr uint32_t kQuarter = 480;

// The size of a sample's header in a SoundFont, and the offsets of its fields.
constexpr size_t kShdrSize = 46;
constexpr size_t kShdrStart = 20;
constexpr size_t kShdrStartLoop = 28;
constexpr size_t kShdrEndLoop = 32;
constexpr size_t kShdrRate = 36;

// The bank of the test cartridges: three samples of 100, 400 and 3000 points.
std::vector<std::vector<int8_t>> TestBank()
{
    std::vector<std::vector<int8_t>> bank;
    for (size_t length : {size_t(100), size_t(400), size_t(3000)})
    {
        std::vector<int8_t> points(length);
        for (size_t i = 0; i < length; i++)
        {
            points[i] = int8_t(i % 100) - 50;
        }
        bank.push_back(points);
    }

    return bank;
}

// A module of `channels` channels whose three samples have the bank's lengths, full volume and no loop, with one
// pattern of 64 empty rows.
TestModule EmptyModule(uint32_t channels = 4)
{
    TestModule module;
    module.channels = channels;
    module.patterns = {std::vector<TestRow>(64)};
    module.samples[0] = {100, 0, 64, 0, 0};
    module.samples[1] = {400, 0, 64, 0, 0};
    module.samples[2] = {3000, 0, 64, 0, 0};

    return module;
}

// A cell with a note of `sample` at `pitch`, and one with an effect alone.
TestCell NoteCell(uint16_t pitch, uint16_t sample, uint8_t effect = 0, uint8_t operand = 0)
{
    return {pitch, sample, effect, operand};
}

TestCell EffectCell(uint8_t effect, uint8_t operand)
{
    return {kNoPitch, 0, effect, operand};
}

// Returns the ROM of a cartridge with the test bank and `module`. One call of the play routine names the module.
Rom RomWith(const TestModule& module)
{
    Cart cart(TestBank());
    cart.AddCall(cart.AddModule(module), kBank, 0);

    return cart.ToRom();
}

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, info, error);

    return info;
}

// A row or a tick that a player ran: the samples from the song's start, and the channels after it.
struct Step
{
    bool row = false;
    uint64_t time = 0;
    int order = 0;
    int row_number = 0;
    std::array<Channel, kMixChannels> channels;
};

// Plays the first song of `rom` for `frames` frames and returns its rows and ticks.
std::vector<Step> Play(const Rom& rom, int frames, std::vector<std::string>* warnings = nullptr)
{
    const DriverInfo info = Detect(rom);
    std::vector<Step> steps;
    if (info.songs.empty())
    {
        return steps;
    }

    Player player(rom, info.songs[0], kFullVolume, info.build);
    const auto record = [&](bool row)
    {
        steps.push_back({row, player.Elapsed(), player.RowOrder(), player.RowNumber(), player.Channels()});
    };
    player.OnStep(record);
    for (int f = 0; f < frames && player.Playing(); f++)
    {
        player.Frame();
    }
    if (warnings)
    {
        *warnings = player.Warnings();
    }

    return steps;
}

// Returns the steps that are rows.
std::vector<Step> Rows(const std::vector<Step>& steps)
{
    std::vector<Step> rows;
    std::copy_if(steps.begin(), steps.end(), std::back_inserter(rows), [](const Step& s) { return s.row; });

    return rows;
}

// Returns the step that the driver works out for `period`.
uint32_t PeriodStep(int32_t period)
{
    return ((uint32_t(kPeriodClock) / uint32_t(period * 2)) << 12) / kRate;
}

// Detection finds the load routine by the signature in its literal pool, the play routine by its call of the load
// routine, the modules by their headers, and each module's bank and start orders in the game's calls of the play
// routine. A module uses the bank of the call that names it. A module that no call names uses the bank of most calls.
void TestDetection()
{
    Cart cart(TestBank());
    TestModule three = EmptyModule();
    three.orders = {0, 0, 0};
    TestModule own = EmptyModule();
    own.own_samples = true;
    const uint32_t first = cart.AddModule(EmptyModule());
    const uint32_t second = cart.AddModule(three);
    const uint32_t third = cart.AddModule(own);
    const uint32_t fourth = cart.AddModule(EmptyModule());
    cart.AddCall(first, kBank, 0);
    cart.AddCall(0, kBank, 0);
    cart.AddCall(second, kBank, 2);
    cart.AddCall(fourth, 0x08000000, 0);
    const Rom rom = cart.ToRom();

    DriverInfo info;
    std::string error;
    const bool found = DetectDriver(rom, info, error);

    SEQLIB_REQUIRE(found && error.empty());
    SEQLIB_CHECK_EQ(info.load_routine, kLoad);
    SEQLIB_CHECK_EQ(info.play_routine, kPlay);
    const std::vector<Song> expected = {
        {first, kBank, 0}, {second, kBank, 0}, {second, kBank, 2}, {third, 0, 0}, {fourth, kBank, 0}};
    SEQLIB_CHECK(info.songs == expected);
    SEQLIB_CHECK(!info.log.empty() && info.warnings.empty());
}

// A cartridge without modules shows no sign of the driver. One with modules and no play routine fails with an error.
// One whose calls name no bank plays each module's points, with a warning.
void TestDetectionFailures()
{
    Cart none(TestBank());
    Cart no_code(TestBank());
    no_code.AddModule(EmptyModule());
    no_code.Put32(kLoad + 8, 0);
    Cart no_bank(TestBank());
    const uint32_t module = no_bank.AddModule(EmptyModule());
    no_bank.AddCall(module, 0, 0);

    DriverInfo info, info_no_code, info_no_bank;
    std::string error, error_no_code, error_no_bank;
    const bool found = DetectDriver(none.ToRom(), info, error);
    const bool found_no_code = DetectDriver(no_code.ToRom(), info_no_code, error_no_code);
    const bool found_no_bank = DetectDriver(no_bank.ToRom(), info_no_bank, error_no_bank);

    SEQLIB_CHECK(!found && error.empty());
    SEQLIB_CHECK(!found_no_code && !error_no_code.empty());
    SEQLIB_REQUIRE(found_no_bank && info_no_bank.songs.size() == 1);
    SEQLIB_CHECK_EQ(info_no_bank.songs[0].bank, 0u);
    SEQLIB_CHECK_EQ(info_no_bank.warnings.size(), size_t(1));
}

// The game's code passes one constant above 0 to the routine that sets the music's volume: the volume that the game
// starts with. The mixer subtracts 64 less the volume from each channel's volume. A channel at or below that plays
// nothing, and its sample keeps its place. Two constants leave the volume full.
void TestMusicVolume()
{
    Cart cart(TestBank());
    TestModule module = EmptyModule();
    module.samples[1].volume = 16;
    module.patterns[0][0] = {NoteCell(200, 3), NoteCell(200, 2)};
    cart.AddCall(cart.AddModule(module), kBank, 0);
    cart.AddMusicVolume(0);
    cart.AddMusicVolume(48);
    Cart two = cart;
    two.AddMusicVolume(32);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);
    const DriverInfo info_two = Detect(two.ToRom());
    Player player(rom, info.songs[0], info.music_volume);
    player.Frame();
    const SongReport report = ConvertSong(rom, info, 0, TempSettings("logikstate_music_volume"), nullptr);
    const std::vector<seqlib::midi::Event> controllers = ReadMidi(report.midi_path).Find(0xB0);

    SEQLIB_CHECK_EQ(info.music_volume_routine, kMusicVolume);
    SEQLIB_CHECK_EQ(info.music_volume, 48);
    SEQLIB_CHECK_EQ(info_two.music_volume, kFullVolume);
    SEQLIB_CHECK(player.MixVolume(player.Channels()[0], 0) == 48 && player.MixVolume(player.Channels()[1], 1) == 0);
    SEQLIB_CHECK(player.Channels()[0].position != 0 && player.Channels()[1].position == 0);
    SEQLIB_REQUIRE(report.ok && report.tracks == 1);
    const auto sets = [](const seqlib::midi::Event& e)
    {
        return e.bytes[1] == 11 && e.bytes[2] == 78;
    };
    SEQLIB_CHECK(std::any_of(controllers.begin(), controllers.end(), sets));
}

// A game whose code switches the sound off plays both FIFOs at half volume from then on, and its conversion plays at
// half the level. The game's code calls the routine for that, or a routine of the driver that calls it. A call that
// only the driver's code reaches doesn't count.
void TestSoundOff()
{
    Cart cart(TestBank());
    TestModule module = EmptyModule();
    module.patterns[0][0] = {NoteCell(200, 3)};
    cart.AddCall(cart.AddModule(module), kBank, 0);
    Cart through = cart, driver_alone = cart;
    cart.AddSoundOff(SoundOffCaller::kGame);
    through.AddSoundOff(SoundOffCaller::kGameThroughDriver);
    driver_alone.AddSoundOff(SoundOffCaller::kDriverAlone);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);
    const SongReport report = ConvertSong(rom, info, 0, TempSettings("logikstate_sound_off"), nullptr);
    const std::vector<seqlib::midi::Event> controllers = ReadMidi(report.midi_path).Find(0xB0);

    SEQLIB_CHECK_EQ(info.sound_off_call, kGameCode + 2);
    SEQLIB_CHECK_EQ(Detect(through.ToRom()).sound_off_call, kGameCode + 2);
    SEQLIB_CHECK_EQ(Detect(driver_alone.ToRom()).sound_off_call, 0u);
    SEQLIB_REQUIRE(report.ok && report.tracks == 1);
    // The sample's volume of 64 at half volume is a level of 32 of 128: 127 times the root of 32 over 127.
    const auto sets = [](const seqlib::midi::Event& e)
    {
        return e.bytes[1] == 11 && e.bytes[2] == 64;
    };
    SEQLIB_CHECK(std::any_of(controllers.begin(), controllers.end(), sets));
}

// ReadModule() reads the header's counts, orders, rows and samples, and refuses a module whose patterns don't fill the
// size that the header names or whose order names a pattern that the module doesn't have. The points of a song's
// samples follow the bank's headers by the bank's lengths, or the module's patterns by the module's lengths.
void TestModuleReader()
{
    Cart cart(TestBank());
    TestModule module = EmptyModule(6);
    module.orders = {1, 0};
    module.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(20)};
    module.patterns[1][3] = {TestCell(), {208, 3, 0xC, 0x20}};
    module.samples[1] = {300, 7, 48, 16, 200};
    const uint32_t address = cart.AddModule(module);
    Cart bad_size = cart;
    bad_size.Put32(address + kModulePatternBytes, 4);
    Cart bad_order = cart;
    bad_order.Put32(address + kModuleOrderList, 2);
    const Rom rom = cart.ToRom();

    ntdlib::logikstate::Module read, unread;
    const bool ok = ReadModule(rom, address, read);
    const Cell cell = ReadCell(rom, read.patterns[1] + (3 * 6 + 1) * 4);
    const std::array<uint32_t, kSamples> banked = SamplePoints(rom, read, {address, kBank, 0});
    const std::array<uint32_t, kSamples> own = SamplePoints(rom, read, {address, 0, 0});

    SEQLIB_REQUIRE(ok);
    SEQLIB_CHECK(read.channels == 6 && read.rate == kRate && read.order_count == 2 && read.pattern_count == 2);
    SEQLIB_CHECK(read.orders[0] == 1 && read.orders[1] == 0 && read.rows[0] == 64 && read.rows[1] == 20);
    SEQLIB_CHECK_EQ(read.patterns[1], address + kModuleSize + 64 * 6 * 4);
    SEQLIB_CHECK(read.samples[1].length == 300 && read.samples[1].fine_tune == 7 && read.samples[1].volume == 48);
    SEQLIB_CHECK(read.samples[1].loop_start == 16 && read.samples[1].loop_length == 200);
    SEQLIB_CHECK(cell.pitch == 208 && cell.sample == 3 && cell.effect == 0xC && cell.operand == 0x20);
    SEQLIB_CHECK(!ReadModule(bad_size.ToRom(), address, unread));
    SEQLIB_CHECK(!ReadModule(bad_order.ToRom(), address, unread));
    SEQLIB_CHECK(!ReadModule(rom, address + 4, unread));
    SEQLIB_CHECK(banked[0] == kBank + kBankPoints && banked[1] == banked[0] + 100 && banked[2] == banked[1] + 400);
    SEQLIB_CHECK(banked[3] == 0 && banked[30] == 0);
    const uint32_t after = address + kModuleSize + (64 + 20) * 6 * 4;
    SEQLIB_CHECK(own[0] == after && own[1] == after + 100 && own[2] == after + 400);
}

// A row lasts the rate times the speed over the tempo's ticks a second, in whole samples, and the ticks of effects come
// every 50th of the rate, whatever the speed and the tempo. A tick that falls due with a row is left out.
void TestRowsAndTicks()
{
    TestModule module = EmptyModule();
    module.patterns[0][2] = {EffectCell(0xF, 0x96)};
    module.patterns[0][3] = {EffectCell(0xF, 0x03)};
    module.patterns[0][4] = {EffectCell(0xF, 0x01), EffectCell(0xF, 0x7D)};

    const std::vector<Step> steps = Play(RomWith(module), 40);

    // The first row's six ticks, then the second row, 2 samples after the sixth tick.
    SEQLIB_REQUIRE(steps.size() > 30);
    for (uint32_t i = 0; i < 8; i++)
    {
        const bool row = i == 0 || i == 7;
        SEQLIB_CHECK_EQ(steps[i].row, row);
        SEQLIB_CHECK_EQ(steps[i].time, uint64_t(i == 7 ? kRow : i * kTick));
    }

    // Tempo 150 makes 60 ticks a second, speed 3 half the ticks, and speed 1 at tempo 125 a row of a tick's length.
    const std::vector<Step> rows = Rows(steps);
    SEQLIB_REQUIRE(rows.size() > 8);
    SEQLIB_CHECK_EQ(rows[3].time - rows[2].time, uint64_t(kRate * 6 / 60));
    SEQLIB_CHECK_EQ(rows[4].time - rows[3].time, uint64_t(kRate * 3 / 60));
    SEQLIB_CHECK_EQ(rows[5].time - rows[4].time, uint64_t(kTick));
    SEQLIB_CHECK_EQ(rows[6].time - rows[5].time, uint64_t(kTick));
    for (size_t r = 5; r + 1 < rows.size(); r++)
    {
        SEQLIB_CHECK_EQ(rows[r].row_number, int(r));
    }
}

// The frame routine's loop counts each pass as a sample more than it mixes. A tick that falls due at a frame's last
// sample therefore waits for the next frame, and the song loses that sample.
void TestFrameEnd()
{
    // At 5050 Hz, a frame has 85 samples and a tick 101. Tick 69 falls due at sample 6969, the last of frame 81. Every
    // sixth tick falls due with a row and is left out.
    TestModule module = EmptyModule();
    module.rate = 5050;
    const uint32_t frame = FrameSamples(module.rate);

    const std::vector<Step> steps = Play(RomWith(module), 100);
    std::vector<uint64_t> ticks;
    for (const Step& s : steps)
    {
        if (!s.row)
        {
            ticks.push_back(s.time);
        }
    }

    SEQLIB_CHECK_EQ(frame, 85u);
    SEQLIB_REQUIRE(ticks.size() > 60);
    SEQLIB_CHECK_EQ(ticks[55], uint64_t(67 * 101));
    SEQLIB_CHECK_EQ(ticks[56], uint64_t(68 * 101));
    SEQLIB_CHECK_EQ(ticks[57], uint64_t(82 * frame));
    SEQLIB_CHECK_EQ(ticks[58], uint64_t(82 * frame + 101));
}

// A slide moves the period at each tick, and the step follows. The slide's offset stays through the rows without a
// note. A new note's first step still counts it, and the note's first tick drops it.
void TestSlides()
{
    TestModule module = EmptyModule();
    module.patterns[0][0] = {NoteCell(200, 1, 1, 4), NoteCell(200, 1, 2, 3)};
    module.patterns[0][2] = {NoteCell(200, 1)};

    const std::vector<Step> steps = Play(RomWith(module), 30);
    const std::vector<Step> rows = Rows(steps);

    SEQLIB_REQUIRE(steps.size() > 20 && rows.size() > 3);
    SEQLIB_CHECK_EQ(steps[0].channels[0].step, TestStep(200));
    SEQLIB_CHECK_EQ(steps[1].channels[0].slide, -4);
    SEQLIB_CHECK_EQ(steps[1].channels[0].step, PeriodStep(428 - 4));
    SEQLIB_CHECK_EQ(steps[1].channels[1].step, PeriodStep(428 + 3));
    SEQLIB_CHECK_EQ(rows[1].channels[0].slide, -24);
    SEQLIB_CHECK_EQ(rows[1].channels[1].slide, 18);
    SEQLIB_CHECK_EQ(rows[2].channels[0].slide, 0);
    SEQLIB_CHECK_EQ(rows[2].channels[0].step, PeriodStep(428 - 24));
    SEQLIB_CHECK_EQ(rows[2].channels[0].position, 0u);
    SEQLIB_CHECK_EQ(rows[3].channels[0].step, TestStep(200));
}

// A volume slide leaves a row's first tick out, raises the volume by its first digit or lowers it by its second, and
// stops at 0 and 64. A volume effect sets any volume.
void TestVolumes()
{
    TestModule module = EmptyModule();
    module.samples[1].volume = 40;
    module.patterns[0][0] = {NoteCell(200, 2, 0xA, 0x20), NoteCell(200, 2, 0xA, 0x03)};
    module.patterns[0][1] = {EffectCell(0xA, 0x80), EffectCell(0xA, 0x0F)};
    module.patterns[0][3] = {EffectCell(0xC, 0x50)};

    const std::vector<Step> steps = Play(RomWith(module), 30);
    const std::vector<Step> rows = Rows(steps);

    SEQLIB_REQUIRE(steps.size() > 20 && rows.size() > 3);
    SEQLIB_CHECK_EQ(steps[0].channels[0].volume, 40);
    SEQLIB_CHECK_EQ(steps[1].channels[0].volume, 40);
    SEQLIB_CHECK_EQ(steps[2].channels[0].volume, 42);
    SEQLIB_CHECK_EQ(rows[1].channels[0].volume, 50);
    SEQLIB_CHECK_EQ(rows[1].channels[1].volume, 25);
    SEQLIB_CHECK_EQ(rows[2].channels[0].volume, kFullVolume);
    SEQLIB_CHECK_EQ(rows[2].channels[1].volume, 0);
    SEQLIB_CHECK_EQ(rows[3].channels[0].volume, 0x50);
}

// A vibrato starts from its wave's start at each row and moves the period by the wave times the depth over 128. An
// arpeggio leaves a row's first tick alone and then plays its first interval, its second twice, and so on.
void TestVibratoAndArpeggio()
{
    TestModule module = EmptyModule();
    module.patterns[0][0] = {NoteCell(200, 1, 4, 0x42), NoteCell(200, 1, 0, 0x47)};

    const std::vector<Step> steps = Play(RomWith(module), 10);

    SEQLIB_REQUIRE(steps.size() > 7);
    const std::array<int16_t, 6> wave = {0, 1, 2, 3, 3, 3};
    const std::array<uint32_t, 6> intervals = {0, 4, 7, 7, 4, 7};
    for (size_t tick = 0; tick < 6; tick++)
    {
        const Step& s = steps[tick + 1];
        SEQLIB_CHECK_EQ(s.channels[0].vibrato, wave[tick]);
        SEQLIB_CHECK_EQ(s.channels[0].step, wave[tick] ? PeriodStep(428 + wave[tick]) : TestStep(200));
        SEQLIB_CHECK_EQ(s.channels[1].step,
                        intervals[tick] ? PeriodStep(TestPeriod(200 + intervals[tick] * 8)) : TestStep(200));
    }
}

// A slide to a note keeps the row's note as its target and uses the volume of the row's sample. The driver compares
// the pitches' steps with the slide's period offset. The note therefore reaches the target's pitch at the first tick.
// Only the first four channels run their effects between rows.
void TestSlideToNote()
{
    TestModule module = EmptyModule(6);
    module.samples[1].volume = 30;
    module.patterns[0][0] = {NoteCell(200, 1), NoteCell(216, 1), {}, {}, NoteCell(200, 1, 1, 4)};
    module.patterns[0][1] = {NoteCell(216, 2, 3, 2), NoteCell(200, 0, 3, 2)};

    const std::vector<Step> steps = Play(RomWith(module), 20);
    const std::vector<Step> rows = Rows(steps);

    SEQLIB_REQUIRE(rows.size() > 2);
    const size_t second =
        size_t(std::find_if(steps.begin() + 1, steps.end(), [](const Step& s) { return s.row; }) - steps.begin());
    SEQLIB_REQUIRE(second + 1 < steps.size());
    const Channel& up = steps[second].channels[0];
    SEQLIB_CHECK(up.pitch == 200 && up.slide_target == 216 && up.slide_step == -2 && up.volume == 30);
    SEQLIB_CHECK(up.sample == 0 && up.position != 0);
    SEQLIB_CHECK_EQ(steps[second].channels[1].slide_step, 2);
    SEQLIB_CHECK(steps[second + 1].channels[0].pitch == 216 && steps[second + 1].channels[0].slide == 0);
    SEQLIB_CHECK_EQ(steps[second + 1].channels[0].step, TestStep(216));
    SEQLIB_CHECK_EQ(steps[second + 1].channels[1].pitch, 200);
    SEQLIB_CHECK(rows[2].channels[4].slide == 0 && rows[2].channels[4].step == TestStep(200));
}

// A jump makes the next row the first of its order. A break moves to the next order, at the cell of 4 times its
// operand. The pattern loop sets a marker with a count of 0. Another count clears the marker, or goes back to the
// pattern's first row when no marker is set. After the last order comes the first.
void TestOrders()
{
    TestModule module = EmptyModule();
    module.orders = {0, 1, 2};
    module.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(64), std::vector<TestRow>(8)};
    module.patterns[0][1] = {EffectCell(0xB, 2)};
    module.patterns[2][1] = {EffectCell(0xE, 0x60)};
    module.patterns[2][2] = {EffectCell(0xE, 0x61)};
    module.patterns[2][3] = {{}, EffectCell(0xD, 2)};
    module.patterns[0][3] = {EffectCell(0xE, 0x62)};
    module.patterns[0][2] = {EffectCell(0xB, 1)};
    module.patterns[1][0] = {EffectCell(0xD, 0), EffectCell(0xD, 5)};

    const std::vector<Step> rows = Rows(Play(RomWith(module), 120));

    // Order 0 jumps to order 2. Order 2 breaks to row 2 of order 0. That row jumps to order 1, whose two breaks count
    // as one. Order 2 then plays to its break again, and order 0's row 3 goes back to row 0.
    const std::vector<std::pair<int, int>> expected = {{0, 0}, {0, 1}, {2, 0}, {2, 1}, {2, 2}, {2, 3}, {0, 2},
                                                       {1, 0}, {2, 0}, {2, 1}, {2, 2}, {2, 3}, {0, 2}};
    SEQLIB_REQUIRE(rows.size() > expected.size());
    for (size_t i = 0; i < expected.size(); i++)
    {
        SEQLIB_CHECK_EQ(rows[i].order, expected[i].first);
        SEQLIB_CHECK_EQ(rows[i].row_number, expected[i].second);
    }
}

// The pattern loop with a count above 0 and no marker goes back to the pattern's first row.
void TestPatternLoop()
{
    TestModule module = EmptyModule();
    module.patterns[0][2] = {EffectCell(0xE, 0x63)};

    const Rom rom = RomWith(module);
    const std::vector<Step> rows = Rows(Play(rom, 60));
    Player player(rom, Detect(rom).songs[0]);

    SEQLIB_REQUIRE(rows.size() > 6);
    SEQLIB_CHECK_EQ(rows[3].row_number, 0);
    SEQLIB_CHECK_EQ(rows[5].row_number, 2);
    SEQLIB_CHECK_EQ(rows[6].row_number, 0);
    SEQLIB_CHECK_EQ(player.LoopRow(), kNoLoopRow);
}

// Detection tells the build with long records by the size of a record in the driver's code. That build plays a channel
// at volume 0 too, applies the music's volume to the module's channels alone, and has ProTracker's pattern loop for
// each channel: E60 marks a row, and E6x goes back to it x times.
void TestLongRecords()
{
    TestModule module = EmptyModule();
    module.patterns[0][0] = {NoteCell(200, 3, 0xC, 0)};
    module.patterns[0][1] = {EffectCell(0xE, 0x60)};
    module.patterns[0][3] = {EffectCell(0xE, 0x62)};
    Cart cart(TestBank());
    cart.AddCall(cart.AddModule(module), kBank, 0);
    const Rom short_rom = cart.ToRom();
    cart.UseLongRecords();
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);
    const std::vector<Step> rows = Rows(Play(rom, 100));
    const std::vector<Step> short_rows = Rows(Play(short_rom, 100));
    Player player(rom, info.songs[0], 48, info.build);
    Channel full;
    full.volume = kFullVolume;

    SEQLIB_CHECK(info.build == Build::kLongRecords && Detect(short_rom).build == Build::kShortRecords);
    SEQLIB_REQUIRE(rows.size() > 10 && short_rows.size() > 10);
    SEQLIB_CHECK(rows[1].channels[0].position != 0 && short_rows[1].channels[0].position == 0);
    SEQLIB_CHECK(player.Sounding(rows[1].channels[0], 0));
    SEQLIB_CHECK_EQ(rows[4].row_number, 1);
    SEQLIB_CHECK_EQ(rows[7].row_number, 1);
    SEQLIB_CHECK_EQ(rows[10].row_number, 4);
    SEQLIB_CHECK_EQ(short_rows[4].row_number, 4);
    SEQLIB_CHECK(player.MixVolume(full, 3) == 48 && player.MixVolume(full, 4) == kFullVolume);
}

// A speed of 0 makes rows without length. The driver can't play them, and the model stops with a warning.
void TestSpeedZero()
{
    TestModule module = EmptyModule();
    module.patterns[0][1] = {EffectCell(0xF, 0)};

    std::vector<std::string> warnings;
    const std::vector<Step> rows = Rows(Play(RomWith(module), 60, &warnings));

    SEQLIB_CHECK_EQ(rows.size(), size_t(2));
    SEQLIB_CHECK_EQ(warnings.size(), size_t(1));
}

// The mixer stops a sample without a loop at its end. At a looped sample's end, the points move on to the loop's start
// and the loop's length becomes the length. A channel at volume 0 keeps its place. A sample number without a note
// sets the channel to that sample's points and volume, and the channel keeps its place and its length.
void TestMixerPlaces()
{
    TestModule module = EmptyModule(6);
    module.samples[1] = {400, 0, 64, 100, 200};
    module.samples[2].volume = 20;
    module.patterns[0][0] = {NoteCell(200, 1), NoteCell(200, 2), NoteCell(104, 3), NoteCell(104, 3)};
    module.patterns[0][1] = {{}, {}, EffectCell(0xC, 0), {kNoPitch, 1, 0, 0}};
    module.patterns[0][3] = {{}, {}, EffectCell(0xC, 0x40)};
    const Rom rom = RomWith(module);
    const DriverInfo info = Detect(rom);
    const uint32_t points = kBank + kBankPoints;

    const std::vector<Step> rows = Rows(Play(rom, 40));
    Player player(rom, info.songs[0]);

    SEQLIB_REQUIRE(rows.size() > 4);
    const Channel& ended = rows[1].channels[0];
    SEQLIB_CHECK(ended.length == 0 && ended.points == points && !player.Sounding(ended, 0));
    const Channel& looped = rows[2].channels[1];
    SEQLIB_CHECK(looped.points == points + 100 + 100 && looped.length == 200 && looped.loop_start == 0);
    SEQLIB_CHECK(looped.first_point == points + 100 && looped.whole_length == 400 && looped.whole_loop_start == 100);
    const Channel& paused = rows[1].channels[2];
    SEQLIB_CHECK(paused.volume == 0 && paused.position != 0 && !player.Sounding(paused, 2));
    SEQLIB_CHECK_EQ(rows[2].channels[2].position, paused.position);
    SEQLIB_CHECK_EQ(rows[3].channels[2].position, paused.position);
    SEQLIB_CHECK(rows[4].channels[2].position > paused.position && rows[4].channels[2].volume == 0x40);
    const Channel& changed = rows[1].channels[3];
    SEQLIB_CHECK(changed.points == points && changed.sample == 0 && changed.volume == 64 && changed.length == 3000);
    SEQLIB_CHECK(changed.position == paused.position && changed.starts == 2 && changed.first_point == points);
}

// Returns true if two times in seconds are within a millisecond. The frames that end a sample or two short move a
// row's time by that much.
bool Near(double a, double b)
{
    return std::fabs(a - b) < 0.001;
}

// Converts the first song of `rom` with the default settings and returns the report.
SongReport Convert(const Rom& rom, const std::string& name)
{
    return ConvertSong(rom, Detect(rom), 0, TempSettings(name), nullptr);
}

// Returns field `offset` of sample `index` of a SoundFont.
uint32_t SampleField(const Sf2Records& sf, size_t index, size_t offset)
{
    return Le32(sf.Record("shdr", kShdrSize, index) + offset);
}

// A module converts to a MIDI file with a track for each channel that plays notes. A row lasts 120 MIDI ticks, 4 rows
// make a quarter note at the starting tempo, a note's key follows its pitch with pitch 200 on key 60, and controller 11
// sets the channel's volume. The loop from the first row is marked, and the file plays it twice.
void TestConversion()
{
    TestModule module = EmptyModule();
    module.samples[1] = {400, 0, 32, 100, 300};
    module.patterns[0][0] = {NoteCell(200, 1), {}, NoteCell(104, 2)};
    module.patterns[0][4] = {NoteCell(208, 1)};
    module.patterns[0][8] = {{}, {}, NoteCell(296, 2)};
    const Rom rom = RomWith(module);

    const SongReport report = Convert(rom, "logikstate_conversion");
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const Sf2Records sf = ReadSf2(report.sf2_path);
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(midi);
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(midi);

    SEQLIB_REQUIRE(report.ok && !report.silent && report.tracks == 2);
    SEQLIB_REQUIRE(midi.tracks.size() == 3 && notes.size() == 8);
    SEQLIB_CHECK_EQ(midi.division, kQuarter);
    SEQLIB_CHECK(notes[0].track == 1 && notes[0].channel == 0 && notes[0].key == 60 && notes[0].velocity == 127);
    SEQLIB_CHECK(notes[0].on == 0 && notes[0].off == 20);
    SEQLIB_CHECK(notes[1].key == 61 && notes[1].on == 4 * kRowTicks);
    SEQLIB_CHECK(notes[4].track == 2 && notes[4].channel == 2 && notes[4].key == 48);
    SEQLIB_CHECK(notes[4].on == 0 && notes[4].off == 8 * kRowTicks);
    SEQLIB_CHECK(notes[5].key == 72 && notes[5].on == 8 * kRowTicks && notes[5].off == 64 * kRowTicks);
    SEQLIB_REQUIRE(markers.size() == 2);
    SEQLIB_CHECK(markers[0].text == "loopStart" && markers[0].tick == 0);
    SEQLIB_CHECK(markers[1].text == "loopEnd" && markers[1].tick == 64 * kRowTicks);
    SEQLIB_CHECK_EQ(midi.tracks[0].end_tick, uint64_t(128 * kRowTicks));

    // Volume 64 is half of full scale, and volume 32 a quarter: controller 11 at 127 times the square root of each.
    const std::vector<seqlib::midi::Event> first = midi.Find(0xB0);
    const std::vector<seqlib::midi::Event> third = midi.Find(0xB2);
    const auto expression = [](const std::vector<seqlib::midi::Event>& events, uint8_t value)
    {
        const auto sets = [&](const seqlib::midi::Event& e)
        {
            return e.bytes[1] == 11 && e.bytes[2] == value;
        };
        return std::any_of(events.begin(), events.end(), sets);
    };
    SEQLIB_CHECK(expression(first, 90) && expression(third, 64));
    SEQLIB_CHECK(midi.Find(0xE0).empty() && midi.Find(0xE2).empty());

    // Each sample plays pitch 200 at 8287 Hz on key 60. The loop that ends with its sample stays in place.
    SEQLIB_REQUIRE(sf.Count("shdr", kShdrSize) == 3);
    SEQLIB_CHECK_EQ(SampleField(sf, 0, kShdrRate), uint32_t(kPeriodClock) / (428 * 2));
    SEQLIB_CHECK_EQ(SampleField(sf, 1, kShdrStartLoop) - SampleField(sf, 1, kShdrStart), 100u);
    SEQLIB_CHECK_EQ(SampleField(sf, 1, kShdrEndLoop) - SampleField(sf, 1, kShdrStart), 400u);
    SEQLIB_CHECK(Near(report.seconds, double(128 * kRow) / kRate));
    SEQLIB_CHECK(report.loop_start == 0 && Near(report.loop_end, double(64 * kRow) / kRate));
}

// The fine tune of a sample is part of the rate of its SoundFont sample. Its notes therefore have no bend. A slide
// bends its note at each tick. A loop that ends before its sample does follows the sample's points as a copy. The
// second note of the slide's channel drops the slide. The module's two passes then play alike.
void TestTuningAndLoops()
{
    TestModule module = EmptyModule();
    module.samples[0].fine_tune = 4;
    module.samples[2] = {3000, 0, 64, 1000, 500};
    module.patterns[0][0] = {NoteCell(200, 1), NoteCell(200, 3, 1, 8)};
    module.patterns[0][32] = {{}, NoteCell(200, 3)};
    const Rom rom = RomWith(module);

    const SongReport report = Convert(rom, "logikstate_tuning");
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const Sf2Records sf = ReadSf2(report.sf2_path);
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(midi);

    SEQLIB_REQUIRE(report.ok && notes.size() == 6 && sf.Count("shdr", kShdrSize) == 3);
    SEQLIB_CHECK(notes[0].key == 60 && notes[2].key == 60);
    SEQLIB_CHECK(midi.Find(0xE0).empty());
    const std::vector<seqlib::midi::Bend> bends = seqlib::midi::Bends(midi);
    SEQLIB_REQUIRE(bends.size() >= 6);
    SEQLIB_CHECK(bends[0].channel == 1 && bends[1].value > 8192 && bends[2].value > bends[1].value);
    SEQLIB_CHECK_EQ(SampleField(sf, 0, kShdrRate), uint32_t(kPeriodClock) / (TestPeriod(204) * 2u));
    SEQLIB_CHECK_EQ(SampleField(sf, 1, kShdrStartLoop) - SampleField(sf, 1, kShdrStart), 3000u);
    SEQLIB_CHECK_EQ(SampleField(sf, 1, kShdrEndLoop) - SampleField(sf, 1, kShdrStart), 3500u);
}

// A note ends where its volume falls to 0. When the volume rises again, a new note starts from the point where the
// sample waited, on an instrument that starts there.
void TestPausedNote()
{
    TestModule module = EmptyModule();
    module.patterns[0][0] = {NoteCell(104, 3)};
    module.patterns[0][1] = {EffectCell(0xC, 0)};
    module.patterns[0][3] = {EffectCell(0xC, 0x40)};
    const Rom rom = RomWith(module);

    const SongReport report = Convert(rom, "logikstate_paused");
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const Sf2Records sf = ReadSf2(report.sf2_path);
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(midi);

    SEQLIB_REQUIRE(report.ok && notes.size() == 4);
    SEQLIB_CHECK(notes[0].on == 0 && notes[0].off == kRowTicks && notes[0].key == 48);
    SEQLIB_CHECK(notes[1].on == 3 * kRowTicks && notes[1].key == 48);
    SEQLIB_CHECK_EQ(midi.Find(0xC0).size(), size_t(4));
    SEQLIB_CHECK_EQ(sf.Count("shdr", kShdrSize), size_t(2));
    SEQLIB_CHECK_EQ(sf.Count("inst", 22), size_t(3));
    bool offset = false;
    for (size_t zone = 0; zone + 1 < sf.Count("ibag", 4); zone++)
    {
        const std::map<uint16_t, uint16_t> gens = sf.ZoneGens(zone);
        const auto it = gens.find(uint16_t(seqlib::sf2::kStartAddrsOffset));
        offset = offset || (it != gens.end() && it->second == (kRow * TestStep(104)) >> 12);
    }
    SEQLIB_CHECK(offset);
}

// A channel keeps its volume when the module comes back to its loop. Where the second pass therefore starts its notes
// unlike the first, the marked loop starts at the second pass.
void TestSecondPass()
{
    TestModule module = EmptyModule();
    module.orders = {0, 1};
    module.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(64)};
    module.patterns[0][0] = {NoteCell(104, 3)};
    module.patterns[1][0] = {NoteCell(104, 0)};
    module.patterns[1][63] = {EffectCell(0xC, 0x20), EffectCell(0xB, 1)};
    const Rom rom = RomWith(module);

    const SongReport report = Convert(rom, "logikstate_second_pass");
    const std::vector<seqlib::midi::Text> markers = seqlib::midi::Markers(ReadMidi(report.midi_path));

    SEQLIB_REQUIRE(report.ok && markers.size() == 2);
    SEQLIB_CHECK_EQ(markers[0].tick, uint64_t(128 * kRowTicks));
    SEQLIB_CHECK_EQ(markers[1].tick, uint64_t(192 * kRowTicks));
    SEQLIB_CHECK(Near(report.loop_start, double(128 * kRow) / kRate));
}

// A module that ends in a loop of empty rows becomes a file without a loop. A jingle ends that way. The file ends where
// the loop would start. A loop that a looping sample sounds through stays.
void TestSilentLoop()
{
    TestModule module = EmptyModule();
    module.orders = {0, 1};
    module.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(64)};
    module.patterns[0][0] = {NoteCell(104, 1)};
    module.patterns[1][63] = {EffectCell(0xB, 1)};
    TestModule held = module;
    held.samples[0] = {100, 0, 64, 0, 100};
    const Rom rom = RomWith(module);
    const Rom held_rom = RomWith(held);

    const SongReport report = Convert(rom, "logikstate_silent_loop");
    const SongReport held_report = Convert(held_rom, "logikstate_held_loop");

    SEQLIB_REQUIRE(report.ok && held_report.ok);
    SEQLIB_CHECK(seqlib::midi::Markers(ReadMidi(report.midi_path)).empty());
    SEQLIB_CHECK(Near(report.seconds, double(64 * kRow) / kRate));
    SEQLIB_CHECK_EQ(seqlib::midi::Markers(ReadMidi(held_report.midi_path)).size(), size_t(2));
}

// ConvertSongFiles() returns the bytes of the files that ConvertSong() writes. A song whose chosen tracks play no note
// is silent, and a song that the game doesn't have returns a report that isn't ok.
void TestSongFiles()
{
    TestModule module = EmptyModule();
    module.patterns[0][0] = {NoteCell(200, 1)};
    const Rom rom = RomWith(module);
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("logikstate_song_files");
    ConvertSettings none = opt;
    none.track_mask = 0;

    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const SongReport inspected = InspectSong(rom, info, 0, opt);
    const SongFiles silent = ConvertSongFiles(rom, info, 0, none);
    const SongFiles missing = ConvertSongFiles(rom, info, 1, opt);

    SEQLIB_REQUIRE(written.ok && files.report.ok && inspected.ok);
    SEQLIB_CHECK(!written.midi_path.empty() && !written.sf2_path.empty());
    SEQLIB_CHECK(files.report.midi_path.empty() && files.report.sf2_path.empty() && inspected.midi_path.empty());
    SEQLIB_CHECK(!files.midi.empty() && files.midi == ReadAll(written.midi_path));
    SEQLIB_CHECK(!files.sf2.empty() && files.sf2 == ReadAll(written.sf2_path));
    SEQLIB_CHECK(files.report.seconds == written.seconds && inspected.seconds == written.seconds);
    SEQLIB_CHECK_EQ(files.report.tracks, written.tracks);
    SEQLIB_CHECK(silent.report.ok && silent.report.silent && silent.midi.empty() && silent.sf2.empty());
    SEQLIB_CHECK(!missing.report.ok && missing.midi.empty() && missing.sf2.empty());
}

// A shared set numbers the instruments of every song together, and each sample is written to the SoundFont once.
void TestSharedSoundfont()
{
    Cart cart(TestBank());
    TestModule first = EmptyModule();
    first.patterns[0][0] = {NoteCell(200, 1), NoteCell(200, 2)};
    TestModule second = EmptyModule();
    second.patterns[0][0] = {NoteCell(200, 2), NoteCell(200, 3)};
    cart.AddCall(cart.AddModule(first), kBank, 0);
    cart.AddCall(cart.AddModule(second), kBank, 0);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("logikstate_shared");

    InstrumentSet shared;
    const SongReport a = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport b = ConvertSong(rom, info, 1, opt, &shared);
    const Sf2File file = shared.Build(rom);

    SEQLIB_CHECK(a.ok && b.ok && a.sf2_path.empty() && b.sf2_path.empty());
    SEQLIB_CHECK_EQ(file.samples.size(), size_t(3));
    SEQLIB_CHECK_EQ(file.presets.size(), size_t(3));
    const std::vector<seqlib::midi::Event> programs = ReadMidi(b.midi_path).Find(0xC1);
    SEQLIB_REQUIRE(!programs.empty());
    SEQLIB_CHECK_EQ(programs[0].bytes[1], 2);
}

int RunTests()
{
    TestDetection();
    TestDetectionFailures();
    TestMusicVolume();
    TestSoundOff();
    TestModuleReader();
    TestRowsAndTicks();
    TestFrameEnd();
    TestSlides();
    TestVolumes();
    TestVibratoAndArpeggio();
    TestSlideToNote();
    TestOrders();
    TestPatternLoop();
    TestLongRecords();
    TestSpeedZero();
    TestMixerPlaces();
    TestConversion();
    TestTuningAndLoops();
    TestPausedNote();
    TestSecondPass();
    TestSilentLoop();
    TestSongFiles();
    TestSharedSoundfont();

    return seqlib::testing::Summary("logikstate");
}

} // namespace
} // namespace ntdlib::sequence_export::logikstate

int main()
{
    return ntdlib::sequence_export::logikstate::RunTests();
}
