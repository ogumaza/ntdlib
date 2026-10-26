// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/stream_sound.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sound_values.h"
#include "ntdlib/rvl/snd/engine.h"

namespace ntdlib::rvl::snd
{
namespace
{

// The channels of a voice: a track's voice plays its track's first two.
constexpr std::size_t kVoiceChannels = 2;

} // namespace

int StrmPlayer::ChannelsKept(const SndBuild& build, const ntdlib::rsar::SoundInfo& sound, uint16_t version,
                             const ntdlib::rsar::StreamFile& stream)
{
    const int in_stream = static_cast<int>(stream.channels.size());
    if (!build.SinceMarch2008())
    {
        return std::min(in_stream, static_cast<int>(kVoiceChannels));
    }

    // ReadStrmSoundInfo: a count from version 1.4 on, and before a bit for each channel. Bits with a gap leave the
    // sound without its info.
    int count = sound.allocate_channels;
    if (version < 0x0104)
    {
        count = 0;
        bool gap = false;
        for (uint32_t bits = sound.allocate_channels; bits != 0; bits >>= 1)
        {
            if ((bits & 1) == 0)
            {
                gap = true;
            }
            else if (gap)
            {
                return -1;
            }
            else
            {
                count++;
            }
        }
    }

    // Setup keeps up to 16 channels. LoadHeader takes the stream's channels for a count of 0. The game's choice for a
    // count beyond the stream's channels isn't known. The model then keeps the stream's channels.
    count = std::min(count, kMaxChannels);
    if (count == 0 || count > in_stream)
    {
        count = std::min(in_stream, kMaxChannels);
    }

    return count;
}

std::optional<std::vector<StrmPlayer::Track>> StrmPlayer::TracksOf(const SndBuild& build,
                                                                   const ntdlib::rsar::SoundInfo& sound,
                                                                   uint16_t version,
                                                                   const ntdlib::rsar::StreamFile& stream)
{
    const int kept = ChannelsKept(build, sound, version, stream);
    if (kept <= 0)
    {
        return std::nullopt;
    }

    // The builds before March 2008 play one voice of the first two channels (SetupPlayer, 0x800496c4).
    if (!build.SinceMarch2008())
    {
        Track track;
        track.active = true;
        for (int channel = 0; channel < kept; channel++)
        {
            track.channels.push_back(static_cast<uint8_t>(channel));
        }

        return std::vector<Track>{track};
    }

    // Setup (Mario Kart Wii, 0x800a5a80): each bit of the entry's tracks up to 8 makes its track play. The player plays
    // the tracks up to the highest one. Without one, the sound doesn't start.
    std::vector<Track> tracks;
    int count = 0;
    for (uint32_t bits = sound.allocate_track_flags; bits != 0; bits >>= 1, count++)
    {
        if ((bits & 1) == 0)
        {
            continue;
        }
        if (count >= kMaxTracks)
        {
            break;
        }

        tracks.resize(static_cast<std::size_t>(count) + 1);
        tracks.back().active = true;
    }
    count = std::min(count, kMaxTracks);
    if (count == 0)
    {
        return std::nullopt;
    }
    tracks.resize(static_cast<std::size_t>(count));

    // LoadHeader reads each track's info from the stream file. A file without one of the tracks fails the sound. Each
    // channel that a track's voice plays has to be one that the player keeps.
    for (std::size_t i = 0; i < tracks.size(); i++)
    {
        if (i >= stream.tracks.size())
        {
            return std::nullopt;
        }

        const ntdlib::rsar::StreamFile::Track& from = stream.tracks[i];
        Track& track = tracks[i];
        track.volume = from.volume;
        track.pan = from.pan;
        track.channels.assign(
            from.channels.begin(),
            from.channels.begin() + static_cast<std::ptrdiff_t>(std::min(from.channels.size(), kVoiceChannels)));
        if (track.active &&
            (track.channels.empty() || std::any_of(track.channels.begin(), track.channels.end(),
                                                   [kept](uint8_t channel) { return channel >= kept; })))
        {
            return std::nullopt;
        }
    }

    return tracks;
}

StrmPlayer::StrmPlayer(int id, const ntdlib::rsar::SoundInfo& info, std::vector<Track> tracks, WaveData stream,
                       std::vector<WaveData::Channel> channels, const SndBuild& build)
    : id_(id),
      tracks_(std::move(tracks)),
      stream_(std::move(stream)),
      channels_(std::move(channels)),
      build_(build),
      voices_(tracks_.size(), kNoVoice)
{
    // PrepareStrmImpl: the volume, a division by 127.
    volume_ = ntdlib::nwsnd::RevolutionPolicy::PlayerVolume(info.volume);
}

void StrmPlayer::Update(Engine& engine)
{
    if (!started_ || finished_)
    {
        return;
    }

    if (!voices_started_)
    {
        if (!StartVoices(engine))
        {
            Stop(engine);
            return;
        }
    }
    else
    {
        // A track whose voice has gone stops the player.
        for (std::size_t track = 0; track < tracks_.size(); track++)
        {
            if (tracks_[track].active && voices_[track] == kNoVoice)
            {
                Stop(engine);
                return;
            }
        }
    }

    // The voices' parameters. The builds before March 2008 give the one voice the player's values. The later builds
    // give each track's voice the track's volume and pan on top. Archive mode leaves the player's pitch at 1, its pan
    // and surround pan at 0, its low-pass filter at 0 and its main send at 0. The sends are those that the game sets
    // on the sound.
    const std::array<float, ax::kAuxBusCount> sends = engine.SoundSends(id_);
    for (std::size_t index = 0; index < tracks_.size(); index++)
    {
        const Track& track = tracks_[index];
        if (!track.active || voices_[index] == kNoVoice)
        {
            continue;
        }

        float volume = 1.0f;
        volume *= volume_;
        float pan = 0.0f;
        if (build_.SinceMarch2008())
        {
            volume *= static_cast<float>(track.volume) / 127.0f;
            pan += ntdlib::nwsnd::PanOfSoundValue(track.pan);
        }

        Voice& voice = engine.Voices().At(voices_[index]);
        voice.SetVolume(volume);
        voice.SetPitch(1.0f);
        voice.SetPan(pan);
        voice.SetSurroundPan(0.0f);
        voice.SetLpfFreq(1.0f);
        voice.SetMainOutVolume(1.0f);
        voice.SetMainSend(0.0f);
        for (std::size_t bus = 0; bus < sends.size(); bus++)
        {
            voice.SetFxSend(static_cast<int>(bus), sends[bus]);
        }
    }
}

bool StrmPlayer::StartVoices(Engine& engine)
{
    // Each track's voice, at the highest priority, for its channels.
    for (std::size_t index = 0; index < tracks_.size(); index++)
    {
        const Track& track = tracks_[index];
        if (!track.active)
        {
            continue;
        }

        Voice* voice = engine.Voices().AllocVoice(static_cast<int>(track.channels.size()), Voice::kPriorityMax,
                                                  id_ + static_cast<int>(index));
        if (voice == nullptr)
        {
            return false;
        }
        voices_[index] = voice->Place();
    }

    // Each voice plays its channels from the stream's start (see stream_sound.h).
    for (std::size_t index = 0; index < tracks_.size(); index++)
    {
        const Track& track = tracks_[index];
        if (!track.active)
        {
            continue;
        }

        WaveData wave = stream_;
        wave.channel_count = static_cast<int>(track.channels.size());
        for (std::size_t i = 0; i < track.channels.size(); i++)
        {
            wave.channels[i] = channels_[track.channels[i]];
        }

        Voice& voice = engine.Voices().At(voices_[index]);
        voice.Setup(wave);
        voice.Start();
    }
    voices_started_ = true;

    return true;
}

void StrmPlayer::VoiceEvent(Engine& engine, int track, Voice::Status status)
{
    if (track < 0 || track >= TrackCount())
    {
        return;
    }

    // The player frees a voice that has played to its end or that was cancelled. A dropped voice is free already.
    VoiceId& voice = voices_[static_cast<std::size_t>(track)];
    if (voice != kNoVoice && (status == Voice::Status::kFinishWave || status == Voice::Status::kCancel))
    {
        engine.Voices().At(voice).Free();
    }

    voice = kNoVoice;
}

void StrmPlayer::Stop(Engine& engine)
{
    for (VoiceId& voice : voices_)
    {
        if (voice != kNoVoice)
        {
            engine.Voices().At(voice).Stop();
            engine.Voices().At(voice).Free();
            voice = kNoVoice;
        }
    }

    finished_ = true;
}

} // namespace ntdlib::rvl::snd
