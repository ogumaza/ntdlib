// SPDX-License-Identifier: MIT

// The builders of the synthetic RSAR archives and their files that sequence_export's tests on the Wii run on: PCM16
// waves, RWAR wave archives, banks of regions with every setting, sequences, and archives of sequence sounds with
// their names and groups. They build on the RSAR helpers of ntdlib::testing::rsar_files. The shared tests of
// sequence_export_test.h build these files when they run on the Wii.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/rounding.h"
#include "ntdlib/testing/rsar_files.h"

namespace ntdlib::sequence_export::wii_tests
{

using ntdlib::testing::rsar_files::Block;
using ntdlib::testing::rsar_files::EndBlock;
using ntdlib::testing::rsar_files::Grow;
using ntdlib::testing::rsar_files::Header;
using ntdlib::testing::rsar_files::InfoTable;
using ntdlib::testing::rsar_files::MakeSequence;
using ntdlib::testing::rsar_files::PutRef;

// Appends `data` and returns its offset.
inline std::size_t Append(std::vector<uint8_t>& file, std::span<const uint8_t> data)
{
    const std::size_t at = Grow(file, data.size());
    std::ranges::copy(data, file.begin() + static_cast<std::ptrdiff_t>(at));

    return at;
}

// Writes a PCM16 wave info at `info` for `length` samples at `sample_rate`, looping from 0 if `loops`, with `channels`
// channels whose samples start at `samples_at` from the wave data's start.
inline void WaveInfo(std::vector<uint8_t>& file, std::size_t info, uint32_t length, uint32_t channels, bool loops,
                     uint32_t sample_rate, uint32_t data_offset, uint32_t samples_at)
{
    file[info] = 1; // PCM16
    file[info + 1] = loops ? 1 : 0;
    file[info + 2] = static_cast<uint8_t>(channels);
    file[info + 3] = static_cast<uint8_t>(sample_rate >> 16);
    xsflib::StoreBe16(&file[info + 4], static_cast<uint16_t>(sample_rate));
    xsflib::StoreBe32(&file[info + 0xC], length - 1); // the last sample's address
    xsflib::StoreBe32(&file[info + 0x10], 0x1C);
    xsflib::StoreBe32(&file[info + 0x14], data_offset);
    for (uint32_t c = 0; c < channels; c++)
    {
        xsflib::StoreBe32(&file[info + 0x1C + c * 4], 0x1C + channels * 4 + c * 0x18);
        xsflib::StoreBe32(&file[info + 0x1C + channels * 4 + c * 0x18], samples_at);
    }
}

// The size of a wave info with `channels` channels.
inline std::size_t WaveInfoSize(uint32_t channels)
{
    return 0x1C + channels * (4 + 0x18);
}

// Appends `samples` as big-endian PCM16 and returns their offset.
inline std::size_t AddSamples(std::vector<uint8_t>& file, const std::vector<int16_t>& samples)
{
    const std::size_t at = Grow(file, samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        xsflib::StoreBe16(&file[at + i * 2], static_cast<uint16_t>(samples[i]));
    }

    return at;
}

// Builds a PCM16 RWAV declaring `length` samples at `sample_rate`. Each of `channels` channels uses `samples`. If
// `loops` is set, the entire wave loops.
inline std::vector<uint8_t> Rwav(uint32_t length, const std::vector<int16_t>& samples, uint32_t channels = 1,
                                 bool loops = false, uint32_t sample_rate = 32000)
{
    std::vector<uint8_t> file = Header("RWAV", 0x0102, 2);
    const std::size_t info = Block(file, "INFO", 0);
    Grow(file, WaveInfoSize(channels));
    WaveInfo(file, info + 8, length, channels, loops, sample_rate, 0, 0);
    EndBlock(file, info, 0);

    const std::size_t data = Block(file, "DATA", 1);
    AddSamples(file, samples);
    EndBlock(file, data, 1);

    return file;
}

// Builds an RWAR holding RWAV files `waves`.
inline std::vector<uint8_t> Rwar(const std::vector<std::vector<uint8_t>>& waves)
{
    return ntdlib::testing::rsar_files::MakeRwarOf(waves);
}

// A test bank region.
struct TestRegion
{
    int32_t wave_index = 0;
    uint8_t attack = 127, decay = 127, sustain = 127, release = 127, hold = 0, note_off_type = 0, key_group = 0;
    uint8_t original_key = 60, volume = 127, pan = 64;
    float tune = 1.0f;
};

// A test instrument: a region for every key and velocity, or a range table of `upper_keys` or `upper_velocities`, one
// region for each.
struct TestInstrument
{
    std::vector<TestRegion> regions = {TestRegion{}};
    std::vector<uint8_t> upper_keys = {};
    std::vector<uint8_t> upper_velocities = {};
};

// Writes `r` at `at`.
inline void WriteRegion(std::vector<uint8_t>& file, std::size_t at, const TestRegion& r)
{
    xsflib::StoreBe32(&file[at], static_cast<uint32_t>(r.wave_index));
    const uint8_t fields[] = {r.attack,        r.decay,     r.sustain,      r.release, r.hold, 0,
                              r.note_off_type, r.key_group, r.original_key, r.volume,  r.pan,  0};
    std::ranges::copy(fields, file.begin() + static_cast<std::ptrdiff_t>(at + 4));

    uint32_t tune;
    std::memcpy(&tune, &r.tune, 4);
    xsflib::StoreBe32(&file[at + 0x10], tune);
}

// Builds an RBNK of `instruments`. A version 1.1 bank also gets a WAVE block describing `waves` PCM16 waves of `length`
// samples each. Their samples follow one another in the bank's wave data. A later bank's waves are an RWAR.
inline std::vector<uint8_t> Rbnk(const std::vector<TestInstrument>& instruments, uint16_t version = 0x0102,
                                 uint32_t waves = 0, uint32_t length = 0)
{
    std::vector<uint8_t> file = Header("RBNK", version, waves ? 2 : 1);
    const std::size_t data = Block(file, "DATA", 0), base = data + 8;
    const std::size_t table = Grow(file, 4 + instruments.size() * 8);
    xsflib::StoreBe32(&file[table], static_cast<uint32_t>(instruments.size()));
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        const TestInstrument& inst = instruments[i];
        if (inst.upper_keys.empty() && inst.upper_velocities.empty())
        {
            const std::size_t region = Grow(file, 0x14);
            WriteRegion(file, region, inst.regions[0]);
            PutRef(file, table + 4 + i * 8, 1, static_cast<uint32_t>(region - base));
            continue;
        }

        // A range table: a count and each entry's upper key or velocity, then a reference to each entry's region.
        auto range_table = [&](const std::vector<uint8_t>& uppers)
        {
            const std::size_t n = uppers.size(), refs = ntdlib::AlignUp(n + 1, 4);
            const std::size_t range = Grow(file, refs + n * 8);
            file[range] = static_cast<uint8_t>(n);
            for (std::size_t k = 0; k < n; k++)
            {
                file[range + 1 + k] = uppers[k];
                const std::size_t region = Grow(file, 0x14);
                WriteRegion(file, region, inst.regions[k]);
                PutRef(file, range + refs + k * 8, 1, static_cast<uint32_t>(region - base));
            }

            return range;
        };

        if (inst.upper_velocities.empty())
        {
            PutRef(file, table + 4 + i * 8, 2, static_cast<uint32_t>(range_table(inst.upper_keys) - base));
            continue;
        }

        // Velocities split under a key table of one entry, for every key.
        const std::size_t keys = Grow(file, 4 + 8);
        file[keys] = 1;
        file[keys + 1] = 127;
        PutRef(file, keys + 4, 2, static_cast<uint32_t>(range_table(inst.upper_velocities) - base));
        PutRef(file, table + 4 + i * 8, 2, static_cast<uint32_t>(keys - base));
    }
    EndBlock(file, data, 0);

