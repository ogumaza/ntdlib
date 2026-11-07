// SPDX-License-Identifier: MIT

// The commands of Brownie Brown's GBA driver in the common form of `ntdlib::commands`, for both revisions of the
// driver.
//
// A sound is a list of channels in the ROM. Each channel takes three words: its number, the address of its data and the
// address of its table. A word of 0xFF ends the list. A channel's data is a run of commands, and F6 n calls and F8 n
// jumps to the address in word n of the channel's table. Each of these is an absolute address, and sounds share what
// they name: a later sound effect can play an earlier sound's channel. A sequence therefore holds a group of sounds,
// and not one sound: a region of the ROM from the first to the last byte that they read, where no other sound reads a
// byte (FindRegions). A region can hold one sound or hundreds. Labels name the items where the sounds' lists start.
//
// The decoder follows every channel of every sound in a region as the driver does. Each list's words, each command that
// a channel reaches and each word of a table that a command reads become items of the sequence. The bytes that nothing
// reads stay as data. The encoder writes the items back at an address, and gives the same bytes for a region that the
// decoder read.
//
// The driver counts time in frames: one tick is one frame of the driver's routine, at the GBA's 59.7275 Hz. A note, a
// rest, a hold and a release each wait for the channel's length, which a length byte chooses from the driver's table.
// The bytes hold no number of ticks, and the channel that reads them decides: the same bytes can run with other
// lengths. A note's and a wait's `value` therefore stay 0. brownie::Sequencer and the performance give the ticks.
//
// The kinds:
//   kOpenTrack  a list's channel: `index` is the channel and `target` the item where its data starts. It takes the
//               channel's word and the data's word. A word item with the address of the channel's table follows it
//   kJump       with `encoding.opcode` kAddressWord: a word that holds an address, in a list or in a table. `target` is
//               the item at that address. Where no item starts there, `target_outside` is set and `target` holds the
//               address. Without kAddressWord: F8. `index` is the word of the channel's table and `target` the item
//               that the word names
//   kCall       F6, with the same fields
//   kReturn     F7
//   kEnd        `index` 0: FF, the channel's end. `index` 1: a list's word of 0xFF, whose first byte can also serve a
//               channel as FF
//   kNote       `key` is the byte: a key before the channel's transpose, or a noise note's setting (NoiseNr43)
//   kWait       FE: the note goes on for the channel's length
//   kNoteOff    `index` 0: EC, a sample channel's rest. `index` 1: ED, a PSG channel's rest
//   kRelease    E0: the envelope goes on from the release envelope `value`, for the channel's length
//   kProgram    `index` 0: E1's sample set, or in the Magical Vacation revision its sample; 1: F0's envelope; 2: F1's
//               NRx1; 3: FC's wave
//   kPan        `index` 0: EA's byte, with the right side's level in the low nibble and the left's in the high. `index`
//               1: FD's byte, with the PSG channel's NR51 bits for the right side in the low nibble and the left in the
//               high
//   kVolume     F2's byte: the level that a PSG channel's envelope loses, or a sample channel's volume
//   kTranspose  F3's signed byte, in cents
//   kTune       F9's signed byte: an addition to a PSG channel's frequency setting, or 12ths of a semitone on a sample
//               channel of the Magical Vacation revision
//   kSweep      `index` 0: FA's NR10. `index` 1: FB, which turns the sweep off
//   kLoopStart  F4: `value` is the count, with 0 for 256
//   kLoopEnd    F5
//   kOther      the opcode in `encoding.opcode` and its argument in `bytes`: a length byte (80-DF), the Sword of Mana
//               revision's E2 (the mixer's mode), E3 (the sound comes back after a fade, and starts sound 0x36), E8 and
//               E9 (a sound effect gives the music's PSG channel back, or takes it), EE (the set of lengths), and each
//               command that does nothing
//
// In the Magical Vacation revision, E2-E9 are F2-F9 again and EF is FF. A command that a channel reads through such a
// byte holds kKeptBytes plus the byte in `encoding.opcode`, and the encoder writes the byte.
//
// A word of a list or of a table has to start on a multiple of 4 in the ROM, for the CPU reads it as a word. The
// encoder puts zeros before one that would start elsewhere.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"

namespace ntdlib::brownie
{

// The flag in `encoding.opcode` of an item that keeps its command's byte.
inline constexpr uint16_t kKeptBytes = 0x100;

// `encoding.opcode` of a kJump that is a word with an address.
inline constexpr uint16_t kAddressWord = 0x200;

// The bytes of a ROM that a group of sounds read: their lists, the commands that their channels reach and the words of
// the tables that those commands read, with the bytes between them. No other sound reads a byte here.
struct Region
{
    friend bool operator==(const Region&, const Region&) = default;

    uint32_t start = 0; // an address
    uint32_t size = 0;
    std::vector<int> sounds; // in the order of their numbers
};

// Finds the regions of `rom`, in the order of their bytes. A sound that ReadSong doesn't read, or whose channels the
// decoder can't follow, lies in no region: a table that doesn't start on a word, or a channel that runs past the ROM.
std::vector<Region> FindRegions(const Rom& rom, const DriverInfo& info);

// What the decoder made of a region.
struct Decoded
{
    commands::Sequence sequence;
    std::vector<int> sounds;     // the region's sounds
    std::vector<uint32_t> lists; // the item where each of them starts its list

    // The region can't be read safely: bytes that read both as a command and as something else, a call that two
    // channels read with other targets, or a table's start that falls inside another item. The decoder then keeps the
    // whole region as data, and `reason` says why.
    bool opaque = false;
    std::string reason;
};

// Decodes `region`.
Decoded DecodeRegion(const Rom& rom, const DriverInfo& info, const Region& region);

// Decodes the region that holds sound `song`. Throws std::invalid_argument for a sound that lies in no region.
Decoded DecodeSong(const Rom& rom, const DriverInfo& info, int song);

// A channel of a list.
struct ListTrack
{
    int channel = 0;
    uint32_t entry = 0; // the item where the channel's data starts
    uint32_t table = 0; // the word item with the address of the channel's table, after the list's kOpenTrack
};

// The channels of the list that starts at item `list`. Throws std::invalid_argument for a list that isn't one: a run of
// kOpenTrack, each with its word, up to a kEnd of `index` 1.
std::vector<ListTrack> TracksOfList(const commands::Sequence& sequence, uint32_t list);

// The bytes of a sequence, and each item's offset in them. `offsets` holds one more entry than the sequence has items:
// the end.
struct Encoded
{
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;
};

// Encodes `sequence` at the address `base`. Throws std::invalid_argument for a command that the revision's bytes can't
// hold, for a target that the sequence lacks, and for a sequence that the driver wouldn't read as its items say: a
// channel that reaches a word or data, that runs past the items, or whose call or jump finds another target in its
// table.
Encoded EncodeSequence(const commands::Sequence& sequence, Revision revision, uint32_t base);

// The scale of the value of a command of `kind` in revision `revision` (commands::UnitOf). A transpose takes 100 cents
// to the driver's step. The other values stay in the driver's units.
commands::Scale ScaleOf(commands::Kind kind, Revision revision);

} // namespace ntdlib::brownie
