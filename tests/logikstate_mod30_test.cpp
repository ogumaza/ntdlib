// SPDX-License-Identifier: MIT

// Unit tests for the GBAMOD30 modules of Logik State's later drivers: the reader, the model of the players and their
// builds, and the conversion. They build small synthetic cartridges in memory. A test reads no game's ROM.

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/mod30.h"
#include "ntdlib/logikstate/mod30_player.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/logikstate/convert.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/logikstate_mod30_cart.h"

namespace ntdlib::sequence_export::logikstate
{
namespace
{

using ntdlib::logikstate::BuildOf;
using ntdlib::logikstate::DecodeMod30Pattern;
using ntdlib::logikstate::DetectDriver;
using ntdlib::logikstate::DetectLayout;
using ntdlib::logikstate::FindMod30Bank;
using ntdlib::logikstate::FindMod30Modules;
using ntdlib::logikstate::Format;
using ntdlib::logikstate::kMod30LongHeader;
using ntdlib::logikstate::kMod30NoNote;
using ntdlib::logikstate::kMod30ShortHeader;
using ntdlib::logikstate::Mod30Bank;
using ntdlib::logikstate::Mod30Cell;
using ntdlib::logikstate::Mod30Group;
using ntdlib::logikstate::Mod30Layout;
using ntdlib::logikstate::Mod30Module;
using ntdlib::logikstate::Mod30Player;
using ntdlib::logikstate::Mod30Settings;
using ntdlib::logikstate::ReadMod30Module;
using ntdlib::logikstate::Song;
using ntdlib::testing::agb_files::CartImage;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::TempSettings;
using namespace ntdlib::testing::logikstate_mod30_cart;

// The mixer's rate and the samples of a row and of a tick at rate index 0, speed 6 and tempo 125: 50 ticks a second.
constexpr uint32_t kRate = 10512;
constexpr uint32_t kRowSamples = kRate * 6 / 50;
constexpr uint32_t kTickSamples = kRate / 50;

// The step of note 48 of a sample with relative note 1 at kRate: 8363 Hz in 4096ths of a point a sample.
constexpr uint32_t kStep48 = (8363u << 12) / kRate;

TestCell Note(uint16_t note, uint8_t sample, uint8_t effect = 0, uint8_t operand = 0)
{
    return {note, sample, 0, effect, operand};
}

TestCell Effect(uint8_t effect, uint8_t operand)
{
    return {kMod30NoNote, 0, 0, effect, operand};
}

// A one-channel module of layout B whose one pattern has `rows` rows, the first of them `first`.
TestModule OneRow(const TestCell& first, uint32_t rows = 4)
{
    TestModule m;
    m.rows = rows;
    std::vector<std::vector<TestCell>> pattern(rows, std::vector<TestCell>(1));
    pattern[0][0] = first;
    m.patterns = {pattern};

    return m;
}

// A bank's samples: sample 1 of 2000 points at volume 32, sample 2 of 100 points at full volume. Neither loops.
std::vector<TestSample> Samples()
{
    return {{2000, 0, 32, 0, 0, 1}, {100, 0, 64, 0, 0, 1}};
}

// The settings of a build with the cart's tables, at rate index 0, full volumes and the loop flag on.
Mod30Settings SettingsOf(Mod30Group group, uint32_t bank, uint32_t header_size = kMod30LongHeader)
{
    Mod30Settings s;
    s.build = BuildOf(group);
    s.bank = bank;
    s.header_size = header_size;
    s.linear = kLinearTable;
    s.loop = true;

    return s;
}

// A cart with `module` and the bank of Samples(), and its parts.
struct Cart
{
    Rom rom;
    uint32_t module = 0;
    uint32_t bank = 0;
};

Cart MakeOne(const TestModule& module, uint32_t header_size = kMod30LongHeader)
{
    std::vector<uint32_t> addresses;
    Cart c;
    c.rom = MakeCart({module}, Samples(), header_size, addresses, c.bank).ToRom();
    c.module = addresses[0];

    return c;
}

// Runs `player` for `frames` frames.
void Run(Mod30Player& player, int frames)
{
    for (int i = 0; i < frames; i++)
    {
        player.Frame();
    }
}

// The reader: each layout's header, its patterns' places, and the cells of the packed streams.
void TestReader()
{
    std::vector<std::vector<TestCell>> first(8, std::vector<TestCell>(2));
    first[0][0] = {60, 2, 33, 0xA, 0x21};
    first[0][1] = Effect(0xF, 0x04);
    first[5][1] = {48, 1, 0, 0, 0};
    const std::vector<std::vector<TestCell>> second(8, std::vector<TestCell>(2));

    for (const Mod30Layout layout : {Mod30Layout::kA, Mod30Layout::kB, Mod30Layout::kC})
    {
        TestModule m;
        m.layout = layout;
        m.channels = 2;
        m.rows = 8;
        m.speed = 5;
        m.tempo = 140;
        m.orders = {1, 0, 1};
        m.patterns = {first, second};
        std::vector<uint32_t> addresses;
        uint32_t bank = 0;
        const Rom rom = MakeCart({m}, Samples(), kMod30LongHeader, addresses, bank).ToRom();

        Mod30Module read;
        SEQLIB_REQUIRE(ReadMod30Module(rom, addresses[0], read));
        SEQLIB_CHECK(DetectLayout(rom, addresses[0]) == layout && read.layout == layout);
        SEQLIB_CHECK(read.channels == 2 && read.rows == 8 && read.speed == 5 && read.tempo == 140);
        SEQLIB_CHECK(read.order_count == 3 && read.real_orders == 3 && read.pattern_count == 2);
        SEQLIB_CHECK(read.orders[0] == 1 && read.orders[2] == 1);
        SEQLIB_CHECK(read.pattern_offsets == (layout == Mod30Layout::kB));
        SEQLIB_CHECK(read.end <= bank && read.end + 4 > bank);
        SEQLIB_CHECK(FindMod30Modules(rom) == addresses);

        std::vector<std::vector<Mod30Cell>> cells;
        std::string error;
        SEQLIB_REQUIRE(DecodeMod30Pattern(rom, read.patterns[0], 2, 8, cells, error));
        SEQLIB_CHECK(cells[0][0].note == 60 && cells[0][0].sample == 2 && cells[0][0].volume == 33);
        SEQLIB_CHECK(cells[0][0].effect == 0xA && cells[0][0].operand == 0x21);
        SEQLIB_CHECK(cells[1][0].effect == 0xF && cells[1][0].operand == 0x04 && cells[1][0].note == kMod30NoNote);
        SEQLIB_CHECK(cells[1][5].note == 48 && cells[1][5].sample == 1);
        SEQLIB_CHECK(cells[0][7].note == kMod30NoNote && cells[0][7].sample == 0);
    }

    // A header that gives no channels isn't a module.
    CartImage bad(0x10000);
    TestModule none = OneRow(Note(48, 1));
    none.channels = 0;
    PutModule(bad, kFirstModule, none);
    Mod30Module unread;
    SEQLIB_CHECK(!ReadMod30Module(bad.ToRom(), kFirstModule, unread));
}

// The bank after the last module: headers of 16 bytes where they fit, and of 12 otherwise.
void TestBank()
{
    for (const uint32_t size : {kMod30LongHeader, kMod30ShortHeader})
    {
        std::vector<uint32_t> addresses;
        uint32_t bank = 0;
        const Rom rom = MakeCart({OneRow(Note(48, 1))}, Samples(), size, addresses, bank).ToRom();
        Mod30Bank found;
        SEQLIB_REQUIRE(FindMod30Bank(rom, addresses, found));
        SEQLIB_CHECK_EQ(found.address, bank);
        SEQLIB_CHECK_EQ(found.header_size, size);
        SEQLIB_CHECK(found.samples[0].length == 2000 && found.samples[0].volume == 32);
        SEQLIB_CHECK(found.samples[1].length == 100 && found.samples[1].relative_note == 1);
        SEQLIB_CHECK_EQ(found.points[1], found.points[0] + 2000);
        SEQLIB_CHECK_EQ(found.end, found.points[1] + 100);
    }
}

// The timing at the start, a note's step from the linear table, and the rate of a sample's root note.
void TestTimingAndPitch()
{
    const Cart c = MakeOne(OneRow(Note(48, 1)));
    for (const Mod30Group group : {Mod30Group::kB, Mod30Group::kE2})
    {
        Mod30Player player(c.rom, c.module, SettingsOf(group, c.bank));
        SEQLIB_REQUIRE(player.Valid());
        player.Frame();
        SEQLIB_CHECK_EQ(player.Rate(), kRate);
        SEQLIB_CHECK(player.Speed() == 6 && player.Tempo() == 125);
        SEQLIB_CHECK_EQ(player.RowSamples(), kRowSamples);
        SEQLIB_CHECK_EQ(player.TickSamples(), kTickSamples);
        SEQLIB_CHECK_EQ(player.Records()[0].step, kStep48);
        SEQLIB_CHECK(player.Records()[0].volume == 32 && player.Records()[0].starts == 1);
        SEQLIB_CHECK(std::fabs(player.RootRate(0) - 8363 * std::exp2(1.0 / 12)) < 2);
    }
}

// The volume after the first row: effect A with operand 0x10 raises the volume at each tick after the first. Group B's
// build raises it by twice the operand. That build runs no tick with the row. A row of 4 ticks therefore raises it
// twice.
void TestVolumeSlides()
{
    TestModule m = OneRow(Note(48, 1, 0xA, 0x10), 4);
    m.speed = 4;
    const Cart c = MakeOne(m);
    const auto volume_at_row_1 = [&](Mod30Group group)
    {
        Mod30Player player(c.rom, c.module, SettingsOf(group, c.bank));
        int32_t volume = -1;
        int rows = 0;
        player.OnStep(
            [&](bool row)
            {
                if (row && ++rows == 2)
                {
                    volume = player.Records()[0].volume;
                }
            });
        Run(player, 20);
        return volume;
    };
    SEQLIB_CHECK_EQ(volume_at_row_1(Mod30Group::kE2), 32 + 3);
    SEQLIB_CHECK_EQ(volume_at_row_1(Mod30Group::kB), 32 + 2 * 2);
}

// Effect F sets the speed with an operand up to 0x1F, and the tempo with a larger one.
void TestSpeedAndTempo()
{
    TestModule m = OneRow(Effect(0xF, 0x03), 4);
    m.patterns[0][1][0] = Effect(0xF, 0x80);
    const Cart c = MakeOne(m);
    Mod30Player player(c.rom, c.module, SettingsOf(Mod30Group::kE2, c.bank));
    std::vector<uint32_t> speeds, tempos;
    player.OnStep(
        [&](bool row)
        {
            if (row)
            {
                speeds.push_back(player.Speed());
                tempos.push_back(player.Tempo());
            }
        });
    Run(player, 30);
    SEQLIB_REQUIRE(speeds.size() >= 2);
    SEQLIB_CHECK(speeds[0] == 3 && tempos[0] == 125);
    SEQLIB_CHECK(speeds[1] == 3 && tempos[1] == 128);
}

// The loop flag: without it, a module stops after its last order. Group A's build has no flag, and always stops.
void TestLoopFlag()
{
    const Cart c = MakeOne(OneRow(Note(48, 1), 4));
    const auto playing_after = [&](Mod30Group group, bool loop)
    {
        Mod30Settings s = SettingsOf(group, c.bank);
        s.loop = loop;
        Mod30Player player(c.rom, c.module, s);
        Run(player, 60);
        return player.Playing();
    };
    SEQLIB_CHECK(playing_after(Mod30Group::kE2, true));
    SEQLIB_CHECK(!playing_after(Mod30Group::kE2, false));
    SEQLIB_CHECK(!playing_after(Mod30Group::kA, true));
}

// The mixer of mode 0 in the LS_Play builds leaves a channel whose scaled volume is 0 alone, and its sample waits.
// Group B's mixer moves it on. The mixer of mode 1 sets the volume of a channel whose sample has ended to 0.
void TestMixers()
{
    const Cart c = MakeOne(OneRow(Note(48, 1)));
    for (const Mod30Group group : {Mod30Group::kE2, Mod30Group::kB})
    {
        Mod30Settings s = SettingsOf(group, c.bank);
        s.music = 0;
        Mod30Player player(c.rom, c.module, s);
        Run(player, 3);
        const bool waits = group == Mod30Group::kE2;
        SEQLIB_CHECK_EQ(player.Records()[0].position == 0, waits);
        SEQLIB_CHECK_EQ(player.Sounding(player.Records()[0], 0), !waits);
    }

    const Cart short_sample = MakeOne(OneRow(Note(48, 2)));
    for (const int mode : {0, 1})
    {
        Mod30Settings s = SettingsOf(Mod30Group::kE2, short_sample.bank);
        s.mixer_mode = mode;
        Mod30Player player(short_sample.rom, short_sample.module, s);
        Run(player, 4);
        SEQLIB_CHECK(!player.Sounding(player.Records()[0], 0));
        SEQLIB_CHECK_EQ(player.Records()[0].volume == 0, mode == 1);
    }
}

// The conversion: a note's key from its rate and the sample's root rate, the loop with the loop flag, and the song's
// end after the last row without it.
void TestConversion()
{
    TestModule m = OneRow(Note(49, 1), 8);
    m.patterns[0][4][0] = Note(61, 1);
    const Cart c = MakeOne(m);
    for (const bool loop : {true, false})
    {
        DriverInfo info;
        info.format = Format::kGbaMod30;
        info.mod30 = SettingsOf(Mod30Group::kE2, c.bank);
        info.music_volume = info.mod30.music;
        Song song;
        song.module = c.module;
        song.bank = c.bank;
        song.loop_flag = loop;
        info.songs = {song};

        const std::string name = loop ? "mod30_loop" : "mod30_once";
        const SongReport report = ConvertSong(c.rom, info, 0, TempSettings(name), nullptr);
        SEQLIB_REQUIRE(report.ok && !report.silent);
        const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(ReadMidi(report.midi_path));
        SEQLIB_REQUIRE(notes.size() >= 2);
        SEQLIB_CHECK(notes[0].key == 60 && notes[1].key == 72);
        SEQLIB_CHECK_EQ(report.loop_end > 0, loop);
        const double seconds = 8.0 * kRowSamples / kRate;
        SEQLIB_CHECK(loop || std::fabs(report.seconds - seconds) < 0.01);
    }
}

// The orders after which a looping module's patterns repeat. A conversion's loop lasts that many orders. In a module
// whose orders all play one pattern, it lasts one order.
void TestRepeatOrders()
{
    const auto repeat_orders = [](const std::vector<uint8_t>& orders, bool loop)
    {
        TestModule m = OneRow(Note(49, 1), 4);
        m.patterns.push_back(m.patterns[0]);
        m.orders = orders;
        const Cart c = MakeOne(m);
        Mod30Settings s = SettingsOf(Mod30Group::kE2, c.bank);
        s.loop = loop;
        return Mod30Player(c.rom, c.module, s).RepeatOrders();
    };
    SEQLIB_CHECK_EQ(repeat_orders({0, 0, 0, 0}, true), 1u);
    SEQLIB_CHECK_EQ(repeat_orders({0, 1, 0, 1}, true), 2u);
    SEQLIB_CHECK_EQ(repeat_orders({0, 1, 1}, true), 3u);
    SEQLIB_CHECK_EQ(repeat_orders({0, 0, 0, 0}, false), 4u);

    TestModule m = OneRow(Note(49, 1), 4);
    m.orders = {0, 0, 0, 0};
    const Cart c = MakeOne(m);
    DriverInfo info;
    info.format = Format::kGbaMod30;
    info.mod30 = SettingsOf(Mod30Group::kE2, c.bank);
    info.music_volume = info.mod30.music;
    Song song;
    song.module = c.module;
    song.bank = c.bank;
    info.songs = {song};
    const SongReport report = ConvertSong(c.rom, info, 0, TempSettings("mod30_repeat"), nullptr);
    SEQLIB_REQUIRE(report.ok && !report.silent);
    SEQLIB_CHECK(std::fabs(report.loop_end - report.loop_start - 4.0 * kRowSamples / kRate) < 0.01);
}

// Detection: a game with GBAMOD30 modules that the table of games doesn't know gets an error, and a game without them
// none.
void TestDetection()
{
    const Cart c = MakeOne(OneRow(Note(48, 1)));
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(!DetectDriver(c.rom, info, error) && error.find("settings") != std::string::npos);

    CartImage empty(0x10000);
    DriverInfo none;
    SEQLIB_CHECK(!DetectDriver(empty.ToRom(), none, error) && error.empty());
}

int RunTests()
{
    TestReader();
    TestBank();
    TestTimingAndPitch();
    TestVolumeSlides();
    TestSpeedAndTempo();
    TestLoopFlag();
    TestMixers();
    TestConversion();
    TestRepeatOrders();
    TestDetection();

    return seqlib::testing::Summary("logikstate_mod30");
}

} // namespace
} // namespace ntdlib::sequence_export::logikstate

int main()
{
    return ntdlib::sequence_export::logikstate::RunTests();
}
