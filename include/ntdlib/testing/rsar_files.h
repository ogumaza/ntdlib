// SPDX-License-Identifier: MIT

// Builders of small NW4R sound files for the tests: banks of both generations with instruments that exercise every way
// that a bank selects a region, wave data and an RWAR for them, a sequence, wave sound files of both generations, an
// archive holding a sequence sound with its sequence and bank in a group, an archive holding wave sounds with their
// file in a group, and an archive with only its tables of sounds and players.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

namespace ntdlib::testing::rsar_files
{

using Bytes = std::vector<uint8_t>;

// Appends `count` zero bytes and returns their offset.
inline size_t Grow(Bytes& file, size_t count)
{
    const size_t at = file.size();
    file.resize(at + count, 0);

    return at;
}

inline void PutRef(Bytes& file, size_t at, uint8_t data_type, uint32_t value)
{
    file[at] = 1;
    file[at + 1] = data_type;
    xsflib::StoreBe32(&file[at + 4], value);
}

// A file header of `size` bytes: the magic, the byte order mark, the version, the header's size, the number of blocks
// and the blocks' offsets and sizes (filled in later). An archive's header takes 0x40 bytes.
inline Bytes Header(const char* magic, uint16_t version, uint16_t blocks, uint16_t size = 0x20)
{
    Bytes file(size, 0);
    std::memcpy(file.data(), magic, 4);
    xsflib::StoreBe16(&file[4], 0xFEFF);
    xsflib::StoreBe16(&file[6], version);
    xsflib::StoreBe16(&file[0xC], size);
    xsflib::StoreBe16(&file[0xE], blocks);

    return file;
}

// Starts a block and records it in the header's slot.
inline size_t Block(Bytes& file, const char* magic, int slot)
{
    const size_t at = Grow(file, 8);
    std::memcpy(&file[at], magic, 4);
    xsflib::StoreBe32(&file[0x10 + slot * 8], static_cast<uint32_t>(at));

    return at;
}

inline void EndBlock(Bytes& file, size_t block, int slot)
{
    const uint32_t size = static_cast<uint32_t>(file.size() - block);
    xsflib::StoreBe32(&file[block + 4], size);
    xsflib::StoreBe32(&file[0x14 + slot * 8], size);
    xsflib::StoreBe32(&file[8], static_cast<uint32_t>(file.size()));
}

// A region (InstParam) playing `wave` at the original key `key`. Returns the region's offset from `base`.
inline size_t PutRegion(Bytes& file, size_t base, int32_t wave, uint8_t key)
{
    const size_t at = Grow(file, 0x30);
    xsflib::StoreBe32(&file[at], static_cast<uint32_t>(wave));
    file[at + 4] = 127;
    file[at + 0xC] = key;
    file[at + 0xD] = 100;
    file[at + 0xE] = 64;
    xsflib::StoreBe32(&file[at + 0x10], 0x3F800000);

    return at - base;
}

// A wave info for one ADPCM channel whose samples start at `data_offset` in the wave data, at offset `at` in `file`.
// The wave plays at `sample_rate` Hz.
inline void PutWaveInfo(Bytes& file, size_t at, uint32_t data_offset, uint16_t pred_scale, bool loop = true,
                        uint16_t sample_rate = 32000)
{
    file[at] = 2;
    file[at + 1] = loop ? 1 : 0;
    file[at + 2] = 1;
    xsflib::StoreBe16(&file[at + 4], sample_rate);
    xsflib::StoreBe32(&file[at + 8], 2);
    xsflib::StoreBe32(&file[at + 0xC], 30);
    xsflib::StoreBe32(&file[at + 0x10], 0x1C);
    xsflib::StoreBe32(&file[at + 0x14], data_offset);
    xsflib::StoreBe32(&file[at + 0x1C], 0x20);
    xsflib::StoreBe32(&file[at + 0x24], 0x3C);
    xsflib::StoreBe32(&file[at + 0x28], 0x01000000);
    xsflib::StoreBe16(&file[at + 0x3C], 0x0400);
    xsflib::StoreBe16(&file[at + 0x3C + 0x22], pred_scale);
}

// A bank's DATA block with these instruments:
//   0: a region (wave 0, key 60)
//   1: a range table by key: up to 63 plays wave 0 (key 40), up to 127 wave 1 (key 80)
//   2: an index table for keys 60 to 62, each a range table by velocity: up to 63 wave 0, up to 127 wave 1
//   3: nothing (type 0)
//   4: an unknown type (4) that plays nothing
//   5: a region with wave index -1 that plays nothing
//   6: a region (wave 0, key 60) whose note-off type ignores note-offs
//   7: a region (wave 0, key 60) in key group 1
//   8 to 69: all the same range table as instrument 1
inline void PutInstruments(Bytes& file)
{
    const size_t data = Block(file, "DATA", 0);
    const size_t base = file.size();
    xsflib::StoreBe32(&file[Grow(file, 4)], 70);
    const size_t refs = Grow(file, 70 * 8);

    PutRef(file, refs, 1, static_cast<uint32_t>(PutRegion(file, base, 0, 60)));

    const size_t range = Grow(file, 4 + 16);
    file[range] = 2;
    file[range + 1] = 63;
    file[range + 2] = 127;
    PutRef(file, range + 4, 1, static_cast<uint32_t>(PutRegion(file, base, 0, 40)));
    PutRef(file, range + 12, 1, static_cast<uint32_t>(PutRegion(file, base, 1, 80)));
    PutRef(file, refs + 8, 2, static_cast<uint32_t>(range - base));

    const size_t velocity = Grow(file, 4 + 16);
    file[velocity] = 2;
    file[velocity + 1] = 63;
    file[velocity + 2] = 127;
    PutRef(file, velocity + 4, 1, static_cast<uint32_t>(PutRegion(file, base, 0, 60)));
    PutRef(file, velocity + 12, 1, static_cast<uint32_t>(PutRegion(file, base, 1, 60)));
    const size_t index = Grow(file, 4 + 24);
    file[index] = 60;
    file[index + 1] = 62;
    for (int i = 0; i < 3; ++i)
    {
        PutRef(file, index + 4 + i * 8, 2, static_cast<uint32_t>(velocity - base));
    }
    PutRef(file, refs + 16, 3, static_cast<uint32_t>(index - base));

    PutRef(file, refs + 32, 4, static_cast<uint32_t>(range - base));
    PutRef(file, refs + 40, 1, static_cast<uint32_t>(PutRegion(file, base, -1, 60)));
    const size_t ignores = PutRegion(file, base, 0, 60);
    file[base + ignores + 0xA] = 1;
    PutRef(file, refs + 48, 1, static_cast<uint32_t>(ignores));
    const size_t grouped = PutRegion(file, base, 0, 60);
    file[base + grouped + 0xB] = 1;
    PutRef(file, refs + 56, 1, static_cast<uint32_t>(grouped));
    for (int i = 8; i < 70; ++i)
    {
        PutRef(file, refs + i * 8, 2, static_cast<uint32_t>(range - base));
    }

    EndBlock(file, data, 0);
}

// A version 1.1 bank. Its WAVE block describes two waves in its raw wave data.
inline Bytes MakeOldBank()
{
    Bytes file = Header("RBNK", 0x0101, 2);
    PutInstruments(file);
    const size_t wave = Block(file, "WAVE", 1);
    const size_t base = file.size();
    xsflib::StoreBe32(&file[Grow(file, 4)], 2);
    const size_t refs = Grow(file, 16);
    for (uint32_t i = 0; i < 2; ++i)
    {
        const size_t info = Grow(file, 0x6C);
        PutWaveInfo(file, info, i * 0x40, static_cast<uint16_t>(0x21 + i));
        PutRef(file, refs + i * 8, 0, static_cast<uint32_t>(info - base));
    }

    EndBlock(file, wave, 1);

    return file;
}

// The raw wave data for MakeOldBank: two waves of 0x40 bytes. Each starts with its predictor/scale.
inline Bytes MakeOldWaveData()
{
    Bytes data(0x80, 0);
    data[0] = 0x21;
    data[0x40] = 0x22;

    return data;
}

// An RWAV holding one ADPCM channel at `sample_rate` Hz. The channel loops unless `loop` is false.
inline Bytes MakeRwav(uint16_t pred_scale, bool loop = true, uint16_t sample_rate = 32000)
{
    Bytes file = Header("RWAV", 0x0102, 2);
    const size_t info = Block(file, "INFO", 0);
    const size_t payload = Grow(file, 0x6C);
    const size_t data = file.size() + 4;
    PutWaveInfo(file, payload, static_cast<uint32_t>(data - payload), pred_scale, loop, sample_rate);
    Grow(file, 4);
    EndBlock(file, info, 0);
    const size_t samples = Block(file, "DATA", 1);
    file[Grow(file, 0x40)] = static_cast<uint8_t>(pred_scale);
    EndBlock(file, samples, 1);

    return file;
}

// An RWAV of `channels` channels of big-endian PCM16 at 32000 Hz. Each channel holds `samples`. The wave's header gives
// `length` samples. A wave that loops does so from its start.
inline Bytes MakePcm16Rwav(uint32_t length, const std::vector<int16_t>& samples, int channels = 1, bool loop = false)
{
    Bytes file = Header("RWAV", 0x0102, 2);
    const size_t info = Block(file, "INFO", 0);
    const size_t kTable = 0x1C;
    const size_t first_channel = kTable + 4 * static_cast<size_t>(channels);
    const size_t payload = Grow(file, first_channel + 0x1C * static_cast<size_t>(channels));

    file[payload] = 1;
    file[payload + 1] = loop ? 1 : 0;
    file[payload + 2] = static_cast<uint8_t>(channels);
    xsflib::StoreBe16(&file[payload + 4], 32000);
    xsflib::StoreBe32(&file[payload + 0xC], length - 1);
    xsflib::StoreBe32(&file[payload + 0x10], static_cast<uint32_t>(kTable));
    for (size_t c = 0; c < static_cast<size_t>(channels); c++)
    {
        const size_t channel = first_channel + 0x1C * c;
        xsflib::StoreBe32(&file[payload + kTable + 4 * c], static_cast<uint32_t>(channel));
        xsflib::StoreBe32(&file[payload + channel], static_cast<uint32_t>(2 * samples.size() * c));
    }

    EndBlock(file, info, 0);
    const size_t data = Block(file, "DATA", 1);
    for (int c = 0; c < channels; c++)
    {
        for (const int16_t sample : samples)
        {
            xsflib::StoreBe16(&file[Grow(file, 2)], static_cast<uint16_t>(sample));
        }
    }

    EndBlock(file, data, 1);

    return file;
}

// An RWAR of `waves`, each an RWAV.
inline Bytes MakeRwarOf(const std::vector<Bytes>& waves)
{
    Bytes file = Header("RWAR", 0x0100, 2);
    const size_t table = Block(file, "TABL", 0);
    xsflib::StoreBe32(&file[Grow(file, 4)], static_cast<uint32_t>(waves.size()));
    const size_t entries = Grow(file, waves.size() * 12);
    EndBlock(file, table, 0);
    const size_t data = Block(file, "DATA", 1);
    for (size_t i = 0; i < waves.size(); ++i)
    {
        const Bytes& rwav = waves[i];
        const size_t at = Grow(file, rwav.size());
        std::memcpy(&file[at], rwav.data(), rwav.size());
        PutRef(file, entries + i * 12, 0, static_cast<uint32_t>(at - data));
        xsflib::StoreBe32(&file[entries + i * 12 + 8], static_cast<uint32_t>(rwav.size()));
    }

    EndBlock(file, data, 1);

    return file;
}

// An RWAR of two RWAVs at `sample_rate` Hz.
inline Bytes MakeRwar(bool loop = true, uint16_t sample_rate = 32000)
{
    return MakeRwarOf({MakeRwav(0x31, loop, sample_rate), MakeRwav(0x32, loop, sample_rate)});
}

// A version 1.2 bank without a WAVE block. MakeRwar builds its waves.
inline Bytes MakeNewBank()
{
    Bytes file = Header("RBNK", 0x0102, 1);
    PutInstruments(file);

    return file;
}

// A note of a wave sound for MakeWaveSoundFile, with the fields of the file's note info.
struct WaveSoundNote
{
    int32_t wave = 0;
    uint8_t attack = 127;
    uint8_t decay = 127;
    uint8_t sustain = 127;
    uint8_t release = 127;
    uint8_t hold = 0;
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    uint8_t surround_pan = 0;
    float pitch = 1.0f;
};

// A wave sound for MakeWaveSoundFile, with the fields of the file's wave sound info and its notes.
struct WaveSoundSpec
{
    float pitch = 1.0f;
    uint8_t pan = 64;
    uint8_t surround_pan = 0;
    uint8_t fx_sends[3] = {0, 0, 0};
    uint8_t main_send = 127;
    std::vector<WaveSoundNote> notes;
};

// An RWSD file of `version` that holds `sounds`. Every field goes in, whatever the version reads. The DATA block's
// references count from its count. Each wave sound refers to its settings, an empty table of tracks and its table of
// notes. Up to version 1.2, a WAVE block describes `waves` waves of one ADPCM channel each over MakeOldWaveData's raw
// samples. Wave i starts at 0x40 times i there. The WAVE block of version 1.0 gives no count. From version 1.3, the
// file has no WAVE block. Its wave data is then an RWAR.
inline Bytes MakeWaveSoundFile(uint16_t version, const std::vector<WaveSoundSpec>& sounds, uint32_t waves = 2)
{
    const auto put_float = [](Bytes& file, size_t at, float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        xsflib::StoreBe32(&file[at], bits);
    };

    const bool wave_block = version <= 0x0102;
    Bytes file = Header("RWSD", version, wave_block ? 2 : 1);
    const size_t data = Block(file, "DATA", 0);
    const size_t base = file.size();
    xsflib::StoreBe32(&file[Grow(file, 4)], static_cast<uint32_t>(sounds.size()));
    const size_t refs = Grow(file, sounds.size() * 8);
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const WaveSoundSpec& sound = sounds[i];
        const size_t wsd = Grow(file, 0x18);
        PutRef(file, refs + i * 8, 0, static_cast<uint32_t>(wsd - base));

        const size_t info = Grow(file, 0x20);
        put_float(file, info, sound.pitch);
        file[info + 4] = sound.pan;
        file[info + 5] = sound.surround_pan;
        std::memcpy(&file[info + 6], sound.fx_sends, 3);
        file[info + 9] = sound.main_send;
        PutRef(file, wsd, 0, static_cast<uint32_t>(info - base));

        const size_t tracks = Grow(file, 4);
        PutRef(file, wsd + 8, 0, static_cast<uint32_t>(tracks - base));

        const size_t notes = Grow(file, 4 + sound.notes.size() * 8);
        xsflib::StoreBe32(&file[notes], static_cast<uint32_t>(sound.notes.size()));
        PutRef(file, wsd + 0x10, 0, static_cast<uint32_t>(notes - base));
        for (size_t j = 0; j < sound.notes.size(); ++j)
        {
            const WaveSoundNote& note = sound.notes[j];
            const size_t at = Grow(file, 0x20);
            xsflib::StoreBe32(&file[at], static_cast<uint32_t>(note.wave));
            file[at + 4] = note.attack;
            file[at + 5] = note.decay;
            file[at + 6] = note.sustain;
            file[at + 7] = note.release;
            file[at + 8] = note.hold;
            file[at + 0xC] = note.original_key;
            file[at + 0xD] = note.volume;
            file[at + 0xE] = note.pan;
            file[at + 0xF] = note.surround_pan;
            put_float(file, at + 0x10, note.pitch);
            PutRef(file, notes + 4 + j * 8, 0, static_cast<uint32_t>(at - base));
        }
    }

