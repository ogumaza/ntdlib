// SPDX-License-Identifier: MIT

// Tests of the DS's sound unit (ntdlib/ntr/sound.h). The expected values follow GBATEK's description of the channels,
// the mixer's steps and the captures' format, worked out by hand. The captures' timing is the model's (sound.h). The
// enhanced mixing mode's tests take GBATEK's gains in floating point and the mode's rules in sound.h: the waves'
// samples at the channel's place, the steps' band limit and the delay of 32 samples.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/sound.h"
#include "ntdlib/wave/adpcm.h"

namespace
{

using ntdlib::ntr::SoundOutput;
using ntdlib::ntr::SoundUnit;
using ntdlib::wave::ImaAdpcmState;

constexpr uint32_t kBusy = 1u << 31;
constexpr uint16_t kBias = 0x200;
constexpr uint32_t kWaveAddress = 0x02000100;

// A timer whose period is one output sample.
constexpr uint32_t kOneSamplePeriod = 0x10000 - ntdlib::ntr::kTicksPerSample;

enum Format : uint32_t
{
    kPcm8 = 0u << 29,
    kPcm16 = 1u << 29,
    kAdpcm = 2u << 29,
    kPsg = 3u << 29,
};

constexpr uint32_t kLoop = 1u << 27;
constexpr uint32_t kOneShot = 2u << 27;

// The captures' registers, and a capture buffer.
constexpr uint32_t kCaptureControl = 0x04000508;
constexpr uint32_t kCaptureAddress = 0x04000510;
constexpr uint32_t kCaptureLength = 0x04000514;
constexpr uint8_t kCaptureStart = 0x80;
constexpr uint8_t kCaptureOneShot = 0x04;
constexpr uint8_t kCapturePcm8 = 0x08;
constexpr uint32_t kBufferAddress = 0x02002000;

uint32_t ChannelAddress(int channel, uint32_t offset)
{
    return ntdlib::ntr::kSoundRegisters + static_cast<uint32_t>(channel) * 0x10 + offset;
}

// A unit with the master volume at 127, the bias at 0x200 and the output on.
struct Unit
{
    explicit Unit(std::vector<uint8_t> wave = {}) : memory(0x10000), unit(memory)
    {
        for (std::size_t i = 0; i < wave.size(); i++)
        {
            memory[kWaveAddress - 0x02000000 + i] = wave[i];
        }
        unit.Write(0x04000500, 4, 0x807F);
        unit.Write(0x04000504, 4, kBias);
    }

    // Sets up and starts a channel: SAD, TMR, PNT and LEN first, then SOUNDxCNT with the busy bit.
    void Start(int channel, uint32_t control, uint32_t loop_words, uint32_t words, uint32_t timer = kOneSamplePeriod)
    {
        unit.Write(ChannelAddress(channel, 0x4), 4, kWaveAddress);
        unit.Write(ChannelAddress(channel, 0x8), 4, timer | loop_words << 16);
        unit.Write(ChannelAddress(channel, 0xC), 4, words);
        unit.Write(ChannelAddress(channel, 0x0), 4, control | kBusy);
    }

    std::vector<SoundOutput> Render(int samples)
    {
        std::vector<SoundOutput> out;
        unit.Run(static_cast<uint64_t>(samples) * ntdlib::ntr::kTicksPerSample, out);
        return out;
    }

    // The enhanced mode's output for `frames` frames, the left and the right side in turn.
    std::vector<float> RenderEnhanced(int frames)
    {
        std::vector<float> out;
        for (int frame = 0; frame < frames; frame++)
        {
            unit.Run(ntdlib::ntr::kEnhancedFrameTicks, out);
        }
        return out;
    }

    // Sets up and starts capture `capture` at `address`: SNDCAPxDAD and SNDCAPxLEN first, then SNDCAPxCNT with the
    // start bit. The capture counts with channel 1's or channel 3's timer, which takes `timer`.
    void StartCapture(int capture, uint8_t control, uint32_t words, uint32_t timer = kOneSamplePeriod,
                      uint32_t address = kBufferAddress)
    {
        const auto at = static_cast<uint32_t>(capture);
        unit.Write(ChannelAddress(capture * 2 + 1, 0x8), 2, timer);
        unit.Write(kCaptureAddress + at * 8, 4, address);
        unit.Write(kCaptureLength + at * 8, 4, words);
        unit.Write(kCaptureControl + at, 1, control | kCaptureStart);
    }

    // The 16-bit sample at `index` in the capture buffer.
    int16_t Captured(int index) const
    {
        const std::size_t at = kBufferAddress - 0x02000000 + 2 * static_cast<std::size_t>(index);
        return static_cast<int16_t>(memory[at] | memory[at + 1] << 8);
    }

