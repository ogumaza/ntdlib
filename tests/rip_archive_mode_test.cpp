// SPDX-License-Identifier: MIT

// Tests the archive mode of rip tools (rip/archive_mode.h) with a made-up rip format, on archives built with
// sar_files.h in a temporary folder: the search for a loose archive's group files past an entry whose type can't be
// read, the line for a loose archive without sequences, a game's rip that goes on past an archive whose rip throws,
// the files that a library holds, a second rip into the folder of the first, the lines of --list for a game's
// archives, the warning for a group file that isn't there, and the states that a rip plays a sound in.
// rip_archive_mode_tags_test.cpp tests the minis' tags.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rip/archive_mode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "rip_archive_mode_test_util.h"
#include "temp_folder.h"

namespace
{

using namespace rip_archive_mode_tests;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

// A Switch archive with one sequence, SEQ. Its bank, file 1, is only in group GROUP_A. The group file is outside the
// archive.
std::vector<uint8_t> ArchiveWithGroup()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ", .file = 0, .player = 0, .banks = {0}}};
    spec.banks = {{"BANK_A", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(false, {0xFF}, {}), "", false}, {{}, "", true}};

    return sar_files::Archive(false, spec);
}

// A Switch archive with one sequence, SEQ, and no groups.
std::vector<uint8_t> ArchiveWithoutGroups()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ", .file = 0, .player = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{sar_files::Sequence(false, {0xFF}, {}), "", false}};

    return sar_files::Archive(false, spec);
}

// A Switch archive with one stream, STRM, and no sequences. The stream's file is outside the archive.
std::vector<uint8_t> ArchiveWithoutSequences()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "STRM", .kind = sar_files::SoundSpec::kStream, .file = 0, .player = 0}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {{{}, "stream/STRM.bfstm", false}};

    return sar_files::Archive(false, spec);
}

// GROUP_A's file: a copy of file 1, a bank without instruments.
std::vector<uint8_t> GroupA()
{
    return sar_files::Group(false, {{1, sar_files::Bank(false, sar_files::BankSpec{})}});
}

// Writes a loose archive and its group file into `folder`, and gives the archive's path.
fs::path WriteLooseArchive(const TempFolder& folder)
{
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithGroup());
    folder.Write("GROUP_A.bfgrp", GroupA());

    return input;
}

// Rips `input`, a loose archive in `folder`, into the folder's out folder. Gives whether the rip succeeded and SEQ
// ripped with its bank. Only the group file holds the bank.
bool RipsWithGroup(const TempFolder& folder, const fs::path& input)
{
    const fs::path out = folder.Path() / "out";
    xsflib::rip::FileNames names(".tstlib", ".minitst");
    const int result =
        rip::RipLooseArchive(TestTool(), input.string(), out, NoLengths(), names, TestRip{}, NoDescription);

    return result == 0 && fs::exists(out / "loose.tstlib") && fs::exists(out / "0000 SEQ.minitst");
}

// A loose archive's group file is found past an entry whose type can't be read: a broken symbolic link here. Without
// the group file, SEQ would have no bank and would rip nothing. On a system that doesn't let the test make the link,
// such as Windows without developer mode, the rip runs without the link.
void TestLooseArchiveGroupPastBrokenLink()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = WriteLooseArchive(folder);
    std::error_code ignored;
    fs::create_symlink(folder.Path() / "missing.bfgrp", folder.Path() / "old.bfgrp", ignored);

    Check(RipsWithGroup(folder, input), "SEQ rips with its bank from the group file past the broken link");
}

// A loose archive's group file is found past folders that can't be opened. The walk goes on past each one. Without the
// group file, SEQ would have no bank and would rip nothing.
void TestLooseArchiveGroupPastLockedFolders()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = WriteLooseArchive(folder);
    const ntdlib::testing::LockedFolders locked(
        {folder.Path() / "0_locked", folder.Path() / "m_locked", folder.Path() / "z_locked"});

    Check(RipsWithGroup(folder, input), "SEQ rips with its bank from the group file past the locked folders");
}

