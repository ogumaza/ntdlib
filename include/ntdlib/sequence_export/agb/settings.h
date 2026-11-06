// SPDX-License-Identifier: MIT

// The settings and the results of a GBA song's conversion, and the names of its files.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"

namespace ntdlib::sequence_export::agb
{

// The settings for converting songs.
struct ConvertSettings
{
    int loops = 2;                    // times a looping song's loop is played
    uint32_t track_mask = 0xFFFFFFFF; // tracks to include (bit t = track t; Krawall has 32, the others 16 at most)
    bool voice_channels = false;      // a MIDI channel for each of the driver's sound channels, where it can
    bool psg_exclusive = false;       // an exclusive class for each PSG channel on the slow releases (MP2K)
    bool frame_timing = false;        // each event on the frame the driver plays it in, rather than on the beat
    int version = 0;                  // the version of the song to convert: an index into Music::Versions()
    std::string out_dir = ".";
    std::string base_name = "song";
    std::string program; // the name of the converting program, for the files' comments and the SoundFont's ISFT
};

// A version of a song: one of the ways in which the game plays it.
struct SongVersion
{
    std::string name;   // what messages give after the song's number, such as "under water", or nothing
    std::string suffix; // what the files' names have after the song's number, such as "_underwater", or nothing
};

// A song's length, loop points, track count and conversion result. Only InspectSong() gives the song's address.
struct SongReport
{
    bool ok = false;        // converted, or skipped for playing no notes
    bool silent = false;    // no notes on the chosen tracks, so nothing was written
    uint32_t address = 0;   // the song's data in the ROM: its header, or a Konami song's sequence data
    double seconds = 0;     // duration including the requested number of loop passes
    double loop_start = -1; // seconds; -1 if the song doesn't loop
    double loop_end = -1;
    int tracks = 0; // chosen tracks that play notes
    double bpm = 0;
    std::string midi_path, sf2_path;
    std::vector<std::string> warnings;
};

// Makes a single filename component valid on Windows as well as macOS and Linux. Keeps UTF-8 characters intact.
// These rules strip every trailing dot and space, put '_' before a device name and give "output" for an empty name.
inline std::string SafeFileName(std::string name)
{
    static const seqlib::FileNameRules kRules = {seqlib::TrailingRule::kRemoveAll, seqlib::DeviceMark::kBeforeName,
                                                 false, "output"};

    return seqlib::SafeFileName(std::move(name), kRules);
}

// Returns a song's number for file names and titles: at least two digits, and as many as the largest of `count` songs'.
std::string SongNumber(int song, int count);

// Returns the path of a song's files without their extensions. The files go in the folder in `settings`. Their names
// start with the base name in `settings`, an underscore and `number`. SafeFileName() makes the base name safe for a
// file name.
std::string SongStem(const ConvertSettings& settings, const std::string& number);

} // namespace ntdlib::sequence_export::agb
