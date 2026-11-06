// SPDX-License-Identifier: MIT

// The songs and the sound effects of a game's files, each with the wave banks that it plays. A game that uses DSE
// loads a song with its wave bank, and keeps the main bank loaded: the bank whose samples the other banks' waves play.
// The files say which bank goes with which song only through their names and folders. A bank of sound effects lists
// the ids of the banks that each of its effects plays (SoundEffects::BankIds). A bank holds its id at 0x0E. Byte 0x4B
// of its header is 1 for a main bank and 2 for a bank whose waves lie in a main bank, whose id it gives at 0x40.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ntdlib::dse
{

// A file of a game, by its path in the game's file system, with '/' between folders.
struct NamedFile
{
    std::string path;
    std::span<const uint8_t> bytes;
};

// A song, and the files of its banks: indices into the list of files.
struct SongSet
{
    std::size_t song = 0;
    std::optional<std::size_t> bank = {};      // the song's wave bank
    std::optional<std::size_t> main_bank = {}; // the main bank that holds the bank's samples
};

// The songs of `files`: each file that starts with smdl. A song's bank is the wave bank (swdl) in the song's folder
// whose name is the song's without its extension, such as bgm0001.swd for bgm0001.smd. Without one, it is the folder's
// only wave bank that no song shares a name with and that isn't a main bank, such as me.swd for each of me0000.smd to
// me0013.smd. A bank whose waves lie in a main bank takes the main bank of its id: the one in its folder, or else the
// first in `files`. The sets come in the order of the songs' files.
std::vector<SongSet> FindSongSets(std::span<const NamedFile> files);

// The banks of a sound effect: indices into the list of files, and the ids of its list that no file holds.
struct EffectBanks
{
    std::vector<std::size_t> banks = {};       // in the order of the effect's list
    std::optional<std::size_t> main_bank = {}; // the main bank of the first bank whose waves lie in one
    std::vector<uint16_t> missing = {};
};

// A bank of sound effects (sedl), and the banks of each of its effects, by the effect's slot.
struct EffectSet
{
    std::size_t effects = 0;
    std::vector<EffectBanks> slots = {}; // empty for a file that can't be read
};

// The banks of sound effects of `files`: each file that starts with sedl, in their order. An effect plays the banks
// of the ids in its list. The bank of an id is the one of that id in the folder of the sound effects. Without one, it
// is a bank of that id that no song plays (FindSongSets), and of several, the one named after the id's low byte in
// three digits: Pokémon Mystery Dungeon: Explorers of Sky's code loads 048.swd for the id 0x6430. Else it is the first
// of them. A song's bank of that id comes last. The main bank of a bank whose waves lie in one is as for a song.
std::vector<EffectSet> FindEffectSets(std::span<const NamedFile> files);

// A song, or a sound effect of a bank of sound effects: the path of the song's file or of the bank's, and the effect's
// slot.
struct SequenceRef
{
    friend auto operator<=>(const SequenceRef&, const SequenceRef&) = default;

    std::string path = {};
    std::optional<uint16_t> effect = {};
};

// The files that a song or a sound effect plays: indices into the list of files.
struct SequenceSet
{
    std::size_t file = 0;                // the song, or the bank of sound effects
    std::vector<std::size_t> banks = {}; // the wave banks, in the order that the game loads them
    std::optional<std::size_t> main_bank = {};
};

// The files that FindSongSets and FindEffectSets find for `sequence`. Nothing when `files` hold no such song, or no
// such bank of sound effects with that slot.
std::optional<SequenceSet> FindSequenceSet(std::span<const NamedFile> files, const SequenceRef& sequence);

} // namespace ntdlib::dse
