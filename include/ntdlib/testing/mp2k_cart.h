// SPDX-License-Identifier: MIT

// Builds small MP2K cartridges for tests: a synthetic copy of the code patterns that the driver's detection reads,
// with songs, voice groups and samples placed one after another. A Track holds a track's commands, and a Cart places
// them as songs.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/testing/agb_cart.h"

namespace ntdlib::testing::mp2k_cart
{
using ntdlib::agb::kRomBase;
using ntdlib::mp2k::ClockLength;
using ntdlib::mp2k::kVoiceKeySplit;
using enum ntdlib::mp2k::Command;

// Addresses of the synthetic driver's parts in a test cartridge.
inline constexpr uint32_t kInit = kRomBase + 0x100;          // the init's setting of the driver's mode
inline constexpr uint32_t kInitPool = kRomBase + 0x120;      // its literal pool
inline constexpr uint32_t kSongStart = kRomBase + 0x200;     // the routine that starts a song
inline constexpr uint32_t kSongStartPool = kRomBase + 0x228; // its literal pool
inline constexpr uint32_t kSpecialTest = kRomBase + 0x300;   // the mixer's test for reversed and compressed samples
inline constexpr uint32_t kMonoVolume = kRomBase + 0x340;    // the mono mixer's volume code, where a test puts it
inline constexpr uint32_t kPlayerTable = kRomBase + 0x400;

// Addresses of Camelot's changes in a test cartridge that has them.
inline constexpr uint32_t kUnusedSongStart =
    kRomBase + 0x180; // a copy of the routine that starts a song, its pool zeroed
inline constexpr uint32_t kCamelotSearch = kRomBase + 0x360;    // the note start's search for a channel
inline constexpr uint32_t kCamelotEchoWords = kRomBase + 0x370; // the literals of the output stage's echo
inline constexpr uint32_t kCamelotPulse = kRomBase + 0x380;     // the mixer's pulse wave
inline constexpr uint32_t kLaterCamelot = kRomBase + 0x3A0;     // the code of a later version of Camelot's driver

// Addresses of the Metroid games' changes in a test cartridge that has them.
inline constexpr uint32_t kMetroidTempo = kRomBase + 0x3D0;  // the sequencer's TEMPO handler
inline constexpr uint32_t kMetroidTables = kRomBase + 0x3E0; // the loads of the player and song tables, and their pool
inline constexpr uint32_t kSongTable = kRomBase + 0x500;
inline constexpr uint32_t kData = kRomBase + 0x1000; // storage for songs, voices and samples

// The driver's mode that the test cartridges' init sets: 13379 Hz, 5 DirectSound channels, master volume 12.
inline constexpr uint32_t kMode = 0x0094C500;

// The mode of a test cartridge with Camelot's mixer: 31536 Hz, 5 DirectSound channels, master volume 12. The mixer
// mixes 528 points a frame at that rate.
inline constexpr uint32_t kCamelotMode = 0x0099C500;

// The mode of a test cartridge with the mixer of a later version of Camelot's driver: 15768 Hz and 264 points a frame.
inline constexpr uint32_t kLaterCamelotMode = 0x0095C500;

// The routine that starts a song, with its call to MPlayStart, whose literal pool is 0x28 bytes after its start.
inline constexpr uint16_t kSongStartCode[18] = {0xB500, 0x0400, 0x4A08, 0x4909, 0x0B40, 0x1840, 0x8883, 0x0059, 0x18C9,
                                                0x0089, 0x1889, 0x680A, 0x6801, 0x1C10, 0xF000, 0xF800, 0xBC01, 0x4700};

// Returns the index of `ticks` in the driver's table of note lengths and waits, or -1 if it isn't one of them.
inline int LengthIndex(int ticks)
{
    for (int i = 0; i < 49; i++)
    {
        if (ClockLength(i) == ticks)
        {
            return i;
        }
    }

    return -1;
}

// Appends `v` to `bytes` in little-endian byte order.
inline void AppendLe32(std::vector<uint8_t>& bytes, uint32_t v)
{
    bytes.resize(bytes.size() + 4);
    xsflib::StoreLe32(&bytes[bytes.size() - 4], v);
}

// A track's commands, with jumps to labels in the same track, which Bytes() turns into addresses.
class Track
{
public:
    // Returns the position of the next command, for jumps to it.
    size_t Here() const
    {
        return bytes_.size();
    }

