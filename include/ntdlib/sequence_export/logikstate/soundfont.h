// SPDX-License-Identifier: MIT

// The SoundFont of the samples that a module's notes play.

#pragma once

#include <compare>
#include <cstdint>
#include <map>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/sequence_export/agb/sf2.h"

namespace ntdlib::sequence_export::logikstate
{

using ntdlib::agb::Rom;
using ntdlib::logikstate::Module;
using ntdlib::sequence_export::agb::Sf2File;

// The pitch that a sample without a fine tune plays at its SoundFont sample's rate, and the MIDI key that the SoundFont
// assigns to it: ProTracker's C-2, with the period 428. The table of a GBAMOD1 module starts a semitone below C-1, and
// has that step at kMod1RootPitch.
constexpr uint32_t kRootPitch = 200;
constexpr uint32_t kMod1RootPitch = 104;
constexpr int kRootKey = 60;

// A sample as a note plays it: the address of its first point, its length and loop as the channel has them, its fine
// tune, and the point that the note starts from. Two modules can set other lengths and loops for
// the points of one sample of a bank.
struct PlayedSample
{
    auto operator<=>(const PlayedSample&) const = default;

    uint32_t points = 0;
    uint32_t length = 0;
    uint32_t loop_start = 0;
    uint32_t loop_length = 0;
    uint32_t rate = 0;   // the rate in hertz of the sample's root key with its fine tune
    uint32_t offset = 0; // the point of the SoundFont sample that the note starts from
    bool loops = false;  // the mixer goes on to the loop at the sample's end
};

// Returns true if the mixer of a GBAMOD1 or GBAMOD2 module loops a sample with a loop of `loop_length` points.
bool Loops(uint32_t loop_length);

// Returns the count of the points of a sample's SoundFont sample before its loop. The mixer plays a sample to its end
// before it goes to the loop. A loop that ends before the sample does therefore follows the sample's points as a copy.
uint32_t LoopOffset(const PlayedSample& sample);

// Each instrument is a sample played from a given point. Instruments become SoundFont presets, numbered in order of
// first use. A shared SoundFont can use one set for all the game's modules.
class InstrumentSet
{
public:
    // Returns the program of `sample`, number `number` from 1 in its module, adding it if it's new. Programs from 128
    // on go in the banks after the first.
    int Program(const PlayedSample& sample, int number);

    // Returns the SoundFont of the instruments.
    Sf2File Build(const Rom& rom) const;

private:
    struct Instrument
    {
        int number = 0;
        int program = 0;
    };

    std::map<PlayedSample, Instrument> programs_;
};

// Returns the rate in hertz at which the player plays kRootPitch of a sample with fine tune `fine_tune` in `module`.
// The SoundFont sample has that rate, on key kRootKey. A GBAMOD1 module plays kMod1RootPitch at its
// step in 4096ths of a point, times the mixer's rate.
uint32_t SampleRate(const Rom& rom, const Module& module, uint16_t fine_tune);

} // namespace ntdlib::sequence_export::logikstate
