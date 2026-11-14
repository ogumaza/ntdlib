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
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/cli/rip.h"
#include "xsflib/files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/file_names.h"
#include "xsflib/rip/tool.h"

#include "ntdlib/rip/archive.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/shortest_decimal.h"

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

    // Works out the timings of `jobs` by playing them with `settings` on the format's model, on `threads` threads. A
    // job's volume scales what the analysis counts as sound. The player scales the output by the mini's volume tag.
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

    // Whether a rip's library holds only the files that its sequences play, and the group files that hold them
    // (KeepSequenceData). An archive mode's model reads nothing else. A game mode that runs the game's code reads each
    // file at the place that the archive's file table gives. Its library keeps the archive's layout. Its build_library
    // can put zeros in place of the files that the sequences don't play (KeepLoadedFiles). By default, every library
    // holds the whole archive.
    std::function<bool(const Rip&)> keeps_sequence_data = [](const Rip&)
    {
        return false;
    };

    // The sounds of `archive`, the rip's archive, that the jobs play without being jobs themselves, such as a sound
    // that a job's script starts. A library that holds only the files that its sequences play (keeps_sequence_data)
    // keeps theirs too. Null for none.
    std::function<std::vector<uint32_t>(const Rip&, const sar::SoundArchive& archive, const std::vector<Job>& jobs)>
        played_sounds;

    // The factor that the game's code gives a sound, outside the sound archive. The sound's mini holds it as its volume
    // tag. Null for none.
    std::function<float(const Rip&, const Job&)> sound_volume;

    // The variants of a sound that waits for variables that the game sets when it starts the sound, such as the
    // material of the ground under a footstep. Each variant gives the variables that it plays with, by their numbers in
    // the format's tag (variables_tag). A sound with variants is ripped once for each of them, and not without them.
    // A variant's label is the sound's label, a space and its variables as format_variables writes them, such as
    // "SE_STEP 15=1". Its mini holds the variables in variables_tag. The length analysis gets them in Job::variables.
    // By default no sound has variants.
    std::function<std::vector<xsflib::psf::Variables>(const Rip&, const Job&)> variants = [](const Rip&, const Job&)
    {
        return std::vector<xsflib::psf::Variables>();
    };

    // The tag that holds a variant's variables in its mini, such as "csf_var". A format whose tool gives variants
    // requires it. A format that holds a job's variables in the mini's reserved area (mini_reserved) leaves it empty.
    // Its minis then get no tag for them.
    std::string variables_tag;

    // The text of a variant's variables in its label and in variables_tag. By default, the form of the PSF tag
    // 3sf_var (xsflib::psf::FormatVariables).
    std::function<std::string(const xsflib::psf::Variables&)> format_variables = xsflib::psf::FormatVariables;

    // Whether the tag `<format>_sound` of a later state or a variant holds the sound's label alone. Otherwise the tag
    // holds the job's label, as the title does: the sound's label with the state's or the variant's name.
    bool sound_tag_holds_sound_label = false;

    // The library of a rip that plays the sequences at `sounds`, by their indices in the archive, with `tags`. By
    // default it's an archive-mode library (BuildArchiveLibrary). The default doesn't use `sounds`.
    std::function<std::vector<uint8_t>(const Rip&, std::span<const uint32_t> sounds, const xsflib::psf::Tags& tags)>
        build_library = [](const Rip& to_rip, std::span<const uint32_t>, const xsflib::psf::Tags& tags)
    {
        return BuildArchiveLibrary<Format>(to_rip.archive, to_rip.groups, to_rip.settings, tags);
    };

    // The states that a rip plays the selected sounds in, where the game's code sets the volumes of their tracks: the
    // jobs that take the place of `selected`, each with the track volumes of one state. Each sound comes in the state
    // that the game starts it in, under its label, followed by its later states, each under a label that names the
    // state. --only then picks the jobs by their labels. By default, `selected` as it is.
    std::function<std::vector<Job>(const Rip&, const std::vector<Job>& selected)> sound_states =
        [](const Rip&, const std::vector<Job>& selected)
    {
        return selected;
    };

    // The reserved area of a job's mini: a descriptor that takes the place of the library's, or nothing. A job whose
    // tracks aren't all at 1 has to have one. By default nothing.
    std::function<std::vector<uint8_t>(const Rip&, const Job& job)> mini_reserved = [](const Rip&, const Job&)
    {
        return std::vector<uint8_t>{};
    };
};

// Sets the volume tag to `volume`, unless it's 1, as the shortest decimal that reads back as the same float.
inline void SetVolumeTag(float volume, xsflib::psf::Tags& tags)
{
    if (volume == 1.0f)
    {
        return;
    }

    tags["volume"] = ShortestDecimal(volume);
}

