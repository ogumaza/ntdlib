// SPDX-License-Identifier: MIT

#include "ntdlib/rsar/convert.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rsar/wsd_file.h"
#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/byte_order.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::rsar
{
namespace
{

// The channels of a wave that NW4R SND plays.
constexpr std::size_t kPlayedChannels = 2;

// The most channels and tracks that a stream file of the Wii counts, the highest rate that its 24 bits hold, and the
// version of the Wii U's streams through which a later console's stream goes to the Wii.
constexpr std::size_t kMostStreamCount = 255;
constexpr uint32_t kMostRate = 0xFFFFFF;
constexpr uint32_t kLaterStreamVersion = 0x00040000;

// The bytes that `samples` samples take in `encoding`: a byte each in 8-bit PCM, two in 16-bit PCM, and a frame of 8
// bytes for each 14 samples or part of 14 in DSP-ADPCM.
std::size_t BytesOf(WaveEncoding encoding, uint32_t samples)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return samples;
    case WaveEncoding::kPcm16:
        return std::size_t{samples} * 2;
    case WaveEncoding::kDspAdpcm:
        return (std::size_t{samples} + 13) / 14 * 8;
    }

    return 0;
}

WaveEncoding WiiEncodingOf(sar::WaveEncoding encoding)
{
    switch (encoding)
    {
    case sar::WaveEncoding::kPcm8:
        return WaveEncoding::kPcm8;
    case sar::WaveEncoding::kPcm16:
        return WaveEncoding::kPcm16;
    case sar::WaveEncoding::kDspAdpcm:
        return WaveEncoding::kDspAdpcm;
    default:
        throw std::invalid_argument("a wave in an encoding that the Wii doesn't play, such as IMA-ADPCM");
    }
}

sar::WaveEncoding EncodingOf(WaveEncoding encoding)
{
    switch (encoding)
    {
    case WaveEncoding::kPcm8:
        return sar::WaveEncoding::kPcm8;
    case WaveEncoding::kPcm16:
        return sar::WaveEncoding::kPcm16;
    case WaveEncoding::kDspAdpcm:
        return sar::WaveEncoding::kDspAdpcm;
    }

    return sar::WaveEncoding::kPcm16;
}

// Whether the files of `flavor` hold their 16-bit samples little-endian: the 3DS's and the Switch's do.
bool LittleEndian(sar::Flavor flavor)
{
    return flavor != sar::Flavor::kCafe;
}

// The region that NW4R SND plays for a note of `key` at `velocity` on `program` of `bank`, or null where it plays
// nothing. A region whose wave lies past the bank's waves plays nothing either (Bank::NoteOn).
const Region* Played(const Bank& bank, uint32_t program, int key, int velocity)
{
    const Region* region = bank.Find(program, key, velocity);
    if (region == nullptr || static_cast<std::size_t>(region->wave_index) >= bank.Waves().size())
    {
        return nullptr;
    }

    return region;
}

// `region`'s values in a velocity region, as `build` reads them.
sar::VelocityRegion ValuesOf(const Region& region, const SndBuild& build)
{
    const bool march_2008 = build.SinceMarch2008();
    sar::VelocityRegion values;
    values.wave_id_index = static_cast<uint32_t>(region.wave_index);
    values.original_key = region.original_key;
    values.volume = region.volume;
    values.pan = region.pan;
    values.pitch = region.tune;
    values.ignore_note_off = march_2008 && region.note_off_type == 1;
    values.key_group = march_2008 ? region.key_group : 0;
    values.interpolation = 0;
    values.adshr = {region.attack, region.decay, region.sustain, march_2008 ? region.hold : uint8_t{0}, region.release};

    return values;
}

