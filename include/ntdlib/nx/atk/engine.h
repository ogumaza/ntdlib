// SPDX-License-Identifier: MIT

// Archive mode's model of nn::atk for one sound archive. Each frame, the model runs what the sound thread runs
// (SoundThread::Run, 0xc18b40, around its FrameProcess, 0xc18590), then the audio renderer's frame. The model reads the
// archive's banks and waves where they lie in the archive, as a game's sound heap holds them, and plays the archive's
// sequence sounds and wave sounds. A stream sound plays from the bytes of its stream file, which the game keeps beside
// the archive. Each function's comment gives the address of the routine it follows in WarioWare: Get It Together!'s
// main.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/length_hooks.h"
#include "ntdlib/nwsnd/sound_players.h"
#include "ntdlib/nwsnd/voice.h"
#include "ntdlib/nx/atk/channel.h"
#include "ntdlib/nx/atk/effect.h"
#include "ntdlib/nx/atk/hardware.h"
#include "ntdlib/nx/atk/low_level_voice.h"
#include "ntdlib/nx/atk/sequence.h"
#include "ntdlib/nx/atk/sound_players.h"
#include "ntdlib/nx/atk/util.h"
#include "ntdlib/nx/atk/voice.h"
#include "ntdlib/nx/atk/voice_renderer.h"
#include "ntdlib/nx/audio/i3dl2.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"

namespace ntdlib::nx::atk
{

struct EngineOptions
{
    // The sound system's voice count (SoundSystemParam's +0x58): 96 unless the game changes it before its sound system
    // starts. The sound system's channel count and the renderer's voice count are the same.
    int voice_count = 96;
    HardwareSettings hardware;

    // The I3DL2 reverb a scene of the reference game puts on each of the aux buses A to C, if any (I3dl2AuxEffect).
    std::array<std::optional<audio::I3dl2ReverbParameterSet>, 3> aux_reverbs;
};

class Engine : public ntdlib::nwsnd::LengthHooks
{
public:
    // A copy of the engine's state between frames, for a snapshot of the model (see ArchiveModel::Snapshot).
    struct Snapshot;

    // Plays on `renderer`. The voices output to the renderer's mixes through `receiver`. The archive must outlive the
    // engine.
    Engine(audio::Renderer& renderer, const sar::SoundArchive& archive, const OutputReceiver& receiver,
           EngineOptions options = {});

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts sequence sound `index`, by its index in the archive's sound table. The game's sound archive player does
    // the same when a sound is started before a frame. The sound starts in the second sound frame after that (the first
    // frame takes the start to the sound thread). Returns the player, or nullptr if there's no such sequence or its
    // files aren't in the archive.
    SequenceSoundPlayer* StartSequence(uint32_t index);

    // Starts wave sound `index` in the same way. The player plays the wave of the sound's first note (see
    // nwsnd/wave_sound_player.h). Returns the player, or nullptr if there's no such wave sound, or if the archive lacks
    // its wave sound file or the file lacks the sound. A missing wave leaves the player nothing to play: it finishes in
    // its first frame. Throws xsflib::FormatError for a wave sound file that ntdlib can't read.
    WaveSoundPlayer* StartWaveSound(uint32_t index);

    // Starts stream sound `index` in the same way, with `file`: the bytes of the stream file that the sound's file
    // names beside the archive. The engine keeps the stream's channels. Returns the player, or nullptr if there's no
    // such stream sound or no track plays. Throws xsflib::FormatError for a file that sar::StreamFile::Read refuses.
    StreamSoundPlayer* StartStreamSound(uint32_t index, std::span<const uint8_t> file);

    // The same for a stream sound of `info`, which the archive needn't hold.
    StreamSoundPlayer* StartStreamSound(const sar::SoundInfo& info, std::span<const uint8_t> file);

    // A frame of 5 ms: the sound thread's frame, then the renderer's frame.
    void RunFrame();

