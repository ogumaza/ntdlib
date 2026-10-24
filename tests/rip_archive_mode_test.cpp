// SPDX-License-Identifier: MIT

// Tests the archive mode of rip tools (rip/archive_mode.h) with a made-up rip format, on archives built with
// sar_files.h in a temporary folder: the search for a loose archive's group files past an entry whose type can't be
// read, the line for a loose archive without sequences, a game's rip that goes on past an archive whose rip throws,
// the files that a library holds, a second rip into the folder of the first, the lines of --list for a game's
// archives, and the warning for a group file that isn't there.

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <span>
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
#include "temp_folder.h"

namespace
{

namespace fs = std::filesystem;
namespace rip = ntdlib::rip;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

using ntdlib::testing::TempFolder;
using xsflib::testing::Check;

// A rip format for the tests: the archive and its group files as FILE chunks, and a descriptor with their paths.
struct TestFormat
{
    using ProgramBuilder = xsflib::psf::StreamingProgramBuilder;
    using FileChunk = xsflib::psf::FileChunk;

    struct ArchiveDescriptor
    {
        std::string archive_path;
        std::vector<std::string> group_paths;

        std::vector<uint8_t> Serialize() const
        {
            return {};
        }
    };

    static constexpr uint8_t kPsfVersion = 0x7F;
};

// What a rip of the test format holds: the archive, its group files and the descriptor.
struct TestRip
{
    TestFormat::FileChunk archive;
    std::vector<TestFormat::FileChunk> groups;
    TestFormat::ArchiveDescriptor settings;
};

// The test format's tool, for the Switch's archives with kStopAtFirstProblem.
rip::ArchiveRipTool<TestFormat, TestRip> TestTool()
{
    rip::ArchiveRipTool<TestFormat, TestRip> tool;
    tool.format = "tst";
    tool.mini_word = "minitst";
    tool.library_extension = ".tstlib";
    tool.mini_extension = ".minitst";
    tool.sar_options = {sar::Flavor::kNx, {}, false, sar::Rules::kStopAtFirstProblem};

    return tool;
}

// The options of a rip without the length analysis.
xsflib::cli::RipOptions NoLengths()
{
    xsflib::cli::RipOptions options;
    options.lengths = false;

    return options;
}

// A Switch archive with one sequence, SEQ. Its bank, file 1, is only in group GROUP_A. The group file lies outside the
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

// A Switch archive with one stream, STRM, and no sequences. The stream's file lies outside the archive.
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

// The text that `run` writes to `stream`, stdout or stderr. While `run` runs, the stream's file descriptor points at a
// temporary file.
template <typename Run>
std::string OutputOf(std::FILE* stream, const Run& run)
{
    std::FILE* file = std::tmpfile();
    if (file == nullptr)
    {
        return "no temporary file";
    }

    std::fflush(stream);
#ifdef _WIN32
    const int descriptor = _fileno(stream);
    const int saved = _dup(descriptor);
    _dup2(_fileno(file), descriptor);
#else
    const int descriptor = fileno(stream);
    const int saved = dup(descriptor);
    dup2(fileno(file), descriptor);
#endif
    run();
    std::fflush(stream);
#ifdef _WIN32
    _dup2(saved, descriptor);
    _close(saved);
#else
    dup2(saved, descriptor);
    close(saved);
#endif

    std::string text;
    std::rewind(file);
    for (int c; (c = std::fgetc(file)) != EOF;)
    {
        text.push_back(static_cast<char>(c));
    }
    std::fclose(file);

    return text;
}

// A loose archive's group file is found past an entry whose type can't be read: a broken symbolic link here. Without
// the group file, SEQ would lack its bank and rip nothing. On a system that doesn't let the test make the link, such
// as Windows without developer mode, the rip runs without the link.
void TestLooseArchiveGroupPastBrokenLink()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("loose.bfsar", ArchiveWithGroup());
    folder.Write("GROUP_A.bfgrp", GroupA());
    std::error_code ignored;
    fs::create_symlink(folder.Path() / "missing.bfgrp", folder.Path() / "old.bfgrp", ignored);

    const fs::path out = folder.Path() / "out";
    xsflib::rip::FileNames names(".tstlib", ".minitst");
    const auto describe = [](std::vector<uint8_t>)
    {
        return std::string();
    };

    const int result = rip::RipLooseArchive(TestTool(), input.string(), out, NoLengths(), names, TestRip{}, describe);

    Check(result == 0, "a loose archive rips");
    Check(fs::exists(out / "loose.tstlib") && fs::exists(out / "0000 SEQ.minitst"),
          "SEQ rips with its bank from the group file past the broken link");
}

