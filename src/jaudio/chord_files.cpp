// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/chord_files.h"

#include <algorithm>

#include "xsflib/binary.h"

namespace ntdlib::jaudio
{

namespace
{

constexpr std::size_t kChordSize = 16;
constexpr std::size_t kRecordSize = 16;

// The pitch classes of C major's degrees (0x8024b99c).
constexpr std::array<int, 7> kMajorDegrees = {0, 2, 4, 5, 7, 9, 11};

bool HasTag(std::span<const uint8_t> data, std::size_t at, const char* tag)
{
    return data.size() >= at + 4 && std::equal(tag, tag + 4, data.begin() + static_cast<std::ptrdiff_t>(at));
}

Chord ReadChord(const xsflib::Reader& r, std::size_t at, bool has_scale)
{
    Chord chord{
        .root = r.U8(at),
        .kind = r.U8(at + 1),
        .seventh = r.U8(at + 2),
        .added = {r.U8(at + 3), r.U8(at + 4), r.U8(at + 5)},
        .fifth = r.U8(at + 6),
        .scale_root = r.U8(at + 7),
        .no_third = r.U8(at + 8),
    };
    if (has_scale)
    {
        const std::size_t scale = r.U32(at + 12);
        std::array<uint8_t, 7> codes{};
        for (std::size_t i = 0; i < codes.size(); i++)
        {
            codes[i] = r.U8(scale + i);
        }

        chord.scale = codes;
    }

    return chord;
}

// Sets tone `index` to the pitch class of `key` and `interval` (0x8024b360). kChordNone stays as it is.
void SetTone(ChordTones& tones, std::size_t index, int key, uint8_t interval)
{
    const auto key8 = static_cast<uint8_t>(key);
    tones.pitch_classes[index] = key8 == kChordNone ? key8 : static_cast<uint8_t>(key8 % 12);
    tones.intervals[index] = interval;
}

// Adds a tone before the first tone with a higher pitch class (0x8024b500). The last tone drops out. Nothing changes if
// no tone has a higher pitch class.
void InsertTone(ChordTones& tones, uint8_t pitch_class, uint8_t interval)
{
    std::size_t at = 0;
    while (at < tones.pitch_classes.size() && tones.pitch_classes[at] <= pitch_class)
    {
        at++;
    }

    if (at == tones.pitch_classes.size())
    {
        return;
    }

    for (std::size_t i = tones.pitch_classes.size() - 1; i > at; i--)
    {
        tones.pitch_classes[i] = tones.pitch_classes[i - 1];
        tones.intervals[i] = tones.intervals[i - 1];
    }

    tones.pitch_classes[at] = pitch_class;
    tones.intervals[at] = interval;
}

// The pitch class that a fit keeps: byte 7 of the chord, or the root without it.
uint8_t ScaleRoot(const Chord& chord)
{
    return chord.scale_root != kChordNone ? chord.scale_root : chord.root;
}

} // namespace

std::optional<uint8_t> ChordFile::ChordAt(std::size_t order, std::size_t position) const
{
    if (order >= orders.size() || position >= orders[order].size())
    {
        return std::nullopt;
    }

    return orders[order][position];
}

ChordFile ReadChordFile(std::span<const uint8_t> data)
{
    ChordFile file;
    if (HasTag(data, 0, "CITS"))
    {
        file.has_scales = true;
    }
    else if (!HasTag(data, 0, "CITL"))
    {
        throw xsflib::FormatError("not a chord file");
    }

    const xsflib::Reader r(data, true);
    const uint16_t order_count = r.U16(8);
    const uint16_t chord_count = r.U16(10);
    for (std::size_t i = 0; i < order_count; i++)
    {
        const std::size_t at = r.U32(12 + 4 * i);
        const uint8_t count = r.U8(at);
        const std::span<const uint8_t> indices = r.Sub(at + 1, count);
        file.orders.emplace_back(indices.begin(), indices.end());
    }

    for (std::size_t i = 0; i < chord_count; i++)
    {
        const std::size_t at = r.U32(12 + 4 * (order_count + i));
        r.Sub(at, kChordSize);
        file.chords.push_back(ReadChord(r, at, file.has_scales));
    }

    return file;
}

uint8_t ChordTones::IntervalOf(uint8_t pitch_class) const
{
    for (std::size_t i = 0; i < pitch_classes.size(); i++)
    {
        if (pitch_classes[i] == pitch_class)
        {
            return intervals[i];
        }
    }

    return kChordNone;
}

bool ChordTones::Has(uint8_t key) const
{
    const auto pitch_class = static_cast<uint8_t>(key % 12);
    return std::ranges::find(pitch_classes, pitch_class) != pitch_classes.end();
}

uint8_t ChordTones::Nearest(uint8_t key, bool downward) const
{
    int at = key;
    for (int i = 0; i < 12; i++)
    {
        const auto key8 = static_cast<uint8_t>(at);
        if (downward ? key8 <= 12 : key8 >= 120)
        {
            return kChordNone;
        }

        at += downward ? -1 : 1;
        if (Has(static_cast<uint8_t>(at)))
        {
            return static_cast<uint8_t>(at);
        }
    }

    return kChordNone;
}

uint8_t FitToChord(const Chord& chord, uint8_t key, bool downward)
{
    const ChordTones tones = TonesOf(chord);
    if (key % 12 == ScaleRoot(chord) || tones.Has(key))
    {
        return key;
    }

    return tones.Nearest(key, downward);
}

ChordTones TonesOf(const Chord& chord)
{
    ChordTones tones;
    tones.pitch_classes.fill(kChordNone);
    tones.intervals.fill(kChordNone);
    if (chord.root == kChordNone)
    {
        return tones;
    }

    // The root keeps its byte as it is.
    tones.pitch_classes[0] = chord.root;
    tones.intervals[0] = 0;
    std::size_t next = 1;
    auto add = [&](uint8_t interval)
    {
        SetTone(tones, next++, chord.root + interval, interval);
    };
    auto add_third = [&](uint8_t interval)
    {
        if (chord.no_third == 0)
        {
            add(interval);
        }
    };
    auto add_fifth = [&]
    {
        switch (chord.fifth)
        {
        case 1:
            add(8);
            break;
        case 2:
            add(6);
            break;
        case kChordNone:
            break;
        default:
            add(7);
            break;
        }
    };

    switch (static_cast<ChordKind>(chord.kind))
    {
    case ChordKind::kMajor:
        add_third(4);
        add_fifth();
        break;
    case ChordKind::kMinor:
        add_third(3);
        add_fifth();
        break;
    case ChordKind::kSuspended:
        add_third(5);
        add_fifth();
        break;
    case ChordKind::kDiminishedSeventh:
        add_third(3);
        add(6);
        add(9);
        break;
    case ChordKind::kAugmented:
        add_third(4);
        add(8);
        break;
    default:
        tones.pitch_classes.fill(kChordNone);
        tones.intervals.fill(kChordNone);
        return tones;
    }

    // The seventh takes the next tone and leaves it free for the added tones.
    if (chord.seventh == 1)
    {
        SetTone(tones, next, chord.root + 10, 10);
    }
    else if (chord.seventh == 2)
    {
        SetTone(tones, next, chord.root + 11, 11);
    }

    for (const uint8_t added : chord.added)
    {
        if (added < kChordNone)
        {
            InsertTone(tones, static_cast<uint8_t>((added + chord.root) % 12), added);
        }
    }

    return tones;
}

int ScaleDegree(const std::array<uint8_t, 7>& scale, int degree)
{
    int pitch_class = kMajorDegrees[static_cast<std::size_t>(degree)];
    switch (scale[static_cast<std::size_t>(degree)])
    {
    case 1:
        pitch_class += 1;
        break;
    case 2:
        pitch_class -= 1;
        break;
    case 3:
        pitch_class += 2;
        break;
    case 4:
        pitch_class -= 2;
        break;
    case kChordNone:
        return -1;
    default:
        break;
    }

    if (pitch_class > 12)
    {
        pitch_class -= 12;
    }
    else if (pitch_class < 0)
    {
        pitch_class += 12;
    }

    return pitch_class;
}

bool InScale(const std::array<uint8_t, 7>& scale, int key)
{
    const int pitch_class = key % 12;
    for (int degree = 0; degree < 7; degree++)
    {
        if (ScaleDegree(scale, degree) == pitch_class)
        {
            return true;
        }
    }

    return false;
}

uint8_t FitToScale(const Chord& chord, uint8_t key, bool downward)
{
    if (!chord.scale)
    {
        return kChordNone;
    }

    if (key % 12 == ScaleRoot(chord) || InScale(*chord.scale, key))
    {
        return key;
    }

    return NearestInScale(chord, key, downward);
}

uint8_t NearestInScale(const Chord& chord, uint8_t key_byte, bool downward)
{
    if (!chord.scale)
    {
        return kChordNone;
    }

    const std::array<uint8_t, 7>& scale = *chord.scale;
    int key = key_byte;
    for (int i = 0; i < 12; i++)
    {
        const auto key8 = static_cast<uint8_t>(key);
        if (downward ? key8 <= 12 : key8 >= 120)
        {
            return kChordNone;
        }

        key += downward ? -1 : 1;
        if (InScale(scale, static_cast<uint8_t>(key)))
        {
            return static_cast<uint8_t>(key);
        }
    }

    return kChordNone;
}

int StepUpScale(const std::array<uint8_t, 7>& scale, int key, int steps, bool count_repeats, bool keep_octave)
{
    const int pitch_class = key % 12;
    const int octave = key / 12;
    int degree = 0;
    while (degree < 7 && ScaleDegree(scale, degree) != pitch_class)
    {
        degree++;
    }

    if (degree == 7)
    {
        return key;
    }

    int octaves = 0;
    int last = pitch_class;
    auto step = [&]
    {
        if (++degree == 7)
        {
            degree = 0;
            octaves++;
        }
    };
    for (int i = 0; i < steps; i++)
    {
        if (count_repeats)
        {
            step();
            continue;
        }

        int previous = 0;
        do
        {
            previous = last;
            step();
            last = ScaleDegree(scale, degree);
        } while (last == previous);
    }

    return ScaleDegree(scale, degree) + octave * 12 + (keep_octave ? 0 : octaves * 12);
}

ChordDatabase ReadChordDatabase(std::span<const uint8_t> data)
{
    if (!HasTag(data, 4, "CDSE"))
    {
        throw xsflib::FormatError("not a chord database");
    }

    const xsflib::Reader r(data, true);
    ChordDatabase database;
    const uint32_t entry_count = r.U32(8);
    const std::size_t records = r.U32(12);
    for (std::size_t i = 0; i < entry_count; i++)
    {
        ChordDatabase::Entry entry{.offset = r.U32(16 + 4 * i)};
        const uint16_t part_count = r.U16(entry.offset);
        entry.steps = r.U32(entry.offset + 4);
        r.U8(entry.steps);
        for (std::size_t j = 0; j < part_count; j++)
        {
            ChordDatabase::Part part{.offset = r.U32(entry.offset + 8 + 4 * j)};
            const uint8_t block_count = r.U8(part.offset);
            for (std::size_t k = 0; k < block_count; k++)
            {
                part.blocks.push_back(r.U32(part.offset + 4 + 4 * k));
            }

            entry.parts.push_back(std::move(part));
        }

        database.entries.push_back(std::move(entry));
    }

    database.record_count = r.U32(records);
    for (std::size_t i = 0; i < database.record_count; i++)
    {
        const std::size_t at = records + 4 + kRecordSize * i;
        if (at + kRecordSize > data.size())
        {
            break;
        }

        database.records.push_back({r.U32(at), r.U32(at + 4), r.U32(at + 8), r.U32(at + 12)});
    }

    return database;
}

} // namespace ntdlib::jaudio
