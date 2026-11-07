// SPDX-License-Identifier: MIT

// Unit tests for the model of the GBA's sound hardware (ntdlib/agb/apu.h). The expected values come from GBATEK's
// description of the GBA's sound and Pan Docs' description of the Game Boy's channels. A few come from mGBA 0.11.0,
// where neither says: the wave's lag of one digit and the level that a stopped wave holds. The enhanced mode's values
// come from the header's rules: the output stage's volumes without the shift's rounding, the band-limited steps'
// response and delay, and a square wave's Fourier series. A grown PSG unit has to play as the console's unit plays
// with the same stores: the header's rule.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/apu.h"

namespace
{

using ntdlib::agb::Apu;
using ntdlib::agb::kApuFifoA;
using ntdlib::agb::kApuSquare1;
using ntdlib::agb::kApuSquare2;
using ntdlib::agb::kEnhancedFrameSamples;
using ntdlib::agb::kFrameCycles;
using ntdlib::agb::kGrownPsgBytes;
using ntdlib::agb::kGrownPsgRegisters;
using ntdlib::agb::kRegSound1CntL;
using ntdlib::agb::kSecondCycles;
using ntdlib::agb::kSequencerCycles;
using ntdlib::agb::kSoundRegisterBytes;

constexpr uint32_t kNr10 = kRegSound1CntL + 0x00;
constexpr uint32_t kNr11 = kRegSound1CntL + 0x02;
constexpr uint32_t kNr12 = kRegSound1CntL + 0x03;
constexpr uint32_t kNr13 = kRegSound1CntL + 0x04;
constexpr uint32_t kNr14 = kRegSound1CntL + 0x05;
constexpr uint32_t kNr21 = kRegSound1CntL + 0x08;
constexpr uint32_t kNr22 = kRegSound1CntL + 0x09;
constexpr uint32_t kNr23 = kRegSound1CntL + 0x0C;
constexpr uint32_t kNr30 = kRegSound1CntL + 0x10;
constexpr uint32_t kNr31 = kRegSound1CntL + 0x12;
constexpr uint32_t kNr32 = kRegSound1CntL + 0x13;
constexpr uint32_t kNr33 = kRegSound1CntL + 0x14;
constexpr uint32_t kNr34 = kRegSound1CntL + 0x15;
constexpr uint32_t kNr41 = kRegSound1CntL + 0x18;
constexpr uint32_t kNr42 = kRegSound1CntL + 0x19;
constexpr uint32_t kNr43 = kRegSound1CntL + 0x1C;
constexpr uint32_t kNr44 = kRegSound1CntL + 0x1D;
constexpr uint32_t kNr50 = kRegSound1CntL + 0x20;
constexpr uint32_t kNr51 = kRegSound1CntL + 0x21;
constexpr uint32_t kSoundCntH = kRegSound1CntL + 0x22;
constexpr uint32_t kSoundCntX = kRegSound1CntL + 0x24;
constexpr uint32_t kSoundBias = kRegSound1CntL + 0x28;
constexpr uint32_t kWaveRam = kRegSound1CntL + 0x30;

// The cycles of an output sample at the default resolution's 32,768 Hz.
constexpr uint32_t kSampleCycles = 512;

// The output of one PSG level with the master volume at 7 and the PSG at 100%: 2 * 8 steps of 64.
constexpr int kLevel = 1024;

// The sound on, both master volumes at 7, every PSG channel on both sides and the PSG at 100%.
Apu PoweredOn()
{
    Apu apu;
    apu.Store(kSoundCntX, 1, 0x80);
    apu.Store(kNr50, 1, 0x77);
    apu.Store(kNr51, 1, 0xFF);
    apu.Store(kSoundCntH, 2, 0x0002);
    return apu;
}

// Runs `samples` output samples, and returns the left side's.
std::vector<int16_t> Left(Apu& apu, size_t samples)
{
    std::vector<int16_t> out;
    apu.Run(samples * (kSecondCycles / uint64_t(apu.Rate())), out);
    std::vector<int16_t> left;
    for (size_t i = 0; i < out.size(); i += 2)
    {
        left.push_back(out[i]);
    }
    return left;
}

// Pan Docs' waveforms of the squares' duties, from the first step: 12.5%, 25%, 50% and 75%.
void TestSquareDuties()
{
    const char* waveforms[4] = {"00000001", "10000001", "10000111", "01111110"};
    for (int duty = 0; duty < 4; ++duty)
    {
        Apu apu = PoweredOn();
        apu.Store(kNr11, 1, uint32_t(duty << 6));
        apu.Store(kNr12, 1, 0xF0);
        apu.Store(kNr13, 2, 0x8400); // a frequency of 1024 and a restart: 16,384 cycles, 32 samples, a step
        const auto left = Left(apu, 512);
        bool matches = true;
        for (size_t i = 0; i < left.size(); ++i)
        {
            matches &= left[i] == (waveforms[duty][i / 32 % 8] == '1' ? 15 * kLevel : 0);
        }
        XSFLIB_CHECK(matches);
    }
}

// GBATEK: a square plays 131,072 / (2048 - n) Hz. A frequency of 1792 gives 512 Hz: a rise every 64 samples.
void TestSquareFrequency()
{
    Apu apu = PoweredOn();
    apu.Store(kNr21, 1, 0x80);
    apu.Store(kNr22, 1, 0xF0);
    apu.Store(kNr23, 2, 0x8700); // 0x700: 1792
    const auto left = Left(apu, 1024);
    std::vector<size_t> rises;
    for (size_t i = 1; i < left.size(); ++i)
    {
        if (left[i] != 0 && left[i - 1] == 0)
        {
            rises.push_back(i);
        }
    }
    XSFLIB_CHECK(rises.size() == 16);
    bool apart = true;
    for (size_t k = 1; k < rises.size(); ++k)
    {
        apart &= rises[k] - rises[k - 1] == 64;
    }
    XSFLIB_CHECK(apart);
}

// Pan Docs: an envelope steps every n / 64 s, at the frame sequencer's step 7, up to 15 or down to 0. A square at a
// step a sample, 75% high, shows its level at steps 1 to 6 of its duty.
std::vector<int> Levels(uint8_t nr12, size_t samples)
{
    Apu apu = PoweredOn();
    apu.Store(kNr11, 1, 0xC0);
    apu.Store(kNr12, 1, nr12);
    apu.Store(kNr13, 2, 0x87E0); // 0x7E0: 2016, a step of 512 cycles
    apu.SetSequencer(7, 10 * kSampleCycles);
    const auto left = Left(apu, samples);
    std::vector<int> levels;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (i % 8 >= 1 && i % 8 <= 6)
        {
            levels.push_back(left[i] / kLevel);
        }
        else if (left[i] != 0)
        {
            levels.push_back(-1); // a low step that isn't 0
        }
    }
    return levels;
}

