// SPDX-License-Identifier: MIT

// Tests the parts of a rip tool that rip sound archives (rip/archive.h): item IDs, and on archives built with
// sar_files.h the sounds that --only and --bgm pick, the groups kept outside the archive, by name and in SARC archives,
// and the data a sound requires. It also tests the search for group files and SARC archives among the paths of the
// files of a game, and the kinds of sound archive (sar/formats.h). rip_archive_library_test.cpp tests the libraries.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "ntdlib/rip/archive.h"
#include "ntdlib/rip/player_names.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "rip_archive_test_util.h"

using namespace xsflib::testing;

using namespace ntdlib::testing;

using namespace rip_archive_tests;

namespace
{

namespace rip = ntdlib::rip;
namespace sar = ntdlib::sar;

const sar::Options kStrictRules{sar::Flavor::kNx, {}, false, sar::Rules::kStopAtFirstProblem};

// A job with states gets a job for each, the first under its label and the others under the label and the state's
// name, with the state's values. The other jobs stay as they are, their variables too.
void TestWithStates()
{
    struct State
    {
        std::string name;
        uint16_t muted = 0;
    };

    const std::vector<State> layered = {{"", 0x0006}, {"later", 0x0001}, {"all", 0}};
    const std::vector<rip::Job> selected = {{.index = 0, .label = "SEQ_A", .variables = {{1, 4}}},
                                            {.index = 1, .label = "SEQ_B"},
                                            {.index = 2, .label = "SEQ_B2"}};
    const auto states_of = [&layered](const rip::Job& job)
    {
        return job.label == "SEQ_B" ? std::span<const State>(layered) : std::span<const State>();
    };
    const auto apply = [](const State& state, rip::Job& job)
    {
        job.track_volumes = rip::TrackVolumesOf(state.muted);
    };

    const std::vector<rip::Job> jobs = rip::WithStates(selected, states_of, apply);

    Check(jobs.size() == 5 && jobs[0].label == "SEQ_A" && jobs[0].track_volumes == rip::kFullTrackVolumes &&
              jobs[0].variables == xsflib::psf::Variables{{1, 4}} && jobs[1].index == 1 && jobs[1].label == "SEQ_B" &&
              jobs[1].track_volumes == rip::TrackVolumesOf(0x0006) && jobs[2].index == 1 &&
              jobs[2].label == "SEQ_B later" && jobs[2].track_volumes == rip::TrackVolumesOf(0x0001) &&
              jobs[3].index == 1 && jobs[3].label == "SEQ_B all" && jobs[3].track_volumes == rip::kFullTrackVolumes &&
              jobs[4].label == "SEQ_B2",
          "a job's states follow it in order, and the other jobs stay as they are");
}

void TestItems()
{
    XSFLIB_CHECK(rip::ItemIndex(0x03000002, rip::kBankItem, 3) == 2u);
    XSFLIB_CHECK(!rip::ItemIndex(0x05000002, rip::kBankItem, 3));
    XSFLIB_CHECK(!rip::ItemIndex(0x03000003, rip::kBankItem, 3));
}

// An archive with a sound of each kind that the rip tools distinguish: music by its label or by its player, effects, a
// stream, and a sequence whose sequence data, bank and wave archive are kept elsewhere.
std::vector<uint8_t> MixedArchive()
{
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}};

    sar_files::ArchiveSpec spec;
    spec.sounds = {
        {.name = "BGM_TITLE", .file = 0, .player = 1, .banks = {0}},
        {.name = "SE_JUMP", .file = 0, .player = 0},
        {.name = "THEME", .file = 0, .player = 2},
        {.name = "MIXED", .file = 0, .player = 3},
        {.name = "STRM_VOICE", .kind = sar_files::SoundSpec::kStream, .file = 0},
        {.name = "SE_MISSING", .file = 3, .player = 0, .banks = {1, 0xFFFFFFFF}},
    };
    spec.banks = {{"BANK_A", 1}, {"BANK_B", 4}};
    spec.wave_archives = {{"WARC_A", 2}};
    spec.players = {{"PLAYER_SE", 1}, {"PLAYER_BGM1", 1}, {"SONG_PLAYER", 1}, {"BGM_SE", 1}};
    spec.groups = {{"GROUP_OUTSIDE", 0xFFFFFFFF}, {"GROUP_ALSO_OUTSIDE", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(false, {0xFF}, {{"Start", 0}}), "", false},
                  {sar_files::Bank(false, bank), "", false},
                  {{}, "wave/WARC_A.bfwar", false},
                  {{}, "seq/SE_MISSING.bfseq", false},
                  {{}, "bank/BANK_B.bfbnk", false}};

    return sar_files::Archive(false, spec);
}

