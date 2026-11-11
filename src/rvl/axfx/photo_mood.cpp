// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/axfx/photo_mood.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "ntdlib/fused.h"
#include "ntdlib/rvl/axfx/axfx.h"

namespace ntdlib::rvl::axfx
{
namespace
{

constexpr int kRingCount = 4;

// The samples before the frame that modes 1 and 4 read with it, for their filters.
constexpr int kEchoHistory = 11;
constexpr int kRadioHistory = 21;

// The delay of the output that modes 1 and 3 mix in: 60 frames.
constexpr int kFeedbackDelay = 60 * kSamplesPerFrame;

// The gains, in 4096ths.
constexpr int32_t kEchoFeedback = 0x999;    // 0.6
constexpr int32_t kOctaveGain = 0x1333;     // 1.2
constexpr int32_t kVibratoDry = 0xccc;      // 0.8
constexpr int32_t kVibratoFeedback = 0x333; // 0.2
constexpr int32_t kRadioGain = 0xe66;       // 0.9
constexpr int32_t kRadioNoise = 0x19a;      // 0.1 of 0 to 4095

// The vibrato's read starts 2 frames back. Its offsets move it by up to 22 samples either way.
constexpr int kVibratoDelay = 2 * kSamplesPerFrame;
constexpr int32_t kVibratoDepth = 22;

// The low 7 bits that mode 4 clears.
constexpr uint32_t kRadioMask = 0xffffff80;

// The 32-bit integer operations: mullw, add, slwi by 12, and the division by 4096 towards 0 (srawi by 12, then addze).
int32_t Mul(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

int32_t Add(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t Shl12(int32_t a)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) << 12);
}

int32_t Descale(int32_t a)
{
    return a / 4096;
}

// The division towards minus infinity that the game's index arithmetic makes from a division towards 0.
int FloorDiv(int a, int b)
{
    const int quotient = a / b;
    return a % b < 0 ? quotient - 1 : quotient;
}

// The table's values were rounded to 6 decimals before the compiler converted them to single precision. A small
// negative value gives -0.
float Round6(double value)
{
    const auto rounded = static_cast<float>(static_cast<double>(std::llround(value * 1e6)) / 1e6);
    return std::copysign(rounded, static_cast<float>(value));
}

// The game passes an angle in 65536ths of a turn as a 16-bit integer. A quantized load (GQR3: unsigned 16-bit, the OS's
// setting) converts it, and the game multiplies it by 1/256.
float Index(uint32_t angle)
{
    return (1.0f / 256.0f) * static_cast<float>(angle & 0xffff);
}

// The table's entry for `index`, the absolute value of an index in 256ths of a turn: SinFIdx and CosFIdx bring it
// within 65536 first. A quantized store (GQR3) truncates it to an unsigned 16-bit integer. The fraction's step goes on
// from the entry.
struct Lookup
{
    const PhotoMood::SinCos* entry;
    float fraction;
};

Lookup Look(float index)
{
    float value = std::fabs(index);
    while (value > 65536.0f)
    {
        value -= 65536.0f;
    }

    const auto whole = static_cast<uint32_t>(std::min(value, 65535.0f));
    return {&PhotoMood::SinCosTable()[whole & 0xff], value - static_cast<float>(whole)};
}

// The constructor's band-pass filters: tap `half` is `middle`. Tap `half` ± k is 4096 (sin(k ω_high) − sin(k ω_low))
// / (π k), with π as 0x3243 / 4096. Each ω is given by its step of angle for k, in 8192ths of a turn.
template <std::size_t N>
std::array<int32_t, N> BandPass(int32_t middle, uint32_t high_step, uint32_t low_step)
{
    constexpr int kHalf = static_cast<int>(N / 2);
    std::array<int32_t, N> taps{};
    taps[kHalf] = middle;
    for (int k = 1; k <= kHalf; k++)
    {
        const uint32_t high_angle = (high_step * static_cast<uint32_t>(k) << 3) & 0xfff8;
        const uint32_t low_angle = (low_step * static_cast<uint32_t>(k) << 3) & 0xfff8;
        const int32_t high = ntdlib::PowerPcToInt32(4096.0f * PhotoMood::SinFIdx(Index(high_angle)));
        const int32_t low = ntdlib::PowerPcToInt32(4096.0f * PhotoMood::SinFIdx(Index(low_angle)));
        const int32_t tap = Shl12(high - low) / (0x3243 * k);
        taps[static_cast<std::size_t>(kHalf - k)] = tap;
        taps[static_cast<std::size_t>(kHalf + k)] = tap;
    }

    return taps;
}

// The angle of entry `i` of a table of `n` entries that covers one turn: i × 65536 / n, a division of 64-bit integers.
uint32_t AngleOf(int i, int n)
{
    return static_cast<uint32_t>((static_cast<uint64_t>(i) << 16) / static_cast<uint64_t>(n));
}

PhotoMood::Tables BuildTables()
{
    PhotoMood::Tables tables;
    tables.echo_filter = BandPass<11>(0xd00, 0xe00, 0x100);
    tables.radio_filter = BandPass<21>(0x266, 0x366, 0x100);

    // 0x8004b534 with type 0: a Hamming window, 0.54 − 0.46 cos.
    const int window_size = static_cast<int>(tables.window.size());
    for (int i = 0; i < window_size; i++)
    {
        const int32_t cos = ntdlib::PowerPcToInt32(4096.0f * PhotoMood::CosFIdx(Index(AngleOf(i, window_size))));
        tables.window[static_cast<std::size_t>(i)] = Descale(Mul(-cos, 0x75c)) + 0x8a3;
    }

    const int offsets = static_cast<int>(tables.vibrato_offset.size());
    for (int i = 0; i < offsets; i++)
    {
        const int32_t sin = ntdlib::PowerPcToInt32(4096.0f * PhotoMood::SinFIdx(Index(AngleOf(i, offsets))));
        tables.vibrato_offset[static_cast<std::size_t>(i)] = Descale(Mul(sin, kVibratoDepth));
    }

    return tables;
}

} // namespace

