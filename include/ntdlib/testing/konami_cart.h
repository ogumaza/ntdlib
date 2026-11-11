// SPDX-License-Identifier: MIT

// Small synthetic cartridges of Konami's GBA driver for tests. SyntheticRom contains two songs and a sample at fixed
// places, for a driver whose tables a test gives by hand. ScanRom lays out a song table and a sample table for the
// detection scans to find. Neither contains the driver's code.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/testing/agb_files.h"

namespace ntdlib::testing::konami_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::konami::Revision;

// The size of a song table entry with 16 tracks, as the tests' songs have.
inline constexpr uint32_t kSongEntrySize = 36;

// A cartridge image being put together.
class Image : public ntdlib::testing::agb_files::CartImage
{
public:
    using CartImage::CartImage;

    // Writes entry `song` of the song table at `table` in revision `revision`: its base address and the offset of every
    // track. Each offset is `rest` unless `offsets` gives another. Entries of the WCT 2004 revision have the same
    // layout as those of the Ultimate Masters revision.
    void Song(uint32_t table, int song, uint32_t base, uint16_t rest, const std::vector<std::pair<int, int>>& offsets,
              Revision revision = Revision::kUltimateMasters)
    {
        const uint32_t e = table + uint32_t(song) * SongEntrySize(revision);
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
};

// Songs and one sample at fixed places, for use with DriverOverrides.
inline constexpr uint32_t kSongTable = kRomBase + 0x100;
inline constexpr uint32_t kSongBase = kRomBase + 0x200;
inline constexpr uint32_t kSampleTable = kRomBase + 0x400;
inline constexpr uint32_t kSampleHeader = kRomBase + 0x500;

// The starts of song 0's tracks, relative to kSongBase: every track ends at once except 0 (PSG), 4 and 5 (samples).
inline constexpr uint16_t kRestOffset = 0x100;
inline const std::vector<std::pair<int, int>> kTrackOffsets = {{0, 0x40}, {4, 0x00}, {5, 0x80}};

// Returns a cartridge without driver code, holding two songs and a sample at the places above. Song 0 loops and plays
// notes on tracks 0, 4 and 5; song 1 plays none.
inline Image SyntheticImage()
{
    Image m(0x1000);

    // Song 0.
    m.Song(kSongTable, 0, kSongBase, kRestOffset, kTrackOffsets);
    m.PutBytes(kSongBase + 0x100, {0x00, 0xFD});

    // Track 4: note (vol 16, sample 5), wait 10, rest, wait 5, loop the song.
    m.PutBytes(kSongBase + 0x00, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xE0, 0x05, 0xFF, 0x01, 0x00});

    // Track 5: wait 5, loop point (+2 bytes), wait 2, note (vol 7, sample 5, +12), wait 3, rest, wait 1, end.
    m.PutBytes(kSongBase + 0x80, {0x05, 0xF3, 0xAA, 0xBB, 0x02, 0xB7, 0x05, 0x0C, 0x03, 0xE0, 0x01, 0xFE});

    // Track 0: duty 25%, PSG note (vol 3, note 24), wait 4, rest, wait 0, end.
    m.PutBytes(kSongBase + 0x40, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE});

    // Song 1: a placeholder that plays no notes. Every track: rest, wait 5, end.
    m.Song(kSongTable, 1, kSongBase + 0x180, 0, {});
    m.PutBytes(kSongBase + 0x180, {0x00, 0xE0, 0x05, 0xFE});

    // Sample 5: 64-point sawtooth at the mixer rate, one-shot.
    m.Put32(kSampleTable + 4 * 5, kSampleHeader);
    m.Put32(kSampleHeader, 0x1000);
    m.Put32(kSampleHeader + 4, 64);
    m.Put32(kSampleHeader + 8, 0xFFFFFFFF);
    for (int i = 0; i < 64; i++)
    {
        m.Put8(kSampleHeader + 12 + i, uint8_t(int8_t(i * 4 - 128)));
    }

    return m;
}

inline Rom SyntheticRom()
{
    return SyntheticImage().ToRom();
}

// The addresses ScanRom puts the song and sample tables at.
inline constexpr uint32_t kScanSongTable = kRomBase + 0x100;
inline constexpr uint32_t kScanSampleTable = kRomBase + 0x168;

// Returns a cartridge with no driver code, but with song and sample tables laid out the way the driver's data is, for
// the detection scans to find: two songs right after the song table, the tracks of each song tiling its data, and
// sample headers right after the sample table. `gap` puts that many bytes between the two songs.
inline Image ScanImage(uint32_t gap = 0)
{
    Image m(0x400);
    constexpr uint32_t kSong0 = kScanSongTable + 2 * kSongEntrySize;

    // Song 0: track 4 at +0 (note, sample 0), track 0 at +8 (PSG note), the other tracks share +17.
    m.Song(kScanSongTable, 0, kSong0, 17, {{4, 0}, {0, 8}});
    m.PutBytes(kSong0 + 0, {0x00, 0xA8, 0x10, 0x00, 0x0A, 0xE0, 0x05, 0xFE});
    m.PutBytes(kSong0 + 8, {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE});
    m.PutBytes(kSong0 + 17, {0x00, 0xFD});

    // Song 1, from the end of song 0's data: track 5 at +0 (note, sample 1, then stop the song), the other tracks share
    // +8.
    const uint32_t song1 = kSong0 + 19 + gap;
    m.Song(kScanSongTable, 1, song1, 8, {{5, 0}});
    m.PutBytes(song1 + 0, {0x00, 0xA3, 0x01, 0x08, 0xE0, 0x00, 0xFF, 0x00});
    m.PutBytes(song1 + 8, {0x00, 0xFD});

    // Samples 0 (one-shot) and 1 (looped), with their headers right after the table.
    constexpr uint32_t kHeader0 = kScanSampleTable + 8, kHeader1 = kHeader0 + 12 + 32;
    m.Put32(kScanSampleTable, kHeader0);
    m.Put32(kScanSampleTable + 4, kHeader1);

    m.Put32(kHeader0, 0x1000);
    m.Put32(kHeader0 + 4, 32);
    m.Put32(kHeader0 + 8, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 32; i++)
    {
        m.Put8(kHeader0 + 12 + i, uint8_t(i * 8));
    }

    m.Put32(kHeader1, 0xC2D);
    m.Put32(kHeader1 + 4, 16);
    m.Put32(kHeader1 + 8, 4);
    for (uint32_t i = 0; i < 16; i++)
    {
        m.Put8(kHeader1 + 12 + i, uint8_t(i * 16));
    }

    return m;
}

inline Rom ScanRom(uint32_t gap = 0)
{
    return ScanImage(gap).ToRom();
}

} // namespace ntdlib::testing::konami_cart