void TestArchive()
{
    const sar::SoundArchive archive = sar::SoundArchive::Load(MixedArchive(), kStrictRules);
    if (!Check(archive.Sounds().size() == 6, "the archive's sounds"))
    {
        return;
    }

    XSFLIB_CHECK(rip::IsMusic(archive, 0));  // BGM in its label
    XSFLIB_CHECK(!rip::IsMusic(archive, 1)); // an effects player
    XSFLIB_CHECK(rip::IsMusic(archive, 2));  // SONG_PLAYER
    XSFLIB_CHECK(!rip::IsMusic(archive, 3)); // a player of both
    XSFLIB_CHECK(!rip::IsMusic(archive, 4)); // a stream
    XSFLIB_CHECK(!rip::IsMusic(archive, 6)); // past the sounds

    // The players' names alone, in any case.
    XSFLIB_CHECK(rip::IsMusicPlayerName("Music_2") && rip::IsMusicPlayerName("FANFARE"));
    XSFLIB_CHECK(!rip::IsMusicPlayerName("PLAYER") && !rip::IsMusicPlayerName("") && !rip::IsMusicPlayerName("SE_BGM"));
    XSFLIB_CHECK(rip::PlayerNameWords("Player_BGM12__3") == (std::vector<std::string>{"player", "bgm", "", ""}));

    const auto indices = [](const std::vector<rip::Job>& jobs)
    {
        std::vector<uint32_t> out;
        for (const rip::Job& job : jobs)
        {
            out.push_back(job.index);
            Check(!job.timing, "a job's timing before the length analysis");
        }

        return out;
    };

    XSFLIB_CHECK(
        (indices(rip::SelectSounds(archive, std::regex(".*"), false)) == std::vector<uint32_t>{0, 1, 2, 3, 5}));
    XSFLIB_CHECK((indices(rip::SelectSounds(archive, std::regex(".*"), true)) == std::vector<uint32_t>{0, 2}));
    XSFLIB_CHECK((indices(rip::SelectSounds(archive, std::regex("^SE_"), false)) == std::vector<uint32_t>{1, 5}));
    XSFLIB_CHECK(rip::SelectSounds(archive, std::regex("JUMP"), false).at(0).label == "SE_JUMP");

    XSFLIB_CHECK((rip::ExternalGroups(archive) == std::vector<std::string>{"GROUP_OUTSIDE", "GROUP_ALSO_OUTSIDE"}));

    XSFLIB_CHECK((rip::MissingData(archive, 0, kStrictRules) == std::vector<std::string>{"wave archive WARC_A"}));
    XSFLIB_CHECK(rip::MissingData(archive, 1, kStrictRules).empty());
    XSFLIB_CHECK(
        (rip::MissingData(archive, 5, kStrictRules) == std::vector<std::string>{"its sequence data", "bank BANK_B"}));
    XSFLIB_CHECK((rip::MissingData(archive, 9, kStrictRules) == std::vector<std::string>{"the sound itself"}));

    // A group's file has the group's name and ".bfgrp", in any case.
    XSFLIB_CHECK(rip::IsGroupFileName("GROUP_OUTSIDE.bfgrp", "GROUP_OUTSIDE"));
    XSFLIB_CHECK(rip::IsGroupFileName("group_outside.BFGRP", "GROUP_OUTSIDE"));
    XSFLIB_CHECK(!rip::IsGroupFileName("GROUP_OUTSIDE_2.bfgrp", "GROUP_OUTSIDE"));
    XSFLIB_CHECK(!rip::IsGroupFileName("GROUP_OUTSIDE.bfgrp.bak", "GROUP_OUTSIDE"));
    XSFLIB_CHECK(!rip::IsGroupFileName("GROUP_OUTSIDE.bfsar", "GROUP_OUTSIDE"));

    // On the 3DS, a group's file has the group's name and ".bcgrp".
    XSFLIB_CHECK(sar::GroupFileName("GROUP_OUTSIDE", sar::Flavor::kCtr) == "GROUP_OUTSIDE.bcgrp");
    XSFLIB_CHECK(sar::GroupFileName("GROUP_OUTSIDE", sar::Flavor::kCafe) == "GROUP_OUTSIDE.bfgrp");
    XSFLIB_CHECK(sar::GroupFileName("GROUP_OUTSIDE", sar::Flavor::kNx) == "GROUP_OUTSIDE.bfgrp");
    XSFLIB_CHECK(rip::IsGroupFileName("group_outside.BCGRP", "GROUP_OUTSIDE", sar::Flavor::kCtr));
    XSFLIB_CHECK(!rip::IsGroupFileName("GROUP_OUTSIDE.bfgrp", "GROUP_OUTSIDE", sar::Flavor::kCtr));
    XSFLIB_CHECK(rip::IsGroupFileName("GROUP_OUTSIDE.bfgrp", "GROUP_OUTSIDE", sar::Flavor::kNx));
    XSFLIB_CHECK(!rip::IsGroupFileName("GROUP_OUTSIDE.bcgrp", "GROUP_OUTSIDE", sar::Flavor::kCafe));

    // The groups that a rip looks for beside the archive.
    const auto find_outside = [](const std::string& name)
    {
        return name == "GROUP_OUTSIDE" ? std::optional<Chunk>(Chunk{name, {}}) : std::nullopt;
    };
    const std::vector<Chunk> found = rip::FindGroups<Chunk>(MixedArchive(), find_outside, kStrictRules);
    XSFLIB_CHECK(Paths(found) == std::vector<std::string>{"GROUP_OUTSIDE"});
    XSFLIB_CHECK(
        rip::FindGroups<Chunk>({1, 2, 3}, [](const std::string&) { return std::nullopt; }, kStrictRules).empty());
}

