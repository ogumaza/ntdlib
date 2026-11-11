// SPDX-License-Identifier: MIT

// The mixing of the Brownie Brown sequencer: the points of the sample voices, the mixes of two and five voices, the
// FIFOs, the meters, and the enhanced mode.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "sequencer_internal.h"

namespace ntdlib::brownie
{

// The mixer's progress: each voice it plays takes the next 16 points of its sample, or 8, 4 or 2 that it repeats, after
// going back to its loop at its end, and a voice at the end of a sample without a loop stays silent. With the mixer's
// mode 0, it plays voices 0 and 1 only.
uint32_t Sequencer::LoadWord(uint32_t address) const
{
    const uint32_t word = rom_.U32(address & ~3u);
    const uint32_t turn = 8 * (address & 3);
    return turn ? (word >> turn) | (word << (32 - turn)) : word;
}

Sequencer::Points16 Sequencer::PointsOfTwoVoiceMix(const Voice& v, uint32_t point) const
{
    Points16 out;
    if (v.mode & 1)
    {
        // Each of 8 points plays twice.
        for (int w = 0; w < 2; w++)
        {
            const uint32_t word = LoadWord(point + 4 * uint32_t(w));
            for (int i = 0; i < 4; i++)
            {
                const uint8_t value = uint8_t(word >> (8 * i));
                out[size_t(8 * w + 2 * i)] = value;
                out[size_t(8 * w + 2 * i + 1)] = value;
            }
        }
    }
    else if (v.mode & 2)
    {
        // Each of 4 points plays four times.
        const uint32_t word = LoadWord(point);
        for (int i = 0; i < kMixPoints; i++)
        {
            out[size_t(i)] = uint8_t(word >> (8 * (i / 4)));
        }
    }
    else if (v.mode & 4)
    {
        // Each of 2 points plays eight times.
        const uint32_t word = LoadWord(point);
        for (int i = 0; i < kMixPoints; i++)
        {
            out[size_t(i)] = uint8_t(word >> (8 * (i / 8)));
        }
    }
    else
    {
        for (int i = 0; i < kMixPoints; i++)
        {
            out[size_t(i)] = rom_.U8(point + uint32_t(i));
        }
    }

    return out;
}

Sequencer::Points16 Sequencer::PointsOfFiveVoiceMix(const Voice& v, uint32_t point) const
{
    // The point between two others is their mean as offset-binary values, rounded down.
    const auto between = [](uint8_t a, uint8_t b)
    {
        return uint8_t((((a ^ 0x80u) + (b ^ 0x80u)) >> 1) - 0x80u);
    };

    Points16 out;
    if (v.mode & 1)
    {
        const uint32_t first = LoadWord(point);
        const uint32_t second = LoadWord(point + 4);
        std::array<uint8_t, 9> p;
        for (int i = 0; i < 4; i++)
        {
            p[size_t(i)] = uint8_t(first >> (8 * i));
            p[size_t(i + 4)] = uint8_t(second >> (8 * i));
        }

        p[8] = rom_.U8(point + 8);
        for (size_t i = 0; i < 8; i++)
        {
            out[2 * i] = p[i];
            out[2 * i + 1] = between(p[i], p[i + 1]);
        }
    }
    else if (v.mode & 2)
    {
        const uint32_t word = LoadWord(point);
        std::array<uint8_t, 5> p;
        for (int i = 0; i < 4; i++)
        {
            p[size_t(i)] = uint8_t(word >> (8 * i));
        }

        p[4] = rom_.U8(point + 4);
        for (size_t i = 0; i < 4; i++)
        {
            out[4 * i] = p[i];
            out[4 * i + 1] = p[i];
            out[4 * i + 2] = between(p[i], p[i + 1]);
            out[4 * i + 3] = between(p[i], p[i + 1]);
        }
    }
    else if (v.mode & 4)
    {
        const uint32_t word = LoadWord(point);
        const std::array<uint8_t, 3> p = {uint8_t(word), uint8_t(word >> 8), rom_.U8(point + 2)};
        for (size_t i = 0; i < 2; i++)
        {
            for (size_t j = 0; j < 4; j++)
            {
                out[8 * i + j] = p[i];
                out[8 * i + 4 + j] = between(p[i], p[i + 1]);
            }
        }
    }
    else
    {
        for (int w = 0; w < 4; w++)
        {
            const uint32_t word = LoadWord(point + 4 * uint32_t(w));
            for (int i = 0; i < 4; i++)
            {
                out[size_t(4 * w + i)] = uint8_t(word >> (8 * i));
            }
        }
    }

    return out;
}

void Sequencer::MixTwoVoices(const VoicePoints& points)
{
    // Two negative points take the sum of their sizes, rounded, and its negative. Any other pair takes the signed sum,
    // rounded. The sums wrap in the FIFO's byte.
    const auto part = [](uint8_t a, uint8_t b, uint8_t level_a, uint8_t level_b)
    {
        if ((a & 0x80) && (b & 0x80))
        {
            const uint32_t size = (256u - a) * level_a + (256u - b) * level_b;
            return int8_t(uint8_t(0u - ((size + 128) >> 8)));
        }

        return int8_t(uint8_t((int32_t(int8_t(a)) * level_a + int32_t(int8_t(b)) * level_b + 128) >> 8));
    };

    const Voice& a = voices_[0];
    const Voice& b = voices_[1];
    for (size_t i = 0; i < kMixPoints; i++)
    {
        fifo_bytes_[0].push_back(part(points[0][i], points[1][i], a.right, b.right));
        fifo_bytes_[1].push_back(part(points[0][i], points[1][i], a.left, b.left));
    }
}

void Sequencer::MixFiveVoices(const VoicePoints& points)
{
    for (size_t i = 0; i < kMixPoints; i++)
    {
        int32_t right = 0;
        int32_t left = 0;
        for (size_t v = 0; v < kVoiceCount; v++)
        {
            const int32_t point = int8_t(points[v][i]);
            right += point * voices_[v].right;
            left += point * voices_[v].left;
        }

        fifo_bytes_[0].push_back(int8_t(uint8_t((right + 128) >> 8)));
        fifo_bytes_[1].push_back(int8_t(uint8_t((left + 128) >> 8)));
    }
}

void Sequencer::Mix()
{
    interrupts_ = uint16_t(interrupts_ & ~kMixerInterrupts);
    dma_control_ = kDmaOn;
    const int voices = Get8(kMixerMode) == 0 ? kMode0Voices : kVoiceCount;
    VoicePoints points = {};
    for (int i = 0; i < voices; i++)
    {
        Voice& v = voices_[size_t(i)];
        uint32_t point = v.point;
        if (point == v.end)
        {
            point = v.loop;
            if (point == v.end)
            {
                continue;
            }
        }

        if (keep_bytes_ && !(muted_channels_ >> (kFirstSampleChannel + i) & 1))
        {
            points[size_t(i)] = voices == kVoiceCount ? PointsOfFiveVoiceMix(v, point) : PointsOfTwoVoiceMix(v, point);
            if (metering_ && !enhanced_)
            {
                for (const uint8_t value : points[size_t(i)])
                {
                    const float size = std::fabs(float(int8_t(value))) / 256.0f;
                    MeterPart(kFirstSampleChannel + i, 0, size * float(v.right));
                    MeterPart(kFirstSampleChannel + i, 1, size * float(v.left));
                }
            }
        }

        v.point = point + ((v.mode & 1) ? 8 : (v.mode & 2) ? 4 : (v.mode & 4) ? 2 : 16);
    }

    if (keep_bytes_)
    {
        if (voices == kVoiceCount)
        {
            MixFiveVoices(points);
        }
        else
        {
            MixTwoVoices(points);
        }
    }

    interrupts_ = uint16_t(interrupts_ | kMixerInterrupts);
}

// The FIFOs in the Magical Vacation revision: at each overflow of a FIFO's timer until the next frame, the FIFO plays a
// point. If it then holds 16 or fewer, it asks for more. The request interrupts while the FIFO's DMA is on.
void Sequencer::RunFifos()
{
    const uint64_t end = (uint64_t(frame_) + 1) * kFrameCycles;
    for (int x = 0; x < kFifoCount; x++)
    {
        Fifo& f = fifos_[size_t(x)];
        while (f.running && f.next < end)
        {
            now_ = f.next;
            f.count = f.count > 0 ? f.count - 1 : 0;
            if (f.count <= kFifoRequest && f.dma && (interrupts_ & (kFifoAInterrupt << x)))
            {
                Interrupt(x);
            }
            f.next += Period(x);
        }
    }
}

void Sequencer::MeterPart(int channel, int fifo, float part)
{
    float& peak = voice_peaks_[size_t(channel)][size_t(fifo)];
    peak = std::max(peak, part);
}

int Sequencer::PsgChannelTrack(int psg) const
{
    const bool effect = (channels_[size_t(psg)].flags & kFlagEffect) != 0;
    return effect ? psg + kFirstEffectChannel : psg;
}

void Sequencer::MeterVoices(bool on)
{
    metering_ = on;
    if (!on)
    {
        voice_peaks_ = {};
    }
}

void Sequencer::StartEnhancedMixing()
{
    Enhanced& e = enhanced_.emplace();
    e.out.assign(2 * size_t(kEnhancedFrameSamples), 0.0f);
}

void Sequencer::SetEnhancedFrames(bool on)
{
    if (enhanced_)
    {
        enhanced_->frames = on;
    }
}

std::span<const float> Sequencer::EnhancedFifo(int fifo) const
{
    if (!enhanced_)
    {
        return {};
    }

    return std::span<const float>(enhanced_->out).subspan(size_t(fifo) * kEnhancedFrameSamples, kEnhancedFrameSamples);
}

std::array<uint8_t, ntdlib::agb::kSoundRegisterBytes> Sequencer::SoundRegisters() const
{
    std::array<uint8_t, ntdlib::agb::kSoundRegisterBytes> out = {};
    std::copy(registers_.begin(), registers_.end(), out.begin());
    out[kIoSoundBias + 1 - kIoNr10] = 0x02;
    return out;
}

void Sequencer::StartEnhanced(int voice, uint32_t start, uint32_t end, uint32_t loop)
{
    if (enhanced_)
    {
        EnhancedVoice& e = enhanced_->voices[size_t(voice)];
        e = EnhancedVoice();
        e.start = start;
        e.end = end;
        e.loop = loop;
        e.starting = true;
    }
}

void Sequencer::StopEnhanced(int voice)
{
    if (enhanced_)
    {
        enhanced_->voices[size_t(voice)] = EnhancedVoice();
    }
}

float Sequencer::PointOf(const EnhancedVoice& e, int64_t point) const
{
    if (point < 0)
    {
        return 0.0f;
    }

    uint64_t at = uint64_t(e.start) + uint64_t(point);
    if (at >= e.end)
    {
        if (e.loop >= e.end)
        {
            return 0.0f;
        }

        at = e.loop + (at - e.loop) % (e.end - e.loop);
    }

    return float(rom_.S8(uint32_t(at)));
}

void Sequencer::MixEnhancedVoice(int voice, int channel, std::array<float, 2> to, uint32_t period)
{
    EnhancedVoice& e = enhanced_->voices[size_t(voice)];
    const std::array<float, 2> from = e.starting ? to : e.levels;
    e.levels = to;
    e.starting = false;

    // The place moves by a point for each of the console's points that the frame's time holds.
    const uint64_t move = (uint64_t(kFrameCycles) << 32) / period;
    const uint64_t start = e.position;
    e.position += move;
    if (!enhanced_->frames || (muted_channels_ >> channel & 1))
    {
        return;
    }

    constexpr double kUnit = 4294967296.0;
    const size_t samples = kEnhancedFrameSamples;
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * kUnit));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(start >> 32) - (width - 1);
    const int64_t last = int64_t((start + move) >> 32) + width;
    std::vector<float> input(size_t(last - first + 1));
    for (int64_t n = first; n <= last; n++)
    {
        input[size_t(n - first)] = PointOf(e, n);
    }

    float* a = enhanced_->out.data();
    float* b = a + samples;
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    for (size_t i = 0; i < samples; i++)
    {
        const uint64_t position = start + uint64_t(i) * move / samples;
        const int64_t point = int64_t(position >> 32);
        const float value = kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position >> 16) & 0xFFFF);
        const float part_a = value * (from[0] + float(i) * slopes[0]);
        const float part_b = value * (from[1] + float(i) * slopes[1]);
        a[i] += part_a;
        b[i] += part_b;
        if (metering_)
        {
            MeterPart(channel, 0, std::fabs(part_a));
            MeterPart(channel, 1, std::fabs(part_b));
        }
    }
}

