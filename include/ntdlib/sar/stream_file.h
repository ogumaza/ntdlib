// SPDX-License-Identifier: MIT

// A stream file of the 3DS (CSTM), the Wii U or the Switch (FSTM). A game keeps its streams in files beside its sound
// archive, and the archive's file table gives each one's path. The game plays a stream from the disc, a block at a
// time. The writer lays a file out as NintendoWare's tools do. The header and its table of blocks come first, padded
// with zeros to 32 bytes. INFO, SEEK and DATA follow. Each block starts with its magic and its size, and zeros pad it
// to 32 bytes.
//
// INFO holds three references: to the stream's settings, to a table of references to the tracks' infos, and to a table
// of references to the channels' infos. The settings follow, then the tables. A file without tracks gives the tracks'
// table type 0 and offset -1. A channel's info holds a reference to its DSP-ADPCM info. The DSP-ADPCM infos follow, 46
// bytes each, one after another.
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
// the loop's original start and end.
//
// Version 0x00030000 lays a file out the same way, without the loop's original start and end at the settings' end. The
// tables after the settings start 8 bytes earlier. Many games of the Wii U read stream files only up to that version.

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
    // DSP-ADPCM channel's info. The last piece holds the bytes up to the last block's size rounded up to 32. `history`
    // holds SEEK's two samples before each block: the sample just before it, then the one before that.
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

    // The file's bytes, in the layout of `version`. Throws std::invalid_argument for a version other than 0x00030000
    // and 0x00040000, an IMA-ADPCM stream, a DSP-ADPCM channel without its info, a PCM channel with one, channels whose
    // samples or history don't fit the settings, and a track that plays a channel that the stream lacks.
    std::vector<uint8_t> Write() const;

    // The bytes of each channel's piece of every block but the last.
    uint32_t BlockSize() const;

    // The number of blocks.
    uint32_t BlockCount() const;

    // The samples in the last block of each channel.
    uint32_t LastBlockSamples() const;

    // The bytes of each channel's piece of the last block, up to the last sample's nibble or byte.
    uint32_t LastBlockSize() const;

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

// A stream of `flavor` and version `version` that holds `channels`, each one's samples at `sample_rate`, in
// `encoding`: DSP-ADPCM or 16-bit PCM. A loop from `loop_start` to the end of the samples moves forward to the start of
// a block, as the tools move it. The samples from its start up to its new start then follow the samples. DSP-ADPCM
// takes the encoder of wave/encode.h, with the sums of `flavor`'s decoder. SEEK takes the samples before each block.
// Throws std::invalid_argument for another encoding, no channels, channels of different lengths, no samples, a loop
// start past the end of the samples, and more samples than the file's 32-bit sizes hold.
StreamFile MakeStream(Flavor flavor, uint32_t version, WaveEncoding encoding, uint32_t sample_rate,
                      std::span<const std::vector<int16_t>> channels, std::optional<uint32_t> loop_start);

} // namespace ntdlib::sar