// An archive with two groups kept outside it. Group A holds bank A, file 1, and group B holds bank B, file 2. Without
// `names` the archive has no names.
std::vector<uint8_t> GroupedArchive(bool names = true)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ", .file = 0, .player = 0, .banks = {0, 1}}};
    spec.banks = {{"BANK_A", 1}, {"BANK_B", 2}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 0xFFFFFFFF}, {"GROUP_B", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(false, {0xFF}, {}), "", false}, {{}, "", true}, {{}, "", true}};
    spec.names = names;

    return sar_files::Archive(false, spec);
}

void TestGroupsInSarcs()
{
    const std::vector<uint8_t> group_a = sar_files::Group(false, {{1, Bytes("FBNK A")}});
    const std::vector<uint8_t> group_b = sar_files::Group(false, {{2, Bytes("FBNK B")}});

    // The SARCs that the search may read, by path. Another archive's group holds a wave archive as file 1.
    const std::map<std::string, std::vector<uint8_t>> sarcs = {
        {"Other.sarc", sar_files::Sarc(false, {{"GROUP_X.bfgrp", sar_files::Group(false, {{1, Bytes("FWAR")}})}})},
        {"Res.sarc", sar_files::Sarc(false, {{"README", Bytes("text")}, {"Sound/GROUP_B.bfgrp", group_b}})},
        {"Nameless.sarc", sar_files::Sarc(false, {Bytes("text"), group_b})}};
    std::vector<std::string> read;
    const auto read_sarc = [&](const std::string& path, std::vector<uint8_t>& bytes)
    {
        read.push_back(path);
        const auto sarc = sarcs.find(path);
        if (sarc == sarcs.end())
        {
            return false;
        }

        bytes = sarc->second;

        return true;
    };
    const auto find_a = [&group_a](const std::string& name)
    {
        return name == "GROUP_A" ? std::optional<Chunk>(Chunk{"GROUP_A.bfgrp", group_a}) : std::nullopt;
    };
    const auto search = [&](const std::vector<uint8_t>& archive, const std::vector<std::string>& paths, auto find)
    {
        read.clear();

        return rip::FindGroups<Chunk>(archive, find, kStrictRules, [&paths] { return paths; }, read_sarc);
    };

    // Group A by its name. Group B from a SARC, at the SARC's path and its name there. A SARC that can't be read and
    // another archive's group are passed over.
    const std::vector<Chunk> named = search(GroupedArchive(), {"Gone.sarc", "Other.sarc", "Res.sarc"}, find_a);
    XSFLIB_CHECK((Paths(named) == std::vector<std::string>{"GROUP_A.bfgrp", "Res.sarc/Sound/GROUP_B.bfgrp"}));
    XSFLIB_CHECK(named.size() == 2 && named[1].data == group_b);
    XSFLIB_CHECK((read == std::vector<std::string>{"Gone.sarc", "Other.sarc", "Res.sarc"}));

    // A group file without a name in its SARC is kept at its index there.
    XSFLIB_CHECK((Paths(search(GroupedArchive(), {"Nameless.sarc"}, find_a)) ==
                  std::vector<std::string>{"GROUP_A.bfgrp", "Nameless.sarc/1"}));

    // With every group found by name, no SARC is read.
    const auto find_both = [&](const std::string& name)
    {
        return std::optional<Chunk>(Chunk{name + ".bfgrp", name == "GROUP_A" ? group_a : group_b});
    };
    XSFLIB_CHECK((Paths(search(GroupedArchive(), {"Res.sarc"}, find_both)) ==
                  std::vector<std::string>{"GROUP_A.bfgrp", "GROUP_B.bfgrp"}));
    XSFLIB_CHECK(read.empty());

    // An archive without names has no groups to find by name. Its groups are still missing. The SARCs are then read.
    const auto find_none = [](const std::string&) -> std::optional<Chunk>
    {
        return std::nullopt;
    };
    XSFLIB_CHECK((Paths(search(GroupedArchive(false), {"Res.sarc"}, find_none)) ==
                  std::vector<std::string>{"Res.sarc/Sound/GROUP_B.bfgrp"}));

    // Without a list of SARCs, only the groups found by name.
    XSFLIB_CHECK((Paths(rip::FindGroups<Chunk>(GroupedArchive(), find_a, kStrictRules)) ==
                  std::vector<std::string>{"GROUP_A.bfgrp"}));
}

} // namespace

