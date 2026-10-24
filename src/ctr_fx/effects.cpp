// SPDX-License-Identifier: MIT

#include "ntdlib/ctr_fx/effects.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ntdlib/ctr_fx/vfp.h"

namespace ntdlib::ctr_fx
{
namespace
{

constexpr uint32_t kLibraryFrame = 160;          // the samples of the libraries' frame: the unit of the filter sizes
constexpr uint32_t kShortestTime = 0x409c6a7f;   // 4.888 ms, the shortest time that a reverb's buffer counts
constexpr uint32_t kFramesPerMs = 0x3e517e1d;    // 0.20458265
constexpr uint32_t kMostDamping = 0x3f733333;    // 0.95
constexpr uint32_t kMicrosecondsPerFrame = 4888; // the delay's frame

// The ARM11's 32-bit products, sums and differences. They wrap.
int32_t Times(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

int32_t Plus(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t Minus(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// A sample times a gain in 128ths. The effects scale their filters' samples this way: the magnitude's product,
// shifted, takes the sample's sign.
int32_t Scaled(int32_t sample, int32_t gain)
{
    if (sample >= 0)
    {
        return Times(sample, gain) >> 7;
    }

    return Minus(0, Times(Minus(0, sample), gain) >> 7);
}

// A gain from 0 to 1 as the libraries check it: not below 0, and with a bit pattern no greater than that of 1. A
// negative NaN passes.
bool InRange(float gain)
{
    return !(gain < 0.0f) && std::bit_cast<int32_t>(gain) <= std::bit_cast<int32_t>(1.0f);
}

bool IsNnSnd(Library library)
{
    return library != Library::kNwSnd;
}

// The damping, taken as 0.95 above that, and the damping filter's two gains in 128ths: of the input, and of the last
// output.
struct Damping
{
    int32_t input;
    int32_t feedback;
};

Damping DampingOf(float damping, bool high_pass)
{
    const float d = std::bit_cast<int32_t>(damping) > static_cast<int32_t>(kMostDamping)
                        ? std::bit_cast<float>(kMostDamping)
                        : damping;
    if (high_pass)
    {
        return {vfp::ToS32(vfp::Mul(vfp::Sub(d, 1.0f), 128.0f)), vfp::ToS32(vfp::Mul(d, -128.0f))};
    }

    return {vfp::ToS32(vfp::Mul(vfp::Sub(1.0f, d), 128.0f)), vfp::ToS32(vfp::Mul(d, 128.0f))};
}

void CheckFormat(const FrameFormat& format)
{
    if (format.samples == 0 || format.samples > 1u << 16)
    {
        throw std::invalid_argument("a frame format needs 1 to 65,536 samples");
    }
}

// The checks that the reverb's SetParam makes, and the model's checks of the filter sizes.
bool Acceptable(Library library, const ReverbParam& param)
{
    if (!InRange(param.coloration) || !InRange(param.damping) || !InRange(param.early_gain) ||
        !InRange(param.fused_gain))
    {
        return false;
    }
    if ((IsNnSnd(library) && param.enable_surround) || (library == Library::kNnSnd2011 && param.high_pass_damping))
    {
        return false;
    }

    for (const uint32_t size : param.filter_sizes)
    {
        if (size == 0 || size % kLibraryFrame != 0 || size > 1u << 24)
        {
            return false;
        }
    }

    return true;
}

} // namespace

uint32_t ReverbFrames(uint32_t time)
{
    const float ms = vfp::FromU32(time);
    const float from =
        std::bit_cast<int32_t>(ms) > static_cast<int32_t>(kShortestTime) ? ms : std::bit_cast<float>(kShortestTime);

    return vfp::ToU32(vfp::Mul(from, std::bit_cast<float>(kFramesPerMs)));
}

uint32_t DelayFrames(uint32_t time)
{
    const uint32_t frames = (time * 1000u) / kMicrosecondsPerFrame;

    return frames == 0 ? 1 : frames;
}

Reverb::Reverb(Library library, const ReverbParam& param, FrameFormat format)
    : library_(library), format_(format), param_(param), setup_(param)
{
    CheckFormat(format);
    if (!Acceptable(library, param))
    {
        throw std::invalid_argument("the reverb's parameters are out of range");
    }

    LayOut();
    Configure();
}

bool Reverb::SetParam(const ReverbParam& param)
{
    if (!Acceptable(library_, param) || param.early_reflection_time > setup_.early_reflection_time ||
        param.pre_delay_time > setup_.pre_delay_time || (param.enable_surround && !setup_.enable_surround))
    {
        return false;
    }

    for (std::size_t i = 0; i < 3; i++)
    {
        if (param.filter_sizes[i] > setup_.filter_sizes[i])
        {
            return false;
        }
    }

    param_ = param;
    if (IsNnSnd(library_))
    {
        setup_ = param;
        LayOut();
    }

    Configure();

    return true;
}

void Reverb::LayOut()
{
    // Initialize lays each channel's buffers out from the setup's times and sizes.
    channel_count_ = setup_.enable_surround ? 4 : 2;
    const std::size_t samples = format_.samples;
    const std::size_t early = std::size_t{ReverbFrames(setup_.early_reflection_time)} * samples;
    const std::size_t pre_delay = std::size_t{ReverbFrames(setup_.pre_delay_time)} * samples;
    std::array<std::size_t, 3> filters{};
    for (std::size_t i = 0; i < 3; i++)
    {
        filters[i] = std::size_t{setup_.filter_sizes[i] / kLibraryFrame} * samples;
    }

    const std::size_t per_channel = early + pre_delay + filters[0] + filters[1] + filters[2];
    if (per_channel * channel_count_ > std::size_t{1} << 28)
    {
        throw std::invalid_argument("the reverb's buffers are too long");
    }

    for (std::size_t c = 0; c < channel_count_; c++)
    {
        early_[c] = c * per_channel;
        pre_delay_[c] = early_[c] + early;
        comb_[c][0] = pre_delay_[c] + pre_delay;
        comb_[c][1] = comb_[c][0] + filters[0];
        all_pass_[c] = comb_[c][1] + filters[1];
    }
    memory_.assign(per_channel * channel_count_, 0);
}

void Reverb::Configure()
{
    const auto samples = static_cast<uint32_t>(format_.samples);
    channel_count_ = param_.enable_surround ? 4 : 2;
    early_length_ = ReverbFrames(param_.early_reflection_time) * samples;
    pre_delay_length_ = ReverbFrames(param_.pre_delay_time) * samples;
    comb_length_[0] = param_.filter_sizes[0] / kLibraryFrame * samples;
    comb_length_[1] = param_.filter_sizes[1] / kLibraryFrame * samples;
    all_pass_length_ = param_.filter_sizes[2] / kLibraryFrame * samples;
    early_index_ = 0;
    pre_delay_index_ = 0;
    comb_index_ = {};
    all_pass_index_ = 0;

    // Each comb filter's gain falls by 60 dB over the fused time: 10^(-3·length/(time·rate)).
    const float fused = vfp::Mul(vfp::Mul(vfp::FromU32(param_.fused_time), 0.001f), format_.rate);
    for (std::size_t i = 0; i < 2; i++)
    {
        const float exponent = vfp::Div(vfp::Mul(vfp::FromS32(static_cast<int32_t>(comb_length_[i])), -3.0f), fused);
        comb_gain_[i] = vfp::ToS32(vfp::Mul(vfp::PowF(10.0f, exponent), 128.0f));
    }

    early_gain_ = vfp::ToS32(vfp::Mul(param_.early_gain, 128.0f));
    fused_gain_ = vfp::ToS32(vfp::Mul(param_.fused_gain, 128.0f));
    all_pass_gain_ = vfp::ToS32(vfp::Mul(param_.coloration, 128.0f));
    const Damping damping = DampingOf(param_.damping, param_.high_pass_damping);
    damping_input_ = damping.input;
    damping_feedback_ = damping.feedback;
    std::fill(memory_.begin(), memory_.end(), 0);
}

void Reverb::Process(const Channels& channels, std::size_t samples)
{
    if (samples != format_.samples)
    {
        throw std::invalid_argument("a frame of another length");
    }

    // Every channel starts from the same positions.
    for (std::size_t c = 0; c < channel_count_; c++)
    {
        int32_t* const x = channels[c];
        int32_t* const early = memory_.data() + early_[c] + early_index_;
        int32_t* const pre_delay = memory_.data() + pre_delay_[c] + pre_delay_index_;
        int32_t* const comb0 = memory_.data() + comb_[c][0] + comb_index_[0];
        int32_t* const comb1 = memory_.data() + comb_[c][1] + comb_index_[1];
        int32_t* const all_pass = memory_.data() + all_pass_[c] + all_pass_index_;
        int32_t damped = damping_state_[c];
        for (std::size_t n = 0; n < samples; n++)
        {
            const int32_t input = x[n];
            const int32_t reflection = early[n];
            early[n] = input;
            const int32_t delayed = pre_delay[n];
            pre_delay[n] = input;
            const int32_t first = comb0[n];
            comb0[n] = Plus(delayed, Scaled(first, comb_gain_[0]));
            const int32_t second = comb1[n];
            comb1[n] = Plus(delayed, Scaled(second, comb_gain_[1]));
            const int32_t held = all_pass[n];
            const int32_t fed = Plus(Scaled(held, all_pass_gain_), Minus(first, second));
            all_pass[n] = fed;
            const int32_t passed = Minus(held, Scaled(fed, all_pass_gain_));

            // nn::snd of 2011 takes the damping times the sum of the input and the last output from the input.
            if (library_ == Library::kNnSnd2011)
            {
                damped = Minus(passed, Times(damping_feedback_, Plus(damped, passed)) >> 7);
            }
            else
            {
                damped = Plus(Times(damping_input_, passed), Times(damped, damping_feedback_)) >> 7;
            }

            x[n] = Plus(Times(fused_gain_, damped), Times(reflection, early_gain_)) >> 7;
        }
        damping_state_[c] = damped;
    }

    // Each position moves on by the frame, and starts again at its buffer's length.
    const auto step = static_cast<uint32_t>(samples);
    const auto advance = [&](uint32_t& index, uint32_t length)
    {
        index += step;
        if (length <= index)
        {
            index = 0;
        }
    };
    advance(early_index_, early_length_);
    advance(pre_delay_index_, pre_delay_length_);
    advance(comb_index_[0], comb_length_[0]);
    advance(comb_index_[1], comb_length_[1]);
    advance(all_pass_index_, all_pass_length_);
}

const ReverbParam& Reverb::Param() const
{
    return param_;
}

std::size_t Reverb::ChannelCount() const
{
    return channel_count_;
}

Delay::Delay(const DelayParam& param, FrameFormat format) : format_(format), param_(param), setup_(param)
{
    CheckFormat(format);
    if (!SetParam(param))
    {
        throw std::invalid_argument("the delay's parameters are out of range");
    }

    // Initialize gives each channel a line of the setup's frames.
    setup_ = param;
    setup_frames_ = frames_;
    if (setup_frames_ * format.samples * channel_count_ > std::size_t{1} << 28)
    {
        throw std::invalid_argument("the delay's lines are too long");
    }

    memory_.assign(setup_frames_ * format.samples * channel_count_, 0);
    frame_index_ = 0;
    damping_state_ = {};
}

bool Delay::SetParam(const DelayParam& param)
{
    if (!InRange(param.damping) || !InRange(param.feedback))
    {
        return false;
    }
    if (!memory_.empty() &&
        (param.delay_time > setup_.delay_time || (param.enable_surround && !setup_.enable_surround)))
    {
        return false;
    }

    param_ = param;
    frames_ = DelayFrames(param.delay_time);
    channel_count_ = param.enable_surround ? 4 : 2;
    feedback_ = vfp::ToS32(vfp::Mul(param.feedback, 128.0f));
    const Damping damping = DampingOf(param.damping, false);
    damping_input_ = damping.input;
    damping_feedback_ = damping.feedback;

    return true;
}

void Delay::Process(const Channels& channels, std::size_t samples)
{
    if (samples != format_.samples)
    {
        throw std::invalid_argument("a frame of another length");
    }

    // The frame's place in each line. After a shorter delay, a place past the new length still lies in the line.
    const std::size_t at = std::size_t{frame_index_} * samples;
    for (std::size_t c = 0; c < channel_count_; c++)
    {
        int32_t* const x = channels[c];
        int32_t* const line = memory_.data() + c * setup_frames_ * samples + at;
        int32_t damped = damping_state_[c];
        for (std::size_t n = 0; n < samples; n++)
        {
            const int32_t delayed = line[n];
            const int32_t fed = Minus(x[n], Scaled(delayed, feedback_));
            damped = Plus(Times(damping_input_, fed), Times(damping_feedback_, damped)) >> 7;
            line[n] = damped;
            x[n] = delayed;
        }
        damping_state_[c] = damped;
    }

    frame_index_++;
    if (frames_ <= frame_index_)
    {
        frame_index_ = 0;
    }
}

const DelayParam& Delay::Param() const
{
    return param_;
}

std::size_t Delay::ChannelCount() const
{
    return channel_count_;
}

} // namespace ntdlib::ctr_fx