    // Puts an effect after the others on aux bus `bus` (0 to 2: A to C) of the voices' receiver, and enables the
    // effect. The reference game does the same (SoundSystem::AppendEffect for an aux, 0xc12200,
    // OutputMixer::AppendEffect, 0xc2d6b0, and EffectAux::SetEnabled, 0xc2b510). The effect's buffers hold one ring of
    // whole frames, with all of the effect's channels in each frame.
    void AppendEffect(int bus, std::unique_ptr<EffectAux> effect);

    // Whether a sound still plays or a channel still sounds.
    bool IsBusy() const;

    // A host's mute of track `track` of the player at `player` among the engine's players (its Id()). nn::atk has no
    // such mute. The game's mute and silence of a track change what the track plays. A muted track plays on as before,
    // and none of the voices of its notes reach the mixes, their releases and sends included. A change takes effect
    // across the next frame (see audio::Renderer::SetVoiceMuted). The mutes aren't part of a snapshot. They stay
    // through a restore. A wave sound's channel plays on track 0. A stream's voices take no mutes and no meters.
    void MuteTrack(int player, int track, bool mute);
    bool IsTrackMuted(int player, int track) const;

    // A host's meters of the 16 tracks of the player at `player`, or no meters for -1. nn::atk has no such meters. A
    // track's meter adds up its voices' output as it reaches the console's output: the front left and right of each
    // bus without an effect. TrackPeaks gives the peaks of track `track`'s meter in the last frame, left and right, on
    // the scale of the output's 16-bit samples. Without meters, it gives 0. The meters change nothing in the output.
    // They aren't part of a snapshot, and stay through a restore.
    void MeterTracks(int player);
    std::array<int32_t, 2> TrackPeaks(int track) const;

    Snapshot Save() const;

    // Returns the engine to a snapshot that it saved. The players that started after the snapshot go, and the pointers
    // that StartSequence, StartWaveSound and StartStreamSound gave for them dangle. The other players keep their
    // places. The restore replaces each effect with a copy of the snapshot's effect. Pointers kept to the effects that
    // AppendEffect took then dangle too. The banks and waves that notes have read stay read. Throws
    // std::invalid_argument for a snapshot of another engine, or one with players or effects that the engine doesn't
    // have.
    void Restore(const Snapshot& snapshot);

    Random& Rng()
    {
        return random_;
    }

    // The sequences' global variables, which every player shares, from -1.
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_vars_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    const ChannelManager& Channels() const
    {
        return channels_;
    }

    MultiVoiceManager& Voices()
    {
        return voices_;
    }

    const MultiVoiceManager& Voices() const
    {
        return voices_;
    }

    // The MultiVoices' voices on the audio renderer.
    VoiceRenderer& Renderer()
    {
        return voice_renderer_;
    }

    LowLevelVoiceAllocator& LowLevelVoices()
    {
        return allocator_;
    }

    // A note on the bank in a sequence's bank slot (Bank::NoteOn, 0xc0b140): the velocity region for the key and
    // velocity, its wave, and a channel that starts it. Returns nullptr if the slot, the program, the region or the
    // wave isn't there, or no channel is free.
    Channel* NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info);

    // Passes a channel's callback on to its owner: track `track` of the player at `player` among the engine's players.
    void ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status);

    // Passes the callback of a stream's voice on to the stream's player.
    void VoiceEvent(const ntdlib::nwsnd::VoiceOwner& owner, ntdlib::nwsnd::VoiceCallbackStatus status);

    // The counted loops that the length analysis takes as endless, each a track and its loop's body (see ntdlib's
    // LoopsForEver).
    void SetEndlessLoops(std::set<std::pair<int, uint32_t>> loops)
    {
        endless_loops_ = std::move(loops);
    }

    bool LoopsForEver(const SequenceSoundPlayer&, int track, uint32_t address) const
    {
        return endless_loops_.contains({track, address});
    }