// The instrument of `program` of `bank` as key regions and velocity regions, or nothing for a program that plays no
// region at any key and velocity.
std::optional<sar::BankFile::Instrument> InstrumentOf(const Bank& bank, uint32_t program, const SndBuild& build)
{
    // The region that each key plays at each velocity.
    using Column = std::array<const Region*, 128>;
    std::array<Column, 128> grid{};
    for (int key = 0; key < 128; ++key)
    {
        for (int velocity = 0; velocity < 128; ++velocity)
        {
            grid[key][velocity] = Played(bank, program, key, velocity);
        }
    }

    sar::BankFile::Instrument instrument;
    for (int first = 0; first < 128;)
    {
        int last = first;
        while (last + 1 < 128 && grid[last + 1] == grid[first])
        {
            ++last;
        }

        const Column& column = grid[first];
        sar::BankFile::KeyRegion key_region{
            .first_key = static_cast<uint8_t>(first), .last_key = static_cast<uint8_t>(last), .regions = {}};
        for (int low = 0; low < 128;)
        {
            int high = low;
            while (high + 1 < 128 && column[high + 1] == column[low])
            {
                ++high;
            }
            if (column[low] != nullptr)
            {
                key_region.regions.push_back({.first_velocity = static_cast<uint8_t>(low),
                                              .last_velocity = static_cast<uint8_t>(high),
                                              .values = ValuesOf(*column[low], build)});
            }
            low = high + 1;
        }
        if (!key_region.regions.empty())
        {
            instrument.key_regions.push_back(std::move(key_region));
        }
        first = last + 1;
    }
    if (instrument.key_regions.empty())
    {
        return std::nullopt;
    }

    return instrument;
}

// A velocity region's values in a region of the Wii, without its wave.
Region RegionOf(const sar::VelocityRegion& values)
{
    Region region;
    region.attack = values.adshr.attack;
    region.decay = values.adshr.decay;
    region.sustain = values.adshr.sustain;
    region.release = values.adshr.release;
    region.hold = values.adshr.hold;
    region.note_off_type = values.ignore_note_off ? 1 : 0;
    region.key_group = values.key_group;
    region.original_key = values.original_key;
    region.volume = values.volume;
    region.pan = values.pan;
    region.tune = values.pitch;

    return region;
}

// The waves that a file of the Wii holds of `waves`, in their order, and the index of each entry's wave among them, or
// -1 for an entry without a wave.
struct HeldWaves
{
    std::vector<int32_t> indexes;
    std::vector<WaveFile> waves;
};

HeldWaves HeldWavesOf(const std::vector<std::optional<WaveFile>>& waves)
{
    HeldWaves held;
    for (const std::optional<WaveFile>& wave : waves)
    {
        held.indexes.push_back(wave ? static_cast<int32_t>(held.waves.size()) : -1);
        if (wave)
        {
            held.waves.push_back(*wave);
        }
    }

    return held;
}

} // namespace

sar::WaveFile WaveFileOf(const Wave& wave, sar::Flavor flavor, uint32_t version)
{
    if (wave.channels.empty())
    {
        throw std::invalid_argument("a wave of the Wii without channels");
    }

    sar::WaveFile file;
    file.flavor = flavor;
    file.version = version;
    file.encoding = EncodingOf(wave.encoding);
    file.loop = wave.loop;
    file.sample_rate = wave.sample_rate;
    file.loop_start = wave.loop_start;
    file.loop_end = wave.loop_end;
    file.original_loop_start = wave.loop_start;

    const std::size_t bytes = BytesOf(wave.encoding, wave.loop_end);
    const std::size_t channels = std::min(wave.channels.size(), kPlayedChannels);
    for (std::size_t c = 0; c < channels; ++c)
    {
        const WaveChannel& channel = wave.channels[c];
        if (channel.data.size() < bytes)
        {
            throw std::invalid_argument("the samples of a channel of a wave of the Wii end before the wave's end");
        }

        sar::WaveFile::Channel& out = file.channels.emplace_back();
        out.samples.assign(channel.data.begin(), channel.data.begin() + static_cast<std::ptrdiff_t>(bytes));
        if (wave.encoding == WaveEncoding::kPcm16 && LittleEndian(flavor))
        {
            for (std::size_t i = 0; i + 1 < out.samples.size(); i += 2)
            {
                std::swap(out.samples[i], out.samples[i + 1]);
            }
        }
        if (wave.encoding == WaveEncoding::kDspAdpcm)
        {
            const DspAdpcmInfo info = channel.adpcm.value_or(DspAdpcmInfo{});
            out.adpcm = sar::DspAdpcmInfo{.coefs = info.coefs,
                                          .pred_scale = info.pred_scale,
                                          .yn1 = info.yn1,
                                          .yn2 = info.yn2,
                                          .loop_pred_scale = info.loop_pred_scale,
                                          .loop_yn1 = info.loop_yn1,
                                          .loop_yn2 = info.loop_yn2};
        }
    }

    return file;
}

