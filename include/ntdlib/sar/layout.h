// SPDX-License-Identifier: MIT

// A sound archive of the 3DS, the Wii U or the Switch, cut into pieces for an editor: its sequence files, with their
// bytecode decoded into commands (ntdlib/commands/sequence.h), its other files, such as banks and wave archives, the
// zeros that align whatever follows each file, and the runs of bytes between them. The archive's tables stay in those
// runs. Each field of the tables that holds an offset or a size becomes a distance between two places in the pieces.
// Joining lays the pieces out again and writes the distances into the fields. The pieces of an archive join to give the
// archive back byte for byte. After an edit, each sequence file takes the size that its commands give, and the files
// after it move. ReplaceFile gives any other file new bytes, and the files after it move the same way. FileCopies finds
// the copies of a file. WriteTableValues writes the values of the tables that have a place there, such as a sound's
// volume. TableValuePlaces finds which of a sound's optional values have a place. StartFields finds where the tables
// hold each sound's start.
//
// A game can keep groups in files beside the archive. The cut takes them too, each into pieces of its own. A copy of
// one of the archive's sequence files there becomes a piece of the same sequence, and an edit of the sequence reaches
// it. ReplaceFile and FileCopies reach the copies of a file in the group files.

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

// A group file that a game keeps beside an archive, cut by the rules that cut the archive. The sequence pieces of a
// group file name sequences of the archive's layout. The fields lie in the group file's runs, and their places lie
// among the group file's pieces. A place at the end lies at the group file's end. Join gives the group file back from
// these pieces and fields.
struct GroupFileLayout
{
    friend bool operator==(const GroupFileLayout&, const GroupFileLayout&) = default;

    std::vector<Piece> pieces; // in the order of the group file's bytes
    std::vector<Field> fields; // the fields whose distances an edit can change
};

struct Layout
{
    friend bool operator==(const Layout&, const Layout&) = default;

    Flavor flavor = Flavor::kNx;
    std::vector<commands::Sequence> sequences; // in the order of their first files
    std::vector<Piece> pieces;                 // in the order of the archive's bytes
    std::vector<Field> fields;                 // the fields whose distances an edit can change
    std::vector<GroupFileLayout> group_files;  // the group files beside the archive, in the order that Cut took them
};

// Cuts `archive`, read with `options`. A sequence file becomes a piece when its commands give its bytes back. The
// copies of a file in the archive's groups become pieces of the same sequence. A sequence file stays in the bytes when
// it overlaps another, when a field of the tables points inside it, or when any copy of it can't become a piece. So
// does a file with bytes that no command reaches, but for zeros at its end: the player can run into them, and an edit
// couldn't move the offsets that they hold. The zeros that pad a sequence's bytecode stay out of its commands: the
// file's writer adds them back. Any other file becomes a run of its own when it lies whole in the archive, when zeros
// pad it to the alignment or it ends the archive, and when no field lies in it or points inside it. A group holds
// fields, and stays in the bytes. The files in it can still become runs. A sequence file that stays in the bytes can
// become a run too. Padding follows each sequence piece and each run of a file. Its alignment is the largest that
// every file of the archive keeps, up to 64 bytes. The padding after a file that ends the archive aligns to 1 byte. It
// adds no bytes. In an archive cut short, a group that lies partly past the end stays in the bytes whole. An archive
// whose fields can't all be placed becomes one run. Throws xsflib::FormatError if the archive's tables can't be read.
Layout Cut(std::span<const uint8_t> archive, const Options& options);

// Cuts `archive` and `group_files`, the group files that the game keeps beside it, read with `options`. Unless a copy
// in a group file keeps a sequence file in the bytes, the archive comes out as Cut(archive, options) gives it. Each
// group file comes out by the same rules as the archive. The FILE block of a group file holds the copies of the group's
// files, and the tables of the group file hold its fields. A copy of a sequence file joins the archive's sequence when
// the archive holds a copy of the file too. Unless every copy, in the archive and in the group files, holds the same
// bytes, none becomes a piece. A sequence file that only group files hold stays in their bytes: the archive's tables
// would hold the starts of the file's sounds in another file's bytecode. Such a file can still become a run of its
// own. A group file that isn't a group file of the archive's console becomes one run. So does each group file when the
// archive becomes one run. Throws xsflib::FormatError if the archive's tables can't be read.
Layout Cut(std::span<const uint8_t> archive, std::span<const std::vector<uint8_t>> group_files, const Options& options);

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

// The pieces in which file `file` holds `bytes`. `pieces`, with `sequences` and `fields`, join to an archive of
// `flavor`. Each copy of the file there, in the FILE block and in the groups, has to be a run of its own. Cut makes
// such runs. Each run takes `bytes`. Joining the new pieces lays the files after each copy out again. The tables
// follow. Throws std::invalid_argument for a file that the archive lacks, a copy of it that isn't a run of its own, a
// file whose bytes another file ID names too, and pieces that join to an archive whose tables don't give `bytes` for
// each copy and the bytes as before for every other file. Throws xsflib::FormatError if the tables can't be read.
std::vector<Piece> ReplaceFile(Flavor flavor, std::span<const commands::Sequence> sequences,
                               std::span<const Piece> pieces, std::span<const Field> fields, uint32_t file,
                               std::span<const uint8_t> bytes);

// The pieces of an archive and of the group files beside it.
struct ReplacedPieces
{
    friend bool operator==(const ReplacedPieces&, const ReplacedPieces&) = default;