// A loose archive without sequences rips nothing, and the rip says so in a line of its own. The exit code is 0. With
// --bgm, the rip reports instead that no sequence is music, and the exit code is 1.
void TestLooseArchiveWithoutSequences()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("streams.bfsar", ArchiveWithoutSequences());
    const fs::path out = folder.Path() / "out";
    xsflib::cli::RipOptions bgm = NoLengths();
    bgm.bgm = true;
    int result = -1;
    int bgm_result = -1;
    const auto rip_archive = [&]
    {
        xsflib::rip::FileNames names(".tstlib", ".minitst");
        result = rip::RipLooseArchive(TestTool(), input.string(), out, NoLengths(), names, TestRip{}, NoDescription);
    };
    const auto rip_music = [&]
    {
        xsflib::rip::FileNames names(".tstlib", ".minitst");
        bgm_result = rip::RipLooseArchive(TestTool(), input.string(), out, bgm, names, TestRip{}, NoDescription);
    };

    const std::string messages = OutputOf(stderr, rip_archive);
    const std::string bgm_messages = OutputOf(stderr, rip_music);

    Check(result == 0 && messages == "streams.bfsar: no sequences to rip\n",
          "the rip of an archive without sequences says so: " + messages);
    Check(bgm_result == 1 && bgm_messages == "error: no sequence is music (--bgm)\n",
          "with --bgm, the rip reports that no sequence is music: " + bgm_messages);
    Check(!fs::exists(out), "neither rip writes a file");
}

// A game's rip goes on past an archive whose rip throws. The first archive's group file is damaged. AddGroupFile throws
// a FormatError for it. The rip reports the archive, counts it as failed and rips the second archive. The exit code is
// then 1. An archive that can't be read gives 1 too.
void TestGameRipGoesOnPastAnError()
{
    const TempFolder folder("rip_archive_mode_test");
    const std::map<std::string, std::vector<uint8_t>> files = {{"Sound/broken.bfsar", ArchiveWithGroup()},
                                                               {"Sound/whole.bfsar", ArchiveWithoutGroups()}};

    const auto read_file = [&files](const std::string& path, std::vector<uint8_t>& bytes)
    {
        const auto file = files.find(path);
        if (file == files.end())
        {
            return false;
        }

        bytes = file->second;

        return true;
    };

    const auto find_group = [](const std::string& name) -> std::optional<TestFormat::FileChunk>
    {
        return TestFormat::FileChunk{name + ".bfgrp", {'d', 'a', 'm', 'a', 'g', 'e', 'd'}};
    };

    xsflib::cli::FileNameRegistries registries;
    int result = -1;
    const auto rip_game = [&]
    {
        result = rip::RipArchives(TestTool(), "game", folder.Path(), {"Sound/broken.bfsar", "Sound/whole.bfsar"},
                                  TestRip{}, read_file, find_group, "Game", NoLengths(), registries);
    };

    const std::string escaped = xsflib::testing::ErrorOf(rip_game);

    Check(escaped.empty(), "no error escapes the game's rip: " + escaped);
    Check(result == 1, "the game's rip exits with 1");
    Check(!fs::exists(folder.Path() / "broken"), "the archive whose rip throws writes nothing");
    // FileNames replaces the space in the library's name, "Game whole", with '_'.
    Check(fs::exists(folder.Path() / "whole" / "Game_whole.tstlib") &&
              fs::exists(folder.Path() / "whole" / "0000 SEQ.minitst"),
          "the next archive rips");
}

// The reserved area of the mini at `path`, or "unreadable".
std::vector<uint8_t> ReservedOf(const fs::path& path)
{
    std::vector<uint8_t> bytes;
    xsflib::psf::PsfFile file;
    if (!xsflib::psf::ReadWholeFile(path.string(), bytes) || xsflib::psf::ParsePsf(bytes, file, false))
    {
        return {'u', 'n', 'r', 'e', 'a', 'd', 'a', 'b', 'l', 'e'};
    }

    return file.reserved;
}

