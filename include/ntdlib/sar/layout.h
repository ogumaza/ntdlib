// SPDX-License-Identifier: MIT

// A sound archive of the 3DS, the Wii U or the Switch, cut into pieces for an editor: its sequence files, with their
// bytecode decoded into commands (ntdlib/commands/sequence.h), the zeros that align whatever follows each of them, and
// the runs of bytes between them. Banks, waves and the archive's tables stay in the runs. Each field of the tables that
// holds an offset or a size becomes a distance between two places in the pieces. Joining lays the pieces out again and
// writes the distances into the fields. The pieces of an archive join to give the archive back byte for byte. After an
// edit, each sequence file takes the size that its commands give, and the files after it move. WriteTableValues writes
// the values of the tables that have a place there, such as a sound's volume. StartFields finds where the tables hold
// each sound's start.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// A place in the archive: a byte of a piece, or the start of an item in a sequence piece's bytecode. In an archive cut
// short, a place can lie past the end: its byte counts from the end.
struct Place
{
    friend bool operator==(const Place&, const Place&) = default;

    uint32_t piece = 0;  // the piece, or the number of pieces for the archive's end
    uint32_t offset = 0; // the byte, from the piece's start, or the item
    bool item = false;   // `offset` is an item of the piece's sequence
};

// A field of the archive's tables that holds an offset or a size. The field is 32 bits in the archive's byte order, at
// `at` in a run. Join writes the distance from `from` to `to` into it.
struct Field
{
    friend bool operator==(const Field&, const Field&) = default;

    Place at;
    Place from;
    Place to;
};

// A sequence file of the archive.
struct SequencePiece
{
    friend bool operator==(const SequencePiece&, const SequencePiece&) = default;

    uint32_t sequence = 0; // the sequence, in Layout::sequences
    uint32_t version = 0;  // the file's version
};

// The zeros that align the next piece to a multiple of `alignment` bytes from the archive's start.
struct Padding
{
    friend bool operator==(const Padding&, const Padding&) = default;

    uint32_t alignment = 1;
};

// A run of the archive's bytes, a sequence file or padding.
using Piece = std::variant<std::vector<uint8_t>, SequencePiece, Padding>;

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Flavor flavor = Flavor::kNx;
    std::vector<commands::Sequence> sequences; // in the order of their first files
    std::vector<Piece> pieces;                 // in the order of the archive's bytes
    std::vector<Field> fields;                 // the fields whose distances an edit can change
};

// Cuts `archive`, read with `options`. A sequence file becomes a piece when its commands give its bytes back. The
// copies of a file in the archive's groups become pieces of the same sequence. A file stays in the bytes when it
// overlaps another, when a field of the tables points inside it, or when any copy of it can't become a piece. So does a
// file with bytes that no command reaches, but for zeros at its end: the player can run into them, and an edit couldn't
// move the offsets that they hold. The zeros that pad a sequence's bytecode stay out of its commands: the file's writer
// adds them back. Padding follows each sequence piece. Its alignment is the largest that every file of the archive
// keeps, up to 64 bytes. In an archive cut short, a group that lies partly past the end stays in the bytes whole. An
// archive whose fields can't all be placed becomes one run. Throws xsflib::FormatError if the archive's tables can't be
// read.
Layout Cut(std::span<const uint8_t> archive, const Options& options);

// The archive's bytes from its pieces, whose sequences are in `sequences`, with `fields` written. Throws
// std::invalid_argument for a piece that names a sequence the layout lacks, a sequence that the encoder can't write, a
// place past its piece, a field outside a run, or a distance below zero or past 32 bits.
std::vector<uint8_t> Join(Flavor flavor, std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
                          std::span<const Field> fields);

// Where each of `pieces` starts in `archive`, the archive of `flavor` that they join to: the position of each piece's
// first byte, then the end of the last piece. A sequence piece takes the size in its file's header. A sound's file
// starts at a sequence piece when the archive's FileOffset for it is that piece's start. Throws std::invalid_argument
// for a piece that runs past the archive's end.
std::vector<std::size_t> Starts(std::span<const uint8_t> archive, Flavor flavor, std::span<const Piece> pieces);

// Writes the values of `sounds` and `players` into the tables of `archive`, an archive of `flavor`. Each value goes
// where SoundArchive reads it with Rules::kStopAtFirstProblem. Sound i takes the values of `sounds[i]`: its player,
// volume, pan mode and curve, player priority and actor player, and for a sequence sound its banks, its tracks' flags,
// its channel priority and its release priority fix. Player i takes the sound limit of `players[i]`. A value that the
// tables don't hold stays as it is: an option that a sound leaves out, and a bank past the sound's list of banks. So do
// a 3DS sound's player, player priority and actor player. The reader doesn't read those. The names, the files, the
// kinds and the starts stay too. Bytes that the reader doesn't read keep theirs. A release priority fix that stays on
// keeps its byte. Throws xsflib::FormatError if the tables can't be read.
void WriteTableValues(std::span<uint8_t> archive, Flavor flavor, std::span<const SoundInfo> sounds,
                      std::span<const PlayerInfo> players);

// Where the tables of `archive`, an archive of `flavor`, hold each sound's start in its sequence file's bytecode: the
// position of the 32-bit field from the archive's start, for each sound of the sound table in order. A sound that isn't
// a sequence sound has none. Neither has a sequence sound whose entry leaves the start out: it starts where the
// bytecode starts. Cut makes each of these fields one of the layout's fields when the sound's file becomes a piece. An
// editor finds a sound's start among them. Throws xsflib::FormatError if the tables can't be read.
std::vector<std::optional<std::size_t>> StartFields(std::span<const uint8_t> archive, Flavor flavor);

} // namespace ntdlib::sar
