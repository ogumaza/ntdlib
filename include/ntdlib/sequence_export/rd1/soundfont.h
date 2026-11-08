// SPDX-License-Identifier: MIT

// The SoundFont of Nintendo R&D1's driver: its instrument records as presets.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/instruments.h"
#include "ntdlib/sequence_export/agb/sf2.h"

namespace ntdlib::sequence_export::rd1
{

namespace sf2 = seqlib::sf2;
using ntdlib::agb::Rom;
using ntdlib::rd1::DriverInfo;
using ntdlib::rd1::InstrumentRecord;
using ntdlib::sequence_export::agb::Sf2File;
using ntdlib::sequence_export::agb::VolumeEnvelope;

// The PSG volume that a PSG zone plays at its full level. The driver's volume is (v >> 3) + (v >> 4), at most 15, for a
// voice's level v of at most 125 (0x080F28F8). A zone plays the straight line 3v/16 that the volume follows under its
// limit, up to v = 125. Each note's velocity scales the line down to the driver's limit and whole steps.
constexpr double kPsgFullVolume = 125 * 3 / 16.0;

// A sample zone releases over this many times the frames of the driver release. The driver's level falls in a straight
// line, and a SoundFont release falls in a straight line in decibels. FadeStretch() stretches a fade of 16 frames or
// more 6 times and a shorter fade less. A sample zone takes one stretch at every length.
constexpr double kSampleReleaseStretch = 7.0;

// Returns the volume envelope of record `r`'s zones. A note with a sweep keeps its volume to its end (Sweeps()). The
// release falls from `release_from`, 0 to 1, or from the sustain level when it's negative. A sample zone releases over
// kSampleReleaseStretch times the frames of the driver release, and a PSG zone over FadeSeconds() of them.
VolumeEnvelope ZoneEnvelope(const InstrumentRecord& r, double release_from = -1);

// The instruments that songs play, and the keys that they play on drum kits and key splits.
class InstrumentSet
{
public:
    // Keeps references to `rom` and `info`, which have to outlive the set.
    InstrumentSet(const Rom& rom, const DriverInfo& info);

    // Adds key `key` of program `program` of bank `bank`. `release_level` is the driver's level, 0 to 0x7F, at which a
    // note on the key starts its release, or -1 if the note ends before its release.
    void Add(int bank, int program, int key, int release_level);

    // Returns the level, 0 to 1, from which the sample zone of key `key` of program `program` of bank `bank` releases:
    // the mean level at which the notes added on its zone start their release. A drum kit and a key split have a zone
    // for each key, and other programs one for all keys. Returns -1 if none of the notes releases.
    double ReleaseFrom(int bank, int program, int key) const;

    // Returns a SoundFont with a preset for each program added: bank `bank`, program `program`.
    Sf2File Build() const;

private:
    const Rom& rom_;
    const DriverInfo& info_;
    std::map<std::pair<int, int>, std::set<int>> keys_;
    std::map<std::tuple<int, int, int>, std::pair<int64_t, int>> releases_; // the levels' sum and count on each key
};

} // namespace ntdlib::sequence_export::rd1