    if (waves)
    {
        const std::size_t wave = Block(file, "WAVE", 1), wave_base = wave + 8;
        const std::size_t refs = Grow(file, 4 + waves * 8);
        xsflib::StoreBe32(&file[refs], waves);
        for (uint32_t i = 0; i < waves; i++)
        {
            const std::size_t info = Grow(file, WaveInfoSize(1));
            WaveInfo(file, info, length, 1, false, 32000, i * length * 2, 0);
            PutRef(file, refs + 4 + i * 8, 0, static_cast<uint32_t>(info - wave_base));
        }
        EndBlock(file, wave, 1);
    }

    return file;
}

// Where MakeSequence without labels puts the commands in an RSEQ: after the file's header, the DATA block's header and
// the offset of the commands.
inline constexpr std::size_t kRseqCommands = 0x20 + 8 + 4;

// A test file and the wave data that goes with it.
struct ArchiveFile
{
    std::vector<uint8_t> data;
    std::vector<uint8_t> wave_data = {};
};

// A test sequence sound.
struct SequenceSound
{
    uint32_t file_id = 0;
    uint32_t start_offset = 0;
    uint32_t bank = 0;
    uint8_t pan_mode = 0, pan_curve = 0;
    uint8_t volume = 127;
    uint32_t allocate_track_flags = 1;
};

