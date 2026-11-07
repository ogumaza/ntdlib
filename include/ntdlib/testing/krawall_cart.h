// SPDX-License-Identifier: MIT

// Builds small cartridges with Krawall for tests: a synthetic copy of the code patterns that detection reads, the
// player's tables, the game's samples and the modules. A Pattern holds a pattern's rows as Cells, and a Cart places
// modules of patterns.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::krawall_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::krawall::Build;
using ntdlib::krawall::kModeJingle;
using ntdlib::krawall::kModulePatterns;
using ntdlib::krawall::kOrderEnd;
using ntdlib::krawall::kOrderSkip;
using ntdlib::krawall::kPatternRows;
using ntdlib::krawall::PatternData;

// Addresses in a test cartridge: the code patterns that detection reads, the game's calls of the library's settings,
// the game's routine that plays a jingle and its calls, the game's routine that plays an entry of its table of what it
// plays, the version string, the player's tables, the game's tables of samples and modules, the table of what the game
// plays, the samples and the modules.
inline constexpr uint32_t kPlay = kRomBase + 0x100;
inline constexpr uint32_t kTables = kRomBase + 0x140;
inline constexpr uint32_t kTimer = kRomBase + 0x180;
inline constexpr uint32_t kFrame = kRomBase + 0x1C0;
inline constexpr uint32_t kInit = kRomBase + 0x200;
inline constexpr uint32_t kQuality = kRomBase + 0x240;
inline constexpr uint32_t kMaster = kRomBase + 0x280;
inline constexpr uint32_t kGameInit = kRomBase + 0x2C0;
inline constexpr uint32_t kJingle = kRomBase + 0x300;
inline constexpr uint32_t kJingleCalls = kRomBase + 0x340;
inline constexpr uint32_t kPlayEntry = kRomBase + 0x380;
inline constexpr uint32_t kVersion = kRomBase + 0x400;
inline constexpr uint32_t kSineTable = kRomBase + 0x1000;
inline constexpr uint32_t kRampTable = kRomBase + 0x1080;
inline constexpr uint32_t kSquareTable = kRomBase + 0x1100;
inline constexpr uint32_t kRandomTable = kRomBase + 0x1180;
inline constexpr uint32_t kPeriodTable = kRomBase + 0x1200;
inline constexpr uint32_t kFineTuneTable = kRomBase + 0x1300;
inline constexpr uint32_t kLinearTable = kRomBase + 0x1400;
inline constexpr uint32_t kSampleTable = kRomBase + 0x2000;
inline constexpr uint32_t kInstrumentTable = kRomBase + 0x2100;
inline constexpr uint32_t kModuleTable = kRomBase + 0x2200;
inline constexpr uint32_t kPlayTable = kRomBase + 0x2800;
inline constexpr uint32_t kSampleData = kRomBase + 0x3000;
inline constexpr uint32_t kModules = kRomBase + 0x4000;

// The test samples: one that loops forwards, one that doesn't loop, and one that loops back and forth.
inline constexpr uint32_t kForwardPoints = 128, kForwardLoop = 32;
inline constexpr uint32_t kOneShotPoints = 1024;
inline constexpr uint32_t kBidiPoints = 64, kBidiLoop = 16;

// The effects that the test modules use.
inline constexpr uint8_t kSetSpeed = 1, kJump = 4, kBreak = 5, kVolumeSlide = 6, kPortaUp = 15, kOffset = 27, kPan = 34;

// The ticks of a row and the mixer's samples of a tick in the test modules: speed 3, and tempo 125, whose ticks the
// sound timer makes 4 times 245760 / 3000 samples long.
inline constexpr int kSpeed = 3;
inline constexpr uint32_t kTickSamples = 4 * (245760 / 3000);

// The rate that the player plays C-4 at with no fine tune: 8363 Hz, from the clock over C-4's period, 1712.
inline constexpr uint32_t kC4Rate = 14317456 / 1712;

