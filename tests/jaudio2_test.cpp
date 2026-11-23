// SPDX-License-Identifier: MIT

// The model of JAudio2 (jaudio2) on files built for the test: the load of a game's wave archives into the DSP's memory
// (LoadWaveSystems), the engine's snapshots, and the wave archives that a sound can play (WaveArchivesOf).

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "jaudio_engine_checks.h"
#include "ntdlib/jaudio/sequence_walk.h"
#include "ntdlib/jaudio/wave_system.h"
#include "ntdlib/jaudio2/engine.h"
#include "ntdlib/testing/jaudio_files.h"

using namespace xsflib::testing;

namespace
{

namespace nj = ntdlib::jaudio;
namespace j2 = ntdlib::jaudio2;
namespace checks = ntdlib::testing::jaudio_engine_checks;

using checks::Run;

// Two systems. System 1 lists a.aw and b.aw, and system 2 lists b.aw again and c.aw. The test doesn't give c.aw. Wave 3
// of system 1 is at 8 in b.aw.
std::vector<nj::WaveSystem> Systems()
{
    nj::WaveSystem first{.number = 1};
    first.groups = {{.archive = "a.aw"}, {.archive = "b.aw"}};
    first.waves[3] = {.offset = 8, .archive = "b.aw"};
    nj::WaveSystem second{.number = 2};
    second.groups = {{.archive = "b.aw"}, {.archive = "c.aw"}};

    return {first, second};
}

// The archives go from the base on, each once, in the order of their names and on 32-byte boundaries. The default base
// is SoundFiles::kWaves. A wave's address in its bank follows its archive. A missing archive is named and gets no
// space.
void TestLoadWaveSystems()
{
    const std::map<std::string, std::vector<uint8_t>> files = {{"a.aw", std::vector<uint8_t>(40, 0xaa)},
                                                               {"b.aw", std::vector<uint8_t>(16, 0xbb)}};
    const j2::SoundFiles::WaveArchives waves = [&files](const std::string& name) -> const std::vector<uint8_t>*
    {
        const auto file = files.find(name);

        return file != files.end() ? &file->second : nullptr;
    };

    const j2::LoadedWaves at_base = j2::LoadWaveSystems(Systems(), waves, 0x8000);
    const j2::LoadedWaves at_default = j2::LoadWaveSystems(Systems(), waves);

    Check(at_base.memory->Size() == 0x8060 && at_base.memory->Byte(0x7fff) == 0 &&
              at_base.memory->Byte(0x8000) == 0xaa && at_base.memory->Byte(0x8027) == 0xaa &&
              at_base.memory->Byte(0x8028) == 0 && at_base.memory->Byte(0x8040) == 0xbb &&
              at_base.memory->Byte(0x804f) == 0xbb,
          "the archives from the base on");
    const j2::BankWave* wave = at_base.banks.at(1).Find(3);
    Check(wave != nullptr && wave->address == 0x8048, "a wave's address follows its archive");
    Check(at_base.missing == std::vector<std::string>{"c.aw"}, "the missing archive");
    const j2::BankWave* default_wave = at_default.banks.at(1).Find(3);
    Check(at_default.memory->Size() == j2::SoundFiles::kWaves + 0x60 &&
              at_default.memory->Byte(j2::SoundFiles::kWaves) == 0xaa && default_wave != nullptr &&
              default_wave->address == j2::SoundFiles::kWaves + 0x48,
          "the default base");
}

namespace files = ntdlib::testing::jaudio_files;

// A sequence of three tracks. The root opens two children and waits for ever. Child 0 plays key 60 on note 1 for 24
// ticks of every 48. Child 1 fades its volume over 96 ticks and plays gate notes of 48 ticks on key 55, one every 48
// ticks. Each new note cuts the last one's release. The bank (BankV1File) plays its looping wave 0 for both.
std::vector<uint8_t> SnapshotSequence()
{
    files::Bytes s;
    s.U8(0xc1).U8(0).U24(0x30);  // open track 0
    s.U8(0xc1).U8(1).U24(0x50);  // open track 1
    s.U8(0xd8).U8(0x62).U16(48); // a timebase of 48
    s.U8(0xe0).U16(120);         // a tempo of 120
    const auto root_loop = static_cast<uint32_t>(s.Size());
    s.U8(0xf1).U8(96).U8(0xc7).U24(root_loop);
    s.PadTo(0x30);

    s.U8(0xe1).U16(0x0000); // bank 0, program 0
    s.U8(60).U8(1).U8(60);  // key 60 on note 1
    s.U8(0xf1).U8(24).U8(0x81).U8(0xf1).U8(24);
    s.U8(0xc7).U24(0x33);
    s.PadTo(0x50);

    s.U8(0xe2).U8(0).U8(0xe3).U8(0);   // bank 0, program 0
    s.U8(0xba).U8(0).U8(0x40).U16(96); // the volume to half over 96 ticks
    s.U8(55).U8(0).U8(50).U8(48);      // key 55, a gate note of 48 ticks
    s.U8(0xf1).U8(48).U8(0xc7).U24(0x59);

    return s.Data();
}

// A snapshot taken in the middle of the sound, with notes sounding and a level moving, and restored after more
// sub-frames, gives from there the output of a run without the restore, sample for sample. A snapshot of another engine
// or of an earlier start can't be restored.
void TestSnapshots()
{
    const files::SoundTableBlocks sounds = files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<uint8_t> startup = files::StartUpArchive(files::BankV1File(0, 10, 20000), {}, sounds);
    const std::vector<uint8_t> sequences = files::RarcFile({{7, "field.bms", SnapshotSequence()}});
    const std::vector<uint8_t> aw = files::AwFile();
    const j2::SoundFiles game(startup, sequences,
                              [&aw](const std::string& name) { return name == "test.aw" ? &aw : nullptr; });
    const uint32_t id = game.Archive().Sounds().at(0).Id();

    const auto start = [id](j2::Engine& engine)
    {
        return engine.Start(id);
    };
    checks::CheckSnapshots<j2::Engine>(game, start, 157, 900);
}

// The cursors of the tracks (Cursor), two frames into the snapshot sequence. The root waits at 0x13, after its four
// commands and the wait. Track 0 waits at 0x38, after a wait. Track 1 waits at 0x5d, after a gate note: a gate note's
// time is a wait.
void TestCursor()
{
    const files::SoundTableBlocks sounds = files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<uint8_t> startup = files::StartUpArchive(files::BankV1File(0, 10, 20000), {}, sounds);
    const std::vector<uint8_t> sequences = files::RarcFile({{7, "field.bms", SnapshotSequence()}});
    const std::vector<uint8_t> aw = files::AwFile();
    const j2::SoundFiles game(startup, sequences,
                              [&aw](const std::string& name) { return name == "test.aw" ? &aw : nullptr; });

    j2::Engine engine(game, {});
    XSFLIB_CHECK(engine.Cursor(0) == 0);
    XSFLIB_CHECK(!engine.Start(game.Archive().Sounds().at(0).Id()));
    Run(engine, 2 * j2::Engine::kSubframesPerFrame);
    const std::vector<uint32_t> order = engine.TreeOrder();
    XSFLIB_CHECK(order.size() == 3);
    if (order.size() == 3)
    {
        XSFLIB_CHECK(engine.Cursor(order[0]) == 0x13);
        XSFLIB_CHECK(engine.Cursor(order[1]) == 0x38);
        XSFLIB_CHECK(engine.Cursor(order[2]) == 0x5d);
    }
}

using Names = std::vector<std::string>;
constexpr uint16_t kAny = nj::InstrumentUse::kAny;

// The archives of waves (WaveArchivesOf): a wave takes the archive that WaveSystem::waves gives its id in each system
// of its number. Wave 3 of system 1 is in both of the system's groups. The system's reader keeps the last group's
// record, b.aw, and a.aw is left out. A second system of number 1 gives wave 3 in d.aw too. A wave that no
// system gives adds nothing. AllWaveArchives gives every group's archive.
void TestWaveArchivesOfWaves()
{
    nj::WaveSystem first{.number = 1};
    first.groups = {{.archive = "a.aw"}, {.archive = "b.aw"}};
    first.waves[3] = {.offset = 8, .archive = "b.aw"};
    first.waves[4] = {.offset = 0, .archive = "a.aw"};
    nj::WaveSystem second{.number = 2};
    second.groups = {{.archive = "c.aw"}};
    second.waves[3] = {.archive = "c.aw"};
    nj::WaveSystem again{.number = 1};
    again.groups = {{.archive = "d.aw"}};
    again.waves[3] = {.archive = "d.aw"};
    const std::vector<nj::WaveSystem> systems = {first, second, again};

    XSFLIB_CHECK(j2::WaveArchivesOf({first, second}, {{1, 3}}) == (Names{"b.aw"}));
    XSFLIB_CHECK(j2::WaveArchivesOf(systems, {{1, 3}}) == (Names{"b.aw", "d.aw"}));
    XSFLIB_CHECK(j2::WaveArchivesOf(systems, {{1, 4}, {2, 3}}) == (Names{"a.aw", "c.aw"}));
    XSFLIB_CHECK(j2::WaveArchivesOf(systems, {{1, 5}, {3, 3}}).empty());
    XSFLIB_CHECK(j2::AllWaveArchives(systems) == (Names{"a.aw", "b.aw", "c.aw", "d.aw"}));
}

// A bank for WaveArchivesOfWalk: its wave system and the waves of its programs.
struct TestBank
{
    uint32_t system = 0;
    std::map<uint32_t, std::vector<uint32_t>> programs;

