// SPDX-License-Identifier: MIT

// The performance of a sound of Brownie Brown's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::brownie::Sequencer) plays the sound as ConvertSong does. The performance holds the notes of ConvertSong's
// MIDI file, each with the command that started it. It also holds each channel's stretches of commands, and each
// channel's changes of its volume and pan.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::brownie
{

// How a channel's commands led to a note of the MIDI file.
enum class NoteCause : uint8_t
{
    kNote,    // a note command in the note's frame
    kEarlier, // no note command in the note's frame: the channel fell silent during an earlier note and sounds again,
              // such as when a sound effect gives a music channel's PSG channel back
};

// A note's ticks in the MIDI file, on the song's beat.
struct MidiPlace
{
    bool operator==(const MidiPlace&) const = default;

    uint32_t on = 0;
    uint32_t off = 0;
};

// A recorded sound: PerformanceBase, with the records for an editor.
//
// A tick is one of the driver's frames (ntdlib::agb::kFrameRate of them to a second). A note, a rest, a hold or a
// release makes a channel wait for the channel's length in frames. The driver's tempo never changes. `timebase` is the
// number of whole frames nearest to a quarter note at the song's first tempo, and the one entry of `tempo` gives that
// many frames their time. Each of the driver's channels plays on the track of the same number, and a preset is 128
// times the note's bank plus its program. Offsets count from `base`, the address of the first byte of the region that
// holds the sound's list (ntdlib::brownie::FindRegions). ntdlib::brownie::DecodeSong decodes the bytes from there. Such
// a region can hold many sounds.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of reads, in the order that they ran, and the command of each note, at the note's ID less
    // 1 (TrackEvent::note). A stretch begins where its channel starts, and after a jump, a call, a return or the end of
    // a repeat that goes back. A stretch's depth is 1 inside a call: the driver has no nested calls. Lengths count as
    // commands here: the decoder gives each one an item. `note_commands[i].stretch` counts in the stretches of the
    // note's track.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;
    std::vector<NoteCause> note_causes;

    // Each note's ticks in the MIDI file, at the note's ID less 1, with kBeatQuarterTicks to a quarter note. Those
    // ticks follow the beat that ntdlib::sequence_export::agb::BeatGrid works out, and `midi_end_tick` gives the file's
    // end.
    std::vector<MidiPlace> midi_places;
    uint32_t midi_end_tick = 0;

    // Each track's changes of its volume and pan, in the order of their ticks. The volume is the value that F2 holds,
    // from 0 to 255. The pan runs from -64 for the left to 63 for the right. Both are 0 before a command sets them. The
    // driver has no sends. In the Magical Vacation revision, the sample channels have no pan.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A sound whose list can't be read gives a performance without tracks,
// with a warning. A sound that plays no note gives a performance with no notes, and its stretches and its mix stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::brownie::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::brownie
