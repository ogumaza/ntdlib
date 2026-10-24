// SPDX-License-Identifier: MIT

// Decoders for the ADPCM formats of Nintendo's consoles: DSP-ADPCM (GameCube, Wii, Wii U, 3DS and Switch), the 3DS's
// IMA-ADPCM and the GameCube's AFC. Each decoder stops where its data ends. A caller that requires every sample checks
// the length of the result.

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::wave
{

// How a DSP-ADPCM decoder adds up a sample. The forms give the same results unless the sum passes 32 bits or a frame's
// scale is 2^15. No real wave does either.
enum class DspAdpcmSum
{
    // In 64 bits, then clamped to 16 bits.
    kWide,

    // In 32 bits that wrap, then clamped. AX on the Wii U and the Switch's audio renderer add up this way.
    kWrap,

    // As NW4R SND's decoder on the CPU does. The scale is a 16-bit value: 2^15 becomes -2^15. The sums wrap at 32 bits.
    // The rounding shifts by 10, adds 1 and shifts by 1. It can't wrap.
    kNw4rCpu,

    // As the DSP's accelerator on the GameCube, the Wii and the Wii U does. The prediction's sum, with its rounding,
    // wraps at 32 bits and is shifted. The nibble's term is added after the shift. Only a sum that wraps distinguishes
    // it from kWrap.
    kAccelerator,
};

// The value of a 4-bit two's complement nibble (0 to 15): -8 to 7.
inline int32_t SignedNibble(int32_t nibble)
{
    return nibble >= 8 ? nibble - 16 : nibble;
}

// One DSP-ADPCM sample from a signed nibble (-8 to 7), the frame's scale as a shift (0 to 15), the predictor's
// coefficients and the two samples before it. The function is inline: a renderer's decoder calls it for every sample.
inline int16_t DspAdpcmSample(int32_t nibble, int32_t shift, int32_t c1, int32_t c2, int32_t hist1, int32_t hist2,
                              DspAdpcmSum sum)
{
    switch (sum)
    {
    case DspAdpcmSum::kWide:
        {
            // Use 64 bits: the sum of two coefficient/sample products can overflow 32 bits.
            const int32_t scale = 1 << shift;
            const int64_t v = (int64_t{nibble} * scale << 11) + 1024 + int64_t{c1} * hist1 + int64_t{c2} * hist2;
            return static_cast<int16_t>(std::clamp<int64_t>(v >> 11, -32768, 32767));
        }

    case DspAdpcmSum::kWrap:
        {
            // The sums wrap at 32 bits. A right shift then rounds toward minus infinity.
            const uint32_t scale = uint32_t{1} << shift;
            const uint32_t v = static_cast<uint32_t>(nibble) * 2048 * scale +
                               static_cast<uint32_t>(c1) * static_cast<uint32_t>(hist1) +
                               static_cast<uint32_t>(c2) * static_cast<uint32_t>(hist2) + 1024;
            return static_cast<int16_t>(std::clamp(static_cast<int32_t>(v) >> 11, -32768, 32767));
        }

    case DspAdpcmSum::kNw4rCpu:
        {
            const int32_t scale = static_cast<int16_t>(1 << shift);
            const uint32_t v = static_cast<uint32_t>(c1) * static_cast<uint32_t>(hist1) +
                               static_cast<uint32_t>(c2) * static_cast<uint32_t>(hist2) +
                               static_cast<uint32_t>(scale * nibble * 2048);
            const int32_t rounded = ((static_cast<int32_t>(v) >> 10) + 1) >> 1;
            return static_cast<int16_t>(std::clamp(rounded, -32768, 32767));
        }

    case DspAdpcmSum::kAccelerator:
        {
            const uint32_t prediction = uint32_t{0x400} + static_cast<uint32_t>(c1) * static_cast<uint32_t>(hist1) +
                                        static_cast<uint32_t>(c2) * static_cast<uint32_t>(hist2);
            const int32_t value = nibble * (1 << shift) + (static_cast<int32_t>(prediction) >> 11);
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }
    }

    return 0;
}

// Decodes up to `samples` samples of DSP-ADPCM: 8-byte frames of 14 samples, the high nibble of each byte first.
// `hist1` and `hist2` are the two samples before the first.
std::vector<int16_t> DecodeDspAdpcm(std::span<const uint8_t> data, uint32_t samples, std::span<const int16_t, 16> coefs,
                                    int16_t hist1, int16_t hist2, DspAdpcmSum sum);

// Decodes up to `samples` samples of IMA-ADPCM, the low nibble of each byte first, from the sample `sample` and the
// step index `index`. An index past 88 counts as 88.
std::vector<int16_t> DecodeImaAdpcm(std::span<const uint8_t> data, uint32_t samples, int16_t sample, uint8_t index);

// An IMA-ADPCM decoder's state between two samples: the last sample, and the index into the table of steps.
struct ImaAdpcmState
{
    friend bool operator==(const ImaAdpcmState&, const ImaAdpcmState&) = default;

    int16_t sample = 0;
    uint8_t index = 0;
};

// The decoder's state after the nibble `nibble` (0 to 15) from `state`. Its sample is the decoded sample. An index past
// 88 counts as 88. DecodeImaAdpcm decodes each nibble this way.
ImaAdpcmState ImaAdpcmNext(ImaAdpcmState state, uint8_t nibble);

// Decodes up to `samples` samples of the GameCube's 4-bit ADPCM (AFC): 9-byte frames of 16 samples, the high nibble of
// each byte first. `hist1` and `hist2` are the two samples before the first. A frame that the data cuts short isn't
// decoded.
std::vector<int16_t> DecodeAfc(std::span<const uint8_t> data, uint32_t samples, int16_t hist1, int16_t hist2);

} // namespace ntdlib::wave
