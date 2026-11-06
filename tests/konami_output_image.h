// SPDX-License-Identifier: MIT

// Small synthetic cartridges for the tests of Konami's output stage and player.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/testing/agb_cart.h"

namespace konami_testing
{

using ntdlib::agb::kRomBase;
using ntdlib::konami::DriverInfo;
using ntdlib::konami::Revision;
using ntdlib::konami::SongEntrySize;
using ntdlib::konami::TrackCount;

// The places of a cartridge's tables.
constexpr uint32_t kSongs = kRomBase + 0x100;
constexpr uint32_t kBase = kRomBase + 0x400;
constexpr uint32_t kMixer = kRomBase + 0x2000;
constexpr uint32_t kSampleTable = kRomBase + 0x2800;
constexpr uint32_t kSecondTable = kRomBase + 0x2880;
constexpr uint32_t kPitchTable = kRomBase + 0x2900; // the entry of pitch 0
constexpr uint32_t kWaves = kRomBase + 0x2A00;
constexpr uint32_t kNoises = kRomBase + 0x2B00;
constexpr uint32_t kFrequencies = kRomBase + 0x3000;
constexpr uint32_t kPeriods = kRomBase + 0x4000;
constexpr uint32_t kMap = kRomBase + 0x4800;
constexpr uint32_t kLevelTable = kRomBase + 0x4900; // a mixer's table of 256 bytes for each level
constexpr uint32_t kVolumes = kRomBase + 0x8000;    // a build's levels of each volume and pan
constexpr uint32_t kHeaders = kRomBase + 0xC000;
constexpr uint32_t kImageSize = 0x18000;
constexpr double kMixRate = 13379.0; // 1,254 CPU cycles a point: exactly 14 blocks a frame

// The build's size of a voice record, in the mixer's code: sub r7, r7, #size.
constexpr uint32_t kRecords28 = 0xE247701C;
constexpr uint32_t kRecords24 = 0xE2477018;
constexpr uint32_t kRecords20 = 0;

// GBATEK's names of the sound registers.
constexpr uint32_t kSound1CntL = 0x04000060;
constexpr uint32_t kSound1CntH = 0x04000062;
constexpr uint32_t kSound1CntX = 0x04000064;
constexpr uint32_t kSound2CntL = 0x04000068;
constexpr uint32_t kSound2CntH = 0x0400006C;
constexpr uint32_t kSound3CntL = 0x04000070;
constexpr uint32_t kSound3CntH = 0x04000072;
constexpr uint32_t kSound3CntX = 0x04000074;
constexpr uint32_t kSound4CntL = 0x04000078;
constexpr uint32_t kSound4CntH = 0x0400007C;
constexpr uint32_t kNr50 = 0x04000080;
constexpr uint32_t kNr51 = 0x04000081;
constexpr uint32_t kSoundCntH = 0x04000082;
constexpr uint32_t kSoundCntHHigh = 0x04000083;
constexpr uint32_t kWaveRam = 0x04000090;

inline const std::vector<uint8_t> kWaitingTrack = {0x7F, 0xFD};

class Image : public ntdlib::testing::agb_files::CartImage
{
public:
    Image() : CartImage(kImageSize)
    {
        // The pitch table round its centre: 4096 * 2 ^ (pitch / 384).
        const std::array<uint16_t, 5> centre = {4081, 4089, 4096, 4103, 4111};
        for (uint32_t i = 0; i < centre.size(); i++)
        {
            Put16(kPitchTable - 4 + 2 * i, centre[i]);
        }

        // The frequency stores show the pitch that chose them.
        for (uint32_t p = 0; p < 0x800; p++)
        {
            Put16(kFrequencies + 2 * p, uint16_t(0x8000 | p));
        }

        Put16(kNoises, 0x1234);
    }

    // Writes song `song`'s entry: each track starts at `rest` unless `offsets` gives another start.
    void Song(int song, uint32_t base, uint16_t rest, const std::vector<std::pair<int, int>>& offsets,
              Revision revision)
    {
        const uint32_t e = kSongs + uint32_t(song) * SongEntrySize(revision);
        Put32(e, base);
        for (int t = 0; t < TrackCount(revision); t++)
        {
            Put16(e + 4 + 2 * uint32_t(t), rest);
        }
        for (const auto& [t, o] : offsets)
        {
            Put16(e + 4 + 2 * uint32_t(t), uint16_t(o));
        }
    }

