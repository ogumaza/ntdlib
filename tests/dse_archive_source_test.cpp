// SPDX-License-Identifier: MIT

// Tests of DSE's steps of archive mode (ntdlib/dse/archive_source.h) in xsflib's archive mode, on a set of a test
// program with the files of dse_song_files.h. The source must play what dse::Player plays.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/player/archive_source.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/archive_source.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace dse = ntdlib::dse;
using dse_song_files::Bytes;

constexpr const char* kSong = "SOUND/BGM/bgm0001.smd";
constexpr const char* kBank = "SOUND/BGM/bgm0001.swd";
constexpr const char* kMainBank = "SOUND/BGM/bgm.swd";
constexpr const char* kEffects = "SOUND/SE/fx.sed";
constexpr uint16_t kEffectsId = 0x0101;

// A test program's set: its files by path, and the sequence that it plays.
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
    dse::SequenceRef sequence;
    std::optional<std::string> also_load; // a bank that the selection also loads
};

struct TestProgram
{
    using Set = TestSet;

    static std::optional<std::string> Select(TestSet& set, dse::ArchiveSelection& selection)
    {
        const std::optional<dse::ArchiveSelection> found =
            dse::SelectionOf(set.Named(), set.sequence, dse::Build::kLater);
        if (!found)
        {
            return "no such sequence";
        }
        selection = *found;
        if (set.also_load)
        {
            selection.bank_paths.push_back(*set.also_load);
        }
        return std::nullopt;
    }
};

using Source = xsflib::player::ArchiveSource<dse::ArchiveTraits<TestProgram>>;

// One effect: a note of the song's bank's program 0 for 24 ticks, a wait of 24 ticks and the end.
Bytes EffectsFile()
{
    const Bytes track = dse_song_files::Join({{0, 0, 0, 0, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xA0, 0x04},
                                              dse_song_files::Note(0x60, 0, 0, 24, 1),
                                              {0x92, 0x18, 0x98}});
    return dse_song_files::EffectsFile({dse_song_files::Effect{.tracks = {track}}}, kEffectsId);
}

TestSet MakeSet(dse::SequenceRef sequence = {kSong, std::nullopt})
{
    TestSet set;
    set.files[kSong] = dse_song_files::SongFile();
    set.files[kBank] = dse_song_files::BankFile();
    set.files[kMainBank] = dse_song_files::MainBankFile();
    set.files[kEffects] = EffectsFile();
    set.sequence = std::move(sequence);
    return set;
}

// The samples of `frames` ticks of the sequence of `set`, on a player that plays the files directly.
std::vector<int16_t> PlayDirectly(const TestSet& set, int frames)
{
    const std::optional<dse::ArchiveSelection> selection =
        dse::SelectionOf(set.Named(), set.sequence, dse::Build::kLater);
    XSFLIB_CHECK(selection.has_value());
    std::optional<dse::Song> song;
    std::optional<dse::SoundEffects> effects;
    if (set.sequence.effect)
    {
        effects = dse::SoundEffects::Parse(set.files.at(set.sequence.path));
    }
    else
    {
        song = dse::Song::Parse(set.files.at(set.sequence.path));
    }
    std::vector<dse::WaveBank> banks;
    for (const std::string& path : selection->bank_paths)
    {
        banks.push_back(dse::WaveBank::Parse(set.files.at(path)));
    }
    const dse::WaveBank main_bank = dse::WaveBank::Parse(set.files.at(*selection->main_bank_path));

    dse::SequenceFiles files{.song = song ? &*song : nullptr,
                             .effects = effects ? &*effects : nullptr,
                             .effect = set.sequence.effect.value_or(0),
                             .main_bank = &main_bank};
    for (const dse::WaveBank& bank : banks)
    {
        files.banks.push_back(&bank);
    }
    dse::Player player;
    player.LoadBanks(files);
    XSFLIB_CHECK(player.Start(files));
    std::vector<int16_t> out;
    std::vector<ntdlib::ntr::SoundOutput> samples;
    for (int i = 0; i < frames; i++)
    {
        samples.clear();
        player.RunTick(samples);
        ntdlib::ntr::AppendPcm16(samples, out);
    }
    return out;
}

