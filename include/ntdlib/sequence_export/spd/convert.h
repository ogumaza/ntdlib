// SPDX-License-Identifier: MIT

// The conversion of a sound of Nintendo SPD's sound driver for the DS to MIDI, a SoundFont and SFZ files. The model of
// the whole driver (ntdlib/spd/player.h) plays the sound a frame at a time, with the sounds that its actions start, and
// the performer records the model's writes to the sound unit's registers.
//
// - Each hardware channel becomes a MIDI track. A channel's start ends the track's note and starts the next one. A
//   sample that plays once ends with its last sample. A channel that the driver clears ends its note.
// - A MIDI tick is a frame of the driver (about 7.82 ms). The quarter note holds the frames of 48 of the driver's ticks
//   at the first tempo of the sound's first track, rounded: the bars of most sequences then line up in an editor.
// - The pitch of a channel's timer becomes the note's key and a pitch bend. A sample's key is its root key at the timer
//   that its header gives. A square wave's or noise's key is 64 at the driver's timer of 0x18DA. Every later change of
//   the timer is a bend.
// - The sound unit's level is linear in the channel's volume: volume / 128. The driver's envelopes, fades and volumes
//   all move that volume, frame by frame. CC7 stays at 127. CC11 holds the level, with the gain of the channel's pan
//   (linear_pan.h). CC10 holds the pan.
// - Each sample that the sound plays is a preset with one zone over every key, with its loop, no envelope and the
//   shortest release. A square wave or noise is a preset of the sound unit's wave (psg_samples.h).
// - The capture reverb's channels play what the sound unit captured. MIDI and SoundFont can't hold them, and they're
//   left out. The reverb's level, the volume of those channels from 0 to 127, goes to every track as CC91: the
//   nearest form. CC91 is 0 while the reverb is off. The report counts the sounds that play it as an approximation.
// - A sound whose tracks loop plays from the start of the longest loop. The MIDI file marks the loop from the frame of
//   that track's 0xF8 to the frame of its first 0xF9, and plays it the times that the options give.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "seqlib/sfz/track_file.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "ntdlib/spd/driver.h"
#include "ntdlib/spd/song.h"

namespace ntdlib::sequence_export::spd
{

// What a hardware channel plays: a sample by its header's address in the game's memory, or the sound unit's wave.
struct Source
{
    uint32_t header = 0; // the sample's header, or 0 for a square wave or noise
    uint8_t control = 0; // SOUNDxCNT bits 24-30: the duty, the repeat mode and the format
    uint16_t loop = 0;   // SOUNDxPNT, in words
    uint32_t length = 0; // SOUNDxLEN, in words
    bool noise = false;  // a channel of the noise's group plays the wave as noise

    auto operator<=>(const Source&) const = default;
};

// A recorded sound: PerformanceBase, with the source of each preset.
struct Performance : PerformanceBase
{
    std::vector<Source> presets;
};

// Options for playing a sound.
struct PerformOptions
{
    // Main loop repetitions, at least 1.
    int loops = 1;

    // The value that 0xFC and 0xFD test on player 0 (the ARM9's command 10).
    int branch_flag = 0;
};

// The sound data in memory, as the game lays it out: the header, set A and the block of set B.
struct SoundData
{
    std::span<const uint8_t> header;
    const ntdlib::spd::Block* set_a = nullptr;
    const ntdlib::spd::Block* set_b = nullptr; // or null
    int set_b_slot = 0;
};

// Plays sound `sound` (from 1) of the block in slot `slot` (0 for set A) and records it. Throws xsflib::FormatError for
// a sound that the model can't play.
Performance Perform(const SoundData& data, int slot, int sound, const PerformOptions& options = {});

// The files of a converted sound, its performance, and its statistics for the conversion's report.
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

// Plays the sound and builds its files. `name` is the sound's name, and `program` the name that the files' comments
// give the program, such as "xyz2midi".
Conversion ConvertSequence(const SoundData& data, int slot, int sound, const std::string& name,
                           const PerformOptions& options, const std::string& program);

// The sound data of a cartridge image, read: the driver's file, its blocks and its sounds (driver.h).
struct SongSet
{
    ntdlib::spd::DriverInfo info;
    std::span<const uint8_t> data; // the file's bytes, in the image
    std::vector<ntdlib::spd::Block> blocks;
};

// The sound data in `rom`, or none when the image holds none. A file that can't be read gives a line in `warnings`.
std::vector<SongSet> FindSongs(const ntdlib::ntr::Rom& rom, std::vector<std::string>& warnings);

// The sound data that plays song `song` of `set` (an index into DriverInfo::songs): set A, and the song's block as set
// B. A song of set A plays without a set B.
SoundData SoundDataOf(const SongSet& set, int song);

} // namespace ntdlib::sequence_export::spd
