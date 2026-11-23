// SPDX-License-Identifier: MIT

// What the engines of the 3DS, the Wii U and the Switch do in the same way: the start of a wave sound, the pass of a
// channel's callback to its player, whether a sound still plays, and the host's mutes of tracks. NintendoWare's sound
// libraries have no such mutes. Each engine forwards StartWaveSound, ChannelEvent, IsBusy, MuteTrack and IsTrackMuted
// here. Each engine applies the mutes to the voices of its console.

#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <variant>

#include "ntdlib/sar/formats.h"

namespace ntdlib::nwsnd
{

// Starts wave sound `index` of `archive`, by its index in the archive's sound table. Calls `start` with the sound's
// info, the sound in its wave sound file, and the ID of the wave of the sound's first note, as WaveFileReader reads it.
// The ID is nullptr for a sound without notes, or a note whose wave the file doesn't list. Returns what `start`
// returns, or nullptr if there's no such wave sound, or if the archive has no wave sound file or the file doesn't hold
// the sound. Throws xsflib::FormatError for a wave sound file that ntdlib can't read.
template <typename Start>
auto StartWaveSound(const sar::SoundArchive& archive, uint32_t index, const sar::Options& options, const Start& start)
    -> decltype(start(archive.Sounds()[index], std::declval<const sar::WaveSoundFile::WaveSound&>(), nullptr))
{
    if (index >= archive.Sounds().size())
    {
        return nullptr;
    }

    const sar::SoundInfo& info = archive.Sounds()[index];
    if (info.type != sar::SoundType::kWaveSound)
    {
        return nullptr;
    }

    const std::span<const uint8_t> file = archive.FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const sar::WaveSoundFile wsd = sar::WaveSoundFile::Parse(file, options);
    if (info.wave_sound_index >= wsd.sounds.size())
    {
        return nullptr;
    }

    const sar::WaveSoundFile::WaveSound& sound = wsd.sounds[info.wave_sound_index];
    const bool listed = !sound.notes.empty() && sound.notes[0].wave_id_index < wsd.waves.size();

    return start(info, sound, listed ? &wsd.waves[sound.notes[0].wave_id_index] : nullptr);
}

// Passes a channel's callback on to its owner, the player at `player` among `players`, if that kind of player accepts
// channel events.
template <typename Engine, typename Players, typename Channel>
void ForwardChannelEvent(Engine& engine, Players& players, int player, int track, Channel& channel)
{
    const auto forward = [&](auto& owner)
    {
        if constexpr (requires { owner.ChannelEvent(engine, track, channel); })
        {
            owner.ChannelEvent(engine, track, channel);
        }
    };
    std::visit(forward, players[player]);
}

// Whether a player hasn't finished, or a channel still sounds.
template <typename Players, typename Channels>
bool IsBusy(const Players& players, const Channels& channels)
{
    return players.AnyPlaying() || channels.ActiveCount() > 0;
}

// The tracks that a host mutes, each a player's Id() among the engine's players and a track.
class TrackMutes
{
public:
    void Set(int player, int track, bool mute)
    {
        if (mute)
        {
            muted_.insert({player, track});
        }
        else
        {
            muted_.erase({player, track});
        }
    }

    bool Contains(int player, int track) const
    {
        return muted_.contains({player, track});
    }

private:
    std::set<std::pair<int, int>> muted_;
};

} // namespace ntdlib::nwsnd