sar::BankFile BankFileOf(const Bank& bank, const SndBuild& build, sar::Flavor flavor, uint32_t version,
                         uint32_t wave_archive)
{
    sar::BankFile file;
    file.flavor = flavor;
    file.version = version;
    for (std::size_t i = 0; i < bank.Waves().size(); ++i)
    {
        file.waves.push_back({.wave_archive_item = wave_archive, .index = static_cast<uint32_t>(i)});
    }
    for (uint32_t program = 0; program < bank.InstrumentCount(); ++program)
    {
        file.instruments.push_back(InstrumentOf(bank, program, build));
    }

    return file;
}

sar::StreamFile StreamFileOf(const StreamFile& stream, sar::Flavor flavor, uint32_t version)
{
    if (stream.loop && stream.loop_start % stream.block_samples != 0)
    {
        throw std::invalid_argument("a stream of the Wii whose loop starts inside a block");
    }

    // The stream as the Wii U holds it, in the Wii's byte order, then as a stream of `flavor`.
    sar::StreamFile later;
    later.flavor = sar::Flavor::kCafe;
    later.version = 0x00040000;
    later.encoding = EncodingOf(stream.encoding);
    later.loop = stream.loop;
    later.sample_rate = stream.sample_rate;
    later.loop_start = stream.loop ? stream.loop_start : 0;
    later.sample_count = stream.sample_count;
    later.block_samples = stream.block_samples;
    later.original_loop_start = later.loop_start;
    later.original_loop_end = stream.sample_count;
    std::vector<sar::StreamFile::Track>& tracks = later.tracks.emplace();
    for (const StreamFile::Track& track : stream.tracks)
    {
        tracks.push_back({.volume = track.volume, .pan = track.pan, .span = 0, .flags = 0, .channels = track.channels});
    }

    // The decoded samples serve where ADPC holds no entry for a block's start.
    const uint32_t blocks = stream.BlockCount();
    const uint32_t interval = stream.history_interval;
    const auto entry_of = [&](uint32_t block) -> std::optional<std::size_t>
    {
        const uint64_t start = uint64_t{block} * stream.block_samples;
        if (interval == 0 || start % interval != 0)
        {
            return std::nullopt;
        }

        return static_cast<std::size_t>(start / interval);
    };
    std::optional<wave::Pcm> decoded;
    for (const StreamFile::Channel& channel : stream.channels)
    {
        for (uint32_t b = 0; !decoded && b < blocks; ++b)
        {
            const std::optional<std::size_t> entry = entry_of(b);
            if (!entry || *entry >= channel.history.size())
            {
                decoded = DecodeStream(stream);
            }
        }
    }

    for (std::size_t c = 0; c < stream.channels.size(); ++c)
    {
        const StreamFile::Channel& channel = stream.channels[c];
        sar::StreamFile::Channel& out = later.channels.emplace_back();
        out.samples = channel.samples;
        if (channel.adpcm)
        {
            const DspAdpcmInfo& info = *channel.adpcm;
            out.adpcm = sar::DspAdpcmInfo{.coefs = info.coefs,
                                          .pred_scale = info.pred_scale,
                                          .yn1 = info.yn1,
                                          .yn2 = info.yn2,
                                          .loop_pred_scale = info.loop_pred_scale,
                                          .loop_yn1 = info.loop_yn1,
                                          .loop_yn2 = info.loop_yn2};
        }
        for (uint32_t b = 0; b < blocks; ++b)
        {
            const std::optional<std::size_t> entry = entry_of(b);
            if (entry && *entry < channel.history.size())
            {
                out.history.push_back(channel.history[*entry]);
                continue;
            }

            const std::vector<int16_t>& samples = decoded->channels[c];
            const std::size_t start = std::size_t{b} * stream.block_samples;
            out.history.push_back(
                {start >= 1 ? samples[start - 1] : int16_t{0}, start >= 2 ? samples[start - 2] : int16_t{0}});
        }
    }

    return sar::ConvertStream(later, flavor, version);
}

