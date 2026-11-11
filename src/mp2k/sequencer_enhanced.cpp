// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/wave/sinc.h"
#include "sequencer_shared.h"

namespace ntdlib::mp2k
{

void Sequencer::StartEnhancedMixing()
{
    if (!mixing_)
    {
        throw std::logic_error("the enhanced mode mixes after StartMixing()");
    }
    enhanced_ = true;
    enhanced_channels_.assign(channels_.size(), EnhancedChannel{});
    enhanced_out_.assign(2 * size_t(agb::kEnhancedFrameSamples), 0.0f);
    enhanced_history_.assign(2 * size_t(dma_period_) * agb::kEnhancedFrameSamples, 0.0f);
    enhanced_echo_.assign(2 * size_t(agb::kEnhancedFrameSamples), 0.0f);
}

std::span<const float> Sequencer::EnhancedRight() const
{
    if (!enhanced_)
    {
        return {};
    }
    return std::span<const float>(enhanced_out_).first(agb::kEnhancedFrameSamples);
}

std::span<const float> Sequencer::EnhancedLeft() const
{
    if (!enhanced_)
    {
        return {};
    }
    return std::span<const float>(enhanced_out_)
        .subspan(info_.mono ? 0 : agb::kEnhancedFrameSamples, agb::kEnhancedFrameSamples);
}

void Sequencer::StartEnhancedPart()
{
    // The part of the frame in the buffer, and in the enhanced mode's history: 0 to the frames that the buffer holds.
    const size_t samples = agb::kEnhancedFrameSamples;
    const size_t parts = dma_period_;
    const size_t part = part_ / (uint32_t(info_.samples_per_frame) * uint32_t(PointsPerMixed()));
    float* right = enhanced_out_.data();
    float* left = right + samples;

    // Camelot's sums start with the echo that the last frame worked out.
    if (info_.camelot_mixer)
    {
        std::copy(enhanced_echo_.begin(), enhanced_echo_.end(), enhanced_out_.begin());
        return;
    }
    if (!reverb_)
    {
        std::fill(enhanced_out_.begin(), enhanced_out_.end(), 0.0f);
        return;
    }

    // The reverb reads the parts that the console's reverb reads: the frame's part and the next, from the last pass.
    const size_t next = (part + 1) % parts;
    const float* history_right = enhanced_history_.data();
    const float* history_left = history_right + parts * samples;
    const float* here_right = history_right + part * samples;
    const float* here_left = history_left + part * samples;
    const float* next_right = history_right + next * samples;
    const float* next_left = history_left + next * samples;
    if (info_.mono)
    {
        const float scale = float(reverb_) / 256.0f;
        for (size_t i = 0; i < samples; i++)
        {
            right[i] = (here_right[i] + next_right[i]) * scale;
            left[i] = right[i];
        }
        return;
    }
    const float scale = float(reverb_) / 512.0f;
    for (size_t i = 0; i < samples; i++)
    {
        right[i] = ((here_right[i] + here_left[i]) + (next_right[i] + next_left[i])) * scale;
        left[i] = right[i];
    }
}

void Sequencer::MixEnhanced(int index, const Channel& c, const Wave& wave, std::array<float, 2> to, bool starting)
{
    EnhancedChannel& e = enhanced_channels_[size_t(index)];
    const std::array<float, 2> from = starting ? to : e.volumes;
    e.volumes = to;
    const bool sounds = enhanced_frames_ && (from[0] != 0.0f || from[1] != 0.0f || to[0] != 0.0f || to[1] != 0.0f);
    const size_t samples = agb::kEnhancedFrameSamples;
    const uint32_t points = uint32_t(info_.samples_per_frame);
    float* right = enhanced_out_.data();
    float* left = right + samples;
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    const auto add = [&](size_t i, float value)
    {
        right[i] += value * (from[0] + float(i) * slopes[0]);
        left[i] += value * (from[1] + float(i) * slopes[1]);
    };

    // A synth voice of Camelot's mixer: its points of the frame, after its last points of the last frame, and the
    // first points of the next frame at the same pitch. The saw wave plays at half the volumes.
    if (info_.camelot_mixer && !(c.type & kVoiceFixed) && (c.synth || c.count == 0))
    {
        constexpr size_t kAhead = 32;
        std::array<int32_t, size_t(kMostFramePoints) + kAhead> raw;
        Channel voice = c;
        const uint32_t step = c.frequency * info_.step_scale;
        const bool saw = CamelotSynthPoints(voice, step, raw.data(), int(points));
        CamelotSynthPoints(voice, step, raw.data() + points, int(kAhead));
        std::array<float, 32 + size_t(kMostFramePoints) + kAhead> input;
        if (starting)
        {
            e.history = {};
        }
        std::copy(e.history.begin(), e.history.end(), input.begin());
        for (size_t i = 0; i < points + kAhead; i++)
        {
            input[32 + i] = saw ? float(raw[i]) * 0.5f : float(raw[i]);
        }
        std::copy(input.begin() + points, input.begin() + points + 32, e.history.begin());
        if (!sounds)
        {
            return;
        }

        const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(points) / double(samples));
        const size_t width = size_t(kernel.HalfWidth());
        for (size_t i = 0; i < samples; i++)
        {
            const uint64_t position = (uint64_t(i) * points << 23) / samples;
            const size_t point = size_t(position >> 23);
            add(i, kernel.At(&input[32 + point + 1 - width], uint32_t(position & 0x7FFFFF) >> 7));
        }
        return;
    }