const PhotoMood::Tables& PhotoMood::BuiltTables()
{
    static const Tables tables = BuildTables();
    return tables;
}

const std::array<PhotoMood::SinCos, 256>& PhotoMood::SinCosTable()
{
    static const std::array<SinCos, 256> table = []
    {
        std::array<SinCos, 256> entries{};
        for (std::size_t i = 0; i < entries.size(); i++)
        {
            const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) / 256.0;
            const double next = 2.0 * std::numbers::pi * static_cast<double>(i + 1) / 256.0;
            entries[i] = {Round6(std::sin(angle)), Round6(std::cos(angle)), Round6(std::sin(next) - std::sin(angle)),
                          Round6(std::cos(next) - std::cos(angle))};
        }

        return entries;
    }();

    return table;
}

float PhotoMood::SinFIdx(float index)
{
    const Lookup lookup = Look(index);
    const float step = lookup.fraction * lookup.entry->sin_step;
    const float value = lookup.entry->sin + step;

    return index < 0.0f ? -value : value;
}

float PhotoMood::CosFIdx(float index)
{
    const Lookup lookup = Look(index);
    const float step = lookup.fraction * lookup.entry->cos_step;

    return lookup.entry->cos + step;
}

PhotoMood::PhotoMood() : rings_(static_cast<std::size_t>(kRingCount * kRingSamples), 0)
{
    BuiltTables();
}

bool PhotoMood::Init(const Params& params)
{
    mode_ = params.mode;

    return true;
}

bool PhotoMood::Settings(const Params& params)
{
    mode_ = params.mode;

    return true;
}

void PhotoMood::Shutdown()
{
}

int32_t& PhotoMood::At(int ring, int t)
{
    // The game's signed remainder gives the same frame until its counter overflows, after 2^31 frames (74 days).
    const int frames = FloorDiv(t, kSamplesPerFrame);
    const int sample = t - frames * kSamplesPerFrame;
    const int frame = (position_ + frames + kRingFrames) & (kRingFrames - 1);

    return rings_[static_cast<std::size_t>(ring * kRingSamples + frame * kSamplesPerFrame + sample)];
}

void PhotoMood::Process(AuxFrame& frame)
{
    for (int channel = 0; channel < 2; channel++)
    {
        for (int i = 0; i < kSamplesPerFrame; i++)
        {
            At(InputRing(channel), i) = frame.channels[channel][i];
        }
    }

    switch (mode_)
    {
    case 1:
        Echo(frame);
        break;
    case 2:
        OctaveDown(frame);
        break;
    case 3:
        Vibrato(frame);
        break;
    case 4:
        Radio(frame);
        break;
    default:
        break;
    }

    for (int channel = 0; channel < 2; channel++)
    {
        for (int i = 0; i < kSamplesPerFrame; i++)
        {
            At(OutputRing(channel), i) = frame.channels[channel][i];
        }
    }

    position_ = Add(position_, 1);
}

void PhotoMood::Echo(AuxFrame& frame)
{
    const Tables& tables = BuiltTables();
    constexpr int kTaps = 11;
    for (int channel = 0; channel < 2; channel++)
    {
        std::array<int32_t, kEchoHistory + kSamplesPerFrame> mixed{};
        for (int n = 0; n < static_cast<int>(mixed.size()); n++)
        {
            const int t = n - kEchoHistory;
            const int32_t input = At(InputRing(channel), t);
            const int32_t delayed = At(OutputRing(channel), t - kFeedbackDelay);
            mixed[static_cast<std::size_t>(n)] = Descale(Add(Shl12(input), Mul(delayed, kEchoFeedback)));
        }

        for (int i = 0; i < kSamplesPerFrame; i++)
        {
            int32_t sum = 0;
            for (int k = 0; k < kTaps; k++)
            {
                sum = Add(sum, Mul(tables.echo_filter[static_cast<std::size_t>(k)],
                                   mixed[static_cast<std::size_t>(kEchoHistory + i - k)]));
            }

            frame.channels[channel][i] = Descale(sum);
        }
    }
}

