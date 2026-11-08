// SPDX-License-Identifier: MIT

#include "ntdlib/alphadream/game_driver.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/thumb.h"
#include "step_routine.h"

namespace ntdlib::alphadream
{
namespace
{

// The mix: it fills half 0 when the half is free and the DMA plays half 1, or else half 1 in the same way, and calls
// the routine of the sample channels.
constexpr const char* kMixPattern = "4Bxx 7819 2900 D1xx 7898 2800 D0xx 2101 7019 4Axx Exxx 785A 2A00 D1xx 7898 2801";
constexpr uint32_t kHalvesLoad = 0;
constexpr uint32_t kBufferLoad = 0x12;
constexpr uint32_t kChannelsCall = 0x34;
constexpr uint32_t kMaxChannelsLoad = 0x20;

// VBlank's swap: the DMA's next half goes to the other one, and that half is free.
constexpr const char* kSwapPattern = "4Bxx 2200 7898 2101 4048 7098 541A D1xx";

// The mixer's setup: LZ77UnCompWram of the mixer, then the clear of the sample channels' records.
constexpr const char* kSetupPattern = "A0xx 49xx DF11 48xx 2100 2210 C002 3A01 D1FC";

// The mixer's step from a sample's rate and a pitch.
constexpr uint32_t kStepScaleLoad = 0x1E;

// The halves' offset from FIFO A's buffer to FIFO B's.
constexpr uint32_t kBufferBBytes = 2 * kFrameSamples;

// DMA 1's control and its enable bit. The swap writes DMA 2's control in the same way.
constexpr uint32_t kRegDma1Control = 0x040000C6;
constexpr uint32_t kDmaEnable = 0x8000;

constexpr uint32_t kRegTimer0Reload = 0x04000100;

bool InRam(uint32_t address, uint32_t base, uint32_t size)
{
    return address >= base && address < base + size;
}

// The literal that `ldr rX, [pc, #imm]` at `at` loads, or 0 when no such load is there or the literal is outside
// IWRAM, and outside EWRAM too unless `ewram`.
uint32_t RamLiteral(const Rom& rom, uint32_t at, bool ewram = false)
{
    if (!rom.Contains(at, 2) || (rom.U16(at) & 0xF800) != 0x4800)
    {
        return 0;
    }

    const uint32_t value = agb::ThumbLiteral(rom, at);
    const bool in_ram =
        InRam(value, agb::kIwramBase, agb::kIwramSize) || (ewram && InRam(value, agb::kEwramBase, agb::kEwramSize));

    return in_ram ? value : 0;
}

// The state that `ldr r0, [pc, #imm]` at `at` loads into r0, or 0 when the load isn't there or the state is outside
// EWRAM.
uint32_t StateLoad(const Rom& rom, uint32_t at)
{
    if (!rom.Contains(at, 2) || (rom.U16(at) & 0xFF00) != 0x4800)
    {
        return 0;
    }

    const uint32_t value = agb::ThumbLiteral(rom, at);

    return InRam(value, agb::kEwramBase, agb::kEwramSize) ? value : 0;
}

// Finds the game's call of the init beside a call of the setup: after it in A88E and A84J, and before it in B85A.
bool FindInit(const Rom& rom, GameRoutines& r)
{
    for (const uint32_t call : agb::FindCalls(rom, r.setup))
    {
        if (const uint32_t state = StateLoad(rom, call + 4))
        {
            if (const uint32_t init = agb::BlTarget(rom, call + 6))
            {
                r.state = state;
                r.init = init;
                r.init_first = false;
                return true;
            }
        }
        if (call >= agb::kRomBase + 6)
        {
            const uint32_t state = StateLoad(rom, call - 6);
            const uint32_t init = agb::BlTarget(rom, call - 4);
            if (state && init)
            {
                r.state = state;
                r.init = init;
                r.init_first = true;
                return true;
            }
        }
    }

    return false;
}

} // namespace

std::optional<GameRoutines> FindGameRoutines(const Rom& rom, const DriverInfo& info)
{
    GameRoutines r;
    r.start = info.start_routine;
    r.mix = agb::FindFirstThumb(rom, kMixPattern);
    r.swap = agb::FindFirstThumb(rom, kSwapPattern);
    r.setup = agb::FindFirstThumb(rom, kSetupPattern);
    r.step = agb::FindFirstThumb(rom, kStepRoutine);
    if (!r.start || !r.mix || !r.swap || !r.setup || !r.step)
    {
        return std::nullopt;
    }

    r.step_scale = agb::ThumbLiteral(rom, r.step + kStepScaleLoad);
    r.halves = RamLiteral(rom, r.mix + kHalvesLoad);
    r.buffer = RamLiteral(rom, r.mix + kBufferLoad);

    // The routine of the sample channels loads their records first: from IWRAM in A88E and A84J, and EWRAM in B85A.
    if (const uint32_t routine = agb::BlTarget(rom, r.mix + kChannelsCall))
    {
        for (uint32_t at = routine; at < routine + kMaxChannelsLoad && !r.channels; at += 2)
        {
            r.channels = RamLiteral(rom, at, true);
        }
    }

    // The game calls the per-frame routine just before the mix.
    for (const uint32_t call : agb::FindCalls(rom, r.mix))
    {
        if (call >= agb::kRomBase + 4)
        {
            if (const uint32_t frame = agb::BlTarget(rom, call - 4))
            {
                r.frame = frame;
                break;
            }
        }
    }

    if (!FindInit(rom, r) || !r.halves || !r.buffer || !r.channels || !r.frame)
    {
        return std::nullopt;
    }

    return r;
}

uint32_t MixStep(const Rom& rom, const DriverInfo& info, const GameRoutines& routines, uint32_t rate, int key, int fine)
{
    // The key's note and octave from the key table. The note's step is between two entries of the step table, by
    // the fine tune, and falls an octave for each octave below the table's. The sample's rate scales it, and the
    // routine's factor last.
    const uint8_t entry = rom.U8(info.key_table + uint32_t(uint8_t(key)));
    const uint32_t at = info.step_table + 4 * uint32_t(entry & 0xF);
    const uint32_t low = rom.U32(at);
    const uint32_t high = rom.U32(at + 4);
    const uint32_t step = (low + ((high - low) * uint32_t(uint8_t(fine)) >> 8)) >> (entry >> 4);
    const uint32_t scaled = uint32_t(uint64_t(rate) * step >> 32);

    return scaled + uint32_t(uint64_t(scaled) * routines.step_scale >> 32);
}

GameDriver::GameDriver(const Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }

