// SPDX-License-Identifier: MIT

// A host's player of the Nintendo 64's sound engines. A FrameModel holds one engine's model. FramePlayer runs the
// model's frames on the audio microcode's model, and gives a host samples, snapshots, mutes and meters.
//
// A track is a channel of the engine's sequence. A libultra player's channels and the N64 Music Tools' channels keep
// their numbers. In an engine with several players, tracks 0 to 15 are the first player's and tracks 16 to 31 the
// second's. A voice belongs to the track of the note that it last started.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/n64/al_player.h"
#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/ambrella_player.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/audio_rsp.h"
#include "ntdlib/n64/ead_early.h"
#include "ntdlib/n64/ead_later.h"
#include "ntdlib/n64/ead_middle.h"
#include "ntdlib/n64/enhanced_mix.h"
#include "ntdlib/n64/mus_player.h"
#include "ntdlib/n64/sbn_player.h"

namespace ntdlib::n64
{

// What a host's player has to know of a model.
struct FrameFormat
{
    AudioMicrocode microcode = AudioMicrocode::kStandard;
    int output_rate = 32000;
    int frame_step = 16; // a frame's samples are a multiple of this
    std::vector<VoiceArea> areas;
};

FrameFormat AlFrameFormat(const AlSynthConfig& config);
FrameFormat EadEarlyFrameFormat(const EadEarlyConfig& config);
FrameFormat EadMiddleFrameFormat(const EadMiddleConfig& config);
FrameFormat EadLaterFrameFormat(const EadLaterConfig& config);
FrameFormat SbnFrameFormat(const SbnPlayerConfig& config);

class FrameModel
{
public:
    virtual ~FrameModel() = default;

    // A copy for a snapshot. The copy shares the original's memory. A restore puts that memory back.
    virtual std::unique_ptr<FrameModel> Clone() const = 0;

    // Takes the state of `other`, a clone of this model. The hooks of this model stay.
    virtual void TakeState(const FrameModel& other) = 0;

    // Appends a frame's commands to `list`. The list saves the samples at `output`.
    virtual void BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples) = 0;

    // Whether a sequence plays or a voice still sounds.
    virtual bool Busy() const = 0;

    virtual const FrameFormat& Format() const = 0;

protected:
    FrameModel() = default;
    FrameModel(const FrameModel&) = default;
    FrameModel& operator=(const FrameModel&) = default;
};

// A libultra sequence player on the synthesis driver, in either form of the driver.
class AlFrames final : public FrameModel
{
public:
    AlFrames(const AlSynthConfig& synth, const AlPlayerConfig& player, AudioMemory& memory);

    AlPlayer& Player();
    AlSynth& Synth();

    std::unique_ptr<FrameModel> Clone() const override;
    void TakeState(const FrameModel& other) override;
    void BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples) override;
    bool Busy() const override;
    const FrameFormat& Format() const override;

private:
    AlSynth synth_;
    AlPlayer player_;
    FrameFormat format_;
};

// The player in Hey You, Pikachu! on the compact form of the synthesis driver.
class AmbrellaFrames final : public FrameModel
{
public:
    AmbrellaFrames(const AlSynthConfig& synth, const AmbrellaPlayerConfig& player, AudioMemory& memory);

    AmbrellaPlayer& Player();
    AlSynth& Synth();

    std::unique_ptr<FrameModel> Clone() const override;
    void TakeState(const FrameModel& other) override;
    void BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples) override;
    bool Busy() const override;
    const FrameFormat& Format() const override;

private:
    AlSynth synth_;
    AmbrellaPlayer player_;
    FrameFormat format_;
};

// A player of the N64 Music Tools on the synthesis driver. `Model` is MusPlayer or MusLatePlayer.
template <typename Model, typename Config>
class MusFrames final : public FrameModel
{
public:
    MusFrames(const AlSynthConfig& synth, const Config& player, AudioMemory& memory);
    MusFrames(const MusFrames& other);
    MusFrames& operator=(const MusFrames&) = delete;

    Model& Player();
    AlSynth& Synth();

    std::unique_ptr<FrameModel> Clone() const override;
    void TakeState(const FrameModel& other) override;
    void BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples) override;
    bool Busy() const override;
    const FrameFormat& Format() const override;

private:
    AlSynth synth_;
    Model player_;
    int32_t client_samples_ = 0;
    FrameFormat format_;
};

using MusEarlyFrames = MusFrames<MusPlayer, MusPlayerConfig>;
using MusLateFrames = MusFrames<MusLatePlayer, MusLatePlayerConfig>;

// frame_player.cpp instantiates the templates for these models.
extern template class MusFrames<MusPlayer, MusPlayerConfig>;
extern template class MusFrames<MusLatePlayer, MusLatePlayerConfig>;

// An engine that builds its lists itself. `Model` is EadEarlyPlayer, EadMiddlePlayer, EadLaterPlayer or SbnPlayer.
template <typename Model>
class EngineFrames final : public FrameModel
{
public:
    // The arguments after `format` go to the model's constructor.
    template <typename... Arguments>
    explicit EngineFrames(FrameFormat format, Arguments&&... arguments)
        : player_(std::forward<Arguments>(arguments)...), format_(std::move(format))
    {
    }

    Model& Player();

    std::unique_ptr<FrameModel> Clone() const override;
    void TakeState(const FrameModel& other) override;
    void BuildFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples) override;
    bool Busy() const override;
    const FrameFormat& Format() const override;

private:
    Model player_;
    FrameFormat format_;
};

