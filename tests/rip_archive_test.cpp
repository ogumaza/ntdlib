// SPDX-License-Identifier: MIT

// Tests the parts of a rip tool that rip sound archives (rip/archive.h): item IDs, and on archives built with
// sar_files.h the sounds that --only and --bgm pick, the groups kept outside the archive, by name and in SARC archives,
// the data a sound requires, and the part of an archive and of its group files that a library holds.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

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

// A console's flavor, its name in the checks, and the alignment of the files in its archives' FILE blocks: 64 bytes
// from the archive's start on the Switch, and 32 bytes on the 3DS and the Wii U.
struct Console
{
    sar::Flavor flavor;
    const char* name;
    std::size_t alignment;
};

constexpr std::array<Console, 3> kConsoles = {
    {{sar::Flavor::kCtr, "3DS", 32}, {sar::Flavor::kCafe, "Wii U", 32}, {sar::Flavor::kNx, "Switch", 64}}};

sar::Options StrictRules(sar::Flavor flavor)
{
    return {flavor, {}, false, sar::Rules::kStopAtFirstProblem};
}

// `size` bytes that count up from `first`: a file whose bytes the cut copies without reading them.
std::vector<uint8_t> Filler(std::size_t size, uint8_t first)
{
    std::vector<uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; i++)
    {
        bytes[i] = static_cast<uint8_t>(first + i);
    }

    return bytes;
}

// A bank whose waves are wave 0 of each of `wave_archives`, by index.
std::vector<uint8_t> BankOf(sar::Flavor flavor, const std::vector<uint32_t>& wave_archives)
{
    sar_files::BankSpec bank;
    for (const uint32_t wave_archive : wave_archives)
    {
        bank.waves.push_back({sar::ItemId(sar::kWaveArchiveItem, wave_archive), 0});
    }

    return sar_files::Bank(flavor, bank);
}

// A wave archive of one wave of `size` bytes.
std::vector<uint8_t> WaveArchiveOf(sar::Flavor flavor, std::size_t size, uint8_t first)
{
    return sar_files::WaveArchive(flavor, {Filler(size, first)});
}

// The 16-bit and 32-bit fields of `bytes`, an archive of `flavor`, in its byte order.
uint16_t Field16(std::span<const uint8_t> bytes, sar::Flavor flavor, std::size_t at)
{
    return flavor == sar::Flavor::kCafe ? xsflib::LoadBe16(&bytes[at]) : xsflib::LoadLe16(&bytes[at]);
}

uint32_t Field32(std::span<const uint8_t> bytes, sar::Flavor flavor, std::size_t at)
{
    return flavor == sar::Flavor::kCafe ? xsflib::LoadBe32(&bytes[at]) : xsflib::LoadLe32(&bytes[at]);
}

void PutField32(std::span<uint8_t> bytes, sar::Flavor flavor, std::size_t at, uint32_t value)
{
    flavor == sar::Flavor::kCafe ? xsflib::StoreBe32(&bytes[at], value) : xsflib::StoreLe32(&bytes[at], value);
}

// Where the header's table of blocks lists the FILE block, or the end of the table.
std::size_t FileBlockEntry(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    const std::size_t end = 0x14 + std::size_t{12} * Field16(bytes, flavor, 0x10);
    std::size_t entry = 0x14;
    while (entry < end && Field16(bytes, flavor, entry) != 0x2002)
    {
        entry += 12;
    }

    return entry;
}

// The cut of `archive` for `sounds`, read back with the group files beside the archive that the cut keeps. Their
// places among the archive's group files go to `group_files`. Nothing if there's no cut.
std::optional<sar::SoundArchive> CutOf(const sar::SoundArchive& archive, const std::vector<uint32_t>& sounds,
                                       std::vector<std::size_t>* group_files = nullptr)
{
    const std::optional<rip::LibraryArchive> kept = rip::KeepSequenceData(archive, sounds);
    if (!kept)
    {
        return std::nullopt;
    }

    sar::SoundArchive cut = sar::SoundArchive::Load(kept->archive, archive.ReadOptions());
    for (const std::size_t group : kept->group_files)
    {
        cut.AddGroupFile(archive.GroupFiles()[group]);
    }
    if (group_files != nullptr)
    {
        *group_files = kept->group_files;
    }

    return cut;
}

// Whether `cut` holds the files `kept` with the bytes that `archive` gives them, and no bytes for its other files. The
// cut's tables have to give the sounds, banks and wave archives that the archive's tables give. The cut's header and
// FILE block have to give its size. The cut can't be truncated.
bool HoldsOnly(const sar::SoundArchive& archive, const sar::SoundArchive& cut, const std::set<uint32_t>& kept)
{
    bool holds = cut.FileCount() == archive.FileCount() && !cut.Truncated() &&
                 cut.Sounds().size() == archive.Sounds().size() && cut.Banks().size() == archive.Banks().size() &&
                 cut.WaveArchives().size() == archive.WaveArchives().size();
    for (std::size_t i = 0; holds && i < archive.Sounds().size(); i++)
    {
        holds = cut.Sounds()[i].name == archive.Sounds()[i].name &&
                cut.Sounds()[i].file_id == archive.Sounds()[i].file_id &&
                cut.Sounds()[i].banks == archive.Sounds()[i].banks;
    }
    for (uint32_t file = 0; holds && file < archive.FileCount(); file++)
    {
        holds = kept.contains(file)
                    ? !archive.FileData(file).empty() && std::ranges::equal(cut.FileData(file), archive.FileData(file))
                    : cut.FileData(file).empty();
    }

    const std::span<const uint8_t> bytes = cut.Bytes();
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::size_t entry = FileBlockEntry(bytes, flavor);
    const std::size_t file_block = Field32(bytes, flavor, entry + 4);

    return holds && Field32(bytes, flavor, 0x0C) == bytes.size() &&
           Field32(bytes, flavor, entry + 8) == bytes.size() - file_block &&
           Field32(bytes, flavor, file_block + 4) == bytes.size() - file_block;
}

