// SPDX-License-Identifier: MIT

// Sequence conversion to MIDI, SoundFont and SFZ.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/sf2/soundfont.h"

#include "ntdlib/sequence_export/jaudio/instruments.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sfz.h"
#include "ntdlib/sequence_export/jaudio/wave.h"

namespace ntdlib::sequence_export::jaudio
{

// Converted files, performance data and statistics for the conversion report.
struct Conversion
{
    Performance performance;
    std::vector<uint8_t> midi;
    std::vector<uint8_t> sf2;
    SfzInstruments sfz;
    std::size_t samples = 0; // SoundFont samples
    int channels = 0;        // MIDI channels used
    std::vector<std::string> warnings;
};

// Perform `sound` and build its MIDI, SoundFont and SFZ files from the banks and waves in `banks`. `program` is the
// name that the messages and the files' comments give the program, such as "xyz2midi". Throw FormatError if the
// sequence cannot be read.
Conversion ConvertSequence(const SequenceSound& sound, BankSet& banks, const PerformOptions& options,
                           const std::string& program);

// The vibrato's frequency at a rate of 1, in the SoundFont and the SFZ files: 72 sub-frames a cycle.
constexpr double kVibratoHz = 1.0 / (72.0 * kSubFrameSeconds);

// Preset names for both writers: the bank and program, numbered when a program has several presets.
std::vector<std::string> PresetNames(const std::vector<PresetKey>& keys);

// The zones of preset `preset` (an index into Performance::presets) in `perf`, from the banks that `banks` reads.
std::vector<PresetZone> ZonesOf(std::size_t preset, const Performance& perf, BankSet& banks);

// The name of wave `wave_id` of wave system `wave_system` among a sequence's samples, such as "w0_12".
std::string WaveName(uint32_t wave_system, uint16_t wave_id);

// The samples of a sequence's SoundFont, `sf`: the samples of each wave, added once, on first use. A wave goes by its
// name, such as "w0_12" (WaveName). `sf` has to outlive the samples.
class SoundFontSamples
{
public:
    // A wave's sample in the SoundFont: its id, and whether it loops.
    struct Sample
    {
        uint16_t id = 0;
        bool loop = false;
    };

    explicit SoundFontSamples(seqlib::sf2::SoundFont& sf) : sf_(sf)
    {
    }

    // The sample of the wave named `name`. On first use, `pcm` gives the wave, or nullptr for a wave that can't be
    // read, and the sample takes root key `root_key`. Returns nothing for a wave that can't be read.
    std::optional<Sample> Get(const std::string& name, uint8_t root_key, const std::function<const Pcm*()>& pcm);

private:
    seqlib::sf2::SoundFont& sf_;
    std::map<std::string, std::optional<Sample>> samples_;
};

} // namespace ntdlib::sequence_export::jaudio
