// SPDX-License-Identifier: MIT

// The children of track 0 that Twilight Princess's sound layer, Z2, mutes as it starts a piece of music. RulesOf in
// sequence_export/jaudio/game.h reads the table.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace ntdlib::jaudio
{

// A piece of music as the game starts it: the piece's JAudio sound ID, the children of its track 0 that the game sets
// to a volume of 0 (bit n for child n), and the suffix of the start's name. A piece with two starts has two entries.
// The first start's name has no suffix.
struct MutedStart
{
    uint32_t sound = 0;
    uint16_t muted_children = 0;
    std::string_view suffix;
};

// Twilight Princess's starts. They hold on the GameCube, on the Wii and in HD. As the game starts a piece,
// Z2SeqMgr::bgmStart (0x802af010 in the GameCube's USA main.dol, 0x80293634 in the Wii's) sets some children of its
// track 0 to a volume of 0 through setChildTrackVolume (0x802b3fec, Wii 0x802977a4). For four pieces, it calls
// changeBgmStatus (0x802aff8c, Wii 0x80294254) with a state of 0. Hyrule Field's day music goes through
// fieldBgmStart (0x802b545c, Wii 0x80298678) and fieldRidingMute (0x802b556c, Wii 0x80298784). Its two sets of tracks
// are two arrangements of the same music. HD's code does the same: its bgmStart (0x02c24d44) calls changeBgmStatus
// (0x02c2286c) with 0 for the same four pieces, and its fieldRidingMute calls setChildTrackVolume (0x02c20b24) from
// 0x02c24aa0. The states that the scenes and actors set later aren't here.
inline constexpr std::array<MutedStart, 6> kTwilightPrincessStarts{{
    {0x01000000, 0x0fe0, ""},        // Hyrule Field's day music while Link walks: children 5 to 11
    {0x01000000, 0xf000, "_riding"}, // the same music while he rides: children 12 to 15
    {0x01000004, 0x3800, ""},        // the horseback battle: children 11 to 13
    {0x01000005, 0x0600, ""},        // Ordon Village: children 9 and 10
    {0x0100004d, 0x1c00, ""},        // the snow woman's second piece: children 10 to 12
    {0x01000087, 0x3c00, ""},        // Ganon's second fight: children 10 to 13
}};

} // namespace ntdlib::jaudio
