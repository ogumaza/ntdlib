// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/nintendo_land_effects.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "cafe/axfx/util.h"
#include "ntdlib/cafe/axfx/axfx.h"
#include "ntdlib/cafe/axfx/math.h"
#include "ntdlib/fused.h"

namespace ntdlib::cafe::snd::nintendo_land
{
namespace
{

float Float(uint32_t word)
{
    return std::bit_cast<float>(word);
}

// The classes' rates (reverb 0x02018D64, delay 0x02017F08): 32,000 for 0, 48,000 for 1 and 0 for any other value.
float Rate(uint32_t mode)
{
    return mode == 0 ? 32000.0f : mode == 1 ? 48000.0f : 0.0f;
}

// The classes' conversion of a length to an unsigned integer: from 2^31 up, the value less 2^31 converted, with 2^31
// added back. A NaN fails the test and converts directly.
uint32_t ToUnsigned(float x)
{
    if (x >= 2147483648.0f)
    {
        return static_cast<uint32_t>(PowerPcToInt32(x - 2147483648.0f)) + 0x80000000u;
    }

    return static_cast<uint32_t>(PowerPcToInt32(x));
}

// The classes' damping: at least 0.05. A NaN takes 0.05.
float Damping(float damping)
{
    return damping >= 0.05f ? damping : 0.05f;
}

// Whether the byte at the start of a word is set: the flag of the threaded path.
bool FirstByte(uint32_t word)
{
    return (word >> 24) != 0;
}

// The early reflection's lengths for each index, at 32 kHz (0x10006E00) and at 48 kHz (0x10006E20).
constexpr std::array<uint32_t, 8> kEarlyLengths32{163, 317, 479, 641, 797, 967, 1123, 1283};
constexpr std::array<uint32_t, 8> kEarlyLengths48{241, 479, 719, 967, 1193, 1451, 1693, 1931};

// The lengths of the two combs and the two all-passes for each set, at 32 kHz (0x10006E98) and at 48 kHz (0x10006EF8).
using LengthSet = std::array<uint32_t, 4>;
constexpr std::array<LengthSet, 6> kLengthSets32{{{1789, 1999, 433, 149},
                                                  {149, 293, 251, 103},
                                                  {947, 1361, 433, 137},
                                                  {1279, 1531, 509, 149},
                                                  {1531, 1847, 563, 179},
                                                  {1823, 2357, 571, 137}}};
constexpr std::array<LengthSet, 6> kLengthSets48{{{2683, 2999, 647, 223},
                                                  {223, 439, 379, 157},
                                                  {1423, 2039, 647, 211},
                                                  {1913, 2297, 761, 223},
                                                  {2297, 2777, 839, 269},
                                                  {2731, 3539, 857, 211}}};

// A cleared line of `length` samples, its position at 0.
PairLine ClearedLine(uint32_t length)
{
    PairLine line;
    line.samples.assign(length, {});
    return line;
}

} // namespace

bool FrameRing::Init(uint32_t frames, uint32_t start)
{
    if (frames == 0 || frames > 64)
    {
        return false;
    }

    size_ = frames * kFrameSamples;
    for (std::vector<int32_t>& channel : ring_)
    {
        channel.assign(size_, 0);
    }
    start_ = start;
    write_ = 0;
    read_ = 0;
    processed_ = 0;
    count_ = 0;

    return true;
}

void FrameRing::Exchange(int32_t* left, int32_t* right)
{
    std::copy_n(left, kFrameSamples, ring_[0].begin() + write_);
    std::copy_n(right, kFrameSamples, ring_[1].begin() + write_);
    Step(write_);
    if (count_ < start_)
    {
        count_++;
    }

    // Silence while the worker hasn't processed the next frame out, as on the first frame, and until `start` frames
    // have come in.
    if (processed_ == read_ || count_ < start_)
    {
        std::fill_n(left, kFrameSamples, 0);
        std::fill_n(right, kFrameSamples, 0);
        return;
    }

    std::copy_n(ring_[0].begin() + read_, kFrameSamples, left);
    std::copy_n(ring_[1].begin() + read_, kFrameSamples, right);
    Step(read_);
}

void FrameRing::Step(uint32_t& position) const
{
    position += kFrameSamples;
    if (position >= size_)
    {
        position = 0;
    }
}

void PairLine::Step()
{
    position++;
    if (position >= samples.size())
    {
        position = 0;
    }
}

bool Reverb::Init(std::span<const uint32_t> words)
{
    on_ = false;
    if (words.size() < kParamWords || words[5] >= kEarlyLengths32.size() || words[6] >= kLengthSets32.size())
    {
        return false;
    }

    // The lengths (0x02018D98). A rate other than 0 takes the tables for 48 kHz.
    const uint32_t mode = words[9];
    const float rate = Rate(mode);
    const uint32_t early_index = words[5];
    const LengthSet& lengths = (mode == 0 ? kLengthSets32 : kLengthSets48)[words[6]];
    const uint32_t pre_length = ToUnsigned(rate * Float(words[0]));
    if (pre_length > kMaxLength)
    {
        return false;
    }
    early_ = ClearedLine((mode == 0 ? kEarlyLengths32 : kEarlyLengths48)[early_index]);
    pre_ = ClearedLine(pre_length);
    combs_ = {ClearedLine(lengths[0]), ClearedLine(lengths[1])};
    all_passes_ = {ClearedLine(lengths[2]), ClearedLine(lengths[3])};
    early_feedback_ = early_index <= 3 ? -0.33f : 0.33f;

    // The coefficients (0x02018F0C). Each comb decays by 60 dB in the time that words[1] gives.
    const float gain = Float(words[4]) * 0.6f;
    early_gain_ = Float(words[7]) * gain;
    for (std::size_t i = 0; i < comb_gains_.size(); i++)
    {
        const float exponent = (-3.0f * static_cast<float>(lengths[i])) / (Float(words[1]) * rate);
        comb_gains_[i] = axfx::Pow(10.0f, exponent);
    }
    coloration_ = Float(words[2]);
    damping_ = Damping(Float(words[3]));
    keep_ = 1.0f - damping_;
    reverb_gain_ = Float(words[8]) * gain;
    low_pass_ = {};

    threaded_ = FirstByte(words[10]);
    if (threaded_ && !ring_.Init(words[11], words[12]))
    {
        return false;
    }

    on_ = true;
    return true;
}

void Reverb::Process(const axfx::Buffers& frame, uint32_t samples)
{
    int32_t* const left = frame[axfx::kLeft];
    int32_t* const right = frame[axfx::kRight];
    if (!on_ || samples < kFrameSamples || left == nullptr || right == nullptr)
    {
        return;
    }

    if (threaded_)
    {
        ring_.Run(left, right, [this](int32_t* l, int32_t* r) { Kernel(l, r); });
    }
    else
    {
        Kernel(left, right);
    }
}

void Reverb::Kernel(int32_t* left, int32_t* right)
{
    const std::array<int32_t*, 2> channels{left, right};
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        for (std::size_t c = 0; c < channels.size(); c++)
        {
            const float x = static_cast<float>(channels[c][n]);

            // The early reflection, a comb with feedback.
            const float reflection = early_.samples[early_.position][c];
            early_.samples[early_.position][c] = axfx::Fmadds(reflection, early_feedback_, x);
            const float early = reflection * early_gain_;

            // The pre-delay, where it has a length.
            float delayed = x;
            if (!pre_.samples.empty())
            {
                delayed = pre_.samples[pre_.position][c];
                pre_.samples[pre_.position][c] = x;
            }

            // The two combs.
            const float comb0 = combs_[0].samples[combs_[0].position][c];
            combs_[0].samples[combs_[0].position][c] = axfx::Fmadds(comb0, comb_gains_[0], delayed);
            const float comb1 = combs_[1].samples[combs_[1].position][c];
            combs_[1].samples[combs_[1].position][c] = axfx::Fmadds(comb1, comb_gains_[1], delayed);
            const float combs = comb0 + comb1;

            // The first all-pass, the low-pass and the second all-pass.
            const float held0 = all_passes_[0].samples[all_passes_[0].position][c];
            const float in0 = axfx::Fmadds(held0, coloration_, combs);
            all_passes_[0].samples[all_passes_[0].position][c] = in0;
            const float out0 = axfx::NegatedFms(in0, coloration_, held0);
            low_pass_[c] = axfx::Fmadds(keep_, low_pass_[c], damping_ * out0);
            const float held1 = all_passes_[1].samples[all_passes_[1].position][c];
            const float in1 = axfx::Fmadds(held1, coloration_, low_pass_[c]);
            all_passes_[1].samples[all_passes_[1].position][c] = in1;
            const float out1 = axfx::NegatedFms(in1, coloration_, held1);

            channels[c][n] = PowerPcToInt32(axfx::Fmadds(out1, reverb_gain_, early));
        }

        early_.Step();
        if (!pre_.samples.empty())
        {
            pre_.Step();
        }
        combs_[0].Step();
        combs_[1].Step();
        all_passes_[0].Step();
        all_passes_[1].Step();
    }
}

