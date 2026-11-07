// SPDX-License-Identifier: MIT

// The performance of a song of Quintet's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::quintet::Sequencer) plays the song as ConvertSong does. The performance holds the notes of ConvertSong's
// MIDI file, each with the command that started it. It also holds each channel's stretches of commands, and the changes
// of each channel's volume.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::quintet
{

// A recorded song: PerformanceBase, with the records for an editor.
//
// The ticks are the MIDI file's: 37 for each of the driver's, and 3552 a quarter note. The song's 6 channels play on
// tracks 0 to 5: square 1, square 2, the wave channel, the noise channel, FIFO A and FIFO B. A preset is the note's
// instrument in the conversion. Every note has the velocity 127. Offsets count from `base`, the byte after the song's
// header. ntdlib::quintet::DecodeRomSequence decodes the bytes from there, and ntdlib::quintet::SequenceOfSong gives
// the sequence.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of commands, in the order that they ran, and the command of each note, at the note's ID
    // less 1 (TrackEvent::note). A stretch begins where its channel starts, where FF goes back to the loop point and
    // where D7 goes back to the start of its repeat (Entry::kLoop). Its depth counts the repeats that the channel is
    // in. `note_commands[i].stretch` counts in the stretches of the note's track.
    //
    // The MIDI file's tempo follows the first channel. A channel at another tempo T plays each of its ticks in
    // 37 × T0 / T of the file's ticks, where T0 is the first channel's tempo. On such a channel, a stretch also begins
    // after each note, rest and wait. It then holds at most one wait, at its end, and starts at the tick of the note or
    // rest that ends it. It keeps the entry of the stretch that it splits, and its depth counts the repeats that the
    // channel is in at its first command.
    //
    // A stretch also begins after 7F sets a channel's tempo, and on every channel after the first channel's 7F. The
    // driver counts the channel's ticks again from there, and the first note after it starts its frame. Such a stretch
    // keeps the entry of the stretch before it. With ConvertSettings::frame_timing, every note moves to the tick at
    // which its frame starts, as in the MIDI file. A stretch's waits then give its notes' ticks only to the frame.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;

    // Each track's changes of its channel's volume (Mix::kVolume), in the order of their ticks, and 0 before a command
    // sets it. 5E sets any byte. 5F steps the volume up to 15 on a square or the noise channel, 3 on the wave channel
    // (4 in the J revision) and 1 on a FIFO, and 6E steps it down to 0. The changes of a tick give the volume at its
    // end. The driver has no pan and no sends for a channel.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays `song` (a song or a sound effect, in the numbering of SongCount()) as ConvertSong does with `settings`, and
// records its performance. Each note of the MIDI file that ConvertSong writes is a note of the performance. A song
// whose header can't be read gives a performance without tracks, with a warning. A song that plays no note gives a
// performance with no notes, and its stretches and its mix stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::quintet::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::quintet
