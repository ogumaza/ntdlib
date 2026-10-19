// SPDX-License-Identifier: MIT

// The waves and the archive that the tests of the Wii U's and the Switch's archive engines play, in the console's byte
// order: big-endian on the Wii U, little-endian on the Switch.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

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

} // namespace nwsnd_engine_files
