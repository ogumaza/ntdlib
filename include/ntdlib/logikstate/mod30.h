// SPDX-License-Identifier: MIT

// The GBAMOD30 modules of Logik State's later GBA sound drivers (GBAModPlay of 2002 and 2003, and LS_Play of 2003):
// the three layouts of a module's header, the packed patterns, and the bank of samples that follows a game's last
// module. The drivers read the bank's address, the mixer's rate and the volumes from the game's code, not from a
// module.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"

namespace ntdlib::logikstate
{

using ntdlib::agb::Rom;

// A module starts with the text "GBAMOD30": these two words.
constexpr uint32_t kMod30Signature1 = 0x4D414247;
constexpr uint32_t kMod30Signature2 = 0x3033444F;

// The word of every layout that gives the module's channels.
constexpr uint32_t kMod30Channels = 0x28;

// The mixer's rate of rate index 0. Layouts A and C keep it at this offset. The drivers don't read it.
constexpr uint32_t kMod30RateWord = 0x34;
constexpr uint32_t kMod30Rate0 = 10512;

// The limits of a module: the orders, the patterns and the samples that the header has room for.
constexpr int kMod30Orders = 256;
constexpr int kMod30Patterns = 256;
constexpr int kMod30Samples = 256;

// The byte that ends a module's list of orders in layouts B and C.
constexpr uint8_t kMod30EndOfOrders = 0xFF;

// The three layouts of a module's header.
enum class Mod30Layout
{
    // GBAMOD2's header moved down by 0x20 bytes, with the tables of steps and periods still in it. The patterns'
    // sizes follow the list of orders.
    kA,
    // A compact header: the count of orders at 0x34, a map of the samples, and the patterns' offsets.
    kB,
    // Layout B with the rate kept at 0x34 and the map and the list of orders in the other order. The patterns' table
    // holds sizes, or in one build the offsets of layout B.
    kC,
};

// Where a layout keeps its fields. Each field is a word unless its comment says otherwise.
struct Mod30HeaderLayout
{
    uint32_t order_count = 0;   // the count of orders that the drivers compare with
    uint32_t sample_map = 0;    // a byte a sample slot (layouts B and C) or a word a sample (layout A)
    uint32_t pattern_count = 0; // the count of patterns
    uint32_t rows = 0;          // the rows of a pattern
    uint32_t amiga = 0;         // not 0 for Amiga periods, 0 for linear periods
    uint32_t tempo = 0;         // the starting tempo
    uint32_t speed = 0;         // the starting speed
    uint32_t orders = 0;        // a byte an order: its pattern
    uint32_t pattern_table = 0; // a word a pattern: its size, or its offset from `patterns`
    uint32_t patterns = 0;      // the patterns' data
};

// Returns where `layout` keeps its fields.
const Mod30HeaderLayout& HeaderLayout(Mod30Layout layout);

// A pattern is a block for each channel, in channel order. A block starts with five words. The drivers don't read the
// first. The second and the third give the offsets of the second and the third stream from the block's data. The
// fourth gives the end of the third stream, and the drivers don't read it. The fifth gives the size of the data, up to
// a multiple of 4. The three streams follow.
constexpr uint32_t kMod30BlockStreamB = 0x04;
constexpr uint32_t kMod30BlockStreamC = 0x08;
constexpr uint32_t kMod30BlockStreamEnd = 0x0C;
constexpr uint32_t kMod30BlockSize = 0x10;
constexpr uint32_t kMod30BlockHeader = 0x14;

// The first stream gives a halfword a row, high byte first: the note in its top 9 bits, and the sample from 1 below
// them. The second gives a byte a row: the volume column, the volume plus 1 or 0 for none. The third gives a halfword
// a row, high byte first: the effect and its operand. Each stream is a list of runs: a count, then the value of that
// many rows.
constexpr uint16_t kMod30NoNote = 0x1FF;
constexpr int kMod30NoteShift = 7;
constexpr uint16_t kMod30CellSample = 0x7F;

// A module's header as the drivers read it.
struct Mod30Module
{
    uint32_t address = 0;
    Mod30Layout layout = Mod30Layout::kB;
    bool pattern_offsets = false; // the patterns' table holds offsets, not sizes
    uint32_t channels = 0;
    uint32_t order_count = 0; // the header's count: 255 in some games' modules of layout B
    uint32_t real_orders = 0; // the orders up to the first kMod30EndOfOrders, or the header's count if smaller
    uint32_t pattern_count = 0;
    uint32_t rows = 0;
    uint32_t amiga = 0;
    uint32_t tempo = 0;
    uint32_t speed = 0;
    std::array<uint8_t, kMod30Orders> orders = {};
    // The address of each pattern's first block, as the load routine or the decoder works it out. A table of sizes
    // gives 0 to a pattern past the count.
    std::array<uint32_t, kMod30Patterns> patterns = {};
    uint32_t end = 0; // the address after the last block of the last pattern
};

// Returns the layout of the module header at `address`. The header starts with the signature. A or C if the word at
// 0x34 is the rate of rate index 0, C if the words at 0x13C and 0x140 then hold a count of patterns and of rows, and B
// otherwise.
Mod30Layout DetectLayout(const Rom& rom, uint32_t address);

// Reads the header of the module at `address`. Returns false if no module that a driver can play starts there.
bool ReadMod30Module(const Rom& rom, uint32_t address, Mod30Module& module);

// Returns the addresses of the image's modules, in the order of their addresses: the signature at a word's start, with
// a header that ReadMod30Module() reads.
std::vector<uint32_t> FindMod30Modules(const Rom& rom);

// A sample's header in a bank.
struct Mod30Sample
{
    uint32_t length = 0;      // in points; 0 for no sample
    int16_t fine_tune = 0;    // Amiga mode: added to the note; linear mode: half of it comes off the period
    uint16_t volume = 0;      // up to 64
    uint16_t loop_start = 0;  // in points
    uint16_t loop_length = 0; // in points; 0 for no loop
    int16_t relative_note = 0;
};

// A bank of samples: 256 headers, then the points of each sample in turn, signed 8-bit. The headers are 12 bytes, with
// a halfword for the length, or 16 bytes, with a word for it.
constexpr uint32_t kMod30ShortHeader = 12;
constexpr uint32_t kMod30LongHeader = 16;

struct Mod30Bank
{
    uint32_t address = 0;
    uint32_t header_size = kMod30ShortHeader;
    std::array<Mod30Sample, kMod30Samples> samples = {};
    std::array<uint32_t, kMod30Samples> points = {}; // each sample's first point, or 0 for a sample without points
    uint32_t end = 0;                                // the address after the last point
};

// Reads the bank at `address` with headers of `header_size` bytes. Returns false if its points don't fit in `rom`.
bool ReadMod30Bank(const Rom& rom, uint32_t address, uint32_t header_size, Mod30Bank& bank);

// Finds the bank that follows the last of `modules`, at the next word or the word after it, with headers of 16 bytes
// if they fit and of 12 otherwise. Returns false if neither fits.
bool FindMod30Bank(const Rom& rom, const std::vector<uint32_t>& modules, Mod30Bank& bank);

// The streams of one channel's block in a pattern, decoded row by row, as the check of a module's data uses them.
struct Mod30Cell
{
    uint16_t note = kMod30NoNote;
    uint8_t sample = 0; // from 1, or 0 for none
    uint8_t volume = 0; // the volume column: the volume plus 1, or 0 for none
    uint8_t effect = 0;
    uint8_t operand = 0;
};

// Decodes `rows` rows of each channel of the pattern whose first block is at `block`. Returns false and sets `error`
// if a stream leaves its block, or doesn't end with a count of 0 after its rows.
bool DecodeMod30Pattern(const Rom& rom, uint32_t block, uint32_t channels, uint32_t rows,
                        std::vector<std::vector<Mod30Cell>>& cells, std::string& error);

} // namespace ntdlib::logikstate
