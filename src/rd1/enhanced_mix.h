// SPDX-License-Identifier: MIT

// The enhanced mixing mode of R&D1's mixer: its voices, its echo and its filter of CC72 in floating point at
// agb::kEnhancedRate. ArchivePlayer gives it the mixer's records before each frame's mix, and the console's samples
// that the mix covers. The console's mixer runs on beside it.
//
// - A voice's samples go through wave::SincKernel in place of the console's nearest sample or straight line. Its
//   levels move in a straight line across a frame, from the last frame's to the frame's. A voice that wasn't on, or
//   that doesn't go on from where the last frame left it, takes its levels at once.
// - A voice's level on a side is its gain times its level, plus the filter's gain for a voice through the filter, over
//   2^14: the console's arithmetic without its truncations.
// - The echo reads each FIFO's output from the delay that the console's ring gives it. It goes through the console's
//   two one-pole stages, at the console's level.
// - The filter of CC72 is the console's one-pole low-pass or high-pass. It takes the echo and the voices that go
//   through it, in the frames in which such a voice plays.
// - A one-pole stage of the console, y[n] = c y[n - 1] + (1 - c) x[n], delays its input by c / (1 - c) samples at low
//   frequencies. At the mode's rate, each stage keeps that delay's length in time. Its coefficient is
//   c / (c + r (1 - c)), where r is the console's samples in each of the mode's.
// - Nothing wraps, rounds or saturates.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/rd1/driver.h"
#include "ntdlib/rd1/game_driver.h"

namespace ntdlib::rd1
{

// Returns the little-endian word at byte `at` of a voice's record.
inline uint32_t RecordWord(const std::array<uint8_t, kVoiceRecordBytes>& record, size_t at)
{
    return uint32_t(record[at]) | uint32_t(record[at + 1]) << 8 | uint32_t(record[at + 2]) << 16 |
           uint32_t(record[at + 3]) << 24;
}

class EnhancedMix
{
public:
    // `period` is the cycles of the console's samples, and `lead` the console's samples that the FIFOs hold before the
    // first frame's mix.
    EnhancedMix(const Rom& rom, uint32_t period, uint64_t lead);

    // The mode's samples of silence before the first frame's: the time of the lead.
    size_t Lead() const
    {
        return lead_;
    }

    // Mixes the frame. The frame covers `samples` of the console's samples. Without `heard`, the frame's samples are
    // silent: the voices move on without mixing, and the echo's input is silent too.
    void Frame(const std::vector<MixVoice>& voices, const MixSettings& settings, size_t samples, bool heard);

    // The frame's samples of each FIFO, on the scale of 8-bit samples.
    const std::array<std::vector<float>, 2>& Out() const
    {
        return out_;
    }

    // The frame's largest part of each track's voices in each FIFO, on the same scale.
    const std::array<std::array<float, 2>, kMaxChannels>& Parts() const
    {
        return parts_;
    }

private:
    // What a mixer voice did in the last frame.
    struct VoiceState
    {
        bool known = false;
        uint32_t base = 0;
        uint32_t next = 0; // the place where the next frame goes on
        std::array<float, 2> levels = {};
    };

    uint64_t Scaled(uint64_t console_samples) const;
    float Past(size_t side, uint64_t delay, size_t i) const;
    void MixVoiceInto(const MixVoice& voice, const MixSettings& settings, size_t samples,
                      std::array<std::vector<float>, 2>& target);

    const Rom& rom_;
    uint32_t period_ = 0;
    double ratio_ = 0; // the console's samples in each of the mode's
    size_t lead_ = 0;
    uint64_t console_ = 0; // the console's samples up to the next frame's, from the start
    std::array<VoiceState, kMaxVoices> voices_ = {};
    std::array<std::vector<float>, 2> out_;
    std::array<std::array<float, 2>, kMaxChannels> parts_ = {};

    // The output's history for the echo, and the echo's and the filter's states on each side.
    std::array<std::vector<float>, 2> history_;
    uint64_t written_ = 0;
    std::array<float, 2> echo_low_ = {};
    std::array<float, 2> echo_band_ = {};
    std::array<float, 2> filter_state_ = {};
    bool filter_high_ = false;
};

} // namespace ntdlib::rd1