// Whether each of `files` starts on a multiple of `alignment` bytes in `cut`.
bool Aligned(const sar::SoundArchive& cut, const std::set<uint32_t>& files, std::size_t alignment)
{
    const auto aligned = [&](uint32_t file)
    {
        const std::optional<std::size_t> offset = cut.FileOffset(file);
        return offset && *offset % alignment == 0;
    };

    return std::ranges::all_of(files, aligned);
}

// An archive of three sequences and a wave sound. SEQ_A plays BANK_A. SEQ_B plays BANK_B and has an empty slot. SEQ_C
// plays BANK_A and BANK_C. BANK_A's waves are in WARC_A, BANK_B's in WARC_A and WARC_B, and BANK_C's in WARC_C. The
// wave sound's file is file 9.
std::vector<uint8_t> ThreeSequences(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {
        {.name = "SEQ_A", .file = 0, .player = 0, .banks = {0}},
        {.name = "SEQ_B", .file = 1, .player = 0, .banks = {1, 0xFFFFFFFF}},
        {.name = "SEQ_C", .file = 2, .player = 0, .banks = {0, 2}},
        {.name = "WSD_A", .kind = sar_files::SoundSpec::kWaveSound, .file = 9, .player = 0},
    };
    spec.banks = {{"BANK_A", 3}, {"BANK_B", 4}, {"BANK_C", 5}};
    spec.wave_archives = {{"WARC_A", 6}, {"WARC_B", 7}, {"WARC_C", 8}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(flavor, {0xFF}, {})},
                  {sar_files::Sequence(flavor, {0x00, 0xFF}, {})},
                  {sar_files::Sequence(flavor, {0x00, 0x00, 0xFF}, {})},
                  {BankOf(flavor, {0})},
                  {BankOf(flavor, {0, 1})},
                  {BankOf(flavor, {2})},
                  {WaveArchiveOf(flavor, 100, 1)},
                  {WaveArchiveOf(flavor, 200, 2)},
                  {WaveArchiveOf(flavor, 150, 3)},
                  {Filler(80, 4)}};

    return sar_files::Archive(flavor, spec);
}

// The cut keeps each sequence's file, the files of its banks and of their wave archives, and nothing else: the wave
// sound's file goes. A bank or wave archive that two sequences play is kept once. An empty slot keeps nothing. Each
// kept file starts on the console's alignment.
void TestKeepSequenceData()
{
    for (const Console& console : kConsoles)
    {
        const sar::SoundArchive archive =
            sar::SoundArchive::Load(ThreeSequences(console.flavor), StrictRules(console.flavor));
        const std::string name = console.name;

        const std::optional<sar::SoundArchive> a = CutOf(archive, {0});
        const std::optional<sar::SoundArchive> b = CutOf(archive, {1});
        const std::optional<sar::SoundArchive> a_and_c = CutOf(archive, {2, 0});
        const std::optional<sar::SoundArchive> all = CutOf(archive, {0, 1, 2});

        Check(a && HoldsOnly(archive, *a, {0, 3, 6}) && Aligned(*a, {0, 3, 6}, console.alignment),
              name + ": SEQ_A's files");
        Check(b && HoldsOnly(archive, *b, {1, 4, 6, 7}) && Aligned(*b, {1, 4, 6, 7}, console.alignment),
              name + ": SEQ_B's files");
        Check(a_and_c && HoldsOnly(archive, *a_and_c, {0, 2, 3, 5, 6, 8}), name + ": the files of SEQ_A and SEQ_C");
        Check(all && HoldsOnly(archive, *all, {0, 1, 2, 3, 4, 5, 6, 7, 8}) &&
                  Aligned(*all, {0, 1, 2, 3, 4, 5, 6, 7, 8}, console.alignment),
              name + ": the files of every sequence");
        Check(a && a_and_c && a->Bytes().size() < a_and_c->Bytes().size() &&
                  a_and_c->Bytes().size() < archive.Bytes().size(),
              name + ": each cut is smaller");
    }
}

// An archive whose BANK_A, file 2, lies only in GROUP_A's file, file 3. SEQ_B's bank, file 5, has a place in the FILE
// block. GROUP_B's file, file 4, holds a copy of it too.
std::vector<uint8_t> GroupsInArchive(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0, .player = 0, .banks = {0}},
                   {.name = "SEQ_B", .file = 1, .player = 0, .banks = {1}}};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 5}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 3}, {"GROUP_B", 4}};
    spec.files = {{sar_files::Sequence(flavor, {0xFF}, {})},
                  {sar_files::Sequence(flavor, {0x00, 0xFF}, {})},
                  {.group_only = true},
                  {sar_files::Group(flavor, {{2, BankOf(flavor, {})}})},
                  {sar_files::Group(flavor, {{5, BankOf(flavor, {})}})},
                  {BankOf(flavor, {})}};

    return sar_files::Archive(flavor, spec);
}