// An event of a test pattern: a row, a channel, and the note, instrument, volume byte, effect and operand, with -1 for
// what the event leaves out.
struct Cell
{
    int row = 0;
    int channel = 0;
    int note = -1;
    int instrument = 0;
    int volume = -1;
    int effect = -1;
    int op = 0;
};

// A test pattern: its rows, and its events.
struct Pattern
{
    int rows = 0;
    std::vector<Cell> cells;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, Krawall's tables,
// and the game's samples and modules.
class Cart : public ntdlib::testing::agb_files::CartImage
{
public:
    explicit Cart(bool code = true) : CartImage(0x10000)
    {
        WriteTables();
        if (code)
        {
            WriteCode();
        }
    }

    void PutString(uint32_t a, const std::string& s)
    {
        for (size_t i = 0; i < s.size(); i++)
        {
            Put8(a + uint32_t(i), uint8_t(s[i]));
        }
    }

    // Writes a module of `channels` channels, at the S3M module's speed and tempo, with `cells` in its one pattern of
    // `rows` rows, and adds it to the game's table of modules if `listed`. The header goes at `at`, if given, as in a
    // game. Returns its header's address.
    uint32_t Module(int channels, int rows, const std::vector<Cell>& cells, bool listed = true, uint32_t at = 0)
    {
        return Module(channels, {{rows, cells}}, listed, at);
    }

    // Writes a module of `channels` channels, at the S3M module's speed and tempo, whose orders play `patterns` in
    // turn, and adds it to the game's table of modules if `listed`. The header goes at `at`, if given, as in a game.
    // Returns its header's address.
    uint32_t Module(int channels, const std::vector<Pattern>& patterns, bool listed = true, uint32_t at = 0)
    {
        std::vector<uint32_t> addresses;
        uint32_t header = next_module_;
        for (const Pattern& p : patterns)
        {
            addresses.push_back(header);
            header = WritePattern(header, p);
        }
        const uint32_t next = header;
        if (at)
        {
            header = at;
            size_ = std::max<size_t>(size_, at - kRomBase + kModulePatterns + 4 * patterns.size() + 0x100);
            Resize(size_);
        }

        // The header: an order for each pattern, the pans, the global volume, speed and tempo, and the patterns'
        // addresses.
        Put8(header, uint8_t(channels));
        Put8(header + 1, uint8_t(patterns.size()));
        Put8(header + 2, 0);
        for (uint32_t o = 0; o < 256; o++)
        {
            Put8(header + 3 + o, o < patterns.size() ? uint8_t(o) : kOrderEnd);
        }
        Put8(header + 0x163, 64);
        Put8(header + 0x164, uint8_t(kSpeed));
        Put8(header + 0x165, 125);
        for (size_t i = 0; i < addresses.size(); i++)
        {
            Put32(header + kModulePatterns + 4 * uint32_t(i), addresses[i]);
        }
        const uint32_t end = at ? next : header + kModulePatterns + 4 * uint32_t(patterns.size());
        next_module_ = (end + 0xFF) & ~0xFFu;
        if (listed)
        {
            Put32(kModuleTable + 4 * uint32_t(listed_++), header);
        }

        return header;
    }

    // Makes the module at `header` hold songs separated by +++. Each song plays its patterns in turn.
    void Songs(uint32_t header, const std::vector<std::vector<uint8_t>>& songs)
    {
        std::vector<uint8_t> orders;
        for (size_t s = 0; s < songs.size(); s++)
        {
            if (s > 0)
            {
                orders.push_back(kOrderSkip);
            }
            Put8(header + 0x123 + uint32_t(s), uint8_t(orders.size()));
            orders.insert(orders.end(), songs[s].begin(), songs[s].end());
        }

        Put8(header + 1, uint8_t(orders.size()));
        for (uint32_t o = 0; o < 256; o++)
        {
            Put8(header + 3 + o, o < orders.size() ? orders[o] : kOrderEnd);
        }
    }

