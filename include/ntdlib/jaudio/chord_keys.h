// SPDX-License-Identifier: MIT

// The keys that Donkey Kong Jungle Beat's chord effects play: each part of an entry of the chord database (see
// chord_files.h) works out a key at each step of the effect from the chord of the moment. The game writes part k's key
// to port 0xb + k of the effect's sequence. The addresses are in the main.dol of the GameCube release (USA).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/jaudio/chord_files.h"

namespace ntdlib::jaudio
{

// The chord code's two random generators. Each steps as x * 0x19660d + 0x3c6ef35f and starts at 0: the first use seeds
// it with 0 (0x802c1320).
struct ChordRandom
{
    uint32_t draw = 0;    // the draw of a mode (0x8024ed84, r13-0x4d9c)
    uint32_t shuffle = 0; // the shuffles of the picks (0x8024f3a0, r13-0x4d94)
};

// What a part's key reads besides the part.
struct ChordMoment
{
    // The chord at the music's position, or nullptr if the position has no chord (0x8024ace4).
    const Chord* chord = nullptr;
    // The key that a pick takes when it finds none: the byte that +0xce0 of the game's object points to. That word is
    // +0x20 of the chord system at +0xcc0: the chord that a lookup last found (0x8024a8a8). So the key is the root of
    // the last chord that the music's position gave. A record's start clears the word (0x8024a808), and the game would
    // read address 0 before the first chord.
    uint8_t fallback_key = 0;
    // The byte that the record's third word points to, if it gives one. A pick of kind 7 takes it, or 60 without it.
    // Each step stores the parts' keys above 0 there.
    std::optional<uint8_t> shared_key;
    // Whether each key moves to the nearest tone of the chord, toward the shared key: the game does that for the
    // effects 0x50042 and 0x50045 unless +0x940 of the game's object is set and the chord hasn't moved since the
    // effect's last step (0x8024e07c).
    bool fit_to_shared = false;
};

// A part of an entry: its blocks of 21 bytes and its state (0x2c bytes from +8 of a player, 0x8024d754).
class ChordPart
{
public:
    // The block index that ends a part.
    static constexpr int kEnded = kChordNone;

    // Part `part` of the chord database `data`. The keys stay from the higher of the part's and the entry's lowest key
    // to the lower of their highest. Where one gives 0 for both, the other's hold (0x8024f240).
    ChordPart(std::span<const uint8_t> data, const ChordDatabase::Part& part, uint8_t entry_lowest,
              uint8_t entry_highest);

    // Starts the part at `block` (0x8024d80c, 0x8024de64).
    void Start(int block);

    // The part's key at a step (0x8024dfcc): -1 once the part has ended. `keys` gives every part's key so far, for a
    // block that takes another part's key.
    int Step(const ChordMoment& moment, std::span<const int> keys, ChordRandom& random);

    int Key() const
    {
        return key_;
    }

    int Block() const
    {
        return block_;
    }

    int Lowest() const
    {
        return lowest_;
    }

    int Highest() const
    {
        return highest_;
    }

private:
    std::span<const uint8_t> BlockAt(int block) const;
    int Base(std::span<const uint8_t> block, const ChordMoment& moment, std::span<const int> keys,
             ChordRandom& random) const;
    int Fold(std::span<const uint8_t> block, int key) const;
    void NextBlock(std::span<const uint8_t> block);

    std::span<const uint8_t> data_;
    ChordDatabase::Part part_;
    uint8_t last_block_ = 0; // byte 1 of the part: the count of blocks that a step to the next block passes
    int block_ = 0;
    int previous_ = 0;
    int key_ = 0;
    int lowest_ = 0;
    int highest_ = 0;
};

// The keys of a step for each part, in order (the loop of 0x8024da30): the key that the part writes to its port, or
// -1 for a part that writes none. Each key above 0 goes to `moment.shared_key` if the record gives one. `keys` holds
// each part's stored key (ChordPart::Key) and is updated as the parts run.
std::vector<int> StepChordParts(std::span<ChordPart> parts, ChordMoment& moment, ChordRandom& random,
                                std::span<int> keys);

// The key that a step works out from `base` (0x8024ed84). Exposed for tests.
int ChordKey(std::span<const uint8_t> block, int base, const ChordMoment& moment, ChordRandom& random);

} // namespace ntdlib::jaudio
