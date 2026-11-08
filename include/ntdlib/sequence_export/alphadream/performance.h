// SPDX-License-Identifier: MIT

// The performance of a song of AlphaDream's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::sequence_export::alphadream::Sequencer) plays the song as ConvertSong does. The performance holds the notes
// of ConvertSong's MIDI file, each with the note's bytes that started it. It also holds each track's stretches of
// commands, and each track's changes of its volume and pan.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::alphadream
{

// A note's ticks in the MIDI file, on the song's beat.
struct MidiPlace
{
    bool operator==(const MidiPlace&) const = default;

    uint32_t on = 0;
    uint32_t off = 0;
};

// A recorded song: PerformanceBase, with the records for an editor.
//
// A tick is one of the driver's frames (ntdlib::agb::kFrameRate of them to a second). `timebase` is the number of whole
// frames nearest to a quarter note at the song's first tempo, and the one entry of `tempo` gives that many frames their
// time. Each of the song's tracks plays on the performance's track of the same number: tracks 0 to 7 play samples, and
// tracks 8 to 11 the two squares, the wave and the noise. A preset is the note's preset in ConvertSong's SoundFont: 512
// times the kind of the track's channel plus the instrument (PresetNumber). Every note has the velocity 127. Offsets
// count from `base`, the song's header. ntdlib::alphadream::DecodeRomSequence decodes the bytes from there, and
// ntdlib::alphadream::SequenceOfSong gives the sequence.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of reads, in the order that they ran, and the command of each note, at the note's ID less
    // 1 (TrackEvent::note). A stretch begins where its track starts (Entry::kOpen), and after each jump that the track
    // takes: Entry::kLoop for a jump to its command or before it, and Entry::kJump for one forward. The bytes of ticks
    // after a note count as commands here: the decoder gives each one an item. `note_commands[i].stretch` counts in
    // the stretches of the note's track. A note that the channel's restore starts when the song starts has no command.
    // Its offset and its stretch stay at 0.
    std::array<std::vector<Stretch>, ntdlib::alphadream::kTracks> stretches;
    std::vector<NoteCommand> note_commands;

    // Each note's ticks in the MIDI file, at the note's ID less 1, with kBeatQuarterTicks to a quarter note. Those
    // ticks follow the beat that ntdlib::sequence_export::agb::BeatGrid works out, and `midi_end_tick` gives the file's
    // end. A note that starts on the song's last tick ends a tick later, and the file then ends with it.
    std::vector<MidiPlace> midi_places;
    uint32_t midi_end_tick = 0;

    // Each track's changes of its volume and pan, in the order of their ticks. The volume is the track's, from 0 to
    // 255, and 200 before a command sets it. In A88E's version F1 sets it. In B85A's and A84J's versions, each note's
    // third byte sets it, and a note counts as a change only where its byte differs from the volume before it. The pan
    // is F2's byte from -64 for the left to 63 for the right, and 0 before a command sets it. The byte runs from 0 at
    // the right through 0x7F and 0x80 at the centre to 0xFF at the left.
    std::array<std::vector<MixChange>, ntdlib::alphadream::kTracks> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A song that the song table doesn't name, or that plays no track,
// gives a performance without tracks, with a warning. A song that plays no note gives a performance with no notes, and
// its stretches and its mix stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::alphadream::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::alphadream