// The selection of the song and of the effect: the song's bank and the main bank, and the effect's bank of its id.
void TestSelection()
{
    const TestSet set = MakeSet();
    const std::optional<dse::ArchiveSelection> song = dse::SelectionOf(set.Named(), {kSong}, dse::Build::kEarlier);
    XSFLIB_CHECK(song && song->bank_paths == std::vector<std::string>{kBank} && song->main_bank_path == kMainBank);
    XSFLIB_CHECK(song && song->build == dse::Build::kEarlier);
    const std::optional<dse::ArchiveSelection> effect =
        dse::SelectionOf(set.Named(), {kEffects, 0}, dse::Build::kLater);
    XSFLIB_CHECK(effect && effect->bank_paths == std::vector<std::string>{kBank});
    XSFLIB_CHECK(effect && effect->main_bank_path == kMainBank);
    XSFLIB_CHECK(!dse::SelectionOf(set.Named(), {kEffects, 1}, dse::Build::kLater));
    XSFLIB_CHECK(!dse::SelectionOf(set.Named(), {kBank}, dse::Build::kLater));
}

void TestPlaysAsThePlayer(const dse::SequenceRef& sequence, int frames)
{
    TestSet set = MakeSet(sequence);
    const std::vector<int16_t> expected = PlayDirectly(set, frames);
    Source source;
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Busy());
    XSFLIB_CHECK(!source.Start(set, out));
    XSFLIB_CHECK(out.empty() && source.Busy());
    for (int i = 0; i < frames; i++)
    {
        XSFLIB_CHECK(source.RunFrame(out));
    }
    XSFLIB_CHECK(out == expected);
}

// A restore of a snapshot plays the same samples again.
void TestSnapshot()
{
    TestSet set = MakeSet();
    Source source;
    std::vector<int16_t> out;
    XSFLIB_CHECK(!source.Start(set, out));
    for (int i = 0; i < 100; i++)
    {
        source.RunFrame(out);
    }
    const auto snapshot = source.Save(nullptr);
    std::vector<int16_t> first;
    for (int i = 0; i < 200; i++)
    {
        source.RunFrame(first);
    }
    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int i = 0; i < 200; i++)
    {
        source.RunFrame(again);
    }
    XSFLIB_CHECK(!first.empty() && again == first);
}

// A sequence that the files lack, a selection of a file that the set lacks, and a sound effect that the driver
// refuses: its five tracks don't fit in a sequence of the pool.
void TestRefusals()
{
    Source source;
    std::vector<int16_t> out;
    TestSet missing = MakeSet({"SOUND/BGM/bgm0002.smd", std::nullopt});
    XSFLIB_CHECK(source.Start(missing, out) == "no such sequence");

    TestSet unloaded = MakeSet();
    unloaded.also_load = "SOUND/BGM/none.swd";
    XSFLIB_CHECK(source.Start(unloaded, out) == "the set has no file SOUND/BGM/none.swd");

    const Bytes track = {0, 0, 0, 0, 0x98};
    TestSet refused = MakeSet({kEffects, 0});
    refused.files[kEffects] = dse_song_files::EffectsFile(
        {dse_song_files::Effect{.tracks = {track, track, track, track, track}}}, kEffectsId);
    XSFLIB_CHECK(source.Start(refused, out) == "the driver refuses the sound effect");
    XSFLIB_CHECK(out.empty());
}

} // namespace

int main()
{
    TestSelection();
    TestPlaysAsThePlayer({kSong, std::nullopt}, 1200);
    TestPlaysAsThePlayer({kEffects, 0}, 200);
    TestSnapshot();
    TestRefusals();
    return xsflib::testing::Summary("DSE's archive mode");
}
