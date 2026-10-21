// SPDX-License-Identifier: MIT

// Copies of instruments from one bank file into another. A sound that takes another bank can still select programs
// that only its old bank holds. Each such program's instrument can then go into the new bank.
//
// A bank file of the 3DS (CBNK), the Wii U or the Switch (FBNK) holds one block, INFO. Its body starts with two
// references: to the table of waves and to the table of instruments. The table of waves holds a wave archive's item ID
// and a wave's index for each wave. The table of instruments holds a reference for each program, to the program's
// instrument or to nothing. An instrument holds a reference to its table of key regions. A key region holds a reference
// to its table of velocity regions. A velocity region holds its wave's index in the table of waves and its optional
// values. The value of its envelope leads to a reference to the envelope's 5 bytes. NintendoWare's tools lay each
// instrument out in one run: the instrument, its table, then each key region in turn. A key region is followed by its
// table, then by each velocity region with the reference to its envelope and the envelope, padded to 4 bytes. Several
// keys of a table can name one key region. That key region goes in once: at the first key that names it. The runs
// follow the table of instruments. The table of waves ends the block and the file.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

// An instrument to copy into a bank: the bank file that holds it, its program there and its program in the bank.
struct InstrumentCopy
{
    std::span<const uint8_t> from;
    uint32_t program = 0;
    uint32_t to = 0;
};

// `bank`, a bank file of `flavor`, with the instruments of `copies`. Each copy takes the whole instrument, byte for
// byte: its key regions, its velocity regions and their envelopes, with every byte that the readers skip. Only the
// offsets and the waves' indices change. Each wave of a copy takes the first entry of the bank's table of waves that
// names the same wave archive and wave, or else a new entry after the table's last. The copies go after the bank's last
// instrument, in the order of their programs. Each is laid out as the tools lay an instrument out. A program past the
// end of the table of instruments makes the table longer, with references to nothing before it. The bank's other bytes
// stay. Only the offsets across the new bytes, the counts of the two tables and the sizes of INFO and the file change.
// With no copies, gives `bank` back unchanged.
//
// Throws std::invalid_argument for a bank file of another flavor, for a program that the bank or another copy takes
// already, for a program past 0xFFFF and for a program without an instrument in its source. A sequence's program change
// selects programs up to 0xFFFF. Throws xsflib::FormatError for a damaged bank file, and for unknown options of a
// velocity region.
std::vector<uint8_t> WithInstruments(std::span<const uint8_t> bank, Flavor flavor,
                                     std::span<const InstrumentCopy> copies);

} // namespace ntdlib::sar