    std::vector<uint8_t> memory;
    SoundUnit unit;
};

std::vector<uint8_t> Pcm16(const std::vector<int16_t>& samples)
{
    std::vector<uint8_t> bytes;
    for (const int16_t sample : samples)
    {
        bytes.push_back(static_cast<uint8_t>(sample & 0xFF));
        bytes.push_back(static_cast<uint8_t>((sample >> 8) & 0xFF));
    }

    return bytes;
}

// The left side of a sample at volume 127 and pan 0, with the master volume at 127: the sample over 64, rounded down.
uint16_t Left(int16_t sample)
{
    return static_cast<uint16_t>(kBias + (sample >> 6));
}

void TestAdpcmStep()
{
    // The DS stops a negative step at -0x7FFF. The IMA's decoder stops at -0x8000.
    const ImaAdpcmState low{-0x7FF0, 88};

    const ImaAdpcmState next = ntdlib::ntr::AdpcmStep(low, 15);

    XSFLIB_CHECK(next.sample == -0x7FFF && next.index == 88);
    XSFLIB_CHECK(ntdlib::wave::ImaAdpcmNext(low, 15).sample == -0x8000);
    // A positive step from -0x8000 has no lower clamp. Index 0's step of 7 adds 7 / 8 = 0 for nibble 0.
    XSFLIB_CHECK(ntdlib::ntr::AdpcmStep({-0x8000, 0}, 0).sample == -0x8000);
    XSFLIB_CHECK(ntdlib::ntr::AdpcmStep({-0x8000, 0}, 8).sample == -0x7FFF);
    // Past index 88, the table of steps holds 0, and the next index stops at 88.
    XSFLIB_CHECK(ntdlib::ntr::AdpcmStep({100, 100}, 3) == (ImaAdpcmState{100, 88}));
    XSFLIB_CHECK(ntdlib::ntr::AdpcmStep({-0x8000, 120}, 8) == (ImaAdpcmState{-0x7FFF, 88}));
    XSFLIB_CHECK(ntdlib::ntr::AdpcmHeader(0x00598001) == (ImaAdpcmState{-0x7FFF, 0x59}));
}

void TestRegisters()
{
    Unit unit;

    unit.unit.Write(ChannelAddress(0, 0), 4, 0xFFFFFFFF);
    unit.unit.Write(ChannelAddress(0, 4), 4, 0xFFFFFFFF);
    unit.unit.Write(0x04000500, 4, 0xFFFFFFFF);
    unit.unit.Write(0x04000504, 4, 0xFFFFFFFF);

    XSFLIB_CHECK(unit.unit.Read(ChannelAddress(0, 0), 4) == 0xFF7F837F);
    XSFLIB_CHECK(unit.unit.Read(ChannelAddress(0, 2), 1) == 0x7F);
    XSFLIB_CHECK(unit.unit.Read(ChannelAddress(0, 4), 4) == 0);
    XSFLIB_CHECK(unit.unit.Read(0x04000500, 4) == 0xBF7F);
    XSFLIB_CHECK(unit.unit.Read(0x04000504, 2) == 0x3FF);
    XSFLIB_CHECK(unit.unit.Busy(0));
}

void TestOneShot()
{
    const std::vector<int16_t> samples = {0x1000, -0x2000, 0x4000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080};
    Unit unit(Pcm16(samples));
    unit.Start(2, 127 | kOneShot | kPcm16, 0, 4);

    // PCM starts after 3 periods of the timer: the third overflow brings the first sample.
    const std::vector<SoundOutput> start = unit.Render(9);
    XSFLIB_CHECK(unit.unit.Busy(2));
    const std::vector<SoundOutput> last = unit.Render(1);
    XSFLIB_CHECK(!unit.unit.Busy(2));
    const std::vector<SoundOutput> after = unit.Render(2);

    XSFLIB_CHECK(start[0] == (SoundOutput{kBias, kBias}) && start[1] == (SoundOutput{kBias, kBias}));
    for (std::size_t i = 0; i < 7; i++)
    {
        XSFLIB_CHECK(start[i + 2] == (SoundOutput{Left(samples[i]), kBias}));
    }
    XSFLIB_CHECK(Left(-0x2000) == 0x180);
    // The busy bit clears as the last sample starts. The sample plays to the end of its period.
    XSFLIB_CHECK(last[0] == (SoundOutput{Left(samples[7]), kBias}));
    XSFLIB_CHECK(after[0] == (SoundOutput{kBias, kBias}) && after[1] == (SoundOutput{kBias, kBias}));
}

void TestHold()
{
    Unit unit(Pcm16({0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x2000}));
    unit.Start(0, 127 | (1u << 15) | kOneShot | kPcm16, 0, 4);

    const std::vector<SoundOutput> out = unit.Render(14);

    XSFLIB_CHECK(!unit.unit.Busy(0));
    XSFLIB_CHECK(out[9] == (SoundOutput{Left(0x2000), kBias}));
    XSFLIB_CHECK(out[13] == (SoundOutput{Left(0x2000), kBias}));
}

void TestPcm8Loop()
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 16; i++)
    {
        bytes.push_back(static_cast<uint8_t>(i * 8 - 64));
    }
    Unit unit(bytes);
    // 2 words of 4 samples before the loop, and 2 words in it.
    unit.Start(15, 127 | kLoop | kPcm8, 2, 2);

    const std::vector<SoundOutput> out = unit.Render(2 + 16 + 8 + 8);

    std::vector<int> expected;
    for (int i = 0; i < 16; i++)
    {
        expected.push_back(i);
    }
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 8; i < 16; i++)
        {
            expected.push_back(i);
        }
    }
    for (std::size_t i = 0; i < expected.size(); i++)
    {
        const int8_t byte = static_cast<int8_t>(bytes[static_cast<std::size_t>(expected[i])]);
        XSFLIB_CHECK(out[i + 2] == (SoundOutput{Left(static_cast<int16_t>(byte * 256)), kBias}));
    }
    XSFLIB_CHECK(unit.unit.Busy(15));
}

void TestAdpcmLoop()
{
    // The header gives the sample 0x123 and the index 20. 3 words of nibbles follow: 8 samples before the loop, and 16
    // in it.
    std::vector<uint8_t> bytes = {0x23, 0x01, 20, 0};
    const std::vector<uint8_t> nibbles = {0x71, 0x9C, 0x05, 0xF3, 0x48, 0xB2, 0x6E, 0xD0, 0x37, 0xA1, 0x5F, 0x0C};
    bytes.insert(bytes.end(), nibbles.begin(), nibbles.end());
    Unit unit(bytes);
    unit.Start(4, 127 | kLoop | kAdpcm, 2, 2);

    const std::vector<SoundOutput> out = unit.Render(10 + 24 + 16);

    // The channel decodes each nibble in turn, and takes the decoder's state before sample 8 again at each loop.
    std::vector<int16_t> expected;
    ImaAdpcmState state{0x123, 20};
    ImaAdpcmState loop_state;
    for (int i = 0; i < 24; i++)
    {
        if (i == 8)
        {
            loop_state = state;
        }
        const uint8_t byte = nibbles[static_cast<std::size_t>(i / 2)];
        state = ntdlib::ntr::AdpcmStep(state, (i & 1) ? byte >> 4 : byte & 0x0F);
        expected.push_back(state.sample);
    }
    state = loop_state;
    for (int i = 8; i < 24; i++)
    {
        const uint8_t byte = nibbles[static_cast<std::size_t>(i / 2)];
        state = ntdlib::ntr::AdpcmStep(state, (i & 1) ? byte >> 4 : byte & 0x0F);
        expected.push_back(state.sample);
    }

    // ADPCM starts after 11 periods: the 11th overflow brings the first sample.
    XSFLIB_CHECK(out[9] == (SoundOutput{kBias, kBias}));
    for (std::size_t i = 0; i < expected.size(); i++)
    {
        XSFLIB_CHECK(out[i + 10] == (SoundOutput{Left(expected[i]), kBias}));
    }
}