// A loose archive without sequences rips nothing, and the rip says so in a line of its own. The exit code is 0. With
// --bgm, the rip reports instead that no sequence is music, and the exit code is 1.
void TestLooseArchiveWithoutSequences()
{
    const TempFolder folder("rip_archive_mode_test");
    const fs::path input = folder.Write("streams.bfsar", ArchiveWithoutSequences());
    const fs::path out = folder.Path() / "out";
    const auto describe = [](std::vector<uint8_t>)
    {
        return std::string();
    };
    xsflib::cli::RipOptions bgm = NoLengths();
    bgm.bgm = true;
    int result = -1;
    int bgm_result = -1;
    const auto rip_archive = [&]
    {
        xsflib::rip::FileNames names(".tstlib", ".minitst");
        result = rip::RipLooseArchive(TestTool(), input.string(), out, NoLengths(), names, TestRip{}, describe);
    };
    const auto rip_music = [&]
    {
        xsflib::rip::FileNames names(".tstlib", ".minitst");
        bgm_result = rip::RipLooseArchive(TestTool(), input.string(), out, bgm, names, TestRip{}, describe);
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

// A Switch archive with two sequences. SEQ_A's bank, file 2, is only in GROUP_A, a group file beside the archive.
// SEQ_B's bank, file 3, has a place in the FILE block. GROUP_B, beside the archive, holds a copy of it.
std::vector<uint8_t> ArchiveWithTwoGroups()
{
    sar_files::ArchiveSpec spec;
    spec.sounds = {{.name = "SEQ_A", .file = 0, .player = 0, .banks = {0}},
                   {.name = "SEQ_B", .file = 1, .player = 0, .banks = {1}}};
    spec.banks = {{"BANK_A", 2}, {"BANK_B", 3}};
    spec.players = {{"PLAYER", 1}};
    spec.groups = {{"GROUP_A", 0xFFFFFFFF}, {"GROUP_B", 0xFFFFFFFF}};
    spec.files = {{sar_files::Sequence(false, {0xFF}, {}), "", false},
                  {sar_files::Sequence(false, {0x00, 0xFF}, {}), "", false},
                  {{}, "", true},
                  {sar_files::Bank(false, sar_files::BankSpec{}), "", false}};

    return sar_files::Archive(false, spec);
}

// A group's file that holds a copy of file `file`, a bank without instruments.
std::vector<uint8_t> GroupOf(uint32_t file)
{
    return sar_files::Group(false, {{file, sar_files::Bank(false, sar_files::BankSpec{})}});
}

// The files that the library at `path` holds, by their paths. Empty when the library can't be read.
std::map<std::string, std::vector<uint8_t>> LibraryFiles(const fs::path& path)
{
    std::vector<uint8_t> bytes;
    xsflib::psf::ProgramChunks chunks;
    std::map<std::string, std::vector<uint8_t>> files;
    if (xsflib::psf::ReadWholeFile(path.string(), bytes) && !xsflib::psf::ParsePsfChunks(bytes, chunks))
    {
        for (xsflib::psf::FileChunk& file : chunks.files)
        {
            files[file.path] = std::move(file.data);
        }
    }

    return files;
}

// The names of `files`, in order.
std::vector<std::string> NamesOf(const std::map<std::string, std::vector<uint8_t>>& files)
{
    std::vector<std::string> names;
    for (const auto& [name, data] : files)
    {
        names.push_back(name);
    }

    return names;
}

// Rips the loose archive `input` with `tool` into the folder `out` beside it, with --only `only` when it isn't empty,
// and with the length analysis when `lengths` is set. Returns the files of the library, and adds the messages to
// `messages`.
std::map<std::string, std::vector<uint8_t>> RipLoose(const rip::ArchiveRipTool<TestFormat, TestRip>& tool,
                                                     const fs::path& input, const std::string& out,
                                                     const std::string& only, std::string& messages,
                                                     bool lengths = false)
{
    xsflib::cli::RipOptions options = NoLengths();
    options.lengths = lengths;
    if (!only.empty())
    {
        options.only = std::regex(only);
        options.only_text = only;
    }
    const auto describe = [](std::vector<uint8_t>)
    {
        return std::string();
    };
    const fs::path folder = input.parent_path() / out;
    const auto rip_loose = [&]
    {
        xsflib::rip::FileNames names(".tstlib", ".minitst");
        rip::RipLooseArchive(tool, input.string(), folder, options, names, TestRip{}, describe);
    };

    messages += OutputOf(stderr, rip_loose);

    return LibraryFiles(folder / (input.stem().string() + ".tstlib"));
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
            job.timing = rip::EndAt(10, 5.0, 0.0);
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

// The `_lib` tag of the mini at `path`. Empty when the mini can't be read or has no such tag.
std::string LibraryTag(const fs::path& path)
{
    std::vector<uint8_t> bytes;
    xsflib::psf::PsfFile mini;
    if (!xsflib::psf::ReadWholeFile(path.string(), bytes) || xsflib::psf::ParsePsf(bytes, mini, false))
    {
        return "";
    }

    const auto lib = mini.tags.find("_lib");

    return lib == mini.tags.end() ? "" : lib->second;
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
    for (std::size_t entry = 0x14; entry < 0x14 + 12 * std::size_t{xsflib::LoadLe16(&bytes[0x10])}; entry += 12)
    {
        if (xsflib::LoadLe16(&bytes[entry]) == 0x2001)
        {
            const uint32_t offset = xsflib::LoadLe32(&bytes[entry + 4]);
            const uint32_t size = xsflib::LoadLe32(&bytes[entry + 8]);
            const std::vector<uint8_t> info(bytes.begin() + offset, bytes.begin() + offset + size);
            xsflib::StoreLe32(&bytes[entry + 4], static_cast<uint32_t>(bytes.size()));
            bytes.insert(bytes.end(), info.begin(), info.end());
        }
    }
    xsflib::StoreLe32(&bytes[0x0C], static_cast<uint32_t>(bytes.size()));

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

int main()
{
    TestLooseArchiveGroupPastBrokenLink();
    TestLooseArchiveWithoutSequences();
    TestGameRipGoesOnPastAnError();
    TestLibraryKeepsSequenceData();
    TestLibraryGetsRippedSounds();
    TestCutAfterLengthAnalysis();
    TestSecondRipKeepsFirstLibrary();
    TestWholeArchiveWhenFilesCantBeCut();
    TestListArchives();
    TestMissingGroupFileNames();

    return xsflib::testing::Summary("rip_archive_mode");
}