// sound_states gives each state of a sound a mini of its own, after the sound's, with the track volumes of the state.
// --only picks the states by their labels: a sound without its later states, or a later state alone. The state at the
// start keeps the sound's label. mini_reserved gives each job's mini its reserved area. By default, a sound has one
// state, with every track at 1, and its mini has no reserved area.
void TestSoundStates()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;

    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    std::vector<std::pair<std::string, float>> reserved_for;
    tool.sound_states = [](const TestRip&, const std::vector<rip::Job>& selected)
    {
        std::vector<rip::Job> jobs;
        for (const rip::Job& job : selected)
        {
            jobs.push_back(job);
            if (job.label == "SEQ_A")
            {
                jobs.back().track_volumes[1] = 0.0f;
                rip::Job later = job;
                later.label = "SEQ_A later";
                later.track_volumes[0] = 0.5f;
                jobs.push_back(later);
            }
        }

        return jobs;
    };
    tool.mini_reserved = [&reserved_for](const TestRip&, const rip::Job& job)
    {
        reserved_for.emplace_back(job.label, job.track_volumes[0] + 2.0f * job.track_volumes[1]);
        return std::vector<uint8_t>(job.label.begin(), job.label.end());
    };

    RipLoose(tool, input, "states", "", messages);
    RipLoose(tool, input, "only", "SEQ_A$", messages);
    RipLoose(tool, input, "later", "later$", messages);
    RipLoose(TestTool(), input, "default", "", messages);

    const fs::path states = folder.Path() / "states";
    const std::vector<std::pair<std::string, float>> expected = {
        {"SEQ_A", 1.0f}, {"SEQ_A later", 2.5f}, {"SEQ_B", 3.0f}, {"SEQ_A", 1.0f}, {"SEQ_A later", 2.5f}};
    Check(reserved_for == expected, "the jobs' states, in order: each sound's start, then its later states");
    Check(ReservedOf(states / "0000 SEQ_A.minitst") == std::vector<uint8_t>{'S', 'E', 'Q', '_', 'A'} &&
              ReservedOf(states / "0000 SEQ_A_later.minitst").size() == 11 &&
              ReservedOf(states / "0001 SEQ_B.minitst").size() == 5,
          "each mini has its job's reserved area");
    Check(fs::exists(folder.Path() / "only" / "0000 SEQ_A.minitst") &&
              !fs::exists(folder.Path() / "only" / "0000 SEQ_A_later.minitst"),
          "a later state whose label doesn't match --only isn't ripped");
    Check(fs::exists(folder.Path() / "later" / "0000 SEQ_A_later.minitst") &&
              !fs::exists(folder.Path() / "later" / "0000 SEQ_A.minitst"),
          "--only can pick a later state alone");
    Check(ReservedOf(folder.Path() / "default" / "0000 SEQ_A.minitst").empty() &&
              !fs::exists(folder.Path() / "default" / "0000 SEQ_A_later.minitst"),
          "by default, a sound has one state, and its mini no reserved area");
}

// A tool that cuts each library's archive down to the files that its sequences play (KeepSequenceData).
rip::ArchiveRipTool<TestFormat, TestRip> CuttingTool()
{
    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    tool.keeps_sequence_data = [](const TestRip&)
    {
        return true;
    };

    return tool;
}

