// SPDX-License-Identifier: MIT

#include "ntdlib/ctr/nnsnd/sound_system.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/device.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr/nnsnd/voice.h"

namespace ntdlib::ctr::nnsnd
{
namespace
{

// A voice of this priority is never dropped to make room or for want of DSP cycles.
constexpr int kMaxPriority = 0x7fff;

// The DSP cycles that nn::snd lets a frame take (0x4c0eb4).
constexpr int32_t kFrameCycles = 622535;

// The DSP cycles the output takes a frame (0x18d548), by output mode and clipping mode. With headphones in (the shared
// page's flag), surround takes 199,600. The model never sets that flag.
int32_t OutputCycles(uint16_t output_mode, uint16_t clipping_mode)
{
    int32_t cycles = 52600;
    switch (output_mode)
    {
    case 0:
        cycles = 57100;
        break;
    case 1:
        cycles = 55600;
        break;
    case 2:
        cycles = 225600;
        break;
    default:
        break;
    }

    if (clipping_mode == 0)
    {
        cycles += 1500;
    }
    else if (clipping_mode == 1)
    {
        cycles += 10000;
    }

    return cycles;
}

} // namespace

SoundSystem::SoundSystem(dsp::Device& dsp) : link_(dsp), allocated_(kNumSources, false), drop_owners_(kNumSources)
{
    for (int i = 0; i < kNumSources; i++)
    {
        voices_.emplace_back(i);
    }
}

SoundSystem::SoundSystem(const SoundSystem& other)
    : link_(other.link_),
      voices_(other.voices_.begin(), other.voices_.begin() + other.VoiceCount()),
      allocated_(other.allocated_),
      drop_owners_(other.drop_owners_),
      drop_handler_(other.drop_handler_),
      aux_callbacks_(other.aux_callbacks_),
      priority_list_(other.priority_list_),
      voice_cycles_(other.voice_cycles_)
{
}

SoundSystem& SoundSystem::operator=(const SoundSystem& other)
{
    if (this == &other)
    {
        return *this;
    }

    // The voices past the copy's stay where they are, out of use. A voice that the snapshots or the nw::snd voices
    // point at then never moves.
    link_ = other.link_;
    while (voices_.size() < static_cast<std::size_t>(other.VoiceCount()))
    {
        voices_.emplace_back(static_cast<int>(voices_.size()));
    }

    std::copy(other.voices_.begin(), other.voices_.begin() + other.VoiceCount(), voices_.begin());
    allocated_ = other.allocated_;
    drop_owners_ = other.drop_owners_;
    drop_handler_ = other.drop_handler_;
    aux_callbacks_ = other.aux_callbacks_;
    priority_list_ = other.priority_list_;
    voice_cycles_ = other.voice_cycles_;

    return *this;
}

bool SoundSystem::Initialize(std::span<const uint8_t> component, const DspDefaults& defaults)
{
    if (!link_.Initialize(component))
    {
        return false;
    }

    WriteDspDefaults(defaults);

    // 0x148190: every voice's parameters are written once with all dirty bits set.
    for (Voice& v : VoicesInUse())
    {
        v.MarkAllDirty();
        v.UpdateParameters(link_);
    }

    return true;
}

void SoundSystem::WriteDspDefaults(const DspDefaults& defaults)
{
    // 0x148470 / 0x14862c: master volume, aux return volumes, aux bus enables, front bypass, rear ratio, surround
    // depth, clipping mode, output buffer count, output mode, sync mode.
    auto& c = link_.Mixer();
    c.master_volume = 1.0f;
    c.aux_return[0] = 1.0f;
    c.aux_return[1] = 1.0f;
    c.aux_enable[0] = 0;
    c.aux_enable[1] = 0;
    c.aux_bypass[0] = 0;
    c.aux_bypass[1] = 0;
    c.rear_ratio = 0x8000;
    c.surround_depth = 0x7fff;
    c.clipping_mode = defaults.clipping_mode;
    c.output_buffers = 2;
    c.output_mode = defaults.output_mode;
    c.dirty |= (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 15) | (1u << 16) | (1u << 24) | (1u << 25) |
               (1u << 26) | (1u << 27) | (1u << 29) | (1u << 31);
    c.sync_mode = 1; // MixerParams +0xbc: nn::snd writes the system sound mode here
    c.dirty2 |= 1u << 16;

    // The voices get what the output leaves of the frame's cycles (0x18b380). The aux buses' effects (0x18d2f4) take
    // none: the model has none. When its voice manager's flag at +0xe is set, nn::snd also takes off the cycles over
    // 655,300 that the DSP last reported. The game doesn't set the flag.
    voice_cycles_ = kFrameCycles - OutputCycles(defaults.output_mode, defaults.clipping_mode);
}

Voice* SoundSystem::AllocVoice(int priority, DropOwner owner)
{
    // 0x18eb6c
    if (priority >= 0x8000)
    {
        return nullptr;
    }

    const bool all_allocated = priority_list_.size() == static_cast<std::size_t>(VoiceCount());
    if (all_allocated && Enhanced() && VoiceCount() < link_.MaxSources())
    {
        AddVoice();
    }
    else if (all_allocated)
    {
        // All voices busy: drop the lowest-priority one unless it outranks the request, and tell its owner.
        Voice* lowest = priority_list_.back();
        if (lowest->Priority() == kMaxPriority || lowest->Priority() > priority)
        {
            return nullptr;
        }

        DropVoice(lowest);
    }

    for (int i = 0; i < VoiceCount(); i++)
    {
        if (allocated_[i])
        {
            continue;
        }

        allocated_[i] = true;
        drop_owners_[i] = owner;
        Voice& v = voices_[i];
        v.Reset();
        v.SetPriority(priority);
        InsertByPriority(&v);
        v.SetState(VoiceState::kPause); // VoiceImpl::SetState(Pause): enable = 0
        v.BumpSyncCounter();
        return &v;
    }

    return nullptr;
}

void SoundSystem::AddVoice()
{
    // A voice that a restore put out of use comes back in its place. It starts again as a new voice.
    const int id = VoiceCount();
    link_.GrowSources(id + 1);
    if (static_cast<std::size_t>(id) < voices_.size())
    {
        voices_[static_cast<std::size_t>(id)] = Voice(id);
    }
    else
    {
        voices_.emplace_back(id);
    }

    allocated_.push_back(false);
    drop_owners_.emplace_back();
    Voice& voice = voices_[static_cast<std::size_t>(id)];
    voice.MarkAllDirty();
    voice.UpdateParameters(link_);
}

void SoundSystem::InsertByPriority(Voice* voice)
{
    // The list runs from the highest priority to the lowest; a voice goes in front of voices of equal priority, so the
    // tail is the oldest of the lowest priority (0x18ec58).
    auto it = std::find_if(priority_list_.begin(), priority_list_.end(),
                           [&](Voice* v) { return v->Priority() <= voice->Priority(); });
    priority_list_.insert(it, voice);
}

void SoundSystem::FreeVoice(Voice* voice)
{
    // 0x18ecfc
    if (!voice || !allocated_[voice->Id()])
    {
        return;
    }

    const int i = voice->Id();

    if (voice->State() != VoiceState::kStop)
    {
        voice->SetState(VoiceState::kStop);
    }

    allocated_[i] = false;
    drop_owners_[i] = {};
    priority_list_.remove(voice);
}

void SoundSystem::DropVoice(Voice* voice)
{
    // The drop handler runs once the voice is free.
    const DropOwner owner = drop_owners_[voice->Id()];
    FreeVoice(voice);
    if (owner.place >= 0 && drop_handler_)
    {
        drop_handler_(owner, voice);
    }
}

void SoundSystem::ChangePriority(Voice* voice, int priority)
{
    priority_list_.remove(voice);
    voice->SetPriority(priority);
    InsertByPriority(voice);
}

void SoundSystem::SetAuxCallback(int bus, AuxCallback callback)
{
    // 0x17f184: the bus is on while it has a callback. nn::snd writes the switch into the frame that it prepares, and
    // marks it for the DSP (0x17940c).
    const bool on = static_cast<bool>(callback);
    aux_callbacks_[static_cast<std::size_t>(bus)] = std::move(callback);
    MixerParams& mixer = link_.Mixer();
    mixer.aux_enable[bus] = on ? 1 : 0;
    mixer.dirty |= 1u << (8 + bus);
}

void SoundSystem::SetAuxReturnVolume(int bus, float volume)
{
    // 0x179624: nn::snd writes the volume into the frame that it prepares, and marks it for the DSP.
    MixerParams& mixer = link_.Mixer();
    mixer.aux_return[bus] = volume;
    mixer.dirty |= 1u << (24 + bus);
}

bool SoundSystem::WaitForDspSync()
{
    if (!link_.WaitForFrame())
    {
        return false;
    }

    for (Voice& v : VoicesInUse())
    {
        v.ProcessStatus(link_);
    }

    return true;
}

void SoundSystem::SendParameterToDsp()
{
    // 0x18b2d4: each aux bus's callback takes the bus's samples that the DSP left in the read region, bus A's first.
    for (std::size_t bus = 0; bus < aux_callbacks_.size(); bus++)
    {
        if (aux_callbacks_[bus])
        {
            aux_callbacks_[bus](link_.AuxChannels(static_cast<int>(bus)), link_.AuxSamples());
        }
    }

    // 0x18d368 / 0x18d514
    for (Voice& v : VoicesInUse())
    {
        v.UpdateParameters(link_);
    }

    for (Voice& v : VoicesInUse())
    {
        v.UpdateWaveBuffers(link_);
    }

    // 0x18d39c: in priority order, playing voices with wave buffers get their sync count and enable written while their
    // DSP cycles fit what the frame has left. A voice that doesn't fit is dropped unless its priority is the highest.
    // The walk goes over a copy of the order: a dropped voice's owner can free others, and the game's walk passes over
    // them as stopped. The arithmetic matches the ARM code: 32-bit values and signed comparisons.
    int32_t cycles = voice_cycles_;
    const std::vector<Voice*> order(priority_list_.begin(), priority_list_.end());
    for (Voice* v : order)
    {
        if (v->State() != VoiceState::kPlay || !v->WaveBufferHead())
        {
            continue;
        }

        const auto cost = static_cast<int32_t>(v->DspCost());
        if (cost > cycles && v->Priority() != kMaxPriority && !Enhanced())
        {
            DropVoice(v);
            continue;
        }

        v->ApplySyncCount(link_);
        if (v->NeedsEnable())
        {
            v->EnableOnDsp(link_);
        }

        cycles = static_cast<int32_t>(static_cast<uint32_t>(cycles) - static_cast<uint32_t>(cost));
    }

    // The game writes the DSP side of a stop straight into the frame being prepared (see Voice::SetState). So a voice
    // that the walk dropped, and the other channel that its owner freed, go silent in this frame.
    for (Voice& v : VoicesInUse())
    {
        v.ApplyPendingState(link_);
    }

    link_.Commit();
}

SoundSystem::VoicePlace SoundSystem::PlaceOf(int id) const
{
    const Voice& voice = voices_[id];
    const SourceReport& report = link_.SourceReportOf(id);

    VoicePlace place;
    place.playing = allocated_[id] && voice.State() == VoiceState::kPlay && report.enabled != 0;
    place.position = report.position;
    for (const WaveBuffer* buffer = voice.WaveBufferHead(); buffer; buffer = buffer->next)
    {
        place.loops = place.loops || buffer->loop;
    }

    return place;
}

} // namespace ntdlib::ctr::nnsnd
