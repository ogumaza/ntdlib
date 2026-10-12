// SPDX-License-Identifier: MIT

// Where NW4R SND places each channel of a stereo wave on the Wii.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ntdlib::sequence_export
{

// The level of a stereo wave's channels in balance pan mode at a pan of 0: -3 dB (see WaveChannelPan). The SoundFont
// scales their samples by this level. Every synth then plays them at the game's level.
constexpr double kBalanceScale = 0.70710678118654752440;
constexpr double kBalanceDb = -3.0102999566398120; // 20 log10(kBalanceScale)

// A wave channel's place in NW4R SND's stereo mix: its pan, -1 (left) to 1 (right), and its gain in dB.
struct ChannelPan
{
    double pan = 0.0;
    double gain_db = 0.0;
};

// Returns the place of channel `channel` of a wave with `channels` channels, for a note at pan `pan`
// (Voice::CalcMixParam, main.dol 0x8004d590). A mono wave plays at `pan`. In dual mode, each channel of a stereo wave
// pans from its side. In balance mode, each channel stays on its side, at the level the pan law gives that side: -3 dB
// at a pan of 0. Dual mode plays the channel at full level there.
inline ChannelPan WaveChannelPan(double pan, std::size_t channel, std::size_t channels, bool balance)
{
    if (channels < 2)
    {
        return {pan, 0.0};
    }

    const double side = channel == 0 ? -1.0 : 1.0;
    if (!balance)
    {
        return {pan + side, 0.0};
    }

    const double level = (1.0 + side * std::clamp(pan, -1.0, 1.0)) / 2.0;
    return {side, level > 0.0 ? 10.0 * std::log10(level) : -144.0};
}

} // namespace ntdlib::sequence_export