void TestEnvelope()
{
    // The envelope's steps come at sample 10 and every 512 samples after it.
    const auto expected = [](size_t samples, int start, int direction, int period)
    {
        std::vector<int> levels;
        for (size_t i = 0; i < samples; ++i)
        {
            if (i % 8 < 1 || i % 8 > 6)
            {
                continue;
            }
            const int ticks = i < 10 ? 0 : int(1 + (i - 10) / 512);
            const int steps = ticks / period;
            levels.push_back(std::clamp(start + direction * steps, 0, 15));
        }
        return levels;
    };
    XSFLIB_CHECK(Levels(0xF1, 512 * 18) == expected(512 * 18, 15, -1, 1));
    XSFLIB_CHECK(Levels(0x09, 512 * 18) == expected(512 * 18, 0, 1, 1));
    XSFLIB_CHECK(Levels(0xA3, 512 * 12) == expected(512 * 12, 10, -1, 3));
    XSFLIB_CHECK(Levels(0x70, 512 * 4) == expected(512 * 4, 7, 0, 1));
}

// GBATEK: a square plays (64 - t) / 256 s with its length on, the noise as well, and the wave (256 - t) / 256 s. Pan
// Docs: the lengths count at the frame sequencer's steps 0, 2, 4 and 6, and a length of 0 counts 64 or 256.
void TestLength()
{
    Apu apu = PoweredOn();
    apu.Store(kNr11, 1, 0x80 | 60); // 4
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0xC400); // a restart with the length on
    apu.SetSequencer(0, 1000);
    std::vector<int16_t> out;
    apu.Run(1000 + 3 * 2 * kSequencerCycles - 1, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 1) == 1);
    apu.Run(1, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 1) == 0);

    // A length of 0 counts 64, and the wave's 256: 32 and 128 steps 0, 2, 4 and 6.
    apu = PoweredOn();
    apu.Store(kNr41, 1, 0);
    apu.Store(kNr42, 1, 0xF0);
    apu.Store(kNr44, 1, 0xC0);
    apu.Store(kNr30, 1, 0x80);
    apu.Store(kNr31, 1, 0);
    apu.Store(kNr34, 1, 0xC0);
    apu.SetSequencer(0, 1);
    apu.Run(1 + 63 * 2 * kSequencerCycles - 1, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 0x0C) == 0x0C);
    apu.Run(1, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 0x0C) == 0x04);
    apu.Run(192 * 2 * kSequencerCycles - 1, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 0x04) == 0x04);
    apu.Run(1, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 0x04) == 0);

    // Without its length on, a channel plays on.
    apu = PoweredOn();
    apu.Store(kNr11, 1, 0x80 | 63);
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8400);
    apu.Run(20 * kSequencerCycles, out);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 1) == 1);
}

// GBATEK: the sweep gives X(t) = X(t-1) +/- X(t-1) / 2^n every time units of 7.8 ms. Pan Docs: it steps at the frame
// sequencer's steps 2 and 6, and turns the channel off when the next frequency would pass 2047.
void TestSweep()
{
    Apu apu = PoweredOn();
    apu.Store(kNr10, 1, 0x11); // a time of 1, up, a shift of 1
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8100); // 256
    apu.SetSequencer(2, 100);
    const auto frequency = [&]
    {
        return apu.Register(kNr13) | (apu.Register(kNr14) & 7) << 8;
    };
    std::vector<int16_t> out;
    apu.Run(99, out);
    XSFLIB_CHECK(frequency() == 256);
    for (int expected : {384, 576, 864, 1296})
    {
        apu.Run(1, out);
        XSFLIB_CHECK(frequency() == expected);
        XSFLIB_CHECK((apu.Register(kSoundCntX) & 1) == 1);
        apu.Run(4 * kSequencerCycles - 1, out);
    }
    // 1944 takes its place, and 2916 would pass 2047.
    apu.Run(1, out);
    XSFLIB_CHECK(frequency() == 1944);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 1) == 0);

    // Down, with a time of 2: every other step 2 or 6.
    apu = PoweredOn();
    apu.Store(kNr10, 1, 0x29);
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8100);
    apu.SetSequencer(2, 100);
    apu.Run(100 + 4 * kSequencerCycles, out);
    XSFLIB_CHECK(frequency() == 128);
    apu.Run(8 * kSequencerCycles, out);
    XSFLIB_CHECK(frequency() == 64);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 1) == 1);
}

