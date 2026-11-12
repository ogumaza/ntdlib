// SPDX-License-Identifier: MIT

// A synthetic cartridge with Logik State's GBA driver, for tests: the routines that detection looks for, the calls
// that play the modules, a bank of samples and the modules themselves, in either format.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/player.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::logikstate_cart
{

using ntdlib::agb::Rom;
using ntdlib::logikstate::Format;
using ntdlib::logikstate::kBankPoints;
using ntdlib::logikstate::kCellSampleShift;
using ntdlib::logikstate::kCellSize;
using ntdlib::logikstate::kMaxPatterns;
using ntdlib::logikstate::kMod1OrderList;
using ntdlib::logikstate::kMod1Orders;
using ntdlib::logikstate::kMod1PatternBytes;
using ntdlib::logikstate::kMod1Patterns;
using ntdlib::logikstate::kMod1Pitches;
using ntdlib::logikstate::kMod1PointBytes;
using ntdlib::logikstate::kMod1Rate;
using ntdlib::logikstate::kMod1Rows;
using ntdlib::logikstate::kMod1Samples;
using ntdlib::logikstate::kMod1Signature2;
using ntdlib::logikstate::kMod1Size;
using ntdlib::logikstate::kMod1Steps;
using ntdlib::logikstate::kModuleChannels;
using ntdlib::logikstate::kModuleOrderList;
using ntdlib::logikstate::kModuleOrders;
using ntdlib::logikstate::kModuleOwnSamples;
using ntdlib::logikstate::kModulePatternBytes;
using ntdlib::logikstate::kModulePatterns;
using ntdlib::logikstate::kModulePeriods;
using ntdlib::logikstate::kModuleRate;
using ntdlib::logikstate::kModuleRows;
using ntdlib::logikstate::kModuleSamples;
using ntdlib::logikstate::kModuleSize;
using ntdlib::logikstate::kModuleSteps;
using ntdlib::logikstate::kNoPitch;
using ntdlib::logikstate::kPeriodClock;
using ntdlib::logikstate::kPitches;
using ntdlib::logikstate::kSampleFineTune;
using ntdlib::logikstate::kSampleLength;
using ntdlib::logikstate::kSampleLoopLength;
using ntdlib::logikstate::kSampleLoopStart;
using ntdlib::logikstate::kSamples;
using ntdlib::logikstate::kSampleSize;
using ntdlib::logikstate::kSampleVolume;
using ntdlib::logikstate::kSemitonePitches;
using ntdlib::logikstate::kSignature1;
using ntdlib::logikstate::kSignature2;
using ntdlib::logikstate::Sample;
using ntdlib::testing::agb_files::CartImage;

// The cartridge's layout: the load routine, the start routine, the play routine, the routines that set the master
// volume, the music's volume and the sound effects' volume, the routine that switches the sound off, the game's calls,
// the bank of samples, and the first module. Each module starts kModuleSpace after the one before.
constexpr uint32_t kLoad = 0x08000200;
constexpr uint32_t kStart = 0x08000210;
constexpr uint32_t kPlay = 0x08000240;
constexpr uint32_t kMasterVolume = 0x08000250;
constexpr uint32_t kMusicVolume = 0x0800025C;
constexpr uint32_t kEffectsVolume = 0x08000268;
constexpr uint32_t kSoundOff = 0x08000274;
constexpr uint32_t kRecordCode = 0x08000F00;

// Where a test puts a GBAMOD1 game's table data: a zero word and the event entries, the words that point at entries,
// and the table's record of kRecordSize bytes.
constexpr uint32_t kTableData = 0x08002400;
constexpr uint32_t kTablePointers = 0x08002600;
constexpr uint32_t kTableRecord = 0x08002800;
constexpr uint32_t kRecordSize = 0x274;

// The routine that starts the output, where a test puts one, and the routine that switches the sound off after it.
constexpr uint32_t kOutput = 0x08000E00;
constexpr uint32_t kOutputSoundOff = 0x08000E20;

// Where the game's code other than its calls of the play routine goes: past the driver's code and the bank.
constexpr uint32_t kGameCode = 0x08002000;

// What calls the routine that switches the sound off in a test cartridge.
enum class SoundOffCaller
{
    kGame,              // the game's code
    kGameThroughDriver, // a routine of the driver that the game's code calls
    kDriverAlone,       // a routine of the driver that only the driver calls
};
constexpr uint32_t kCalls = 0x08000280;
constexpr uint32_t kBank = 0x08001000;
constexpr uint32_t kFirstModule = 0x08004000;
constexpr uint32_t kModuleSpace = 0x8000;
constexpr size_t kCartSize = 0x40000;

// The space that a call of the play routine uses with its literals. A call that sets the music's volume uses as
// much.
constexpr uint32_t kCallSize = 0x14;

// The address of the master volume's variable. The other two volumes' variables follow it.
constexpr uint32_t kVolumeVariables = 0x03000010;

// The mixer's rate of the test modules, and a frame's samples at it.
constexpr uint32_t kRate = 21024;
constexpr uint32_t kFrame = 352;

// A cell of a test pattern.
struct TestCell
{
    uint16_t pitch = kNoPitch;
    uint16_t sample = 0;
    uint8_t effect = 0;
    uint8_t operand = 0;
};

// A row: a cell for each channel from the first. The channels after the last cell get empty cells.
using TestRow = std::vector<TestCell>;

// A test module.
struct TestModule
{
    uint32_t channels = 4;
    uint32_t rate = kRate;
    std::vector<uint8_t> orders = {0};
    std::vector<std::vector<TestRow>> patterns;
    std::array<Sample, kSamples> samples = {};
    bool own_samples = false;
    // A GBAMOD1 module has 4 channels, and its points follow its patterns: each sample's from `points`, or zeros.
    Format format = Format::kGbaMod2;
    std::vector<std::vector<int8_t>> points;
};

// Returns the period of `pitch` in the test modules' table: ProTracker's periods for the semitones, with C-1's 856 at
// pitch 104, and a straight line between two semitones. The table contains whole numbers alone.
inline uint16_t TestPeriod(uint32_t pitch)
{
    constexpr std::array<uint32_t, 12> kOctave = {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453};
    const auto semitone = [&](uint32_t s)
    {
        // Semitone 0 is the B below C-0, and semitone 1 is C-0, at pitch 8.
        return s == 0 ? kOctave[11] * 4 : (kOctave[(s - 1) % 12] * 2) >> ((s - 1) / 12);
    };
    const uint32_t below = semitone(pitch / kSemitonePitches);
    const uint32_t above = semitone(pitch / kSemitonePitches + 1);

    return uint16_t(below - (below - above) * (pitch % kSemitonePitches) / kSemitonePitches);
}

// Returns the step of `pitch` in a test module's table at `rate`. The driver works a step out from a period the same
// way.
inline uint32_t TestStep(uint32_t pitch, uint32_t rate = kRate)
{
    const uint32_t period = TestPeriod(pitch);
    return period ? ((uint32_t(kPeriodClock) / (period * 2)) << 12) / rate : 0;
}

// An event entry of a test table: a jingle of `value`, the order, or a sample. A word of the ROM points at an entry
// with `pointed`.
struct TestEntry
{
    uint8_t value = 0;
    bool jingle = true;
    bool pointed = true;
};

// Returns the step of `pitch` in a GBAMOD1 test module's table, whose pitch 8 is ProTracker's C-1: the step of the
// pitch 96 above it in a GBAMOD2 test module.
inline uint32_t TestMod1Step(uint32_t pitch, uint32_t rate = kRate)
{
    return TestStep(pitch + 96, rate);
}

// A cartridge with the driver's routines and a bank of samples. A test adds its modules and the game's calls.
class Cart
{
public:
    // Starts a cartridge whose bank contains `bank`: the points of each sample from the first.
    explicit Cart(const std::vector<std::vector<int8_t>>& bank) : image_(kCartSize)
    {
        // The load routine: push {r4, lr}; ldr r0, =signature; pop {r4, pc}, and the signature's two words.
        image_.PutHalfwords(kLoad, {0xB510, 0x4801, 0xBD10, 0x0000});
        image_.Put32(kLoad + 8, kSignature1);
        image_.Put32(kLoad + 12, kSignature2);

        // The start routine: bx lr. The play routine: push {r4, lr}; bl load; bl start; pop {r4, pc}.
        image_.Put16(kStart, 0x4770);
        image_.Put16(kPlay, 0xB510);
        image_.PutBl(kPlay + 2, kLoad);
        image_.PutBl(kPlay + 6, kStart);
        image_.Put16(kPlay + 10, 0xBD10);

        // The three routines that set a volume: ldr r1, =variable; str r0, [r1]; bx lr.
        for (uint32_t i = 0; i < 3; i++)
        {
            image_.PutHalfwords(kMasterVolume + i * 12, {0x4901, 0x6008, 0x4770, 0x0000});
            image_.Put32(kMasterVolume + i * 12 + 8, kVolumeVariables + i * 4);
        }

        // The routine that switches the sound off: ldr r1, =SOUNDCNT_X; movs r0, #0; strh r0, [r1]; bx lr.
        image_.PutHalfwords(kSoundOff, {0x4901, 0x2000, 0x8008, 0x4770});
        image_.Put32(kSoundOff + 8, 0x04000084);

        uint32_t at = kBank + kBankPoints;
        for (size_t i = 0; i < bank.size(); i++)
        {
            image_.Put16(kBank + uint32_t(i) * kSampleSize + kSampleLength, uint16_t(bank[i].size()));
            for (int8_t point : bank[i])
            {
                image_.Put8(at++, uint8_t(point));
            }
        }
    }

    // Writes `module` at the next module's place and returns its address.
    uint32_t AddModule(const TestModule& module)
    {
        const uint32_t address = next_module_;
        next_module_ += kModuleSpace;
        if (module.format == Format::kGbaMod1)
        {
            AddMod1Module(address, module);
            return address;
        }

        image_.Put32(address, kSignature1);
        image_.Put32(address + 4, kSignature2);
        image_.Put32(address + kModuleChannels, module.channels);
        image_.Put32(address + kModuleRate, module.rate);
        image_.Put32(address + kModuleOrders, uint32_t(module.orders.size()));
        image_.Put32(address + kModulePatterns, uint32_t(module.patterns.size()));
        image_.Put32(address + kModuleOwnSamples, module.own_samples ? 0xFFFFFFFF : 0);
        for (uint32_t pitch = 0; pitch < uint32_t(kPitches); pitch++)
        {
            image_.Put32(address + kModuleSteps + pitch * 4, TestStep(pitch, module.rate));
            image_.Put16(address + kModulePeriods + pitch * 2, TestPeriod(pitch));
        }
        for (size_t i = 0; i < module.orders.size(); i++)
        {
            image_.Put8(address + kModuleOrderList + uint32_t(i), module.orders[i]);
        }
        for (uint32_t i = 0; i < uint32_t(kSamples); i++)
        {
            const uint32_t header = address + kModuleSamples + i * kSampleSize;
            image_.Put16(header + kSampleLength, module.samples[i].length);
            image_.Put16(header + kSampleFineTune, module.samples[i].fine_tune);
            image_.Put16(header + kSampleVolume, module.samples[i].volume);
            image_.Put16(header + kSampleLoopStart, module.samples[i].loop_start);
            image_.Put16(header + kSampleLoopLength, module.samples[i].loop_length);
        }

        // The patterns: each row's cells, a channel after another.
        uint32_t at = address + kModuleSize;
        for (size_t p = 0; p < module.patterns.size(); p++)
        {
            image_.Put32(address + kModuleRows + uint32_t(p) * 4, uint32_t(module.patterns[p].size()));
            for (const TestRow& row : module.patterns[p])
            {
                for (uint32_t c = 0; c < module.channels; c++)
                {
                    const TestCell cell = c < row.size() ? row[c] : TestCell();
                    image_.Put16(at, uint16_t(cell.pitch | (cell.sample << kCellSampleShift)));
                    image_.Put16(at + 2, uint16_t((cell.effect << 8) | cell.operand));
                    at += kCellSize;
                }
            }
        }
        image_.Put32(address + kModulePatternBytes, at - (address + kModuleSize));

        return address;
    }

    // Writes the signature of GBAMOD1 modules into the load routine.
    void UseMod1()
    {
        image_.Put32(kLoad + 12, kMod1Signature2);
    }

    // Writes a routine that starts the output with `sound_control` for SOUNDCNT_H: ldr r0, =DMA1CNT_H; ldr r1, =value;
    // strh r1, [r0]; bx lr. The routine that switches the sound off moves after it, out of reach of the play routine.
    // The GBAMOD1 build has it there too.
    void AddOutput(uint16_t sound_control)
    {
        image_.PutHalfwords(kOutput, {0x4801, 0x4902, 0x8001, 0x4770});
        image_.Put32(kOutput + 8, 0x040000C6);
        image_.Put32(kOutput + 12, sound_control);
        image_.PutHalfwords(kSoundOff, {0, 0, 0, 0});
        image_.Put32(kSoundOff + 8, 0);
        image_.PutHalfwords(kOutputSoundOff, {0x4901, 0x2000, 0x8008, 0x4770});
        image_.Put32(kOutputSoundOff + 8, 0x04000084);
        sound_off_ = kOutputSoundOff;
    }

    // Sets the game code of the cartridge to `code`, of 4 letters.
    void SetGameCode(const char* code)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            image_.Put8(0x080000AC + i, uint8_t(code[i]));
        }
    }

    // Writes a table's data in the layout of Pinball Challenge Deluxe: the record, whose word +0x60 names `module`,
    // whose words +0x10 and +0x14 name the data's start and end, and which contains `bytes` at their offsets, and
    // before it a zero word and `entries`. `first_word` starts the record.
    void AddTable(uint32_t module, const std::vector<std::pair<uint32_t, uint8_t>>& bytes,
                  const std::vector<TestEntry>& entries, uint32_t first_word = 0)
    {
        image_.Put32(kTableData, 0);
        uint32_t pointer = kTablePointers;
        for (size_t i = 0; i < entries.size(); i++)
        {
            const uint32_t at = kTableData + 4 + uint32_t(i) * 4;
            const TestEntry& e = entries[i];
            image_.PutBytes(at, {uint8_t(e.jingle ? 0x80 | e.value : e.value), 0, uint8_t(e.jingle ? 50 : 0),
                                 uint8_t(e.jingle ? 0 : 3)});
            if (e.pointed)
            {
                image_.Put32(pointer, at);
                pointer += 4;
            }
        }
        image_.Put32(kTableData + 4 + uint32_t(entries.size()) * 4, 0xFFFFFFFF);

        image_.Put32(kTableRecord, first_word);
        image_.Put32(kTableRecord + 0x10, kTableData);
        image_.Put32(kTableRecord + 0x14, kTableRecord + kRecordSize);
        image_.Put32(kTableRecord + 0x60, module);
        for (const auto& [offset, value] : bytes)
        {
            image_.Put8(kTableRecord + offset, value);
        }
    }

    // Adds a call of the play routine with the bank at `bank` and the order `order`. With a module's address, the call
    // names the module: ldr r0, =module. With 0, the call loads the module from memory: ldr r0, [r0].
    void AddCall(uint32_t module, uint32_t bank, uint8_t order)
    {
        const uint32_t call = next_call_;
        next_call_ += kCallSize;
        image_.PutHalfwords(call, {uint16_t(module ? 0x4802 : 0x6800), 0x4A03, uint16_t(0x2100 | order)});
        image_.PutBl(call + 6, kPlay);
        image_.Put16(call + 10, 0x4770);
        image_.Put32(call + 12, module);
        image_.Put32(call + 16, bank);
    }

    // Adds a call that sets the music's volume to `volume`: movs r0, #volume; bl; bx lr.
    void AddMusicVolume(uint8_t volume)
    {
        const uint32_t call = next_call_;
        next_call_ += kCallSize;
        image_.Put16(call, uint16_t(0x2000 | volume));
        image_.PutBl(call + 2, kMusicVolume);
        image_.Put16(call + 6, 0x4770);
    }

    // Adds a call of the routine that switches the sound off: push {lr}; bl; pop {pc}. kGame puts the call in the
    // game's code, at kGameCode. The two others put it in a routine of the driver, among the calls, and a call of that
    // routine in the game's code or in another routine of the driver.
    void AddSoundOff(SoundOffCaller caller)
    {
        const auto call = [&](uint32_t at, uint32_t target)
        {
            image_.Put16(at, 0xB500);
            image_.PutBl(at + 2, target);
            image_.Put16(at + 6, 0xBD00);
        };
        if (caller == SoundOffCaller::kGame)
        {
            call(kGameCode, sound_off_);
            return;
        }

        const uint32_t routine = next_call_;
        next_call_ += kCallSize;
        call(routine, sound_off_);
        if (caller == SoundOffCaller::kGameThroughDriver)
        {
            call(kGameCode, routine);
        }
        else
        {
            call(next_call_, routine);
            next_call_ += kCallSize;
        }
    }

    // Adds the mark of the build with long records to the driver's code: instructions that add the size of a long
    // record, in the code after the play routine.
    void UseLongRecords()
    {
        image_.PutHalfwords(kRecordCode, {0x3454, 0x3454, 0x3454, 0x3454});
    }

    // Writes `v` at address `a`, for a test that damages the cartridge.
    void Put32(uint32_t a, uint32_t v)
    {
        image_.Put32(a, v);
    }

    Rom ToRom() const
    {
        return image_.ToRom();
    }

