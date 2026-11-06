// SPDX-License-Identifier: MIT

#include "ntdlib/mp2k/driver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/thumb.h"
#include "ntdlib/mp2k/song.h"

namespace ntdlib::mp2k
{
using ntdlib::agb::BlTarget;
using ntdlib::agb::FindThumb;
using ntdlib::agb::Hex;
using ntdlib::agb::kRomBase;
using ntdlib::agb::ParseThumbPattern;
using ntdlib::agb::Rom;
using ntdlib::agb::ThumbLiteral;

namespace
{

// The mixer's output for a frame at each of its rates, from the driver's table.
constexpr uint16_t kSamplesPerFrame[12] = {96, 132, 176, 224, 264, 304, 352, 448, 528, 608, 672, 704};

// Maximum numbers of songs and music players to read during detection.
constexpr int kMaxSongs = 4096;
constexpr int kMaxPlayers = 64;

// A game whose code gives some songs another number of DirectSound channels. Before the game starts a song up to 127,
// its routine at `routine` calls m4aSoundMode with `channels` for each of `songs`, and with the init's number for the
// other songs. The songs above 127 keep the number that the last song left. `code` is the routine's code up to that
// call, in ParseThumbPattern()'s form. Its comparisons hold the songs' numbers. A game whose routine differs keeps the
// init's number for every song.
struct ChannelSetting
{
    std::string_view game_code;
    uint32_t routine;
    std::string_view code;
    int channels;
    std::span<const int> songs;
};

constexpr int kFireEmblemSongs[] = {41, 42, 90, 92, 95, 116};
constexpr int kSacredStonesSongs[] = {1, 2, 64, 66, 67, 86, 116};

constexpr ChannelSetting kChannelSettings[] = {
    // Fire Emblem (The Blazing Blade in Japan) starts a song through its routine at 0x08003F14. That routine calls this
    // one.
    {.game_code = "AE7E",
     .routine = 0x08003FC0,
     .code = "B580 B081 466F 6038 6838 285A D010 285A DC04 282A DC18 2829 DB16 E009 285F D007 285F DC02 285C D003 "
             "E00E 2874 D000 E00B 4804 2108 5641 2908 D002 2008 F7FF FFC6",
     .channels = 8,
     .songs = kFireEmblemSongs},
    // Fire Emblem: The Sacred Stones starts a song through its routine at 0x08002890. That routine calls this one.
    {.game_code = "BE8E",
     .routine = 0x080028FC,
     .code = "B500 2843 DC08 2842 DA0A 2801 DB14 2802 DD06 2840 D004 E00F 2856 D001 2874 D10B 4804 7A00 0600 1600 "
             "2808 D00E 2008 F7FF FFDD",
     .channels = 8,
     .songs = kSacredStonesSongs},
};

// The routine that starts a song, m4aSongNumStart: push {lr}; lsls r0, r0, #16; ldr r2, =players; ldr r1, =songs;
// lsrs r0, r0, #13; adds r0, r0, r1; ldrh r3, [r0, #4]; lsls r1, r3, #1; adds r1, r1, r3; lsls r1, r1, #2;
// adds r1, r1, r2; ldr r2, [r1]; ldr r1, [r0]; adds r0, r2, #0; bl MPlayStart; pop {r0}; bx r0. Some games' compiler
// swaps r2 and r3: ldr r3, =players and ldrh r2, [r0, #4], then lsls r1, r2, #1; adds r1, r1, r2; lsls r1, r1, #2;
// adds r1, r1, r3. The patterns end before the bl. The two loads' offsets aren't compared.
constexpr std::string_view kSongStart[] = {"B500 0400 4Axx 49xx 0B40 1840 8883 0059 18C9 0089 1889 680A 6801 1C10",
                                           "B500 0400 4Bxx 49xx 0B40 1840 8882 0051 1889 0089 18C9 680A 6801 1C10"};

// The offset of the routine's bl MPlayStart, and the routine's length, in bytes.
constexpr uint32_t kSongStartBl = 28;
constexpr uint32_t kSongStartSize = 36;

// The mixer's test for a voice that plays its sample backwards or a compressed sample, in the ARM code the init copies
// to IWRAM: ldrb r0, [r4, #1]; tst r0, #0x30. Older versions of the driver don't have it.
constexpr uint8_t kSpecialTest[8] = {0x01, 0x00, 0xD4, 0xE5, 0x30, 0x00, 0x10, 0xE3};

// The mono mixer's volume for a channel, in Thumb code: ldrb r0, [r4, #2]; ldrb r1, [r4, #3]; adds r0, r0, r1;
// muls r0, r5, r0; lsrs r0, r0, #9; strb r0, [r4, #10]. It plays the channel at the average of its right and left
// volumes, on both sides, through FIFO A alone.
constexpr uint8_t kMonoVolume[12] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};

// Camelot's note start, in Thumb code, where it looks through the DirectSound channels for one to take: adds r4, #0x50;
// movs r7, #0xc7; movs r2, #0; movs r6, #0; movs r0, #3; ldrb r1, [r4]. A movs r3 before it holds the count of
// channels.
constexpr uint8_t kCamelotChannelSearch[12] = {0x50, 0x34, 0xC7, 0x27, 0x00, 0x22, 0x00, 0x26, 0x03, 0x20, 0x21, 0x78};

// Where a later version's SoundMain goes to its mixer with a count of 12 channels, whatever the driver's mode says:
// movs r0, #12; adds r4, #0x50; ldr r3, [pc, #0]; bx r3. The earlier one loads the count from the SoundInfo. The later
// note start looks through as many.
constexpr uint8_t kCamelotTwelveChannels[8] = {0x0C, 0x20, 0x50, 0x34, 0x00, 0x4B, 0x18, 0x47};

// A later version's note start, where a player's priority of 120 gives its notes a priority of 255: ldrb r1, [r6, #9];
// cmp r1, #0x78; bne; movs r0, #0xff; b; ldrb r0, [r5, #0x1d]; adds r0, r0, r1; cmp r0, #0xfe.
constexpr uint8_t kCamelotTopPriority[16] = {0x71, 0x7A, 0x78, 0x29, 0x01, 0xD1, 0xFF, 0x20,
                                             0x04, 0xE0, 0x68, 0x7F, 0x40, 0x18, 0xFE, 0x28};

// Camelot's mixer, in the ARM code the game copies to IWRAM, where it moves a pulse wave's duty on: ldrb r6, [r3, #2];
// add r2, r2, r6, lsl #24; ldrb r6, [r3, #4]; adds r6, r2, r6, lsl #24; mvnmi r6, r6.
constexpr uint8_t kCamelotPulse[20] = {0x02, 0x60, 0xD3, 0xE5, 0x06, 0x2C, 0x82, 0xE0, 0x04, 0x60,
                                       0xD3, 0xE5, 0x06, 0x6C, 0x92, 0xE0, 0x06, 0x60, 0xE0, 0x41};

// The output stage of a Camelot mixer that writes one point for each of its points: mov r8, #0xFF000000;
// orr r8, r8, r8, lsr #16; and lr, r8, r8, lsl #7; mov r3, #1584. The other writes two, with 3168 bytes a side.
constexpr uint8_t kCamelotOnePoint[16] = {0xFF, 0x84, 0xA0, 0xE3, 0x28, 0x88, 0x88, 0xE1,
                                          0x88, 0xE3, 0x08, 0xE0, 0x63, 0x3E, 0xA0, 0xE3};

// Where a Camelot mixer's path for fixed-pitch samples continues a sample of 4-bit steps: tst r0, #32;
// ldrne r7, [r4, #0x1c].
constexpr uint8_t kCamelotAdpcm[8] = {0x20, 0x00, 0x10, 0xE3, 0x1C, 0x70, 0x94, 0x15};

// The TEMPO command's handler of the sequencer that Metroid Fusion and Metroid: Zero Mission share: ldr r2, [r1,
// #0x24]; ldrb r3, [r2, #1]; adds r2, #2; str r2, [r1, #0x24]; lsls r2, r3, #1; strh r2, [r0, #0xa]; cmp r2, #0x96. The
// SDK's library has no such code. The sequencer reads its commands in other ways.
constexpr uint8_t kMetroidTempo[14] = {0x4A, 0x6A, 0x53, 0x78, 0x02, 0x32, 0x4A,
                                       0x62, 0x5A, 0x00, 0x42, 0x81, 0x96, 0x2A};

// The end of Metroid Fusion's read of a note's arguments, after the key: ldrb r2, [r1, #1]; movs r0, #1; ldrsb r0, [r1,
// r0]; cmp r0, #0; blt; strb r2, [r4, #0xf]; adds r0, r3, #1; str r0, [r4, #0x24]; ldrb r1, [r4] (0x080017B2). It reads
// no third byte. Metroid: Zero Mission reads a third byte. As in the SDK's library, that byte adds to the note's
// length.
constexpr uint8_t kMetroidShortNote[18] = {0x4A, 0x78, 0x01, 0x20, 0x08, 0x56, 0x00, 0x28, 0x02,
                                           0xDB, 0xE2, 0x73, 0x58, 0x1C, 0x60, 0x62, 0x21, 0x78};

// The distance, in bytes, that the literals of a Camelot mixer's echo can lie before kCamelotPulse.
constexpr uint32_t kCamelotEchoReach = 0x400;

// The greatest distance, in bytes, that the init's setting of the driver's mode can be before its literal pool.
constexpr uint32_t kInitReach = 0x200;

// The Thumb instruction mov r8, r0.
constexpr uint16_t kMovR8R0 = 0x4680;

// Returns true if a Thumb BL instruction starts at `at`.
bool IsBl(const Rom& rom, uint32_t at)
{
    return (rom.U16(at) & 0xF800) == 0xF000 && (rom.U16(at + 2) & 0xF800) == 0xF800;
}

// Returns true if `address` is in the GBA's work RAM, where the driver keeps its music players.
bool InRam(uint32_t address)
{
    return (address >= 0x02000000 && address < 0x02040000) || (address >= 0x03000000 && address < 0x03008000);
}

// Finds the routine that starts a song, and reads the addresses of the music player table and the song table from its
// literal pool. A copy of the routine whose pool doesn't hold the tables, such as an unused one whose pool is zeroed,
// is passed over. Returns false if there's none.
bool FindSongStart(const Rom& rom, DriverInfo& info)
{
    // The matches of both forms, in address order.
    std::vector<uint32_t> hits;
    for (std::string_view form : kSongStart)
    {
        const std::vector<uint32_t> found = FindThumb(rom, ParseThumbPattern(form));
        hits.insert(hits.end(), found.begin(), found.end());
    }
    std::sort(hits.begin(), hits.end());

    for (uint32_t at : hits)
    {
        if (!rom.Contains(at, kSongStartSize) || !IsBl(rom, at + kSongStartBl) ||
            rom.U16(at + kSongStartBl + 4) != 0xBC01)
        {
            continue;
        }

        const uint32_t player_table = ThumbLiteral(rom, at + 4);
        const uint32_t song_table = ThumbLiteral(rom, at + 6);
        if (!rom.Contains(player_table, 12) || !rom.Contains(song_table, 8))
        {
            continue;
        }

        info.song_start = at;
        info.player_table = player_table;
        info.song_table = song_table;

        return true;
    }

    return false;
}

// Returns true if `mode` is a setting the driver's init could give it: a rate, the output's resolution, and nothing in
// the top byte but Metroid Fusion's 1. Its routine reads that as stereo output.
bool IsSoundMode(uint32_t mode)
{
    const uint32_t rate = (mode >> 16) & 0xF;
    const uint32_t dac = (mode >> 20) & 0xF;
    return (mode >> 24) <= 1 && rate >= 1 && rate <= 12 && dac >= 8 && dac <= 11;
}

// Finds the init's setting of the driver's mode: `ldr r0, =mode; bl m4aSoundMode`, a little before a literal pool that
// holds the music player table's address, which is the init's. Metroid Fusion's init keeps the mode for a later call
// instead: `ldr r0, =mode; mov r8, r0`. Returns 0 if there's none.
uint32_t FindSoundMode(const Rom& rom, uint32_t player_table)
{
    const uint8_t* data = rom.Ptr(kRomBase);
    const size_t size = rom.Size();
    for (size_t o = 0; o + 4 <= size; o += 4)
    {
        if (xsflib::LoadLe32(data + o) != player_table)
        {
            continue;
        }

        // The setting closest to the pool belongs to the init, whose pool it is.
        const uint32_t pool = kRomBase + uint32_t(o);
        uint32_t mode = 0;
        for (uint32_t at = pool > kRomBase + kInitReach ? pool - kInitReach : kRomBase; at + 6 <= pool; at += 2)
        {
            if ((rom.U16(at) >> 8) == 0x48 && (IsBl(rom, at + 2) || rom.U16(at + 2) == kMovR8R0) &&
                IsSoundMode(ThumbLiteral(rom, at)))
            {
                mode = ThumbLiteral(rom, at);
            }
        }
        if (mode)
        {
            return mode;
        }
    }

    return 0;
}

// The modes of games that work them out while they run, so that no literal holds them.
struct GameMode
{
    const char* game_code;
    uint32_t mode;
};

constexpr GameMode kGameModes[] = {
    // Spyro: Season of Flame: the routine at 0x0800398C stores its settings at 0x03000860. It sets the mode from them
    // at 0x080039E6: 8 DirectSound channels, a master volume of 10 and 13379 Hz. Its call with 10512 Hz at 0x08004112
    // finds SoundInfo locked in an mGBA run, and the rate stays.
    {"ASYE", 0x00D4A880},
    // These games set the mode after the init with `movs r0, #imm; lsls r0, r0, #n`, which FindGameSoundMode() doesn't
    // read. The sound init of Sugar Sugar Rune: Heart ga Ippai! Moegi Gakuen, at 0x080064E4, sets a master volume of 6
    // at 0x080064FE. The sound init of Futari wa Precure Max Heart: Maji Maji! Fight de IN Janai, at 0x08004EC4, sets 6
    // at 0x08004EDE. The sound init of Futari wa Precure: Arienaai Yume no Kuni wa Daimeikyuu, at 0x0800F9A8, sets 8
    // DirectSound channels at 0x0800F9BA and a master volume of 8 at 0x0800F9C2. The Cheetah Girls' sound init at
    // 0x08035EE0 sets 8 channels at 0x08035EEA. The Proud Family's sound init at 0x080078B0 sets 6 at 0x080078BA. A
    // routine of that game sets 12 at 0x080DB5E8 for a scene, and 6 again at 0x080DB6F8.
    {"B4LJ", 0x00006000},
    {"BFMJ", 0x00006000},
    {"BFPJ", 0x00008800},
    {"BCQE", 0x00000800},
    {"BD7E", 0x00000600},
    // Strawberry Shortcake: Sweet Dreams' music routine at 0x0800172C sets 6 channels at 0x08001760 each time it starts
    // a song. The init sets 8.
    {"B4TE", 0x00000600},
    // The Little Mermaid: Magic in Two Kingdoms sets 8 DirectSound channels at boot (0x08045D26), after the init's 10.
    // kSongSettings gives the songs of its scenes other numbers.
    {"BN9E", 0x00000800},
};

// Settings that a game's code gives some songs when it starts them (see SongSetting): a mode before the start, and a
// volume for every track of the player after it.
struct SongSettingRow
{
    std::string_view game_code;
    uint32_t mode;
    int volume;
    std::span<const int> songs;
};

// Steel Empire starts a song through its routine at 0x08005C54. That routine gives the player the volume of the song's
// halfword in the table at 0x080460FC (0x08005CF8). Songs 1 to 10 and 12 to 14 have 0x122, and songs 17 and 22 have
// 0x15C. The others have 0x100.
constexpr int kSteelEmpireLouder[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14};
constexpr int kSteelEmpireLoudest[] = {17, 22};

// The Barbie Diaries: High School Mystery starts its music through its routine at 0x08000D34. The routine sets the
// DirectSound channels from an argument. The rooms play songs 3, 5 and 7 with 6 channels. The title, the opening and
// two other story scenes play songs 4 and 6 with 8. Two rooms play song 4 with 6 channels (0x08009478, 0x08014C00),
// and one room plays song 6 with 6 (0x0801022E).
constexpr int kBarbieSix[] = {3, 5, 7};
constexpr int kBarbieEight[] = {4, 6};

// The Little Mermaid: Magic in Two Kingdoms gives the songs of its scenes their channels when it starts them. The
// action scenes play songs 7, 8, 11 and 58 with 5. The other scenes play songs 1, 9 and 10 with 6. The boot's 8 stays
// for the menus' songs 0 and 60, and for song 9 in the first room until another scene has set a number. Three rooms
// play song 8 with 6 channels (0x080256CC, 0x08025FCC, 0x080267D4).
constexpr int kMermaidFive[] = {7, 8, 11, 58};
constexpr int kMermaidSix[] = {1, 9, 10};

// Mr. Driller 2 sets a reverb of 50 when a game starts (0x08003922 and 6 more places). It sets a reverb of 0 when the
// game ends. The game's songs 1, 2, 3, 5 and 8 play with 50. The attract mode plays song 2 dry. The scene at
// 0x08010B94 sets 8 DirectSound channels and starts song 79. The init sets 4.
constexpr int kDrillerReverb[] = {1, 2, 3, 5, 8};
constexpr int kDrillerEight[] = {79};

// Strawberry Shortcake: Ice Cream Island Riding Camp starts song 2 after the title's song 8. Song 8's header sets a
// reverb of 20. Song 2's header sets none. Song 2 therefore plays with 20.
constexpr int kStrawberryReverb[] = {2};

constexpr SongSettingRow kSongSettings[] = {
    {.game_code = "BBIE", .mode = 0x600, .volume = kFullPlayerVolume, .songs = kBarbieSix},
    {.game_code = "BBIE", .mode = 0x800, .volume = kFullPlayerVolume, .songs = kBarbieEight},
    {.game_code = "BN9E", .mode = 0x500, .volume = kFullPlayerVolume, .songs = kMermaidFive},
    {.game_code = "BN9E", .mode = 0x600, .volume = kFullPlayerVolume, .songs = kMermaidSix},
    {.game_code = "BR2E", .mode = 0x80 | 50, .volume = kFullPlayerVolume, .songs = kDrillerReverb},
    {.game_code = "BR2E", .mode = 0x800, .volume = kFullPlayerVolume, .songs = kDrillerEight},
    {.game_code = "B35P", .mode = 0x80 | 20, .volume = kFullPlayerVolume, .songs = kStrawberryReverb},
    {.game_code = "BKTP", .mode = 0, .volume = 0x122, .songs = kSteelEmpireLouder},
    {.game_code = "BKTP", .mode = 0, .volume = 0x15C, .songs = kSteelEmpireLoudest},
};

// Kuru Kuru Kururin: the words at this address give the DirectSound channels of songs 1 to 23. The routines that start
// a song, from 0x080181FC, set the channels from it first.
constexpr uint32_t kKururinChannels = 0x0801EF6C;
constexpr int kKururinSongs = 24;

// Finds the mode that a game's code sets straight after the init: `bl m4aSoundInit; ldr r0, =mode; bl m4aSoundMode`.
// The init's call of m4aSoundMode with `init_mode` gives the two routines. Returns 0 if there's none. Guilty Gear X:
// Advance Edition sets a master volume of 8 that way, at 0x0802C230, and F-Zero: GP Legend sets 6 DirectSound channels,
// at 0x08000E34.
uint32_t FindGameSoundMode(const Rom& rom, uint32_t init_mode)
{
    uint32_t init_call = 0;
    uint32_t sound_mode = 0;
    for (uint32_t at = kRomBase; at + 6 <= rom.End() && !init_call; at += 2)
    {
        if ((rom.U16(at) >> 8) == 0x48 && IsBl(rom, at + 2) &&
            rom.Contains(((at + 4) & ~3u) + (rom.U16(at) & 0xFFu) * 4, 4) && ThumbLiteral(rom, at) == init_mode)
        {
            init_call = at;
            sound_mode = BlTarget(rom, at + 2);
        }
    }
    if (!init_call)
    {
        return 0;
    }

    for (uint32_t at = kRomBase; at + 10 <= rom.End(); at += 2)
    {
        if (!IsBl(rom, at) || (rom.U16(at + 4) >> 8) != 0x48 || !IsBl(rom, at + 6) ||
            BlTarget(rom, at + 6) != sound_mode)
        {
            continue;
        }

        const uint32_t init = BlTarget(rom, at);
        const uint32_t literal = ((at + 8) & ~3u) + (rom.U16(at + 4) & 0xFFu) * 4;
        if (init <= init_call && init_call - init < kInitReach && rom.Contains(literal, 4) &&
            rom.U32(literal) >> 24 == 0)
        {
            return rom.U32(literal);
        }
    }

    return 0;
}

// Applies the driver's mode setting, as m4aSoundMode does: each field that isn't 0 replaces the driver's setting.
void ApplySoundMode(uint32_t mode, DriverInfo& info)
{
    if (mode & 0xFF)
    {
        info.reverb = int(mode & 0x7F);
    }
    if (mode & 0xF00)
    {
        info.max_channels = int((mode >> 8) & 0xF);
    }
    if (mode & 0xF000)
    {
        info.master_volume = int((mode >> 12) & 0xF);
    }
    if (mode & 0xB00000)
    {
        info.dac_bits = uint8_t((mode & 0x300000) >> 14);
    }
    if (mode & 0xF0000)
    {
        info.rate_index = int((mode >> 16) & 0xF);
    }
}

// Returns true if the ROM holds `code`.
template <size_t N>
bool HasCode(const Rom& rom, const uint8_t (&code)[N])
{
    const uint8_t* data = rom.Ptr(kRomBase);
    const uint8_t* end = data + rom.Size();
    return std::search(data, end, std::begin(code), std::end(code)) != end;
}

// Reads the echo of Camelot's mixer from the literals of its output stage, which lie before kCamelotPulse. Two words
// multiply a side's old points. The low half of the first holds the level to the same side, and its high half the level
// to the other side. The second holds the two the other way round, with a borrow into its high half. The word 8 bytes
// before them gives the left side's distance to the right side's output. Keeps the defaults when no words of that shape
// lie there, and a third of a frame for the distance when it wouldn't fit in a frame.
void ReadCamelotEcho(const Rom& rom, DriverInfo& info)
{
    info.camelot_tap = (info.samples_per_frame / 3) & ~1;
    const uint8_t* data = rom.Ptr(kRomBase);
    const uint8_t* end = data + rom.Size();
    const uint8_t* found = std::search(data, end, std::begin(kCamelotPulse), std::end(kCamelotPulse));
    if (found == end)
    {
        return;
    }

    const uint32_t pulse = kRomBase + uint32_t(found - data);
    for (uint32_t back = 4 + (pulse & 3u); back <= kCamelotEchoReach; back += 4)
    {
        const uint32_t at = pulse - back;
        if (at < kRomBase + 8 || !rom.Contains(at - 8, 16))
        {
            return;
        }

        const uint32_t first = rom.U32(at);
        const uint32_t second = rom.U32(at + 4);
        const uint32_t level = first & 0xFFFF;
        if (first >> 28 == 0xF && first >> 16 == (second & 0xFFFF) && second >> 16 == level - 1)
        {
            info.camelot_echo = int(level);
            info.camelot_cross_echo = int(int16_t(first >> 16));
            const uint32_t tap = rom.U32(at - 8);
            if (tap > 0 && tap < uint32_t(info.samples_per_frame) && tap % 2 == 0)
            {
                info.camelot_tap = int(tap);
            }
            return;
        }
    }
}

// Reads the music player table: 12 bytes a player, up to the first entry whose state or tracks aren't in RAM, or that
// has no tracks. An entry of zeros is an unused player. It plays no tracks. Such entries after the last player don't
// count.
void ReadPlayers(const Rom& rom, DriverInfo& info)
{
    size_t used = 0;
    for (int p = 0; p < kMaxPlayers; p++)
    {
        const uint32_t at = info.player_table + 12 * uint32_t(p);
        if (!rom.Contains(at, 12))
        {
            break;
        }

        Player player;
        const bool unused = rom.U32(at) == 0 && rom.U32(at + 4) == 0 && rom.U32(at + 8) == 0;
        if (!unused)
        {
            if (!InRam(rom.U32(at)) || !InRam(rom.U32(at + 4)) || rom.U8(at + 8) == 0)
            {
                break;
            }

            player.track_count = rom.U8(at + 8) > kMaxTracks ? kMaxTracks : rom.U8(at + 8);
            used = size_t(p) + 1;
        }
        info.players.push_back(player);
    }

    info.players.resize(used);
}

// Returns true if the ROM holds the Thumb code that `text` gives in ParseThumbPattern()'s form at `at`.
bool HasThumbAt(const Rom& rom, uint32_t at, std::string_view text)
{
    const std::vector<agb::ThumbPattern> pattern = ParseThumbPattern(text);
    if (!rom.Contains(at, uint32_t(2 * pattern.size())))
    {
        return false;
    }

    for (size_t i = 0; i < pattern.size(); i++)
    {
        if ((rom.U16(at + 2 * uint32_t(i)) & pattern[i].mask) != pattern[i].value)
        {
            return false;
        }
    }

    return true;
}

// Finds the songs that the game's code gives another number of DirectSound channels, from kChannelSettings.
void FindSongChannels(const Rom& rom, DriverInfo& info)
{
    for (const ChannelSetting& game : kChannelSettings)
    {
        if (rom.GameCode() == game.game_code && HasThumbAt(rom, game.routine, game.code))
        {
            info.song_channels.routine = game.routine;
            info.song_channels.channels = std::min(game.channels, kMaxDirectChannels);
            info.song_channels.songs.assign(game.songs.begin(), game.songs.end());
        }
    }
}

// Returns the DirectSound channels that the mixer has for song `song`: the init's, or those that the game's code gives
// the song through its routine before it starts it.
int ChannelsForSong(const DriverInfo& info, int song)
{
    const std::vector<int>& songs = info.song_channels.songs;
    return std::find(songs.begin(), songs.end(), song) != songs.end() ? info.song_channels.channels : info.max_channels;
}

// Fills info.song_settings from the game's rows of kSongSettings.
void FindSongSettings(const Rom& rom, DriverInfo& info)
{
    for (const SongSettingRow& row : kSongSettings)
    {
        if (rom.GameCode() != row.game_code)
        {
            continue;
        }

        for (int song : row.songs)
        {
            info.song_settings.push_back({.song = song, .mode = row.mode, .volume = row.volume});
        }
    }
}

// Returns the setting that the game's code gives song `song`, or nullptr if it gives none.
const SongSetting* FindSetting(const DriverInfo& info, int song)
{
    const auto found = std::find_if(info.song_settings.begin(), info.song_settings.end(),
                                    [song](const SongSetting& s) { return s.song == song; });
    return found != info.song_settings.end() ? &*found : nullptr;
}

// Returns a list of songs for the log, such as "songs 1, 2 and 5".
std::string SongList(const std::vector<int>& songs)
{
    std::string list = songs.size() == 1 ? "song " : "songs ";
    for (size_t i = 0; i < songs.size(); i++)
    {
        list += (i == 0 ? "" : i + 1 == songs.size() ? " and " : ", ") + std::to_string(songs[i]);
    }

    return list;
}

// Returns what a song's setting changes, for the log, such as "8 DirectSound channels, reverb 50".
std::string SettingText(const SongSetting& setting)
{
    std::vector<std::string> parts;
    if (setting.mode & 0xF00)
    {
        parts.push_back(std::to_string((setting.mode >> 8) & 0xF) + " DirectSound channels");
    }
    if (setting.mode & 0xFF)
    {
        parts.push_back("reverb " + std::to_string(setting.mode & 0x7F));
    }
    if (setting.volume != kFullPlayerVolume)
    {
        char volume[16];
        std::snprintf(volume, sizeof volume, "0x%X", unsigned(setting.volume));
        parts.push_back(std::string("player volume ") + volume);
    }

    std::string text;
    for (const std::string& part : parts)
    {
        text += (text.empty() ? "" : ", ") + part;
    }

    return text;
}

// Returns how many songs the table at `table` has: entries up to the first that isn't a song, or names a music player
// the table of players doesn't have, apart from all-zero entries. An all-zero entry is empty, since a GSF rip can zero
// the entries of the songs it doesn't hold, and those after the last song don't count.
int CountSongs(const Rom& rom, const DriverInfo& info, uint32_t table)
{
    int count = 0;
    SongHeader header;
    for (int n = 0; n < kMaxSongs && rom.Contains(table + 8u * uint32_t(n), 8); n++)
    {
        const uint32_t at = table + 8u * uint32_t(n);
        if (rom.U32(at) == 0 && rom.U32(at + 4) == 0)
        {
            continue;
        }
        if (!ReadSongHeader(rom, rom.U32(at), header) ||
            (!info.players.empty() && rom.U16(at + 4) >= info.players.size()))
        {
            break;
        }

        count = n + 1;
    }

    return count;
}

// Finds the tables of a game with the Metroid games' sequencer and no m4aSongNumStart (Metroid: Zero Mission): two
// loads of literals in a row, the music player table's and then the song table's, and a shift of the song's number by
// 3 within the next two instructions (0x08002B28 in Zero Mission). The first pair whose tables read wins. Returns false
// if there's none.
bool FindMetroidTables(const Rom& rom, DriverInfo& info)
{
    const uint32_t end = kRomBase + uint32_t(rom.Size());
    for (uint32_t at = kRomBase; at + 10 <= end; at += 2)
    {
        if ((rom.U16(at) & 0xF800) != 0x4800 || (rom.U16(at + 2) & 0xF800) != 0x4800)
        {
            continue;
        }

        // lsls rd, rm, #3
        if ((rom.U16(at + 4) & 0xFFC0) != 0x00C0 && (rom.U16(at + 6) & 0xFFC0) != 0x00C0)
        {
            continue;
        }

        const uint32_t player_table = ThumbLiteral(rom, at);
        const uint32_t song_table = ThumbLiteral(rom, at + 2);
        if (!rom.Contains(player_table, 12) || !rom.Contains(song_table, 8))
        {
            continue;
        }

        DriverInfo candidate;
        candidate.player_table = player_table;
        ReadPlayers(rom, candidate);
        if (candidate.players.empty() || CountSongs(rom, candidate, song_table) == 0)
        {
            continue;
        }

        info.player_table = player_table;
        info.song_table = song_table;
        info.log.push_back("song and music player tables read at " + Hex(at));
        return true;
    }

    return false;
}

// Finds the Metroid games' table of square and wave channel settings, one for each key, from the routine that reads it:
// a test of the voice's PSG channel against 3, then the load of the table and an index of the key by 2 (0x08001E56 in
// Metroid Fusion). Returns 0 if there's none.
uint32_t FindMetroidPsgTable(const Rom& rom)
{
    constexpr uint16_t kCode[] = {0x3034, 0x7801, 0x2007, 0x4008, 0x2803};
    const uint32_t end = kRomBase + uint32_t(rom.Size());
    for (uint32_t at = kRomBase; at + 20 <= end; at += 2)
    {
        bool match = true;
        for (size_t i = 0; i < std::size(kCode) && match; i++)
        {
            match = rom.U16(at + 2 * uint32_t(i)) == kCode[i];
        }

        // bhi, ldr r1, [pc, #imm], lsls r0, r4, #1, adds r0, r0, r1, ldrh r3, [r0]
        if (!match || (rom.U16(at + 10) & 0xFF00) != 0xD800 || (rom.U16(at + 12) & 0xFF00) != 0x4900 ||
            rom.U16(at + 14) != 0x0060 || rom.U16(at + 16) != 0x1840 || rom.U16(at + 18) != 0x8803)
        {
            continue;
        }

        const uint32_t table = ThumbLiteral(rom, at + 12);
        if (rom.Contains(table, 2 * 129))
        {
            return table;
        }
    }

    return 0;
}

} // namespace