// The wave's digits 0 to 15, then 15 down to 0, in bank 1. Bank 0 plays while the CPU writes them.
Apu WithWave(uint8_t nr30, uint8_t nr32)
{
    Apu apu = PoweredOn();
    apu.Store(kNr30, 1, 0x00);
    const uint8_t bytes[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
                               0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};
    for (uint32_t i = 0; i < 16; ++i)
    {
        apu.Store(kWaveRam + i, 1, bytes[i]);
    }
    apu.Store(kNr30, 1, nr30);
    apu.Store(kNr32, 1, nr32);
    apu.Store(kNr33, 2, 0x87C0); // 0x7C0: 1984, a digit of 512 cycles
    return apu;
}

int Digit(size_t n)
{
    n %= 32;
    return n < 16 ? int(n) : int(31 - n);
}

// GBATEK: the wave plays its 32 digits, the high nibble of each byte first, at 2,097,152 / (2048 - n) digits a second.
// Each digit sounds a step late: the first sample plays the digit before the first (mGBA 0.11.0).
void TestWave()
{
    Apu apu = WithWave(0xC0, 0x20); // bank 1, at 100%
    const auto left = Left(apu, 96);
    bool matches = true;
    for (size_t i = 0; i < left.size(); ++i)
    {
        matches &= left[i] == Digit(i + 31) * kLevel;
    }
    XSFLIB_CHECK(matches);

    // GBATEK's volumes: 0%, 100%, 50%, 25%, and 75% with bit 7. The first sample after digit 15 plays it.
    const int volumes[5][2] = {{0x00, 0}, {0x20, 15}, {0x40, 7}, {0x60, 3}, {0x80, 11}};
    for (const auto& [nr32, level] : volumes)
    {
        Apu loud = WithWave(0xC0, uint8_t(nr32));
        XSFLIB_CHECK(Left(loud, 17)[16] == level * kLevel);
    }

    // 64 digits: bank 0, then bank 1. Bank 0 holds the same digits, once the CPU has written them there.
    apu = WithWave(0xC0, 0x20);
    const uint8_t zeros[16] = {};
    for (uint32_t i = 0; i < 16; ++i)
    {
        apu.Store(kWaveRam + i, 1, zeros[i]); // bank 0 while bank 1 plays
    }
    apu.Store(kNr30, 1, 0xA0);
    apu.Store(kNr34, 1, 0x87);
    const auto both = Left(apu, 66);
    // The position is 0: the first sample plays bank 1's last digit.
    XSFLIB_CHECK(both[0] == 0);
    bool banks = true;
    for (size_t i = 1; i <= 32; ++i)
    {
        banks &= both[i] == 0;
    }
    for (size_t i = 33; i < 65; ++i)
    {
        banks &= both[i] == Digit(i - 33) * kLevel;
    }
    XSFLIB_CHECK(banks);
}

// GBATEK: the wave RAM is a shift register. A restart leaves the position, and the next digit comes a whole step later.
// A stopped wave holds its last level (mGBA 0.11.0).
void TestWaveRestartAndStop()
{
    Apu apu = WithWave(0xC0, 0x20);
    std::vector<int16_t> out;
    apu.Run(10 * kSampleCycles + 256, out); // position 10, halfway through its step
    apu.Store(kNr34, 1, 0x87);
    const auto after = Left(apu, 3);
    // Without the restart, the next step would come before the first of these samples.
    XSFLIB_CHECK(after[0] == Digit(9) * kLevel);
    XSFLIB_CHECK(after[1] == Digit(10) * kLevel);
    XSFLIB_CHECK(after[2] == Digit(11) * kLevel);

    // Halfway through digit 12's step.
    apu.Run(256, out);

    apu.Store(kNr30, 1, 0x40);
    const auto held = Left(apu, 40);
    bool holds = true;
    for (int16_t sample : held)
    {
        holds &= sample == Digit(12) * kLevel;
    }
    XSFLIB_CHECK(holds);
    XSFLIB_CHECK((apu.Register(kSoundCntX) & 4) == 0);
}

// GBATEK: the noise steps 524,288 Hz / r / 2^(s+1) times a second, r = 0.5 for a ratio of 0. Each step shifts the
// counter right, and a carry takes 0x60 (7 bits) or 0x6000 (15 bits) into it. GBATEK's output is high with a carry: the
// Game Boy's analogue output. The output falls as the level rises. The channel gives its level without a carry. mGBA
// 0.11.0 does the same. GBATEK leaves the output before the counter's first step open. mGBA's recordings stay silent
// until that step.
void TestNoise()
{
    for (const bool seven : {true, false})
    {
        Apu apu = PoweredOn();
        apu.Store(kNr42, 1, 0xF0);
        apu.Store(kNr43, 1, seven ? 0x39u : 0x31u); // a shift of 3, a ratio of 1: a step of 512 cycles
        apu.Store(kNr44, 1, 0x80);
        const auto left = Left(apu, 300);
        uint32_t counter = seven ? 0x40 : 0x4000;
        bool carry = true;
        bool matches = left[0] == 0;
        for (size_t i = 1; i < left.size(); ++i)
        {
            carry = (counter & 1) != 0;
            counter >>= 1;
            if (carry)
            {
                counter ^= seven ? 0x60 : 0x6000;
            }
            matches &= left[i] == (carry ? 0 : 15 * kLevel);
        }
        XSFLIB_CHECK(matches);
        if (seven)
        {
            // 127 steps a period.
            bool repeats = true;
            for (size_t i = 1; i + 127 < left.size(); ++i)
            {
                repeats &= left[i] == left[i + 127];
            }
            XSFLIB_CHECK(repeats);
        }
    }

    // A ratio of 0 counts half: a shift of 4 steps every 512 cycles too.
    Apu a = PoweredOn();
    Apu b = PoweredOn();
    for (Apu* apu : {&a, &b})
    {
        apu->Store(kNr42, 1, 0xF0);
    }
    a.Store(kNr43, 1, 0x48);
    b.Store(kNr43, 1, 0x39);
    a.Store(kNr44, 1, 0x80);
    b.Store(kNr44, 1, 0x80);
    XSFLIB_CHECK(Left(a, 200) == Left(b, 200));
}

