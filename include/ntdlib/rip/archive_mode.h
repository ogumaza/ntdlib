// SPDX-License-Identifier: MIT

// A PSF rip tool's archive mode for NintendoWare's FSAR sound archives on the Wii U and the Switch, on xsflib's rip
// frame (xsflib/cli/rip.h): it rips a game's sound archives, or a loose one, into a library and a mini per sequence. A
// tool gives its format's names, the archive reader's options and its length analysis in an ArchiveRipTool. A tool
// whose game mode runs the game's code also gives the hooks for that mode. The header requires xsflib::cli.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"

namespace ntdlib::rip
{

// What differs between the rip formats' archive modes. `Format` is the rip format's types (BuildArchiveLibrary). `Rip`
// holds what a sound archive is ripped with: at least its file `archive` and the group files `groups`, as
// Format::FileChunk, and the descriptor's `settings`, as Format::ArchiveDescriptor. The length analysis plays with the
// same settings.
template <typename Format, typename Rip>
struct ArchiveRipTool
{
    using FileChunk = typename Format::FileChunk;
    using Settings = typename Format::ArchiveDescriptor;

    // The format's name in its tags (`<format>by` and `<format>_mode`, xsflib::cli::RipTags) and in the names of sounds
    // without a label (`<format>_sound`), and the minis' name in the messages, such as "minipsf".
    std::string format;
    std::string mini_word;

    // The extensions of the format's libraries and minis, such as ".psflib" and ".minipsf".
    std::string library_extension;
    std::string mini_extension;

    // The archive reader's options for the format's archives.
    sar::Options sar_options;

    // Works out the timings of `jobs` by playing them with `settings` on the format's model, on `threads` threads.
    std::function<void(std::vector<Job>& jobs, sar::SoundArchive archive, const Settings& settings, unsigned threads)>
        analyze_lengths;

    // Whether a rip plays with the group files kept outside the archive. A game mode that runs the game's code loads
    // none.
    std::function<bool(const Rip&)> uses_groups = [](const Rip&)
    {
        return true;
    };

    // The mode in the tags (xsflib::cli::RipTags), or null for none.
    std::function<const char*(const Rip&)> mode = [](const Rip&) -> const char*
    {
        return nullptr;
    };

    // The library of a rip, with `tags`. By default it's an archive-mode library (BuildArchiveLibrary).
    std::function<std::vector<uint8_t>(const Rip&, const xsflib::psf::Tags& tags)> build_library =
        [](const Rip& to_rip, const xsflib::psf::Tags& tags)
    {
        return BuildArchiveLibrary<Format>(to_rip.archive, to_rip.groups, to_rip.settings, tags);
    };
};

// Rips one sound archive into `out_dir`, and adds the number of sounds --only picked to `matched`. `names` is the
// registry of file names for `out_dir`. Returns the number of minis written, or -1 on an error.
template <typename Format, typename Rip>
int RipArchive(const ArchiveRipTool<Format, Rip>& tool, const std::filesystem::path& out_dir,
               xsflib::rip::FileNames& names, const std::string& lib_base, const Rip& to_rip,
               const std::string& game_name, const xsflib::cli::RipOptions& o, std::size_t& matched)
{
    const std::string& path = to_rip.archive.path;
    std::optional<sar::SoundArchive> archive;
    try
    {
        archive = sar::SoundArchive::Load(to_rip.archive.data, tool.sar_options);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
        return -1;
    }

    if (archive->Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated: sounds whose data lies past its end are skipped\n",
                     path.c_str());
    }

    if (tool.uses_groups(to_rip))
    {
        for (const auto& group : to_rip.groups)
        {
            archive->AddGroupFile(group.data);
        }
    }

    std::vector<Job> jobs = SelectSounds(*archive, o.only, o.bgm);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    // Sequences whose data isn't all there would play with missing instruments. A sound whose volume in the archive is
    // 0 makes no sound. The game's sound library multiplies everything the sound plays by that volume.
    std::vector<Job> complete;
    std::size_t missing_data = 0;
    std::size_t silent = 0;
    for (Job& job : jobs)
    {
        const auto missing = MissingData(*archive, job.index, tool.sar_options);
        if (!missing.empty())
        {
            missing_data++;
            std::string what;
            for (std::size_t i = 0; i < missing.size(); i++)
            {
                what += (i ? ", " : "") + missing[i];
            }
            std::fprintf(stderr, "skipped %s: missing %s\n", job.label.c_str(), what.c_str());
        }
        else if (archive->Sounds()[job.index].volume == 0)
        {
            silent++;
            std::fprintf(stderr, "skipped %s: it makes no sound (volume 0)\n", job.label.c_str());
        }
        else
        {
            complete.push_back(std::move(job));
        }
    }

