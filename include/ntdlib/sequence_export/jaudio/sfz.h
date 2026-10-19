// SPDX-License-Identifier: MIT

// SFZ instruments and WAV samples for a performance.
//
// Each sequence track gets one SFZ file containing its presets, selected by program change. SFZ players ignore MIDI
// channels. Each file serves the matching MIDI track and its layers. Output targets sfizz.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/wave.h"

namespace ntdlib::sequence_export::jaudio
{

class BankSet;

// What seqlib's MIDI writer reports about a file, such as each track's pitch bend range.
using seqlib::midi::MidiReport;

// A sequence's SFZ files, named after their MIDI tracks, such as "Track 3.sfz", and the samples that they share, named
// "w<wave system>_<wave>.wav" (seqlib/sfz/track_file.h).
using SfzSample = seqlib::sfz::Sample;
using SfzInstruments = seqlib::sfz::Instruments;

// Shared sample directory, beside the sequence directories.
inline const std::string& kSfzSamplesFolder = seqlib::sfz::kSamplesFolder;

// The samples of a sequence's SFZ files: a WAV file for each wave, made once, on first use. A wave goes by its name,
// such as "w0_12" (WaveName).
class SfzSamples
{
public:
    // A sample's file name and loop. SFZ's loop end is the loop's last sample. A wave that can't be read has no name.
    struct Wave
    {
        std::string name;
        bool loop = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
    };

    // The sample of the wave named `name`, in the file <name>.wav. On first use, `pcm` gives the wave, or nullptr for
    // a wave that can't be read.
    const Wave& Get(const std::string& name, const std::function<const Pcm*()>& pcm);

    // Takes the WAV files made. The collection is left empty.
    std::vector<SfzSample> Take()
    {
        return std::move(files_);
    }

private:
    std::map<std::string, Wave> waves_;
    std::vector<SfzSample> files_;
};

// Build SFZ files for active tracks in `perf`. Take the bend ranges from `midi`. `title` and `preset_names` supply
// names for comments. `program` is the name that goes in each file's first comment.
SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks, const std::string& title,
                        const std::vector<std::string>& preset_names, const std::string& program);

} // namespace ntdlib::sequence_export::jaudio
