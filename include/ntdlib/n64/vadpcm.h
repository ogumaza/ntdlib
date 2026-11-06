// SPDX-License-Identifier: MIT

// VADPCM, the Nintendo 64's compressed sample format. A wave is a run of frames of 9 bytes. A frame's first byte holds
// a scale in its high 4 bits and the number of a predictor in its low 4 bits. The other 8 bytes hold 16 residuals of 4
// bits, the high nibble first. A frame gives 16 samples in two groups of 8.
//
// A book holds the predictors. Each predictor has `order` rows of 8 coefficients. A coefficient is a fixed-point value
// with 11 fractional bits. The audio microcodes and libultra's tools use an order of 2. Each sample of a group is the
// sum of four parts, shifted right by 11 and saturated to 16 bits: the sample before the group times the first row, the
// group's last sample before that times the second row, the residual shifted left by 11, and the group's earlier
// residuals times the second row. The sum's shift rounds towards minus infinity.
//
// The decoder is the arithmetic of the audio microcodes. The encoder finds, for each frame, a predictor and a scale
// that give the requested samples exactly. Samples that came from the decoder therefore encode to frames that decode to
// them again. Other samples get the frame with the least squared error.
//
// A small frame holds 5 bytes, for Nintendo EAD's later engines. Its first byte is the same. The other 4 hold 16
// residuals of 2 bits, the high bits first. The rest of the arithmetic is the same. The microcodes' scales stop at 12
// for a frame and at 14 for a small frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ntdlib::n64
{

constexpr std::size_t kVadpcmFrameSize = 9;
constexpr std::size_t kVadpcmSmallFrameSize = 5;
constexpr std::size_t kVadpcmFrameSamples = 16;
constexpr std::size_t kVadpcmGroupSamples = 8;
constexpr int kVadpcmCoefficientBits = 11;

// A book of predictors, in the layout of a bank file's ALADPCMBook. `coefficients` holds order * predictor_count * 8
// values: the rows of predictor 0, then those of predictor 1, and so on.
struct VadpcmBook
{
    friend bool operator==(const VadpcmBook&, const VadpcmBook&) = default;

    int32_t order = 2;
    int32_t predictor_count = 0;
    std::vector<int16_t> coefficients = {};

    // The coefficient of a predictor's row at a sample of the group.
    int16_t Coefficient(int predictor, int row, int sample) const;

    // Whether the sizes fit together and the order is 2. The decoder and the encoder take no other book.
    bool Usable() const;
};

// The 16 samples that a frame gives. The microcodes keep them between frames, and a bank file's loops hold them.
using VadpcmState = std::array<int16_t, kVadpcmFrameSamples>;

// A frame's fields.
struct VadpcmFrame
{
    friend bool operator==(const VadpcmFrame&, const VadpcmFrame&) = default;

    uint8_t scale = 0;
    uint8_t predictor = 0;
    std::array<int8_t, kVadpcmFrameSamples> residuals = {}; // from -8 to 7

    static VadpcmFrame Parse(std::span<const uint8_t, kVadpcmFrameSize> bytes);
    std::array<uint8_t, kVadpcmFrameSize> Write() const;
};

// Decodes a frame. `state` holds the previous frame's samples and receives this frame's. A predictor that the book
// lacks throws xsflib::FormatError.
void DecodeVadpcmFrame(std::span<const uint8_t, kVadpcmFrameSize> frame, const VadpcmBook& book, VadpcmState& state);

// Decodes every whole frame of `frames`, from a state of zeros or the given one. Bytes after the last whole frame are
// left out. `state` receives the state after the last frame.
std::vector<int16_t> DecodeVadpcm(std::span<const uint8_t> frames, const VadpcmBook& book);
std::vector<int16_t> DecodeVadpcm(std::span<const uint8_t> frames, const VadpcmBook& book, VadpcmState& state);

// The same for small frames.
void DecodeVadpcmSmallFrame(std::span<const uint8_t, kVadpcmSmallFrameSize> frame, const VadpcmBook& book,
                            VadpcmState& state);
std::vector<int16_t> DecodeVadpcmSmall(std::span<const uint8_t> frames, const VadpcmBook& book);
std::vector<int16_t> DecodeVadpcmSmall(std::span<const uint8_t> frames, const VadpcmBook& book, VadpcmState& state);

// Encodes samples to frames from a state of zeros. Samples short of a whole frame are followed by zeros. Samples that
// the decoder gave under `book` come out as frames that decode to the same samples.
std::vector<uint8_t> EncodeVadpcm(std::span<const int16_t> samples, const VadpcmBook& book);

} // namespace ntdlib::n64
