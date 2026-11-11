// SPDX-License-Identifier: MIT

// The songs and the sound effects of DSE in a game's files, read, each with its banks (ntdlib/dse/sets.h), for a
// converter.

#pragma once

#include <cstdint>
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
    std::shared_ptr<const ntdlib::dse::WaveBank> main_bank = {};

    // The files for Perform. They point into the input, which must outlive them.
    SequenceFiles Files() const;
};

// The songs of `files` in the order of FindSongSets, then the sound effects in the order of FindEffectSets and their
// slots. A file that can't be read gives a line in `warnings`. So does a song without its bank, which is left out, and
// a sound effect whose list names a bank that no file holds.
std::vector<SequenceInput> ReadSequences(std::span<const ntdlib::dse::NamedFile> files,
                                         std::vector<std::string>& warnings);

} // namespace ntdlib::sequence_export::dse
