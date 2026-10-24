// SPDX-License-Identifier: MIT

// AXFX's standard reverb in the build of October 2006, modelled on WarioWare: Smooth Moves' code. AXFXReverbStdExp
// does the work. AXFXReverbStd wraps it with fewer parameters. The build of September 2008 (Mario Kart Wii and Wii
// Music) has the same Exp reverb, with SettingsUpdate besides. Smooth Moves' NW4R SND has a class FxReverbStd that
// calls the wrapper (SetParam at 0x800e33ac). The game puts the class on bus A. The parameters come from a table of 10
// rows at 0x804da11c. The music's sequence picks the row. For each new row, the game first calls SetParam. Next the
// game takes the effect off the bus and appends it again (0x80100e40). With the effect on the bus, these steps call
// Settings, Shutdown and Init in turn. The reverb then starts again from silence with the new row's parameters.
//
// Each channel runs the same network: an early line with feedback, a pre-delay, two combs in parallel, an allpass, a
// low-pass and a second allpass. The standard reverb has no crosstalk between the channels.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/rvl/axfx/axfx.h"

namespace ntdlib::rvl::axfx
{

// AXFXReverbStdExp (0x800332e8 to 0x80033f30 in Smooth Moves).
class ReverbStdExp
{
public:
    // The parameters in the reverb's structure, named as the same fields in AXFX_REVERBHI_EXP are. That structure has
    // a crosstalk besides. The model leaves out bus_in and bus_out. They mix the reverb with other buses. The game sets
    // them to null. send_gain only scales the output to bus_out. Init still checks its range.
    struct Params
    {
        uint32_t early_mode = 0;
        float pre_delay_time_max = 0.0f;
        float pre_delay_time = 0.0f;
        uint32_t fused_mode = 0;
        float fused_time = 0.0f;
        float coloration = 0.0f;
        float damping = 0.0f;
        float early_gain = 0.0f;
        float fused_gain = 0.0f;
        float out_gain = 0.0f;
        float send_gain = 0.0f;
    };

    // The bytes that Init allocates for `params` (AXFXReverbStdExpGetMemSize, 0x800332e8).
    static uint32_t GetMemSize(const Params& params);

    // Allocates the delay lines, clears them and sets the reverb up for `params` (AXFXReverbStdExpInit, 0x80033348).
    // Returns false if a parameter is out of range. Process then leaves frames as they are until an Init or Settings
    // succeeds. A reverb that hasn't started leaves them as they are too.
    bool Init(const Params& params);

    // Sets the reverb up from the start for `params` (AXFXReverbStdExpSettings, 0x800334c4). Settings frees the delay
    // lines and calls Init. After it succeeds, Process leaves the next frame as it is.
    bool Settings(const Params& params);

    // Clears the delay lines and sets the reverb up for `params` without allocating the lines again
    // (AXFXReverbStdExpSettingsUpdate, 0x8012b688 in Mario Kart Wii). Only the September 2008 build has it. NW4R SND's
    // FxReverbStd of 2008 calls it when pre_delay_time_max stays the same. After it succeeds, Process leaves the next
    // frame as it is.
    bool SettingsUpdate(const Params& params);

    // Frees the delay lines (AXFXReverbStdExpShutdown, 0x80033594). Process then leaves frames as they are.
    void Shutdown();

    // Runs one frame of the bus through the reverb, in place (AXFXReverbStdExpCallback, 0x800335e8).
    void Process(AuxFrame& frame);

private:
    static constexpr int kCombs = 2;
    static constexpr int kAllpasses = 2;

    // One sample of one channel's network.
    float ProcessChannel(int channel, float input, float early_gain, float fused_gain, float lowpass_gain);

    // Moves each line's position on by a sample.
    void AdvancePositions();

    // Sets the positions, lengths and coefficients for params_ (__InitParams, 0x80033c88). Returns false if a
    // parameter is out of range.
    bool InitParams();

    // __BzeroDelayLines (0x80033a98) and __FreeDelayLine (0x80033b98).
    void ClearDelayLines();
    void FreeDelayLines();

    // Calls `function` for every delay line.
    void ForEachLine(void (*function)(std::vector<float>& line));

    Params params_;

    // The game's `active` field (see kStopped). The class sets and clears the bits as ReverbHiExp does.
    uint32_t active_ = kStopped;

    // The early line. It feeds back into itself.
    std::array<std::vector<float>, kChannelCount> early_line_;
    uint32_t early_pos_ = 0;
    uint32_t early_length_ = 0;
    float early_coef_ = 0.0f;

    // The pre-delay. A length of 0 leaves the input undelayed.
    std::array<std::vector<float>, kChannelCount> pre_delay_line_;
    uint32_t pre_delay_pos_ = 0;
    uint32_t pre_delay_length_ = 0;

    // The combs, by channel and comb.
    std::array<std::array<std::vector<float>, kCombs>, kChannelCount> comb_line_;
    std::array<uint32_t, kCombs> comb_pos_{};
    std::array<uint32_t, kCombs> comb_length_{};
    std::array<float, kCombs> comb_coef_{};

    // The allpasses, by channel and allpass.
    std::array<std::array<std::vector<float>, kAllpasses>, kChannelCount> allpass_line_;
    std::array<uint32_t, kAllpasses> allpass_pos_{};
    std::array<uint32_t, kAllpasses> allpass_length_{};
    float allpass_coef_ = 0.0f;

    // The low-pass between the allpasses.
    std::array<float, kChannelCount> last_lpf_out_{};
    float lpf_coef_ = 0.0f;
};

// AXFXReverbStd (0x800331fc to 0x800332e4 in Smooth Moves): the Exp reverb with five parameters. Init and Settings
// convert them to the Exp reverb's parameters. AXFXReverbHi converts its parameters in the same way.
class ReverbStd
{
public:
    // The parameters in the wrapper's structure, named as the same fields in AXFX_REVERBHI are.
    struct Params
    {
        float coloration = 0.0f;
        float mix = 0.0f;
        float time = 0.0f;
        float damping = 0.0f;
        float pre_delay = 0.0f;
    };

    // AXFXReverbStdGetMemSize (0x800331fc).
    static uint32_t GetMemSize(const Params& params);

    // AXFXReverbStdInit (0x80033208), AXFXReverbStdSettings (0x80033288), AXFXReverbStdShutdown (0x80033264) and
    // AXFXReverbStdCallback (0x800332e4). Each calls the matching function of the Exp reverb.
    bool Init(const Params& params);
    bool Settings(const Params& params);
    void Shutdown();
    void Process(AuxFrame& frame);

private:
    // An Exp reverb in early mode 5 with no early reflections, fused mode 0 and full fused gain.
    static ReverbStdExp::Params Convert(const Params& params);

    ReverbStdExp exp_;
};

} // namespace ntdlib::rvl::axfx
