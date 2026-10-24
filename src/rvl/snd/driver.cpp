// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/snd/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/util.h"

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

// The zero buffer's length in the units of each format: 256 bytes.
uint32_t ZeroBufferUnits(ax::SampleFormat format)
{
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        return 0x200;
    case ax::SampleFormat::kPcm8:
        return 0x100;
    default:
        return 0x80;
    }
}

// The ramp that moves a mix volume from `from` to `to` over a frame (CalcAxvpbDelta).
int MixDelta(uint16_t from, uint16_t to)
{
    return (to - from) / ax::kSamplesPerFrame;
}

// A mix gain as AX's volume (CalcMixVolume). In AX's volume, 0x8000 is 1.
uint16_t MixVolume(float volume)
{
    return static_cast<uint16_t>(std::min<uint32_t>(0xffff, ToUnsigned(static_cast<float>(ax::kMaxVolume) * volume)));
}

// Voice::SetVolume (0x8004c830), SetVeVolume and SetMainOutVolume hold a gain from the channel to 0 and 1. Builds from
// September 2009 on hold it only to 0. The gain can then go above full level.
float HeldGain(float gain, const SndBuild& build)
{
    return build.SinceSeptember2009() ? std::max(gain, 0.0f) : std::clamp(gain, 0.0f, 1.0f);
}

// The mix at the volumes of `mix`, with no ramps.
ax::PbMix StillMix(const AxVoice::MixParam& mix)
{
    ax::PbMix still;
    still.l = mix.l;
    still.r = mix.r;
    still.s = mix.s;
    still.aux_a_l = mix.aux_a_l;
    still.aux_a_r = mix.aux_a_r;
    still.aux_a_s = mix.aux_a_s;
    still.aux_b_l = mix.aux_b_l;
    still.aux_b_r = mix.aux_b_r;
    still.aux_b_s = mix.aux_b_s;
    still.aux_c_l = mix.aux_c_l;
    still.aux_c_r = mix.aux_c_r;
    still.aux_c_s = mix.aux_c_s;

    return still;
}

} // namespace

ax::SampleFormat ToAxFormat(ntdlib::rsar::WaveEncoding format)
{
    switch (format)
    {
    case ntdlib::rsar::WaveEncoding::kPcm16:
        return ax::SampleFormat::kPcm16;
    case ntdlib::rsar::WaveEncoding::kPcm8:
        return ax::SampleFormat::kPcm8;
    default:
        return ax::SampleFormat::kAdpcm;
    }
}

void AxVoice::Setup(ntdlib::PAddr wave, ax::SampleFormat format, int sample_rate)
{
    wave_data_ = wave;
    format_ = format;
    sample_rate_ = sample_rate;
    mix_prev_ = MixParam{};
    first_mix_update_ = true;
}

bool AxVoice::IsPlayFinished() const
{
    if (wave_data_ == 0)
    {
        return false;
    }

    const uint32_t address =
        IsAvailable() ? ax::Join(vpb_->pb.addr.current_address_hi, vpb_->pb.addr.current_address_lo) : 0;
    const uint32_t begin = DspAddressBySample(engine_->ZeroBuffer(), 0, format_);
    return begin <= address && address < begin + ZeroBufferUnits(format_);
}

void AxVoice::SetAddr(bool loop, ntdlib::PAddr wave, uint32_t loop_start, uint32_t loop_end)
{
    if (!IsAvailable())
    {
        return;
    }

    // A wave that doesn't loop continues into the zero buffer. The microcode stops it there.
    const ntdlib::PAddr zero = engine_->ZeroBuffer();
    const uint32_t loop_address =
        loop ? DspAddressBySample(wave, loop_start, format_) : DspAddressBySample(zero, 0, format_);
    const uint32_t start = DspAddressBySample(wave, 0, format_);
    const uint32_t end = DspAddressBySample(wave, loop_end - 1, format_);

    ax::PbAddr address;
    address.loop_flag = loop ? 1 : 0;
    address.format = static_cast<uint16_t>(format_);
    address.loop_address_hi = static_cast<uint16_t>(loop_address >> 16);
    address.loop_address_lo = static_cast<uint16_t>(loop_address);
    address.end_address_hi = static_cast<uint16_t>(end >> 16);
    address.end_address_lo = static_cast<uint16_t>(end);
    address.current_address_hi = static_cast<uint16_t>(start >> 16);
    address.current_address_lo = static_cast<uint16_t>(start);
    vpb_->SetAddr(address);
}

