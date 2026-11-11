// SPDX-License-Identifier: MIT

// The Photo Channel 1.1's mood effect, modelled on the channel's code. The effect isn't part of AXFX. The channel
// defines it as an effect class of NW4R SND (FxBase, with its virtual functions at 0x801f3d70). The model is kept with
// the AXFX models because it runs on the same frames. As the channel starts, its sound manager puts the effect on bus B
// (0x80048a40). The effect starts off. While a photo with a mood is shown, the channel turns it on (0x80049614) and
// sets its mode by the mood (SetMode, 0x80049820). Moods 1 to 4 give modes 1, 4, 3 and 2 (0x800212c8). The channel
// also moves the music's sends from the main mix and bus C to bus B. The effect's output then takes the music's place:
//
// - Mode 1, an echo: each sample adds 0.6 of the effect's output from 60 frames before. An 11-tap band-pass filter from
//   1 kHz to 14 kHz then runs on the sum.
// - Mode 2, an octave down: two grains, 32 frames apart, read the input at half speed. Each has a Hamming window 64
//   frames long. The output is 1.2 times their sum.
// - Mode 3, a vibrato: each sample reads the input from 170 to 214 samples before, by a sine with a period of 30
//   frames. 0.2 of the effect's output from 60 frames before is mixed with 0.8 of that.
// - Mode 4, an old radio: the two channels make one. A 21-tap band-pass filter from 1 kHz to 3.4 kHz runs on it. A
//   random gain from 0.9 to 1.0 changes every 8 samples, and the low 7 bits of each sample are cleared. Both channels
//   get the result.
//
// The arithmetic is in 32-bit integers that wrap, with gains in 4096ths. The constructor (0x8004984c) builds the
// filters, the window and the vibrato's offsets from nw4r::math's table of sines. The model builds them the same way.
// UpdateBuffer (0x80049d80) handles the left and right channels and leaves the surround channel as it is.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/rvl/axfx/axfx.h"

namespace ntdlib::rvl::axfx
{

// The effect object. A copy holds the whole state.
class PhotoMood
{
public:
    // The frames that each of the effect's rings holds.
    static constexpr int kRingFrames = 64;
    static constexpr int kRingSamples = kRingFrames * kSamplesPerFrame;

    // The frames of a vibrato's period.
    static constexpr int kVibratoFrames = 30;

    // The mode that SetMode (0x80049820) stores at +0x10. SetMode holds it to 0 to 4. Mode 0 leaves the frames as they
    // are.
    struct Params
    {
        uint32_t mode = 0;
    };

    // The tables that the constructor builds, at their offsets in the object.
    struct Tables
    {
        std::array<int32_t, 11> echo_filter{};                                   // +0x1801c, mode 1
        std::array<int32_t, 21> radio_filter{};                                  // +0x18048, mode 4
        std::array<int32_t, kRingSamples> window{};                              // +0x1809c, mode 2
        std::array<int32_t, kVibratoFrames * kSamplesPerFrame> vibrato_offset{}; // +0x1e09c, mode 3
    };

    // One entry of nw4r::math's table of sines and cosines (0x801edf10): the sine and cosine of 2π i / 256, and the
    // steps to the next entry's, each rounded to 6 decimals.
    struct SinCos
    {
        float sin;
        float cos;
        float sin_step;
        float cos_step;
    };

    PhotoMood();

    // The parameters that SetMode gives for `mode`.
    static Params SetMode(int32_t mode)
    {
        return {static_cast<uint32_t>(mode <= 0 ? 0 : (mode >= 4 ? 4 : mode))};
    }

    // The constructor's tables. Every object has the same ones.
    static const Tables& BuiltTables();

    // The 256 entries of nw4r::math's table of sines and cosines. The model computes them.
    static const std::array<SinCos, 256>& SinCosTable();

    // nw4r::math's SinFIdx (0x8013c4d4) and CosFIdx (0x8013c53c): the sine and the cosine of an angle in 256ths of a
    // turn, from the table, with a linear step between its entries.
    static float SinFIdx(float index);
    static float CosFIdx(float index);

    // StartUp (0x8004b9a0) does nothing but succeed. The model takes the mode here.
    bool Init(const Params& params);

    // SetMode while the effect runs.
    bool Settings(const Params& params);

    // Shutdown (0x8004b99c) does nothing.
    void Shutdown();

    // UpdateBuffer (0x80049d80): stores the frame's input in the input rings, applies the mode and stores the output in
    // the output rings. The rings move on by a frame.
    void Process(AuxFrame& frame);

private:
    // The ring that holds a channel's input or output.
    static int InputRing(int channel)
    {
        return channel;
    }

    static int OutputRing(int channel)
    {
        return 2 + channel;
    }

    // The ring's sample `t` samples from the start of the frame. A negative `t` reaches back into earlier frames.
    int32_t& At(int ring, int t);

    // The modes. The game puts modes 1 and 3 inline in UpdateBuffer. Mode 2 is at 0x8004a708 and mode 4 at 0x8004aac4.
    void Echo(AuxFrame& frame);
    void OctaveDown(AuxFrame& frame);
    void Vibrato(AuxFrame& frame);
    void Radio(AuxFrame& frame);

    // The four rings, one after another as the object holds them (+0x1c): the left input, the right input, the left
    // output and the right output. Mode 2 reads one sample past the end of an input ring, into the next ring. The game
    // does the same.
    std::vector<int32_t> rings_;

    // The frame counter at +0x18. Each ring's frame `position_ % 64` is the current frame.
    int32_t position_ = 0;

    uint32_t mode_ = 0;

    // Mode 4's random number at 0x80383910. It starts at 0 as the channel starts.
    uint32_t random_ = 0;
};

} // namespace ntdlib::rvl::axfx
