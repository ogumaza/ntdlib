// SPDX-License-Identifier: MIT

// A small synthetic cartridge of Ubisoft Milan's driver for the tests. It holds the code that the driver's detection
// reads, the PSG's tables, and a sound bank of the resources that a test adds. Resource 0 is the kit that the kit table
// names. No game ROM is needed.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/ubimilan/driver.h"

namespace ntdlib::testing::ubimilan_image
{

using ntdlib::agb::kRomBase;
using ntdlib::testing::agb_files::CartImage;

// The code that detection reads, the PSG's tables and the sound bank.
inline constexpr uint32_t kStep = kRomBase + 0x100;
inline constexpr uint32_t kTakeBank = kRomBase + 0x180;
inline constexpr uint32_t kWrapper = kRomBase + 0x1C0;
inline constexpr uint32_t kFileLookup = kRomBase + 0x200;
inline constexpr uint32_t kStart = kRomBase + 0x240;
inline constexpr uint32_t kInstrumentCode = kRomBase + 0x300;
inline constexpr uint32_t kNoiseCode = kRomBase + 0x380;
inline constexpr uint32_t kFrequencyCode = kRomBase + 0x3C0;
inline constexpr uint32_t kWaveCode = kRomBase + 0x440;
inline constexpr uint32_t kSoundInit = kRomBase + 0x480;
inline constexpr uint32_t kFineCode = kRomBase + 0x560;
inline constexpr uint32_t kSquare1Table = kRomBase + 0x600;
inline constexpr uint32_t kSquare2Table = kRomBase + 0x7E0;
inline constexpr uint32_t kWaveInstruments = kRomBase + 0x9C0;
inline constexpr uint32_t kFrequencyTable = kRomBase + 0xC00;
inline constexpr uint32_t kNoiseTable = kRomBase + 0xD00;
inline constexpr uint32_t kWaveTable = kRomBase + 0xD80;
inline constexpr uint32_t kBank = kRomBase + 0x1000;
inline constexpr uint32_t kFineTable = kRomBase + 0x40000;

// The offset of a bank's kit table in a TestImage: room for 60 resources before it.
inline constexpr uint32_t kKitTableOffset = 0x100;

// The driver's code that detection reads: the sequencer's step, the routine that takes the sound bank, the game's
// routine that calls it, the archive's lookup of the bank, the call of the two with file 2, the PSG's
// lookups of its tables, and the sound init with Timer 0's float, for a reload of 0xFBFF.
inline void WriteDriverCode(CartImage& cart)
{
    cart.PutHalfwords(kStep, {0xB530, 0x1C04, 0x68E0, 0x2800, 0xD03D, 0x8820, 0x1C25, 0x3514, 0x2800,
                              0xD133, 0x6920, 0x2800, 0xD017, 0x2000, 0x6120, 0x68A2, 0x2A00, 0xD008,
                              0x6062, 0x7811, 0x7850, 0x0200, 0x4301, 0x1C90, 0x6060, 0x8021});
    cart.PutHalfwords(kTakeBank, {0x4A07, 0x6110, 0x8801, 0x8011, 0x8841, 0x8051, 0x6841, 0x1841, 0x6051, 0x6881,
                                  0x1841, 0x6091, 0x68C1, 0x1840, 0x60D0, 0x4770});
    cart.Put32(kTakeBank + 0x20, 0x02003B00);
    cart.Put16(kWrapper, 0xB500);
    cart.PutBl(kWrapper + 2, kTakeBank);
    cart.Put16(kWrapper + 6, 0xBD00);
    cart.PutHalfwords(kFileLookup, {0x4801, 0x4770});
    cart.Put32(kFileLookup + 8, kBank);
    cart.Put16(kStart, 0x2002);
    cart.PutBl(kStart + 2, kFileLookup);
    cart.PutBl(kStart + 6, kWrapper);

    cart.PutHalfwords(kInstrumentCode,
                      {0xB500, 0x0600, 0x0E00, 0x2901, 0xD00E, 0x2901, 0xDC02, 0x2900, 0xD003, 0xE015, 0x2902, 0xD00D,
                       0xE012, 0x0100, 0x4901, 0x1840, 0xE00F, 0x0000, 0x0000, 0x0000, 0x0100, 0x4901, 0x1840, 0xE008,
                       0x0000, 0x0000, 0x0100, 0x4901, 0x1840, 0xE002, 0x0000, 0x0000, 0x2000, 0xBC02, 0x4708});
    cart.Put32(kInstrumentCode + 0x24, kSquare1Table);
    cart.Put32(kInstrumentCode + 0x30, kSquare2Table);
    cart.Put32(kInstrumentCode + 0x3C, kWaveInstruments);
    cart.PutHalfwords(kNoiseCode,
                      {0x00B1, 0x480A, 0x1809, 0x780A, 0x3101, 0x7808, 0x3101, 0x4338, 0x0200, 0x4310, 0x4A07, 0x8010});
    cart.Put32(kNoiseCode + 0x2C, kNoiseTable);
    cart.PutHalfwords(kFrequencyCode, {0x4914, 0x4642, 0x0050, 0x1840, 0x8807, 0x6BE0, 0x0040, 0x1840, 0x8800});
    cart.Put32(kFrequencyCode + 0x54, kFrequencyTable);
    cart.PutHalfwords(kWaveCode, {0x0168, 0x4906, 0x1840});
    cart.Put32(kWaveCode + 0x1C, kWaveTable);
    cart.PutHalfwords(kSoundInit,
                      {0x4929, 0x2001, 0x6008, 0x4929, 0x200E, 0x7008, 0x3101, 0x20A9, 0x7008, 0x3101, 0x2080, 0x7008,
                       0x4A25, 0x8810, 0x2380, 0x01DB, 0x1C19, 0x4308, 0x8010, 0x4C23, 0x4823, 0x4924, 0x6809, 0xF001,
                       0xFD9D, 0x6020, 0x4922, 0x2009, 0xF7FE, 0xFEFC, 0x4921, 0x200A, 0xF7FE, 0xFEF8, 0x4820});
    cart.Put32(kSoundInit + 0xC8, 0x44800570);
    cart.PutHalfwords(kFineCode, {0x4904, 0x0050, 0x1840, 0x8801, 0x2B01, 0xD000, 0x2B01, 0xDC00, 0x2B00, 0xD000});
    cart.Put32(kFineCode + 0x14, kFineTable);
}

// A cartridge whose sound bank takes what a test adds.
class TestImage : public CartImage
{
public:
    // What a PSG channel's program holds: the bytes of its registers, its modulation, its slide and its flags.
    struct Instrument
    {
        std::vector<uint8_t> bytes;
        uint16_t modulation_delay = 0;
        uint16_t modulation_depth = 0;
        uint16_t modulation_period = 0;
        uint16_t slide_frames = 0;
        uint16_t flags = 0;
    };