void AxVoice::SetSrcType(SrcType type, float pitch)
{
    if (!IsAvailable())
    {
        return;
    }

    // The automatic choice takes the coefficients for the lowest cutoff that the rate requires. The more the wave is
    // sped up, the lower the cutoff.
    if (type == SrcType::k4TapAuto)
    {
        const float ratio = pitch * static_cast<float>(sample_rate_) / static_cast<float>(ax::kSampleRate);
        if (ratio > 4.0f / 3.0f)
        {
            type = SrcType::k4Tap8k;
        }
        else if (ratio > 1.0f)
        {
            type = SrcType::k4Tap12k;
        }
        else
        {
            type = SrcType::k4Tap16k;
        }
    }

    // AxVoiceParamBlock::SetVoiceSrcType (0x80034af8).
    ax::Pb& pb = vpb_->pb;
    switch (type)
    {
    case SrcType::kNone:
        pb.src_select = static_cast<uint16_t>(ax::SrcMode::kNone);
        break;

    case SrcType::kLinear:
        pb.src_select = static_cast<uint16_t>(ax::SrcMode::kLinear);
        break;

    case SrcType::k4Tap8k:
        pb.src_select = static_cast<uint16_t>(ax::SrcMode::kFourTap);
        pb.coef_select = 0;
        break;

    case SrcType::k4Tap12k:
        pb.src_select = static_cast<uint16_t>(ax::SrcMode::kFourTap);
        pb.coef_select = 1;
        break;

    case SrcType::k4Tap16k:
        pb.src_select = static_cast<uint16_t>(ax::SrcMode::kFourTap);
        pb.coef_select = 2;
        break;

    default:
        break;
    }

    sync_ |= ax::sync::kSelect;
}

void AxVoice::SetAdpcm(const ntdlib::rsar::DspAdpcmInfo& info)
{
    if (!IsAvailable())
    {
        return;
    }

    // PCM16 and PCM8 decode with no prediction and a gain that scales them to 16 bits.
    ax::PbAdpcm adpcm;
    switch (format_)
    {
    case ax::SampleFormat::kAdpcm:
        for (int i = 0; i < 16; i++)
        {
            adpcm.a[i] = static_cast<uint16_t>(info.coefs[i]);
        }

        adpcm.gain = info.gain;
        adpcm.pred_scale = info.pred_scale;
        adpcm.yn1 = static_cast<uint16_t>(info.yn1);
        adpcm.yn2 = static_cast<uint16_t>(info.yn2);
        break;

    case ax::SampleFormat::kPcm16:
        adpcm.gain = ax::kPcm16Gain;
        break;

    case ax::SampleFormat::kPcm8:
        adpcm.gain = ax::kPcm8Gain;
        break;
    }

    vpb_->pb.adpcm = adpcm;
    sync_ |= ax::sync::kAdpcm;
}

void AxVoice::SetAdpcmLoop(const ntdlib::rsar::DspAdpcmInfo& info)
{
    if (!IsAvailable())
    {
        return;
    }

    ax::PbAdpcmLoop loop;
    if (format_ == ax::SampleFormat::kAdpcm)
    {
        loop.loop_pred_scale = info.loop_pred_scale;
        loop.loop_yn1 = static_cast<uint16_t>(info.loop_yn1);
        loop.loop_yn2 = static_cast<uint16_t>(info.loop_yn2);
    }

    vpb_->pb.adpcm_loop = loop;
    sync_ |= ax::sync::kAdpcmLoop;
}

