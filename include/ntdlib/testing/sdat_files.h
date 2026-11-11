// SPDX-License-Identifier: MIT

// Builders of small DS sound files for the tests: sequences (SSEQ), sequence archives (SSAR), banks (SBNK) with every
// kind of instrument, waves (SWAV), wave archives (SWAR), streams (STRM) and sound archives (SDAT) with or without
// names. Each file is little-endian and starts with the 16-byte header that ntdlib/sdat/formats.h describes. A sound
// archive's files lie in its FILE block, each on a multiple of 32 bytes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/testing/byte_writer.h"

namespace ntdlib::testing::sdat_files
{

using Bytes = std::vector<uint8_t>;

inline void Put16(Bytes& file, std::size_t at, uint32_t value)
{
    xsflib::StoreLe16(&file[at], static_cast<uint16_t>(value));
}

inline void Put32(Bytes& file, std::size_t at, uint32_t value)
{
    xsflib::StoreLe32(&file[at], value);
}

inline void Append16(Bytes& file, uint32_t value)
{
    xsflib::AppendLe16(file, static_cast<uint16_t>(value));
}

inline void Append32(Bytes& file, uint32_t value)
{
    xsflib::AppendLe32(file, value);
}

inline void AppendMagic(Bytes& file, const char* magic)
{
    file.insert(file.end(), magic, magic + 4);
}

// Appends zeros up to a multiple of `alignment`.
inline void Align(Bytes& file, std::size_t alignment)
{
    file.resize(AlignUp(file.size(), alignment), 0);
}

// A file header for `blocks` blocks. Finish() fills in the size.
inline Bytes Header(const char* magic, uint16_t header_size = 0x10, uint16_t blocks = 1)
{
    Bytes file;
    AppendMagic(file, magic);
    Append16(file, 0xFEFF);
    Append16(file, 0x0100);
    Append32(file, 0);
    Append16(file, header_size);
    Append16(file, blocks);
    file.resize(header_size, 0);

    return file;
}

// Writes the file's size into its header.
inline Bytes Finish(Bytes file)
{
    Put32(file, 8, static_cast<uint32_t>(file.size()));

    return file;
}

// Starts a block. EndBlock() fills in its size.
inline std::size_t StartBlock(Bytes& file, const char* magic)
{
    const std::size_t at = file.size();
    AppendMagic(file, magic);
    Append32(file, 0);

    return at;
}

inline void EndBlock(Bytes& file, std::size_t block)
{
    Put32(file, block + 4, static_cast<uint32_t>(file.size() - block));
}

// A sequence file holding `bytecode`.
inline Bytes Sequence(const Bytes& bytecode)
{
    Bytes file = Header("SSEQ");
    const std::size_t block = StartBlock(file, "DATA");
    Append32(file, 0x1C);
    file.insert(file.end(), bytecode.begin(), bytecode.end());
    Align(file, 4);
    EndBlock(file, block);

    return Finish(file);
}

// A sequence of a sequence archive: its offset in the bytecode and its settings.
struct ArchivedSequence
{
    uint32_t offset = 0;
    uint16_t bank = 0;
    uint8_t volume = 127;
    uint8_t channel_priority = 64;
    uint8_t player_priority = 64;
    uint8_t player = 0;
};

inline Bytes SequenceArchive(const std::vector<ArchivedSequence>& sequences, const Bytes& bytecode)
{
    Bytes file = Header("SSAR");
    const std::size_t block = StartBlock(file, "DATA");
    const std::size_t data_offset = file.size() + 8 + sequences.size() * 12;
    Append32(file, static_cast<uint32_t>(data_offset));
    Append32(file, static_cast<uint32_t>(sequences.size()));
    for (const ArchivedSequence& s : sequences)
    {
        Append32(file, s.offset);
        Append16(file, s.bank);
        file.insert(file.end(), {s.volume, s.channel_priority, s.player_priority, s.player, 0, 0});
    }
    file.insert(file.end(), bytecode.begin(), bytecode.end());
    Align(file, 4);
    EndBlock(file, block);

    return Finish(file);
}

// A region's parameters: the wave, the wave archive's slot, the root key, the envelope and the pan.
struct Region
{
    uint8_t type = 1;
    uint16_t wave = 0;
    uint16_t wave_archive = 0;
    uint8_t root_key = 60;
    uint8_t attack = 127;
    uint8_t decay = 127;
    uint8_t sustain = 127;
    uint8_t release = 127;
    uint8_t pan = 64;
};

inline void AppendRegion(Bytes& file, const Region& region)
{
    Append16(file, region.wave);
    Append16(file, region.wave_archive);
    file.insert(file.end(), {region.root_key, region.attack, region.decay, region.sustain, region.release, region.pan});
}

// An instrument: one region (types 1 to 5), a drum set from `min_key` (type 16) or a key split (type 17), whose
// regions end at `split_keys`.
struct Instrument
{
    uint8_t type = 0;
    Region region = {};
    uint8_t min_key = 0;
    std::vector<uint8_t> split_keys = {};
    std::vector<Region> regions = {};
};

inline Bytes Bank(const std::vector<Instrument>& instruments)
{
    Bytes file = Header("SBNK");
    const std::size_t block = StartBlock(file, "DATA");
    file.resize(file.size() + 32, 0);
    Append32(file, static_cast<uint32_t>(instruments.size()));
    const std::size_t table = file.size();
    file.resize(table + instruments.size() * 4, 0);
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        const Instrument& instrument = instruments[i];
        if (instrument.type == 0)
        {
            continue;
        }

        Put32(file, table + i * 4, static_cast<uint32_t>(file.size() << 8 | instrument.type));
        if (instrument.type == 16)
        {
            file.push_back(instrument.min_key);
            file.push_back(static_cast<uint8_t>(instrument.min_key + instrument.regions.size() - 1));
        }
        else if (instrument.type == 17)
        {
            for (std::size_t k = 0; k < 8; k++)
            {
                file.push_back(k < instrument.split_keys.size() ? instrument.split_keys[k] : 0);
            }
        }
        else
        {
            AppendRegion(file, instrument.region);
            continue;
        }
        for (const Region& region : instrument.regions)
        {
            file.push_back(region.type);
            file.push_back(0);
            AppendRegion(file, region);
        }
    }
    Align(file, 4);
    EndBlock(file, block);

