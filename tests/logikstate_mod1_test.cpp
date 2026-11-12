// SPDX-License-Identifier: MIT

// Unit tests for the GBAMOD1 build of Logik State's driver. They build small synthetic cartridges in memory. A test
// reads no game's ROM.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/mod1_player.h"
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

using ntdlib::logikstate::Channel;
using ntdlib::logikstate::DetectDriver;
using ntdlib::logikstate::Format;
using ntdlib::logikstate::kMod1MixChannels;
using ntdlib::logikstate::kMod1Size;
using ntdlib::logikstate::kNoPitch;
using ntdlib::logikstate::kNoteSlideScale;
using ntdlib::logikstate::kSamples;
using ntdlib::logikstate::Mod1Player;
using ntdlib::logikstate::ReadCell;
using ntdlib::logikstate::ReadModule;
using ntdlib::logikstate::SamplePoints;
using ntdlib::logikstate::Song;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::TempSettings;
using namespace ntdlib::testing::logikstate_cart;

// The SOUNDCNT_H of the GBAMOD1 build and of the GBAMOD2 builds. The first sends the mix to FIFO A alone, and the
// second to both FIFOs.
constexpr uint16_t kOneFifo = 0x8B0E;
constexpr uint16_t kTwoFifos = 0xFB0E;

// The pitch that plays a sample at its SoundFont sample's rate, on key 60.
constexpr uint16_t kRoot = 104;

// A GBAMOD1 module whose three samples have 100, 400 and 3000 points, full volume and no loop, with one pattern of 64
// empty rows.
TestModule Mod1Module()
{
    TestModule module;
    module.format = Format::kGbaMod1;
    module.patterns = {std::vector<TestRow>(64)};
    module.samples[0] = {100, 0, 64, 0, 0};
    module.samples[1] = {400, 0, 64, 0, 0};
    module.samples[2] = {3000, 0, 64, 0, 0};

    return module;
}

TestCell NoteCell(uint16_t pitch, uint16_t sample, uint8_t effect = 0, uint8_t operand = 0)
{
    return {pitch, sample, effect, operand};
}

TestCell EffectCell(uint8_t effect, uint8_t operand)
{
    return {kNoPitch, 0, effect, operand};
}

// A cartridge with the GBAMOD1 build's routines: the load routine with its signature, and the output's start with
// `sound_control`.
Cart Mod1Cart(uint16_t sound_control = kOneFifo)
{
    Cart cart({});
    cart.UseMod1();
    cart.AddOutput(sound_control);

    return cart;
}

Rom Mod1Rom(const TestModule& module)
{
    Cart cart = Mod1Cart();
    cart.AddModule(module);

    return cart.ToRom();
}

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, info, error);

    return info;
}

// A row or a tick that a player ran, with the channels after it.
struct Step
{
    bool row = false;
    int order = 0;
    int row_number = 0;
    std::array<Channel, kMod1MixChannels> channels;
};

// Plays `song` of `rom` for `frames` frames and returns its rows and ticks.
std::vector<Step> Play(const Rom& rom, const Song& song, int frames)
{
    std::vector<Step> steps;
    Mod1Player player(rom, song);
    const auto record = [&](bool row)
    {
        steps.push_back({row, player.RowOrder(), player.RowNumber(), player.Channels()});
    };
    player.OnStep(record);
    for (int f = 0; f < frames && player.Playing(); f++)
    {
        player.Frame();
    }

    return steps;
}

// Plays the first song of `rom`.
std::vector<Step> Play(const Rom& rom, int frames)
{
    const DriverInfo info = Detect(rom);
    return info.songs.empty() ? std::vector<Step>() : Play(rom, info.songs[0], frames);
}

std::vector<Step> Rows(const std::vector<Step>& steps)
{
    std::vector<Step> rows;
    std::copy_if(steps.begin(), steps.end(), std::back_inserter(rows), [](const Step& s) { return s.row; });

    return rows;
}

// Returns the order of each row that starts a pattern.
std::vector<int> PatternOrders(const std::vector<Step>& steps)
{
    std::vector<int> orders;
    for (const Step& s : steps)
    {
        if (s.row && s.row_number == 0)
        {
            orders.push_back(s.order);
        }
    }

    return orders;
}