    uint32_t WaveSystemNumber() const
    {
        return system;
    }

    std::vector<uint32_t> ProgramWaves(uint32_t program) const
    {
        const auto waves = programs.find(program);

        return waves != programs.end() ? waves->second : std::vector<uint32_t>();
    }
};

// The archives of a walk's uses (WaveArchivesOfWalk): a use takes the bank that its number names, or every bank for
// kAny, and its program's waves, or every program's below 0xf0 for kAny. An incomplete walk takes every archive.
void TestWaveArchivesOfWalk()
{
    nj::WaveSystem first{.number = 1};
    first.groups = {{.archive = "a.aw"}, {.archive = "b.aw"}};
    first.waves[3] = {.archive = "b.aw"};
    first.waves[4] = {.archive = "a.aw"};
    nj::WaveSystem second{.number = 2};
    second.groups = {{.archive = "c.aw"}, {.archive = "e.aw"}};
    second.waves[3] = {.archive = "c.aw"};
    second.waves[0xf0] = {.archive = "e.aw"};
    const std::vector<nj::WaveSystem> systems = {first, second};

    const TestBank zero{.system = 1, .programs = {{0, {3}}, {1, {4}}}};
    const TestBank seven{.system = 2, .programs = {{0, {3}}, {0xf0, {0xf0}}}};
    std::array<const TestBank*, 256> banks{};
    banks[0] = &zero;
    banks[7] = &seven;

    const auto archives = [&](std::set<nj::InstrumentUse> uses, bool complete = true)
    {
        nj::SequenceInstruments walk;
        walk.complete = complete;
        walk.uses = std::move(uses);

        return j2::WaveArchivesOfWalk(walk, banks, systems);
    };

    XSFLIB_CHECK(archives({{0, 0}}) == (Names{"b.aw"}));
    XSFLIB_CHECK(archives({{7, 0}}) == (Names{"c.aw"}));
    XSFLIB_CHECK(archives({{kAny, 0}}) == (Names{"b.aw", "c.aw"}));
    XSFLIB_CHECK(archives({{0, kAny}}) == (Names{"a.aw", "b.aw"}));
    XSFLIB_CHECK(archives({{7, kAny}}) == (Names{"c.aw"}));
    XSFLIB_CHECK(archives({{5, 0}, {0, 2}}).empty());
    XSFLIB_CHECK(archives({}).empty());
    XSFLIB_CHECK(archives({}, false) == (Names{"a.aw", "b.aw", "c.aw", "e.aw"}));
}

// A JAudio2 sequence that sets the bank and the program, then plays a note. A bank of 0xff takes any number.
std::vector<uint8_t> NoteOn(uint8_t bank, uint8_t program)
{
    files::Bytes s;
    if (bank == 0xff)
    {
        s.U8(0x90).U8(0x80).U8(0xe2).U8(0x03);
    }
    else
    {
        s.U8(0xe2).U8(bank);
    }
    s.U8(0xe3).U8(program).U8(60).U8(1).U8(60).U8(0xff);

    return s.Data();
}

// A bank's waves (ProgramWaves), and the archives that each sound of a start-up archive with two wave systems can play
// (SoundFiles::WaveArchivesOf). Each system has an archive of two waves: a.aw for system 0 and b.aw for system 1. Bank
// 0 plays system 0. Two banks give number 1: the one of system 1, then one of system 0. The engine takes the second.
// Bank 2 plays system 1. Program 0 of each plays waves 0 and 1.
void TestSoundArchives()
{
    std::vector<files::SoundSpec> table;
    for (const uint16_t file : {1, 2, 3, 4, 5, 6, 7, 9})
    {
        table.push_back({.type = 0x11, .file = file, .name = "SOUND_" + std::to_string(file)});
    }
    const std::vector<uint8_t> startup =
        files::StartUpArchive({{0, files::WaveSystemFile(0, 2, "a.aw")}, {1, files::WaveSystemFile(1, 2, "b.aw")}},
                              {{0, files::BankV1File(0, 10, 100)},
                               {1, files::BankV1File(1, 10, 100)},
                               {1, files::BankV1File(2, 10, 100)},
                               {0, files::BankV1File(1, 10, 100)}},
                              {}, files::SoundTable({{"SE", {{"TEST", table}}}}));
    const std::vector<uint8_t> sequences = files::RarcFile({{1, "1.bms", NoteOn(0, 0)},
                                                            {2, "2.bms", NoteOn(1, 0)},
                                                            {3, "3.bms", NoteOn(2, 0)},
                                                            {4, "4.bms", NoteOn(0xff, 0)},
                                                            {5, "5.bms", {0xc9, 0x00, 0x00, 0x00, 0x10, 0xff}},
                                                            {6, "6.bms", NoteOn(0, 5)},
                                                            {7, "7.bms", NoteOn(7, 0)}});
    const std::vector<uint8_t> aw = files::AwFile();
    const j2::SoundFiles game(startup, sequences, [&aw](const std::string&) { return &aw; });

    XSFLIB_REQUIRE(game.Banks().size() == 4);
    XSFLIB_CHECK((game.Banks()[0]->ProgramWaves(0) == std::vector<uint32_t>{0, 1}));
    XSFLIB_CHECK(game.Banks()[0]->ProgramWaves(1).empty() && game.Banks()[0]->ProgramWaves(0x1000).empty());
    XSFLIB_CHECK(game.Banks()[0]->WaveSystemNumber() == 0 && game.Banks()[1]->WaveSystemNumber() == 1);

    const std::vector<nj::SoundEntry>& sounds = game.Archive().Sounds();
    XSFLIB_REQUIRE(sounds.size() == 8);
    const std::array<Names, 8> expected = {Names{"a.aw"},         Names{"a.aw"}, Names{"b.aw"}, Names{"a.aw", "b.aw"},
                                           Names{"a.aw", "b.aw"}, Names{},       Names{},       Names{}};
    for (std::size_t i = 0; i < sounds.size(); i++)
    {
        Check(game.WaveArchivesOf(sounds[i]) == expected[i], sounds[i].name + "'s archives");
    }
}

} // namespace

int main()
{
    const auto tests = []
    {
        TestLoadWaveSystems();
        TestSnapshots();
        TestCursor();
        TestWaveArchivesOfWaves();
        TestWaveArchivesOfWalk();
        TestSoundArchives();
    };

    return RunTest("jaudio2", tests);
}
