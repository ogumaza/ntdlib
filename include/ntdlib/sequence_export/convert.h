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

// A bank's instruments as a SoundFont and as SFZ files, with the samples that the SFZ files share, and the warnings.
struct BankConversion
{
    std::vector<uint8_t> sf2;
    SfzInstruments sfz;
    std::size_t samples = 0; // SoundFont samples (two per stereo wave)
    std::vector<std::string> warnings;
};

// Builds a SoundFont and SFZ files of every instrument of bank `bank` of a CSAR or FSAR archive. Program p's instrument
// becomes the SoundFont's preset p % 128 in bank p / 128, and an SFZ file by itself (BuildBankSfz). Each instrument
// plays as a track plays it with its starting settings: without envelope overrides or a filter, at the centre, with an
// LFO whose depth CC1 sets. `program` is the name that goes in the SoundFont's ISFT and comment and in the SFZ files.
// Throws std::out_of_range when the archive lacks the bank.
BankConversion ConvertBank(const sar::SoundArchive& archive, std::size_t bank, const Settings& settings,
                           const std::string& program);

} // namespace ntdlib::sequence_export
