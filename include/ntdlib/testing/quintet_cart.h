// SPDX-License-Identifier: MIT

// Builds small cartridges with Quintet's GBA driver for tests: a synthetic copy of the code patterns that the
// driver's detection reads, for each revision, the driver's tables and the game's files. A Channel holds a channel's
// commands, Song() makes a song of channels, and a Cart places songs, sound effects, noise macros and samples.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::quintet_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::quintet::Revision;

// Addresses in a test cartridge: the driver's code that detection reads, the game's file lookup and the code that
// passes the files to the driver, the driver's tables, and the game's files.
inline constexpr uint32_t kPlay = kRomBase + 0x100;
inline constexpr uint32_t kPlayEffect = kRomBase + 0x180;
inline constexpr uint32_t kCode = kRomBase + 0x200;
inline constexpr uint32_t kLookup = kRomBase + 0x600;
inline constexpr uint32_t kGameCode = kRomBase + 0x700;
inline constexpr uint32_t kLoader = kRomBase + 0x800;
inline constexpr uint32_t kLoaderCall = kRomBase + 0x840;
inline constexpr uint32_t kEffectCode = kRomBase + 0x780;
inline constexpr uint32_t kLengthTable = kRomBase + 0x900;
inline constexpr uint32_t kFrequencyTable = kRomBase + 0x910;
inline constexpr uint32_t kPcmPitchTable = kRomBase + 0x9C0;
inline constexpr uint32_t kLfoTable = kRomBase + 0xA40;
inline constexpr uint32_t kFiles = kRomBase + 0x1000; // file n is at kFiles + n * 0x1000

// The game's files, and the driver's variables that the game stores them in.
inline constexpr int kWaveFile = 1;
inline constexpr int kMacroFile = 2;
inline constexpr int kSfxMacroFile = 3;
inline constexpr int kSampleFile = 4;
inline constexpr int kSfxSampleFile = 5;
inline constexpr int kMusicFile = 6;
inline constexpr int kEffectsFile = 7;
inline constexpr uint32_t kWaveVariable = 0x03000790;
inline constexpr uint32_t kMacroVariable = 0x03000794; // the A revision's music macros, or the J revision's two tables

// Returns the address of file `file`.
inline constexpr uint32_t FileAddress(int file)
{
    return kFiles + uint32_t(file) * 0x1000;
}

// A channel's data being written, one command at a time.
class Channel
{
public:
    Channel& Bytes(std::vector<uint8_t> b)
    {
        bytes_.insert(bytes_.end(), b.begin(), b.end());

        return *this;
    }

    // Adds a note of `pitch` (0-11, 12 to play again, 13 to rest) with the length that index `length` of the length
    // table gives: 384 ticks divided by 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 or 128.
    Channel& Note(int pitch, int length)
    {
        return Bytes({uint8_t(length << 4 | pitch)});
    }

    Channel& Rest(int length)
    {
        return Note(13, length);
    }

    std::vector<uint8_t> End()
    {
        bytes_.push_back(0xFF);

        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
};

// Returns a song's bytes: its length, its 6 channels' offsets and their data. Channels left out end at once.
inline std::vector<uint8_t> Song(std::vector<std::vector<uint8_t>> channels)
{
    channels.resize(6, {0xFF});
    std::vector<uint8_t> data;
    std::vector<uint16_t> offsets;
    for (const std::vector<uint8_t>& c : channels)
    {
        offsets.push_back(uint16_t(14 + data.size()));
        data.insert(data.end(), c.begin(), c.end());
    }
    if (data.size() % 2)
    {
        data.push_back(0);
    }

    std::vector<uint8_t> song;
    auto put16 = [&](uint16_t v)
    {
        song.push_back(uint8_t(v));
        song.push_back(uint8_t(v >> 8));
    };

    put16(uint16_t(14 + data.size()));
    for (uint16_t o : offsets)
    {
        put16(o);
    }
    song.insert(song.end(), data.begin(), data.end());

    return song;
}

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, for revision
// `revision`, the driver's tables and the game's files.
class Cart : public ntdlib::testing::agb_files::CartImage
{
public:
    explicit Cart(Revision revision, bool code = true) : CartImage(0x10000), revision_(revision)
    {
        WriteTables();
        if (code)
        {
            WriteCode();
        }

        // A wave of a saw, and the second wave a square, after the table's 2-byte header.
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(FileAddress(kWaveFile) + 2 + i, uint8_t((2 * i) << 4 | (2 * i + 1)));
            Put8(FileAddress(kWaveFile) + 18 + i, i < 8 ? 0xFF : 0x00);
        }
    }

