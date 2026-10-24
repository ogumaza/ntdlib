// SPDX-License-Identifier: MIT

// AXFXDelay, the RVL SDK's delay for an aux bus, modelled on the games' code. Each channel runs through a delay line
// with feedback and an output gain. The arithmetic is in integers. The builds of October 2006 (WarioWare: Smooth Moves)
// and December 2006 (Wii Sports and Wii Play) have the same code. Comments give Wii Sports' addresses, and Smooth
// Moves' after "October 2006". Wii Play's are Wii Sports' less 0x618.

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

} // namespace ntdlib::rvl::axfx
