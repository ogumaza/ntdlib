// SPDX-License-Identifier: MIT

// A small synthetic cartridge of Nintendo R&D2's driver for the tests. No game ROM is needed. It holds a copy of the
// code patterns that detection reads, the driver's tables, the game's settings, and samples, a bank and sequences. The
// cartridge holds none of the driver's routines: nothing can run its code.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/text.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"

namespace ntdlib::testing::rd2_cart
{

using ntdlib::agb::kRomBase;
using ntdlib::rd2::Revision;

// Addresses in a test cartridge: the driver's code that detection reads, the game's call to its init, the driver's
// tables, and the game's settings and data.
constexpr uint32_t kInit = kRomBase + 0x100;
constexpr uint32_t kGameInit = kRomBase + 0x180;
constexpr uint32_t kCode = kRomBase + 0x200;
constexpr uint32_t kPitchTable = kRomBase + 0x800;
constexpr uint32_t kFrequencyTable = kRomBase + 0xA00;
constexpr uint32_t kNoiseTable = kRomBase + 0xB00;
constexpr uint32_t kLfoTable = kRomBase + 0xB80;
constexpr uint32_t kWaveVolumes = kRomBase + 0xC80;
constexpr uint32_t kVoiceClasses = kRomBase + 0xC88;
constexpr uint32_t kKeyEnvelope = kRomBase + 0xC90;
constexpr uint32_t kSettings = kRomBase + 0xD00;
constexpr uint32_t kSampleSets = kRomBase + 0xE00;
constexpr uint32_t kBanks = kRomBase + 0xF00;
constexpr uint32_t kBankSampleSets = kRomBase + 0xF80;
constexpr uint32_t kSequences = kRomBase + 0x1000;
constexpr uint32_t kBankLists = kRomBase + 0x1080;
constexpr uint32_t kSamples = kRomBase + 0x2000; // the sample set's table, then its samples
constexpr uint32_t kBank = kRomBase + 0x4000;
constexpr uint32_t kSequenceData = kRomBase + 0x6000;
constexpr uint32_t kEffects = kRomBase + 0x1100; // the table of the sound effects' sequences
constexpr uint32_t kEffectBankLists = kRomBase + 0x1180;
constexpr uint32_t kEffectData = kRomBase + 0x9000;

// The game's play-music routine, its table of music, and the routines that its bl's call.
constexpr uint32_t kPlayMusic = kRomBase + 0x600;
constexpr uint32_t kMusicTable = kRomBase + 0x780;
constexpr uint32_t kSetVoices = kRomBase + 0x7C0;
constexpr uint32_t kRequest = kRomBase + 0x7C4;
constexpr uint32_t kSetVolume2 = kRomBase + 0x7C8;
constexpr uint32_t kMuteRequest = kRomBase + 0x7D0;
constexpr uint32_t kYoshiMute = kRomBase + 0x700;
constexpr uint32_t kRequestQueue = 0x03000100; // the RAM word that holds the next request's address

// The test bank's instruments.
constexpr uint8_t kLooped = 0;      // a looped sample, with an envelope that rises and falls
constexpr uint8_t kPlain = 1;       // a sample without a loop, held at full level
constexpr uint8_t kSquare = 2;      // square 2 at a duty of 25%
constexpr uint8_t kDrums = 3;       // a drum kit from note 36: the plain sample at pan 0x20, square 2 at 0x40 and 0x50
constexpr uint8_t kKeyed = 4;       // a sample for each key
constexpr uint8_t kSplit = 5;       // the plain sample up to note 59, the looped one above
constexpr uint8_t kNoise = 6;       // noise
constexpr uint8_t kSquareSteps = 7; // square 2 with a table of duties: 12.5%, 25%, then 75%, a frame each
constexpr uint8_t kNoiseSteps = 8;  // noise with a table of widths: the 7-bit noise, then the 15-bit one
constexpr uint8_t kWave = 9;        // the wave voice: it leaves out its envelope, the one that rises and falls
constexpr uint8_t kHeld = 10; // the looped sample, held at full level, with its loop's rate at the mixer's at note 60

// A track's data being written, one command at a time.
class TrackData
{
public:
    TrackData& Bytes(std::vector<uint8_t> b)
    {
        bytes_.insert(bytes_.end(), b.begin(), b.end());

        return *this;
    }

    // Adds a note with a length and velocity of its own.
    TrackData& Note(uint8_t note, uint16_t length, uint8_t velocity)
    {
        bytes_.push_back(uint8_t(0x60 + note));
        if (length >= 0x80)
        {
            bytes_.push_back(uint8_t(0x80 | length >> 8));
        }
        bytes_.push_back(uint8_t(length));
        bytes_.push_back(velocity);

        return *this;
    }

