// SPDX-License-Identifier: MIT

#include "ntdlib/mp2k/game_driver.h"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/thumb.h"

namespace ntdlib::mp2k
{

using ntdlib::agb::BlTarget;
using ntdlib::agb::Hex;
using ntdlib::agb::ThumbLiteral;

namespace
{

// SoundMain opens with ldr r0, =kSoundInfoPointer; ldr r0, [r0]; ldr r2, =kIdent; ldr r3, [r0]; cmp r2, r3.
// m4aSoundVSync opens the same way, with subs r3, r2 in place of the comparison.
constexpr uint32_t kIdent = 0x68736D53;
constexpr uint16_t kLoadR0FromR0 = 0x6800;
constexpr uint16_t kLoadR3FromR0 = 0x6803;
constexpr uint16_t kCompareR2R3 = 0x429A;

// m4aSoundMain: push {lr}, a bl to SoundMain, pop {r0} and bx r0. The routine that starts a song follows it.
constexpr uint32_t kMainSize = 12;
constexpr uint16_t kPushLr = 0xB500;
constexpr uint16_t kInitPush = 0xB570;

// The init's call of m4aSoundMode lies at most this far before the literal pool that holds the music player table's
// address. The init starts at most this far before the call.
constexpr uint32_t kInitReach = 0x200;

// After m4aSoundVSync loads kSoundInfoPointer, it checks the SoundInfo's ident and then counts down the DMA counter
// with ldrb rX, [rY, #4] and subs rX, #1. The count down lies within this many instructions of the load.
constexpr int kVsyncReach = 12;

// A music player's entry in the player table: the player's structure, its tracks and their number.
constexpr uint32_t kPlayerEntrySize = 12;

// Camelot's SoundMain opens with adr r1, pool and ldm r1!, {r0, r2, r3}: the stack's limit, kIdent and
// kSoundInfoPointer. Within kCamelotMainReach, it loads the buffer with ldr r5, =buffer, and the copy of the DMA
// counter with ldr r4, =copy - 1 and ldrb r4, [r4, #1]. It jumps to the mixer with ldr r3, [pc, #0] and bx r3.
constexpr uint16_t kLoadPoolR1 = 0xC90D;
constexpr uint16_t kLoadCounterCopy = 0x7864;
constexpr uint16_t kLoadR3Next = 0x4B00;
constexpr uint16_t kBxR3 = 0x4718;
constexpr uint32_t kCamelotMainReach = 0x100;

// The VBlank interrupt's call of the output stage after SoundMain: cmp r0, #0; bne; ldr r3, =output; movs r0, #n;
// mov lr, r3 and the second half of a BL alone. The ARM7TDMI runs that half as a jump to LR. Another version goes there
// with cmp r0, #0; beq, and then runs movs r0, #n; ldr r1, =output; mov lr, r1 and the same half.
constexpr uint16_t kCompareR0Zero = 0x2800;
constexpr uint16_t kMoveLrR3 = 0x469E;
constexpr uint16_t kMoveLrR1 = 0x468E;
constexpr uint16_t kBlSecondHalf = 0xF800;

// A DMA3 transfer's control in the boot code's literal pool: enabled, at once, in words, with the count of words in the
// low half.
constexpr uint32_t kCopyControl = 0x84000000;

bool IsPush(uint16_t instruction)
{
    return (instruction & 0xFE00) == 0xB400;
}

// Whether the instruction pushes the link register.
bool IsCallPush(uint16_t instruction)
{
    return (instruction & 0xFF00) == 0xB500;
}

// Whether the instruction returns: bx to a register, or a pop of the program counter.
bool IsReturn(uint16_t instruction)
{
    return (instruction & 0xFF87) == 0x4700 || (instruction & 0xFF00) == 0xBD00;
}

bool IsLiteralLoad(const Rom& rom, uint32_t at, int reg, uint32_t value)
{
    return rom.Contains(at, 2) && rom.U16(at) >> 8 == 0x48u + uint32_t(reg) && ThumbLiteral(rom, at) == value;
}

// Returns the address in `candidates` nearest to `target`, or 0 when there's none.
uint32_t Nearest(const std::vector<uint32_t>& candidates, uint32_t target)
{
    uint32_t best = 0;
    for (uint32_t c : candidates)
    {
        const uint32_t distance = c > target ? c - target : target - c;
        if (!best || distance < (best > target ? best - target : target - best))
        {
            best = c;
        }
    }

    return best;
}

// Returns the start of the routine that holds `at`: the nearest push of the link register before it, within
// kInitReach, with no return in between. Returns 0 when there's none.
uint32_t RoutineHolding(const Rom& rom, uint32_t at)
{
    for (uint32_t back = at; back + kInitReach > at && rom.Contains(back - 2, 2); back -= 2)
    {
        const uint16_t instruction = rom.U16(back - 2);
        if (IsCallPush(instruction))
        {
            return back - 2;
        }
        if (IsReturn(instruction))
        {
            return 0;
        }
    }

    return 0;
}

// Finds the init from its call of m4aSoundMode with the mode that detection found: ldr r0, =mode and a bl, before the
// literal pool that holds the music player table's address. Returns 0 when there's none.
uint32_t FindInitFromMode(const Rom& rom, const DriverInfo& info)
{
    if (!info.sound_mode || !info.player_table)
    {
        return 0;
    }

    std::vector<uint32_t> inits;
    for (uint32_t pool = agb::kRomBase; rom.Contains(pool, 4); pool += 4)
    {
        if (rom.U32(pool) != info.player_table)
        {
            continue;
        }

        uint32_t call = 0;
        for (uint32_t at = pool > agb::kRomBase + kInitReach ? pool - kInitReach : agb::kRomBase; at + 6 <= pool;
             at += 2)
        {
            if (IsLiteralLoad(rom, at, 0, info.sound_mode) && BlTarget(rom, at + 2))
            {
                call = at;
            }
        }
        if (const uint32_t init = call ? RoutineHolding(rom, call) : 0)
        {
            inits.push_back(init);
        }
    }

    return Nearest(inits, info.song_start);
}

bool InRam(uint32_t address, uint32_t size)
{
    const auto inside = [&](uint32_t base, uint32_t region)
    {
        return address >= base && address - base <= region - size;
    };
    return inside(agb::kEwramBase, agb::kEwramSize) || inside(agb::kIwramBase, agb::kIwramSize);
}

// Reads the call of the output stage that follows the call of SoundMain at `call`, into `routines`. Returns false when
// the code there has neither shape.
bool ReadOutputCall(const Rom& rom, uint32_t call, GameRoutines& routines)
{
    if (!rom.Contains(call, 16) || rom.U16(call + 4) != kCompareR0Zero)
    {
        return false;
    }

    const uint16_t branch = rom.U16(call + 6);
    if ((branch & 0xFF00) == 0xD100 && (rom.U16(call + 8) & 0xFF00) == 0x4B00 &&
        (rom.U16(call + 10) & 0xFF00) == 0x2000 && rom.U16(call + 12) == kMoveLrR3 &&
        rom.U16(call + 14) == kBlSecondHalf)
    {
        routines.output = ThumbLiteral(rom, call + 8) & ~1u;
        routines.output_argument = rom.U16(call + 10) & 0xFFu;
        return true;
    }

    if ((branch & 0xFF00) == 0xD000)
    {
        const uint32_t to = call + 10 + 2 * (branch & 0xFFu);
        if (rom.Contains(to, 8) && (rom.U16(to) & 0xFF00) == 0x2000 && (rom.U16(to + 2) & 0xFF00) == 0x4900 &&
            rom.U16(to + 4) == kMoveLrR1 && rom.U16(to + 6) == kBlSecondHalf)
        {
            routines.output = ThumbLiteral(rom, to + 2) & ~1u;
            routines.output_argument = rom.U16(to) & 0xFFu;
            return true;
        }
    }
    return false;
}

// Finds Camelot's SoundMain, the parts of the mixer that it names, the boot code's copy of the mixer and the output
// stage. `routines` keeps the SDK's init, and takes the game's init that calls it. Returns false and sets `error` when
// a part has no copy of its shape.
bool FindCamelotRoutines(const Rom& rom, const std::vector<uint32_t>& vsyncs, GameRoutines& routines,
                         std::string& error)
{
    uint32_t main = 0;
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 4) && !main; at += 2)
    {
        if ((rom.U16(at) & 0xFF00) != 0xA100 || rom.U16(at + 2) != kLoadPoolR1)
        {
            continue;
        }
        const uint32_t pool = ((at + 4) & ~3u) + 4 * (rom.U16(at) & 0xFFu);
        if (rom.Contains(pool, 12) && rom.U32(pool + 4) == kIdent && rom.U32(pool + 8) == kSoundInfoPointer)
        {
            main = at;
        }
    }
    if (!main)
    {
        error = "no routine has the shape of Camelot's SoundMain";
        return false;
    }