// GBATEK: a timer's overflow takes a sample from each FIFO on it. A FIFO's sample counts 2 or 4 times, at 50% or 100%,
// and an empty FIFO goes on with its last sample.
void TestFifos()
{
    Apu apu = PoweredOn();
    apu.Store(kSoundCntH, 2, 0x7304); // A at 100% on timer 0, B at 50% on timer 1, both sides
    const int8_t a[] = {10, 20, 30};
    const int8_t b[] = {-7};
    apu.Queue(0, a);
    apu.Queue(1, b);
    apu.SetTimer(0, 1024, 100);
    apu.SetTimer(1, 5000, 3000);
    XSFLIB_CHECK(apu.Queued(0) == 3);
    std::vector<int16_t> out;
    apu.Run(16 * kSampleCycles, out);
    XSFLIB_CHECK(apu.Queued(0) == 0 && apu.Queued(1) == 0);
    // Timer 0 overflows at cycles 100, 1124 and 2148, timer 1 at 3000. The samples fall every 512 cycles.
    const int expected_a[16] = {0, 10, 10, 20, 20, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30};
    bool matches = true;
    for (size_t i = 0; i < 16; ++i)
    {
        const int sample_b = i * kSampleCycles > 3000 ? -7 : 0;
        const int sum = expected_a[i] * 4 + sample_b * 2;
        matches &= out[2 * i] == sum * 64 && out[2 * i + 1] == sum * 64;
    }
    XSFLIB_CHECK(matches);

    // Bits 11 and 15 empty a FIFO. The FIFO then gives 0.
    apu.Queue(0, a);
    apu.Store(kSoundCntH + 1, 1, 0x08 | 0x73);
    XSFLIB_CHECK(apu.Queued(0) == 0);
    XSFLIB_CHECK(apu.Register(kSoundCntH + 1) == 0x73);
    out.clear();
    apu.Run(kSampleCycles, out);
    XSFLIB_CHECK(out[0] == -7 * 2 * 64);
}

// GBATEK's output stage: the PSG's levels times the master volume plus 1, times 2, at 25%, 50% or 100%, plus the
// FIFOs, plus the bias, clipped to 0 to 1023, keeping the resolution's top bits.
void TestOutputStage()
{
    const auto first = [](Apu& apu)
    {
        return Left(apu, 1)[0];
    };
    const auto square = [](uint16_t cnt_h, uint8_t nr50)
    {
        Apu apu = PoweredOn();
        apu.Store(kSoundCntH, 2, cnt_h);
        apu.Store(kNr50, 1, nr50);
        apu.Store(kNr11, 1, 0xC0);
        apu.Store(kNr12, 1, 0xF0);
        apu.Store(kNr13, 2, 0x8000);
        std::vector<int16_t> out;
        apu.Run(2 * 32768, out); // a step of 32,768 cycles: the last sample lies in step 1, a high step
        return out;
    };
    // Level 15 at master volume 7: 240 steps at 100%, 120 at 50% and 60 at 25%. Master volume 3 on the left: 120.
    const int16_t full = square(0x0002, 0x77).back();
    XSFLIB_CHECK(full == 240 * 64);
    XSFLIB_CHECK(square(0x0001, 0x77).back() == 120 * 64);
    XSFLIB_CHECK(square(0x0000, 0x77).back() == 60 * 64);
    const auto sides = square(0x0002, 0x37);
    XSFLIB_CHECK(sides[sides.size() - 2] == 120 * 64 && sides.back() == 240 * 64);

    // Clipping: two FIFOs of 127 at 100% reach 512 + 1016, past 1023. Two of -128 reach -512, below 0.
    for (const auto& [sample, expected] : {std::pair<int8_t, int>{127, (1022 - 512) * 64}, {-128, -512 * 64}})
    {
        Apu apu = PoweredOn();
        apu.Store(kSoundCntH, 2, 0x330C);
        const int8_t samples[] = {sample};
        apu.Queue(0, samples);
        apu.Queue(1, samples);
        apu.SetTimer(0, 100, 0);
        XSFLIB_CHECK(first(apu) == expected);
    }

    // The resolution: a FIFO's 1 at 50% adds 2 steps. At 32,768 Hz the output keeps 9 bits, 512 + 2. At 65,536 Hz it
    // keeps 8 and drops the 2.
    for (const auto& [bias, expected] : {std::pair<uint16_t, int>{0x0200, 2 * 64}, {0x4200, 0}})
    {
        Apu apu = PoweredOn();
        apu.Store(kSoundBias, 2, bias);
        apu.Store(kSoundCntH, 2, 0x0300);
        const int8_t samples[] = {1};
        apu.Queue(0, samples);
        apu.SetTimer(0, 100, 0);
        XSFLIB_CHECK(first(apu) == expected);
    }

    // A side's enables: NR51's channels and SOUNDCNT_H's FIFOs.
    Apu apu = PoweredOn();
    apu.Store(kNr51, 1, 0x10);        // square 1 on the left only
    apu.Store(kSoundCntH, 2, 0x0106); // FIFO A at 100% on the right only
    apu.Store(kNr11, 1, 0xC0);
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8000);
    const int8_t samples[] = {-50};
    apu.Queue(0, samples);
    apu.SetTimer(0, 100, 0);
    std::vector<int16_t> out;
    apu.Run(2 * 32768 + kSampleCycles, out); // step 1 of the square
    XSFLIB_CHECK(out[out.size() - 2] == 240 * 64);
    XSFLIB_CHECK(out.back() == -200 * 64);

    // The bias sets the middle: without the sound on, the output is 0.
    Apu off;
    XSFLIB_CHECK(first(off) == 0);
}