    return Finish(file);
}

// A wave's parameters and its data. The loop's start and length count 32-bit words.
struct Wave
{
    uint8_t encoding = 0;
    uint8_t loop = 0;
    uint16_t sample_rate = 32768;
    uint16_t loop_start = 0;
    uint32_t loop_length = 0;
    Bytes data = {};
};

inline void AppendWave(Bytes& file, const Wave& wave)
{
    file.push_back(wave.encoding);
    file.push_back(wave.loop);
    Append16(file, wave.sample_rate);
    Append16(file, static_cast<uint32_t>(16756991 / wave.sample_rate));
    Append16(file, wave.loop_start);
    Append32(file, wave.loop_length);
    file.insert(file.end(), wave.data.begin(), wave.data.end());
}

inline Bytes WaveFile(const Wave& wave)
{
    Bytes file = Header("SWAV");
    const std::size_t block = StartBlock(file, "DATA");
    AppendWave(file, wave);
    EndBlock(file, block);

    return Finish(file);
}

inline Bytes WaveArchive(const std::vector<Wave>& waves)
{
    Bytes file = Header("SWAR");
    const std::size_t block = StartBlock(file, "DATA");
    file.resize(file.size() + 32, 0);
    Append32(file, static_cast<uint32_t>(waves.size()));
    const std::size_t table = file.size();
    file.resize(table + waves.size() * 4, 0);
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        Put32(file, table + i * 4, static_cast<uint32_t>(file.size()));
        AppendWave(file, waves[i]);
    }
    EndBlock(file, block);

    return Finish(file);
}