// A group file among a game's paths: by its file name in any case, the first that can be read. A folder's name doesn't
// count.
void TestFindGroupFile()
{
    const std::vector<std::string> paths = {"content/Sound/GROUP_A.BFGRP", "content/group_b.bfgrp",
                                            "content/group_b.bfgrp/x", "group_b.bfgrp", "content/Sound/x.bfsar"};
    std::vector<std::string> tried;
    const auto read = [&tried](const std::string& path, std::vector<uint8_t>& data)
    {
        tried.push_back(path);
        if (path == "content/group_b.bfgrp")
        {
            return false;
        }

        data = {static_cast<uint8_t>(path.size())};
        return true;
    };

    const std::optional<Chunk> a = rip::FindGroupFile<Chunk>(paths, read, "group_a");
    XSFLIB_CHECK(a && a->path == "content/Sound/GROUP_A.BFGRP" && a->data == std::vector<uint8_t>{27});

    tried.clear();
    const std::optional<Chunk> b = rip::FindGroupFile<Chunk>(paths, read, "GROUP_B");
    XSFLIB_CHECK(b && b->path == "group_b.bfgrp");
    XSFLIB_CHECK((tried == std::vector<std::string>{"content/group_b.bfgrp", "group_b.bfgrp"}));
    XSFLIB_CHECK(!rip::FindGroupFile<Chunk>(paths, read, "group_c"));
}

