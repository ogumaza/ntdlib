// SPDX-License-Identifier: MIT

// Model of nn::snd's global state: 24 voices, the DSP link, the DSP configuration, and the per-frame receive/send
// sequence (code.bin 0x185f50 / 0x186070 / 0x18b40c / 0x18b2d4).
//
// On a DSP in the enhanced mixing mode (dsp::FirmwareModel), the voices can grow to the device's most sources. When
// every voice is in use, AllocVoice takes a new voice in place of a drop, and no voice is dropped for want of the
// frame's DSP cycles.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/ctr/dsp/device.h"
#include "ntdlib/ctr/nnsnd/dsp_abi.h"
#include "ntdlib/ctr/nnsnd/dsp_link.h"
#include "ntdlib/ctr/nnsnd/voice.h"

namespace ntdlib::ctr::nnsnd
{

// The DSP settings nn::snd writes when it starts (0x148470 and 0x1345fc) that a player can change.
struct DspDefaults
{
    uint16_t clipping_mode = 1;
    uint16_t output_mode = 1; // stereo (system setting)
};

// The owner of a voice: the place of the nw::snd voice that holds it, and whether that voice is stereo. AllocVoice
// records it. A voice without an owner has place -1.
struct DropOwner
{
    int place = -1;
    bool stereo = false;
};

class SoundSystem
{
public:
    // Called with a voice's owner after nn::snd has dropped the voice.
    using DropHandler = std::function<void(DropOwner owner, Voice* dropped)>;

    // The processing of an aux bus each frame: the bus's front left, front right, rear left and rear right channels, of
    // `samples` 32-bit samples each, to change in place. A frame holds 160 samples, or 235 in the enhanced mode. It
    // stands for the aux callback that a game registers with nn::snd (RegisterAuxCallback, 0x17f184) and for the SDK's
    // delay or reverb on the bus, which run in that order (0x18d6a8 and 0x18d5c8).
    using AuxCallback = std::function<void(const std::array<int32_t*, 4>& channels, std::size_t samples)>;

    // Where a DSP voice is in its wave, for a length analysis. A voice plays while it's allocated, its state
    // is play and the DSP reports it enabled. Its wave loops when one of its queued buffers loops. The position counts
    // the samples into the buffer that the DSP plays. It falls each time the wave goes back to its loop start.
    struct VoicePlace
    {
        bool playing = false;
        bool loops = false;
        uint32_t position = 0;
    };

    explicit SoundSystem(dsp::Device& dsp);

    // A copy for a snapshot, with the voices in use. Its priority list points at the voices of the system that it
    // copies.
    SoundSystem(const SoundSystem& other);

    // Restores a copy of this system. Each voice keeps its address: the nw::snd voices and the snapshots point at them.
    // The voices that the copy lacks go out of use and stay in their places. Those that only the copy holds come back
    // in their places.
    SoundSystem& operator=(const SoundSystem& other);

    bool Initialize(std::span<const uint8_t> component, const DspDefaults& defaults);

    // Allocates a free voice (nn::snd AllocVoice). When all 24 are in use, it drops the lowest-priority one to make
    // room. It returns nullptr instead if that one outranks the request or has priority 0x7fff. nw::snd uses at most 23
    // voices at once. A drop here happens only when some of them are stereo. In the enhanced mode, a new voice takes
    // the place of the drop while the device has room for its source.
    Voice* AllocVoice(int priority, DropOwner owner = {});

    // The voices in use. The enhanced mode can grow them.
    int VoiceCount() const
    {
        return static_cast<int>(allocated_.size());
    }

    // Sets the handler of drops. The nw::snd engine's voice manager sets it once.
    void SetDropHandler(DropHandler handler)
    {
        drop_handler_ = std::move(handler);
    }

    void FreeVoice(Voice* voice);

    // nn::snd Voice::SetPriority: re-sorts the voice in the drop-order list.
    void ChangePriority(Voice* voice, int priority);

    // A host's mute of the DSP voice `id` (Voice::SetHostMuted).
    void SetHostMuted(int id, bool muted)
    {
        voices_[static_cast<std::size_t>(id)].SetHostMuted(muted);
    }

    // Sets the processing of aux bus `bus` (0 or 1) once the system has started, or takes it away with an empty
    // callback. A callback turns the bus on in the DSP's mixer, as nn::snd does (0x17940c). The firmware then adds the
    // bus's samples, as the callback left them, to the output two frames after it mixed them. Without a callback, the
    // bus stays off.
    void SetAuxCallback(int bus, AuxCallback callback);

    // Sets the volume at which aux bus `bus` (0 or 1) returns to the output, as nn::snd does (0x179624). Both buses
    // start at 1.0. The firmware takes the volume in 32768ths, below 32768: 1.0 and more return the bus at 32767/32768.
    // A volume of 0 or less returns nothing. A change reaches the DSP with the frame's parameters.
    void SetAuxReturnVolume(int bus, float volume);

    // Runs the DSP to the end of the current frame and processes the returned statuses.
    bool WaitForDspSync();

    // Runs the aux buses' callbacks, writes all voice and DSP parameters and commits the frame.
    void SendParameterToDsp();

    VoicePlace PlaceOf(int id) const;

    // The DSP device, for a host's meters (dsp::Device::SetMeters). DSP voice `id` plays on the device's source `id`.
    dsp::Device& Dsp() const
    {
        return link_.Dsp();
    }

private:
    void WriteDspDefaults(const DspDefaults& defaults);
    void InsertByPriority(Voice* voice);

    // Frees a voice and calls the drop handler with the voice's owner (0x18d444). nn::snd drops a voice to make room
    // for another, or because the voice doesn't fit the frame's DSP cycles.
    void DropVoice(Voice* voice);

    // Adds a voice in the enhanced mode, with a source of its own on the device. Its parameters go to the DSP as
    // Initialize sends the first voices'.
    void AddVoice();

    // Whether the DSP mixes in the enhanced mode.
    bool Enhanced() const
    {
        return link_.MaxSources() > kNumSources;
    }

    // The voices in use: the first VoiceCount() of voices_.
    auto VoicesInUse()
    {
        return std::ranges::subrange(voices_.begin(), voices_.begin() + VoiceCount());
    }

    DspLink link_;

    // The voices by id. A deque keeps each voice where it is while the voices grow. A restore takes no voice away: the
    // voices past VoiceCount() are out of use.
    std::deque<Voice> voices_;

    std::vector<bool> allocated_; // for each voice in use
    std::vector<DropOwner> drop_owners_;
    DropHandler drop_handler_;
    std::array<AuxCallback, 2> aux_callbacks_;
    std::list<Voice*> priority_list_; // highest priority first
    int32_t voice_cycles_ = 0;        // the DSP cycles a frame that voices may take
};

} // namespace ntdlib::ctr::nnsnd
