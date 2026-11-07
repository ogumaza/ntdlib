// SPDX-License-Identifier: MIT

#include "ntdlib/krawall/game_driver.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"

namespace ntdlib::krawall
{
namespace
{

// The stand-in for getDmaAddress(&left, &right): it stores the buffers' addresses and returns the frame's samples, as
// a movs and an lsls by 1 or more.
std::array<uint16_t, 12> DmaStandIn(uint32_t left, uint32_t right, int samples)
{
    int shift = 1;
    while (samples >> shift >= 256)
    {
        shift++;
    }
    if ((samples >> shift) << shift != samples)
    {
        throw std::invalid_argument("the stand-in for getDmaAddress() can't return " + std::to_string(samples));
    }

    return {
        0x4A03,                                // ldr r2, =left
        0x6002,                                // str r2, [r0]
        0x4A03,                                // ldr r2, =right
        0x600A,                                // str r2, [r1]
        uint16_t(0x2000 | (samples >> shift)), // movs r0, #samples >> shift
        uint16_t(shift << 6),                  // lsls r0, r0, #shift
        0x4770,                                // bx lr
        0x46C0,                                // nop
        uint16_t(left),
        uint16_t(left >> 16),
        uint16_t(right),
        uint16_t(right >> 16),
    };
}

} // namespace

GameDriver::GameDriver(const agb::Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    if (routines_.vram)
    {
        throw std::invalid_argument("the game keeps Krawall's code in the video memory, which isn't modelled");
    }

    for (const GameRoutines::Copy& copy : routines_.copies)
    {
        for (uint32_t i = 0; i < copy.size; i++)
        {
            system_.Store(copy.destination + i, 1, system_.Load(copy.source + i, 1));
        }
    }
    for (const GameRoutines::Store& store : routines_.stores)
    {
        system_.Store(store.address, store.size, store.value);
    }

    if (routines_.dma_address)
    {
        if (routines_.dma_address & 3)
        {
            throw std::invalid_argument("getDmaAddress() isn't 4-byte aligned");
        }
        const auto stand_in = DmaStandIn(routines_.out_left, routines_.out_right, routines_.frame_samples);
        for (size_t i = 0; i < stand_in.size(); i++)
        {
            system_.Store(routines_.dma_address + uint32_t(2 * i), 2, stand_in[i]);
        }
    }

    for (const GameRoutines::Call& call : routines_.init)
    {
        Call(call);
    }
}

uint32_t GameDriver::Call(const GameRoutines::Call& call)
{
    return system_.Call(call.arm ? call.routine : call.routine | 1, call.args);
}

uint32_t GameDriver::Play(const DriverInfo& info, const ModuleSong& song)
{
    const int mode = PlayMode(song);
    unpacked_low_ = unpacked_high_ = translation_ = 0;
    if (routines_.play_entry)
    {
        // The game's routine plays the entry in its mode and at its music volume.
        uint32_t n = 0;
        while (system_.Load(routines_.module_table + 4 * n, 4) != song.module)
        {
            if (++n == 0x10000)
            {
                throw std::invalid_argument("the game's table of modules doesn't list the module");
            }
        }
        Call({routines_.play_entry, {n}});
        return song.module;
    }

    if (!routines_.play_file)
    {
        Call({routines_.play, {song.module, uint32_t(mode), uint32_t(std::max(song.song, 0))}});
        for (const GameRoutines::Store& store : routines_.after_play)
        {
            system_.Store(store.address, store.size, store.value);
        }
        if (song.music_volume >= 0 && routines_.set_music_vol)
        {
            Call({routines_.set_music_vol, {uint32_t(song.music_volume), 0}});
        }
        return song.module;
    }

    const auto at = std::find(info.modules.begin(), info.modules.end(), song.module);
    if (at == info.modules.end() || size_t(at - info.modules.begin()) >= info.files.size())
    {
        throw std::invalid_argument("the module has no music file");
    }
    const uint32_t file = info.files[size_t(at - info.modules.begin())];
    Call({routines_.play_file, {file, mode & kModeJingle ? 0u : 1u}});
    const uint32_t module = system_.Load(routines_.player + 8, 4);

    // The file lists the samples to unpack, and the module's data follow at the next multiple of 4, with their size.
    const uint32_t samples = system_.Load(file, 1);
    const uint32_t packed = (file + 1 + samples + 3) & ~3u;
    const uint32_t size = system_.Load(packed, 2) | system_.Load(packed + 2, 1) << 16;
    unpacked_low_ = system_.Load(routines_.unpacked, 4);
    unpacked_high_ = unpacked_low_ + size;
    translation_ = song.module - module;
    return module;
}

void GameDriver::SkipFrame()
{
    if (routines_.frame_routine)
    {
        Call({routines_.frame_routine, {}});
    }
}

void GameDriver::Frame()
{
    if (routines_.frame_routine)
    {
        Call({routines_.frame_routine, {}});
    }
    for (const GameRoutines::Store& store : routines_.worker_state)
    {
        system_.Store(store.address, store.size, store.value);
    }
    Call({routines_.worker, {}, true});
}

std::vector<int8_t> GameDriver::Left()
{
    std::vector<int8_t> out(size_t(routines_.frame_samples));
    for (size_t i = 0; i < out.size(); i++)
    {
        out[i] = int8_t(system_.Load(routines_.out_left + uint32_t(i), 1));
    }

    return out;
}

std::vector<int8_t> GameDriver::Right()
{
    std::vector<int8_t> out(size_t(routines_.frame_samples));
    for (size_t i = 0; i < out.size(); i++)
    {
        out[i] = int8_t(system_.Load(routines_.out_right + uint32_t(i), 1));
    }

    return out;
}

bool GameDriver::Playing()
{
    return system_.Load(routines_.player + 4, 4) != 0;
}

std::vector<uint8_t> GameDriver::Bytes(uint32_t address, uint32_t size)
{
    std::vector<uint8_t> bytes(size);
    for (uint32_t i = 0; i < size; i++)
    {
        bytes[i] = uint8_t(system_.Load(address + i, 1));
    }

    return bytes;
}

void GameDriver::Translate(std::vector<uint8_t>& record, std::span<const uint32_t> offsets) const
{
    if (unpacked_high_ == 0)
    {
        return;
    }

    for (const uint32_t at : offsets)
    {
        uint32_t word = uint32_t(record[at]) | uint32_t(record[at + 1]) << 8 | uint32_t(record[at + 2]) << 16 |
                        uint32_t(record[at + 3]) << 24;
        if (word >= unpacked_low_ && word < unpacked_high_)
        {
            word += translation_;
            for (uint32_t i = 0; i < 4; i++)
            {
                record[at + i] = uint8_t(word >> (8 * i));
            }
        }
    }
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace ntdlib::krawall
