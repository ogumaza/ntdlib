// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/game_driver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::rd2
{

GameDriver::GameDriver(const agb::Rom& rom, const GameRoutines& routines)
    : routines_(routines), system_(rom), player_(routines.music_player)
{
    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }
    };

    system_.Call(routines_.sound_init | 1);
}

void GameDriver::Play(const DriverInfo& info, int song, int version)
{
    const MusicSettings settings = VersionSettings(info, song, version);
    if (const Effect* effect = SongEffect(info, song))
    {
        player_ = routines_.effect_player;
        const std::array<uint32_t, 3> args = {uint32_t(player_), uint32_t(effect->sequence), uint32_t(effect->track)};
        system_.Call(routines_.effect_request | 1, args);
    }
    else if (settings.music >= 0 && routines_.play_music)
    {
        player_ = routines_.music_player;
        const std::array<uint32_t, 1> music = {uint32_t(settings.music)};
        system_.Call(routines_.play_music | 1, music);
        if (routines_.yoshi_mute)
        {
            const std::array<uint32_t, 1> riding = {settings.riding_yoshi ? 1u : 0u};
            system_.Call(routines_.yoshi_mute | 1, riding);
        }
    }
    else
    {
        player_ = routines_.music_player;
        const std::array<uint32_t, 2> args = {uint32_t(player_), uint32_t(song)};
        system_.Call(routines_.request | 1, args);
    }

    system_.Call(routines_.commit | 1);
}

void GameDriver::SetEchoGoal(uint8_t level)
{
    system_.Store(routines_.echo_goal, 1, level);
}

uint32_t GameDriver::PlayerAddress()
{
    return routines_.players + uint32_t(routines_.player_bytes) * uint32_t(player_);
}

void GameDriver::MuteTrack(int track, bool mute)
{
    const uint32_t record = system_.Load(PlayerAddress() + 8 + 4 * uint32_t(track), 4);
    if (record)
    {
        system_.Store(record + kTrackMuteByte, 1, mute ? 1 : 0);
    }
}

void GameDriver::Frame()
{
    sound_writes_.clear();
    system_.Call(routines_.vblank | 1);
    system_.Call(routines_.frame | 1);
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

std::vector<int8_t> GameDriver::OutputBytes()
{
    const uint32_t index = system_.Load(routines_.output_index, 1);
    const uint32_t buffer = system_.Load(routines_.outputs + 4 * index, 4);
    std::vector<int8_t> bytes(352);
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = int8_t(system_.Load(buffer + i, 1));
    }

    return bytes;
}

std::vector<uint8_t> GameDriver::VoiceBytes(int voice)
{
    std::vector<uint8_t> bytes(kVoiceRecordBytes);
    const uint32_t at = routines_.voices + uint32_t(voice * kVoiceRecordBytes);
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = uint8_t(system_.Load(at + i, 1));
    }

    return bytes;
}

bool GameDriver::Playing()
{
    return system_.Load(PlayerAddress() + uint32_t(routines_.player_playing), 1) != 0;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace ntdlib::rd2
