// SPDX-License-Identifier: MIT

// Builds small cartridges with Rare's GBA driver for tests: a synthetic copy of the code patterns that the driver's
// detection reads, with tunes, instruments and samples placed one after another. A Track holds a track's commands, and
// a Cart places them as tunes.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/rare/song.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::rare_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::rare::Format;
using ntdlib::rare::kCmdBend;
using ntdlib::rare::kCmdControl;
using ntdlib::rare::kCmdDelay1;
using ntdlib::rare::kCmdDelay2;
using ntdlib::rare::kCmdDelay3;
using ntdlib::rare::kCmdEnd;
using ntdlib::rare::kCmdNoteOff;
using ntdlib::rare::kCmdNoteOn;
using ntdlib::rare::kCmdNoteOnB;
using ntdlib::rare::kCmdProgram;
using ntdlib::rare::kCmdTempo;
using ntdlib::rare::kInstSample;
using ntdlib::rare::kLoopForward;
using ntdlib::rare::kLoopOnce;

// Addresses of the synthetic driver's components in a test cartridge.
inline constexpr uint32_t kInit = kRomBase + 0x100;     // the init routine (Thumb)
inline constexpr uint32_t kInitPool = kRomBase + 0x180; // its literal pool
inline constexpr uint32_t kCodeSize = kRomBase + 0x1F0; // a word: the size of the driver's code
inline constexpr uint32_t kCode = kRomBase + 0x200;     // the driver's code (ARM), which the game copies to IWRAM
inline constexpr uint32_t kCodeBytes = 0x200;
inline constexpr uint32_t kChannelSize = kRomBase + 0x400; // a word: the size of a channel's note slots
inline constexpr uint32_t kPitchTable = kRomBase + 0x600;  // 2^(n/12) for n = -64..64, with entry 0 here
inline constexpr uint32_t kSineTable = kRomBase + 0x800;
inline constexpr uint32_t kFadeTable = kRomBase + 0xC10;
inline constexpr uint32_t kTuneTable = kRomBase + 0x1000;
inline constexpr uint32_t kData = kRomBase + 0x1100; // storage for tunes, instruments and samples

// A cartridge image being put together: a synthetic copy of the driver's code patterns that detection reads, and tunes,
// instruments and samples placed one after another.
class Cart : public ntdlib::testing::agb_files::CartImage
{
public:
    // Makes a cartridge whose driver reads the given format, with `slots` note slots for each channel. Without `code`,
    // it has no driver code, only the tune table.
    explicit Cart(Format format, int slots = 6, bool code = true) : CartImage(0x40000), next_(kData)
    {
        if (code)
        {
            WriteDriver(format, slots);
        }
    }

    // Places `bytes` at the next free address, aligned to `align`. Returns the address.
    uint32_t Place(const std::vector<uint8_t>& bytes, uint32_t align = 4)
    {
        next_ = (next_ + align - 1) & ~(align - 1);
        const uint32_t at = next_;
        PutBytes(at, bytes);
        next_ += uint32_t(bytes.size());

        return at;
    }

    // Places a sample instrument whose sample holds `pcm`, with the driver's guard byte after it. A loop covers its
    // last `loop` bytes.
    uint32_t Sample(const std::vector<int8_t>& pcm, uint32_t loop = 0, uint32_t rate = 11025, uint32_t root = 60,
                    std::vector<uint32_t> envelope = {99, 99, 99, 99}, int32_t fine_tune = 0, int32_t bend_range = 2,
                    uint32_t vibrato_rate = 0, int32_t vibrato_depth = 0)
    {
        std::vector<uint8_t> data(pcm.begin(), pcm.end());
        data.push_back(loop ? data[data.size() - loop] : data.back());
        const uint32_t start = Place(data, 1);
        const uint32_t end = start + uint32_t(pcm.size());

        return Instrument({kInstSample, loop ? kLoopForward : kLoopOnce, rate, root, start, loop ? loop : end - start,
                           end, 0, 0, envelope[0], envelope[1], envelope[2], envelope[3], uint32_t(fine_tune),
                           uint32_t(bend_range), vibrato_rate, uint32_t(vibrato_depth)});
    }

