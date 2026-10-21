// SPDX-License-Identifier: MIT

// The player of a stream sound (driver::StreamSoundPlayer in nw::snd and nn::atk): ntdlib's model of it, a template on
// the sequence model's policy (see sequence.h). The comment on a method that follows one of the game's routines gives
// the routine's address in Animal Crossing: amiibo Festival's executable, then in WarioWare: Get It Together!'s main.
//
// A game plays a stream from its file, a block at a time, into buffers that its voices play in turn. The model holds
// the whole file. Each of the stream's channels plays on a MultiVoice of one channel, as one wave from the stream's
// start, with the stream's loop: the blocks' samples decode as one run (see sar/stream_file.h). The voices take the
// highest priority, 255, and no channel. Up to 8 tracks each play up to 2 of the channels, with their volume, pan,
// surround pan, sends and filters, and with the sound's volume, pitch and sends. Each frame, the player gives each
// track's voices their parameters, as a track gives its channels theirs. The player finishes once a voice has ended:
// the stream ran out, or another voice took its place.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

namespace ntdlib::nwsnd
{

// The tracks that a stream sound plays, in their order: at most 8, each with at most 2 of the stream's channels. The
// archive's table gives them. A stream file of version 2.0.0 or earlier can hold a table of tracks too. The player then
// takes each of those tracks' volume, pan and channels in the place of the archive's track, with the defaults for the
// rest of the settings (StreamSoundFileReader::IsTrackInfoAvailable, 0x022BEFD8, and
// StreamSoundLoader::ReadTrackInfoFromStreamSoundFile). `valid` gives the tracks that play, a bit each from bit 0.
struct StreamTracks
{
    uint16_t valid = 0;
    std::vector<sar::StreamTrackInfo> tracks;
};

inline StreamTracks StreamTracksOf(const sar::StreamSoundInfo& sound, const sar::StreamFile& file)
{
    // SetupTrack: the tracks up to the last valid one, 8 at most.
    constexpr std::size_t kMaxTracks = 8;
    constexpr std::size_t kMaxChannels = 2;
    StreamTracks result;
    result.valid = sound.valid_tracks;
    std::size_t count = 0;
    for (uint32_t bits = sound.valid_tracks; bits != 0; bits >>= 1)
    {
        ++count;
    }
    count = std::min(count, kMaxTracks);
    result.tracks.resize(count);
    for (std::size_t i = 0; i < count && i < sound.tracks.size(); ++i)
    {
        result.tracks[i] = sound.tracks[i];
    }

    if (file.version <= 0x00020000 && file.tracks)
    {
        for (std::size_t i = 0; i < std::min(file.tracks->size(), count); ++i)
        {
            const sar::StreamFile::Track& from = (*file.tracks)[i];
            sar::StreamTrackInfo track;
            track.volume = from.volume;
            track.pan = from.pan;
            track.channels = from.channels;
            result.tracks[i] = track;
        }
    }

    for (sar::StreamTrackInfo& track : result.tracks)
    {
        if (track.channels.size() > kMaxChannels)
        {
            track.channels.resize(kMaxChannels);
        }
    }

    return result;
}

// The wave that a voice plays for channel `channel` of `stream`: the channel's samples from the stream's start, with
// the stream's format, rate and loop, and the DSP-ADPCM decoder's state at the start and at the loop. The caller places
// the samples where the console's voices read them. The format is DSP-ADPCM unless the stream holds PCM.
template <typename WaveInfo>
WaveInfo StreamChannelWave(const sar::StreamFile& stream, std::size_t channel)
{
    using SampleFormat = decltype(WaveInfo::format);
    WaveInfo wave;
    switch (stream.encoding)
    {
    case sar::WaveEncoding::kPcm8:
        wave.format = SampleFormat::kPcm8;
        break;
    case sar::WaveEncoding::kPcm16:
        wave.format = SampleFormat::kPcm16;
        break;
    default:
        wave.format = SampleFormat::kDspAdpcm;
        break;
    }

    wave.loop = stream.loop;
    wave.channel_count = 1;
    wave.sample_rate = stream.sample_rate;
    wave.loop_start = stream.loop_start;
    wave.loop_end = stream.sample_count;
    wave.original_loop_start = stream.original_loop_start;
    const sar::StreamFile::Channel& from = stream.channels[channel];
    if (from.adpcm)
    {
        auto& to = wave.channels[0];
        to.coefs = from.adpcm->coefs;
        to.context = {from.adpcm->pred_scale, from.adpcm->yn1, from.adpcm->yn2};
        to.loop_context = {from.adpcm->loop_pred_scale, from.adpcm->loop_yn1, from.adpcm->loop_yn2};
    }

    return wave;
}

template <typename Policy>
class StreamSoundPlayer
{
public:
    using Engine = typename Policy::Engine;
    using Receiver = typename Policy::Receiver;
    using WaveInfo = typename Policy::WaveInfo;

