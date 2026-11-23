// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/ax/system.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "cafe/ax/system_internal.h"
#include "ntdlib/ax/ax.h"
#include "ntdlib/ax/lpf.h"
#include "ntdlib/ax/voices.h"
#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/render.h"
#include "ntdlib/cafe/ax/voice.h"
#include "ntdlib/fused.h"

namespace ntdlib::cafe::ax
{

System::System(const Memory& memory) : memory_(&memory)
{
    Init(Library::kSndCore);
}

void System::Init(Library library, bool renderer_48k)
{
    Start(library, renderer_48k, kVoiceCount, false);
}

void System::InitEnhanced(int voice_count)
{
    Start(Library::kSndCore2, true, voice_count, true);
}

std::span<const float> System::OutputFloat() const
{
    return enhanced_ != nullptr ? std::span<const float>(enhanced_->output) : std::span<const float>();
}

void System::Start(Library library, bool renderer_48k, int voice_count, bool enhanced)
{
    library_ = library;
    samples_per_frame_ = library == Library::kSndCore2 && renderer_48k ? kSamplesPerFrame48k : kSamplesPerFrame32k;
    in_frame_ = false;
    silent_ = false;
    frame_ = 0;
    default_mixer_select_ = 1;
    user_count_ = 0;

    // Every voice free, in index order. The first voice acquired is then the last one, voice 95 of the console's 96.
    const auto count = static_cast<std::size_t>(std::max(voice_count, 1));
    allocation_ = ntdlib::ax::Voices(static_cast<int>(count));
    voices_.assign(count, Voice{});
    for (std::size_t i = 0; i < count; ++i)
    {
        voices_[i].index = static_cast<uint32_t>(i);
    }
    first_taken_ = static_cast<uint32_t>(count);
    host_.assign(count, HostVoice{});
    enhanced_.reset();
    if (enhanced)
    {
        enhanced_ = std::make_unique<EnhancedState>();
        enhanced_->voices.assign(count, EnhancedVoice{});
        enhanced_->meter_sums.assign(meter_sums_.size(), {});
    }

    frame_callback_ = {};
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (AuxBus& aux : aux_[device])
        {
            aux = AuxBus{};
            for (std::vector<int32_t>& buffer : aux.buffers)
            {
                buffer.assign(static_cast<std::size_t>(channels) * kMaxSamplesPerFrame, 0);
            }
        }
    }

    mixers_[0] = Mixer{};
    mixers_[0].channels = kTvChannels;
    mixers_[0].devices = 1;
    mixers_[1] = Mixer{};
    mixers_[1].channels = kDrcChannels;
    mixers_[1].devices = kDrcCount;

    buses_ = {};
    main_ = {};
    output_ = {};
    output48_ = {};
    stereo_ = {};
}

Voice* System::AcquireVoice(uint32_t priority, Voice::Callback callback, uint32_t context)
{
    if (priority == kPriorityFree || priority > kPriorityMax)
    {
        return nullptr;
    }

    // None free: the oldest voice of the lowest priority below the request. It leaves its list, then its owner is
    // notified, as sndcore2's AXAcquireVoiceEx (0x02000E74 in OS version 15702) does.
    const std::optional<ntdlib::ax::Voices::Taken> taken = allocation_.Take(priority);
    if (!taken)
    {
        return nullptr;
    }

    Voice* voice = &voices_[taken->voice];
    if (taken->stolen && voice->callback)
    {
        voice->callback(*voice, voice->context, 0);
    }

    allocation_.Push(taken->voice, priority);
    voice->callback = std::move(callback);
    voice->context = context;
    ResetVoice(*voice);
    host_[taken->voice] = HostVoice{};
    first_taken_ = std::min(first_taken_, taken->voice);
    if (enhanced_ != nullptr)
    {
        enhanced_->voices[taken->voice].starts = true;
    }

    return voice;
}