StreamFile WiiStreamOf(const sar::StreamFile& stream)
{
    if (stream.loop && stream.loop_start % stream.block_samples != 0)
    {
        throw std::invalid_argument("a stream whose loop starts inside a block");
    }
    if (stream.channels.size() > kMostStreamCount || (stream.tracks && stream.tracks->size() > kMostStreamCount))
    {
        throw std::invalid_argument("a stream with more than 255 channels or tracks");
    }
    if (stream.sample_rate > kMostRate)
    {
        throw std::invalid_argument("a stream whose rate passes the Wii's 24 bits");
    }

    // The stream as the Wii U holds it: without samples past its end, and in the Wii's byte order.
    const sar::StreamFile later = sar::ConvertStream(stream, sar::Flavor::kCafe, kLaterStreamVersion);
    StreamFile wii;
    wii.encoding = WiiEncodingOf(later.encoding);
    wii.loop = later.loop;
    wii.sample_rate = later.sample_rate;
    wii.loop_start = later.loop ? later.loop_start : 0;
    wii.sample_count = later.sample_count;
    wii.block_samples = StreamBlockSamples(wii.encoding);
    if (later.block_samples != wii.block_samples)
    {
        throw std::invalid_argument("a stream whose blocks don't take 8 KiB");
    }
    wii.history_interval = wii.encoding == WaveEncoding::kDspAdpcm ? wii.block_samples : 0;
    wii.extended_tracks = true;

    // The tracks: the stream's, or a track for each pair of channels.
    if (later.tracks)
    {
        for (const sar::StreamFile::Track& track : *later.tracks)
        {
            wii.tracks.push_back({.volume = track.volume, .pan = track.pan, .channels = track.channels});
        }
    }
    else
    {
        for (std::size_t c = 0; c < later.channels.size(); c += 2)
        {
            StreamFile::Track& track = wii.tracks.emplace_back();
            track.channels.push_back(static_cast<uint8_t>(c));
            if (c + 1 < later.channels.size())
            {
                track.channels.push_back(static_cast<uint8_t>(c + 1));
            }
        }
    }

    // Each channel's blocks, its info and ADPC's entries. SEEK holds an entry before each block.
    for (const sar::StreamFile::Channel& channel : later.channels)
    {
        StreamFile::Channel& out = wii.channels.emplace_back();
        out.samples = channel.samples;
        if (channel.adpcm && wii.encoding == WaveEncoding::kDspAdpcm)
        {
            const sar::DspAdpcmInfo& info = *channel.adpcm;
            out.adpcm = DspAdpcmInfo{.coefs = info.coefs,
                                     .yn1 = info.yn1,
                                     .yn2 = info.yn2,
                                     .gain = 0,
                                     .pred_scale = info.pred_scale,
                                     .loop_pred_scale = info.loop_pred_scale,
                                     .loop_yn1 = info.loop_yn1,
                                     .loop_yn2 = info.loop_yn2};
            out.history = channel.history;
        }
    }

    return wii;
}

WaveFile WiiWaveOf(const sar::WaveFile& wave)
{
    if (wave.channels.empty())
    {
        throw std::invalid_argument("a wave without channels");
    }

    WaveFile file;
    file.encoding = WiiEncodingOf(wave.encoding);
    file.loop = wave.loop;
    file.sample_rate = wave.sample_rate;
    file.loop_start = wave.loop_start;
    file.loop_end = wave.loop_end;
    if (file.sample_rate > 0xFFFFFF)
    {
        throw std::invalid_argument("a wave whose rate passes the Wii's 24 bits");
    }

    const std::size_t bytes = BytesOf(file.encoding, wave.loop_end);
    const std::size_t channels = std::min(wave.channels.size(), kPlayedChannels);
    for (std::size_t c = 0; c < channels; ++c)
    {
        const sar::WaveFile::Channel& channel = wave.channels[c];
        if (channel.samples.size() < bytes)
        {
            throw std::invalid_argument("the samples of a channel of a wave end before the wave's end");
        }

        WaveFile::Channel& out = file.channels.emplace_back();
        out.samples.assign(channel.samples.begin(), channel.samples.begin() + static_cast<std::ptrdiff_t>(bytes));
        if (file.encoding == WaveEncoding::kPcm16 && LittleEndian(wave.flavor))
        {
            for (std::size_t i = 0; i + 1 < out.samples.size(); i += 2)
            {
                std::swap(out.samples[i], out.samples[i + 1]);
            }
        }
        if (file.encoding == WaveEncoding::kDspAdpcm)
        {
            const sar::DspAdpcmInfo info = channel.adpcm.value_or(sar::DspAdpcmInfo{});
            out.adpcm = DspAdpcmInfo{.coefs = info.coefs,
                                     .yn1 = info.yn1,
                                     .yn2 = info.yn2,
                                     .pred_scale = info.pred_scale,
                                     .loop_pred_scale = info.loop_pred_scale,
                                     .loop_yn1 = info.loop_yn1,
                                     .loop_yn2 = info.loop_yn2};
        }
    }

    return file;
}

