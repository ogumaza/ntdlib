// SPDX-License-Identifier: MIT

// Builders of small NW4R sound files for the tests: banks of both generations with instruments that exercise every way
// that a bank selects a region, wave data and an RWAR for them, a sequence, wave sound files of both generations, an
// archive holding a sequence sound with its sequence and bank in a group, an archive holding wave sounds with their
// file in a group, an archive with only its tables of sounds and players, and an archive whose files lie in several
// groups.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/testing/byte_writer.h"

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
        xsflib::StoreBe32(&file[info], std::bit_cast<uint32_t>(sound.pitch));
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
            xsflib::StoreBe32(&file[at + 0x10], std::bit_cast<uint32_t>(note.pitch));
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

    file.resize(AlignUp(file.size(), 4), 0);
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

// An item of a group, for AppendFileBlock: the offset of the item's entry in INFO, its file and its wave data.
struct GroupItem
{
    size_t entry = 0;
    Bytes bytes;
    Bytes wave_data;
};

// A group, for AppendFileBlock: the offset of the group's entry in INFO, and its items.
struct FileGroup
{
    size_t entry = 0;
    std::vector<GroupItem> items;
};

// Appends an archive's FILE block, block 2: a header of 0x20 bytes, then the files of each group one after another,
// then their wave data. Stores the offset and the size of each file and each wave data in the entry of its item, and
// those of the files and the wave data of each group in the entry of the group. An item without wave data keeps zeros
// there.
inline void AppendFileBlock(Bytes& file, const std::vector<FileGroup>& groups)
{
    const size_t data = Block(file, "FILE", 2);
    Grow(file, 0x18);
    for (const FileGroup& group : groups)
    {
        const size_t group_files = file.size();
        for (const GroupItem& item : group.items)
        {
            xsflib::StoreBe32(&file[item.entry + 4], static_cast<uint32_t>(file.size() - group_files));
            xsflib::StoreBe32(&file[item.entry + 8], static_cast<uint32_t>(item.bytes.size()));
            file.insert(file.end(), item.bytes.begin(), item.bytes.end());
        }

        const size_t group_waves = file.size();
        for (const GroupItem& item : group.items)
        {
            if (!item.wave_data.empty())
            {
                xsflib::StoreBe32(&file[item.entry + 0xC], static_cast<uint32_t>(file.size() - group_waves));
                xsflib::StoreBe32(&file[item.entry + 0x10], static_cast<uint32_t>(item.wave_data.size()));
                file.insert(file.end(), item.wave_data.begin(), item.wave_data.end());
            }
        }

        xsflib::StoreBe32(&file[group.entry + 0x10], static_cast<uint32_t>(group_files));
        xsflib::StoreBe32(&file[group.entry + 0x14], static_cast<uint32_t>(group_waves - group_files));
        xsflib::StoreBe32(&file[group.entry + 0x18], static_cast<uint32_t>(group_waves));
        xsflib::StoreBe32(&file[group.entry + 0x1C], static_cast<uint32_t>(file.size() - group_waves));
    }
    EndBlock(file, data, 2);
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
    AppendFileBlock(file, {{group, {{item[0], sequence, {}}, {item[1], bank_file, rwar}}}});

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
    AppendFileBlock(file, {{group, {{item, wave_sounds, wave_data}}}});

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

// The archive of MakeArchive with a sequence of `commands` and the tracks in the mask `tracks`. MakeArchive's waves are
// silent: each frame of their DSP-ADPCM holds only its header. Here each frame gives the samples of its nibbles at a
// scale of 2^11, and the waves sound.
inline Bytes MakeAudibleArchive(const Bytes& commands, uint32_t tracks)
{
    Bytes bytes = MakeArchive(MakeSequence(commands), 0, tracks);
    constexpr uint8_t kNibbles[] = {0x7F, 0x19, 0xE3, 0x5A, 0xC6, 0x2D, 0x91};
    for (std::size_t at = 0; at + 4 <= bytes.size(); at++)
    {
        if (std::memcmp(&bytes[at], "RWAV", 4) != 0)
        {
            continue;
        }

        // The wave's DATA block holds 0x40 bytes of samples after its header of 8 bytes.
        const uint32_t data = xsflib::LoadBe32(&bytes[at + 0x18]);
        for (std::size_t frame = 0; frame < 0x40; frame += 8)
        {
            uint8_t* samples = &bytes[at + data + 8 + frame];
            samples[0] = 0x0B;
            std::memcpy(samples + 1, kNibbles, sizeof(kNibbles));
        }
    }

    return bytes;
}

// An archive that MakeGroupedArchive builds, with where its FILE block starts and where its INFO block holds each
// group's entry and each item's.
struct GroupedArchive
{
    Bytes bytes;
    std::size_t file_block = 0;
    std::vector<std::size_t> groups;
    std::vector<std::vector<std::size_t>> items;
};

// A group for MakeGroupedArchive: whether the archive holds it, and the IDs of the files that its items hold.
struct GroupFiles
{
    bool internal;
    std::vector<uint32_t> files;
};

// A file's places for MakeGroupedArchive: a group and an item of the group for each.
using FilePlaces = std::vector<std::pair<uint32_t, uint32_t>>;

// An archive of version 1.4 without names. It holds `files` (each file's data and wave data) in `groups`, and `places`
// lists each file's places. Sound i plays sequence file `sequences[i]` with bank i, and bank i's file is `banks[i]`.
// Each internal group holds its files, then their wave data, each at a multiple of 32 bytes. An external group's
// offsets count from the start of its file.
inline GroupedArchive MakeGroupedArchive(const std::vector<std::pair<Bytes, Bytes>>& files,
                                         const std::vector<GroupFiles>& groups,
                                         const std::vector<FilePlaces>& places, const std::vector<uint32_t>& sequences,
                                         const std::vector<uint32_t>& banks)
{
    GroupedArchive archive;
    Bytes& file = archive.bytes;
    file = Header("RSAR", 0x0104, 2, 0x40);
    const auto put = [&file](std::size_t at, std::size_t value)
    {
        xsflib::StoreBe32(&file[at], static_cast<uint32_t>(value));
    };

    const auto pad = [](Bytes& data)
    {
        data.resize((data.size() + 0x1F) & ~std::size_t{0x1F}, 0);
    };

    // INFO: the tables of sounds, banks, files and groups. A table holds a count, then a reference to each entry. The
    // references count from the block's payload.
    const std::size_t info = Grow(file, 8);
    std::memcpy(&file[info], "INFO", 4);
    const std::size_t base = file.size();
    const std::size_t tables = Grow(file, 0x30);

    const auto entry = [&](std::size_t refs, std::size_t i, std::size_t size)
    {
        const std::size_t at = Grow(file, size);
        PutRef(file, refs + i * 8, 0, static_cast<uint32_t>(at - base));

        return at;
    };

    const std::size_t sound_refs = InfoTable(file, tables, base, sequences.size());
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        const std::size_t sound = entry(sound_refs, i, 0x2C);
        put(sound + 4, sequences[i]);
        file[sound + 0x16] = 1;
        const std::size_t sequence = Grow(file, 0x14);
        PutRef(file, sound + 0x18, 1, static_cast<uint32_t>(sequence - base));
        put(sequence + 4, i);
    }

    const std::size_t bank_refs = InfoTable(file, tables + 8, base, banks.size());
    for (std::size_t i = 0; i < banks.size(); i++)
    {
        put(entry(bank_refs, i, 8) + 4, banks[i]);
    }

    const std::size_t file_refs = InfoTable(file, tables + 0x18, base, files.size());
    for (std::size_t f = 0; f < files.size(); f++)
    {
        const std::size_t at = entry(file_refs, f, 0x1C);
        put(at, files[f].first.size());
        put(at + 4, files[f].second.size());
        const std::size_t place_refs = InfoTable(file, at + 0x14, base, places[f].size());
        for (std::size_t p = 0; p < places[f].size(); p++)
        {
            const std::size_t place = entry(place_refs, p, 8);
            put(place, places[f][p].first);
            put(place + 4, places[f][p].second);
        }
    }

    const std::size_t group_refs = InfoTable(file, tables + 0x20, base, groups.size());
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        archive.groups.push_back(entry(group_refs, g, 0x28));
        if (!groups[g].internal)
        {
            const std::size_t path = Grow(file, 8);
            std::memcpy(&file[path], "GROUP", 5);
            PutRef(file, archive.groups[g] + 8, 0, static_cast<uint32_t>(path - base));
        }

        const std::size_t item_refs = InfoTable(file, archive.groups[g] + 0x20, base, groups[g].files.size());
        std::vector<std::size_t>& items = archive.items.emplace_back();
        for (std::size_t i = 0; i < groups[g].files.size(); i++)
        {
            items.push_back(entry(item_refs, i, 0x18));
            put(items[i], groups[g].files[i]);
        }
    }

    pad(file);
    put(info + 4, file.size() - info);
    put(0x18, info);
    put(0x1C, file.size() - info);

    // FILE: each internal group's files, then their wave data. An external group's file holds them in the same way.
    archive.file_block = Grow(file, 0x20);
    std::memcpy(&file[archive.file_block], "FILE", 4);
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        Bytes external;
        Bytes& out = groups[g].internal ? file : external;
        const std::size_t group = archive.groups[g];
        const std::vector<std::size_t>& items = archive.items[g];
        std::size_t at = out.size();
        put(group + 0x10, at);
        for (std::size_t i = 0; i < items.size(); i++)
        {
            const Bytes& data = files[groups[g].files[i]].first;
            put(items[i] + 4, out.size() - at);
            put(items[i] + 8, data.size());
            out.insert(out.end(), data.begin(), data.end());
            pad(out);
        }

        put(group + 0x14, out.size() - at);
        at = out.size();
        put(group + 0x18, at);
        for (std::size_t i = 0; i < items.size(); i++)
        {
            const Bytes& data = files[groups[g].files[i]].second;
            put(items[i] + 0xC, out.size() - at);
            put(items[i] + 0x10, data.size());
            out.insert(out.end(), data.begin(), data.end());
            pad(out);
        }

        put(group + 0x1C, out.size() - at);
    }

    put(archive.file_block + 4, file.size() - archive.file_block);
    put(0x20, archive.file_block);
    put(0x24, file.size() - archive.file_block);
    put(8, file.size());

    return archive;
}

} // namespace ntdlib::testing::rsar_files
