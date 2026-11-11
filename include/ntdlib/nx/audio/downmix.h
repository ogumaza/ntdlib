// SPDX-License-Identifier: MIT

// The Switch's downmix of the 6 channels of a device to the 2 of a stereo output. The audio system module runs it after
// the renderer, not the game. This follows the module of firmware 22.5.0. The read-only data of the module contains the
// program of the audio DSP at 0xc2470 (0x82dc8 bytes, linked at 0x80500000). Addresses below 0x80500000 are in the
// module. The others are in the DSP program.
//
// The DSP's global mixer mixes every session into 6 channels, the renderer session included (0x805019c0). Its output
// stage (0x80502236) scales each channel by the device's volume. For a 2-channel device, the stage then calls one of
// two downmix functions, from the pointers at 0x80552af0 (the main output) and 0x80552af4. The module sets them through
// message 3 to the DSP (0x80500c3c). For the main output, 0x683d0 picks the mono downmix when the output's mode is
// mono, and the stereo downmix otherwise. The speaker, the headphones and a TV in stereo therefore all play the stereo
// downmix. 0x66dd0 sets the other pointer to the stereo downmix too. A TV with 6 channels plays them without a downmix.
//
// The channels come in the order of a 6-channel device sink: front left, front right, centre, low frequency, rear left
// and rear right. Each function drops the low-frequency channel. At full device volume, the downmix uses the renderer
// samples unchanged.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::nx::audio
{

enum class Downmix : uint8_t
{
    // 0x80501930: L = FL + C × g + RL × g and R = FR + C × g + RR × g, with g = 0xb53c / 2^16 (-3.0 dB). Each product
    // is rounded down. Each sum is saturated to 16 bits.
    kStereo,

    // 0x805017e2: the stereo downmix's two sums, each saturated to 16 bits, added and halved, rounded down. Both
    // channels play the result.
    kMono,

    // 0x8050188a: the stereo downmix's sums times 0x6007 / 2^16, rounded down and saturated to 16 bits. The table in
    // the DSP program contains this function. Nothing in the module selects it.
    kScaled,

    // No downmix: the front pair only. The centre and the rear channels are left out.
    kFrontPair,
};

// Downmixes one sample of the 6 channels, in the order above, to left and right.
std::array<int16_t, 2> DownmixSample(std::span<const int16_t, 6> channels, Downmix downmix);

// Downmixes `input`, interleaved samples of the 6 channels, and appends them to `output` as interleaved stereo.
void DownmixFrame(std::span<const int16_t> input, Downmix downmix, std::vector<int16_t>& output);

} // namespace ntdlib::nx::audio
