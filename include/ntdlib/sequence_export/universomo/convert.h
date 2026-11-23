// SPDX-License-Identifier: MIT

// The conversion of a song of Universomo's sound driver for the DS (Beat City) to MIDI, a SoundFont and SFZ files. The
// model of the driver's sequencer (ntdlib/universomo/player.h) plays the song a tick at a time, and the performer
// records the sound commands that it sends.
//
// - Each of the song's tracks plays on one hardware channel, and becomes a MIDI track. A channel's set-up starts a
//   note and ends the track's note before it. A note that plays its sample once ends with the sample.
// - The pitch of a channel's timer becomes the note's key and a pitch bend. Key 60 is pitch 0, the timer's period
//   0x7D3. The key is the nearest to the timer's pitch, and the bend holds the rest. Every later change of the timer is
//   a bend.
// - The sound unit's level is linear in the channel's volume: volume / 128 over the divider. CC7 stays at 127. CC11
//   holds the level, with the gain of the channel's pan (linear_pan.h). CC7 and CC11 play the square of their value.
//   CC10 holds the pan, 74 for even channels and 54 for odd ones.
// - Each sample of the bank that the song plays is a preset with one zone over every key: the sample at 8366 Hz on key
//   60, with its loop, no envelope and the shortest release. The sound unit cuts a channel at once.
// - A song that loops plays its passes from its restart order. The MIDI file marks the first pass that plays as the
//   pass after it does (passes.h), and plays the main loop the times that the options give. The game plays a note
//   that sounds at the loop's end on into the next pass. A player that loops on the markers cuts it, and the report
//   counts those notes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "seqlib/sfz/track_file.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "ntdlib/universomo/formats.h"

namespace ntdlib::sequence_export::universomo
{

// The rate at which a sample plays at key 60 without a bend: the rate of the timer's period 0x7D3, 8365.95 Hz, rounded.
// The rounding plays every note 0.011 cents high.
constexpr uint32_t kRootRate = 8366;

// A recorded song: PerformanceBase, with the bank's sample of each preset.
struct Performance : PerformanceBase
{
    std::vector<uint8_t> presets;
};

// Options for playing a song.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays one marked pass, after an earlier pass where that one
    // doesn't play as the pass after it.
    int loops = 1;

    // Whether the song loops, as the game's table of sounds gives it.
    bool loop = true;
};

// Plays `song` with the samples of `bank` and records it. Throws xsflib::FormatError for a song that the sequencer
// can't play.
Performance Perform(const ntdlib::universomo::Song& song, const ntdlib::universomo::SampleBank& bank,
                    const PerformOptions& options = {});

// The files of a converted song, its performance, and its statistics for the conversion's report.
struct Conversion
{
    Performance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    seqlib::sfz::Instruments sfz;
    std::size_t samples = 0; // SoundFont samples
    int channels = 0;        // MIDI channels used
    std::vector<std::string> warnings;
};

// Plays `song` and builds its files. `name` is the song's name, and `program` the name that the files' comments give
// the program, such as "xyz2midi".
Conversion ConvertSequence(const ntdlib::universomo::Song& song, const ntdlib::universomo::SampleBank& bank,
                           const std::string& name, const PerformOptions& options, const std::string& program);

// A song in a cartridge image: its number, its name and whether it loops, with the song read.
struct SongInput
{
    uint32_t number = 0;
    std::string name;
    bool loop = true;
    ntdlib::universomo::Song song;
};

// The songs of a cartridge image that holds Universomo's files: each song file in the folder that holds the sample
// bank's file, in the order of their numbers, with the bank. The folder's table of sounds gives which songs loop.
// Without the table every song loops. A song that can't be read gives a line in `warnings` and is left out,
// and so is a bank that can't be read. A song's name is "au" and its number, such as "au105".
struct SongSet
{
    std::string folder; // the bank's folder in the image's file system, with its last '/', or empty
    ntdlib::universomo::SampleBank bank;
    std::vector<SongInput> songs;
};

std::vector<SongSet> FindSongs(const ntdlib::ntr::Rom& rom, std::vector<std::string>& warnings);

} // namespace ntdlib::sequence_export::universomo
