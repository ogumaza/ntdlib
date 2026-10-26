// SPDX-License-Identifier: MIT

// The values of the tables of a sound archive of the Wii (RSAR) that an editor changes in place. Each lies at a fixed
// place in its entry of the INFO block, where SoundArchive reads it (ntdlib/rsar/formats.h). A sound's entry holds its
// player at 8 bytes in, its volume at 0x14 and its player priority at 0x15. From version 1.1 on, its kind lies at 0x16.
// From version 1.2 on, its pan mode and pan curve lie at 0x28 and 0x29. A reference at 0x18 leads to its details. A
// sequence sound's details hold its start, its bank's index in the bank table and the tracks that it allocates, a word
// each. Its channel priority follows, and from version 1.3 on its release priority fix. A player's entry holds the
// number of sounds that it plays at once at 4 bytes in. A new value takes the place of the old one, and nothing moves.
//
// A file's entry holds the file's size first. The entry of a file beside the archive, such as a stream, holds a
// reference to its path at 0xC. The tools give such a file's entry the size of the file beside the archive. NW4R SND
// opens the file by its path alone, from its start to its end (SoundArchive::detail_OpenFileStream, main.dol
// 0x80041e60).

#pragma once

#include <cstdint>
#include <span>
#include <utility>

#include "ntdlib/rsar/formats.h"

namespace ntdlib::rsar
{

// Writes the values of `sounds` and `players` into the INFO block of `archive`. Sound i takes the values of
// `sounds[i]`: its player, volume and player priority, its pan mode and curve from version 1.2 on, and for a sequence
// sound its bank, its tracks' flags, its channel priority and, from version 1.3 on, its release priority fix. Player i
// takes the number of sounds of `players[i]`. The kind comes from the archive. The names, the files, the kinds, the
// starts, the 3D settings, the remote filter, the user's parameters, a player's heap and the values that the version
// doesn't hold stay as they are. So do the sounds and players past those of `sounds` and `players`. A release priority
// fix that stays on keeps its byte. Throws xsflib::FormatError if the tables can't be read, and std::invalid_argument
// for more sounds or players than the tables hold.
void WriteTableValues(std::span<uint8_t> archive, std::span<const SoundInfo> sounds,
                      std::span<const PlayerInfo> players);

// Writes the size of each file of `sizes`, a file's ID and its size, into the file's entry in `archive`. Each file has
// to lie beside the archive. Throws xsflib::FormatError if the file table can't be read, and std::invalid_argument for
// a file that the table lacks or that doesn't lie beside the archive.
void WriteExternalFileSizes(std::span<uint8_t> archive, std::span<const std::pair<uint32_t, uint32_t>> sizes);

} // namespace ntdlib::rsar
