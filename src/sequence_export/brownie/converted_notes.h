// SPDX-License-Identifier: MIT

// The notes that the conversion of a sound of Brownie Brown's driver writes, for the sound's performance.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/sequence_export/agb/settings.h"

namespace ntdlib::sequence_export::brownie
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
    int bank = 0;
    int program = 0;
    uint32_t command = 0;       // the address of the note command that started it
    uint32_t command_frame = 0; // the frame that ran the command
};

// A sound as its conversion plays it.
struct ConvertedSong
{
    std::vector<std::string> warnings;
    bool truncated = false;
    bool loops = false;
    uint32_t loop_start = 0; // frames
    uint32_t loop_end = 0;
    uint32_t end_frame = 0;
    uint32_t end_tick = 0;
    double quarter_frames = 0;                       // the frames of a quarter note at the song's first tempo
    std::vector<ntdlib::brownie::ChannelRead> reads; // what the channels read, in the order the driver read it
    std::vector<ConvertedNote> notes;                // each channel's notes in order, the channels in order
};

// Plays `song` as ConvertSong() does with `settings`. Each note of the MIDI file becomes a note of the result.
ConvertedSong ConvertedNotes(const ntdlib::agb::Rom& rom, const ntdlib::brownie::DriverInfo& info, int song,
                             const agb::ConvertSettings& settings);

} // namespace ntdlib::sequence_export::brownie