// ReadModule() reads a GBAMOD1 header: 4 channels, the rate, the orders, the rows of each pattern in a byte, and the
// samples. The patterns follow the header, and the points follow the patterns. A patterns' size that doesn't match the
// rows is refused.
void TestReader()
{
    Cart cart = Mod1Cart();
    TestModule module = Mod1Module();
    module.orders = {1, 0};
    module.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(20)};
    module.patterns[1][3] = {TestCell(), NoteCell(208, 3, 0xC, 0x20)};
    module.samples[1] = {300, 13, 48, 16, 200};
    const uint32_t address = cart.AddModule(module);
    Cart bad = cart;
    bad.Put32(address + 8, 4);
    const Rom rom = cart.ToRom();

    ntdlib::logikstate::Module read, unread;
    const bool ok = ReadModule(rom, address, read);
    const ntdlib::logikstate::Cell cell = ReadCell(rom, read.patterns[1] + (3 * 4 + 1) * 4);
    const std::array<uint32_t, kSamples> points = SamplePoints(rom, read, {address, 0, 0});

    SEQLIB_REQUIRE(ok);
    SEQLIB_CHECK(read.format == Format::kGbaMod1 && read.own_samples);
    SEQLIB_CHECK(read.channels == 4 && read.rate == kRate && read.order_count == 2 && read.pattern_count == 2);
    SEQLIB_CHECK(read.orders[0] == 1 && read.rows[0] == 64 && read.rows[1] == 20);
    SEQLIB_CHECK_EQ(read.steps, address + 0x18);
    SEQLIB_CHECK_EQ(read.patterns[1], address + kMod1Size + 64 * 16);
    SEQLIB_CHECK_EQ(read.points, address + kMod1Size + 84 * 16);
    SEQLIB_CHECK(read.samples[1].length == 300 && read.samples[1].fine_tune == 13 && read.samples[1].volume == 48);
    SEQLIB_CHECK(cell.pitch == 208 && cell.sample == 3 && cell.effect == 0xC && cell.operand == 0x20);
    SEQLIB_CHECK(points[0] == read.points && points[1] == read.points + 100 && points[3] == 0);
    SEQLIB_CHECK(!ReadModule(bad.ToRom(), address, unread));
}

// Detection tells the format by the load routine's signature, and the FIFOs by the output's SOUNDCNT_H. A GBAMOD1
// module contains many pieces: a song starts at order 0 and after each order whose pattern has a jump, with no range of
// orders. The routine that switches the sound off is after the output's start, and a call from the game's code
// counts.
void TestDetection()
{
    Cart cart = Mod1Cart();
    TestModule pieces = Mod1Module();
    pieces.orders = {0, 1, 2, 2};
    pieces.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(64), std::vector<TestRow>(64)};
    pieces.patterns[0][10] = {{}, EffectCell(0xB, 0)};
    pieces.patterns[2][5] = {{}, {}, {}, EffectCell(0xB, 2)};
    const uint32_t first = cart.AddModule(pieces);
    const uint32_t second = cart.AddModule(Mod1Module());
    Cart sound_off = cart;
    sound_off.AddSoundOff(SoundOffCaller::kGame);
    Cart two = Mod1Cart(kTwoFifos);
    two.AddModule(Mod1Module());
    const Rom rom = cart.ToRom();

    DriverInfo info;
    std::string error;
    const bool found = DetectDriver(rom, info, error);
    const DriverInfo with_sound_off = Detect(sound_off.ToRom());

    SEQLIB_REQUIRE(found && error.empty());
    SEQLIB_CHECK(info.format == Format::kGbaMod1);
    SEQLIB_CHECK(info.load_routine == kLoad && info.play_routine == kPlay);
    SEQLIB_CHECK_EQ(info.fifos, 1);
    SEQLIB_CHECK_EQ(Detect(two.ToRom()).fifos, 2);
    SEQLIB_CHECK(info.warnings.empty() && info.sound_off_call == 0);
    SEQLIB_CHECK_EQ(with_sound_off.sound_off_call, kGameCode + 2);
    SEQLIB_REQUIRE(info.songs.size() == 4);
    const std::array<std::pair<uint32_t, int>, 4> expected = {{{first, 0}, {first, 1}, {first, 3}, {second, 0}}};
    for (size_t i = 0; i < expected.size(); i++)
    {
        const Song& s = info.songs[i];
        SEQLIB_CHECK(s.module == expected[i].first && s.order == expected[i].second && s.bank == 0);
        SEQLIB_CHECK(s.first_order == -1 && s.last_order == -1 && !s.plays_once);
        SEQLIB_CHECK_EQ(s.note_slide_scale, kNoteSlideScale);
    }
}

