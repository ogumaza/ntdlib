// SPDX-License-Identifier: MIT

// The performance of a sequence of Nintendo R&D2's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::rd2::Sequencer) plays the sequence as ConvertSong does. The performance holds the notes of ConvertSong's
// MIDI file, each with the command that started it. It also holds each track's stretches of commands, and each track's
// changes of its volume, pan and echo.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::rd2
{

// A note's ticks in the MIDI file.
struct MidiPlace
{
    bool operator==(const MidiPlace&) const = default;

    uint32_t on = 0;
    uint32_t off = 0;
};

// A recorded sequence: PerformanceBase, with the records for an editor.
//
// A tick is one of the driver's ticks: a wait of n in a track's data is n ticks, and 24 ticks make a quarter note. A
// track that command F8 starts begins at a frame's time. That time can fall between ticks, and the track's ticks are
// the nearest ones.
// Each of the player's tracks plays on the track of the same number. A preset is 128 times the note's bank plus its
// instrument. A note's velocity is the driver's, up to 255. Offsets count from `base`: the address of the sequence, or
// for a sound effect, the address of its effects' sequence. ntdlib::rd2::DecodeRomSequence decodes the bytes from
// there. ntdlib::rd2::SequenceOfSong gives the sequence.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of reads, in the order that they ran, and the command of each note, at the note's ID less
    // 1 (TrackEvent::note). A stretch begins where its track starts, and after a jump, a call or its return. A
    // stretch's depth is the number of calls under way on its track. Each note comes from a note command of its track,
    // and `note_commands[i].stretch` counts in that track's stretches.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;

    // Each note's ticks in the MIDI file, at the note's ID less 1, with 3,600 ticks to a quarter note. `midi_end_tick`
    // gives the file's end.
    std::vector<MidiPlace> midi_places;
    uint32_t midi_end_tick = 0;

    // Each track's changes of its volume, pan and echo, in the order of their ticks. The volume is command E0's value,
    // and 0x80 before a command sets it. The pan is command C3's value less 64: 0 lies at the centre, and 0 before a
    // command sets it. A command's value can lie outside -64 to 63: the driver holds it to the voice's range. The send
    // to aux bus A is command E3's value: the track's share of the echo. It is 0 before a command sets it, and 0x7F for
    // a sound effect's track. A track that F8 starts takes the volume, pan and echo of the track that runs F8, at its
    // first command. A track that starts itself takes the values before a command sets them.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A song without a sequence or a track gives a performance without
// tracks, with a warning. A song that plays no note gives a performance with no notes, and its stretches and its mix
// stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::rd2::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rd2