bool AxVoice::SetMix(const MixParam& mix)
{
    if (!IsAvailable())
    {
        return false;
    }

    // The build of October 2006 sets the mix at once, with no ramp (CalcMixParam, 0x800dd970 in Smooth Moves' main.dol,
    // and AXSetVoiceMix).
    if (engine_->Build().October2006())
    {
        SetVoiceMix(StillMix(mix), true);

        return false;
    }

    // A voice that hasn't started jumps straight to its mix. A playing voice ramps from where it is.
    if (first_mix_update_ || !IsRun())
    {
        mix_prev_ = mix;
        first_mix_update_ = false;
    }

    const bool need_update = mix_prev_ != mix;

    // Each volume starts where the last frame's ramp left it. The ramp moves it towards the target over the frame. A
    // volume reaches its target when the ramp would overshoot, or when the target is 0.
    const auto ramp = [](uint16_t& prev, uint16_t target, uint16_t& volume, uint16_t& delta)
    {
        volume = prev;
        const int step = MixDelta(prev, target);
        delta = static_cast<uint16_t>(step);
        if (target == 0 || step == 0)
        {
            prev = target;
        }
        else
        {
            prev = static_cast<uint16_t>(prev + step * ax::kSamplesPerFrame);
        }
    };

    ax::PbMix pb_mix;
    ramp(mix_prev_.l, mix.l, pb_mix.l, pb_mix.delta_l);
    ramp(mix_prev_.r, mix.r, pb_mix.r, pb_mix.delta_r);
    ramp(mix_prev_.s, mix.s, pb_mix.s, pb_mix.delta_s);
    ramp(mix_prev_.aux_a_l, mix.aux_a_l, pb_mix.aux_a_l, pb_mix.delta_aux_a_l);
    ramp(mix_prev_.aux_a_r, mix.aux_a_r, pb_mix.aux_a_r, pb_mix.delta_aux_a_r);
    ramp(mix_prev_.aux_a_s, mix.aux_a_s, pb_mix.aux_a_s, pb_mix.delta_aux_a_s);
    ramp(mix_prev_.aux_b_l, mix.aux_b_l, pb_mix.aux_b_l, pb_mix.delta_aux_b_l);
    ramp(mix_prev_.aux_b_r, mix.aux_b_r, pb_mix.aux_b_r, pb_mix.delta_aux_b_r);
    ramp(mix_prev_.aux_b_s, mix.aux_b_s, pb_mix.aux_b_s, pb_mix.delta_aux_b_s);
    ramp(mix_prev_.aux_c_l, mix.aux_c_l, pb_mix.aux_c_l, pb_mix.delta_aux_c_l);
    ramp(mix_prev_.aux_c_r, mix.aux_c_r, pb_mix.aux_c_r, pb_mix.delta_aux_c_r);
    ramp(mix_prev_.aux_c_s, mix.aux_c_s, pb_mix.aux_c_s, pb_mix.delta_aux_c_s);
    SetVoiceMix(pb_mix, false);

    return need_update;
}

void AxVoice::SetSrc(float ratio, bool initial)
{
    if (!IsAvailable())
    {
        return;
    }

    // The ratio to AX's 32 kHz, as 16.16 fixed point. A voice that starts also starts its converter from rest.
    const float dsp_ratio = ratio * static_cast<float>(sample_rate_) / static_cast<float>(ax::kSampleRate);
    const uint32_t bits = ToUnsigned(65536.0f * dsp_ratio);
    ax::Pb& pb = vpb_->pb;
    if (initial)
    {
        pb.src = ax::PbSrc{};
        pb.src.ratio_hi = static_cast<uint16_t>(bits >> 16);
        pb.src.ratio_lo = static_cast<uint16_t>(bits);
        sync_ &= ~ax::sync::kSrcRatio;
        sync_ |= ax::sync::kSrc;
    }
    else
    {
        pb.src.ratio_hi = static_cast<uint16_t>(bits >> 16);
        pb.src.ratio_lo = static_cast<uint16_t>(bits);
        if (!(sync_ & ax::sync::kSrc))
        {
            sync_ |= ax::sync::kSrcRatio;
        }
    }
}

void AxVoice::SetVe(float volume, float init_volume)
{
    if (!IsAvailable())
    {
        return;
    }

    // AxVoiceParamBlock::SetVoiceVe: the first call also sets where the envelope starts. A gain of 1 is 32767. In older
    // builds, Voice's setters have already held the gain to 1 (see HeldGain). Builds from September 2009 on hold the
    // envelope's volume within 0 to 0xffff here.
    const bool above_full = engine_->Build().SinceSeptember2009();
    const auto to_volume = [above_full](float gain)
    {
        const int32_t value = ntdlib::nwsnd::RevolutionPolicy::ToInt32(32767.0f * gain);
        return static_cast<uint16_t>(above_full ? std::clamp(value, 0, 0xffff) : value);
    };

    if (first_ve_update_)
    {
        prev_volume_ = to_volume(init_volume);
        first_ve_update_ = false;
    }

    volume_ = to_volume(volume);
}

