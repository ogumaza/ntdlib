// SPDX-License-Identifier: MIT

// A wave file of the 3DS (CWAV), the Wii U or the Switch (FWAV), and a wave archive of the same consoles (CWAR or
// FWAR), in parts that a writer can put back together. The writers lay the files out as NintendoWare's tools do. The
// header and its table of blocks come first, padded with zeros to 32 bytes. Each block starts with its magic and its
// size.
//
// A wave file's INFO block holds the wave's settings and a table of references to the channels' infos. The infos
// follow, then the DSP-ADPCM infos of a DSP-ADPCM wave's channels, 46 bytes each, one after another. A PCM channel's
// reference to a DSP-ADPCM info has type 0 and offset -1. Zeros pad the block to 32 bytes. DATA follows. Each
// channel's samples start on a boundary of the file: 64 bytes on the Switch, and 32 bytes on the 3DS and the Wii U.
// Zeros fill the gaps. The last channel's samples end the file.
//
// A wave archive's INFO block holds each wave's offset in FILE and its size. Zeros pad the block to 32 bytes. FILE
// follows. Its waves start 32 bytes after the block's start or later: each wave starts on a boundary of the archive, of
// the same size as in a wave file. The last wave ends the file. An archive without waves ends 32 bytes after FILE's
// start.
//
// A bank file and a wave sound file name their waves through a table of waves. WithWaveArchives puts other wave
// archives in the table, for a file that goes into another archive.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"

namespace ntdlib::sar
{

struct WaveFile
{
    // A channel's samples in the wave's encoding and the file's byte order, and a DSP-ADPCM channel's info.
    struct Channel
    {
        std::vector<uint8_t> samples;
        std::optional<DspAdpcmInfo> adpcm;
    };

    // Reads `file`. A channel's samples run up to the next channel's samples, or to the end of DATA for the last
    // channel. Throws xsflib::FormatError for a file that isn't a wave file, one without INFO or DATA, an IMA-ADPCM
    // wave, and channels whose samples don't follow one another in the channels' order. A file laid out another way
    // reads too, and Write() then gives other bytes.
    static WaveFile Read(std::span<const uint8_t> file);

    // The file's bytes. Throws std::invalid_argument for an IMA-ADPCM wave, a DSP-ADPCM channel without its info, and a
    // PCM channel with one.
    std::vector<uint8_t> Write() const;

    Flavor flavor = Flavor::kNx;
    uint32_t version = 0x00010200;
    WaveEncoding encoding = WaveEncoding::kPcm16;
    bool loop = false;
    uint32_t sample_rate = 48000;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0; // the samples' count when the wave doesn't loop

    // The loop's original start: the field after the loop's end. It can differ from loop_start, such as when the loop
    // moved to the start of a DSP-ADPCM frame. ntdlib's models play from loop_start.
    uint32_t original_loop_start = 0;

    std::vector<Channel> channels;
};

struct WaveArchiveFile
{
    // Reads `file`. Throws xsflib::FormatError for a file that isn't a wave archive, and one without INFO or FILE. A
    // file laid out another way reads too, and Write() then gives other bytes.
    static WaveArchiveFile Read(std::span<const uint8_t> file);

    // The file's bytes.
    std::vector<uint8_t> Write() const;

    Flavor flavor = Flavor::kNx;
    uint32_t version = 0x00010000;
    std::vector<std::vector<uint8_t>> waves; // each a wave file's bytes, in the archive's order
};

// `file`, a bank file or a wave sound file of `flavor` (CBNK, FBNK, CWSD or FWSD), with other wave archives in its
// table of waves. The file's INFO block refers to the table: a count, then a wave archive's item ID and a wave's index
// for each wave. Each ID that `wave_archives` maps takes the ID that it maps to. The file's other bytes stay. A bank's
// regions and a wave sound's notes name their waves by their places in the table, and those stay too. Throws
// xsflib::FormatError for bytes that aren't such a file, a file of another console, and a file without a table of
// waves.
std::vector<uint8_t> WithWaveArchives(std::span<const uint8_t> file, Flavor flavor,
                                      const std::map<uint32_t, uint32_t>& wave_archives);

} // namespace ntdlib::sar