// The jobs that rip `selected` in the states that `tool` gives them (ArchiveRipTool::sound_states), in order. A job is
// left out when its label doesn't match `only`. --only can therefore pick a later state alone, or a sound without its
// later states.
template <typename Format, typename Rip>
std::vector<Job> WithSoundStates(const ArchiveRipTool<Format, Rip>& tool, const Rip& to_rip,
                                 const std::vector<Job>& selected, const std::regex& only)
{
    std::vector<Job> jobs = tool.sound_states(to_rip, selected);
    std::erase_if(jobs, [&only](const Job& job) { return !std::regex_search(job.label, only); });

    return jobs;
}

// Adds `to_rip`'s group files to `archive` when the rip plays with them.
template <typename Format, typename Rip>
void AddGroupFiles(const ArchiveRipTool<Format, Rip>& tool, const Rip& to_rip, sar::SoundArchive& archive)
{
    if (tool.uses_groups(to_rip))
    {
        for (const auto& group : to_rip.groups)
        {
            archive.AddGroupFile(group.data);
        }
    }
}

// `jobs`, with each job whose sound has variants (ArchiveRipTool::variants) replaced by a job for each variant, in the
// order that the variants come.
template <typename Format, typename Rip>
std::vector<Job> AddVariants(const ArchiveRipTool<Format, Rip>& tool, const Rip& to_rip, std::vector<Job> jobs)
{
    std::vector<Job> ripped;
    for (Job& job : jobs)
    {
        const std::vector<xsflib::psf::Variables> variants = tool.variants(to_rip, job);
        if (variants.empty())
        {
            ripped.push_back(std::move(job));
            continue;
        }

        for (const xsflib::psf::Variables& variables : variants)
        {
            Job variant = job;
            variant.label += " " + tool.format_variables(variables);
            variant.variables = variables;
            ripped.push_back(std::move(variant));
        }
    }

    return ripped;
}

// Cuts `to_rip`'s archive down to the files that `jobs` and `more_sounds` play, and its group files down to those that
// hold any of them (KeepSequenceData). `archive` is `to_rip`'s archive, with the group files that the rip plays with.
// Leaves both whole, after a warning, when those files can't be cut out of the archive.
template <typename Format, typename Rip>
void KeepJobsData(const sar::SoundArchive& archive, const std::vector<Job>& jobs, std::span<const uint32_t> more_sounds,
                  Rip& to_rip)
{
    std::vector<uint32_t> sounds;
    for (const Job& job : jobs)
    {
        sounds.push_back(job.index);
    }
    sounds.insert(sounds.end(), more_sounds.begin(), more_sounds.end());

    std::optional<LibraryArchive> kept = KeepSequenceData(archive, sounds);
    if (!kept)
    {
        std::fprintf(stderr,
                     "warning: the files that the sequences play can't be cut out of %s. The library holds the whole "
                     "archive\n",
                     to_rip.archive.path.c_str());
        return;
    }

    to_rip.archive.data = std::move(kept->archive);
    std::vector<typename Format::FileChunk> groups;
    for (const std::size_t group : kept->group_files)
    {
        groups.push_back(std::move(to_rip.groups[group]));
    }
    to_rip.groups = std::move(groups);
}