// The extension of a group file follows the flavor of the archive: ".bcgrp" on the 3DS, ".bfgrp" on the Wii U and
// the Switch.
void TestFindGroupFileByFlavor()
{
    const std::vector<std::string> paths = {"romfs/GROUP_A.bfgrp", "romfs/sound/group_a.BCGRP"};
    const auto read = [](const std::string& path, std::vector<uint8_t>& data)
    {
        data = {static_cast<uint8_t>(path.size())};
        return true;
    };

    const std::optional<Chunk> ctr = rip::FindGroupFile<Chunk>(paths, read, "GROUP_A", sar::Flavor::kCtr);
    XSFLIB_CHECK(ctr && ctr->path == "romfs/sound/group_a.BCGRP");
    for (const sar::Flavor flavor : {sar::Flavor::kCafe, sar::Flavor::kNx})
    {
        const std::optional<Chunk> other = rip::FindGroupFile<Chunk>(paths, read, "GROUP_A", flavor);
        XSFLIB_CHECK(other && other->path == "romfs/GROUP_A.bfgrp");
    }

    const std::optional<Chunk> plain = rip::FindGroupFile<Chunk>(paths, read, "GROUP_A");
    XSFLIB_CHECK(plain && plain->path == "romfs/GROUP_A.bfgrp");
    XSFLIB_CHECK(!rip::FindGroupFile<Chunk>(std::vector<std::string>{"GROUP_A.bcgrp"}, read, "GROUP_A"));
}

// A group file in a folder: found by the flavor's extension in a folder below, with its path from the folder in UTF-8
// and a '/' between folders. A folder with the file's name doesn't count. The check of the UTF-8 path has effect on
// Windows only. The test has no UTF-8 manifest, and a narrow conversion there fails for the kanji.
void TestFindGroupFileIn()
{
    const xsflib::testing::TempFolder temp("ntdlib_rip_archive_test");
    // "Sound", a space and two kanji, in UTF-8. Kanji have no decomposed form. Any volume therefore keeps their bytes.
    const std::string folder_name = "Sound \xE6\x97\xA5\xE6\x9C\xAC";
    const std::filesystem::path below = temp.Path() / "Sound" / xsflib::PathFromUtf8(folder_name);
    std::filesystem::create_directories(below / "GROUP_B.bcgrp");
    const std::vector<uint8_t> ctr_data = {1, 2, 3};
    const std::vector<uint8_t> cafe_data = {4, 5};
    XSFLIB_CHECK(!xsflib::WriteFile(below / "group_a.BCGRP", ctr_data));
    XSFLIB_CHECK(!xsflib::WriteFile(below / "GROUP_A.bfgrp", cafe_data));

    const std::optional<Chunk> ctr = rip::FindGroupFileIn<Chunk>(temp.Path(), "GROUP_A", sar::Flavor::kCtr);
    XSFLIB_CHECK(ctr && ctr->path == "Sound/" + folder_name + "/group_a.BCGRP" && ctr->data == ctr_data);
    const std::optional<Chunk> nx = rip::FindGroupFileIn<Chunk>(temp.Path(), "group_a", sar::Flavor::kNx);
    XSFLIB_CHECK(nx && nx->path == "Sound/" + folder_name + "/GROUP_A.bfgrp" && nx->data == cafe_data);
    XSFLIB_CHECK(!rip::FindGroupFileIn<Chunk>(temp.Path(), "GROUP_B", sar::Flavor::kCtr));
    XSFLIB_CHECK(!rip::FindGroupFileIn<Chunk>(temp.Path() / "missing", "GROUP_A", sar::Flavor::kCtr));
}

// The search in a folder passes over a folder that it can't open, and goes on to the next one. Windows still lists a
// folder whose permissions are none. The search reads both folders there.
void TestFindGroupFileInPastLockedFolder()
{
    const xsflib::testing::TempFolder temp("ntdlib_rip_archive_test");
    std::filesystem::create_directories(temp.Path() / "a");
    std::filesystem::create_directories(temp.Path() / "b");
    std::vector<std::filesystem::path> order;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(temp.Path()))
    {
        order.push_back(entry.path().filename());
    }

    XSFLIB_CHECK(order.size() == 2);
    if (order.size() != 2)
    {
        return;
    }

    const std::vector<uint8_t> data = {1, 2, 3};
    XSFLIB_CHECK(!xsflib::WriteFile(temp.Path() / order[1] / "GROUP_A.bcgrp", data));
    std::filesystem::permissions(temp.Path() / order[0], std::filesystem::perms::none);
    const std::optional<Chunk> group = rip::FindGroupFileIn<Chunk>(temp.Path(), "GROUP_A", sar::Flavor::kCtr);
    // Removing the folder requires the permissions back.
    std::filesystem::permissions(temp.Path() / order[0], std::filesystem::perms::owner_all);

    XSFLIB_CHECK(group && group->path == xsflib::Utf8(order[1]) + "/GROUP_A.bcgrp" && group->data == data);
}

