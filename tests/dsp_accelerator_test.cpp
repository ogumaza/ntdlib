// SPDX-License-Identifier: MIT

// Checks the accelerator's public reads (ntdlib/dsp/accelerator.h) and the accumulators' helpers
// (ntdlib/dsp/accumulator.h): the raw reads of each size, ADPCM decoded across a frame's header as wave's decoder
// decodes it, the end address with a handler that stops the reads and with one that loops, and PCM scaled by the gain.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dsp/accelerator.h"
#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/wave/adpcm.h"

namespace
{

using ntdlib::dsp::Accelerator;
using ntdlib::dsp::AcceleratorDecode;
using ntdlib::dsp::AcceleratorNext;
using ntdlib::dsp::AcceleratorRaw;
using xsflib::testing::Check;

// Two DSP-ADPCM frames: a header (predictor and scale) and 14 nibbles each.
const std::vector<uint8_t> kFrames = {0x12, 0x17, 0xf3, 0x80, 0x4c, 0x21, 0x9e, 0x05,
                                      0x25, 0x7f, 0x01, 0x88, 0xe2, 0x3a, 0x64, 0xc9};

// Coefficient pairs in the order of registers 0xa0 to 0xaf.
const std::array<uint16_t, 16> kCoefs = {0x0400, 0x0000, 0x0800, 0xfc00, 0x0c00, 0xf800, 0x0600, 0xfe00,
                                         0x0200, 0x0100, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000};

// A reader of `data`'s bytes. A byte past the end reads as 0.
struct Bytes
{
    uint8_t operator()(uint32_t address) const
    {
        return address < data.size() ? data[address] : 0;
    }

    const std::vector<uint8_t>& data;
};

void TestRaw()
{
    const std::vector<uint8_t> kData = {0xab, 0xcd, 0xef};
    const Bytes byte{kData};

    Check(AcceleratorRaw(0, 0, byte) == 0xa && AcceleratorRaw(1, 0, byte) == 0xb && AcceleratorRaw(3, 0, byte) == 0xd,
          "nibbles, the high one first");
    Check(AcceleratorRaw(2, 1, byte) == 0xef, "bytes");
    Check(AcceleratorRaw(0, 2, byte) == 0xabcd && AcceleratorRaw(1, 2, byte) == 0xef00, "16-bit words");
    Check(AcceleratorRaw(0, 3, byte) == 0, "size 3 reads 0");
}

// Reads the two frames from the first sample of the first. The wave module's decoder gives the same samples with the
// accelerator's sum. The end address is the second frame's last nibble. There the handler stops the reads.
void TestAdpcmToTheEnd()
{
    Accelerator a;
    a.start = 2;
    a.current = 2;
    a.end = 31;
    a.pred_scale = kFrames[0];
    const Bytes byte{kFrames};
    int ends = 0;
    const auto stop = [&]
    {
        ++ends;
        a.reads_stopped = true;
    };
    std::vector<int16_t> read;

    for (int i = 0; i < 28; ++i)
    {
        read.push_back(static_cast<int16_t>(AcceleratorNext(a, kCoefs, byte, stop)));
        if (i == 13)
        {
            Check(a.pred_scale == kFrames[8] && a.current == 18, "the second frame's header, read as it starts");
        }
    }

    std::array<int16_t, 16> coefs{};
    for (std::size_t i = 0; i < coefs.size(); ++i)
    {
        coefs[i] = static_cast<int16_t>(kCoefs[i]);
    }
    const std::vector<int16_t> expected =
        ntdlib::wave::DecodeDspAdpcm(kFrames, 28, coefs, 0, 0, ntdlib::wave::DspAdpcmSum::kAccelerator);
    Check(read == expected, "the wave module's decoder gives the same samples");
    Check(ends == 1 && a.current == a.start, "the end address calls the handler once, back at the start");
    Check(AcceleratorNext(a, kCoefs, byte, [] {}) == 0, "a read after the reads stop gives 0");
}

// A handler that loops: it loads the loop's predictor and history, and the reads go on from the start.
void TestAdpcmLoop()
{
    Accelerator a;
    a.start = 2;
    a.current = 2;
    a.end = 15;
    a.pred_scale = kFrames[0];
    const Bytes byte{kFrames};
    std::vector<uint16_t> first;
    std::vector<uint16_t> second;
    const auto loop = [&a]
    {
        a.pred_scale = kFrames[0];
        a.yn1 = 0;
        a.yn2 = 0;
    };

    for (int i = 0; i < 14; ++i)
    {
        first.push_back(AcceleratorNext(a, kCoefs, byte, loop));
    }
    for (int i = 0; i < 14; ++i)
    {
        second.push_back(AcceleratorNext(a, kCoefs, byte, loop));
    }

    Check(!a.reads_stopped && first == second, "a loop plays the frame again");
}

void TestPcm()
{
    // PCM16 (size 2), each term shifted by 11. A gain of 0x400 halves 0x1000, and coefficient pair 5 (0, 0) adds none
    // of the history.
    Accelerator a;
    a.format = 0x0a;
    a.gain = 0x0400;
    a.pred_scale = 0x50;
    a.yn1 = 1000;

    Check(static_cast<int16_t>(AcceleratorDecode(a, kCoefs, 0x1000)) == 0x800 && a.yn1 == 0x800 && a.yn2 == 1000,
          "PCM scaled by the gain");
}

void TestAccumulators()
{
    using ntdlib::dsp::MiddleWord;
    using ntdlib::dsp::Wrap40;

    Check(Wrap40(int64_t{1} << 39) == -(int64_t{1} << 39) && Wrap40(-1) == -1 && Wrap40(int64_t{1} << 40) == 0,
          "Wrap40");
    Check(MiddleWord(0x12345678) == 0x1234 && MiddleWord(-0x10000) == -1, "the middle word in range");
    Check(MiddleWord(int64_t{1} << 31) == 0x7fff && MiddleWord(-(int64_t{1} << 31) - 1) == -0x8000,
          "the middle word saturates outside 32 bits");
}

} // namespace

int main()
{
    TestRaw();
    TestAdpcmToTheEnd();
    TestAdpcmLoop();
    TestPcm();
    TestAccumulators();

    return xsflib::testing::Summary("dsp_accelerator");
}
