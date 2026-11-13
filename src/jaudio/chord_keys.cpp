// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/chord_keys.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

namespace ntdlib::jaudio
{

namespace
{

constexpr std::size_t kBlockSize = 21;

// The bytes of a block.
constexpr std::size_t kKind = 0;
constexpr std::size_t kValue = 1;
constexpr std::size_t kMode = 2;
constexpr std::size_t kFit = 3;
constexpr std::size_t kTones = 4;
constexpr std::size_t kSteps = 5;
constexpr std::size_t kAdd = 6;
constexpr std::size_t kCountRepeats = 7;
constexpr std::size_t kAboveRange = 9;
constexpr std::size_t kBelowRange = 10;
constexpr std::size_t kNext = 12;
constexpr std::size_t kMasks = 13;
constexpr int kMaskCount = 8;

// The kinds of block (byte 0) whose base a pick gives (0x8024e298).
enum Kind : uint8_t
{
    kOwnKey = 0,
    kPartKey = 1,
    kPickTone = 2,
    kPickDegree = 3,
    kPickPitchClass = 4,
    kSilent = 5,
    kFixedKey = 6,
    kSharedKey = 7,
};

uint32_t NextRandom(uint32_t& x)
{
    x = x * 0x19660du + 0x3c6ef35fu;
    return x;
}

// The numbers 0 to n - 1 in an order that the shuffle generator gives (0x8024f3a0): n swaps of two places.
template <std::size_t N>
std::array<int, N> Shuffled(uint32_t& seed)
{
    std::array<int, N> order{};
    for (std::size_t i = 0; i < N; i++)
    {
        order[i] = static_cast<int>(i);
    }

    for (std::size_t i = 0; i < N; i++)
    {
        const uint32_t first = NextRandom(seed) >> 9;
        const uint32_t second = NextRandom(seed) >> 9;
        std::swap(order[first % N], order[second % N]);
    }

    return order;
}

uint8_t ScaleRootOf(const Chord& chord)
{
    return chord.scale_root != kChordNone ? chord.scale_root : chord.root;
}

// Tone `index` of a pick: tones 0 to 6, and the scale root for 7.
uint8_t PickTone(const Chord& chord, const ChordTones& tones, int index)
{
    return index == 7 ? ScaleRootOf(chord) : tones.pitch_classes[static_cast<std::size_t>(index)];
}

// Whether a tone matches a mask of a block: 0xff takes any tone. Bits 0 and 1 take the scale root and the root. Bits
// 2 to 7 take the tones whose intervals are 3 to 5, 6 to 8, 8 and 9, 10 and 11, 13 to 15, and 16 to 18 (0x8024e3f4,
// 0x8024e6d0).
bool Matches(uint8_t mask, uint8_t tone, const Chord& chord, const ChordTones& tones)
{
    if (mask == 0xff)
    {
        return true;
    }

    if (((mask & 1) != 0 && tone == ScaleRootOf(chord)) || ((mask & 2) != 0 && tone == chord.root))
    {
        return true;
    }

    constexpr std::array<std::pair<uint8_t, uint8_t>, 6> kIntervals = {
        {{3, 5}, {6, 8}, {8, 9}, {10, 11}, {13, 15}, {16, 18}}};
    const uint8_t interval = tones.IntervalOf(tone);
    for (std::size_t bit = 0; bit < kIntervals.size(); bit++)
    {
        if ((mask & (4u << bit)) != 0 && interval >= kIntervals[bit].first && interval <= kIntervals[bit].second)
        {
            return true;
        }
    }

    return false;
}

// A tone that the block's masks take, in an order that the shuffle generator gives (0x8024e3f4). Each mask in turn
// gets a new order. The root if no mask takes a tone.
int PickToneByMasks(std::span<const uint8_t> block, const Chord& chord, ChordRandom& random)
{
    const ChordTones tones = TonesOf(chord);
    for (int i = 0; i < kMaskCount; i++)
    {
        const uint8_t mask = block[kMasks + static_cast<std::size_t>(i)];
        for (const int index : Shuffled<8>(random.shuffle))
        {
            const uint8_t tone = PickTone(chord, tones, index);
            if (tone < kChordNone && Matches(mask, tone, chord, tones))
            {
                return tone;
            }
        }
    }

    return chord.root;
}

// The tone nearest above `pitch_class`, or below it, among those that the first mask that takes any tone takes
// (0x8024e6d0). The root if no mask takes a tone.
int NearestToneByMasks(std::span<const uint8_t> block, const Chord& chord, int pitch_class, bool upward)
{
    const ChordTones tones = TonesOf(chord);
    for (int i = 0; i < kMaskCount; i++)
    {
        const uint8_t mask = block[kMasks + static_cast<std::size_t>(i)];
        int nearest = kChordNone;
        int found = -1;
        for (int index = 0; index < 8; index++)
        {
            const uint8_t tone = PickTone(chord, tones, index);
            if (tone >= kChordNone || !Matches(mask, tone, chord, tones))
            {
                continue;
            }

            // The distance up or down to the tone, within an octave (0x8024eacc).
            const int distance = upward ? (tone < pitch_class ? tone + 12 : tone) - pitch_class
                                        : (tone > pitch_class ? pitch_class + 12 : pitch_class) - tone;
            if (distance < nearest)
            {
                nearest = distance;
                found = tone;
            }
        }

        if (found != -1)
        {
            return found;
        }
    }

    return chord.root;
}

// A degree of the chord's scale that a mask takes, in an order that the shuffle generator gives (0x8024eafc): 0x7e
// takes nothing, 0x7f any degree, a byte with bit 7 the degree that many scale steps above the root, and any other
// byte that degree. -1 if the chord has no scale or no mask takes a degree.
int PickDegree(std::span<const uint8_t> block, const Chord& chord, ChordRandom& random)
{
    if (!chord.scale)
    {
        return -1;
    }

    const std::array<uint8_t, 7>& scale = *chord.scale;
    for (const int degree : Shuffled<7>(random.shuffle))
    {
        const int pitch_class = ScaleDegree(scale, degree);
        for (int i = 0; i < kMaskCount; i++)
        {
            const uint8_t mask = block[kMasks + static_cast<std::size_t>(i)];
            if (mask == 0x7e)
            {
                continue;
            }

            if (mask == 0x7f)
            {
                return pitch_class;
            }

            // Jungle Beat's database gives no other byte above 6 here. The game would read past the scale.
            const int target = (mask & 0x80) != 0
                                   ? StepUpScale(scale, chord.root, mask & 0x7f, block[kCountRepeats] != 0, true)
                               : mask <= 6 ? ScaleDegree(scale, mask)
                                           : -2;
            if (pitch_class == target)
            {
                return pitch_class;
            }
        }
    }

    return -1;
}

// A pitch class that a mask takes, in an order that the shuffle generator gives (0x8024ec68): 0x7e takes nothing,
// 0x7f any pitch class, 0 the pitch class 0, and any other byte the pitch class that many semitones (bits 0 to 6)
// above the root. -1 if no mask takes one.
int PickPitchClass(std::span<const uint8_t> block, const Chord& chord, ChordRandom& random)
{
    for (const int pitch_class : Shuffled<12>(random.shuffle))
    {
        for (int i = 0; i < kMaskCount; i++)
        {
            const uint8_t mask = block[kMasks + static_cast<std::size_t>(i)];
            if (mask == 0x7e)
            {
                continue;
            }

            if (mask == 0x7f)
            {
                return pitch_class;
            }

            const int target = mask == 0 ? 0 : (chord.root + (mask & 0x7f)) % 12;
            if (pitch_class == target)
            {
                return pitch_class;
            }
        }
    }

    return -1;
}

// `key` raised by octaves until it reaches `limit` (the loops at 0x8024f030 and 0x8024f14c).
int RaiseTo(int key, int limit)
{
    while (key < limit)
    {
        key += 12;
    }

    return key;
}

} // namespace

ChordPart::ChordPart(std::span<const uint8_t> data, const ChordDatabase::Part& part, uint8_t entry_lowest,
                     uint8_t entry_highest)
    : data_(data), part_(part)
{
    const xsflib::Reader r(data, true);
    last_block_ = r.U8(part.offset + 1);
    const uint8_t part_lowest = r.U8(part.offset + 2);
    const uint8_t part_highest = r.U8(part.offset + 3);
    for (const uint32_t block : part.blocks)
    {
        r.Sub(block, kBlockSize);
    }

    if (entry_lowest == 0 && entry_highest == 0)
    {
        lowest_ = part_lowest;
        highest_ = part_highest;
    }
    else if (part_lowest == 0 && part_highest == 0)
    {
        lowest_ = entry_lowest;
        highest_ = entry_highest;
    }
    else
    {
        lowest_ = std::max(part_lowest, entry_lowest);
        highest_ = std::min(part_highest, entry_highest);
    }
}

void ChordPart::Start(int block)
{
    block_ = block;
}

std::span<const uint8_t> ChordPart::BlockAt(int block) const
{
    const auto index = static_cast<std::size_t>(block);
    if (index >= part_.blocks.size())
    {
        throw xsflib::FormatError("a chord part goes to a block it doesn't have");
    }

    return data_.subspan(part_.blocks[index], kBlockSize);
}

int ChordPart::Base(std::span<const uint8_t> block, const ChordMoment& moment, std::span<const int> keys,
                    ChordRandom& random) const
{
    int base = 0;
    bool fold = false;
    switch (block[kKind])
    {
    case kOwnKey:
        base = key_;
        break;
    case kPartKey:
        if (block[kValue] >= keys.size())
        {
            throw xsflib::FormatError("a chord part takes the key of a part that the entry doesn't have");
        }

        base = keys[block[kValue]];
        break;
    case kPickTone:
        base = moment.chord ? PickToneByMasks(block, *moment.chord, random) : -1;
        fold = true;
        break;
    case kPickDegree:
        base = moment.chord ? PickDegree(block, *moment.chord, random) : -1;
        fold = true;
        break;
    case kPickPitchClass:
        base = moment.chord ? PickPitchClass(block, *moment.chord, random) : -1;
        fold = true;
        break;
    case kSharedKey:
        base = moment.shared_key ? *moment.shared_key : 60;
        break;
    default:
        return 0;
    }

    if (base < 0)
    {
        base = moment.fallback_key;
    }

    return fold ? Fold(block, base) : base;
}

int ChordPart::Fold(std::span<const uint8_t> block, int key) const
{
    // 0x8024f2c8: a key below the range rises into it, then up to the highest octave in it (byte 1 of 0x7f) or by
    // byte 1's octaves. A key above the range falls into it.
    if (key < lowest_)
    {
        key = RaiseTo(key, lowest_);
        const uint8_t octaves = block[kValue];
        if (octaves == kChordNone)
        {
            if (key <= highest_)
            {
                key += (highest_ - key) / 12 * 12;
            }
            else
            {
                key -= 12;
            }
        }
        else
        {
            key += octaves * 12;
        }

        return std::min(key, highest_);
    }

    while (key > highest_)
    {
        key -= 12;
    }

    return std::max(key, lowest_);
}

void ChordPart::NextBlock(std::span<const uint8_t> block)
{
    // 0x8024e190.
    const uint8_t next = block[kNext];
    switch (next)
    {
    case 0xfc:
        {
            const uint8_t before = BlockAt(previous_)[kNext];
            block_ = before == 0xff ? kEnded : before == 0xfe ? previous_ + 1 : 0;
            break;
        }
    case 0xfd:
        block_ = previous_;
        break;
    case 0xfe:
        block_ = block_ < last_block_ - 1 ? block_ + 1 : kEnded;
        break;
    case 0xff:
        block_ = kEnded;
        break;
    default:
        block_ = next;
        break;
    }
}

int ChordPart::Step(const ChordMoment& moment, std::span<const int> keys, ChordRandom& random)
{
    if (block_ == kEnded)
    {
        return -1;
    }

    previous_ = block_;
    int base = 0;
    // A key out of the range can send the part to another block, twice at most. The key then stays as it was.
    for (int pass = 0; pass < 2; pass++)
    {
        const std::span<const uint8_t> block = BlockAt(block_);
        base = block[kKind] == kFixedKey ? block[kValue] : Base(block, moment, keys, random);
        if (base != 0)
        {
            int key = ChordKey(block, base, moment, random);
            if (moment.fit_to_shared && moment.chord && moment.shared_key)
            {
                key = FitToChord(*moment.chord, static_cast<uint8_t>(key), key < *moment.shared_key);
            }

            if (key > highest_)
            {
                if (block[kAboveRange] != kChordNone)
                {
                    previous_ = block_;
                    block_ = block[kAboveRange];
                    continue;
                }

                while (key > highest_)
                {
                    key -= 12;
                }
            }
            else if (key < lowest_)
            {
                if (block[kBelowRange] != kChordNone)
                {
                    previous_ = block_;
                    block_ = block[kBelowRange];
                    continue;
                }

                key = RaiseTo(key, lowest_);
            }

            key_ = key;
        }

        NextBlock(block);
        break;
    }

    return base == 0 ? 0 : key_;
}

int ChordKey(std::span<const uint8_t> block, int base, const ChordMoment& moment, ChordRandom& random)
{
    if (!moment.chord)
    {
        return base;
    }

    const Chord& chord = *moment.chord;
    const uint8_t mode_byte = block[kMode];
    int mode = mode_byte & 0xf;
    int key = base;
    if ((mode_byte & 0x80) != 0)
    {
        // Move to a tone that the masks take, in the base's octave.
        const int octave = base / 12;
        const int pitch_class = base % 12;
        auto below = [&]
        {
            const int tone = NearestToneByMasks(block, chord, pitch_class, false);
            return tone > pitch_class ? tone - 12 : tone;
        };
        auto above = [&]
        {
            const int tone = NearestToneByMasks(block, chord, pitch_class, true);
            return tone < pitch_class ? tone + 12 : tone;
        };
        int tone = 0;
        if (mode == 2)
        {
            tone = below();
        }
        else if (mode == 3)
        {
            const int down = below();
            const int up = above();
            if (up - pitch_class > pitch_class - down)
            {
                tone = down;
                mode = 2;
            }
            else
            {
                tone = up;
                mode = 1;
            }
        }
        else
        {
            tone = above();
        }

        key = tone + octave * 12;
        if (key < 0)
        {
            key += 12;
        }
    }
    else if (mode == 3)
    {
        mode = (NextRandom(random.draw) >> 9 & 1) != 0 ? 1 : 2;
    }

    const auto key8 = [](int value)
    {
        return static_cast<uint8_t>(value);
    };
    const auto signed_byte = [&](std::size_t at)
    {
        return static_cast<int>(static_cast<int8_t>(block[at]));
    };
    switch (mode)
    {
    case 0:
        // A fit by byte 3. Any other byte gives the base back, not the key that a move to a tone gave.
        switch (block[kFit])
        {
        case 1:
            return FitToChord(chord, key8(key), false);
        case 2:
            return FitToChord(chord, key8(key), true);
        case 3:
            return FitToScale(chord, key8(key), false);
        case 4:
            return FitToScale(chord, key8(key), true);
        default:
            return base;
        }
    case 1:
    case 2:
        {
            const bool up = mode == 1;
            const int tones = signed_byte(kTones);
            const int start = key + (up ? 1 : -1) * ((tones >> 6) & 3) * 12;
            int moved = start;
            if ((tones & 0x20) != 0)
            {
                const int tone = PickToneByMasks(block, chord, random);
                moved = tone < start ? RaiseTo(tone, start) : tone;
                if (!up)
                {
                    moved -= 12;
                }
            }

            const ChordTones chord_tones = TonesOf(chord);
            for (int i = 0; i < (tones & 0x1f); i++)
            {
                moved = chord_tones.Nearest(key8(moved), !up);
            }

            // Bits 6 and 7 of byte 5 leave the count. Bit 7 makes it negative, and no step runs.
            const uint8_t steps_byte = block[kSteps];
            const int steps = signed_byte(kSteps) & ~0xc0;
            const bool steps_up = up ? (steps_byte & 0x80) == 0 : (steps_byte & 0x40) != 0;
            for (int i = 0; i < steps; i++)
            {
                moved = steps_up ? FitToScale(Chord{.root = kChordNone, .scale = chord.scale}, key8(moved + 1), false)
                                 : FitToScale(Chord{.root = kChordNone, .scale = chord.scale}, key8(moved - 1), true);
            }

            moved += signed_byte(kAdd);
            if (moved == start)
            {
                return FitToScale(chord, key8(start), !up);
            }

            return moved;
        }
    default:
        return key;
    }
}

std::vector<int> StepChordParts(std::span<ChordPart> parts, ChordMoment& moment, ChordRandom& random,
                                std::span<int> keys)
{
    std::vector<int> written;
    for (std::size_t i = 0; i < parts.size(); i++)
    {
        const int key = parts[i].Step(moment, keys, random);
        keys[i] = parts[i].Key();
        written.push_back(key);
        if (key > 0 && moment.shared_key)
        {
            moment.shared_key = static_cast<uint8_t>(key);
        }
    }

    return written;
}

} // namespace ntdlib::jaudio
