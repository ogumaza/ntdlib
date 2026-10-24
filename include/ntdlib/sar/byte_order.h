// SPDX-License-Identifier: MIT

// The files of the 3DS's sound archives, the Wii U's and the Switch's hold the same structures. The Wii U's files are
// big-endian, and the 3DS's and the Switch's are little-endian. The 3DS's magics start with C, and the others' with F.
// Each console numbers its versions in its own way. A bank file (CBNK or FBNK), a wave sound file (CWSD or FWSD), a
// wave file (CWAV or FWAV) or a wave archive (CWAR or FWAR) of one console becomes a file of another with these
// functions. So does a stream (CSTM or FSTM).
//
// A bank file and a wave sound file hold only their INFO block. WithConsole walks the block's structures and writes
// each field in the byte order of the console that the file goes to. Each of the walk's structures holds fields of 1, 2
// or 4 bytes: references (a type, 2 bytes of padding and an offset), counts, item IDs, offsets, floats and optional
// parameters. An optional parameter is a field of bits, then a 4-byte value for each bit that is set. A region's
// parameters can give its original key, volume, pan, pitch, a field of flags and an envelope (bits 0 to 4 and 9). A
// wave sound's settings can give its pan, its pitch, its filter, its sends and its envelope (bits 0, 1, 2, 8 and 9).
// The value of an envelope gives where a reference to the envelope's 5 bytes lies. The value of the sends gives where
// they lie: the main send, the number of effect sends, then each effect send, a byte each.
//
// A wave's settings and DSP-ADPCM infos hold fields of 1, 2 or 4 bytes too. 16-bit PCM samples take the byte order of
// the console that the wave goes to. DSP-ADPCM's frames and 8-bit PCM samples stay as they are. The Switch starts each
// channel's samples on 64 bytes, and the 3DS and the Wii U on 32. ConvertWave writes the wave again through WaveFile,
// and the samples move to the console's boundaries. A stream holds the same kinds of fields and samples. Its SEEK block
// is little-endian on every console, and stays.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"

namespace ntdlib::sar
{

// `file`, a bank file (CBNK or FBNK) or a wave sound file (CWSD or FWSD), as a file of `flavor` with version
// `version`: with the letter of `flavor`'s magics, in its byte order. The structures keep their places, and so does
// every other byte. Throws xsflib::FormatError for a file of another kind, a big-endian file of the 3DS, a file without
// its INFO block, a reference or an option that the walk doesn't know, structures that overlap, and a byte that isn't
// zero where no structure lies.
std::vector<uint8_t> WithConsole(std::span<const uint8_t> file, Flavor flavor, uint32_t version);

// `file`, a bank file (FBNK) or a wave sound file (FWSD) of the Wii U or the Switch, as a file of the other of the two
// with version `version` (WithConsole). Throws xsflib::FormatError for a 3DS file too.
std::vector<uint8_t> WithOtherByteOrder(std::span<const uint8_t> file, uint32_t version);

// `wave`, a wave of any of the three consoles, as a wave of `flavor` with version `version`. The settings and the
// DSP-ADPCM infos stay. Each channel keeps the samples that the wave's length requires, and drops the bytes after them:
// 2 bytes for each sample of 16-bit PCM, 1 for each sample of 8-bit PCM, and 8 for each frame of 14 samples of
// DSP-ADPCM. 16-bit PCM takes `flavor`'s byte order. The version decides the loop's original start: a wave of the 3DS
// holds it from version 0x02010100 on, and one of the Wii U or the Switch from 0x00010200 on. A file of an earlier
// version holds 0 there. A wave converted from such a version to one that holds the start takes the loop's start there,
// or 0 without a loop.
WaveFile ConvertWave(const WaveFile& wave, Flavor flavor, uint32_t version);

// `stream`, a stream of any of the three consoles, as a stream of `flavor` with version `version`. The settings, the
// tracks, the DSP-ADPCM infos and the history stay. 16-bit PCM takes `flavor`'s byte order. A stream that goes to
// another console drops its samples past the end (WithoutSamplesPastEnd). A stream for the 3DS therefore holds none,
// though the 3DS's tools encode a loop's first 128 samples there. Its tracks take a span and flags of 0.
StreamFile ConvertStream(const StreamFile& stream, Flavor flavor, uint32_t version);

// `archive`, a wave archive of any of the three consoles, as a wave archive of `flavor` with version `version`. Each
// wave becomes a wave of `flavor` with version `wave_version`, as ConvertWave makes it. Throws xsflib::FormatError for
// a wave that WaveFile::Read refuses.
WaveArchiveFile ConvertWaveArchive(const WaveArchiveFile& archive, Flavor flavor, uint32_t version,
                                   uint32_t wave_version);

} // namespace ntdlib::sar