private:
    // Writes a GBAMOD1 module at `address`: the header, the table of steps, the patterns and the points.
    void AddMod1Module(uint32_t address, const TestModule& module)
    {
        image_.Put32(address, kSignature1);
        image_.Put32(address + 4, kMod1Signature2);
        image_.Put32(address + kMod1Rate, module.rate);
        image_.Put32(address + kMod1Orders, uint32_t(module.orders.size()));
        image_.Put32(address + kMod1Patterns, uint32_t(module.patterns.size()));
        for (uint32_t pitch = 0; pitch < uint32_t(kMod1Pitches); pitch++)
        {
            image_.Put32(address + kMod1Steps + pitch * 4, TestMod1Step(pitch, module.rate));
        }
        for (size_t i = 0; i < module.orders.size(); i++)
        {
            image_.Put8(address + kMod1OrderList + uint32_t(i), module.orders[i]);
        }
        uint32_t point_bytes = 0;
        for (uint32_t i = 0; i < uint32_t(kSamples); i++)
        {
            const uint32_t header = address + kMod1Samples + i * kSampleSize;
            image_.Put16(header + kSampleLength, module.samples[i].length);
            image_.Put16(header + kSampleFineTune, module.samples[i].fine_tune);
            image_.Put16(header + kSampleVolume, module.samples[i].volume);
            image_.Put16(header + kSampleLoopStart, module.samples[i].loop_start);
            image_.Put16(header + kSampleLoopLength, module.samples[i].loop_length);
            point_bytes += module.samples[i].length;
        }

        // The patterns, 4 cells a row, and then each sample's points.
        uint32_t at = address + kMod1Size;
        for (size_t p = 0; p < module.patterns.size(); p++)
        {
            image_.Put8(address + kMod1Rows + uint32_t(p), uint8_t(module.patterns[p].size()));
            for (const TestRow& row : module.patterns[p])
            {
                for (uint32_t c = 0; c < 4; c++)
                {
                    const TestCell cell = c < row.size() ? row[c] : TestCell();
                    image_.Put16(at, uint16_t(cell.pitch | (cell.sample << kCellSampleShift)));
                    image_.Put16(at + 2, uint16_t((cell.effect << 8) | cell.operand));
                    at += kCellSize;
                }
            }
        }
        image_.Put32(address + kMod1PatternBytes, at - (address + kMod1Size));
        image_.Put32(address + kMod1PointBytes, point_bytes);
        for (uint32_t i = 0; i < uint32_t(kSamples); i++)
        {
            for (uint32_t k = 0; k < module.samples[i].length; k++)
            {
                const bool given = i < module.points.size() && k < module.points[i].size();
                image_.Put8(at++, given ? uint8_t(module.points[i][k]) : 0);
            }
        }
    }

    CartImage image_;
    uint32_t next_module_ = kFirstModule;
    uint32_t next_call_ = kCalls;
    uint32_t sound_off_ = kSoundOff;
};

} // namespace ntdlib::testing::logikstate_cart
