// SPDX-License-Identifier: MIT

#include "ntdlib/ctr/snd/util.h"

#include <cmath>
#include <cstdint>

#include "ntdlib/ctr/snd/tables.h"

namespace ntdlib::ctr::snd::util
{

void CalcMonoFilterCoefficients(uint16_t freq, int16_t& b0, int16_t& a1, bool enhanced)
{
    // code.bin 0x17f210. The cosine is nn::math's CosFIdx (0x172d44). CosFIdx takes an angle in 1/256 turns and
    // interpolates the cosines in its table of sines and cosines linearly. w is therefore cos(2 pi f / 32000), or cos(2
    // pi f / 48000) in the enhanced mode. At 16000 Hz or less the angle stays under half a turn. CosFIdx's wrapping of
    // larger and negative angles then changes nothing.
    if (freq > 16000)
    {
        freq = 16000;
    }

    const float x = static_cast<float>(freq) * (enhanced ? 256.0f / 48000.0f : 0.008f);
    const uint32_t i = static_cast<uint32_t>(x);
    const float w = tables::kSin[i][2] + (x - static_cast<float>(i)) * tables::kSin[i][3];
    const float b = 2.0f - w;
    const float c = std::sqrt(b * b - 1.0f) - b;
    b0 = static_cast<int16_t>(static_cast<int>((c + 1.0f) * 32768.0f));
    a1 = static_cast<int16_t>(-static_cast<int>(c * 32768.0f));
}

namespace
{

// The library's tables of the same filters for 48 kHz follow those for 32 kHz. They are types 6 to 10. The 3DS's policy
// here takes them too.
struct EnhancedPolicy : ntdlib::nwsnd::CtrPolicy
{
    static constexpr int kBiquadPresetTypes = 10;
};

} // namespace

const int16_t* BiquadPresetCoefficients(int type, float value, bool enhanced)
{
    constexpr int kTypes = ntdlib::nwsnd::CtrPolicy::kBiquadPresetTypes;
    if (enhanced && type >= 1 && type <= kTypes)
    {
        return ntdlib::nwsnd::BiquadPresetCoefficients<EnhancedPolicy>(type + kTypes, value);
    }

    return ntdlib::nwsnd::BiquadPresetCoefficients<ntdlib::nwsnd::CtrPolicy>(type, value);
}

} // namespace ntdlib::ctr::snd::util
