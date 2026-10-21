// SPDX-License-Identifier: MIT

// The waves, the streams and the archives that the tests of the Wii U's and the Switch's archive engines play, in the
// console's byte order: big-endian on the Wii U, little-endian on the Switch.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/testing/sar_files.h"

namespace nwsnd_engine_files
{

namespace sar_files = ntdlib::testing::sar_files;

// `count` PCM16 samples of `value`.
inline std::vector<uint8_t> ConstantWave(bool big_endian, int count, int16_t value)
{
    const auto high = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
    const auto low = static_cast<uint8_t>(static_cast<uint16_t>(value));
    std::vector<uint8_t> bytes;
    for (int i = 0; i < count; ++i)
    {
        bytes.push_back(big_endian ? high : low);
        bytes.push_back(big_endian ? low : high);
    }

    return bytes;
}

// A PCM16 wave of `samples` samples of `value` at 48 kHz.
inline sar_files::WaveSpec ConstantPcmWave(bool big_endian, int samples, int16_t value = 0x1000)
{
    sar_files::WaveSpec wave;
    wave.encoding = 1;
    wave.sample_rate = 48000;
    wave.loop_end = static_cast<uint32_t>(samples);
    wave.channels = {ConstantWave(big_endian, samples, value)};

    return wave;
}

// A stereo DSP-ADPCM wave at 32 kHz that loops from sample 2800 to its end at sample 15988. Each frame's header picks
// one of the 8 coefficient pairs and a scale. The nibbles are pseudo-random.
inline sar_files::WaveSpec LoopingAdpcmWave()
{
    constexpr int kSamples = 15988;
    sar_files::WaveSpec wave;
    wave.encoding = 2;
    wave.loop = true;
    wave.sample_rate = 32000;
    wave.loop_start = 2800;
    wave.loop_end = kSamples;
    uint32_t seed = 1;
    for (int channel = 0; channel < 2; ++channel)
    {
        std::vector<uint8_t> bytes;
        for (int frame = 0; frame < kSamples / 14; ++frame)
        {
            bytes.push_back(static_cast<uint8_t>((frame % 8) << 4 | (frame + channel) % 12));
            for (int i = 0; i < 7; ++i)
            {
                seed = seed * 1103515245u + 12345u;
                bytes.push_back(static_cast<uint8_t>(seed >> 16));
            }
        }
        wave.channels.push_back(std::move(bytes));
    }

    return wave;
}

// An archive with one sequence sound that plays `bytecode` on a bank with an instrument for each of `waves`. Program i
// plays wave i for every key, at original key 60. The sound allocates the tracks in `tracks`, a bit each.
inline std::vector<uint8_t> ProgramArchive(bool big_endian, const std::vector<uint8_t>& bytecode,
                                           const std::vector<sar_files::WaveSpec>& waves, uint32_t tracks = 1)
{
    sar_files::BankSpec bank;
    std::vector<std::vector<uint8_t>> wave_files;
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        sar_files::Region region;
        region.wave = static_cast<uint32_t>(i);
        region.original_key = 60;
        sar_files::VelocityTable velocities;
        velocities.entries = {{0, region}};
        sar_files::KeyTable keys;
        keys.entries = {{0, velocities}};
        bank.waves.push_back({0x05000000, static_cast<uint32_t>(i)});
        bank.instruments.push_back(keys);
        wave_files.push_back(sar_files::Wave(big_endian, waves[i]));
    }

    sar_files::SoundSpec sound;
    sound.name = "SEQ_NOTE";
    sound.volume = 127;
    sound.banks = {0};
    sound.allocate_track_flags = tracks;
    sar_files::ArchiveSpec spec;
    spec.sounds = {sound};
    spec.banks = {{"BANK_NOTE", 1}};
    spec.wave_archives = {{"WARC_NOTE", 2}};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{sar_files::Sequence(big_endian, bytecode, {{"Note", 0}}), "", false},
                  {sar_files::Bank(big_endian, bank), "", false},
                  {sar_files::WaveArchive(big_endian, wave_files), "", false}};

    return sar_files::Archive(big_endian, spec);
}