void System::FreeVoice(Voice& voice)
{
    if (allocation_.Priority(voice.index) == kPriorityFree)
    {
        return;
    }

    voice.is_protected = false;
    voice.protect_count = 0;
    allocation_.Remove(voice.index);
    ResetVoice(voice);
    voice.callback = {};
    allocation_.Push(voice.index, kPriorityFree);
}

void System::SetVoicePriority(Voice& voice, uint32_t priority)
{
    if (allocation_.Priority(voice.index) == kPriorityFree || priority == kPriorityFree || priority > kPriorityMax)
    {
        return;
    }

    allocation_.Remove(voice.index);
    allocation_.Push(voice.index, priority);
}

void System::SetVoiceState(Voice& voice, VoiceState state)
{
    if (state == voice.state)
    {
        return;
    }

    Touch(voice);
    voice.state = state;
    voice.game.state = state;
    voice.sync |= sync::kState;
}

void System::SetVoiceType(Voice& voice, VoiceType type)
{
    Touch(voice);
    voice.game.type = type;
    voice.sync |= sync::kType;
}

void System::SetVoiceOffsets(Voice& voice, const Offsets& offsets)
{
    const uint32_t base = offsets.samples;
    if (base == 0)
    {
        return;
    }

    Touch(voice);
    voice.offsets = offsets;

    uint32_t extension = 0;
    switch (offsets.format)
    {
    case SampleFormat::kPcm8:
        extension = (base + offsets.current) >> 29;
        break;
    case SampleFormat::kPcm16:
        extension = (base + 2 * offsets.current) >> 29;
        break;
    case SampleFormat::kAdpcm:
        extension = (base + offsets.current / 2) >> 29;
        break;
    default:
        return;
    }

    Pb& game = voice.game;
    const uint32_t mask = AddressMask(offsets.format);
    game.loop_flag = offsets.loop_flag;
    game.format = offsets.format;
    game.extension = static_cast<uint16_t>(extension);
    game.loop_address = AddressOf(offsets.format, base, offsets.loop) & mask;
    game.end_address = AddressOf(offsets.format, base, offsets.end) & mask;
    game.current_address = AddressOf(offsets.format, base, offsets.current) & mask;

    if (offsets.format != SampleFormat::kAdpcm)
    {
        game.adpcm = Adpcm{};
        game.adpcm.gain = offsets.format == SampleFormat::kPcm8 ? 0x0100 : 0x0800;
        voice.sync |= sync::kAdpcm;
    }

    voice.sync &= ~(sync::kLoopFlag | sync::kLoopAddress | sync::kEndAddress | sync::kCurrentAddress);
    voice.sync |= sync::kAddresses;
}

void System::SetVoiceLoop(Voice& voice, bool loop)
{
    Touch(voice);
    voice.offsets.loop_flag = loop;
    voice.game.loop_flag = loop;
    voice.sync |= sync::kLoopFlag;
}

void System::SetVoiceLoopOffset(Voice& voice, uint32_t offset)
{
    Touch(voice);
    voice.game.loop_address = AddressOf(voice.game.format, voice.offsets.samples & 0x1FFFFFFF, offset);
    voice.offsets.loop = offset;
    voice.sync |= sync::kLoopAddress;
}

void System::SetVoiceEndOffset(Voice& voice, uint32_t offset)
{
    Touch(voice);
    voice.game.end_address = AddressOf(voice.game.format, voice.offsets.samples & 0x1FFFFFFF, offset);
    voice.offsets.end = offset;
    voice.sync |= sync::kEndAddress;
}

void System::SetVoiceLoopOffsetEx(Voice& voice, uint32_t offset, uint32_t samples)
{
    voice.offsets.samples = samples;
    SetVoiceLoopOffset(voice, offset);
}

void System::SetVoiceEndOffsetEx(Voice& voice, uint32_t offset, uint32_t samples)
{
    voice.offsets.samples = samples;
    SetVoiceEndOffset(voice, offset);
}

