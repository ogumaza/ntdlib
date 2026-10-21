// SPDX-License-Identifier: MIT

// The settings that a wave sound and a stream's track hold as bytes, as the players of nw::snd and nn::atk read them:
// the pan, the surround pan, a send and a filter. A sequence's tracks read their pan and surround pan another way (see
// console.h).

#pragma once

#include <cstdint>

namespace ntdlib::nwsnd
{

// A pan from 0 to 127 as -1 to 1, 64 in the centre: 0 and 1 both give their distance from 63, the others their
// distance from 64. Each is a 63rd.
inline float PanOfSoundValue(uint8_t pan)
{
    const int centred = pan < 2 ? pan - 63 : pan - 64;

    return static_cast<float>(centred) / 63.0f;
}

// A surround pan from 0 to 127 as 0 to 2: up to 63 in 63rds, and from 64 the value plus 1 in 64ths.
inline float SpanOfSoundValue(uint8_t span)
{
    if (span <= 63)
    {
        return static_cast<float>(span) / 63.0f;
    }

    return static_cast<float>(span + 1) * (1.0f / 64.0f);
}

// A send, a volume or the biquad filter's value, from 0 to 127 as 0 to 1. The main send is that less 1.
inline float FractionOfSoundValue(uint8_t value)
{
    return static_cast<float>(value) / 127.0f;
}

} // namespace ntdlib::nwsnd
