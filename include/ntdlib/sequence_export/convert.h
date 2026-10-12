// SPDX-License-Identifier: MIT

// Sequence conversion to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"

namespace ntdlib::rsar
{

class SoundArchive;

} // namespace ntdlib::rsar

namespace ntdlib::sequence_export
{

// Converted files, performance data and statistics for the conversion report.
struct Conversion
{
    Performance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    SfzInstruments sfz;
    std::size_t samples = 0; // SoundFont samples (two per stereo wave)
    int channels = 0;        // MIDI channels used
    std::vector<std::string> warnings;
};

// Performs sequence `sound_index` of a CSAR or FSAR archive and builds its MIDI, SoundFont and SFZ files. `program` is
// the name that goes in the SoundFont's ISFT and comment and in the SFZ files. Throws FormatError if the sequence
// cannot be read.
Conversion ConvertSequence(const sar::SoundArchive& archive, uint32_t sound_index, const PerformOptions& options,
                           const Settings& settings, const std::string& program);

// The same for a sequence of the Wii's RSAR format.
Conversion ConvertSequence(const rsar::SoundArchive& archive, uint32_t sound_index, const PerformOptions& options,
                           const Settings& settings, const std::string& program);

} // namespace ntdlib::sequence_export
