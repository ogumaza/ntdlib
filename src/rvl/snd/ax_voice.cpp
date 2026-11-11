// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <utility>

#include "ntdlib/nwsnd/util.h"
#include "ntdlib/rvl/ax/pb.h"
#include "ntdlib/rvl/ax/system.h"
#include "ntdlib/rvl/ax/voice.h"
#include "ntdlib/rvl/snd/driver.h"
#include "ntdlib/rvl/snd/engine.h"
#include "ntdlib/rvl/snd/util.h"
#include "ntdlib/wave/adpcm.h"

#include "rvl/snd/held_gain.h"

namespace ntdlib::rvl::snd
{
namespace
{

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

// The warp's constants in the game's code.
constexpr float kWarpNear = 0x1.33333p-3f; // 0.14999998
constexpr float kWarpFar = 0x1.666668p-2f; // 0.35000002
static_assert(std::bit_cast<uint32_t>(kWarpNear) == 0x3e199998 && std::bit_cast<uint32_t>(kWarpFar) == 0x3eb33334);

// The warp of a voice's pan and surround pan for Dolby Pro Logic II, in Skyward Sword HD's NW4R SND (CalcMixParam,
// main 0x102a530 to 0x102a73c). d is the surround pan less 1: -1 at the front, 1 at the rear. Where the pan is further
// from the centre than d, the pan's size becomes 0.85 times its size less 0.15 times d (0.35 times d behind). The
// surround pan then comes from the pan. In front, it takes 1.12 times the size of a pan on the left, and 0.88 times the
// size of one on the right. Elsewhere, the surround pan comes from d, and a pan behind halves.
void WarpDpl2(float& pan, float& span)
{
    const float d = span + -1.0f;
    const float pan_size = pan < 0.0f ? -pan : pan;
    const float d_size = d < 0.0f ? -d : d;
    const bool rear = !(d <= 0.0f);
    float shifted = 0.0f;
    if (!(pan_size <= d_size))
    {
        const float ratio = -d / pan;
        const bool right = pan >= 0.0f;
        const float factor = (rear ? kWarpFar : kWarpNear) * ratio + (right ? 0.85f : -0.85f);
        const float scaled = pan * (right && !rear ? 0.88f : right ? 1.12f : -1.12f);
        pan = pan_size * factor;
        shifted = rear ? scaled + -0.12f : (d + d + scaled) + -0.12f;
    }
    else
    {
        if (rear)
        {
            pan = pan * 0.5f;
        }
        shifted = d * (rear ? 1.12f : 0.88f) + -0.12f;
    }

    span = shifted + 1.0f;
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

AxVoice::MixParam CalcMix(const MixInput& input, int channel, const SndBuild& build, ntdlib::nwsnd::OutputMode mode)
{
    // CalcMixParam (0x8004d590) on the main line. The Wii's policy places each channel of the wave: its pan, the sides
    // that it reaches, and the pan curve.
    using Policy = ntdlib::nwsnd::RevolutionPolicy;
    const Policy::WavePlacement placement = Policy::PlacementOfWaveChannel(
        input.pan, channel, input.channel_count, static_cast<uint32_t>(input.pan_mode), input.pan_curve, build);
    const PanInfo info = ntdlib::nwsnd::PanInfoOf(placement.curve);
    const float main_volume = input.main_out_volume;
    const float main = main_volume * input.main_send;
    const float fx_a = main_volume * input.fx_send[0];
    const float fx_b = main_volume * input.fx_send[1];
    const float fx_c = main_volume * input.fx_send[2];
    AxVoice::MixParam mix;
    if (mode == ntdlib::nwsnd::OutputMode::kDpl2)
    {
        // Skyward Sword HD's CalcMixParam for Dolby Pro Logic II (main 0x102a434 to 0x102ac6c). A channel that reaches
        // both sides takes the warp. The pan gives the left and right ratios, and the surround pan the front and rear
        // ratios. Each mix takes the product of one of each: left front, right front, left rear and right rear. Each
        // bus's rear right is a volume of aux bus C. Bus C has no mix in this mode.
        float pan = placement.pan;
        float span = input.surround_pan;
        if (placement.side == Policy::WaveSide::kBoth)
        {
            WarpDpl2(pan, span);
        }

        const float left = placement.side != Policy::WaveSide::kRightOnly ? CalcPanRatio(pan, info) : 0.0f;
        const float right = placement.side != Policy::WaveSide::kLeftOnly ? CalcPanRatio(-pan, info) : 0.0f;
        const float front = CalcSurroundPanRatio(span, info);
        const float rear = CalcSurroundPanRatio(2.0f - span, info);
        const float front_left = front * left;
        const float front_right = front * right;
        const float rear_left = rear * left;
        const float rear_right = rear * right;
        mix.l = MixVolume(main * front_left);
        mix.r = MixVolume(main * front_right);
        mix.s = MixVolume(main * rear_left);
        mix.aux_a_l = MixVolume(fx_a * front_left);
        mix.aux_a_r = MixVolume(fx_a * front_right);
        mix.aux_a_s = MixVolume(fx_a * rear_left);
        mix.aux_b_l = MixVolume(fx_b * front_left);
        mix.aux_b_r = MixVolume(fx_b * front_right);
        mix.aux_b_s = MixVolume(fx_b * rear_left);
        mix.aux_c_l = MixVolume(main * rear_right);
        mix.aux_c_r = MixVolume(fx_a * rear_right);
        mix.aux_c_s = MixVolume(fx_b * rear_right);

        return mix;
    }

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

    return mix;
}

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
        return base * 2 + wave::DspAdpcmNibbleAddress(samples);
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

} // namespace ntdlib::rvl::snd