    // Adds waits that add up to `ticks`, each the longest of the driver's lengths that fits.
    Track& Wait(int ticks)
    {
        while (ticks > 0)
        {
            int step = std::min(ticks, 96);
            int index = LengthIndex(step);
            while (index < 0)
            {
                index = LengthIndex(--step);
            }

            bytes_.push_back(uint8_t(kCmdWait + index));
            ticks -= ClockLength(index);
        }

        return *this;
    }

    // Adds a note of `ticks`, which has to be one of the driver's lengths, with the arguments that aren't -1.
    Track& Note(int ticks, int key = -1, int velocity = -1, int extra = -1)
    {
        const int index = LengthIndex(ticks);
        if (index <= 0)
        {
            throw std::invalid_argument("a note of a length that isn't one of the driver's");
        }
        bytes_.push_back(uint8_t(kCmdTie + std::max(index, 1)));
        for (int arg : {key, velocity, extra})
        {
            if (arg >= 0)
            {
                bytes_.push_back(uint8_t(arg));
            }
        }

        return *this;
    }

    // The commands below add their bytes one at a time, since GCC wrongly warns about a null memmove when a short list
    // is inserted at -O3.
    Track& Tie(int key, int velocity)
    {
        bytes_.push_back(kCmdTie);
        bytes_.push_back(uint8_t(key));
        bytes_.push_back(uint8_t(velocity));

        return *this;
    }

    Track& Eot(int key)
    {
        bytes_.push_back(kCmdEot);
        bytes_.push_back(uint8_t(key));

        return *this;
    }

    // Adds a command with one argument.
    Track& Cmd(uint8_t command, int arg)
    {
        bytes_.push_back(command);
        bytes_.push_back(uint8_t(arg));

        return *this;
    }

    Track& Raw(std::vector<uint8_t> bytes)
    {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());

        return *this;
    }

    Track& Goto(size_t label)
    {
        return Jump(kCmdGoto, {}, label);
    }

    Track& Patt(size_t label)
    {
        return Jump(kCmdPatt, {}, label);
    }

    Track& Rept(int count, size_t label)
    {
        return Jump(kCmdRept, {uint8_t(count)}, label);
    }

    Track& Fine()
    {
        bytes_.push_back(kCmdFine);

        return *this;
    }

    // Adds a memacc command whose operation `op` jumps to `label` (6 to 17).
    Track& MemAccJump(int op, int byte, int value, size_t label)
    {
        return Jump(kCmdMemAcc, {uint8_t(op), uint8_t(byte), uint8_t(value)}, label);
    }

    // Returns the track's bytes for placing them at `at`.
    std::vector<uint8_t> Bytes(uint32_t at) const
    {
        std::vector<uint8_t> out = bytes_;
        for (const auto& [where, label] : fixups_)
        {
            xsflib::StoreLe32(&out[where], at + uint32_t(label));
        }

        return out;
    }

private:
    Track& Jump(uint8_t command, std::vector<uint8_t> args, size_t label)
    {
        bytes_.push_back(command);
        bytes_.insert(bytes_.end(), args.begin(), args.end());
        fixups_.push_back({bytes_.size(), label});
        bytes_.insert(bytes_.end(), 4, 0);

        return *this;
    }

    std::vector<uint8_t> bytes_;
    std::vector<std::pair<size_t, size_t>> fixups_;
};

// A cartridge image being put together: a synthetic copy of the code patterns that detection reads, and songs, voices
// and samples placed one after another.
class Cart : public agb_files::CartImage
{
public:
    // A mode that a game's code puts in r0 with `movs r0, #value; lsls r0, r0, #shift`.
    struct ShiftedMode
    {
        uint8_t value;
        int shift;
    };

    // Makes a cartridge whose init gives the driver `mode`, with music players of the given track counts. Without
    // `code`, it has neither the init nor the routine that starts a song, and without `special`, its mixer doesn't play
    // reversed or compressed samples.
    explicit Cart(uint32_t mode = kMode, std::vector<int> players = {10, 3}, bool code = true, bool special = true)
        : CartImage(0x40000), next_(kData)
    {
        for (size_t p = 0; p < players.size(); p++)
        {
            const uint32_t at = kPlayerTable + 12 * uint32_t(p);
            Put32(at, 0x03007420 + 0x40 * uint32_t(p));
            Put32(at + 4, 0x03001340 + 0x500 * uint32_t(p));
            Put8(at + 8, uint8_t(players[p]));
        }
        if (code)
        {
            WriteDriver(mode, special);
        }
    }