void AxVoice::SetVeOctober2006(uint16_t volume, int16_t delta)
{
    if (!IsAvailable())
    {
        return;
    }

    vpb_->pb.ve = {volume, static_cast<uint16_t>(delta)};
    vpb_->sync |= ax::sync::kVe;
}

void AxVoice::SetLpf(uint16_t freq)
{
    if (!IsAvailable())
    {
        return;
    }

    // Off, new coefficients for a filter that's on, or on from rest. The game turns the filter off with a block whose
    // coefficients it leaves unset. The microcode ignores them while the filter is off. The model writes zeros.
    ax::Pb& pb = vpb_->pb;
    if (freq >= 16000)
    {
        pb.lpf = ax::PbLpf{};
        sync_ |= ax::sync::kLpf;
    }
    else if (pb.lpf.on == ax::kLpfOn)
    {
        const ax::LpfCoefs coefs = ax::CalcLpfCoefs(freq);
        pb.lpf.a0 = coefs.a0;
        pb.lpf.b0 = coefs.b0;
        sync_ |= ax::sync::kLpfCoefs;
    }
    else
    {
        const ax::LpfCoefs coefs = ax::CalcLpfCoefs(freq);
        pb.lpf = {ax::kLpfOn, 0, coefs.a0, coefs.b0};
        sync_ |= ax::sync::kLpf;
    }
}

void AxVoice::SetBiquad(int type, float value)
{
    if (!IsAvailable())
    {
        return;
    }

    // Off, new coefficients for a filter that's on, or on from rest, as for the low-pass filter. The game's block for
    // off leaves the coefficients unset. The model writes zeros.
    const std::optional<std::array<uint16_t, 5>> coefs = BiquadFilterCoefs(type, value);
    ax::Pb& pb = vpb_->pb;
    if (!coefs)
    {
        pb.biquad = ax::PbBiquad{};
        sync_ |= ax::sync::kBiquad;
    }
    else if (pb.biquad.on == ax::kBiquadOn)
    {
        pb.biquad.b0 = (*coefs)[0];
        pb.biquad.b1 = (*coefs)[1];
        pb.biquad.b2 = (*coefs)[2];
        pb.biquad.a1 = (*coefs)[3];
        pb.biquad.a2 = (*coefs)[4];
        sync_ |= ax::sync::kBiquadCoefs;
    }
    else
    {
        pb.biquad = {ax::kBiquadOn, 0, 0, 0, 0, (*coefs)[0], (*coefs)[1], (*coefs)[2], (*coefs)[3], (*coefs)[4]};
        sync_ |= ax::sync::kBiquad;
    }
}

void AxVoice::SetPriority(uint32_t priority)
{
    if (IsAvailable())
    {
        engine_->Ax().SetVoicePriority(vpb_, priority);
    }
}

void AxVoice::SetVoiceType(ax::VoiceType type)
{
    if (!IsAvailable())
    {
        return;
    }

    vpb_->pb.type = static_cast<uint16_t>(type);
    sync_ |= ax::sync::kType;
}

void AxVoice::EnableRemote(bool enable)
{
    if (!IsAvailable())
    {
        return;
    }

    vpb_->pb.remote = enable ? 1 : 0;
    sync_ |= ax::sync::kRemote;
}

