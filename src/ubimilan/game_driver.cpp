// SPDX-License-Identifier: MIT

#include "ntdlib/ubimilan/game_driver.h"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace ntdlib::ubimilan
{
namespace
{

// A voice's fields, from its record.
constexpr uint32_t kVoiceSize = 0x28;
constexpr uint32_t kVoiceStopped = 0x1C;
constexpr uint32_t kVoiceLoops = 0x24;

// The word that gives the mixer's address, and the one that the game's start sets so that the first FIFO interrupt runs
// the mixer.
constexpr uint32_t kMixerPointer = 0x03000270;
constexpr uint32_t kMixerFlag = 0x0300026C;

// SOUNDBIAS, and what the BIOS leaves there.
constexpr uint32_t kSoundBias = 0x04000088;
constexpr uint32_t kBiosBias = 0x0200;

} // namespace

GameDriver::GameDriver(const Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    // The BIOS leaves the bias that the game's start adds to.
    system_.Store(kSoundBias, 2, kBiosBias);
    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }
    };

    Call(routines_.mixer_setup | 1);

    // A copy of the mixer at mixer_ram takes the place of the copy that the game's allocator places.
    const uint32_t size = system_.Load(routines_.mixer_size, 4);
    for (uint32_t i = 0; i < size; i += 4)
    {
        system_.Store(routines_.mixer_ram + i, 4, system_.Load(routines_.mixer + i, 4));
    }

    system_.Store(kMixerPointer, 4, routines_.mixer_ram);
    system_.Store(kMixerFlag, 4, 1);
    const std::array<uint32_t, 1> file = {routines_.bank_file};
    bank_ = system_.Call(routines_.file_lookup | 1, file);
    const std::array<uint32_t, 1> bank = {bank_};
    Call(routines_.init | 1, bank);
    init_writes_.swap(sound_writes_);

    const uint32_t reload = system_.Load(agb::kRegTimer0Reload, 2);
    run_cycles_ = kRunOutput * (0x10000 - reload);
}

void GameDriver::Call(uint32_t address, std::span<const uint32_t> args)
{
    system_.Call(address, args);
}

void GameDriver::Play(uint32_t piece)
{
    const std::array<uint32_t, 2> args = {piece, 1};
    Call(routines_.start | 1, args);
}

void GameDriver::Frame(std::vector<int8_t>& bytes)
{
    // The stores of a piece's start count in its first frame.
    if (frames_ > 0)
    {
        sound_writes_.clear();
    }

    Call(routines_.step | 1);
    Call(routines_.update | 1);
    frames_++;
    const uint64_t due = frames_ * agb::kFrameCycles / run_cycles_;
    while (runs_ < due)
    {
        Call(routines_.mixer_ram);
        for (uint32_t i = 0; i < kRunOutput; i++)
        {
            bytes.push_back(int8_t(system_.Load(routines_.output + i, 1)));
        }

        runs_++;
    }
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

const std::vector<agb::RegisterWrite>& GameDriver::InitWrites() const
{
    return init_writes_;
}

std::array<uint32_t, 5> GameDriver::TrackState()
{
    const uint32_t track = routines_.track;
    return {system_.Load(track, 2), system_.Load(track + 4, 4), system_.Load(track + 8, 4),
            system_.Load(track + 0xC, 4), system_.Load(track + 0x10, 4)};
}

bool GameDriver::VoicePlays(int v)
{
    return system_.Load(routines_.voices + uint32_t(v) * kVoiceSize + kVoiceStopped, 4) == 0;
}

std::array<uint32_t, 6> GameDriver::VoiceState(int v)
{
    const uint32_t voice = routines_.voices + uint32_t(v) * kVoiceSize;
    return {system_.Load(voice, 4),       system_.Load(voice + 4, 4),    system_.Load(voice + 8, 4),
            system_.Load(voice + 0xC, 4), system_.Load(voice + 0x10, 4), system_.Load(voice + kVoiceLoops, 4)};
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace ntdlib::ubimilan