    std::vector<Piece> pieces;                   // the archive's pieces
    std::vector<std::vector<Piece>> group_files; // the pieces of each group file, in order
};

// The pieces in which file `file` holds `bytes`, in the archive and in `group_files`, the group files beside it.
// `pieces`, with `sequences` and `fields`, join to the archive of `flavor`. Each copy of the file, in the archive and
// in the group files, has to be a run of its own. Each copy takes `bytes` by the rules of ReplaceFile. The
// archive need not hold a copy, and neither need a group file. A group file whose tables can't be read holds no copy.
// Throws std::invalid_argument for a file that none of them holds, and the exceptions of ReplaceFile for the archive
// and for each group file that holds a copy.
ReplacedPieces ReplaceFile(Flavor flavor, std::span<const commands::Sequence> sequences, std::span<const Piece> pieces,
                           std::span<const Field> fields, std::span<const GroupFileLayout> group_files, uint32_t file,
                           std::span<const uint8_t> bytes);

// A copy of a file in an archive: where it starts, the file ID of the group's file that holds it, if a group's file
// does, and its bytes. For a copy in a group file beside the archive, `group_file` gives the group file's place in
// the list of group files, and `start` the copy's start in the group file.
struct FileCopy
{
    friend bool operator==(const FileCopy&, const FileCopy&) = default;

    std::size_t start = 0;
    std::optional<uint32_t> group;
    std::vector<uint8_t> bytes;
    std::optional<std::size_t> group_file;
};

// The copies of file `file`, in the FILE block and in the groups, in the order of their starts. `pieces`, with
// `sequences` and `fields`, join to an archive of `flavor`. A group can hold a copy with other bytes than the copy that
// the file table names. Empty for a file that the archive doesn't hold. Throws xsflib::FormatError if the tables can't
// be read.
std::vector<FileCopy> FileCopies(Flavor flavor, std::span<const commands::Sequence> sequences,
                                 std::span<const Piece> pieces, std::span<const Field> fields, uint32_t file);

// The copies of file `file` in the archive, as FileCopies gives them, then those in `group_files`, the group files
// beside it, in the order of the group files and of the copies' starts. A group file whose tables can't be read holds
// no copy. Throws the exceptions of FileCopies.
std::vector<FileCopy> FileCopies(Flavor flavor, std::span<const commands::Sequence> sequences,
                                 std::span<const Piece> pieces, std::span<const Field> fields,
                                 std::span<const GroupFileLayout> group_files, uint32_t file);

// Writes the values of `sounds` and `players` into the tables of `archive`, an archive of `flavor`. Each value goes
// where SoundArchive reads it with Rules::kStopAtFirstProblem. Sound i takes the values of `sounds[i]`: its player,
// volume, pan mode and curve, player priority and actor player, and for a sequence sound its banks, its tracks' flags,
// its channel priority and its release priority fix. Player i takes the sound limit of `players[i]`. A value that the
// tables don't hold stays as it is: an option that a sound leaves out, and a bank past the sound's list of banks. So do
// a 3DS sound's player, player priority and actor player. The reader doesn't read those. The names, the files, the
// kinds and the starts stay too. Bytes that the reader doesn't read keep theirs. A release priority fix that stays on
// keeps its byte. WithValuePlaces and WithBankLists (ntdlib/sar/tables.h) give the entries places for the options and
// the banks first. Throws xsflib::FormatError if the tables can't be read.
void WriteTableValues(std::span<uint8_t> archive, Flavor flavor, std::span<const SoundInfo> sounds,
                      std::span<const PlayerInfo> players);

// The values of a sound that its entry holds only with an option: the pan mode and curve, the player priority and the
// actor player, and a sequence sound's channel priority and release priority fix. Every entry holds the sound's player
// and volume, and a sequence sound's tracks' flags. WriteTableValues writes an optional value only where the entry
// holds it. WithValuePlaces (ntdlib/sar/tables.h) gives an entry the places that it leaves out.
struct ValuePlaces
{
    friend bool operator==(const ValuePlaces&, const ValuePlaces&) = default;

    bool pan = false;      // the pan mode and curve
    bool priority = false; // the player priority and the actor player
    bool channel = false;  // a sequence sound's channel priority and release priority fix
};

// Which optional values the tables of `archive`, an archive of `flavor`, hold for each sound of the sound table, in
// order. The reader reads only the archive's header and its INFO block. `archive` can end after the block. On the 3DS,
// no sound has a place for a priority: the reader reads no 3DS sound's player priority or actor player. Throws
// xsflib::FormatError if the tables can't be read.
std::vector<ValuePlaces> TableValuePlaces(std::span<const uint8_t> archive, Flavor flavor);

// Where the tables of `archive`, an archive of `flavor`, hold each sound's start in its sequence file's bytecode: the
// position of the 32-bit field from the archive's start, for each sound of the sound table in order. A sound that isn't
// a sequence sound has none. Neither has a sequence sound whose entry leaves the start out: it starts where the
// bytecode starts. Cut makes each of these fields one of the layout's fields when the sound's file becomes a piece. An
// editor finds a sound's start among them. Throws xsflib::FormatError if the tables can't be read.
std::vector<std::optional<std::size_t>> StartFields(std::span<const uint8_t> archive, Flavor flavor);

} // namespace ntdlib::sar