    EndBlock(file, data, 0);
    if (!wave_block)
    {
        return file;
    }

    // The WAVE block: the count from version 1.1, then the offset of each wave info from the block's start.
    const size_t block = Block(file, "WAVE", 1);
    if (version >= 0x0101)
    {
        xsflib::StoreBe32(&file[Grow(file, 4)], waves);
    }

    const size_t offsets = Grow(file, waves * size_t{4});
    for (uint32_t i = 0; i < waves; ++i)
    {
        const size_t info = Grow(file, 0x6C);
        PutWaveInfo(file, info, i * 0x40, static_cast<uint16_t>(0x21 + i));
        xsflib::StoreBe32(&file[offsets + i * 4], static_cast<uint32_t>(info - block));
    }

    EndBlock(file, block, 1);

    return file;
}

// A sequence file holding `commands`, with labels at 0 and 8.
inline Bytes MakeSequence(const Bytes& commands)
{
    Bytes file = Header("RSEQ", 0x0100, 2);
    const size_t data = Block(file, "DATA", 0);
    xsflib::StoreBe32(&file[Grow(file, 4)], 0xC);
    const size_t at = Grow(file, commands.size());
    std::memcpy(&file[at], commands.data(), commands.size());

    EndBlock(file, data, 0);
    const size_t labels = Block(file, "LABL", 1);
    const size_t base = file.size();
    xsflib::StoreBe32(&file[Grow(file, 4)], 2);
    const size_t offsets = Grow(file, 8);
    const char* const kNames[] = {"SEQ_A", "SEQ_BB"};
    for (uint32_t i = 0; i < 2; ++i)
    {
        xsflib::StoreBe32(&file[offsets + i * 4], static_cast<uint32_t>(file.size() - base));
        const size_t label = Grow(file, 8 + 8);
        xsflib::StoreBe32(&file[label], i * 8);
        xsflib::StoreBe32(&file[label + 4], static_cast<uint32_t>(std::strlen(kNames[i])));
        std::memcpy(&file[label + 8], kNames[i], std::strlen(kNames[i]));
    }

    EndBlock(file, labels, 1);

    return file;
}

