// SPDX-License-Identifier: MIT

// nw::snd's players of wave sounds and of stream sounds on the 3DS (driver::WaveSoundPlayer and
// driver::StreamSoundPlayer), modelled on Pokémon X's code.bin. The comment on a method that follows one of the game's
// routines gives the routine's address. The players follow ntdlib's players of the Wii U and the Switch
// (nwsnd/wave_sound_player.h and nwsnd/stream_sound_player.h). The 3DS's players multiply by code.bin's reciprocals of
// 63 and 127, and they read fewer settings.
//
// A wave sound plays the wave of its first note on a channel, as a track plays a note. The channel takes the wave
// sound's pitch, pan, surround pan, sends to buses A and B and envelope. It takes the player's filters alone: the 3DS
// reads no filters from a wave sound file.
//
// A game plays a stream from its file, a block at a time, into buffers that its voices play in turn. The model holds
// the whole file. Each of the stream's channels plays on a voice of one channel, as one wave from the stream's start,
// with the stream's loop: the blocks' samples decode as one run (see sar/stream_file.h). The voices take the DSP's
// highest priority, 0x7fff, and no channel. Up to 4 tracks each play up to 2 of up to 8 channels, with their volume,
// pan and surround pan, and with the sound's volume. A track's flags give its voices the front bypass, or their mix
// into the DSP's bus of two channels (see nnsnd::Voice::SetTwoChannelBus). The voices take the player's pitch, filters
// and sends: the 3DS's tracks have none. The voices keep the pan mode and curve 0. The player finishes once a voice has
// ended: the stream ran out, or another voice took its place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

namespace ntdlib::ctr::snd
{

class Engine;

// What the game gives a player through the sound's handle (BasicSoundPlayer's parameters, which 0x31f1a0 sets). The
// model keeps their starting values, with the sound's volume, pan mode and curve from the archive.
struct PlayerParameters
{
    float volume = 1.0f;            // +0x14
    float pitch = 1.0f;             // +0x18
    float pan = 0.0f;               // +0x1c
    float span = 0.0f;              // +0x20
    float lpf = 0.0f;               // +0x24
    float biquad_value = 0.0f;      // +0x28
    uint8_t biquad_type = 0;        // +0x2c
    bool front_bypass = false;      // +0x2e
    float main_send = 0.0f;         // +0x30
    uint8_t pan_mode = 0;           // +0x34
    uint8_t pan_curve = 0;          // +0x35
    std::array<float, 2> fx_send{}; // +0x38
};

class WaveSoundPlayer
{
public:
    // Player `id` of an engine, for the sound of `info` with the settings of `sound` from its wave sound file. `wave`
    // is the wave of the sound's first note, or nothing when the note or its wave is missing.
    WaveSoundPlayer(int id, const ntdlib::sar::SoundInfo& info, const ntdlib::sar::WaveSoundFile::WaveSound& sound,
                    std::optional<WaveInfo> wave);

    // The player's place among the engine's players. The channel gives it as its owner, with track 0.
    int Id() const
    {
        return id_;
    }

    // The sound thread's start. The channel starts in the player's next update.
    void Start()
    {
        started_ = true;
    }

    // Each sound frame: the channel starts in the first, and takes the player's parameters in each. The player finishes
    // once its channel has ended, or when the channel can't start (Update, 0x31efdc).
    void Update(Engine& engine);

    // The channel's callback (0x31efb4): the player lets its channel go.
    void ChannelEvent(Engine&, int, Channel&)
    {
        channel_ = ntdlib::nwsnd::kNoChannel;
    }

    bool IsFinished() const
    {
        return finished_;
    }

    // Whether the sound has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    // The place of the channel that plays the wave, or kNoChannel before it starts and after it ends.
    ntdlib::nwsnd::ChannelId ChannelPlace() const
    {
        return channel_;
    }

private:
    // Allocates the channel, gives it the envelope and starts the wave (StartChannel, 0x31ec24). Returns whether the
    // channel started.
    bool StartChannel(Engine& engine);