    // The fine table gives each place the number of the place, for a test to read off. The image ends in a run of
    // zeros.
    TestImage() : CartImage(kFineTable - kRomBase + 2 * kFinePlaces + kEndZeros)
    {
        WriteDriverCode(*this);
        for (uint32_t i = 0; i < 128; i++)
        {
            Put16(kFrequencyTable + 2 * i, uint16_t(1000 + i));
        }

        Put32(kNoiseTable, 0xC000F01E);
        for (uint32_t i = 0; i < 32; i++)
        {
            Put8(kWaveTable + i, uint8_t(i * 8));
        }

        for (uint32_t i = 0; i < kFinePlaces; i++)
        {
            Put16(kFineTable + 2 * i, uint16_t(i));
        }

        Put32(kBank + 4, 0x10);
        Put32(kBank + 8, 0x10);
        Put32(kBank + 12, kKitTableOffset);
        Put16(kBank + kKitTableOffset, 0);

        std::vector<uint8_t> kit = {uint8_t(ntdlib::ubimilan::kKit), 1, 0xFF, 0xFF};
        kit.resize(4 + 2 * (256 + 256), 0xFF);
        Add(kit);
    }

    // Adds a sample of `points`, and returns its resource.
    int AddSample(const std::vector<int8_t>& points, bool loop = false, uint32_t loop_start = 0, uint32_t loop_end = 0)
    {
        std::vector<uint8_t> bytes = {uint8_t(ntdlib::ubimilan::kSample), uint8_t(loop ? 1 : 0), 0x40, 0xFF};
        for (const uint32_t value : {uint32_t(points.size()), loop_start, loop_end})
        {
            for (int i = 0; i < 4; i++)
            {
                bytes.push_back(uint8_t(value >> (8 * i)));
            }
        }

        for (const int8_t point : points)
        {
            bytes.push_back(uint8_t(point));
        }

        return Add(bytes);
    }

