// SPDX-License-Identifier: MIT

// The model of JAudio1 for Pikmin 2 (jaudio1), on tables and files built for the test: an oscillator's test for a
// table that repeats a part for ever (Oscillator::Cycling), the engine's snapshots, and the wave archives that a
// sequence can play (SoundFiles::WaveArchivesOf). At a DSP rate of 48,000 Hz, a
// point's time counts in steps of the oscillator.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "jaudio_engine_checks.h"
#include "ntdlib/jaudio/rarc.h"
#include "ntdlib/jaudio1/engine.h"
#include "ntdlib/jaudio1/oscillator.h"
#include "ntdlib/testing/jaudio_files.h"

using namespace xsflib::testing;

namespace
{

namespace j1 = ntdlib::jaudio1;
namespace checks = ntdlib::testing::jaudio_engine_checks;

using checks::Run;

constexpr float kDacRate = 48000.0f;

constexpr int16_t kLinear = 0;
using j1::kPointHold;
using j1::kPointJump;
using j1::kPointStop;

// The cycles of the oscillator's state from its start on: whether each step finds it in the repeated part.
template <std::size_t N>
std::string Cycles(const std::array<j1::OscPoint, N>& points, int steps)
{
    j1::OscData data;
    data.attack = {points.data(), points.size()};
    j1::Oscillator oscillator;
    oscillator.Init();
    oscillator.InitStart(&data, kDacRate);

    std::string cycles;
    for (int i = 0; i < steps; i++)
    {
        cycles += oscillator.Cycling() ? 'c' : '-';
        oscillator.IncCounter(kDacRate);
    }

    return cycles;
}

void TestTimeScale()
{
    XSFLIB_CHECK(j1::Oscillator::TimeScale(kDacRate) == 1.0f);
}

// A table like a vibrato's: 2 steps up, then 3 steps down and 3 steps up for ever. The oscillator reads point 1, the
// jump's target, after the first 2 steps.
void TestCycle()
{
    const std::array<j1::OscPoint, 4> kTable = {{
        {kLinear, 2, 0x7fff},
        {kLinear, 3, 0},
        {kLinear, 3, 0x7fff},
        {kPointJump, 0, 1},
    }};

    XSFLIB_CHECK(Cycles(kTable, 40) == "--" + std::string(38, 'c'));

    // A jump to the first point repeats the whole table from the start.
    const std::array<j1::OscPoint, 3> kWhole = {{
        {kLinear, 2, 0x7fff},
        {kLinear, 2, 0},
        {kPointJump, 0, 0},
    }};

    XSFLIB_CHECK(Cycles(kWhole, 20) == std::string(20, 'c'));
}

// A table that ends in a hold or a stop, or jumps on to a later point, repeats nothing.
void TestNoCycle()
{
    const std::array<j1::OscPoint, 2> kHoldTable = {{{kLinear, 2, 0x7fff}, {kPointHold, 0, 0}}};
    XSFLIB_CHECK(Cycles(kHoldTable, 10) == std::string(10, '-'));

    const std::array<j1::OscPoint, 2> kStopTable = {{{kLinear, 2, 0x7fff}, {kPointStop, 0, 0}}};
    XSFLIB_CHECK(Cycles(kStopTable, 10) == std::string(10, '-'));

    const std::array<j1::OscPoint, 4> kForward = {{
        {kLinear, 2, 0x7fff},
        {kPointJump, 0, 2},
        {kLinear, 2, 0},
        {kPointHold, 0, 0},
    }};

    XSFLIB_CHECK(Cycles(kForward, 10) == std::string(10, '-'));

    // A hold before the jump ends the table there.
    const std::array<j1::OscPoint, 4> kHeld = {{
        {kLinear, 2, 0x7fff},
        {kPointHold, 0, 0},
        {kLinear, 2, 0},
        {kPointJump, 0, 0},
    }};

    XSFLIB_CHECK(Cycles(kHeld, 10) == std::string(10, '-'));

    // A table that runs out of points reads a stop.
    const std::array<j1::OscPoint, 1> kUnended = {{{kLinear, 2, 0x7fff}}};
    XSFLIB_CHECK(Cycles(kUnended, 10) == std::string(10, '-'));
}

// An oscillator without settings, and one in its release, repeat nothing.
void TestOtherStates()
{
    j1::Oscillator none;
    none.Init();
    none.InitStart(nullptr, kDacRate);

    XSFLIB_CHECK(!none.Cycling());

    const std::array<j1::OscPoint, 3> kTable = {{
        {kLinear, 2, 0x7fff},
        {kLinear, 2, 0},
        {kPointJump, 0, 0},
    }};
    const std::array<j1::OscPoint, 2> kReleaseTable = {{{kLinear, 4, 0}, {kPointStop, 0, 0}}};

    j1::OscData data;
    data.attack = {kTable.data(), kTable.size()};
    data.release = {kReleaseTable.data(), kReleaseTable.size()};
    j1::Oscillator oscillator;
    oscillator.Init();
    oscillator.InitStart(&data, kDacRate);
    XSFLIB_CHECK(oscillator.Cycling());

    oscillator.Release(kDacRate);

    XSFLIB_CHECK(oscillator.State().state == j1::Oscillator::kRelease);
    XSFLIB_CHECK(!oscillator.Cycling());
}

namespace files = ntdlib::testing::jaudio_files;

// A sequence of three tracks. The root opens two children and waits for ever. Child 0 plays key 60 on note 1 for 24
// ticks of every 48. Child 1 takes outer parameters (EXTSet), fades its volume to half over 96 ticks and plays notes
// of 48 ticks on key 67, one every 48 ticks. Each new note cuts the last one's release.
std::vector<uint8_t> SnapshotSequence()
{
    files::Bytes s;
    s.U8(0xc1).U8(0).U24(0x30); // open track 0
    s.U8(0xc1).U8(1).U24(0x50); // open track 1
    s.U8(0xfe).U16(48);         // a timebase of 48
    s.U8(0xfd).U16(120);        // a tempo of 120
    const auto root_loop = static_cast<uint32_t>(s.Size());
    s.U8(0x80).U8(96).U8(0xc8).U8(0).U24(root_loop);
    s.PadTo(0x30);

    s.U8(0xa4).U8(6).U8(0); // bank 0, program 0
    s.U8(60).U8(1).U8(100); // key 60 on note 1
    s.U8(0x80).U8(24).U8(0x81).U8(0x80).U8(24);
    s.U8(0xc8).U8(0).U24(0x33);
    s.PadTo(0x50);

    s.U8(0xa4).U8(6).U8(0);                  // bank 0, program 0
    s.U8(0xee).U24(0x100);                   // the outer parameters of the sequence's place 0x100
    s.U8(0x9a).U8(0).U8(0x40).U8(96);        // the volume to half over 96 ticks
    s.U8(67).U8(0x08).U8(80).U8(100).U8(48); // key 67, a note of 48 ticks that the track waits for
    s.U8(0xc8).U8(0).U24(0x5b);

    return s.Data();
}

// A snapshot taken in the middle of a piece, with notes sounding and a level moving, and restored after more
// sub-frames, gives from there the output of a run without the restore, sample for sample. A snapshot of another engine
// or of an earlier start can't be restored.
void TestSnapshots()
{
    const std::vector<uint8_t> aaf =
        files::AafFile({{.category = 16, .file = 7}}, {"test.aw"}, 5, {}, false, files::BankV0File(0, 60, false));
    const std::vector<uint8_t> sequences =
        files::RarcFile({{7, "field.bms", SnapshotSequence()}}, false, ntdlib::jaudio::kWideRarcEntrySize);
    const std::vector<uint8_t> aw = files::AwFile();
    const j1::SoundFiles game(aaf, sequences,
                              [&aw](const std::string& name) { return name == "test.aw" ? &aw : nullptr; });

    constexpr int kBefore = 157;
    constexpr int kAfter = 900;
    const auto start_sound = [](j1::Engine& engine)
    {
        return engine.Start(7, {});
    };
    const auto [head, tail] = checks::CheckSnapshots<j1::Engine>(game, start_sound, kBefore, kAfter);

    // A snapshot from before the first sub-frame holds no outer parameters of EXTSet. Restoring it, running past the
    // EXTSet again and then restoring the later snapshot gives the same output as the straight run. A restore that
    // dropped the node of the later snapshot's track left that track pointing at freed memory.
    j1::Engine seeking(game, {});
    XSFLIB_CHECK(!seeking.Start(7, {}));
    const j1::Engine::Snapshot start = seeking.Save();
    XSFLIB_CHECK(Run(seeking, kBefore) == head);
    const j1::Engine::Snapshot middle = seeking.Save();
    XSFLIB_CHECK(Run(seeking, kAfter) == tail);
    seeking.Restore(start);
    XSFLIB_CHECK(Run(seeking, kBefore) == head);
    XSFLIB_CHECK(Run(seeking, kAfter) == tail);
    seeking.Restore(middle);
    XSFLIB_CHECK(Run(seeking, kAfter) == tail);
}

// The cursors of the tracks (Cursor), two frames into the snapshot sequence: each track waits after its first wait
// command. The root waits at 0x12, after its four commands and the wait. Track 0 waits at 0x38. Track 1 waits at 0x60,
// after a note that it waits for.
void TestCursor()
{
    const std::vector<uint8_t> aaf =
        files::AafFile({{.category = 16, .file = 7}}, {"test.aw"}, 5, {}, false, files::BankV0File(0, 60, false));
    const std::vector<uint8_t> sequences =
        files::RarcFile({{7, "field.bms", SnapshotSequence()}}, false, ntdlib::jaudio::kWideRarcEntrySize);
    const std::vector<uint8_t> aw = files::AwFile();
    const j1::SoundFiles game(aaf, sequences,
                              [&aw](const std::string& name) { return name == "test.aw" ? &aw : nullptr; });

    j1::Engine engine(game, {});
    XSFLIB_CHECK(engine.Cursor(0) == 0);
    XSFLIB_CHECK(!engine.Start(7, {}));
    Run(engine, 2 * j1::Engine::kSubframesPerFrame);
    const std::vector<uint32_t> order = engine.TreeOrder();
    XSFLIB_CHECK(order.size() == 3);
    if (order.size() == 3)
    {
        XSFLIB_CHECK(engine.Cursor(order[0]) == 0x12);
        XSFLIB_CHECK(engine.Cursor(order[1]) == 0x38);
        XSFLIB_CHECK(engine.Cursor(order[2]) == 0x60);
    }
}

using Names = std::vector<std::string>;

// A JAudio1 sequence that stores `bank_program` in register 6 (0xac's 16-bit operand), or any number for 0xffff, then
// plays a note.
std::vector<uint8_t> NoteOn(uint16_t bank_program)
{
    files::Bytes s;
    if (bank_program == 0xffff)
    {
        s.U8(0xa0).U8(6).U8(3);
    }
    else
    {
        s.U8(0xac).U8(6).U16(bank_program);
    }
    s.U8(60).U8(1).U8(100).U8(0xff);

    return s.Data();
}

// A bank's waves (ProgramWaves), and the archives that each file of a sequence archive can play
// (SoundFiles::WaveArchivesOf). Two wave systems each have an archive of one wave: a.aw for system 0 and b.aw for
// system 1. The bank at place 0 has number 0 and plays system 0. Number 1 names the last bank that gives it: place 1
// plays system 1, place 3 system 0. The bank at place 2 has number 2 and plays system 1. Each bank's program 0 plays
// wave 0, and its drum set 0xe4 wave 1. No system has wave 1. A number that no bank gives names no bank.
void TestWaveArchives()
{
    const std::vector<uint8_t> aaf = files::AafFile({{.category = 16, .file = 1}}, {"a.aw", "b.aw"}, 5, {}, false,
                                                    files::BankV0File(0, 60, false), {}, 0,
                                                    {{1, files::BankV0File(1, 60, false)},
                                                     {1, files::BankV0File(2, 60, false)},
                                                     {0, files::BankV0File(1, 60, false)}});
    const std::vector<uint8_t> sequences = files::RarcFile({{1, "1.bms", NoteOn(0x0000)},
                                                            {2, "2.bms", NoteOn(0x0100)},
                                                            {3, "3.bms", NoteOn(0x0200)},
                                                            {4, "4.bms", NoteOn(0xffff)},
                                                            {5, "5.bms", {0xdf, 0x00, 0x00, 0x00, 0x10, 0xff}},
                                                            {6, "6.bms", NoteOn(0x0005)},
                                                            {7, "7.bms", NoteOn(0x0700)},
                                                            {8, "8.bms", NoteOn(0x00e4)}},
                                                           false, ntdlib::jaudio::kWideRarcEntrySize);
    const std::vector<uint8_t> aw = files::AwFile();
    const j1::SoundFiles game(aaf, sequences, [&aw](const std::string&) { return &aw; });

    XSFLIB_REQUIRE(game.Banks().size() == 4);
    XSFLIB_CHECK((game.Banks()[0]->ProgramWaves(0) == std::vector<uint32_t>{0}));
    XSFLIB_CHECK((game.Banks()[0]->ProgramWaves(0xe4) == std::vector<uint32_t>{1}));
    XSFLIB_CHECK(game.Banks()[0]->ProgramWaves(5).empty() && game.Banks()[0]->ProgramWaves(0x100).empty());
    XSFLIB_CHECK(game.Banks()[0]->WaveSystemNumber() == 0 && game.Banks()[1]->WaveSystemNumber() == 1);

    XSFLIB_CHECK(game.WaveArchivesOf(1) == (Names{"a.aw"}));
    XSFLIB_CHECK(game.WaveArchivesOf(2) == (Names{"a.aw"}));
    XSFLIB_CHECK(game.WaveArchivesOf(3) == (Names{"b.aw"}));
    XSFLIB_CHECK(game.WaveArchivesOf(4) == (Names{"a.aw", "b.aw"}));
    XSFLIB_CHECK(game.WaveArchivesOf(5) == (Names{"a.aw", "b.aw"}));
    XSFLIB_CHECK(game.WaveArchivesOf(6).empty());
    XSFLIB_CHECK(game.WaveArchivesOf(7).empty());
    XSFLIB_CHECK(game.WaveArchivesOf(8).empty());
    XSFLIB_CHECK(game.WaveArchivesOf(9).empty());
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestTimeScale();
        TestCycle();
        TestNoCycle();
        TestOtherStates();
        TestSnapshots();
        TestCursor();
        TestWaveArchives();
    };

    return RunTest("jaudio1", tests);
}
