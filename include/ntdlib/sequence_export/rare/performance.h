// SPDX-License-Identifier: MIT

// The performance of a tune of Rare's GBA driver for an editor. ntdlib's model of the driver (ntdlib::rare::Sequencer)
// plays the tune as ConvertSong does. The performance holds the notes of ConvertSong's MIDI file, each with the command
// that started it. It also holds each track's stretches of commands, and each track's changes of the volume of its
// channels.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::rare
{

// A recorded tune: PerformanceBase, with the records for an editor.
//
// A tick is one of the tune's ticks: a delay of n in a track's data is n ticks, and the tune's header gives the ticks
// of a quarter note (`timebase`). With ConvertSettings::frame_timing, the notes move to the ticks at which their frames
// start, as in the MIDI file. Each of the tune's first 16 tracks plays on the track of the same number. A preset is the
// note's program. A note's velocity is the driver's, up to 127. Offsets count from `base`, the address of the tune's
// first track. ntdlib::rare::DecodeRomSequence decodes the bytes from there. ntdlib::rare::SequenceOfSong gives the
// sequence.
struct Performance : PerformanceBase
{
    uint32_t base = 0;

    // Each track's stretches of reads, in the order that they ran, and the command of each note, at the note's ID less
    // 1 (TrackEvent::note). A stretch begins where its track starts, and where the track goes back to its loop's start
    // (Entry::kLoop). The driver has no calls: each stretch's depth is 0. `note_commands[i].stretch` counts in the
    // stretches of the note's track.
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;

    // Each track's changes of the volume of its channels (controller 7), in the order of their ticks: the command's
    // value, from 0 to 127. A channel's volume is 127 before a command sets it. A track can set the volumes of several
    // channels. The changes of a tick give the last command's value. The driver has no pan and no sends.
    std::array<std::vector<MixChange>, 16> mix;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A tune whose header can't be read gives a performance without
// tracks, with a warning. A tune that plays no note gives a performance with no notes, and its stretches and its mix
// stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::rare::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rare