// An archive's SYMB block, block 0: the string table, with `strings` in order. The payload starts with the string
// table's offset. The four search trees' offsets after it stay 0.
inline void PutSymbols(Bytes& file, const std::vector<const char*>& strings)
{
    const size_t symbols = Block(file, "SYMB", 0);
    const size_t base = file.size();
    xsflib::StoreBe32(&file[Grow(file, 20)], 20);
    xsflib::StoreBe32(&file[Grow(file, 4)], static_cast<uint32_t>(strings.size()));
    const size_t offsets = Grow(file, strings.size() * 4);
    for (size_t i = 0; i < strings.size(); ++i)
    {
        xsflib::StoreBe32(&file[offsets + i * 4], static_cast<uint32_t>(file.size() - base));
        const size_t at = Grow(file, std::strlen(strings[i]) + 1);
        std::memcpy(&file[at], strings[i], std::strlen(strings[i]));
    }

    file.resize((file.size() + 3) & ~size_t{3}, 0);
    EndBlock(file, symbols, 0);
}

// Appends a table of `count` references to an archive's INFO block, after the count, and points the reference at `ref`
// to the table. INFO's references count from `base`, the start of its payload. The payload starts with a reference to
// each of the archive's tables: sounds at 0, banks at 8, players at 0x10, files at 0x18 and groups at 0x20. Returns
// where the table's references start.
inline size_t InfoTable(Bytes& file, size_t ref, size_t base, size_t count)
{
    const size_t at = Grow(file, 4 + count * 8);
    xsflib::StoreBe32(&file[at], static_cast<uint32_t>(count));
    PutRef(file, ref, 0, static_cast<uint32_t>(at - base));

    return at + 4;
}

