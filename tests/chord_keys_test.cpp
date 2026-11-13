// SPDX-License-Identifier: MIT

// Unit tests for the keys of the chord effects (jaudio/chord_keys.h), with made-up blocks. The orders of the shuffles
// come from the generator x * 0x19660d + 0x3c6ef35f from 0: the first shuffle of 8 gives 2, 1, 0, 4, 3, 5, 7, 6, the
// first of 7 gives 0, 5, 2, 3, 4, 6, 1, and the first of 12 starts 3, 7, 6.

#include "ntdlib/jaudio/chord_keys.h"

#include <array>
#include <cstdint>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

using namespace ntdlib::jaudio;

namespace
{

constexpr uint8_t kNone = kChordNone;

// A block of 21 bytes: the kind, the value, the mode, the fit, the tones, the steps, the added semitones, the repeats
// flag, an unused byte, the blocks for keys above and below the range, an unused byte, the next block and 8 masks.
struct Block
{
    uint8_t kind = 6;
    uint8_t value = 60;
    uint8_t mode = 0;
    uint8_t fit = 0;
    uint8_t tones = 0;
    uint8_t steps = 0;
    uint8_t add = 0;
    uint8_t above = kNone;
    uint8_t below = kNone;
    uint8_t next = 0xfe;
    std::array<uint8_t, 8> masks = {0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e};

    std::vector<uint8_t> Bytes() const
    {
        std::vector<uint8_t> bytes = {kind, value, mode, fit, tones, steps, add, 0, 0, above, below, 0, next};
        bytes.insert(bytes.end(), masks.begin(), masks.end());
        return bytes;
    }
};

void Put32(std::vector<uint8_t>& data, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        data.push_back(static_cast<uint8_t>(value >> shift));
    }
}

// A chord database with one entry of one part that holds `blocks`. The part gives no range.
std::vector<uint8_t> MakeDatabase(const std::vector<Block>& blocks)
{
    std::vector<uint8_t> data;
    Put32(data, 0);
    data.insert(data.end(), {'C', 'D', 'S', 'E'});
    Put32(data, 1);
    const uint32_t entry = 0x14;
    const uint32_t steps = entry + 12;
    const uint32_t part = steps + 4;
    const uint32_t first_block = part + 4 + 4 * static_cast<uint32_t>(blocks.size());
    const uint32_t records = first_block + 21 * static_cast<uint32_t>(blocks.size());
    Put32(data, records);
    Put32(data, entry);
    data.insert(data.end(), {0, 1, 0, 0});
    Put32(data, steps);
    Put32(data, part);
    data.insert(data.end(), {1, 4, 0, 5});
    data.insert(data.end(), {static_cast<uint8_t>(blocks.size()), static_cast<uint8_t>(blocks.size()), 0, 0});
    for (std::size_t i = 0; i < blocks.size(); i++)
    {
        Put32(data, first_block + 21 * static_cast<uint32_t>(i));
    }

    for (const Block& block : blocks)
    {
        const std::vector<uint8_t> bytes = block.Bytes();
        data.insert(data.end(), bytes.begin(), bytes.end());
    }

    Put32(data, 0);
    return data;
}

// The part of a database made from `blocks`, with the range 48 to 72.
struct TestPart
{
    std::vector<uint8_t> data;
    ChordDatabase database;
    std::vector<ChordPart> parts;

    explicit TestPart(const std::vector<Block>& blocks, uint8_t lowest = 48, uint8_t highest = 72)
        : data(MakeDatabase(blocks)), database(ReadChordDatabase(data))
    {
        parts.emplace_back(data, database.entries[0].parts[0], lowest, highest);
    }

    int Step(const ChordMoment& moment, ChordRandom& random)
    {
        std::array<int, 1> keys = {parts[0].Key()};
        return parts[0].Step(moment, keys, random);
    }
};

Chord CMajor()
{
    return {.root = 0, .kind = 0, .scale = std::array<uint8_t, 7>{}};
}