    uint32_t mixer = 0;
    for (uint32_t at = main; at < main + kCamelotMainReach && rom.Contains(at, 6); at += 2)
    {
        if ((rom.U16(at) & 0xFF00) == 0x4D00 && (rom.U16(at + 2) & 0xFF00) == 0x4C00 &&
            rom.U16(at + 4) == kLoadCounterCopy)
        {
            routines.pcm_buffer = ThumbLiteral(rom, at);
            routines.counter_copy = ThumbLiteral(rom, at + 2) + 1;
        }
        if (rom.U16(at) == kLoadR3Next && rom.U16(at + 2) == kBxR3)
        {
            mixer = ThumbLiteral(rom, at) & ~1u;
            break;
        }
    }
    if (!mixer || !routines.pcm_buffer || !InRam(routines.pcm_buffer, 4 * kPcmBufferSize) ||
        !InRam(routines.counter_copy, 1))
    {
        error = "Camelot's SoundMain has no jump to the mixer, or no buffer";
        return false;
    }

    for (uint32_t pool = agb::kRomBase; rom.Contains(pool, 12); pool += 4)
    {
        const uint32_t source = rom.U32(pool);
        const uint32_t destination = rom.U32(pool + 4);
        const uint32_t control = rom.U32(pool + 8);
        const uint32_t size = 4 * (control & 0xFFFF);
        if ((control & 0xFFFF0000) == kCopyControl && rom.Contains(source, size) && InRam(destination, size) &&
            mixer >= destination && mixer < destination + size)
        {
            routines.copy_source = source;
            routines.copy_destination = destination;
            routines.copy_size = size;
            break;
        }
    }
    if (!routines.copy_size)
    {
        error = "no DMA transfer copies Camelot's mixer";
        return false;
    }

