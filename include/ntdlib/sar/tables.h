// SPDX-License-Identifier: MIT

// The tables of a sound archive of the 3DS (CSAR), the Wii U or the Switch (FSAR): its STRG and INFO blocks
// (ntdlib/sar/string_block.h and ntdlib/sar/info_block.h). TablesOf finds them in an archive. WithTables puts new ones
// in their place. WithNewFile, WithNewExternalFile, WithNewSequenceSound, WithNewWaveSounds, WithNewStreamSound,
// WithNewBank, WithNewWaveArchive and WithNewGroup add items through it. WithBankLists gives sequence sounds lists of
// banks of other lengths. WithFileBytes gives a file of the FILE block other bytes. WithValuePlaces gives sounds'
// entries the places of optional values that they leave out.
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
//
// A stream sound's details start with the tracks that play, a bit each, and the count of the stream's channels. A
// reference to the table of tracks follows. From version 2.1.0 on, the pitch and a reference to the sound's sends
// follow it. Some of the tools' archives of 2.1.0 hold a reference to nothing after them, and every archive from 2.2.0
// on holds that reference and a word of ones. In 2.0.0, a word of zeros follows the reference to the table. The table
// follows, then for each track its info, the indices of its channels and, from 2.1.0 on, its sends. The sound's sends
// come last. A track's info holds its volume, pan, span and flags, and a reference to the indices. From 2.1.0 on, a
// reference to its sends, its low-pass filter and its biquad filter's type and value follow. Sends hold the main send
// and a send to each aux bus, in 8 bytes.
//
// A sound's entry holds the values of its options up to bit 8 in the order of the bits: the string index, the pan, the
// priorities, a value that a few Switch sounds hold in bit 3, and the offset of the 3D settings. The values of bit 17
// and of the user's bits 28 to 31 follow. The 3D settings follow the options, and the details follow the 3D settings.
// The offset of the 3D settings and the reference to the details count from the entry's start. The references in the
// details count from the details' start. The tools give every sound's entry the options of the string index, the pan,
// the priorities and the 3D settings. They give every sequence sound's details the options of the start and the
// channel priority.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
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

// `archive`, an archive of `flavor`, with a new file outside it at `path`: a path that the game reads from the
// archive's folder, such as stream/BGM_TITLE.bfstm. The file table takes an entry for the file at its end, as the tools
// write one: a reference to the path, no options, then the path with a zero byte after it, and zeros up to a multiple
// of 4 bytes. The INFO block grows, and the files move as WithTables moves them. Throws std::invalid_argument for an
// empty path and a path with a zero byte, and the exceptions of WithTables.
ArchiveWithFile WithNewExternalFile(std::span<const uint8_t> archive, Flavor flavor, const std::string& path);

// `archive`, an archive of `flavor`, with `bytes` as a new file after its last one. The file table takes an entry for
// the file at its end, as the tools write one: a reference to the file's location, no options, then the location with
// the file's offset and size. On the Wii U and the Switch, a reference to the list of the groups that hold the file
// follows, and the list is empty. The file starts on the first multiple of 64 or 32 bytes from the old archive's end,
// and the archive ends with it. The INFO block grows, and the files move as WithTables moves them. Throws
// std::invalid_argument for an archive that would pass 4 GB, and the exceptions of WithTables.
ArchiveWithFile WithNewFile(std::span<const uint8_t> archive, Flavor flavor, std::span<const uint8_t> bytes);

// A file whose bytes lie inside another file's bytes, such as a copy in a group's file that the file table names: its
// offset from the other file's start before and after the other file takes new bytes.
struct InnerFile
{
    uint32_t from = 0;
    uint32_t to = 0;
};