void TestPsg()
{
    Unit unit;
    // A duty of 3: 4 low samples, then 4 high, at the centre: each side at half the level.
    unit.Start(8, 127 | (64u << 16) | (3u << 24) | kPsg, 0, 0);
    unit.Start(9, 127 | (64u << 16) | (7u << 24) | kPsg, 0, 0, 0x10000 - 2 * ntdlib::ntr::kTicksPerSample);
    unit.Start(0, 127 | kPsg, 0, 0);

    const std::vector<SoundOutput> out = unit.Render(9);

    // Each channel's low level, -0x7FFF, gives -255.99 for each side, and the high level 255.99. The mixer adds the
    // channels before the master volume rounds down. Duty 7 holds the low level, and its timer steps each second
    // sample: channel 9 joins at the second sample. Channel 0 plays nothing.
    const std::vector<uint16_t> expected = {kBias - 256, 0, 0, 0, kBias, kBias, kBias, kBias, 0};
    for (std::size_t i = 0; i < expected.size(); i++)
    {
        XSFLIB_CHECK(out[i] == (SoundOutput{expected[i], expected[i]}));
    }
    XSFLIB_CHECK(unit.unit.Busy(0));
}

void TestNoise()
{
    Unit unit;
    unit.Start(14, 127 | kPsg, 0, 0);
    unit.Start(15, 127 | (127u << 16) | kPsg, 0, 0);

    const std::vector<SoundOutput> out = unit.Render(40);

    // GBATEK's sequence: X starts at 0x7FFF. Each step shifts X right. A carry gives the low level and X ^= 0x6000.
    uint32_t x = 0x7FFF;
    for (const SoundOutput& sample : out)
    {
        const bool carry = (x & 1) != 0;
        x >>= 1;
        if (carry)
        {
            x ^= 0x6000;
        }
        const uint16_t level = carry ? 0 : 0x3FF;
        XSFLIB_CHECK(sample == (SoundOutput{level, level}));
    }
}

void TestMixer()
{
    Unit unit(Pcm16(std::vector<int16_t>(8, 0x4000)));
    for (int i = 0; i < 8; i++)
    {
        unit.memory[0x200 + 2 * static_cast<std::size_t>(i)] = 0x00;
        unit.memory[0x201 + 2 * static_cast<std::size_t>(i)] = 0xF0; // -0x1000
    }
    unit.unit.Write(0x04000500, 4, 0x8000 | 100);
    // 0x4000 at volume 64, divided by 2, at pan 96: 4096, then 1024 left and 3072 right.
    unit.Start(3, 64 | (1u << 8) | (96u << 16) | kLoop | kPcm16, 0, 4);
    // -0x1000 at volume 127, divided by 16, at pan 127: -256, all of it right.
    unit.unit.Write(ChannelAddress(5, 0x4), 4, kWaveAddress + 0x100);
    unit.unit.Write(ChannelAddress(5, 0x8), 4, kOneSamplePeriod);
    unit.unit.Write(ChannelAddress(5, 0xC), 4, 4);
    unit.unit.Write(ChannelAddress(5, 0x0), 4, 127 | (3u << 8) | (127u << 16) | kLoop | kPcm16 | kBusy);

    std::vector<SoundOutput> out = unit.Render(4);

    // The master volume of 100: 1024 * 100 / 8192 = 12.5, and (3072 - 256) * 100 / 8192 = 34.375, both rounded down.
    XSFLIB_CHECK(out[3] == (SoundOutput{kBias + 12, kBias + 34}));

    // Without the master enable bit, the output is the bias.
    unit.unit.Write(0x04000500, 4, 100);
    out = unit.Render(1);
    XSFLIB_CHECK(out[0] == (SoundOutput{kBias, kBias}));

    // The left side from channel 3 alone, and the right side from the mixer without channel 3.
    unit.unit.Write(0x04000500, 4, 0x8000 | (1u << 13) | (2u << 8) | 100);
    out = unit.Render(1);
    XSFLIB_CHECK(out[0] == (SoundOutput{kBias + 12, kBias - 4}));

    // A bias of 0x3F0 clips at 0x3FF.
    unit.unit.Write(0x04000500, 4, 0x8000 | 100);
    unit.unit.Write(0x04000504, 4, 0x3F0);
    out = unit.Render(1);
    XSFLIB_CHECK(out[0] == (SoundOutput{0x3FC, 0x3FF}));
}