    TrackData& Wait(uint16_t ticks)
    {
        bytes_.push_back(0xC1);
        if (ticks >= 0x80)
        {
            bytes_.push_back(uint8_t(0x80 | ticks >> 8));
        }
        bytes_.push_back(uint8_t(ticks));

        return *this;
    }

    std::vector<uint8_t> End()
    {
        bytes_.push_back(0xFF);

        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, in a revision's
// form, the driver's tables, and the game's settings, samples, bank and sequences.
class Cart : public ntdlib::testing::agb_files::CartImage
{
public:
    explicit Cart(bool code = true, Revision revision = Revision::kLinkToThePast) : CartImage(0x10000)
    {
        WriteTables();
        if (code)
        {
            WriteCode(revision);
        }
        WriteSettings();
        WriteSamples();
        WriteBank();
    }

    // Writes the sequences, each a list of tracks, with their tables of offsets. Each plays bank 0.
    void Sequences(const std::vector<std::vector<std::vector<uint8_t>>>& sequences)
    {
        uint32_t at = kSequenceData;
        const uint32_t lists = kBankLists + 4 * uint32_t(sequences.size());
        for (size_t s = 0; s < sequences.size(); s++)
        {
            Put32(kSequences + 4 * uint32_t(s), at - kSequences);
            Put32(kBankLists + 4 * uint32_t(s), lists - kBankLists);
            const auto& tracks = sequences[s];
            Put8(at, uint8_t(tracks.size()));
            uint32_t data = at + 2 + 2 * uint32_t(tracks.size());
            for (size_t t = 0; t < tracks.size(); t++)
            {
                Put16(at + 2 + 2 * uint32_t(t), uint16_t(data - at));
                for (uint8_t b : tracks[t])
                {
                    Put8(data++, b);
                }
            }
            at = (data + 3) & ~3u;
        }
        Put16(lists, 0);
        Put16(lists + 2, 0);
    }

    // Writes the sound effects' sequences. Each is a list of tracks. A sequence starts with a halfword for each track:
    // the track's offset from the sequence's start, or 0 for an empty track. The game's settings point to the
    // sequences. Each sequence plays bank 0.
    void Effects(const std::vector<std::vector<std::vector<uint8_t>>>& sequences)
    {
        Put32(kSettings + 12, kEffects);
        Put32(kSettings + 24, kEffectBankLists);
        uint32_t at = kEffectData;
        const uint32_t lists = kEffectBankLists + 4 * uint32_t(sequences.size());
        for (size_t s = 0; s < sequences.size(); s++)
        {
            Put32(kEffects + 4 * uint32_t(s), at - kEffects);
            Put32(kEffectBankLists + 4 * uint32_t(s), lists - kEffectBankLists);
            const auto& tracks = sequences[s];
            uint32_t data = at + 2 * uint32_t(tracks.size());
            for (size_t t = 0; t < tracks.size(); t++)
            {
                Put16(at + 2 * uint32_t(t), tracks[t].empty() ? 0 : uint16_t(data - at));
                for (uint8_t b : tracks[t])
                {
                    Put8(data++, b);
                }
            }
            at = (data + 3) & ~3u;
        }
        Put16(lists, 0);
    }

    // Writes a play-music routine in the form of Super Mario Advance 3's, from its lookup of the music's entry on, and
    // its table, a halfword for each music number. The routine gives the driver 6 sample voices for music whose entry
    // has bit 10 set, and 7 for the rest. Each of `volumes` is a music number and the player's second volume that the
    // routine sets for it.
    void PlayMusic(const std::vector<uint16_t>& table, const std::vector<std::pair<int, int>>& volumes)
    {
        std::string code =
            "4900 0060 1840 8801 7805 2080 00C0 4008 2800 D003 2006 0000 0000 E002 2007 0000 0000 1C29 "
            "2013 0000 0000";
        for (const auto& [music, volume] : volumes)
        {
            code += " " + xsflib::Hex(0x2C00u | music, 4, false) + " D103 2013 " +
                    xsflib::Hex(0x2100u | volume, 4, false) + " 0000 0000";
        }
        code += " 4770";
        Code(kPlayMusic, code.c_str(), {{0, kMusicTable}});
        PutBl(kPlayMusic + 22, kSetVoices);
        PutBl(kPlayMusic + 30, kSetVoices);
        PutBl(kPlayMusic + 38, kRequest);
        for (uint32_t i = 0; i < volumes.size(); i++)
        {
            PutBl(kPlayMusic + 50 + 12 * i, kSetVolume2);
        }

        for (uint32_t m = 0; m < table.size(); m++)
        {
            Put16(kMusicTable + 2 * m, table[m]);
        }
    }

    // Writes a play-music routine in the form of the one in Super Mario Advance 2, from its lookup of the music's
    // sequence on, its table, a halfword for each music number, and the driver's routine that mutes tracks. The routine
    // mutes tracks of sequence `sequence`: those in `music_mask` for music number `music`, and those in `mask` for the
    // other music numbers that play the sequence. Each mask is a value of up to 8 bits, shifted left by up to 31.
    void MutingPlayMusic(const std::vector<uint16_t>& table, int sequence, int music, std::pair<int, int> mask,
                         std::pair<int, int> music_mask)
    {
        const auto movs_r1 = [](int value)
        {
            return xsflib::Hex(0x2100u | unsigned(value), 4, false);
        };
        const auto lsls_r1 = [](int shift)
        {
            return xsflib::Hex(0x0009u | unsigned(shift) << 6, 4, false);
        };
        const std::string code = "4800 0069 1809 780C 2013 1C21 0000 0000 1C28 0000 0000 " +
                                 xsflib::Hex(0x2C00u | unsigned(sequence), 4, false) + " D109 " + movs_r1(mask.first) +
                                 " " + lsls_r1(mask.second) + " " + xsflib::Hex(0x2D00u | unsigned(music), 4, false) +
                                 " D101 " + movs_r1(music_mask.first) + " " + lsls_r1(music_mask.second) +
                                 " 2013 2201 0000 0000 4770";
        Code(kPlayMusic, code.c_str(), {{0, kMusicTable}});
        PutBl(kPlayMusic + 12, kRequest);
        PutBl(kPlayMusic + 18, kSetVoices);
        PutBl(kPlayMusic + 42, kMuteRequest);
        Code(kMuteRequest, "B510 0612 0E12 4B00 681C 2380 005B 8023 0400 4310 6060 60A1 BD10", {{3, kRequestQueue}});

        for (uint32_t m = 0; m < table.size(); m++)
        {
            Put16(kMusicTable + 2 * m, table[m]);
        }
    }

    // Writes Super Mario Advance 2's routine that mutes the tracks in `mask` of the music whose entry has bit 9 set
    // unless the player rides Yoshi. The routine reads the table and makes the request that MutingPlayMusic() writes.
    void YoshiMute(int mask)
    {
        const std::string code =
            "B510 0600 0E04 0000 0000 0400 1400 2800 DB10 4900 0040 1840 8801 2080 0080 4008 "
            "2800 D007 2200 2C00 D100 2201 2013 " +
            xsflib::Hex(0x2100u | unsigned(mask), 4, false) + " 0000 0000 BC10 BC01 4700";
        Code(kYoshiMute, code.c_str(), {{9, kMusicTable}});
        PutBl(kYoshiMute + 6, kSetVoices);
        PutBl(kYoshiMute + 48, kMuteRequest);
    }

private:
    // Writes the halfwords of `code` at `at`, each ldr of `loads` (its halfword and its literal) loading from a pool
    // after the code. Returns the next free word.
    uint32_t Code(uint32_t at, const char* code, const std::vector<std::pair<int, uint32_t>>& loads = {})
    {
        std::istringstream in(code);
        std::string word;
        uint32_t a = at;
        while (in >> word)
        {
            Put16(a, uint16_t(std::stoul(word, nullptr, 16)));
            a += 2;
        }

        uint32_t pool = (a + 3) & ~3u;
        for (const auto& [index, literal] : loads)
        {
            const uint32_t ins = at + 2 * uint32_t(index);
            const uint16_t op = uint16_t(Get8(ins + 1) << 8);
            Put16(ins, uint16_t(op | ((pool - ((ins + 4) & ~3u)) / 4)));
            Put32(pool, literal);
            pool += 4;
        }

        return pool;
    }

    // Writes the tables: each pitch index's sample pitch, 0x8000 at index 0x30, and square frequency setting, middle C
    // at index 0x30; the noise settings; a sine; the wave volumes; the voice classes; and the envelope of a sample for
    // each key.
    void WriteTables()
    {
        for (int i = 0; i <= 0x78; i++)
        {
            Put32(kPitchTable + 4 * uint32_t(i), uint32_t(std::lround(0x800 * std::pow(2.0, i / 12.0))));
            const double hz = 261.6255653005986 * std::pow(2.0, (i - 0x30) / 12.0);
            Put16(kFrequencyTable + 2 * uint32_t(i), uint16_t(std::lround(std::max(0.0, 2048 - 131072 / hz))));
            Put8(kNoiseTable + uint32_t(i), uint8_t(std::max(0, 0x77 - i)));
        }
        for (uint32_t i = 0; i < 256; i++)
        {
            Put8(kLfoTable + i, uint8_t(int8_t(std::lround(127 * std::sin(i * 6.283185307179586 / 256)))));
        }

        const uint8_t kWaves[5] = {0x00, 0x60, 0x40, 0x80, 0x20};
        for (uint32_t i = 0; i < 5; i++)
        {
            Put8(kWaveVolumes + i, kWaves[i]);
            Put8(kVoiceClasses + i, uint8_t(i));
        }
        Put32(kKeyEnvelope, 0x7FFF0001);
        Put32(kKeyEnvelope + 4, 0x0000FFFF);
    }

    // Writes the driver's code that detection reads, as the revision's compiler built it, and the game's call to the
    // driver's init.
    void WriteCode(Revision revision)
    {
        Code(kInit, "B530 4900 6008 4900 2000 7008 2080 7008 3904 4A00 1C10 8008 3102 200D 7008 BD30");
        Put16(kGameInit, 0x4801);
        PutBl(kGameInit + 2, kInit);
        Put16(kGameInit + 6, 0x4770);
        Put32(kGameInit + 8, kSettings);

        uint32_t at = kCode;
        if (revision == Revision::kSuperMarioAdvance2)
        {
            at = Code(at,
                      "0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 2800 "
                      "D108 4800 1104 8913 0918 0868 E00E 46C0 46C0 46C0 2804 D007 4800 4770",
                      {{19, kPitchTable}, {30, kFrequencyTable}});
            at = Code(at, "0400 0C01 2977 D900 2177 4800 1808 7800 4770", {{5, kNoiseTable}});
            at = Code(at, "68A0 6882 2A00 D000 6860 2800 D100 4800 6A29 0849 1809 7809 4770", {{7, kLfoTable}});
            at = Code(at, "4900 7830 1840 7800 1C29 3152 780A 1C29 4770", {{0, kVoiceClasses}});
            at = Code(at, "B530 1C05 7868 2801 D11A 7A6C 0224 4770");
        }
        else
        {
            at = Code(at,
                      "B500 0609 0E09 0612 0E12 3130 1A89 0409 0C0A 1409 2900 DA01 2200 E002 2977 DD00 2278 7800 "
                      "2800 D107 4800 1104 8913 0918 0868 E00D 46C0 46C0 2804 D007 4800 4770",
                      {{20, kPitchTable}, {30, kFrequencyTable}});
            at = Code(at, "B500 0400 0C01 2977 D900 2177 4800 1808 7800 4770", {{6, kNoiseTable}});
            at = Code(at, "68A0 6883 2B00 D000 6860 2800 D100 4800 6A29 0849 1809 7809 4770", {{7, kLfoTable}});
            at = Code(at, "4900 7830 1840 7800 1C29 3152 7809 4770", {{0, kVoiceClasses}});
        }
        at = Code(at, "2904 D900 2104 0609 0E09 4A00 4800 1809 7808 7010 4770", {{6, kWaveVolumes}});
        Code(at, "8869 1859 4A00 0070 1840 8800 8050 6022 4800 6060 4770", {{8, kKeyEnvelope}});
    }

    // Writes the settings: one sample set, one bank that plays it, and the sequences and their lists of banks.
    void WriteSettings()
    {
        const uint32_t kTables[7] = {kSampleSets, kBanks, kSequences, 0, kBankSampleSets, kBankLists, 0};
        for (uint32_t i = 0; i < 7; i++)
        {
            Put32(kSettings + 4 * i, kTables[i]);
        }
        Put32(kSampleSets, kSamples - kSampleSets);
        Put32(kBanks, kBank - kBanks);
        Put16(kBankSampleSets, 0);
    }

    // Writes the samples: a looped saw at 10512 Hz, and a square wave without a loop at 21024 Hz.
    void WriteSamples()
    {
        const uint32_t kFirst = kSamples + 8;
        Put32(kSamples, kFirst - kSamples);
        Put32(kFirst, 64);
        Put32(kFirst + 4, 10512);
        Put32(kFirst + 8, 16);
        Put32(kFirst + 12, 64);
        for (uint32_t i = 0; i < 64; i++)
        {
            Put8(kFirst + 16 + i, uint8_t(int8_t(i * 4 - 128)));
        }

        const uint32_t kSecond = kFirst + 16 + 64;
        Put32(kSamples + 4, kSecond - kSamples);
        Put32(kSecond, 4000);
        Put32(kSecond + 4, 21024);
        Put32(kSecond + 8, 0);
        Put32(kSecond + 12, 0);
        for (uint32_t i = 0; i < 4000; i++)
        {
            Put8(kSecond + 16 + i, uint8_t(i % 20 < 10 ? 100 : -100));
        }
    }

    // Writes the bank: the instruments' offsets, then the instruments, their envelopes and tables, all from the bank's
    // start.
    void WriteBank()
    {
        uint32_t at = 0x40;
        auto add = [&](std::vector<uint8_t> bytes)
        {
            const uint32_t here = at;
            for (uint8_t b : bytes)
            {
                Put8(kBank + at++, b);
            }
            at = (at + 3) & ~3u;

            return uint16_t(here);
        };

        auto region = [](uint8_t type, uint8_t flags, uint16_t arg, uint16_t envelope, uint8_t release, uint8_t tuning)
        {
            return std::vector<uint8_t>{
                type,   flags, uint8_t(arg), uint8_t(arg >> 8), uint8_t(envelope), uint8_t(envelope >> 8), release,
                tuning, 8};
        };

        // Envelopes: up in 4 frames and down to half in 8, and up at once and held.
        const uint16_t rise = add({4, 0, 0xFF, 0x7F, 8, 0, 0x80, 0x3F, 0xFF, 0xFF, 0, 0});
        const uint16_t held = add({1, 0, 0xFF, 0x7F, 0xFF, 0xFF, 0, 0});

        Put16(kBank + 2 * kLooped, add(region(0, 0, 0, rise, 0x46, 60)));
        Put16(kBank + 2 * kPlain, add(region(0, 0, 1, held, 0x00, 48)));
        const uint16_t square = add(region(2, 0, 1, held, 0x40, 0x30));
        Put16(kBank + 2 * kSquare, square);
        const uint16_t plain = add(region(0, 0, 1, held, 0x00, 0x30));
        // Square 2 a semitone higher. Two PSG drums with the same tuning share a key and its zone.
        const uint16_t higher = add(region(2, 0, 1, held, 0x40, 0x2F));
        const uint16_t kit = add({uint8_t(plain), uint8_t(plain >> 8), 0x20, 0, uint8_t(square), uint8_t(square >> 8),
                                  0x40, 0, uint8_t(higher), uint8_t(higher >> 8), 0x50, 0});
        Put16(kBank + 2 * kDrums, add({0x10, 0, uint8_t(kit), uint8_t(kit >> 8), 36, 0, 0, 0}));
        std::vector<uint8_t> keys;
        for (int k = 0; k < 128; k++)
        {
            keys.push_back(uint8_t(k & 1));
            keys.push_back(0);
        }
        const uint16_t key_table = add(keys);
        Put16(kBank + 2 * kKeyed, add({0x11, 0, uint8_t(key_table), uint8_t(key_table >> 8), 0, 0, 0, 0}));
        const uint16_t looped = add(region(0, 0, 0, rise, 0x46, 60));
        const uint16_t low = add(region(0, 0, 1, held, 0x00, 48));
        const uint16_t split =
            add({59, 0, uint8_t(low), uint8_t(low >> 8), 255, 0, uint8_t(looped), uint8_t(looped >> 8)});
        Put16(kBank + 2 * kSplit, add({0x12, 0, uint8_t(split), uint8_t(split >> 8), 0, 0, 0, 0}));
        Put16(kBank + 2 * kNoise, add(region(4, 0, 0, held, 0x20, 0x30)));
        const uint16_t duties = add({3, 0, 0, 1, 3});
        Put16(kBank + 2 * kSquareSteps, add(region(2, 1, duties, held, 0x40, 0x30)));
        const uint16_t widths = add({2, 0, 1, 0});
        Put16(kBank + 2 * kNoiseSteps, add(region(4, 1, widths, held, 0x20, 0x30)));
        const uint16_t wave =
            add({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10});
        Put16(kBank + 2 * kWave, add(region(3, 0, wave, rise, 0x46, 0x30)));
        Put16(kBank + 2 * kHeld, add(region(0, 0, 0, held, 0x00, 60)));
    }
};

} // namespace ntdlib::testing::rd2_cart
