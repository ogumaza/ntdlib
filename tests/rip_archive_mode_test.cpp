// SPDX-License-Identifier: MIT

// Tests the archive mode of rip tools (rip/archive_mode.h) with a made-up rip format, on archives built with
// sar_files.h in a temporary folder: the search for a loose archive's group files past an entry whose type can't be
// read, and a game's rip that goes on past an archive whose rip throws.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ntdlib/rip/archive_mode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "temp_folder.h"
#include "xsflib/cli/rip.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/testing/check.h"

namespace
{

namespace fs = std::filesystem;
namespace rip = ntdlib::rip;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

using ntdlib::testing::TempFolder;
using xsflib::testing::Check;
using xsflib::testing::failures;

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

// GROUP_A's file: a copy of file 1, a bank without instruments.
std::vector<uint8_t> GroupA()
{
    return sar_files::Group(false, {{1, sar_files::Bank(false, sar_files::BankSpec{})}});
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

} // namespace

int main()
{
    TestLooseArchiveGroupPastBrokenLink();
    TestGameRipGoesOnPastAnError();

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
