// SPDX-License-Identifier: MIT

// A small synthetic cartridge of Brownie Brown's driver for the tests, built in memory, so no game ROM is needed.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/testing/agb_cart.h"

namespace brownie_testing
{

using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::brownie::DriverInfo;
using ntdlib::brownie::DriverOverrides;
using ntdlib::brownie::kRateSteps;
using ntdlib::brownie::Revision;

// Addresses in a test cartridge: the init's setting of Timer 0 and its reload value, the per-frame routine's code that
// detection reads and its literal pool, the command table and its routines, and the driver's tables.
inline constexpr uint32_t kTimer = kRomBase + 0x100;
inline constexpr uint32_t kReload = kRomBase + 0x1F0;
inline constexpr uint32_t kFrame = kRomBase + 0x200;
inline constexpr uint32_t kFramePool = kRomBase + 0x400;
inline constexpr uint32_t kCommands = kRomBase + 0x500;
inline constexpr uint32_t kCommandCode = kRomBase + 0x600; // 0x20 bytes for each command's routine
inline constexpr uint32_t kLengthTable = kRomBase + 0xA00;
inline constexpr uint32_t kFrequencyTable = kRomBase + 0xB00;
inline constexpr uint32_t kControlRegisters = kRomBase + 0xC00;
inline constexpr uint32_t kFrequencyRegisters = kControlRegisters + 0x20;
inline constexpr uint32_t kPanTable = kControlRegisters + 0x40;
inline constexpr uint32_t kEnvelopeTable = kRomBase + 0xD00;
inline constexpr uint32_t kEnvelopes = kRomBase + 0xD40;
inline constexpr uint32_t kSampleSets = kRomBase + 0xE00;
inline constexpr uint32_t kSampleEntries = kRomBase + 0xE40;
inline constexpr uint32_t kWaveTable = kRomBase + 0xF00;
inline constexpr uint32_t kWaves = kRomBase + 0xF40;
inline constexpr uint32_t kSongTable = kRomBase + 0x1000;
inline constexpr uint32_t kSongLists = kRomBase + 0x1100;
inline constexpr uint32_t kSequences = kRomBase + 0x2000;
inline constexpr uint32_t kRateTable = kRomBase + 0x6000; // the Magical Vacation revision's
inline constexpr uint32_t kSamples = kRomBase + 0x8000;   // sample s is at kSamples + 0x100 * s

// The bytes of each sample, the semitones of a sample set and the envelopes of the test cartridge.
inline constexpr uint32_t kSampleBytes = 0x40;
inline constexpr int kSemitones = 12;
inline constexpr int kPsgHold = 0;     // volume 15 for as long as the note lasts
inline constexpr int kPsgSteps = 1;    // 15 for 2 frames, 8 for 3 frames, then 4
inline constexpr int kPsgRelease = 2;  // the DAC off
inline constexpr int kSampleHold = 3;  // level 255 for as long as the note lasts
inline constexpr int kSampleRamp = 4;  // level 255, down 8 a frame
inline constexpr int kPsgHardware = 5; // volume 15 with the hardware's envelope going down at each of its steps

// A sound's channel: its number, its data, and the places in the data, as offsets, that F6 calls and F8 jumps to.
struct TestChannel
{
    int channel = 0;
    std::vector<uint8_t> data;
    std::vector<uint32_t> targets;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, the driver's tables
// and the sounds, for a revision of the driver. Sound 0 lists no channels, since the driver's queue takes 0 for no
// request.
class Cart : public ntdlib::testing::agb_files::CartImage
{
public:
    explicit Cart(bool code = true, Revision revision = Revision::kSwordOfMana)
        : CartImage(0x10000), vacation_(revision == Revision::kMagicalVacation)
    {
        WriteTables();
        if (code)
        {
            WriteCode();
        }
        Sound(0, {});
    }