    // Writes the game's routine that plays an entry of its table of modules once, in krapPlay()'s jingle mode. The
    // routine takes the form of the one in Digimon Racing: push {r4, lr}; lsls r0, r0, #24; lsrs r4, r0, #24; ldr r1,
    // =table; lsls r0, r4, #2; adds r0, r0, r1; ldr r0, [r0]; movs r1, #mode; movs r2, #0; bl krapPlay. Digimon Racing
    // gives the jingle mode alone. One of the calls gives entry `entry` with movs r0, #entry. The other passes r5.
    void Jingle(int entry, int mode = kModeJingle)
    {
        PutHalfwords(kJingle,
                     {0xB510, 0x0600, 0x0E04, 0x4906, 0x00A0, 0x1840, 0x6800, uint16_t(0x2100 | mode), 0x2200});
        PutBl(kJingle + 18, kPlay);
        PutHalfwords(kJingle + 22, {0xBC10, 0xBC01, 0x4700});
        Put32(kJingle + 0x20, kModuleTable);
        Put16(kJingleCalls, uint16_t(0x2000 | entry));
        PutBl(kJingleCalls + 2, kJingle);
        Put16(kJingleCalls + 6, 0x1C28);
        PutBl(kJingleCalls + 8, kJingle);
    }

    // Writes the game's routine that plays an entry of its table of what it plays, in the form of Eragon's: ldr r1,
    // =table; lsls r0, r6, #3; adds r0, r0, r1; ldr r3, [r0]; ldr r4, [r0, #4]; ldrb r1, [r0, #4]; lsrs r2, r4, #8;
    // lsls r2, r2, #24; lsrs r2, r2, #24; adds r0, r3, #0; bl krapPlay. The table holds `entries`: a module, a mode
    // and a song each, in 8 bytes.
    void PlayTable(const std::vector<std::array<uint32_t, 3>>& entries)
    {
        PutHalfwords(kPlayEntry, {0x4906, 0x00F0, 0x1840, 0x6803, 0x6844, 0x7901, 0x0A22, 0x0612, 0x0E12, 0x1C18});
        PutBl(kPlayEntry + 20, kPlay);
        Put16(kPlayEntry + 24, 0x4770);
        Put32(kPlayEntry + 28, kPlayTable);

        for (size_t i = 0; i < entries.size(); i++)
        {
            const uint32_t entry = kPlayTable + 8 * uint32_t(i);
            Put32(entry, entries[i][0]);
            Put8(entry + 4, uint8_t(entries[i][1]));
            Put8(entry + 5, uint8_t(entries[i][2]));
        }
    }