void AxVoice::Sync()
{
    if (!IsAvailable())
    {
        return;
    }

    // The build of October 2006 sets the envelope in Voice::CalcAxVe. Only the other changes go to AX here.
    if (engine_->Build().October2006())
    {
        vpb_->sync |= sync_;
        sync_ = 0;

        return;
    }

    // The envelope starts the frame at the volume that the model computed for the end of the last frame. It ramps by a
    // whole step per sample towards the target. The step is rounded towards whichever value comes closer. The game's
    // alternative step is 1, or -1 when the step is 0.
    ax::PbVe& ve = vpb_->pb.ve;
    ve.current_volume = prev_volume_;
    const auto delta_in = static_cast<int16_t>((volume_ - prev_volume_) / ax::kSamplesPerFrame);
    const int16_t delta_out = delta_in != 0 ? 1 : -1;
    const int predicted_in = prev_volume_ + delta_in * ax::kSamplesPerFrame;
    const int predicted_out = prev_volume_ + delta_out * ax::kSamplesPerFrame;
    int16_t delta = std::abs(volume_ - predicted_in) < std::abs(volume_ - predicted_out) ? delta_in : delta_out;

    // A step of 0, after one, lands on the target at once. The ramp mustn't take the volume out of 0 to 32767 by the
    // end of the frame. Builds from September 2009 on allow 0 to 0xffff. Builds from 2008 keep the step in range first
    // (Mario Kart Wii, AxVoiceParamBlock::Sync, 0x8008bd60). A step that the range cuts to 0 then lands on the target
    // too. A releasing note's last frame shows this.
    const auto land = [&]
    {
        if (delta == 0 && prev_delta_ == 0)
        {
            ve.current_volume = volume_;
        }
    };

    const int max_volume = engine_->Build().SinceSeptember2009() ? 0xffff : 32767;
    const auto keep_in_range = [&]
    {
        const int next_volume = prev_volume_ + delta * ax::kSamplesPerFrame;
        if (next_volume < 0)
        {
            delta = static_cast<int16_t>(-prev_volume_ / ax::kSamplesPerFrame);
        }
        else if (next_volume > max_volume)
        {
            delta = static_cast<int16_t>((max_volume - prev_volume_) / ax::kSamplesPerFrame);
        }
    };

    if (engine_->Build().SinceMarch2008())
    {
        keep_in_range();
        land();
    }
    else
    {
        land();
        keep_in_range();
    }

    ve.current_delta = static_cast<uint16_t>(delta);
    sync_ &= ~ax::sync::kVeDelta;
    sync_ |= ax::sync::kVe;
    prev_volume_ = ve.current_volume;
    prev_delta_ = delta;
    vpb_->sync |= sync_;
    sync_ = 0;
}

void AxVoice::Run()
{
    if (IsAvailable())
    {
        vpb_->SetState(ax::VoiceState::kRun);
    }
}

void AxVoice::Stop()
{
    if (IsRun())
    {
        vpb_->SetState(ax::VoiceState::kStop);
    }
}

uint32_t AxVoice::DspAddressBySample(ntdlib::PAddr base, uint32_t samples, ax::SampleFormat format)
{
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        return samples / ax::kAdpcmSamplesPerFrame * ax::kAdpcmNibblesPerFrame + samples % ax::kAdpcmSamplesPerFrame +
               base * 2 + 2;
    case ax::SampleFormat::kPcm8:
        return base + samples;
    case ax::SampleFormat::kPcm16:
        return base / 2 + samples;
    }

    return 0;
}

uint32_t AxVoice::SampleByDspAddress(uint32_t address, ax::SampleFormat format)
{
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        {
            const uint32_t frames = address / ax::kAdpcmNibblesPerFrame;
            return frames * ax::kAdpcmSamplesPerFrame + address % ax::kAdpcmNibblesPerFrame - 2;
        }

    case ax::SampleFormat::kPcm8:
    case ax::SampleFormat::kPcm16:
        return address;
    }

    return 0;
}

