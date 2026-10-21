// SPDX-License-Identifier: MIT

// Uses ntdlib from another project: decodes four PCM8 samples.

#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/wave/pcm.h"

int main()
{
    const std::vector<uint8_t> kData = {0x00, 0x7f, 0x80, 0xff};

    const std::vector<int16_t> samples = ntdlib::wave::DecodePcm8(kData, 4);

    XSFLIB_CHECK(samples == std::vector<int16_t>({0, 0x7f00, -0x8000, -0x100}));

    return xsflib::testing::Summary("package");
}
