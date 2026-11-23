// SPDX-License-Identifier: MIT

// An engine for nw::snd's model (nwsnd/) whose voices play nothing. It plays a sound archive's sequence on the model,
// with the sound's banks, and a recording follows what the sequence plays through the model's hooks. A position model
// in place of a renderer determines when a one-shot wave has played out: AX's 16.16 position at 32 kHz on the Wii U and
// the Wii, the audio renderer's Q15 position at 48 kHz on the Switch, and the DSP's position at 32,728 Hz on the 3DS.
//
// The engine is a base class. The class that derives from it is the model's engine (Policy::Engine), and gives the
// hooks. The engine holds no pointers into itself. A copy of the derived class plays on apart from the original.

#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/nwsnd/archive_files.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/nwsnd/sequence_impl.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/recording_rules.h"
#include "ntdlib/sequence_export/recording_voices.h"

namespace ntdlib::sequence_export
{

// The engine. `Derived` derives from it and gives the hooks (see nwsnd/sequence.h). `Banks` gives each note's velocity
// region and wave: BankSet, or another type with BankSet's FindRegion and GetWave. `VoiceLayer` gives the console's
// voices and their steps in a frame (see MultiVoiceLayer). `count` channels and voices are enough that none is ever
// dropped. A model of a console's counts takes `channels` and `voices` apart.
template <typename Console, typename Derived, typename Banks = BankSet,
          template <typename> class VoiceLayer = MultiVoiceLayer>
class RecordingEngine
{
public:
    using Policy = RecordingPolicy<Console, Derived>;
    using Channel = typename Policy::Channel;
    using Player = nwsnd::SequenceSoundPlayer<Policy>;

    RecordingEngine(Banks& banks, int count) : RecordingEngine(banks, count, count)
    {
    }

    RecordingEngine(Banks& banks, int channels, int voices) : banks_(&banks), channels_(channels), layer_(voices)
    {
    }

    nwsnd::ChannelManager<Policy>& Channels()
    {
        return channels_;
    }

    const nwsnd::ChannelManager<Policy>& Channels() const
    {
        return channels_;
    }

    // The voices that the channels play on.
    auto& Voices()
    {
        return layer_.Voices();
    }

    // The position model, for a voice layer with one.
    auto& Renderer()
        requires requires(VoiceLayer<Policy>& layer) { layer.Renderer(); }
    {
        return layer_.Renderer();
    }

    // A recording pairs a note with the voice at `place` by the voice's key. The key is never reused. PositionOf gives
    // the end of that voice's wave, or nothing once the voice is gone.
    int PositionKey(nwsnd::VoiceId place)
    {
        return layer_.PositionKey(place);
    }

    std::optional<WaveEnd> PositionOf(int key) const
    {
        return layer_.PositionOf(key);
    }

    // The frame in which a one-shot wave runs out when its note starts in frame `frame` at the pitch ratio `ratio`,
    // before the note's voice has a rate (the voice layer's PredictEnd).
    static uint64_t PredictEnd(uint64_t frame, uint32_t samples, float ratio, uint32_t sample_rate)
    {
        return VoiceLayer<Policy>::PredictEnd(frame, samples, ratio, sample_rate);
    }

    nwsnd::Random& Rng()
    {
        return random_;
    }

    std::array<int16_t, 16>& GlobalVariables()
    {
        return globals_;
    }

    // The player of the sequence, once Play has made it.
    Player& GetPlayer()
    {
        return *player_;
    }

    const Player& GetPlayer() const
    {
        return *player_;
    }

    // The frame being processed, counted from the frame of the call to Play.
    uint64_t Frame() const
    {
        return frame_;
    }

    // Starts sound `info` with sequence `data`. The player starts `start_frames` frames later. The default of 1 is
    // nw::snd's timing: SoundArchivePlayer starts a sound in the sound thread's next frame.
    void Play(const sar::SoundInfo& info, std::span<const uint8_t> data, int start_frames = 1)
    {
        player_.emplace(0, info, data);
        start_frames_ = start_frames;
    }

    // A frame of the sound thread (SoundThread::FrameProcess): the voice layer's first step, the sound's start when
    // it's due, the player's ticks and the tracks' parameters, the channels, then the voice layer's last step.
    void RunFrame()
    {
        BeginFrame();
        if (player_)
        {
            player_->Update(static_cast<Derived&>(*this));
        }
        FinishFrame();
    }

    // A frame in parts, for an engine that runs the player's ticks itself: BeginFrame, the ticks in steps (see
    // SequenceSoundPlayer::BeginTick), the player's UpdateTrackParams, then FinishFrame.
    void BeginFrame()
    {
        layer_.BeginFrame(static_cast<Derived&>(*this), frame_);
        if (start_frames_ >= 0 && start_frames_-- == 0)
        {
            player_->Start();
        }
    }

    void FinishFrame()
    {
        Derived& engine = static_cast<Derived&>(*this);
        channels_.UpdateAllChannel(engine);
        layer_.FinishFrame(engine, random_);
        ++frame_;
    }

    // A channel's callback: the player's track that owns the channel. A channel whose voice went to another note
    // reports the drop to the derived engine first (OnChannelDropped), if it has the hook.
    void ChannelEvent(int, int track, Channel& channel, typename Channel::CallbackStatus status)
    {
        Derived& engine = static_cast<Derived&>(*this);
        if constexpr (requires { engine.OnChannelDropped(track, channel); })
        {
            if (status == Channel::CallbackStatus::kDrop)
            {
                engine.OnChannelDropped(track, channel);
            }
        }

        if (player_)
        {
            player_->ChannelEvent(engine, track, channel);
        }
    }

    // Bank::NoteOn: the region of the note's program, key and velocity in the bank of slot `slot`, a channel with a
    // voice for each of the wave's channels (two at most), and the channel's start. Returns nullptr if no region or
    // wave plays the note, or no channel is free. A note that finds no free channel calls the derived engine's
    // OnNoChannel with its track, if it has the hook.
    Channel* NoteOn(const std::vector<uint32_t>&, int slot, const typename Policy::NoteOnInfo& info)
    {
        const sar::VelocityRegion* region = banks_->FindRegion(slot, info.prg_no, info.key, info.velocity);
        if (!region)
        {
            return nullptr;
        }

        const sar::Pcm* pcm = banks_->GetWave(slot, region->wave_id_index);
        if (!pcm)
        {
            return nullptr;
        }

        Derived& engine = static_cast<Derived&>(*this);
        const int channel_count = std::min(static_cast<int>(pcm->channels.size()), 2);
        Channel* channel =
            channels_.AllocChannel(engine, channel_count, info.priority, info.owner_player, info.owner_track);
        if (!channel)
        {
            if constexpr (requires { engine.OnNoChannel(info.owner_track); })
            {
                engine.OnNoChannel(info.owner_track);
            }

            return nullptr;
        }

        ChannelRules<Console>::SetNote(engine, *channel, info, *region);

        typename Policy::WaveInfo wave;
        wave.sample_rate = pcm->sample_rate;
        wave.loop = pcm->loop;
        wave.samples = pcm->Length();
        channel->Start(engine, wave, info.length);

        return channel;
    }

private:
    Banks* banks_;
    nwsnd::ChannelManager<Policy> channels_;
    VoiceLayer<Policy> layer_;
    nwsnd::Random random_;
    std::array<int16_t, 16> globals_{};
    std::optional<Player> player_;
    int start_frames_ = -1;
    uint64_t frame_ = 0;
};

} // namespace ntdlib::sequence_export