    // The length analysis finds the other sequences that never sound. Some play no notes, and some play their notes at
    // a volume of 0.
    if (o.lengths && !complete.empty())
    {
        tool.analyze_lengths(complete, std::move(*archive), to_rip.settings, o.jobs);
        std::vector<Job> sounding;
        for (Job& job : complete)
        {
            if (job.timing && job.timing->silent)
            {
                silent++;
                std::fprintf(stderr, "skipped %s: it makes no sound\n", job.label.c_str());
            }
            else
            {
                sounding.push_back(std::move(job));
            }
        }
        complete = std::move(sounding);
    }

    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", path.c_str(),
                     xsflib::cli::SkippedText(missing_data, "data not there", silent, "sequence").c_str());
        return 0;
    }

    const xsflib::psf::Tags common = xsflib::cli::RipTags(o, game_name, tool.format, tool.mode(to_rip));
    const std::string lib_name = names.Library(lib_base);
    const std::vector<uint8_t> lib = tool.build_library(to_rip, common);
    std::filesystem::create_directories(out_dir);
    if (!xsflib::rip::WriteFile(out_dir / xsflib::PathFromUtf8(lib_name), lib))
    {
        std::fprintf(stderr, "error: can't write %s\n", lib_name.c_str());
        return -1;
    }
    std::fprintf(stderr, "wrote %s (%zu bytes)\n", lib_name.c_str(), lib.size());

    const auto build_mini = [&lib_name](const Job& job, const xsflib::psf::Tags& tags)
    {
        return BuildMini<Format>(job.index, lib_name, tags);
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, complete, common, tool.format + "_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d %s files%s\n", path.c_str(), count, tool.mini_word.c_str(),
                     xsflib::cli::SkippedText(missing_data, "data not there", silent, "sequence").c_str());
    }

    return count;
}

// What a sound archive holds (such as "FSAR 2.2.0, 2911 sequences, 28 wave sounds, 220 streams"), or why it can't be
// read, with the archive reader's `options`.
inline std::string DescribeArchive(std::vector<uint8_t> bytes, const sar::Options& options)
{
    try
    {
        const sar::SoundArchive archive = sar::SoundArchive::Load(std::move(bytes), options);
        std::size_t sequences = 0, wave_sounds = 0, streams = 0;
        for (const sar::SoundInfo& sound : archive.Sounds())
        {
            sequences += sound.type == sar::SoundType::kSequence;
            wave_sounds += sound.type == sar::SoundType::kWaveSound;
            streams += sound.type == sar::SoundType::kStream;
        }

        const uint32_t v = archive.Version();
        char text[160];
        std::snprintf(text, sizeof(text), "FSAR %u.%u.%u, %zu sequences, %zu wave sounds, %zu streams%s",
                      v >> 16 & 0xFF, v >> 8 & 0xFF, v & 0xFF, sequences, wave_sounds, streams,
                      archive.Truncated() ? ", truncated" : "");

        return text;
    }
    catch (const std::exception& e)
    {
        return std::string("can't be read: ") + e.what();
    }
}

// Rips a game's sound archives `archives`, or the one --archive names, into a folder each in `out_dir`. Each starts as
// `common` with the archive's file and the group files that `find_group` finds by their names. `read_file` reads a file
// of the game. `check`, if set, may reject an archive: it reports why and returns false. `find_sarcs`, if set, lists
// the game's SARC archives beside an archive. FindGroups searches them while a group is missing. Each archive's folder
// takes its registry of file names from `registries`. Returns the exit code.
template <typename Format, typename Rip, typename ReadFile, typename FindGroup>
int RipArchives(const ArchiveRipTool<Format, Rip>& tool, const std::string& input, const std::filesystem::path& out_dir,
                const std::vector<std::string>& archives, const Rip& common, const ReadFile& read_file,
                const FindGroup& find_group, const std::string& game_name, const xsflib::cli::RipOptions& o,
                xsflib::cli::FileNameRegistries& registries,
                const std::function<bool(const std::string& path, const std::vector<uint8_t>& bytes)>& check = {},
                const std::function<std::vector<std::string>(const std::string& archive_path)>& find_sarcs = {})
{
    const std::vector<std::string> folders = xsflib::rip::ArchiveFolders(archives);
    int total = 0;
    std::size_t matched = 0;
    bool failed = false; // an archive that can't be ripped doesn't stop the others
    for (std::size_t a = 0; a < archives.size(); a++)
    {
        const std::string& path = archives[a];
        if (!o.only_archive.empty() && path != o.only_archive)
        {
            continue;
        }

        // An exception, such as the FormatError of a damaged group file, fails this archive alone.
        try
        {
            Rip to_rip = common;
            to_rip.archive.path = path;
            if (!read_file(path, to_rip.archive.data))
            {
                std::fprintf(stderr, "error: can't read %s\n", path.c_str());
                failed = true;
                continue;
            }
            if (check && !check(path, to_rip.archive.data))
            {
                failed = true;
                continue;
            }

            const auto list_sarcs = [&find_sarcs, &path]
            {
                return find_sarcs ? find_sarcs(path) : std::vector<std::string>();
            };
            const auto read_sarc = [&read_file](const std::string& sarc, std::vector<uint8_t>& bytes)
            {
                return read_file(sarc, bytes);
            };
            to_rip.groups = FindGroups<typename Format::FileChunk>(to_rip.archive.data, find_group, tool.sar_options,
                                                                   list_sarcs, read_sarc);

            // Each archive's set goes in a folder of its own.
            const std::filesystem::path archive_dir = out_dir / folders[a];
            const int n =
                RipArchive(tool, archive_dir,
                           xsflib::cli::NamesIn(registries, archive_dir, tool.library_extension, tool.mini_extension),
                           game_name + " " + folders[a], to_rip, game_name, o, matched);
            if (n < 0)
            {
                failed = true;
                continue;
            }
            total += n;
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "error: %s: %s\n", path.c_str(), e.what());
            failed = true;
        }
    }

    if (matched == 0 && (!o.only_text.empty() || o.bgm) && !failed)
    {
        return NoSoundMatches(o.only_text, o.bgm);
    }

    std::fprintf(stderr, "%s: %d %s files\n", xsflib::rip::InputPath(input).filename().string().c_str(), total,
                 tool.mini_word.c_str());
    if (total > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return failed ? 1 : 0;
}

