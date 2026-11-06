// SPDX-License-Identifier: MIT

// Writers of the files of the DS's sound archive (SDAT): sequences (SSEQ), sequence archives (SSAR), banks (SBNK), wave
// archives (SWAR) and waves (SWAV). Each writer lays its file out as the SDK's tools do. formats.h reads them, and
// layout.h lays out the archive that holds them. EncodeWave encodes samples into a wave.
//
// The tools write each file with version 1.0, a header of 16 bytes and one DATA block that runs to the file's end. The
// file ends on a multiple of 4 bytes, with zeros after its data. A wave's bytes are its 12 bytes of parameters
// (SNDWaveParam), then its samples.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sdat/formats.h"

namespace ntdlib::sdat
{

// A sequence file of `bytecode`. The bytecode starts at 0x1C.
std::vector<uint8_t> SequenceFile(std::span<const uint8_t> bytecode);

// A sequence archive of `sequences`, with `bytecode` after their table. Each sequence's offset counts from the
// bytecode's start.
std::vector<uint8_t> SequenceArchiveFile(std::span<const ArchivedSequence> sequences,
                                         std::span<const uint8_t> bytecode);

// A bank file of `bank`'s instruments. The table gives each instrument a record: its type, and its data's offset in the
// file. A record of kNone is 0. The instruments' data follows the table, in the order of their offsets. An instrument
// with an offset of 0 is a new one. Its data goes after the others, in the table's order. Instruments with the same
// offset and the same data share it. Bytes of the bank that no record points to aren't written. Throws
// std::invalid_argument for a drum set whose regions don't cover its keys, for a key split whose regions don't match
// its keys, and for data past the 24 bits of an offset.
std::vector<uint8_t> BankFile(const Bank& bank);

// A wave's bytes: its parameters, then its samples.
std::vector<uint8_t> WaveBytes(const Wave& wave);

// A wave file of a wave's bytes. WaveBytes and EncodeWave give such bytes.
std::vector<uint8_t> WaveFile(std::span<const uint8_t> wave);

// A wave archive of waves' bytes. WaveBytes and EncodeWave give such bytes. The waves follow the table of their
// offsets, each on a multiple of 4 bytes.
std::vector<uint8_t> WaveArchiveFile(std::span<const std::vector<uint8_t>> waves);

// Encodes `samples` into a wave's bytes, at `sample_rate`, in `encoding`. A wave loops from sample `loop_start` to its
// end when `loop_start` is given.
//
// The channel reads a wave in 32-bit words: 4 samples of PCM8, 2 of PCM16 or 8 of IMA-ADPCM. An IMA-ADPCM wave starts
// with a word of the decoder's state. The loop's start and length count words. A loop start inside a word moves forward
// to the next word's start. The samples from the old start then follow the loop's end. A loop whose length isn't a
// whole number of words repeats until it is. A wave without a loop ends with zeros up to a word's end. The timer is
// 16,756,991 / `sample_rate`, rounded down. The SDK's tools give most waves that timer. PCM8 takes each sample's high
// byte, rounded to the nearest. IMA-ADPCM decodes each nibble with the DS's step (ntr::AdpcmStep). The channel keeps
// the decoder's state at the loop's start, and each pass of the loop plays as the first.
//
// Throws std::invalid_argument for a rate below 256 Hz, an unknown encoding, a loop start at or past the end, and a
// loop start past the 65,535 words of its field. The timer of a rate below 256 Hz doesn't fit its 16 bits.
std::vector<uint8_t> EncodeWave(std::span<const int16_t> samples, uint16_t sample_rate, WaveEncoding encoding,
                                std::optional<uint32_t> loop_start);

} // namespace ntdlib::sdat
