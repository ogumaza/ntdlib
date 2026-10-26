// SPDX-License-Identifier: MIT

// A builder of stream files of the Wii (RSTM) for the tests, laid out as NintendoWare for Revolution's tools lay them
// out (ntdlib/rsar/stream_file.h). The header takes 64 bytes, and HEAD, ADPC and DATA follow it without gaps. HEAD's
// settings start 24 bytes into its payload, after its three references, and the tracks' table follows them. Each
// track's info follows the tracks' references. The channels' table follows the tracks, then each channel's info with
// its DSP-ADPCM info after it. A channel in PCM holds a reference of offset 0. DATA's samples start 32 bytes into it.
// ADPC's entries hold the encoder's input before each point, as the tools write them. The builder encodes DSP-ADPCM
// with the encoder of wave/encode.h and the Wii's sums.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/wave/encode.h"

namespace ntdlib::testing::rsar_streams
{

using Bytes = std::vector<uint8_t>;

// A track of a stream: the channels that it plays, and in an extended info its volume and pan.
struct Track
{
    uint8_t volume = 127;
    uint8_t pan = 64;
    std::vector<uint8_t> channels;
};

// A stream: each channel's samples before the tools encode them, all of the same length. A loop runs from its start to
// the end of the samples. DSP-ADPCM's entries of ADPC lie `history_interval` samples apart.
struct StreamSpec
{
    rsar::WaveEncoding encoding = rsar::WaveEncoding::kDspAdpcm;
    uint32_t sample_rate = 32000;
    std::optional<uint32_t> loop_start;
    std::vector<std::vector<int16_t>> channels;
    uint32_t history_interval = 14336;
    bool extended_tracks = true;
    std::vector<Track> tracks;
};

// What the tools encode for a channel of `spec`: its DSP-ADPCM, or its samples in big-endian 16-bit PCM.
struct EncodedChannel
{
    Bytes samples;
    wave::DspAdpcmChannel adpcm;
};

inline EncodedChannel Encode(const StreamSpec& spec, std::span<const int16_t> samples)
{
    EncodedChannel channel;
    if (spec.encoding == rsar::WaveEncoding::kDspAdpcm)
    {
        channel.adpcm = wave::EncodeDspAdpcm(samples, spec.loop_start.value_or(0), wave::DspAdpcmSum::kWide);
        channel.samples = channel.adpcm.data;
    }
    else if (spec.encoding == rsar::WaveEncoding::kPcm16)
    {
        channel.samples.resize(samples.size() * 2);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            xsflib::StoreBe16(&channel.samples[i * 2], static_cast<uint16_t>(samples[i]));
        }
    }
    else
    {
        throw std::invalid_argument("the builder writes DSP-ADPCM and 16-bit PCM only");
    }

    return channel;
}

