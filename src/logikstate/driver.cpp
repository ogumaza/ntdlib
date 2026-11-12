// SPDX-License-Identifier: MIT

#include "ntdlib/logikstate/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "mod1_pieces.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/thumb.h"

namespace ntdlib::logikstate
{

using ntdlib::agb::BlTarget;
using ntdlib::agb::FindCalls;
using ntdlib::agb::Hex;
using ntdlib::agb::kFrameCycles;
using ntdlib::agb::kRomBase;
using ntdlib::agb::kSecondCycles;
using ntdlib::agb::ThumbLiteral;

namespace
{

// Thumb's `push {..., lr}`, `ldr rX, [pc, #imm]` and `movs r1, #imm`.
constexpr uint16_t kPushLr = 0xB500;
constexpr uint16_t kPushLrMask = 0xFF00;
constexpr uint16_t kLoadLiteral = 0x4800;
constexpr uint16_t kLoadLiteralMask = 0xF800;
constexpr uint16_t kMoveR1 = 0x2100;
constexpr uint16_t kMoveR1Mask = 0xFF00;

// The furthest that a routine's start is before the load of the signature and before the call of the load routine,
// and the furthest that a call's arguments are before the call, in bytes.
constexpr uint32_t kLoadSpan = 0x80;
constexpr uint32_t kPlaySpan = 0x40;
constexpr uint32_t kArgumentSpan = 0x18;

// The furthest that a Thumb `ldr rX, [pc, #imm]` is before its word.
constexpr uint32_t kLiteralReach = 0x400;

// Returns true if the Thumb instruction at `at` is `ldr rX, [pc, #imm]` and loads the word at `word`.
bool LoadsWord(const Rom& rom, uint32_t at, uint32_t word)
{
    const uint16_t op = rom.U16(at);
    return (op & kLoadLiteralMask) == kLoadLiteral && ((at + 4) & ~3u) + (op & 0xFFu) * 4 == word;
}

// Returns the address of the nearest `push {..., lr}` at or before `at`, up to `span` bytes back, or 0.
uint32_t RoutineStart(const Rom& rom, uint32_t at, uint32_t span)
{
    for (uint32_t a = at & ~1u; a + span >= at && rom.Contains(a, 2); a -= 2)
    {
        if ((rom.U16(a) & kPushLrMask) == kPushLr)
        {
            return a;
        }
    }

    return 0;
}

// The furthest that the signature's second word is after its first in the load routine's literal pool, in bytes.
constexpr uint32_t kSignatureSpan = 0x100;

// Returns the second word of a signature after `word`, up to kSignatureSpan away, or 0.
uint32_t SecondWordAfter(const Rom& rom, uint32_t word)
{
    for (uint32_t at = word + 4; at <= word + kSignatureSpan; at += 4)
    {
        const uint32_t value = rom.U32(at);
        if (value == kSignature2 || value == kMod1Signature2)
        {
            return value;
        }
    }

    return 0;
}

// The load routine, and the format of the modules that it reads.
struct LoadRoutine
{
    uint32_t address = 0;
    Format format = Format::kGbaMod2;
};

// Returns the load routine: the one that compares a module's first two words with the signature. The signature's
// words are in the routine's literal pool, where no module starts. A build keeps them side by side, and another has
// other words between them. The second word tells the format.
LoadRoutine FindLoadRoutine(const Rom& rom)
{
    for (uint32_t word = kRomBase; rom.Contains(word, 8); word += 4)
    {
        Module module;
        const uint32_t second = rom.U32(word) == kSignature1 ? SecondWordAfter(rom, word) : 0;
        if (!second || ReadModule(rom, word, module))
        {
            continue;
        }

        for (uint32_t at = word - 2; at + kLiteralReach >= word && rom.Contains(at, 2); at -= 2)
        {
            const uint32_t start = LoadsWord(rom, at, word) ? RoutineStart(rom, at, kLoadSpan) : 0;
            if (start)
            {
                return {start, second == kMod1Signature2 ? Format::kGbaMod1 : Format::kGbaMod2};
            }
        }
    }

    return {};
}

// Thumb's `pop {..., pc}` and `bx rX`. Each ends a routine.
constexpr uint16_t kPopPc = 0xBD00;
constexpr uint16_t kPopPcMask = 0xFF00;
constexpr uint16_t kBranchExchange = 0x4700;
constexpr uint16_t kBranchExchangeMask = 0xFF87;

// Returns true if the routine that makes the call at `call` calls another routine after it, within kPlaySpan bytes and
// before it returns.
bool CallsAfter(const Rom& rom, uint32_t call)
{
    for (uint32_t at = call + 4; at < call + 4 + kPlaySpan; at += 2)
    {
        const uint16_t op = rom.U16(at);
        if ((op & kPopPcMask) == kPopPc || (op & kBranchExchangeMask) == kBranchExchange)
        {
            return false;
        }
        if (BlTarget(rom, at))
        {
            return true;
        }
    }

    return false;
}

// Returns the play routine: the routine that calls the load routine and then the start routine. The game's main code
// of the GBAMOD1 build also loads a module, for its sound effects, and starts no music.
uint32_t FindPlayRoutine(const Rom& rom, uint32_t load)
{
    for (uint32_t call : FindCalls(rom, load))
    {
        const uint32_t start = RoutineStart(rom, call, kPlaySpan);
        if (start && CallsAfter(rom, call))
        {
            return start;
        }
    }

    return 0;
}

// Returns true if a bank of samples starts at `bank`: every header's length is 0 or more, some sample has points, and
// the points are in the ROM.
bool ValidBank(const Rom& rom, uint32_t bank)
{
    if (!rom.Contains(bank, kBankPoints))
    {
        return false;
    }

    uint32_t points = 0;
    for (uint32_t i = 0; i < uint32_t(kSamples); i++)
    {
        const int16_t length = int16_t(rom.U16(bank + i * kSampleSize + kSampleLength));
        if (length < 0)
        {
            return false;
        }
        points += uint32_t(length);
    }

    return points > 0 && rom.Contains(bank + kBankPoints, points);
}

// The arguments of a call of the play routine that the code before the call loads as constants. A 0 stands for an
// argument that the code works out.
struct PlayCall
{
    uint32_t module = 0;
    uint32_t bank = 0;
    int order = 0;
};

// Reads the arguments of the call at `call`, from the instructions between the call before it and the call itself.
PlayCall ReadCall(const Rom& rom, uint32_t call)
{
    PlayCall out;
    bool have_module = false, have_bank = false, have_order = false;
    for (uint32_t at = call - 2; at + kArgumentSpan >= call && !BlTarget(rom, at - 2); at -= 2)
    {
        const uint16_t op = rom.U16(at);
        const int reg = (op >> 8) & 7;
        if ((op & kLoadLiteralMask) == kLoadLiteral && reg == 0 && !have_module)
        {
            out.module = ThumbLiteral(rom, at);
            have_module = true;
        }
        else if ((op & kLoadLiteralMask) == kLoadLiteral && reg == 2 && !have_bank)
        {
            out.bank = ThumbLiteral(rom, at);
            have_bank = true;
        }
        else if ((op & kMoveR1Mask) == kMoveR1 && !have_order)
        {
            out.order = op & 0xFF;
            have_order = true;
        }
    }

    return out;
}

// A routine that sets one of the game's three volumes: ldr r1, [pc, #4]; str r0, [r1]; bx lr, a halfword of padding
// and the word with the variable's address.
constexpr std::array<uint16_t, 3> kVolumeSetter = {0x4901, 0x6008, 0x4770};
constexpr uint32_t kVolumeSetterSize = 12;
constexpr uint32_t kVolumeSetterWord = 8;

// The furthest that the three routines are after the play routine's start, in bytes.
constexpr uint32_t kVolumeSpan = 0x100;

// Thumb's `movs r0, #imm`.
constexpr uint16_t kMoveR0 = 0x2000;
constexpr uint16_t kMoveR0Mask = 0xFF00;

// Returns the routine that sets the music's volume, or 0. Three routines after the play routine set the master volume,
// the music's volume and the sound effects' volume, in that order, and their variables follow one another in memory.
uint32_t FindMusicVolumeRoutine(const Rom& rom, uint32_t play)
{
    for (uint32_t at = play; at < play + kVolumeSpan; at += 2)
    {
        bool found = true;
        for (uint32_t i = 0; i < 3 && found; i++)
        {
            const uint32_t setter = at + i * kVolumeSetterSize;
            found = rom.U16(setter) == kVolumeSetter[0] && rom.U16(setter + 2) == kVolumeSetter[1] &&
                    rom.U16(setter + 4) == kVolumeSetter[2] &&
                    rom.U32(setter + kVolumeSetterWord) == rom.U32(at + kVolumeSetterWord) + i * 4;
        }
        if (found)
        {
            return at + kVolumeSetterSize;
        }
    }

    return 0;
}

// Returns the music's volume that the game's code passes to `routine` as a constant above 0, or kFullVolume if no
// call passes one, or if the calls pass several. The address of the first such call is written to `call`.
int32_t FindMusicVolume(const Rom& rom, uint32_t routine, uint32_t& call)
{
    int32_t volume = 0;
    for (uint32_t at : FindCalls(rom, routine))
    {
        const uint16_t op = rom.U16(at - 2);
        const int32_t value = op & 0xFF;
        if ((op & kMoveR0Mask) != kMoveR0 || value == 0 || value > kFullVolume)
        {
            continue;
        }
        if (volume != 0 && value != volume)
        {
            return kFullVolume;
        }

        call = volume == 0 ? at : call;
        volume = value;
    }

    return volume ? volume : kFullVolume;
}

// The routine that switches the sound off: ldr r1, [pc, #4]; movs r0, #0; strh r0, [r1]; bx lr, and the word with
// SOUNDCNT_X's address.
constexpr std::array<uint16_t, 4> kSoundOff = {0x4901, 0x2000, 0x8008, 0x4770};
constexpr uint32_t kSoundOffWord = 8;
constexpr uint32_t kSoundControlX = 0x04000084;

// The furthest that a routine's start is before one of its calls of that routine, in bytes.
constexpr uint32_t kCallerSpan = 0x400;

// Returns the routine that switches the sound off, or 0. It is after the play routine, like the routines that set
// the volumes, in the GBAMOD2 builds, and after the routine that starts the output (`output`, or 0 if not found) in
// the GBAMOD1 build.
uint32_t FindSoundOffRoutine(const Rom& rom, uint32_t play, uint32_t output)
{
    for (uint32_t from : {play, output})
    {
        for (uint32_t at = from; from && at < from + kVolumeSpan; at += 2)
        {
            if (rom.U16(at) == kSoundOff[0] && rom.U16(at + 2) == kSoundOff[1] && rom.U16(at + 4) == kSoundOff[2] &&
                rom.U16(at + 6) == kSoundOff[3] && rom.U32(at + kSoundOffWord) == kSoundControlX)
            {
                return at;
            }
        }
    }

    return 0;
}

// The driver's code: from kDriverBefore bytes before the play routine to kDriverSpan bytes after its start.
constexpr uint32_t kDriverBefore = 0x400;
constexpr uint32_t kDriverSpan = 0x1800;

bool InDriver(uint32_t at, uint32_t play)
{
    return at + kDriverBefore >= play && at < play + kDriverSpan;
}

// Returns a call from the game's code that reaches `routine`, or 0: a call of `routine` from outside the driver's
// code, or a call from there of a routine of the driver that calls `routine`. Calls within the driver alone don't
// count: the driver has parts that a game never runs, and one of them switches the sound off.
uint32_t FindGameCall(const Rom& rom, uint32_t routine, uint32_t play)
{
    const std::vector<uint32_t> calls = FindCalls(rom, routine);
    for (uint32_t at : calls)
    {
        if (!InDriver(at, play))
        {
            return at;
        }
    }
    for (uint32_t at : calls)
    {
        const uint32_t start = RoutineStart(rom, at, kCallerSpan);
        for (uint32_t outer : start ? FindCalls(rom, start) : std::vector<uint32_t>())
        {
            if (!InDriver(outer, play))
            {
                return outer;
            }
        }
    }

    return 0;
}

// Thumb's `movs rX, #imm` and `adds rX, #imm`, and the sizes of a mixer channel's record in the two builds. The
// driver's code moves from a record to the next with them.
constexpr uint16_t kMoveImmediate = 0x2000;
constexpr uint16_t kAddImmediate = 0x3000;
constexpr uint16_t kImmediateMask = 0xF800;
constexpr uint32_t kShortRecord = 0x4C;
constexpr uint32_t kLongRecord = 0x54;

// Returns the driver's build: the size of a channel's record that more of the code after the play routine names.
Build FindBuild(const Rom& rom, uint32_t play)
{
    int short_records = 0, long_records = 0;
    for (uint32_t at = play; at < play + kDriverSpan; at += 2)
    {
        const uint16_t op = rom.U16(at);
        if ((op & kImmediateMask) == kMoveImmediate || (op & kImmediateMask) == kAddImmediate)
        {
            short_records += (op & 0xFFu) == kShortRecord;
            long_records += (op & 0xFFu) == kLongRecord;
        }
    }

    return long_records > short_records ? Build::kLongRecords : Build::kShortRecords;
}

// The music's volume that a game sets at its start, where the game's code passes it to the driver from a variable.
// Detection reads only a constant.
struct GameVolume
{
    const char* game_code;
    int32_t music_volume;
};

constexpr GameVolume kGameVolumes[] = {
    // Zapper's default options contain 0x20 for the music and for the sound effects (0x080C760A stores them at
    // 0x03000981
    // and 0x03000982). The routine at 0x080C7748 passes them to the driver.
    {"AZPE", 0x20},
    // Pinball Challenge Deluxe's options keep a level from 0 to 10, 10 by default (0x08003728). The game converts it
    // to a volume, times 0x6666 >> 12 (0x08003960): 63 for level 10. The start routine copies it to 0x02000710.
    {"APLP", 63},
};

// The scale of the slide to a note that a game sets for a module, by the module's place among the ROM's modules.
struct GameSlideScale
{
    const char* game_code;
    size_t module;
    uint32_t scale;
};

constexpr GameSlideScale kGameSlideScales[] = {
    // Pinball Challenge Deluxe writes the scale at 0x020006F0. The initialised data sets it to kNoteSlideScale (from
    // 0x083E1DC0). The title's handler sets 8 (0x080037B6) for module 1. A table's start (0x080072E2) sets 8 for
    // tables 0 to 3 and kNoteSlideScale for tables 4 to 7. Tables 0 to 3 play modules 3 to 6, and tables 4 to 7 play
    // modules 7 to 10.
    {"APLP", 1, 8}, {"APLP", 3, 8}, {"APLP", 4, 8}, {"APLP", 5, 8}, {"APLP", 6, 8},
};

// Where a format keeps the fields of a module's header.
struct Layout
{
    uint32_t signature2;
    uint32_t size;
    uint32_t pattern_bytes;
    uint32_t rate;
    uint32_t orders;
    uint32_t steps;
    uint32_t samples;
    uint32_t patterns;
    uint32_t order_list;
    uint32_t rows;
    uint32_t row_size; // the bytes of a pattern's count of rows
};

constexpr Layout kMod2Layout = {.signature2 = kSignature2,
                                .size = kModuleSize,
                                .pattern_bytes = kModulePatternBytes,
                                .rate = kModuleRate,
                                .orders = kModuleOrders,
                                .steps = kModuleSteps,
                                .samples = kModuleSamples,
                                .patterns = kModulePatterns,
                                .order_list = kModuleOrderList,
                                .rows = kModuleRows,
                                .row_size = 4};
constexpr Layout kMod1Layout = {.signature2 = kMod1Signature2,
                                .size = kMod1Size,
                                .pattern_bytes = kMod1PatternBytes,
                                .rate = kMod1Rate,
                                .orders = kMod1Orders,
                                .steps = kMod1Steps,
                                .samples = kMod1Samples,
                                .patterns = kMod1Patterns,
                                .order_list = kMod1OrderList,
                                .rows = kMod1Rows,
                                .row_size = 1};

const Layout& LayoutOf(Format format)
{
    return format == Format::kGbaMod1 ? kMod1Layout : kMod2Layout;
}

// Returns the address of every module of `format` in the ROM, in order.
std::vector<uint32_t> FindModules(const Rom& rom, Format format)
{
    std::vector<uint32_t> modules;
    const uint32_t second = LayoutOf(format).signature2;
    for (uint32_t at = kRomBase; rom.Contains(at, 8); at += 4)
    {
        Module module;
        if (rom.U32(at) == kSignature1 && rom.U32(at + 4) == second && ReadModule(rom, at, module))
        {
            modules.push_back(at);
        }
    }

    return modules;
}

// Returns the format of the first module in the ROM, if any.
bool FirstFormat(const Rom& rom, Format& format)
{
    for (uint32_t at = kRomBase; rom.Contains(at, 8); at += 4)
    {
        Module module;
        if (rom.U32(at) == kSignature1 && ReadModule(rom, at, module))
        {
            format = module.format;
            return true;
        }
    }

    return false;
}

// Sets the music's volume as the game sets it at its start.
void SetMusicVolume(const Rom& rom, DriverInfo& info)
{
    info.music_volume_routine = FindMusicVolumeRoutine(rom, info.play_routine);
    uint32_t volume_call = 0;
    info.music_volume =
        info.music_volume_routine ? FindMusicVolume(rom, info.music_volume_routine, volume_call) : kFullVolume;
    if (info.music_volume != kFullVolume)
    {
        info.log.push_back("music volume " + std::to_string(info.music_volume) + " of " + std::to_string(kFullVolume) +
                           ", which the game sets at " + Hex(volume_call));
    }
    for (const GameVolume& game : kGameVolumes)
    {
        if (rom.GameCode() == game.game_code)
        {
            info.music_volume = game.music_volume;
            info.log.push_back("music volume " + std::to_string(info.music_volume) + " of " +
                               std::to_string(kFullVolume) + ", the default of the game's options");
        }
    }
}

// Logs the count of modules, the first one's address and the mixer's rate.
void LogModules(const Rom& rom, const std::vector<uint32_t>& modules, DriverInfo& info)
{
    Module first;
    ReadModule(rom, modules.front(), first);
    info.log.push_back(std::to_string(modules.size()) + " modules, the first at " + Hex(modules.front()) + ", mixer " +
                       std::to_string(first.rate) + " Hz");
}

// Thumb's `ldr rX, [pc, #imm]` loads of the two words that start the driver's output: DMA1CNT_H's address, and then
// within kOutputSpan bytes the value of SOUNDCNT_H. In the value, bits 8 and 9 send FIFO A to the right and the left,
// bits 12 and 13 send FIFO B, and bits 11 and 15 reset the FIFOs.
constexpr uint32_t kDmaControl = 0x040000C6;
constexpr uint32_t kOutputSpan = 0x20;
constexpr uint32_t kFifoA = 0x0300;
constexpr uint32_t kFifoB = 0x3000;
constexpr uint32_t kFifoResets = 0x8800;

// The routine that starts the driver's output: the address of its load of DMA1CNT_H's address, or 0 if not found, and
// the count of FIFOs that it sends the mix to.
struct Output
{
    uint32_t at = 0;
    int fifos = 2;
};

// Returns the routine that starts the output, from its value of SOUNDCNT_H. Logs a count of FIFOs other than 2.
Output FindOutput(const Rom& rom, std::vector<std::string>& log)
{
    for (uint32_t at = kRomBase; rom.Contains(at, 2); at += 2)
    {
        if ((rom.U16(at) & kLoadLiteralMask) != kLoadLiteral || ThumbLiteral(rom, at) != kDmaControl)
        {
            continue;
        }

        for (uint32_t next = at + 2; next < at + kOutputSpan; next += 2)
        {
            const uint32_t value = (rom.U16(next) & kLoadLiteralMask) == kLoadLiteral ? ThumbLiteral(rom, next) : 0;
            if (value > 0xFFFF || !(value & kFifoResets) || !(value & (kFifoA | kFifoB)))
            {
                continue;
            }

            const int fifos = ((value & kFifoA) ? 1 : 0) + ((value & kFifoB) ? 1 : 0);
            if (fifos != 2)
            {
                char text[16];
                std::snprintf(text, sizeof text, "0x%04X", unsigned(value));
                log.push_back("one FIFO plays the mix (SOUNDCNT_H " + std::string(text) + " at " + Hex(next) + ")");
            }
            return {at, fifos};
        }
    }

    return {};
}

// Sets the call from the game's code that switches the sound off, if any.
void SetSoundOff(const Rom& rom, const Output& output, DriverInfo& info)
{
    const uint32_t sound_off = FindSoundOffRoutine(rom, info.play_routine, output.at);
    info.sound_off_call = sound_off ? FindGameCall(rom, sound_off, info.play_routine) : 0;
    if (info.sound_off_call)
    {
        info.log.push_back(std::string(info.fifos == 2 ? "both FIFOs" : "the FIFO") +
                           " at half volume, after the game switches the sound off at " + Hex(info.sound_off_call));
    }
}

// The effect of a jump, in a cell's second halfword's high byte.
constexpr uint16_t kJumpEffect = 0xB;

// Returns true if a cell of pattern `pattern` of `module` jumps.
bool PatternJumps(const Rom& rom, const Module& module, uint32_t pattern)
{
    for (uint32_t i = 0; i < module.rows[pattern] * module.channels; i++)
    {
        if (ReadCell(rom, module.patterns[pattern] + i * kCellSize).effect == kJumpEffect)
        {
            return true;
        }
    }

    return false;
}

// Finds the songs of the GBAMOD1 build. A module contains many short pieces, each a run of orders that ends with a
// jump: a song starts at order 0 and after each order whose pattern has a jump, with no range. Each module uses the
// scale of the slide to a note that the game sets for it.
void DetectMod1(const Rom& rom, const std::vector<uint32_t>& modules, DriverInfo& info)
{
    for (size_t i = 0; i < modules.size(); i++)
    {
        Module module;
        ReadModule(rom, modules[i], module);
        Song song;
        song.module = modules[i];
        for (const GameSlideScale& game : kGameSlideScales)
        {
            song.note_slide_scale =
                rom.GameCode() == game.game_code && game.module == i ? game.scale : song.note_slide_scale;
        }

        info.songs.push_back(song);
        for (uint32_t order = 0; order + 1 < module.order_count; order++)
        {
            if (PatternJumps(rom, module, module.orders[order]))
            {
                song.order = int(order + 1);
                info.songs.push_back(song);
            }
        }
    }

    info.log.push_back("modules in the format GBAMOD1., with 4 channels of music and 2 of sound effects");
    info.log.push_back("play routine at " + Hex(info.play_routine) + ", load routine at " + Hex(info.load_routine));
    LogModules(rom, modules, info);
    info.log.push_back(std::to_string(info.songs.size()) +
                       " pieces: order 0 of each module, and each order after a "
                       "pattern with a jump");
    ClassifyMod1Pieces(rom, modules, info);
    SetMusicVolume(rom, info);
}

} // namespace

bool ReadModule(const Rom& rom, uint32_t address, Module& module)
{
    const uint32_t second = rom.U32(address + 4);
    if (rom.U32(address) != kSignature1 || (second != kSignature2 && second != kMod1Signature2))
    {
        return false;
    }

    // The counts that the player's tables have room for. A GBAMOD1 module has 4 channels and its points.
    Module m;
    m.format = second == kMod1Signature2 ? Format::kGbaMod1 : Format::kGbaMod2;
    const Layout& layout = LayoutOf(m.format);
    if (!rom.Contains(address, layout.size))
    {
        return false;
    }
    m.address = address;
    m.steps = address + layout.steps;
    m.channels = m.format == Format::kGbaMod1 ? kMod1Channels : rom.U32(address + kModuleChannels);
    m.rate = rom.U32(address + layout.rate);
    m.order_count = rom.U32(address + layout.orders);
    m.pattern_count = rom.U32(address + layout.patterns);
    m.own_samples = m.format == Format::kGbaMod1 || rom.S32(address + kModuleOwnSamples) < 0;
    if (m.channels == 0 || m.channels > uint32_t(kMixChannels) || m.rate == 0 || m.order_count == 0 ||
        m.order_count > uint32_t(kMaxOrders) || m.pattern_count == 0 || m.pattern_count > uint32_t(kMaxPatterns))
    {
        return false;
    }

    // The patterns follow the header, each with its rows of a cell a channel. Together they fill the size that the
    // header names.
    uint32_t at = address + layout.size;
    for (uint32_t p = 0; p < m.pattern_count; p++)
    {
        const uint32_t count = address + layout.rows + p * layout.row_size;
        m.rows[p] = layout.row_size == 1 ? rom.U8(count) : rom.U32(count);
        if (m.rows[p] > uint32_t(kPatternRows))
        {
            return false;
        }
        m.patterns[p] = at;
        at += m.rows[p] * m.channels * kCellSize;
    }
    if (at - (address + layout.size) != rom.U32(address + layout.pattern_bytes) || !rom.Contains(address, at - address))
    {
        return false;
    }
    m.points = at;

    for (uint32_t i = 0; i < m.order_count; i++)
    {
        m.orders[i] = rom.U8(address + layout.order_list + i);
        if (m.orders[i] >= m.pattern_count)
        {
            return false;
        }
    }

    for (uint32_t i = 0; i < uint32_t(kSamples); i++)
    {
        const uint32_t header = address + layout.samples + i * kSampleSize;
        m.samples[i] = {rom.U16(header + kSampleLength), rom.U16(header + kSampleFineTune),
                        rom.U16(header + kSampleVolume), rom.U16(header + kSampleLoopStart),
                        rom.U16(header + kSampleLoopLength)};
    }
    module = m;

    return true;
}

std::array<uint32_t, kSamples> SamplePoints(const Rom& rom, const Module& module, const Song& song)
{
    // The load routine reads the lengths from the bank's headers when the game passes it a bank, and the rest of each
    // sample from the module's header.
    const bool banked = song.bank != 0 && !module.own_samples;

    std::array<uint32_t, kSamples> points = {};
    uint32_t at = banked ? song.bank + kBankPoints : module.points;
    for (uint32_t i = 0; i < uint32_t(kSamples); i++)
    {
        const int16_t length =
            int16_t(banked ? rom.U16(song.bank + i * kSampleSize + kSampleLength) : module.samples[i].length);
        points[i] = length != 0 ? at : 0;
        at += uint32_t(int32_t(length));
    }

    return points;
}

Cell ReadCell(const Rom& rom, uint32_t address)
{
    const uint16_t note = rom.U16(address);
    const uint16_t effect = rom.U16(address + 2);

    return {uint16_t(note & kCellPitch), uint16_t((note >> kCellSampleShift) & kCellSample), uint16_t(effect >> 8),
            uint8_t(effect & 0xFF)};
}

uint32_t FrameSamples(uint32_t rate)
{
    return uint32_t((uint64_t(rate) * kFrameCycles + kSecondCycles / 2) / kSecondCycles);
}

bool DetectDriver(const Rom& rom, DriverInfo& info, std::string& error)
{
    error.clear();
    Format first_format = Format::kGbaMod2;
    if (!FirstFormat(rom, first_format))
    {
        return DetectMod30(rom, info, error);
    }

    // The play routine calls the load routine, and the game calls the play routine with a module and a bank. The
    // signature in the load routine tells the format of the modules.
    const LoadRoutine load = FindLoadRoutine(rom);
    info.load_routine = load.address;
    info.play_routine = load.address ? FindPlayRoutine(rom, load.address) : 0;
    info.format = load.address ? load.format : first_format;
    const std::vector<uint32_t> modules = FindModules(rom, info.format);
    if (!info.play_routine || modules.empty())
    {
        error = "found " + std::to_string(FindModules(rom, first_format).size()) +
                " modules of Logik State's sound driver, but not the routine that plays them";
        return false;
    }

    const Output output = FindOutput(rom, info.log);
    info.fifos = output.fifos;
    if (info.format == Format::kGbaMod1)
    {
        DetectMod1(rom, modules, info);
        SetSoundOff(rom, output, info);
        return true;
    }

    info.build = FindBuild(rom, info.play_routine);
    if (info.build == Build::kLongRecords)
    {
        info.log.push_back("the build with channel records of 0x54 bytes");
    }

    // Each call's bank, and the orders above 0 that the calls start their modules at. A call that works its module out
    // can play any module with its bank.
    std::map<uint32_t, uint32_t> module_banks;
    std::map<uint32_t, int> bank_calls;
    std::vector<PlayCall> calls;
    for (uint32_t at : FindCalls(rom, info.play_routine))
    {
        PlayCall call = ReadCall(rom, at);
        call.bank = ValidBank(rom, call.bank) ? call.bank : 0;
        call.module = std::binary_search(modules.begin(), modules.end(), call.module) ? call.module : 0;
        if (call.bank)
        {
            bank_calls[call.bank]++;
        }
        if (call.module && call.bank)
        {
            module_banks.try_emplace(call.module, call.bank);
        }
        calls.push_back(call);
    }

    const auto fewer = [](const auto& a, const auto& b)
    {
        return a.second < b.second;
    };
    const auto commonest = std::max_element(bank_calls.begin(), bank_calls.end(), fewer);
    const uint32_t usual_bank = commonest != bank_calls.end() ? commonest->first : 0;

    // A song for each module from its first order, and one for each other order that a call starts a module at.
    for (uint32_t address : modules)
    {
        Module module;
        ReadModule(rom, address, module);
        const auto own = module_banks.find(address);
        const uint32_t bank = module.own_samples ? 0 : own != module_banks.end() ? own->second : usual_bank;
        info.songs.push_back({address, bank, 0});
        for (const PlayCall& call : calls)
        {
            if (call.module == address && call.order > 0 && uint32_t(call.order) < module.order_count)
            {
                info.songs.push_back({address, bank, call.order});
            }
        }
    }
    std::sort(info.songs.begin(), info.songs.end());
    info.songs.erase(std::unique(info.songs.begin(), info.songs.end()), info.songs.end());

    info.log.push_back("play routine at " + Hex(info.play_routine) + ", load routine at " + Hex(info.load_routine));
    LogModules(rom, modules, info);
    for (const auto& [bank, count] : bank_calls)
    {
        info.log.push_back("bank of samples at " + Hex(bank));
    }
    if (bank_calls.empty())
    {
        info.warnings.push_back("found no bank of samples, so each module plays the points after its patterns");
    }

    // The music's volume as the game sets it at its start.
    SetMusicVolume(rom, info);

    // The game's use of the sound's switch. The switch halves the volume of the FIFOs.
    SetSoundOff(rom, output, info);

    return true;
}

} // namespace ntdlib::logikstate
