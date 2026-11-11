// SPDX-License-Identifier: MIT

// The steps of a sequence's conversion around a program's performer and its SoundFont and SFZ builders. Each step takes
// the program's Conversion: a struct with the fields that the frame reads (frame.h). Its `performance` is a
// PerformanceBase. `midi` and `sf2` hold the files, `samples` the SoundFont's number of samples, `channels` the number
// of MIDI channels used, and `warnings` the conversion's warnings.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"
#include "xsflib/binary.h"

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

// The attenuation that every zone of a SoundFont takes when its loudest zone plays at `loudest` (see
// CutAboveFullLevel), and the message that reports it. The message is empty for a cut under 0.5 cB.
struct LevelCut
{
    double centibels = 0.0;
    std::string message;
};

inline LevelCut LevelCutOf(double loudest)
{
    constexpr double kLeastReportedCut = 0.5;
    LevelCut cut;
    if (loudest > 1.0)
    {
        cut.centibels = 200.0 * std::log10(loudest);
    }
    if (cut.centibels >= kLeastReportedCut)
    {
        char what[160];
        std::snprintf(what, sizeof(what),
                      "instrument volume above full level (SoundFont attenuated by %.1f dB; SFZ keeps the level)",
                      cut.centibels / 10.0);
        cut.message = what;
    }

    return cut;
}

// The attenuation in centibels that every zone of the SoundFont of a sequence takes when the loudest zone plays at
// `loudest`, an amplitude where 1 is full level. SoundFont zones can't amplify their samples. The same attenuation in
// every zone keeps their balance. Where the zones take 0.5 cB or more, `c.performance` records the approximation. The
// report gives the attenuation in tenths of a decibel. A smaller one would read 0.0 dB. The SFZ files keep the levels.
// A zone at full level or below takes none.
template <typename Conversion>
double CutAboveFullLevel(Conversion& c, double loudest)
{
    const LevelCut cut = LevelCutOf(loudest);
    if (!cut.message.empty())
    {
        c.performance.approximations[cut.message] = seqlib::midi::NoteCount(c.performance);
    }

    return cut.centibels;
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

// Runs `convert` and returns its Conversion. `errors` lists the errors that the input's reader has collected, and the
// ones from `reported` on are new since the last conversion. A conversion that succeeds adds the new errors to its
// warnings. A conversion that fails throws FormatError with the new errors added to its message, each after "; ". It
// rethrows a failure unchanged when there are no new errors. Either way, `reported` moves to the end of `errors`. The
// next conversion then doesn't report them again.
template <typename Convert>
auto ConvertReportingErrors(const std::vector<std::string>& errors, std::size_t& reported, Convert&& convert)
    -> decltype(convert())
{
    decltype(convert()) c;
    try
    {
        c = convert();
    }
    catch (const std::exception& e)
    {
        if (reported == errors.size())
        {
            throw;
        }

        std::string message = e.what();
        for (; reported < errors.size(); reported++)
        {
            message += "; " + errors[reported];
        }
        throw xsflib::FormatError(message);
    }

    for (; reported < errors.size(); reported++)
    {
        c.warnings.push_back(errors[reported]);
    }

    return c;
}

} // namespace ntdlib::sequence_export
