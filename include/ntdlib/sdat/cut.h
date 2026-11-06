// SPDX-License-Identifier: MIT

// A DS sound archive (SDAT) cut into pieces for an editor, and joined again, in the shapes of sar/layout.h. Each
// sequence file (SSEQ) whose bytecode the decoder reads (ntr/snd/commands.h) gives a sequence piece: the bytecode from
// its start. The file's 0x1C bytes of headers stay in the run before the piece. Any other file becomes a run of its
// own, a sequence archive (SSAR) included. The archive's other bytes become runs. The pieces of an archive join to give
// the archive back byte for byte.
//
// The fields hold offsets and sizes: the archive's size in its header, the FILE block's size in the header and in the
// block, each file's offset and size in the FAT, and each sequence file's size and DATA block's size in its header. An
// offset counts from the archive's start. A sequence that grows or shrinks moves the files after it, and the fields
// follow. ReplaceFile gives any other file new bytes, and the files after it move the same way.
//
// The SDK's tools end a sequence file on a multiple of 4 bytes with zeros, and put each file on a multiple of 32 bytes
// from the archive's start with zeros before it. The cut keeps those zeros as padding, where the archive holds them.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"

namespace ntdlib::sdat
{

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    std::vector<commands::Sequence> sequences; // in the order of their pieces
    std::vector<sar::Piece> pieces;            // in the order of the archive's bytes
    std::vector<sar::Field> fields;
};

// Cuts `sdat`, an archive from its start to the size in its header. A file with bytes whose FAT entry lies clear of
// every other file's, after the header's blocks, becomes a piece of its own. A sequence file becomes a sequence piece
// when it has the header that the tools write, and when the decoder reads its bytecode without keeping it as data and
// the encoding gives the bytes back. The zeros up to 3 bytes at the bytecode's end are the file's padding when the
// decoder ends there on a command. Any other such file becomes a run. Zeros up to the next file's multiple of 32 bytes
// become padding. A file that ends on that multiple takes the padding too, without bytes. A field holds a distance:
// from its first place to its second. Each field holds the archive's value.
// An archive that the pieces and fields don't give back byte for byte, and one with no such file, becomes one run. A
// place names a byte of a run, or the start of a piece or of the archive's end. Throws xsflib::FormatError for bytes
// that aren't an archive.
Layout Cut(std::span<const uint8_t> sdat);

// The archive's bytes from its pieces and its fields. Each sequence encodes at its piece. Padding brings the next piece
// to a multiple of its alignment from the archive's start with zeros. Each field takes the distance between its places,
// as a 32-bit little-endian number. Throws std::invalid_argument for a sequence piece without a sequence, a field that
// doesn't lie in a run, a place that the pieces lack or that lies inside a sequence, a distance that doesn't fit in 32
// bits, and an archive past 4 GiB. Throws the exceptions of ntr::snd::EncodeSequence.
std::vector<uint8_t> Join(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                          std::span<const sar::Field> fields);

// The sequence of `file`, a sequence file (SSEQ), by the cut's rules: the file has the header that the tools write, and
// the decoder reads its bytecode without keeping it as data, and the encoding gives the bytes back. Nothing for any
// other bytes.
std::optional<commands::Sequence> ReadSequenceFile(std::span<const uint8_t> file);

// The pieces in which file `file` holds `bytes`. `pieces`, with `sequences` and `fields`, join to an archive. The file
// has to be a run of its own: Cut makes such runs. Joining the new pieces lays the files after it out again, and the
// tables follow. Throws std::invalid_argument for a file that the archive lacks, a file that isn't a run of its own,
// and pieces that join to an archive whose tables don't give `bytes` for the file and the bytes as before for every
// other file. Throws the exceptions of Join, and xsflib::FormatError if the tables can't be read.
std::vector<sar::Piece> ReplaceFile(std::span<const commands::Sequence> sequences, std::span<const sar::Piece> pieces,
                                    std::span<const sar::Field> fields, uint32_t file, std::span<const uint8_t> bytes);

} // namespace ntdlib::sdat