// The host's mutes and meters, on TestMixer's channels. Before the master volume, channel 3 gives 262,144 left and
// 786,432 right, and channel 5 gives -65,536 right. A meter scales its sum by the master volume of 100 over 2^15.
void TestMutesAndMeters()
{
    Unit unit(Pcm16(std::vector<int16_t>(8, 0x4000)));
    for (int i = 0; i < 8; i++)
    {
        unit.memory[0x200 + 2 * static_cast<std::size_t>(i)] = 0x00;
        unit.memory[0x201 + 2 * static_cast<std::size_t>(i)] = 0xF0;
    }
    unit.unit.Write(0x04000500, 4, 0x8000 | 100);
    unit.Start(3, 64 | (1u << 8) | (96u << 16) | kLoop | kPcm16, 0, 4);
    unit.unit.Write(ChannelAddress(5, 0x4), 4, kWaveAddress + 0x100);
    unit.unit.Write(ChannelAddress(5, 0x8), 4, kOneSamplePeriod);
    unit.unit.Write(ChannelAddress(5, 0xC), 4, 4);
    unit.unit.Write(ChannelAddress(5, 0x0), 4, 127 | (3u << 8) | (127u << 16) | kLoop | kPcm16 | kBusy);

    // The meters change nothing in the output: 262,144 * 100 / 2^15 = 800, 786,432 * 100 / 2^15 = 2,400, and
    // 65,536 * 100 / 2^15 = 200.
    std::array<int8_t, 16> meters;
    meters.fill(-1);
    meters[3] = 0;
    meters[5] = 2;
    unit.unit.SetMeters(meters);
    std::vector<SoundOutput> out = unit.Render(4);
    XSFLIB_CHECK(out[3] == (SoundOutput{kBias + 12, kBias + 34}));
    XSFLIB_CHECK(unit.unit.MeterPeaks()[0] == (std::array<int32_t, 2>{800, 2400}));
    XSFLIB_CHECK(unit.unit.MeterPeaks()[2] == (std::array<int32_t, 2>{0, 200}));
    XSFLIB_CHECK(unit.unit.MeterPeaks()[1] == (std::array<int32_t, 2>{0, 0}));

    // A muted channel leaves the mix and reaches no meter: 786,432 * 100 / 2^21 = 37.5, rounded down.
    unit.unit.SetMutedChannels(1u << 5);
    unit.unit.SetMeters(meters);
    out = unit.Render(1);
    XSFLIB_CHECK(out[0] == (SoundOutput{kBias + 12, kBias + 37}));
    XSFLIB_CHECK(unit.unit.MeterPeaks()[0] == (std::array<int32_t, 2>{800, 2400}));
    XSFLIB_CHECK(unit.unit.MeterPeaks()[2] == (std::array<int32_t, 2>{0, 0}));

    // Without the master enable bit, the meters stay at 0.
    unit.unit.SetMutedChannels(0);
    unit.unit.Write(0x04000500, 4, 100);
    unit.unit.SetMeters(meters);
    out = unit.Render(1);
    XSFLIB_CHECK(unit.unit.MeterPeaks()[0] == (std::array<int32_t, 2>{0, 0}));
}

// A run without mixing leaves the channels where a mixed run does, and the meters as they were.
void TestRunWithoutMixing()
{
    std::vector<int16_t> ramp;
    for (int i = 0; i < 64; i++)
    {
        ramp.push_back(static_cast<int16_t>(i * 0x100));
    }
    Unit mixed(Pcm16(ramp));
    Unit skipped(Pcm16(ramp));
    mixed.Start(2, 127 | kOneShot | kPcm16, 0, 32);
    skipped.Start(2, 127 | kOneShot | kPcm16, 0, 32);
    std::array<int8_t, 16> meters;
    meters.fill(-1);
    meters[2] = 4;
    skipped.unit.SetMeters(meters);

    const std::vector<SoundOutput> first = mixed.Render(40);
    skipped.unit.Run(40 * ntdlib::ntr::kTicksPerSample);
    XSFLIB_CHECK(skipped.unit.MeterPeaks()[4] == (std::array<int32_t, 2>{0, 0}));
    XSFLIB_CHECK(skipped.unit.Phase() == mixed.unit.Phase());
    const std::vector<SoundOutput> rest = skipped.Render(40);
    XSFLIB_CHECK(mixed.Render(40) == rest);
    XSFLIB_CHECK(first[39] != (SoundOutput{kBias, kBias}));

    // The meter keeps the ramp's peak after the wave has ended. The output holds that peak over 64, rounded down.
    int32_t highest = 0;
    for (const SoundOutput& sample : rest)
    {
        highest = std::max(highest, (sample.left - kBias) * 64);
    }
    const int32_t peak = skipped.unit.MeterPeaks()[4][0];
    XSFLIB_CHECK(rest.back() == (SoundOutput{kBias, kBias}));
    XSFLIB_CHECK(highest > 0 && peak >= highest && peak < highest + 64);
}

void TestShortSound()
{
    Unit unit(Pcm16(std::vector<int16_t>(8, 0x4000)));
    // Fewer than 4 words: the channel stays busy and plays nothing.
    unit.Start(6, 127 | kOneShot | kPcm16, 1, 2);

    const std::vector<SoundOutput> out = unit.Render(20);

    XSFLIB_CHECK(unit.unit.Busy(6));
    XSFLIB_CHECK(out[19] == (SoundOutput{kBias, kBias}));
}

void TestStop()
{
    Unit unit(Pcm16(std::vector<int16_t>(8, 0x4000)));
    unit.Start(7, 127 | kLoop | kPcm16, 0, 4);
    std::vector<SoundOutput> out = unit.Render(4);
    XSFLIB_CHECK(out[3] == (SoundOutput{Left(0x4000), kBias}));

    // A write of the busy bit to a busy channel doesn't start it again. A write that clears it stops the channel.
    unit.unit.Write(ChannelAddress(7, 3), 1, unit.unit.Read(ChannelAddress(7, 3), 1) | 0x80);
    out = unit.Render(1);
    XSFLIB_CHECK(out[0] == (SoundOutput{Left(0x4000), kBias}));
    unit.unit.Write(ChannelAddress(7, 0), 4, 127 | kLoop | kPcm16);
    out = unit.Render(1);
    XSFLIB_CHECK(!unit.unit.Busy(7));
    XSFLIB_CHECK(out[0] == (SoundOutput{kBias, kBias}));
}

