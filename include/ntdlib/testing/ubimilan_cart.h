// SPDX-License-Identifier: MIT

// A synthetic cartridge of Ubisoft Milan's driver for tests, built on ubimilan_image.h. Its sound bank contains a kit,
// three samples and three pieces of music. The PSG's tables are equal-tempered, and its fine table is inside the
// cartridge. With Tomb Raider's game code, the bank also contains resources up to the samples that play as music.
// OlderBuildCart makes a cartridge of the driver's older build.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/ubimilan_image.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/sequencer.h"

namespace ntdlib::testing::ubimilan_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::testing::ubimilan_image::kBank;
using ntdlib::testing::ubimilan_image::kFileLookup;
using ntdlib::testing::ubimilan_image::kFineCode;
using ntdlib::testing::ubimilan_image::kFrequencyTable;
using ntdlib::testing::ubimilan_image::kNoiseTable;
using ntdlib::testing::ubimilan_image::kSquare1Table;
using ntdlib::testing::ubimilan_image::kSquare2Table;
using ntdlib::testing::ubimilan_image::kStart;
using ntdlib::testing::ubimilan_image::kTakeBank;
using ntdlib::testing::ubimilan_image::kWaveInstruments;
using ntdlib::testing::ubimilan_image::kWaveTable;
using ntdlib::testing::ubimilan_image::kWrapper;
using ntdlib::ubimilan::kKit;
using ntdlib::ubimilan::kMusic;
using ntdlib::ubimilan::kNoResource;
using ntdlib::ubimilan::kSample;
using ntdlib::ubimilan::kSequence;

// The fine table of the test cartridge, for the PSG's update. A test can put a sequence at kPsgSequenceAt in place of
// the sequence of the third piece.
inline constexpr uint32_t kFineTable = kRomBase + 0xA000;
inline constexpr uint32_t kPsgSequenceAt = kRomBase + 0xE000;

// The keys that the fine table covers, from key 36.
inline constexpr int kFineKeys = 48;

// The sound bank's resources.
enum : int
{
    kKitId,
    kShortId,   // 64 points, no loop: channel 9's key 40
    kLoopId,    // 100 points, looping from 20 to 90: key 41
    kLongId,    // 4000 points, no loop: key 42
    kOnceSeqId, // the first piece's sequence
    kOnceId,    // the first piece: plays its sequence once
    kLoopSeqId,
    kLoopsId, // the second piece: loops its sequence, with a voice of its own for the kit
    kMixSeqId,
    kMixId, // the third piece: overlapping notes, the PSG and a key outside the kit
    kResourceCount,

    // Resources in Tomb Raider's numbers, in a cart that has them.
    kLoopSampleId = 103,   // 4096 points, looping from the first to the last: the first sample that plays as music
    kInsideSampleId = 104, // 4112 points, looping from 16 to the last
    kOnceSampleId = 107,   // 4000 points, no loop
    kLastSampleId = 108,
};