// The SARC archives in the folder of a sound archive, sorted, and none from a folder below it or with another
// extension.
void TestSarcsBeside()
{
    const std::vector<std::string> paths = {"content/Sound/b.sarc", "content/Sound/sub/c.sarc", "content/Sound/a.sarc",
                                            "content/Sound/d.szs",  "content/e.sarc",           "content/Sound/A.SARC"};
    XSFLIB_CHECK((rip::SarcsBeside(paths, "content/Sound/x.bfsar") ==
                  std::vector<std::string>{"content/Sound/a.sarc", "content/Sound/b.sarc"}));
    XSFLIB_CHECK((rip::SarcsBeside(paths, "x.bfsar").empty()));
    XSFLIB_CHECK((rip::SarcsBeside(std::vector<std::string>{"a.sarc", "b/c.sarc"}, "x.bfsar") ==
                  std::vector<std::string>{"a.sarc"}));
}

// The kind of a sound archive by its first bytes, and its name in messages. An FSAR archive's kind follows its byte
// order mark.
void TestSoundArchiveKinds()
{
    using sar::SoundArchiveKind;
    using sar::SoundArchiveKindName;
    using sar::SoundArchiveKindOf;

    XSFLIB_CHECK(SoundArchiveKindOf("SDAT\xFF\xFE\x00\x01") == SoundArchiveKind::kSdat);
    XSFLIB_CHECK(SoundArchiveKindOf("RSAR\xFE\xFF") == SoundArchiveKind::kRsar);
    XSFLIB_CHECK(SoundArchiveKindOf("CSAR\xFF\xFE") == SoundArchiveKind::kCsar);
    XSFLIB_CHECK(SoundArchiveKindOf(std::string_view("FSAR\xFE\xFF\x00\x14", 8)) == SoundArchiveKind::kFsarBig);
    XSFLIB_CHECK(SoundArchiveKindOf("FSAR\xFF\xFE") == SoundArchiveKind::kFsarLittle);
    XSFLIB_CHECK(SoundArchiveKindOf("FSAR") == SoundArchiveKind::kFsarUnknown);
    XSFLIB_CHECK(SoundArchiveKindOf("FSAR\xFE\xFE") == SoundArchiveKind::kFsarUnknown);
    XSFLIB_CHECK(SoundArchiveKindOf("FSA") == SoundArchiveKind::kOther);
    XSFLIB_CHECK(SoundArchiveKindOf("fsar\xFE\xFF") == SoundArchiveKind::kOther);
    XSFLIB_CHECK(SoundArchiveKindOf("") == SoundArchiveKind::kOther);

    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kSdat) == "a DS sound archive (SDAT)");
    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kRsar) == "a Wii sound archive (RSAR)");
    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kCsar) == "a 3DS sound archive (CSAR)");
    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kFsarBig) == "a Wii U sound archive (big-endian FSAR)");
    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kFsarLittle) == "a Switch sound archive (little-endian FSAR)");
    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kFsarUnknown) == "a Wii U or Switch sound archive (FSAR)");
    XSFLIB_CHECK(SoundArchiveKindName(SoundArchiveKind::kOther).empty());
}

int main()
{
    TestSoundArchiveKinds();
    TestFindGroupFile();
    TestFindGroupFileByFlavor();
    TestFindGroupFileIn();
    TestFindGroupFileInPastLockedFolder();
    TestSarcsBeside();
    TestItems();
    TestWithStates();
    TestArchive();
    TestGroupsInSarcs();

    return xsflib::testing::Summary("rip_archive");
}
