// SPDX-License-Identifier: MIT

// The nw::snd sound engine for one sound archive: owns the voice and channel managers, the HardwareManager state and
// the players of sequences, wave sounds and stream sounds, and runs the sound frame (SoundThread::FrameProcess,
// code.bin 0x1666d0) between the receive and send steps of the nn::snd model it's given.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/device.h"
#include "ntdlib/ctr/dsp/fcram.h"
#include "ntdlib/ctr/nnsnd/sound_system.h"
#include "ntdlib/ctr/snd/sequence.h"
#include "ntdlib/ctr/snd/sound_players.h"
#include "ntdlib/ctr/snd/voice.h"
#include "ntdlib/nwsnd/length_hooks.h"
#include "ntdlib/nwsnd/sound_players.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/types.h"

namespace ntdlib::ctr::snd
{

struct EngineOptions
{
    // HardwareManager output mode (0 mono, 1 stereo, 2 surround).
    uint8_t output_mode = 1;

    // The enhanced mixing mode, on a DSP in that mode (dsp::FirmwareModel). When every voice is in use, the voice
    // manager takes another in place of a drop, up to VoiceManager::kEnhancedVoiceCount voices, and nn::snd does the
    // same with its voices. The voices work out their filters' coefficients for 48 kHz. Off, the model follows the
    // console to the bit.
    bool enhanced = false;
};

// A length analysis sets the engine's hooks (LengthHooks). ntdlib's SequenceSoundPlayer reports the
// sequence's loops, every track's passes and the open-track commands to them.
class Engine : public ntdlib::nwsnd::LengthHooks
{
public:
    struct LoadedWarc
    {
        std::span<const uint8_t> file;
        ntdlib::PAddr base = 0; // the whole CWAR's address in FCRAM
        ntdlib::sar::WaveArchive archive;
    };

    // The players, each in its place (its Id()).
    using Players = ntdlib::nwsnd::SoundPlayers<SequenceSoundPlayer, WaveSoundPlayer, StreamSoundPlayer>;

    // The engine's state, for a snapshot of the model (see ArchiveModel::Snapshot): a copy of the voices' pool, the
    // channels, the players and the caches. It keeps alive everything it points at.
    struct Snapshot
    {
        VoiceManager::Snapshot voices;
        ChannelManager channels{kChannelCount};
        ntdlib::nwsnd::Random random;
        Players::Snapshot players;
        std::map<uint32_t, std::shared_ptr<const ntdlib::sar::Bank>> banks;
        std::map<uint32_t, std::shared_ptr<const LoadedWarc>> warcs;
        std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves;
        std::array<int16_t, 16> global_vars{};
    };

    // More channels than can ever be in use at once. Each channel holds one of the 23 voices. A new note takes a
    // channel before a voice. While a voice is dropped to make room, there can be one channel more. The enhanced mode
    // keeps as many to spare beside its voices.
    static constexpr int kChannelCount = 64;
    static constexpr int kEnhancedChannelCount =
        static_cast<int>(VoiceManager::kEnhancedVoiceCount) + kChannelCount - VoiceManager::kVoiceCount;

    Engine(nnsnd::SoundSystem& snd, Fcram& fcram, const ntdlib::sar::SoundArchive& archive, EngineOptions options = {});

    nnsnd::SoundSystem& Snd()
    {
        return snd_;
    }

    // nw::snd's voices, by the places that channels hold.
    VoiceManager& Voices()
    {
        return voices_;
    }

    ChannelManager& Channels()
    {
        return channels_;
    }

    const ChannelManager& Channels() const
    {
        return channels_;
    }

    ntdlib::nwsnd::Random& Rng()
    {
        return random_;
    }

    uint8_t OutputMode() const
    {
        return options_.output_mode;
    }

    bool Enhanced() const
    {
        return options_.enhanced;
    }

    // Starts a sequence sound by its index in the archive's sound table. Returns the player, or nullptr if there's no
    // such sequence or its file isn't in the archive. Throws if the sequence's data is malformed.
    SequenceSoundPlayer* StartSequence(uint32_t index);