// Captures one channel that plays `sample` for ever at volume 127 and pan `pan`, from channel `channel`. Capture 0
// takes the left side and capture 1 the right. The capture's 2-word buffer fills with the sample's value.
std::vector<uint8_t> CaptureOf(int16_t sample, uint32_t pan, int capture, uint8_t control, int channels = 1)
{
    Unit unit(Pcm16(std::vector<int16_t>(8, sample)));
    for (int channel = 0; channel < channels; channel++)
    {
        unit.Start(channel * 2, 127 | (pan << 16) | kLoop | kPcm16, 0, 4);
    }
    unit.StartCapture(capture, control, 2);
    unit.Render(20);
    const auto start = static_cast<std::ptrdiff_t>(kBufferAddress - 0x02000000);
    return {unit.memory.begin() + start, unit.memory.begin() + start + 8};
}

std::vector<uint8_t> Repeated(std::vector<uint8_t> bytes, int times)
{
    std::vector<uint8_t> out;
    for (int i = 0; i < times; i++)
    {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    return out;
}

void TestCaptureValues()
{
    // At pan 64, a sample of 3 gives each side 3 * 16 * 128 * 64 / 1024 = 384: 1.5 in the mixer's 16.8 bits. A
    // positive value rounds down. A negative one whose fraction's top bit is set rounds up: -1.5 gives -1.
    XSFLIB_CHECK(CaptureOf(3, 64, 0, 0) == Repeated({0x01, 0x00}, 4));
    XSFLIB_CHECK(CaptureOf(3, 64, 1, 0) == Repeated({0x01, 0x00}, 4));
    XSFLIB_CHECK(CaptureOf(-3, 64, 0, 0) == Repeated({0xFF, 0xFF}, 4));
    // At pan 96 the left side takes 32/128: -5 gives -320, -1.25 or -2 plus 0.75, and rounds up to -1. -15 gives
    // -960, -3.75 or -4 plus 0.25, and stays at -4.
    XSFLIB_CHECK(CaptureOf(-5, 96, 0, 0) == Repeated({0xFF, 0xFF}, 4));
    XSFLIB_CHECK(CaptureOf(-15, 96, 0, 0) == Repeated({0xFC, 0xFF}, 4));
    // Two channels at the limits pass the 16.8 bits, and clip to 0x7FFF and -0x8000.
    XSFLIB_CHECK(CaptureOf(0x7FFF, 0, 0, 0, 2) == Repeated({0xFF, 0x7F}, 4));
    XSFLIB_CHECK(CaptureOf(-0x8000, 0, 0, 0, 2) == Repeated({0x00, 0x80}, 4));
    // PCM8 takes the 16.8 bits over 0x100. -384 is -1 plus 0xFE80 / 0x10000, and rounds up to 0. -0x400000 gives
    // -0x40. 0x408000 gives 0x40.
    XSFLIB_CHECK(CaptureOf(-3, 64, 0, kCapturePcm8) == std::vector<uint8_t>(8, 0x00));
    XSFLIB_CHECK(CaptureOf(-0x4000, 0, 0, kCapturePcm8) == std::vector<uint8_t>(8, 0xC0));
    XSFLIB_CHECK(CaptureOf(0x4080, 0, 0, kCapturePcm8) == std::vector<uint8_t>(8, 0x40));
    XSFLIB_CHECK(CaptureOf(0x7FFF, 0, 0, kCapturePcm8, 2) == std::vector<uint8_t>(8, 0x7F));

    // The captures take the mixer before the master volume of 10, and leave out a muted channel.
    for (const uint16_t muted : {0, 1 << 2})
    {
        Unit unit(Pcm16(std::vector<int16_t>(8, 0x100)));
        unit.unit.Write(0x04000500, 4, 0x8000 | 10);
        unit.Start(2, 127 | (127u << 16) | kLoop | kPcm16, 0, 4);
        unit.StartCapture(1, 0, 2);
        unit.unit.SetMutedChannels(muted);
        unit.Render(20);
        XSFLIB_CHECK(unit.Captured(0) == (muted ? 0 : 0x100) && unit.Captured(3) == (muted ? 0 : 0x100));
        XSFLIB_CHECK(unit.unit.Read(kCaptureControl, 2) == 0x8000);
    }

    // SOUNDCNT's bit 12 takes channel 1 out of the mixer, and bit 13 channel 3. The captures take the mixer without
    // them. Channel 0 and the other channel each give the left side 0x100.
    for (const int channel : {1, 3})
    {
        for (const bool out : {false, true})
        {
            Unit unit(Pcm16(std::vector<int16_t>(8, 0x100)));
            unit.unit.Write(0x04000500, 4, 0x807F | (out ? 1u << (channel == 1 ? 12 : 13) : 0));
            unit.Start(0, 127 | kLoop | kPcm16, 0, 4);
            unit.Start(channel, 127 | kLoop | kPcm16, 0, 4);
            unit.StartCapture(0, 0, 2);
            unit.Render(20);
            XSFLIB_CHECK(unit.Captured(0) == (out ? 0x100 : 0x200) && unit.Captured(3) == (out ? 0x100 : 0x200));
        }
    }
}

// A capture takes its sample at each overflow of its timer, after the channels have stepped. Channel 0 plays its first
// sample at the third overflow. The capture takes a sample from the first overflow on, into slots 0 to 3 in turn.
// GBATEK gives the registers' bits.
void TestCaptureBuffer()
{
    const std::vector<int16_t> ramp = {1, 2, 3, 4, 5, 6, 7, 8};
    Unit looped(Pcm16(ramp));
    looped.Start(0, 127 | kLoop | kPcm16, 0, 4);
    looped.StartCapture(0, 0, 2);
    looped.Render(10);
    // Overflows 7 to 10 fill slots 2, 3, 0 and 1 with the ramp's samples 4 to 7.
    XSFLIB_CHECK(looped.Captured(0) == 7 && looped.Captured(1) == 8 && looped.Captured(2) == 5 &&
                 looped.Captured(3) == 6);
    XSFLIB_CHECK(looped.unit.Read(kCaptureControl, 1) == kCaptureStart);

    // A timer of half an output sample's period overflows twice in each output sample, and the capture takes a sample
    // at each overflow: 5 output samples hold the same 10 overflows. SNDCAPxDAD drops the address's low 2 bits.
    constexpr uint32_t kHalfSamplePeriod = 0x10000 - ntdlib::ntr::kTicksPerSample / 2;
    Unit fast(Pcm16(ramp));
    fast.Start(0, 127 | kLoop | kPcm16, 0, 4, kHalfSamplePeriod);
    fast.StartCapture(0, 0, 2, kHalfSamplePeriod, kBufferAddress | 3);
    fast.Render(5);
    XSFLIB_CHECK(fast.Captured(0) == 7 && fast.Captured(1) == 8 && fast.Captured(2) == 5 && fast.Captured(3) == 6);
    XSFLIB_CHECK(fast.unit.Read(kCaptureAddress, 4) == kBufferAddress);

    // A write that clears the start bit stops the capture.
    looped.unit.Write(kCaptureControl, 1, 0);
    looped.Render(4);
    XSFLIB_CHECK(looped.Captured(0) == 7 && looped.Captured(1) == 8);

    // A one-shot capture clears its start bit when its buffer is full, and writes nothing past it. A length of 0
    // counts as 1 word.
    Unit once(Pcm16(ramp));
    std::fill_n(once.memory.begin() + (kBufferAddress - 0x02000000), 16, 0xAA);
    once.Start(0, 127 | kLoop | kPcm16, 0, 4);
    once.StartCapture(0, kCaptureOneShot, 0);
    once.Render(1);
    XSFLIB_CHECK(once.unit.Read(kCaptureControl, 1) == (kCaptureStart | kCaptureOneShot));
    once.Render(9);
    XSFLIB_CHECK(once.unit.Read(kCaptureControl, 1) == kCaptureOneShot);
    XSFLIB_CHECK(once.Captured(0) == 0 && once.Captured(1) == 0);
    XSFLIB_CHECK(once.Captured(2) == static_cast<int16_t>(0xAAAA));
}

// The SDK's reverb: channel 1 plays capture 0's buffer in a loop, with the same timer, at volume 30 and pan 0. An
// impulse on channel 0 comes back each pass of the 32-sample buffer, 34 samples later: the channel reads its words 8
// words ahead, so it plays what the capture took one pass before, two overflows after the capture took it.
void TestReverb()
{
    std::vector<int16_t> impulse(8, 0);
    impulse[0] = 0x4000;
    const auto setup = [&](Unit& unit)
    {
        unit.Start(0, 127 | kOneShot | kPcm16, 0, 4);
        unit.unit.Write(ChannelAddress(1, 0x4), 4, kBufferAddress);
        unit.unit.Write(ChannelAddress(1, 0x8), 4, kOneSamplePeriod);
        unit.unit.Write(ChannelAddress(1, 0xC), 4, 16);
        unit.unit.Write(ChannelAddress(1, 0x0), 4, 30 | kLoop | kPcm16 | kBusy);
        unit.StartCapture(0, 0, 16);
    };
    Unit unit(Pcm16(impulse));
    setup(unit);

    const std::vector<SoundOutput> out = unit.Render(80);

    // The impulse: 0x4000 * 256 in the mixer, 0x100 at the output. Its capture, 0x4000, at volume 30: 983,040 in the
    // mixer, 60 at the output. Captured again: 3,840, and 14 at the output. The capture at the 71st overflow takes
    // that echo at volume 30: 900, in slot 6.
    for (std::size_t i = 0; i < out.size(); i++)
    {
        const int left = i == 2 ? 0x100 : i == 36 ? 60 : i == 70 ? 14 : 0;
        XSFLIB_CHECK(out[i] == (SoundOutput{static_cast<uint16_t>(kBias + left), kBias}));
    }
    XSFLIB_CHECK(unit.Captured(6) == 900);

    // A run without the output captures the same samples.
    Unit quiet(Pcm16(impulse));
    setup(quiet);
    quiet.unit.Run(80 * ntdlib::ntr::kTicksPerSample);
    XSFLIB_CHECK(quiet.memory == unit.memory);
}

// The enhanced mode's samples of one side.
std::vector<double> Side(const std::vector<float>& out, int side)
{
    std::vector<double> samples;
    for (std::size_t i = static_cast<std::size_t>(side); i < out.size(); i += 2)
    {
        samples.push_back(out[i]);
    }
    return samples;
}

// The amplitude of the tone at `frequency` Hz in `samples` at the enhanced mode's rate, under a Hann window.
double Amplitude(const std::vector<double>& samples, double frequency)
{
    constexpr double kPi = 3.14159265358979323846;
    std::complex<double> sum = 0.0;
    double weights = 0.0;
    const double n = static_cast<double>(samples.size());
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const double weight = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / n);
        const double angle = 2.0 * kPi * frequency * static_cast<double>(i) / ntdlib::ntr::kEnhancedRate;
        sum += weight * samples[i] * std::complex<double>(std::cos(angle), -std::sin(angle));
        weights += weight;
    }
    return 2.0 * std::abs(sum) / weights;
}