// A file that only a group's file in the archive holds keeps that whole group's file. A group's file goes when each
// file that it holds a copy of has a place in the FILE block.
void TestKeepGroupsInArchive()
{
    for (const Console& console : kConsoles)
    {
        const sar::SoundArchive archive =
            sar::SoundArchive::Load(GroupsInArchive(console.flavor), StrictRules(console.flavor));
        const std::string name = console.name;

        const std::optional<sar::SoundArchive> a = CutOf(archive, {0});
        const std::optional<sar::SoundArchive> b = CutOf(archive, {1});

        Check(a && HoldsOnly(archive, *a, {0, 2, 3}) && Aligned(*a, {0, 3}, console.alignment),
              name + ": SEQ_A's bank comes with GROUP_A's file");
        Check(b && HoldsOnly(archive, *b, {1, 5}), name + ": SEQ_B's bank comes without a group");
    }
}

// An archive whose banks lie in the group files beside it: GROUP_A holds BANK_A, file 2, and GROUP_B holds BANK_B, file
// 3. GROUP_C holds another copy of BANK_A.
std::vector<uint8_t> GroupsBeside(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0, .player = 0, .banks = {0}},
                   {.name = "SEQ_B", .file = 1, .player = 0, .banks = {1}}};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 3}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 0xFFFFFFFF}, {"GROUP_B", 0xFFFFFFFF}, {"GROUP_C", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(flavor, {0xFF}, {})},
                  {sar_files::Sequence(flavor, {0x00, 0xFF}, {})},
                  {.group_only = true},
                  {.group_only = true}};

    return sar_files::Archive(flavor, spec);
}

// The cut keeps the group files beside the archive that hold the copies that a model reads: for each file, the first
// group file that holds a copy of it. They keep their order.
void TestKeepGroupFilesBeside()
{
    for (const Console& console : kConsoles)
    {
        sar::SoundArchive archive = sar::SoundArchive::Load(GroupsBeside(console.flavor), StrictRules(console.flavor));
        archive.AddGroupFile(sar_files::Group(console.flavor, {{2, BankOf(console.flavor, {})}}));
        archive.AddGroupFile(sar_files::Group(console.flavor, {{3, BankOf(console.flavor, {0})}}));
        archive.AddGroupFile(sar_files::Group(console.flavor, {{2, BankOf(console.flavor, {0, 0})}}));
        const std::string name = console.name;

        std::vector<std::size_t> a_groups;
        std::vector<std::size_t> b_groups;
        std::vector<std::size_t> both_groups;
        const std::optional<sar::SoundArchive> a = CutOf(archive, {0}, &a_groups);
        const std::optional<sar::SoundArchive> b = CutOf(archive, {1}, &b_groups);
        const std::optional<sar::SoundArchive> both = CutOf(archive, {1, 0}, &both_groups);

        Check(a && HoldsOnly(archive, *a, {0, 2}) && a_groups == std::vector<std::size_t>{0},
              name + ": SEQ_A's bank comes with GROUP_A");
        Check(b && HoldsOnly(archive, *b, {1, 3}) && b_groups == std::vector<std::size_t>{1},
              name + ": SEQ_B's bank comes with GROUP_B");
        Check(both && HoldsOnly(archive, *both, {0, 1, 2, 3}) && both_groups == std::vector<std::size_t>{0, 1},
              name + ": both banks come with their groups, in order");
    }
}

// Gives file `file` of `bytes`, an archive of `flavor`, the place of its copy in file `group`, a group's file. The
// file's place then lies inside the group's file. Returns false when the group's file holds no copy of the file, or
// when the file table's place isn't found.
bool PlaceInGroup(std::vector<uint8_t>& bytes, sar::Flavor flavor, uint32_t file, uint32_t group)
{
    const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, StrictRules(flavor));
    const std::span<const uint8_t> data = archive.FileData(file);
    const std::span<const uint8_t> holder = archive.FileData(group);
    const auto copy = std::ranges::search(holder, data);
    if (data.empty() || copy.empty())
    {
        return false;
    }

    // The file table holds the place's offset, then its size. The tables lie before the FILE block.
    const std::size_t file_block = Field32(bytes, flavor, FileBlockEntry(bytes, flavor) + 4);
    const std::size_t files = file_block + 8;
    std::array<uint8_t, 8> place{};
    PutField32(place, flavor, 0, static_cast<uint32_t>(*archive.FileOffset(file) - files));
    PutField32(place, flavor, 4, static_cast<uint32_t>(data.size()));
    const std::span<uint8_t> tables = std::span(bytes).first(file_block);
    const auto at = std::ranges::search(tables, place);
    if (at.empty())
    {
        return false;
    }

    const auto copy_start = static_cast<std::size_t>(copy.begin() - holder.begin());
    PutField32(bytes, flavor, static_cast<std::size_t>(at.begin() - tables.begin()),
               static_cast<uint32_t>(*archive.FileOffset(group) + copy_start - files));

    return true;
}

// An archive whose BANK_A, file 2, takes the place of its copy in GROUP_A's file, file 3. GROUP_A's file also holds a
// copy of BANK_B, file 4. BANK_B has no place. BANK_C, file 5, takes the place of that copy too. GROUP_A doesn't list
// BANK_C, and no sound plays it.
std::vector<uint8_t> FileInGroup(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0, .player = 0, .banks = {0}},
                   {.name = "SEQ_B", .file = 1, .player = 0, .banks = {1}}};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 4}, {"BANK_C", 5}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 3}};
    spec.files = {{sar_files::Sequence(flavor, {0xFF}, {})},
                  {sar_files::Sequence(flavor, {0x00, 0xFF}, {})},
                  {BankOf(flavor, {})},
                  {sar_files::Group(flavor, {{2, BankOf(flavor, {})}, {4, BankOf(flavor, {0})}})},
                  {.group_only = true},
                  {BankOf(flavor, {0})}};

    return sar_files::Archive(flavor, spec);
}

