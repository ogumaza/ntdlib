// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/ax/system.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

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
namespace
{

int16_t ClampToInt16(int32_t x)
{
    return static_cast<int16_t>(std::clamp(x, -32768, 32767));
}

// The renderer's addresses keep 29 bits of the byte address: 29 for PCM8's bytes, 28 for PCM16's words and 30 for
// ADPCM's nibbles.
uint32_t AddressMask(SampleFormat format)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return 0x1FFFFFFF;
    case SampleFormat::kPcm16:
        return 0x0FFFFFFF;
    default:
        return 0x3FFFFFFF;
    }
}

// The address in the format's units of the sample `offset` units past the sample base `base`.
uint32_t AddressOf(SampleFormat format, uint32_t base, uint32_t offset)
{
    switch (format)
    {
    case SampleFormat::kPcm8:
        return base + offset;
    case SampleFormat::kPcm16:
        return base / 2 + offset;
    default:
        return 2 * base + offset;
    }
}

// The renderer's progress into the game's copy of a voice, for each thing that the game hasn't changed since.
void TakeRendererProgress(Voice& voice)
{
    Pb& game = voice.game;
    const Pb& renderer = voice.renderer;
    const uint32_t bits = voice.sync;
    if (!(bits & sync::kState))
    {
        game.state = renderer.state;
        voice.state = renderer.state;
    }
    if (!(bits & (sync::kAddresses | sync::kCurrentAddress)))
    {
        game.current_address = renderer.current_address;
    }
    if (!(bits & sync::kVe))
    {
        game.ve.volume = renderer.ve.volume;
    }
    if (!(bits & sync::kAdpcm))
    {
        game.adpcm = renderer.adpcm;
    }
    if (!(bits & sync::kSrc))
    {
        game.src.frac = renderer.src.frac;
        game.src.last = renderer.src.last;
    }
}

// The game's changes of the addresses into the renderer's copy. The bit of the whole addresses wins over each
// address's bit.
void ApplyAddressChanges(const Pb& game, Pb& renderer, uint32_t bits)
{
    if (bits & sync::kAddresses)
    {
        renderer.loop_flag = game.loop_flag;
        renderer.format = game.format;
        renderer.extension = game.extension;
        renderer.loop_address = game.loop_address;
        renderer.end_address = game.end_address;
        renderer.current_address = game.current_address;
        return;
    }

    if (bits & sync::kLoopFlag)
    {
        renderer.loop_flag = game.loop_flag;
    }
    if (bits & sync::kLoopAddress)
    {
        renderer.loop_address = game.loop_address;
    }
    if (bits & sync::kEndAddress)
    {
        renderer.end_address = game.end_address;
    }
    if (bits & sync::kCurrentAddress)
    {
        renderer.current_address = game.current_address;
    }
}

// The game's changes of the low-pass and the biquad into the renderer's copy. For each filter, the coefficients' bit
// wins over the whole filter's.
void ApplyFilterChanges(const Pb& game, Pb& renderer, uint32_t bits)
{
    if (bits & sync::kLpfCoefs)
    {
        renderer.lpf.a0 = game.lpf.a0;
        renderer.lpf.b0 = game.lpf.b0;
    }
    else if (bits & sync::kLpf)
    {
        renderer.lpf = game.lpf;
    }
    if (bits & sync::kBiquadCoefs)
    {
        renderer.biquad.b0 = game.biquad.b0;
        renderer.biquad.b1 = game.biquad.b1;
        renderer.biquad.b2 = game.biquad.b2;
        renderer.biquad.a1 = game.biquad.a1;
        renderer.biquad.a2 = game.biquad.a2;
    }
    else if (bits & sync::kBiquad)
    {
        renderer.biquad = game.biquad;
    }
}

// The game's changes into the renderer's copy of a voice. The whole SRC's bit wins over the ratio's. With
// `stale_history`, a new ADPCM block keeps the renderer's history.
void ApplyGameChanges(const Pb& game, Pb& renderer, uint32_t bits, bool stale_history)
{
    if (bits & sync::kSrcType)
    {
        renderer.src_mode = game.src_mode;
        renderer.four_tap_filter = game.four_tap_filter;
    }
    if (bits & sync::kMixMasks)
    {
        renderer.tv_mask = game.tv_mask;
        renderer.drc_mask = game.drc_mask;
    }
    if (bits & sync::kState)
    {
        renderer.state = game.state;
    }
    if (bits & sync::kType)
    {
        renderer.type = game.type;
    }
    if (bits & sync::kMix)
    {
        renderer.tv_mix = game.tv_mix;
        renderer.drc_mix = game.drc_mix;
    }
    if (bits & sync::kVe)
    {
        renderer.ve = game.ve;
    }
    ApplyAddressChanges(game, renderer, bits);
    if (bits & sync::kAdpcm)
    {
        const int16_t yn1 = renderer.adpcm.yn1;
        const int16_t yn2 = renderer.adpcm.yn2;
        renderer.adpcm = game.adpcm;
        if (stale_history)
        {
            renderer.adpcm.yn1 = yn1;
            renderer.adpcm.yn2 = yn2;
        }
    }
    if (bits & sync::kSrc)
    {
        renderer.src = game.src;
    }
    else if (bits & sync::kSrcRatio)
    {
        renderer.src.ratio = game.src.ratio;
    }
    if (bits & sync::kAdpcmLoop)
    {
        renderer.adpcm_loop = game.adpcm_loop;
    }
    ApplyFilterChanges(game, renderer, bits);
}

