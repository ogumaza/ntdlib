// SPDX-License-Identifier: MIT

// The tables of a sound archive of the 3DS (CSAR), the Wii U or the Switch (FSAR): its STRG and INFO blocks
// (ntdlib/sar/string_block.h and ntdlib/sar/info_block.h). TablesOf finds them in an archive. WithTables puts new ones
// in their place. WithNewFile, WithNewSequenceSound, WithNewWaveSounds, WithNewBank and WithNewWaveArchive add items
// through it. WithBankLists gives sequence sounds lists of banks of other lengths.
//
// NintendoWare's tools lay an archive out as its header, then its STRG, INFO and FILE blocks, in that order and
// without gaps. The header's table of blocks lists the three. An archive without names lists its STRG block at offset
// 0xFFFFFFFF. The FILE block holds the archive's files and ends the archive. Its magic and size come first. Zeros
// follow, up to the first file. Each file starts on a multiple of 64 bytes from the archive's start on the Switch, and
// of 32 bytes on the 3DS and the Wii U. The first file starts at the first such multiple after the block's size. The
// last file ends the archive. The INFO block's file table gives each file's offset from the end of the FILE block's
// size.
//
// An entry of the INFO block holds its options at a fixed place: 20 bytes in for a sound, 24 for a sound group, 12 for
// a bank, 8 for a wave archive, and 4 for a group and a player. The options start with a field of bits. A value follows
// for each bit that is set, in the order of the bits. Bit 0 gives the item's string index, the place of its name in
// the string table. A sound's entry holds a reference to its details 12 bytes in. A sequence sound's details start with
// a reference to its list of banks. The tracks that it allocates and its options follow. Bit 0 of the options gives
// its start in the file's bytecode, and bit 1 its channel priority. The list of banks ends the sound's entry. A wave
// sound's details start with its index in its file. The count of its tracks follows, then its options. The tools write
// a count of 1, and options with only bit 0, the channel priority, on the Wii U and the Switch.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"