int Key(const Block& block, int base, const Chord* chord, ChordRandom& random)
{
    const ChordMoment moment{.chord = chord};
    return ChordKey(block.Bytes(), base, moment, random);
}

int Key(const Block& block, int base)
{
    const Chord chord = CMajor();
    ChordRandom random;
    return Key(block, base, &chord, random);
}

void TestFits()
{
    XSFLIB_CHECK(Key({.fit = 1}, 62) == 64);
    XSFLIB_CHECK(Key({.fit = 2}, 62) == 60);
    XSFLIB_CHECK(Key({.fit = 3}, 61) == 62);
    XSFLIB_CHECK(Key({.fit = 4}, 61) == 60);
    XSFLIB_CHECK(Key({.fit = 0}, 61) == 61);
    ChordRandom random;
    XSFLIB_CHECK(Key({.fit = 1}, 62, nullptr, random) == 62);
}

void TestMoveToTone()
{
    // Above 62 (D), the nearest tone that mask 0xff takes is E. Mode 0 without a fit gives the base back.
    Block block{.mode = 0x80, .masks = {0xff, 0, 0, 0, 0, 0, 0, 0}};
    XSFLIB_CHECK(Key(block, 62) == 62);
    block.fit = 1;
    XSFLIB_CHECK(Key(block, 62) == 64);
    block.mode = 0x84;
    XSFLIB_CHECK(Key(block, 62) == 64);

    // Below D, C. Both are 2 semitones away for nibble 3, which then takes the tone above and plays mode 1.
    block.mode = 0x82;
    XSFLIB_CHECK(Key(block, 62) == 60);
    block.mode = 0x83;
    XSFLIB_CHECK(Key(block, 62) == 64);

    // Bit 3 of a mask takes the fifth.
    block.mode = 0x84;
    block.masks[0] = 0x08;
    XSFLIB_CHECK(Key(block, 62) == 67);

    // The first mask that takes a tone decides. Bit 1 takes the root.
    block.masks = {0, 0x02, 0x08, 0, 0, 0, 0, 0};
    XSFLIB_CHECK(Key(block, 62) == 72);
}

void TestModes()
{
    // Mode 1: an octave, a tone and two scale steps up from 60: 72, E, F, G.
    Block up{.mode = 1, .tones = 0x41, .steps = 0x02};
    XSFLIB_CHECK(Key(up, 60) == 79);
    // Back at the start after 7 semitones down: the start, fitted to the scale.
    up.add = static_cast<uint8_t>(-7);
    XSFLIB_CHECK(Key(up, 60) == 72);
    up.add = 0;
    // Bit 7 of byte 5 leaves a count below 0: no step.
    up.steps = 0x82;
    XSFLIB_CHECK(Key(up, 60) == 76);

    // Mode 2: an octave and a tone down from 60: 48, G. Bit 6 of byte 5 steps up the scale, to A. Without it, down to
    // F.
    Block down{.mode = 2, .tones = 0x41, .steps = 0x41};
    XSFLIB_CHECK(Key(down, 60) == 45);
    down.steps = 0x01;
    XSFLIB_CHECK(Key(down, 60) == 41);

    // Mode 3 draws mode 1 or 2. The first draw gives 0x3c6ef35f, whose bit 9 is set: mode 1. The fit then moves
    // C sharp up to D.
    const Chord chord = CMajor();
    ChordRandom random;
    XSFLIB_CHECK(Key({.mode = 3}, 61, &chord, random) == 62);
    XSFLIB_CHECK(random.draw == 0x3c6ef35f);
    // The second draw, 0x47502932, has bit 9 clear: mode 2, and the fit moves down to C.
    XSFLIB_CHECK(Key({.mode = 3}, 61, &chord, random) == 60);

    // Modes above 3 keep the key.
    XSFLIB_CHECK(Key({.mode = 5, .fit = 1}, 61) == 61);
}

