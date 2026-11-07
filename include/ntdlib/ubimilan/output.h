// SPDX-License-Identifier: MIT

// A model of Ubisoft Milan's driver with its mixer: the driver's stores to the PSG's registers (ubimilan::Sequencer, in
// its strict mode), and the bytes that the mixer gives the FIFOs. Each of the mixer's runs makes 16 bytes from 8 points
// of each voice that plays. It multiplies each point by its voice's volume, and adds the voices. It puts a point
// halfway between each pair of sums, and averages each point with the one before. Each byte is that point over 64,
// clipped to 8 bits. Both FIFOs play the same bytes: the music is mono. A DMA word transfer copies a voice's points,
// and takes them from the word that holds the voice's position.
//
// In the enhanced mixing mode (StartEnhancedMixing), the console's mixer goes on as before: the driver's stores, its
// voices and the end of its notes stay the console's. Frame() also gives agb::kEnhancedFrameSamples samples a frame in
// floating point. The mode has no anchor: only a measure against the console's mode shows what it gives. Its rules:
//
// - The samples lie on the scale of the console's 8-bit bytes. Nothing clips or rounds a sum. A voice's level is its
//   volume over 64. The console's level is the same.
// - The kit's notes take kEnhancedVoices voices in place of the mixer's 4, by the driver's rules: a piece that keeps a
//   voice for the kit plays each note on its last voice. A note takes the last voice that no sound has, or else its
//   last voice. A note-off stops the voice of the kit's last note. A voice is free again in the frame after the one
//   that ends it. A reset of the game ends every voice.
// - A voice plays the console's points: the same points in the same order, 8 at a time, up to the same loop. A sample's
//   position moves on at the console's rate of points. The band-limited interpolator (wave::SincKernel) takes the
//   points around each position, in place of the console's points in between and its average. Before a sample's start
//   and past its end, the points are 0.
//
// Each operation takes its operands in a fixed order, and every platform gives the same samples.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/sequencer.h"

namespace ntdlib::ubimilan
{

// The voices of the enhanced mixing mode.
inline constexpr int kEnhancedVoices = 32;

class Output
{
public:
    // Keeps references to `rom` and `info`. They have to outlive this. Starts song `song` as the game does.
    Output(const Rom& rom, const DriverInfo& info, int song);

    // Returns false if the song can't be played.
    bool Valid() const
    {
        return song_.Valid();
    }

    // Runs a frame: the sequencer's step, the sound update and the mixer's runs. SoundWrites() and Fifo() hold what the
    // frame gave, and in the enhanced mode, EnhancedFifo().
    void Frame();

    // The last frame's stores to the PSG's registers and the wave RAM, in order. The first frame's take in the stores
    // of the piece's start.
    const std::vector<agb::RegisterWrite>& SoundWrites() const
    {
        return writes_;
    }

    // The bytes that the last frame gave the FIFOs. Both FIFOs take them.
    const std::vector<int8_t>& Fifo() const
    {
        return bytes_;
    }

    // Puts the model in the enhanced mixing mode from its current frame on.
    void StartEnhancedMixing();

    // The samples that the last frame gave the FIFOs in the enhanced mode: agb::kEnhancedFrameSamples, or none outside
    // the mode. They're silent while SetEnhancedFrames(false) holds.
    const std::vector<float>& EnhancedFifo() const
    {
        return enhanced_samples_;
    }

    // Lets a frame in the enhanced mode skip its samples, to reach a place in a song quickly. The voices move on.
    void SetEnhancedFrames(bool on)
    {
        enhanced_frames_ = on;
    }

    // Leaves the voices out of the mix from the next frame on, when `muted`. They go on as before.
    void MuteVoices(bool muted)
    {
        voices_muted_ = muted;
    }

    // From the next frame on, keeps the largest sum of the voices in the frame's samples when `on`.
    void MeterVoices(bool on);

    // The last frame's largest sum of the voices, on the scale of the console's 8-bit bytes. 0 without meters, for
    // muted voices and after a frame in the enhanced mode without samples.
    float VoicesPeak() const
    {
        return voices_peak_;
    }

    // Whether the mixer's voices, and the enhanced mode's, have all stopped.
    bool VoicesSilent() const;

    const Sequencer& Song() const
    {
        return song_;
    }

private:
    // A voice of the enhanced mode.
    struct EnhancedVoice
    {
        bool busy = false;
        bool playing = false;
        uint32_t first = 0; // the word of the sample's first point
        uint32_t heard = 0; // a sample without a loop: the points that the console's mixer adds
        bool loop = false;
        uint32_t first_pass = 0; // a sample with a loop: the points before the voice first goes back
        uint32_t loop_first = 0; // the word of the loop's first point
        uint32_t loop_points = 0;
        float level = 0.0f;
        uint64_t position = 0; // in 2^32nds of a point
    };

    void MixRun(const MixerRun& run);
    void TakeKitEvents();
    void MixEnhanced();
    float PointOf(const EnhancedVoice& voice, int64_t n) const;
    void MixEnhancedVoice(EnhancedVoice& voice, uint64_t move);

    const Rom& rom_;
    const DriverInfo& info_;
    Sequencer song_;
    std::vector<agb::RegisterWrite> writes_;
    std::vector<int8_t> bytes_;
    std::vector<MixerRun> runs_;
    int32_t last_sum_ = 0;  // the last sum of the mixer's last run
    int32_t last_half_ = 0; // the half of the mixer's last point, for its average
    bool voices_muted_ = false;
    bool metering_ = false;
    float voices_peak_ = 0.0f;

    // The enhanced mode.
    bool enhanced_ = false;
    bool enhanced_frames_ = true;
    std::vector<EnhancedVoice> enhanced_voices_;
    int enhanced_kit_voice_ = 0;
    std::vector<float> enhanced_samples_;
};

} // namespace ntdlib::ubimilan