using EadEarlyFrames = EngineFrames<EadEarlyPlayer>;
using EadMiddleFrames = EngineFrames<EadMiddlePlayer>;
using EadLaterFrames = EngineFrames<EadLaterPlayer>;
using SbnFrames = EngineFrames<SbnPlayer>;

extern template class EngineFrames<EadEarlyPlayer>;
extern template class EngineFrames<EadMiddlePlayer>;
extern template class EngineFrames<EadLaterPlayer>;
extern template class EngineFrames<SbnPlayer>;

// How a FramePlayer mixes.
struct FramePlayerOptions
{
    // The enhanced mixing mode (see EnhancedMix). Off, the output follows the microcodes to the bit.
    bool enhanced = false;

    // The enhanced mode's output rate, or 0 for the game's. It takes no other value without the mode.
    int output_rate = 0;
};

class FramePlayer
{
public:
    static constexpr int kHostTracks = 32;

    // `memory` is the memory of the model's buffers. It holds the player's output too. `frame_samples` is the mean of
    // the samples in a frame: the output rate over the game's rate of lists. Each frame's samples are a multiple of the
    // format's step, and they keep to that mean as the game's do.
    //
    // In the enhanced mode, the model runs some frames ahead of the output. What a host changes in the model reaches
    // the output that many frames later than without the mode. A host's mutes reach the next frame that the player
    // mixes, and ramp across it. Samples and positions count at the output rate.
    FramePlayer(std::unique_ptr<FrameModel> model, AudioMemory& memory, double frame_samples,
                const FramePlayerOptions& options = {});
    FramePlayer(const FramePlayer&) = delete;
    FramePlayer& operator=(const FramePlayer&) = delete;

    FrameModel& Model();
    const FrameModel& Model() const;

    // The rate of the output.
    int OutputRate() const;

    // Plays `samples` samples and appends them to `out`, the left and the right side in turn. The enhanced mode rounds
    // its samples and holds them to 16 bits here.
    void Run(std::size_t samples, std::vector<int16_t>& out);

    // The same, on the scale of 16-bit samples. Without the enhanced mode, each sample is the 16-bit sample.
    void Run(std::size_t samples, std::vector<float>& out);

    // Runs `samples` samples in the same way, without mixing the frames that this starts. A voice's output is lost, and
    // the microcode's filters and delay lines keep their old states. The first mixed frames after this differ from a
    // full run's until the states settle. The rest of a frame that started without mixing is silent. The meters give 0.
    void Run(std::size_t samples);

    // The samples that the player has run to.
    uint64_t Position() const;

    bool Busy() const;

    // A host's mute of track `track`, from 0 to kHostTracks - 1. A muted track plays on as before, and none of the
    // voices of its notes reach the output or the effects, their releases included. A change takes effect with the next
    // frame. The mutes aren't part of a snapshot. They stay through a restore. Throws std::out_of_range for another
    // track.
    void MuteTrack(int track, bool mute);
    bool IsTrackMuted(int track) const;

    // A host's meters of the tracks. A track's meter adds up the output of its voices as the mixer adds them, on both
    // buses and before the game's effects. TrackPeaks gives the peaks of track `track`'s meter in the frames of the
    // last run, left and right, on the scale of 16-bit samples. Without meters, and for another track, it gives 0. The
    // meters change nothing in the output. They aren't part of a snapshot, and stay through a restore.
    void MeterTracks(bool meter);
    std::array<int32_t, 2> TrackPeaks(int track) const;

    // The player's state between runs, with what the model holds in memory. The model's hooks aren't part of it.
    class Snapshot
    {
    public:
        Snapshot(Snapshot&&) noexcept = default;
        Snapshot& operator=(Snapshot&&) noexcept = default;

    private:
        friend class FramePlayer;

        Snapshot() = default;

        const FramePlayer* owner_ = nullptr;
        std::unique_ptr<FrameModel> model_;
        std::vector<uint8_t> memory_;
        AudioRsp rsp_;
        std::vector<int16_t> pending_;
        std::size_t pending_read_ = 0;
        double frame_debt_ = 0.0;
        uint64_t position_ = 0;
        std::optional<EnhancedMix> enhanced_;
        std::vector<float> float_pending_;
    };

    Snapshot Save() const;

    // Returns the player to `snapshot`. Throws std::invalid_argument for another player's snapshot.
    void Restore(const Snapshot& snapshot);

private:
    using Peaks = std::array<std::array<int32_t, 2>, kHostTracks>;

    int BuildNextFrame();
    void StartFrame(bool mix);
    void RecordFrame();
    void MixNextFrame();
    void RunEnhancedWithoutMixing(std::size_t samples);
    void RunMarked();
    void EndVoice(int track);
    void EndChunk();
    void ClearPeaks();

    std::unique_ptr<FrameModel> model_;
    AudioMemory* memory_;
    double frame_samples_;
    double frame_debt_ = 0.0;
    AudioRsp rsp_;
    AudioList list_;
    uint32_t output_ = 0;
    std::vector<int16_t> pending_;
    std::size_t pending_read_ = 0;
    uint64_t position_ = 0;
    std::optional<EnhancedMix> enhanced_;
    std::vector<float> float_pending_;

    uint32_t muted_ = 0;
    bool metered_ = false;
    std::vector<uint8_t> before_; // the voice areas, before a voice's commands
    std::size_t area_samples_ = 0;
    std::vector<int32_t> sums_; // a chunk's sums: by track, by side and by sample
    uint32_t summed_tracks_ = 0;
    Peaks frame_peaks_ = {};
    Peaks peaks_ = {};
};

} // namespace ntdlib::n64