// `archive`, an archive of `flavor`, in which file `file` of its FILE block holds `bytes`. Unless the file ends the
// archive, zeros pad it to a multiple of 64 or 32 bytes. The files after it move by whole multiples, and the file
// table's offsets and size follow. Another file ID that names the same bytes takes them too. A file that fields of the
// tables lie in, such as a group's file, can take bytes this way. A file whose bytes lie inside the old bytes at an
// offset that `inner` gives moves to the new offset, and keeps its size. Throws std::invalid_argument for a file past
// the file table, for one that the FILE block doesn't hold, for an inner file that would run past the new bytes, and
// for an archive that would pass 4 GB. Throws xsflib::FormatError for a file whose padding holds bytes other than
// zeros, for a file that another file overlaps otherwise, and the exceptions of WithTables.
std::vector<uint8_t> WithFileBytes(std::span<const uint8_t> archive, Flavor flavor, uint32_t file,
                                   std::span<const uint8_t> bytes, std::span<const InnerFile> inner = {});

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
// options with the model's channel priority. In a 3DS archive of CSAR 2.0.0.0, the options also give the offset of the
// sends that the tools write after them. The new names go in after the sounds' names, in order, and each string index
// after them moves up. The STRG block's tree takes the names too. Throws std::invalid_argument for a model past the
// sound table or of another kind, a file past the file table, a name for an archive without a STRG block, no name for
// an archive with one, and a name that the block holds already. Throws xsflib::FormatError for entries that lay out
// their options or the model's details another way than the tools, and the exceptions of WithTables.
std::vector<uint8_t> WithNewWaveSounds(std::span<const uint8_t> archive, Flavor flavor,
                                       std::span<const NewWaveSound> sounds);

// A stream sound to add to an archive: a copy of sound `model` with another name, file and details.
struct NewStreamSound
{
    uint32_t model = 0;     // the index of the sound that the new one copies: a sound of any kind
    std::string name;       // empty for an archive without a STRG block
    uint32_t file = 0;      // the ID of the stream file
    StreamSoundInfo stream; // the tracks that play, the channels, the pitch, the sends, and each track
};

// `archive`, an archive of `flavor`, with `sound` at the end of its sound table. The new entry copies the model's: its
// player, volume, pan, priorities and 3D settings, and the options that ntdlib doesn't read. It takes the new sound's
// string index and file, and stream details for `sound.stream`, laid out as the tools lay them out in an archive of the
// archive's version. The details follow the 3D settings. A sequence sound or a wave sound holds a 32-bit value between
// the two, and the copy leaves the model's out. In a 3DS archive of CSAR 2.0.0.0, a stream sound holds the value too,
// and the copy keeps the model's. A stream sound model keeps the fields of its details before its table of tracks. The
// reference after the sends then leads to no extension, and the sound names no prefetch file: both describe the model's
// stream. A model of another kind takes the fields of the version: from 2.2.0 on, the reference to nothing and the word
// of ones. Before 2.1.0, the details hold no pitch and no sends. In a 3DS archive before CSAR 2.3.0.0, the details hold
// the tracks that play and the count of channels alone. The stream file holds the tracks there. CSAR 2.3.0.0 lays the
// details out as FSAR 2.0.0 does, after a field of options without values. From CSAR 2.3.2.0 on, the details take the
// layout of FSAR 2.2.0, with sends of 4 bytes. The new name goes in after the sounds' names, and each string index
// after it moves up by one. The STRG block's tree takes the name too. Throws std::invalid_argument for a 3DS archive
// before CSAR 2.3.2.0 of a version other than 2.0.0.0, 2.2.0.0 and 2.3.0.0, a model past the sound table, a file past
// the file table, more than 8 tracks, a track that plays no channel, more than 16 or a channel past the count of
// channels, a bit of the tracks that play past the tracks, any track in a 3DS archive before CSAR 2.3.0.0 or a bit past
// 8 tracks there, a name for an archive without a STRG block, no name for an archive with one, and a name that the
// block holds already. ntdlib's reader reads 8 tracks of a stream sound, and 16 channels of a track. Throws
// xsflib::FormatError for entries that lay out their options, their 3D settings or the model's details another way than
// the tools, and the exceptions of WithTables.
std::vector<uint8_t> WithNewStreamSound(std::span<const uint8_t> archive, Flavor flavor, const NewStreamSound& sound);