    // Places `bytes` at the next free address, aligned to `align`. Returns the address.
    uint32_t Place(const std::vector<uint8_t>& bytes, uint32_t align = 4)
    {
        next_ = (next_ + align - 1) & ~(align - 1);
        const uint32_t at = next_;
        PutBytes(at, bytes);
        next_ += uint32_t(bytes.size());

        return at;
    }

    // Places a sample that plays at `rate` for key 60, looping from `loop_start` to its end if that isn't -1.
    uint32_t Wave(const std::vector<int8_t>& pcm, uint32_t rate = 13379, int loop_start = -1)
    {
        std::vector<uint8_t> bytes;
        AppendLe32(bytes, loop_start >= 0 ? 0x40000000u : 0);
        AppendLe32(bytes, rate * 1024);
        AppendLe32(bytes, loop_start >= 0 ? uint32_t(loop_start) : 0);
        AppendLe32(bytes, uint32_t(pcm.size()));
        bytes.insert(bytes.end(), pcm.begin(), pcm.end());

        return Place(bytes);
    }

    // Places a synth voice's sample for Camelot's mixer: a looping sample of no length, whose data is 0x80, the type,
    // and `params`. It plays at 13379 Hz for key 60.
    uint32_t Synth(uint8_t type, const std::vector<uint8_t>& params)
    {
        std::vector<uint8_t> bytes = {0, 0, 0, 0x40};
        AppendLe32(bytes, 13379 * 1024);
        bytes.resize(16, 0);
        bytes.push_back(0x80);
        bytes.push_back(type);
        bytes.insert(bytes.end(), params.begin(), params.end());

        return Place(bytes);
    }

    // Writes Camelot's changes: an unused copy of the routine that starts a song before the one the game uses, with its
    // literal pool zeroed, the note start's search for a channel, and the mixer's pulse wave.
    void WriteCamelot()
    {
        for (uint32_t i = 0; i < 18; i++)
        {
            Put16(kUnusedSongStart + 2 * i, kSongStartCode[i]);
        }

        const uint8_t kSearch[16] = {0x0A, 0x23, 0x50, 0x34, 0xC7, 0x27, 0x00, 0x22,
                                     0x00, 0x26, 0x03, 0x20, 0x21, 0x78, 0x39, 0x42};
        const uint8_t kPulse[20] = {0x02, 0x60, 0xD3, 0xE5, 0x06, 0x2C, 0x82, 0xE0, 0x04, 0x60,
                                    0xD3, 0xE5, 0x06, 0x6C, 0x92, 0xE0, 0x06, 0x60, 0xE0, 0x41};
        for (uint32_t i = 0; i < 16; i++)
        {
            Put8(kCamelotSearch + i, kSearch[i]);
        }
        for (uint32_t i = 0; i < 20; i++)
        {
            Put8(kCamelotPulse + i, kPulse[i]);
        }
    }

    // Writes what a later version of Camelot's driver changes, after WriteCamelot(): the output stage's one point for
    // each of the mixer's, the literals of its echo, SoundMain's count of 12 channels, the mixer's path for samples of
    // 4-bit steps, and the priority of the note start.
    void WriteLaterCamelot(uint32_t tap, int echo, int cross_echo)
    {
        Put32(kCamelotEchoWords, tap);
        Put32(kCamelotEchoWords + 8, uint32_t(cross_echo) << 16 | uint32_t(echo));
        Put32(kCamelotEchoWords + 12, (uint32_t(echo) << 16) + uint32_t(cross_echo));

        const uint8_t kCode[48] = {0xFF, 0x84, 0xA0, 0xE3, 0x28, 0x88, 0x88, 0xE1, 0x88, 0xE3, 0x08, 0xE0,
                                   0x63, 0x3E, 0xA0, 0xE3, 0x20, 0x00, 0x10, 0xE3, 0x1C, 0x70, 0x94, 0x15,
                                   0x0C, 0x20, 0x50, 0x34, 0x00, 0x4B, 0x18, 0x47, 0x71, 0x7A, 0x78, 0x29,
                                   0x01, 0xD1, 0xFF, 0x20, 0x04, 0xE0, 0x68, 0x7F, 0x40, 0x18, 0xFE, 0x28};
        for (uint32_t i = 0; i < 48; i++)
        {
            Put8(kLaterCamelot + i, kCode[i]);
        }
    }