// With keeps_sequence_data, a library holds only the files that its sequences play, and the group files that hold the
// copies of any of them. By default, a library holds the whole archive and every group file found.
void TestLibraryKeepsSequenceData()
{
    const TempFolder folder("rip_archive_mode_test");
    const std::vector<uint8_t> archive = ArchiveWithTwoGroups();
    const fs::path input = folder.Write("loose.bfsar", archive);
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    const sar::Options options = TestTool().sar_options;
    std::string messages;

    const auto whole = RipLoose(TestTool(), input, "whole", "", messages);
    const auto both = RipLoose(CuttingTool(), input, "both", "", messages);
    const auto b = RipLoose(CuttingTool(), input, "b", "^SEQ_B$", messages);

    Check(NamesOf(whole) == std::vector<std::string>{"GROUP_A.bfgrp", "GROUP_B.bfgrp", "loose.bfsar"} &&
              whole.at("loose.bfsar") == archive,
          "by default, the library holds the whole archive and both group files");
    Check(
        NamesOf(both) == std::vector<std::string>{"GROUP_A.bfgrp", "loose.bfsar"} && both.at("loose.bfsar") != archive,
        "the library of both sequences holds a cut archive and GROUP_A only");
    Check(NamesOf(b) == std::vector<std::string>{"loose.bfsar"}, "SEQ_B's library holds no group file");
    Check(messages.find("warning") == std::string::npos, "no warning: " + messages);
    if (NamesOf(both).size() != 2 || NamesOf(b).size() != 1)
    {
        return;
    }

    // The cut archives give the sequences' files with the bytes of the whole archive.
    sar::SoundArchive old_archive = sar::SoundArchive::Load(archive, options);
    old_archive.AddGroupFile(GroupOf(2));
    old_archive.AddGroupFile(GroupOf(3));
    sar::SoundArchive both_archive = sar::SoundArchive::Load(both.at("loose.bfsar"), options);
    both_archive.AddGroupFile(both.at("GROUP_A.bfgrp"));
    const sar::SoundArchive b_archive = sar::SoundArchive::Load(b.at("loose.bfsar"), options);
    const auto same = [&](const sar::SoundArchive& cut, uint32_t file)
    {
        return !cut.FileData(file).empty() && std::ranges::equal(cut.FileData(file), old_archive.FileData(file));
    };
    Check(same(both_archive, 0) && same(both_archive, 1) && same(both_archive, 2) && same(both_archive, 3),
          "the files of both sequences");
    Check(same(b_archive, 1) && same(b_archive, 3) && b_archive.FileData(0).empty() && b_archive.FileData(2).empty(),
          "SEQ_B's files, and none of SEQ_A's");
}

// A library also keeps the files of the sounds that played_sounds names: here SEQ_A. SEQ_B starts SEQ_A. With --only
// SEQ_B, the library is then the same as that of both sequences.
void TestLibraryKeepsPlayedSounds()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    rip::ArchiveRipTool<TestFormat, TestRip> tool = CuttingTool();
    tool.played_sounds = [](const TestRip&, const sar::SoundArchive&, const std::vector<rip::Job>& jobs)
    {
        const bool b_alone = jobs.size() == 1 && jobs[0].index == 1;
        return b_alone ? std::vector<uint32_t>{0} : std::vector<uint32_t>{};
    };
    std::string messages;

    const auto both = RipLoose(CuttingTool(), input, "both", "", messages);
    const auto b = RipLoose(tool, input, "b", "^SEQ_B$", messages);

    Check(NamesOf(b) == std::vector<std::string>{"GROUP_A.bfgrp", "loose.bfsar"} && b == both,
          "SEQ_B's library keeps the files of SEQ_A, which SEQ_B plays");
}

// build_library gets the indices of the sequences that the rip writes minis for, in the archive's order. The default
// library holds the whole archive and every group file found, whichever sequences the rip writes.
void TestLibraryGetsRippedSounds()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;

    std::vector<std::vector<uint32_t>> received;
    rip::ArchiveRipTool<TestFormat, TestRip> tool = TestTool();
    const auto default_library = tool.build_library;
    tool.build_library = [&received, default_library](const TestRip& to_rip, std::span<const uint32_t> sounds,
                                                      const xsflib::psf::Tags& tags)
    {
        received.emplace_back(sounds.begin(), sounds.end());
        return default_library(to_rip, sounds, tags);
    };

    const auto both = RipLoose(tool, input, "both", "", messages);
    const auto b = RipLoose(tool, input, "b", "^SEQ_B$", messages);
    const auto whole = RipLoose(TestTool(), input, "whole", "", messages);

    Check(received == std::vector<std::vector<uint32_t>>{{0, 1}, {1}},
          "build_library gets SEQ_A and SEQ_B, then SEQ_B alone");
    Check(NamesOf(whole).size() == 3 && both == whole && b == whole,
          "the default library doesn't depend on the sequences");
}

