// SPDX-License-Identifier: MIT

// The conversion of a sequence of a DS sound archive (SDAT) to MIDI, a SoundFont and SFZ files. The performer
// (performance.h) plays the sequence on the driver's model. Each preset of the performance is a program of the
// sequence's bank, with the track's envelope and LFO. It becomes a SoundFont instrument and preset, and a <master> of
// the SFZ files, with a zone for each of the program's regions (instruments.h). A bank's instruments convert to
// SoundFonts and SFZ files by themselves too (ConvertBank). They take the same zones and regions.
//
// The LFO's depth goes by CC1. A pitch LFO is SoundFont's vibrato LFO, and a volume LFO its modulation LFO. Both are
// triangles in SoundFont. The DS's LFO is a sine, and so is SFZ's. A pan LFO is SFZ's alone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/ntr/instruments.h"
#include "ntdlib/sequence_export/ntr/performance.h"

namespace ntdlib::sequence_export::ntr
{

// The files of a converted sequence, its performance, and its statistics for the conversion's report.
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

// Plays `sequence` of `archive` and builds its files. `name` is the sequence's name in the files, and `program` the
// name that their comments give the program, such as "xyz2midi". Throws xsflib::FormatError when the archive lacks the
// sequence, its file or its bank, and ntdlib::ntr::snd::Hang when a track hangs the driver.
Conversion ConvertSequence(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                           const std::string& name, const PerformOptions& options, const std::string& program);

// The presets' names for both writers: the program, numbered when a program has several presets ("p5 #2").
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys);

// The gain of `zone`'s pan against the centre's (PanGain), in amplitude. A zone off the centre plays above full level.
double ZoneGain(const PresetZone& zone);

// An LFO's frequency at the speed `speed`: the driver steps a cycle of 128 steps by speed / 4 each frame.
double LfoHz(uint8_t speed);

// The LFO's depth at CC1's full value, per step of its range: in cents for a pitch LFO, and in dB for a volume LFO. The
// driver's LFO swings by 127 times its depth times its range over 16384 of a semitone, or 60 times that in 0.1 dB.
constexpr double kPitchLfoCents = 100.0 * 127.0 * 127.0 / 16384.0;
constexpr double kVolumeLfoDb = 6.0 * 127.0 * 127.0 / 16384.0;

// A pan LFO's swing at CC1's full value, per step of its range, in percent of a full pan: 127 times its depth times its
// range over 16384 of the pan's 64 steps.
constexpr double kPanLfoPercent = 100.0 * 127.0 * 127.0 / 16384.0;

// The sequence's SFZ files: one for each MIDI track, with the presets that it plays, and the WAV files of the samples,
// named after the waves (BankSet::WaveName). The bend ranges come from `midi`, and the names from `title` and
// `preset_names`. `program` goes in each file's first comment.
seqlib::sfz::Instruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, BankSet& banks,
                                  const ntdlib::ntr::snd::Build& build, const std::string& title,
                                  const std::vector<std::string>& preset_names, const std::string& program);

// Builds SoundFonts and SFZ files of the instruments at `programs` of bank `bank` of `archive`, in ascending order,
// each once. Each program p that plays something becomes preset p % 128 in bank p / 128 of a SoundFont, and an SFZ file
// by itself (BuildBankSfz). It plays as a track plays it with its starting settings: with the envelopes of its regions,
// at the centre, and with an LFO whose depth CC1 sets. A program that plays nothing gets no preset and no file, and a
// warning, in the order given. A zone above full level makes every zone of the SoundFonts quieter. A sequence's
// conversion does the same. A warning says so. `program` is the name that goes in the SoundFonts' ISFT and comments and
// in the SFZ files. Throws xsflib::FormatError when the archive lacks the bank.
ntdlib::sequence_export::BankConversion ConvertBank(const sdat::Archive& archive, uint16_t bank,
                                                    std::span<const uint32_t> programs,
                                                    const ntdlib::ntr::snd::Build& build, const std::string& program);

// The SFZ file of each of `programs` of the bank of `banks`, each of which plays its instrument by itself as a track
// plays it with its starting settings, and the WAV files of the samples, named after the waves (BankSet::WaveName).
// Program p's file is "<title>_<p>.sfz", with p padded to the digits of the last program. `title` names the bank.
// `program` goes in each file's first comment.
seqlib::sfz::Instruments BuildBankSfz(std::span<const uint32_t> programs, BankSet& banks,
                                      const ntdlib::ntr::snd::Build& build, const std::string& title,
                                      const std::string& program);

} // namespace ntdlib::sequence_export::ntr
