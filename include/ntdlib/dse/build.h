// SPDX-License-Identifier: MIT

// The build of DSE's driver in a game's code. The driver runs each command of a track (codes 0x90 to 0xFF) through a
// table of 112 handlers, one ARM function's address for each code. The codes that the build doesn't support share one
// handler. A build is known by a fingerprint of that code: the table's shape (which codes share a handler), the words
// of each handler and of each function that it calls or branches to, on and on, and the words that their loads from
// literal pools read. The fingerprint leaves out what moves with the code: the offsets of branches and the words that
// point into the DS's memory. The same code at another address gives the same fingerprint. Code that differs in any
// other word gives another one.
//
// The builds that the model covers come from the ARM9 binaries of Pokémon Mystery Dungeon: Explorers of Time (the
// earlier build) and Explorers of Sky (the later build). Explorers of Darkness holds Time's code. A game's code is one
// of them when its fingerprint is that build's, and its sequencer, channels and voices are too: the words of a stretch
// at a fixed distance from the lowest handler, with the same parts left out. Soma Bringer runs the earlier build, and
// Professor Layton and the Unwound Future the later build, each with code that differs a little: build.cpp gives
// their fingerprints and cores too. Any other code is a build that the model doesn't cover, and the model's behaviour
// for it is unknown.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/dse/driver.h"
#include "ntdlib/ntr/rom.h"

namespace ntdlib::dse
{

// A part of a game's code as the console runs it: its address and its bytes, decompressed.
struct CodeBlock
{
    uint32_t address = 0;
    std::span<const uint8_t> bytes;
};

// A table of the driver's handlers in a game's code, and the fingerprint of the code that it reaches.
struct DriverCode
{
    uint32_t table = 0;    // the table's address
    std::size_t block = 0; // the index of the block that holds it
    uint32_t lowest_handler = 0;
    uint64_t fingerprint = 0;  // as the comment at the top of this file says
    std::size_t functions = 0; // the functions that the fingerprint covers
    std::size_t words = 0;     // their words
};

// Each place in `code` that holds a table of the driver's handlers: 112 words that point to word-aligned code in the
// same block, each at an instruction that always runs, where the codes that the build doesn't support share a handler
// (25 to 80 of them) that code 0xFF uses and code 0x98 doesn't, and at least 30 handlers differ. A function that a
// handler reaches is read in the block that branches to it, or else in the first block. The functions in other places,
// such as the ARM9's fast memory, count as one unknown function each.
std::vector<DriverCode> FindDriverCode(std::span<const CodeBlock> code);

// The build of the driver in `code`: the build that the model covers whose fingerprint and core a table of handlers
// gives (FindDriverCode). Nothing when no table gives one, or when tables give both.
std::optional<Build> BuildOf(std::span<const CodeBlock> code);

// The build of the driver in the code of cartridge image `rom`: its ARM9 binary and its ARM9 overlays, each
// decompressed (ntr::Arm9CodeAndOverlays). Nothing for a build that the model doesn't cover, or for an ARM9 binary
// that doesn't decompress.
std::optional<Build> BuildOf(const ntr::Rom& rom);

} // namespace ntdlib::dse