// The length analysis takes the archive. The cut then reads the archive again, with the group files, and the library
// holds what it holds without the analysis.
void TestCutAfterLengthAnalysis()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    std::string messages;

    rip::ArchiveRipTool<TestFormat, TestRip> tool = CuttingTool();
    std::size_t analyzed = 0;
    tool.analyze_lengths =
        [&analyzed](std::vector<rip::Job>& jobs, sar::SoundArchive, const TestFormat::ArchiveDescriptor&, unsigned)
    {
        for (rip::Job& job : jobs)
        {
            job.timing = EndedAt50Ms();
            analyzed++;
        }
    };

    const auto without = RipLoose(CuttingTool(), input, "without", "", messages);
    const auto with = RipLoose(tool, input, "with", "", messages, true);

    Check(analyzed == 2, "the analysis took both sequences");
    Check(NamesOf(with) == std::vector<std::string>{"GROUP_A.bfgrp", "loose.bfsar"} && with == without,
          "the library holds the cut archive and GROUP_A, as without the analysis");
    Check(messages.find("warning") == std::string::npos, "no warning: " + messages);
}

// A second rip of fewer sequences into the folder of the first keeps the first rip's library. The second rip's library
// holds other bytes. It takes the next free name, and the second rip's mini names it.
void TestSecondRipKeepsFirstLibrary()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithTwoGroups());
    folder.Write("GROUP_A.bfgrp", GroupOf(2));
    folder.Write("GROUP_B.bfgrp", GroupOf(3));
    const fs::path out = folder.Path() / "out";
    std::string messages;
    RipLoose(CuttingTool(), input, "out", "", messages);
    std::vector<uint8_t> first;
    xsflib::psf::ReadWholeFile((out / "loose.tstlib").string(), first);

    RipLoose(CuttingTool(), input, "out", "^SEQ_B$", messages);

    std::vector<uint8_t> after;
    xsflib::psf::ReadWholeFile((out / "loose.tstlib").string(), after);
    Check(!first.empty() && after == first, "the first rip's library stays as it was");
    Check(NamesOf(LibraryFiles(out / "loose_2.tstlib")) == std::vector<std::string>{"loose.bfsar"},
          "the second rip's library takes the next name and holds SEQ_B's cut archive");
    Check(LibraryTag(out / "0000 SEQ_A.minitst") == "loose.tstlib", "SEQ_A's mini names the first library");
    Check(LibraryTag(out / "0001 SEQ_B.minitst") == "loose_2.tstlib", "SEQ_B's mini names the second library");
    Check(messages.find("wrote loose_2.tstlib") != std::string::npos, "the second rip's message: " + messages);

    // Every mini in the folder names a library there.
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(out, ec))
    {
        if (entry.path().extension() == ".minitst")
        {
            const std::string lib = LibraryTag(entry.path());
            Check(!lib.empty() && fs::exists(out / lib), "the library of " + entry.path().filename().string());
        }
    }
}

// ArchiveWithoutGroups with its INFO block moved past its FILE block. The FILE block then doesn't follow the archive's
// other blocks.
std::vector<uint8_t> InfoPastFiles()
{
    std::vector<uint8_t> bytes = ArchiveWithoutGroups();
    if (const std::optional<std::size_t> entry = sar_files::FindBlockEntry(bytes, sar::Flavor::kNx, 0x2001))
    {
        const uint32_t offset = xsflib::LoadLe32(&bytes[*entry + 4]);
        const uint32_t size = xsflib::LoadLe32(&bytes[*entry + 8]);
        const std::vector<uint8_t> info(bytes.begin() + offset, bytes.begin() + offset + size);
        xsflib::StoreLe32(&bytes[*entry + 4], static_cast<uint32_t>(bytes.size()));
        bytes.insert(bytes.end(), info.begin(), info.end());
    }

    xsflib::StoreLe32(&bytes[sar_files::kFileSizeField], static_cast<uint32_t>(bytes.size()));

    return bytes;
}

