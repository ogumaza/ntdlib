// SPDX-License-Identifier: MIT

// The audio renderer's reverb effect, after Nintendo's renderer for the CPU. The functions follow
// InitializeReverbEffect (0x228120), UpdateReverbEffectParameter (0x228480) and ApplyReverbEffect (0x229680) in
// WarioWare: Move It!'s sdk. The input goes into a pre-delay line. Ten taps of that line give the early reflections.
// A later tap feeds a network of four feedback lines, each through an all-pass on a shorter line, and the lines
// return through a filter that makes the reverb die away in the decay time. The arithmetic is in Q14, on samples
// scaled by 64. Each product keeps the low 32 bits of its value shifted right by 14, and each sum wraps.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::nx::audio
{

// The most channels a reverb has.
inline constexpr int kReverbChannels = 6;

// The reverb's lines: four feedback lines and their four all-passes.
inline constexpr int kReverbLines = 4;

// The early reflections' taps.
inline constexpr int kReverbTaps = 10;

// The early modes and the late modes there are.
inline constexpr int kReverbModes = 5;

// nn::audio::ReverbParameter as the client sets it, without the buffers: the sample rate in kHz in Q14, the modes,
// the pre-delay in ms in Q14, the decay time in s in Q14, and the gains and ratios in Q14. The early mode picks a
// room, a chamber, a hall, a cathedral or no early reflections (0 to 4). The late mode picks a room, a hall, a plate, a
// cathedral or the longest lines (0 to 4).
struct ReverbParameter
{
    int channel_count = 0;
    int max_channel_count = 0;
    int32_t sample_rate = 0;
    int32_t early_mode = 4;
    int32_t early_gain = 0;
    int32_t predelay = 0;
    int32_t late_mode = 0;
    int32_t late_gain = 0;
    int32_t decay_time = 1638;
    int32_t high_frequency_decay_ratio = 1638;
    int32_t coloration = 0;
    int32_t reverb_gain = 0;
    int32_t out_gain = 0;
    int32_t dry_gain = 0x4000;
};

// A line in the reverb's buffer: the cells the buffer gives it, the cells it uses now, its first cell, and the cell it
// reads and writes next.
struct ReverbLine
{
    uint32_t most = 0;
    uint32_t length = 0;
    int64_t start = 0;
    int64_t cursor = 0;
};

// nn::audio::ReverbState: the lines in one buffer, the early reflections' delays and gains, the late reverb's delay,
// and each feedback line's gains and its filter's last output.
struct ReverbState
{
    std::vector<int32_t> buffer;
    std::array<ReverbLine, kReverbLines> fdn;
    std::array<ReverbLine, kReverbLines> all_pass;
    ReverbLine predelay;
    ReverbLine center;
    std::array<int32_t, kReverbTaps> early_delays{}; // each tap's delay, plus 1
    std::array<int32_t, kReverbTaps> early_gains{};
    int32_t late_delay = 0;
    int32_t all_pass_gain = 0;
    std::array<int32_t, kReverbLines> fdn_gains{};
    std::array<int32_t, kReverbLines> filter_gains{};
    std::array<int32_t, kReverbLines> filtered{};
};

// Lays the lines out in the buffer, at their longest for the sample rate, then works out the delays and gains, and
// clears the cells that the lines use (InitializeReverbEffect). The pre-delay line holds 350 ms with `long_predelay`.
// Without it, the line holds 150 ms, the length before the renderer's revision 3. The cells beyond a line's length keep
// what they held.
void InitializeReverb(const ReverbParameter& parameter, ReverbState& state, bool long_predelay);

// Works out the delays and gains again, keeps the lines' samples, and starts each feedback line and all-pass again at
// its first cell (UpdateReverbEffectParameter). The modes set the lengths of the lines.
void UpdateReverb(const ReverbParameter& parameter, ReverbState& state);

// Runs `count` samples of each channel from inputs[k] to outputs[k] (ApplyReverbEffect). A channel's input and output
// may be the same buffer. A reverb of 0, 3, 5 or more than 6 channels passes its inputs through.
void ApplyReverb(const ReverbParameter& parameter, ReverbState& state, std::span<const int32_t* const> inputs,
                 std::span<int32_t* const> outputs, int count);

// Passes `count` samples of each channel from inputs[k] to outputs[k] where they differ. A disabled reverb does that.
void PassReverb(const ReverbParameter& parameter, std::span<const int32_t* const> inputs,
                std::span<int32_t* const> outputs, int count);

// nn::audio::ReverbParameterSet: a reverb's parameters as a client gives them. The pre-delay is in ms and the decay
// time in seconds.
struct ReverbParameterSet
{
    int32_t early_mode = 0;
    float early_gain = 0.0f;
    float predelay = 0.0f;
    int32_t late_mode = 0;
    float late_gain = 0.0f;
    float decay_time = 0.0f;
    float high_frequency_decay_ratio = 0.0f;
    float coloration = 0.0f;
    float reverb_gain = 0.0f;
    float out_gain = 0.0f;
    float dry_gain = 0.0f;
};

// A reverb that a game runs on the CPU over an aux bus's samples (nn::audio::AuxReverbType and its buffer). This
// follows the client in Skyward Sword HD's sdk (NintendoSDK 11.4.0).
class AuxReverb
{
public:
    // InitializeAuxReverb (0x3bd00): `channels` channels at `sample_rate`, every parameter 0, and every channel on
    // block 0. The reverb sets up at the first Process.
    AuxReverb(int32_t sample_rate, int channels);

    // SetAuxReverbInputOutput (0x3bff0): the block that each channel takes and gives. The channel count becomes
    // `inputs.size()`, held to the channels that the reverb was made with and to 6. The reverb sets up again at the
    // next Process. `outputs` must be as long as `inputs`.
    void SetInputOutput(std::span<const int8_t> inputs, std::span<const int8_t> outputs);

    // SetAuxReverbParameters (0x3be40): each value in Q14, towards zero. A reverb that has run works out its delays
    // and gains again at the next Process. A mode above 4 throws std::invalid_argument. Nintendo's code stops there.
    void SetParameters(const ReverbParameterSet& parameters);

    // ResetAuxReverb (0x3c0a0): the reverb sets up again at the next Process.
    void Reset();

    // ProcessAuxReverb (0x3c0b0): `sample_count` samples of each channel. Channel k takes its samples at input +
    // its block × sample_count and gives them at output + its block × sample_count. The output may be the input.
    void Process(int32_t* output, const int32_t* input, uint32_t sample_count);

private:
    ReverbParameter parameter_;
    std::array<int8_t, kReverbChannels> inputs_{};
    std::array<int8_t, kReverbChannels> outputs_{};
    uint8_t status_ = 0; // 0 set up, 1 work out the delays and gains again, 2 ready
    ReverbState state_;
};

} // namespace ntdlib::nx::audio