// An archive of version 1.4 with one sequence sound, named in SYMB. The sound's sequence and bank (with its RWAR as
// wave data) are in one group. The sound starts `offset` bytes into the sequence's commands, with the tracks in
// `allocate_track`. The waves loop unless `loop` is false, and play at `sample_rate` Hz.
inline Bytes MakeArchive(const Bytes& sequence, uint32_t offset, uint32_t allocate_track, bool loop = true,
                         uint16_t sample_rate = 32000)
{
    Bytes file = Header("RSAR", 0x0104, 3, 0x40);
    PutSymbols(file, {"SEQ_TEST", "BANK_TEST"});

    // INFO: the tables of sounds, banks, files and groups.
    const size_t info = Block(file, "INFO", 1);
    const size_t base = file.size();
    const size_t tables = Grow(file, 0x30);

    const size_t sounds = InfoTable(file, tables, base, 1);
    const size_t sound = Grow(file, 0x2C);
    PutRef(file, sounds, 0, static_cast<uint32_t>(sound - base));
    xsflib::StoreBe32(&file[sound], 0);
    xsflib::StoreBe32(&file[sound + 4], 0);
    file[sound + 0x14] = 90;
    file[sound + 0x16] = 1;
    file[sound + 0x28] = 0;
    file[sound + 0x29] = 3;
    const size_t seq = Grow(file, 0x14);
    PutRef(file, sound + 0x18, 1, static_cast<uint32_t>(seq - base));
    xsflib::StoreBe32(&file[seq], offset);
    xsflib::StoreBe32(&file[seq + 8], allocate_track);
    file[seq + 0xC] = 70;
    file[seq + 0xD] = 1;

    const size_t banks = InfoTable(file, tables + 8, base, 1);
    const size_t bank = Grow(file, 8);
    PutRef(file, banks, 0, static_cast<uint32_t>(bank - base));
    xsflib::StoreBe32(&file[bank], 1);
    xsflib::StoreBe32(&file[bank + 4], 1);

    // A file's entry holds a table of the file's positions: a group and an item of the group for each. File f is item f
    // of group 0.
    const Bytes bank_file = MakeNewBank();
    const Bytes rwar = MakeRwar(loop, sample_rate);
    const size_t files = InfoTable(file, tables + 0x18, base, 2);
    for (uint32_t f = 0; f < 2; ++f)
    {
        const size_t entry = Grow(file, 0x1C);
        PutRef(file, files + f * 8, 0, static_cast<uint32_t>(entry - base));
        const size_t positions = InfoTable(file, entry + 0x14, base, 1);
        const size_t position = Grow(file, 8);
        xsflib::StoreBe32(&file[position + 4], f);
        PutRef(file, positions, 0, static_cast<uint32_t>(position - base));
    }

    // The group's entry holds a table of its items: the sequence, then the bank.
    const size_t groups = InfoTable(file, tables + 0x20, base, 1);
    const size_t group = Grow(file, 0x28);
    PutRef(file, groups, 0, static_cast<uint32_t>(group - base));
    const size_t items = InfoTable(file, group + 0x20, base, 2);
    size_t item[2];
    for (uint32_t i = 0; i < 2; ++i)
    {
        item[i] = Grow(file, 0x18);
        PutRef(file, items + i * 8, 0, static_cast<uint32_t>(item[i] - base));
        xsflib::StoreBe32(&file[item[i]], i);
    }

    EndBlock(file, info, 1);

    // FILE: the group's files (the sequence, then the bank), then its wave data (the bank's RWAR). The block's header
    // takes 0x20 bytes.
    const size_t data = Block(file, "FILE", 2);
    Grow(file, 0x18);
    const size_t group_files = file.size();
    file.insert(file.end(), sequence.begin(), sequence.end());
    file.insert(file.end(), bank_file.begin(), bank_file.end());
    const size_t group_waves = file.size();
    file.insert(file.end(), rwar.begin(), rwar.end());
    xsflib::StoreBe32(&file[group + 0x10], static_cast<uint32_t>(group_files));
    xsflib::StoreBe32(&file[group + 0x14], static_cast<uint32_t>(group_waves - group_files));
    xsflib::StoreBe32(&file[group + 0x18], static_cast<uint32_t>(group_waves));
    xsflib::StoreBe32(&file[group + 0x1C], static_cast<uint32_t>(rwar.size()));
    xsflib::StoreBe32(&file[item[0] + 8], static_cast<uint32_t>(sequence.size()));
    xsflib::StoreBe32(&file[item[1] + 4], static_cast<uint32_t>(sequence.size()));
    xsflib::StoreBe32(&file[item[1] + 8], static_cast<uint32_t>(bank_file.size()));
    xsflib::StoreBe32(&file[item[1] + 0x10], static_cast<uint32_t>(rwar.size()));
    EndBlock(file, data, 2);

    return file;
}

