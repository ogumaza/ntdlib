// SPDX-License-Identifier: MIT

// SFZ instruments and WAV samples for a performance.
//
// Each sequence track gets one SFZ file containing its presets, selected by program change. SFZ players ignore MIDI
// channels. Each file therefore serves the matching MIDI track and its layers. Filter settings are sent on MIDI
// controllers (see filter.h). Output targets sfizz. Comparisons with game renders use sfizz too.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

class BankSet;
class RsarBankSet;

// A sequence's SFZ files ("Track <n>.sfz", or "<label> Track <n>.sfz" for a piece of several sequences) and the samples
// that they share, such as "w0_12.wav", or "w0_12L.wav" and "w0_12R.wav" for stereo (seqlib/sfz/track_file.h).
using SfzFile = seqlib::sfz::File;
using SfzSample = seqlib::sfz::Sample;
using SfzInstruments = seqlib::sfz::Instruments;

// Shared sample directory, beside the sequence directories.
inline const std::string& kSfzSamplesFolder = seqlib::sfz::kSamplesFolder;

// Builds SFZ files for active tracks in `perf` with bend ranges from `midi`, for the CSAR and FSAR formats. `title` and
// `preset_names` supply names for comments. `program` is the name that goes in each file's first comment.
SfzInstruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, BankSet& banks,
                        const std::string& title, const std::vector<std::string>& preset_names,
                        const Settings& settings, const std::string& program);

// The same for the Wii's RSAR format. `balance` plays stereo waves in balance pan mode.
SfzInstruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, RsarBankSet& banks,
                        const std::string& title, const std::vector<std::string>& preset_names, bool balance,
                        const Settings& settings, const std::string& program);

// Builds an SFZ file for each of `programs` of the bank in slot 0 of `banks`, for the CSAR and FSAR formats. Each file
// plays its instrument by itself, as a track plays it with its starting settings. Program p's file is
// "<title>_<p>.sfz", with p padded to the digits of the last program. `title` names the bank. The files find their
// samples in the samples' folder beside their folder. `program` is the name that goes in each file's first comment.
SfzInstruments BuildBankSfz(const std::vector<uint32_t>& programs, BankSet& banks, const std::string& title,
                            const Settings& settings, const std::string& program);

// The same for the bank of `banks`, of the Wii's RSAR format. `balance` plays stereo waves in balance pan mode. From
// the build of March 2008 on (settings.wii_build), the regions take their holds and note-off types.
SfzInstruments BuildBankSfz(const std::vector<uint32_t>& programs, RsarBankSet& banks, const std::string& title,
                            bool balance, const Settings& settings, const std::string& program);

} // namespace ntdlib::sequence_export