void AxVoice::SetVoiceMix(const ax::PbMix& mix, bool sync_now)
{
    // AxVoiceParamBlock::SetVoiceMix (0x80034880). The mixer control has a bit for each volume that isn't 0. It also
    // has a ramp bit for each ramp that isn't 0.
    namespace m = ax::mixer;
    uint32_t ctrl = 0;
    const auto flag = [&ctrl](uint16_t value, uint32_t bits)
    {
        if (value != 0)
        {
            ctrl |= bits;
        }
    };

    flag(mix.l, m::kL);
    flag(mix.delta_l, m::kDelta | m::kL);
    flag(mix.r, m::kR);
    flag(mix.delta_r, m::kDelta | m::kR);
    flag(mix.aux_a_l, m::kAuxAL);
    flag(mix.delta_aux_a_l, m::kDeltaA | m::kAuxAL);
    flag(mix.aux_a_r, m::kAuxAR);
    flag(mix.delta_aux_a_r, m::kDeltaA | m::kAuxAR);
    flag(mix.aux_b_l, m::kAuxBL);
    flag(mix.delta_aux_b_l, m::kDeltaB | m::kAuxBL);
    flag(mix.aux_b_r, m::kAuxBR);
    flag(mix.delta_aux_b_r, m::kDeltaB | m::kAuxBR);
    flag(mix.aux_c_l, m::kAuxCL);
    flag(mix.delta_aux_c_l, m::kDeltaC | m::kAuxCL);
    flag(mix.aux_c_r, m::kAuxCR);
    flag(mix.delta_aux_c_r, m::kDeltaC | m::kAuxCR);
    flag(mix.s, m::kS);
    flag(mix.delta_s, m::kDeltaS | m::kS);
    flag(mix.aux_a_s, m::kAuxAS);
    flag(mix.delta_aux_a_s, m::kDeltaAS | m::kAuxAS);
    flag(mix.aux_b_s, m::kAuxBS);
    flag(mix.delta_aux_b_s, m::kDeltaBS | m::kAuxBS);
    flag(mix.aux_c_s, m::kAuxCS);
    flag(mix.delta_aux_c_s, m::kDeltaCS | m::kAuxCS);

    vpb_->pb.mix = mix;
    vpb_->pb.SetMixerCtrl(ctrl);
    if (sync_now)
    {
        vpb_->sync |= ax::sync::kMix | ax::sync::kMixerCtrl;
    }
    else
    {
        sync_ |= ax::sync::kMix | ax::sync::kMixerCtrl;
    }
}

void AxVoice::UpdateDelta()
{
    // AxVoiceParamBlock::UpdateDelta: the envelope ends its ramp where the frame took it.
    if (!IsAvailable())
    {
        return;
    }

    prev_volume_ =
        static_cast<uint16_t>(prev_volume_ + static_cast<int16_t>(vpb_->pb.ve.current_delta) * ax::kSamplesPerFrame);
    vpb_->pb.ve.current_volume = prev_volume_;
    vpb_->pb.ve.current_delta = 0;
    vpb_->sync |= ax::sync::kVe;
}

void AxVoice::ResetDelta()
{
    if (!IsAvailable())
    {
        return;
    }

    // The envelope and the mix stop their ramps where they are.
    UpdateDelta();
    SetVoiceMix(StillMix(mix_prev_), true);
}

AxVoiceManager::AxVoiceManager(Engine& engine) : engine_(engine)
{
    for (std::unique_ptr<AxVoice>& voice : pool_)
    {
        voice = std::make_unique<AxVoice>(engine);
        free_.push_back(voice.get());
    }
}

AxVoiceManager::Snapshot AxVoiceManager::Save() const
{
    Snapshot snapshot{{}, free_, active_, reserved_};
    snapshot.voices.reserve(pool_.size());
    for (const std::unique_ptr<AxVoice>& voice : pool_)
    {
        snapshot.voices.push_back(*voice);
    }

    return snapshot;
}

void AxVoiceManager::Restore(const Snapshot& snapshot)
{
    for (std::size_t i = 0; i < pool_.size(); i++)
    {
        *pool_[i] = snapshot.voices[i];
    }

    free_ = snapshot.free;
    active_ = snapshot.active;
    reserved_ = snapshot.reserved;
}

AxVoice* AxVoiceManager::AcquireAxVoice(uint32_t priority, AxVoice::Callback callback)
{
    AxVoice* voice = Alloc();
    if (voice == nullptr)
    {
        return nullptr;
    }

    // AX calls this when it takes the voice away (AxVoice::VoiceCallback, 0x80032f58).
    const auto on_drop = [this, voice](ax::Voice*)
    {
        voice->vpb_ = nullptr;
        voice->sync_ = 0;
        ReserveForFreeAxVoice(voice);
    };
    ax::Voice* vpb = engine_.Ax().AcquireVoice(priority, on_drop);
    if (vpb == nullptr)
    {
        Free(voice);

        return nullptr;
    }

    // AxVoiceParamBlock::Set (0x80034854).
    voice->vpb_ = vpb;
    voice->sync_ = 0;
    voice->first_ve_update_ = true;
    voice->volume_ = ax::kMaxVolume;
    voice->prev_volume_ = ax::kMaxVolume;
    voice->prev_delta_ = 0;
    voice->callback_ = std::move(callback);

    return voice;
}