// A file whose place lies inside a group's file keeps only its bytes when the group's file goes. When the group's file
// stays, every file whose place lies inside it keeps its place there, whether the group lists the file or not.
void TestKeepFileInGroup()
{
    for (const Console& console : kConsoles)
    {
        std::vector<uint8_t> bytes = FileInGroup(console.flavor);
        const std::string name = console.name;
        if (!Check(PlaceInGroup(bytes, console.flavor, 2, 3) && PlaceInGroup(bytes, console.flavor, 5, 3),
                   name + ": the places of BANK_A and BANK_C lie in GROUP_A's file"))
        {
            continue;
        }

        const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, StrictRules(console.flavor));
        const auto in_group = [](const sar::SoundArchive& from)
        {
            return from.FileOffset(2) && from.FileOffset(3) ? *from.FileOffset(2) - *from.FileOffset(3) : 0;
        };

        const std::optional<sar::SoundArchive> a = CutOf(archive, {0});
        const std::optional<sar::SoundArchive> b = CutOf(archive, {1});

        Check(in_group(archive) > 0, name + ": BANK_A lies inside GROUP_A's file");
        Check(a && HoldsOnly(archive, *a, {0, 2}) && Aligned(*a, {0, 2}, console.alignment),
              name + ": BANK_A goes without GROUP_A's file");
        Check(b && HoldsOnly(archive, *b, {1, 2, 3, 4, 5}) && in_group(*b) == in_group(archive),
              name + ": BANK_B comes with GROUP_A's file, and BANK_A and BANK_C keep their places inside it");
        Check(a && b && a->Bytes().size() < b->Bytes().size(), name + ": the cut without the group is smaller");
    }
}

// A truncated archive: its last two files, WARC_C and the wave sound's file, end past the archive's end. The cut drops
// their places. The cut isn't truncated.
void TestKeepFromTruncatedArchive()
{
    for (const Console& console : kConsoles)
    {
        std::vector<uint8_t> bytes = ThreeSequences(console.flavor);
        bytes.resize(bytes.size() - 120);
        const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, StrictRules(console.flavor));
        const std::string name = console.name;

        const std::optional<sar::SoundArchive> a = CutOf(archive, {0});

        Check(archive.Truncated() && archive.FileData(8).empty() && archive.FileData(9).empty(),
              name + ": the archive is truncated");
        Check(a && HoldsOnly(archive, *a, {0, 3, 6}), name + ": SEQ_A's files from it");
    }
}

// An archive whose sequence plays a bank that can't be read: the bank's file holds 64 bytes that count up.
std::vector<uint8_t> UnreadableBank(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ", .file = 0, .player = 0, .banks = {0}}};
    spec.banks = {{"BANK", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(flavor, {0xFF}, {})}, {Filler(64, 5)}};

    return sar_files::Archive(flavor, spec);
}

// Without sounds, the cut keeps no file. Its FILE block then ends on the first multiple of the alignment after the
// block's size. A sound that isn't a sequence, or a bank that can't be read, gives no cut.
void TestKeepNothing()
{
    for (const Console& console : kConsoles)
    {
        const sar::SoundArchive archive =
            sar::SoundArchive::Load(ThreeSequences(console.flavor), StrictRules(console.flavor));
        const sar::SoundArchive unreadable_bank =
            sar::SoundArchive::Load(UnreadableBank(console.flavor), StrictRules(console.flavor));
        const std::string name = console.name;

        const std::optional<sar::SoundArchive> none = CutOf(archive, {});

        Check(none && HoldsOnly(archive, *none, {}) && none->Bytes().size() % console.alignment == 0,
              name + ": a cut without sounds");
        Check(!rip::KeepSequenceData(archive, std::vector<uint32_t>{3}), name + ": no cut for a wave sound");
        Check(!rip::KeepSequenceData(archive, std::vector<uint32_t>{0, 4}), name + ": no cut for a sound past the end");
        Check(!rip::KeepSequenceData(unreadable_bank, std::vector<uint32_t>{0}),
              name + ": no cut with a bank that can't be read");
    }
}

// Moves the place that the file table gives file `file` of `bytes`, an archive of `flavor`, to `to` bytes from the
// archive's start. A negative `to` lies before the archive. The tools put every place in the INFO block. Returns false
// when the file table's place isn't found.
bool MovePlace(std::vector<uint8_t>& bytes, sar::Flavor flavor, uint32_t file, int64_t to)
{
    const sar::SoundArchive archive = sar::SoundArchive::Load(bytes, StrictRules(flavor));
    const std::size_t file_block = Field32(bytes, flavor, FileBlockEntry(bytes, flavor) + 4);
    const std::size_t files = file_block + 8;

    // A place holds a reference's type, then the file's offset and its size. A file's entry refers to its place by the
    // place's offset from the entry. The tables lie before the FILE block.
    std::array<uint8_t, 8> offset_and_size{};
    PutField32(offset_and_size, flavor, 0, static_cast<uint32_t>(*archive.FileOffset(file) - files));
    PutField32(offset_and_size, flavor, 4, static_cast<uint32_t>(archive.FileData(file).size()));
    const std::span<uint8_t> tables = std::span(bytes).first(file_block);
    const auto found = std::ranges::search(tables, offset_and_size);
    if (found.empty())
    {
        return false;
    }

    const auto place = static_cast<std::size_t>(found.begin() - tables.begin()) - 4;
    for (std::size_t entry = 0; entry + 8 <= place; entry += 4)
    {
        if (Field16(bytes, flavor, entry) == 0x220C && entry + Field32(bytes, flavor, entry + 4) == place)
        {
            PutField32(bytes, flavor, entry + 4, static_cast<uint32_t>(to - static_cast<int64_t>(entry)));
            return true;
        }
    }

    return false;
}

