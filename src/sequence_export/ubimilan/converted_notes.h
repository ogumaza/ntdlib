// SPDX-License-Identifier: MIT

// The notes that the conversion of a piece of Ubisoft Milan's driver writes, for the piece's performance.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/sequencer.h"

namespace ntdlib::sequence_export::ubimilan
{

// A note of the MIDI file, with the frames that the driver plays it in and its ticks in the file.
struct ConvertedNote
{
    int channel = 0; // the performance's track: the sequence channel, or 3 for the noise
    uint32_t on_frame = 0;
    uint32_t off_frame = 0;
    uint32_t on_tick = 0;
    uint32_t off_tick = 0;
    int key = 0;
    int velocity = 0;
    int bank = 0;
    int program = 0;
    uint32_t command = 0; // the address of the command that started it
    uint32_t read = 0;    // the read of the sequence that ran the command
};

// A piece as its conversion plays it.
struct ConvertedSong
{
    std::vector<std::string> warnings;
    bool truncated = false;
    bool loops = false;
    uint32_t loop_start = 0; // frames
    uint32_t loop_end = 0;
    uint32_t end_frame = 0;
    uint32_t end_tick = 0;
    double quarter_frames = 0;                         // the frames of a quarter note at the piece's first tempo
    std::vector<ntdlib::ubimilan::SequenceRead> reads; // each pass of the track through a sequence
    std::vector<ConvertedNote> notes;                  // each channel's notes in order, the channels in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result.
ConvertedSong ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::ubimilan::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::ubimilan