    // Writes entry `sound` of the song table: the list of its channels, and each one's data and subroutines.
    void Sound(int sound, const std::vector<TestChannel>& channels)
    {
        Put32(kSongTable + 4 * uint32_t(sound), next_list_);
        for (const TestChannel& c : channels)
        {
            const uint32_t data = next_data_;
            for (size_t i = 0; i < c.data.size(); i++)
            {
                Put8(data + uint32_t(i), c.data[i]);
            }

            const uint32_t subroutines = (data + uint32_t(c.data.size()) + 3) & ~3u;
            for (size_t i = 0; i < c.targets.size(); i++)
            {
                Put32(subroutines + 4 * uint32_t(i), data + c.targets[i]);
            }
            next_data_ = subroutines + 4 * uint32_t(c.targets.size()) + 4;

            Put32(next_list_, uint32_t(c.channel));
            Put32(next_list_ + 4, data);
            Put32(next_list_ + 8, subroutines);
            next_list_ += 12;
        }
        Put32(next_list_, 0xFF);
        next_list_ += 4;
    }

    // Puts the next list and the next channel's data at these addresses.
    void MoveTo(uint32_t list, uint32_t data)
    {
        next_list_ = list;
        next_data_ = data;
    }

private:
    // Writes `insn`, an ldr from the pc with an offset of 0, at `at`, giving it the offset of `pool`, which holds
    // `value`.
    void Ldr(uint32_t at, uint32_t insn, uint32_t pool, uint32_t value)
    {
        Put32(at, insn | (pool - (at + 8)));
        Put32(pool, value);
    }

    // Writes the driver's tables: lengths, frequency settings tuned a third of a semitone below key 37 + n, the PSG's
    // registers and pans, envelopes, a sample set of 12 samples, of which the even ones loop, which the Magical
    // Vacation revision plays one by one, a wave, and that revision's timer reload values, 1024 Hz times 2 to the n /
    // 144.
    void WriteTables()
    {
        const uint8_t kLengths[16] = {4, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192};
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(kLengthTable + i, kLengths[i]);
        }
        for (int n = 0; n < 84; n++)
        {
            const double hz = 440 * std::pow(2.0, (37 + n - 1.0 / 3 - 69) / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(n), uint16_t(std::lround(std::max(0.0, 2048 - 131072 / hz))));
        }

        const uint32_t kControls[4] = {0x04000062, 0x04000068, 0x04000072, 0x04000078};
        const uint32_t kFrequencies[4] = {0x04000064, 0x0400006C, 0x04000074, 0x0400007C};
        for (uint32_t c = 0; c < 8; c++)
        {
            Put32(kControlRegisters + 4 * c, kControls[c & 3]);
            Put32(kFrequencyRegisters + 4 * c, kFrequencies[c & 3]);
        }
        for (uint32_t c = 0; c < 12; c++)
        {
            Put8(kPanTable + c, uint8_t(0x11 << (c & 3)));
            Put8(kPanTable + 12 + c, uint8_t(~(0x11 << (c & 3))));
        }

        const std::vector<std::vector<uint8_t>> kEnvelopeData = {
            {0, 0xF0}, {2, 0xF0, 3, 0x80, 0, 0x40}, {0, 0x00}, {0, 0x00, 0xFF}, {0, 0x88, 0xFF}, {0, 0xF1}};
        uint32_t at = kEnvelopes;
        for (size_t e = 0; e < kEnvelopeData.size(); e++)
        {
            Put32(kEnvelopeTable + 4 * uint32_t(e), at);
            for (uint8_t b : kEnvelopeData[e])
            {
                Put8(at++, b);
            }
        }

        Put32(kSampleSets, kSampleEntries);
        for (uint32_t s = 0; s < uint32_t(kSemitones); s++)
        {
            const uint32_t start = kSamples + 0x100 * s;
            const uint32_t end = start + kSampleBytes;
            Put32(kSampleEntries + 4 * s, kSampleEntries + 0x30 + 12 * s);
            Put32(kSampleEntries + 0x30 + 12 * s, start);
            Put32(kSampleEntries + 0x30 + 12 * s + 4, end);
            Put32(kSampleEntries + 0x30 + 12 * s + 8, s % 2 ? end : start);
            for (uint32_t i = 0; i < kSampleBytes; i++)
            {
                Put8(start + i, uint8_t(i * (s + 3) % 256 - 128));
            }
        }

