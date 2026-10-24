// SPDX-License-Identifier: MIT

// Tests of the IMA-ADPCM encoder (ntdlib/wave/encode_ima.h). The decoder of adpcm.h decodes what the encoder writes.
// The tests check what follows from the format and from the encoder's rule: each decoded sample is the closest that any
// nibble reaches from the state before it, the first state's step suits the opening best, and a loop's later passes
// decode as the first.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode_ima.h"
#include "wave_cases.h"

namespace
{

using ntdlib::wave::DecodeImaAdpcm;
using ntdlib::wave::EncodeImaAdpcm;
using ntdlib::wave::ImaAdpcmBytes;
using ntdlib::wave::ImaAdpcmChannel;
using ntdlib::wave::ImaAdpcmNext;
using ntdlib::wave::ImaAdpcmState;

constexpr double kPi = 3.14159265358979323846;

// The nibble of sample `i` of `data`.
uint8_t NibbleAt(std::span<const uint8_t> data, std::size_t i)
{
    return i % 2 == 0 ? data[i / 2] & 0x0f : data[i / 2] >> 4;
}

// The decoder's states after each sample of `data`, `count` of them, from `start`.
std::vector<ImaAdpcmState> StatesOf(std::span<const uint8_t> data, std::size_t count, ImaAdpcmState start)
{
    std::vector<ImaAdpcmState> states;
    ImaAdpcmState state = start;
    for (std::size_t i = 0; i < count; i++)
    {
        state = ImaAdpcmNext(state, NibbleAt(data, i));
        states.push_back(state);
    }

    return states;
}

// A sine of `frequency` cycles a sample, `count` samples long, at `amplitude`.
std::vector<int16_t> Sine(double frequency, double amplitude, std::size_t count)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; i++)
    {
        samples[i] = static_cast<int16_t>(std::lround(amplitude * std::sin(2.0 * kPi * frequency * i)));
    }

    return samples;
}

// Noise from the tests' generator, uniform over [-amplitude, amplitude].
std::vector<int16_t> Noise(uint64_t seed, int32_t amplitude, std::size_t count)
{
    ntdlib::wave::synthetic::Random random(seed);
    std::vector<int16_t> samples(count);
    for (int16_t& sample : samples)
    {
        sample = static_cast<int16_t>(static_cast<int32_t>(random.Below(2 * amplitude + 1)) - amplitude);
    }

    return samples;
}

// Notes of a tone with 8 harmonics that start at full level every 1,000 samples and decay.
std::vector<int16_t> Plucks(std::size_t count)
{
    std::vector<int16_t> samples(count);
    for (std::size_t i = 0; i < count; i++)
    {
        const double t = static_cast<double>(i % 1000);
        double y = 0.0;
        for (int h = 1; h <= 8; h++)
        {
            y += std::sin(2.0 * kPi * 0.005 * h * static_cast<double>(i)) / h;
        }
        samples[i] = static_cast<int16_t>(std::lround(9000.0 * y * std::exp(-t / 300.0)));
    }

    return samples;
}

void TestBytes()
{
    XSFLIB_CHECK(ImaAdpcmBytes(0) == std::size_t{0});
    XSFLIB_CHECK(ImaAdpcmBytes(1) == std::size_t{1});
    XSFLIB_CHECK(ImaAdpcmBytes(2) == std::size_t{1});
    XSFLIB_CHECK(ImaAdpcmBytes(7) == std::size_t{4});

    // An odd count leaves the last byte's high nibble at 0.
    const std::vector<int16_t> noise = Noise(5, 20000, 7);
    const ImaAdpcmChannel channel = EncodeImaAdpcm(noise, 0);
    XSFLIB_CHECK(channel.data.size() == std::size_t{4});
    XSFLIB_CHECK(channel.data[3] >> 4 == 0);

    const ImaAdpcmChannel none = EncodeImaAdpcm({}, 0);
    XSFLIB_CHECK(none.data.empty());
    XSFLIB_CHECK(none.start == ImaAdpcmState{});
    XSFLIB_CHECK(none.loop == ImaAdpcmState{});
}

void TestSilence()
{
    // The smallest step adds 7 / 8, rounded down to 0, so silence comes back exactly.
    const std::vector<int16_t> silence(1001, 0);
    const ImaAdpcmChannel channel = EncodeImaAdpcm(silence, 0);
    XSFLIB_CHECK(channel.start == ImaAdpcmState{});
    XSFLIB_CHECK(DecodeImaAdpcm(channel.data, 1001, channel.start.sample, channel.start.index) == silence);
}

