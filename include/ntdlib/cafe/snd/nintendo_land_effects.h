// SPDX-License-Identifier: MIT

// Nintendo Land's two effect classes for AX's aux buses: a reverb and a delay. The game's code defines them. They
// derive from nw::snd's FxBase, and the game appends them to its buses with nw::snd's AppendEffect. Comments give the
// addresses in the executable of the American release (Lunch.rpx).
//
// Each model follows its class's code operation for operation. The classes compute in paired singles, the left channel
// in one half and the right in the other. The models fuse the same multiplies and adds (axfx::Fmadds and
// axfx::NegatedFms) and keep every other operation apart. The reverb takes its combs' gains from the powf that the game
// links (0x02B01FBC). For each scene's settings, axfx::Pow gives the same bits.
//
// A class's UpdateBuffer processes 96 samples of the left and right channels: a frame of the 32 kHz renderer. The
// surround channel stays as it is. Unlike nw::snd's classes, these process their frames while the device mixes for
// Dolby Pro Logic II too.
//
// Both classes have two paths. In the unthreaded path, UpdateBuffer runs the class's kernel on the bus's samples in
// place. In the threaded path, UpdateBuffer exchanges the bus's frame for one from a ring of frames (FrameRing). A
// thread of the game's then runs the kernel on the frames that came in. Every scene of the game takes the threaded
// path.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/cafe/axfx/axfx.h"

namespace ntdlib::cafe::snd::nintendo_land
{

// The samples of each channel that UpdateBuffer processes in a frame.
inline constexpr uint32_t kFrameSamples = 96;

// The longest line that a model sets up, in samples (about 22 s at 48 kHz). A parameter that asks for a longer one
// fails Init.
inline constexpr uint32_t kMaxLength = 1u << 20;

// The threaded path's ring of frames (reverb 0x02018B80 and 0x02018624, delay 0x02017D24 and 0x020178D0). Each frame
// that comes in goes into the ring. Until `start` frames have come in, the bus gets silence in its place. From then on,
// the bus gets the oldest processed frame. After each exchange, the worker thread runs the kernel in place on each
// frame that came in since its last run. The model runs the kernel right after the exchange. The model thereby assumes
// that the thread always finishes before the next frame. Every scene sets a start of 4 and a ring of 8 frames. The bus
// then gets each frame 3 frames (288 samples) late. The first 3 frames are silent.
class FrameRing
{
public:
    // Clears a ring of `frames` frames and sets the count of frames before the first comes out to `start`. Returns
    // false for a ring of no frames or of more than 64.
    bool Init(uint32_t frames, uint32_t start);

    // Puts the frame of `left` and `right` into the ring, and puts the oldest processed frame, or silence, in its
    // place. Then runs `kernel(left, right)` on each frame that came in since the last run.
    template <typename Kernel>
    void Run(int32_t* left, int32_t* right, Kernel&& kernel)
    {
        Exchange(left, right);
        while (processed_ != write_)
        {
            kernel(ring_[0].data() + processed_, ring_[1].data() + processed_);
            Step(processed_);
        }
    }

private:
    void Exchange(int32_t* left, int32_t* right);

    // Moves a position on by a frame. The position wraps to 0 at the ring's end.
    void Step(uint32_t& position) const;

    std::array<std::vector<int32_t>, 2> ring_;
    uint32_t size_ = 0;      // samples in each channel's ring
    uint32_t start_ = 0;     // frames before the first comes out
    uint32_t write_ = 0;     // where the next frame goes in
    uint32_t read_ = 0;      // the next frame to come out
    uint32_t processed_ = 0; // the first frame that the kernel hasn't run on
    uint32_t count_ = 0;     // frames that came in, up to start_
};

// A delay line of stereo samples and its position.
struct PairLine
{
    std::vector<std::array<float, 2>> samples;
    uint32_t position = 0;

