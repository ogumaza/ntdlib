// SPDX-License-Identifier: MIT

// The effects that the 3DS's sound libraries run on the ARM11 over an aux bus: the reverb and the delay of nw::snd
// (FxReverb and FxDelay) and of nn::snd. Each model gives the libraries' samples bit for bit. A model is a value: a
// copy holds its whole state, and a copy of a running model is a snapshot of it.
//
// The 3DS mixes the aux buses in frames of 160 samples at 32,728 Hz. Each sample is a 32-bit integer on the scale of a
// 16-bit one. A bus has four channels: front left, front right, rear left and rear right. The effects process a frame
// in place. They work in integers, with gains in 128ths. Their setup computes the gains in the ARM11's single
// precision (vfp.h), and takes each gain's whole part.
//
// The libraries keep each buffer in whole frames. Another frame length keeps the same number of frames, and therefore
// the same times, such as frames of 235 samples at about 48,070 Hz. The coefficients then follow from the buffers'
// lengths at that rate. The damping filters keep their coefficients: their cutoffs move with the rate.
//
// nw::snd's FxDelay and nn::snd's FxDelay compute the same: Delay models both. nn::snd's FxReverb comes in two
// versions. Both process two channels. The later one computes as nw::snd's FxReverb does. The earlier one's damping
// filter subtracts its last output where the later one adds it. The libraries read the early reflection at different
// points of each sample. That can't change the output: each buffer holds a frame at least, and no two buffers share
// memory.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ntdlib::ctr_fx
{

// The frames that a model processes: the samples of a frame and their rate in Hz.
struct FrameFormat
{
    std::size_t samples = 160;
    float rate = 32728.0f;
};

// The four channels of an aux bus's frame. A model reads and writes only the channels that it processes.
using Channels = std::array<int32_t*, 4>;

// The library whose reverb a model follows.
enum class Library
{
    kNwSnd,     // nw::snd's FxReverb
    kNnSnd,     // nn::snd's FxReverb as games from 2013 on link it, such as Pokémon X and Majora's Mask 3D
    kNnSnd2011, // nn::snd's FxReverb as Ocarina of Time 3D links it (2011)
};

// The reverb's parameters: nw::snd's FxReverb::Param, with the filter sizes in place of the pointer to them. The
// defaults are those of nw::snd, and of nn::snd too.
struct ReverbParam
{
    uint32_t early_reflection_time = 60; // ms
    uint32_t fused_time = 4000;          // ms, the decay of the comb filters by 60 dB
    uint32_t pre_delay_time = 100;       // ms
    float coloration = 0.5f;             // the all-pass filter's gain, from 0 to 1
    float damping = 0.4f;                // from 0 to 1, taken as 0.95 above that

    // The lengths of the two comb filters and of the all-pass filter, in samples of 160-sample frames: a whole number
    // of frames each, and one at least. The libraries read past a filter that isn't whole frames, and the model
    // refuses one.
    std::array<uint32_t, 3> filter_sizes{3040, 3680, 2080};

    float early_gain = 0.6f;      // from 0 to 1
    float fused_gain = 0.4f;      // from 0 to 1
    bool enable_surround = false; // all four channels, nw::snd only

    // The damping filter keeps the highs in place of the lows. nn::snd of 2011 has no such option: its filter always
    // keeps the highs.
    bool high_pass_damping = false;
};

// The delay's parameters: nw::snd's FxDelay::Param. nn::snd's FxDelay takes the same. The defaults are those of
// nw::snd.
struct DelayParam
{
    uint32_t delay_time = 250;    // ms
    float feedback = 0.4f;        // from 0 to 1
    float damping = 0.5f;         // from 0 to 1, taken as 0.95 above that
    bool enable_surround = false; // all four channels
};

// The frames of a reverb's delay of `time` ms: the time, 4.888 ms at least, times 0.20458265 frames a millisecond. The
// product rounds to nearest in single precision, and its whole part gives the frames. The product for 4.888 ms rounds
// to 1. Every buffer therefore holds a frame at least.
uint32_t ReverbFrames(uint32_t time);

// The frames of a delay of `time` ms: the microseconds over 4,888, rounded down, and 1 at least. The microseconds wrap
// at 2^32.
uint32_t DelayFrames(uint32_t time);

// nw::snd's FxReverb, or one of nn::snd's two versions. Each channel runs an early reflection, and a pre-delay that
// feeds two comb filters in parallel. Their difference goes through an all-pass filter and a one-pole damping filter.
// The output is the early reflection times its gain and the damping filter's output times the fused gain.
class Reverb
{
public:
    // Sets the reverb up as the library's SetParam and Initialize do, with the buffers in silence. Throws
    // std::invalid_argument for parameters that the library refuses, for filter sizes that aren't whole frames or
    // hold none, for surround with nn::snd, for high-pass damping with nn::snd of 2011, and for a frame format without
    // samples.
    Reverb(Library library, const ReverbParam& param, FrameFormat format = {});

    // Changes the parameters after the setup. The times and the filter sizes can't grow past those of the setup, and
    // surround can't start after a setup without it. The buffers fall silent and start again from their first frames.
    // The damping filters keep their states. nw::snd's SetParam keeps the buffers where its setup laid them out.
    // nn::snd's SetParameters only stores the values. They take effect when the game sets the effect on its bus again,
    // and Initialize lays the buffers out again from them. For nn::snd, this function does both. Returns false, and
    // changes nothing, for parameters that the library refuses.
    bool SetParam(const ReverbParam& param);

    // Processes a frame in place: channels 0 and 1, or all four with surround. Throws std::invalid_argument when
    // `samples` isn't the format's length.
    void Process(const Channels& channels, std::size_t samples);

    const ReverbParam& Param() const;
    std::size_t ChannelCount() const;

private:
    // Lays the buffers out from the setup's times and sizes, in silence.
    void LayOut();

    // The coefficients and the buffers' lengths, from the current parameters. The buffers fall silent.
    void Configure();

    Library library_;
    FrameFormat format_;
    ReverbParam param_;
    ReverbParam setup_; // the parameters that laid the buffers out
    std::size_t channel_count_ = 2;

    // The buffers of each channel lie one after another: the early reflection, the pre-delay, the two comb filters
    // and the all-pass filter.
    std::vector<int32_t> memory_;
    std::array<std::size_t, 4> early_{};
    std::array<std::size_t, 4> pre_delay_{};
    std::array<std::array<std::size_t, 2>, 4> comb_{};
    std::array<std::size_t, 4> all_pass_{};

    uint32_t early_length_ = 0;
    uint32_t pre_delay_length_ = 0;
    std::array<uint32_t, 2> comb_length_{};
    uint32_t all_pass_length_ = 0;
    uint32_t early_index_ = 0;
    uint32_t pre_delay_index_ = 0;
    std::array<uint32_t, 2> comb_index_{};
    uint32_t all_pass_index_ = 0;

    std::array<int32_t, 2> comb_gain_{};
    int32_t all_pass_gain_ = 0;
    int32_t early_gain_ = 0;
    int32_t fused_gain_ = 0;
    int32_t damping_input_ = 0;    // the damping filter's gain of its input
    int32_t damping_feedback_ = 0; // and of its last output
    std::array<int32_t, 4> damping_state_{};
};

// nw::snd's FxDelay and nn::snd's FxDelay: a delay line on each channel. The line takes the input less the delayed
// sample times the feedback, through a one-pole damping filter. The output is the delayed sample.
class Delay
{
public:
    // Sets the delay up as the library's SetParam and Initialize do, with the lines in silence. Throws
    // std::invalid_argument for parameters that the library refuses, and for a frame format without samples.
    explicit Delay(const DelayParam& param, FrameFormat format = {});

    // Changes the parameters as SetParam does after Initialize. The delay can't grow past the setup's delay, and
    // surround can't start after a setup without it. The lines keep their samples and their position, and the damping
    // filters their states. Returns false, and changes nothing, for parameters that the library refuses.
    bool SetParam(const DelayParam& param);

    // Processes a frame in place: channels 0 and 1, or all four with surround. Throws std::invalid_argument when
    // `samples` isn't the format's length.
    void Process(const Channels& channels, std::size_t samples);

    const DelayParam& Param() const;
    std::size_t ChannelCount() const;

private:
    FrameFormat format_;
    DelayParam param_;
    DelayParam setup_;
    std::size_t setup_frames_ = 1; // each channel's line holds the setup's frames
    std::size_t channel_count_ = 2;
    std::vector<int32_t> memory_;

    uint32_t frames_ = 1;
    uint32_t frame_index_ = 0;
    int32_t feedback_ = 0;
    int32_t damping_input_ = 0;
    int32_t damping_feedback_ = 0;
    std::array<int32_t, 4> damping_state_{};
};

} // namespace ntdlib::ctr_fx
