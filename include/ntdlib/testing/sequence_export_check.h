// SPDX-License-Identifier: MIT

// Checks of a conversion's output for tests: a sequence's SFZ instruments, a sequence's conversion, and every sequence
// of an archive. The MIDI files and SoundFonts are checked with seqlib's readers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/settings.h"
#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"

namespace ntdlib::testing
{

// Checks that SFZ files match the MIDI tracks named `track_names`, reference the sequence's WAV samples and keep loops
// within sample bounds. Returns an error, or an empty string on success. `Instruments` is sequence_export's
// SfzInstruments, or another type with the same `files` and `samples`.
template <typename Instruments>
std::string SfzProblem(const Instruments& sfz, const std::vector<std::string>& track_names)
{
    std::map<std::string, uint32_t> frames;
    for (const auto& s : sfz.samples)
    {
        const std::vector<uint8_t>& w = s.wav;
        if (w.size() < 44 || std::string(w.begin(), w.begin() + 4) != "RIFF" ||
            xsflib::LoadLe32(w.data() + 4) != w.size() - 8 || xsflib::LoadLe32(w.data() + 40) != w.size() - 44)
        {
            return "broken WAV file " + s.name;
        }

        frames[s.name] = xsflib::LoadLe32(w.data() + 40) / 2;
    }

    std::set<std::string> names, tracks;
    for (const auto& f : sfz.files)
    {
        names.insert(f.name);
    }
    for (std::size_t t = 1; t < track_names.size(); t++) // skip the conductor track
    {
        const std::string& track = track_names[t];
        tracks.insert(track.substr(0, track.find(" (layer")) + ".sfz");
    }
    if (names != tracks)
    {
        return std::to_string(names.size()) + " files for " + std::to_string(tracks.size()) + " tracks";
    }

    for (const auto& f : sfz.files)
    {
        std::istringstream lines(f.text);
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.starts_with("<region> "))
            {
                continue;
            }

            std::map<std::string, std::string> opcodes;
            std::istringstream words(line.substr(9));
            for (std::string word; words >> word;)
            {
                opcodes[word.substr(0, word.find('='))] = word.substr(word.find('=') + 1);
            }

            const auto it = frames.find(opcodes["sample"]);
            if (it == frames.end())
            {
                return f.name + " plays a sample the sequence doesn't have: " + opcodes["sample"];
            }
            if (opcodes.count("loop_end"))
            {
                const uint64_t start = std::stoull(opcodes["loop_start"]), end = std::stoull(opcodes["loop_end"]);
                if (start >= end || end >= it->second)
                {
                    return f.name + " loops outside " + it->first;
                }
            }
        }
    }

    return "";
}

// Checks sequence `name`'s conversion: its MIDI file (seqlib::midi::Problems), its SoundFont (seqlib::sf2::Check), its
// SFZ files (SfzProblem), and its frames. A tick's events go no later than the tick, and no earlier than the frame of
// the tick before. Reports each problem on stderr. Returns the number of failed checks. `Conversion` is
// sequence_export's Conversion, or another type with the same fields.
template <typename Conversion>
int CheckConversion(const Conversion& c, const std::string& name)
{
    int failures = 0;
    const std::vector<std::string> midi_problems = seqlib::midi::Problems(c.midi);
    const std::vector<std::string> sf2_problems = seqlib::sf2::Check(c.sf2);
    const std::string sfz_problem =
        midi_problems.empty() ? SfzProblem(c.sfz, seqlib::midi::TrackNames(seqlib::midi::Read(c.midi))) : "";
    if (!midi_problems.empty() || !sf2_problems.empty() || !sfz_problem.empty())
    {
        std::fprintf(stderr, "%s: midi %s, sf2 %s, sfz %s\n", name.c_str(),
                     midi_problems.empty() ? "ok" : midi_problems[0].c_str(),
                     sf2_problems.empty() ? "ok" : sf2_problems[0].c_str(),
                     sfz_problem.empty() ? "ok" : sfz_problem.c_str());
        failures++;
    }

    const std::vector<double>& frames = c.performance.frame_ticks;
    for (std::size_t k = 0; k < frames.size(); k++)
    {
        if (frames[k] > static_cast<double>(k) || (k > 0 && frames[k] < frames[k - 1]))
        {
            std::fprintf(stderr, "%s: tick %zu's sound frame starts at tick %g\n", name.c_str(), k, frames[k]);
            failures++;
            break;
        }
    }

    return failures;
}

namespace sequence_export_check_detail
{

// CheckArchive for either reader's archive.
template <typename Archive>
int CheckEverySequence(const Archive& archive, const sequence_export::Settings& settings, const std::string& program)
{
    int failures = 0, sequences = 0, looping = 0;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const auto& s = archive.Sounds()[i];
        if (s.type != decltype(s.type)::kSequence || archive.FileMissing(s.file_id))
        {
            continue;
        }

        sequences++;
        sequence_export::Conversion c;
        try
        {
            c = sequence_export::ConvertSequence(archive, i, {}, settings, program);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", s.name.c_str(), e.what());
            failures++;
            continue;
        }

        failures += CheckConversion(c, s.name);
        if (c.performance.loop)
        {
            looping++;
        }
    }

    std::printf("archive: %d sequences, %d loop\n", sequences, looping);
    if (sequences == 0)
    {
        std::fprintf(stderr, "the archive has no sequences\n");
        failures++;
    }

    return failures;
}

} // namespace sequence_export_check_detail

// Converts every sequence of `archive` as `program` with `settings`, and checks each conversion (see CheckConversion).
// A sequence missing from a truncated archive can't be converted. The converter reports the missing sequence.
// CheckArchive leaves it out. Prints the number of sequences and of those that loop. Returns the number of failed
// checks. An archive with no sequences counts as one failed check.
inline int CheckArchive(const sar::SoundArchive& archive, const sequence_export::Settings& settings,
                        const std::string& program)
{
    return sequence_export_check_detail::CheckEverySequence(archive, settings, program);
}

// The same check for the Wii's `archive`. Its `settings` are those of the build of NW4R SND that plays it.
inline int CheckArchive(const rsar::SoundArchive& archive, const sequence_export::Settings& settings,
                        const std::string& program)
{
    return sequence_export_check_detail::CheckEverySequence(archive, settings, program);
}

// Reads the CSAR or FSAR archive at `path` with settings.archive, and checks it (see CheckArchive). An archive that
// can't be read is one failure.
inline int CheckArchiveFile(const std::string& path, const sequence_export::Settings& settings,
                            const std::string& program)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes) || bytes.empty())
    {
        std::fprintf(stderr, "can't read %s\n", path.c_str());
        return 1;
    }

    try
    {
        return CheckArchive(sar::SoundArchive::Load(std::move(bytes), settings.archive), settings, program);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
        return 1;
    }
}

// The main of a converter's archive check, <program>_test <archive><extension>. Checks the CSAR or FSAR archive that
// the one argument names, as `program` with `settings` (CheckArchiveFile). Returns main's exit code: 0 when every check
// passes, 1 when one fails, and 2 without the one argument.
inline int RunArchiveCheck(int argc, char** argv, const sequence_export::Settings& settings, const std::string& program,
                           const std::string& extension)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s_test <archive%s>\n", program.c_str(), extension.c_str());
        return 2;
    }

    const int failures = CheckArchiveFile(argv[1], settings, program);
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("all checks passed");

    return 0;
}

} // namespace ntdlib::testing