void TestPicks()
{
    const Chord chord = CMajor();
    const ChordMoment moment{.chord = &chord};

    // Any tone: the shuffle's first tone is index 2, G. G (7) rises to 55, then to the highest octave in the range.
    {
        TestPart part({{.kind = 2, .value = kNone, .masks = {0xff, 0, 0, 0, 0, 0, 0, 0}}});
        ChordRandom random;
        XSFLIB_CHECK(part.Step(moment, random) == 67);
        XSFLIB_CHECK(random.shuffle == 0x83c6b450);
    }

    // The root: the shuffle reaches index 0 third. Byte 1 of 0 leaves it in the lowest octave of the range.
    {
        TestPart part({{.kind = 2, .value = 0, .masks = {0x02, 0, 0, 0, 0, 0, 0, 0}}});
        ChordRandom random;
        XSFLIB_CHECK(part.Step(moment, random) == 48);
    }

    // A degree of the scale: any degree takes the shuffle's first, C. Degree 4 is G. A degree one step above the
    // root is D.
    {
        TestPart part({{.kind = 3, .value = 0, .masks = {0x7f, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e}},
                       {.kind = 3, .value = 0, .masks = {0x04, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e}},
                       {.kind = 3, .value = 0, .masks = {0x81, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e}}});
        ChordRandom first;
        XSFLIB_CHECK(part.Step(moment, first) == 48);
        ChordRandom second;
        XSFLIB_CHECK(part.Step(moment, second) == 55);
        ChordRandom third;
        XSFLIB_CHECK(part.Step(moment, third) == 50);
    }

    // A pitch class: any takes the shuffle's first, 3. 7 semitones above the root takes G, the second.
    {
        TestPart part({{.kind = 4, .value = 0, .masks = {0x7f, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e}},
                       {.kind = 4, .value = 0, .masks = {0x07, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e, 0x7e}}});
        ChordRandom first;
        XSFLIB_CHECK(part.Step(moment, first) == 51);
        ChordRandom second;
        XSFLIB_CHECK(part.Step(moment, second) == 55);
    }

    // Without a chord, a pick takes the fallback key, and the keys pass through.
    {
        TestPart part({{.kind = 2, .value = 0, .masks = {0xff, 0, 0, 0, 0, 0, 0, 0}}});
        ChordRandom random;
        XSFLIB_CHECK(part.Step({.fallback_key = 62}, random) == 62);
        XSFLIB_CHECK(random.shuffle == 0);
    }

    // The shared key, or 60 without one.
    {
        TestPart part({{.kind = 7}, {.kind = 7}});
        ChordRandom random;
        XSFLIB_CHECK(part.Step(moment, random) == 60);
        XSFLIB_CHECK(part.Step({.chord = &chord, .shared_key = 64}, random) == 64);
    }
}

