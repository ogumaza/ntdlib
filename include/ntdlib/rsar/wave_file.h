// SPDX-License-Identifier: MIT

// A wave of the Wii as its values, and a wave archive of the Wii (RWAR), in parts that a writer can put back together.
// The writers lay the files out as NintendoWare for Revolution's tools do.
//
// A wave's info takes 28 bytes: its encoding, whether it loops, its number of channels, its rate, its loop's start and
// end as DSP addresses, the offset of its table of channels, and the place of its samples. The table holds the offset
// of each channel's info. A channel's info takes 28 bytes: the offset of its samples from the place of the wave's
// samples, the offset of its DSP-ADPCM info or 0, and its four volumes. The table follows the wave's info, then the
// channels' infos, then the DSP-ADPCM infos of a DSP-ADPCM wave's channels, 48 bytes each. The other offsets count from
// the wave's info. A loop's start is the address of its first sample, and its end the address of the wave's last
// sample: in nibbles for DSP-ADPCM, with two for the header of each frame of 14 samples, in samples for PCM16 and in
// bytes for PCM8. A wave without a loop gives the same addresses. A loop can start past the wave's last sample: it then
// holds no sample.
//
// An RWAV file's header takes 32 bytes and lists INFO and DATA. INFO holds the wave's info. The place of its samples is
// the offset of DATA from the wave's info. Zeros pad INFO to 32 bytes. DATA's samples start 8 bytes into the block, and
// each channel's samples follow the previous channel's. Zeros pad DATA to 32 bytes.
//
// An RWAR file's header lists TABL and DATA. TABL holds the number of waves, then a reference to each wave from DATA's
// start and the wave's size. Zeros pad TABL to 32 bytes. The waves start 32 bytes into DATA, one after another.
//
// A bank of version 1.1 and a wave sound file up to version 1.2 describe their waves in a WAVE block, over raw samples
// in their wave data. Each wave's samples start on a multiple of 32 bytes there, in the order of the waves, and the
// place of a wave's samples is their offset. Each channel's samples follow the previous channel's. Zeros pad the wave
// data to 32 bytes.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/rsar/formats.h"

namespace ntdlib::rsar
{

struct WaveFile
{
    // A channel's volume of 1 as 8.24 fixed point.
    static constexpr uint32_t kUnity = 0x01000000;

    // A channel's samples in the wave's encoding, big-endian, its DSP-ADPCM info, and its volumes to the front left,
    // the front right, the rear left and the rear right, as 8.24 fixed point.
    struct Channel
    {
        friend bool operator==(const Channel&, const Channel&) = default;

        std::vector<uint8_t> samples;
        std::optional<DspAdpcmInfo> adpcm;
        std::array<uint32_t, 4> volumes = {kUnity, kUnity, kUnity, kUnity};
    };

    // Reads an RWAV file. A channel's samples run up to the next channel's samples. The last channel's run to the end
    // of DATA, without the zeros that pad DATA to 32 bytes past the wave's last sample. Throws xsflib::FormatError for
    // bytes that aren't an RWAV file, one without INFO or DATA, and channels whose samples don't follow one another in
    // the channels' order. A file laid out another way reads too, and Write() then gives other bytes.
    static WaveFile Read(std::span<const uint8_t> file);

    // The RWAV file's bytes. Throws std::invalid_argument for a wave without channels or of more than 255, a rate past
    // 24 bits, a wave without samples, a loop that starts past the wave's end, an end past what the addresses hold, a
    // DSP-ADPCM channel without its info, and a PCM channel with one.
    std::vector<uint8_t> Write() const;

    friend bool operator==(const WaveFile&, const WaveFile&) = default;

    uint16_t version = 0x0102;
    WaveEncoding encoding = WaveEncoding::kDspAdpcm;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0; // the loop's first sample
    uint32_t loop_end = 0;   // one past the wave's last sample: the wave's length, looping or not
    std::vector<Channel> channels;
};

struct WaveArchiveFile
{
    // Reads `file`. Throws xsflib::FormatError for bytes that aren't an RWAR file, and one without TABL or DATA. A file
    // laid out another way reads too, and Write() then gives other bytes.
    static WaveArchiveFile Read(std::span<const uint8_t> file);

    // The file's bytes. Throws std::invalid_argument for a wave whose size isn't a multiple of 32 bytes.
    std::vector<uint8_t> Write() const;

    friend bool operator==(const WaveArchiveFile&, const WaveArchiveFile&) = default;

    uint16_t version = 0x0100;
    std::vector<std::vector<uint8_t>> waves; // each an RWAV file's bytes, in the archive's order
};

// The wave data of a bank of version 1.1 or of a wave sound file of version 1.2 that holds `waves`: their raw samples,
// laid out as the tools lay them out. A WAVE block that describes the same waves names their places there.
std::vector<uint8_t> RawWaveData(std::span<const WaveFile> waves);

} // namespace ntdlib::rsar