// The commands and countdowns of the pieces' sequences.
inline const std::vector<uint8_t> kOnceSequence = {
    0x00, 0x00, 0x99, 40,   64,   0x09, 0x00, // key 40 at velocity 64 on frame 0, then 10 frames
    0x89, 40,   0,    0x00, 0x00,             // its note-off on frame 10
    0x99, 41,   32,   0x13, 0x00,             // key 41 at 32, then 20 frames
    0x89, 41,   0,    0x00, 0x00,             // its note-off on frame 30
    0xFC,
};
inline const std::vector<uint8_t> kLoopSequence = {
    0x00, 0x00, 0x99, 42,   63,   0x04, 0x00, // key 42 on frame 0
    0x89, 42,   0,    0x09, 0x00,             // off on frame 5
    0x99, 40,   63,   0x04, 0x00,             // key 40 on frame 15
    0x89, 40,   0,    0x09, 0x00,             // off on frame 20, then the end on frame 30
    0xFC,
};
inline const std::vector<uint8_t> kMixSequence = {
    0x00, 0x00, 0xC0, 1,    0x00, 0x00, // square 1's instrument 1 on frame 0
    0x90, 60,   0x70, 0x00, 0x00,       // square 1's key 60, at volume 7
    0x99, 42,   63,   0x04, 0x00,       // key 42's long sample
    0x99, 41,   32,   0x04, 0x00,       // key 41's loop on frame 5, on another voice
    0x89, 42,   0,    0x09, 0x00,       // a note-off on frame 10: it stops the last note's voice
    0x80, 60,   0,    0x00, 0x00,       // square 1's note-off on frame 20
    0x90, 62,   0x00, 0x04, 0x00,       // a note at volume 0: it plays nothing
    0x80, 62,   0,    0x00, 0x00,       // on frame 25
    0x99, 50,   63,   0x00, 0x00,       // a key outside the kit
    0x99, 50,   63,   0x00, 0x00,       // and again, without a second warning
    0xFC,
};

// The PSG's modulation, glide, legato, wave program and noise, in a sequence that a test puts in place of the third
// piece's.
inline const std::vector<uint8_t> kPsgSequence = {
    0x00, 0x00, 0xC0, 2,    0x00, 0x00, // square 1's instrument 2, with the modulation, on frame 0
    0x90, 60,   0x70, 0x00, 0x00,       // square 1's key 60 at volume 7
    0xC1, 1,    0x00, 0x00,             // square 2's instrument 1, with the pitch mode
    0x91, 60,   0x50, 0x00, 0x00,       // square 2's key 60 at volume 5
    0xC2, 1,    0x00, 0x00,             // the wave's program 1, which loads its wave
    0x92, 48,   0x20, 0x00, 0x00,       // the wave's key 48 at full volume
    0x99, 0,    0x00, 0x04, 0x00,       // noise key 0, then frame 5
    0x91, 64,   0x50, 0x0E, 0x00,       // square 2's key 64, which glides from key 60, then frame 20
    0x80, 60,   0,    0x00, 0x00,       // square 1's note-off
    0x81, 64,   0,    0x00, 0x00,       // square 2's note-off
    0xC1, 2,    0x00, 0x00,             // square 2's instrument 2, with legato
    0x91, 60,   0x50, 0x04, 0x00,       // key 60, then frame 25
    0x91, 62,   0x50, 0x04, 0x00,       // key 62, which goes on from key 60, then frame 30
    0x81, 62,   0,    0x00, 0x00,       // square 2's note-off
    0x82, 48,   0,    0x00, 0x00,       // the wave's note-off
    0xFC,
};

// Returns the frequency setting of a square `index` keys above key 36, equal-tempered: the frequency table's entry
// `index`, or with a fraction, the fine table's entry 100 times `index`.
inline uint16_t Setting(double index)
{
    const double hz = 440.0 * std::pow(2.0, (index + 36 - 69) / 12.0);
    return uint16_t(std::max(0L, std::lround(2048 - 131072 / hz)));
}

class Cart : public ntdlib::testing::agb_files::CartImage
{
public:
    // With `tomb_raider`, the cart has Tomb Raider's game code and resources up to its samples that play as music.
    explicit Cart(bool tomb_raider = false) : CartImage(0x10000), tomb_raider_(tomb_raider)
    {
        ntdlib::testing::ubimilan_image::WriteDriverCode(*this);
        WriteTables();
        WriteBank();
    }

    // Returns the address of resource `id`.
    uint32_t Resource(int id) const
    {
        return resources_[size_t(id)];
    }