    // The sample as the console's mixer plays it: forwards or backwards, compressed or not, with or without a loop,
    // and its step for each of the console's points. A newer driver's mixer leaves a compressed voice with an
    // uncompressed sample out. In Camelot's mixer, as in its driver, a loop of no length plays as no loop.
    const bool special = !info_.camelot_mixer && info_.special_samples && (c.type & (kVoiceReverse | kVoiceCompressed));
    const bool reverse = special && (c.type & kVoiceReverse);
    if (special && !reverse && wave.type == 0)
    {
        return;
    }
    const bool compressed = special && wave.type != 0;

    // A sample of 4-bit steps in Camelot's mixer plays its decoded points, and has no loop.
    const std::vector<float>* steps =
        PlaysCamelotSteps(c) && int32_t(wave.size) < 0 ? &CamelotStepPoints(wave) : nullptr;
    const uint32_t wave_size = steps ? uint32_t(steps->size()) : wave.size;
    const uint32_t loop_length = wave.size - wave.loop_start;
    const bool loops = !reverse && !steps && (c.status & kStatusLoop) && loop_length > 0;
    const uint64_t step = (c.type & kVoiceFixed) ? 0x800000u : uint64_t(uint32_t(c.frequency * info_.step_scale));
    const uint64_t move = step * points;
    const uint64_t start = e.position;
    e.position += move;
    if (!sounds)
    {
        return;
    }

    // The sample's points that the frame's positions reach, with the interpolator's span on each side.
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * 8388608.0));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(start >> 23) - (width - 1);
    const int64_t last = int64_t((start + move) >> 23) + width;
    std::vector<float> input(size_t(last - first + 1));
    DecodedBlock block;
    for (int64_t n = first; n <= last; n++)
    {
        float value = 0.0f;
        if (n >= 0)
        {
            uint64_t at = uint64_t(n);
            if (at >= wave.size && loops)
            {
                at = wave.loop_start + (at - wave.loop_start) % loop_length;
            }
            if (at < wave_size)
            {
                const uint32_t point = reverse ? wave.size - 1 - uint32_t(at) : uint32_t(at);
                if (steps)
                {
                    value = (*steps)[point];
                }
                else
                {
                    value =
                        float(compressed ? CompressedPoint(rom_, c.wave, point, block) : rom_.S8(wave.Data() + point));
                }
            }
        }
        input[size_t(n - first)] = value;
    }

    for (size_t i = 0; i < samples; i++)
    {
        const uint64_t position = start + uint64_t(i) * move / samples;
        const int64_t point = int64_t(position >> 23);
        add(i, kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position & 0x7FFFFF) >> 7));
    }
}

void Sequencer::EndEnhancedPart()
{
    // A frame without the enhanced mode's mixing is silent. The reverb and the echo take it as silence.
    const size_t samples = agb::kEnhancedFrameSamples;
    const size_t parts = dma_period_;
    const size_t part = part_ / (uint32_t(info_.samples_per_frame) * uint32_t(PointsPerMixed()));
    if (!enhanced_frames_)
    {
        std::fill(enhanced_out_.begin(), enhanced_out_.end(), 0.0f);
    }
    const float* right = enhanced_out_.data();
    const float* left = right + samples;
    float* history_right = enhanced_history_.data();
    float* history_left = history_right + parts * samples;
    const auto keep = [](float value)
    {
        return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
    };

    // Camelot's echo for the next frame: from the part of the buffer's frames before this one, before this frame takes
    // its place, from this frame's right side, and for the left side from the right side camelot_tap points before.
    if (info_.camelot_mixer)
    {
        const size_t tap = samples * size_t(info_.camelot_tap) / size_t(info_.samples_per_frame);
        const float level = float(info_.camelot_echo);
        const float cross = float(info_.camelot_cross_echo);
        const float share = info_.camelot_output_points == 1 ? 1.0f / 4.0f : 1.0f / 4.0f + 1.0f / 512.0f;
        const float* last_right = history_right + (part + parts - 1) % parts * samples;
        float* echo_right = enhanced_echo_.data();
        float* echo_left = echo_right + samples;
        for (size_t i = 0; i < samples; i++)
        {
            const float old_right = history_right[part * samples + i];
            const float old_left = history_left[part * samples + i];
            const float delayed = i >= tap ? right[i - tap] : last_right[samples - tap + i];
            echo_right[i] = keep((level * old_right + cross * old_left) / 128.0f + right[i] * share);
            echo_left[i] = keep((level * old_left + cross * old_right) / 128.0f + delayed * (1.0f / 4.0f));
        }
    }

    for (size_t i = 0; i < samples; i++)
    {
        history_right[part * samples + i] = keep(right[i]);
        history_left[part * samples + i] = keep(info_.mono ? right[i] : left[i]);
    }
}

} // namespace ntdlib::mp2k
