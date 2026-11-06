// SPDX-License-Identifier: MIT

// The performance of an MP2K song for an editor. ntdlib's model of the driver (ntdlib::mp2k::Sequencer) plays the song
// as ConvertSong does. The performance holds the notes of ConvertSong's MIDI file, each with the command that started
// it. It also holds each track's stretches of commands, and each track's changes of its volume and pan.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::mp2k
{

// A note's place on the driver's sound channels, for an editor.
struct NoteChannel
{
    bool operator==(const NoteChannel&) const = default;

    int8_t channel = -1; // the first of the driver's channels that plays it: ntdlib::mp2k::kFirstPsgChannel and on for
                         // the PSG's
    uint8_t psg = 0;     // the PSG channel that its voice plays on, 1 to 4, or 0 for a sample
    std::optional<uint32_t> cut_tick; // the tick where a later note took a channel that still held it
};

// A note that the driver never started, for an editor: it found no channel, its voice plays nothing, or a later note
// took its channel before the mixer started it.
struct UnstartedNote
{
    bool operator==(const UnstartedNote&) const = default;

    uint32_t tick = 0;
    uint32_t length = 0; // the note command's length in ticks, or 0 for a tied note
    uint8_t track = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint8_t psg = 0;     // the PSG channel that its voice plays on, 1 to 4, or 0 for a sample
    uint32_t offset = 0; // the note command's offset from `base`
};

// A recorded song: PerformanceBase, with the records for an editor.
//
// The ticks are the driver's: 24 to a quarter note. A tempo change comes at each tick where the driver's ticks take
// another time. Each of the song's tracks plays on the track of the same number. Offsets count from `base`, the
// address of the song's first byte. ntdlib::mp2k::DecodeSong decodes the bytes from there.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of commands, in the order that they ran, and the note command of each note, at the note's
    // ID less 1 (TrackEvent::note). A stretch begins where its track starts, and after a jump, a pattern's call or
    // return, or a repeat that goes back. A stretch's depth is the number of patterns on the track's stack.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;

    // Each note's place on the driver's channels, at the note's ID less 1, and the notes that the driver never started,
    // in the order of their ticks. The MIDI file holds none of those.
    std::vector<NoteChannel> note_channels;
    std::vector<UnstartedNote> unstarted;

    // Each track's changes of its volume and pan, in the order of their ticks. The volume runs from 0 to 127, and the
    // pan from -64 for the left to 63 for the right. Both are 0 before a command sets them. MP2K has no sends.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes with a MIDI channel for each track is a note of the performance. A song whose header can't be
// read, or whose music player is unused, gives a performance without tracks, with a warning.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::mp2k::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::mp2k