    // Player `id` of an engine, for the stream sound of `info`, with `tracks`. `channels` holds a wave for each channel
    // that the player plays: the stream's channels, up to 16, each as one wave. The voices output to `receiver` on a
    // console whose policy has receivers.
    StreamSoundPlayer(int id, const typename Policy::SoundInfo& info, const StreamTracks& tracks,
                      std::vector<WaveInfo> channels, const Receiver* receiver = nullptr);

    // The player's place among the engine's players. Each voice gives it as its owner, with its channel.
    int Id() const
    {
        return id_;
    }

    // The sound thread's start: a voice for each channel (AllocVoices), then the voices of the tracks that play start
    // with their waves (StartPlayer). The player finishes at once when the voices can't all be allocated.
    void Start(Engine& engine);

    // Each sound frame: the player finishes when a voice has gone, or else gives each track's voices their parameters
    // (Update, 0x022B5394 and 0xc0fb40, and UpdateVoiceParams for each track, 0x022B5024 and 0xc10460).
    void Update(Engine& engine);

    // The callback of channel `channel`'s voice (VoiceCallbackFunc, 0x022C6E38 and 0xc0fb00): a voice that ended or
    // was cancelled is freed, and the player lets a dropped one go.
    void VoiceEvent(Engine& engine, int channel, VoiceCallbackStatus status);

    bool IsFinished() const
    {
        return finished_;
    }

    // Whether the sound has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    int ChannelCount() const
    {
        return static_cast<int>(waves_.size());
    }

    // The MultiVoice that plays channel `channel`, or kNoVoice.
    VoiceId ChannelVoice(int channel) const
    {
        return channel >= 0 && channel < ChannelCount() ? voices_[static_cast<std::size_t>(channel)] : kNoVoice;
    }

private:
    // A track (StreamTrack): whether it plays, its settings, and what the game sets through the sound's handle: its
    // volume, its output line (-1 takes the player's) and its output parameters.
    struct Track
    {
        bool active = false;
        sar::StreamTrackInfo info;
        float volume = 1.0f;
        int32_t output_line = -1;
        typename Policy::Outputs outputs;
    };

    void UpdateVoiceParams(Engine& engine, const Track& track);

    // Frees every voice and finishes (FinishPlayer and FreeVoices, 0x022B3854 and 0x022B39CC, and 0xc0e0b0).
    void FinishPlayer(Engine& engine);

    int id_;
    std::vector<Track> tracks_;
    std::vector<WaveInfo> waves_;
    std::vector<VoiceId> voices_; // by channel
    bool started_ = false;
    bool finished_ = false;

    // What BasicSound gives the player from the sound's info and what the game sets (BasicSoundPlayer's parameters), as
    // a sequence's player holds them.
    float volume_ = 1.0f;
    float pitch_ = 1.0f;
    float lpf_ = 0.0f;
    float biquad_value_ = 0.0f;
    int8_t biquad_type_ = -1; // replaces the tracks' type when 0 or more
    uint32_t pan_mode_ = 0;
    uint32_t pan_curve_ = 0;
    uint32_t output_line_ = 1;
    typename Policy::Outputs outputs_;
    const Receiver* receiver_ = nullptr;

    // The sound's pitch and sends in the archive (ItemData, 0x022B3F68): the main send less 1, and the aux sends.
    float item_pitch_ = 1.0f;
    float item_main_send_ = 0.0f;
    std::array<float, 3> item_fx_send_{};
};

} // namespace ntdlib::nwsnd