// Each decoded sample is the closest that any of the 16 nibbles reaches from the state before it.
void CheckClosest(std::span<const int16_t> samples)
{
    const ImaAdpcmChannel channel = EncodeImaAdpcm(samples, 0);
    const std::vector<ImaAdpcmState> states = StatesOf(channel.data, samples.size(), channel.start);
    XSFLIB_CHECK(channel.start.sample == samples[0]);
    std::vector<int16_t> decoded;
    std::size_t farther = 0;
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        const ImaAdpcmState before = i == 0 ? channel.start : states[i - 1];
        const int32_t error = std::abs(samples[i] - states[i].sample);
        for (uint8_t n = 0; n < 16; n++)
        {
            farther += std::abs(samples[i] - ImaAdpcmNext(before, n).sample) < error ? 1 : 0;
        }
        decoded.push_back(states[i].sample);
    }
    XSFLIB_CHECK(farther == std::size_t{0});
    XSFLIB_CHECK(DecodeImaAdpcm(channel.data, static_cast<uint32_t>(samples.size()), channel.start.sample,
                         channel.start.index) == decoded);
}

void TestClosest()
{
    CheckClosest(Sine(440.0 / 48000.0, 12000.0, 48000));
    CheckClosest(Sine(6000.0 / 48000.0, 30000.0, 48000));
    CheckClosest(Noise(1, 8000, 48000));
    CheckClosest(Noise(2, 32767, 4800));
    CheckClosest(Plucks(48000));
}

void TestOpening()
{
    // A loud square wave from the first sample: a first step from the start of the table would take several samples
    // to reach it. The first state's step does at least as well on the opening as each other step.
    std::vector<int16_t> square(64);
    for (std::size_t i = 0; i < square.size(); i++)
    {
        square[i] = static_cast<int16_t>((i / 8) % 2 == 0 ? 20000 : -20000);
    }
    const ImaAdpcmChannel channel = EncodeImaAdpcm(square, 0);
    XSFLIB_CHECK(channel.start.index > 0);
    const auto error = [&](ImaAdpcmState start)
    {
        double sum = 0.0;
        ImaAdpcmState state = start;
        for (const int16_t sample : square)
        {
            ImaAdpcmState best = ImaAdpcmNext(state, 0);
            for (uint8_t n = 1; n < 16; n++)
            {
                const ImaAdpcmState next = ImaAdpcmNext(state, n);
                if (std::abs(sample - next.sample) < std::abs(sample - best.sample))
                {
                    best = next;
                }
            }
            state = best;
            sum += static_cast<double>(sample - state.sample) * (sample - state.sample);
        }

        return sum;
    };
    const double chosen = error(channel.start);
    for (int index = 0; index <= 88; index++)
    {
        XSFLIB_CHECK(chosen <= error({square[0], static_cast<uint8_t>(index)}));
    }
}

void TestLoop()
{
    const std::vector<int16_t> sine = Sine(1.0 / 37.0, 15000.0, 4001);
    for (const uint32_t loop_start : {0u, 1u, 2u, 999u, 2000u, 4000u, 4001u, 5000u})
    {
        const ImaAdpcmChannel channel = EncodeImaAdpcm(sine, loop_start);
        const std::vector<ImaAdpcmState> states = StatesOf(channel.data, sine.size(), channel.start);
        const std::size_t start = std::min<std::size_t>(loop_start, sine.size());
        XSFLIB_CHECK(channel.loop == (start == 0 ? channel.start : states[start - 1]));

        // A later pass from the loop's state decodes the first pass's samples.
        ImaAdpcmState state = channel.loop;
        std::size_t differ = 0;
        for (std::size_t i = start; i < sine.size(); i++)
        {
            state = ImaAdpcmNext(state, NibbleAt(channel.data, i));
            differ += state == states[i] ? 0 : 1;
        }
        XSFLIB_CHECK(differ == std::size_t{0});
    }
}

} // namespace

int main()
{
    TestBytes();
    TestSilence();
    TestClosest();
    TestOpening();
    TestLoop();

    return xsflib::testing::Summary("IMA-ADPCM encoder");
}
