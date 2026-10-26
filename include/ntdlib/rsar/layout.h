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
// sequence file become pieces of the same sequence. ReplaceFile gives any other file, or a file's wave data, new bytes
// in every copy, and the files after each copy move. FileCopies finds the copies.

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
// follow stays one run. Throws xsflib::FormatError if ntdlib's reader can't read the tables. A sequence file of
// `known` takes the commands that `known` gives it, by the rules of sar::Cut.
Layout Cut(std::span<const uint8_t> archive, const SndBuild& build, const sar::KnownSequences& known = {});

// The archive's bytes from its pieces and its fields. The sequences encode by the rules of `build`, which has to be the
// build that Cut took. Throws the exceptions of sar::Join.
std::vector<uint8_t> Join(const SndBuild& build, std::span<const commands::Sequence> sequences,
                          std::span<const sar::Piece> pieces, std::span<const sar::Field> fields);

// Where each of `pieces` starts in `archive`, which Join made from them, and then where the last one ends. Throws the
// exceptions of sar::Starts.
std::vector<std::size_t> Starts(std::span<const uint8_t> archive, std::span<const sar::Piece> pieces);

// The pieces in which file `file` holds `bytes`, and its wave data `wave_data` where the call gives it. `pieces`, with
// `sequences` and `fields`, join to the archive by the rules of `build`. Each copy of the file and of its wave data in
// the archive's groups has to be a run of its own. Cut makes such runs. Each run takes its new bytes, with zeros up to
// a multiple of 32 bytes: NW4R's tools pad each file and each wave data to that. Joining the new pieces lays the files
// after each copy out again, and the tables follow. Throws std::invalid_argument for a file that no group holds, wave
// data for a file without wave data, a copy that isn't a run of its own, bytes that another file's copy names too, and
// pieces that join to an archive whose tables don't give the new bytes in each copy and the bytes as before everywhere
// else. Throws xsflib::FormatError if the tables can't be read.
std::vector<sar::Piece> ReplaceFile(const SndBuild& build, std::span<const commands::Sequence> sequences,
                                    std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                    uint32_t file, std::span<const uint8_t> bytes,
                                    std::optional<std::span<const uint8_t>> wave_data = std::nullopt);

// The copies of file `file` in the archive's groups, or of its wave data with `wave_data`, in the order of their
// starts. `pieces`, with `sequences` and `fields`, join to the archive by the rules of `build`. A copy names no group's
// file: an RSAR's groups have none. Empty for a file that no group holds, and for a file without wave data. Throws
// xsflib::FormatError if the tables can't be read.
std::vector<sar::FileCopy> FileCopies(const SndBuild& build, std::span<const commands::Sequence> sequences,
                                      std::span<const sar::Piece> pieces, std::span<const sar::Field> fields,
                                      uint32_t file, bool wave_data = false);

// Where the tables of `archive` hold each sound's start in its sequence file's bytecode: the position of the 32-bit
// field from the archive's start, for each sound of the sound table in order. A sound that isn't a sequence sound has
// none. Cut makes each of these fields one of the layout's fields when the sound's file becomes a piece. Throws
// xsflib::FormatError if the tables can't be read.
std::vector<std::optional<std::size_t>> StartFields(std::span<const uint8_t> archive);

} // namespace ntdlib::rsar
