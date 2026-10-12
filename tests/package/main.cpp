// SPDX-License-Identifier: MIT

// Uses ntdlib from another project: decodes four PCM8 samples.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "ntdlib/wave/pcm.h"
#include "xsflib/testing/check.h"

int main()
{
    const std::vector<uint8_t> data = {0x00, 0x7f, 0x80, 0xff};

    const std::vector<int16_t> samples = ntdlib::wave::DecodePcm8(data, 4);

    XSFLIB_CHECK(samples == std::vector<int16_t>({0, 0x7f00, -0x8000, -0x100}));

    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::printf("package test passed\n");

    return 0;
}