        Put32(kWaveTable, kWaves);
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(kWaves + i, uint8_t((2 * i) << 4 | (2 * i + 1)));
        }

        for (int i = 0; i < 85 * kRateSteps; i++)
        {
            Put16(kRateTable + 2 * uint32_t(i), uint16_t(0x10000 - std::lround(16384 * std::pow(2.0, -i / 144.0))));
        }
    }

    // Writes the code that detection reads: the init's setting of Timer 0, the parts of the per-frame routine, and the
    // routines of commands F0, E1 and FC. The Magical Vacation revision's routine pushes r8 too, runs 12 channels and
    // loads the rate table, and its init doesn't set Timer 0.
    void WriteCode()
    {
        if (!vacation_)
        {
            Ldr(kTimer, 0xE59F1000, kTimer + 0x20, kReload);
            Put32(kTimer + 4, 0xE5910000);
            Put32(kTimer + 8, 0xE1C30AB0);
            Put16(kReload, 0xFC00);
        }

        // The start of the routine, its loop over the channels, and its loads of the tables.
        const uint32_t kStart[10] = {0xE92D00F0, 0xE3A07000, 0xE59F1000, 0xE5D12000, 0xE3120001,
                                     0x0A000000, 0xE5D12001, 0xE2422001, 0xE3520000, 0x0A000000};
        uint32_t at = kFrame;
        for (uint32_t w : kStart)
        {
            Put32(at, w);
            at += 4;
        }
        Ldr(kFrame + 8, 0xE59F1000, kFramePool, 0x03002ACC);
        for (uint32_t w : {0xE2811038u, 0xE2877001u, vacation_ ? 0xE337000Cu : 0xE337000Du})
        {
            Put32(at, w);
            at += 4;
        }
        if (vacation_)
        {
            Put32(kFrame, 0xE92D01F0);
        }

        uint32_t pool = kFramePool + 4;
        const auto load = [&](uint32_t insn, uint32_t value)
        {
            Ldr(at, insn, pool, value);
            at += 4;
            pool += 4;
        };

        const auto word = [&](uint32_t w)
        {
            Put32(at, w);
            at += 4;
        };

        word(0xE200001F);
        load(0xE59F3000, kCommands);
        word(0xE793F100);
        load(0xE59F2000, kSongTable);
        word(0xE7920101);
        load(0xE59F3000, kLengthTable);
        word(0xE7D30000);
        load(0xE59F2000, kFrequencyTable);
        word(0xE1A00080);
        word(0xE19250B0);
        load(0xE59F3000, kPanTable);
        word(0xE7F34001);
        load(0xE59F2000, kControlRegisters);
        word(0xE7922107);
        word(0xE1C230B0);
        load(0xE59F2000, kFrequencyRegisters);
        word(0xE7922107);
        if (vacation_)
        {
            load(0xE59F2000, kRateTable);
            word(0xE1A00080);
            word(0xE19200B0);
        }

        // The command routines, of which F0's, E1's and FC's load the envelopes, the sample sets or samples and the
        // waves.
        for (uint32_t c = 0; c < 32; c++)
        {
            const uint32_t routine = kCommandCode + 0x20 * c;
            Put32(kCommands + 4 * c, routine);
            Put32(routine, 0xE4D20001);
            const uint32_t samples = vacation_ ? kSampleEntries : kSampleSets;
            const uint32_t table = c == 0x10 ? kEnvelopeTable : c == 0x01 ? samples : kWaveTable;
            const uint32_t use = c == 0x10 ? 0xE0833100 : c == 0x01 ? 0xE7930100 : c == 0x1C ? 0xE7934100 : 0xE1A00000;
            Ldr(routine + 4, 0xE59F3000, routine + 0x10, table);
            Put32(routine + 8, use);
        }
    }

    bool vacation_;
    uint32_t next_list_ = kSongLists;
    uint32_t next_data_ = kSequences;
};

// Returns the driver info of a cartridge, failing the test if detection fails.
inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));
    SEQLIB_CHECK(error.empty());

    return info;
}

} // namespace brownie_testing