    // Places a drum kit or key split, with a bend range of 2 semitones, whose key map gives each key in `keys` the
    // instrument after it.
    uint32_t Split(uint32_t type, const std::vector<std::pair<std::pair<int, int>, uint32_t>>& keys)
    {
        std::vector<uint8_t> map(128, 0xFF);
        std::vector<uint8_t> table;
        for (size_t i = 0; i < keys.size(); i++)
        {
            for (int k = keys[i].first.first; k <= keys[i].first.second; k++)
            {
                map[size_t(k)] = uint8_t(i);
            }
            for (int b = 0; b < 4; b++)
            {
                table.push_back(uint8_t(keys[i].second >> (8 * b)));
            }
        }

        const uint32_t map_at = Place(map, 1);
        const uint32_t table_at = Place(table);
        return Instrument({type, 0xFFFFFFFF, 0, 0, 0, 0, 0, map_at, table_at, 0, 0, 0, 0, 0, 2, 0, 0});
    }

    // Places a program map and instrument table that give each program in `programs` its instrument. Returns the map
    // and the table.
    std::pair<uint32_t, uint32_t> Bank(const std::vector<std::pair<int, uint32_t>>& programs)
    {
        std::vector<uint8_t> map(128, 0xFF);
        std::vector<uint8_t> table;
        for (size_t i = 0; i < programs.size(); i++)
        {
            map[size_t(programs[i].first)] = uint8_t(i);
            for (int b = 0; b < 4; b++)
            {
                table.push_back(uint8_t(programs[i].second >> (8 * b)));
            }
        }

        const uint32_t map_at = Place(map, 1);
        return {map_at, Place(table)};
    }

    // Places a tune with these tracks, at 480 ticks to the quarter note, and adds it to the tune table. Returns its
    // header's address.
    uint32_t Tune(const std::vector<std::vector<uint8_t>>& tracks, std::pair<uint32_t, uint32_t> bank)
    {
        std::vector<uint8_t> list;
        for (const std::vector<uint8_t>& t : tracks)
        {
            const uint32_t at = Place(t, 1);
            for (int b = 0; b < 4; b++)
            {
                list.push_back(uint8_t(at >> (8 * b)));
            }
        }

        const uint32_t list_at = Place(list);
        std::vector<uint8_t> header;
        for (uint32_t v : {uint32_t(tracks.size()), uint32_t(480), list_at, bank.first, bank.second})
        {
            for (int b = 0; b < 4; b++)
            {
                header.push_back(uint8_t(v >> (8 * b)));
            }
        }

        const uint32_t header_at = Place(header);
        Put32(kTuneTable + 4 * tunes_++, header_at);

        return header_at;
    }

    // Adds the code from which detection tells the revision of Donkey Kong Country 3: the controller handler's
    // comparisons with 20 to 23, and the routine that moves the notes on storing the free state of loop modes 3 and 1
    // through r0.
    void AddEnvelopeControllers()
    {
        const uint32_t kRevision[] = {0xE3520014, 0x0A000000, 0xE3520015, 0x0A000000, 0xE3520016, 0x0A000000,
                                      0xE3520017, 0xE3A03010, 0xE5C03000, 0xEA000002, 0xE3A03010, 0xE5C03000};
        for (uint32_t i = 0; i < std::size(kRevision); i++)
        {
            Put32(kCode + 0x100 + 4 * i, kRevision[i]);
        }
    }

    // Adds the test in the mixer of Donkey Kong Country 3's revision that leaves out a note at a level of 0: mul r4,
    // r3, r4; lsr r4, r4, #7; cmp r4, #0; beq.
    void AddLevelTest()
    {
        const uint32_t kTest[] = {0xE0040493, 0xE1A043A4, 0xE3540000, 0x0A000000};
        for (uint32_t i = 0; i < std::size(kTest); i++)
        {
            Put32(kCode + 0x180 + 4 * i, kTest[i]);
        }
    }

