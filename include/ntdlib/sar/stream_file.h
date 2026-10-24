// SPDX-License-Identifier: MIT

// A stream file of the 3DS (CSTM), the Wii U or the Switch (FSTM). A game keeps its streams in files beside its sound
// archive, and the archive's file table gives each one's path. The game plays a stream from the disc, a block at a
// time. The writer lays a file out as NintendoWare's tools do. The header and its table of blocks come first, padded
// with zeros to 32 bytes. INFO, SEEK and DATA follow. Each block starts with its magic and its size, and zeros pad it
// to 32 bytes.
//
// INFO holds three references: to the stream's settings, to a table of references to the tracks' infos, and to a table
// of references to the channels' infos. The settings follow, then the two tables. A file without tracks gives the
// tracks' table type 0 and offset -1. Each track's info follows the tables, with the table of its channels after it.
// The channels' infos follow the tracks'. A channel's info holds a reference to its DSP-ADPCM info. The DSP-ADPCM infos
// follow, 46 bytes each, one after another.
//
// DATA holds the samples in blocks. Each block holds a piece of each channel, in the channels' order: the block's size
// for each block but the last, and the last block's size rounded up to 32 bytes for the last. The samples start 32
// bytes after DATA's start. A block of DSP-ADPCM holds whole frames of 14 samples, and the last block ends with a whole
// frame too. Zeros follow it. The last block's size in the settings counts the bytes up to the last sample's nibble.
//
// SEEK holds the two samples before each block of each channel, in the order of DATA's pieces: the sample just before
// the block, then the one before it. The tools write the samples that they encoded, not the decoded ones, and they
// write them little-endian in both byte orders. A game that starts a stream in the middle takes the decoder's history
// from there. A stream that plays from its start decodes its blocks as one run of frames.
//
// The tools move a loop's start forward to the start of a block. The samples from the loop's old start up to its new
// start follow the loop's old end, and the stream ends there. The loop then plays the same samples. The settings keep
// the loop's original start and end. The 3DS's tools move a loop from the first sample to the second block.
//
// Version 0x00030000 lays a file out the same way, without the loop's original start and end at the settings' end. The
// tables after the settings start 8 bytes earlier. Many games of the Wii U read stream files only up to that version.
//
// The 3DS's files take versions 0x02000000, 0x02010000, 0x02030000 and 0x02030100. The settings of 0x02000000 end
// after the reference to the samples, without the regions' size and reference. 0x02010000 lays the settings out as
// 0x00030000 does, and 0x02030000 and 0x02030100 as 0x00040000 does. A track's span and flags are zeros there.
//
// The 3DS's tools encode samples past a stream's end. After a loop, the loop's first 128 samples follow the end. The
// last block's size in the settings counts the bytes up to the last of them that the block holds. A last block without
// room for them all ends full, and DATA holds the rest after it: a piece for each channel, in the channels' order, each
// one padded to 32 bytes. SEEK then holds one more entry for each channel, for the samples before those pieces. A file
// of version 0x02000000 holds one sample past the end of a stream without a loop. Its last block's count of samples
// leaves out the last 127.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::sar
{

struct StreamFile
{
    // A track: its volume and pan, its span, its flags and the channels that it plays. The game plays a stream's tracks
    // from its archive's table of sounds. The file can hold them too.
    struct Track
    {
        friend bool operator==(const Track&, const Track&) = default;

        uint8_t volume = 127;
        uint8_t pan = 64;
        uint8_t span = 0;
        uint8_t flags = 0;
        std::vector<uint8_t> channels;
    };

    // A channel: its pieces of DATA one after another, in the stream's encoding and the file's byte order, and a
    // DSP-ADPCM channel's info. The last block's piece holds the bytes up to the last block's size rounded up to 32. A
    // piece of the samples past the end that the last block has no room for follows it. `history` holds SEEK's two
    // samples before each block, and before that piece: the sample just before it, then the one before that.
    struct Channel
    {
        friend bool operator==(const Channel&, const Channel&) = default;

        std::vector<uint8_t> samples;
        std::optional<DspAdpcmInfo> adpcm;
        std::vector<std::array<int16_t, 2>> history;
    };

    friend bool operator==(const StreamFile&, const StreamFile&) = default;

    // Reads `file`. Throws xsflib::FormatError for a file that isn't a stream file, one without INFO, SEEK or DATA, an
    // IMA-ADPCM stream, a stream with regions, settings whose blocks don't add up to the samples, and blocks that run
    // past DATA or SEEK. A file laid out another way reads too, and Write() then gives other bytes.
    static StreamFile Read(std::span<const uint8_t> file);

    // The file's bytes, in the layout of `version`. Throws std::invalid_argument for a version of another layout than
    // those above, an IMA-ADPCM stream, a DSP-ADPCM channel without its info, a PCM channel with one, channels whose
    // samples or history don't fit the settings, more uncounted samples than the last block holds, and a track that
    // plays a channel that the stream lacks.
    std::vector<uint8_t> Write() const;

    // The bytes of each channel's piece of every block but the last.
    uint32_t BlockSize() const;

    // The number of blocks.
    uint32_t BlockCount() const;

    // The samples in the last block of each channel, as the settings count them.
    uint32_t LastBlockSamples() const;

    // The bytes of each channel's piece of the last block, up to the nibble or byte of the last sample that the block
    // holds. The samples past the end that the block holds count too.
    uint32_t LastBlockSize() const;

    // The samples past the end that the last block has no room for.
    uint32_t OverflowSamples() const;

    // The bytes of each channel's piece of the samples past the end that the last block has no room for, padded to 32
    // bytes, or 0.
    uint32_t OverflowSize() const;

    Flavor flavor = Flavor::kNx;
    uint32_t version = 0x00040000;
    WaveEncoding encoding = WaveEncoding::kDspAdpcm;
    bool loop = false;
    uint32_t sample_rate = 48000;
    uint32_t loop_start = 0;
    uint32_t sample_count = 0;

    // The samples in a block of each channel: 14,336 for DSP-ADPCM, 4,096 for 16-bit PCM and 8,192 for 8-bit PCM. Each
    // channel's piece of a block then takes 8 KiB.
    uint32_t block_samples = 14336;

    // The loop's original start and end, before the tools moved the loop to a block. A stream without a loop holds 0
    // and its length. A file of version 0x00030000 holds neither. Read() then gives the loop's start and the stream's
    // length, as the tools write them for a loop that they didn't move.
    uint32_t original_loop_start = 0;
    uint32_t original_loop_end = 0;

    // The samples that the file holds past the stream's last sample, and the samples at the end of the last block that
    // the settings' count of its samples leaves out. Only the 3DS's files hold either (see above).
    uint32_t samples_past_end = 0;
    uint32_t uncounted_samples = 0;

    std::optional<std::vector<Track>> tracks; // nothing when the file holds no table of tracks
    std::vector<Channel> channels;
};

// The samples in a block of each channel of a stream of `encoding`: the samples that fill 8 KiB.
uint32_t StreamBlockSamples(WaveEncoding encoding);

// Decodes every channel of `stream`, sample_count samples each, as the console's decoder plays the stream from its
// start: each channel's blocks as one run. DSP-ADPCM sums in 32 bits that wrap on the Wii U and the Switch, and in 64
// bits on the 3DS. Throws xsflib::FormatError for an encoding that can't be decoded, a DSP-ADPCM channel without its
// info and a channel shorter than its samples.
wave::Pcm DecodeStream(const StreamFile& stream);

// `stream` without its samples past the end, as the tools of the Wii U and the Switch lay a stream out. The bytes
// after the frame or the sample that holds the last sample become zeros, and the last block's count counts every
// sample.
StreamFile WithoutSamplesPastEnd(const StreamFile& stream);

// A stream of `flavor` and version `version` that holds `channels`, each one's samples at `sample_rate`, in `encoding`:
// DSP-ADPCM or 16-bit PCM. A loop from `loop_start` to the end of the samples moves forward to the start of a block, as
// the tools move it: on the 3DS to the start of the block after the one that holds it, and on the other consoles to the
// first block's start at or after it. The samples from its start up to its new start then follow the samples. A stream
// of the 3DS holds the samples past its end that the 3DS's tools encode. DSP-ADPCM takes the encoder of wave/encode.h,
// with the sums of `flavor`'s decoder. SEEK takes the samples before each block. Throws std::invalid_argument for
// another encoding, no channels, channels of different lengths, no samples, a loop start past the end of the samples,
// and more samples than the file's 32-bit sizes hold.
StreamFile MakeStream(Flavor flavor, uint32_t version, WaveEncoding encoding, uint32_t sample_rate,
                      std::span<const std::vector<int16_t>> channels, std::optional<uint32_t> loop_start);

} // namespace ntdlib::sar