// Test archive tables: the archive's version, its sequences and banks (by file ID), its sounds' names, and the groups
// that hold its files (by file ID; by default one group holds them all). Defaults allow partial designated initializers
// without warnings.
struct ArchiveLists
{
    uint16_t version = 0x0104;
    std::vector<SequenceSound> sequences = {};
    std::vector<uint32_t> banks = {};
    std::vector<std::string> names = {};
    std::vector<std::vector<uint32_t>> groups = {};
};

// The groups of `lists`, or one group that holds each of the `files` files.
inline std::vector<std::vector<uint32_t>> ArchiveGroups(std::size_t files, const ArchiveLists& lists)
{
    std::vector<std::vector<uint32_t>> groups = lists.groups;
    if (groups.empty())
    {
        groups.emplace_back();
        for (uint32_t id = 0; id < files; id++)
        {
            groups[0].push_back(id);
        }
    }

    return groups;
}

// Adds the SYMB block: an offset, from the block's payload, to a table of offsets, from the payload too, to `names`.
inline void AddSymbols(std::vector<uint8_t>& file, const std::vector<std::string>& names)
{
    const std::size_t symb = Block(file, "SYMB", 0), symb_base = symb + 8;
    Grow(file, 4);
    const std::size_t strings = Grow(file, 4 + names.size() * 4);
    xsflib::StoreBe32(&file[symb_base], static_cast<uint32_t>(strings - symb_base));
    xsflib::StoreBe32(&file[strings], static_cast<uint32_t>(names.size()));
    for (std::size_t i = 0; i < names.size(); i++)
    {
        const std::size_t at = Grow(file, names[i].size() + 1);
        std::memcpy(&file[at], names[i].data(), names[i].size());
        xsflib::StoreBe32(&file[strings + 4 + i * 4], static_cast<uint32_t>(at - symb_base));
    }
    EndBlock(file, symb, 0);
}

// Adds the INFO block's sound table: a sequence sound for each of `sequences`. Sound i takes symbol i as its name, for
// i below `names`.
inline void AddSounds(std::vector<uint8_t>& file, std::size_t base, const std::vector<SequenceSound>& sequences,
                      std::size_t names)
{
    const std::size_t sounds = InfoTable(file, base, base, sequences.size());
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        const SequenceSound& s = sequences[i];
        const std::size_t e = Grow(file, 0x2C), d = Grow(file, 0x10);
        PutRef(file, sounds + i * 8, 0, static_cast<uint32_t>(e - base));
        xsflib::StoreBe32(&file[e], i < names ? static_cast<uint32_t>(i) : 0xffffffff);
        xsflib::StoreBe32(&file[e + 4], s.file_id);
        file[e + 0x14] = s.volume;
        file[e + 0x16] = 1; // a sequence
        PutRef(file, e + 0x18, 1, static_cast<uint32_t>(d - base));
        file[e + 0x28] = s.pan_mode;
        file[e + 0x29] = s.pan_curve;
        xsflib::StoreBe32(&file[d], s.start_offset);
        xsflib::StoreBe32(&file[d + 4], s.bank);
        xsflib::StoreBe32(&file[d + 8], s.allocate_track_flags);
        file[d + 0xC] = 64;
    }
}