    // Writes the songs into the music file, one after another.
    void Songs(const std::vector<std::vector<uint8_t>>& songs)
    {
        uint32_t at = FileAddress(kMusicFile);
        for (const std::vector<uint8_t>& s : songs)
        {
            for (size_t i = 0; i < s.size(); i++)
            {
                Put8(at + uint32_t(i), s[i]);
            }
            at += uint32_t(s.size());
        }
    }

    // Writes the sound effects into the sound effects' file, the driver's routine that plays one, and the game's code
    // that passes it the file. In the A revision, the game's sound init then gives the driver the sound effects' noise
    // macros and samples too. Its routine for the noise macros stores them in the word after the music's.
    void Effects(const std::vector<std::vector<uint8_t>>& effects)
    {
        uint32_t at = FileAddress(kEffectsFile);
        for (const std::vector<uint8_t>& e : effects)
        {
            for (size_t i = 0; i < e.size(); i++)
            {
                Put8(at + uint32_t(i), e[i]);
            }
            at += uint32_t(e.size());
        }

        const bool j = revision_ == Revision::kJ;
        Pattern(kPlayEffect,
                j ? "B570 4900 6008 2500 4800 6801 0068 1840 8840 1846 7830 28FF 4770"
                : revision_ == Revision::kS
                    ? "B570 4A00 2100 7011 4900 6008 2500 4E00 4800 6801 0068 1840 8840 1844 7820 28FF 4770"
                    : "B570 4900 6008 2500 4E00 4800 6801 0068 1840 8840 1844 7820 28FF 4770");
        at = PassFile(kEffectCode, kEffectsFile, kPlayEffect);
        if (!j)
        {
            const uint32_t setter = at;
            at = Setter(at, 0x6008, kMacroVariable + 4);
            at = PassFile(at, kSfxMacroFile, setter);
            PassFile(at, kSfxSampleFile, sample_setters_[1]);
        }
    }

    // Writes noise macros into a macro file: macro m starts at the offset that entry m + 1 of the table at the file's
    // start gives.
    void Macros(int file, const std::vector<std::vector<uint8_t>>& macros)
    {
        const uint32_t table = FileAddress(file);
        uint32_t at = table + 2 + 2 * uint32_t(macros.size() + 1);
        for (size_t m = 0; m < macros.size(); m++)
        {
            Put16(table + 2 + 2 * uint32_t(m + 1), uint16_t(at - table));
            for (uint8_t b : macros[m])
            {
                Put8(at++, b);
            }
        }
    }

    // Writes samples into a sample file, each with a header of its size and rate. Returns the address of each one's
    // data. Throws std::length_error where they run into the next file.
    std::vector<uint32_t> Samples(int file, const std::vector<std::pair<std::vector<int8_t>, int>>& samples)
    {
        std::vector<uint32_t> data;
        uint32_t at = FileAddress(file);
        for (const auto& [pcm, rate] : samples)
        {
            const uint32_t size = 6 + uint32_t(pcm.size());
            Put32(at, size);
            Put16(at + 4, uint16_t(rate));
            for (size_t i = 0; i < pcm.size(); i++)
            {
                Put8(at + 6 + uint32_t(i), uint8_t(pcm[i]));
            }
            data.push_back(at + 6);
            at += size;
        }
        Put32(at, 0);
        if (at + 4 > FileAddress(file + 1))
        {
            throw std::length_error("the samples run into the next file");
        }

        return data;
    }

    // Writes a music file at file `file`, as the J revision's loader reads it: the songs, the sound effects, the wave
    // table, the noise macros of banks 0 and 1, and the samples of banks 0 and 1. A word of its size comes before each
    // part. Each list of songs ends with a length of 0. Each bank has one sample. Returns the address of each part.
    std::array<uint32_t, 7> MusicFile(int file, const std::vector<std::vector<uint8_t>>& songs,
                                      const std::vector<std::vector<uint8_t>>& effects)
    {
        std::array<uint32_t, 7> parts = {};
        uint32_t at = FileAddress(file);
        for (size_t p = 0; p < parts.size(); p++)
        {
            std::vector<uint8_t> data(16, 0x11);
            if (p < 2)
            {
                data.clear();
                for (const std::vector<uint8_t>& s : p == 0 ? songs : effects)
                {
                    data.insert(data.end(), s.begin(), s.end());
                }
                data.resize(data.size() + 2, 0);
            }
            else if (p >= 5)
            {
                // A sample's size, its rate and its 4 points, and a size of 0 after it.
                data = {10, 0, 0, 0, 0x00, 0x20, 1, 2, 3, 4, 0, 0, 0, 0};
            }
            data.resize((data.size() + 3) & ~size_t(3), 0);

            Put32(at, uint32_t(data.size()));
            parts[p] = at + 4;
            for (size_t i = 0; i < data.size(); i++)
            {
                Put8(at + 4 + uint32_t(i), data[i]);
            }
            at += 4 + uint32_t(data.size());
        }

        return parts;
    }