// Slides 1 and 2 add 4 times the operand to the step at each tick, up and down: they're linear in frequency. A slide
// to a note moves at the operand times the game's scale, and the note reaches the target's pitch where the
// step reaches the target's step.
void TestSlides()
{
    TestModule module = Mod1Module();
    module.patterns[0][0] = {NoteCell(kRoot, 3, 1, 3), NoteCell(kRoot, 3, 2, 3), NoteCell(kRoot, 3)};
    module.patterns[0][1] = {{}, {}, NoteCell(kRoot + 8, 0, 3, 1)};
    const Rom rom = Mod1Rom(module);
    Song slow = Detect(rom).songs[0];
    slow.note_slide_scale = 8;

    const std::vector<Step> steps = Play(rom, 20);
    const std::vector<Step> slow_steps = Play(rom, slow, 20);

    // The first row, its six ticks, then the second row and its ticks.
    SEQLIB_REQUIRE(steps.size() > 12 && slow_steps.size() > 12);
    SEQLIB_CHECK(steps[0].row && steps[7].row);
    for (int tick = 1; tick <= 3; tick++)
    {
        SEQLIB_CHECK_EQ(steps[size_t(tick)].channels[0].step, TestMod1Step(kRoot) + 12u * uint32_t(tick));
        SEQLIB_CHECK_EQ(steps[size_t(tick)].channels[1].step, TestMod1Step(kRoot) - 12u * uint32_t(tick));
    }

    // The scale 0x28 makes a speed of 40 a tick, and the slide of 96 to the next semitone reaches it at the third
    // tick. The scale 8 makes 8 a tick.
    const uint32_t gap = TestMod1Step(kRoot + 8) - TestMod1Step(kRoot);
    const Channel& first_tick = steps[8].channels[2];
    SEQLIB_REQUIRE(gap > 80 && gap <= 120);
    SEQLIB_CHECK(first_tick.slide_speed == 40 && first_tick.slide == 40 && first_tick.pitch == kRoot);
    SEQLIB_CHECK(steps[10].channels[2].pitch == kRoot + 8 && steps[10].channels[2].slide == 0);
    SEQLIB_CHECK_EQ(steps[10].channels[2].step, TestMod1Step(kRoot + 8));
    SEQLIB_CHECK(slow_steps[8].channels[2].slide_speed == 8 && slow_steps[10].channels[2].slide == 24);
}

// A vibrato moves the pitch by half of the wave times the depth over 256. An arpeggio keeps its intervals and plays
// nothing. Effects 5, 6 and E do nothing.
void TestVibratoAndSilentEffects()
{
    TestModule module = Mod1Module();
    module.samples[2].volume = 40;
    module.patterns[0][0] = {NoteCell(kRoot, 1, 4, 0x48), NoteCell(kRoot, 1, 0, 0x47), NoteCell(kRoot, 3, 6, 0x08),
                             NoteCell(kRoot, 3, 0xE, 0x60)};
    module.patterns[0][1] = {{}, {}, EffectCell(5, 0x20)};

    const std::vector<Step> steps = Play(Mod1Rom(module), 20);

    SEQLIB_REQUIRE(steps.size() > 13);
    const std::array<int16_t, 6> wave = {0, 3, 5, 7, 7, 7};
    for (size_t tick = 0; tick < 6; tick++)
    {
        const Step& s = steps[tick + 1];
        SEQLIB_CHECK_EQ(s.channels[0].vibrato, wave[tick]);
        SEQLIB_CHECK_EQ(s.channels[0].step, TestMod1Step(kRoot + uint32_t(wave[tick] / 2)));
        SEQLIB_CHECK(s.channels[1].arpeggio_1 == 4 && s.channels[1].arpeggio_2 == 7);
        SEQLIB_CHECK_EQ(s.channels[1].step, TestMod1Step(kRoot));
        SEQLIB_CHECK(s.channels[2].effect == -1 && s.channels[2].volume == 40);
        SEQLIB_CHECK(s.channels[3].effect == -1 && s.channels[3].volume == 40);
    }
    SEQLIB_CHECK(steps[12].channels[2].effect == -1 && steps[12].channels[2].volume == 40);
}

