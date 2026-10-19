// SPDX-License-Identifier: MIT

// The INFO block of a sound archive of the 3DS (CSAR), the Wii U or the Switch (FSAR): the tables of the archive's
// items. The block's magic and size come first. Eight references follow. Their offsets count from the end of the
// block's size. The first seven lead to the tables of sounds, sound groups, banks, wave archives, groups, players and
// files, in that order. The last one leads to the archive player's settings. A table holds its count of entries and a
// reference to each entry. These offsets count from the table's start. Entry i of a table is the item whose ID has
// index i.
//
// The writer lays the block out as NintendoWare's tools do. The tables follow the references in their order. The
// entries of each table follow it in order. The next table follows the last entry. An entry's structures follow the
// entry, before the next one: a sound's details, a bank's list of wave archives or a file's location, for example.
// The offsets in these structures count from places inside the entry. An entry therefore moves as one piece. InfoBlock
// keeps each entry as its bytes up to the next entry or the next table. The archive player's settings follow the last
// table. They take 20 bytes. Zeros pad the block to 32 bytes.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

struct InfoBlock
{
    friend bool operator==(const InfoBlock&, const InfoBlock&) = default;

    // Reads `block`, an INFO block from its magic to its end, in the byte order of `flavor`. Throws
    // xsflib::FormatError for bytes that aren't an INFO block, and for a block laid out another way. Write() gives
    // the bytes of each block that Read takes.
    static InfoBlock Read(std::span<const uint8_t> block, Flavor flavor);

    // The block's bytes. Throws std::invalid_argument for an entry without bytes, or with a size that isn't a
    // multiple of 4.
    std::vector<uint8_t> Write() const;

    Flavor flavor = Flavor::kNx;

    // The entries of each table, in order.
    std::vector<std::vector<uint8_t>> sounds;
    std::vector<std::vector<uint8_t>> sound_groups;
    std::vector<std::vector<uint8_t>> banks;
    std::vector<std::vector<uint8_t>> wave_archives;
    std::vector<std::vector<uint8_t>> groups;
    std::vector<std::vector<uint8_t>> players;
    std::vector<std::vector<uint8_t>> files;

    std::array<uint8_t, 20> player_settings{}; // the archive player's settings
};

} // namespace ntdlib::sar