inline uint32_t RoundUp(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The bytes of `count` samples of `encoding`, up to the nibble or byte of the last one.
inline uint32_t BytesOf(rsar::WaveEncoding encoding, uint32_t count)
{
    if (encoding != rsar::WaveEncoding::kDspAdpcm)
    {
        return count * 2;
    }

    return count == 0 ? 0 : (count - 1) / 14 * 8 + 2 + (count - 1) % 14 / 2;
}

inline void PutRef(Bytes& file, std::size_t at, uint8_t data_type, uint32_t offset)
{
    file[at] = 1;
    file[at + 1] = data_type;
    xsflib::StoreBe32(&file[at + 4], offset);
}

// Appends `count` zero bytes and returns their offset.
inline std::size_t Grow(Bytes& file, std::size_t count)
{
    const std::size_t at = file.size();
    file.resize(at + count, 0);

    return at;
}

// The stream file of `spec`. Throws std::invalid_argument for a spec without channels or samples, channels of
// different lengths, or an encoding other than DSP-ADPCM and 16-bit PCM.
inline Bytes MakeStream(const StreamSpec& spec)
{
    if (spec.channels.empty() || spec.channels.front().empty())
    {
        throw std::invalid_argument("a stream without channels or samples");
    }
    const auto count = static_cast<uint32_t>(spec.channels.front().size());
    std::vector<EncodedChannel> encoded;
    for (const std::vector<int16_t>& samples : spec.channels)
    {
        if (samples.size() != count)
        {
            throw std::invalid_argument("channels of different lengths");
        }
        encoded.push_back(Encode(spec, samples));
    }

    const bool adpcm = spec.encoding == rsar::WaveEncoding::kDspAdpcm;
    const uint32_t block_samples = adpcm ? 14336 : 4096;
    const uint32_t blocks = (count + block_samples - 1) / block_samples;
    const uint32_t last_samples = count - (blocks - 1) * block_samples;
    const uint32_t last_size = BytesOf(spec.encoding, last_samples);
    const uint32_t last_padded = RoundUp(last_size, 32);
    const auto channels = static_cast<uint32_t>(spec.channels.size());

    Bytes file(0x40, 0);
    std::memcpy(file.data(), "RSTM", 4);
    xsflib::StoreBe16(&file[4], 0xFEFF);
    xsflib::StoreBe16(&file[6], 0x0100);
    xsflib::StoreBe16(&file[0xC], 0x40);
    xsflib::StoreBe16(&file[0xE], 2);

    // HEAD: its references, the settings, the tracks and the channels.
    const std::size_t head = Grow(file, 8);
    std::memcpy(&file[head], "HEAD", 4);
    const std::size_t base = file.size();
    Grow(file, 24);
    const std::size_t settings = Grow(file, 0x34);
    PutRef(file, base, 0, static_cast<uint32_t>(settings - base));
    file[settings] = static_cast<uint8_t>(spec.encoding);
    file[settings + 1] = spec.loop_start.has_value();
    file[settings + 2] = static_cast<uint8_t>(channels);
    file[settings + 3] = static_cast<uint8_t>(spec.sample_rate >> 16);
    xsflib::StoreBe16(&file[settings + 4], static_cast<uint16_t>(spec.sample_rate));
    xsflib::StoreBe32(&file[settings + 8], spec.loop_start.value_or(0));
    xsflib::StoreBe32(&file[settings + 0x0C], count);
    xsflib::StoreBe32(&file[settings + 0x14], blocks);
    xsflib::StoreBe32(&file[settings + 0x18], 0x2000);
    xsflib::StoreBe32(&file[settings + 0x1C], block_samples);
    xsflib::StoreBe32(&file[settings + 0x20], last_size);
    xsflib::StoreBe32(&file[settings + 0x24], last_samples);
    xsflib::StoreBe32(&file[settings + 0x28], last_padded);
    if (adpcm)
    {
        xsflib::StoreBe32(&file[settings + 0x2C], spec.history_interval);
        xsflib::StoreBe32(&file[settings + 0x30], 4);
    }

    const std::size_t tracks = Grow(file, 4 + spec.tracks.size() * 8);
    const uint8_t track_type = spec.extended_tracks ? 1 : 0;
    PutRef(file, base + 8, 0, static_cast<uint32_t>(tracks - base));
    file[tracks] = static_cast<uint8_t>(spec.tracks.size());
    file[tracks + 1] = track_type;
    for (std::size_t t = 0; t < spec.tracks.size(); ++t)
    {
        const Track& track = spec.tracks[t];
        const std::size_t info = Grow(file, (spec.extended_tracks ? 8 : 0) + RoundUp(1 + track.channels.size(), 4));
        PutRef(file, tracks + 4 + t * 8, track_type, static_cast<uint32_t>(info - base));
        std::size_t at = info;
        if (spec.extended_tracks)
        {
            file[info] = track.volume;
            file[info + 1] = track.pan;
            at = info + 8;
        }
        file[at] = static_cast<uint8_t>(track.channels.size());
        std::copy(track.channels.begin(), track.channels.end(), file.begin() + static_cast<std::ptrdiff_t>(at + 1));
    }

    const std::size_t table = Grow(file, 4 + channels * 8);
    PutRef(file, base + 16, 0, static_cast<uint32_t>(table - base));
    file[table] = static_cast<uint8_t>(channels);
    for (uint32_t c = 0; c < channels; ++c)
    {
        const std::size_t info = Grow(file, 8);
        PutRef(file, table + 4 + c * 8, 0, static_cast<uint32_t>(info - base));
        if (!adpcm)
        {
            PutRef(file, info, 0, 0);
            continue;
        }
        const std::size_t at = Grow(file, 0x30);
        PutRef(file, info, 0, static_cast<uint32_t>(at - base));
        const wave::DspAdpcmChannel& channel = encoded[c].adpcm;
        for (std::size_t i = 0; i < channel.coefs.size(); ++i)
        {
            xsflib::StoreBe16(&file[at + i * 2], static_cast<uint16_t>(channel.coefs[i]));
        }
        xsflib::StoreBe16(&file[at + 0x22], channel.pred_scale);
        xsflib::StoreBe16(&file[at + 0x28], channel.loop_pred_scale);
        xsflib::StoreBe16(&file[at + 0x2A], static_cast<uint16_t>(channel.loop_yn1));
        xsflib::StoreBe16(&file[at + 0x2C], static_cast<uint16_t>(channel.loop_yn2));
    }
    file.resize(RoundUp(static_cast<uint32_t>(file.size()), 32), 0);
    xsflib::StoreBe32(&file[head + 4], static_cast<uint32_t>(file.size() - head));
    xsflib::StoreBe32(&file[0x10], static_cast<uint32_t>(head));
    xsflib::StoreBe32(&file[0x14], static_cast<uint32_t>(file.size() - head));

    // ADPC: the samples before each point, as the tools count the points (for each block, its bytes over the
    // interval, rounded up).
    if (adpcm)
    {
        const std::size_t adpc = Grow(file, 8);
        std::memcpy(&file[adpc], "ADPC", 4);
        const uint32_t interval = spec.history_interval;
        const std::size_t entries =
            std::size_t{blocks - 1} * ((0x2000 + interval - 1) / interval) + (last_size + interval - 1) / interval;
        for (std::size_t e = 0; e < entries; ++e)
        {
            const std::size_t point = e * interval;
            for (uint32_t c = 0; c < channels; ++c)
            {
                const std::vector<int16_t>& samples = spec.channels[c];
                const std::size_t at = Grow(file, 4);
                const int16_t yn1 = point >= 1 && point - 1 < count ? samples[point - 1] : 0;
                const int16_t yn2 = point >= 2 && point - 2 < count ? samples[point - 2] : 0;
                xsflib::StoreBe16(&file[at], static_cast<uint16_t>(yn1));
                xsflib::StoreBe16(&file[at + 2], static_cast<uint16_t>(yn2));
            }
        }
        file.resize(RoundUp(static_cast<uint32_t>(file.size()), 32), 0);
        xsflib::StoreBe32(&file[adpc + 4], static_cast<uint32_t>(file.size() - adpc));
        xsflib::StoreBe32(&file[0x18], static_cast<uint32_t>(adpc));
        xsflib::StoreBe32(&file[0x1C], static_cast<uint32_t>(file.size() - adpc));
    }

    // DATA: each block's piece of each channel, the last ones padded to 32 bytes.
    const std::size_t data = Grow(file, 32);
    std::memcpy(&file[data], "DATA", 4);
    xsflib::StoreBe32(&file[data + 8], 0x18);
    xsflib::StoreBe32(&file[settings + 0x10], static_cast<uint32_t>(data + 32));
    for (uint32_t b = 0; b < blocks; ++b)
    {
        const bool final = b + 1 == blocks;
        for (uint32_t c = 0; c < channels; ++c)
        {
            const Bytes& samples = encoded[c].samples;
            const std::size_t from = std::size_t{b} * 0x2000;
            const std::size_t size = final ? last_size : 0x2000;
            const std::size_t at = Grow(file, final ? last_padded : 0x2000);
            const std::size_t copied = std::min(size, samples.size() - std::min(from, samples.size()));
            std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(from), copied,
                        file.begin() + static_cast<std::ptrdiff_t>(at));
        }
    }
    xsflib::StoreBe32(&file[data + 4], static_cast<uint32_t>(file.size() - data));
    xsflib::StoreBe32(&file[0x20], static_cast<uint32_t>(data));
    xsflib::StoreBe32(&file[0x24], static_cast<uint32_t>(file.size() - data));
    xsflib::StoreBe32(&file[8], static_cast<uint32_t>(file.size()));

    return file;
}

} // namespace ntdlib::testing::rsar_streams