bool Delay::Init(std::span<const uint32_t> words)
{
    on_ = false;
    if (words.size() < kParamWords)
    {
        return false;
    }

    // The length (0x02017F3C).
    const uint32_t length = ToUnsigned(Rate(words[4]) * (Float(words[0]) / 1000.0f));
    if (length > kMaxLength)
    {
        return false;
    }
    line_ = ClearedLine(std::max(length, 1u));

    // The coefficients (0x02017FC0).
    level_ = Float(words[2]);
    feedback_ = Float(words[1]);
    damping_ = Damping(Float(words[3]));
    keep_ = 1.0f - damping_;
    low_pass_ = {};

    threaded_ = FirstByte(words[5]);
    if (threaded_ && !ring_.Init(words[6], words[7]))
    {
        return false;
    }

    on_ = true;
    return true;
}

void Delay::Process(const axfx::Buffers& frame, uint32_t samples)
{
    int32_t* const left = frame[axfx::kLeft];
    int32_t* const right = frame[axfx::kRight];
    if (!on_ || samples < kFrameSamples || left == nullptr || right == nullptr)
    {
        return;
    }

    if (threaded_)
    {
        ring_.Run(left, right, [this](int32_t* l, int32_t* r) { Kernel(l, r); });
    }
    else
    {
        Kernel(left, right);
    }
}

void Delay::Kernel(int32_t* left, int32_t* right)
{
    const std::array<int32_t*, 2> channels{left, right};
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        for (std::size_t c = 0; c < channels.size(); c++)
        {
            const float x = static_cast<float>(channels[c][n]);
            low_pass_[c] = axfx::Fmadds(keep_, low_pass_[c], damping_ * x);
            const float delayed = line_.samples[line_.position][c];
            line_.samples[line_.position][c] = axfx::Fmadds(delayed, feedback_, low_pass_[c]);
            channels[c][n] = PowerPcToInt32(delayed * level_);
        }
        line_.Step();
    }
}

} // namespace ntdlib::cafe::snd::nintendo_land