// Rips the loose sound archive `input` into `out_dir`, with `to_rip`'s settings. `names` is the registry of file names
// for `out_dir`. `describe` gives what an archive holds, for --list. `notice` comes before the rip. Returns the exit
// code.
template <typename Format, typename Rip>
int RipLooseArchive(const ArchiveRipTool<Format, Rip>& tool, const std::string& input,
                    const std::filesystem::path& out_dir, const xsflib::cli::RipOptions& o,
                    xsflib::rip::FileNames& names, Rip to_rip,
                    const std::function<std::string(std::vector<uint8_t>)>& describe, const std::string& notice = "")
{
    namespace fs = std::filesystem;
    using FileChunk = typename Format::FileChunk;

    const fs::path path = xsflib::rip::InputPath(input);
    const std::string file_name = path.filename().string();
    if (!o.only_archive.empty())
    {
        std::fprintf(stderr, "error: --archive picks one of a game's sound archives, and %s is one already\n",
                     input.c_str());
        return 2;
    }

    to_rip.archive.path = file_name;
    if (!xsflib::psf::ReadWholeFile(path.string(), to_rip.archive.data))
    {
        std::fprintf(stderr, "error: can't read %s\n", input.c_str());
        return 1;
    }
    if (o.list)
    {
        std::printf("%s: a sound archive, %s\n", input.c_str(), describe(to_rip.archive.data).c_str());
        return 0;
    }

    // A group file is found by its name in the archive's folder or a folder below it. The group file is kept at its
    // path from there. The search passes over an entry whose type can't be read, such as a broken symbolic link.
    const fs::path folder = path.parent_path();
    const auto find_group = [&folder](const std::string& name) -> std::optional<FileChunk>
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            FileChunk group{it->path().lexically_relative(folder).generic_string(), {}};
            std::error_code type_error;
            if (it->is_regular_file(type_error) && IsGroupFileName(it->path().filename().string(), name) &&
                xsflib::psf::ReadWholeFile(it->path().string(), group.data))
            {
                return group;
            }
        }

        return std::nullopt;
    };

    // The SARC archives in the archive's folder, by their names' UTF-8 bytes. A group file in one is kept at the SARC's
    // name, a slash and its name in the SARC.
    const auto list_sarcs = [&folder]
    {
        std::vector<std::string> sarcs;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->path().extension() == ".sarc")
            {
                sarcs.push_back(xsflib::Utf8(it->path().filename()));
            }
        }

        std::sort(sarcs.begin(), sarcs.end());

        return sarcs;
    };
    const auto read_sarc = [&folder](const std::string& name, std::vector<uint8_t>& bytes)
    {
        return xsflib::psf::ReadWholeFile((folder / xsflib::PathFromUtf8(name)).string(), bytes);
    };
    to_rip.groups = FindGroups<FileChunk>(to_rip.archive.data, find_group, tool.sar_options, list_sarcs, read_sarc);

    std::fprintf(stderr, "%s", notice.c_str());
    const std::string game_name = o.game_name.empty() ? xsflib::rip::NameOfInput(input) : o.game_name;
    std::size_t matched = 0;
    const int count = RipArchive(tool, out_dir, names, game_name, to_rip, game_name, o, matched);
    if (count < 0)
    {
        return 1;
    }
    if (matched == 0 && (!o.only_text.empty() || o.bgm))
    {
        return NoSoundMatches(o.only_text, o.bgm);
    }
    if (count > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return 0;
}

} // namespace ntdlib::rip
