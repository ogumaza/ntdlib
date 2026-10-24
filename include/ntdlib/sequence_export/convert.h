// SPDX-License-Identifier: MIT

// Sequence conversion to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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

// The same for sequences `sound_indices` of the Wii's RSAR format that a game starts in one sound frame and plays as
// one piece: one MIDI file, one SoundFont and one set of SFZ files, titled `name` (PerformTogether in performer.h).
// The SoundFont's comment lists the sequences. Each SFZ file is named after its MIDI track. Throws FormatError if a
// sequence cannot be read or the piece's tracks with notes are more than 16. Throws std::invalid_argument when
// `sound_indices` is empty or the sequences don't share their bank and pan mode.
Conversion ConvertTogether(const rsar::SoundArchive& archive, std::span<const uint32_t> sound_indices,
                           const std::string& name, const PerformOptions& options, const Settings& settings,
                           const std::string& program);

// A SoundFont of a bank's instruments, from program `first` to program `last`.
struct BankSoundFont
{
    uint32_t first = 0;
    uint32_t last = 0;
    std::vector<uint8_t> sf2;
    std::size_t samples = 0; // SoundFont samples (two per stereo wave)
};

// A bank's instruments as SoundFonts and as SFZ files, with the samples that the SFZ files share, and the warnings. One
// SoundFont holds every instrument, unless the instruments exceed the format's 65,535 zones, generators or modulators.
// They then go into several SoundFonts, each with the instruments of a run of programs, in order.
struct BankConversion
{
    std::vector<BankSoundFont> sound_fonts;
    SfzInstruments sfz;
    std::vector<std::string> warnings;
};

// Builds SoundFonts and SFZ files of every instrument of bank `bank` of a CSAR or FSAR archive. Program p's instrument
// becomes preset p % 128 in bank p / 128 of a SoundFont, and an SFZ file by itself (BuildBankSfz). Each instrument
// plays as a track plays it with its starting settings: without envelope overrides or a filter, at the centre, with an
// LFO whose depth CC1 sets. `program` is the name that goes in the SoundFonts' ISFT and comments and in the SFZ files.
// Throws std::out_of_range when the archive lacks the bank.
BankConversion ConvertBank(const sar::SoundArchive& archive, std::size_t bank, const Settings& settings,
                           const std::string& program);

// The same for bank `bank` of the Wii's RSAR format, by the rules of NW4R SND's build `settings.wii_build`. The
// programs are those that play a region at some key and velocity (rsar::Bank::Plays). Stereo waves play as in a sound
// of pan mode `pan_mode` (rsar::SoundInfo::pan_mode): in balance mode for 1, unless the build is of October 2006. That
// build has only dual mode.
BankConversion ConvertBank(const rsar::SoundArchive& archive, std::size_t bank, uint8_t pan_mode,
                           const Settings& settings, const std::string& program);

} // namespace ntdlib::sequence_export