// A library whose sequences' files can't be cut out of its archive holds the whole archive, after a warning.
void TestWholeArchiveWhenFilesCantBeCut()
{
    const TempFolder folder("rip_archive_mode_test");
    const std::vector<uint8_t> archive = InfoPastFiles();
    const fs::path input = folder.Write("moved.bfsar", archive);
    std::string messages;

    const auto files = RipLoose(CuttingTool(), input, "out", "", messages);

    Check(sar::SoundArchive::Load(archive, TestTool().sar_options).Sounds().size() == 1, "the archive can be read");
    Check(NamesOf(files) == std::vector<std::string>{"moved.bfsar"} && files.at("moved.bfsar") == archive,
          "the library holds the whole archive");
    Check(messages.find("warning: the files that the sequences play can't be cut out of moved.bfsar. The library "
                        "holds the whole archive\n") != std::string::npos,
          "the warning: " + messages);
}

// --list prints a line for each of a game's archives: its path, its size and what it holds, or that it can't be read.
void TestListArchives()
{
    const std::vector<uint8_t> archive = ArchiveWithoutGroups();
    const auto read_file = [&archive](const std::string& path, std::vector<uint8_t>& bytes)
    {
        if (path != "Sound/whole.bfsar")
        {
            return false;
        }

        bytes = archive;

        return true;
    };
    const auto describe = [](std::vector<uint8_t> bytes)
    {
        return "described in " + std::to_string(bytes.size()) + " bytes";
    };
    const auto list = [&]
    {
        rip::ListArchives({"Sound/whole.bfsar", "Sound/gone.bfsar"}, read_file, describe);
    };

    const std::string lines = OutputOf(stdout, list);

    const std::string size = std::to_string(archive.size());
    Check(lines == "  Sound/whole.bfsar: " + size + " bytes, described in " + size + " bytes\n" +
                       "  Sound/gone.bfsar: 0 bytes, can't be read\n",
          "the lines of a game's archives: " + lines);
}

// The warning for a group that isn't found names the group's file as the archive's console names it: .bfgrp on the Wii
// U and the Switch, and .bcgrp on the 3DS.
void TestMissingGroupFileNames()
{
    const auto warning = [](sar::Flavor flavor)
    {
        sar_files::ArchiveSpec spec;
        spec.sounds = {{.name = "SEQ", .file = 0, .player = 0, .banks = {0}}};
        spec.banks = {{"BANK_A", 1}};
        spec.players = {{"PLAYER", 1}};
        spec.groups = {{"GROUP_A", sar::kExternalGroupFile}};
        spec.files = {{sar_files::Sequence(flavor, {0xFF}, {}), "", false}, {{}, "", true}};
        const std::vector<uint8_t> archive = sar_files::Archive(flavor, spec);
        const auto find = [](const std::string&)
        {
            return std::optional<TestFormat::FileChunk>();
        };
        const sar::Options options{flavor, {}, false, sar::Rules::kStopAtFirstProblem};
        std::vector<TestFormat::FileChunk> found;
        const auto search = [&]
        {
            found = rip::FindGroups<TestFormat::FileChunk>(archive, find, options);
        };
        const std::string text = OutputOf(stderr, search);

        return found.empty() ? text : "a group found";
    };

    const std::string wii_u = warning(sar::Flavor::kCafe);
    const std::string nx = warning(sar::Flavor::kNx);
    const std::string ctr = warning(sar::Flavor::kCtr);
    Check(wii_u == "warning: group GROUP_A is kept outside the archive, and GROUP_A.bfgrp isn't there\n",
          "the Wii U's group file: " + wii_u);
    Check(nx == wii_u, "the Switch's group file: " + nx);
    Check(ctr == "warning: group GROUP_A is kept outside the archive, and GROUP_A.bcgrp isn't there\n",
          "the 3DS's group file: " + ctr);
}

} // namespace