void Sequencer::MixEnhanced()
{
    if (!enhanced_)
    {
        return;
    }

    std::fill(enhanced_->out.begin(), enhanced_->out.end(), 0.0f);
    if (vacation_)
    {
        // Each FIFO plays its sample at its timer's rate, at the owner's channel.
        for (int x = 0; x < kFifoCount; x++)
        {
            if (enhanced_->voices[size_t(x)].start != 0)
            {
                const float level =
                    float((x == 0 ? (fifo_voices_[size_t(x)].volume & 0xFF) : fifo_voices_[size_t(x)].volume) + 1) /
                    256.0f;
                MixEnhancedVoice(x, std::max(fifos_[size_t(x)].owner, 0),
                                 {x == 0 ? level : 0.0f, x == 1 ? level : 0.0f}, uint32_t(Period(x)));
            }
        }

        return;
    }

    if (mix_start_ < 0)
    {
        return;
    }

    const int voices = Get8(kMixerMode) == 0 ? kMode0Voices : kVoiceCount;
    for (int i = 0; i < voices; i++)
    {
        const Voice& v = voices_[size_t(i)];
        if (enhanced_->voices[size_t(i)].start != 0)
        {
            const int shift = (v.mode & 1) ? 1 : (v.mode & 2) ? 2 : (v.mode & 4) ? 3 : 0;
            MixEnhancedVoice(i, kFirstSampleChannel + i, {float(v.right) / 256.0f, float(v.left) / 256.0f},
                             info_.point_cycles << shift);
        }
    }
}

} // namespace ntdlib::brownie