// The past values that an enhanced voice keeps, from a new SRC, low-pass or biquad.
void ResetEnhancedPast(EnhancedVoice& enhanced, const Pb& renderer, uint32_t bits)
{
    if (bits & sync::kSrc)
    {
        ResetHistory(enhanced, renderer.src.last);
    }
    if ((bits & sync::kLpf) && !(bits & sync::kLpfCoefs))
    {
        enhanced.lpf = static_cast<float>(renderer.lpf.yn1);
    }
    if ((bits & sync::kBiquad) && !(bits & sync::kBiquadCoefs))
    {
        const Biquad& biquad = renderer.biquad;
        enhanced.biquad = {static_cast<float>(biquad.xn1), static_cast<float>(biquad.xn2),
                           static_cast<float>(biquad.yn1), static_cast<float>(biquad.yn2)};
    }
}

} // namespace

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

void System::RenderFrame()
{
    in_frame_ = true;
    const int n = samples_per_frame_;
    TakeVoices();
    if (render_observer_)
    {
        render_observer_(order_);
    }
    if (!mixing_)
    {
        AdvanceWithoutMixing();
        return;
    }
    silent_ = false;
    MixVoices();
    MeasureMeters(n);

    // The aux buses go out and come back. Then the game's callbacks run.
    if (enhanced_ != nullptr)
    {
        enhanced_->main = enhanced_->buses;
        ReturnAuxEnhanced(frame_);
    }
    else
    {
        main_ = buses_;
        ReturnAux(frame_);
    }
    RunAuxCallbacks(frame_);
    if (frame_callback_)
    {
        frame_callback_();
    }
    if (enhanced_ != nullptr)
    {
        FinalMixStageEnhanced();
        ++frame_;
        in_frame_ = false;
        return;
    }

    // Each device's main bus becomes the device's output at unity volume.
    const auto samples = static_cast<std::size_t>(n);
    for (int channel = 0; channel < kTvChannels; ++channel)
    {
        OutputBus({main_.tv[0][channel].data(), samples}, kUnityVolume, kUnityVolume,
                  {output_.tv[channel].data(), samples});
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int channel = 0; channel < kDrcChannels; ++channel)
        {
            OutputBus({main_.drc[drc][0][channel].data(), samples}, kUnityVolume, kUnityVolume,
                      {output_.drc[drc][channel].data(), samples});
        }
    }

    // The final mix stage, then the TV's stereo output, clamped to 16 bits (sndcore2 0x02008078 and 0x020092BC in
    // version 15702 of the OS). Between the two the console also runs the post-final-mix callback, the GamePad's
    // virtual surround, the OS's transition fade and the compressor. None of those is modelled.
    FinalMixStage();
    for (int i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        stereo_[2 * i] = ClampToInt16(output48_.tv[0][i]);
        stereo_[2 * i + 1] = ClampToInt16(output48_.tv[1][i]);
    }

    ++frame_;
    in_frame_ = false;
}

void System::TakeVoices()
{
    // The voices to render are those that are playing or have changes waiting. They're taken by priority from the
    // highest, oldest first within each priority. Each voice takes its changes now. The voices are rendered in the
    // opposite order.
    order_.clear();
    const auto take = [this](uint32_t index)
    {
        Voice* voice = &voices_[index];
        if (voice->game.state == VoiceState::kRun || voice->sync)
        {
            Sync(*voice);
            order_.push_back(voice);
        }
    };
    for (uint32_t priority = kPriorityMax; priority >= 1; --priority)
    {
        allocation_.ForEachOldestFirst(priority, take);
    }

    // A free voice is stopped in both copies.
    const auto stop = [this](uint32_t index)
    {
        Voice& voice = voices_[index];
        voice.state = VoiceState::kStop;
        voice.game.state = VoiceState::kStop;
        voice.renderer.state = VoiceState::kStop;
    };
    allocation_.ForEachNewestFirst(kPriorityFree, stop);
}