bool DetectDriver(const Rom& rom, const DriverOverrides& overrides, DriverInfo& info, std::string& error)
{
    info = DriverInfo();

    // Find the mixer's test for reversed and compressed samples, the mono mixer's volume and Camelot's changes, which a
    // game given its song table can have too.
    info.special_samples = HasCode(rom, kSpecialTest);
    info.mono = HasCode(rom, kMonoVolume);
    info.camelot_sequencer = HasCode(rom, kCamelotChannelSearch);
    info.camelot_top_priority = info.camelot_sequencer && HasCode(rom, kCamelotTopPriority);
    info.camelot_mixer = HasCode(rom, kCamelotPulse);
    info.metroid_sequencer = HasCode(rom, kMetroidTempo);
    info.metroid_short_notes = info.metroid_sequencer && HasCode(rom, kMetroidShortNote);
    if (info.metroid_sequencer)
    {
        info.metroid_psg_table = FindMetroidPsgTable(rom);
    }

    // Find the routine that starts a song, and from it the tables, and the init's setting of the driver's mode. A game
    // with the Metroid games' sequencer may have no such routine. Its code gives the tables elsewhere.
    if (FindSongStart(rom, info) || (info.metroid_sequencer && FindMetroidTables(rom, info)))
    {
        ReadPlayers(rom, info);
        info.sound_mode = FindSoundMode(rom, info.player_table);
        if (info.sound_mode)
        {
            ApplySoundMode(info.sound_mode, info);
            ApplySoundMode(FindGameSoundMode(rom, info.sound_mode), info);
            for (const GameMode& row : kGameModes)
            {
                if (rom.GameCode() == row.game_code)
                {
                    ApplySoundMode(row.mode, info);
                }
            }
        }
        else
        {
            info.warnings.push_back(
                "the driver's settings weren't found, so its defaults are assumed: 13379 Hz and "
                "8 DirectSound channels");
        }
        FindSongChannels(rom, info);
    }
    FindSongSettings(rom, info);

    if (overrides.song_table)
    {
        info.song_table = overrides.song_table;
    }
    if (info.song_table == 0)
    {
        error.clear();
        return false;
    }

    info.song_count = overrides.song_count ? overrides.song_count : CountSongs(rom, info, info.song_table);
    if (info.song_count == 0)
    {
        error = "the song table at " + Hex(info.song_table) + " has no valid songs (try --song-count)";
        return false;
    }

    if (info.max_channels > kMaxDirectChannels)
    {
        info.max_channels = kMaxDirectChannels;
    }
    if (info.camelot_mixer && HasCode(rom, kCamelotTwelveChannels))
    {
        info.max_channels = kMaxDirectChannels;
    }

    // The rate's setting gives the mixer's output for a frame, from which the driver works out its rate in Hz.
    info.samples_per_frame = kSamplesPerFrame[info.rate_index - 1];
    info.mix_rate = (597275 * info.samples_per_frame + 5000) / 10000;
    info.step_scale = (16777216u / uint32_t(info.mix_rate) + 1) >> 1;
    if (info.camelot_mixer)
    {
        info.camelot_output_points = HasCode(rom, kCamelotOnePoint) ? 1 : 2;
        info.camelot_adpcm = HasCode(rom, kCamelotAdpcm);
        ReadCamelotEcho(rom, info);
    }

    // Report what was found.
    if (info.song_start)
    {
        info.log.push_back("song start routine at " + Hex(info.song_start));
    }
    if (info.song_start || info.metroid_sequencer)
    {
        // Camelot's mixer leaves out the master volume and the reverb, and has an echo instead.
        std::string mixer = "mixer: " + std::to_string(info.mix_rate) + " Hz, up to " +
                            std::to_string(info.max_channels) + " DirectSound channels";
        if (info.camelot_mixer)
        {
            mixer += ", Camelot's, with synth voices and an echo";
        }
        else
        {
            mixer += ", master volume " + std::to_string(info.master_volume);
            if (info.reverb)
            {
                mixer += ", reverb " + std::to_string(info.reverb);
            }
        }
        if (info.mono)
        {
            mixer += ", mono";
        }

        info.log.push_back(mixer);

        // The songs that the game's code gives another number of channels.
        const std::vector<int>& songs = info.song_channels.songs;
        if (!songs.empty())
        {
            info.log.push_back(SongList(songs) + ": " + std::to_string(info.song_channels.channels) +
                               " DirectSound channels (the game's routine at " + Hex(info.song_channels.routine) + ")");
        }

        // The songs that the game's code gives settings, a line for each kind of setting.
        std::vector<SongSetting> kinds;
        for (const SongSetting& setting : info.song_settings)
        {
            if (std::none_of(kinds.begin(), kinds.end(), [&setting](const SongSetting& k)
                             { return k.mode == setting.mode && k.volume == setting.volume; }))
            {
                kinds.push_back(setting);
            }
        }
        for (const SongSetting& kind : kinds)
        {
            std::vector<int> list;
            for (const SongSetting& setting : info.song_settings)
            {
                if (setting.mode == kind.mode && setting.volume == kind.volume)
                {
                    list.push_back(setting.song);
                }
            }
            info.log.push_back(SongList(list) + ": " + SettingText(kind) + " (the game's code)");
        }

        if (info.camelot_sequencer)
        {
            info.log.push_back("note start: Camelot's choice of channels, volumes and rates");
        }
        if (info.metroid_sequencer)
        {
            info.log.push_back("sequencer: the Metroid games' rework of the SDK's sequencer");
            if (info.metroid_psg_table)
            {
                info.log.push_back("PSG frequency table at " + Hex(info.metroid_psg_table));
            }
        }
    }
    info.log.push_back("song table at " + Hex(info.song_table) + ": " + std::to_string(info.song_count) + " song" +
                       (info.song_count == 1 ? "" : "s") +
                       (info.players.empty() ? ""
                                             : ", on " + std::to_string(info.players.size()) + " music player" +
                                                   (info.players.size() == 1 ? "" : "s")));

    return true;
}