// `archive`, an archive of `flavor`, whose sequence sounds hold the lists of banks in `banks`, by the sounds' places in
// the sound table: bank item IDs, or 0xFFFFFFFF for an empty slot. The tools write a list only up to the last bank that
// a sound plays. A list ends its sound's entry. A longer list therefore makes the entry longer, and a shorter one makes
// it shorter. The other sounds keep their lists. Throws std::invalid_argument for a sound past the sound table, a sound
// that isn't a sequence sound, more than four banks and a bank past the bank table. Throws xsflib::FormatError for an
// entry whose details are laid out another way than the tools lay them out, and the exceptions of WithTables.
std::vector<uint8_t> WithBankLists(std::span<const uint8_t> archive, Flavor flavor,
                                   const std::map<uint32_t, std::vector<uint32_t>>& banks);

// `archive`, an archive of `flavor`, whose sounds' entries hold the places of the optional values in `places`, by the
// sounds' places in the sound table (TableValuePlaces). An entry that leaves out such a value's option takes it as the
// tools lay out an entry that holds it. The option's bit goes on, and its value goes in among the options' values in
// the order of the bits. The 3D settings and the details then move 4 bytes on, and the offset of the 3D settings and
// the reference to the details follow them. A sequence sound's channel priority goes in among the options of its
// details the same way, and its list of banks moves. A new value holds what the reader reads without the option: a pan
// mode and a pan curve of 0, a player priority of 64 and an actor player of 0, and a channel priority of 64 without the
// release priority fix. The archive therefore reads as before, and WriteTableValues can then write the new values. An
// entry that holds an option keeps it, and an option that `places` leaves out stays as it is. Without a new option,
// the archive comes back as it is. Throws std::invalid_argument for a sound past the sound table and a channel priority
// of a sound that isn't a sequence sound. Throws xsflib::FormatError for an entry that the tools don't lay out so, such
// as one with an unknown option, and the exceptions of WithTables.
std::vector<uint8_t> WithValuePlaces(std::span<const uint8_t> archive, Flavor flavor,
                                     const std::map<uint32_t, ValuePlaces>& places);

// A bank to add to an archive: its name, its file and the wave archives that its entry lists.
struct NewBank
{
    std::string name;                         // empty for an archive without a STRG block
    uint32_t file = 0;                        // the ID of the bank file
    std::vector<uint32_t> wave_archives = {}; // wave archive item IDs
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

// `archive`, an archive of `flavor`, with a new group named `name` at the end of its group table. The group's file
// holds no files and lists no items (EmptyGroupFile in ntdlib/sar/groups.h). On the 3DS, it takes the highest version
// of the groups' files that the archive holds, or 1.0.0.0 without one. It goes after the archive's last file, as
// WithNewFile puts a file there. The FILE block holds it up to the end of its INFX block's entries, or whole in a
// version without INFX, and the file table gives it that size (GroupSizeInArchive). Its entry in the file table is
// laid out as the tools lay out a group's file's: the reference to the location has type 0. The group's entry holds
// the file, then the options with the string index after the groups' names. Each string index after it moves up by
// one, and the STRG block's tree takes the name too. In an archive without a STRG block, the group takes the string
// index all the same, as a new bank does. Throws std::invalid_argument for a name for an archive without a STRG block,
// no name for an archive with one, a name that the block holds already, and an archive that would pass 4 GB. Throws
// xsflib::FormatError for entries that lay out their options another way than the tools, and the exceptions of
// WithTables.
std::vector<uint8_t> WithNewGroup(std::span<const uint8_t> archive, Flavor flavor, const std::string& name);

} // namespace ntdlib::sar