void System::SetVoiceCurrentOffset(Voice& voice, uint32_t offset)
{
    Touch(voice);
    voice.game.current_address = AddressOf(voice.game.format, voice.offsets.samples & 0x1FFFFFFF, offset);
    voice.offsets.current = offset;
    voice.sync |= sync::kCurrentAddress;
}

Offsets System::GetVoiceOffsets(const Voice& voice) const
{
    // The positions in the game's copy, with the extension's three bits, less the sample base, in the units of the
    // format the offsets were set with.
    Offsets offsets = voice.offsets;
    const uint32_t base = offsets.samples;
    const uint32_t high = static_cast<uint32_t>(voice.game.extension) << 29;
    const auto offset_of = [&](uint32_t address)
    {
        switch (offsets.format)
        {
        case SampleFormat::kPcm8:
            return (address | high) - base;
        case SampleFormat::kPcm16:
            return (address | high / 2) - base / 2;
        default:
            return (address | high * 2) - 2 * base;
        }
    };

    offsets.loop = offset_of(voice.game.loop_address);
    offsets.end = offset_of(voice.game.end_address);
    offsets.current = offset_of(voice.game.current_address);

    return offsets;
}

Offsets System::GetVoiceOffsetsEx(Voice& voice, uint32_t samples)
{
    voice.offsets.samples = samples;
    voice.offsets = GetVoiceOffsets(voice);

    return voice.offsets;
}

uint32_t System::GetVoiceLoopCount(Voice& voice)
{
    // sndcore2 0x020121D8 and snd_core 0x0200DC30 in version 15702 of the OS, on the game's copy. For either renderer,
    // a frame is 96 samples in snd_core and 144 in sndcore2. The loop's length in output samples wraps for a loop of
    // 2^16 units or more. So a long loop can count as a short one. The first call after the voice is acquired measures
    // from position 0.
    const Pb& game = voice.game;
    const uint32_t loop = game.loop_address;
    const uint32_t end = game.end_address;
    const uint32_t current = game.current_address;
    const uint32_t last = std::exchange(voice.last_position, current);
    const uint32_t ratio = game.src_mode == SrcMode::kNone ? 0x10000 : game.src.ratio;
    const uint32_t frame = library_ == Library::kSndCore ? kSamplesPerFrame32k : kOutputSamplesPerFrame;
    if (ratio != 0 && end > loop)
    {
        const uint32_t length = ((end - loop) << 16) / ratio;
        if (length <= frame)
        {
            // A division by zero gives zero on the console's CPU.
            voice.loop_count += length != 0 ? (last - loop + frame) / length : 0;
            return voice.loop_count;
        }
    }

    if (current >= loop && end >= last && (last > current || loop > end))
    {
        ++voice.loop_count;
    }

    return voice.loop_count;
}

void System::SetVoiceAdpcm(Voice& voice, const Adpcm& adpcm)
{
    Touch(voice);
    voice.game.adpcm = adpcm;
    voice.sync |= sync::kAdpcm;
}

void System::SetVoiceAdpcmLoop(Voice& voice, const AdpcmLoop& loop)
{
    Touch(voice);
    voice.game.adpcm_loop.predictor_scale = loop.predictor_scale;
    if (voice.game.type == VoiceType::kNormal)
    {
        voice.game.adpcm_loop.yn1 = loop.yn1;
        voice.game.adpcm_loop.yn2 = loop.yn2;
    }
    voice.sync |= sync::kAdpcmLoop;
}

void System::SetVoiceSrc(Voice& voice, const Src& src)
{
    Touch(voice);
    voice.game.src = src;
    voice.sync = (voice.sync | sync::kSrc) & ~sync::kSrcRatio;
}

void System::SetVoiceSrcRatio(Voice& voice, float ratio)
{
    Touch(voice);
    const auto fixed = static_cast<uint32_t>(std::clamp(ntdlib::PowerPcToInt32(ratio * 65536.0f), 0, 0x80000));
    if (fixed != voice.game.src.ratio)
    {
        voice.game.src.ratio = fixed;
        voice.sync |= sync::kSrcRatio;
    }
}

