// SPDX-License-Identifier: MIT

// A sound archive of the Wii (RSAR) cut into pieces for an editor, and joined again, by the rules of sar/layout.h. The
// archive's sequence files (RSEQ) become sequences of commands, and the bytes between them runs of bytes. Each field
// of the tables that holds an offset or a size becomes a distance between two places in the pieces: the archive's
// size, each block's offset and size, each group's offsets and sizes, those of each of a group's items, and each
// file's sizes in the file table. Joining lays the pieces out again and writes the distances into the fields. The
// pieces of an archive join to give the archive back byte for byte. After an edit, each sequence file takes the size
// that its commands give, and the files after it move. A sequence sound's start follows its command.
//
// An RSAR keeps its files in its groups. A group holds its files one after another, then their wave data. The groups
// follow one another in the FILE block. A file that several groups hold has a copy in each, and the copies of a
// sequence file become pieces of the same sequence.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rsar
{

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<commands::Sequence> sequences; // in the order of their first files
    std::vector<sar::Piece> pieces;            // in the order of the archive's bytes
    std::vector<sar::Field> fields;            // the fields whose distances an edit can change
};

// Cuts `archive`. The sequences' bytecode decodes by the rules of NW4R SND's build `build`. A sequence file becomes a
// piece by the rules of sar::Cut (sar/layout.h): when its commands give its bytes back, when no field points inside
// it, and when every copy of it can become a piece. Any other file becomes a run of its own when zeros pad it to the
// archive's alignment, and when no field lies in it or points inside it. An archive whose tables the cutter can't
// follow stays one run. Throws xsflib::FormatError if ntdlib's reader can't read the tables.
Layout Cut(std::span<const uint8_t> archive, const SndBuild& build);

// The archive's bytes from its pieces and its fields. The sequences encode by the rules of `build`, which has to be the
// build that Cut took. Throws the exceptions of sar::Join.
std::vector<uint8_t> Join(const SndBuild& build, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields);

// Where each of `pieces` starts in `archive`, which Join made from them, and then where the last one ends. Throws the
// exceptions of sar::Starts.
std::vector<std::size_t> Starts(std::span<const uint8_t> archive, std::span<const sar::Piece> pieces);

// Where the tables of `archive` hold each sound's start in its sequence file's bytecode: the position of the 32-bit
// field from the archive's start, for each sound of the sound table in order. A sound that isn't a sequence sound has
// none. Cut makes each of these fields one of the layout's fields when the sound's file becomes a piece. Throws
// xsflib::FormatError if the tables can't be read.
std::vector<std::optional<std::size_t>> StartFields(std::span<const uint8_t> archive);

} // namespace ntdlib::rsar