void TestEnhancedFrames()
{
    // Each frame of kEnhancedFrameTicks ticks holds kEnhancedFrameSamples samples, in runs of any length. A run
    // without output passes its samples' times.
    Unit unit;
    unit.unit.StartEnhanced();
    XSFLIB_CHECK(unit.unit.Enhanced());
    std::vector<float> out;
    unit.unit.Run(1000, out);
    unit.unit.Run(ntdlib::ntr::kEnhancedFrameTicks - 1000, out);
    XSFLIB_CHECK(out.size() == 2 * ntdlib::ntr::kEnhancedFrameSamples);
    unit.unit.Run(ntdlib::ntr::kEnhancedFrameTicks);
    unit.unit.Run(3 * ntdlib::ntr::kEnhancedFrameTicks, out);
    XSFLIB_CHECK(out.size() == 8 * ntdlib::ntr::kEnhancedFrameSamples);

    Unit console;
    bool refused = false;
    try
    {
        console.unit.Run(100, out);
    }
    catch (const std::logic_error&)
    {
        refused = true;
    }
    XSFLIB_CHECK(refused);
}

void TestEnhancedSine()
{
    // A looped sine of 64 samples at 0x4000, one sample per 512 ticks: 511.4 Hz. Volume 127 and pan 0 give the left
    // side the whole sample, and the master volume of 127 keeps it: 0.5 of full scale. Sample 0 starts at the third
    // overflow (3 * 512 ticks), and the output runs 32 samples late. The right side stays silent.
    constexpr double kPi = 3.14159265358979323846;
    std::vector<int16_t> sine;
    for (int i = 0; i < 64; i++)
    {
        sine.push_back(static_cast<int16_t>(std::lround(0x4000 * std::sin(2.0 * kPi * i / 64.0))));
    }
    Unit unit(Pcm16(sine));
    unit.unit.StartEnhanced();
    unit.Start(0, 127 | kPcm16 | kLoop, 0, 32);
    const std::vector<float> out = unit.RenderEnhanced(20);

    double error = 0.0;
    double right = 0.0;
    for (std::size_t m = 200; m < out.size() / 2; m++)
    {
        const double time =
            static_cast<double>(m - 32) * ntdlib::ntr::kEnhancedFrameTicks / ntdlib::ntr::kEnhancedFrameSamples;
        const double place = time / 512.0 - 3.0;
        const double expected = 0.5 * std::sin(2.0 * kPi * place / 64.0);
        error = std::max(error, std::abs(out[2 * m] - expected));
        right = std::max(right, std::abs(static_cast<double>(out[2 * m + 1])));
    }
    XSFLIB_CHECK(error < 1e-3);
    XSFLIB_CHECK(right == 0.0);
}

