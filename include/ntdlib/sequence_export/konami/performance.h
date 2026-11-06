// SPDX-License-Identifier: MIT

// The performance of a song of Konami's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::konami::Sequencer) plays the song as ConvertSong does. The performance holds the notes of ConvertSong's MIDI
// file, each with the command that started it. It also holds each track's stretches of commands, and each track's
// changes of its volume and pan.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::konami
{

// How the commands of a track led to a note of the MIDI file. The driver and its output stage can start a note without
// a note command.
enum class NoteCause : uint8_t
{
    kNote,    // a note command in the note's frame
    kOther,   // another command in the note's frame, such as a volume that restarts a PSG channel
    kEarlier, // no command in the note's frame: the sound of this earlier note command starts again, such as in a step
              // of a PSG note's envelope or a new wave load during a bend
    kNone,    // no command: the note's offset and stretch mean nothing
};

// The track and the cause of a note's command. A note can play on the track after the command's: the older revisions'
// PSG tracks that pan off the centre play one side on the next channel, and Dungeon Dice Monsters' Ax commands play on
// the next track.
struct NoteStart
{
    bool operator==(const NoteStart&) const = default;

    uint8_t track = 0;
    NoteCause cause = NoteCause::kNote;
};

// A note's ticks in the MIDI file, on the song's beat.
struct MidiPlace
{
    bool operator==(const MidiPlace&) const = default;

    uint32_t on = 0;
    uint32_t off = 0;
};

// A recorded song: PerformanceBase, with the records for an editor.
//
// A tick is one of the driver's frames (ntdlib::agb::kFrameRate of them to a second), and a delay of n in a track's
// data is n ticks. The driver's tempo never changes. `timebase` is the number of whole frames nearest to a quarter note
// at the song's first tempo, and the one entry of `tempo` gives that many frames their time. Each of the song's tracks
// plays on the track of the same number, and a preset is 128 times the note's bank plus its program. Offsets count from
// `base`, the address of the song's first byte. ntdlib::konami::DecodeSong decodes the bytes from there.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of reads, in the order that they ran, and the command of each note, at the note's ID less
    // 1 (TrackEvent::note). A stretch begins where its track starts, and after a jump, a call or its return, or a loop
    // of the song. Delays count as commands here: the decoder gives each one an item. A stretch's depth is the number
    // of calls under way on its track. `note_commands[i].stretch` counts in the stretches of `note_starts[i].track`.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;
    std::vector<NoteStart> note_starts;

    // Each note's ticks in the MIDI file, at the note's ID less 1, with kBeatQuarterTicks to a quarter note. Those
    // ticks follow the beat that ntdlib::sequence_export::agb::BeatGrid works out, and `midi_end_tick` gives the file's
    // end.
    std::vector<MidiPlace> midi_places;
    uint32_t midi_end_tick = 0;

    // Each track's changes of its volume and pan, in the order of their ticks. The volume is the value that the command
    // holds: what it runs up to depends on the revision and the track. The pan runs from -64 for the left to 63 for the
    // right. Both are 0 before a command sets them. The driver has no sends. Dungeon Dice Monsters and the sample
    // tracks of the Eternal Duelist and Captain Tsubasa revisions have no pan of a track.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A song whose header can't be read gives a performance without
// tracks, with a warning. A song that plays no note gives a performance with no notes, and its stretches and its mix
// stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::konami::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::konami