// The descriptor of a library: the settings, with the paths of the archive and of the group files in their order. The
// settings' paths give way.
void TestLibraryDescriptor()
{
    const TestFormat::FileChunk archive{"content/Sound/a.bfsar", {1, 2}};
    const std::vector<TestFormat::FileChunk> groups = {{"content/Sound/g2.bfgrp", {3}}, {"content/Sound/g1.bfgrp", {}}};
    TestFormat::ArchiveDescriptor settings;
    settings.archive_path = "old.bfsar";
    settings.group_paths = {"old.bfgrp"};

    const TestFormat::ArchiveDescriptor d = ntdlib::rip::LibraryDescriptor<TestFormat>(archive, groups, settings);
    XSFLIB_CHECK(d.archive_path == "content/Sound/a.bfsar");
    XSFLIB_CHECK((d.group_paths == std::vector<std::string>{"content/Sound/g2.bfgrp", "content/Sound/g1.bfgrp"}));

    const TestFormat::ArchiveDescriptor alone = ntdlib::rip::LibraryDescriptor<TestFormat>(archive, {}, settings);
    XSFLIB_CHECK(alone.archive_path == "content/Sound/a.bfsar" && alone.group_paths.empty());
}

// A mini of several sounds lists their item IDs in its SND chunk, in their order. A mini of one sound is BuildMini's.
// An empty list throws std::invalid_argument.
void TestMiniOfSounds()
{
    const std::vector<uint32_t> piece = {9, 8, 7};
    xsflib::psf::StreamingProgramBuilder expected;
    expected.AddSounds(std::vector<uint32_t>{0x01000009, 0x01000008, 0x01000007});

    const auto build_empty = []
    {
        rip::BuildMiniOfSounds<TestFormat>(std::vector<uint32_t>{}, "lib.psflib", {});
    };

    const std::vector<uint8_t> mini = rip::BuildMiniOfSounds<TestFormat>(piece, "lib.psflib", {});
    const std::vector<uint8_t> one = rip::BuildMiniOfSounds<TestFormat>(std::vector<uint32_t>{5}, "lib.psflib", {});
    const std::string empty = xsflib::testing::ErrorOf<std::invalid_argument>(build_empty);

    XSFLIB_CHECK(mini == expected.WritePsf(TestFormat::kPsfVersion, {}, {{"_lib", "lib.psflib"}}));
    XSFLIB_CHECK(one == rip::BuildMini<TestFormat>(5, "lib.psflib", {}));
    XSFLIB_CHECK(!empty.empty());
}

// An archive that can be read loads, and one that can't gives nothing.
void TestLoadArchiveToRip()
{
    const std::vector<uint8_t> archive = ArchiveWithoutGroups();
    const std::optional<sar::SoundArchive> loaded = rip::LoadArchiveToRip(archive, "a.bfsar", TestTool().sar_options);
    const std::optional<sar::SoundArchive> broken = rip::LoadArchiveToRip({1, 2, 3}, "b.bfsar", TestTool().sar_options);

    XSFLIB_CHECK(loaded && loaded->Sounds().size() == 1 && !loaded->Truncated());
    XSFLIB_CHECK(!broken);
}

int main()
{
    TestLibraryDescriptor();
    TestMiniOfSounds();
    TestLoadArchiveToRip();
    TestLooseArchiveGroupPastBrokenLink();
    TestLooseArchiveGroupPastLockedFolders();
    TestLooseArchiveWithoutSequences();
    TestGameRipGoesOnPastAnError();
    TestLibraryKeepsSequenceData();
    TestLibraryKeepsPlayedSounds();
    TestLibraryGetsRippedSounds();
    TestCutAfterLengthAnalysis();
    TestSecondRipKeepsFirstLibrary();
    TestWholeArchiveWhenFilesCantBeCut();
    TestListArchives();
    TestMissingGroupFileNames();
    TestSoundStates();

    return xsflib::testing::Summary("rip_archive_mode");
}
