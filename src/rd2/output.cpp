// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/output.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::rd2
{
namespace
{

// The buffer's parts: the mix's left and right sides, then the echo buffer's.
constexpr size_t kSide = kFrameSamples;
constexpr size_t kEchoBuffer = 2 * kSide;

// The level above which the echo is off.
constexpr uint8_t kLastEchoLevel = 15;

// The offsets in a sample's header. The points follow the header.
constexpr uint32_t kSampleLength = 0;
constexpr uint32_t kSampleLoopStart = 8;
constexpr uint32_t kSampleLoopEnd = 12;
constexpr uint32_t kSamplePoints = 16;

constexpr double kUnit = 4294967296.0; // the enhanced mode's positions in a point

} // namespace

Output::Output(const Rom& rom, const DriverInfo& info)
    : rom_(rom), info_(info), traits_(TraitsOf(info.revision)), history_(size_t(kEchoFrames) * kEchoBuffer)
{
    // The older revision's init leaves the level at its highest. Its echo always runs and shifts everything away.
    if (traits_.older_levels)
    {
        echo_level_ = 31;
        echo_goal_ = 31;
    }
}

void Output::Play(int song, int version)
{
    sequencer_.emplace(rom_, info_, song, version);
    sequencer_->KeepMixes(true);
    sequencer_->KeepTimes(false);
    voice_tracks_.assign(size_t(kVoices), -1);
    psg_tracks_.assign(1, {-1, -1, -1, -1});
    ran_ = false;
}

std::vector<uint8_t> Output::SoundRegisters() const
{
    std::vector<uint8_t> registers(agb::kSoundRegisterBytes);
    const auto set = [&registers](uint32_t address, uint8_t value)
    {
        registers[address - agb::kRegSound1CntL] = value;
    };

    set(0x04000060, 0x08); // square 1 without a sweep
    set(0x04000063, 0xF0); // square 1's envelope
    set(0x04000080, 0x77); // the PSG at full volume on both sides
    set(0x04000081, 0xFF); // each PSG channel on both sides
    set(0x04000082, 0x0D); // the PSG at half volume and the FIFOs at full volume
    set(0x04000083, 0x12); // FIFO A on the left and FIFO B on the right, both on timer 0
    set(0x04000084, 0x80); // the sound on
    set(0x04000089, 0x42); // with the bias's low byte at 0: the bias at 0x200, and 8 bits at 65,536 Hz

    return registers;
}

int Output::PsgChannelTrack(int unit, int channel) const
{
    return size_t(unit) < psg_tracks_.size() ? psg_tracks_[size_t(unit)][size_t(channel)] : -1;
}

void Output::StartEnhancedMixing()
{
    if (!sequencer_ || ran_)
    {
        throw std::logic_error("the enhanced mode starts after the request and before the song's first frame");
    }

    Enhanced enhanced;
    for (size_t side = 0; side < 2; side++)
    {
        enhanced.out[side].assign(agb::kEnhancedFrameSamples, 0.0f);
        enhanced.wet[side].assign(agb::kEnhancedFrameSamples, 0.0f);
        enhanced.line[side].assign(size_t(kEchoFrames) * agb::kEnhancedFrameSamples, 0.0f);
    }

    enhanced_.emplace(std::move(enhanced));
    sequencer_->GrowVoices(true);
}

void Output::Frame()
{
    ran_ = true;
    sequencer_->Step();
    FollowEvents();
    if (metering_)
    {
        peaks_ = {};
    }

    MixConsole();
}

void Output::FollowEvents()
{
    const std::vector<Voice>& voices = sequencer_->Voices();
    if (voice_tracks_.size() < voices.size())
    {
        voice_tracks_.resize(voices.size(), -1);
    }

    for (const Event& e : sequencer_->Events())
    {
        if (e.kind != Event::kNote || e.voice < 0)
        {
            continue;
        }

        voice_tracks_[size_t(e.voice)] = e.track;
        const Voice& v = voices[size_t(e.voice)];
        if (v.type != kSampleType)
        {
            if (size_t(v.unit) >= psg_tracks_.size())
            {
                psg_tracks_.resize(size_t(v.unit) + 1, {-1, -1, -1, -1});
            }

            psg_tracks_[v.unit][size_t(v.type - 1)] = e.track;
        }
    }
}

bool Output::Muted(int voice) const
{
    const int track = voice_tracks_[size_t(voice)];
    return track >= 0 && (muted_ >> track & 1);
}

void Output::MixVoice(const VoiceMix& mix, bool to_echo)
{
    // A voice's levels for its points, with its pan a byte and its level 16 bits.
    const bool older = traits_.older_levels;
    const uint32_t shift = older ? 7 : 8;
    const int32_t level = int32_t(mix.volume & 0xFFFF);
    const int32_t left = int32_t((uint32_t((127 - int32_t(mix.pan)) * level) & 0xFFFF) >> shift);
    const int32_t right = int32_t((uint32_t(int32_t(mix.pan) * level) & 0xFFFF) >> shift);
    const size_t base = to_echo ? kEchoBuffer : 0;
    const uint32_t data = mix.sample + kSamplePoints;
    const bool metering = metering_;
    const int track = voice_tracks_[size_t(mix.voice)];
    int32_t largest_left = 0;
    int32_t largest_right = 0;
    const std::vector<MixRun>& runs = sequencer_->MixRuns();
    for (uint32_t r = 0; r < mix.runs; r++)
    {
        const MixRun& run = runs[mix.run + r];
        uint32_t position = run.position;
        size_t at = base + size_t(run.first);
        for (int32_t i = 0; i < run.points; i++, at++)
        {
            const int32_t point = rom_.S8(data + (position >> 8));
            position += mix.step;
            int32_t part_left = left * point;
            int32_t part_right = right * point;
            if (metering)
            {
                largest_left = std::max(largest_left, std::abs(part_left));
                largest_right = std::max(largest_right, std::abs(part_right));
            }

            if (older)
            {
                part_left >>= 8;
                part_right >>= 8;
            }

            // A point past the buffers lands in the driver's other variables. The model drops it.
            if (at < buffer_.size())
            {
                buffer_[at] = int16_t(buffer_[at] + part_left);
            }
            if (at + kSide < buffer_.size())
            {
                buffer_[at + kSide] = int16_t(buffer_[at + kSide] + part_right);
            }
        }
    }

    if (metering && track >= 0)
    {
        const float scale = older ? 256.0f : 128.0f;
        std::array<float, 2>& peak = peaks_[size_t(track)];
        peak[0] = std::max(peak[0], float(largest_left) / scale);
        peak[1] = std::max(peak[1], float(largest_right) / scale);
    }
}

void Output::RunEcho()
{
    const uint8_t level = echo_level_;
    const bool older = traits_.older_levels;
    const uint32_t round = level == 0 ? 0 : 1u << (level - 1);
    const int32_t towards_zero = int32_t((1u << level) - 1);
    for (size_t i = 0; i < kEchoBuffer; i++)
    {
        int32_t e = buffer_[kEchoBuffer + i];
        buffer_[i] = int16_t(buffer_[i] + e);
        if (older)
        {
            e = int32_t(uint32_t(e) + round) >> level;
        }
        else
        {
            e = (e < 0 ? e + towards_zero : e) >> level;
        }

        buffer_[kEchoBuffer + i] = int16_t(e);
    }

    std::copy_n(buffer_.begin() + std::ptrdiff_t(kEchoBuffer), kEchoBuffer,
                history_.begin() + std::ptrdiff_t(size_t(echo_index_) * kEchoBuffer));
}

void Output::MakeBytes()
{
    const bool older = traits_.older_levels;
    const auto byte = [older](int32_t x)
    {
        if (!older)
        {
            x = (x < 0 ? x + 127 : x) >> 7;
        }

        const uint32_t high = uint32_t(x) & 0xFF80;
        if (high != 0 && high != 0xFF80)
        {
            x = 127 + int32_t(uint32_t(x) >> 31);
        }

        return int8_t(uint8_t(x));
    };

    bytes_silent_ = true;
    for (size_t i = 0; i < kSide; i++)
    {
        fifo_a_[i] = byte(buffer_[i]);
        fifo_b_[i] = byte(buffer_[kSide + i]);
        bytes_silent_ = bytes_silent_ && fifo_a_[i] == 0 && fifo_b_[i] == 0;
    }
}

void Output::MixConsole()
{
    const bool older = traits_.older_levels;
    const bool echo_runs = older || echo_level_ <= kLastEchoLevel;
    if (echo_runs)
    {
        std::copy_n(history_.begin() + std::ptrdiff_t(size_t(echo_index_) * kEchoBuffer), kEchoBuffer,
                    buffer_.begin() + std::ptrdiff_t(kEchoBuffer));
    }

    std::fill_n(buffer_.begin(), kEchoBuffer, int16_t(0));
    for (const VoiceMix& mix : sequencer_->Mixes())
    {
        if (!Muted(mix.voice))
        {
            MixVoice(mix, echo_runs && mix.echo != 0);
        }
    }

    if (enhanced_)
    {
        MixEnhanced(echo_runs);
    }

    // The level moves towards the level that the game asks for.
    if (echo_goal_ < echo_level_)
    {
        echo_level_--;
    }
    else if (echo_goal_ > echo_level_)
    {
        echo_level_++;
    }

    if (older)
    {
        RunEcho();
    }
    else
    {
        // After the level leaves the echo's range, the echo's frames run on until the history holds nothing.
        if (echo_level_ <= kLastEchoLevel)
        {
            echo_counter_ = kEchoFrames;
        }
        else if (echo_counter_ != 0)
        {
            echo_counter_--;
        }

        if (echo_counter_ != 0)
        {
            RunEcho();
        }
    }

    echo_index_++;
    if (echo_index_ >= kEchoFrames)
    {
        echo_index_ = 0;
    }

    MakeBytes();
}

bool Output::MixerSilent() const
{
    if (!sequencer_ || !bytes_silent_)
    {
        return false;
    }

    for (const Voice& v : sequencer_->Voices())
    {
        if (v.type == kSampleType && v.state != kVoiceFree)
        {
            return false;
        }
    }

    return std::all_of(history_.begin(), history_.end(), [](int16_t x) { return x == 0; });
}

void Output::MixEnhancedVoice(const VoiceMix& mix, bool to_echo)
{
    Enhanced& e = *enhanced_;
    if (e.voices.size() <= size_t(mix.voice))
    {
        e.voices.resize(size_t(mix.voice) + 1);
    }

    EnhancedVoice& v = e.voices[size_t(mix.voice)];
    if (mix.starts)
    {
        v.position = 0;
    }

    // The console's arithmetic, without the wraps of its levels: full level on a side gives a sample of the point.
    const float level = float(mix.volume & 0xFFFF);
    const float scale = 1.0f / 32768.0f;
    const std::array<float, 2> to = {float(std::max(127 - int(mix.pan), 0)) * level * scale,
                                     float(int(mix.pan)) * level * scale};
    const std::array<float, 2> from = mix.starts ? to : v.levels;
    v.levels = to;

    // A sample's position moves by the step for each of the console's points in the frame.
    const uint64_t move = uint64_t(mix.step) * kFrameSamples << 24;
    const uint64_t start = v.position;
    v.position += move;
    const bool heard = from[0] != 0.0f || from[1] != 0.0f || to[0] != 0.0f || to[1] != 0.0f;
    if (!enhanced_frames_ || !heard || Muted(mix.voice))
    {
        return;
    }

    const size_t samples = agb::kEnhancedFrameSamples;
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * kUnit));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(start >> 32) - (width - 1);
    const int64_t last = int64_t((start + move) >> 32) + width;
    const int64_t length = rom_.U32(mix.sample + kSampleLength);
    const int64_t loop_start = rom_.U32(mix.sample + kSampleLoopStart);
    const int64_t loop_end = rom_.U32(mix.sample + kSampleLoopEnd);
    const uint32_t data = mix.sample + kSamplePoints;
    const auto point_of = [&](int64_t at)
    {
        if (at < 0)
        {
            return 0.0f;
        }

        if (loop_end != 0)
        {
            if (at >= loop_end)
            {
                if (loop_end <= loop_start)
                {
                    return 0.0f;
                }

                at = loop_start + (at - loop_start) % (loop_end - loop_start);
            }
        }
        else if (at >= length)
        {
            return 0.0f;
        }

        return float(rom_.S8(data + uint32_t(at)));
    };

    std::vector<float> input(size_t(last - first + 1));
    for (int64_t n = first; n <= last; n++)
    {
        input[size_t(n - first)] = point_of(n);
    }

    std::array<std::vector<float>, 2>& target = to_echo ? e.wet : e.out;
    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    const int track = voice_tracks_[size_t(mix.voice)];
    for (size_t i = 0; i < samples; i++)
    {
        const uint64_t position = start + uint64_t(i) * (move / samples);
        const int64_t point = int64_t(position >> 32);
        const float value = kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position >> 16) & 0xFFFF);
        const float left = value * (from[0] + float(i) * slopes[0]);
        const float right = value * (from[1] + float(i) * slopes[1]);
        target[0][i] += left;
        target[1][i] += right;
        if (metering_ && track >= 0)
        {
            std::array<float, 2>& peak = peaks_[size_t(track)];
            peak[0] = std::max(peak[0], std::fabs(left));
            peak[1] = std::max(peak[1], std::fabs(right));
        }
    }
}