// A file table whose place for a file lies past the FILE block's start gives no cut. The cut holds the archive's bytes
// up to the FILE block's files, then the files it keeps. The place for the wave sound's file would lie past its end.
void TestKeepRefusesPlaceAmongFiles()
{
    for (const Console& console : kConsoles)
    {
        std::vector<uint8_t> bytes = ThreeSequences(console.flavor);
        const std::string name = console.name;
        const auto at_end = static_cast<int64_t>(bytes.size()) - 12;
        if (!Check(MovePlace(bytes, console.flavor, 9, at_end),
                   name + ": the wave sound's file has its place at the end"))
        {
            continue;
        }

        const sar::SoundArchive archive =
            sar::SoundArchive::Load(bytes, {console.flavor, {}, false, sar::Rules::kReadPastDamage});

        Check(!rip::KeepSequenceData(archive, std::vector<uint32_t>{0}), name + ": no cut");
    }
}

// A file table whose place for a file lies before the archive's start gives no cut either. The place for the wave
// sound's file starts 4 bytes before the archive. Its offset and its size would be the archive's first 8 bytes, and the
// cut would write them before its first byte.
void TestKeepRefusesPlaceBeforeArchive()
{
    for (const Console& console : kConsoles)
    {
        std::vector<uint8_t> bytes = ThreeSequences(console.flavor);
        const std::string name = console.name;
        if (!Check(MovePlace(bytes, console.flavor, 9, -4), name + ": the wave sound's file has its place before it"))
        {
            continue;
        }

        const sar::SoundArchive archive =
            sar::SoundArchive::Load(bytes, {console.flavor, {}, false, sar::Rules::kReadPastDamage});

        Check(!rip::KeepSequenceData(archive, std::vector<uint32_t>{0}), name + ": no cut");
    }
}

// Whether `copy`, a game mode's copy of `archive` (KeepLoadedFiles with reads that end on multiples of 32 bytes), is
// the archive with zeros in place of its FILE block's bytes, apart from those of the files `kept`. Each kept file keeps
// its bytes, rounded out to multiples of 32 bytes from the archive's start. The builder starts each file on a multiple
// of 32 bytes from the FILE block's files, where the tools start it on one from the archive's start. A kept file's
// rounded bytes can therefore take in the edge of the next file. The copy has to read back with the archive's sounds
// and files, and with the bytes of the kept files.
bool ZerosAllBut(const sar::SoundArchive& archive, const std::vector<uint8_t>& copy, const std::set<uint32_t>& kept)
{
    const std::span<const uint8_t> bytes = archive.Bytes();
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::size_t entry = FileBlockEntry(bytes, flavor);
    const std::size_t files = Field32(bytes, flavor, entry + 4) + 8;
    const std::size_t end = std::min<std::size_t>(files - 8 + Field32(bytes, flavor, entry + 8), bytes.size());
    std::vector<uint8_t> expected(bytes.begin(), bytes.end());
    std::fill(expected.begin() + static_cast<std::ptrdiff_t>(files),
              expected.begin() + static_cast<std::ptrdiff_t>(end), uint8_t{0});
    for (const uint32_t file : kept)
    {
        const std::optional<std::size_t> offset = archive.FileOffset(file);
        if (!offset)
        {
            return false;
        }

        const std::size_t start = *offset / 32 * 32;
        const std::size_t stop = std::min((*offset + archive.FileData(file).size() + 31) / 32 * 32, bytes.size());
        std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(stop),
                  expected.begin() + static_cast<std::ptrdiff_t>(start));
    }

    // The zeros in place of the groups' files aren't group files. The copy reads back by the rules that read past
    // damage.
    sar::Options past_damage = archive.ReadOptions();
    past_damage.rules = sar::Rules::kReadPastDamage;
    const sar::SoundArchive read = sar::SoundArchive::Load(copy, past_damage);
    bool holds = copy == expected && copy != std::vector<uint8_t>(bytes.begin(), bytes.end()) &&
                 read.Sounds().size() == archive.Sounds().size() && read.FileCount() == archive.FileCount();
    for (const uint32_t file : kept)
    {
        holds = holds && std::ranges::equal(read.FileData(file), archive.FileData(file));
    }

    return holds;
}