// Rips one sound archive into `out_dir`, and adds the number of jobs that --only and --bgm picked to `matched`.
// `names` is the registry of file names for `out_dir`. Returns the number of minis written, or -1 on an error.
template <typename Format, typename Rip>
int RipArchive(const ArchiveRipTool<Format, Rip>& tool, const std::filesystem::path& out_dir,
               xsflib::rip::FileNames& names, const std::string& lib_base, Rip to_rip, const std::string& game_name,
               const xsflib::cli::RipOptions& o, std::size_t& matched)
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

    AddGroupFiles(tool, to_rip, *archive);

    // --only picks from the sounds' states (WithSoundStates), whose labels can differ from the sounds'.
    const std::vector<Job> selected = SelectSounds(*archive, std::regex(""), o.bgm);
    std::map<uint32_t, std::string> sound_labels;
    for (const Job& job : selected)
    {
        sound_labels.emplace(job.index, job.label);
    }
    std::vector<Job> jobs = WithSoundStates(tool, to_rip, selected, o.only);
    matched += jobs.size();
    if (jobs.empty())
    {
        return 0;
    }

    if (tool.sound_volume)
    {
        for (Job& job : jobs)
        {
            job.volume = tool.sound_volume(to_rip, job);
        }
    }

    // The sounds to rip: those that have all their data and make a sound, each variant of a sound that has variants.
    // The length analysis finds the silent sequences whose volume isn't 0, and the variants that play nothing.
    Skipped skipped;
    std::vector<Job> complete =
        AddVariants(tool, to_rip, SkipIncomplete(*archive, std::move(jobs), tool.sar_options, skipped));
    if (o.lengths && !complete.empty())
    {
        tool.analyze_lengths(complete, std::move(*archive), to_rip.settings, o.jobs);
        archive.reset();
        complete = SkipSilent(std::move(complete), skipped);
    }

    const std::string skipped_text =
        xsflib::cli::SkippedText(skipped.missing_data, "data not there", skipped.silent, "sequence");
    if (complete.empty())
    {
        std::fprintf(stderr, "%s: nothing to rip%s\n", path.c_str(), skipped_text.c_str());
        return 0;
    }

    // An archive mode's library holds only the files that its sequences play. The cut reads the archive again, with the
    // same group files, when the length analysis took it.
    if (tool.keeps_sequence_data(to_rip))
    {
        if (!archive)
        {
            archive = sar::SoundArchive::Load(to_rip.archive.data, tool.sar_options);
            AddGroupFiles(tool, to_rip, *archive);
        }
        const std::vector<uint32_t> played =
            tool.played_sounds ? tool.played_sounds(to_rip, *archive, complete) : std::vector<uint32_t>{};
        KeepJobsData<Format>(*archive, complete, played, to_rip);
        archive.reset();
    }

    // A sound's variants share its files.
    std::vector<uint32_t> sounds;
    for (const Job& job : complete)
    {
        if (sounds.empty() || sounds.back() != job.index)
        {
            sounds.push_back(job.index);
        }
    }

    const xsflib::psf::Tags common = xsflib::cli::RipTags(o, game_name, tool.format, tool.mode(to_rip));
    const std::optional<std::string> written_lib =
        xsflib::cli::WriteLibrary(out_dir, names, lib_base, tool.build_library(to_rip, sounds, common));
    if (!written_lib)
    {
        return -1;
    }
    const std::string& lib_name = *written_lib;

    const auto build_mini = [&tool, &to_rip, &lib_name, &sound_labels](const Job& job, xsflib::psf::Tags tags)
    {
        const auto sound_label = sound_labels.find(job.index);
        if (tool.sound_tag_holds_sound_label && sound_label != sound_labels.end())
        {
            tags[tool.format + "_sound"] = sound_label->second;
        }
        SetVolumeTag(job.volume, tags);
        if (!job.variables.empty() && !tool.variables_tag.empty())
        {
            tags[tool.variables_tag] = tool.format_variables(job.variables);
        }

        return BuildMini<Format>(job.index, lib_name, std::move(tags), tool.mini_reserved(to_rip, job));
    };
    const int count = xsflib::cli::WriteMinis(out_dir, names, complete, common, tool.format + "_sound", build_mini);
    if (count >= 0)
    {
        std::fprintf(stderr, "%s: %d %s files%s\n", path.c_str(), count, tool.mini_word.c_str(), skipped_text.c_str());
    }

    return count;
}

// What a sound archive holds (its version, and its counts of sequences, wave sounds and streams), or why it can't be
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

// Prints the lines of --list for a game's sound archives `archives`, a line each: the archive's path and size, and
// what it holds (`describe`, such as DescribeArchive), or that `read_file` can't read it.
template <typename ReadFile>
void ListArchives(const std::vector<std::string>& archives, const ReadFile& read_file,
                  const std::function<std::string(std::vector<uint8_t>)>& describe)
{
    for (const std::string& path : archives)
    {
        std::vector<uint8_t> bytes;
        const bool read = read_file(path, bytes);
        const std::size_t size = bytes.size();
        const std::string held = read ? describe(std::move(bytes)) : "can't be read";
        std::printf("  %s: %zu bytes, %s\n", path.c_str(), size, held.c_str());
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

            // The game's SARCs are among its files.
            const auto list_sarcs = [&find_sarcs, &path]
            {
                return find_sarcs ? find_sarcs(path) : std::vector<std::string>();
            };
            to_rip.groups = FindGroups<typename Format::FileChunk>(to_rip.archive.data, find_group, tool.sar_options,
                                                                   list_sarcs, read_file);

            // Each archive's set goes in a folder of its own.
            const std::filesystem::path archive_dir = out_dir / folders[a];
            const int n =
                RipArchive(tool, archive_dir,
                           xsflib::cli::NamesIn(registries, archive_dir, tool.library_extension, tool.mini_extension),
                           game_name + " " + folders[a], std::move(to_rip), game_name, o, matched);
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
    // path from there. The search passes over an entry whose type can't be read, such as a broken symbolic link. It
    // also passes over a folder that can't be opened.
    const fs::path folder = path.parent_path();
    const auto find_group = [&folder](const std::string& name) -> std::optional<FileChunk>
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
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
    const int count = RipArchive(tool, out_dir, names, game_name, std::move(to_rip), game_name, o, matched);
    if (count < 0)
    {
        return 1;
    }
    if (matched == 0 && (!o.only_text.empty() || o.bgm))
    {
        return NoSoundMatches(o.only_text, o.bgm);
    }

    // An archive without sequences, such as one that holds only streams and wave sounds, rips nothing. The line says
    // why no file came out.
    if (matched == 0)
    {
        std::fprintf(stderr, "%s: no sequences to rip\n", file_name.c_str());
    }
    if (count > 0)
    {
        xsflib::rip::ReportOutput(out_dir);
    }

    return 0;
}

} // namespace ntdlib::rip
