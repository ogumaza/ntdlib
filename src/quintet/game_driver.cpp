// SPDX-License-Identifier: MIT

#include "ntdlib/quintet/game_driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/quintet/song.h"

namespace ntdlib::quintet
{
namespace
{

// The registers whose stores the driver makes: the sound registers up to the FIFOs, DMA 1's and DMA 2's, and timer 0's
// and timer 1's.
struct Range
{
    uint32_t first = 0;
    uint32_t end = 0;
};
constexpr std::array<Range, 3> kRecorded = {
    {{0x04000060, 0x040000A0}, {0x040000BC, 0x040000D4}, {0x04000100, 0x04000108}}};

// The FIFOs' registers, and the DMA sources that a program in EWRAM sets.
constexpr uint32_t kFifoA = 0x040000A0;
constexpr uint32_t kFifoBytes = 4;
constexpr std::array<uint32_t, 2> kDmaSources = {0x040000BC, 0x040000C8};

// The bits of the registers that the driver reads back that the hardware returns: SOUNDCNT_H's resets of the FIFOs read
// as 0, and so do NR30's bits 0 to 4.
struct ReadMask
{
    uint32_t address = 0;
    int size = 0;
    uint32_t mask = 0;
};
constexpr std::array<ReadMask, 2> kReadMasks = {{{0x04000082, 2, 0x770F}, {0x04000070, 1, 0xE0}}};

// SOUNDBIAS as the BIOS leaves it.
constexpr uint32_t kSoundBias = 0x04000088;
constexpr uint32_t kBiosBias = 0x0200;

// The size of a program that the game sends to another console: all of EWRAM.
constexpr uint32_t kProgramBytes = agb::kEwramSize;

} // namespace

GameDriver::GameDriver(const agb::Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    system_.Store(kSoundBias, 2, kBiosBias);

    // A program runs from the start of EWRAM.
    if (routines_.image)
    {
        for (uint32_t offset = 0; offset < kProgramBytes && rom.Contains(routines_.image + offset, 4); offset += 4)
        {
            system_.Store(agb::kEwramBase + offset, 4, rom.U32(routines_.image + offset));
        }
    }

    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (!recording_)
        {
            return;
        }

        if (address >= kFifoA && address < kFifoA + 2 * kFifoBytes)
        {
            value &= size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
            stream_.push_back({address, uint8_t(size), value});
        }
        else if (std::any_of(kRecorded.begin(), kRecorded.end(),
                             [&](const Range& r) { return address >= r.first && address < r.end; }))
        {
            value &= size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
            if (size == 4 && std::find(kDmaSources.begin(), kDmaSources.end(), address) != kDmaSources.end())
            {
                value = RomAddress(value);
            }
            writes_.push_back({address, uint8_t(size), value});
            stream_.push_back(writes_.back());
        }

        MaskReadBack(address, size);
    };

    recording_ = true;
    Call(routines_.sound_init);
    for (const GameRoutines::Call& call : routines_.init_calls)
    {
        Call(call.address, call.args);
    }
    init_writes_ = writes_;
    records_ = routines_.channels;
}

void GameDriver::Call(uint32_t address, std::span<const uint32_t> args)
{
    const uint64_t unmapped = system_.UnmappedAccesses();
    system_.Call(address | 1, args);
    if (system_.UnmappedAccesses() != unmapped)
    {
        throw std::runtime_error("the driver reached memory that the GBA doesn't map");
    }
}

void GameDriver::Play(uint32_t file, int index, bool effect)
{
    writes_.clear();
    stream_.clear();
    if (routines_.loader)
    {
        const std::array<uint32_t, 1> at = {RamAddress(file)};
        Call(routines_.loader, at);
    }

    const std::array<uint32_t, 1> number = {uint32_t(index)};
    records_ = effect ? routines_.effect_channels : routines_.channels;
    Call(effect ? routines_.play_effect : routines_.play, number);
}

void GameDriver::Frame()
{
    writes_.clear();
    stream_.clear();
    Call(routines_.frame);
}

uint32_t GameDriver::Position(int channel)
{
    const uint32_t record = system_.Load(records_ + 4u * uint32_t(channel), 4);
    return system_.Load(record + 4, 4);
}

bool GameDriver::Playing()
{
    for (int c = 0; c < kChannels; c++)
    {
        if (Position(c))
        {
            return true;
        }
    }

    return false;
}

uint32_t GameDriver::RomAddress(uint32_t address) const
{
    if (routines_.image && address >= agb::kEwramBase && address < agb::kEwramBase + kProgramBytes)
    {
        return routines_.image + (address - agb::kEwramBase);
    }

    return address;
}

uint32_t GameDriver::RamAddress(uint32_t address) const
{
    return routines_.image ? agb::kEwramBase + (address - routines_.image) : address;
}

void GameDriver::MaskReadBack(uint32_t address, int size)
{
    for (const ReadMask& m : kReadMasks)
    {
        if (address < m.address + uint32_t(m.size) && m.address < address + uint32_t(size))
        {
            const uint32_t stored = system_.Load(m.address, m.size);
            if ((stored & m.mask) != stored)
            {
                const bool recording = recording_;
                recording_ = false;
                system_.Store(m.address, m.size, stored & m.mask);
                recording_ = recording;
            }
        }
    }
}

} // namespace ntdlib::quintet