    // Writes the game's routine that plays entry n of its table of modules, in the form of Franklin's Great
    // Adventures': push {r4, lr}; adds r4, r0, #0; ldr r0, =modules; lsls r1, r4, #2; adds r0, r1, r0; ldr r0, [r0];
    // ldr r3, =modes; adds r1, r1, r3; ldr r1, [r1]; bl krapPlay; ldr r0, =volumes; adds r0, r4, r0; ldrb r0, [r0];
    // movs r1, #0; bl krapSetMusicVol. The routine reads the table of modules at `modules`. The table of modes holds
    // a word for each entry, and the table of volumes a byte.
    void ModuleSettings(uint32_t modules, const std::vector<uint32_t>& modes, const std::vector<uint8_t>& volumes)
    {
        PutHalfwords(kPlayEntry, {0xB510, 0x1C04, 0x4807, 0x00A1, 0x1808, 0x6800, 0x4B06, 0x18C9, 0x6809});
        PutBl(kPlayEntry + 18, kPlay);
        PutHalfwords(kPlayEntry + 22, {0x4805, 0x1820, 0x7800, 0x2100});
        PutBl(kPlayEntry + 30, kPlayEntry + 34);
        Put16(kPlayEntry + 34, 0x4770);
        Put32(kPlayEntry + 36, modules);
        Put32(kPlayEntry + 40, kPlayTable);
        Put32(kPlayEntry + 44, kPlayTable + 0x100);
        for (size_t i = 0; i < modes.size(); i++)
        {
            Put32(kPlayTable + 4 * uint32_t(i), modes[i]);
            Put8(kPlayTable + 0x100 + uint32_t(i), volumes[i]);
        }
    }

private:
    // Writes pattern `p` at `pattern`: an index of every 4th row's data, the rows and the data. Returns the next word's
    // address after it.
    uint32_t WritePattern(uint32_t pattern, const Pattern& p)
    {
        std::vector<uint8_t> data;
        for (int row = 0; row < p.rows; row++)
        {
            if (row % 4 == 0)
            {
                Put16(pattern + 2 * uint32_t(row / 4), uint16_t(data.size()));
            }
            for (const Cell& c : p.cells)
            {
                if (c.row != row)
                {
                    continue;
                }

                const uint8_t follow = uint8_t(c.channel | (c.note >= 0 ? 0x20 : 0) | (c.volume >= 0 ? 0x40 : 0) |
                                               (c.effect >= 0 ? 0x80 : 0));
                data.push_back(follow);
                if (c.note >= 0)
                {
                    data.push_back(uint8_t(c.note << 1 | (c.instrument >> 8)));
                    data.push_back(uint8_t(c.instrument));
                }
                if (c.volume >= 0)
                {
                    data.push_back(uint8_t(c.volume));
                }
                if (c.effect >= 0)
                {
                    data.push_back(uint8_t(c.effect));
                    data.push_back(uint8_t(c.op));
                }
            }
            data.push_back(0);
        }
        for (int i = (p.rows + 3) / 4; i < 16; i++)
        {
            Put16(pattern + 2 * uint32_t(i), Get16(pattern + 2 * uint32_t((p.rows - 1) / 4)));
        }
        Put8(pattern + kPatternRows, uint8_t(p.rows));
        const uint32_t start = PatternData(Build::k2003_09_01, pattern);
        for (size_t i = 0; i < data.size(); i++)
        {
            Put8(start + uint32_t(i), data[i]);
        }

        return (start + uint32_t(data.size()) + 3) & ~3u;
    }

    // Writes the player's tables: the waves of the vibrato, the periods of the notes from C-0, each octave's half the
    // last's, the fine tunes and the linear frequencies, with the values that detection looks for first. Then the
    // samples and their table.
    void WriteTables()
    {
        const double kPi = std::acos(-1.0);
        const int16_t kRandom[16] = {-28,  185,  162, -32, -161, 170,  -222, -186,
                                     -177, -193, -2,  232, -252, -133, 121,  -127};
        for (uint32_t i = 0; i < 64; i++)
        {
            Put16(kSineTable + 2 * i, uint16_t(int16_t(std::lround(255 * std::sin(2 * kPi * i / 64)))));
            Put16(kRampTable + 2 * i, uint16_t(int16_t(i < 32 ? 8 * i : 8 * (int(i) - 64))));
            Put16(kSquareTable + 2 * i, uint16_t(int16_t(i < 32 ? 255 : -255)));
            Put16(kRandomTable + 2 * i, uint16_t(i < 16 ? kRandom[i] : int16_t(i * 7)));
        }

        const uint16_t kPeriods[12] = {27392, 25855, 24403, 23034, 21741, 20521,
                                       19369, 18282, 17256, 16287, 15373, 14510};
        for (uint32_t n = 0; n < 120; n++)
        {
            Put16(kPeriodTable + 2 * n, uint16_t(kPeriods[n % 12] >> (n / 12)));
        }

        const uint16_t kFineTunes[8] = {34716, 34654, 34591, 34529, 34467, 34405, 34343, 34281};
        for (uint32_t i = 0; i < 64; i++)
        {
            Put16(kFineTuneTable + 2 * i,
                  i < 8 ? kFineTunes[i] : uint16_t(std::lround(32768 * std::pow(2.0, (32.0 - i) / 32 / 12))));
        }

        const uint32_t kLinear[6] = {1096155136, 1097144902, 1098135561, 1099127114, 1100119563, 1101112908};
        for (uint32_t i = 0; i < 768; i++)
        {
            Put32(kLinearTable + 4 * i,
                  i < 6 ? kLinear[i] : uint32_t(std::lround(1096155136 * std::pow(2.0, i / 768.0))));
        }

        // Each sample's header: its loop's length, its end, its rate at C-4, fine tune, relative note, volume, pan,
        // loop and interpolation, then its points.
        struct TestSample
        {
            uint32_t points;
            uint32_t loop_length;
            uint8_t loop;
            uint8_t volume;
        };
        const TestSample kSamples[3] = {
            {kForwardPoints, kForwardLoop, 1, 64}, {kOneShotPoints, 0, 0, 48}, {kBidiPoints, kBidiLoop, 2, 40}};
        uint32_t at = kSampleData;
        for (uint32_t s = 0; s < 3; s++)
        {
            Put32(kSampleTable + 4 * s, at);
            Put32(at, kSamples[s].loop_length);
            Put32(at + 4, at + 0x12 + kSamples[s].points);
            Put32(at + 8, 8363);
            Put8(at + 0xE, kSamples[s].volume);
            Put8(at + 0x10, kSamples[s].loop);
            for (uint32_t i = 0; i < kSamples[s].points; i++)
            {
                Put8(at + 0x12 + i, uint8_t(128 + 100 * std::sin(2 * kPi * i / 16)));
            }
            at = (at + 0x12 + kSamples[s].points + 3) & ~3u;
        }
    }