// A volume slide whose operand has a bit set in both digits keeps the tick's pointer at its channel: neither that
// channel nor the ones after it get a tick. A row's first tick runs no volume slide and keeps no pointer. Another
// operand slides up by its first digit, or down by its second.
void TestVolumeSlideHold()
{
    TestModule holding = Mod1Module();
    holding.samples[1].volume = 40;
    holding.patterns[0][0] = {NoteCell(kRoot, 2, 0xA, 0x11), NoteCell(kRoot, 2, 0xA, 0x20), NoteCell(kRoot, 2, 1, 1)};
    TestModule sliding = holding;
    sliding.patterns[0][0][0].operand = 0x12;

    const std::vector<Step> held = Play(Mod1Rom(holding), 10);
    const std::vector<Step> slid = Play(Mod1Rom(sliding), 10);

    SEQLIB_REQUIRE(held.size() > 6 && slid.size() > 6);
    SEQLIB_CHECK(held[6].channels[0].volume == 40 && held[6].channels[1].volume == 40);
    SEQLIB_CHECK_EQ(held[6].channels[2].slide, 4);
    SEQLIB_CHECK(slid[6].channels[0].volume == 45 && slid[6].channels[1].volume == 50);
    SEQLIB_CHECK_EQ(slid[6].channels[2].slide, 24);
}

// Without a range, the pattern's end moves the order on, and a jump to order n plays order n. With a range, an order
// above its last goes to its first at the pattern's end, and another order plays again: a jump to order n then plays
// order n - 1. A break moves to the next order with no wrap. Past the last order, the frame routine clears the
// channels at each frame.
void TestOrders()
{
    TestModule module = Mod1Module();
    module.orders = {0, 1, 2};
    module.patterns = {std::vector<TestRow>(64), std::vector<TestRow>(64), std::vector<TestRow>(64)};
    module.patterns[0][0] = {NoteCell(kRoot, 3)};
    module.patterns[0][1] = {EffectCell(0xB, 2)};
    module.patterns[2][0] = {NoteCell(kRoot, 3), NoteCell(kRoot, 3)};
    module.patterns[2][2] = {EffectCell(0xD, 0)};
    const Rom rom = Mod1Rom(module);
    const Song plain = Detect(rom).songs[0];
    Song back = plain, held = plain;
    back.order = 1;
    back.first_order = 0;
    back.last_order = 0;
    held.first_order = 0;
    held.last_order = 1;

    const std::vector<Step> plain_steps = Play(rom, plain, 40);
    const std::vector<Step> back_steps = Play(rom, back, 1400);
    const std::vector<Step> held_steps = Play(rom, held, 1400);

    SEQLIB_CHECK(PatternOrders(plain_steps) == std::vector<int>({0, 2, 3}));
    const std::vector<int> back_orders = PatternOrders(back_steps);
    SEQLIB_REQUIRE(back_orders.size() >= 3);
    SEQLIB_CHECK(back_orders[0] == 1 && back_orders[1] == 0 && back_orders[2] == 0);
    const std::vector<int> held_orders = PatternOrders(held_steps);
    SEQLIB_REQUIRE(held_orders.size() >= 3);
    SEQLIB_CHECK(held_orders[0] == 0 && held_orders[1] == 1 && held_orders[2] == 1);
    const auto order_two =
        std::find_if(plain_steps.begin(), plain_steps.end(), [](const Step& s) { return s.order == 2; });
    SEQLIB_REQUIRE(order_two != plain_steps.end());
    SEQLIB_CHECK(order_two->channels[1].points != 0);
    SEQLIB_CHECK(plain_steps.back().order == 3 && plain_steps.back().channels[1].points == 0);
}

