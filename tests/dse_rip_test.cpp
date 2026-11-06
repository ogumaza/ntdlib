// SPDX-License-Identifier: MIT

// Tests of the parts of a rip tool for DSE's songs and sound effects (ntdlib/dse/rip.h), on a game's files that
// dse_song_files.h builds: three songs and two of their banks, the main bank, a folder of jingles, and a bank of sound
// effects. The jingle me0000 plays the folder's bank me.swd, and the jingle me0001 its bank of that name.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/player/archive_source.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/archive_source.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/rip.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace dse = ntdlib::dse;
namespace rip = ntdlib::dse::rip;
using dse::SequenceRef;
using dse_song_files::Bytes;
using xsflib::testing::ErrorOf;

constexpr uint16_t kEffectsId = 0x0101;
constexpr uint16_t kJingleBankId = 0x0202;
constexpr uint16_t kMissingBankId = 0x0303;

// dse_song_files.h's bank with the id `id`.
Bytes BankWithId(uint16_t id)
{
    Bytes bank = dse_song_files::BankFile();
    bank[0x0E] = static_cast<uint8_t>(id);
    bank[0x0F] = static_cast<uint8_t>(id >> 8);
    return bank;
}

// Sound effect 0 plays the banks of the songs' id, sound effect 1 a bank that no file holds, and sound effect 3 the
// bank of jingle me0001. Slot 2 is empty.
Bytes EffectsFile()
{
    const Bytes track = dse_song_files::Join({{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xA0, 0x04},
                                              dse_song_files::Note(0x60, 0, 0, 24, 1),
                                              {0x92, 0x18, 0x98}});
    return dse_song_files::EffectsFile(
        {dse_song_files::Effect{.tracks = {track}},
         dse_song_files::Effect{.tracks = {track}, .banks = {kMissingBankId}}, std::nullopt,
         dse_song_files::Effect{.tracks = {track}, .banks = {kJingleBankId}}},
        kEffectsId);
}

// The game's files, in the order of its file system.
struct Game
{
    std::vector<dse::NamedFile> Files() const
    {
        std::vector<dse::NamedFile> files;
        for (const auto& [path, data] : bytes)
        {
            files.push_back({path, data});
        }
        return files;
    }

    std::vector<std::pair<std::string, Bytes>> bytes = {
        {"SOUND/BGM/bgm.swd", dse_song_files::MainBankFile()},
        {"SOUND/BGM/bgm0001.smd", dse_song_files::SongFile()},
        {"SOUND/BGM/bgm0001.swd", dse_song_files::BankFile()},
        {"SOUND/BGM/bgm0002.smd", dse_song_files::SongFile()},
        {"SOUND/BGM/bgm0002.swd", dse_song_files::BankFile()},
        {"SOUND/BGM/bgm0003.smd", dse_song_files::SongFile()},
        {"SOUND/ME/me.swd", dse_song_files::BankFile()},
        {"SOUND/ME/me0000.smd", dse_song_files::SongFile()},
        {"SOUND/ME/me0001.smd", dse_song_files::SongFile()},
        {"SOUND/ME/me0001.swd", BankWithId(kJingleBankId)},
        {"SOUND/SE/fx.sed", EffectsFile()},
    };
};

std::vector<std::string> Labels(const std::vector<rip::Job>& jobs)
{
    std::vector<std::string> labels;
    for (const rip::Job& job : jobs)
    {
        labels.push_back(job.label);
    }
    return labels;
}

std::vector<std::string> PathsOf(const std::vector<dse::NamedFile>& files,
                                 const std::optional<std::vector<std::size_t>>& kept)
{
    std::vector<std::string> paths;
    for (const std::size_t index : kept.value_or(std::vector<std::size_t>{}))
    {
        paths.push_back(files[index].path);
    }
    return paths;
}

