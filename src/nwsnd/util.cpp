// SPDX-License-Identifier: MIT

#include "ntdlib/nwsnd/util.h"

#include <array>
#include <cmath>
#include <cstdint>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/tables.h"

namespace ntdlib::nwsnd
{

uint64_t ToUint64(float x)
{
    if (!(x >= 1.0f))
    {
        return 0;
    }
    if (x >= 18446744073709551616.0f)
    {
        return UINT64_MAX;
    }

    return static_cast<uint64_t>(x);
}

float CalcPitchRatio(int pitch)
{
    // Whole octaves first, then semitones and 1/256 semitones from the tables. Doubling and halving are exact.
    int octave = 0;
    if (pitch < 0)
    {
        const int down = (-1 - pitch) / 3072 + 1;
        octave = -down;
        pitch += down * 3072;
    }
    if (pitch >= 3072)
    {
        const int up = (pitch - 3072) / 3072 + 1;
        octave += up;
        pitch -= up * 3072;
    }

    const int semitone = pitch / 256;
    const int fine = pitch - semitone * 256;

    float ratio = 1.0f;
    for (int i = 0; i < octave; ++i)
    {
        ratio += ratio;
    }
    for (int i = 0; i > octave; --i)
    {
        ratio *= 0.5f;
    }

    if (semitone != 0)
    {
        ratio *= tables::kSemitone[semitone];
    }
    if (fine != 0)
    {
        ratio *= tables::kFine[fine];
    }

    return ratio;
}

std::array<int16_t, 5> CalcLowPassBiquad(uint16_t frequency, int32_t sample_rate, bool from_table)
{
    if (from_table)
    {
        // A binary search of the cutoffs, then the row of the table for the renderer's rate.
        int index = -1;
        int low = 0;
        int high = 23;
        while (low <= high)
        {
            const int middle = (low + high) / 2;
            if (tables::kLpfFreq[middle] < frequency)
            {
                low = middle + 1;
            }
            else if (tables::kLpfFreq[middle] > frequency)
            {
                high = middle - 1;
            }
            else
            {
                index = middle;
                break;
            }
        }

        const int16_t (*table)[5] = sample_rate == 48000   ? tables::kLowPass48k
                                    : sample_rate == 32000 ? tables::kLowPass32k
                                                           : nullptr;
        if (index < 0 || table == nullptr)
        {
            return {};
        }

        const int16_t* row = table[index];
        return {row[0], row[1], row[2], row[3], row[4]};
    }

    // The exact one-pole: c = 2 - cos(2 pi f / fs), with nn::util's cosine from a turn in 32-bit fixed point, and a1 =
    // (c - sqrt(c^2 - 1)) in Q14. The code works a1 out as (sqrt(c^2 - 1) - c) times -16384.
    const int64_t turn = sample_rate == 0 ? 0 : (static_cast<int64_t>(frequency) << 32) / sample_rate;
    const uint32_t row = static_cast<uint32_t>(turn >> 24) & 0xFF;
    const float fraction = static_cast<float>(static_cast<uint32_t>(turn) & 0xFFFFFF) * 0x1p-24f;
    const float cosine = tables::kSinCos[row][0] + tables::kSinCos[row][2] * fraction;
    const float c = 2.0f - cosine;
    const float root = std::sqrt(c * c + -1.0f);
    const int32_t a1 = NxPolicy::ToInt32((root - c) * -16384.0f);
    return {static_cast<int16_t>(0x4000 - a1), 0, 0, static_cast<int16_t>(a1), 0};
}

} // namespace ntdlib::nwsnd
