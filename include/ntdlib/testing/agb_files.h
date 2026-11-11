// SPDX-License-Identifier: MIT

// Helpers for the tests of the GBA's conversions: the settings of a conversion into the tests' temporary folder, and
// readers of the MIDI files and SoundFonts that it writes.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/read_error.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"

#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::agb_files
{
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::sequence_export::agb::kLoopGuard;
using ntdlib::sequence_export::agb::kMinLoop;

// Returns the default settings for a conversion that writes its files to this run's temp folder. The files' names start
// with `base_name`.
inline ConvertSettings TempSettings(const std::string& base_name);

// A MIDI file's header and its tracks' events, and a SoundFont's chunks, as seqlib's readers give them.
using MidiEvents = seqlib::midi::File;
using Sf2Records = seqlib::sf2::Records;

// Reads a MIDI file with seqlib's reader. A file that seqlib can't read gives no tracks, after a message that says why.
inline MidiEvents ReadMidi(const std::string& path);

// Returns true if the first track has a loopStart marker and, at the marker's tick and after it, a tempo change to the
// tempo in effect there.
inline bool RepeatsTempoAtLoopStart(const MidiEvents& midi);

// Reads a SoundFont's chunks with seqlib's reader. A file that seqlib can't read gives no chunks, after a message that
// says why.
inline Sf2Records ReadSf2(const std::string& path);

// Returns true if the SoundFont has an instrument and every instrument starts with a global zone that holds the
// modulators of seqlib's kGlobalZoneModulators. The pitch wheel then covers the whole range that RPN 0 sets. CC10
// gets the pan range that most synthesizers give it.
inline bool HasGlobalZoneModulators(const Sf2Records& sf);

// Returns true if `s` loops from point `loop_start` over `length` points, repeated as many times as it takes to reach
// kMinLoop points, and its data ends kLoopGuard points after the loop. Every point after the first pass repeats the
// point `length` before it.
inline bool LengthenedLoop(const seqlib::sf2::Sample& s, uint32_t loop_start, uint32_t length);

inline ConvertSettings TempSettings(const std::string& base_name)
{
    ConvertSettings settings;
    settings.out_dir = Utf8(TempFolderPath());
    settings.base_name = base_name;

    return settings;
}

inline MidiEvents ReadMidi(const std::string& path)
{
    try
    {
        return seqlib::midi::Read(ReadAll(path));
    }
    catch (const seqlib::ReadError& e)
    {
        std::printf("can't read %s: %s\n", path.c_str(), e.what());
        return {};
    }
}

inline bool RepeatsTempoAtLoopStart(const MidiEvents& midi)
{
    if (midi.tracks.empty())
    {
        return false;
    }

    // Follow the tempo up to the marker, and then look for a tempo change after it at the same tick.
    std::vector<uint8_t> tempo;
    std::optional<uint64_t> marker;
    for (const auto& [tick, bytes] : midi.tracks[0].events)
    {
        if (marker && tick != *marker)
        {
            break;
        }

        const bool is_tempo = bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51;
        if (marker && is_tempo)
        {
            return bytes == tempo;
        }
        if (is_tempo)
        {
            tempo = bytes;
        }
        else if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0x06 &&
                 std::string(bytes.begin() + 3, bytes.end()) == "loopStart")
        {
            marker = tick;
        }
    }

    return false;
}

inline Sf2Records ReadSf2(const std::string& path)
{
    try
    {
        return seqlib::sf2::Read(ReadAll(path));
    }
    catch (const seqlib::ReadError& e)
    {
        std::printf("can't read %s: %s\n", path.c_str(), e.what());
        return {};
    }
}

inline bool HasGlobalZoneModulators(const Sf2Records& sf)
{
    const size_t records = sf.Count("inst", 22);
    if (records < 2)
    {
        return false;
    }

    // The last instrument record only ends the list.
    for (size_t i = 0; i + 1 < records; i++)
    {
        const uint16_t bag = Le16(sf.Record("inst", 22, i) + 20);
        if (bag >= Le16(sf.Record("inst", 22, i + 1) + 20) || sf.ZoneGens(bag).count(seqlib::sf2::kSampleId))
        {
            return false;
        }

        const uint16_t first = Le16(sf.Record("ibag", 4, bag) + 2);
        const uint16_t end = Le16(sf.Record("ibag", 4, bag + 1) + 2);
        for (const seqlib::sf2::Modulator& wanted : seqlib::sf2::kGlobalZoneModulators)
        {
            bool found = false;
            for (uint16_t m = first; m < end; m++)
            {
                const uint8_t* mod = sf.Record("imod", 10, m);
                found = found || (Le16(mod) == wanted.source && Le16(mod + 2) == wanted.dest &&
                                  int16_t(Le16(mod + 4)) == wanted.amount && Le16(mod + 6) == wanted.amount_source &&
                                  Le16(mod + 8) == wanted.transform);
            }
            if (!found)
            {
                return false;
            }
        }
    }

    return true;
}

inline bool LengthenedLoop(const seqlib::sf2::Sample& s, uint32_t loop_start, uint32_t length)
{
    if (length == 0)
    {
        return false;
    }

    const uint32_t repeats = (kMinLoop + length - 1) / length;
    if (s.loop_start != loop_start || s.loop_end != loop_start + repeats * length ||
        s.data.size() != size_t(s.loop_end) + kLoopGuard)
    {
        return false;
    }
    for (size_t i = size_t(loop_start) + length; i < s.data.size(); i++)
    {
        if (s.data[i] != s.data[i - length])
        {
            return false;
        }
    }

    return true;
}

} // namespace ntdlib::testing::agb_files
