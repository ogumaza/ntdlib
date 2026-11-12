// SPDX-License-Identifier: MIT

// Locating Logik State's GBA sound driver (GBAModPlay) in a ROM: its code, the modules that it plays and the banks of
// samples that the game passes with them.

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/mod30_player.h"

namespace ntdlib::logikstate
{

using ntdlib::agb::Rom;

// A module starts with these two words, the text "GBAMOD2.". A module of the earlier format has kMod1Signature2 as its
// second word, the text "GBAMOD1.".
constexpr uint32_t kSignature1 = 0x4D414247;
constexpr uint32_t kSignature2 = 0x2E32444F;
constexpr uint32_t kMod1Signature2 = 0x2E31444F;

// The two formats of the driver's modules. Each build of the driver plays one of them.
enum class Format
{
    // "GBAMOD1.": 4 channels, a table of steps and no periods, byte-sized row counts, and the points after the
    // patterns. Pinball Challenge Deluxe uses it.
    kGbaMod1,
    // "GBAMOD2.": up to 8 channels, tables of steps and periods, and a bank of samples or the points after the
    // patterns. Aero the Acro-Bat and Zapper use it.
    kGbaMod2,
    // "GBAMOD30": up to 12 channels, packed patterns and a bank of 256 samples. The later builds of the driver play it
    // (mod30_player.h).
    kGbaMod30,
};

// The offsets in a module's header. Every field is a word unless its comment names another size.
constexpr uint32_t kModuleChannels = 0x08;
constexpr uint32_t kModulePatternBytes = 0x0C; // the size of the patterns after the header
constexpr uint32_t kModuleRate = 0x14;         // the mixer's rate in hertz
constexpr uint32_t kModuleOrders = 0x18;       // the count of orders
constexpr uint32_t kModuleSteps = 0x1C;        // a word a pitch: the mixer's step, in 4096ths of a point a sample
constexpr uint32_t kModulePeriods = 0xF5C;     // a halfword a pitch: the Amiga period
constexpr uint32_t kModuleSamples = 0x16FC;    // the headers of the samples
constexpr uint32_t kModulePatterns = 0x1870;   // the count of patterns
constexpr uint32_t kModuleOrderList = 0x1874;  // a byte an order: its pattern
constexpr uint32_t kModuleRows = 0x18F4;       // a word a pattern: its rows
constexpr uint32_t kModuleOwnSamples = 0x1AF4; // below 0 if the module's points follow its patterns
constexpr uint32_t kModuleSize = 0x1AF8;

// The offsets in a GBAMOD1 module's header. The rows are bytes, and the points always follow the patterns.
constexpr uint32_t kMod1PatternBytes = 0x08; // the size of the patterns after the header
constexpr uint32_t kMod1PointBytes = 0x0C;   // the size of the points after the patterns
constexpr uint32_t kMod1Rate = 0x10;
constexpr uint32_t kMod1Orders = 0x14;
constexpr uint32_t kMod1Steps = 0x18; // a word a pitch, like kModuleSteps
constexpr uint32_t kMod1Samples = 0x4B8;
constexpr uint32_t kMod1Patterns = 0x62C;
constexpr uint32_t kMod1OrderList = 0x630;
constexpr uint32_t kMod1Rows = 0x6B0; // a byte a pattern
constexpr uint32_t kMod1Size = 0x730;

// The pitches of a GBAMOD1 module's table of steps, 8 to a semitone from a semitone below ProTracker's C-1, and the
// channels of its rows.
constexpr int kMod1Pitches = 296;
constexpr uint32_t kMod1Channels = 4;

// The pitches of the two tables: 8 to a semitone, from a semitone below C-0. A sample's fine tune is a count of them.
constexpr int kPitches = 976;
constexpr int kSemitonePitches = 8;

// A module's limits: the samples, the orders and the patterns that the header has room for, the rows that the player
// plays of a pattern, and the mixer's channels. The mixer keeps its last two channels for the game's sound effects.
constexpr int kSamples = 31;
constexpr int kMaxOrders = 128;
constexpr int kMaxPatterns = 64;
constexpr int kPatternRows = 64;
constexpr int kMixChannels = 8;

// A sample's header, in a module or at the start of a bank: 12 bytes.
constexpr uint32_t kSampleLength = 0;     // a halfword: the points
constexpr uint32_t kSampleFineTune = 2;   // a halfword: the pitches that the sample plays above its notes
constexpr uint32_t kSampleVolume = 4;     // a halfword, up to 64
constexpr uint32_t kSampleLoopStart = 6;  // a halfword: the loop's first point
constexpr uint32_t kSampleLoopLength = 8; // a halfword: the loop's points, or up to 2 for no loop
constexpr uint32_t kSampleSize = 12;

// A bank of samples: the headers of kSamples samples, then the points of each in turn, signed 8-bit.
constexpr uint32_t kBankPoints = uint32_t(kSamples) * kSampleSize;

// A cell of a pattern, 4 bytes: a halfword with the pitch in its low 9 bits and the sample's number above them, then a
// halfword with the effect in its high byte and the operand in its low byte.
constexpr uint32_t kCellSize = 4;
constexpr uint16_t kCellPitch = 0x1FF;
constexpr uint16_t kNoPitch = 0x1FF; // the pitch of a cell without a note
constexpr int kCellSampleShift = 9;
constexpr uint16_t kCellSample = 0x1F;

// A sample as a module's header describes it.
struct Sample
{
    uint16_t length = 0;
    uint16_t fine_tune = 0;
    uint16_t volume = 0;
    uint16_t loop_start = 0;
    uint16_t loop_length = 0;
};

// A module's header.
struct Module
{
    Format format = Format::kGbaMod2;
    uint32_t address = 0;
    uint32_t steps = 0;  // the address of the table of steps
    uint32_t points = 0; // the address after the patterns, where the module's points start
    uint32_t channels = 0;
    uint32_t rate = 0;
    uint32_t order_count = 0;
    uint32_t pattern_count = 0;
    bool own_samples = false; // the header says that the module's points follow its patterns
    std::array<uint8_t, kMaxOrders> orders = {};
    std::array<uint32_t, kMaxPatterns> rows = {};     // each pattern's rows
    std::array<uint32_t, kMaxPatterns> patterns = {}; // each pattern's address
    std::array<Sample, kSamples> samples = {};
};

// The scale of the slide to a note in the GBAMOD1 build: the initial value of the variable that the game can change.
constexpr uint32_t kNoteSlideScale = 0x28;

// A piece of music: a module, the bank of samples that the game passes to the play routine with it, or 0 for the
// module's points, and the order that the game starts it at.
//
// The GBAMOD1 build's play routine also takes a range of orders. At a pattern's end without a jump, an order above the
// range's last goes to its first, and another order repeats. `first_order` and `last_order` contain the range, or -1
// for none: the order then moves on. The scale of the slide to a note is the variable that the game sets for the
// module. A piece that `plays_once` is a jingle: the game's jingle routine starts it, and its first jump returns to the
// piece that played before. The conversion ends the song where the row with that jump starts.
struct Song
{
    auto operator<=>(const Song&) const = default;