// A wave sound's entry for MakeWaveSoundArchive: its volume, its index in the file, its channel priority, its release
// priority fix, and its pan mode and curve.
struct ArchiveWaveSound
{
    uint8_t volume = 127;
    int32_t index = 0;
    uint8_t channel_priority = 64;
    bool release_priority_fix = false;
    uint8_t pan_mode = 0;
    uint8_t pan_curve = 0;
};

// An archive of version 1.4 with `sounds`, each a wave sound of the RWSD file `wave_sounds`, named WSD_0, WSD_1 and on
// in SYMB. The file and its wave data `wave_data` are in one group. The archive has no banks and no players.
inline Bytes MakeWaveSoundArchive(const Bytes& wave_sounds, const Bytes& wave_data,
                                  const std::vector<ArchiveWaveSound>& sounds)
{
    Bytes file = Header("RSAR", 0x0104, 3, 0x40);
    std::vector<std::string> names;
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        names.push_back("WSD_" + std::to_string(i));
    }

    std::vector<const char*> strings;
    for (const std::string& name : names)
    {
        strings.push_back(name.c_str());
    }

    PutSymbols(file, strings);

    // INFO: the tables of sounds, files and groups, with an empty one of banks. Each sound's details hold its index in
    // the file, its tracks, its channel priority and its release priority fix.
    const size_t info = Block(file, "INFO", 1);
    const size_t base = file.size();
    const size_t tables = Grow(file, 0x30);

    const size_t sound_refs = InfoTable(file, tables, base, sounds.size());
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const ArchiveWaveSound& entry = sounds[i];
        const size_t sound = Grow(file, 0x2C);
        PutRef(file, sound_refs + i * 8, 0, static_cast<uint32_t>(sound - base));
        xsflib::StoreBe32(&file[sound], static_cast<uint32_t>(i));
        file[sound + 0x14] = entry.volume;
        file[sound + 0x15] = 64;
        file[sound + 0x16] = 3;
        file[sound + 0x28] = entry.pan_mode;
        file[sound + 0x29] = entry.pan_curve;
        const size_t detail = Grow(file, 0x10);
        PutRef(file, sound + 0x18, 3, static_cast<uint32_t>(detail - base));
        xsflib::StoreBe32(&file[detail], static_cast<uint32_t>(entry.index));
        xsflib::StoreBe32(&file[detail + 4], 1);
        file[detail + 8] = entry.channel_priority;
        file[detail + 9] = entry.release_priority_fix ? 1 : 0;
    }

    InfoTable(file, tables + 8, base, 0);

    // The file's entry holds a table of its positions: item 0 of group 0.
    const size_t files = InfoTable(file, tables + 0x18, base, 1);
    const size_t entry = Grow(file, 0x1C);
    PutRef(file, files, 0, static_cast<uint32_t>(entry - base));
    const size_t positions = InfoTable(file, entry + 0x14, base, 1);
    const size_t position = Grow(file, 8);
    PutRef(file, positions, 0, static_cast<uint32_t>(position - base));

    const size_t groups = InfoTable(file, tables + 0x20, base, 1);
    const size_t group = Grow(file, 0x28);
    PutRef(file, groups, 0, static_cast<uint32_t>(group - base));
    const size_t items = InfoTable(file, group + 0x20, base, 1);
    const size_t item = Grow(file, 0x18);
    PutRef(file, items, 0, static_cast<uint32_t>(item - base));

    EndBlock(file, info, 1);

    // FILE: the group's file, then its wave data. The block's header takes 0x20 bytes.
    const size_t data = Block(file, "FILE", 2);
    Grow(file, 0x18);
    const size_t group_files = file.size();
    file.insert(file.end(), wave_sounds.begin(), wave_sounds.end());
    const size_t group_waves = file.size();
    file.insert(file.end(), wave_data.begin(), wave_data.end());
    xsflib::StoreBe32(&file[group + 0x10], static_cast<uint32_t>(group_files));
    xsflib::StoreBe32(&file[group + 0x14], static_cast<uint32_t>(wave_sounds.size()));
    xsflib::StoreBe32(&file[group + 0x18], static_cast<uint32_t>(group_waves));
    xsflib::StoreBe32(&file[group + 0x1C], static_cast<uint32_t>(wave_data.size()));
    xsflib::StoreBe32(&file[item + 8], static_cast<uint32_t>(wave_sounds.size()));
    xsflib::StoreBe32(&file[item + 0x10], static_cast<uint32_t>(wave_data.size()));
    EndBlock(file, data, 2);

    return file;
}