// Adds the INFO block's bank table: a bank in each file of `banks`.
inline void AddBanks(std::vector<uint8_t>& file, std::size_t base, const std::vector<uint32_t>& banks)
{
    const std::size_t table = InfoTable(file, base + 8, base, banks.size());
    for (std::size_t i = 0; i < banks.size(); i++)
    {
        const std::size_t e = Grow(file, 0xC);
        PutRef(file, table + i * 8, 0, static_cast<uint32_t>(e - base));
        xsflib::StoreBe32(&file[e], 0xffffffff);
        xsflib::StoreBe32(&file[e + 4], banks[i]);
    }
}

// Adds the INFO block's file table for `files` files: each file's positions, the groups that hold it, and its item in
// each.
inline void AddFilePositions(std::vector<uint8_t>& file, std::size_t base, std::size_t files,
                             const std::vector<std::vector<uint32_t>>& groups)
{
    const std::size_t file_table = InfoTable(file, base + 0x18, base, files);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> positions(files);
    for (uint32_t g = 0; g < groups.size(); g++)
    {
        for (uint32_t k = 0; k < groups[g].size(); k++)
        {
            positions[groups[g][k]].emplace_back(g, k);
        }
    }
    for (std::size_t id = 0; id < files; id++)
    {
        const std::size_t e = Grow(file, 0x1C);
        PutRef(file, file_table + id * 8, 0, static_cast<uint32_t>(e - base));
        const std::size_t places = Grow(file, 4 + positions[id].size() * 8);
        xsflib::StoreBe32(&file[places], static_cast<uint32_t>(positions[id].size()));
        PutRef(file, e + 0x14, 0, static_cast<uint32_t>(places - base));
        for (std::size_t p = 0; p < positions[id].size(); p++)
        {
            const std::size_t position = Grow(file, 8);
            xsflib::StoreBe32(&file[position], positions[id][p].first);
            xsflib::StoreBe32(&file[position + 4], positions[id][p].second);
            PutRef(file, places + 4 + p * 8, 0, static_cast<uint32_t>(position - base));
        }
    }
}

// The INFO block's entries of the groups and of their items, in order. The FILE block fills in where the files are.
struct GroupEntries
{
    std::vector<std::size_t> groups, items;
};

// Adds the INFO block's group table for `groups`.
inline GroupEntries AddGroups(std::vector<uint8_t>& file, std::size_t base,
                              const std::vector<std::vector<uint32_t>>& groups)
{
    const std::size_t group_table = InfoTable(file, base + 0x20, base, groups.size());
    GroupEntries entries;
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const std::size_t e = Grow(file, 0x28);
        PutRef(file, group_table + g * 8, 0, static_cast<uint32_t>(e - base));
        entries.groups.push_back(e);
        const std::size_t items = Grow(file, 4 + groups[g].size() * 8);
        xsflib::StoreBe32(&file[items], static_cast<uint32_t>(groups[g].size()));
        PutRef(file, e + 0x20, 0, static_cast<uint32_t>(items - base));
        for (std::size_t k = 0; k < groups[g].size(); k++)
        {
            const std::size_t item = Grow(file, 0x18);
            xsflib::StoreBe32(&file[item], groups[g][k]);
            PutRef(file, items + 4 + k * 8, 0, static_cast<uint32_t>(item - base));
            entries.items.push_back(item);
        }
    }

    return entries;
}

// Adds the INFO block: references to the sound, bank, player, file and group tables, for `files` files.
inline GroupEntries AddInfo(std::vector<uint8_t>& file, std::size_t files, const ArchiveLists& lists,
                            const std::vector<std::vector<uint32_t>>& groups)
{
    const std::size_t info = Block(file, "INFO", 1), base = info + 8;
    Grow(file, 6 * 8);
    AddSounds(file, base, lists.sequences, lists.names.size());
    AddBanks(file, base, lists.banks);
    InfoTable(file, base + 0x10, base, 0);
    AddFilePositions(file, base, files, groups);
    GroupEntries entries = AddGroups(file, base, groups);
    InfoTable(file, base + 0x28, base, 0);
    EndBlock(file, info, 1);

    return entries;
}