        // The swap turns DMA 1 off and on again. Turning it on reloads its source.
        if (address == kRegDma1Control && size == 2)
        {
            if (!(value & kDmaEnable))
            {
                dma_off_ = true;
            }
            else if (dma_off_)
            {
                dma_off_ = false;
                restarted_ = true;
            }
        }
    };

    const std::array<uint32_t, 1> state = {routines_.state};
    if (routines_.init_first)
    {
        system_.Call(routines_.init | 1, state);
        system_.Call(routines_.setup | 1);
    }
    else
    {
        system_.Call(routines_.setup | 1);
        system_.Call(routines_.init | 1, state);
    }
}

void GameDriver::Start(int player, int song)
{
    sound_writes_.clear();
    const std::array<uint32_t, 2> args = {uint32_t(player), uint32_t(song)};
    system_.Call(routines_.start | 1, args);
}

void GameDriver::Swap()
{
    sound_writes_.clear();
    restarted_ = false;
    dma_off_ = false;
    system_.Call(routines_.swap | 1);
}

void GameDriver::Frame()
{
    sound_writes_.clear();
    system_.Call(routines_.frame | 1);
    if (before_mix)
    {
        before_mix();
    }

    const uint32_t before = system_.Load(routines_.halves, 2);
    system_.Call(routines_.mix | 1);
    const uint32_t after = system_.Load(routines_.halves, 2);
    mixed_half_ = (after & ~before & 1) ? 0 : (after & ~before & 0x100) ? 1 : -1;
}

uint32_t GameDriver::TimerPeriod()
{
    return 0x10000 - system_.Load(kRegTimer0Reload, 2);
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

bool GameDriver::DmaRestarted() const
{
    return restarted_;
}

int GameDriver::MixedHalf() const
{
    return mixed_half_;
}

uint32_t GameDriver::Half(int fifo, int half) const
{
    return routines_.buffer + kBufferBBytes * uint32_t(fifo) + uint32_t(kFrameSamples * half);
}

std::array<uint8_t, kChannelBytes> GameDriver::ChannelBytes(int channel)
{
    std::array<uint8_t, kChannelBytes> bytes = {};
    const uint32_t at = routines_.channels + uint32_t(kChannelBytes * channel);
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = uint8_t(system_.Load(at + i, 1));
    }

    return bytes;
}

uint32_t GameDriver::TrackRecord(int player, int track) const
{
    return routines_.state + kPlayerBytes * uint32_t(player) + kTrackBytes * uint32_t(track);
}

uint32_t GameDriver::Step(uint32_t rate, int key, int fine)
{
    const std::array<uint32_t, 3> args = {rate, uint32_t(key), uint32_t(fine)};

    return system_.Call(routines_.step | 1, args);
}

const GameRoutines& GameDriver::Routines() const
{
    return routines_;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

const agb::System& GameDriver::Gba() const
{
    return system_;
}

} // namespace ntdlib::alphadream
