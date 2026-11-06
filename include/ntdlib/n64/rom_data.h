// SPDX-License-Identifier: MIT

// Finding sound data in a Nintendo 64 image, and the compressed formats that hold it.
//
// MIO0 and Yay0 keep three streams: a stream of bits that say which comes next, a stream of literal bytes and a stream
// of references back into the output. Yaz0 keeps them mixed. A header gives the decoded size.
//
// Nintendo EAD's games with a file table (Ocarina of Time, Majora's Mask) list their files in a table of 16-byte
// entries: the start and the end in the game's virtual addresses, and the start and the end in the image. An end of 0
// in the image means a file stored as it is. The others are Yaz0.
//
// Rare's blocks start with the bytes 0x11 0x72, and a raw DEFLATE stream (RFC 1951) follows. GoldenEye 007 keeps its
// sequences in a table of such blocks. The table starts with a 16-bit count and two bytes of padding. Each entry holds
// its block's offset from the start of the table as 32 bits, the decoded size as 16 bits and the block's size as 16
// bits. The blocks follow the table, each after the one before.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib::n64
{

// A compressed block's decoded bytes. Bad data throws xsflib::FormatError. Each reads the block at the start of `data`.
std::vector<uint8_t> DecodeMio0(std::span<const uint8_t> data);
std::vector<uint8_t> DecodeYay0(std::span<const uint8_t> data);
std::vector<uint8_t> DecodeYaz0(std::span<const uint8_t> data);
std::vector<uint8_t> DecodeRare(std::span<const uint8_t> data);

// A table of Rare's sequences. Reading the table decodes nothing.
struct RareSequenceTable
{
    struct Entry
    {
        uint32_t offset = 0;
        uint16_t size = 0;
        uint16_t block_size = 0;
    };

    // Reads a table at the start of `data`. `data` must also hold the blocks. `data` must outlive the table. A damaged
    // table throws xsflib::FormatError.
    static RareSequenceTable Parse(std::span<const uint8_t> data);

    // A sequence's decoded bytes. A block that doesn't decode to its entry's size throws xsflib::FormatError.
    std::vector<uint8_t> Sequence(std::size_t index) const;

    std::vector<Entry> entries;
    std::span<const uint8_t> data;
};

// Finds a table of Rare's sequences by its entries: each block has to start where the one before ends, with the bytes
// of Rare's blocks. Returns the table's offset in the image.
std::optional<std::size_t> FindRareSequences(std::span<const uint8_t> image);

// The offsets of an image's sound files.
struct FoundFiles
{
    std::vector<std::size_t> sequence_banks;
    std::vector<std::size_t> bank_files;
    std::vector<std::size_t> standard_midi;
};

// Finds libultra's files by their headers. A match has to read as a whole file. Bytes that only look like a header
// don't count. A file's offset is even, but a standard MIDI file's can be anything.
FoundFiles FindFiles(std::span<const uint8_t> image);

// Finds the place of a bank file's samples (the .tbl file): the offset in the image of the start that makes the first
// loops' states come out of the decoder. A wave's loop holds the decoder's state at the loop's start. Returns nothing
// for a bank without such a loop, or one whose samples aren't in the image as they are.
struct BankFile;
std::optional<std::size_t> FindBankSamples(std::span<const uint8_t> image, const BankFile& bank);

// The offsets of the early EAD engine's files in an image.
struct EarlySoundData
{
    std::size_t sequences = 0;
    std::size_t banks = 0;
    std::size_t samples = 0;
    std::optional<std::size_t> bank_sets;
};

// Finds the three files of the early engine, by their revisions and the agreement of their counts, and the table of
// each sequence's banks after the sequences or the samples.
std::optional<EarlySoundData> FindEarlySoundData(std::span<const uint8_t> image);

// A later engine's tables: in the code of a game with a file table (Ocarina of Time, Majora's Mask), or in the image of
// a game without one (Pokémon Stadium 2).
struct LaterSoundData
{
    // The game's code, decoded, or a copy of the image's tables. The tables below are offsets in it.
    std::vector<uint8_t> code;
    std::size_t bank_table = 0;
    std::size_t sequence_table = 0;
    std::size_t sample_table = 0;

    // The offset in `code` of the table of each sequence's banks. The table holds an offset for each sequence, each to
    // a count and the banks. The field is 0 when the data holds no such table.
    std::size_t bank_set_table = 0;

    // The places in the image of the three files that the tables count from.
    std::size_t banks = 0;
    std::size_t sequences = 0;
    std::size_t samples = 0;
};

// Finds the tables through the image's file table, or else in the image. Returns nothing for an image without them.
std::optional<LaterSoundData> FindLaterSoundData(std::span<const uint8_t> image);

} // namespace ntdlib::n64
