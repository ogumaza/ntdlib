// SPDX-License-Identifier: MIT

// The notes that the conversion of a song of Konami's driver writes, for the song's performance.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::konami
{

// A note of the MIDI file, with the frames that the driver plays it in and its ticks in the file.
struct ConvertedNote
{
    int track = 0;
    uint32_t on_frame = 0;
    uint32_t off_frame = 0;
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    int key = 0;
    int velocity = 0;
    int bank = 0;
    int program = 0;
};

// A song as its conversion plays it.
struct ConvertedSong
{
    std::vector<std::string> warnings;
    bool truncated = false;
    bool loops = false;
    uint32_t loop_start = 0; // frames
    uint32_t loop_end = 0;
    uint32_t end_frame = 0;
    uint32_t end_tick = 0;
    double quarter_frames = 0;                 // the frames of a quarter note at the song's first tempo
    std::vector<ntdlib::konami::TrackRead> reads; // each read of the tracks' data
    std::vector<ConvertedNote> notes;          // each track's notes in order, the tracks in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result.
ConvertedSong ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::konami::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::konami