void System::MixVoices()
{
    // The host's mutes and meters take part in the voices' mixes.
    if (enhanced_ != nullptr)
    {
        EnhancedState& enhanced = *enhanced_;
        enhanced.buses = {};
        for (std::array<FloatBuses::Channel, 2>& sums : enhanced.meter_sums)
        {
            sums = {};
        }
        for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
        {
            Voice& each = **voice;
            RenderVoiceEnhanced(each.renderer, enhanced.voices[each.index], *memory_, enhanced.buses,
                                TakeHostMix(each));
        }
    }
    else
    {
        buses_ = {};
        for (std::array<Buses::Channel, 2>& sums : meter_sums_)
        {
            sums = {};
        }
        for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
        {
            RenderVoice((*voice)->renderer, *memory_, samples_per_frame_, buses_, cemu_, TakeHostMix(**voice));
        }
    }
    mutes_at_once_ = false;
}

void System::AdvanceWithoutMixing()
{
    // The voices move on in the same order. Each takes its host's mute as in a mixed frame.
    for (auto voice = order_.rbegin(); voice != order_.rend(); ++voice)
    {
        Voice& each = **voice;
        TakeHostMix(each);
        if (enhanced_ != nullptr)
        {
            AdvanceVoiceEnhanced(each.renderer, enhanced_->voices[each.index], *memory_);
        }
        else
        {
            AdvanceVoice(each.renderer, *memory_, samples_per_frame_, cemu_);
        }
    }
    mutes_at_once_ = false;

    // The frame's buses, output and meters are silent. The buses and the output stay so through the frames that follow
    // without mixing. Each aux bus with a callback takes its return volume as the DSP's.
    if (!silent_)
    {
        buses_ = {};
        main_ = {};
        output_ = {};
        output48_ = {};
        stereo_ = {};
        if (enhanced_ != nullptr)
        {
            enhanced_->buses = {};
            enhanced_->main = {};
            enhanced_->output = {};
        }
        silent_ = true;
    }
    for (std::array<int32_t, 2>& peaks : meter_peaks_)
    {
        peaks = {0, 0};
    }
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        for (std::size_t bus = 0; bus < 3; ++bus)
        {
            aux_peaks_[device][bus] = {0, 0};
            AuxBus& aux = aux_[device][bus];
            if (aux.callback)
            {
                aux.dsp_return_volume = aux.return_volume;
            }
        }
    }

    if (frame_callback_)
    {
        frame_callback_();
    }
    ++frame_;
    in_frame_ = false;
}

void System::SetVoiceMuted(const Voice& voice, bool muted)
{
    host_[voice.index].muted = muted;
}

void System::SetMeters(int count)
{
    const auto meters = static_cast<std::size_t>(std::max(count, 0));
    meter_sums_.assign(meters, {});
    meter_peaks_.assign(meters, {0, 0});
    if (enhanced_ != nullptr)
    {
        enhanced_->meter_sums.assign(meters, {});
    }
}

void System::SetVoiceMeter(const Voice& voice, int meter)
{
    host_[voice.index].meter = meter;
}

std::array<int32_t, 2> System::MeterPeaks(int meter) const
{
    if (meter < 0 || static_cast<std::size_t>(meter) >= meter_peaks_.size())
    {
        return {0, 0};
    }

    return meter_peaks_[static_cast<std::size_t>(meter)];
}

std::array<int32_t, 2> System::AuxReturnPeaks(Device device, uint32_t index, int bus) const
{
    if (bus < 0 || bus > 2)
    {
        return {0, 0};
    }
    if (device == Device::kTv && index == 0)
    {
        return aux_peaks_[0][static_cast<std::size_t>(bus)];
    }
    if (device == Device::kDrc && index < kDrcCount)
    {
        return aux_peaks_[1 + index][static_cast<std::size_t>(bus)];
    }

    return {0, 0};
}

HostMix System::TakeHostMix(const Voice& voice)
{
    HostVoice& host = host_[voice.index];
    HostMix mix;
    mix.was_muted = host.rendered && !mutes_at_once_ ? host.was_muted : host.muted;
    mix.muted = host.muted;
    if (host.meter >= 0 && static_cast<std::size_t>(host.meter) < meter_sums_.size())
    {
        const auto meter = static_cast<std::size_t>(host.meter);
        if (enhanced_ != nullptr)
        {
            mix.float_meter = &enhanced_->meter_sums[meter];
        }
        else
        {
            mix.meter = &meter_sums_[meter];
        }
    }
    host.was_muted = host.muted;
    host.rendered = true;

    return mix;
}