    // Places the parts of Donkey Kong Country 3's fading routine that detection reads: the check of tune `tune`, and
    // the moves that pick the channels to fade out and in. Above water, the routine fades out the channels from
    // `under_first` to `under_last` and fades in those from `above_first` to `above_last`. Under water, it does the
    // reverse. Returns the routine's address.
    uint32_t AddWaterRoutine(int tune, int above_first, int above_last, int under_first, int under_last)
    {
        // ldr r0, [r0]; cmp r0, #tune; bne; movs r0, #1; mov r8, r0. Then mov r1, r8; cmp r1, #0; beq to the moves for
        // the state under water.
        std::vector<uint16_t> code = {0x6800, uint16_t(0x2800 | tune), 0xD1FE, 0x2001, 0x4680, 0x4641, 0x2900, 0xD005};

        // Above water: movs r2, #first out; movs r5, #last out; movs r0, #first in; mov ip, r0; movs r3, #last in; b.
        code.insert(code.end(), {uint16_t(0x2200 | under_first), uint16_t(0x2500 | under_last),
                                 uint16_t(0x2000 | above_first), 0x4684, uint16_t(0x2300 | above_last), 0xE004});

        // Under water: the same moves, with r1 in place of r0.
        code.insert(code.end(), {uint16_t(0x2200 | above_first), uint16_t(0x2500 | above_last),
                                 uint16_t(0x2100 | under_first), 0x468C, uint16_t(0x2300 | under_last)});

        std::vector<uint8_t> bytes;
        for (uint16_t h : code)
        {
            bytes.push_back(uint8_t(h));
            bytes.push_back(uint8_t(h >> 8));
        }

        return Place(bytes, 2);
    }

private:
    uint32_t Instrument(const std::vector<uint32_t>& words)
    {
        std::vector<uint8_t> bytes;
        for (uint32_t v : words)
        {
            for (int b = 0; b < 4; b++)
            {
                bytes.push_back(uint8_t(v >> (8 * b)));
            }
        }

        return Place(bytes);
    }

    // Writes the parts of the driver that detection reads: the init's copy of the driver's code and its table setup,
    // and in the code, the command reader, the note on's slot size, and the pitch, vibrato and fade tables.
    void WriteDriver(Format format, int slots)
    {
        auto thumb_ldr = [&](uint32_t at, int reg, uint32_t literal)
        {
            Put16(at, uint16_t(((0x48 | reg) << 8) | ((literal - ((at + 4) & ~3u)) / 4)));
        };

        auto bl = [&](uint32_t at)
        {
            Put16(at, 0xF000);
            Put16(at + 2, 0xF800);
        };

        // The init, and its copy of the driver's code.
        Put16(kInit, 0xB5F0); // push {r4-r7, lr}
        constexpr uint32_t kCopy = kInit + 0x10;
        thumb_ldr(kCopy, 1, kInitPool);
        thumb_ldr(kCopy + 2, 2, kInitPool + 4);
        thumb_ldr(kCopy + 4, 3, kInitPool + 8);
        Put16(kCopy + 6, 0x681B); // ldr r3, [r3]
        thumb_ldr(kCopy + 8, 4, kInitPool + 12);
        Put16(kCopy + 10, 0x42A3); // cmp r3, r4

        // The table setup after the copy.
        constexpr uint32_t kSetup = kCopy + 0x14;
        thumb_ldr(kSetup, 0, kInitPool + 16);
        thumb_ldr(kSetup + 2, 1, kInitPool + 20);
        Put16(kSetup + 4, 0x6001); // str r1, [r0]
        thumb_ldr(kSetup + 6, 0, kInitPool + 24);
        thumb_ldr(kSetup + 8, 1, kInitPool + 28);
        Put16(kSetup + 10, 0x6001);
        thumb_ldr(kSetup + 12, 0, kInitPool + 32);
        bl(kSetup + 14);
        Put16(kSetup + 18, 0x2008); // movs r0, #8
        bl(kSetup + 20);

        // The init's literal pool, and the size of the driver's code.
        const uint32_t kPool[] = {kCode, 0x03002000, kCodeSize, 0x13EC, 0x03004004, kTuneTable, 0x030014F0, 0, 13379};
        for (uint32_t i = 0; i < 9; i++)
        {
            Put32(kInitPool + 4 * i, kPool[i]);
        }
        Put32(kCodeSize, kCodeBytes);

        // The driver's code, with its literal pool at the end.
        std::vector<uint32_t> code = {0xE4DB0001};
        if (format == Format::kChannelNibble)
        {
            code.insert(code.end(), {0xE1A01220, 0xE3C000F0});
        }
        code.push_back(0xE79C0100);

        constexpr uint32_t kLiterals = kCode + kCodeBytes - 16;
        const std::pair<uint32_t, uint32_t> kLoads[] = {
            {0xE59F4000, 0xE5944000}, {0xE59F6000, 0xE0862103}, {0xE59F6000, 0xE0866101}, {0xE59F2000, 0xE7D21001}};
        const uint32_t kValues[] = {kChannelSize, kPitchTable, kSineTable, kFadeTable};
        for (int i = 0; i < 4; i++)
        {
            const uint32_t at = kCode + 4 * uint32_t(code.size());
            code.push_back(kLoads[i].first | (kLiterals + 4 * uint32_t(i) - (at + 8)));
            code.push_back(kLoads[i].second);
            Put32(kLiterals + 4 * uint32_t(i), kValues[i]);
        }

        for (size_t i = 0; i < code.size(); i++)
        {
            Put32(kCode + 4 * uint32_t(i), code[i]);
        }
        Put32(kChannelSize, uint32_t(slots) * 0x28);

        // The tables: 2^(n/12) in 9.23 fixed point, the vibrato's sine, and a fade table whose entry i is 99 - i, so
        // that a fade with index i takes 100 - i frames, and one with index 99 none.
        for (int n = -64; n <= 64; n++)
        {
            Put32(kPitchTable + uint32_t(n * 4), uint32_t(std::lround(8388608.0 * std::pow(2.0, n / 12.0))));
        }
        for (int i = 0; i <= 256; i++)
        {
            Put32(kSineTable + uint32_t(i) * 4,
                  uint32_t(int32_t(std::lround(335544.32 * std::sin(i * 6.283185307179586 / 256)))));
        }
        for (uint32_t i = 0; i < 100; i++)
        {
            Put8(kFadeTable + i, uint8_t(99 - i));
        }
    }

