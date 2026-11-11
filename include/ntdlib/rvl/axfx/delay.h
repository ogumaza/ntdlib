// SPDX-License-Identifier: MIT

// AXFXDelay and AXFXDelayExp, the RVL SDK's delays for an aux bus, modelled on the games' code. The arithmetic is in
// integers.
//
// AXFXDelay runs each channel through a delay line with feedback and an output gain. The builds of October 2006
// (WarioWare: Smooth Moves) and December 2006 (Wii Sports and Wii Play) have the same code. Comments give Wii Sports'
// addresses, and Smooth Moves' after "October 2006". Wii Play's are Wii Sports' less 0x618.
//
// AXFXDelayExp gives the three channels one delay. A low-pass filter shapes the input that goes into each line, and the
// feedback adds to it. The effect can take another bus's samples as input too, and send its output to a third bus.
// NW4R's FxDelay of 2009 calls it. Comments give Pokémon Rumble's addresses. The game links the build of February 2009.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/rvl/axfx/axfx.h"

namespace ntdlib::rvl::axfx
{

// AXFX_DELAY. A copy of a Delay holds the whole state, the delay lines included.
class Delay
{
public:
    // AXFX_DELAY's parameters for each channel: the delay in ms, and the feedback and the output gain in percent.
    // NW4R's FxDelay::SetParam (0x8003a44c; October 2006, 0x800e2b84) gives the three channels the same values.
    // SetParam holds the delay to 1 to 5,000 ms. It multiplies each gain by 100 in single precision and truncates the
    // product. SetParam then holds the feedback to 99 and the output to 100.
    struct Params
    {
        std::array<uint32_t, kChannelCount> delay{};
        std::array<uint32_t, kChannelCount> feedback{};
        std::array<uint32_t, kChannelCount> output{};
    };

    // AXFXDelayGetMemSize (0x8010974c; October 2006, 0x80032b68): the bytes of the three lines.
    static uint32_t GetMemSize(const Params& params);

    // AXFXDelayInit (0x80109768; October 2006, 0x80032b84): allocates a line of 32 samples for each ms of delay, clears
    // it and converts the gains. Returns false for a delay of 0, a feedback of 100 or more, or an output above 100. The
    // model also returns false for a delay above 524,288 ms. A Wii's memory can't hold such a line. After a failure,
    // the effect stays off. After a successful Init, the next frame passes through unchanged.
    bool Init(const Params& params);

    // AXFXDelaySettings (0x80109968; October 2006, 0x80032d84): frees the lines and runs Init.
    bool Settings(const Params& params);

    // AXFXDelayShutdown (0x80109a8c; October 2006, 0x80032ea8): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXDelayCallback (0x80109b1c; October 2006, 0x80032f38): the frame takes the samples that leave the lines, at
    // the output gain. Each line takes the input plus the leaving sample at the feedback's gain. While the effect is
    // off, the frame stays as it is.
    void Process(AuxFrame& frame);

private:
    // __InitParams (0x80109c9c; October 2006, 0x800330b8).
    bool InitParams();

    // The game's steps that free the lines: the effect goes off first.
    void FreeLines();

    Params params_;
    std::array<std::vector<int32_t>, kChannelCount> line_;
    std::array<uint32_t, kChannelCount> cur_pos_{};
    std::array<uint32_t, kChannelCount> length_{};
    std::array<int32_t, kChannelCount> feedback_gain_{};
    std::array<int32_t, kChannelCount> out_gain_{};

    // AXFX_DELAY's `active` field (see kStopped). Init, Settings and Shutdown set kStopped. A successful Init clears it
    // and sets kSkipFrame.
    uint32_t active_ = kStopped;
};

// AXFX_DELAY_EXP. A copy of a DelayExp holds the whole state, the delay lines included.
class DelayExp
{
public:
    // AXFX_DELAY_EXP's parameters. The structure's busIn and busOut point to another bus's buffers. Process takes those
    // buffers instead.
    struct Params
    {
        float max_delay = 0.0f; // the longest delay in ms, for the lines' length: above 0
        float delay = 0.0f;     // the delay in ms: at most max_delay
        float feedback = 0.0f;  // at least 0 and below 1
        float lpf = 0.0f;       // the low-pass filter: 0 to 1, from the strongest to none
        float out_gain = 0.0f;  // 0 to 1
        float send_gain = 0.0f; // the gain of the output to busOut: 0 to 1
    };

    // AXFXDelayExpGetMemSize (0x802de6c0): the bytes of three lines of the longest delay. The game converts 32 samples
    // for each ms to a signed integer (fctiwz), and multiplies it by 12.
    static uint32_t GetMemSize(const Params& params);

    // AXFXDelayExpInit (0x802de6f0): allocates three lines of 32 samples for each ms of the longest delay, and runs
    // __InitParams. Returns false for a longest delay of 0 or less, or for parameters that __InitParams refuses. The
    // model also returns false for a line above 64 MiB. A Wii's memory can't hold such a line. After a failure, the
    // effect stays off. After a successful Init, the next frame passes through unchanged.
    bool Init(const Params& params);

    // AXFXDelayExpSettings (0x802de9c0): frees the lines and runs Init.
    bool Settings(const Params& params);

    // AXFXDelayExpSettingsUpdate (0x802deb50): runs __InitParams on the lines that the effect has. NW4R's FxDelay calls
    // it while the longest delay stays the same. After a failure, the lines are freed and the effect stays off. After
    // a success, the next frame passes through unchanged.
    bool SettingsUpdate(const Params& params);

    // AXFXDelayExpShutdown (0x802dec60): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXDelayExpCallback (0x802ded10), with busIn and busOut null.
    void Process(AuxFrame& frame);

    // The callback with the buffers of busIn and busOut. A null pointer stands for no bus. A sample of `bus_in` adds to
    // the frame's sample, and the filter takes the sum. Each line takes the filter's output plus the leaving sample at
    // the feedback's gain. The frame takes the leaving samples at the output gain, and `bus_out` at the send gain.
    // While the effect is off, the frame stays as it is.
    void Process(AuxFrame& frame, const AuxFrame* bus_in, AuxFrame* bus_out);

private:
    // __InitParams (0x802def90): checks the parameters, clears the lines and the filter, and converts the delay and the
    // gains.
    bool InitParams();

    // The game's steps that free the lines: the effect goes off first.
    void FreeLines();

    Params params_;
    std::array<std::vector<int32_t>, kChannelCount> line_;
    uint32_t cur_pos_ = 0;
    uint32_t length_ = 0;     // the delay in samples
    uint32_t max_length_ = 0; // the lines' length in samples
    int32_t feedback_gain_ = 0;
    std::array<int32_t, kChannelCount> lpf_state_{};
    int32_t lpf_gain_ = 0; // the weight of the filter's last output, in 128ths
    int32_t out_gain_ = 0;
    int32_t send_gain_ = 0;

    // AXFX_DELAY_EXP's `active` field (see kStopped). Init sets kStopped and clears kSkipFrame. Settings,
    // SettingsUpdate and Shutdown set kStopped. Each successful Init, Settings or SettingsUpdate clears it and sets
    // kSkipFrame.
    uint32_t active_ = kStopped;
};

} // namespace ntdlib::rvl::axfx
