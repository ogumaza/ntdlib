// SPDX-License-Identifier: MIT

// A game's DSE files cut into pieces for an editor. The pieces make up one set. The set is a container of this library,
// not a file of the game. It starts with a directory: the magic word DSET, the set's size, the number of files and 4
// zeros, then for each file its offset from the set's start, its size and its path's offset, all 32-bit and
// little-endian, then the paths, each ended by a zero. Each file follows on a multiple of 16 bytes, in the order of the
// list.
//
// Each song's tracks and each sound effect's tracks become a sequence piece (commands.h). The other bytes stay in runs.
// Each field that holds an offset or a size becomes a distance between two places in the pieces: a file's size in its
// header and in the directory, its offset in the directory, a bank of sound effects' seq chunk's size, and each slot of
// that chunk's table. A slot holds 16 bits. A label at item 0 names each sequence (SequenceLabel). The pieces of a set
// join to give each file back byte for byte. After an edit, each sequence takes the size that its commands give, and
// what follows it moves.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::dse
{

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<commands::Sequence> sequences; // in the order of their pieces
    std::vector<sar::Piece> pieces;            // in the order of the set's bytes
    std::vector<sar::Field> fields;
};

// Cuts `files` into a set. Each song and each bank of sound effects that the readers read becomes pieces when the
// encoder gives its tracks back. Any other file stays one run. So does a bank of sound effects whose seq chunk holds
// other bytes after its last sound effect, or other bytes than zeros after the chunk's end.
Layout Cut(std::span<const NamedFile> files);

// The set's bytes from its pieces, whose sequences are in `sequences`, with `fields` written. Throws
// std::invalid_argument for a piece or a place that names a sequence or a piece that the set lacks, for what
// EncodeTracks refuses, for a field outside a run or of another size than 2 or 4 bytes, and for a distance below zero
// or past the field's bits: a bank of sound effects can't hold more than 64 KiB of sound effects.
std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields);

// Where each of `pieces` starts in the set that they join to, and then the set's end. Throws what Join throws for a
// piece.
std::vector<uint64_t> Starts(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces);

// Whether `bytes` start as a set.
bool IsSet(std::span<const uint8_t> bytes);

// The files of `set`, in its order. Their bytes point into `set`. Throws xsflib::FormatError for a directory that
// doesn't fit the set.
std::vector<NamedFile> ReadSet(std::span<const uint8_t> set);

// A sequence's label: a song's path, or a bank of sound effects' path and the effect's slot. Each character other than
// a letter, a digit or an underscore becomes an underscore, and an underscore goes before a first digit. A label
// therefore starts with a letter or an underscore, and holds only letters, digits and underscores.
std::string SequenceLabel(const SequenceRef& sequence);

// The song or the sound effect of a set's files that `label` names: the first whose label it is. Nothing for a label
// that names neither.
std::optional<SequenceRef> SequenceNamed(std::string_view label, std::span<const NamedFile> files);

// Where the tracks of a song or of sound effect `effect` start in `file`, the song or the bank of sound effects that
// holds them. An offset in the sequence's bytes is an offset in the file less this. Nothing for a sound effect that
// the file's table doesn't hold.
std::optional<uint32_t> TracksOffset(std::span<const uint8_t> file, std::optional<uint16_t> effect);

} // namespace ntdlib::dse