private:
    using Players = ntdlib::nwsnd::SoundPlayers<SequenceSoundPlayer, WaveSoundPlayer, StreamSoundPlayer>;

    // An effect on a bus, with the renderer's aux and the buffer it processes the samples in.
    struct AttachedEffect
    {
        std::unique_ptr<EffectAux> effect;
        int aux = -1;
        std::vector<int32_t> work;
    };

    void FrameProcess();

    // Each effect of each bus, in order, reads what the renderer sent, processes it and writes it back
    // (HardwareManager's update of the effects, 0xc05f30, for the receiver's sub-mix, 0xc2d3b0).
    void UpdateEffects();

    // Mutes each voice of the renderer that plays a note of a muted track, and unmutes the others.
    void MuteVoices();

    // Sets the renderer's meters for the metered player's tracks. Each voice that plays a note of one of its tracks
    // gets the track's meter, and the other voices none.
    void MeterVoices();
    void SetRendererMeters();

    audio::Renderer* renderer_;
    const sar::SoundArchive* archive_;
    const OutputReceiver* receiver_;
    EngineOptions options_;
    LowLevelVoiceAllocator allocator_;
    VoiceRenderer voice_renderer_;
    MultiVoiceManager voices_;
    ChannelManager channels_;
    Random random_;
    std::array<int16_t, 16> global_vars_;

    // The players and the starts that wait. Declared after the channels. The players are then destroyed first, and
    // their tracks' hold on channels goes with them.
    Players players_;
    std::array<std::vector<AttachedEffect>, 4> effects_; // by bus, the main bus first

    // The banks, wave archives and waves that notes play. Each wave reads as WaveFileReader::ReadWaveInfo (0xc0b410)
    // reads it.
    using Files = ntdlib::nwsnd::ArchiveFiles<SequencePolicy>;
    Files files_;

    // The streams that stream sounds play. Their channels stay while the engine runs, through a restore too.
    std::vector<std::unique_ptr<const sar::StreamFile>> streams_;

    std::set<std::pair<int, uint32_t>> endless_loops_;
    std::set<std::pair<int, int>> muted_tracks_; // each a player and a track
    int metered_player_ = -1;                    // the player whose tracks have meters (MeterTracks)
};

// The voices at each level, the channels, the players and the starts that wait, the random numbers, the global
// variables and the effects. The voices point into the engine that saved them.
struct Engine::Snapshot
{
    const Engine* owner = nullptr;
    LowLevelVoiceAllocator::Snapshot low_level_voices;
    VoiceRenderer::Snapshot buffers;
    MultiVoiceManager voices;
    ChannelManager channels;
    Random random;
    std::array<int16_t, 16> global_vars{};
    Players::Snapshot players;
    std::array<std::vector<std::shared_ptr<const EffectAux>>, 4> effects; // by bus, as the engine holds them
};

// Archive mode's whole stack for one sound archive: the renderer, set up as nn::atk sets it up by default, and the
// engine. The final mix has six channels and one bus. Sub-mix 0 has four buses of six channels: the main bus and the
// aux buses A to C, each sent to the final mix's channels at 1.0, with the options' reverbs on the aux buses. The
// voices output to sub-mix 0, and the device sink takes the final mix's front left and right.
struct ArchiveModel
{
    // A copy of the model between frames: the state of the renderer and the engine. A snapshot points into the model
    // that saved it, and can only be restored there.
    struct Snapshot
    {
        audio::Renderer::Snapshot renderer;
        Engine::Snapshot engine;
    };

    // The renderer's frame: 240 samples at 48 kHz.
    static constexpr int32_t kSampleRate = 48000;
    static constexpr int32_t kFrameSamples = 240;

    // `observer`, if any, sees every call made on the renderer, from the first (audio::Renderer::SetObserver).
    explicit ArchiveModel(const sar::SoundArchive& archive, EngineOptions options = {},
                          audio::RendererObserver* observer = nullptr);

    Snapshot Save() const;

    // Returns the model to a snapshot that it saved (see Engine::Restore). Throws std::invalid_argument for a snapshot
    // with a part from another model, or one that the engine refuses. A refused snapshot changes nothing.
    void Restore(const Snapshot& snapshot);

    audio::Renderer renderer;
    OutputReceiver sub_mix;
    std::unique_ptr<Engine> engine;
};

} // namespace ntdlib::nx::atk
