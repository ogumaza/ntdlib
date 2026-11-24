// SPDX-License-Identifier: MIT

// The archive mode's parts of rip tools for the Wii U's and the Switch's sound archives: the sounds to rip, the data a
// sound requires, the groups kept outside an archive, the part of an archive that a library holds, and the libraries
// and minis a rip writes. Rips of the 3DS's sound archives take the part of an archive and the minis from here too. So
// do the game modes, for the archive that their libraries hold. A format gives its types through a traits struct (see
// Format below). The messages are the same for every format. The length analysis and its timing helpers are in xsflib
// (xsflib/rip/length.h).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"
#include "xsflib/rip/tool.h"

#include "ntdlib/file_paths.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/game_rules.h"
#include "ntdlib/sar/sarc.h"

namespace ntdlib::rip
{

// An item ID's types and the index of an item (ntdlib/sar/formats.h).
using sar::ItemIndex;
using sar::kBankItem;
using sar::kPlayerItem;
using sar::kSoundItem;
using sar::kWaveArchiveItem;

// A sequence's timing, and the timing of a sequence that ends or loops at a frame (xsflib/rip/length.h).
using xsflib::rip::EndAt;
using xsflib::rip::LoopAt;
using xsflib::rip::Timing;

// The names of the groups that the archive lists without a file: the game keeps them outside the archive and loads them
// itself. Groups without a name are left out.
std::vector<std::string> ExternalGroups(const sar::SoundArchive& archive);

// What sound `index` requires that the archive doesn't hold: its sequence data, banks the archive doesn't list or keeps
// in other files, and the wave archives that its banks name. Empty when nothing is missing. The banks are read with
// `options`.
std::vector<std::string> MissingData(const sar::SoundArchive& archive, uint32_t index, const sar::Options& options);

// Whether a rip tool's --bgm picks the sound at `index` as music: a sequence whose label has BGM in it, or one that
// plays on a music player. Games give their music players names with BGM, MUSIC, SONG or FANFARE among the words that
// underscores separate (PLAYER_BGM, SONG_PLAYER), even where the music's labels don't say so. A player whose name also
// has the word SE plays sound effects.
bool IsMusic(const sar::SoundArchive& archive, uint32_t index);

// The tracks of a sequence: 16.
constexpr std::size_t kSequenceTracks = 16;

// The volumes of a sequence's tracks that the game's code sets through the sound's handle (the sequence player's
// SetTrackVolume), track 0 first. A volume of 1 leaves a track as the sequence plays it.
using TrackVolumes = std::array<float, kSequenceTracks>;

// Every track at 1: what the sound library gives a sequence's tracks as the sound starts.
constexpr TrackVolumes kFullTrackVolumes = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                            1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

// Every track at 1, but the tracks in the mask `muted`, a bit for each, at 0.
constexpr TrackVolumes TrackVolumesOf(uint16_t muted)
{
    TrackVolumes volumes = kFullTrackVolumes;
    for (std::size_t t = 0; t < volumes.size(); ++t)
    {
        if ((muted >> t & 1) != 0)
        {
            volumes[t] = 0.0f;
        }
    }

    return volumes;
}

// A sound to rip: its index in the archive, its label, its timing once the length analysis has worked it out, the
// volumes that the game's code gives its tracks, and the reverb that the game's code keeps on aux bus B while the sound
// plays. A game can play a sound in more than one state, each with other track volumes or another reverb
// (ArchiveRipTool::sound_states). Each state is a separate job, under a label that names the state. A job can also give
// the variables that the game sets as it starts the sound, by their numbers: a variant of a sound that waits for them
// (ArchiveRipTool::variants in archive_mode.h), or a state. The length analysis sets them before the sequence's first
// tick. A state can also give the tempo ratio and the pitch that the game sets as it starts the sound. A sound that
// never sounds when the game starts it at once can sound in another state that the game can give it.
// A tool's length analysis may then give the job the first of `start_states` in which it sounds, the archive's rules
// for its variables (sar::StartsOf). Or the analysis may give the job the frames that the game's sound system runs
// before the start (`start_frame`): the sound library steps its random numbers once a frame. The same tool's minis
// start the sound that way. The other tools leave both alone.
struct Job
{
    uint32_t index;
    std::string label;
    std::optional<Timing> timing;
    float volume = 1.0f; // the factor that the game's code gives the sound (ArchiveRipTool::sound_volume)
    TrackVolumes track_volumes = kFullTrackVolumes;
    xsflib::psf::Variables variables = {};
    std::span<const sar::VariableValues> start_states = {};
    uint32_t start_frame = 0;