// GBATEK: the rates of the resolutions 9 to 6 bits: 32,768, 65,536, 131,072 and 262,144 Hz. The model puts out the
// higher two at 65,536 Hz.
void TestRate()
{
    const int rates[4] = {32768, 65536, 65536, 65536};
    for (uint32_t resolution = 0; resolution < 4; ++resolution)
    {
        Apu apu;
        apu.Store(kSoundBias, 2, 0x200 | resolution << 14);
        XSFLIB_CHECK(apu.Rate() == rates[resolution]);
        std::vector<int16_t> out;
        apu.Run(kSecondCycles / 64, out);
        XSFLIB_CHECK(out.size() == size_t(2 * rates[resolution] / 64));
    }
}

// Pan Docs: turning the sound off clears NR10 to NR51 and ignores their stores until it's on again. The wave RAM keeps
// its digits. SOUNDCNT_X's bits 0 to 3 give the channels that play.
void TestPower()
{
    Apu apu = WithWave(0xC0, 0x20);
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8400);
    XSFLIB_CHECK(apu.Register(kSoundCntX) == 0x85);
    apu.Store(kSoundCntX, 1, 0x00);
    XSFLIB_CHECK(apu.Register(kSoundCntX) == 0x00);
    bool cleared = true;
    for (uint32_t address = kNr10; address <= kNr51; ++address)
    {
        cleared &= apu.Register(address) == 0;
    }
    XSFLIB_CHECK(cleared);
    apu.Store(kNr50, 1, 0x77);
    XSFLIB_CHECK(apu.Register(kNr50) == 0);
    XSFLIB_CHECK(apu.Register(kSoundBias + 1) == 0x02);
    std::vector<int16_t> out;
    apu.Run(kSampleCycles, out);
    XSFLIB_CHECK(out[0] == 0 && out[1] == 0);

    // On again: the CPU reaches bank 1 with the digits while bank 0 plays.
    apu.Store(kSoundCntX, 1, 0x80);
    XSFLIB_CHECK(apu.Register(kWaveRam) == 0x01 && apu.Register(kWaveRam + 15) == 0x10);
}

// The registers as a game's init leaves them: every channel off, and the FIFOs' resets read back as 0.
void TestRegisters()
{
    std::vector<uint8_t> registers(kSoundRegisterBytes);
    registers[kNr50 - kRegSound1CntL] = 0x77;
    registers[kSoundCntH - kRegSound1CntL + 1] = 0x8B;
    registers[kSoundCntX - kRegSound1CntL] = 0x8F;
    registers[kSoundBias - kRegSound1CntL + 1] = 0x42;
    registers[kWaveRam - kRegSound1CntL] = 0x5A;
    const Apu apu(registers);
    XSFLIB_CHECK(apu.Register(kNr50) == 0x77);
    XSFLIB_CHECK(apu.Register(kSoundCntH + 1) == 0x03);
    XSFLIB_CHECK(apu.Register(kSoundCntX) == 0x80);
    XSFLIB_CHECK(apu.Rate() == 65536);
    XSFLIB_CHECK(apu.Register(kWaveRam) == 0x5A);

    bool refused = false;
    try
    {
        static_cast<void>(Apu(std::vector<uint8_t>(10)));
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
}

// Muted channels leave the output, and their state goes on.
void TestMutes()
{
    Apu apu = PoweredOn();
    apu.Store(kNr11, 1, 0xC0);
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8000);
    apu.Store(kSoundCntH, 2, 0x0306);
    const int8_t samples[] = {25};
    apu.Queue(0, samples);
    apu.SetTimer(0, 100, 0);
    apu.SetMuted(kApuSquare1 | kApuSquare2);
    std::vector<int16_t> out;
    apu.Run(2 * 32768 + kSampleCycles, out);
    XSFLIB_CHECK(out.back() == 100 * 64);
    apu.SetMuted(kApuFifoA);
    out.clear();
    apu.Run(kSampleCycles, out);
    XSFLIB_CHECK(out.back() == 240 * 64);
}

// The enhanced mode's samples of `cycles`, left then right, split into the two sides.
std::pair<std::vector<float>, std::vector<float>> Enhanced(Apu& apu, uint64_t cycles)
{
    std::vector<float> out;
    apu.Run(cycles, out);
    std::pair<std::vector<float>, std::vector<float>> sides;
    for (size_t i = 0; i < out.size(); i += 2)
    {
        sides.first.push_back(out[i]);
        sides.second.push_back(out[i + 1]);
    }
    return sides;
}