    // Writes the J revision's loader of music files, and the game's code that passes it file `file` in a literal: ldr
    // r0, =file; bl loader. The game's code no longer passes the play routine a music file.
    void Loader(int file)
    {
        Pattern(kLoader, "B5F0 1C05 CD10 1C2A 08A0 0080 182D 4B00 2000 7018 8811 4E00 2900 D000 4C00 4770");
        Pattern(kLoaderCall, "4800 F000 F800", 0, FileAddress(file));
        PutBl(kLoaderCall + 2, kLoader);
        for (uint32_t i = 0; i < 12; i += 4)
        {
            Put32(kGameCode + 60 + i, 0);
        }
    }

private:
    // Writes `pattern`'s halfwords at `at`, with its ldr at halfword `ldr` (if any) loading `literal` from a pool after
    // it. Returns the next free word.
    uint32_t Pattern(uint32_t at, const char* pattern, int ldr = -1, uint32_t literal = 0)
    {
        std::istringstream in(pattern);
        std::string word;
        uint32_t a = at;
        while (in >> word)
        {
            Put16(a, uint16_t(std::stoul(word, nullptr, 16)));
            a += 2;
        }

        const uint32_t pool = (a + 3) & ~3u;
        if (ldr >= 0)
        {
            const uint32_t ins = at + 2 * uint32_t(ldr);
            const uint16_t op = uint16_t(Get8(ins + 1) << 8);
            Put16(ins, uint16_t(op | ((pool - ((ins + 4) & ~3u)) / 4)));
            Put32(pool, literal);
        }

        return pool + 4;
    }

    // Writes a routine at the word `at` that stores r0 with `store` (str r0, [r1] or [r1, #4]) into `variable`. Returns
    // the next free word.
    uint32_t Setter(uint32_t at, uint16_t store, uint32_t variable)
    {
        Put16(at, 0x4901);
        Put16(at + 2, store);
        Put16(at + 4, 0x4770);
        Put32(at + 8, variable);

        return at + 12;
    }

    // Writes the game's code that looks file `file` up and passes it to `routine`, at `at`. In the S revision, the code
    // passes the file from a literal instead: ldr r1, =file; adds r0, r1, #0; bl routine. Returns the next free word.
    uint32_t PassFile(uint32_t at, int file, uint32_t routine)
    {
        if (revision_ == Revision::kS)
        {
            const uint32_t next = Pattern(at, "4900 1C08 F000 F800", 0, FileAddress(file));
            PutBl(at + 4, routine);
            return next;
        }

        Put16(at, 0x2000);
        Put16(at + 2, uint16_t(0x2100 | file));
        PutBl(at + 4, kLookup);
        PutBl(at + 8, routine);

        return at + 12;
    }

    // Writes the tables: the note values, the square channels' frequency settings from C2, each note's PCM rate change
    // in 1/1000s of the sample's rate, from two octaves down, and a sine.
    void WriteTables()
    {
        const uint8_t kLengths[13] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128};
        for (uint32_t i = 0; i < 13; i++)
        {
            Put8(kLengthTable + i, kLengths[i]);
        }

        for (int n = 0; n < 84; n++)
        {
            const double hz = 65.40639 * std::pow(2.0, n / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(n), uint16_t(std::lround(std::max(0.0, 2048 - 131072 / hz))));
        }

        for (int n = 0; n < 53; n++)
        {
            Put16(kPcmPitchTable + 2 * uint32_t(n),
                  uint16_t(int16_t(std::lround(1000 * (std::pow(2.0, (n - 24) / 12.0) - 1)))));
        }