BankFile WiiBankOf(const sar::BankFile& bank, uint16_t version, const std::vector<std::optional<WaveFile>>& waves)
{
    HeldWaves held = HeldWavesOf(waves);
    std::vector<std::optional<BankFile::Instrument>> instruments;
    for (const std::optional<sar::BankFile::Instrument>& instrument : bank.instruments)
    {
        if (!instrument)
        {
            instruments.emplace_back();
            continue;
        }

        BankFile::Instrument& out = instruments.emplace_back().emplace();
        for (const sar::BankFile::KeyRegion& key : instrument->key_regions)
        {
            BankFile::KeyRegion& key_region = out.key_regions.emplace_back();
            key_region.first_key = key.first_key;
            key_region.last_key = key.last_key;
            for (const sar::BankFile::Region& velocity : key.regions)
            {
                if (velocity.values.wave_id_index >= waves.size())
                {
                    throw std::invalid_argument("a region plays a wave past the bank's waves");
                }
                Region region = RegionOf(velocity.values);
                region.wave_index = held.indexes[velocity.values.wave_id_index];
                key_region.regions.push_back({velocity.first_velocity, velocity.last_velocity, region});
            }
        }
    }

    return BankFile::FromInstruments(version, instruments, std::move(held.waves));
}

WaveArchiveFile WiiWaveArchiveOf(const sar::WaveArchiveFile& archive)
{
    WaveArchiveFile file;
    for (const std::vector<uint8_t>& wave : archive.waves)
    {
        file.waves.push_back(WiiWaveOf(sar::WaveFile::Read(wave)).Write());
    }

    return file;
}

WsdFile WiiWsdOf(const sar::WaveSoundFile& file, uint16_t version, const std::vector<std::optional<WaveFile>>& waves)
{
    HeldWaves held = HeldWavesOf(waves);
    WsdFile wsd;
    wsd.version = version;
    for (const sar::WaveSoundFile::WaveSound& sound : file.sounds)
    {
        WsdFile::WaveSound& out = wsd.sounds.emplace_back();
        out.pitch = sound.pitch;
        out.pan = sound.pan;
        out.surround_pan = sound.span;
        out.fx_sends = sound.aux_sends;
        out.main_send = sound.main_send;
        for (const std::vector<sar::WaveSoundFile::NoteEvent>& track : sound.tracks)
        {
            WsdFile::Track& events = out.tracks.emplace_back();
            for (const sar::WaveSoundFile::NoteEvent& event : track)
            {
                events.events.push_back({event.position, event.length, event.note});
            }
        }
        for (std::size_t n = 0; n < sound.notes.size(); ++n)
        {
            const sar::VelocityRegion& note = sound.notes[n];
            if (note.wave_id_index >= waves.size())
            {
                throw std::invalid_argument("a note plays a wave past the file's waves");
            }

            const sar::Adshr& envelope = n == 0 ? sound.adshr : note.adshr;
            out.notes.push_back({.wave_index = held.indexes[note.wave_id_index],
                                 .attack = envelope.attack,
                                 .decay = envelope.decay,
                                 .sustain = envelope.sustain,
                                 .release = envelope.release,
                                 .hold = envelope.hold,
                                 .original_key = note.original_key,
                                 .volume = note.volume,
                                 .pan = note.pan,
                                 .pitch = note.pitch});
        }
    }
    wsd.waves = std::move(held.waves);

    return wsd;
}

} // namespace ntdlib::rsar
