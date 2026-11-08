// SPDX-License-Identifier: MIT

// The performance of a song of Nintendo R&D1's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::rd1::Sequencer) plays the song as ConvertSong does. The performance holds the notes of ConvertSong's MIDI
// file, each with the event that started it. It also holds each track's stretches of events, and each track's changes
// of its volume and pan.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/commands.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::rd1
{

// A note's ticks in the MIDI file.
struct MidiPlace
{
    bool operator==(const MidiPlace&) const = default;

    uint32_t on = 0;
    uint32_t off = 0;
};

// A recorded song: PerformanceBase, with the records for an editor.
//
// A tick is one of the ticks of the song's MIDI file, counted through every pass of the loop. `timebase` is the file's
// division. The driver plays the notes of track t on channel t & 15, and the performance's track t & 15 holds them.
// `note_tracks` gives the track of each note. A preset is 128 times the bank of the song's record plus the note's
// program. A note's velocity is the file's, from 1 to 127. Offsets count from `base`: the address of the song's MIDI
// file. ntdlib::rd1::DecodeRomSequence decodes the file. ntdlib::rd1::SequenceOfSong gives the sequence.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of events, in the order that they ran, and the event of each note, at the note's ID less 1
    // (TrackEvent::note). A stretch begins where its track starts, and where the loop takes the track back to the
    // loop's start (Entry::kLoop). Each note comes from a note-on event of its track, and `note_commands[i].stretch`
    // counts in that track's stretches.
    std::array<std::vector<Stretch>, ntdlib::rd1::kMaxTracks> stretches;
    std::vector<NoteCommand> note_commands;
    std::vector<uint8_t> note_tracks;

    // Each note's ticks in the MIDI file, at the note's ID less 1. The file's header gives their division.
    // `midi_end_tick` gives the file's end.
    std::vector<MidiPlace> midi_places;
    uint32_t midi_end_tick = 0;

    // Each track's changes of its volume and pan, in the order of their ticks. The volume is CC7's value, and 100
    // before an event sets it. The pan is CC10's value less 64: 0 is the centre, and 0 before an event sets it. A
    // track's controllers set channel t, and its notes play on channel t & 15. From track 16 on, a track's volume and
    // pan therefore reach none of its notes.
    std::array<std::vector<MixChange>, ntdlib::rd1::kMaxTracks> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A song without a record or a MIDI file that the driver reads gives a
// performance without tracks, with a warning. A song that plays no note gives a performance with no notes, and its
// stretches and its mix stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::rd1::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rd1