void TestLabels()
{
    XSFLIB_CHECK(rip::Label({"SOUND/BGM/bgm0001.smd"}) == "bgm0001");
    XSFLIB_CHECK(rip::Label({"SOUND/SYSTEM/se_sys.sed", 5}) == "se_sys_005");
    XSFLIB_CHECK(rip::Label({"song.smd"}) == "song");
    XSFLIB_CHECK(rip::IsMusic({"SOUND/ME/me0000.smd"}));
    XSFLIB_CHECK(!rip::IsMusic({"SOUND/SE/fx.sed", 0}));
}

// The songs come first, then each slot of the bank of sound effects that holds a sequence.
void TestSelection()
{
    const Game game;
    const std::vector<dse::NamedFile> files = game.Files();
    XSFLIB_CHECK(
        Labels(rip::SelectSequences(files, std::regex(""), false)) ==
        (std::vector<std::string>{"bgm0001", "bgm0002", "bgm0003", "me0000", "me0001", "fx_000", "fx_001", "fx_003"}));
    XSFLIB_CHECK(Labels(rip::SelectSequences(files, std::regex(""), true)) ==
                 (std::vector<std::string>{"bgm0001", "bgm0002", "bgm0003", "me0000", "me0001"}));
    const std::vector<rip::Job> only = rip::SelectSequences(files, std::regex("0002|_001"), false);
    XSFLIB_CHECK(Labels(only) == (std::vector<std::string>{"bgm0002", "fx_001"}));
    XSFLIB_CHECK(only.size() == 2 && only[1].sequence == (SequenceRef{"SOUND/SE/fx.sed", 1}) && !only[1].timing);
}

void TestMissingData()
{
    const Game game;
    std::vector<dse::NamedFile> files = game.Files();
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/BGM/bgm0001.smd"}).empty());
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/ME/me0000.smd"}).empty());
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/SE/fx.sed", 0}).empty());
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/SE/fx.sed", 3}).empty());
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/BGM/bgm0003.smd"}) ==
                 std::vector<std::string>{"the song's wave bank"});
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/SE/fx.sed", 1}) == std::vector<std::string>{"wave bank 0x0303"});
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/SE/fx.sed", 2}) == std::vector<std::string>{"sound effect 2"});
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/SE/fx.sed", 9}) == std::vector<std::string>{"sound effect 9"});
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/BGM/bgm0009.smd"}) ==
                 std::vector<std::string>{"the file SOUND/BGM/bgm0009.smd"});

    // Without the main bank, the song's bank has no samples.
    files.erase(files.begin());
    XSFLIB_CHECK(rip::MissingData(files, {"SOUND/BGM/bgm0001.smd"}) ==
                 std::vector<std::string>{"the main bank 0x0000"});
}

// A library keeps the files that its sequences play. Sound effect 0 plays bgm0001.swd: no bank of its id is outside
// the songs' banks, and that bank comes first. Among the kept files no song plays it, and the effect still finds it
// first.
void TestKeepSequenceData()
{
    const Game game;
    const std::vector<dse::NamedFile> files = game.Files();
    XSFLIB_CHECK(PathsOf(files, rip::KeepSequenceData(files, std::vector<SequenceRef>{{"SOUND/BGM/bgm0002.smd"}})) ==
                 (std::vector<std::string>{"SOUND/BGM/bgm.swd", "SOUND/BGM/bgm0002.smd", "SOUND/BGM/bgm0002.swd"}));
    const std::vector<SequenceRef> song_and_effect = {{"SOUND/BGM/bgm0002.smd"}, {"SOUND/SE/fx.sed", 0}};
    XSFLIB_CHECK(PathsOf(files, rip::KeepSequenceData(files, song_and_effect)) ==
                 (std::vector<std::string>{"SOUND/BGM/bgm.swd", "SOUND/BGM/bgm0001.swd", "SOUND/BGM/bgm0002.smd",
                                           "SOUND/BGM/bgm0002.swd", "SOUND/SE/fx.sed"}));
    const std::vector<SequenceRef> jingle_and_effect = {{"SOUND/ME/me0000.smd"}, {"SOUND/SE/fx.sed", 0}};
    XSFLIB_CHECK(PathsOf(files, rip::KeepSequenceData(files, jingle_and_effect)) ==
                 (std::vector<std::string>{"SOUND/BGM/bgm.swd", "SOUND/BGM/bgm0001.swd", "SOUND/ME/me.swd",
                                           "SOUND/ME/me0000.smd", "SOUND/SE/fx.sed"}));
}

