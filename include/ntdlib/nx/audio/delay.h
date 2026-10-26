// SPDX-License-Identifier: MIT

// The audio renderer's delay effect, after Nintendo's renderer for the CPU. The functions follow
// InitializeDelayEffect (0x226600), SetDelayEffectParameter (0x226790) and ApplyDelayEffect (0x226810) in WarioWare:
// Move It!'s sdk. Each channel has a line as long as the delay. Each sample, the line gives back its oldest sample and
// takes in the input with feedback from the lines, through a one-pole low-pass filter. The arithmetic is in Q14, on
// samples scaled by 64. Each product keeps the low 32 bits of its value shifted right by 14, and each sum wraps.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::nx::audio
{

// The most channels a delay has.
inline constexpr int kDelayChannels = 6;

// nn::audio::DelayParameter as the client sets it, without the buffers: the times in milliseconds, the sample rate and
// the gains in Q14.
struct DelayParameter
{
    int channel_count = 0;
    int max_channel_count = 0;
    int32_t max_delay = 0;
    int32_t delay = 0;
    int32_t sample_rate = 0;
    int32_t in_gain = 0;
    int32_t feedback_gain = 0;
    int32_t wet_gain = 0x4000;
    int32_t dry_gain = 0x4000;
    int32_t channel_spread = 0;
    int32_t low_pass_amount = 0;
};

// nn::audio::DelayState: each channel's line, the cell it reads and writes next, and its filter's last output, with the
// gains that the parameters give.
struct DelayState
{
    std::array<std::vector<int32_t>, kDelayChannels> lines;
    std::array<std::size_t, kDelayChannels> positions{};
    std::array<int32_t, kDelayChannels> low_pass{};
    int32_t feedback = 0;
    int32_t direct = 0;
    int32_t cross = 0;
    int32_t low_pass_in = 0;
    int32_t low_pass_feedback = 0;
};

// Clears each channel's line and filter, and works out the gains (InitializeDelayEffect). A line holds the delay in
// samples, rounded to the nearest, and at most the longest delay. A delay of no samples keeps a line of one cell. The
// console doesn't clear it. The model's cell keeps what it held, or 0 at first.
void InitializeDelay(const DelayParameter& parameter, DelayState& state);

// Works out the gains again and keeps the lines (SetDelayEffectParameter).
void SetDelayGains(const DelayParameter& parameter, DelayState& state);

// Runs `count` samples of each channel from inputs[k] to outputs[k] (ApplyDelayEffect). A channel's input and output
// may be the same buffer. A delay of 3, 5 or more than 6 channels passes its inputs through. With 2 channels, each line
// takes feedback from both lines, the other line's through the spread. With 4 or 6, each line takes feedback from
// itself and from its two neighbours, theirs through half the spread. The neighbours form a ring: front left, front
// right, rear right and rear left, or front left, centre, front right, rear right and rear left. A single channel, and
// the LFE of 6 (the fourth), take feedback only from their line, at the gain the client set. The others scale the
// feedback down first.
void ApplyDelay(const DelayParameter& parameter, DelayState& state, std::span<const int32_t* const> inputs,
                std::span<int32_t* const> outputs, int count);

// Passes `count` samples of each channel from inputs[k] to outputs[k] where they differ. A disabled delay does that.
void PassDelay(const DelayParameter& parameter, std::span<const int32_t* const> inputs,
               std::span<int32_t* const> outputs, int count);

} // namespace ntdlib::nx::audio