    uint32_t next_;
    uint32_t tunes_ = 0;
};

// A track's commands being written, in one of the two formats.
class Track
{
public:
    explicit Track(Format format) : format_(format)
    {
    }

    Track& Tempo(uint32_t micros)
    {
        return Command(kCmdTempo, 0, {uint8_t(micros), uint8_t(micros >> 8), uint8_t(micros >> 16)});
    }

    // Adds a delay in the shortest command that holds it.
    Track& Wait(uint32_t ticks)
    {
        if (ticks < 0x100)
        {
            return Command(kCmdDelay1, 0, {uint8_t(ticks)});
        }
        if (ticks < 0x10000)
        {
            return Command(kCmdDelay2, 0, {uint8_t(ticks), uint8_t(ticks >> 8)});
        }

        return Command(kCmdDelay3, 0, {uint8_t(ticks), uint8_t(ticks >> 8), uint8_t(ticks >> 16)});
    }

    Track& On(int ch, int key, int velocity)
    {
        return Command(kCmdNoteOn, ch, {uint8_t(key), uint8_t(velocity)});
    }

    Track& Off(int ch, int key)
    {
        return Command(kCmdNoteOff, ch, {uint8_t(key), 0x40});
    }

    Track& Control(int ch, int controller, int value)
    {
        return Command(kCmdControl, ch, {uint8_t(controller), uint8_t(value)});
    }

    Track& Program(int ch, int program)
    {
        return Command(kCmdProgram, ch, {uint8_t(program)});
    }

    Track& Bend(int ch, int value)
    {
        return Command(kCmdBend, ch, {uint8_t(value), uint8_t(value >> 8)});
    }

    std::vector<uint8_t> End()
    {
        Command(kCmdEnd, 0, {});

        return bytes_;
    }

private:
    Track& Command(uint8_t command, int ch, std::vector<uint8_t> args)
    {
        const bool channel = command >= kCmdNoteOnB && command <= kCmdBend;
        if (format_ == Format::kChannelNibble)
        {
            bytes_.push_back(uint8_t(command | (channel ? ch << 4 : 0)));
        }
        else
        {
            bytes_.push_back(command);
            if (channel)
            {
                bytes_.push_back(uint8_t(ch));
            }
        }
        bytes_.insert(bytes_.end(), args.begin(), args.end());

        return *this;
    }

    Format format_;
    std::vector<uint8_t> bytes_;
};

// Returns a sample of `length` bytes of a saw wave.
inline std::vector<int8_t> Saw(size_t length = 64)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 4 % 256) - 128));
    }

    return pcm;
}

} // namespace ntdlib::testing::rare_cart
