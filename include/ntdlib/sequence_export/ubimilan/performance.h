// SPDX-License-Identifier: MIT

// The performance of a piece of Ubisoft Milan's GBA driver for an editor. ntdlib's model of the driver
// (ntdlib::ubimilan::Sequencer) plays the piece as ConvertSong does. The performance holds the notes of ConvertSong's
// MIDI file, each with the command that started it. It also holds each pass of the track through a sequence.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::sequence_export::ubimilan
{

// A note's ticks in the MIDI file, on the piece's beat.
struct MidiPlace
{
    bool operator==(const MidiPlace&) const = default;

    uint32_t on = 0;
    uint32_t off = 0;
};

// A sequence of the piece's list.
struct PieceSequence
{
    bool operator==(const PieceSequence&) const = default;

    int resource = 0;  // its resource number
    uint32_t base = 0; // the address of its first byte, from which its offsets count
};

// A recorded piece: PerformanceBase, with the records for an editor.
//
// A tick is one of the driver's frames (ntdlib::agb::kFrameRate of them to a second). The driver's tempo never changes.
// `timebase` is the number of whole frames nearest to a quarter note at the piece's first tempo, and the one entry of
// `tempo` gives that many frames their time. Each of the sequence's channels plays on the track of the same number.
// The noise that channel 9's keys up to 11 play goes on track 3, named "Noise", as in the MIDI file: no sequence
// channel 3 plays notes. A note of a PSG channel is a run of frames in which the channel sounds, at the velocity 127,
// and a noise note's key and program name its drum. A kit note's velocity gives its level. The driver has no volume,
// pan or sends of a track. A preset is 128 times the note's bank plus its program. A piece that is a sample (a song
// past DriverInfo::piece_count) has no sequences and no stretches, and its notes' commands stay at 0.
struct Performance : PerformanceBase
{
    std::vector<PieceSequence> sequences; // in the order of the piece's list

    // The driver has one track of commands for all the channels. A stretch is a pass of that track through a sequence:
    // it begins where the track takes the sequence, and runs up to the last item that the track read. Its offsets count
    // from the sequence's base, and DecodeSequence gives the items there. Countdowns count as commands here: the
    // decoder gives each one an item. `stretch_sequences[i]` gives the place in `sequences` of the sequence that
    // stretch i reads. A piece's first stretch takes Entry::kOpen, a stretch that starts the list again Entry::kLoop,
    // and any other Entry::kJump.
    std::vector<Stretch> stretches;
    std::vector<uint32_t> stretch_sequences;

    // The command of each note, at the note's ID less 1 (TrackEvent::note).
    std::vector<NoteCommand> note_commands;

    // Each note's ticks in the MIDI file, at the note's ID less 1, with kBeatQuarterTicks to a quarter note. Those
    // ticks follow the beat that ntdlib::sequence_export::agb::BeatGrid works out, and `midi_end_tick` gives the file's
    // end.
    std::vector<MidiPlace> midi_places;
    uint32_t midi_end_tick = 0;
};

// Plays `song` as ConvertSong does with `settings`, and records its performance. Each note of the MIDI file that
// ConvertSong writes is a note of the performance. A piece whose list of sequences can't be read gives a performance
// without tracks, with a warning. A piece that plays no note gives a performance with no notes, and its stretches stay.
Performance PerformSong(const ntdlib::agb::Rom& rom, const ntdlib::ubimilan::DriverInfo& info, int song,
                        const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::ubimilan
