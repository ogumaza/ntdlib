// SPDX-License-Identifier: MIT

// A stream file of the Wii (RSTM). A game keeps its streams in files beside its sound archive. The archive's file table
// gives each one's path, relative to the archive's folder. NW4R SND plays a stream from the disc, a block at a time
// (StrmPlayer).
//
// The header holds the offsets and sizes of three blocks: HEAD, ADPC and DATA. A stream in PCM has no ADPC, and the
// header gives it offset 0 and size 0. HEAD holds three references: to the stream's settings, to a table of references
// to the tracks' infos, and to a table of references to the channels' infos. The settings follow the references. The
// tracks' table follows the settings, then each track's info, then the channels' table. A track's info holds the
// channels that the track plays. An extended track's info holds the track's volume and pan before them. The table's
// type says which kind its infos are. Each channel's info is a reference to its DSP-ADPCM info, and the DSP-ADPCM info
// follows it. In PCM, the reference has offset 0 and nothing follows it. Zeros pad HEAD to 32 bytes.
//
// DATA holds the samples in blocks. Each block holds a piece of each channel, in the channels' order: 8 KiB of each
// channel, and in the last block each channel's piece padded to 32 bytes. The settings give the samples' offset in the
// file: 32 bytes after DATA's start. The last block's size in the settings counts the bytes up to the last sample's
// nibble. 16-bit samples are big-endian.
//
// ADPC holds samples of the stream before the tools encoded it. The settings give an interval in samples, and ADPC
// holds a point every interval from the stream's start. Each entry holds the sample just before its point, then the one
// before that, for each channel in turn, big-endian. The tools count the entries as if the interval were in bytes of a
// channel's samples: for each block, its bytes divided by the interval and rounded up. Most streams give a block's
// samples as the interval. Each block then has one entry, for its start. Some give 1,024. The entries then stop part
// of the way through a stream, and only every 14th lies at a block's start. Zeros pad ADPC to 32 bytes. NW4R SND reads
// entry `block` for a stream that starts at block `block`, whatever the interval (StrmFileLoader::ReadAdpcBlockData,
// main.dol 0x80048614). A stream that plays from its start takes the decoder's history from the DSP-ADPCM info, and
// decodes its blocks as one run of frames. The DSP-ADPCM info's state at the loop's start holds the decoded samples
// there.
//
// The header takes 64 bytes. Its count of blocks holds 2, even beside ADPC. HEAD, ADPC and DATA follow it without
// gaps. Each channel's info is followed by its DSP-ADPCM info, and DATA's size field is followed by 0x18: the samples'
// offset from the field. The tools start every loop at a block. They move a loop's start forward to the first block's
// start at or after it, and the samples from the old start up to the new one follow the loop's old end. The stream
// then ends there. The tools give every stream a track for each pair of its channels.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/decode.h"

namespace ntdlib::rsar
{

struct StreamFile
{
    // A track: the channels that it plays, and in an extended info its volume and pan.
    struct Track
    {
        friend bool operator==(const Track&, const Track&) = default;

        uint8_t volume = 127;
        uint8_t pan = 64;
        std::vector<uint8_t> channels;
    };

    // A channel: its pieces of DATA one after another, in the stream's encoding, a DSP-ADPCM channel's info, and the
    // channel's entries of ADPC: for each point, the sample just before it, then the one before that. The last block's
    // piece holds the bytes up to the last block's size rounded up to 32.
    struct Channel
    {
        friend bool operator==(const Channel&, const Channel&) = default;

        std::vector<uint8_t> samples;
        std::optional<DspAdpcmInfo> adpcm;
        std::vector<std::array<int16_t, 2>> history;
    };

    friend bool operator==(const StreamFile&, const StreamFile&) = default;

    // Reads `file`. Throws xsflib::FormatError for a file that isn't a stream file of version 1.0, one without HEAD or
    // DATA, a DSP-ADPCM stream without ADPC, an encoding other than PCM and DSP-ADPCM, settings whose blocks don't add
    // up to the samples, blocks whose pieces hold headers, blocks that run past DATA, entries that run past ADPC, and a
    // track that plays a channel that the stream lacks.
    static StreamFile Read(std::span<const uint8_t> file);

    // The file's bytes, laid out as the tools lay a stream file out. A track's volume and pan go into extended infos
    // only. Throws std::invalid_argument for an encoding other than PCM and DSP-ADPCM, no channels or more than 255, no
    // samples, blocks of another number of samples than StreamBlockSamples gives, a rate of 0 or past 24 bits, a loop
    // that starts at the end or after it, a DSP-ADPCM channel without its info or a PCM channel with one, channels
    // whose samples or entries of ADPC don't fit the settings, a DSP-ADPCM stream without an interval or a PCM stream
    // with one, more than 255 tracks or channels in a track, a track that plays a channel that the stream lacks, and a
    // file past 4 GB.
    std::vector<uint8_t> Write() const;

    // The bytes of each channel's piece of every block but the last: 8 KiB in every stream that the tools write.
    uint32_t BlockSize() const;

    // The number of blocks.
    uint32_t BlockCount() const;

    // The samples in the last block of each channel.
    uint32_t LastBlockSamples() const;

    // The bytes of each channel's piece of the last block, up to the nibble or byte of the last sample.
    uint32_t LastBlockSize() const;

    WaveEncoding encoding = WaveEncoding::kDspAdpcm;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0;
    uint32_t sample_count = 0; // the loop's end: one past the last sample

    // The samples in a block of each channel: 14,336 for DSP-ADPCM, 4,096 for 16-bit PCM and 8,192 for 8-bit PCM.
    uint32_t block_samples = 14336;

    // The samples between ADPC's points. A stream in PCM holds 0.
    uint32_t history_interval = 0;

    bool extended_tracks = false; // whether the tracks' infos hold a volume and a pan
    std::vector<Track> tracks;
    std::vector<Channel> channels;
};

// The samples in a block of each channel of a stream in `encoding`, whose pieces of blocks take 8 KiB. Throws
// std::invalid_argument for another encoding.
uint32_t StreamBlockSamples(WaveEncoding encoding);

// Decodes every channel of `stream`, sample_count samples each, as the stream plays from its start: each channel's
// blocks as one run. DSP-ADPCM sums in 64 bits, as for the Wii's waves (DecodeWave). Throws xsflib::FormatError for a
// channel shorter than its samples, and a DSP-ADPCM channel without its info.
wave::Pcm DecodeStream(const StreamFile& stream);

// A stream that holds `channels`, each one's samples at `sample_rate`, in `encoding`: DSP-ADPCM or 16-bit PCM. A loop
// from `loop_start` to the end of the samples moves forward to the first block's start at or after it, as the tools
// move it. The samples from its start up to its new start then follow the samples. DSP-ADPCM takes the encoder of
// wave/encode.h with the Wii's sums, and ADPC an entry before each block. The stream takes a track for each pair of its
// channels, the last one alone, in extended infos at a volume of 127 and the centre. Throws std::invalid_argument for
// another encoding, no channels or more than 255, channels of different lengths, no samples, a loop start past the end
// of the samples, a rate of 0 or past 24 bits, and more samples than the file's 32-bit sizes hold.
StreamFile MakeStream(WaveEncoding encoding, uint32_t sample_rate, std::span<const std::vector<int16_t>> channels,
                      std::optional<uint32_t> loop_start);

} // namespace ntdlib::rsar