void System::SetVoiceSrcType(Voice& voice, uint32_t type)
{
    Touch(voice);
    if (type == 0)
    {
        voice.game.src_mode = SrcMode::kNone;
    }
    else if (type == 1)
    {
        voice.game.src_mode = SrcMode::kLinear;
    }
    else if (type <= 4)
    {
        voice.game.src_mode = SrcMode::kFourTap;
        voice.game.four_tap_filter = static_cast<uint16_t>(type - 2);
    }
    voice.sync |= sync::kSrcType;
}

void System::SetVoiceVe(Voice& voice, VolumeRamp ve)
{
    Touch(voice);
    voice.game.ve = ve;
    voice.sync |= sync::kVe;
}

int System::SetVoiceDeviceMix(Voice& voice, Device device, uint32_t index, std::span<const ChannelMix> mix)
{
    std::size_t channels = 0;
    switch (device)
    {
    case Device::kTv:
        channels = kTvChannels;
        if (index != 0)
        {
            return -2;
        }
        break;

    case Device::kDrc:
        channels = kDrcChannels;
        if (index >= kDrcCount)
        {
            return -2;
        }
        break;

    case Device::kRemote:
        // The remotes' speakers aren't rendered.
        return index < 4 ? 0 : -2;

    default:
        return -1;
    }
    if (mix.size() < channels)
    {
        return -3;
    }

    // A channel takes part in a bus unless both its volume and its delta are 0. The channel's mask bits indicate
    // whether it ramps.
    Touch(voice);
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        uint16_t mask = 0;
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            const VolumeRamp& ramp = mix[channel][bus];
            const uint16_t bits = ramp.volume == 0 && ramp.delta == 0 ? 0 : ramp.delta != 0 ? 3 : 1;
            mask |= static_cast<uint16_t>(bits << (2 * channel));
            if (device == Device::kTv)
            {
                voice.game.tv_mix[channel][bus] = ramp;
            }
            else
            {
                voice.game.drc_mix[index][channel][bus] = ramp;
            }
        }
        if (device == Device::kTv)
        {
            voice.game.tv_mask[bus] = mask;
        }
        else
        {
            voice.game.drc_mask[index][bus] = mask;
        }
    }
    voice.sync |= sync::kMixMasks | sync::kMix;

    return 0;
}

void System::SetVoiceLpf(Voice& voice, const Lpf& lpf)
{
    Touch(voice);
    voice.game.lpf = lpf;
    voice.sync |= sync::kLpf;
}

void System::SetVoiceLpfCoefs(Voice& voice, int16_t a0, int16_t b0)
{
    Touch(voice);
    voice.game.lpf.a0 = a0;
    voice.game.lpf.b0 = b0;
    voice.sync |= sync::kLpfCoefs;
}

void System::SetVoiceBiquad(Voice& voice, const Biquad& biquad)
{
    Touch(voice);
    voice.game.biquad = biquad;
    voice.sync |= sync::kBiquad;
}

void System::SetVoiceBiquadCoefs(Voice& voice, int16_t b0, int16_t b1, int16_t b2, int16_t a1, int16_t a2)
{
    Touch(voice);
    Biquad& biquad = voice.game.biquad;
    biquad.b0 = b0;
    biquad.b1 = b1;
    biquad.b2 = b2;
    biquad.a1 = a1;
    biquad.a2 = a2;
    voice.sync |= sync::kBiquadCoefs;
}

ntdlib::ax::LpfCoefs System::ComputeLpfCoefs(uint32_t frequency) const
{
    // b0 is the pole of a one-pole low-pass filter at the cutoff. a0 + b0 is unity. snd_core uses 32 kHz. sndcore2
    // takes the renderer's rate and computes k k - 1 in one fused step.
    const bool sndcore2 = library_ == Library::kSndCore2;

    return ntdlib::ax::ComputeLpfCoefs(
        frequency,
        {.rate = sndcore2 ? static_cast<float>(samples_per_frame_ * 1000 / 3) : 32000.0f, .fused = sndcore2});
}

