// SPDX-License-Identifier: MIT

// AXFX's high-quality reverb, modelled on the games' code. AXFXReverbHiExp does the work. AXFXReverbHi wraps it with
// fewer parameters. NW4R SND's FxReverbHi of 2006 and 2007 calls the wrapper. FxReverbHi of 2008 fills the Exp
// reverb's parameters directly.
//
// Each channel runs the same network: early reflections from three taps of an early line, a pre-delay, three combs in
// parallel, two allpasses, a low-pass and a last allpass. The crosstalk then mixes the other two channels into each
// channel's output. The builds of 2006 never write the early line. Its taps read the zeros that Init leaves there. The
// build of September 2008 writes each input sample to it. In everything else the three builds agree.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/rvl/axfx/axfx.h"

namespace ntdlib::rvl::axfx
{

// AXFXReverbHiExp (0x80107820 to 0x8010874c in Wii Sports, 0x80031c38 to 0x80032b64 in Smooth Moves and 0x80377c94
// to 0x80378c78 in Animal Crossing).
class ReverbHiExp
{
public:
    // The parameters in AXFX_REVERBHI_EXP, by the game's names. The model leaves out bus_in and bus_out. They mix the
    // reverb with other buses. Every game sets them to null. send_gain only scales the output to bus_out. Init still
    // checks its range.
    struct Params
    {
        uint32_t early_mode = 0;
        float pre_delay_time_max = 0.0f;
        float pre_delay_time = 0.0f;
        uint32_t fused_mode = 0;
        float fused_time = 0.0f;
        float coloration = 0.0f;
        float damping = 0.0f;
        float crosstalk = 0.0f;
        float early_gain = 0.0f;
        float fused_gain = 0.0f;
        float out_gain = 0.0f;
        float send_gain = 0.0f;
    };

    // A reverb that hasn't started. Process leaves frames as they are until Init succeeds.
    explicit ReverbHiExp(Build build);

    // The bytes that Init allocates for `params` (AXFXReverbHiExpGetMemSize, 0x80107820).
    static uint32_t GetMemSize(const Params& params);

    // Allocates the delay lines, clears them and sets the reverb up for `params` (AXFXReverbHiExpInit, 0x801078a8).
    // Returns false if a parameter is out of range. Process then leaves frames as they are until an Init or Settings
    // succeeds.
    bool Init(const Params& params);

    // Sets the reverb up from the start for `params` (AXFXReverbHiExpSettings, 0x80107a44). Settings frees the delay
    // lines and calls Init. After it succeeds, Process leaves the next frame as it is.
    bool Settings(const Params& params);

    // Clears the delay lines and sets the reverb up for `params` without allocating the lines again
    // (AXFXReverbHiExpSettingsUpdate, 0x80377f88 in Animal Crossing). Only the September 2008 build has it. NW4R SND's
    // FxReverbHi::SetParam of 2008 calls it when pre_delay_time_max stays the same. After it succeeds, Process leaves
    // the next frame as it is.
    bool SettingsUpdate(const Params& params);

    // Frees the delay lines (AXFXReverbHiExpShutdown, 0x80107b14). Process then leaves frames as they are.
    void Shutdown();

    // Runs one frame of the bus through the reverb, in place (AXFXReverbHiExpCallback, 0x80107b68 in Wii Sports and
    // 0x80378090 in Animal Crossing).
    void Process(AuxFrame& frame);

private:
    static constexpr int kTaps = 3;
    static constexpr int kCombs = 3;
    static constexpr int kAllpasses = 2;

    // One sample of one channel's network, before the crosstalk.
    float ProcessChannel(int channel, float input, float fused_gain, float lowpass_gain);

    // Moves each line's position on by a sample.
    void AdvancePositions();

    // Sets the positions, lengths and coefficients for params_ (__InitParams, 0x8010842c). Returns false if a
    // parameter is out of range.
    bool InitParams();

    // __BzeroDelayLines (0x801081f4) and __FreeDelayLine (0x80108318).
    void ClearDelayLines();
    void FreeDelayLines();

    // Calls `function` for every delay line.
    void ForEachLine(void (*function)(std::vector<float>& line));

    Build build_;
    Params params_;

    // The game's `active` field (see kStopped). Init sets kStopped. A successful Init clears the bit again. Settings
    // sets kSkipFrame.
    uint32_t active_ = kStopped;

    // The early reflections. The model allocates the line for the September 2008 build only. The others never write
    // it.
    std::array<std::vector<float>, kChannelCount> early_line_;
    std::array<uint32_t, kTaps> early_pos_{};
    uint32_t early_length_ = 0;
    std::array<float, kTaps> early_coef_{};

    // The pre-delay. A length of 0 leaves the input undelayed.
    std::array<std::vector<float>, kChannelCount> pre_delay_line_;
    uint32_t pre_delay_pos_ = 0;
    uint32_t pre_delay_length_ = 0;

    // The combs, by channel and comb.
    std::array<std::array<std::vector<float>, kCombs>, kChannelCount> comb_line_;
    std::array<uint32_t, kCombs> comb_pos_{};
    std::array<uint32_t, kCombs> comb_length_{};
    std::array<float, kCombs> comb_coef_{};

    // The two allpasses after the combs, by channel and allpass, and the last allpass of each channel.
    std::array<std::array<std::vector<float>, kAllpasses>, kChannelCount> allpass_line_;
    std::array<uint32_t, kAllpasses> allpass_pos_{};
    std::array<uint32_t, kAllpasses> allpass_length_{};
    std::array<std::vector<float>, kChannelCount> last_allpass_line_;
    std::array<uint32_t, kChannelCount> last_allpass_pos_{};
    std::array<uint32_t, kChannelCount> last_allpass_length_{};
    float allpass_coef_ = 0.0f;

    // The low-pass between the allpasses.
    std::array<float, kChannelCount> last_lpf_out_{};
    float lpf_coef_ = 0.0f;
};

// AXFXReverbHi (0x801075e8 to 0x801076e0 in Wii Sports): the Exp reverb with six parameters. Init and Settings
// convert them to the Exp reverb's parameters (__ParamConvert, inlined in both).
class ReverbHi
{
public:
    // The parameters in AXFX_REVERBHI, by the game's names.
    struct Params
    {
        float coloration = 0.0f;
        float mix = 0.0f;
        float time = 0.0f;
        float damping = 0.0f;
        float pre_delay = 0.0f;
        float crosstalk = 0.0f;
    };

    explicit ReverbHi(Build build);

    // AXFXReverbHiGetMemSize (0x801075e8).
    static uint32_t GetMemSize(const Params& params);

    // AXFXReverbHiInit (0x801075f4), AXFXReverbHiSettings (0x8010767c), AXFXReverbHiShutdown (0x80107658) and
    // AXFXReverbHiCallback (0x801076e0). Each calls the matching function of the Exp reverb.
    bool Init(const Params& params);
    bool Settings(const Params& params);
    void Shutdown();
    void Process(AuxFrame& frame);

private:
    // __ParamConvert: an Exp reverb in early mode 5 with no early reflections, fused mode 0 and full fused gain.
    static ReverbHiExp::Params Convert(const Params& params);

    ReverbHiExp exp_;
};

} // namespace ntdlib::rvl::axfx