// A sample without a note starts the sample's points from place 0 with the channel's length. A slide that brings the
// step below 0 plays the sample backwards. A sample whose loop's length is 0 stays in the mixer after its end, with
// the length 0.
void TestMixer()
{
    TestModule module = Mod1Module();
    module.patterns[0][0] = {NoteCell(kRoot, 3), NoteCell(8, 3, 2, 0xFF), NoteCell(296 - 8, 1)};
    module.patterns[0][2] = {{0x1FF, 2}};
    const Rom rom = Mod1Rom(module);

    const std::vector<Step> steps = Play(rom, 20);
    const std::vector<Step> rows = Rows(steps);
    Mod1Player player(rom, Detect(rom).songs[0]);
    for (int f = 0; f < 10; f++)
    {
        player.Frame();
    }

    SEQLIB_REQUIRE(rows.size() > 2 && steps.size() > 4);
    const Channel& changed = rows[2].channels[0];
    SEQLIB_CHECK(changed.position == 0 && changed.length == 3000 && changed.sample == 1);
    SEQLIB_CHECK_EQ(changed.points, SamplePoints(rom, player.Header(), Detect(rom).songs[0])[1]);
    SEQLIB_CHECK(int32_t(steps[1].channels[1].step) < 0);
    SEQLIB_CHECK(steps[2].channels[1].position < steps[1].channels[1].position);
    const Channel& held = player.Channels()[2];
    SEQLIB_CHECK(held.length == 0 && held.points != 0 && player.Sounding(held, 2));
}

// Returns the values of controller 11 on MIDI channel `channel`.
std::vector<int> Expressions(const ntdlib::testing::agb_files::MidiEvents& midi, int channel)
{
    std::vector<int> values;
    for (const seqlib::midi::Event& e : midi.Find(uint8_t(0xB0 + channel)))
    {
        if (e.bytes[1] == 11)
        {
            values.push_back(e.bytes[2]);
        }
    }

    return values;
}

// A GBAMOD1 song converts with pitch 104 on key 60. One FIFO plays at half the level of two, and a game that
// switches the sound off halves it again. A note whose step falls to 0 or below plays at level 0. A song past its
// module's last order ends there, without a loop.
void TestConversion()
{
    TestModule module = Mod1Module();
    module.patterns[0][0] = {NoteCell(kRoot, 2), NoteCell(8, 3, 2, 0xFF)};
    module.patterns[0][4] = {NoteCell(kRoot + 8, 2)};
    Cart cart = Mod1Cart();
    cart.AddModule(module);
    Cart sound_off = cart;
    sound_off.AddSoundOff(SoundOffCaller::kGame);
    const Rom rom = cart.ToRom();
    const Rom quiet_rom = sound_off.ToRom();

    const SongReport report = ConvertSong(rom, Detect(rom), 0, TempSettings("logikstate_mod1_conversion"), nullptr);
    const SongReport quiet =
        ConvertSong(quiet_rom, Detect(quiet_rom), 0, TempSettings("logikstate_mod1_quiet"), nullptr);
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(report.midi_path);
    const std::vector<seqlib::midi::Note> notes = seqlib::midi::Notes(midi);

    SEQLIB_REQUIRE(report.ok && quiet.ok && report.tracks == 2);
    SEQLIB_CHECK(report.loop_end < 0 && seqlib::midi::Markers(midi).empty());
    SEQLIB_REQUIRE(notes.size() == 3);
    SEQLIB_CHECK(notes[0].channel == 0 && notes[0].key == 60 && notes[0].on == 0);
    SEQLIB_CHECK(notes[1].channel == 0 && notes[1].key == 61);
    // Volume 64 of one FIFO is a level of 32 of 128, and of 16 at half volume: controller 11 at 64 and 45.
    const std::vector<int> levels = Expressions(midi, 0);
    const std::vector<int> quiet_levels = Expressions(ReadMidi(quiet.midi_path), 0);
    SEQLIB_CHECK(std::find(levels.begin(), levels.end(), 64) != levels.end());
    SEQLIB_CHECK(std::find(quiet_levels.begin(), quiet_levels.end(), 45) != quiet_levels.end());
    const std::vector<int> backwards = Expressions(midi, 1);
    SEQLIB_CHECK(!backwards.empty() && backwards.back() == 0);
}

// A song that plays once ends where its first row with a jump starts, with no loop. Without the flag, the jump to the
// piece's start loops.
void TestPlaysOnce()
{
    TestModule module = Mod1Module();
    module.patterns[0][0] = {NoteCell(kRoot, 3)};
    module.patterns[0][7] = {EffectCell(0xB, 0)};
    const Rom rom = Mod1Rom(module);
    DriverInfo info = Detect(rom);
    DriverInfo once = info;
    SEQLIB_REQUIRE(once.songs.size() == 1);
    once.songs[0].plays_once = true;

    const SongReport looping = InspectSong(rom, info, 0, TempSettings("logikstate_mod1_looping"));
    const SongReport jingle = ConvertSong(rom, once, 0, TempSettings("logikstate_mod1_once"), nullptr);
    const ntdlib::testing::agb_files::MidiEvents midi = ReadMidi(jingle.midi_path);

    SEQLIB_CHECK(looping.ok && looping.loop_end > 0);
    SEQLIB_REQUIRE(jingle.ok && !jingle.silent);
    SEQLIB_CHECK(jingle.loop_end < 0 && seqlib::midi::Markers(midi).empty());
    SEQLIB_CHECK(std::fabs(jingle.seconds - 7.0 * 2522 / kRate) < 0.002);
}