    // Writes the Metroid games' changes: the sequencer's TEMPO handler, the loads of the player and song tables that a
    // game without the routine that starts a song has (Metroid: Zero Mission), and the routine that reads the table of
    // PSG frequency settings, with that table: 129 settings, `step` apart. With `short_notes`, it also writes
    // Metroid Fusion's read of a note's arguments. That read takes no third byte. Returns the table's address.
    uint32_t WriteMetroid(uint16_t step, bool short_notes = false)
    {
        if (short_notes)
        {
            const std::vector<uint8_t> kShortNote = {0x4A, 0x78, 0x01, 0x20, 0x08, 0x56, 0x00, 0x28, 0x02,
                                                     0xDB, 0xE2, 0x73, 0x58, 0x1C, 0x60, 0x62, 0x21, 0x78};
            Place(kShortNote, 2);
        }

        const uint8_t kTempo[14] = {0x4A, 0x6A, 0x53, 0x78, 0x02, 0x32, 0x4A, 0x62, 0x5A, 0x00, 0x42, 0x81, 0x96, 0x2A};
        for (uint32_t i = 0; i < 14; i++)
        {
            Put8(kMetroidTempo + i, kTempo[i]);
        }

        // ldr r3, =players; ldr r0, =songs; lsls r2, r4, #3; bx lr
        const uint16_t kTables[4] = {0x4B01, 0x4802, 0x00E2, 0x4770};
        for (uint32_t i = 0; i < 4; i++)
        {
            Put16(kMetroidTables + 2 * i, kTables[i]);
        }
        Put32(kMetroidTables + 8, kPlayerTable);
        Put32(kMetroidTables + 12, kSongTable);

        std::vector<uint8_t> table;
        for (uint16_t k = 0; k < 129; k++)
        {
            table.push_back(uint8_t(k * step));
            table.push_back(uint8_t((k * step) >> 8));
        }
        const uint32_t table_at = Place(table);

        // The test of the voice's PSG channel against 3, and the load of the table, indexed by the key times 2.
        std::vector<uint8_t> code;
        for (const int h : {0x3034, 0x7801, 0x2007, 0x4008, 0x2803, 0xD800, 0x4901, 0x0060, 0x1840, 0x8803})
        {
            code.push_back(uint8_t(h));
            code.push_back(uint8_t(h >> 8));
        }
        AppendLe32(code, table_at);
        Place(code);

        return table_at;
    }

    // Places a sample of `points` points of 4-bit steps, for a later version of Camelot's mixer: its size is negative.
    uint32_t StepWave(const std::vector<uint8_t>& steps, uint32_t points)
    {
        std::vector<uint8_t> bytes;
        AppendLe32(bytes, 0);
        AppendLe32(bytes, 13379 * 1024);
        AppendLe32(bytes, 0);
        AppendLe32(bytes, 0u - points);
        bytes.insert(bytes.end(), steps.begin(), steps.end());

        return Place(bytes);
    }

    // Places a compressed sample of `points` points from its blocks of 33 bytes, which plays at 13379 Hz for key 60.
    uint32_t CompressedWave(const std::vector<uint8_t>& blocks, uint32_t points)
    {
        std::vector<uint8_t> bytes(16, 0);
        bytes[0] = 1;
        xsflib::StoreLe32(&bytes[4], 13379 * 1024);
        xsflib::StoreLe32(&bytes[12], points);
        bytes.insert(bytes.end(), blocks.begin(), blocks.end());

        return Place(bytes);
    }

    // Returns a voice's 12 bytes. `length` is a PSG voice's sound length.
    static std::vector<uint8_t> Voice(uint8_t type, uint32_t wave, uint8_t key = 60,
                                      std::vector<uint8_t> envelope = {255, 0, 255, 0}, uint8_t pan_sweep = 0,
                                      uint8_t length = 0)
    {
        return {type,
                key,
                length,
                pan_sweep,
                uint8_t(wave),
                uint8_t(wave >> 8),
                uint8_t(wave >> 16),
                uint8_t(wave >> 24),
                envelope[0],
                envelope[1],
                envelope[2],
                envelope[3]};
    }

    // Places a voice group of 128 voices, with the given ones and the rest zeroed. Returns its address.
    uint32_t VoiceGroup(const std::map<int, std::vector<uint8_t>>& voices)
    {
        std::vector<uint8_t> bytes(128 * 12, 0);
        for (const auto& [index, voice] : voices)
        {
            std::copy(voice.begin(), voice.end(), bytes.begin() + 12 * index);
        }

        return Place(bytes);
    }