    for (uint32_t call : agb::FindCalls(rom, main))
    {
        if (ReadOutputCall(rom, call, routines))
        {
            break;
        }
    }
    if (!routines.output)
    {
        error = "no call of SoundMain is followed by a call of Camelot's output stage";
        return false;
    }

    for (uint32_t call : agb::FindCalls(rom, routines.init))
    {
        if (rom.Contains(call - 2, 2) && rom.U16(call - 2) == kPushLr)
        {
            routines.init = call - 2;
            break;
        }
    }

    routines.main = main;
    routines.vsync = Nearest(vsyncs, main);
    if (!routines.vsync)
    {
        error = "no routine has m4aSoundVSync's shape";
        return false;
    }
    return true;
}

} // namespace

bool FindGameRoutines(const Rom& rom, const DriverInfo& info, GameRoutines& routines, std::string& error)
{
    routines = {};
    if (!info.song_start)
    {
        error = "detection found no routine that starts a song";
        return false;
    }

    // A game can hold more than one copy of the driver, such as in the programs that it sends to other consoles. The
    // game plays its music through the copy nearest to the routine that starts a song.
    std::vector<uint32_t> mains;
    std::vector<uint32_t> vsyncs;
    for (uint32_t at = agb::kRomBase; rom.Contains(at, 12); at += 2)
    {
        if (!IsLiteralLoad(rom, at, 0, kSoundInfoPointer))
        {
            continue;
        }

        if (rom.U16(at + 2) == kLoadR0FromR0 && IsLiteralLoad(rom, at + 4, 2, kIdent) &&
            rom.U16(at + 6) == kLoadR3FromR0 && rom.U16(at + 8) == kCompareR2R3)
        {
            mains.push_back(at);
            continue;
        }

        for (int i = 1; i < kVsyncReach && rom.Contains(at + 2 * i, 4); i++)
        {
            const uint16_t load_byte = rom.U16(at + 2 * i);
            const uint16_t subtract = rom.U16(at + 2 * i + 2);
            if ((load_byte & 0xFFC0) == 0x7900 && subtract == (0x3801 | (load_byte & 7) << 8))
            {
                vsyncs.push_back(rom.Contains(at - 2, 2) && rom.U16(at - 2) == kPushLr ? at - 2 : at);
                break;
            }
        }
    }

    const uint32_t main = Nearest(mains, info.song_start);
    if (!main)
    {
        error = "no routine has SoundMain's shape";
        return false;
    }
    const uint32_t vsync = Nearest(vsyncs, info.song_start);
    if (!vsync)
    {
        error = "no routine has m4aSoundVSync's shape";
        return false;
    }

    // Without the init's call of m4aSoundMode, the init is the routine before m4aSoundMain.
    uint32_t init = FindInitFromMode(rom, info);
    const uint32_t wrapper = info.song_start - kMainSize;
    if (!init && rom.Contains(wrapper, kMainSize) && rom.U16(wrapper) == kPushLr && BlTarget(rom, wrapper + 2) == main)
    {
        for (uint32_t at = wrapper - 2; at + kInitReach >= wrapper && rom.Contains(at, 2); at -= 2)
        {
            if (IsPush(rom.U16(at)))
            {
                init = rom.U16(at) == kInitPush ? at : 0;
                break;
            }
        }
    }
    if (!init)
    {
        error = "no routine has m4aSoundInit's shape";
        return false;
    }

    routines.init = init;
    routines.song_start = info.song_start;
    routines.main = main;
    routines.vsync = vsync;
    return !info.camelot_mixer || FindCamelotRoutines(rom, vsyncs, routines, error);
}

