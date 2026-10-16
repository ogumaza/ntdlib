// SPDX-License-Identifier: MIT

// Archive mode's model of nw::snd for one sound archive. The model runs the sound thread's frame
// (SoundThread::FrameProcess) and AX's frame callback (HwCallbackProc) around cafe/ax. The model lays the archive's
// wave archives out in AX's memory, the same way a game's sound heap would hold them. The model plays the archive's
// sequence sounds. Each function's comment gives the address of the routine it follows in Animal Crossing: amiibo
// Festival's executable.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/cafe/snd/ax_voice.h"
#include "ntdlib/cafe/snd/channel.h"
#include "ntdlib/cafe/snd/effect_setup.h"
#include "ntdlib/cafe/snd/effects.h"
#include "ntdlib/cafe/snd/renderer.h"
#include "ntdlib/cafe/snd/sequence.h"
#include "ntdlib/cafe/snd/util.h"
#include "ntdlib/cafe/snd/voice.h"
#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/sar/formats.h"

namespace ntdlib::cafe::snd
{

struct EngineOptions
{
    // nw::snd's voice and channel count, the .data word at 0x101AA478. The count is 96 unless the game sets another
    // before starting its sound system.
    int voice_count = 96;

    // The devices' output modes and the output volume. Every rip plays the defaults: stereo on both devices. The other
    // modes' paths follow the game's code.
    HardwareSettings hardware;

    // How the game starts AX (ArchiveModel): the 48 kHz renderer rather than the 32 kHz one, and each device's output
    // reaching 48 kHz after its final mix stage rather than before it. The reference game's settings are the defaults.
    bool renderer_48k = false;
    bool upsample_after_final_mix = true;

    // The effects that the game has on its aux buses when the sound starts (see Effects), in the order that it
    // appended them.
    std::vector<EffectSetup> effects;
};

class Engine
{
public:
    // A copy of the engine's state between frames, for a snapshot of the model (see ArchiveModel::Snapshot).
    struct Snapshot;

    // Runs on `system`, with the archive's waves mapped into `memory`. `system` must have been started. AX reads
    // samples from `memory`. The archive must outlive the engine.
    Engine(ax::System& system, ax::Memory& memory, const sar::SoundArchive& archive, EngineOptions options = {});

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    ~Engine();

    // Starts sequence sound `index`, by its index in the archive's sound table. The game's sound archive player does
    // the same when a sound is started before a frame. The sound starts in the second sound frame after that (the first
    // frame takes the start to the sound thread). Returns the player, or nullptr if there's no such sequence or its
    // sequence file isn't in the archive.
    SequenceSoundPlayer* StartSequence(uint32_t index);

    // A frame of 3 ms. AX renders, and AX's frame callback updates nw::snd's AX voices (HwCallbackProc, 0x022A9FD8).
    // Then the sound thread's frame runs (FrameProcess, 0x0229F228).
    void RunFrame();

    // Whether a sound still plays or a channel still sounds.
    bool IsBusy() const;

    // A host's mute of track `track` of the player at `player` among the engine's players (its Id()). nw::snd has no
    // such mute. The game's mute and silence of a track change what the track plays. A muted track plays on as before,
    // and none of the voices of its notes reach AX's buses, their releases and sends included. A change takes effect
    // across the next frame (see ax::System::SetVoiceMuted). The mutes aren't part of a snapshot. They stay through a
    // restore. The track's mix from the frames before the snapshot can still reach the output of the first two frames
    // after the restore (see ax::System::Restore).
    void MuteTrack(int player, int track, bool mute);
    bool IsTrackMuted(int player, int track) const;

    // A host's meters of the 16 tracks of the player at `player`, or no meters for -1. nw::snd has no such meters. A
    // track's meter adds up its voices' mix into the TV's main bus, front left and right. An aux bus reaches the output
    // only through its effects, and the meters leave the aux buses out. TrackPeaks gives the peaks of track `track`'s
    // meter in the last frame, left and right, on the scale of the buses' 16-bit samples (see
    // ax::System::MeterPeaks). Without meters, it gives 0. The meters change nothing in the output. They aren't part
    // of a snapshot, and stay through a restore.
    void MeterTracks(int player);
    std::array<int32_t, 2> TrackPeaks(int track) const;

    Snapshot Save() const;

    // Returns the engine to a snapshot that it saved. The players that started after the snapshot go, and the pointers
    // that StartSequence gave for them dangle. The other players keep their places. The banks and waves that notes have
    // read stay read, and stay in AX's memory. Throws std::invalid_argument for a snapshot of another engine, or one
    // with players that the engine doesn't have. A refused snapshot changes nothing.
    void Restore(const Snapshot& snapshot);

    // Whether an effect is on an aux bus.
    bool HasEffects() const
    {
        return effects_.Any();
    }

    // Whether the effects may still ring: an effect is on a bus, and the TV's output had sound (louder than ±8) in the
    // last second. An effect rings on after the notes have ended. A delay's echo can come back after a quiet gap.
    bool EffectsRinging() const
    {
        return effects_.Any() && ringing_frames_ > 0;
    }

    Random& Rng()
    {
        return random_;
    }