void TestEnhancedSquare()
{
    // A rectangular wave with duty 3, one step per 512 ticks: 8 steps a cycle, 4,091 Hz. At volume 127, pan 0 and
    // master 127, its levels of +-0x7FFF give +-0x7FFF / 0x8000 on the left. Its fundamental then has 4/pi of that.
    // The 7th harmonic, 28.6 kHz, lies past the mode's Nyquist frequency. The band-limited steps leave out its alias
    // at 19.4 kHz. The console's sampling would put it there.
    constexpr double kPi = 3.14159265358979323846;
    Unit unit;
    unit.unit.StartEnhanced();
    unit.Start(8, 127 | (3u << 24) | kPsg, 0, 0);
    const std::vector<double> left = Side(unit.RenderEnhanced(20), 0);
    const std::vector<double> settled(left.begin() + 100, left.end());
    const double fundamental = ntdlib::ntr::kSystemClockHz / 2.0 / 512.0 / 8.0;
    const double level = 4.0 / kPi * 0x7FFF / 0x8000;
    XSFLIB_CHECK(std::abs(Amplitude(settled, fundamental) / level - 1.0) < 2e-3);
    XSFLIB_CHECK(std::abs(Amplitude(settled, 3 * fundamental) / (level / 3.0) - 1.0) < 2e-3);
    XSFLIB_CHECK(Amplitude(settled, ntdlib::ntr::kEnhancedRate - 7 * fundamental) < level * 1e-3);
}

void TestEnhancedGains()
{
    // A held level of 0x4000 at volume 127 and pan 64: 0.25 on each side. Two such channels add up to 0.5, and a
    // third at volume 127 with a divider of 2 adds half as much. Nothing clips: four channels of 0x7FFF at pan 0 give
    // 4 on the left. A change of the volume moves the gains over 250 samples. The output gives them 32 samples late.
    Unit unit(Pcm16(std::vector<int16_t>(8, 0x4000)));
    unit.unit.StartEnhanced();
    unit.Start(0, 127 | (64u << 16) | kPcm16 | kLoop, 0, 4);
    unit.Start(2, 127 | (64u << 16) | kPcm16 | kLoop, 0, 4);
    unit.Start(4, 127 | (1u << 8) | (64u << 16) | kPcm16 | kLoop, 0, 4);
    std::vector<float> out = unit.RenderEnhanced(2);
    XSFLIB_CHECK(std::abs(out[2 * 400] - 0.625f) < 1e-6f);
    XSFLIB_CHECK(std::abs(out[2 * 400 + 1] - 0.625f) < 1e-6f);

    // Channel 0 at volume 63: its gain falls from 0.25 to 0.25 * 63 / 128 over 250 samples.
    unit.unit.Write(ChannelAddress(0, 0x0), 1, 63);
    out = unit.RenderEnhanced(2);
    const float from = 0.625f;
    const float to = 0.375f + 0.25f * 63.0f / 128.0f;
    XSFLIB_CHECK(std::abs(out[2 * 31] - from) < 1e-6f);
    XSFLIB_CHECK(std::abs(out[2 * 32] - from) > 1e-4f);
    XSFLIB_CHECK(std::abs(out[2 * 157] - (from + to) / 2) < 2e-3f);
    XSFLIB_CHECK(std::abs(out[2 * 281] - to) < 1e-6f);
    for (std::size_t m = 33; m < 282; m++)
    {
        XSFLIB_CHECK(out[2 * m] < out[2 * m - 2]);
    }

    Unit loud(Pcm16(std::vector<int16_t>(8, 0x7FFF)));
    loud.unit.StartEnhanced();
    for (int channel = 0; channel < 4; channel++)
    {
        loud.Start(channel * 2, 127 | kPcm16 | kLoop, 0, 4);
    }
    const std::vector<float> loud_out = loud.RenderEnhanced(2);
    XSFLIB_CHECK(std::abs(loud_out[2 * 400] - 4.0f * 0x7FFF / 0x8000) < 1e-5f);
}

