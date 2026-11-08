// SPDX-License-Identifier: MIT

// The notes that the conversion of a song of AlphaDream's driver writes, for the song's performance.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/alphadream/sequencer.h"

namespace ntdlib::sequence_export::alphadream
{

// A note of the MIDI file, with the frames that the driver plays it in, its ticks in the file and the command behind
// it.
struct ConvertedNote
{
    int track = 0;
    uint32_t on_frame = 0;
    uint32_t off_frame = 0;
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    int key = 0;
    int preset = 0;       // the note's preset in the SoundFont: PresetNumber() of the track's kind and the instrument
    uint32_t command = 0; // the address of the note's bytes, or 0 for a note that the channel's restore starts
};

// A song as its conversion plays it.
struct ConvertedSong
{
    bool valid = false; // the song table names a song that plays a track
    std::vector<std::string> warnings;
    bool truncated = false;
    uint32_t address = 0; // the song's header
    bool loops = false;
    uint32_t loop_start = 0; // frames
    uint32_t loop_end = 0;
    uint32_t end_frame = 0;
    uint32_t end_tick = 0; // the MIDI file's end: the song's, or a tick later for a note that starts on its last tick
    double quarter_frames = 0; // the frames of a quarter note at the song's first tempo, or 0 for a silent song
    std::array<std::string, kTracks> track_names;
    std::vector<TrackRead> reads;     // what the tracks read, in the order that the driver read it
    std::vector<ConvertedNote> notes; // each track's notes in order, the tracks in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result.
ConvertedSong ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::alphadream