    // The sequences' global variables. Every player shares them (InitSequenceSoundPlayer, 0x022C8504, sets them to -1).
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

    // The MultiVoices' voices on AX.
    AxRenderer& Renderer()
    {
        return renderer_;
    }

    // A note on the bank in a sequence's bank slot (Bank::NoteOn, 0x022C4A0C): the velocity region for the key and
    // velocity, its wave, and a channel that starts it. Returns nullptr if the slot, the program, the region or the
    // wave isn't there, or no channel is free.
    Channel* NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info);

    // Passes a channel's callback on to its owner: track `track` of the player at `player` among the engine's players.
    void ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status);

    // ntdlib's SequenceSoundPlayer calls these for the length analysis. OnLoop passes each loop to on_loop, and
    // OnTrackLoop each track's pass to on_track_loop, if it's set.
    bool FollowsLoops() const
    {
        return static_cast<bool>(on_loop);
    }

    void OnLoop(const SequenceSoundPlayer&, int track, uint32_t target)
    {
        on_loop(track, target);
    }

    void OnTrackLoop(const SequenceSoundPlayer&, int track, uint32_t target)
    {
        if (on_track_loop)
        {
            on_track_loop(track, target);
        }
    }

    // The length analysis: the loops of the sequence that the engine plays, while it sets on_loop (see ntdlib's
    // SequenceSoundPlayer). The model follows the loops only then. on_track_loop gets every track's passes.
    std::function<void(int track, uint32_t target)> on_loop;
    std::function<void(int track, uint32_t target)> on_track_loop;

private:
    // A start that waits holds its player's place, not a pointer. A restore can put another player object in that
    // place.
    struct PendingStart
    {
        int player; // the player's place in players_: its Id()
        int frames; // sound frames before it starts
    };

    using Files = ntdlib::nwsnd::ArchiveFiles<SequencePolicy>;

    // A second of frames, for EffectsRinging.
    static constexpr int kRingingFrames = 334;

    void FrameProcess();

    // Gives each AX voice the host's mute and meter of the track whose note it plays. A voice that plays no note has
    // neither.
    void SetHostMixes();

    // Puts a wave archive's whole file in AX's memory, as the game loads the file into its sound heap. The waves then
    // lie where they lie in the file.
    void MapWaveArchive(ntdlib::nwsnd::LoadedWaveArchive& loaded);

    ax::System* system_;
    ax::Memory* memory_;
    const sar::SoundArchive* archive_;
    EngineOptions options_;
    AxVoices ax_voices_;
    AxRenderer renderer_;
    MultiVoiceManager voices_;
    ChannelManager channels_;
    Random random_;
    std::array<int16_t, 16> global_vars_;

    // Declared after the channels. The players are then destroyed first, and their tracks' hold on channels goes with
    // them.
    std::vector<std::unique_ptr<SequenceSoundPlayer>> players_;
    std::vector<PendingStart> pending_;

    // The banks, wave archives and waves that notes play. Each wave reads as WaveFileReader::ReadWaveInfo (0x022B85B0)
    // reads it.
    Files files_;
    uint32_t next_address_ = 0x10000000;

    // The effects come last. They take their callbacks off AX's buses before the rest goes.
    Effects effects_;
    int ringing_frames_ = 0;

    std::set<std::pair<int, int>> muted_tracks_; // each a player and a track
    int metered_player_ = -1;                    // the player whose tracks have meters (MeterTracks)
};

// The AX voices with the MultiVoices' buffers, the MultiVoices, the channels, the players and the starts that wait,
// the random numbers, the global variables and the effects. The voices point into the engine that saved them.
struct Engine::Snapshot
{
    const Engine* owner = nullptr;
    AxVoices::Snapshot ax_voices;
    AxRenderer::Snapshot buffers;
    MultiVoiceManager voices;
    ChannelManager channels;
    Random random;
    std::array<int16_t, 16> global_vars{};
    std::vector<SequenceSoundPlayer> players;
    std::vector<PendingStart> pending;
    Effects::Snapshot effects;
    int ringing_frames = 0;
};

// Archive mode's whole stack for one sound archive: AX's memory, AX (started as the options specify) and the engine.
struct ArchiveModel
{
    // A copy of the model between frames: the state of AX and of the engine. A snapshot points into the model that
    // saved it, and can only be restored there. The wave archives that the engine has put in AX's memory stay there.
    struct Snapshot
    {
        ax::System::Snapshot system;
        Engine::Snapshot engine;
    };

    // AX's output: 144 stereo samples at 48 kHz in each frame of 3 ms.
    static constexpr int32_t kSampleRate = 48000;
    static constexpr int32_t kFrameSamples = ax::kOutputSamplesPerFrame;

    explicit ArchiveModel(const sar::SoundArchive& archive, EngineOptions options = {});

    Snapshot Save() const;

    // Returns the model to a snapshot that it saved (see Engine::Restore). Throws std::invalid_argument for a snapshot
    // with a part from another model, or one that the engine refuses. A refused snapshot changes nothing.
    void Restore(const Snapshot& snapshot);

    ax::Memory memory;
    ax::System system{memory};
    std::unique_ptr<Engine> engine;
};

} // namespace ntdlib::cafe::snd