    // The reverb, one of the archive's rules (sar::ArchiveRules::bus_b_presets), or none to leave bus B as the tool's
    // settings have it.
    const sar::ReverbPreset* bus_b_reverb = nullptr;

    // The tempo ratio that the game sets (SequenceSoundHandle::SetTempoRatio), and the pitch that it sets
    // (SoundHandle::SetPitch) as a ratio of frequencies. They come last. Positional initializers of the older fields
    // then keep working.
    float tempo_ratio = 1.0f;
    float pitch = 1.0f;
};

// The jobs that rip each job of `selected` in each of its states, in order. `states_of(job)` gives the job's states, a
// range of objects with a `name`, the state that the game starts the sound in first. A job without states stays as it
// is. Each state gets a copy of the job, under the job's label, a space and the state's name, or under the job's label
// alone for a state without a name. `apply(state, copy)` then sets the state's values in the copy.
template <typename StatesOf, typename Apply>
std::vector<Job> WithStates(const std::vector<Job>& selected, const StatesOf& states_of, const Apply& apply)
{
    std::vector<Job> jobs;
    for (const Job& job : selected)
    {
        const auto states = states_of(job);
        if (std::ranges::empty(states))
        {
            jobs.push_back(job);
            continue;
        }

        for (const auto& state : states)
        {
            Job in_state = job;
            if (!state.name.empty())
            {
                in_state.label += " ";
                in_state.label += state.name;
            }

            apply(state, in_state);
            jobs.push_back(std::move(in_state));
        }
    }

    return jobs;
}

// The sequence sounds whose labels match `only`, and with `bgm` only those that are music (IsMusic). Streams and wave
// sounds aren't ripped.
std::vector<Job> SelectSounds(const sar::SoundArchive& archive, const std::regex& only, bool bgm);

// Reports that no sound matched --only or --bgm, and returns the exit code (xsflib/rip/tool.h).
using xsflib::rip::NoSoundMatches;

// The sounds that a rip leaves out: those whose data isn't all there, and those that make no sound.
struct Skipped
{
    std::size_t missing_data = 0;
    std::size_t silent = 0;
};

// The jobs whose sounds have all their data (MissingData, with `options`) and a volume above 0 in the archive. A
// sequence whose data isn't all there would play with missing instruments. A sound whose volume is 0 makes no sound:
// the game's sound library multiplies everything the sound plays by that volume. Reports each sound that it leaves
// out, and counts it in `skipped`.
std::vector<Job> SkipIncomplete(const sar::SoundArchive& archive, std::vector<Job> jobs, const sar::Options& options,
                                Skipped& skipped);

// Loads the sound archive `bytes` of a rip, read from `path`, with the archive reader's `options`. Prints an error and
// returns nothing when the archive can't be read. Warns that a truncated archive's sounds whose data is past its end
// are skipped.
std::optional<sar::SoundArchive> LoadArchiveToRip(const std::vector<uint8_t>& bytes, const std::string& path,
                                                  const sar::Options& options);

// Whether a file holds a group, by the group file's name (ntdlib/sar/formats.h).
using sar::IsGroupFileName;

// The group files that the archive's external groups name, found with `find`. If `find` finds fewer group files than
// the archive keeps outside it, every group file of the archive in the SARC archives that `list_sarcs` lists is added
// too, in that order (see SoundArchive::MatchesGroupFile). `read_sarc` reads a SARC. A group file from a SARC is kept
// at the SARC's path, a slash and the file's name in the SARC. A file without a name is kept under its index among the
// files that SarcFiles gives. A group that isn't found is reported. If a SARC held a group file of the archive, no
// group is reported. A group file doesn't say which group it is. An archive that can't be read with `options` gives
// none: the rip reports it.
template <typename FileChunk>
std::vector<FileChunk> FindGroups(
    const std::vector<uint8_t>& archive_bytes,
    const std::function<std::optional<FileChunk>(const std::string& name)>& find, const sar::Options& options,
    const std::function<std::vector<std::string>()>& list_sarcs = {},
    const std::function<bool(const std::string& path, std::vector<uint8_t>& bytes)>& read_sarc = {})
{
    std::vector<FileChunk> groups;
    std::vector<std::string> missing;
    bool from_sarcs = false;
    try
    {
        const sar::SoundArchive archive = sar::SoundArchive::Load(archive_bytes, options);
        for (const std::string& name : ExternalGroups(archive))
        {
            if (std::optional<FileChunk> group = find(name))
            {
                groups.push_back(std::move(*group));
            }
            else
            {
                missing.push_back(name);
            }
        }

        // A game can also keep its group files in SARC archives beside the archive, under other names. The groups kept
        // outside the archive include those without a name.
        const auto outside = std::ranges::count_if(
            archive.Groups(), [](const sar::GroupInfo& group) { return group.file_id == sar::kExternalGroupFile; });
        if (groups.size() < static_cast<std::size_t>(outside) && list_sarcs && read_sarc)
        {
            for (const std::string& path : list_sarcs())
            {
                std::vector<uint8_t> sarc;
                if (!read_sarc(path, sarc))
                {
                    continue;
                }

                const std::vector<sar::SarcFile> files = sar::SarcFiles(sarc);
                for (std::size_t i = 0; i < files.size(); i++)
                {
                    if (archive.MatchesGroupFile(files[i].data))
                    {
                        const std::string name = files[i].name.empty() ? std::to_string(i) : files[i].name;
                        groups.push_back(FileChunk{path + "/" + name, {files[i].data.begin(), files[i].data.end()}});
                        from_sarcs = true;
                    }
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return {}; // the rip reports an archive it can't read
    }

    // The warnings name groups. With group files from SARCs, the groups still missing aren't known.
    if (!from_sarcs)
    {
        for (const std::string& name : missing)
        {
            std::fprintf(stderr, "warning: group %s is kept outside the archive, and %s isn't there\n", name.c_str(),
                         sar::GroupFileName(name, options.flavor).c_str());
        }
    }

    return groups;
}

// The file of group `name` of an archive of `flavor` among the files at `paths` in a game, such as the paths of the
// files of a title or a disc. A file matches by its file name, the part of its path after the last '/', in any case
// (IsGroupFileName). `read`, a call (const std::string& path, std::vector<uint8_t>& data) -> bool, reads a file. The
// first file that matches and that `read` reads is the group file. Nothing if none. The default flavor selects
// ".bfgrp", the extension on the Wii U and the Switch.
template <typename FileChunk, typename Paths, typename Read>
std::optional<FileChunk> FindGroupFile(const Paths& paths, const Read& read, const std::string& name,
                                       sar::Flavor flavor = sar::Flavor::kCafe)
{
    for (const std::string& path : paths)
    {
        if (!IsGroupFileName(ntdlib::detail::FileNameOf(path), name, flavor))
        {
            continue;
        }

        FileChunk group{path, {}};
        if (read(path, group.data))
        {
            return group;
        }
    }

    return std::nullopt;
}

// The file of group `name` of an archive of `flavor` in `folder` or a folder below it, by its file name in any case
// (IsGroupFileName). The path of the chunk is the path from `folder` to the file in UTF-8, with a '/' between folders
// on every platform. The search passes over an entry whose type can't be read, such as a broken symbolic link. It also
// passes over a folder that refuses permission, such as System Volume Information at the root of an NTFS drive. Any
// other error ends the search. Nothing if no file matches and reads, or if `folder` can't be opened.
template <typename FileChunk>
std::optional<FileChunk> FindGroupFileIn(const std::filesystem::path& folder, const std::string& name,
                                         sar::Flavor flavor)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec))
    {
        std::error_code type_error;
        if (!it->is_regular_file(type_error) || !IsGroupFileName(xsflib::Utf8(it->path().filename()), name, flavor))
        {
            continue;
        }

        FileChunk group{{}, {}};
        for (const fs::path& part : it->path().lexically_relative(folder))
        {
            group.path += (group.path.empty() ? "" : "/") + xsflib::Utf8(part);
        }

        if (xsflib::ReadFile(it->path(), group.data))
        {
            return group;
        }
    }

    return std::nullopt;
}

// The SARC archives among the files at `paths` that are in the folder of the sound archive at `archive_path`, not in a
// folder below it, in the order of their paths' UTF-8 bytes. A SARC archive's path ends in ".sarc". A game can keep
// group files in them under other names (FindGroups).
template <typename Paths>
std::vector<std::string> SarcsBeside(const Paths& paths, const std::string& archive_path)
{
    const std::string folder = ntdlib::detail::FolderOf(archive_path);
    std::vector<std::string> sarcs;
    for (const std::string& path : paths)
    {
        if (path.starts_with(folder) && path.find('/', folder.size()) == std::string::npos && path.ends_with(".sarc"))
        {
            sarcs.push_back(path);
        }
    }
    std::ranges::sort(sarcs);

    return sarcs;
}

// The part of a sound archive that a library holds: the archive's bytes, and the group files beside the archive that
// the library holds too, by their places among the archive's group files (SoundArchive::GroupFiles).
struct LibraryArchive
{
    std::vector<uint8_t> archive;
    std::vector<std::size_t> group_files;
};

// The part of `archive` that a library of the sequence sounds `sounds` holds. `archive` holds the group files that the
// rip plays with. The archive mode's models read only the files that a sequence plays: its sequence file, the files of
// the banks in its slots, and the files of the wave archives that those banks name. `start_banks` lists the item IDs
// of other banks that a game gives the sequences as it starts them (ctr::snd::SequenceStart::banks). The models read
// their files and the files of the wave archives that they name too. A file that the archive holds only as a copy in a
// group's file is read from the first group's file that holds it, in the archive or beside it.
//
// The library's archive keeps those files, and the group files in the archive that hold the copies that the models
// read. It drops every other file. The header, the STRG block and the INFO block keep their places and their bytes, but
// for three things: the archive's size, the FILE block's size and the places that the file table gives. The FILE block
// holds the kept bytes in the archive's order, laid out as NintendoWare's tools lay out a FILE block
// (ntdlib/sar/tables.h). A file whose bytes are inside the kept bytes keeps its place there. A dropped file has no
// place. The tools write the place of a file that only groups hold the same way. The library keeps the group files
// beside the archive that hold the copies that the models read, in their order.
//
// Each kept file is read back from the library's archive and compared with the file in `archive`. Nothing when the two
// differ, when a bank can't be read, when one of `sounds` isn't a sequence sound of the archive, when the FILE block
// doesn't follow the archive's other blocks, or when the file table puts a file's place among the FILE block's files or
// before the archive's start. The library then holds the whole archive.
std::optional<LibraryArchive> KeepSequenceData(const sar::SoundArchive& archive, std::span<const uint32_t> sounds,
                                               std::span<const uint32_t> start_banks = {});

// The group files beside the archive that `library` holds, cut down to the copies that the models read. `library` is
// KeepSequenceData's part of `archive` for the sequence sounds `sounds` and the banks `start_banks`. A cut group file
// keeps its header, its INFO block and the blocks after its FILE block, such as INFX, with their bytes. Its FILE block
// holds the copies that the models read, in the group file's order. Each starts on a multiple of 32 bytes from the
// file's start, or of 64 bytes on the Switch, as KeepSequenceData lays out the archive's files. The FILE block ends on
// such a multiple. The first item of each kept copy gives the copy's new place. Every other item with a copy becomes a
// link to the archive's file, as NintendoWare's tools write a link: a reference of type 0, with 0xFFFFFFFF for its
// offset and the size. The blocks after the FILE block follow it. The header and the table of blocks give the new sizes
// and places.
//
// The cut group files come in the order of `library.group_files`. Each kept file is read back from the library's
// archive with them, and compared with the file in `archive`. Nothing when the two differ, when a bank can't be read,
// when one of `sounds` isn't a sequence sound of the archive, when a copy that the models read isn't an item's copy, or
// when a group file's blocks don't sit as the tools lay them out: the INFO block and any other before the FILE block,
// and the rest after it. The library then holds the whole group files.
std::optional<std::vector<std::vector<uint8_t>>> KeepGroupCopies(const sar::SoundArchive& archive,
                                                                 const LibraryArchive& library,
                                                                 std::span<const uint32_t> sounds,
                                                                 std::span<const uint32_t> start_banks = {});

// The files of a sound archive and of the group files beside it that a library contains (KeepLibraryFiles). A group
// file is a FileChunk: its path and its data.
template <typename FileChunk>
struct LibraryFiles
{
    std::vector<uint8_t> archive;
    std::vector<FileChunk> groups;
};

// The part of `archive` that a library of the sequence sounds `sounds` contains, with the banks `start_banks`
// (KeepSequenceData), and the group files of `groups` that contain copies of the files that they play. `archive` is
// the archive at `archive_path`, with the group files of `groups` added in their order. With `cut_group_copies`, those
// group files are cut down to the copies (KeepGroupCopies). Nothing after a warning when the files can't be cut out of
// the archive: the library then contains the whole archive and every group file. When only the copies can't be cut
// out of the group files, a warning says that the library contains those group files whole.
template <typename FileChunk>
std::optional<LibraryFiles<FileChunk>> KeepLibraryFiles(const sar::SoundArchive& archive,
                                                        const std::string& archive_path,
                                                        const std::vector<FileChunk>& groups,
                                                        std::span<const uint32_t> sounds,
                                                        std::span<const uint32_t> start_banks, bool cut_group_copies)
{
    std::optional<LibraryArchive> kept = KeepSequenceData(archive, sounds, start_banks);
    if (!kept)
    {
        std::fprintf(stderr,
                     "warning: the files that the sequences play can't be cut out of %s. The library holds the whole "
                     "archive\n",
                     archive_path.c_str());
        return std::nullopt;
    }

    std::optional<std::vector<std::vector<uint8_t>>> cut;
    if (cut_group_copies)
    {
        cut = KeepGroupCopies(archive, *kept, sounds, start_banks);
        if (!cut)
        {
            std::fprintf(stderr,
                         "warning: the copies that the sequences play can't be cut out of the group files. The "
                         "library holds the group files that hold them whole\n");
        }
    }

    LibraryFiles<FileChunk> files{std::move(kept->archive), {}};
    for (std::size_t i = 0; i < kept->group_files.size(); i++)
    {
        const FileChunk& group = groups[kept->group_files[i]];
        files.groups.push_back(FileChunk{group.path, cut ? std::move((*cut)[i]) : group.data});
    }

    return files;
}

// The numbers of an archive's sounds of each type.
struct SoundCounts
{
    std::size_t sequences = 0;
    std::size_t wave_sounds = 0;
    std::size_t streams = 0;
};

SoundCounts CountSounds(const sar::SoundArchive& archive);

// The archive that a game mode's library holds for the sequence sounds `sounds`. The game's sound library reads the
// archive's header and its INFO block. For each sound that it loads, it then reads the files that the sound plays: the
// sequence file, the files of the banks in its slots and the files of the wave archives that those banks name. It reads
// each file at the place that the file table gives. A read ends on a multiple of `read_alignment` bytes from the
// archive's start. A wave archive whose waves load one by one is read in parts, within the same bytes.
//
// The copy keeps the archive's bytes outside the FILE block's files. In the FILE block, it keeps the bytes of each file
// that the sounds play, rounded out to multiples of `read_alignment` bytes. The block's other bytes are zeros. The copy
// keeps the archive's size and its layout. The game therefore reads the same bytes in the same places. Each kept file
// is read back from the copy and compared with the file in `archive`. Nothing when one of `sounds` isn't a sequence
// sound of the archive, when a bank can't be read, when the FILE block doesn't follow the archive's other blocks, when
// a file that the sounds play has no bytes of its own in the FILE block, or when a kept file reads back differently.
// The library then holds the whole archive.
std::optional<std::vector<uint8_t>> KeepLoadedFiles(const sar::SoundArchive& archive, std::span<const uint32_t> sounds,
                                                    std::size_t read_alignment);

// A format's traits for the libraries and minis a rip writes:
//
//   struct Format
//   {
//       using ProgramBuilder = ...;    // AddFileRef(path, data), AddSound(id) and WritePsf(version, reserved, tags)
//       using ArchiveDescriptor = ...; // archive_path, group_paths and Serialize()
//       using FileChunk = ...;         // path and data
//       static constexpr uint8_t kPsfVersion = ...;
//   };

// The descriptor of a library that holds the sound archive `archive` and the group files `groups`: `settings` with the
// paths of the archive and of the group files. A mini whose reserved area takes the place of the library descriptor
// starts from it too.
template <typename Format>
typename Format::ArchiveDescriptor LibraryDescriptor(const typename Format::FileChunk& archive,
                                                     const std::vector<typename Format::FileChunk>& groups,
                                                     typename Format::ArchiveDescriptor settings)
{
    settings.archive_path = archive.path;
    settings.group_paths.clear();
    for (const typename Format::FileChunk& group : groups)
    {
        settings.group_paths.push_back(group.path);
    }

    return settings;
}

// A library holding the sound archive and the group files that its sequences require, with `settings` as its
// descriptor.
template <typename Format>
std::vector<uint8_t> BuildArchiveLibrary(const typename Format::FileChunk& archive,
                                         const std::vector<typename Format::FileChunk>& groups,
                                         typename Format::ArchiveDescriptor settings, const xsflib::psf::Tags& tags)
{
    const typename Format::ArchiveDescriptor descriptor =
        LibraryDescriptor<Format>(archive, groups, std::move(settings));
    typename Format::ProgramBuilder program;
    program.AddFileRef(archive.path, archive.data);
    for (const typename Format::FileChunk& group : groups)
    {
        program.AddFileRef(group.path, group.data);
    }

    return program.WritePsf(Format::kPsfVersion, descriptor.Serialize(), tags);
}

// A mini that plays sound `index` from the library `lib_name`, with `reserved` as its reserved area. A descriptor
// there takes the place of the library's. Rips of the 3DS's sound archives build their minis with it too.
template <typename Format>
std::vector<uint8_t> BuildMini(uint32_t index, const std::string& lib_name, xsflib::psf::Tags tags,
                               const std::vector<uint8_t>& reserved = {})
{
    typename Format::ProgramBuilder program;
    program.AddSound(kSoundItem << 24 | index);
    tags["_lib"] = lib_name;

    return program.WritePsf(Format::kPsfVersion, reserved, tags);
}

// A mini that plays the sounds at `indices` together, in that order, from the library `lib_name`: a `SND ` chunk with a
// list. `reserved` is as BuildMini's. A list of one sound gives BuildMini's mini of that sound. Throws
// std::invalid_argument for an empty list.
template <typename Format>
std::vector<uint8_t> BuildMiniOfSounds(std::span<const uint32_t> indices, const std::string& lib_name,
                                       xsflib::psf::Tags tags, const std::vector<uint8_t>& reserved = {})
{
    std::vector<uint32_t> ids;
    for (const uint32_t index : indices)
    {
        ids.push_back(kSoundItem << 24 | index);
    }

    typename Format::ProgramBuilder program;
    program.AddSounds(ids);
    tags["_lib"] = lib_name;

    return program.WritePsf(Format::kPsfVersion, reserved, tags);
}

} // namespace ntdlib::rip
