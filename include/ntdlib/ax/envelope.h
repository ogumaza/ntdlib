// SPDX-License-Identifier: MIT

// The volume envelope of AX's microcode on the Wii and the Wii U: a gain that starts at the voice's volume and moves by
// the delta each sample. Each pair of samples steps by twice the delta, a 16-bit signed value. The gain is unsigned.

#pragma once

#include <cstdint>

namespace ntdlib::ax
{

// The envelope's step over a pair of samples: twice the delta, a 16-bit signed value.
inline int16_t EnvelopeStep(uint16_t delta)
{
    return static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
}

// The envelope's gain at sample `i` of a frame that starts at `volume`.
inline uint16_t EnvelopeGain(uint16_t volume, uint16_t delta, int32_t i)
{
    const uint32_t start = i % 2 == 0 ? volume : volume + uint32_t{delta};
    return static_cast<uint16_t>(start + static_cast<uint32_t>((i / 2) * EnvelopeStep(delta)));
}

// The envelope's volume after a frame of `samples` samples that starts at `volume`.
inline uint16_t EnvelopeVolumeAfter(uint16_t volume, uint16_t delta, int32_t samples)
{
    return static_cast<uint16_t>(volume + static_cast<uint32_t>((samples / 2) * EnvelopeStep(delta)));
}

} // namespace ntdlib::ax