    // Starts wave sound `index` in the same way. The player plays the wave of the sound's first note (see
    // sound_players.h). Returns the player, or nullptr if there's no such wave sound, or if the archive lacks its wave
    // sound file or the file lacks the sound. A missing wave leaves the player nothing to play: it finishes in its
    // first frame. Throws xsflib::FormatError for a wave sound file that ntdlib can't read.
    WaveSoundPlayer* StartWaveSound(uint32_t index);

    // Starts stream sound `index` in the same way, with `file`: the bytes of the stream file that the sound's file
    // names beside the archive. The stream's channels go into FCRAM, where the DSP reads them. Returns the player, or
    // nullptr if there's no such stream sound, no track plays or no channel plays. Throws xsflib::FormatError for a
    // file that sar::StreamFile::Read refuses, and std::runtime_error when FCRAM can't hold the channels.
    StreamSoundPlayer* StartStreamSound(uint32_t index, std::span<const uint8_t> file);

    // The same for a stream sound of `info`, which the archive needn't hold.
    StreamSoundPlayer* StartStreamSound(const ntdlib::sar::SoundInfo& info, std::span<const uint8_t> file);

    // Bank::NoteOn (code.bin 0x48f954) for the bank in slot `bank_index` of a player's `bank_items`: finds the velocity
    // region, loads its wave and starts a channel for it.
    Channel* NoteOn(const std::vector<uint32_t>& bank_items, int bank_index, const NoteOnInfo& info);

    // A channel's callback, for its owner: track `track` of player `player`.
    void ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus status);

    // A stream voice's callback, for its owner: channel `channel` of player `player`.
    void VoiceEvent(int player, int channel, VoiceCallbackStatus status);

    // One sound frame: nn::snd receive, FrameProcess, nn::snd send. Returns false if the DSP stopped answering.
    bool RunFrame();

    // True while any player hasn't finished or any channel is sounding.
    bool IsBusy() const;

    // A host's mute of track `track` of the player at `player` among the engine's players (its Id()). nw::snd has no
    // such mute. The game's mute and silence of a track change what the track plays. A muted track plays on as before,
    // and the DSP voices of its notes reach the DSP's buses with gains of 0 (nnsnd::Voice::SetHostMuted). A change
    // reaches the DSP with the frame's parameters, and the DSP plays it from the next frame. The mutes aren't part of a
    // snapshot. They stay through a restore.
    void MuteTrack(int player, int track, bool mute);
    bool IsTrackMuted(int player, int track) const;

