// SPDX-License-Identifier: MIT

// The files of the Wii U's sound archives and the Switch's hold the same structures. The Wii U's files are big-endian,
// and the Switch's are little-endian. A bank file (FBNK), a wave sound file (FWSD), a wave file (FWAV) or a wave
// archive (FWAR) of one console becomes a file of the other with these functions.
//
// A bank file and a wave sound file hold only their INFO block. WithOtherByteOrder walks the block's structures and
// writes each field in the other byte order. Each of the walk's structures holds fields of 1, 2 or 4 bytes: references
// (a type, 2 bytes of padding and an offset), counts, item IDs, offsets, floats and optional parameters. An optional
// parameter is a field of bits, then a 4-byte value for each bit that is set. A region's parameters can give its
// original key, volume, pan, pitch, a field of flags and an envelope (bits 0 to 4 and 9). A wave sound's settings can
// give its pan, its pitch, its filter, its sends and its envelope (bits 0, 1, 2, 8 and 9). The value of an envelope
// gives where a reference to the envelope's 5 bytes lies. The value of the sends gives where they lie: the main send,
// the number of effect sends, then each effect send, a byte each.
//
// A wave's settings and DSP-ADPCM infos hold fields of 1, 2 or 4 bytes too. 16-bit PCM samples take the other byte
// order. DSP-ADPCM's frames and 8-bit PCM samples stay as they are. The Switch starts each channel's samples on 64
// bytes, and the Wii U on 32. ConvertWave writes the wave again through WaveFile, and the samples move to the
// console's boundaries. A stream (FSTM) holds the same kinds of fields and samples. Its SEEK block is little-endian on
// both consoles, and stays.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"

namespace ntdlib::sar
{

// `file`, a bank file (FBNK) or a wave sound file (FWSD) of the Wii U or the Switch, in the other console's byte order
// and with version `version`. The structures keep their places, and so does every other byte. Throws
// xsflib::FormatError for a file of another kind, a 3DS file, a file without its INFO block, a reference or an option
// that the walk doesn't know, structures that overlap, and a byte that isn't zero where no structure lies.
std::vector<uint8_t> WithOtherByteOrder(std::span<const uint8_t> file, uint32_t version);

// `wave`, a wave of the Wii U or the Switch, as a wave of `flavor` with version `version`. The settings and the
// DSP-ADPCM infos stay. Each channel keeps the samples that the wave's length requires, and drops the bytes after them:
// 2 bytes for each sample of 16-bit PCM, 1 for each sample of 8-bit PCM, and 8 for each frame of 14 samples of
// DSP-ADPCM. 16-bit PCM takes `flavor`'s byte order. The version decides the loop's original start. A file of a version
// before 0x00010200 holds 0 there. A wave converted from such a version to a later one takes the loop's start there, or
// 0 without a loop. Throws std::invalid_argument for a 3DS wave and for `flavor` of the 3DS.
WaveFile ConvertWave(const WaveFile& wave, Flavor flavor, uint32_t version);

// `stream`, a stream of the Wii U or the Switch, as a stream of `flavor` with version `version`. The settings, the
// tracks, the DSP-ADPCM infos and the history stay. 16-bit PCM takes `flavor`'s byte order. Throws
// std::invalid_argument for a 3DS stream and for `flavor` of the 3DS.
StreamFile ConvertStream(const StreamFile& stream, Flavor flavor, uint32_t version);

// `archive`, a wave archive of the Wii U or the Switch, as a wave archive of `flavor` with version `version`. Each wave
// becomes a wave of `flavor` with version `wave_version`, as ConvertWave makes it. Throws std::invalid_argument for a
// 3DS archive and for `flavor` of the 3DS, and xsflib::FormatError for a wave that WaveFile::Read refuses.
WaveArchiveFile ConvertWaveArchive(const WaveArchiveFile& archive, Flavor flavor, uint32_t version,
                                   uint32_t wave_version);

} // namespace ntdlib::sar