    // Writes song 0, with `bytes` as track `track`. Every other track waits for 127 frames: the silence at a track's
    // end gives output, and stores on a PSG channel.
    void OneTrack(int track, const std::vector<uint8_t>& bytes, Revision revision)
    {
        Song(0, kBase, 0x100, {{track, 0}}, revision);
        PutBytes(kBase, bytes);
        PutBytes(kBase + 0x100, kWaitingTrack);
    }

    // Writes the code by which Output finds the mixer. ldr r5, =table / ldrsh r0, [r7, #12] / movs r0, r0, lsl #17 /
    // ldrhs r5, =second, with the build's record size after them and the literals after that.
    void Mixer(uint32_t record_word)
    {
        Put32(kMixer - 4, 0xE59F5000 | 0x0C);
        Put32(kMixer, 0xE1D700FC);
        Put32(kMixer + 4, 0xE1B00880);
        Put32(kMixer + 8, 0x259F5000 | 0x04);
        Put32(kMixer + 12, record_word);
        Put32(kMixer + 16, kSampleTable);
        Put32(kMixer + 20, kSecondTable);
    }

    // Writes the load of a mixer's table of levels: ldr r0, =table / ldrb r8, [r7, #16], and the literal.
    void LevelTable()
    {
        Put32(kMixer + 32, 0xE59F0000 | 0x04);
        Put32(kMixer + 36, 0xE5D78010);
        Put32(kMixer + 44, kLevelTable);
    }

    // Writes sample `index`'s entry, and its header at kHeaders + 0x1000 * index: its step, its length and its loop's
    // start, or a negative value without a loop, then its bytes.
    void Sample(int index, uint32_t step, uint32_t length, int32_t loop, const std::vector<int8_t>& bytes)
    {
        const uint32_t header = kHeaders + 0x1000 * uint32_t(index);
        Put32(kSampleTable + 4 * uint32_t(index), header);
        Put32(header, step);
        Put32(header + 4, length);
        Put32(header + 8, uint32_t(loop));
        for (uint32_t i = 0; i < bytes.size(); i++)
        {
            Put8(header + 12 + i, uint8_t(bytes[i]));
        }
    }
};

inline DriverInfo InfoOf(Revision revision)
{
    DriverInfo info;
    info.revision = revision;
    info.song_table = kSongs;
    info.sample_table = kSampleTable;
    info.psg_freq_table = kFrequencies;
    info.noise_table = kNoises;
    info.volume_table = kVolumes;
    info.wave_table = kWaves;
    info.mix_rate = kMixRate;
    info.bend_kept = true;
    return info;
}

// The revision without a mixer: a FIFO and a timer serve each two voices.
inline DriverInfo FifoInfo()
{
    DriverInfo info = InfoOf(Revision::kDungeonDiceMonsters);
    info.mix_rate = 0;
    info.sample_map = kMap;
    info.sample_period_table = kPeriods;
    info.vibrato_entry = 1000;
    return info;
}

// Writes a sample of a table of 8-byte entries: its bytes' address, then its blocks of 16 in the high 12 bits and its
// timer period in the low 16.
inline void FifoSample(Image& m, int index, uint32_t blocks, uint32_t period, const std::vector<int8_t>& bytes)
{
    const uint32_t at = kHeaders + 0x1000 * uint32_t(index);
    m.Put32(kSampleTable + 8 * uint32_t(index), at);
    m.Put32(kSampleTable + 8 * uint32_t(index) + 4, blocks << 20 | period);
    for (uint32_t i = 0; i < bytes.size(); i++)
    {
        m.Put8(at + i, uint8_t(bytes[i]));
    }
}

inline std::vector<int8_t> Block(std::initializer_list<int> values)
{
    std::vector<int8_t> bytes;
    for (int i = 0; i < 16; i++)
    {
        for (int v : values)
        {
            bytes.push_back(int8_t(v));
        }
    }
    bytes.resize(16);
    return bytes;
}

} // namespace konami_testing
