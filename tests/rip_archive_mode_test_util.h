// SPDX-License-Identifier: MIT

// Helpers that the tests of the archive mode share (rip_archive_mode_test.cpp and rip_archive_mode_tags_test.cpp): a
// made-up rip format and its tool, archives built with sar_files.h, rips of a loose archive, and reads of the files
// that a rip writes.

#pragma once

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/cli/rip.h"
#include "xsflib/psf/program.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/streaming.h"
#include "xsflib/rip/file_names.h"

#include "ntdlib/rip/archive_mode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"

namespace rip_archive_mode_tests
{

namespace fs = std::filesystem;
namespace rip = ntdlib::rip;
namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

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
inline rip::ArchiveRipTool<TestFormat, TestRip> TestTool()
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
inline xsflib::cli::RipOptions NoLengths()
{
    xsflib::cli::RipOptions options;
    options.lengths = false;

    return options;
}

// The description of an archive in a rip's list: none.
inline std::string NoDescription(const std::vector<uint8_t>&)
{
    return std::string();
}

// The text that `run` writes to `stream`, stdout or stderr. While `run` runs, the stream's file descriptor points at a
// temporary file.
template <typename Run>
inline std::string OutputOf(std::FILE* stream, const Run& run)
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

// A Switch archive with two sequences. SEQ_A's bank, file 2, is only in GROUP_A, a group file beside the archive.
// SEQ_B's bank, file 3, has a place in the FILE block. GROUP_B, beside the archive, holds a copy of it.
inline std::vector<uint8_t> ArchiveWithTwoGroups()
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
inline std::vector<uint8_t> GroupOf(uint32_t file)
{
    return sar_files::Group(false, {{file, sar_files::Bank(false, sar_files::BankSpec{})}});
}

// The files that the library at `path` holds, by their paths. Empty when the library can't be read.
inline std::map<std::string, std::vector<uint8_t>> LibraryFiles(const fs::path& path)
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
inline std::vector<std::string> NamesOf(const std::map<std::string, std::vector<uint8_t>>& files)
{
    std::vector<std::string> names;
    for (const auto& [name, data] : files)
    {
        names.push_back(name);
    }

    return names;
}

// A timing for a test's length analysis: a sequence that ends at 50 ms, with its first sample at the start.
inline rip::Timing EndedAt50Ms()
{
    rip::Timing timing = rip::EndAt(10, 5.0);
    timing.first_sample_ms = 0;
    timing.first_sound_ms = 0;

    return timing;
}

// Rips the loose archive `input` with `tool` into the folder `out` beside it, with --only `only` when it isn't empty,
// and with the length analysis when `lengths` is set. Returns the files of the library, and adds the messages to
// `messages`.
inline std::map<std::string, std::vector<uint8_t>> RipLoose(const rip::ArchiveRipTool<TestFormat, TestRip>& tool,
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
    const fs::path folder = input.parent_path() / out;
    const auto rip_loose = [&]
    {
        xsflib::rip::FileNames names(".tstlib", ".minitst");
        rip::RipLooseArchive(tool, input.string(), folder, options, names, TestRip{}, NoDescription);
    };

    messages += OutputOf(stderr, rip_loose);

    return LibraryFiles(folder / (input.stem().string() + ".tstlib"));
}

// The tag `name` of the mini at `path`. Empty when the mini can't be read or has no such tag.
inline std::string MiniTag(const fs::path& path, const std::string& name)
{
    std::vector<uint8_t> bytes;
    xsflib::psf::PsfFile mini;
    if (!xsflib::psf::ReadWholeFile(path.string(), bytes) || xsflib::psf::ParsePsf(bytes, mini, false))
    {
        return "";
    }

    const auto tag = mini.tags.find(name);

    return tag == mini.tags.end() ? "" : tag->second;
}

// The `_lib` tag of the mini at `path`. Empty when the mini can't be read or has no such tag.
inline std::string LibraryTag(const fs::path& path)
{
    return MiniTag(path, "_lib");
}

} // namespace rip_archive_mode_tests
