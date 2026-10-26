// SPDX-License-Identifier: MIT

// NW4R SND's player of a stream sound (StrmPlayer): ntdlib's model of it on the Wii. The comment on a method that
// follows one of the game's routines gives the routine's address in the main.dol of Wii Sports, then in that of Mario
// Kart Wii.
//
// A game plays a stream from its file, a block at a time, into a ring of buffers that its voices play round. The model
// keeps each of the stream's channels whole in MEM2. A voice plays its channels from the stream's start, with the
// stream's loop: the blocks' samples decode as one run (see rsar/stream_file.h). The game's voice starts with the
// history of the channel's DSP-ADPCM info and the predictor of the channel's first frame. Before the stream's end, the
// game loops the voice to the ring's place of the block that holds the loop's start, with the DSP-ADPCM info's state
// for the loop (UpdateDataLoopAddress, 0x8004a334 and 0x800a7820). The model's voice loops to that block's start in the
// stream with the same state. A stream without a loop ends at its last sample. The game loads each block before its
// voice reaches it, unless the disc falls behind. The game's voices start once the stream's first blocks have loaded.
// The model's voices start in the player's first update.
//
// The builds before March 2008 play the stream's first two channels on one voice. The builds from March 2008 play up to
// 8 tracks, each on a voice of its channels. The sound's entry gives the tracks and the channels that the player
// allocates. The stream file's table gives each track's channels, volume and pan. Every voice takes the highest
// priority, 255. The voices keep the dual pan mode and the square-root curve: no build gives a stream's voices the
// sound's pan mode or curve. Archive mode leaves the game's parameters at their starting values. The voices then take
// the volume of the sound's entry, the tracks' volumes and pans, and the sends that the game sets on the sound.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rvl::snd
{

class Engine;

class StrmPlayer
{
public:
    // The most tracks that a player of the builds from March 2008 plays, and the most channels of the stream that it
    // keeps.
    static constexpr int kMaxTracks = 8;
    static constexpr int kMaxChannels = 16;

    // A track that the player plays: the stream's channels that its voice plays, and the volume and pan that the stream
    // file gives the track. The builds before March 2008 play one track of the stream's first two channels, at full
    // volume and in the centre.
    struct Track
    {
        bool active = false;
        uint8_t volume = 127;
        uint8_t pan = 64;
        std::vector<uint8_t> channels;
    };

    // The tracks that the player of `build` plays for stream sound `sound` of an archive of `version` with `stream`,
    // by their numbers, or nothing if the sound can't play. The builds from March 2008 read the channels and the tracks
    // that the sound allocates from its entry (SoundArchiveFileReader::ReadStrmSoundInfo, Mario Kart Wii's main.dol
    // 0x8009eb70). An archive before version 1.4 gives a bit for each channel. Bits with a gap between them leave the
    // sound without its info. The player keeps the first channels of the stream up to the count, or all of them for a
    // count of 0. It plays the tracks up to the highest one that the sound allocates (StrmPlayer::Setup). The stream
    // file has to hold each of those tracks, and each track's channels have to be among those that the player keeps
    // (LoadHeader and Start). A track's voice plays the track's first two channels.
    static std::optional<std::vector<Track>> TracksOf(const SndBuild& build, const ntdlib::rsar::SoundInfo& sound,
                                                      uint16_t version, const ntdlib::rsar::StreamFile& stream);

    // The number of the stream's channels that the player of `build` keeps for stream sound `sound`, in the order of
    // the stream's channels (see TracksOf), or -1 if the entry's bits leave the sound without its info.
    static int ChannelsKept(const SndBuild& build, const ntdlib::rsar::SoundInfo& sound, uint16_t version,
                            const ntdlib::rsar::StreamFile& stream);

    // Player `id` of an engine, for the stream sound of `info`, with `tracks` (see TracksOf). `stream` holds the
    // stream's format, rate and loop, and `channels` each channel that the player keeps, at its address in MEM2. The
    // player follows `build` of NW4R SND.
    StrmPlayer(int id, const ntdlib::rsar::SoundInfo& info, std::vector<Track> tracks, WaveData stream,
               std::vector<WaveData::Channel> channels, const SndBuild& build);

    // The player's id among the engine's players (see Engine::kStreamPlayers). The owner of a track's voice is the id
    // plus the track's number.
    int Id() const
    {
        return id_;
    }

    // Marks the sound as started. The voices start in the player's next update.
    void Start()
    {
        started_ = true;
    }

    // Each sound frame (Update, 0x80049bb0 and 0x800a6e30): the voices start in the first (StrmPlayer::Start,
    // 0x80048cac and 0x800a5e10). In each frame, the player stops once a track's voice has gone, and otherwise gives
    // each track's voice its parameters (UpdateVoiceParams, 0x800a6ff0). The player finishes when its voices can't all
    // be allocated.
    void Update(Engine& engine);

    // The callback of track `track`'s voice (VoiceCallbackFunc, 0x8004a81c). The player frees a voice that has played
    // to its end or that was cancelled. A dropped voice is free already, and the player only clears its place.
    void VoiceEvent(Engine& engine, int track, Voice::Status status);

    bool IsFinished() const
    {
        return finished_;
    }

    // Whether the sound has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    int TrackCount() const
    {
        return static_cast<int>(tracks_.size());
    }

    // The voice that plays track `track`, or kNoVoice.
    VoiceId TrackVoice(int track) const
    {
        return track >= 0 && track < TrackCount() ? voices_[static_cast<std::size_t>(track)] : kNoVoice;
    }

private:
    // Allocates each track's voice (AllocChannels, 0x800499f0, and AllocVoices, 0x800a6d40), and starts each voice on
    // its channels. Returns false if a voice can't be allocated.
    bool StartVoices(Engine& engine);

    // Stops and frees the voices and finishes (Stop, 0x80048fb8).
    void Stop(Engine& engine);

    int id_;
    std::vector<Track> tracks_;
    WaveData stream_;                         // the format, the rate and the loop
    std::vector<WaveData::Channel> channels_; // the channels that the player keeps
    SndBuild build_;
    std::vector<VoiceId> voices_; // by track
    bool started_ = false;
    bool voices_started_ = false;
    bool finished_ = false;

    // The volume that SoundArchivePlayer::PrepareStrmImpl (0x800452a0) gives the sound from its entry. Archive mode
    // leaves the game's other parameters at their starting values.
    float volume_ = 1.0f;
};

} // namespace ntdlib::rvl::snd
