// SPDX-License-Identifier: MIT

// The notes that the conversion of a song of Nintendo R&D1's driver writes, for the song's performance.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::rd1
{

// A note of the MIDI file, with the track and the address of its event. Ticks without a prefix are the song's: the
// ticks of the song's MIDI file, counted through every pass of its loop.
struct ConvertedNote
{
    int track = 0;        // the driver's track that read the note's event
    int channel = 0;      // the channel that plays it: the track's number, less 16 from track 16 on
    uint32_t command = 0; // the address of the note's event
    uint64_t on = 0;
    uint64_t off = 0;     // the tick nearest the note's end in the MIDI file, after its start
    uint32_t on_tick = 0; // the ticks of the conversion's MIDI file
    uint32_t off_tick = 0;
    int key = 0;
    int velocity = 0;
    int program = 0;
};

// A song as its conversion plays it.
struct ConvertedSong
{
    bool valid = false; // false for a song without a record or a MIDI file that the driver reads
    std::vector<std::string> warnings;
    bool truncated = false; // cut off after an hour
    uint32_t midi = 0;      // the address of the song's MIDI file
    int bank = 0;
    int division = 0; // the song's ticks in a quarter note
    bool loops = false;
    uint64_t loop_start = 0;
    uint64_t loop_end = 0;
    uint64_t end = 0;
    uint32_t end_tick = 0; // the MIDI file's end
    double seconds = 0;
    std::vector<std::pair<uint64_t, uint32_t>> tempos; // each change of the step: (tick, 256ths of a tick a frame)
    std::vector<ntdlib::rd1::TrackRead> reads;         // each read of the tracks' events
    std::vector<ConvertedNote> notes;                  // each channel's notes in order, the channels in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result.
ConvertedSong ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::rd1::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::rd1