// The game's data names the pieces that Pinball Challenge Deluxe starts: bytes of a table's record that it plays as
// music or as jingles, and event entries of jingles. A piece that only jingles start plays once, and so does one that
// only an entry without a pointer names. A piece that music starts, or music and a jingle, loops, and so does one
// that nothing names. The record's jingle at +0x258 counts only where its first word is 0x23232323. A game without an
// entry in the table of games plays each piece as the driver does.
void TestPieces()
{
    TestModule module = Mod1Module();
    module.orders = {0, 1, 2, 3, 4, 5, 6, 7};
    module.patterns.clear();
    for (uint8_t p = 0; p < 8; p++)
    {
        module.patterns.push_back({{NoteCell(kRoot, 3)}, {EffectCell(0xB, p)}});
    }
    const std::vector<std::pair<uint32_t, uint8_t>> bytes = {{0x224, 0}, {0x228, 1}, {0x22C, 1}, {0x230, 1},
                                                             {0x240, 1}, {0x234, 2}, {0x238, 2}, {0x23C, 2},
                                                             {0x244, 2}, {0x248, 2}, {0x258, 6}};
    const std::vector<TestEntry> entries = {{3, true, true}, {4, true, false}, {1, true, true}, {9, false, true}};
    Cart cart = Mod1Cart();
    const uint32_t address = cart.AddModule(module);
    Cart unknown = cart;
    unknown.AddTable(address, bytes, entries);
    cart.SetGameCode("APLP");
    Cart marked = cart;
    cart.AddTable(address, bytes, entries);
    marked.AddTable(address, bytes, entries, 0x23232323);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);
    const DriverInfo marked_info = Detect(marked.ToRom());
    const DriverInfo unknown_info = Detect(unknown.ToRom());
    const SongReport music = InspectSong(rom, info, 1, TempSettings("logikstate_mod1_music"));
    const SongReport jingle = InspectSong(rom, info, 2, TempSettings("logikstate_mod1_jingle"));

    SEQLIB_REQUIRE(info.songs.size() == 8 && marked_info.songs.size() == 8 && unknown_info.songs.size() == 8);
    const std::array<bool, 8> once = {false, false, true, true, true, false, false, false};
    for (size_t s = 0; s < once.size(); s++)
    {
        SEQLIB_CHECK_EQ(info.songs[s].order, int(s));
        SEQLIB_CHECK_EQ(info.songs[s].plays_once, once[s]);
        SEQLIB_CHECK_EQ(marked_info.songs[s].plays_once, once[s] || s == 6);
        SEQLIB_CHECK(!unknown_info.songs[s].plays_once);
        SEQLIB_CHECK(info.songs[s].first_order == -1 && info.songs[s].last_order == -1);
    }
    SEQLIB_CHECK(music.ok && music.loop_end > 0);
    SEQLIB_CHECK(jingle.ok && jingle.loop_end < 0);
    const auto says = [](const DriverInfo& i, const std::string& text)
    {
        const auto has = [&](const std::string& line)
        {
            return line.find(text) != std::string::npos;
        };
        return std::any_of(i.log.begin(), i.log.end(), has);
    };
    SEQLIB_CHECK(
        says(info, "2 pieces loop as music, 2 play once as jingles, 1 play once as jingles that nothing starts"));
    SEQLIB_CHECK(says(unknown_info, "no table of the game's pieces"));
}

int RunTests()
{
    TestReader();
    TestDetection();
    TestSlides();
    TestVibratoAndSilentEffects();
    TestVolumeSlideHold();
    TestOrders();
    TestMixer();
    TestConversion();
    TestPlaysOnce();
    TestPieces();

    return seqlib::testing::Summary("logikstate_mod1");
}

} // namespace
} // namespace ntdlib::sequence_export::logikstate

int main()
{
    return ntdlib::sequence_export::logikstate::RunTests();
}
