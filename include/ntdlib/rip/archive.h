// SPDX-License-Identifier: MIT

// The archive mode's parts of rip tools for the Wii U's and the Switch's sound archives: the sounds to rip, the data a
// sound requires, the groups kept outside an archive, the part of an archive that a library holds, and the libraries
// and minis a rip writes. Rips of the 3DS's sound archives take the part of an archive and the minis from here too. So
// do the game modes, for the archive that their libraries hold. A format gives its types through a traits struct (see
// Format below). The messages are the same for every format. The length analysis and its timing helpers are in xsflib
// (xsflib/rip/length.h).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "ntdlib/sar/formats.h"
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

// The jobs whose sounds the length analysis found to sound. The analysis finds the other sequences that never sound.
// Some play no notes, and some play their notes at a volume of 0. Reports each sound that it leaves out, and counts it
// in `skipped`.
std::vector<Job> SkipSilent(std::vector<Job> jobs, Skipped& skipped);

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

// The part of a sound archive that a library holds: the archive's bytes, and the group files beside the archive that
// the library holds too, by their places among the archive's group files (SoundArchive::GroupFiles).
struct LibraryArchive
{
    std::vector<uint8_t> archive;
    std::vector<std::size_t> group_files;
};

// The part of `archive` that a library of the sequence sounds `sounds` holds. `archive` holds the group files that the
// rip plays with. The archive mode's models read only the files that a sequence plays: its sequence file, the files of
// the banks in its slots, and the files of the wave archives that those banks name. A file that the archive holds only
// as a copy in a group's file is read from the first group's file that holds it, in the archive or beside it.
//
// The library's archive keeps those files, and the group files in the archive that hold the copies that the models
// read. It drops every other file. The header, the STRG block and the INFO block keep their places and their bytes,
// but for three things: the archive's size, the FILE block's size and the places that the file table gives. The FILE
// block holds the kept bytes in the archive's order, laid out as NintendoWare's tools lay out a FILE block
// (ntdlib/sar/tables.h). A file whose bytes lie inside the kept bytes keeps its place there. A dropped file has no
// place. The tools write the place of a file that only groups hold the same way. The library keeps the group files
// beside the archive that hold the copies that the models read, in their order.
//
// Each kept file is read back from the library's archive and compared with the file in `archive`. Nothing when the two
// differ, when a bank can't be read, when one of `sounds` isn't a sequence sound of the archive, when the FILE block
// doesn't follow the archive's other blocks, or when the file table puts a file's place among the FILE block's files or
// before the archive's start. The library then holds the whole archive.
std::optional<LibraryArchive> KeepSequenceData(const sar::SoundArchive& archive, std::span<const uint32_t> sounds);

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

// A mini that plays sound `index` from the library `lib_name`. Rips of the 3DS's sound archives build their minis
// with it too.
template <typename Format>
std::vector<uint8_t> BuildMini(uint32_t index, const std::string& lib_name, xsflib::psf::Tags tags)
{
    typename Format::ProgramBuilder program;
    program.AddSound(kSoundItem << 24 | index);
    tags["_lib"] = lib_name;

    return program.WritePsf(Format::kPsfVersion, {}, tags);
}

} // namespace ntdlib::rip
