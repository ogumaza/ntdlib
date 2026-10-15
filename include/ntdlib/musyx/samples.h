// SPDX-License-Identifier: MIT

// A MusyX sample directory and the samples file that it describes. Each entry gives a sample's place in the samples
// file, its rate, the key that plays it at that rate, its loop and its format. A DSP-ADPCM sample also has the
// decoder's coefficients and the state that the decoder takes up at each repeat of the loop.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ntdlib::musyx
{

// The formats of a sample.
enum class SampleFormat : uint8_t
{
    kAdpcm = 0,     // DSP-ADPCM
    kAdpcmPlus = 1, // DSP-ADPCM, with the decoder's state at each frame for starts within the sample
    kPcm16 = 2,
    kPcm8 = 3,
};

// The DSP-ADPCM decoder's settings for a sample. The decoder starts from silence with the first frame's scale. At each
// repeat of the loop, it takes up the loop's scale and the two samples before the loop start.
struct AdpcmInfo
{
    uint8_t initial_scale = 0;
    uint8_t loop_scale = 0;
    int16_t loop_hist1 = 0; // the sample just before the loop start
    int16_t loop_hist2 = 0; // the one before that
    std::array<int16_t, 16> coefficients{};
};

// An entry of a sample directory.
struct SampleInfo
{
    uint16_t id = 0;
    uint32_t offset = 0; // in the samples file
    uint8_t key = 60;    // the key that plays the sample at its rate
    uint32_t rate = 32000;
    SampleFormat format = SampleFormat::kAdpcm;
    uint32_t length = 0; // in samples
    uint32_t loop_start = 0;
    uint32_t loop_length = 0; // 0 for a sample that doesn't loop
    std::optional<AdpcmInfo> adpcm;
};

// Reads the sample directory `file`, in the file's order. Throws FormatError if it's damaged, or if a sample has a
// format other than those of SampleFormat.
std::vector<SampleInfo> ReadSampleDirectory(std::span<const uint8_t> file);

// A sample decoded to 16-bit PCM.
struct DecodedSample
{
    std::vector<int16_t> pcm;
    bool loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // one past the loop's last sample
};

// Decodes `info`'s sample from the samples file `samples`. A sample that loops ends at its loop end. A repeat of a
// DSP-ADPCM loop decodes from the loop's state in AdpcmInfo. Where a repeat would differ from the first pass, the
// result holds the first pass and then one repeat, and its loop spans the repeat. Every later repeat then plays the
// same samples. Throws FormatError if the sample runs past the end of `samples`, or if a DSP-ADPCM sample has no
// AdpcmInfo.
DecodedSample DecodeSample(const SampleInfo& info, std::span<const uint8_t> samples);

} // namespace ntdlib::musyx
