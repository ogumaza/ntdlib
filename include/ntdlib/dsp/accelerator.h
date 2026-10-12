// SPDX-License-Identifier: MIT

// The DSP's accelerator: its state, and its reads of sample data. The accelerator streams samples from memory and
// decodes them: DSP-ADPCM, or PCM scaled by the gain. The DSP reads it through its hardware registers. A model of a DSP
// program that runs outside the interpreter can read it directly, with the program's handler at the end address in
// place of the DSP's exception.

#pragma once

#include <cstdint>

#include "ntdlib/wave/adpcm.h"

namespace ntdlib::dsp
{

// The accelerator's state.
struct Accelerator
{
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t current = 0;
    uint16_t format = 0;
    uint16_t gain = 0;
    uint16_t yn1 = 0;
    uint16_t yn2 = 0;
    uint16_t pred_scale = 0;
    uint16_t input = 0;
    bool reads_stopped = false;
};

// The raw value at the address `current` for an access size: 0 a nibble (a byte's high nibble first), 1 a byte, 2 a
// 16-bit word with its high byte first. Size 3 reads 0. `byte(address)` reads a byte of the sample data.
template <typename ReadByte>
uint16_t AcceleratorRaw(uint32_t current, int size, const ReadByte& byte)
{
    switch (size)
    {
    case 0:
        return (current & 1) ? byte(current >> 1) & 0xf : byte(current >> 1) >> 4;
    case 1:
        return byte(current);
    case 2:
        return static_cast<uint16_t>(byte(current * 2) << 8 | byte(current * 2 + 1));
    default:
        return 0;
    }
}

// A sample decoded from `raw`: DSP-ADPCM, or PCM scaled by the gain. Both are predicted from the history with the
// coefficient pair that the predictor selects. `coefs[i]` gives coefficient register 0xa0 + i. The sample becomes the
// newest in the history.
template <typename Coefs>
uint16_t AcceleratorDecode(Accelerator& a, const Coefs& coefs, uint16_t raw)
{
    const int pair = a.pred_scale >> 4 & 7;
    const int32_t c1 = static_cast<int16_t>(coefs[pair * 2]);
    const int32_t c2 = static_cast<int16_t>(coefs[pair * 2 + 1]);
    const int32_t yn1 = static_cast<int16_t>(a.yn1);
    const int32_t yn2 = static_cast<int16_t>(a.yn2);
    int32_t sample = 0;
    if ((a.format >> 2 & 3) == 0)
    {
        int32_t x = raw & 0xf;
        if (x >= 8)
        {
            x -= 16;
        }

        sample = wave::DspAdpcmSample(x, a.pred_scale & 0xf, c1, c2, yn1, yn2, wave::DspAdpcmSum::kAccelerator);
    }
    else
    {
        const int shift = (a.format >> 4 & 3) == 0 ? 11 : (a.format >> 4 & 3) == 2 ? 16 : 0;
        const int32_t value = ((static_cast<int32_t>(a.gain) * static_cast<int16_t>(raw)) >> shift) +
                              ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
        sample = static_cast<int16_t>(value);
    }

    a.yn2 = a.yn1;
    a.yn1 = static_cast<uint16_t>(sample);

    return static_cast<uint16_t>(sample);
}

// The next decoded sample: what a read of the accelerator's sample register gives. The sample comes from the data at
// the current address, or from the input register for the formats that take it. Then the address moves on. Decoding
// mode 1 leaves it in place. At an ADPCM frame's header, the accelerator takes the frame's predictor and scale and
// moves past the header. At the end address, the address goes back to the start and `at_end()` runs. The DSP stops the
// reads there and raises exception 5. A model of a program's handler can instead stop the voice, or load the loop's
// state and go on. While the reads are stopped, a read gives 0.
template <typename Coefs, typename ReadByte, typename AtEnd>
uint16_t AcceleratorNext(Accelerator& a, const Coefs& coefs, const ReadByte& byte, const AtEnd& at_end)
{
    if (a.reads_stopped)
    {
        return 0;
    }

    const int decoding = a.format >> 2 & 3;
    const uint16_t sample = AcceleratorDecode(
        a, coefs, decoding == 1 || decoding == 3 ? a.input : AcceleratorRaw(a.current, a.format & 3, byte));
    uint32_t step = 2;
    if (decoding == 0)
    {
        a.current++;
        if ((a.end & 0xf) == 0 && a.current == a.end)
        {
            a.current = a.start + 1;
        }
        else if ((a.end & 0xf) == 1 && a.current == a.end - 1)
        {
            a.current = a.start;
        }
        else if ((a.current & 0xf) == 0)
        {
            a.pred_scale = byte(a.current >> 1);
            a.current += 2;
            step = 4;
        }
    }
    else if (decoding != 1)
    {
        a.current++;
    }

    if (a.current == a.end + step - 1)
    {
        a.current = a.start;
        at_end();
    }

    a.current &= 0xbfffffff;

    return sample;
}

} // namespace ntdlib::dsp