// A sound for MakeSoundTable: its name, type (1 a sequence, 2 a stream, 3 a wave sound) and player.
struct TableSound
{
    const char* name;
    uint8_t type;
    uint32_t player;
};

// An archive of version 1.4 with only its tables of sounds and players, named in SYMB, and no files. Each player plays
// one more sound at once than the one before it. The first plays one.
inline Bytes MakeSoundTable(const std::vector<TableSound>& sounds, const std::vector<const char*>& players)
{
    Bytes file = Header("RSAR", 0x0104, 3, 0x40);

    // SYMB: the names of the sounds, then the names of the players.
    std::vector<const char*> strings;
    for (const TableSound& sound : sounds)
    {
        strings.push_back(sound.name);
    }

    strings.insert(strings.end(), players.begin(), players.end());
    PutSymbols(file, strings);

    // INFO: the tables of sounds and players, with empty ones of banks, files and groups.
    const size_t info = Block(file, "INFO", 1);
    const size_t base = file.size();
    const size_t tables = Grow(file, 0x30);

    const size_t sound_refs = InfoTable(file, tables, base, sounds.size());
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const size_t sound = Grow(file, 0x2C);
        PutRef(file, sound_refs + i * 8, 0, static_cast<uint32_t>(sound - base));
        xsflib::StoreBe32(&file[sound], static_cast<uint32_t>(i));
        xsflib::StoreBe32(&file[sound + 8], sounds[i].player);
        file[sound + 0x14] = 127;
        file[sound + 0x16] = sounds[i].type;
    }

    const size_t player_refs = InfoTable(file, tables + 0x10, base, players.size());
    for (size_t i = 0; i < players.size(); ++i)
    {
        const size_t player = Grow(file, 0x10);
        PutRef(file, player_refs + i * 8, 0, static_cast<uint32_t>(player - base));
        xsflib::StoreBe32(&file[player], static_cast<uint32_t>(sounds.size() + i));
        file[player + 4] = static_cast<uint8_t>(i + 1);
    }

    for (const size_t ref : {0x08, 0x18, 0x20})
    {
        InfoTable(file, tables + ref, base, 0);
    }

    EndBlock(file, info, 1);

    return file;
}

} // namespace ntdlib::testing::rsar_files
