// SPDX-License-Identifier: MIT

// The enhanced mixing mode of AlphaDream's mixer: its sample channels in floating point at agb::kEnhancedRate.
// ArchivePlayer gives it what each channel played in the console's mix of a frame. The console's mixer runs on beside
// it.
//
// - A channel's points go through wave::SincKernel in place of the console's nearest point, along the console's
//   places: the step that the console's step routine works out, round the sample's loop.
// - A channel's level on a side is its volume on that side times the envelope's level, over 2^16: the console's
//   arithmetic without its truncations. Its levels move in a straight line across a frame, from the last frame's to
//   the frame's. A channel that wasn't on, or that doesn't go on from where the last frame left it, takes its levels at
//   once.
// - A sample without a loop is silent past its end.
// - Nothing wraps, rounds or saturates.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/driver.h"

namespace ntdlib::alphadream
{

// What a sample channel played in a frame's mix of the console.
struct MixChannel
{
    int channel = 0;
    SampleHeader sample;
    uint32_t place = 0;              // the place at the frame's start, in 1024ths of a point
    uint32_t next = 0;               // the place where the mix left off
    uint32_t step = 0;               // the step for each of the console's samples, in 1024ths of a point
    int level = 0;                   // the envelope's level in the frame
    std::array<int, 2> volumes = {}; // FIFO A's and FIFO B's: the left and the right volume
    bool silenced = false;           // the player has left the channel out of the output
};

class EnhancedMix
{
public:
    // `period` is the cycles of the console's samples, and `lead` the console's samples that the FIFOs play before the
    // first frame's mix.
    EnhancedMix(const Rom& rom, uint32_t period, uint64_t lead);

    // The mode's samples of silence before the first frame's: the time of the lead.
    size_t Lead() const
    {
        return lead_;
    }

    // Mixes the frame: kFrameSamples of the console's samples. Without `heard`, the frame's samples are silent.
    void Frame(const std::vector<MixChannel>& channels, bool heard);

    // The frame's samples of each FIFO, on the scale of 8-bit samples.
    const std::array<std::vector<float>, 2>& Out() const
    {
        return out_;
    }

    // The frame's largest part of each sample channel in each FIFO, on the same scale.
    const std::array<std::array<float, 2>, kSampleTracks>& Parts() const
    {
        return parts_;
    }

private:
    // What a channel did in the last frame.
    struct ChannelState
    {
        bool known = false;
        uint32_t data = 0;
        uint32_t next = 0; // the place where the next frame goes on
        std::array<float, 2> levels = {};
    };

    uint64_t Scaled(uint64_t console_samples) const;
    void MixChannelInto(const MixChannel& channel, size_t n);

    const Rom& rom_;
    uint32_t period_ = 0;
    size_t lead_ = 0;
    uint64_t console_ = 0; // the console's samples up to the next frame's, from the start
    std::array<ChannelState, kSampleTracks> channels_ = {};
    std::array<std::vector<float>, 2> out_;
    std::array<std::array<float, 2>, kSampleTracks> parts_ = {};
};

} // namespace ntdlib::alphadream
