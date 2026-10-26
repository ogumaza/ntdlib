// SPDX-License-Identifier: MIT

// The wave infos that an RWAV file, a bank's WAVE block and a wave sound file's WAVE block hold, and the raw samples
// that a WAVE block describes (ntdlib/rsar/wave_file.h).

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/rsar/wave_file.h"
#include "rsar/file_blocks.h"
#include "sar/file_bytes.h"

namespace ntdlib::rsar::detail
{

inline constexpr std::size_t kWaveInfoSize = 0x1C;
inline constexpr std::size_t kChannelInfoSize = 0x1C;
inline constexpr std::size_t kAdpcmInfoSize = 0x30;

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

} // namespace ntdlib::rsar::detail