// Each frame of kFrameCycles cycles gives kEnhancedFrameSamples samples: those whose times lie before the run's end.
void TestEnhancedSamples()
{
    Apu apu = PoweredOn();
    apu.StartEnhanced();
    XSFLIB_CHECK(apu.Enhanced());
    XSFLIB_CHECK(Enhanced(apu, kFrameCycles).first.size() == kEnhancedFrameSamples);
    // 1,000 cycles hold the times of samples 804 to 806: 804 + 1000 * 804 / 280896 is 806.9.
    XSFLIB_CHECK(Enhanced(apu, 1000).first.size() == 3);
    XSFLIB_CHECK(Enhanced(apu, kFrameCycles - 1000).first.size() == kEnhancedFrameSamples - 3);

    int refused = 0;
    std::vector<int16_t> strict;
    std::vector<float> enhanced;
    const float samples[] = {1.0f};
    try
    {
        apu.Run(100, strict);
    }
    catch (const std::logic_error&)
    {
        ++refused;
    }
    Apu console = PoweredOn();
    try
    {
        console.Run(100, enhanced);
    }
    catch (const std::logic_error&)
    {
        ++refused;
    }
    try
    {
        console.QueueEnhanced(0, samples);
    }
    catch (const std::logic_error&)
    {
        ++refused;
    }
    XSFLIB_CHECK(refused == 3);
}

// A slow square's plateaus hold the levels exactly, at 128 * (master volume + 1) >> share steps of a 16-bit sample for
// each step of the level. The console's mode rounds the sum down after the shift: with master volume 2 at 25%, it
// gives 22 * 64 for level 15, and the enhanced mode 15 * 96.
void TestEnhancedVolumes()
{
    Apu apu = PoweredOn();
    apu.Store(kNr50, 1, 0x25); // left 2, right 5
    apu.Store(kNr51, 1, 0x11);
    apu.Store(kSoundCntH, 2, 0x0000); // the PSG at 25%
    apu.StartEnhanced();
    apu.Store(kNr11, 1, 0x80);   // 50%
    apu.Store(kNr12, 1, 0xF0);   // level 15
    apu.Store(kNr13, 2, 0x8000); // frequency 0: 32,768 cycles a step
    const auto [left, right] = Enhanced(apu, 8 * 32768);

    // Steps 0, 5, 6 and 7 are high. The fall comes at cycle 32,768, sample 93.8, and the rise at cycle 163,840,
    // sample 469.0. A change reaches from 32 samples before its time to 32 after it, and the output runs 32 samples
    // late. Away from the changes, the plateaus hold the levels exactly.
    const float high_left = 15.0f * 96.0f / 32768.0f;
    const float high_right = 15.0f * 192.0f / 32768.0f;
    bool plateaus = true;
    for (size_t i = 0; i < left.size(); ++i)
    {
        if (i <= 92)
        {
            plateaus &= left[i] == high_left && right[i] == high_right;
        }
        else if (i >= 159 && i <= 468)
        {
            plateaus &= left[i] == 0.0f && right[i] == 0.0f;
        }
        else if (i >= 535)
        {
            plateaus &= left[i] == high_left && right[i] == high_right;
        }
    }
    XSFLIB_CHECK(plateaus);
    XSFLIB_CHECK(left[125] > 0.5f * high_left && left[126] < 0.5f * high_left); // the fall at output sample 125.8

    // A mute takes the square out at once, and its end brings it back. The mute comes at cycle 262,144, sample 750.0,
    // in a high step. The square is high again from cycle 688,128 to 819,200: samples 1,969.6 to 2,344.8.
    apu.SetMuted(kApuSquare1);
    const auto muted = Enhanced(apu, kFrameCycles);
    XSFLIB_CHECK(muted.first[100] == 0.0f && muted.second[100] == 0.0f);
    apu.SetMuted(0);
    const auto unmuted = Enhanced(apu, kFrameCycles);
    XSFLIB_CHECK(unmuted.first[2200 - 1554] == high_left && unmuted.second[2200 - 1554] == high_right);
}

// An envelope's step changes the level at the frame sequencer's step, between the timer's steps. The frame
// sequencer's step 7 comes at cycle 262,144, sample 750.3, while the square is high from cycle 160,000 to 288,000:
// samples 458.0 to 824.3. Level 15 holds until 33 samples after the high step's start plus the output's delay of 32,
// and level 14 from 33 samples after the envelope's step until 33 before the square falls.
void TestEnhancedEnvelope()
{
    Apu apu = PoweredOn();
    apu.Store(kNr50, 1, 0x77);
    apu.Store(kNr51, 1, 0x11);
    apu.Store(kSoundCntH, 2, 0x0002); // the PSG at 100%
    apu.StartEnhanced();
    apu.Store(kNr11, 1, 0x80);        // 50%
    apu.Store(kNr12, 1, 0xF1);        // level 15, down by 1 at each of the envelope's steps
    apu.Store(kNr13, 2, 0x8000 | 48); // 32,000 cycles a step
    const std::vector<float> left = Enhanced(apu, 2 * kFrameCycles).first;
    bool fifteen = true;
    for (size_t i = 523; i <= 749; ++i)
    {
        fifteen &= left[i] == 15.0f * 1024.0f / 32768.0f;
    }
    bool fourteen = true;
    for (size_t i = 816; i <= 823; ++i)
    {
        fourteen &= left[i] == 14.0f * 1024.0f / 32768.0f;
    }
    XSFLIB_CHECK(fifteen && fourteen);
}

