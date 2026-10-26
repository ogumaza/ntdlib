// SPDX-License-Identifier: MIT

// A wave sound file of the Wii (RWSD) as its wave sounds and its waves, in parts that a writer can put back together.
// The writer lays the file out as NintendoWare for Revolution's tools do. The reader of formats.h (WaveSoundFile) reads
// the values that NW4R SND plays.
//
// The file's header takes 32 bytes. It lists DATA and, in a file of version 1.2, WAVE. DATA holds the number of wave
// sounds, then a reference to each wave sound. Each reference takes 8 bytes: 1, three zeros and an offset from the
// number of wave sounds. A wave sound holds three references: to its settings, to its table of tracks and to its table
// of notes. Its settings take 32 bytes: its pitch, its pan, its surround pan, its sends to the aux buses A to C and its
// main send, then zeros. A table holds the number of its entries, then a reference to each entry. A track holds a
// reference to its table of note events. A note event takes 16 bytes: its position, its length, the index of its note
// in the wave sound's table of notes, and zeros. A note takes 48 bytes: its wave's index, its envelope and hold, its
// original key, its volume, its pan, its surround pan and its pitch, then zeros. The structures follow the references
// in pre-order: each wave sound's references, its settings, its table of tracks, each track followed by its table of
// note events and the events, then its table of notes and the notes. Zeros pad DATA to 32 bytes.
//
// A file of version 1.2 describes its waves in WAVE (see wave_file.h). WAVE holds the number of waves, then the offset
// of each wave's info from WAVE's start, then the infos one after another. Zeros pad WAVE to 32 bytes. The file's wave
// data holds the raw samples. A file of version 1.3 has no WAVE block, and its wave data is an RWAR of RWAV files.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/rsar/wave_file.h"

namespace ntdlib::rsar
{

struct WsdFile
{
    // A note event of a track: when the note plays, for how long, and the note's index in the wave sound's notes.
    struct NoteEvent
    {
        friend bool operator==(const NoteEvent&, const NoteEvent&) = default;

        float position = 0.0f;
        float length = 0.0f;
        uint32_t note = 0;
    };

    struct Track
    {
        friend bool operator==(const Track&, const Track&) = default;

        std::vector<NoteEvent> events;
    };

    // A note: the wave that it plays, with its envelope, key, volume, pan and pitch.
    struct Note
    {
        friend bool operator==(const Note&, const Note&) = default;

        int32_t wave_index = 0; // into the file's waves
        uint8_t attack = 127, decay = 127, sustain = 127, release = 127;
        uint8_t hold = 0;
        uint8_t original_key = 60;
        uint8_t volume = 127;
        uint8_t pan = 64;
        uint8_t surround_pan = 0;
        float pitch = 1.0f;
    };

    // A wave sound: its settings, its tracks and its notes.
    struct WaveSound
    {
        friend bool operator==(const WaveSound&, const WaveSound&) = default;

        float pitch = 1.0f;
        uint8_t pan = 64;
        uint8_t surround_pan = 0;
        std::array<uint8_t, 3> fx_sends{}; // the sends to aux buses A to C
        uint8_t main_send = 127;
        std::vector<Track> tracks;
        std::vector<Note> notes;
    };

    // The bytes of a wave sound file and of its wave data.
    struct Files
    {
        friend bool operator==(const Files&, const Files&) = default;

        std::vector<uint8_t> file;
        std::vector<uint8_t> wave_data;
    };

    // Reads the wave sound file in `file` and its waves in `wave_data`. Throws xsflib::FormatError for bytes that
    // aren't a wave sound file of version 1.2 or 1.3, wave data that doesn't hold the samples that a file of
    // version 1.2 describes or isn't an RWAR for a file of version 1.3, and a file that the tools didn't lay out: Write
    // has to give `file` and `wave_data` back.
    static WsdFile Read(std::span<const uint8_t> file, std::span<const uint8_t> wave_data);

    // The file and its wave data, laid out as the tools lay a wave sound file out. Throws std::invalid_argument for a
    // version other than 1.2 and 1.3, and the exceptions of WaveFile::Write.
    Files Write() const;

    friend bool operator==(const WsdFile&, const WsdFile&) = default;

    uint16_t version = 0x0103;
    std::vector<WaveSound> sounds;
    std::vector<WaveFile> waves;
};

} // namespace ntdlib::rsar