// A game mode's copy keeps each sequence's file and the files of its banks and of their wave archives, each up to the
// first multiple of 32 bytes after its end. It zeros the other files, the wave sound's file among them. A bank or wave
// archive that two sequences play is kept once. An empty slot keeps nothing.
void TestKeepLoadedFiles()
{
    for (const Console& console : kConsoles)
    {
        const sar::SoundArchive archive =
            sar::SoundArchive::Load(ThreeSequences(console.flavor), StrictRules(console.flavor));
        const std::string name = console.name;
        const auto copy = [&archive](const std::vector<uint32_t>& sounds)
        {
            return rip::KeepLoadedFiles(archive, sounds, 32);
        };

        const std::optional<std::vector<uint8_t>> a = copy({0});
        const std::optional<std::vector<uint8_t>> b = copy({1});
        const std::optional<std::vector<uint8_t>> a_and_c = copy({2, 0});
        const std::optional<std::vector<uint8_t>> all = copy({0, 1, 2});

        Check(a && ZerosAllBut(archive, *a, {0, 3, 6}), name + ": SEQ_A's files");
        Check(b && ZerosAllBut(archive, *b, {1, 4, 6, 7}), name + ": SEQ_B's files");
        Check(a_and_c && ZerosAllBut(archive, *a_and_c, {0, 2, 3, 5, 6, 8}), name + ": the files of SEQ_A and SEQ_C");
        Check(all && ZerosAllBut(archive, *all, {0, 1, 2, 3, 4, 5, 6, 7, 8}), name + ": the files of every sequence");
    }
}

// A sound that isn't a sequence, a sound past the end, a bank that can't be read, a bank that only a group's file holds
// and a wave archive past a truncated archive's end each give no copy. A bank with a place of its own gives one, with
// zeros in place of the groups' files. So does a sequence of a truncated archive whose files lie before its end.
void TestKeepLoadedFilesRefuses()
{
    for (const Console& console : kConsoles)
    {
        const sar::SoundArchive archive =
            sar::SoundArchive::Load(ThreeSequences(console.flavor), StrictRules(console.flavor));
        const sar::SoundArchive unreadable_bank =
            sar::SoundArchive::Load(UnreadableBank(console.flavor), StrictRules(console.flavor));
        const sar::SoundArchive groups =
            sar::SoundArchive::Load(GroupsInArchive(console.flavor), StrictRules(console.flavor));
        std::vector<uint8_t> cut_short = ThreeSequences(console.flavor);
        cut_short.resize(cut_short.size() - 120);
        const sar::SoundArchive truncated = sar::SoundArchive::Load(cut_short, StrictRules(console.flavor));
        const std::string name = console.name;
        const auto copy = [](const sar::SoundArchive& from, const std::vector<uint32_t>& sounds)
        {
            return rip::KeepLoadedFiles(from, sounds, 32);
        };

        const std::optional<std::vector<uint8_t>> seq_b = copy(groups, {1});
        const std::optional<std::vector<uint8_t>> seq_a_truncated = copy(truncated, {0});

        Check(!copy(archive, {3}), name + ": no copy for a wave sound");
        Check(!copy(archive, {0, 4}), name + ": no copy for a sound past the end");
        Check(!copy(unreadable_bank, {0}), name + ": no copy with a bank that can't be read");
        Check(!copy(groups, {0}), name + ": no copy with a bank that only a group's file holds");
        Check(seq_b && ZerosAllBut(groups, *seq_b, {1, 5}), name + ": SEQ_B's files, and zeros for the groups' files");
        Check(truncated.Truncated() && !copy(truncated, {2}), name + ": no copy with a wave archive past the end");
        Check(seq_a_truncated && ZerosAllBut(truncated, *seq_a_truncated, {0, 3, 6}),
              name + ": SEQ_A's files from the truncated archive");
    }
}

// An archive whose files all lie in two group files beside it. GROUP_COMMON holds SEQ_A's file 0, BANK_A's file 2,
// WARC_A's file 4 and the wave sound's file 5 (CommonGroup). GROUP_STAGE holds another copy of the wave sound's file,
// SEQ_B's file 1, BANK_B's file 3 and another copy of SEQ_A's file (StageGroup). BANK_A's wave lies in WARC_A.
std::vector<uint8_t> CopiesBeside(sar::Flavor flavor)
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0, .player = 0, .banks = {0}},
                   {.name = "SEQ_B", .file = 1, .player = 0, .banks = {1}},
                   {.name = "WSD_A", .kind = sar_files::SoundSpec::kWaveSound, .file = 5, .player = 0}};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 3}};
    spec.wave_archives = {{"WARC_A", 4}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_COMMON", sar::kExternalGroupFile}, {"GROUP_STAGE", sar::kExternalGroupFile}};
    spec.files = std::vector<sar_files::FileSpec>(6, {.group_only = true});

    return sar_files::Archive(flavor, spec);
}

void PutField16(std::span<uint8_t> bytes, sar::Flavor flavor, std::size_t at, uint16_t value)
{
    flavor == sar::Flavor::kCafe ? xsflib::StoreBe16(&bytes[at], value) : xsflib::StoreLe16(&bytes[at], value);
}

// The place and the size of the block of type `type` that the table of blocks of `bytes`, a file of `flavor`, lists,
// or nothing.
std::optional<std::pair<std::size_t, std::size_t>> BlockIn(std::span<const uint8_t> bytes, sar::Flavor flavor,
                                                           uint16_t type)
{
    for (std::size_t entry = 0x14; entry < 0x14 + std::size_t{12} * Field16(bytes, flavor, 0x10); entry += 12)
    {
        if (Field16(bytes, flavor, entry) == type)
        {
            return std::pair<std::size_t, std::size_t>{Field32(bytes, flavor, entry + 4),
                                                       Field32(bytes, flavor, entry + 8)};
        }
    }

    return std::nullopt;
}

