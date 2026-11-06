// SPDX-License-Identifier: MIT

// The parts of a rip tool for DSE's songs and sound effects: the sequences to rip, what a sequence requires, whether
// it's music, the files that a library holds, and the libraries and minis that a rip writes. ntdlib/ntr/rip.h gives
// the same parts for the DS's sound archives. A rip's files are a game's files of DSE at their paths in the game's file
// system: the songs (smdl), the wave banks (swdl) and the banks of sound effects (sedl). A sequence plays on
// dse::Player with the banks that FindSequenceSet (sets.h) finds for it (archive_source.h), and its length comes from
// MeasureSequence (length_model.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "ntdlib/dse/sets.h"

namespace ntdlib::dse::rip
{

// A sequence to rip: the song or the sound effect, its label, and its timing once the length analysis has worked it
// out.
struct Job
{
    SequenceRef sequence = {};
    std::string label = {};
    std::optional<xsflib::rip::Timing> timing = {};
};

// A sequence's label: a song's file name without its extension, such as bgm0001, or a bank of sound effects' with the
// effect's slot in three digits, such as se_sys_005.
std::string Label(const SequenceRef& sequence);

// Whether a rip tool's --bgm picks `sequence` as music: a song is music, and a sound effect isn't.
bool IsMusic(const SequenceRef& sequence);

// The songs of `files`, then each sound effect of each of their banks of sound effects, in the order of the files,
// whose labels match `only`, and with `bgm` only the songs. A slot of a bank of sound effects without a sequence plays
// nothing, and a bank that can't be read gives no sound effects.
std::vector<Job> SelectSequences(std::span<const NamedFile> files, const std::regex& only, bool bgm);

// What `sequence` requires that `files` don't hold: its file, the song's wave bank, the banks of the effect's list,
// and the main bank of a bank whose waves lie in one. Empty when nothing is missing.
std::vector<std::string> MissingData(std::span<const NamedFile> files, const SequenceRef& sequence);

// The files that a library of the sequences `sequences` holds: indices into `files`, in their order. They are each
// sequence's song or bank of sound effects, and the banks and the main bank that it plays (FindSequenceSet). Each
// sequence is found again among the kept files, and must play the same files. Nothing when the kept files give a
// sequence other files, or when one of `sequences` lacks data (MissingData). The library then holds every file.
std::optional<std::vector<std::size_t>> KeepSequenceData(std::span<const NamedFile> files,
                                                         std::span<const SequenceRef> sequences);

// The number that a mini gives a sequence of the library's files `files`. Bits 16 to 31 give the place of the
// sequence's file among the files' paths in sorted order. For a sound effect, bits 0 to 15 give its slot plus 1, and
// for a song 0. Throws std::out_of_range when the files lack the sequence's file, or for a number that its bits can't
// hold.
uint32_t SoundId(std::span<const NamedFile> files, const SequenceRef& sequence);
SequenceRef SequenceOf(std::span<const NamedFile> files, uint32_t sound_id);

// A format's traits for the libraries and minis that a rip writes. ntdlib/ntr/rip.h takes the same traits:
//
//   struct Format
//   {
//       using ProgramBuilder = ...;    // AddFileRef(path, data), AddSound(id) and WritePsf(version, reserved, tags)
//       static constexpr uint8_t kPsfVersion = ...;
//   };
//
// A library's descriptor gives its reserved area through Serialize(), such as the build of DSE that plays the files.

// A library holding the files `kept` of `files` (KeepSequenceData) at their paths, with `descriptor`.
template <typename Format, typename Descriptor>
std::vector<uint8_t> BuildLibrary(std::span<const NamedFile> files, std::span<const std::size_t> kept,
                                  const Descriptor& descriptor, const xsflib::psf::Tags& tags)
{
    typename Format::ProgramBuilder program;
    for (const std::size_t index : kept)
    {
        program.AddFileRef(files[index].path, files[index].bytes);
    }
    return program.WritePsf(Format::kPsfVersion, descriptor.Serialize(), tags);
}

// A mini that plays `sequence` from the library `lib_name`, whose files are `files`.
template <typename Format>
std::vector<uint8_t> BuildMini(std::span<const NamedFile> files, const SequenceRef& sequence,
                               const std::string& lib_name, xsflib::psf::Tags tags)
{
    typename Format::ProgramBuilder program;
    program.AddSound(SoundId(files, sequence));
    tags["_lib"] = lib_name;
    return program.WritePsf(Format::kPsfVersion, {}, tags);
}

} // namespace ntdlib::dse::rip