GameDriver::GameDriver(const Rom& rom, const DriverInfo& info, const GameRoutines& routines)
    : rom_(rom), info_(info), routines_(routines), system_(rom)
{
    // Camelot's mixer lies in the code that the game's boot code copies to IWRAM.
    for (uint32_t i = 0; i < routines_.copy_size; i += 4)
    {
        system_.Store(routines_.copy_destination + i, 4, system_.Load(routines_.copy_source + i, 4));
    }

    try
    {
        system_.Call(routines_.init | 1);
    }
    catch (const std::runtime_error& e)
    {
        throw std::runtime_error(std::string("the driver's init failed: ") + e.what());
    }

    sound_info_ = system_.Load(kSoundInfoPointer, 4);
    if (!InRam(sound_info_, kSoundInfoPcmBuffer + 2 * kPcmBufferSize))
    {
        throw std::runtime_error("the driver's init left no SoundInfo");
    }

    // The mono mixer's init sends DirectSound A to both sides and leaves DirectSound B out.
    mono_ = !(system_.Load(agb::kRegSoundCntH, 2) & 0x3000);

    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (recording_ && address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }
    };
}

void GameDriver::Start(int song)
{
    const uint32_t entry = info_.song_table + 8 * uint32_t(song);
    if (song < 0 || song >= info_.song_count || !rom_.Contains(entry, 8))
    {
        throw std::runtime_error("song " + std::to_string(song) + " is past the song table");
    }

    const uint32_t player = info_.player_table + kPlayerEntrySize * rom_.U16(entry + 4);
    if (!rom_.Contains(player, kPlayerEntrySize) || !InRam(rom_.U32(player), 8))
    {
        throw std::runtime_error("song " + std::to_string(song) + "'s music player is past the player table");
    }

    player_ = rom_.U32(player);
    tracks_ = rom_.U32(player + 4);
    track_count_ = rom_.Ptr(player + 8)[0];
    const std::array<uint32_t, 1> args{uint32_t(song)};
    system_.Call(routines_.song_start | 1, args);
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

bool GameDriver::Playing()
{
    const uint32_t status = system_.Load(player_ + 4, 4);
    if ((status & 0xFFFF) && !(status & 0x80000000))
    {
        return true;
    }

    for (const GameChannel& c : Channels())
    {
        if (c.status)
        {
            return true;
        }
    }

    return false;
}

int GameDriver::TrackOf(uint32_t address) const
{
    if (address < tracks_ || address >= tracks_ + kTrackSize * uint32_t(track_count_))
    {
        return -1;
    }

    return int((address - tracks_) / kTrackSize);
}

void GameDriver::Frame(std::vector<int8_t>& right, std::vector<int8_t>& left)
{
    // The vertical count stays at the start of the vertical blank. A mixer with a limit of lines then never stops
    // early.
    system_.HoldVcount(agb::kVblankLine);
    sound_writes_.clear();
    recording_ = true;
    system_.Call(routines_.vsync | 1);

    // The frame's part of the buffer: the part after the parts that the DMA has played since it started the buffer.
    // Camelot's output stage can write two points for each of the mixer's points, into buffers twice as long.
    const uint32_t scale = routines_.output ? uint32_t(info_.camelot_output_points) : 1;
    const uint32_t counter = system_.Load(sound_info_ + kSoundInfoDmaCounter, 1);
    const uint32_t period = system_.Load(sound_info_ + kSoundInfoDmaPeriod, 1);
    const uint32_t points = scale * system_.Load(sound_info_ + kSoundInfoFramePoints, 4);
    const uint32_t size = scale * kPcmBufferSize;
    const uint32_t offset = counter > 1 ? (period - (counter - 1)) * points : 0;
    if (points > size || offset > size - points)
    {
        recording_ = false;
        system_.HoldVcount(std::nullopt);
        throw std::runtime_error("the driver's frame lies outside its buffer");
    }

    // The VBlank interrupt of Camelot's driver copies the DMA counter for SoundMain, and runs the output stage after
    // it. SoundMain returns 0 when it has mixed the frame.
    if (routines_.output)
    {
        system_.Store(routines_.counter_copy, 1, counter);
    }
    const uint32_t mixed = system_.Call(routines_.main | 1);
    if (routines_.output && mixed == 0)
    {
        const std::array<uint32_t, 1> args{routines_.output_argument};
        system_.Call(routines_.output | 1, args);
    }
    recording_ = false;
    system_.HoldVcount(std::nullopt);

    const uint32_t buffer = (routines_.output ? routines_.pcm_buffer : sound_info_ + kSoundInfoPcmBuffer) + offset;
    right.resize(points);
    left.resize(points);
    for (uint32_t i = 0; i < points; i++)
    {
        right[i] = int8_t(system_.Load(buffer + i, 1));
        left[i] = mono_ ? right[i] : int8_t(system_.Load(buffer + size + i, 1));
    }
}

bool GameDriver::Mono() const
{
    return mono_;
}

double GameDriver::Rate()
{
    return 16777216.0 / (0x10000 - system_.Load(agb::kRegTimer0Reload, 2));
}

std::vector<GameChannel> GameDriver::Channels()
{
    std::vector<GameChannel> channels;
    // A later version of Camelot's driver mixes more channels than the SoundInfo says.
    const uint32_t limit = info_.camelot_mixer ? uint32_t(info_.max_channels)
                                               : uint32_t(system_.Load(sound_info_ + kSoundInfoMaxChannels, 1));
    for (uint32_t i = 0; i < limit; i++)
    {
        channels.push_back(ReadChannel(sound_info_ + kSoundInfoChannels + kChannelSize * i));
    }

    if (const uint32_t cgb = system_.Load(sound_info_ + kSoundInfoCgbChannels, 4))
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            channels.push_back(ReadChannel(cgb + kChannelSize * i));
        }
    }

    return channels;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

uint32_t GameDriver::SoundInfo() const
{
    return sound_info_;
}

GameChannel GameDriver::ReadChannel(uint32_t address)
{
    const auto u8 = [&](uint32_t offset)
    {
        return uint8_t(system_.Load(address + offset, 1));
    };
    const auto u32 = [&](uint32_t offset)
    {
        return system_.Load(address + offset, 4);
    };

    GameChannel c;
    c.status = u8(0);
    c.type = u8(1);
    c.right = u8(2);
    c.left = u8(3);
    c.key = u8(8);
    c.envelope = u8(9);
    c.goal = u8(10);
    c.counter = u8(11);
    c.midi_key = u8(17);
    c.velocity = u8(18);
    c.priority = u8(19);
    c.sustain_goal = u8(25);
    c.pan = u8(27);
    c.count = int32_t(u32(24));
    c.fraction = u32(28);
    c.frequency = u32(32);
    c.wave = u32(36);
    c.position = u32(40);
    c.track = u32(44);
    return c;
}

} // namespace ntdlib::mp2k
