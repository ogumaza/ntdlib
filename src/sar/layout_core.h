// SPDX-License-Identifier: MIT

// The cutting and joining of sar/layout.h for any kind of NintendoWare archive: the CSAR and FSAR archives of the 3DS,
// the Wii U and the Switch (sar/layout.h), and the RSAR archives of the Wii (rsar/layout.h). Each kind reads the
// structure of its archives by its rules: where its files lie, the fields of its tables that hold their offsets and
// sizes, and the sequence sounds' starts. A Container gives the rest that differs between the kinds: the magic of its
// sequence files, its byte order, the grammar of its bytecode, and the reader and the writer of its sequence files.
// These functions do the rest of the work by the rules that sar/layout.h states.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::sar::core
{

// A file of the archive, in its FILE block or in a group there: its file ID, where it starts and its size.
struct Location
{
    uint32_t file = 0;
    std::size_t start = 0;
    std::size_t size = 0;
};

// A field of the archive's tables, with positions from the archive's start: the field, and the distance it holds. A
// size's distance ends where a range that starts at `from` ends. Any other distance ends where something starts.
struct Distance
{
    std::size_t at = 0;
    std::size_t from = 0;
    std::size_t to = 0;
    bool size = false;
};

// A sequence sound's start in its file's bytecode, and where the archive's tables hold it. A sound without the option
// starts at 0.
struct Start
{
    uint32_t file = 0;
    uint32_t offset = 0;
    std::optional<std::size_t> at = std::nullopt;
};

// What the cutter reads of an archive's tables.
struct Structure
{
    std::vector<Location> files;
    std::vector<Distance> distances;
    std::vector<Start> starts;
};

// A file that a cut takes: the archive, or a group file beside it. Its bytes, and what the cutter reads of its tables.
struct Source
{
    std::span<const uint8_t> bytes;
    Structure structure;
};

// A sequence file in parts: its version, its bytecode with the zeros that pad it, and its labels in the file's order.
struct SequenceParts
{
    uint32_t version = 0;
    std::vector<uint8_t> data;
    std::vector<nwsnd::LabelOffset> labels;
};

// A sequence file as a writer lays it out: its bytes, and where its bytecode starts in them.
struct WrittenFile
{
    std::vector<uint8_t> bytes;
    std::size_t bytecode = 0;
};

// The rules of a kind of archive.
struct Container
{
    std::array<char, 4> sequence_magic{}; // the first 4 bytes of a sequence file
    bool big_endian = false;              // the byte order of the tables' fields
    nwsnd::Grammar grammar;               // the rules by which the console reads the sequences' bytecode
    std::size_t size_field = 0x0C;        // where a sequence file's header holds the file's size, in 32 bits

    // The largest alignment that padding keeps. A cut pads each file to the largest power of 2 up to this that every
    // file's start keeps.
    std::size_t most_alignment = 64;

    // Reads a sequence file. Throws xsflib::FormatError for a file that isn't one of the kind's sequence files.
    std::function<SequenceParts(std::span<const uint8_t>)> read;

    // Lays a sequence file out from its parts, as the kind's tools do.
    std::function<WrittenFile(const SequenceParts&)> write;
};

// The pieces of a cut: the sequences, the archive's pieces and fields, and the pieces and fields of each group file.
struct Cutting
{
    std::vector<commands::Sequence> sequences;
    std::vector<Piece> pieces;
    std::vector<Field> fields;
    std::vector<GroupFileLayout> group_files;
};

// Cuts `sources`, the archive and the group files beside it, by their structures and the rules of `container`. Throws
// xsflib::FormatError for a field that the pieces can't place.
Cutting CutBy(std::span<const Source> sources, const Container& container);

// The archive's bytes from its pieces by the rules of `container`. Throws the exceptions of Join (sar/layout.h).
std::vector<uint8_t> JoinBy(const Container& container, std::span<const commands::Sequence> sequences,
                            std::span<const Piece> pieces, std::span<const Field> fields);

// Where each of `pieces` starts in `archive`, by the rules of `container`. Throws the exceptions of Starts
// (sar/layout.h).
std::vector<std::size_t> StartsBy(const Container& container, std::span<const uint8_t> archive,
                                  std::span<const Piece> pieces);

} // namespace ntdlib::sar::core