int SongChannels(const Rom& rom, const DriverInfo& info, int song)
{
    if (rom.GameCode() == "AKRP" && song >= 1 && song < kKururinSongs &&
        rom.Contains(kKururinChannels + 4 * uint32_t(song), 4))
    {
        return std::clamp(int(rom.U32(kKururinChannels + 4 * uint32_t(song))), 1, kMaxDirectChannels);
    }

    const SongSetting* setting = FindSetting(info, song);
    if (setting && (setting->mode & 0xF00))
    {
        return std::min(int((setting->mode >> 8) & 0xF), kMaxDirectChannels);
    }

    return ChannelsForSong(info, song);
}

int SongVolume(const DriverInfo& info, int song)
{
    const SongSetting* setting = FindSetting(info, song);
    return setting ? setting->volume : kFullPlayerVolume;
}

uint32_t SongAddress(const Rom& rom, const DriverInfo& info, int song)
{
    if (song < 0 || song >= info.song_count)
    {
        return 0;
    }

    return rom.U32(info.song_table + 8u * uint32_t(song));
}

Player SongPlayer(const Rom& rom, const DriverInfo& info, int song)
{
    const size_t number = song >= 0 && song < info.song_count ? rom.U16(info.song_table + 8u * uint32_t(song) + 4) : 0;
    if (number < info.players.size())
    {
        return info.players[number];
    }

    Player player;
    player.track_count = kMaxTracks;

    return player;
}

} // namespace ntdlib::mp2k