// A square of 50% at 789.6 Hz: its harmonics below 0.84 of the Nyquist frequency keep their amplitudes within 0.01 dB.
// The square spans 15 * 1,024 steps of a 16-bit sample: 0.46875 of full scale.
void TestEnhancedSquare()
{
    Apu apu = PoweredOn();
    apu.Store(kNr51, 1, 0x11);
    apu.StartEnhanced();
    apu.Store(kNr11, 1, 0x80);
    apu.Store(kNr12, 1, 0xF0);
    apu.Store(kNr13, 2, 0x8000 | 1882); // 131,072 / 166 Hz
    const std::vector<float> left = Enhanced(apu, 100 * kFrameCycles).first;
    const std::vector<float> signal(left.begin() + 804, left.begin() + 804 + 65536);
    constexpr double kPi = 3.14159265358979323846;
    const double rate = double(kSecondCycles) * kEnhancedFrameSamples / kFrameCycles;
    const double fundamental = 131072.0 / 166.0;
    double worst = 0.0;
    for (int k = 1; k * fundamental <= 0.84 * rate / 2.0; k += 2)
    {
        double re = 0.0;
        double im = 0.0;
        double gain = 0.0;
        const double n = double(signal.size());
        for (size_t i = 0; i < signal.size(); ++i)
        {
            const double t = 2.0 * kPi * double(i) / n;
            const double window =
                0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
            const double angle = 2.0 * kPi * k * fundamental * double(i) / rate;
            re += window * signal[i] * std::cos(angle);
            im += window * signal[i] * std::sin(angle);
            gain += window;
        }
        const double amplitude = 2.0 * std::hypot(re, im) / gain;
        const double expected = 2.0 * 0.46875 / (k * kPi);
        worst = std::max(worst, std::fabs(20.0 * std::log10(amplitude / expected)));
    }
    XSFLIB_CHECK(worst <= 0.01);
}

// The FIFOs' samples count 2 or 4 times on the sides that SOUNDCNT_H gives them. They start after the band-limited
// steps' delay of 32 samples. A run without output drops the samples of its time, and the PSG then goes on from its
// level.
void TestEnhancedFifos()
{
    Apu apu = PoweredOn();
    apu.Store(kNr51, 1, 0x00);
    apu.Store(kSoundCntH, 2, 0x6106); // A at 100% on the right, B at 50% on the left
    apu.StartEnhanced();
    std::vector<float> ramp(3000);
    for (size_t i = 0; i < ramp.size(); ++i)
    {
        ramp[i] = float(i % 100) - 50.0f;
    }
    const float b[] = {3.0f, -1.0f};
    apu.QueueEnhanced(0, ramp);
    apu.QueueEnhanced(1, b);
    const auto [left, right] = Enhanced(apu, kFrameCycles);
    bool matches = left[31] == 0.0f && right[31] == 0.0f;
    matches &= left[32] * 32768.0f == 3.0f * 128.0f && left[33] * 32768.0f == -128.0f && left[34] == 0.0f;
    for (size_t i = 32; i < left.size(); ++i)
    {
        matches &= right[i] * 32768.0f == ramp[i - 32] * 256.0f;
    }
    XSFLIB_CHECK(matches);

    // A frame without output takes the ramp's next 804 samples.
    apu.Run(kFrameCycles);
    const auto after = Enhanced(apu, kFrameCycles);
    XSFLIB_CHECK(after.second[0] * 32768.0f == ramp[2 * 804 - 32] * 256.0f);
}

// The console's register at `address` in grown PSG unit `unit`.
uint32_t InUnit(int unit, uint32_t address)
{
    return kGrownPsgRegisters + uint32_t(unit - 1) * kGrownPsgBytes + (address - kRegSound1CntL);
}

// Stores a square 1 that sweeps up and a wave channel with its pattern in unit `unit`, and sends both to the sides in
// the unit's NR51. The console's wave RAM takes another pattern.
void StoreGrownNotes(Apu& apu, int unit)
{
    const auto at = [unit](uint32_t address)
    {
        return unit == 0 ? address : InUnit(unit, address);
    };
    for (uint32_t i = 0; i < 16; i += 4)
    {
        apu.Store(kWaveRam + i, 4, 0x01234567u + i);
    }
    apu.Store(at(kNr30), 1, 0x40);
    for (uint32_t i = 0; i < 16; i += 4)
    {
        apu.Store(at(kWaveRam + i), 4, 0xFEDC9876u - 0x00110011u * i);
    }
    apu.Store(at(kNr30), 1, 0x80);
    apu.Store(at(kNr32), 1, 0x20);
    apu.Store(at(kNr33), 2, 0x8000 | 1800);
    apu.Store(at(kNr10), 1, 0x17);
    apu.Store(at(kNr11), 1, 0x80);
    apu.Store(at(kNr12), 1, 0xF1);
    apu.Store(at(kNr13), 2, 0x8000 | 1500);
    apu.Store(at(kNr51), 1, 0x15);
}