int System::UserBegin()
{
    if (in_frame_)
    {
        return -2;
    }

    return static_cast<int>(++user_count_);
}

int System::UserEnd()
{
    if (in_frame_)
    {
        return -2;
    }
    if (user_count_ == 0)
    {
        return -3;
    }

    if (--user_count_ == 0)
    {
        for (Voice& voice : voices_)
        {
            if (voice.is_protected && voice.protect_count == 0)
            {
                voice.is_protected = false;
            }
        }
    }

    return static_cast<int>(user_count_);
}

int System::VoiceBegin(Voice& voice)
{
    if (in_frame_)
    {
        return -2;
    }

    voice.is_protected = true;

    return static_cast<int>(++voice.protect_count);
}

int System::VoiceEnd(Voice& voice)
{
    if (in_frame_)
    {
        return -2;
    }
    if (!voice.is_protected)
    {
        return -3;
    }

    if (voice.protect_count > 0)
    {
        --voice.protect_count;
    }
    if (voice.protect_count == 0 && user_count_ == 0)
    {
        voice.is_protected = false;
    }

    return static_cast<int>(voice.protect_count);
}

int System::RegisterAuxCallback(Device device, uint32_t index, int bus, AuxCallback callback)
{
    AuxBus* aux = FindAuxBus(device, index, bus);
    if (!aux)
    {
        return device == Device::kTv || device == Device::kDrc ? -2 : -1;
    }

    aux->callback = std::move(callback);
    if (!aux->callback)
    {
        for (std::vector<int32_t>& buffer : aux->buffers)
        {
            std::fill(buffer.begin(), buffer.end(), 0);
        }
        aux->sends.clear();
        aux->returns.clear();
    }

    return 0;
}

void System::SetAuxReturnVolume(Device device, uint32_t index, int bus, uint16_t volume)
{
    if (AuxBus* aux = FindAuxBus(device, index, bus))
    {
        aux->return_volume = volume;
    }
}

System::Mixer* System::MixerOf(Device device)
{
    if (device == Device::kTv)
    {
        return &mixers_[0];
    }
    if (device == Device::kDrc)
    {
        return &mixers_[1];
    }

    return nullptr;
}

void System::RegisterDeviceFinalMixCallback(Device device, FinalMixCallback callback)
{
    if (Mixer* mixer = MixerOf(device))
    {
        mixer->callback = std::move(callback);
    }
}

void System::SetDeviceUpsampleStage(Device device, bool after_final_mix)
{
    if (Mixer* mixer = MixerOf(device))
    {
        mixer->upsample_after = after_final_mix;
    }
}

int System::SetDeviceLinearUpsampler(Device device, uint32_t index, bool linear)
{
    Mixer* mixer = MixerOf(device);
    if (mixer == nullptr)
    {
        return -1;
    }
    if (index >= static_cast<uint32_t>(mixer->devices))
    {
        return -2;
    }

    mixer->linear[index] = linear;

    return 0;
}

int System::SetDeviceRemixMatrix(Device device, uint32_t in, uint32_t out, std::span<const float> matrix)
{
    Mixer* mixer = MixerOf(device);
    if (mixer == nullptr)
    {
        return -1;
    }
    if (in == 0 || in > static_cast<uint32_t>(mixer->channels) || out == 0 || matrix.size() < in * out)
    {
        return -7;
    }

    RemixMatrix remix{in, out, std::vector<float>(matrix.begin(), matrix.begin() + in * out)};
    for (RemixMatrix& old : mixer->matrices)
    {
        if (old.in == in && old.out == out)
        {
            old = std::move(remix);
            return 0;
        }
    }
    mixer->matrices.push_back(std::move(remix));

    return 0;
}

} // namespace ntdlib::cafe::ax