void Output::MixEnhanced(bool echo_runs)
{
    Enhanced& e = *enhanced_;
    const size_t samples = agb::kEnhancedFrameSamples;
    for (size_t side = 0; side < 2; side++)
    {
        std::fill(e.out[side].begin(), e.out[side].end(), 0.0f);
        std::fill(e.wet[side].begin(), e.wet[side].end(), 0.0f);
    }

    // The metering of the console's mixer runs beside the mode. It gives way to the mode's.
    if (metering_)
    {
        peaks_ = {};
    }

    for (const VoiceMix& mix : sequencer_->Mixes())
    {
        MixEnhancedVoice(mix, echo_runs && mix.echo != 0);
    }

    // The echo's line reads and writes the same place: its length gives the delay. An echo that the level has turned
    // off empties the line. The console stops reading its history at once, and clears it over the echo's frames.
    const uint8_t level = uint8_t(echo_goal_ < echo_level_   ? echo_level_ - 1
                                  : echo_goal_ > echo_level_ ? echo_level_ + 1
                                                             : echo_level_);
    const bool line_on = traits_.older_levels || echo_level_ <= kLastEchoLevel;
    const float feedback = level <= kLastEchoLevel ? std::ldexp(1.0f, -int(level)) : 0.0f;
    const size_t length = size_t(kEchoFrames) * samples;
    if (line_on)
    {
        for (size_t side = 0; side < 2; side++)
        {
            std::vector<float>& line = e.line[side];
            size_t at = e.line_position;
            for (size_t i = 0; i < samples; i++)
            {
                const float wet = line[at] + e.wet[side][i];
                e.out[side][i] += wet;
                const float kept = wet * feedback;
                line[at] = std::fabs(kept) < 1.0f / 65536.0f ? 0.0f : kept;
                at = at + 1 == length ? 0 : at + 1;
            }
        }
    }
    else
    {
        for (size_t side = 0; side < 2; side++)
        {
            std::fill_n(e.line[side].begin() + std::ptrdiff_t(e.line_position), samples, 0.0f);
        }
    }

    e.line_position = (e.line_position + samples) % length;
}

} // namespace ntdlib::rd2