    // A host's meters of the 16 tracks of the player at `player`, or no meters for -1. nw::snd has no such meters. A
    // track's meter adds up the mix of its notes' DSP voices into the DSP's main bus (dsp::Device::SetMeters). The
    // voices take their meters with the frame's parameters, and the DSP mixes them in its next frame. TrackPeaks gives
    // the peaks of track `track`'s meter in the frame that the DSP mixed last, left and right, on the scale of the
    // output's samples. On a device without meters, and without meters, it gives 0. The meters change nothing in the
    // output. They aren't part of a snapshot, and stay through a restore.
    void MeterTracks(int player);
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The global sequence variables, which the game keeps in one table for every sequence player (initialized by
    // code.bin 0x13739c). Each engine has its own, so that engines on different threads don't share them.
    std::array<int16_t, 16>& GlobalVariables()
    {
        return global_vars_;
    }

    // Between frames.
    Snapshot Save() const;

    // Restores the engine from a snapshot. Restore FCRAM to the same state so cached wave archive addresses remain
    // valid. The players that started after the snapshot go, and the pointers that the starts gave for them dangle. The
    // other players keep their places. Throws std::invalid_argument for a snapshot with players that the engine doesn't
    // have. A refused snapshot changes nothing.
    void Restore(const Snapshot& snapshot);

private:
    void FrameProcess();

    // Adds a player of kind `Kind`, made from its place and `args`, and starts it at once. Its first update comes in
    // the next frame.
    template <typename Kind, typename... Args>
    Kind* StartNow(Args&&... args)
    {
        Kind& player = players_.Add<Kind>(0, std::forward<Args>(args)...);
        players_.StartDue(*this);

        return &player;
    }

    // Gives each DSP voice the host's mute of the track whose note it plays, and the track's meter if the metered
    // player plays it. A voice that plays no note isn't muted and has no meter.
    void SetHostMixes();
    const ntdlib::sar::Bank* GetBank(uint32_t bank_item);
    std::optional<WaveInfo> GetWave(uint32_t wave_archive_item, uint32_t index);
    const LoadedWarc* GetWarc(uint32_t wave_archive_item);

    nnsnd::SoundSystem& snd_;
    Fcram& fcram_;
    const ntdlib::sar::SoundArchive& archive_;
    EngineOptions options_;
    VoiceManager voices_;
    ChannelManager channels_;
    ntdlib::nwsnd::Random random_;

    Players players_;

    // Caches of what the archive's files hold. Entries never change once made. The banks and wave archives are shared.
    // Snapshots can then hold them.
    std::map<uint32_t, std::shared_ptr<const ntdlib::sar::Bank>> banks_;
    std::map<uint32_t, std::shared_ptr<const LoadedWarc>> warcs_;
    std::map<std::pair<uint32_t, uint32_t>, std::optional<WaveInfo>> waves_;

    std::array<int16_t, 16> global_vars_;

    std::set<std::pair<int, int>> muted_tracks_; // each a player and a track (MuteTrack)
    int metered_player_ = -1;                    // the player whose tracks have meters (MeterTracks)
};

// The FCRAM that archive mode gives the DSP.
inline constexpr std::size_t kArchiveModeFcram = 64 * 1024 * 1024;

// The DSP that archive mode runs on: the model of the DSP's audio firmware (ntdlib/ctr/dsp/firmware_model.h), or Teakra
// running a firmware image that the caller supplies. Both give the same samples. The model also has an enhanced mixing
// mode.
enum class DspKind
{
    kModel,
    kTeakra,
    kEnhancedModel,
};

// Archive mode's whole stack for one sound archive: FCRAM, the DSP, the nn::snd model and the engine. Playback and a
// length analysis both build it this way. A sample sink goes on `dsp` before Start.
struct ArchiveModel
{
    // Snapshot of FCRAM, the DSP, nn::snd and the engine. It holds pointers to this model's objects and can only be
    // restored to this model.
    struct Snapshot
    {
        Fcram::Snapshot fcram;
        std::shared_ptr<const dsp::DeviceState> dsp;
        nnsnd::SoundSystem snd;
        std::optional<Engine::Snapshot> engine;
    };

    explicit ArchiveModel(DspKind dsp_kind = DspKind::kModel);

    // Starts the DSP and then the engine on `archive`, which has to outlive the engine, with `output_mode` (0 mono, 1
    // stereo, 2 surround). Teakra runs `firmware`, and the model needs none. On the model in its enhanced mode, the
    // engine runs in the enhanced mode too. Returns false if the firmware didn't start, or if Teakra has no firmware to
    // run.
    bool Start(const ntdlib::sar::SoundArchive& archive, uint8_t output_mode = 1);
    bool Start(std::span<const uint8_t> firmware, const ntdlib::sar::SoundArchive& archive, uint8_t output_mode = 1);

    // Takes a snapshot between frames, sharing the memory pages that haven't changed since `previous`.
    Snapshot Save(const Snapshot* previous);

    void Restore(const Snapshot& snapshot);

    const DspKind kind;
    Fcram fcram{kArchiveModeFcram};
    std::unique_ptr<dsp::Device> dsp;
    nnsnd::SoundSystem snd{*dsp};
    std::optional<Engine> engine;
};

} // namespace ntdlib::ctr::snd