// Kept files that give a sequence other banks. Sound effect 3 keeps me0001.swd without its jingle. me0000 then finds
// two banks in its folder that no song shares a name with, and no bank. A sequence that lacks data gives nothing too.
void TestKeepRefusals()
{
    const Game game;
    const std::vector<dse::NamedFile> files = game.Files();
    const std::vector<SequenceRef> both_jingles = {{"SOUND/ME/me0000.smd"}, {"SOUND/ME/me0001.smd"}};
    XSFLIB_CHECK(PathsOf(files, rip::KeepSequenceData(files, both_jingles)) ==
                 (std::vector<std::string>{"SOUND/BGM/bgm.swd", "SOUND/ME/me.swd", "SOUND/ME/me0000.smd",
                                           "SOUND/ME/me0001.smd", "SOUND/ME/me0001.swd"}));
    const std::vector<SequenceRef> jingle_and_effect = {{"SOUND/ME/me0000.smd"}, {"SOUND/SE/fx.sed", 3}};
    XSFLIB_CHECK(!rip::KeepSequenceData(files, jingle_and_effect));
    const std::vector<SequenceRef> missing = {{"SOUND/BGM/bgm0001.smd"}, {"SOUND/BGM/bgm0003.smd"}};
    XSFLIB_CHECK(!rip::KeepSequenceData(files, missing));
}

// The files' paths in sorted order: bgm.swd, bgm0001.smd, bgm0001.swd, bgm0002.smd, bgm0002.swd, bgm0003.smd, me.swd,
// me0000.smd, me0001.smd, me0001.swd and fx.sed.
void TestSoundIds()
{
    const Game game;
    const std::vector<dse::NamedFile> files = game.Files();
    XSFLIB_CHECK(rip::SoundId(files, {"SOUND/BGM/bgm0001.smd"}) == 0x10000u);
    XSFLIB_CHECK(rip::SoundId(files, {"SOUND/SE/fx.sed", 1}) == 0xA0002u);
    XSFLIB_CHECK(rip::SequenceOf(files, 0xA0002u) == (SequenceRef{"SOUND/SE/fx.sed", 1}));
    XSFLIB_CHECK(rip::SequenceOf(files, 0x70000u) == (SequenceRef{"SOUND/ME/me0000.smd"}));
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { rip::SoundId(files, {"SOUND/BGM/bgm0009.smd"}); }) ==
                 "the files lack SOUND/BGM/bgm0009.smd");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { rip::SoundId(files, {"SOUND/SE/fx.sed", 0xFFFF}); }) ==
                 "a sound's number can't hold fx_65535");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { rip::SequenceOf(files, 0xB0000u); }) == "the files have no file 11");
}

struct TestDescriptor
{
    std::vector<uint8_t> Serialize() const
    {
        return {build};
    }

    uint8_t build = 0;
};

struct TestFormat
{
    using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;
    static constexpr uint8_t kPsfVersion = 0x7F;
};

// A test program's set: the files of a library, and the sound that its mini names.
struct TestSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto file = files.find(path);
        return file != files.end() ? &file->second : nullptr;
    }

    std::vector<dse::NamedFile> Named() const
    {
        std::vector<dse::NamedFile> named;
        for (const auto& [path, bytes] : files)
        {
            named.push_back({path, bytes});
        }
        return named;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    uint32_t sound = 0;
};

struct TestProgram
{
    using Set = TestSet;

