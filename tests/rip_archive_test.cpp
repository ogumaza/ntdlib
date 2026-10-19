// SPDX-License-Identifier: MIT

// Tests the parts of a rip tool that rip sound archives (rip/archive.h): item IDs, and on an archive built with
// sar_files.h the sounds that --only and --bgm pick, the groups kept outside the archive, by name and in SARC archives,
// and the data a sound requires.

#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

using namespace xsflib::testing;

using namespace ntdlib::testing;

namespace
{

namespace rip = ntdlib::rip;
namespace sar = ntdlib::sar;

const sar::Options kStrictRules{sar::Flavor::kNx, {}, false, sar::Rules::kStopAtFirstProblem};

// A file as a rip keeps it: its path and its data.
struct Chunk
{
    std::string path;
    std::vector<uint8_t> data;
};

std::vector<std::string> Paths(const std::vector<Chunk>& chunks)
{
    std::vector<std::string> paths;
    for (const Chunk& chunk : chunks)
    {
        paths.push_back(chunk.path);
    }

    return paths;
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

std::vector<uint8_t> Bytes(const std::string& text)
{
    return {text.begin(), text.end()};
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

int main()
{
    TestItems();
    TestArchive();
    TestGroupsInSarcs();

    return xsflib::testing::Summary("rip_archive");
}