// An archive with one wave sound, WSD_NOTE, at `volume`, with the settings of `sound`. Its wave sound file names
// `waves`, wave i at index i of a wave archive. The sound's channel priority is `channel_priority`, if given.
inline std::vector<uint8_t> WaveSoundArchive(bool big_endian, const sar_files::WaveSoundSpec& sound,
                                             const std::vector<sar_files::WaveSpec>& waves, uint8_t volume = 127,
                                             std::optional<uint32_t> channel_priority = std::nullopt)
{
    sar_files::WaveSoundFileSpec file;
    std::vector<std::vector<uint8_t>> wave_files;
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        file.waves.push_back({0x05000000, static_cast<uint32_t>(i)});
        wave_files.push_back(sar_files::Wave(big_endian, waves[i]));
    }
    file.sounds = {sound};

    sar_files::SoundSpec entry;
    entry.name = "WSD_NOTE";
    entry.kind = sar_files::SoundSpec::kWaveSound;
    entry.volume = volume;
    entry.channel_priority = channel_priority;
    sar_files::ArchiveSpec spec;
    spec.sounds = {entry};
    spec.wave_archives = {{"WARC_NOTE", 1}};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{sar_files::WaveSoundFile(big_endian, file), "", false},
                  {sar_files::WaveArchive(big_endian, wave_files), "", false}};

    return sar_files::Archive(big_endian, spec);
}

// A wave sound that plays wave 0 at original key 60, with the default settings.
inline sar_files::WaveSoundSpec PlainWaveSound()
{
    sar_files::WaveSoundSpec sound;
    sar_files::Region note;
    note.original_key = 60;
    sound.notes = {note};

    return sound;
}

// A stream track that plays `channels` with the default settings.
inline ntdlib::sar::StreamTrackInfo StreamTrack(std::vector<uint8_t> channels)
{
    ntdlib::sar::StreamTrackInfo track;
    track.channels = std::move(channels);

    return track;
}

// An archive with one stream sound, STRM_NOTE, at `volume`, with `stream`'s tracks. The stream's file lies outside the
// archive.
inline std::vector<uint8_t> StreamArchive(bool big_endian, const ntdlib::sar::StreamSoundInfo& stream,
                                          uint8_t volume = 127)
{
    sar_files::SoundSpec entry;
    entry.name = "STRM_NOTE";
    entry.kind = sar_files::SoundSpec::kStream;
    entry.volume = volume;
    entry.stream = stream;
    sar_files::ArchiveSpec spec;
    spec.sounds = {entry};
    spec.players = {{"PLAYER_NOTE", 1}};
    spec.files = {{{}, "stream/STRM_NOTE.bfstm", false}};

    return sar_files::Archive(big_endian, spec);
}

// The bytes of a stream file of `flavor` with `channels` in 16-bit PCM at 48 kHz, looped from `loop_start` if given,
// and with `tracks` in the file if given. The writer lays the file out as version 4.0.0. Another `version` then takes
// that version's place in the header. The reader reads the layout by its references.
inline std::vector<uint8_t> PcmStream(ntdlib::sar::Flavor flavor, const std::vector<std::vector<int16_t>>& channels,
                                      std::optional<uint32_t> loop_start = std::nullopt,
                                      std::optional<std::vector<ntdlib::sar::StreamFile::Track>> tracks = std::nullopt,
                                      uint32_t version = 0x00040000)
{
    ntdlib::sar::StreamFile stream =
        ntdlib::sar::MakeStream(flavor, 0x00040000, ntdlib::sar::WaveEncoding::kPcm16, 48000, channels, loop_start);
    stream.tracks = std::move(tracks);
    std::vector<uint8_t> bytes = stream.Write();
    const bool big_endian = flavor == ntdlib::sar::Flavor::kCafe;
    for (std::size_t i = 0; i < 4; ++i)
    {
        bytes[8 + i] = static_cast<uint8_t>(version >> (big_endian ? 24 - 8 * i : 8 * i));
    }

    return bytes;
}

} // namespace nwsnd_engine_files
