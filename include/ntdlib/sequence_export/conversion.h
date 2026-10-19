// SPDX-License-Identifier: MIT

// The steps of a sequence's conversion around a program's performer and its SoundFont and SFZ builders. Each step takes
// the program's Conversion: a struct with the fields that the frame reads (frame.h). Its `performance` is a
// PerformanceBase. `midi` and `sf2` hold the files, `samples` the SoundFont's number of samples, `channels` the number
// of MIDI channels used, and `warnings` the conversion's warnings.

#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"

namespace ntdlib::sequence_export
{

// Writes the MIDI file of `c.performance`, titled `title`, with `options`, and notes how many channels it uses. Counts
// the notes that the writer cut short among the performance's approximations. Returns the writer's report.
template <typename Conversion>
seqlib::midi::MidiReport WriteMidiFile(Conversion& c, const std::string& title,
                                       const seqlib::midi::MidiOptions& options = {})
{
    seqlib::midi::MidiReport midi;
    c.midi = seqlib::midi::WriteMidi(c.performance, title, midi, options);
    c.channels = midi.channels;
    if (midi.cut_notes)
    {
        c.performance.approximations["overlapping same-key notes cut short (all MIDI channels in use)"] +=
            midi.cut_notes;
    }

    return midi;
}

// Gives `sf` its INFO fields and writes it, and notes how many samples it holds. INAM is `title`, at most 255 bytes,
// ICMT is `comment`, at most 65,534 bytes, and ISFT is `program`. With their terminating zeros, the format allows 256
// and 65,536 bytes.
template <typename Conversion>
void WriteSoundFont(Conversion& c, seqlib::sf2::SoundFont& sf, const std::string& title, const std::string& comment,
                    const std::string& program)
{
    sf.info.push_back({"INAM", title.substr(0, 255)});
    sf.info.push_back({"ICMT", comment.substr(0, 65534)});
    sf.info.push_back({"ISFT", program});
    c.samples = sf.samples.size();
    c.sf2 = seqlib::sf2::Write(sf);
}

// The attenuation in centibels that every zone of a sequence's SoundFont takes when its loudest zone plays at
// `loudest`, an amplitude where 1 is full level. SoundFont zones can't amplify their samples. The same attenuation in
// every zone keeps their balance. Where the zones take one, `c.performance` records the approximation. The SFZ files
// keep the levels. A zone at full level or below takes none.
template <typename Conversion>
double CutAboveFullLevel(Conversion& c, double loudest)
{
    double quieter = 0.0;
    if (loudest > 1.0)
    {
        quieter = 200.0 * std::log10(loudest);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "instrument volume above full level (SoundFont attenuated by %.1f dB; SFZ keeps the level)",
                      quieter / 10.0);
        c.performance.approximations[what] = seqlib::midi::NoteCount(c.performance);
    }

    return quieter;
}

// Collects the conversion's warnings in the order that PrintConversion prints them: those of the MIDI writer, the
// SoundFont builder, the banks, then the performer.
template <typename Conversion>
void CollectWarnings(Conversion& c, const seqlib::midi::MidiReport& midi, const std::vector<std::string>& builder,
                     const std::vector<std::string>& banks)
{
    c.warnings = midi.warnings;
    c.warnings.insert(c.warnings.end(), builder.begin(), builder.end());
    c.warnings.insert(c.warnings.end(), banks.begin(), banks.end());
    c.warnings.insert(c.warnings.end(), c.performance.warnings.begin(), c.performance.warnings.end());
}

} // namespace ntdlib::sequence_export