void AxVoiceManager::FreeAxVoice(AxVoice* voice)
{
    if (voice->IsAvailable())
    {
        engine_.Ax().FreeVoice(voice->vpb_);
    }

    Free(voice);
}

void AxVoiceManager::ReserveForFreeAxVoice(AxVoice* voice)
{
    voice->reserve_for_free_ = true;
    active_.remove(voice);
    reserved_.push_back(voice);
}

void AxVoiceManager::FreeAllReservedAxVoice()
{
    while (!reserved_.empty())
    {
        AxVoice* voice = reserved_.front();
        if (voice->callback_)
        {
            const AxVoice::Callback callback = voice->callback_;
            callback(voice);
        }

        FreeAxVoice(voice);
    }
}

AxVoice* AxVoiceManager::Alloc()
{
    // AX reuses its voices as soon as it drops them. Free the AxVoices of the dropped voices first.
    FreeAllReservedAxVoice();
    if (free_.empty())
    {
        return nullptr;
    }

    AxVoice* voice = free_.front();
    free_.pop_front();
    *voice = AxVoice(engine_);
    active_.push_back(voice);

    return voice;
}

void AxVoiceManager::Free(AxVoice* voice)
{
    // The game reads the reserve flag after destroying the voice. Destroying the voice leaves the flag unchanged.
    if (voice->reserve_for_free_)
    {
        reserved_.remove(voice);
    }
    else
    {
        active_.remove(voice);
    }

    free_.push_back(voice);
}

bool Voice::Acquire(int channels, int priority, ChannelId owner)
{
    channels = std::clamp(channels, 1, 2);

    // Take an AX voice for each channel. When AX has none free, check whether dropping NW4R SND voices of no higher
    // priority would free enough. Then request an AX voice at a priority high enough to steal one.
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

    return true;
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
    SetMainSend(param.main_send);

    // The game adds the sound's sends to the track's sends (SeqTrack::UpdateChannelParam; Animal Crossing: City Folk,
    // 0x802682b4). The channel then passes the sums on. The model adds them here instead. The sums don't change: each
    // of the game's other terms is 0. The sends are those of the sound whose sequence played the channel's note. A
    // channel that its track has let go keeps them.
    const int player = owner_ == kNoChannel ? -1 : engine_->Channels().At(owner_).source_player;
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
    // CalcMixParam (0x8004d590), for the stereo output mode on the main line. The Wii's policy places each channel of
    // the wave: its pan, the sides that it reaches, and the pan curve.
    const float main_volume = main_out_volume_;
    const float main = main_volume * main_send_;
    const float fx_a = main_volume * fx_send_[0];
    const float fx_b = main_volume * fx_send_[1];
    const float fx_c = main_volume * fx_send_[2];

    using Policy = ntdlib::nwsnd::RevolutionPolicy;
    const Policy::WavePlacement placement = Policy::PlacementOfWaveChannel(
        pan_, channel, channel_count_, static_cast<uint32_t>(pan_mode_), pan_curve_, engine_->Build());
    const PanInfo info = ntdlib::nwsnd::PanInfoOf(placement.curve);
    float left = 0.0f;
    float right = 0.0f;
    if (placement.side != Policy::WaveSide::kRightOnly)
    {
        left = CalcPanRatio(placement.pan, info);
    }

    if (placement.side != Policy::WaveSide::kLeftOnly)
    {
        right = CalcPanRatio(-placement.pan, info);
    }

    mix.l = MixVolume(main * left);
    mix.r = MixVolume(main * right);
    mix.s = MixVolume(0.0f);
    mix.aux_a_l = MixVolume(fx_a * left);
    mix.aux_a_r = MixVolume(fx_a * right);
    mix.aux_a_s = MixVolume(0.0f);
    mix.aux_b_l = MixVolume(fx_b * left);
    mix.aux_b_r = MixVolume(fx_b * right);
    mix.aux_b_s = MixVolume(0.0f);
    mix.aux_c_l = MixVolume(fx_c * left);
    mix.aux_c_r = MixVolume(fx_c * right);
    mix.aux_c_s = MixVolume(0.0f);
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

VoiceManager::VoiceManager(Engine& engine)
{
    for (int place = 0; place < kVoiceCount; place++)
    {
        pool_[place] = std::make_unique<Voice>(engine, place);
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