// Adds the FILE block: each group's files, then their wave data. Fills in the groups' `entries`.
inline void AddFiles(std::vector<uint8_t>& file, const std::vector<ArchiveFile>& files,
                     const std::vector<std::vector<uint32_t>>& groups, const GroupEntries& entries)
{
    const std::size_t file_block = Block(file, "FILE", 2);
    std::size_t item_index = 0;
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const std::size_t e = entries.groups[g];
        const std::size_t start = file.size();
        std::vector<std::size_t> offsets;
        for (const uint32_t id : groups[g])
        {
            offsets.push_back(Append(file, files[id].data) - start);
        }

        const std::size_t wave_start = file.size();
        std::vector<std::size_t> wave_offsets;
        for (const uint32_t id : groups[g])
        {
            wave_offsets.push_back(Append(file, files[id].wave_data) - wave_start);
        }

        xsflib::StoreBe32(&file[e + 0x10], static_cast<uint32_t>(start));
        xsflib::StoreBe32(&file[e + 0x14], static_cast<uint32_t>(wave_start - start));
        xsflib::StoreBe32(&file[e + 0x18], static_cast<uint32_t>(wave_start));
        xsflib::StoreBe32(&file[e + 0x1C], static_cast<uint32_t>(file.size() - wave_start));
        for (std::size_t k = 0; k < groups[g].size(); k++)
        {
            const std::size_t item = entries.items[item_index++];
            const ArchiveFile& f = files[groups[g][k]];
            xsflib::StoreBe32(&file[item + 4], static_cast<uint32_t>(offsets[k]));
            xsflib::StoreBe32(&file[item + 8], static_cast<uint32_t>(f.data.size()));
            xsflib::StoreBe32(&file[item + 0xC], static_cast<uint32_t>(wave_offsets[k]));
            xsflib::StoreBe32(&file[item + 0x10], static_cast<uint32_t>(f.wave_data.size()));
        }
    }
    EndBlock(file, file_block, 2);
}

// Builds a sound archive of `files`, with metadata from `lists`. Each group holds its files and then their wave data.
inline std::vector<uint8_t> Rsar(const std::vector<ArchiveFile>& files, const ArchiveLists& lists = {})
{
    std::vector<uint8_t> file = Header("RSAR", lists.version, 3, 0x40);
    const std::vector<std::vector<uint32_t>> groups = ArchiveGroups(files.size(), lists);
    AddSymbols(file, lists.names);
    const GroupEntries entries = AddInfo(file, files.size(), lists, groups);
    AddFiles(file, files, groups, entries);

    return file;
}

// The archive of the tests of a bank's conversion on the Wii. Its sequence plays programs 0 and 2 of the bank, one
// note each. Program 0 plays a mono wave. Program 1 has a region without a wave. Program 2 plays a stereo wave at key
// 64, at `volume` and pan 40, with an envelope of its own. Sound 0 plays the sequence in pan mode 1 and sound 1 in pan
// mode 0.
inline std::vector<uint8_t> BankTestRsar(uint8_t volume)
{
    TestRegion second;
    second.wave_index = 1;
    second.original_key = 64;
    second.volume = volume;
    second.pan = 40;
    second.attack = 120;
    second.decay = 100;
    second.sustain = 90;
    second.hold = 20;
    second.release = 110;
    std::vector<ArchiveFile> files(2);
    files[0].data = MakeSequence({0x81, 0x00, 0x3c, 0x64, 0x30, 0x81, 0x02, 0x40, 0x64, 0x30, 0xff}, {});
    files[1].data = Rbnk({TestInstrument{}, TestInstrument{{{.wave_index = -1}}}, TestInstrument{{second}}});
    files[1].wave_data =
        Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true), Rwav(32, std::vector<int16_t>(32, -2000), 2, true)});

    return Rsar(files, {.sequences = {{.pan_mode = 1}, {.pan_mode = 0}}, .banks = {1}});
}

} // namespace ntdlib::sequence_export::wii_tests