        for (uint32_t i = 0; i < 256; i++)
        {
            Put8(kLfoTable + i, uint8_t(int8_t(std::lround(127 * std::sin(i * 6.283185307179586 / 256)))));
        }
    }

    // Writes the driver's code that detection reads, the game's lookup of its files, and the game's code that gives the
    // driver its files and plays the songs.
    void WriteCode()
    {
        const bool j = revision_ == Revision::kJ;
        if (j)
        {
            Pattern(kPlay, "B5F0 1C07 2500 4E00 00AC CE01 2100 F000 F800 4800 1824 6820 4770");
        }
        else
        {
            Pattern(kPlay, "B5F0 1C07 2400 4E00 4D00 CE01 F000 F800 CD01 4770");
        }

        // The S revision's note reader takes the place of the code that reads the length table.
        uint32_t at = revision_ == Revision::kS
                          ? Pattern(kCode, "2080 4030 2800 D000 E000 46B0 7821 20C0 0040")
                          : Pattern(kCode, "250D DD00 E100 220C DD00 E100 4800 1810 7801 20C0 0040", 6, kLengthTable);
        at = Pattern(at, "2E53 DD00 2653 4D00 1C30 210C", 3, kFrequencyTable);
        at = Pattern(at, "2A34 DD00 2234 4800 0051", 3, kPcmPitchTable);
        at = Pattern(at, "4800 0411 1409 1809 2000 5608", 0, kLfoTable);
        at = Pattern(at, "B530 0600 0E02 4800 6801 1C05 2900", 3, kWaveVariable);
        if (j)
        {
            at = Pattern(at, "B510 0600 0E03 4800 6802 1C14 3490 2000 6020 2B00 D000 4900 7B52 0090 1840", 11,
                         kMacroVariable);
        }
        else
        {
            at = Pattern(at, "0600 0E03 1C19 4800 7800 2800 D000 4800 6802", 7, kMacroVariable);
        }

        const uint32_t loader = at;
        at = Pattern(loader, j ? "B570 4B00 008A 18D2 1C03 6013 4800 2200 024D 247F" : "B570 1C02 4800 2500 024C 237F");

        // The setters: ldr r1, =variable; str r0, [r1] (or [r1, #4] for the second table); bx lr, and the routines that
        // pass a sample file to the loader with its bank.
        const uint32_t wave_setter = at;
        at = Setter(at, 0x6008, kWaveVariable);
        const uint32_t macro_setter = at;
        at = Setter(at, 0x6008, kMacroVariable);
        const uint32_t sfx_macro_setter = at;
        at = Setter(at, 0x6048, kMacroVariable);
        for (uint32_t bank = 0; bank < 2; bank++)
        {
            sample_setters_[bank] = at;
            Put16(at, 0xB500);
            Put16(at + 2, uint16_t(0x2100 | bank));
            PutBl(at + 4, loader);
            Put16(at + 8, 0xBC01);
            Put16(at + 10, 0x4700);
            at += 12;
        }

        // The game's lookup: file n of any archive is at kFiles + n * 0x1000.
        Put16(kLookup, 0x0309);     // lsls r1, r1, #12
        Put16(kLookup + 2, 0x4801); // ldr r0, =kFiles
        Put16(kLookup + 4, 0x1840); // adds r0, r0, r1
        Put16(kLookup + 6, 0x4770); // bx lr
        Put32(kLookup + 8, kFiles);

        // The game's sound init gives the driver its files, and its play-song routine passes the music file.
        at = PassFile(kGameCode, kWaveFile, wave_setter);
        at = PassFile(at, kMacroFile, macro_setter);
        at = PassFile(at, kSampleFile, sample_setters_[0]);
        if (j)
        {
            at = PassFile(at, kSfxMacroFile, sfx_macro_setter);
            at = PassFile(at, kSfxSampleFile, sample_setters_[1]);
        }
        if (revision_ == Revision::kS)
        {
            // The S revision's game loads the music file's address and skips to the song before its call: ldr r0,
            // =file; str r0, [r7, #4]; ldr r0, [r7, #4]; bl play.
            Pattern(at, "4800 6078 6878 F000 F800", 0, FileAddress(kMusicFile));
            PutBl(at + 6, kPlay);
        }
        else
        {
            PassFile(at, kMusicFile, kPlay);
        }
    }

    Revision revision_;
    std::array<uint32_t, 2> sample_setters_ = {}; // the routines that pass each bank's samples to the loader
};

// Returns a saw wave of `length` signed bytes.
inline std::vector<int8_t> Saw(size_t length)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 7 % 256) - 128));
    }

    return pcm;
}

} // namespace ntdlib::testing::quintet_cart