// A stream of `channels` channels whose blocks hold `block_bytes` bytes, the last `last_block_bytes`. `data` holds the
// blocks, each channel's part in turn.
struct Stream
{
    uint8_t encoding = 1;
    uint8_t loop = 0;
    uint8_t channels = 1;
    uint16_t sample_rate = 32728;
    uint32_t loop_start = 0;
    uint32_t samples = 0;
    uint32_t blocks = 0;
    uint32_t block_bytes = 0;
    uint32_t block_samples = 0;
    uint32_t last_block_bytes = 0;
    uint32_t last_block_samples = 0;
    Bytes data;
};

inline Bytes StreamFile(const Stream& stream)
{
    Bytes file = Header("STRM", 0x10, 2);
    const std::size_t head = StartBlock(file, "HEAD");
    file.insert(file.end(), {stream.encoding, stream.loop, stream.channels, 0});
    Append16(file, stream.sample_rate);
    Append16(file, static_cast<uint32_t>(16756991 / stream.sample_rate / 32));
    Append32(file, stream.loop_start);
    Append32(file, stream.samples);
    const std::size_t data_offset = file.size();
    Append32(file, 0);
    Append32(file, stream.blocks);
    Append32(file, stream.block_bytes);
    Append32(file, stream.block_samples);
    Append32(file, stream.last_block_bytes);
    Append32(file, stream.last_block_samples);
    file.resize(file.size() + 32, 0);
    EndBlock(file, head);
    const std::size_t data = StartBlock(file, "DATA");
    Put32(file, data_offset, static_cast<uint32_t>(file.size()));
    file.insert(file.end(), stream.data.begin(), stream.data.end());
    EndBlock(file, data);

    return Finish(file);
}

// An archive's item: its record's bytes (empty for an index without one) and its name (empty for none).
struct Item
{
    Bytes record = {};
    std::string name = {};
    std::vector<std::string> sequence_names = {}; // a sequence archive's sequences
    bool present = true;
};

// A sequence's record: its file, its bank, its volume, its priorities and its player.
inline Item SequenceItem(uint32_t file_id, uint16_t bank, std::string name, uint8_t player = 0)
{
    Item item{{}, std::move(name)};
    Append32(item.record, file_id);
    Append16(item.record, bank);
    item.record.insert(item.record.end(), {100, 64, 32, player, 0, 0});

    return item;
}

inline Item SequenceArchiveItem(uint32_t file_id, std::string name, std::vector<std::string> sequence_names)
{
    Item item{{}, std::move(name), std::move(sequence_names)};
    Append32(item.record, file_id);

    return item;
}

inline Item BankItem(uint32_t file_id, std::array<uint16_t, 4> wave_archives, std::string name)
{
    Item item{{}, std::move(name)};
    Append32(item.record, file_id);
    for (const uint16_t wave_archive : wave_archives)
    {
        Append16(item.record, wave_archive);
    }

    return item;
}

inline Item FileItem(uint32_t file_id, std::string name)
{
    Item item{{}, std::move(name)};
    Append32(item.record, file_id);

    return item;
}

inline Item PlayerItem(uint8_t max_sequences, uint16_t channels, uint32_t heap_size, std::string name)
{
    Item item{{}, std::move(name)};
    item.record.insert(item.record.end(), {max_sequences, 0});
    Append16(item.record, channels);
    Append32(item.record, heap_size);

    return item;
}

inline Item AbsentItem()
{
    return Item{{}, "", {}, false};
}

// An archive's items of the eight kinds and its files. The names go into a SYMB block when `symbols` is set.
struct Archive
{
    std::array<std::vector<Item>, 8> items;
    std::vector<Bytes> files;
    bool symbols = true;
};