namespace ntdlib::sar
{

// The tables of an archive: bytes of the archive.
struct Tables
{
    std::optional<std::span<const uint8_t>> strings; // the STRG block, if the archive has one
    std::span<const uint8_t> info;                   // the INFO block
};

// The tables of `archive`, an archive of `flavor`, from the header's table of blocks. Throws xsflib::FormatError for
// an archive without an INFO block, and for a block that runs past the archive's end.
Tables TablesOf(std::span<const uint8_t> archive, Flavor flavor);

// `archive`, an archive of `flavor`, with the blocks of `strings` and `info` in the place of its STRG and INFO blocks.
// The FILE block follows them, with the same files in the same order. The zeros before its first file change to keep
// each file's offset from the archive's start a multiple of 64 or 32 bytes. The offsets in the file table of `info`
// change with them. The header's table of blocks and the archive's size follow the new blocks. Throws
// xsflib::FormatError for an archive laid out another way, for an archive cut short, and for a file of `info` whose
// location lies outside its entry or before the first file. Throws std::invalid_argument for `strings` with an archive
// without a STRG block, for no `strings` with an archive with one, and for tables of another flavor.
std::vector<uint8_t> WithTables(std::span<const uint8_t> archive, Flavor flavor,
                                const std::optional<StringBlock>& strings, InfoBlock info);

// An archive with a new file, and the file's ID.
struct ArchiveWithFile
{
    std::vector<uint8_t> archive;
    uint32_t file = 0;
};

// `archive`, an archive of `flavor`, with `bytes` as a new file after its last one. The file table takes an entry for
// the file at its end, as the tools write one: a reference to the file's location, no options, then the location with
// the file's offset and size. On the Wii U and the Switch, a reference to the list of the groups that hold the file
// follows, and the list is empty. The file starts on the first multiple of 64 or 32 bytes from the old archive's end,
// and the archive ends with it. The INFO block grows, and the files move as WithTables moves them. Throws
// std::invalid_argument for an archive that would pass 4 GB, and the exceptions of WithTables.
ArchiveWithFile WithNewFile(std::span<const uint8_t> archive, Flavor flavor, std::span<const uint8_t> bytes);

// A sequence sound to add to an archive: a copy of sequence sound `model`, with a name, a file, a start and banks of
// its own.
struct NewSequenceSound
{
    uint32_t model = 0;          // the index of the sound that the new one copies
    std::string name;            // empty for an archive without a STRG block
    uint32_t file = 0;           // the ID of the sequence file that the sound plays
    uint32_t start = 0;          // where the sound starts in the file's bytecode
    std::vector<uint32_t> banks; // the bank slots: bank item IDs, or 0xFFFFFFFF for an empty slot
};

// `archive`, an archive of `flavor`, with `sound` at the end of its sound table. The new entry copies the model's: its
// player, volume, pan, priorities and 3D settings, the tracks that it allocates, and the options that ntdlib doesn't
// read. It takes the new sound's string index, file, start and banks. The tools keep the names in the string table in
// the order of the INFO block's tables. The new name goes in after the sounds' names, and each string index after it
// moves up by one. The STRG block's tree takes the name too. A model whose details leave the start out gives only a
// start of 0. Throws std::invalid_argument for a model past the sound table, a model that isn't a sequence sound, a
// file past the file table, more than four banks or a bank past the bank table, a name for an archive without a STRG
// block, no name for an archive with one, a name that the block holds already, and a start for a model without one.
// Throws xsflib::FormatError for entries that lay out their options or the model's details another way than the tools,
// and the exceptions of WithTables.
std::vector<uint8_t> WithNewSequenceSound(std::span<const uint8_t> archive, Flavor flavor,
                                          const NewSequenceSound& sound);

// A wave sound to add to an archive: a copy of sound `model` with another name, file and index.
struct NewWaveSound
{
    uint32_t model = 0; // the index of the sound that the new one copies: a wave sound or a sequence sound
    std::string name;   // empty for an archive without a STRG block
    uint32_t file = 0;  // the ID of the wave sound file that holds the wave sound
    uint32_t index = 0; // the wave sound's index in the file
};

// `archive`, an archive of `flavor`, with `sounds` at the end of its sound table, in order. Each new entry copies its
// model's: its player, volume, pan, priorities and 3D settings, and the options that ntdlib doesn't read. It takes the
// new sound's string index, file and index. A wave sound model's details stay as they are otherwise. A sequence sound
// model's details give way to a wave sound's. They hold what the tools write: the index, a count of 1 track, and
// options with the model's channel priority. The new names go in after the sounds' names, in order, and each string
// index after them moves up. The STRG block's tree takes the names too. Throws std::invalid_argument for a model past
// the sound table or of another kind, a file past the file table, a name for an archive without a STRG block, no name
// for an archive with one, and a name that the block holds already. Throws xsflib::FormatError for entries that lay out
// their options or the model's details another way than the tools, and the exceptions of WithTables.
std::vector<uint8_t> WithNewWaveSounds(std::span<const uint8_t> archive, Flavor flavor,
                                       std::span<const NewWaveSound> sounds);

// `archive`, an archive of `flavor`, whose sequence sounds hold the lists of banks in `banks`, by the sounds' places in
// the sound table: bank item IDs, or 0xFFFFFFFF for an empty slot. The tools write a list only up to the last bank that
// a sound plays. A list ends its sound's entry. A longer list therefore makes the entry longer, and a shorter one makes
// it shorter. The other sounds keep their lists. Throws std::invalid_argument for a sound past the sound table, a sound
// that isn't a sequence sound, more than four banks and a bank past the bank table. Throws xsflib::FormatError for an
// entry whose details are laid out another way than the tools lay them out, and the exceptions of WithTables.
std::vector<uint8_t> WithBankLists(std::span<const uint8_t> archive, Flavor flavor,
                                   const std::map<uint32_t, std::vector<uint32_t>>& banks);

// A bank to add to an archive: its name, its file and the wave archives that its entry lists.
struct NewBank
{
    std::string name;                    // empty for an archive without a STRG block
    uint32_t file = 0;                   // the ID of the bank file
    std::vector<uint32_t> wave_archives; // wave archive item IDs
};

// `archive`, an archive of `flavor`, with `bank` at the end of its bank table. The entry is laid out as the tools lay
// out a bank's: the file, a reference to the list of wave archives, the options with the string index, then the list.
// The tools leave the list empty in most banks, and in every bank of the Switch. Where they fill it, it holds the wave
// archives that the bank file's waves name, in the order of their first use. The new name goes in after the banks'
// names, and each string index after it moves up by one. The STRG block's tree takes the name too. Throws
// std::invalid_argument for a file past the file table, a wave archive past the wave archive table, a name for an
// archive without a STRG block, no name for an archive with one, and a name that the block holds already. Throws
// xsflib::FormatError for entries that lay out their options another way than the tools, and the exceptions of
// WithTables.
std::vector<uint8_t> WithNewBank(std::span<const uint8_t> archive, Flavor flavor, const NewBank& bank);

// A wave archive to add to an archive: its name and its file.
struct NewWaveArchive
{
    std::string name;  // empty for a wave archive without a name
    uint32_t file = 0; // the ID of the wave archive file
};

// `archive`, an archive of `flavor`, with `wave_archive` at the end of its wave archive table. The entry is laid out as
// the tools lay out a wave archive's that loads whole: the file, a word of zeros, then the options. Most of the tools'
// wave archives have no name, and their options are empty. A named wave archive's options give its string index. The
// new name then goes in after the wave archives' names, and each string index after it moves up by one. The STRG
// block's tree takes the name too. Throws std::invalid_argument for a file past the file table, a name for an archive
// without a STRG block, and a name that the block holds already. Throws xsflib::FormatError for entries that lay out
// their options another way than the tools, and the exceptions of WithTables.
std::vector<uint8_t> WithNewWaveArchive(std::span<const uint8_t> archive, Flavor flavor,
                                        const NewWaveArchive& wave_archive);

} // namespace ntdlib::sar
