// SPDX-License-Identifier: MIT

// The archive mode's parts of a rip tool for the DS's sound archives (SDAT): the sequences to rip, what a sequence
// requires, whether it's music, the part of an archive that a library holds, and the libraries and minis that a rip
// writes. ntdlib/rip/archive.h gives the same parts for the later consoles. A sequence plays on ArchivePlayer
// (archive_player.h), and its length comes from MeasureSequence (length_model.h).

#pragma once

#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"

namespace ntdlib::ntr::rip
{

// A sequence to rip: where the archive holds it, its label, and its timing once the length analysis has worked it out.
struct Job
{
    SequenceRef sequence;
    std::string label;
    std::optional<xsflib::rip::Timing> timing;
};

// A sequence's label (ntdlib::ntr::Label in archive_player.h).
using ntdlib::ntr::Label;

// Whether a rip tool's --bgm picks `sequence` as music: a sequence whose label has BGM in it, or one that plays on a
// music player. Games give their music players names with BGM, MUSIC, SONG or FANFARE among the words that
// underscores separate (PLAYER_BGM), even where the music's labels don't say so. A player whose name also has the word
// SE plays sound effects.
bool IsMusic(const sdat::Archive& archive, const SequenceRef& sequence);

// The archive's sequences, then each sequence of each sequence archive, whose labels match `only`, and with `bgm` only
// those that are music (IsMusic).
std::vector<Job> SelectSequences(const sdat::Archive& archive, const std::regex& only, bool bgm);

// What `sequence` requires that the archive doesn't hold: its file, its bank's record and file, and the files of the
// wave archives that the bank names. Empty when nothing is missing.
std::vector<std::string> MissingData(const sdat::Archive& archive, const SequenceRef& sequence);

// The archive that a library of the sequences `sequences` holds. The tables keep every record. A sequence therefore
// keeps its number. The FILE block keeps the files that the sequences play: each sequence's file or sequence archive's
// file, its bank's file and the files of the wave archives that the bank names. Every other file is empty. The archive
// is laid out as the SDK's tools lay one out (sdat::BuildArchive). Each kept file is read back from the library and
// compared with the file in `archive`. Nothing when the two differ, when one of `sequences` lacks data (MissingData),
// or when the archive's layout isn't the tools' layout (sdat::ReadArchiveModel). The library then holds the whole
// archive.
std::optional<std::vector<uint8_t>> KeepSequenceData(const sdat::Archive& archive,
                                                     std::span<const SequenceRef> sequences);

// The number that a mini gives a sequence: its number, or for a sequence archive's sequence, bit 31 set, the archive's
// number in bits 16 to 30 and the sequence's in bits 0 to 15. Throws std::out_of_range for a number that its bits
// can't hold.
uint32_t SoundId(const SequenceRef& sequence);
SequenceRef SequenceOf(uint32_t sound_id);

// A format's traits for the libraries and minis that a rip writes. ntdlib/rip/archive.h takes the same traits:
//
//   struct Format
//   {
//       using ProgramBuilder = ...;    // AddFileRef(path, data), AddSound(id) and WritePsf(version, reserved, tags)
//       using ArchiveDescriptor = ...; // archive_path and Serialize()
//       static constexpr uint8_t kPsfVersion = ...;
//   };

// A library holding the sound archive `archive` at `path`, with `settings` as its descriptor.
template <typename Format>
std::vector<uint8_t> BuildArchiveLibrary(const std::string& path, std::span<const uint8_t> archive,
                                         typename Format::ArchiveDescriptor settings, const xsflib::psf::Tags& tags)
{
    settings.archive_path = path;
    typename Format::ProgramBuilder program;
    program.AddFileRef(path, archive);
    return program.WritePsf(Format::kPsfVersion, settings.Serialize(), tags);
}

// A mini that plays `sequence` from the library `lib_name`.
template <typename Format>
std::vector<uint8_t> BuildMini(const SequenceRef& sequence, const std::string& lib_name, xsflib::psf::Tags tags)
{
    typename Format::ProgramBuilder program;
    program.AddSound(SoundId(sequence));
    tags["_lib"] = lib_name;
    return program.WritePsf(Format::kPsfVersion, {}, tags);
}

} // namespace ntdlib::ntr::rip