// `group`, a group file of `flavor`, with an INFX block after its FILE block, as the tools write one. The builder
// leaves room in the header for a third block's entry.
std::vector<uint8_t> WithInfx(std::vector<uint8_t> group, sar::Flavor flavor)
{
    const std::size_t start = group.size();
    const std::vector<uint8_t> magic = Bytes("INFX");
    group.insert(group.end(), magic.begin(), magic.end());
    group.resize(start + 24);
    PutField32(group, flavor, start + 4, 24);
    PutField32(group, flavor, start + 8, 1);
    PutField32(group, flavor, start + 16, sar::ItemId(sar::kSoundItem, 0));
    PutField16(group, flavor, 0x10, 3);
    PutField16(group, flavor, 0x2C, 0x7802);
    PutField32(group, flavor, 0x30, static_cast<uint32_t>(start));
    PutField32(group, flavor, 0x34, 24);
    PutField32(group, flavor, 0x0C, static_cast<uint32_t>(group.size()));

    return group;
}

std::vector<uint8_t> CommonGroup(sar::Flavor flavor)
{
    return WithInfx(sar_files::Group(flavor, {{0, sar_files::Sequence(flavor, {0xFF}, {})},
                                              {2, BankOf(flavor, {0})},
                                              {4, WaveArchiveOf(flavor, 300, 5)},
                                              {5, Filler(900, 6)}}),
                    flavor);
}

std::vector<uint8_t> StageGroup(sar::Flavor flavor)
{
    return sar_files::Group(flavor, {{5, Filler(900, 7)},
                                     {1, sar_files::Sequence(flavor, {0x00, 0xFF}, {})},
                                     {3, BankOf(flavor, {})},
                                     {0, sar_files::Sequence(flavor, {0xFF}, {})}});
}

// An item of a group file: its file ID, the type of its reference, and the offset and the size that it gives.
struct GroupItem
{
    friend bool operator==(const GroupItem&, const GroupItem&) = default;

