// SPDX-License-Identifier: MIT

// The SFZ files of a GBA conversion, made from its SoundFont (sf2.h). A song's MIDI file gets an SFZ file for each of
// its MIDI tracks, named after the track. The file contains a <master> for each preset that the track selects. A
// <master> matches its preset's program by loprog and hiprog. Where the presets are in more than one bank, it matches
// the bank by CC0 too. Each zone of the preset's instrument becomes a region with the zone's sample, keys, root key,
// tuning and loop. The samples become WAV files that the SFZ files share, in seqlib's samples folder.
//
// The GBA's mixers step through their samples without interpolation, and the PSG plays its steps as they are. The
// regions therefore use sample_quality=0, at which sfizz plays the nearest sample. The frame of each file follows
// seqlib's track files: CC10's pan curve, the track's bend range, and CC7 and CC11 squared, without smoothing. Output
// targets sfizz.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "seqlib/sfz/track_file.h"

#include "ntdlib/sequence_export/agb/sf2.h"

namespace ntdlib::sequence_export::agb
{

// A MIDI track of a song's file: its name, its bend range in semitones, and the presets that it selects. The presets
// are indices into the SoundFont's presets.
struct SfzTrack
{
    std::string name;
    int bend_range = 2;
    std::vector<std::size_t> presets;
};

// The SFZ file of each of `tracks`, for a song whose SoundFont is `sf`: "<name>.sfz", with the name made safe for a
// file name. A track without presets gets no file. `title` names the song in each file's first comment, and `program`
// the program that converted it. Throws std::invalid_argument for a preset index past the SoundFont's presets, and for
// a zone that contains a generator that the regions don't use. The zones of the GBA conversions' square, wave, noise
// and sample instruments contain only keys, a root key, a tuning, a sample mode and a sample.
seqlib::sfz::Instruments TrackSfz(const Sf2File& sf, std::span<const SfzTrack> tracks, const std::string& title,
                                  const std::string& program);

// The SFZ file of each preset of `sf`, "<title>_<p>.sfz". Each file plays its preset by itself. The number p is the
// preset's bank times 128 plus its program, padded to the digits of the largest. Each file starts with the bend range
// of a GBA conversion's MIDI track, 2 semitones. The files find their samples in the samples' folder beside their
// folder. Throws std::invalid_argument as TrackSfz does.
seqlib::sfz::Instruments PresetSfz(const Sf2File& sf, const std::string& title, const std::string& program);

} // namespace ntdlib::sequence_export::agb