    // Gives the third piece the sequence of `commands` in place of its sequence.
    void ReplaceMixSequence(const std::vector<uint8_t>& commands)
    {
        PutBytes(kPsgSequenceAt, Sequence(commands));
        Put32(kBank + 0x10 + 4 * uint32_t(kMixSeqId), kPsgSequenceAt - kBank);
    }

private:
    // Square 1, instrument 1: a 50% duty and an envelope that the velocity sets. Instrument 2 adds a modulation: a
    // delay of 2 frames, then a triangle of 20 cents either way, 5 cents a frame. Square 2, instrument 1: a 50% duty
    // and the pitch mode, with glides over 4 frames. Instrument 2: a 25% duty and legato. Program 1 of the wave channel
    // has another wave. The frequency table and the fine table are equal-tempered from key 36. The PSG's update reads
    // the fine table here, inside the cartridge. Noise key 0 plays for 34 steps of the length counter at volume 15.
    void WriteTables()
    {
        Put32(kFineCode + 0x14, kFineTable);
        Put8(kSquare1Table + 16 + 1, 0x80);
        Put8(kSquare1Table + 16 + 3, 0x80);
        const uint32_t modulated = kSquare1Table + 32;
        Put8(modulated + 1, 0x80);
        Put8(modulated + 3, 0x80);
        Put16(modulated + 4, 2);
        Put16(modulated + 6, 20);
        Put16(modulated + 8, 2);
        Put16(modulated + 12, 1);
        const uint32_t gliding = kSquare2Table + 16;
        Put8(gliding, 0x80);
        Put8(gliding + 2, 0x80);
        Put16(gliding + 10, 4);
        Put16(gliding + 12, 2);
        const uint32_t legato = kSquare2Table + 32;
        Put8(legato, 0x40);
        Put8(legato + 2, 0x80);
        Put16(legato + 12, 4);
        Put8(kWaveInstruments + 16 + 1, 0x80);

        for (int i = 0; i < 128; i++)
        {
            Put16(kFrequencyTable + 2 * uint32_t(i), Setting(i));
        }
        for (int c = 0; c < 100 * kFineKeys; c++)
        {
            Put16(kFineTable + 2 * uint32_t(c), Setting(c / 100.0));
        }
        Put32(kNoiseTable, 0xC000F01E);
        for (uint32_t i = 0; i < 32; i++)
        {
            Put8(kWaveTable + i, uint8_t(i * 8));
            Put8(kWaveTable + 32 + i, uint8_t(i < 16 ? 0xFF : 0x00));
        }
    }

    // The sound bank: its counts and the offsets of its tables, the table of resources, the kit table, and the
    // resources, each at a word's boundary.
    void WriteBank()
    {
        const int count = tomb_raider_ ? kLastSampleId + 1 : kResourceCount;
        const uint32_t kit_table = tomb_raider_ ? 0x10 + 4 * uint32_t(count) : 0x60;
        uint32_t next = kBank + kit_table + 0x20;
        const auto add = [&](int id, const std::vector<uint8_t>& bytes)
        {
            resources_[size_t(id)] = next;
            for (size_t i = 0; i < bytes.size(); i++)
            {
                Put8(next + uint32_t(i), bytes[i]);
            }
            Put32(kBank + 0x10 + 4 * uint32_t(id), next - kBank);
            next = (next + uint32_t(bytes.size()) + 3) & ~3u;
        };

        Put16(kBank + 2, uint16_t(count));
        Put32(kBank + 4, 0x10);
        Put32(kBank + 8, 0x10);
        Put32(kBank + 12, kit_table);
        Put16(kBank + kit_table, kKitId);

        std::vector<uint8_t> kit = {kKit, 1, 0xFF, 0xFF};
        for (int key = 0; key < 128; key++)
        {
            const int id = key == 40 ? kShortId : key == 41 ? kLoopId : key == 42 ? kLongId : kNoResource;
            kit.push_back(uint8_t(id));
            kit.push_back(uint8_t(id >> 8));
        }
        add(kKitId, kit);
        add(kShortId, Sample(64, false, 0, 0));
        add(kLoopId, Sample(100, true, 20, 90));
        add(kLongId, Sample(4000, false, 0, 0));
        add(kOnceSeqId, Sequence(kOnceSequence));
        add(kOnceId, Music(0, kOnceSeqId));
        add(kLoopSeqId, Sequence(kLoopSequence));
        add(kLoopsId, Music(9, kLoopSeqId));
        add(kMixSeqId, Sequence(kMixSequence));
        add(kMixId, Music(0, kMixSeqId));

        // Resources 103 to 108: the samples that play as music, and short samples up to them.
        if (tomb_raider_)
        {
            PutBytes(kRomBase + 0xAC, {'A', 'L', '9', 'P'});
            for (int id = kResourceCount; id <= kLastSampleId; id++)
            {
                add(id, id == kLoopSampleId     ? Sample(4096, true, 0, 4096)
                        : id == kInsideSampleId ? Sample(4112, true, 16, 4112)
                        : id == kOnceSampleId   ? Sample(4000, false, 0, 0)
                                                : Sample(16, false, 0, 0));
            }
        }
    }

