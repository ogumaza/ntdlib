// SPDX-License-Identifier: MIT

// AX's low-pass coefficients (see lpf.h).

#include "ntdlib/ax/lpf.h"

#include <cmath>
#include <cstdint>

namespace ntdlib::ax
{

LpfCoefs ComputeLpfCoefs(uint32_t frequency, const LpfSettings& settings)
{
    // 6.2831855f is the float nearest to 2 pi. The arithmetic between the calls is in float.
    const float w = 6.2831855f * static_cast<float>(frequency) / settings.rate;
    const float k =
        2.0f - (settings.double_precision ? static_cast<float>(std::cos(static_cast<double>(w))) : std::cos(w));
    const float k2 = settings.fused ? std::fma(k, k, -1.0f) : k * k - 1.0f;
    const float root =
        settings.double_precision ? static_cast<float>(std::sqrt(static_cast<double>(k2))) : std::sqrt(k2);

    // b0 is at most 32768. A float converts toward zero.
    const auto b0 = static_cast<uint16_t>(static_cast<int32_t>((k - root) * 32768.0f));

    return {static_cast<uint16_t>(32767 - b0), b0};
}

} // namespace ntdlib::ax