    uint32_t module = 0;
    uint32_t bank = 0;
    int order = 0;
    int first_order = -1;
    int last_order = -1;
    bool plays_once = false;
    uint32_t note_slide_scale = kNoteSlideScale;
    // The loop flag that the game plays a GBAMOD30 module with. Without it, the module stops after its last order.
    bool loop_flag = true;
};

// The full volume of a channel, and of the game's three volume settings: the master volume, the music's and the sound
// effects'.
constexpr int32_t kFullVolume = 0x40;

// The mixer's channels that the music's volume acts on in the build with short records. The sound effects' volume acts
// on the others.
constexpr int kMusicChannels = 6;

// The driver's builds that play GBAMOD2 modules. The size of a mixer channel's record tells them apart.
enum class Build
{
    // The build of Aero the Acro-Bat, with records of 0x4C bytes. The mixer leaves a channel at volume 0 alone, and its
    // sample waits. The music's volume acts on the first kMusicChannels channels. Effect E6 sets and clears one marker
    // for the module and never goes back to it.
    kShortRecords,
    // The build of Zapper, with records of 0x54 bytes. The mixer plays a channel at volume 0 too, and its sample moves
    // on. The music's volume acts on the module's channels. Effect E6 is ProTracker's pattern loop, with a row and a
    // count for each channel.
    kLongRecords,
};

// What detection found: the driver's routines and the game's music.
struct DriverInfo
{
    uint32_t load_routine = 0; // reads the module at r1 and the bank at r2 (Thumb)
    uint32_t play_routine = 0; // plays the module at r0 from order r1 with the bank at r2 (Thumb)
    // The modules' format. The GBAMOD1 build's play routine takes the range of orders in r2 and r3, and no bank.
    Format format = Format::kGbaMod2;
    Build build = Build::kShortRecords; // the build of the GBAMOD2 driver
    // The FIFOs that play the mix: 2 where the driver's SOUNDCNT_H sends the mix to FIFO A and FIFO B, and 1 where it
    // sends the mix to FIFO A alone. One FIFO plays the mix at half the level of two.
    int fifos = 2;
    uint32_t music_volume_routine = 0; // sets the music's volume to r0 (Thumb), or 0 if not found
    // The music's volume that the game sets when it starts, before its player changes it in the game's options: the
    // one constant above 0 that the game's code passes to the routine. It's kFullVolume if the code passes none, or
    // several.
    int32_t music_volume = kFullVolume;
    // A call from the game's code, outside the driver, that switches the sound off (SOUNDCNT_X), or 0: a call of the
    // driver's routine for that, or of a routine of the driver that calls it.
    // In mGBA 0.11.0, the switch clears the low byte of SOUNDCNT_H, and both FIFOs then play at half volume. The driver
    // sets SOUNDCNT_H only once, when it first starts its output. A game with such a call therefore plays its music
    // at half the level from the first call on.
    uint32_t sound_off_call = 0;
    std::vector<Song> songs; // in the order of their modules' addresses, or of the game's songs for GBAMOD30
    // What the game gives the driver that plays GBAMOD30 modules.
    Mod30Settings mod30;
    std::vector<std::string> log;
    std::vector<std::string> warnings;
};

// Finds the later builds of the driver for the games in the table of mod30_games.cpp. Those builds play GBAMOD30
// modules. Returns false and leaves `error` empty if the game has no GBAMOD30 module. Returns false and sets `error` if
// the game has modules and their bank or the game's settings can't be found.
bool DetectMod30(const Rom& rom, DriverInfo& info, std::string& error);

// Finds the driver in `rom` by its modules and its code. Returns false and leaves `error` empty if the game shows no
// sign of the driver. Returns false and sets `error` if the game has modules and their samples can't be found.
bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error);

// Reads the header of the module at `address`, in either format. Returns false if no module that the player can play
// starts there.
bool ReadModule(const Rom& rom, uint32_t address, Module& module);

// Returns the address of the first point of each sample of `song`, or 0 for a sample without points. The driver's load
// routine works the addresses out the same way. The points come from the bank, in the order of the bank's headers, or
// from the module's end, in the order of the module's headers.
std::array<uint32_t, kSamples> SamplePoints(const Rom& rom, const Module& module, const Song& song);

// A cell of a pattern.
struct Cell
{
    uint16_t pitch = kNoPitch;
    uint16_t sample = 0; // the sample's number from 1, or 0 for none
    uint16_t effect = 0; // the second halfword's high byte: the effect in its low 4 bits
    uint8_t operand = 0;
};

// Reads the cell at `address`.
Cell ReadCell(const Rom& rom, uint32_t address);

// Returns the mixer's samples a frame at `rate` hertz: the driver mixes this many at each of the game's calls.
uint32_t FrameSamples(uint32_t rate);

} // namespace ntdlib::logikstate
