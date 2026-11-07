// SPDX-License-Identifier: MIT

#include "ntdlib/ubimilan/output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/sequencer.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::ubimilan
{
namespace
{

// The shift from the mixer's sums to its bytes. It also gives the scale of a voice's level.
constexpr int kOutputShift = 6;
constexpr float kLevelScale = 64.0f;

// The points of the whole runs that take a voice past `points` points, and of at least one: a voice's checks come after
// a run's move.
uint32_t PointsOfRuns(int64_t points)
{
    return uint32_t(std::max<int64_t>((points + kRunPoints - 1) / kRunPoints, 1)) * kRunPoints;
}

} // namespace

Output::Output(const Rom& rom, const DriverInfo& info, int song) : rom_(rom), info_(info), song_(rom, info, song, true)
{
}

void Output::Frame()
{
    runs_.clear();
    writes_ = song_.Step(runs_);
    bytes_.clear();
    voices_peak_ = 0.0f;
    for (const MixerRun& run : runs_)
    {
        MixRun(run);
    }

    if (enhanced_)
    {
        TakeKitEvents();
        MixEnhanced();
    }
}

void Output::MixRun(const MixerRun& run)
{
    std::array<int32_t, kRunOutput> out{};
    for (uint32_t i = 0; i < kRunPoints; i++)
    {
        int32_t sum = 0;
        for (const RunVoice& voice : run)
        {
            if (!voice.heard || voices_muted_)
            {
                continue;
            }

            const int32_t point = int8_t(BusByte(rom_, (voice.position & ~3u) + i));
            sum += point * int32_t(voice.volume);
        }

        out[size_t(2 * i + 1)] = sum;
        if (metering_ && !enhanced_)
        {
            voices_peak_ = std::max(voices_peak_, float(std::abs(sum)) / kLevelScale);
        }
    }

    out[0] = (last_sum_ + out[1]) >> 1;
    for (size_t i = 2; i < out.size(); i += 2)
    {
        out[i] = (out[i - 1] + out[i + 1]) >> 1;
    }

    last_sum_ = out[out.size() - 1];
    for (const int32_t point : out)
    {
        const int32_t half = point >> 1;
        const int32_t average = half + last_half_;
        last_half_ = half;
        bytes_.push_back(int8_t(std::clamp(average >> kOutputShift, -128, 127)));
    }
}

void Output::StartEnhancedMixing()
{
    enhanced_ = true;
    enhanced_voices_.assign(size_t(kEnhancedVoices), EnhancedVoice());
    enhanced_kit_voice_ = 0;
    enhanced_samples_.assign(size_t(agb::kEnhancedFrameSamples), 0.0f);
}

void Output::TakeKitEvents()
{
    const bool fixed = song_.Piece().KeepsKitVoice();
    for (const KitEvent& event : song_.KitEvents())
    {
        if (!event.on)
        {
            EnhancedVoice& voice = enhanced_voices_[size_t(enhanced_kit_voice_)];
            voice.playing = false;
            voice.busy = false;
            continue;
        }

        int v = kEnhancedVoices - 1;
        for (int i = 0; i < kEnhancedVoices && !fixed; i++)
        {
            v = enhanced_voices_[size_t(i)].busy ? v : i;
        }

        enhanced_voices_[size_t(v)].busy = true;
        enhanced_kit_voice_ = v;
        EnhancedVoice& voice = enhanced_voices_[size_t(v)];
        voice.playing = true;
        voice.first = event.data & ~3u;
        voice.loop = event.loop;
        voice.level = float(event.volume) / kLevelScale;
        voice.position = 0;
        if (event.loop)
        {
            voice.first_pass = PointsOfRuns(int64_t(event.loop_end) - int64_t(event.data));
            voice.loop_first = event.loop_start & ~3u;
            voice.loop_points = PointsOfRuns(int64_t(event.loop_end) - int64_t(event.loop_start));
        }
        else
        {
            // The run that stops the voice leaves out its points.
            voice.heard = PointsOfRuns(event.length) - kRunPoints;
        }
    }

    // A voice that ended in an earlier frame is free again. A reset of the game ends every voice.
    for (EnhancedVoice& voice : enhanced_voices_)
    {
        voice.playing = voice.playing && !song_.Reset();
        if (!voice.playing)
        {
            voice.busy = false;
        }
    }
}

float Output::PointOf(const EnhancedVoice& voice, int64_t n) const
{
    if (n < 0)
    {
        return 0.0f;
    }

    if (!voice.loop)
    {
        return n < int64_t(voice.heard) ? float(int8_t(BusByte(rom_, voice.first + uint32_t(n)))) : 0.0f;
    }

    if (n < int64_t(voice.first_pass))
    {
        return float(int8_t(BusByte(rom_, voice.first + uint32_t(n))));
    }

    const int64_t in_loop = (n - int64_t(voice.first_pass)) % int64_t(voice.loop_points);
    return float(int8_t(BusByte(rom_, voice.loop_first + uint32_t(in_loop))));
}

void Output::MixEnhancedVoice(EnhancedVoice& voice, uint64_t move)
{
    const size_t samples = agb::kEnhancedFrameSamples;
    constexpr double kUnit = 4294967296.0;
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * kUnit));
    const int64_t width = kernel.HalfWidth();
    const uint64_t start = voice.position;
    voice.position += move;
    const bool heard = enhanced_frames_ && !voices_muted_;
    if (heard)
    {
        const int64_t first = int64_t(start >> 32) - (width - 1);
        const int64_t last = int64_t((start + move) >> 32) + width;
        std::vector<float> input(size_t(last - first + 1));
        for (int64_t n = first; n <= last; n++)
        {
            input[size_t(n - first)] = PointOf(voice, n);
        }

        for (size_t i = 0; i < samples; i++)
        {
            const uint64_t position = start + uint64_t(i) * move / samples;
            const int64_t point = int64_t(position >> 32);
            const float value =
                kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position >> 16) & 0xFFFF);
            enhanced_samples_[i] += value * voice.level;
        }
    }

    // A sample without a loop ends once the kernel has passed its last point.
    if (!voice.loop && int64_t(voice.position >> 32) - width > int64_t(voice.heard))
    {
        voice.playing = false;
    }
}

void Output::MixEnhanced()
{
    std::fill(enhanced_samples_.begin(), enhanced_samples_.end(), 0.0f);
    const uint64_t move = (uint64_t(agb::kFrameCycles) << 32) / (2 * uint64_t(OutputCycles(info_)));
    for (EnhancedVoice& voice : enhanced_voices_)
    {
        if (voice.playing)
        {
            MixEnhancedVoice(voice, move);
        }
    }

    if (metering_)
    {
        for (const float sample : enhanced_samples_)
        {
            voices_peak_ = std::max(voices_peak_, std::fabs(sample));
        }
    }
}

void Output::MeterVoices(bool on)
{
    metering_ = on;
    if (!on)
    {
        voices_peak_ = 0.0f;
    }
}

bool Output::VoicesSilent() const
{
    for (int v = 0; v < kVoiceCount; v++)
    {
        if (song_.VoicePlays(v))
        {
            return false;
        }
    }

    for (const EnhancedVoice& voice : enhanced_voices_)
    {
        if (voice.playing)
        {
            return false;
        }
    }

    return true;
}

} // namespace ntdlib::ubimilan