    // Returns a key split's voice: keys up to `split` play voice 0 of `table`, and the others voice 1.
    std::vector<uint8_t> KeySplit(uint32_t table, int split)
    {
        std::vector<uint8_t> keys(128, 1);
        std::fill(keys.begin(), keys.begin() + split + 1, uint8_t(0));
        const uint32_t key_table = Place(keys, 1);
        std::vector<uint8_t> voice = Voice(kVoiceKeySplit, table);
        xsflib::StoreLe32(&voice[8], key_table);

        return voice;
    }

    // Places a song with these tracks, and adds it to the song table. Returns its header's address.
    uint32_t Song(const std::vector<Track>& tracks, uint32_t voices, int player = 0, uint8_t priority = 0,
                  uint8_t reverb = 0)
    {
        // Place the tracks, and list the header's words: the voice group, then each track's address.
        std::vector<uint32_t> words = {voices};
        for (const Track& t : tracks)
        {
            next_ = (next_ + 3) & ~3u;
            words.push_back(Place(t.Bytes(next_), 1));
        }

        std::vector<uint8_t> header = {uint8_t(tracks.size()), 0, priority, reverb};
        for (uint32_t v : words)
        {
            AppendLe32(header, v);
        }

        const uint32_t header_at = Place(header);
        Put32(kSongTable + 8 * songs_, header_at);
        Put16(kSongTable + 8 * songs_ + 4, uint16_t(player));
        Put16(kSongTable + 8 * songs_ + 6, uint16_t(player));
        songs_++;

        return header_at;
    }

    // Writes `code` as the header's game code, which a game's rows in the driver's detection go by.
    void GameCode(std::string_view code)
    {
        PutBytes(kRomBase + 0xAC, std::vector<uint8_t>(code.begin(), code.end()));
    }

    // Writes a game's call of m4aSoundMode with `mode`: straight after its call of m4aSoundInit, or after another
    // routine's.
    void GameSoundMode(uint32_t mode, bool after_init)
    {
        constexpr uint32_t kGameCall = kRomBase + 0x140;
        constexpr uint32_t kGamePool = kRomBase + 0x14C;
        PutBl(kGameCall, after_init ? kInit : kRomBase + 0x1F0);
        Put16(kGameCall + 4, uint16_t(0x4800 | ((kGamePool - ((kGameCall + 8) & ~3u)) / 4)));
        PutBl(kGameCall + 6, kInit + 6);
        Put32(kGamePool, mode);
    }

    // Writes a game's calls of m4aSoundMode straight after its call of m4aSoundInit, each with a mode from movs and
    // lsls. Before them, the game calls `other_calls` other routines.
    void GameShiftedSoundModes(const std::vector<ShiftedMode>& modes, int other_calls)
    {
        uint32_t at = kRomBase + 0x160;
        PutBl(at, kInit);
        at += 4;

        for (int i = 0; i < other_calls; i++)
        {
            PutBl(at, kRomBase + 0x1F0);
            at += 4;
        }

        for (const ShiftedMode& m : modes)
        {
            Put16(at, uint16_t(0x2000 | m.value));
            Put16(at + 2, uint16_t(m.shift << 6));
            PutBl(at + 4, kInit + 6);
            at += 8;
        }
    }

private:
    // Writes the parts of the driver that detection reads: the init's setting of the mode, the routine that starts a
    // song with its literal pool, and the mixer's test for reversed and compressed samples.
    void WriteDriver(uint32_t mode, bool special)
    {
        // ldr r0, =mode; bl m4aSoundMode, with the music player table in the same literal pool.
        Put16(kInit, uint16_t(0x4800 | ((kInitPool - ((kInit + 4) & ~3u)) / 4)));
        Put16(kInit + 2, 0xF000);
        Put16(kInit + 4, 0xF800);
        Put32(kInitPool, mode);
        Put32(kInitPool + 4, kPlayerTable);

        for (uint32_t i = 0; i < 18; i++)
        {
            Put16(kSongStart + 2 * i, kSongStartCode[i]);
        }
        Put32(kSongStartPool, kPlayerTable);
        Put32(kSongStartPool + 4, kSongTable);

        if (special)
        {
            const uint8_t kTest[] = {0x01, 0x00, 0xD4, 0xE5, 0x30, 0x00, 0x10, 0xE3};
            for (uint32_t i = 0; i < 8; i++)
            {
                Put8(kSpecialTest + i, kTest[i]);
            }
        }
    }

    uint32_t next_;
    uint32_t songs_ = 0;
};

// Returns a sample of a saw wave, `length` points long.
inline std::vector<int8_t> Saw(size_t length = 64)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 4 % 256) - 128));
    }

    return pcm;
}

} // namespace ntdlib::testing::mp2k_cart
