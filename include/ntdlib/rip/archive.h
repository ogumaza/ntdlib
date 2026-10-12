// SPDX-License-Identifier: MIT

// The archive mode's parts of rip tools for the Wii U's and the Switch's sound archives: the sounds to rip, the data a
// sound requires, the groups kept outside an archive, and the libraries and minis a rip writes. A format gives its
// types through a traits struct (see Format below). The messages are the same for every format. The length analysis and
// its timing helpers are in xsflib (xsflib/rip/length.h).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/sarc.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

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

// A sound to rip: its index in the archive, its label, and its timing once the length analysis has worked it out.
struct Job
{
    uint32_t index;
    std::string label;
    std::optional<Timing> timing;
};

// The sequence sounds whose labels match `only`, and with `bgm` only those that are music (IsMusic). Streams and wave
// sounds aren't ripped.
std::vector<Job> SelectSounds(const sar::SoundArchive& archive, const std::regex& only, bool bgm);

// Reports that no sound matched --only (`only_text`, empty if it wasn't given) or --bgm. Returns the exit code, 1.
int NoSoundMatches(const std::string& only_text, bool bgm);

// Whether a file holds a group, by the group file's name (ntdlib/sar/formats.h).
using sar::IsGroupFileName;

// The group files that the archive's external groups name, found with `find`. While a group is still missing, the
// archive's group files in the SARC archives that `list_sarcs` lists are added too, in that order (see
// SoundArchive::MatchesGroupFile). `read_sarc` reads a SARC. A group file from a SARC is kept at the SARC's path, a
// slash and the file's name in the SARC. A file without a name is kept under its index among the files that SarcFiles
// gives. A group that isn't found is reported. If a SARC held a group file of the archive, no group is reported. A
// group file doesn't say which group it is. An archive that can't be read with `options` gives none: the rip reports
// it.
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
            std::fprintf(stderr, "warning: group %s is kept outside the archive, and %s.bfgrp isn't there\n",
                         name.c_str(), name.c_str());
        }
    }

    return groups;
}

// A format's traits for the libraries and minis a rip writes:
//
//   struct Format
//   {
//       using ProgramBuilder = ...;    // AddFileRef(path, data), AddSound(id) and WritePsf(version, reserved, tags)
//       using ArchiveDescriptor = ...; // archive_path, group_paths and Serialize()
//       using FileChunk = ...;         // path and data
//       static constexpr uint8_t kPsfVersion = ...;
//   };

// A library holding the sound archive and the group files that its sequences require, with `settings` as its
// descriptor.
template <typename Format>
std::vector<uint8_t> BuildArchiveLibrary(const typename Format::FileChunk& archive,
                                         const std::vector<typename Format::FileChunk>& groups,
                                         typename Format::ArchiveDescriptor settings, const xsflib::psf::Tags& tags)
{
    settings.archive_path = archive.path;
    settings.group_paths.clear();
    typename Format::ProgramBuilder program;
    program.AddFileRef(archive.path, archive.data);
    for (const typename Format::FileChunk& group : groups)
    {
        settings.group_paths.push_back(group.path);
        program.AddFileRef(group.path, group.data);
    }

    return program.WritePsf(Format::kPsfVersion, settings.Serialize(), tags);
}

// A mini that plays sound `index` from the library `lib_name`.
template <typename Format>
std::vector<uint8_t> BuildMini(uint32_t index, const std::string& lib_name, xsflib::psf::Tags tags)
{
    typename Format::ProgramBuilder program;
    program.AddSound(kSoundItem << 24 | index);
    tags["_lib"] = lib_name;

    return program.WritePsf(Format::kPsfVersion, {}, tags);
}

} // namespace ntdlib::rip
