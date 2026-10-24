// SPDX-License-Identifier: MIT

// AXFXChorusExp and AXFXChorus, the RVL SDK's chorus for an aux bus, modelled on the games' code. ChorusExp delays each
// channel by a time that a sine LFO varies. A 4-tap interpolator reads the delayed signal. Chorus is the wrapper that
// NW4R's FxChorus calls. Its parameters are a delay, a variation and the LFO's period, in ms. The wrapper runs
// ChorusExp with no feedback and no other buses. The builds of October 2006 (WarioWare: Smooth Moves) and December 2006
// (Wii Sports and Wii Play) have the same code. Comments give Wii Sports' addresses, and Smooth Moves' after "October
// 2006". Wii Play's are Wii Sports' less 0x618.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/rvl/axfx/axfx.h"

namespace ntdlib::rvl::axfx
{

// AXFX_CHORUS_EXP. A copy of a ChorusExp holds the whole state, the delay lines included.
class ChorusExp
{
public:
    // AXFX_CHORUS_EXP's parameters. The structure's busIn and busOut point to another bus's buffers. Process takes
    // those buffers instead.
    struct Params
    {
        float delay_time = 0.0f; // the delay in ms: 0.1 to 50
        float depth = 0.0f;      // the LFO's swing over the delay: 0 to 1
        float rate = 0.0f;       // the LFO's frequency in Hz: 0.1 to 2
        float feedback = 0.0f;   // at least 0 and below 1
        float out_gain = 0.0f;   // 0 to 1
        float send_gain = 0.0f;  // the gain of the output to busOut: 0 to 1
    };

    // AXFXChorusExpGetMemSize (0x80109f44; October 2006, 0x80034098): the bytes of the three lines.
    static uint32_t GetMemSize();

    // AXFXChorusExpInit (0x80109f50; October 2006, 0x800340a4): allocates and clears three lines of 3,200 samples
    // (100 ms), and starts the LFO. Returns false for a parameter outside its range. The effect then stays off.
    bool Init(const Params& params);

    // AXFXChorusExpSettings (0x8010a1ac; October 2006, 0x80034300): frees the lines and runs Init. After a successful
    // Settings, the next frame passes through unchanged.
    bool Settings(const Params& params);

    // AXFXChorusExpShutdown (0x8010a2d4; October 2006, 0x80034428): frees the lines and turns the effect off.
    void Shutdown();

    // AXFXChorusExpCallback (0x8010a364; October 2006, 0x800344b8), with busIn and busOut null.
    void Process(AuxFrame& frame);

    // The callback with the buffers of busIn and busOut. A null pointer stands for no bus. A sample of `bus_in` adds to
    // the input. The line takes the input plus the output at the feedback's gain. The frame takes the output at the
    // output gain, and `bus_out` at the send gain. While the effect is off, the frame stays as it is.
    void Process(AuxFrame& frame, const AuxFrame* bus_in, AuxFrame* bus_out);

private:
    // AXFX_CHORUS_EXP_LFO: the LFO's step, depth, phase and interpolation, in 16.16 fixed point. The game also stores
    // the table's address, and a step in samples (stepSamp) that nothing reads.
    struct Lfo
    {
        int32_t phase_add = 0;
        int32_t depth_samp = 0;
        uint32_t phase = 0;
        uint32_t sign = 0;
        uint32_t last_num = 0;
        int32_t last_value = 0;
        int32_t grad = 0;
        int32_t grad_factor = 0;
    };

    // __InitParams (0x8010a6c4; October 2006, 0x80034818).
    bool InitParams();

    // The game's steps that free the lines: the effect goes off first.
    void FreeLines();

    // __CalcLFO (0x8010a8bc; October 2006, 0x80034a10): the LFO's offsets of the read position for a frame's samples.
    std::array<int32_t, kSamplesPerFrame> CalcLfo();

    Params params_;
    std::array<std::vector<float>, kChannelCount> line_;
    uint32_t in_pos_ = 0;
    uint32_t out_pos_ = 0;
    uint32_t last_pos_ = 0;
    uint32_t size_fp_ = 0;
    uint32_t size_ = 0;
    Lfo lfo_;
    std::array<std::array<float, 4>, kChannelCount> history_{};
    uint32_t hist_index_ = 0;

    // AXFX_CHORUS_EXP's `active` field (see kStopped). Init, Settings and Shutdown set kStopped. A successful Init
    // clears it. A successful Settings sets kSkipFrame.
    uint32_t active_ = kStopped;
};

// AXFX_CHORUS: ChorusExp with its parameters given as times in ms.
class Chorus
{
public:
    // AXFX_CHORUS's parameters, in ms. NW4R's FxChorus::SetParam (0x80039f54; October 2006, 0x800e26d8) derives them
    // from a delay time in ms, a depth and a rate in Hz. SetParam computes each in single precision and truncates it to
    // an integer. The base delay is the delay time, held to 1 to 50. The variation is the delay time times the depth,
    // held to 50. The period is 1,000 divided by the rate, held to 500 to 10,000.
    struct Params
    {
        uint32_t base_delay = 0;
        uint32_t variation = 0;
        uint32_t period = 0;
    };

    // AXFXChorusGetMemSize (0x80109de0; October 2006, 0x80033f34): ChorusExp's.
    static uint32_t GetMemSize();

    // AXFXChorusInit (0x80109de4; October 2006, 0x80033f38): ChorusExp's Init with a delay time of `base_delay`, a
    // depth of `variation` / `base_delay` and a rate of 1,000 / `period`. The feedback and the send gain are 0, the
    // output gain 1, and busIn and busOut null.
    bool Init(const Params& params);

    // AXFXChorusSettings (0x80109ea4; October 2006, 0x80033ff8): ChorusExp's Settings with those parameters.
    bool Settings(const Params& params);

    // AXFXChorusShutdown (0x80109e80; October 2006, 0x80033fd4): ChorusExp's Shutdown.
    void Shutdown();

    // AXFXChorusCallback (0x80109f40; October 2006, 0x80034094): ChorusExp's callback.
    void Process(AuxFrame& frame);

private:
    ChorusExp exp_;
};

} // namespace ntdlib::rvl::axfx
