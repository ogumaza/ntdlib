// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <memory>

#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/snd/engine.h"

#include "rvl/snd/held_gain.h"

namespace ntdlib::rvl::snd
{
namespace
{

// The AX priorities that NW4R SND gives its voices: the highest for a voice of priority 255, one above the middle for
// the rest, and the middle for a voice in its release. A voice that must drop another to get its AX voice requests one
// more than usual.
constexpr uint32_t kAxPriorityTop = ax::kPriorityMax;
constexpr uint32_t kAxPriorityNormal = ax::kPriorityMax / 2 + 1;
constexpr uint32_t kAxPriorityRelease = ax::kPriorityMax / 2;
constexpr uint32_t kAxPriorityDrop = ax::kPriorityMax / 2 + 2;

} // namespace

bool Voice::Acquire(int channels, int priority, ChannelId owner)
{
    channels = std::clamp(channels, 1, 2);

    // Take an AX voice for each channel. When AX has none free, check whether dropping NW4R SND voices of no higher
    // priority would free enough. Then request an AX voice at a priority high enough to steal one. A game's build may
    // drop a voice first (EngineOptions::drop_voice_for_ax_voice).
    const uint32_t ax_priority = priority == kPriorityMax ? kAxPriorityTop : kAxPriorityNormal;
    AxVoiceManager& manager = engine_->AxVoices();
    std::array<AxVoice*, 2> table{};
    const auto on_drop = [this](AxVoice* dropped)
    {
        OnAxVoiceDropped(dropped);
    };

    // Whether dropping the voices of no higher priority would free `needed` AX voices.
    const auto can_drop = [this, priority](int needed)
    {
        for (const VoiceId id : engine_->Voices().PrioVoices())
        {
            const Voice& other = engine_->Voices().At(id);
            if (priority < other.Priority())
            {
                break;
            }

            needed -= other.AxVoiceCount();
            if (needed <= 0)
            {
                break;
            }
        }

        return needed <= 0;
    };

    for (int i = 0; i < channels; i++)
    {
        AxVoice* voice = manager.AcquireAxVoice(ax_priority, on_drop);

        // When no AxVoice is free and EngineOptions::drop_voice_for_ax_voice is set, the voice manager drops the voice
        // of the lowest priority if that priority is at or under this voice's. The number of AxVoices that frees
        // doesn't matter. Then the voice asks for an AxVoice again.
        if (voice == nullptr && engine_->Options().drop_voice_for_ax_voice)
        {
            if (!engine_->Voices().PrioVoices().empty())
            {
                engine_->Voices().DropLowestPriorityVoice(priority);
            }

            voice = manager.AcquireAxVoice(ax_priority, on_drop);
        }

        if (voice == nullptr && can_drop(channels - i))
        {
            voice = manager.AcquireAxVoice(ax_priority == kAxPriorityTop ? kAxPriorityTop : kAxPriorityDrop, on_drop);
        }

        if (voice == nullptr)
        {
            for (int j = 0; j < i; j++)
            {
                manager.FreeAxVoice(table[j]);
            }

            return false;
        }

        table[i] = voice;
    }

    for (int i = 0; i < channels; i++)
    {
        table[i]->SetPriority(ax_priority);
        ax_voices_[i] = table[i];
    }

    InitParam(channels, owner);

    return true;
}

void Voice::InitParam(int channels, ChannelId owner)
{
    // InitParam (0x8004ba80).
    channel_count_ = channels;
    owner_ = owner;
    sync_flag_ = 0;
    started_ = false;
    volume_ = 1.0f;
    ve_init_volume_ = 0.0f;
    ve_target_volume_ = 1.0f;
    lpf_freq_ = 1.0f;
    biquad_type_ = 0;
    biquad_value_ = 0.0f;
    pan_ = 0.0f;
    surround_pan_ = 0.0f;
    main_out_volume_ = 1.0f;
    main_send_ = 1.0f;
    fx_send_ = {};
    pitch_ = 1.0f;
    pan_mode_ = PanMode::kDual;
    pan_curve_ = 0;
    first_ve_update_ = true;
    prev_ve_volume_ = 1.0f;
    active_ = true;
}

void Voice::Free()
{
    if (!active_)
    {
        return;
    }

    for (int i = 0; i < channel_count_; i++)
    {
        if (ax_voices_[i] != nullptr)
        {
            engine_->AxVoices().FreeAxVoice(ax_voices_[i]);
            ax_voices_[i] = nullptr;
        }
    }

    channel_count_ = 0;
    engine_->Voices().FreeVoice(this);
    active_ = false;
}

void Voice::StopFinished()
{
    if (active_ && started_ && IsPlayFinished())
    {
        if (owner_ != kNoChannel)
        {
            engine_->OnVoiceEvent(owner_, Status::kFinishWave);
        }

        started_ = false;
        starting_ = false;
    }
}

void Voice::Calc()
{
    if (!starting_)
    {
        return;
    }

    if (sync_flag_ & kSyncSrc)
    {
        CalcAxSrc(false);
        sync_flag_ &= ~kSyncSrc;
    }

    if (sync_flag_ & kSyncVe)
    {
        if (!CalcAxVe())
        {
            sync_flag_ &= ~kSyncVe;
        }
    }

    if (sync_flag_ & kSyncMix)
    {
        if (!CalcAxMix())
        {
            sync_flag_ &= ~kSyncMix;
        }
    }

    if (sync_flag_ & kSyncLpf)
    {
        CalcAxLpf();
        sync_flag_ &= ~kSyncLpf;
    }

    if (sync_flag_ & kSyncBiquad)
    {
        CalcAxBiquadFilter();
        sync_flag_ &= ~kSyncBiquad;
    }
}

void Voice::Update()
{
    if (!active_)
    {
        return;
    }

    // A voice starts with its converter at rest. It runs from this frame.
    if ((sync_flag_ & kSyncSrcInitial) && starting_ && !started_)
    {
        CalcAxSrc(true);
        RunAllAxVoice();
        started_ = true;
        sync_flag_ &= ~(kSyncSrcInitial | kSyncSrc);
    }

    if (started_)
    {
        SyncAxVoice();
    }
}

void Voice::Setup(const WaveData& data)
{
    const ax::SampleFormat format = ToAxFormat(data.format);
    for (int i = 0; i < channel_count_; i++)
    {
        AxVoice* voice = ax_voices_[i];
        if (voice == nullptr)
        {
            continue;
        }

        const WaveData::Channel& channel = data.channels[i];
        voice->Setup(channel.data, format, static_cast<int>(data.sample_rate));
        voice->SetAddr(data.loop, channel.data, data.loop_start, data.loop_end);
        if (format == ax::SampleFormat::kAdpcm)
        {
            voice->SetAdpcm(channel.adpcm);
            voice->SetAdpcmLoop(channel.adpcm);
        }

        voice->SetSrcType(AxVoice::SrcType::k4TapAuto, pitch_);
        voice->SetVoiceType(ax::VoiceType::kNormal);
    }

    starting_ = false;
    started_ = false;
    sync_flag_ |= kSyncLpf | kSyncMix | kSyncVe;
}

void Voice::Start()
{
    starting_ = true;
    sync_flag_ |= kSyncSrcInitial;
}

void Voice::Stop()
{
    if (started_)
    {
        StopAllAxVoice();
        started_ = false;
    }

    starting_ = false;
}

void Voice::SetVolume(float volume)
{
    volume = HeldGain(volume, engine_->Build());
    if (volume != volume_)
    {
        volume_ = volume;
        sync_flag_ |= kSyncVe;
    }
}

void Voice::SetVeVolume(float target, float init)
{
    target = HeldGain(target, engine_->Build());
    init = HeldGain(init, engine_->Build());
    if (init == ve_init_volume_ && target == ve_target_volume_)
    {
        return;
    }

    ve_init_volume_ = init;
    ve_target_volume_ = target;
    sync_flag_ |= kSyncVe;
}

void Voice::SetPitch(float pitch)
{
    if (pitch != pitch_)
    {
        pitch_ = pitch;
        sync_flag_ |= kSyncSrc;
    }
}

void Voice::SetPanMode(PanMode mode)
{
    if (mode != pan_mode_)
    {
        pan_mode_ = mode;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetPanCurve(uint8_t curve)
{
    if (curve != pan_curve_)
    {
        pan_curve_ = curve;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetPan(float pan)
{
    if (pan != pan_)
    {
        pan_ = pan;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetSurroundPan(float pan)
{
    if (pan != surround_pan_)
    {
        surround_pan_ = pan;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetLpfFreq(float freq)
{
    freq = std::clamp(freq, 0.0f, 1.0f);
    if (freq != lpf_freq_)
    {
        lpf_freq_ = freq;
        sync_flag_ |= kSyncLpf;
    }
}

void Voice::SetBiquadFilter(int type, float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (type != biquad_type_ || value != biquad_value_)
    {
        biquad_type_ = static_cast<uint8_t>(type);
        biquad_value_ = value;
        sync_flag_ |= kSyncBiquad;
    }
}

void Voice::SetMainOutVolume(float volume)
{
    volume = HeldGain(volume, engine_->Build());
    if (volume != main_out_volume_)
    {
        main_out_volume_ = volume;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetMainSend(float send)
{
    // The main send is relative: 0 is the full send. Builds from September 2009 on hold the sends only to 0 here. Their
    // CalcMixParam holds the sends to 0 and 1. The model holds them to 0 and 1 here in every build. The mix comes out
    // the same.
    send = std::clamp(send + 1.0f, 0.0f, 1.0f);
    if (send != main_send_)
    {
        main_send_ = send;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetFxSend(int bus, float send)
{
    // The model holds the send to 0 and 1 here in every build (see SetMainSend).
    send = std::clamp(send, 0.0f, 1.0f);
    if (send != fx_send_[bus])
    {
        fx_send_[bus] = send;
        sync_flag_ |= kSyncMix;
    }
}

void Voice::SetParam(const ntdlib::nwsnd::RevolutionPolicy::ChannelVoiceParam& param)
{
    engine_->OnVoiceParam(owner_, param);

    SetPanMode(static_cast<PanMode>(param.pan_mode));
    SetPanCurve(static_cast<uint8_t>(param.pan_curve));
    SetVolume(param.volume);
    SetVeVolume(param.ve_target_volume, param.ve_init_volume);
    SetPitch(param.pitch);
    SetPan(param.pan);
    SetSurroundPan(param.surround_pan);
    SetLpfFreq(param.lpf_freq);
    SetBiquadFilter(param.biquad_type, param.biquad_value);
    SetMainOutVolume(param.main_out_volume);

    // The game adds the sound's sends to the track's sends (SeqTrack::UpdateChannelParam; Animal Crossing: City Folk,
    // 0x802682b4). The channel then passes the sums on. The model adds them here instead. The sums don't change: each
    // of the game's other terms is 0. The sends are those of the sound whose sequence played the channel's note. A
    // channel that its track has let go keeps them. The sound's main send adds to the track's main send in the same
    // way.
    const int player = owner_ == kNoChannel ? -1 : engine_->Channels().At(owner_).source_player;
    SetMainSend(param.main_send + engine_->SoundMainSend(player));
    const std::array<float, ax::kAuxBusCount> sends = engine_->SoundSends(player);
    for (std::size_t bus = 0; bus < param.fx_send.size(); bus++)
    {
        SetFxSend(static_cast<int>(bus), param.fx_send[bus] + sends[bus]);
    }
}

void Voice::SetPriority(int priority)
{
    priority_ = priority;
    engine_->Voices().ChangeVoicePriority(this);
    if (priority_ != kPriorityRelease)
    {
        return;
    }

    ForEachAxVoice([](AxVoice& voice) { voice.SetPriority(kAxPriorityRelease); });
}

void Voice::UpdateVoicesPriority()
{
    if (priority_ == kPriorityRelease)
    {
        return;
    }

    ForEachAxVoice([](AxVoice& voice) { voice.SetPriority(kAxPriorityNormal); });
}

bool Voice::IsPlayFinished() const
{
    return active_ && ax_voices_[0] != nullptr && ax_voices_[0]->IsPlayFinished();
}

void Voice::CalcAxSrc(bool initial)
{
    ForEachAxVoice([this, initial](AxVoice& voice) { voice.SetSrc(pitch_, initial); });
}

bool Voice::CalcAxVe()
{
    // CalcAxVe (0x8004d044): the voice's volume, scaled by the master volume, at both ends of the frame's ramp.
    float volume = 1.0f;
    volume *= volume_;
    volume *= engine_->OutputVolume();
    if (engine_->Build().October2006())
    {
        // The build of October 2006 (0x800dd140 in Smooth Moves' main.dol) sets the AX voices' envelopes itself. The
        // ramp starts at the volume where the last ramp ended. Once the voice runs, that volume comes from the last
        // frame. The ramp steps by the difference over the frame's 96 samples. The step is rounded towards 0. The next
        // ramp starts at the target.
        const bool running = ax_voices_[0] != nullptr && ax_voices_[0]->IsRun();
        const float start = first_ve_update_ || !running ? volume : prev_ve_volume_;
        const auto init = static_cast<uint16_t>(static_cast<int32_t>(32767.0f * (start * ve_init_volume_)));
        const auto end = static_cast<uint16_t>(static_cast<int32_t>(32767.0f * (volume * ve_target_volume_)));
        const auto delta = static_cast<int16_t>((end - init) / ax::kSamplesPerFrame);
        ForEachAxVoice([init, delta](AxVoice& voice) { voice.SetVeOctober2006(init, delta); });

        prev_ve_volume_ = volume;
        first_ve_update_ = false;
        ve_init_volume_ = ve_target_volume_;

        return end != init;
    }

    const float target = volume * ve_target_volume_;
    const float init = volume * ve_init_volume_;
    ForEachAxVoice([target, init](AxVoice& voice) { voice.SetVe(target, init); });

    return false;
}

bool Voice::CalcAxMix()
{
    // CalcAxMix (0x8004d134). The voice plays on the main line only. The Wii Remotes stay off.
    bool next_update = false;
    for (int i = 0; i < channel_count_; i++)
    {
        AxVoice* voice = ax_voices_[i];
        if (voice == nullptr)
        {
            continue;
        }

        AxVoice::MixParam mix;
        CalcMixParam(i, mix);
        next_update |= voice->SetMix(mix);
        voice->EnableRemote(false);
    }

    return next_update;
}

void Voice::CalcAxLpf()
{
    const int freq = CalcLpfFreq(lpf_freq_, engine_->Build());
    ForEachAxVoice([freq](AxVoice& voice) { voice.SetLpf(static_cast<uint16_t>(freq)); });
}

void Voice::CalcAxBiquadFilter()
{
    ForEachAxVoice([this](AxVoice& voice) { voice.SetBiquad(biquad_type_, biquad_value_); });
}

void Voice::CalcMixParam(int channel, AxVoice::MixParam& mix) const
{
    const MixInput input{main_out_volume_, main_send_,     fx_send_,  pan_,
                         surround_pan_,    channel_count_, pan_mode_, pan_curve_};
    mix = CalcMix(input, channel, engine_->Build(), engine_->GetOutputMode());
}

void Voice::SyncAxVoice()
{
    ForEachAxVoice([](AxVoice& voice) { voice.Sync(); });
}

void Voice::RunAllAxVoice()
{
    ForEachAxVoice([](AxVoice& voice) { voice.Run(); });
}

void Voice::StopAllAxVoice()
{
    ForEachAxVoice([](AxVoice& voice) { voice.Stop(); });
}

void Voice::ResetDelta()
{
    ForEachAxVoice([](AxVoice& voice) { voice.ResetDelta(); });
}

void Voice::OnAxVoiceDropped(AxVoice* dropped)
{
    // AxVoiceCallbackFunc (0x8004d2a0). Stop and free the voice's other AX voices. AX has freed the one it dropped.
    for (int i = 0; i < channel_count_; i++)
    {
        AxVoice* voice = ax_voices_[i];
        if (voice != nullptr && voice != dropped)
        {
            voice->Stop();
            engine_->AxVoices().FreeAxVoice(voice);
        }

        ax_voices_[i] = nullptr;
    }

    starting_ = false;
    channel_count_ = 0;
    Free();
    if (owner_ != kNoChannel)
    {
        engine_->OnVoiceEvent(owner_, Status::kDropDsp);
    }
}

VoiceManager::VoiceManager(Engine& engine, int count)
{
    pool_.reserve(static_cast<std::size_t>(count));
    for (int place = 0; place < count; place++)
    {
        pool_.push_back(std::make_unique<Voice>(engine, place));
        free_.push_back(place);
    }
}

VoiceManager::Snapshot VoiceManager::Save() const
{
    Snapshot snapshot{{}, free_, prio_list_};
    snapshot.voices.reserve(pool_.size());
    for (const std::unique_ptr<Voice>& voice : pool_)
    {
        snapshot.voices.push_back(*voice);
    }

    return snapshot;
}

void VoiceManager::Restore(const Snapshot& snapshot)
{
    for (std::size_t i = 0; i < pool_.size(); i++)
    {
        *pool_[i] = snapshot.voices[i];
    }

    free_ = snapshot.free;
    prio_list_ = snapshot.prio_list;
}

Voice* VoiceManager::AllocVoice(int channels, int priority, ChannelId owner)
{
    if (free_.empty() && DropLowestPriorityVoice(priority) == 0)
    {
        return nullptr;
    }

    Voice* voice = &At(free_.front());
    if (!voice->Acquire(channels, priority, owner))
    {
        return nullptr;
    }

    voice->priority_ = priority & Voice::kPriorityMax;
    AppendVoiceList(voice);
    UpdateEachVoicePriority(std::find(prio_list_.begin(), prio_list_.end(), voice->place_));

    return voice;
}

void VoiceManager::FreeVoice(Voice* voice)
{
    RemoveVoiceList(voice);
}

void VoiceManager::UpdateAllVoices()
{
    // As in the game's safe iteration, each pass moves past a voice before its step. The step may free the voice.
    const auto for_each = [this](void (Voice::*step)())
    {
        for (auto it = prio_list_.begin(); it != prio_list_.end();)
        {
            Voice& voice = At(*it++);
            (voice.*step)();
        }
    };

    for_each(&Voice::StopFinished);
    for_each(&Voice::Calc);
    for_each(&Voice::Update);
}

void VoiceManager::ChangeVoicePriority(Voice* voice)
{
    RemoveVoiceList(voice);
    AppendVoiceList(voice);
    UpdateEachVoicePriority(std::find(prio_list_.begin(), prio_list_.end(), voice->place_));
}

void VoiceManager::NotifyVoiceUpdate()
{
    for (auto it = prio_list_.begin(); it != prio_list_.end();)
    {
        At(*it++).ResetDelta();
    }
}

int VoiceManager::DropLowestPriorityVoice(int priority)
{
    Voice* voice = &At(prio_list_.front());
    if (voice->Priority() > priority)
    {
        return 0;
    }

    const int dropped = voice->AxVoiceCount();
    voice->Stop();
    voice->Free();
    if (voice->owner_ != kNoChannel)
    {
        voice->engine_->OnVoiceEvent(voice->owner_, Voice::Status::kDropVoice);
    }

    return dropped;
}

void VoiceManager::AppendVoiceList(Voice* voice)
{
    free_.remove(voice->place_);

    // Insert after the last voice of no higher priority. The list runs from the lowest priority, oldest first.
    auto it = prio_list_.end();
    while (it != prio_list_.begin() && At(*std::prev(it)).Priority() > voice->Priority())
    {
        --it;
    }

    prio_list_.insert(it, voice->place_);
}

void VoiceManager::RemoveVoiceList(Voice* voice)
{
    prio_list_.remove(voice->place_);
    free_.push_back(voice->place_);
}

void VoiceManager::UpdateEachVoicePriority(std::list<VoiceId>::iterator begin)
{
    for (auto it = begin; it != prio_list_.end(); ++it)
    {
        if (At(*it).Priority() <= Voice::kPriorityRelease)
        {
            return;
        }

        if (At(*it).Priority() != Voice::kPriorityMax)
        {
            At(*it).UpdateVoicesPriority();
        }
    }
}

VoiceId VoiceManager::AllocVoice(Engine&, int channels, uint32_t priority, ChannelId owner)
{
    const Voice* voice = AllocVoice(channels, static_cast<int>(priority), owner);
    return voice != nullptr ? voice->Place() : kNoVoice;
}

} // namespace ntdlib::rvl::snd