// Appends the SYMB block of `archive` to `file`, and stores the block's offset and size in the header.
inline void WriteSymb(Bytes& file, const Archive& archive)
{
    const std::size_t symb = StartBlock(file, "SYMB");
    file.resize(file.size() + 8 * 4 + 24, 0);
    std::vector<std::pair<std::size_t, std::string>> names;
    for (std::size_t kind = 0; kind < 8; kind++)
    {
        Put32(file, symb + 8 + kind * 4, static_cast<uint32_t>(file.size() - symb));
        const std::vector<Item>& items = archive.items[kind];
        Append32(file, static_cast<uint32_t>(items.size()));
        const std::size_t list = file.size();
        file.resize(list + items.size() * (kind == 1 ? 8 : 4), 0);
        for (std::size_t i = 0; i < items.size(); i++)
        {
            if (!items[i].name.empty())
            {
                names.emplace_back(list + i * (kind == 1 ? 8 : 4), items[i].name);
            }
            if (kind == 1)
            {
                Put32(file, list + i * 8 + 4, static_cast<uint32_t>(file.size() - symb));
                Append32(file, static_cast<uint32_t>(items[i].sequence_names.size()));
                const std::size_t sub = file.size();
                file.resize(sub + items[i].sequence_names.size() * 4, 0);
                for (std::size_t k = 0; k < items[i].sequence_names.size(); k++)
                {
                    names.emplace_back(sub + k * 4, items[i].sequence_names[k]);
                }
            }
        }
    }
    for (const auto& [at, name] : names)
    {
        Put32(file, at, static_cast<uint32_t>(file.size() - symb));
        file.insert(file.end(), name.begin(), name.end());
        file.push_back(0);
    }
    Align(file, 4);
    EndBlock(file, symb);
    Put32(file, 0x10, static_cast<uint32_t>(symb));
    Put32(file, 0x14, static_cast<uint32_t>(file.size() - symb));
}

inline Bytes SoundArchive(const Archive& archive)
{
    Bytes file = Header("SDAT", 0x40, archive.symbols ? 4 : 3);

    // SYMB: a list of name offsets for each kind. The sequence archives' list gives each archive's name and the
    // offset of its sequences' list.
    if (archive.symbols)
    {
        WriteSymb(file, archive);
    }

    // INFO: a list of record offsets for each kind, then the records.
    const std::size_t info = StartBlock(file, "INFO");
    file.resize(file.size() + 8 * 4 + 24, 0);
    for (std::size_t kind = 0; kind < 8; kind++)
    {
        Put32(file, info + 8 + kind * 4, static_cast<uint32_t>(file.size() - info));
        const std::vector<Item>& items = archive.items[kind];
        Append32(file, static_cast<uint32_t>(items.size()));
        const std::size_t list = file.size();
        file.resize(list + items.size() * 4, 0);
        for (std::size_t i = 0; i < items.size(); i++)
        {
            if (items[i].present)
            {
                Put32(file, list + i * 4, static_cast<uint32_t>(file.size() - info));
                file.insert(file.end(), items[i].record.begin(), items[i].record.end());
            }
        }
    }
    Align(file, 4);
    EndBlock(file, info);
    Put32(file, 0x18, static_cast<uint32_t>(info));
    Put32(file, 0x1C, static_cast<uint32_t>(file.size() - info));

    // FAT: 16 bytes for each file. FILE: the files, each on a multiple of 32 bytes.
    const std::size_t fat = StartBlock(file, "FAT ");
    Append32(file, static_cast<uint32_t>(archive.files.size()));
    const std::size_t entries = file.size();
    file.resize(entries + archive.files.size() * 16, 0);
    EndBlock(file, fat);
    Put32(file, 0x20, static_cast<uint32_t>(fat));
    Put32(file, 0x24, static_cast<uint32_t>(file.size() - fat));
    const std::size_t data = StartBlock(file, "FILE");
    Append32(file, static_cast<uint32_t>(archive.files.size()));
    Append32(file, 0);
    for (std::size_t i = 0; i < archive.files.size(); i++)
    {
        Align(file, 32);
        Put32(file, entries + i * 16, static_cast<uint32_t>(file.size()));
        Put32(file, entries + i * 16 + 4, static_cast<uint32_t>(archive.files[i].size()));
        file.insert(file.end(), archive.files[i].begin(), archive.files[i].end());
    }
    EndBlock(file, data);
    Put32(file, 0x28, static_cast<uint32_t>(data));
    Put32(file, 0x2C, static_cast<uint32_t>(file.size() - data));

    return Finish(file);
}

} // namespace ntdlib::testing::sdat_files