    // Writes the patterns of krapPlay(), the loads of the tables in the row's routine, the timers' setup,
    // getDmaAddress(), kragInit(), kramQualityMode() and kramSetMasterVol(), the game's calls of the last three, and
    // the version string.
    void WriteCode()
    {
        PutHalfwords(kPlay,
                     {0xB5F0, 0x4657, 0x464E, 0x4645, 0xB4E0, 0x1C0F, 0x4680, 0x4692, 0x1C38, 0x2204, 0x4010, 0x2800});
        PutHalfwords(kTables,
                     {0x0407, 0x4907, 0x0BBA, 0x588F, 0x0071, 0x616F, 0x3902, 0x5A78, 0x4F04, 0x0082, 0x58B8, 0x6128});
        Put32(kTables + 0x20, kInstrumentTable);
        Put32(kTables + 0x24, kSampleTable);
        PutHalfwords(kTimer, {0x4800, 0x4900, 0x4B00, 0x8008, 0x3102, 0x4805, 0x800B, 0x3906, 0x8008});
        Put32(kTimer + 0x20, 0xFC00);
        PutHalfwords(kFrame, {0x4900, 0x238A, 0x0058, 0x680B, 0x181A, 0x600A});
        PutHalfwords(kInit, {0xB510, 0x1C04, 0x2080, 0xF000, 0xF93B, 0x1C20, 0xF000, 0xF800, 0xF000, 0xF800});
        PutHalfwords(kQuality, {0xB530, 0x2210, 0x4002, 0x2A00, 0xD000, 0x4900, 0x2301, 0x700B});
        PutHalfwords(kMaster, {0xB5F0, 0x1C04, 0x0C20, 0x2800, 0xD000, 0x4900});

        // kragInit(1), kramSetMasterVol(0x40080) with the level from a literal, and kramQualityMode(0).
        Put16(kGameInit, 0x2001);
        PutBl(kGameInit + 2, kInit);
        Put16(kGameInit + 6, 0x4806);
        Put32(kGameInit + 0x20, 0x40080);
        PutBl(kGameInit + 8, kMaster);
        Put16(kGameInit + 0xC, 0x2000);
        PutBl(kGameInit + 0xE, kQuality);
        PutString(kVersion, "$Id: Krawall $Date: 2003/09/01 06:51:01 $");
    }

    uint32_t next_module_ = kModules;
    size_t size_ = 0x10000;
    int listed_ = 0;
};

} // namespace ntdlib::testing::krawall_cart
