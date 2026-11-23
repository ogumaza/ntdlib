// SPDX-License-Identifier: MIT

// The songs and the sound effects of DSE in a game's files, read, each with its banks (ntdlib/dse/sets.h), for a
// converter.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/sequence_export/dse/performance.h"

namespace ntdlib::sequence_export::dse
{

// A song or a sound effect and its banks, read. The inputs share the files that they have in common.
struct SequenceInput
{
    std::string folder = {}; // the file's folder in the game's files, with its last '/', or empty

    // A song's file name without its extension. A sound effect's is that of its bank of sound effects, with the
    // effect's slot in three digits after an underscore: "se_sys_005".
    std::string name = {};

    ntdlib::dse::SequenceRef ref = {}; // the file, and the sound effect's slot
    std::shared_ptr<const ntdlib::dse::Song> song = {};
    std::shared_ptr<const ntdlib::dse::SoundEffects> effects = {}; // in place of a song
    uint16_t effect = 0;
    std::vector<std::shared_ptr<const ntdlib::dse::WaveBank>> banks = {};
    std::vector<std::string> bank_paths = {}; // the banks' files, in the order of `banks`
    std::shared_ptr<const ntdlib::dse::WaveBank> main_bank = {};

    // The files for Perform. They point into the input. The input must outlive them.
    SequenceFiles Files() const;
};

// The songs of `files` in the order of FindSongSets, then the sound effects in the order of FindEffectSets and their
// slots. A file that can't be read gives a line in `warnings`. So does a song without its bank, and a sound effect
// whose list names a bank that no file contains. The song is left out.
std::vector<SequenceInput> ReadSequences(std::span<const ntdlib::dse::NamedFile> files,
                                         std::vector<std::string>& warnings);

// The names of the banks' waves in the SFZ files of `inputs`, for inputs that share a folder of samples
// (BankSet::SetSfzNames). Two banks of one id in different files would give their waves the same names
// ("w<bank>_<wave>"). For each input, the result maps each such id to the stem of its bank's file, made safe for a file
// name. Where two such banks have one stem, or a stem matches the "w<bank>" of a kept name, the banks take their paths
// without the extension, made safe. The other banks keep their names. Each input's `bank_paths` must name its banks.
std::vector<std::map<uint16_t, std::string>> SfzBankNames(std::span<const SequenceInput* const> inputs);

} // namespace ntdlib::sequence_export::dse
