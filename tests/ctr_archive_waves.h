// SPDX-License-Identifier: MIT

// Waves and a sequence for the tests of the 3DS's archive model (ctr_snd_test, ctr_enhanced_test).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/testing/sar_files.h"

namespace ntdlib::ctr::archive_test
{

// A looped DSP-ADPCM wave of 400 frames of 14 samples, from a fixed run of numbers. Each frame's header picks a
// predictor of 0 to 7 and a scale of 0 to 11. The loop runs from frame 100 to the end.
inline testing::sar_files::WaveSpec AdpcmWave()
{
    std::vector<uint8_t> bytes(400 * 8);
    uint32_t state = 1;
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        state = state * 1103515245u + 12345u;
        const auto value = static_cast<uint8_t>(state >> 16);
        bytes[i] = i % 8 == 0 ? static_cast<uint8_t>((value & 0x70) | (value % 12)) : value;
    }

    testing::sar_files::WaveSpec wave;
    wave.encoding = 2;
    wave.loop = true;
    wave.loop_start = 100 * 14;
    wave.loop_end = 400 * 14;
    wave.channels = {bytes};

    return wave;
}

// A PCM8 wave of a sawtooth, 0.5 s long. It plays once.
inline testing::sar_files::WaveSpec Pcm8Wave()
{
    std::vector<uint8_t> bytes(16000);
    for (std::size_t i = 0; i < bytes.size(); i++)
    {
        bytes[i] = static_cast<uint8_t>(3 * i);
    }

    testing::sar_files::WaveSpec wave;
    wave.encoding = 0;
    wave.loop_end = static_cast<uint32_t>(bytes.size());
    wave.channels = {bytes};

    return wave;
}

// A sequence of notes on programs 0 to 2. It sets the pan to 48. With the note wait off, it starts key 64 on program 1,
// key 55 at velocity 80 on program 2 and key 72 on program 0, and waits 96 ticks. With the note wait on, it then plays
// keys 48, 60 and 67 on the three programs for 48 ticks each.
inline std::vector<uint8_t> Notes()
{
    return {0xC0, 0x30, 0xC7, 0x00, 0x81, 0x01, 0x40, 0x7F, 0x60, 0x81, 0x02, 0x37, 0x50,
            0x60, 0x81, 0x00, 0x48, 0x7F, 0x60, 0x80, 0x60, 0xC7, 0x01, 0x81, 0x01, 0x30,
            0x7F, 0x30, 0x81, 0x02, 0x3C, 0x7F, 0x30, 0x81, 0x00, 0x43, 0x7F, 0x30, 0xFF};
}

} // namespace ntdlib::ctr::archive_test
