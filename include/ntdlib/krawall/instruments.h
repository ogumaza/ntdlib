// SPDX-License-Identifier: MIT

// The instruments and samples that Krawall's modules play. Every sound is a sample of the game's table of samples:
// 8-bit PCM. A row of an S3M module names a sample by its number in that table, from 1. A row of an XM module names an
// instrument by its number from 1: an entry of the game's table of instruments, or in the modified build, of the
// module's instruments. An instrument maps each of its 96 notes to a sample of the game's table. The player reads
// instruments and samples through the unpacked copy of the ROM (DriverInfo::unpacked) where there is one.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"

namespace ntdlib::krawall
{

using ntdlib::agb::Rom;

// The notes that an instrument maps to samples, from C-0.
inline constexpr int kInstrumentNotes = 96;

// A sample of the game's table.
struct Sample
{
    uint32_t header = 0;      // its header (kSampleLoopLength and the rest)
    uint32_t data = 0;        // its first point: 8-bit, unsigned, with 0x80 as silence
    uint32_t end = 0;         // the address just past its last point
    uint8_t loop = 0;         // 0 none, 1 forwards, 2 back and forth
    uint32_t loop_length = 0; // the loop's points, which end at `end`
    int8_t fine_tune = 0;     // in 128ths of a semitone
    int8_t relative_note = 0; // the semitones that the player adds to a note
    uint8_t volume = 0;       // 0-64
    int8_t pan = 0;           // -64 to 64: the pan that a note of an XM instrument takes from the sample
    bool interpolate = false; // the mixer interpolates it in its quality mode 1

    // The rate in Hz of its C-4 with a relative note of 0, from the table of Amiga periods with its fine tune. A module
    // of linear frequencies works the rate out from its table of linear frequencies instead.
    uint32_t rate = 0;
};

// Reads sample `number` of the game's table, from 0. Gives nothing for a table that the game lacks, a header outside
// the ROM, or points that don't lie in it.
std::optional<Sample> ReadSample(const Rom& rom, const DriverInfo& info, int number);

// A run of an instrument's notes that play one sample.
struct Region
{
    friend bool operator==(const Region&, const Region&) = default;

    int first = 1;   // the run's first note, from 1 for C-0
    int last = 1;    // its last note
    int sample = -1; // the sample's number in the game's table
};

// An instrument that a module's rows name.
struct Instrument
{
    uint32_t address = 0;        // the XM instrument, or 0 where the instrument is a sample of an S3M module
    std::vector<Region> regions; // over the notes from 1 to kInstrumentNotes, in order
};

// Reads instrument `instrument`, from 1, as the rows of `module` name it. In a module without XM instruments, it's
// sample `instrument` - 1 of the game's table, in one region over every note. Gives nothing for 0, or where the game's
// tables or the module's instruments don't lie in the ROM.
std::optional<Instrument> ReadInstrument(const Rom& rom, const DriverInfo& info, const ModuleInfo& module,
                                         uint16_t instrument);

// What a row of `module` with note `note` (from 1 for C-0) and instrument `instrument` (from 1) plays, as the player
// picks it.
struct Lookup
{
    uint32_t instrument = 0; // the XM instrument, or 0 in a module without them
    int sample = -1;         // the sample's number in the game's table
    uint32_t header = 0;     // the sample's header
    int key = 0;             // the note that the sample plays, from 0 for C-0, after the sample's relative note
};

// Looks up `note` with `instrument` in `module`. An XM instrument takes the sample of the row's note, before the
// sample's relative note. Gives nothing for a note off, a note past kInstrumentNotes, or tables that don't lie in the
// ROM.
std::optional<Lookup> LookUpInstrument(const Rom& rom, const DriverInfo& info, const ModuleInfo& module,
                                       uint16_t instrument, uint8_t note);

} // namespace ntdlib::krawall