System::Snapshot System::Save() const
{
    const auto first = static_cast<std::ptrdiff_t>(first_taken_);
    Snapshot snapshot{this, frame_,      user_count_, first_taken_, {voices_.begin() + first, voices_.end()},
                      {},   allocation_, aux_,        mixers_};
    if (enhanced_ != nullptr)
    {
        snapshot.enhanced.assign(enhanced_->voices.begin() + first, enhanced_->voices.end());
    }

    return snapshot;
}

void System::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this)
    {
        throw std::invalid_argument("a snapshot of another AX");
    }

    frame_ = snapshot.frame;
    user_count_ = snapshot.user_count;
    allocation_ = snapshot.allocation;

    // The voices taken first since the snapshot go back to how Init left them. The others take the snapshot's copies.
    for (uint32_t index = first_taken_; index < snapshot.first_taken; ++index)
    {
        voices_[index] = Voice{};
        voices_[index].index = index;
        if (enhanced_ != nullptr)
        {
            enhanced_->voices[index] = EnhancedVoice{};
        }
    }
    const auto first = static_cast<std::ptrdiff_t>(snapshot.first_taken);
    std::copy(snapshot.voices.begin(), snapshot.voices.end(), voices_.begin() + first);
    if (enhanced_ != nullptr)
    {
        std::copy(snapshot.enhanced.begin(), snapshot.enhanced.end(), enhanced_->voices.begin() + first);
    }
    first_taken_ = snapshot.first_taken;

    // The buses and the final mix stages take back their state, and keep their callbacks.
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        for (std::size_t bus = 0; bus < aux_[device].size(); ++bus)
        {
            AuxBus& aux = aux_[device][bus];
            const AuxBus& saved = snapshot.aux[device][bus];
            aux.return_volume = saved.return_volume;
            aux.dsp_return_volume = saved.dsp_return_volume;
            aux.buffers = saved.buffers;
            aux.sends = saved.sends;
            aux.returns = saved.returns;
        }
    }
    for (std::size_t i = 0; i < mixers_.size(); ++i)
    {
        FinalMixCallback callback = std::move(mixers_[i].callback);
        mixers_[i] = snapshot.mixers[i];
        mixers_[i].callback = std::move(callback);
    }

    // The next frame takes the voices' mutes at once.
    mutes_at_once_ = true;
}

void System::ResetVoice(Voice& voice)
{
    // What every acquired voice starts with. The rest of the game's copy (the addresses, the ADPCM state, the envelope,
    // the mix volumes, the ratio, the filters' coefficients) keeps the last owner's values until the game sets it. The
    // renderer's copy keeps all of the last owner's values until the changes reach it.
    voice.state = VoiceState::kStop;
    voice.sync = sync::kState | sync::kItd | sync::kSrc | sync::kLpf | sync::kBiquad | sync::kRmtOn | sync::kRmtSrc |
                 sync::kRmtIir;
    voice.last_position = 0;
    voice.loop_count = 0;

    Pb& game = voice.game;
    game.state = VoiceState::kStop;
    game.lpf.on = 0;
    game.biquad.on = 0;
    game.mixer_select = static_cast<uint16_t>(default_mixer_select_);
    game.src_mode = SrcMode::kFourTap;
    game.src.frac = 0;
    game.src.last = {};
    game.tv_mask = {};
    game.drc_mask = {};
}

void System::Touch(Voice& voice)
{
    // While the game has begun protection, the voices it changes are protected too.
    if (user_count_ > 0 && !in_frame_)
    {
        voice.is_protected = true;
    }
}

void System::Sync(Voice& voice)
{
    // The renderer's progress reaches the game's copy. If the game has changed the same thing since, the game's change
    // stays.
    TakeRendererProgress(voice);

    // A protected voice's changes wait.
    if (voice.is_protected)
    {
        return;
    }

    // In the enhanced mode, a voice that the game starts takes its gains at once.
    const uint32_t bits = voice.sync;
    if (enhanced_ != nullptr && (bits & sync::kState) && voice.renderer.state != VoiceState::kRun &&
        voice.game.state == VoiceState::kRun)
    {
        enhanced_->voices[voice.index].starts = true;
    }

    // The game's changes reach the renderer's copy.
    ApplyGameChanges(voice.game, voice.renderer, bits, cemu_.stale_history);

    // In the enhanced mode, a new SRC, low-pass or biquad sets the past values that the voice keeps in that mode.
    if (enhanced_ != nullptr)
    {
        ResetEnhancedPast(enhanced_->voices[voice.index], voice.renderer, bits);
    }

    voice.sync = 0;
}

} // namespace ntdlib::cafe::ax
