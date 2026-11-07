// SPDX-License-Identifier: MIT

#include "ntdlib/rare/game_driver.h"

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/system.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/sequencer.h"

namespace ntdlib::rare
{

GameDriver::GameDriver(const agb::Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }
    };

    system_.Call(routines_.init | 1);
}

void GameDriver::Play(const DriverInfo& info, int tune, uint16_t silenced)
{
    const std::array<uint32_t, 1> request = {uint32_t(tune)};
    system_.Call(routines_.request | 1, request);

    volume_ = uint32_t(rare::MusicVolume(info, tune));
    if (routines_.volume_routine)
    {
        system_.Store(routines_.volume_option, 2, routines_.volume_level);
        volume_ = system_.Call(routines_.volume_routine | 1);
    }
    const std::array<uint32_t, 1> volume = {volume_};
    system_.Call(routines_.volume_setter | 1, volume);
    silenced_ = silenced;
}

void GameDriver::Frame()
{
    // The game sets the levels between the driver's frames. The driver gives every channel a level of 128 as a tune
    // starts, so the tune's first frame plays every channel.
    for (int c = 0; c < kChannels && routines_.channel_levels; c++)
    {
        if (silenced_ >> c & 1)
        {
            system_.Store(routines_.channel_levels + uint32_t(c), 1, 0);
        }
    }

    sound_writes_.clear();
    system_.Call(routines_.frame | 1);
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

std::vector<int8_t> GameDriver::OutputBytes()
{
    const uint32_t flag = system_.Load(routines_.buffer_flag, 4);
    const uint32_t buffer = system_.Load(routines_.buffers[flag == 0 ? 0 : 1], 4);
    std::vector<int8_t> bytes(system_.Load(routines_.samples, 4));
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = int8_t(system_.Load(buffer + i, 1));
    }

    return bytes;
}

std::vector<uint8_t> GameDriver::SlotBytes(int slot)
{
    std::vector<uint8_t> bytes(kSlotRecordBytes);
    const uint32_t at = routines_.note_slots + uint32_t(slot * kSlotRecordBytes);
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = uint8_t(system_.Load(at + i, 1));
    }

    return bytes;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace ntdlib::rare
