// SPDX-License-Identifier: MIT

// The wave infos that an RWAV file, a bank's WAVE block and a wave sound file's WAVE block hold, and the raw samples
// that a WAVE block describes (ntdlib/rsar/wave_file.h).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/encode.h"
#include "rsar/file_blocks.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar::detail
{

inline constexpr std::size_t kWaveInfoSize = 0x1C;
inline constexpr std::size_t kChannelInfoSize = 0x1C;
inline constexpr std::size_t kAdpcmInfoSize = 0x30;

// The DSP address of sample `sample` in `encoding`: a nibble's for DSP-ADPCM, past the two of each frame's header, and
// the sample's for PCM.
inline uint32_t AddressOf(uint32_t sample, WaveEncoding encoding)
{
    if (encoding != WaveEncoding::kDspAdpcm)
    {
        return sample;
    }

    return wave::DspAdpcmNibbleAddress(sample);
}

// The sample that DSP address `address` points at in `encoding`. An address on a frame's header counts as the frame's
// first sample.
inline uint32_t SampleAt(uint32_t address, WaveEncoding encoding)
{
    if (encoding != WaveEncoding::kDspAdpcm)
    {
        return address;
    }

    return wave::DspAdpcmSampleAt(address);
}

// The bytes that `length` samples take in `encoding`: a byte each in 8-bit PCM, two in 16-bit PCM, and whole frames of
// 8 bytes in DSP-ADPCM.
inline std::size_t SamplesSize(WaveEncoding encoding, uint32_t length)
{
    switch (encoding)
    {
    case WaveEncoding::kDspAdpcm:
        return wave::DspAdpcmBytes(length);
    case WaveEncoding::kPcm16:
        return std::size_t{length} * 2;
    default:
        return length;
    }
}

// The size of the info of a wave of `channels` channels in `encoding`, with its table and its channels' infos.
std::size_t WaveInfoSize(WaveEncoding encoding, std::size_t channels);

// Puts the info of `wave` at `at` of `out`: the place of its samples, and each channel's offset from there. The info
// takes WaveInfoSize bytes. Throws std::invalid_argument as WaveFile::Write does.
void PutWaveInfo(sar::detail::Out& out, std::size_t at, const WaveFile& wave, uint32_t place,
                 std::span<const uint32_t> channel_offsets);

// A wave info that `r` holds at `offset`: the wave's values without its samples, the place of its samples, and each
// channel's offset from there. Throws xsflib::FormatError for a wave of an unknown encoding.
struct ReadInfo
{
    WaveFile wave;
    uint32_t place = 0;
    std::vector<uint32_t> channel_offsets;
};
ReadInfo ReadWaveInfo(const xsflib::Reader& r, std::size_t offset);

// Where the samples of a wave's last channel end, of the bytes from `start` up to `stop` in `bytes`: past the wave's
// last sample when zeros pad the rest to 32 bytes from the start of `bytes`, or else at `stop`.
std::size_t LastSamplesEnd(std::span<const uint8_t> bytes, std::size_t start, std::size_t stop, const WaveFile& wave);

// The waves of a WAVE block whose infos `r` holds at `offsets`, over the raw samples of `wave_data`. Each channel's
// samples run up to the next channel's samples in the wave data, or to its end. A wave's last channel ends past the
// wave's last sample when zeros pad the rest to 32 bytes. Throws xsflib::FormatError for waves whose samples start past
// the wave data, or two channels whose samples start at one place.
std::vector<WaveFile> ReadRawWaves(const xsflib::Reader& r, std::span<const std::size_t> offsets,
                                   std::span<const uint8_t> wave_data);

// The raw samples of `waves` for a WAVE block, and the place of each wave's samples there.
struct RawWaves
{
    std::vector<uint8_t> wave_data;
    std::vector<uint32_t> places;
    std::vector<std::vector<uint32_t>> channel_offsets;
};
RawWaves LayRawWaves(std::span<const WaveFile> waves);

// The waves of the RWAR whose bytes `wave_data` holds, an RWAV file each.
std::vector<WaveFile> ReadArchivedWaves(std::span<const uint8_t> wave_data);

// The bytes of an RWAR that holds each of `waves` as an RWAV file.
std::vector<uint8_t> ArchiveWaves(std::span<const WaveFile> waves);

// Lays `model` out again, and throws other_layout(what) when its Write fails or gives other bytes than `file` and
// `wave_data`. The readers of the files that the tools lay out check each model that they read this way.
template <typename Model, typename Error>
void CheckWrittenBack(const Model& model, std::span<const uint8_t> file, std::span<const uint8_t> wave_data,
                      Error (*other_layout)(const std::string&))
{
    typename Model::Files written;
    try
    {
        written = model.Write();
    }
    catch (const std::invalid_argument& error)
    {
        throw other_layout(error.what());
    }
    if (!std::ranges::equal(written.file, file))
    {
        throw other_layout("Write gives other bytes of the file");
    }
    if (!std::ranges::equal(written.wave_data, wave_data))
    {
        throw other_layout("Write gives other bytes of the wave data");
    }
}

} // namespace ntdlib::rsar::detail
