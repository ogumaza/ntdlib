// SPDX-License-Identifier: MIT

// The conversion of a song or a sound effect of DSE, Procyon Studio's sound driver for the DS, to MIDI, a SoundFont and
// SFZ files. The performer (performance.h) plays the sequence on the driver's model. Each preset of the performance is
// a program of a bank, with the channel's envelope and LFOs. It becomes a SoundFont instrument and preset, and a
// <master> of the SFZ files, with a zone for each of the program's splits (instruments.h).
//
// A pitch LFO is SoundFont's vibrato LFO, and a volume LFO its modulation LFO. Both are triangles in SoundFont. A pan
// LFO, an LFO's fade and the other waveforms are SFZ's alone. The driver adds a volume LFO to the note's level before
// it squares the level. Its swing in dB therefore depends on the note's level. The zones and regions take the swing at
// a velocity of 127.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "seqlib/sfz/track_file.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/dse/instruments.h"
#include "ntdlib/sequence_export/dse/performance.h"

namespace ntdlib::sequence_export::dse
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

// Plays the song or the sound effect of `files` and builds its files. `name` is the sequence's name, and `program` the
// name that their comments give the program, such as "xyz2midi". Throws std::invalid_argument for files with neither,
// and dse::Hang when a track hangs the driver.
Conversion ConvertSequence(const SequenceFiles& files, const std::string& name, const PerformOptions& options,
                           const std::string& program);

// The presets' names for both writers: the bank and the program, numbered when a program has several presets
// ("b1 p5 #2").
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys);

// The zone's root key and tuning in SoundFont's and SFZ's terms, for a sample of `sample_rate`: the voice's rate at
// the root key, without a bend, gives the tuning. The root key is the zone's key nearest to middle C.
struct ZoneTuning
{
    int root_key = 60;
    int coarse = 0; // semitones
    int fine = 0;   // cents
};

ZoneTuning TuningOf(const PresetZone& zone, uint32_t sample_rate);

// The zone's level against full level, in amplitude: the square of its volume, and the gain of its pan against the
// centre's (linear_pan.h). A zone off the centre plays above full level.
double ZoneLevel(const PresetZone& zone);

// The sequence's SFZ files: one for each MIDI track, with the presets that it plays, and the WAV files of the samples,
// named after the waves (BankSet::WaveName). The bend ranges come from `midi`, and the names from `title` and
// `preset_names`. `program` goes in each file's first comment.
seqlib::sfz::Instruments BuildSfz(const Performance& perf, const seqlib::midi::MidiReport& midi, BankSet& banks,
                                  const std::string& title, const std::vector<std::string>& preset_names,
                                  const std::string& program);

// Builds SoundFonts and SFZ files of the programs at `programs` of `bank`, in ascending order, each once. `main_bank`
// is the bank that holds the waves of a bank without samples, or null. Each program p that plays something becomes
// preset p % 128 in bank p / 128 of a SoundFont, and an SFZ file by itself (BuildBankSfz). It plays as a channel plays
// it when it selects the program, with the splits' envelopes and the program's LFOs. A program that plays nothing gets
// no preset and no file, and a warning, in the order given. A zone above full level makes every zone of the SoundFonts
// quieter. A warning says so. `name` names the bank in the files, and `program` is the name that goes in the
// SoundFonts' ISFT and comments and in the SFZ files.
ntdlib::sequence_export::BankConversion ConvertBank(const ntdlib::dse::WaveBank& bank,
                                                    const ntdlib::dse::WaveBank* main_bank,
                                                    std::span<const uint32_t> programs, const std::string& name,
                                                    const std::string& program);

// The SFZ file of each of `programs` of bank `id` of `banks`, each of which plays its program by itself as a channel
// plays it when it selects the program, and the WAV files of the samples, named after the waves (BankSet::WaveName).
// Program p's file is "<title>_<p>.sfz", with p padded to the digits of the last program. `title` names the bank.
// `program` goes in each file's first comment.
seqlib::sfz::Instruments BuildBankSfz(std::span<const uint32_t> programs, BankSet& banks, uint16_t id,
                                      const std::string& title, const std::string& program);

} // namespace ntdlib::sequence_export::dse
