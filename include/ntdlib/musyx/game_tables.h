// SPDX-License-Identifier: MIT

// The tables in a MusyX game's main.dol that list its songs and effects, for the games whose files don't list them.
//
// Camelot's Mario Golf: Toadstool Tour and Mario Power Tennis name each file on the disc by a number (CamelotPath).
// Their code lists the songs, the ranges of the effects that the game loads at start-up, the voice banks, the
// narrations and the courts' banks. Each group of sounds is four files, two numbers apart: the pool, the project, the
// sample directory and the samples.
//
// Intelligent Systems' WarioWare, Inc.: Mega Party Game$! lists its music in a sound room, the songs that play as
// effects in a second table, and the effects that its play function starts in a third.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/dol.h"

namespace ntdlib::musyx
{

// The word of a Camelot table that marks no files.
inline constexpr uint32_t kCamelotNoFiles = 0xffffffff;

// The disc path of the file with Camelot's file number `file_number`. The number contains the file's index above its
// low bit. The game's path function drops that bit and writes the index in base 26 with the letters A to Z, three
// letters with a slash between each, from "A/A/A".
std::string CamelotPath(uint32_t file_number);

// A list of songs in main.dol: `count` entries of `entry_size` bytes at `address`. An entry ends with three words: the
// song's file number, its ID, and its volume in the top byte. Before them, the entry contains the song's name with
// `name_inline`, or else the address of the name.
struct CamelotSongList
{
    uint32_t address = 0;
    uint32_t count = 0;
    uint32_t entry_size = 0;
    bool name_inline = false;
};

// A song of a Camelot game's list: its name as the list gives it, its file number (CamelotPath), its ID in the song
// group's MIDI setups and its volume in 127ths.
struct CamelotSong
{
    std::string name;
    uint32_t file_number = 0;
    uint16_t id = 0;
    uint8_t volume = 127;
};

// The songs of `list` in `dol`, the game's main.dol, in the list's order. Throws FormatError if `dol` has no data
// where an entry or a name should be.
std::vector<CamelotSong> ReadCamelotSongs(const Dol& dol, const CamelotSongList& list);

// A range of the IDs of the effects that a Camelot game loads at start-up, the first and the last.
struct CamelotRange
{
    uint32_t first = 0;
    uint32_t last = 0;
};

// The ranges of the table at `address` in `dol`: pairs of words, up to a word of 0xFFFFFFFF, at most 256 of them.
// Throws FormatError if `dol` has no data where a word should be.
std::vector<CamelotRange> ReadCamelotRanges(const Dol& dol, uint32_t address);

// A table of voice banks: `count` entries of `entry_size` bytes at `address`. An entry starts with the bank's name in
// `name_size` bytes, or with the name's address for a size of 0. Four words follow: the number of the bank's first
// file, the count of its effects, the first effect's ID and the group's MusyX ID.
struct VoiceBankTable
{
    uint32_t address = 0;
    uint32_t count = 0;
    uint32_t entry_size = 0;
    uint32_t name_size = 0;
};

// A voice bank of a Camelot game: its name, the number of the first file of its group, and its effects:
// `effect_count` IDs from `first_effect` on in the group whose MusyX ID is `group_id`.
struct VoiceBank
{
    std::string name;
    uint32_t first_file = 0;
    uint32_t effect_count = 0;
    uint32_t first_effect = 0;
    uint16_t group_id = 0;
};

// The voice banks of `table` in `dol`. Throws FormatError if `dol` has no data where an entry or a name should be.
std::vector<VoiceBank> ReadVoiceBanks(const Dol& dol, const VoiceBankTable& table);

// A narration of a Camelot game: the number of the first file of its group, or kCamelotNoFiles for a narration of the
// group that the game keeps loaded, and its effect: the effect `effect_id` of the group whose MusyX ID is `group_id`.
struct Narration
{
    uint32_t first_file = kCamelotNoFiles;
    uint16_t group_id = 0;
    uint16_t effect_id = 0;
};

// The narrations of the table at `address` in `dol`: `count` entries of 8 bytes. An entry contains the first file's
// number in a word, then the group's ID and the effect's ID in a half each. Throws FormatError if `dol` has no data
// where an entry should be.
std::vector<Narration> ReadNarrations(const Dol& dol, uint32_t address, uint32_t count);

// A court's bank of a Camelot game: the number of the first file of its group, or kCamelotNoFiles, and the MusyX ID of
// the group. The court's effects are those of its group.
struct CourtBank
{
    uint32_t first_file = kCamelotNoFiles;
    uint16_t group_id = 0;
};

// The courts' banks of the table at `address` in `dol`: `count` entries of 8 bytes. An entry contains the first file's
// number in a word, then the group's ID in the low half of a word. Throws FormatError if `dol` has no data where an
// entry should be.
std::vector<CourtBank> ReadCourtBanks(const Dol& dol, uint32_t address, uint32_t count);

// A song that a game's sound room lists: its name, its number, the ID of the song group that plays it, and its volume.
// The number is the song's ID in the group's MIDI setups and its place in the file of songs. `entry_word` is the first
// word of the sound room's entry. That word contains the group's ID in its top byte and the number in its low 16 bits.
// ReadEffectSongs leaves it at 0.
struct SoundRoomSong
{
    std::string name;
    uint16_t number = 0;
    uint16_t group_id = 0;
    uint8_t volume = 127; // in 127ths
    uint32_t entry_word = 0;
};

// The songs that the sound room in `dol` lists: `count` entries of 0x20 bytes at `address`. An entry's first word
// contains the song group's ID in its top byte and the song's number in its low 16 bits. Byte 4 is the song's volume.
// The word at 0xc points at the song's English name. The game's text marks each space with the bytes 0x94 0xBC. Throws
// FormatError if `dol` has no data where an entry or a name should be.
std::vector<SoundRoomSong> ReadSoundRoom(const Dol& dol, uint32_t address, uint32_t count);

// The songs that a game's table of the songs that play as effects lists: `count` entries of 8 bytes at `address` in
// `dol`. An entry contains the song group's ID in its first byte, the song's number in a big-endian half at +2 and the
// song's volume at +4. Each song takes "se-" and its number in four digits as its name, such as "se-0013". Throws
// FormatError if `dol` has no data where an entry should be.
std::vector<SoundRoomSong> ReadEffectSongs(const Dol& dol, uint32_t address, uint32_t count);

// An effect that a game's play function starts: the number that the function takes for it, and the effect's volume
// and its sends to the reverb (CC91) and the chorus (CC93).
struct PlayedEffect
{
    uint16_t number = 0;
    uint8_t volume = 127;
    uint8_t reverb = 0;
    uint8_t chorus = 0;
};

// The effects that a game's table of effects lists: `count` entries of 6 bytes at `address` in `dol`. An entry contains
// the play function's number in a big-endian half, the volume at +2, the send to the reverb at +4 and the send to the
// chorus at +5. Throws FormatError if `dol` has no data where an entry should be.
std::vector<PlayedEffect> ReadPlayedEffects(const Dol& dol, uint32_t address, uint32_t count);

} // namespace ntdlib::musyx