    // Adds a sequence of `commands`, the bytes after its first 4, and returns its resource.
    int AddSequence(const std::vector<uint8_t>& commands)
    {
        std::vector<uint8_t> bytes = {uint8_t(ntdlib::ubimilan::kSequence), 0xFF, 0xFF, 0xFF};
        bytes.insert(bytes.end(), commands.begin(), commands.end());
        return Add(bytes);
    }

    // Adds a piece that plays `sequences`. Bit 0 of `flags` loops it, and bit 3 keeps a voice for the kit. Returns its
    // resource.
    int AddPiece(int flags, const std::vector<int>& sequences)
    {
        std::vector<uint8_t> bytes = {
            uint8_t(ntdlib::ubimilan::kMusic), 0xFF,           0xFF, 0xFF, uint8_t(sequences.size()),
            uint8_t(sequences.size() >> 8),    uint8_t(flags), 0};
        for (const int sequence : sequences)
        {
            bytes.push_back(uint8_t(sequence));
            bytes.push_back(uint8_t(sequence >> 8));
        }

        return Add(bytes);
    }

    // Has the kit's key `key` play resource `resource`.
    void SetKey(int key, int resource)
    {
        Put16(Resource(0) + 4 + 2 * uint32_t(key), uint16_t(resource));
    }

    // Gives PSG channel `channel` (0 to 2) the instrument of program `program`.
    void SetInstrument(int channel, int program, const Instrument& instrument)
    {
        const uint32_t tables[] = {kSquare1Table, kSquare2Table, kWaveInstruments};
        const uint32_t at = tables[channel] + 16 * uint32_t(program);
        for (size_t i = 0; i < instrument.bytes.size(); i++)
        {
            Put8(at + uint32_t(i), instrument.bytes[i]);
        }

        Put16(at + 4, instrument.modulation_delay);
        Put16(at + 6, instrument.modulation_depth);
        Put16(at + 8, instrument.modulation_period);
        Put16(at + 10, instrument.slide_frames);
        Put16(at + 12, instrument.flags);
    }

    // The address of resource `id`.
    uint32_t Resource(int id) const
    {
        return resources_[size_t(id)];
    }

    // Adds a resource of `bytes`, and returns it.
    int Add(const std::vector<uint8_t>& bytes)
    {
        const int id = int(resources_.size());
        resources_.push_back(next_);
        PutBytes(next_, bytes);
        Put32(kBank + 0x10 + 4 * uint32_t(id), next_ - kBank);
        Put16(kBank + 2, uint16_t(id + 1));
        next_ = (next_ + uint32_t(bytes.size()) + 3) & ~3u;
        return id;
    }

private:
    static constexpr uint32_t kFinePlaces = 128 * 100;
    static constexpr uint32_t kEndZeros = 0x1000;

    std::vector<uint32_t> resources_;
    uint32_t next_ = kBank + kKitTableOffset + 0x10;
};

} // namespace ntdlib::testing::ubimilan_image
