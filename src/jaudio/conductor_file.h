// SPDX-License-Identifier: MIT

// The reader of the conductor files (.cnd) of Pikmin 2. A file contains the settings of PSAutoBgm's conductor.
// JADUtility serializes the conductor's tree into it. Each object has bytes before its values, its values in their
// list's order, bytes after them, then its children. Each value is a byte or a 16-bit value, and a name is 8 bytes. The
// files of the GameCube release are big-endian. The files of the Switch release are little-endian, with the same
// layout. Addresses are in main.dol of the GameCube's USA release.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::jaudio::detail
{

// The bytes of each object: its values, and the bytes that the game skips around them (JADUtility::PrmSetBase::load,
// 0x800ba610). The game skips 17 bytes after the conductor's values (0x8033d5c4), 3 before and after each track's
// (0x80339718 and 0x80339760), and 21 after each module's (0x8033a378).
inline constexpr std::size_t kConductorValueBytes = 3;
inline constexpr std::size_t kConductorAfterBytes = 17;
inline constexpr std::size_t kConductorTrackAroundBytes = 3;
inline constexpr std::size_t kConductorTrackValueBytes = 14;
inline constexpr std::size_t kConductorModuleValueBytes = 19;
inline constexpr std::size_t kConductorModuleAfterBytes = 21;
inline constexpr std::size_t kConductorNameBytes = 8;

// A module's values, in the order in which PSAutoBgm::Module's constructor (0x80339e5c) lists them. The byte at +4 is
// a value that the game's code never reads.
struct ConductorModuleValues
{
    uint16_t start = 0; // the beats before the module starts
    int16_t beats = 0;  // the counted beats before the track moves on to its next module
    uint8_t in_order = 0;
    uint8_t port_high = 0; // the high and low bytes of the value that the module writes to the track's port 3
    uint8_t port_low = 0;
    std::array<uint16_t, 2> lists{};  // the module's two lists, by their numbers in the sequence's two tables of lists
    std::array<uint8_t, 2> repeats{}; // the repeats of the module's two cycles
    int16_t transpose = 0;
    uint8_t volume = 0;
    uint8_t pan = 0;
    uint8_t half_start = 0; // not 0 if the module starts half a beat later
};

// A track's values (PSAutoBgm::Track's constructor, 0x803394f8), and its modules.
struct ConductorTrackValues
{
    uint16_t start = 0;     // the beats before the track starts
    uint8_t half_start = 0; // not 0 if the track starts half a beat later
    std::string name;
    uint8_t follows = 0; // the track whose choices this one copies, or 16 for none
    uint8_t send = 0;    // the effect send, over 127
    std::vector<ConductorModuleValues> modules;
};

// The conductor's values (JADUtility::PrmSetRc::load, 0x8033cf38), and its tracks.
struct ConductorValues
{
    uint8_t beat_interval = 0; // the ticks between the root track's requests for a beat
    uint8_t tempo = 0;         // the root track's tempo
    std::vector<ConductorTrackValues> tracks;
};

// The values of the module at `at`.
inline ConductorModuleValues ConductorModuleAt(const xsflib::Reader& r, std::size_t at)
{
    ConductorModuleValues module;
    module.start = r.U16(at);
    module.beats = r.S16(at + 2);
    module.in_order = r.U8(at + 5);
    module.port_high = r.U8(at + 6);
    module.port_low = r.U8(at + 7);
    module.lists = {r.U16(at + 8), r.U16(at + 10)};
    module.repeats = {r.U8(at + 12), r.U8(at + 13)};
    module.transpose = r.S16(at + 14);
    module.volume = r.U8(at + 16);
    module.pan = r.U8(at + 17);
    module.half_start = r.U8(at + 18);

    return module;
}

// The values of the track at `at`, without its modules. The name ends at its first zero byte. With
// `check_whole_name`, all 8 bytes of the name have to lie inside the file before the name is read. Without it, the
// reads stop at the zero byte. The two give different offsets in the message for a file that ends inside a name.
inline ConductorTrackValues ConductorTrackAt(const xsflib::Reader& r, std::size_t at, bool check_whole_name)
{
    ConductorTrackValues track;
    track.start = r.U16(at + 1);
    track.half_start = r.U8(at + 3);

    if (check_whole_name)
    {
        r.Sub(at + 4, kConductorNameBytes);
    }
    for (std::size_t c = 0; c < kConductorNameBytes && r.U8(at + 4 + c) != 0; c++)
    {
        track.name += static_cast<char>(r.U8(at + 4 + c));
    }

    track.follows = r.U8(at + 12);
    track.send = r.U8(at + 13);

    return track;
}

// Reads a conductor file, big-endian or little-endian. Throws xsflib::FormatError if the file is too short for the
// tracks and modules that it lists. `check_whole_name` is as for ConductorTrackAt.
inline ConductorValues ReadConductorValues(std::span<const uint8_t> bytes, bool big_endian, bool check_whole_name)
{
    const xsflib::Reader r(bytes, big_endian);
    ConductorValues conductor;
    conductor.beat_interval = r.U8(0);
    conductor.tempo = r.U8(1);
    const uint8_t track_count = r.U8(2);
    std::size_t at = kConductorValueBytes + kConductorAfterBytes;

    for (uint8_t t = 0; t < track_count; t++)
    {
        at += kConductorTrackAroundBytes;
        const uint8_t module_count = r.U8(at);
        ConductorTrackValues track = ConductorTrackAt(r, at, check_whole_name);
        at += kConductorTrackValueBytes + kConductorTrackAroundBytes;

        for (uint8_t m = 0; m < module_count; m++)
        {
            track.modules.push_back(ConductorModuleAt(r, at));
            at += kConductorModuleValueBytes + kConductorModuleAfterBytes;
        }
        conductor.tracks.push_back(std::move(track));
    }

    return conductor;
}

} // namespace ntdlib::jaudio::detail