// A grown unit's channels play as the console's do, with their sweep, envelope and wave RAM. The unit's NR51 sends them
// to the sides. SetMutedUnits mutes the unit alone, and UnitPeaks gives the unit's peaks apart from the console's.
void TestGrownUnits()
{
    for (const bool enhanced : {false, true})
    {
        Apu console = PoweredOn();
        Apu grown = PoweredOn();
        console.MeterChannels(true);
        grown.MeterChannels(true);
        if (enhanced)
        {
            console.StartEnhanced();
            grown.StartEnhanced();
        }
        console.Store(kNr51, 1, 0x00);
        grown.Store(kNr51, 1, 0x00);
        StoreGrownNotes(console, 0);
        StoreGrownNotes(grown, 1);
        XSFLIB_CHECK(console.PsgUnits() == 1);
        XSFLIB_CHECK(grown.PsgUnits() == 2);
        XSFLIB_CHECK(grown.Register(InUnit(1, kNr51)) == 0x15);
        XSFLIB_CHECK(grown.Register(InUnit(1, kSoundCntX)) == 0x05);
        XSFLIB_CHECK(grown.Register(kSoundCntX) == 0x80);

        if (enhanced)
        {
            const auto a = Enhanced(console, 30 * kFrameCycles);
            const auto b = Enhanced(grown, 30 * kFrameCycles);
            XSFLIB_CHECK(a == b);
            XSFLIB_CHECK(*std::max_element(a.first.begin(), a.first.end()) > 0.1f);
        }
        else
        {
            std::vector<int16_t> a;
            std::vector<int16_t> b;
            console.Run(30 * kFrameCycles, a);
            grown.Run(30 * kFrameCycles, b);
            XSFLIB_CHECK(a == b);
            XSFLIB_CHECK(*std::max_element(a.begin(), a.end()) > 0);
        }
        for (int c = 0; c < 4; ++c)
        {
            XSFLIB_CHECK(grown.UnitPeaks(1, c) == console.ChannelPeaks(c));
            XSFLIB_CHECK((grown.ChannelPeaks(c) == std::array<int32_t, 2>{}));
        }

        // A mute of the unit's square 1 and wave leaves the output silent.
        const uint8_t mutes[] = {uint8_t(kApuSquare1 | 0x04)};
        grown.SetMutedUnits(mutes);
        if (enhanced)
        {
            const auto quiet = Enhanced(grown, 10 * kFrameCycles);
            XSFLIB_CHECK(std::all_of(quiet.first.begin() + 200, quiet.first.end(), [](float x) { return x == 0.0f; }));
        }
        else
        {
            std::vector<int16_t> quiet;
            grown.Run(kFrameCycles, quiet);
            XSFLIB_CHECK(std::all_of(quiet.begin(), quiet.end(), [](int16_t x) { return x == 0; }));
        }
    }
}

// PsgSilent(): a channel that's off holds no sound. Nor does one at a level of 0, unless its envelope rises from there.
// A wave channel holds its volume in NR32.
void TestPsgSilent()
{
    Apu off = PoweredOn();
    XSFLIB_CHECK(off.PsgSilent());

    // A note at a level of 15, without an envelope.
    Apu square = PoweredOn();
    square.Store(kNr11, 1, 0x80);
    square.Store(kNr12, 1, 0xF0);
    square.Store(kNr13, 2, 0x8400);
    XSFLIB_CHECK(!square.PsgSilent());

    // At a period of 1, the envelope falls by 1 every 8 of the frame sequencer's steps: 15 times from a level of 15.
    Apu falling = PoweredOn();
    falling.Store(kNr21, 1, 0x80);
    falling.Store(kNr22, 1, 0xF1);
    falling.Store(kNr23, 2, 0x8400);
    XSFLIB_CHECK(!falling.PsgSilent());
    falling.Run(14 * 8 * kSequencerCycles);
    XSFLIB_CHECK(!falling.PsgSilent());
    falling.Run(2 * 8 * kSequencerCycles);
    XSFLIB_CHECK(falling.PsgSilent());

    // From a level of 0, an envelope with a period rises. Without a period, it stays at 0. NR42's bit 3 keeps the
    // channel on at that level.
    Apu rising = PoweredOn();
    rising.Store(kNr42, 1, 0x09);
    rising.Store(kNr43, 2, 0x8000);
    XSFLIB_CHECK(!rising.PsgSilent());
    Apu stopped = PoweredOn();
    stopped.Store(kNr42, 1, 0x08);
    stopped.Store(kNr43, 2, 0x8000);
    XSFLIB_CHECK(stopped.PsgSilent());

    // A wave channel plays at the volume of NR32's high bits, and a volume of 0 holds it at a constant.
    Apu wave = PoweredOn();
    wave.Store(kNr30, 1, 0x80);
    wave.Store(kNr32, 1, 0x20);
    wave.Store(kNr33, 2, 0x8400);
    XSFLIB_CHECK(!wave.PsgSilent());
    wave.Store(kNr32, 1, 0x00);
    XSFLIB_CHECK(wave.PsgSilent());
    wave.Store(kNr32, 1, 0x80);
    XSFLIB_CHECK(!wave.PsgSilent());
    wave.Store(kNr30, 1, 0x00);
    XSFLIB_CHECK(wave.PsgSilent());

    // A grown unit's channels count too.
    Apu grown = PoweredOn();
    StoreGrownNotes(grown, 1);
    XSFLIB_CHECK(grown.PsgUnits() == 2);
    XSFLIB_CHECK(!grown.PsgSilent());
}

void TestSequencerRange()
{
    Apu apu;
    int refused = 0;
    for (const auto& [step, cycles] : {std::pair<int, uint32_t>{8, 1}, {-1, 1}, {0, 0}, {0, kSequencerCycles + 1}})
    {
        try
        {
            apu.SetSequencer(step, cycles);
        }
        catch (const std::invalid_argument&)
        {
            ++refused;
        }
    }
    XSFLIB_CHECK(refused == 4);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("agb_apu",
                                    []
                                    {
                                        TestSquareDuties();
                                        TestSquareFrequency();
                                        TestEnvelope();
                                        TestLength();
                                        TestSweep();
                                        TestWave();
                                        TestWaveRestartAndStop();
                                        TestNoise();
                                        TestFifos();
                                        TestOutputStage();
                                        TestRate();
                                        TestPower();
                                        TestRegisters();
                                        TestMutes();
                                        TestPsgSilent();
                                        TestSequencerRange();
                                        TestGrownUnits();
                                        TestEnhancedSamples();
                                        TestEnhancedVolumes();
                                        TestEnhancedEnvelope();
                                        TestEnhancedSquare();
                                        TestEnhancedFifos();
                                    });
}
