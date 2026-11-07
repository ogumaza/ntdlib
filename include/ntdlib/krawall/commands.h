// SPDX-License-Identifier: MIT

// The patterns of Krawall's modules in the common form of `ntdlib::commands`. A sequence holds a module's patterns: the
// bytes from its first pattern to the end of the pattern that starts last. In the games, the module's header follows.
// The patterns lie back to back, in the order of the module's table of patterns, each padded with bytes of 0 to a
// multiple of 4 bytes from the sequence's start. A pattern starts with an index of every 4th row's data and its row
// count (kPatternIndexEntries, kPatternRows). Its rows follow. A row lists the channels that it has data for, and a 0
// ends it.
//
// The items of a pattern:
//   kOther         the pattern's start, with kPatternOpcode in `encoding.opcode`. The encoder works the index and the
//                  row count out from the rows. In a pattern of fewer than 64 rows, the index's entries past the rows
//                  are spare. `bytes` holds them where they aren't 0, as the pattern holds them
//   kNote          a note, with the note's number from 1 for C-0 in `key` and its instrument in `sample`, from 1, or 0
//                  for none. A note has no velocity
//   kProgram       an instrument without a note, in `value`
//   kNoteOff       a note off (kNoteOff in the note's place), with its instrument in `sample`, or 0 for none
//   kVolume        the volume column's byte, in the module's codes
//   kOther         an effect, with kEffectOpcode plus the effect's number in `encoding.opcode` and its operand in
//                  `bytes`
//   kWait          the end of `value` rows: the last row that has items, and the empty rows after it
//
// Each item but a wait and a pattern's start names its channel in `index`. A row's items of one channel make one entry
// of the row, in the order note, volume and effect. A pattern's row count is the sum of its waits. Values stay in the
// module's units. A wait counts rows. The builds from 2004/09/17 on store an instrument in a third byte where it's
// above 255, and in a third byte of 0 where `encoding.value_bytes` is 3.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/krawall/driver.h"

namespace ntdlib::krawall
{

using ntdlib::agb::Rom;

// The opcodes of a pattern's start and of an effect, in `encoding.opcode` of a kOther item.
inline constexpr uint16_t kPatternOpcode = 0x200;
inline constexpr uint16_t kEffectOpcode = 0x100;

// What a build reads differently in a pattern.
struct Traits
{
    friend bool operator==(const Traits&, const Traits&) = default;

    // The builds from 2004/09/17 on: a row count of a halfword, the note in a byte's low 7 bits and an instrument of up
    // to 16 bits. The builds before them have a row count of a byte, the note in the top 7 bits and an instrument of up
    // to 9 bits.
    bool wide = false;
};

// The traits of the build that `info` describes.
Traits TraitsOf(const DriverInfo& info);

// What the decoder made of a sequence's bytes.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<uint32_t> entries; // the item where each pattern starts, in the order of the module's patterns

    // The patterns can't be read as the encoder would write them: a pattern that runs past the bytes or into the
    // next, an index that doesn't give the rows' data, padding that isn't the encoder's, or patterns out of order. The
    // decoder then keeps the whole sequence as data, split at the patterns' starts, and `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes `data`, a sequence's bytes from its start, with its patterns at the offsets `starts`. Throws
// std::invalid_argument for empty bytes or a start outside them.
Decoded DecodeSequence(std::span<const uint8_t> data, std::span<const uint32_t> starts, const Traits& traits);

// The sequences that a ROM's Krawall holds: one for each module of DriverInfo::modules.
int SequenceCount(const DriverInfo& info);

// The sequence that holds piece `song`: its module's. Throws std::out_of_range for a piece that the game lacks.
int SequenceOfSong(const DriverInfo& info, int song);

// The address of sequence `sequence`'s first byte: its module's first pattern, or 0 where the module's header can't be
// read. In the modified build, the address is in the unpacked modules (DriverInfo::unpacked). Throws std::out_of_range
// past SequenceCount().
uint32_t SequenceAddress(const Rom& rom, const DriverInfo& info, int sequence);

// Decodes sequence `sequence`: its module's patterns, from the first to the end of the pattern that starts last, with
// that pattern's padding. Where that pattern's rows don't read, the sequence ends at the module's header, if the header
// follows the patterns. In the modified build, the bytes come from the unpacked modules. Throws std::out_of_range past
// SequenceCount(), and std::invalid_argument for a module whose header can't be read, a pattern before the first or
// outside the ROM, and a last pattern that doesn't read without a header after it.
Decoded DecodeRomSequence(const Rom& rom, const DriverInfo& info, int sequence);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence`, with its patterns at the items `entries`. Throws std::invalid_argument for an entry that isn't a
// pattern's start, a value that the build's bytes can't hold, an item that a pattern can't hold, items before the
// first pattern, a row without its end, and an index entry past 65535 bytes.
Encoded EncodeSequence(const commands::Sequence& sequence, std::span<const uint32_t> entries, const Traits& traits);

// The offset of each item of `sequence` in its bytes, then the end. An item that starts an entry of a row starts at
// the entry's byte of flags. Throws the exceptions of EncodeSequence for the items.
std::vector<uint32_t> ItemOffsets(const commands::Sequence& sequence, const Traits& traits);

// The scale of the value of a command of `kind` (commands::UnitOf): the module's units, unchanged.
commands::Scale ScaleOf(commands::Kind kind);

} // namespace ntdlib::krawall