    // Moves the position on by a sample. The position wraps to 0 at the line's end.
    void Step();
};

// The game's reverb (vtable 0x10006F98). An early reflection runs in parallel with a pre-delay, two combs in parallel,
// an all-pass, a low-pass and a second all-pass. The output adds the two paths. A copy of a Reverb holds its whole
// state, its lines included.
class Reverb
{
public:
    // The words of the class's parameter structure, in order: the pre-delay (s), the time in which the combs decay by
    // 60 dB (s), the all-passes' coefficient, the damping, the gain, the early reflection's length (an index), the
    // set of comb and all-pass lengths (an index), the early reflection's level, the reverb's level, the rate (0 for
    // 32 kHz, 1 for 48 kHz), whether it takes the threaded path (the word's first byte), and the ring's frames and
    // start (FrameRing).
    static constexpr uint32_t kParamWords = 13;

    // SetParam (0x02019044) and Initialize (0x02018B04): the lengths and coefficients from `words`, at least
    // kParamWords of them, with every line and the ring cleared. Returns false for an index past the end of its
    // table, a line longer than kMaxLength or a ring that FrameRing::Init refuses. The effect then stays off.
    bool Init(std::span<const uint32_t> words);

    // UpdateBuffer (0x02018D38): the first 96 samples of `frame`'s left and right channels, in place. A frame of fewer
    // samples, or without both channels, stays as it is. So does every frame while the effect is off.
    void Process(const axfx::Buffers& frame, uint32_t samples);

private:
    // The kernel (0x020182C4): 96 samples in place.
    void Kernel(int32_t* left, int32_t* right);

    bool on_ = false;
    bool threaded_ = false;
    FrameRing ring_;
    PairLine early_;
    PairLine pre_; // no samples for no pre-delay
    std::array<PairLine, 2> combs_;
    std::array<PairLine, 2> all_passes_;
    float early_feedback_ = 0.0f; // the early reflection's feedback, -0.33 or 0.33
    float early_gain_ = 0.0f;
    std::array<float, 2> comb_gains_{};
    float coloration_ = 0.0f; // the all-passes' coefficient
    float damping_ = 0.0f;    // at least 0.05
    float keep_ = 0.0f;       // 1 less the damping
    float reverb_gain_ = 0.0f;
    std::array<float, 2> low_pass_{};
};

// The game's delay (vtable 0x10006B84). Each channel runs through a one-pole low-pass into a delay line with feedback.
// The output takes the line's output at the level. A copy of a Delay holds its whole state, its line included.
class Delay
{
public:
    // The words of the class's parameter structure, in order: the delay (ms), the feedback, the level, the damping,
    // the rate (0 for 32 kHz, 1 for 48 kHz), whether it takes the threaded path (the word's first byte), and the ring's
    // frames and start (FrameRing).
    static constexpr uint32_t kParamWords = 8;

    // SetParam (0x02018004) and Initialize (0x02017CA8): the length and coefficients from `words`, at least
    // kParamWords of them, with the line and the ring cleared. A delay shorter than a sample keeps a line of one
    // sample. Returns false for a line longer than kMaxLength or a ring that FrameRing::Init refuses. The effect then
    // stays off.
    bool Init(std::span<const uint32_t> words);

    // UpdateBuffer (0x02017EDC), as Reverb::Process.
    void Process(const axfx::Buffers& frame, uint32_t samples);

private:
    // The kernel (0x020177E0): 96 samples in place.
    void Kernel(int32_t* left, int32_t* right);

    bool on_ = false;
    bool threaded_ = false;
    FrameRing ring_;
    PairLine line_;
    float feedback_ = 0.0f;
    float level_ = 0.0f;
    float damping_ = 0.0f; // at least 0.05
    float keep_ = 0.0f;    // 1 less the damping
    std::array<float, 2> low_pass_{};
};

} // namespace ntdlib::cafe::snd::nintendo_land