void TestEnhancedStop()
{
    // A held level of 0x4000 at pan 0 stops. Its samples after the one that plays go silent, and the interpolator's
    // window of 32 samples on each side, 64 samples of 512 ticks, passes the stop within 0.7 frames. The output then
    // stays at 0. With the hold flag, the last sample stays.
    for (const bool hold : {false, true})
    {
        Unit unit(Pcm16(std::vector<int16_t>(8, 0x4000)));
        unit.unit.StartEnhanced();
        const uint32_t control = 127 | kPcm16 | kLoop | (hold ? 1u << 15 : 0);
        unit.Start(0, control, 0, 4);
        std::vector<float> out = unit.RenderEnhanced(2);
        XSFLIB_CHECK(std::abs(out[2 * 400] - 0.5f) < 1e-6f);
        unit.unit.Write(ChannelAddress(0, 0x0), 4, control);
        XSFLIB_CHECK(!unit.unit.Busy(0));
        out = unit.RenderEnhanced(3);
        for (std::size_t m = 300; m < out.size() / 2; m++)
        {
            XSFLIB_CHECK(std::abs(out[2 * m] - (hold ? 0.5f : 0.0f)) < 1e-6f);
        }
    }
}

void TestEnhancedRunWithoutOutput()
{
    // A rectangular wave plays through two frames without output. From the second frame with output, the samples are
    // those of a unit that had output all along. After the wave stops, both fall to 0 and stay there.
    constexpr uint32_t kControl = 127 | (3u << 24) | kPsg;
    Unit heard;
    Unit caught_up;
    for (Unit* unit : {&heard, &caught_up})
    {
        unit->unit.StartEnhanced();
        unit->Start(8, kControl, 0, 0);
    }
    heard.RenderEnhanced(3);
    caught_up.unit.Run(2 * ntdlib::ntr::kEnhancedFrameTicks);
    caught_up.RenderEnhanced(1);
    XSFLIB_CHECK(heard.RenderEnhanced(2) == caught_up.RenderEnhanced(2));

    for (Unit* unit : {&heard, &caught_up})
    {
        unit->unit.Write(ChannelAddress(8, 0x0), 4, kControl);
        const std::vector<float> out = unit->RenderEnhanced(2);
        for (std::size_t m = 100; m < out.size() / 2; m++)
        {
            XSFLIB_CHECK(out[2 * m] == 0.0f);
        }
    }
}

void TestEnhancedHeldLevelWithoutOutput()
{
    // A rectangular wave that starts in a run without output and holds its level plays from the second frame with
    // output as in a unit that had output all along. So does its stop between two runs. A duty of 7 holds the level
    // low.
    constexpr uint32_t kControl = 127 | (7u << 24) | kPsg;
    Unit heard;
    Unit caught_up;
    for (Unit* each : {&heard, &caught_up})
    {
        each->unit.StartEnhanced();
    }
    heard.RenderEnhanced(1);
    heard.Start(8, kControl, 0, 0);
    heard.RenderEnhanced(2);
    caught_up.unit.Run(ntdlib::ntr::kEnhancedFrameTicks);
    caught_up.Start(8, kControl, 0, 0);
    caught_up.unit.Run(ntdlib::ntr::kEnhancedFrameTicks);
    caught_up.RenderEnhanced(1);
    XSFLIB_CHECK(heard.RenderEnhanced(2) == caught_up.RenderEnhanced(2));
    for (Unit* each : {&heard, &caught_up})
    {
        each->unit.Write(ChannelAddress(8, 0x0), 4, kControl);
    }
    XSFLIB_CHECK(heard.RenderEnhanced(2) == caught_up.RenderEnhanced(2));
}

void TestGrownChannels()
{
    // A grown channel plays as its twin: channel 16 with twin 8 gives channel 8's rectangular wave, in both modes.
    constexpr uint32_t kGrown = ntdlib::ntr::kGrownChannelRegisters;
    for (const bool enhanced : {false, true})
    {
        Unit console;
        Unit grown;
        if (enhanced)
        {
            console.unit.StartEnhanced();
            grown.unit.StartEnhanced();
        }
        console.Start(8, 127 | (40u << 16) | (2u << 24) | kPsg, 0, 0);
        grown.unit.Write(kGrown + ntdlib::ntr::kGrownChannelTwin, 4, 8);
        grown.unit.Write(kGrown + 0x8, 4, kOneSamplePeriod);
        grown.unit.Write(kGrown, 4, 127 | (40u << 16) | (2u << 24) | kPsg | kBusy);
        XSFLIB_CHECK(grown.unit.ChannelCount() == 17);
        XSFLIB_CHECK(grown.unit.Busy(16));
        XSFLIB_CHECK(grown.unit.Read(kGrown + ntdlib::ntr::kGrownChannelTwin, 4) == 8);
        if (enhanced)
        {
            XSFLIB_CHECK(console.RenderEnhanced(3) == grown.RenderEnhanced(3));
        }
        else
        {
            XSFLIB_CHECK(console.Render(300) == grown.Render(300));
        }
        grown.unit.Write(kGrown + 3, 1, 0);
        XSFLIB_CHECK(!grown.unit.Busy(16));
    }
}

} // namespace

int main()
{
    TestAdpcmStep();
    TestRegisters();
    TestOneShot();
    TestHold();
    TestPcm8Loop();
    TestAdpcmLoop();
    TestPsg();
    TestNoise();
    TestMixer();
    TestMutesAndMeters();
    TestRunWithoutMixing();
    TestShortSound();
    TestStop();
    TestCaptureValues();
    TestCaptureBuffer();
    TestReverb();
    TestEnhancedFrames();
    TestEnhancedSine();
    TestEnhancedSquare();
    TestEnhancedGains();
    TestEnhancedStop();
    TestEnhancedRunWithoutOutput();
    TestEnhancedHeldLevelWithoutOutput();
    TestGrownChannels();

    return xsflib::testing::Summary("DS sound unit");
}