    // The player's parameters and the wave sound's settings reach the channel (UpdateChannel, 0x31ed80).
    void UpdateChannel(Engine& engine);

    int id_;
    std::optional<WaveInfo> wave_;
    ntdlib::sar::WaveSoundFile::WaveSound settings_; // without the tracks and the notes
    bool started_ = false;
    bool finished_ = false;
    bool channel_started_ = false; // +0x5c
    ntdlib::nwsnd::ChannelId channel_ = ntdlib::nwsnd::kNoChannel;
    PlayerParameters parameters_;
    float pan_range_ = 1.0f;            // +0x60
    uint8_t channel_priority_ = 64;     // +0x5e
    bool release_priority_fix_ = false; // +0x5d
};

// The tracks that a stream sound plays, in their order: the tracks up to the last that plays, at most 4, each with at
// most 2 of the stream's channels. `valid` gives the tracks that play, a bit each from bit 0. From CSAR 2.3.0.0 on, the
// archive's table of tracks gives them (SoundArchiveFileReader::ReadStreamSoundInfo, 0x48f64c). A track past the table
// plays no channel. An archive before 2.3.0.0 holds no table. The model then takes the stream file's tracks, as the
// Wii U's library does with a file of version 2.0.0 or earlier. No game's code shows that for the 3DS.
struct StreamTracks
{
    uint16_t valid = 0;
    std::vector<ntdlib::sar::StreamTrackInfo> tracks;
};

StreamTracks StreamTracksOf(const ntdlib::sar::StreamSoundInfo& sound, const ntdlib::sar::StreamFile& file);

class StreamSoundPlayer
{
public:
    // The most channels and tracks that the player plays (0x180898).
    static constexpr std::size_t kMaxChannels = 8;
    static constexpr std::size_t kMaxTracks = 4;

    // Player `id` of an engine, for the stream sound of `info`, with `tracks`. `channels` holds a wave for each channel
    // that the player plays: the stream's channels, up to 8, each as one wave.
    StreamSoundPlayer(int id, const ntdlib::sar::SoundInfo& info, const StreamTracks& tracks,
                      std::vector<WaveInfo> channels);

    // The player's place among the engine's players. Each voice gives it as its owner, with its channel.
    int Id() const
    {
        return id_;
    }

    // The sound thread's start: a voice for each channel and the modes of the tracks' voices (0x180128), then the
    // voices of the tracks that play start with their waves (StartPlayer, 0x31f9dc). The player finishes at once when
    // the voices can't all be allocated.
    void Start(Engine& engine);

    // Each sound frame: the player finishes when a voice has gone, or else gives each track's voices their parameters
    // (Update, 0x31fb28, and UpdateVoiceParams for each track, 0x31f6d0).
    void Update(Engine& engine);

    // The callback of channel `channel`'s voice (0x187164): a voice whose wave ended or that was cancelled is freed,
    // and the player lets a dropped one go.
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

    // The voice that plays channel `channel`, or kNoVoice.
    VoiceId ChannelVoice(int channel) const
    {
        return channel >= 0 && channel < ChannelCount() ? voices_[static_cast<std::size_t>(channel)] : kNoVoice;
    }

private:
    // A track (+0x1adc, 32 bytes each): whether it plays, its settings, and what the game sets through the sound's
    // handle: its volume, pan and surround pan.
    struct Track
    {
        bool active = false;               // +0x0
        ntdlib::sar::StreamTrackInfo info; // +0xc: the channels' count, then the volume, the pan, the span, flags
        float volume = 1.0f;               // +0x14
        float pan = 0.0f;                  // +0x18
        float span = 0.0f;                 // +0x1c
    };

    void UpdateVoiceParams(Engine& engine, const Track& track);

    // Frees every voice and finishes (0x31f8e4).
    void FinishPlayer(Engine& engine);

    int id_;
    std::vector<Track> tracks_;
    std::vector<WaveInfo> waves_;
    std::vector<VoiceId> voices_; // by channel
    bool started_ = false;
    bool finished_ = false;
    PlayerParameters parameters_;
};

} // namespace ntdlib::ctr::snd