void TestBlocks()
{
    const Chord chord = CMajor();
    const ChordMoment moment{.chord = &chord};
    ChordRandom random;

    // Block 1 lies above the range and falls by an octave. Block 2 lies below it and sends the part back to block 0,
    // which then plays in the same step. Block 3 is silent: the key stays, and the step gives 0.
    TestPart part({{.value = 60, .next = 0xfe},
                   {.value = 74, .next = 0xfe},
                   {.value = 40, .below = 0, .next = 3},
                   {.kind = 5, .next = 0xff}});
    XSFLIB_CHECK(part.Step(moment, random) == 60);
    XSFLIB_CHECK(part.Step(moment, random) == 62);
    XSFLIB_CHECK(part.Step(moment, random) == 60);
    XSFLIB_CHECK(part.parts[0].Block() == 1);
    part.parts[0].Start(3);
    XSFLIB_CHECK(part.Step(moment, random) == 0);
    XSFLIB_CHECK(part.parts[0].Key() == 60);
    XSFLIB_CHECK(part.Step(moment, random) == -1);

    // Two jumps in a step keep the key, and the part stays at the second jump's block.
    TestPart loop({{.value = 60, .next = 1}, {.value = 40, .below = 2}, {.value = 30, .below = 1}});
    XSFLIB_CHECK(loop.Step(moment, random) == 60);
    XSFLIB_CHECK(loop.Step(moment, random) == 60);
    XSFLIB_CHECK(loop.parts[0].Block() == 1);

    // 0xfd returns to the block that the step started from. 0xfc reads the next byte of that block: after a jump,
    // the block before the jump. Its 0xfe goes on from that block. Without a jump, 0xfc reads itself and goes to block
    // 0.
    TestPart back({{.value = 60, .next = 1}, {.value = 62, .next = 0xfd}});
    XSFLIB_CHECK(back.Step(moment, random) == 60);
    XSFLIB_CHECK(back.Step(moment, random) == 62);
    XSFLIB_CHECK(back.parts[0].Block() == 1);
    TestPart follow(
        {{.value = 60, .next = 0xfe}, {.value = 40, .below = 2, .next = 0xfe}, {.value = 62, .next = 0xfc}});
    XSFLIB_CHECK(follow.Step(moment, random) == 60);
    XSFLIB_CHECK(follow.Step(moment, random) == 62);
    XSFLIB_CHECK(follow.parts[0].Block() == 2);
    TestPart self({{.value = 60, .next = 1}, {.value = 62, .next = 0xfc}});
    XSFLIB_CHECK(self.Step(moment, random) == 60);
    XSFLIB_CHECK(self.Step(moment, random) == 62);
    XSFLIB_CHECK(self.parts[0].Block() == 0);

    // 0xfe past the last block ends the part.
    TestPart last({{.value = 60, .next = 0xfe}});
    XSFLIB_CHECK(last.Step(moment, random) == 60);
    XSFLIB_CHECK(last.Step(moment, random) == -1);
}

void TestRanges()
{
    // The entry's range holds where the part gives none.
    TestPart part({{.value = 60}}, 50, 70);
    XSFLIB_CHECK(part.parts[0].Lowest() == 50 && part.parts[0].Highest() == 70);

    // A key below the range rises by octaves where byte 10 is kChordNone.
    TestPart low({{.value = 30}});
    ChordRandom random;
    XSFLIB_CHECK(low.Step({}, random) == 54);
}

void TestSharedKey()
{
    // Each key above 0 goes to the shared key, which the next part reads.
    const Chord chord = CMajor();
    std::vector<uint8_t> data = MakeDatabase({{.value = 64}});
    const ChordDatabase database = ReadChordDatabase(data);
    std::vector<ChordPart> parts;
    parts.emplace_back(data, database.entries[0].parts[0], 48, 72);
    ChordMoment moment{.chord = &chord, .shared_key = 60};
    ChordRandom random;
    std::vector<int> keys = {0};
    StepChordParts(parts, moment, random, keys);
    XSFLIB_CHECK(moment.shared_key == 64);
    XSFLIB_CHECK(keys[0] == 64);

    // A fit toward the shared key: E (64) is above D (62), and D sharp moves up to E. Below 62 it would move down.
    std::vector<uint8_t> fit_data = MakeDatabase({{.value = 63}, {.value = 63}});
    const ChordDatabase fit_database = ReadChordDatabase(fit_data);
    ChordPart fit(fit_data, fit_database.entries[0].parts[0], 48, 72);
    const std::array<int, 1> fit_keys = {0};
    XSFLIB_CHECK(fit.Step({.chord = &chord, .shared_key = 62, .fit_to_shared = true}, fit_keys, random) == 64);
    XSFLIB_CHECK(fit.Step({.chord = &chord, .shared_key = 66, .fit_to_shared = true}, fit_keys, random) == 60);
}

} // namespace

int main()
{
    TestFits();
    TestMoveToTone();
    TestModes();
    TestPicks();
    TestBlocks();
    TestRanges();
    TestSharedKey();

    return xsflib::testing::Summary("chord keys");
}