    static std::vector<uint8_t> Sample(uint32_t length, bool loop, uint32_t start, uint32_t end)
    {
        std::vector<uint8_t> b = {kSample, uint8_t(loop ? 1 : 0), 0x40, 0xFF};
        for (uint32_t v : {length, loop ? start : 0x42424242u, loop ? end : 0x42424242u})
        {
            for (int i = 0; i < 4; i++)
            {
                b.push_back(uint8_t(v >> (8 * i)));
            }
        }
        for (uint32_t i = 0; i < length; i++)
        {
            b.push_back(uint8_t(int8_t((i * 7) % 200 - 100)));
        }

        return b;
    }

    static std::vector<uint8_t> Sequence(const std::vector<uint8_t>& commands)
    {
        std::vector<uint8_t> b = {kSequence, 0xFF, 0xFF, 0xFF};
        b.insert(b.end(), commands.begin(), commands.end());

        return b;
    }

    static std::vector<uint8_t> Music(int flags, int sequence)
    {
        return {kMusic, 0xFF, 0xFF, 0xFF, 1, 0, uint8_t(flags), 0, uint8_t(sequence), 0};
    }

    bool tomb_raider_ = false;
    std::array<uint32_t, kLastSampleId + 1> resources_ = {};
};

// A cartridge of the driver's older build: the routine that takes the sound bank, the game's routine that calls it, the
// archive's lookup of the bank, the call of the two with file 2, and a bank of two samples.
inline ntdlib::testing::agb_files::CartImage OlderBuildCart()
{
    ntdlib::testing::agb_files::CartImage cart(0x10000);
    cart.PutHalfwords(kTakeBank, {0x4A06, 0x60D0, 0x8801, 0x8011, 0x8841, 0x8051, 0x6841, 0x1841, 0x6051, 0x6881,
                                  0x1840, 0x6090, 0x4770});
    cart.Put32(kTakeBank + 0x1C, 0x02003B00);
    cart.Put16(kWrapper, 0xB500);
    cart.PutBl(kWrapper + 2, kTakeBank);
    cart.Put16(kWrapper + 6, 0xBD00);
    cart.PutHalfwords(kFileLookup, {0x4801, 0x4770});
    cart.Put32(kFileLookup + 8, kBank);
    cart.Put16(kStart, 0x2002);
    cart.PutBl(kStart + 2, kFileLookup);
    cart.PutBl(kStart + 6, kWrapper);
    cart.Put16(kBank + 2, 2);
    cart.Put32(kBank + 4, 0x10);
    cart.Put32(kBank + 8, 0x10);
    cart.Put32(kBank + 0x10, 0x20);
    cart.Put32(kBank + 0x14, 0x30);
    cart.Put8(kBank + 0x20, kSample);
    cart.Put8(kBank + 0x30, kSample);

    return cart;
}

} // namespace ntdlib::testing::ubimilan_cart
