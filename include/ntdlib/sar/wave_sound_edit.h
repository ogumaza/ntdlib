// SPDX-License-Identifier: MIT

// Edits of the wave sounds in a wave sound file of the 3DS (CWSD), the Wii U or the Switch (FWSD). The file holds one
// block, INFO. Its body holds a reference to the table of waves and one to the table of wave sounds (see WaveSoundFile
// in formats.h). A wave sound's settings are optional parameters: the pan and the surround pan in the first, the pitch
// in the second, the filters in the third, the offset of the sends in the ninth and the offset of a reference to the
// envelope in the tenth. The sends hold the main send, the number of aux sends and the aux sends. A note holds its
// wave's index in the table of waves. On the 3DS, the edits reach only the values that the reader reads there: the
// pan, the surround pan, the pitch, the sends to buses A and B, and the envelope.
//
// NintendoWare's tools list in the table of waves each wave that the notes play, once, in the order in which the wave
// sounds' notes first name them. The table of wave sounds follows the table of waves. Each reference after the table
// counts from a place after it, and the file ends with INFO's last structure.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// The values of a wave sound that its settings hold a place for.
struct WaveSoundPlaces
{
    friend bool operator==(const WaveSoundPlaces&, const WaveSoundPlaces&) = default;

    bool pan = false; // the pan and the surround pan
    bool pitch = false;
    bool filters = false; // only from version 1.1.0 of the file on, where the libraries read them
    bool sends = false;
    uint8_t aux_sends = 0; // the aux sends that the sends hold, up to 3, or up to 2 on the 3DS
    bool envelope = false;
};

// The places of wave sound `sound` of `file`, a wave sound file of `flavor`. A file of the 3DS gives no place for the
// filters. Throws std::invalid_argument for a sound past the file's table. Throws xsflib::FormatError for a file that
// isn't a wave sound file of `flavor`.
WaveSoundPlaces WaveSoundValuePlaces(std::span<const uint8_t> file, Flavor flavor, std::size_t sound);

// `file` with wave sound `sound`'s pan, surround pan, pitch, filters, sends and envelope from `values`. Its tracks and
// notes stay, and so does every other byte. Throws std::invalid_argument where a value without a place differs from the
// value that the reader gives without it, and as WaveSoundValuePlaces does.
std::vector<uint8_t> WithWaveSoundValues(std::span<const uint8_t> file, Flavor flavor, std::size_t sound,
                                         const WaveSoundFile::WaveSound& values);

// `file` with note `note` of wave sound `sound` playing `wave`. The table of waves then lists the waves of the notes as
// the tools list them, and each note names its wave's entry. The table can grow or shrink. The structures after it then
// move, and only the reference to the table of wave sounds, the size of INFO and the size of the file change with them.
// Throws std::invalid_argument for a note past the sound's table, and as WaveSoundValuePlaces does.
std::vector<uint8_t> WithWaveSoundWave(std::span<const uint8_t> file, Flavor flavor, std::size_t sound,
                                       std::size_t note, WaveId wave);

} // namespace ntdlib::sar