void PhotoMood::OctaveDown(AuxFrame& frame)
{
    const Tables& tables = BuiltTables();
    constexpr int kHalfFrame = kSamplesPerFrame / 2;
    for (int channel = 0; channel < 2; channel++)
    {
        std::array<int32_t, kSamplesPerFrame> sum{};
        for (int grain = 0; grain < 2; grain++)
        {
            // A grain started `age` frames ago and has read half a frame in each frame since.
            const int age = (position_ + grain * kRingFrames / 2) & (kRingFrames - 1);
            const int read = age * kHalfFrame;
            const int start_frame = FloorDiv(read, kSamplesPerFrame) - age;
            const int start_sample = read - FloorDiv(read, kSamplesPerFrame) * kSamplesPerFrame;
            const int frame_index = (position_ + start_frame + kRingFrames) & (kRingFrames - 1);
            const auto start = static_cast<std::size_t>(InputRing(channel) * kRingSamples +
                                                        frame_index * kSamplesPerFrame + start_sample);
            for (int i = 0; i < kSamplesPerFrame; i++)
            {
                const auto k = static_cast<std::size_t>(i / 2);
                int32_t pair = 0;
                if (i == kSamplesPerFrame - 1)
                {
                    // The last sample reads its second sample through the ring.
                    const int next = read + kHalfFrame;
                    const int next_frame = FloorDiv(next, kSamplesPerFrame) - age;
                    const int next_sample = next - FloorDiv(next, kSamplesPerFrame) * kSamplesPerFrame;
                    const int next_index = (position_ + next_frame + kRingFrames) & (kRingFrames - 1);
                    const int32_t second = rings_[static_cast<std::size_t>(
                        InputRing(channel) * kRingSamples + next_index * kSamplesPerFrame + next_sample)];
                    pair = Add(second, rings_[start + k]);
                }
                else
                {
                    // The others read straight on from the start. Sample 94's second sample can lie past the end of
                    // the ring, in the next ring.
                    pair = Add(rings_[start + k], rings_[start + k + 1]);
                }

                const int32_t weight = tables.window[static_cast<std::size_t>(age * kSamplesPerFrame + i)];
                sum[static_cast<std::size_t>(i)] =
                    Add(sum[static_cast<std::size_t>(i)], Descale(Mul(pair / 2, weight)));
            }
        }

        for (int i = 0; i < kSamplesPerFrame; i++)
        {
            frame.channels[channel][i] = Descale(Mul(sum[static_cast<std::size_t>(i)], kOctaveGain));
        }
    }
}

void PhotoMood::Vibrato(AuxFrame& frame)
{
    const Tables& tables = BuiltTables();
    const int phase = position_ % kVibratoFrames;
    for (int channel = 0; channel < 2; channel++)
    {
        for (int i = 0; i < kSamplesPerFrame; i++)
        {
            const int32_t offset = tables.vibrato_offset[static_cast<std::size_t>(phase * kSamplesPerFrame + i)];
            const int32_t read = At(InputRing(channel), i + offset - kVibratoDelay);
            const int32_t delayed = At(OutputRing(channel), i - kFeedbackDelay);
            frame.channels[channel][i] = Descale(Add(Mul(read, kVibratoDry), Mul(delayed, kVibratoFeedback)));
        }
    }
}

void PhotoMood::Radio(AuxFrame& frame)
{
    const Tables& tables = BuiltTables();
    constexpr int kTaps = 21;
    std::array<int32_t, kRadioHistory + kSamplesPerFrame> mono{};
    for (int n = 0; n < static_cast<int>(mono.size()); n++)
    {
        const int t = n - kRadioHistory;
        const int32_t sum = Add(At(InputRing(0), t), At(InputRing(1), t));
        mono[static_cast<std::size_t>(n)] = Descale(Shl12(sum));
    }

    std::array<int32_t, kSamplesPerFrame> output{};
    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        int32_t sum = 0;
        for (int k = 0; k < kTaps; k++)
        {
            sum = Add(sum, Mul(tables.radio_filter[static_cast<std::size_t>(k)],
                               mono[static_cast<std::size_t>(kRadioHistory + i - k)]));
        }

        output[static_cast<std::size_t>(i)] = Descale(sum);
    }

    for (int i = 0; i < kSamplesPerFrame; i++)
    {
        if (i % 8 == 0)
        {
            random_ = random_ * 0x80d + 7;
        }

        const auto noise = static_cast<int32_t>(random_ & 0xfff);
        const int32_t gain = (noise * kRadioNoise >> 12) + kRadioGain;
        const int32_t scaled = Descale(Mul(output[static_cast<std::size_t>(i)], gain));
        output[static_cast<std::size_t>(i)] = static_cast<int32_t>(static_cast<uint32_t>(scaled) & kRadioMask);
    }

    for (int channel = 0; channel < 2; channel++)
    {
        std::copy(output.begin(), output.end(), frame.channels[channel].begin());
    }
}

} // namespace ntdlib::rvl::axfx