    static std::optional<std::string> Select(TestSet& set, dse::ArchiveSelection& selection)
    {
        const std::vector<dse::NamedFile> named = set.Named();
        const std::optional<dse::ArchiveSelection> found =
            dse::SelectionOf(named, rip::SequenceOf(named, set.sound), dse::Build::kLater);
        if (!found)
        {
            return "no such sequence";
        }
        selection = *found;
        return std::nullopt;
    }
};

using Source = xsflib::player::ArchiveSource<dse::ArchiveTraits<TestProgram>>;

// The samples of `frames` frames of the set's sound.
std::vector<int16_t> Play(TestSet& set, int frames)
{
    Source source;
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Start(set, out));
    for (int i = 0; i < frames; i++)
    {
        source.RunFrame(out);
    }
    return out;
}

// A library holds the kept files at their paths, with the descriptor in the reserved area. A mini names its sequence's
// sound and its library. A set made of the library and a mini plays as the whole game's files do.
void TestLibraryAndMini()
{
    const Game game;
    const std::vector<dse::NamedFile> files = game.Files();
    const std::vector<SequenceRef> sequences = {{"SOUND/BGM/bgm0002.smd"}, {"SOUND/SE/fx.sed", 0}};
    const std::vector<std::size_t> kept = rip::KeepSequenceData(files, sequences).value_or(std::vector<std::size_t>{});
    const Bytes library = rip::BuildLibrary<TestFormat>(files, kept, TestDescriptor{.build = 1}, {{"title", "a game"}});
    xsflib::psf::PsfFile psf;
    xsflib::psf::ProgramChunks chunks;
    XSFLIB_CHECK(!xsflib::psf::ParsePsf(library, psf) && !xsflib::psf::ParseProgram(psf.program, chunks));
    XSFLIB_CHECK(psf.version == 0x7F && psf.tags["title"] == "a game" && psf.reserved == Bytes{1});
    XSFLIB_CHECK(chunks.files.size() == kept.size() && !chunks.sound);

    TestSet whole;
    for (const auto& [path, bytes] : game.bytes)
    {
        whole.files[path] = bytes;
    }
    TestSet cut;
    std::vector<dse::NamedFile> library_files;
    for (const std::size_t index : kept)
    {
        library_files.push_back(files[index]);
        const std::vector<uint8_t>* file = xsflib::psf::FindFile(chunks.files, files[index].path);
        XSFLIB_CHECK(file && *file == whole.files[files[index].path]);
        cut.files[files[index].path] = file ? *file : Bytes{};
    }

    for (const SequenceRef& sequence : sequences)
    {
        const Bytes mini = rip::BuildMini<TestFormat>(library_files, sequence, "game.lib", {{"title", "a sequence"}});
        xsflib::psf::PsfFile mini_psf;
        xsflib::psf::ProgramChunks mini_chunks;
        XSFLIB_CHECK(!xsflib::psf::ParsePsf(mini, mini_psf) &&
                     !xsflib::psf::ParseProgram(mini_psf.program, mini_chunks));
        XSFLIB_CHECK(mini_psf.reserved.empty() && mini_chunks.files.empty());
        XSFLIB_CHECK(mini_psf.tags["_lib"] == "game.lib" && mini_psf.tags["title"] == "a sequence");
        XSFLIB_CHECK(mini_chunks.sound == rip::SoundId(library_files, sequence));

        cut.sound = mini_chunks.sound.value_or(0);
        whole.sound = rip::SoundId(whole.Named(), sequence);
        const std::vector<int16_t> played = Play(cut, 300);
        XSFLIB_CHECK(std::ranges::any_of(played, [](int16_t sample) { return sample != 0; }));
        XSFLIB_CHECK(played == Play(whole, 300));
    }
}

} // namespace

int main()
{
    TestLabels();
    TestSelection();
    TestMissingData();
    TestKeepSequenceData();
    TestKeepRefusals();
    TestSoundIds();
    TestLibraryAndMini();
    return xsflib::testing::Summary("DSE's rip parts");
}