    uint32_t file = 0;
    uint16_t type = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// The items of `group`, a group file of `flavor`.
std::vector<GroupItem> ItemsOf(std::span<const uint8_t> group, sar::Flavor flavor)
{
    const std::size_t table = BlockIn(group, flavor, 0x7800).value().first + 8;
    std::vector<GroupItem> items;
    for (uint32_t i = 0; i < Field32(group, flavor, table); i++)
    {
        const std::size_t item = table + Field32(group, flavor, table + 4 + i * 8 + 4);
        items.push_back({Field32(group, flavor, item), Field16(group, flavor, item + 4),
                         Field32(group, flavor, item + 8), Field32(group, flavor, item + 12)});
    }

    return items;
}

// Whether `cut`, a cut of `group` (a group file of `flavor`), keeps the copies of the files `kept` and links the rest.
// Each kept copy keeps its bytes and starts on a multiple of `alignment` bytes. The header and the FILE block give the
// new sizes. The blocks after the FILE block follow it with their bytes.
bool KeepsCopies(std::span<const uint8_t> group, std::span<const uint8_t> cut, sar::Flavor flavor,
                 const std::set<uint32_t>& kept, std::size_t alignment)
{
    const auto file_block = BlockIn(cut, flavor, 0x7801);
    const auto whole_file_block = BlockIn(group, flavor, 0x7801);
    if (!file_block || !whole_file_block || ItemsOf(cut, flavor).size() != ItemsOf(group, flavor).size())
    {
        return false;
    }

    const std::size_t data = file_block->first + 8;
    const std::size_t file_end = file_block->first + file_block->second;
    bool keeps = Field32(cut, flavor, 0x0C) == cut.size() &&
                 Field32(cut, flavor, file_block->first + 4) == file_block->second && file_end % alignment == 0 &&
                 cut.size() < group.size();
    const std::vector<GroupItem> items = ItemsOf(cut, flavor);
    const std::vector<GroupItem> whole_items = ItemsOf(group, flavor);
    for (std::size_t i = 0; keeps && i < items.size(); i++)
    {
        const GroupItem& item = items[i];
        const GroupItem& whole = whole_items[i];
        if (!kept.contains(item.file) ||
            std::ranges::any_of(std::span(whole_items).first(i),
                                [&](const GroupItem& before) { return before.file == item.file; }))
        {
            keeps = item == GroupItem{whole.file, 0, 0xFFFFFFFF, 0xFFFFFFFF};
            continue;
        }

        keeps = item.type == 0x1F00 && item.size == whole.size && (data + item.offset) % alignment == 0 &&
                data + item.offset + item.size <= file_end &&
                std::ranges::equal(cut.subspan(data + item.offset, item.size),
                                   group.subspan(whole_file_block->first + 8 + whole.offset, whole.size));
    }

    // The blocks after the FILE block keep their bytes.
    const auto infx = BlockIn(group, flavor, 0x7802);
    const auto cut_infx = BlockIn(cut, flavor, 0x7802);
    return keeps && (!infx || (cut_infx && cut_infx->first == file_end && cut_infx->second == infx->second &&
                               std::ranges::equal(cut.subspan(cut_infx->first, cut_infx->second),
                                                  group.subspan(infx->first, infx->second))));
}

// The cut of `archive` for `sounds`, with the cut of the group files beside it that the cut keeps.
struct CutWithGroups
{
    rip::LibraryArchive library;
    std::vector<std::vector<uint8_t>> groups;
};

std::optional<CutWithGroups> CutWithGroupsOf(const sar::SoundArchive& archive, const std::vector<uint32_t>& sounds)
{
    std::optional<rip::LibraryArchive> library = rip::KeepSequenceData(archive, sounds);
    std::optional<std::vector<std::vector<uint8_t>>> groups =
        library ? rip::KeepGroupCopies(archive, *library, sounds) : std::nullopt;
    if (!groups)
    {
        return std::nullopt;
    }

    return CutWithGroups{std::move(*library), std::move(*groups)};
}

// Whether `cut` reads back with the bytes that `archive` gives the files `kept`, and no bytes for its other files.
bool ReadsBack(const sar::SoundArchive& archive, const CutWithGroups& cut, const std::set<uint32_t>& kept)
{
    sar::SoundArchive read = sar::SoundArchive::Load(cut.library.archive, archive.ReadOptions());
    for (const std::vector<uint8_t>& group : cut.groups)
    {
        read.AddGroupFile(group);
    }

    bool holds = read.FileCount() == archive.FileCount();
    for (uint32_t file = 0; holds && file < archive.FileCount(); file++)
    {
        holds = kept.contains(file) ? std::ranges::equal(read.FileData(file), archive.FileData(file))
                                    : read.FileData(file).empty();
    }

    return holds;
}

// A 3DS archive lists its groups. A rip finds the groups kept outside it by their names.
void TestGroupsOfThe3ds()
{
    const sar::Options options = StrictRules(sar::Flavor::kCtr);
    const sar::SoundArchive archive = sar::SoundArchive::Load(CopiesBeside(sar::Flavor::kCtr), options);
    XSFLIB_CHECK((rip::ExternalGroups(archive) == std::vector<std::string>{"GROUP_COMMON", "GROUP_STAGE"}));

    const auto find = [](const std::string& name)
    {
        return std::optional<Chunk>(Chunk{sar::GroupFileName(name, sar::Flavor::kCtr), {}});
    };
    XSFLIB_CHECK((Paths(rip::FindGroups<Chunk>(CopiesBeside(sar::Flavor::kCtr), find, options)) ==
                  std::vector<std::string>{"GROUP_COMMON.bcgrp", "GROUP_STAGE.bcgrp"}));
}

// The group files that a library holds keep the copies that a model reads: for each file, the first copy among the
// group files. Every other copy becomes a link to the archive's file, and the INFX block follows the FILE block. Each
// cut reads back with the library's archive.
void TestKeepGroupCopies()
{
    for (const Console& console : kConsoles)
    {
        const sar::Flavor flavor = console.flavor;
        sar::SoundArchive archive = sar::SoundArchive::Load(CopiesBeside(flavor), StrictRules(flavor));
        archive.AddGroupFile(CommonGroup(flavor));
        archive.AddGroupFile(StageGroup(flavor));
        const std::string name = console.name;

        const std::optional<CutWithGroups> a = CutWithGroupsOf(archive, {0});
        const std::optional<CutWithGroups> b = CutWithGroupsOf(archive, {1});
        const std::optional<CutWithGroups> both = CutWithGroupsOf(archive, {1, 0});

        Check(a && a->library.group_files == std::vector<std::size_t>{0} &&
                  KeepsCopies(CommonGroup(flavor), a->groups.at(0), flavor, {0, 2, 4}, console.alignment) &&
                  ReadsBack(archive, *a, {0, 2, 4}),
              name + ": SEQ_A's files come from GROUP_COMMON, without the wave sound's");
        Check(b && b->library.group_files == std::vector<std::size_t>{1} &&
                  KeepsCopies(StageGroup(flavor), b->groups.at(0), flavor, {1, 3}, console.alignment) &&
                  ReadsBack(archive, *b, {1, 3}),
              name + ": SEQ_B's files come from GROUP_STAGE, without the other copies");
        Check(both && both->library.group_files == std::vector<std::size_t>{0, 1} &&
                  KeepsCopies(CommonGroup(flavor), both->groups.at(0), flavor, {0, 2, 4}, console.alignment) &&
                  KeepsCopies(StageGroup(flavor), both->groups.at(1), flavor, {1, 3}, console.alignment) &&
                  ReadsBack(archive, *both, {0, 1, 2, 3, 4}),
              name + ": both sequences' files, each from the first group file that holds it");

        // A block that overlaps the FILE block gives no cut.
        std::vector<uint8_t> overlapping = CommonGroup(flavor);
        PutField32(overlapping, flavor, 0x30, BlockIn(overlapping, flavor, 0x7801).value().first + 8);
        sar::SoundArchive with_overlap = sar::SoundArchive::Load(CopiesBeside(flavor), StrictRules(flavor));
        with_overlap.AddGroupFile(overlapping);
        const std::optional<rip::LibraryArchive> library =
            rip::KeepSequenceData(with_overlap, std::vector<uint32_t>{0});
        Check(library && !rip::KeepGroupCopies(with_overlap, *library, std::vector<uint32_t>{0}),
              name + ": no cut of a group file whose INFX block overlaps its FILE block");
    }
}

} // namespace

int main()
{
    TestItems();
    TestArchive();
    TestGroupsInSarcs();
    TestKeepSequenceData();
    TestKeepGroupsInArchive();
    TestKeepGroupFilesBeside();
    TestKeepFileInGroup();
    TestKeepFromTruncatedArchive();
    TestKeepNothing();
    TestKeepRefusesPlaceAmongFiles();
    TestKeepRefusesPlaceBeforeArchive();
    TestKeepLoadedFiles();
    TestKeepLoadedFilesRefuses();
    TestGroupsOfThe3ds();
    TestKeepGroupCopies();

    return xsflib::testing::Summary("rip_archive");
}
