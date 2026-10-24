// SPDX-License-Identifier: MIT

// Builds sound archives for tests as NintendoWare's tools lay them out (ntdlib/sar/tables.h), from the specs of
// ntdlib/testing/sar_files.h. The header takes 64 bytes. The STRG, INFO and FILE blocks follow it without gaps. The
// STRG block lists the names in the order of the INFO block's tables, and an item without a name has none. Each entry
// of the INFO block holds what the tools write. A sound's entry holds its string index, its pan, its priorities and its
// 3D settings, then its details. A sequence sound and a wave sound hold a 32-bit value between the two. A sequence
// sound's details hold its start, its channel priority and its banks. A wave sound's details hold its index and its
// channel priority. A stream's details are those of sar_files.h's StreamDetails, as FSAR 2.2.0 lays them out, or 12
// zero bytes for a spec without them. A file's entry on the Wii U and the Switch lists the groups that its spec names.
// The reference to a group's file there has type 0, and the reference to any other file type 0x1F00. The player's
// settings give the size of a stream's buffer in a byte, and a zero follows it. Each file starts on a multiple of 64
// bytes from the archive's start on the Switch, and of 32 bytes on the other consoles. The writers of
// ntdlib/sar/tables.h take these archives. They refuse those of ntdlib/testing/sar_files.h. Group builds a group's file
// as the tools lay it out (ntdlib/sar/groups.h). WithoutValuePlaces takes optional values out of the sounds' entries of
// such an archive, for the tests of sar::WithValuePlaces. Bank builds a bank file as the tools lay one out
// (ntdlib/sar/bank_edit.h).

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/testing/sar_files.h"

namespace ntdlib::testing::sar_tools
{
namespace detail
{

constexpr uint32_t kNone = 0xFFFFFFFF;
constexpr std::size_t kHeaderSize = 64;

// The names of an archive in the order of the INFO block's tables.
class Names
{
public:
    explicit Names(sar::Flavor flavor)
    {
        block.flavor = flavor;
    }

    // The option that holds the string index of item `index` of type `type`, named `name`. Nothing for an item without
    // a name.
    std::vector<std::pair<int, uint32_t>> Option(const std::string& name, uint32_t type, std::size_t index)
    {
        if (name.empty())
        {
            return {};
        }

        block.names.push_back({name, sar::ItemId(type, static_cast<uint32_t>(index))});

        return {{0, static_cast<uint32_t>(block.names.size() - 1)}};
    }

    sar::StringBlock block;
};

// The entry of `sound`, sound `index` of the archive.
inline std::vector<uint8_t> SoundEntry(sar::Flavor flavor, const sar_files::SoundSpec& sound, std::size_t index,
                                       Names& names)
{
    sar_files::Bytes entry(flavor == sar::Flavor::kCafe);
    entry.Add(20);
    entry.U32(0, sound.file);
    entry.U32(4, sar::ItemId(sar::kPlayerItem, sound.player));
    entry.U8(8, sound.volume);

    // The options: the string index, the pan, the priorities and the offset of the 3D settings, which follow them. A
    // sequence sound and a wave sound hold a 32-bit value of 0 after the 3D settings. A stream sound holds none.
    std::vector<std::pair<int, uint32_t>> values = names.Option(sound.name, sar::kSoundItem, index);
    values.push_back({1, sound.pan.value_or(0)});
    values.push_back({2, sound.priority.value_or(64)});
    values.push_back({8, 0});
    const std::size_t options = entry.Options(values);
    const std::size_t settings = entry.Add(sound.kind == sar_files::SoundSpec::kStream ? 16 : 20);
    entry.U32(options + 4 * values.size(), static_cast<uint32_t>(settings));
    entry.U32(settings, 0xF);
    entry.U32(settings + 4, 0x3F000000); // a decay ratio of 0.5
    entry.U8(settings + 8, 1);

    // The details.
    std::size_t detail = 0;
    switch (sound.kind)
    {
    case sar_files::SoundSpec::kSequence:
        {
            detail = entry.Add(12);
            entry.U32(detail + 8, sound.allocate_track_flags);
            entry.Options({{0, sound.start_offset.value_or(0)}, {1, sound.channel_priority.value_or(64)}});
            const std::size_t banks = entry.Add(4 + 4 * sound.banks.size());
            entry.Ref(detail, 0x0100, banks, detail);
            entry.U32(banks, static_cast<uint32_t>(sound.banks.size()));
            for (std::size_t k = 0; k < sound.banks.size(); ++k)
            {
                const uint32_t bank = sound.banks[k];
                entry.U32(banks + 4 + 4 * k, bank == kNone ? kNone : sar::ItemId(sar::kBankItem, bank));
            }
            break;
        }

    case sar_files::SoundSpec::kWaveSound:
        detail = entry.Add(8);
        entry.U32(detail, sound.wave_sound_index);
        entry.U32(detail + 4, 1);
        entry.Options({{0, sound.channel_priority.value_or(64)}});
        break;

    case sar_files::SoundSpec::kStream:
        detail = sound.stream ? sar_files::StreamDetails(entry, *sound.stream) : entry.Add(12);
        break;
    }
    entry.Ref(12, static_cast<uint16_t>(sound.kind), detail, 0);

    return entry.data;
}

// The entry of an item that holds its file's ID, then a reference to a list of 32-bit values, then its options: a bank
// and its list of wave archives, which the specs leave empty.
inline std::vector<uint8_t> BankEntry(sar::Flavor flavor, uint32_t file, std::vector<std::pair<int, uint32_t>> options)
{
    sar_files::Bytes entry(flavor == sar::Flavor::kCafe);
    entry.Add(12);
    entry.U32(0, file);
    entry.Options(options);
    const std::size_t list = entry.Add(4);
    entry.Ref(4, 0x0100, list, 0);

    return entry.data;
}

// The entry of an item that holds a 32-bit value or two, then its options: a wave archive's file and its count of
// waves, a group's file, and a player's limit of sounds.
inline std::vector<uint8_t> PlainEntry(sar::Flavor flavor, const std::vector<uint32_t>& fields,
                                       std::vector<std::pair<int, uint32_t>> options)
{
    sar_files::Bytes entry(flavor == sar::Flavor::kCafe);
    entry.Add(4 * fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        entry.U32(4 * i, fields[i]);
    }
    entry.Options(options);

    return entry.data;
}

// The entry of `file`, which starts `offset` bytes after the end of the FILE block's size. `group_file` gives whether
// the file is a group's file.
inline std::vector<uint8_t> FileEntry(sar::Flavor flavor, const sar_files::FileSpec& file, uint32_t offset,
                                      bool group_file)
{
    sar_files::Bytes entry(flavor == sar::Flavor::kCafe);
    entry.Add(12);
    if (!file.external.empty())
    {
        const std::size_t path = entry.Add(file.external.size() + 1);
        std::memcpy(entry.data.data() + path, file.external.data(), file.external.size());
        entry.Align(4);
        entry.Ref(0, 0x220D, path, 0);

        return entry.data;
    }

    // The location: the file's offset and size, or none for a file that only a group holds.
    const std::size_t location = entry.Add(12);
    entry.Ref(0, 0x220C, location, 0);
    if (file.group_only)
    {
        entry.U32(location + 4, kNone);
        entry.U32(location + 8, kNone);
    }
    else
    {
        entry.SizedRef(location, group_file ? 0 : 0x1F00, offset, 0, file.bytes.size());
    }

    // The groups that hold the file.
    if (flavor != sar::Flavor::kCtr)
    {
        const std::size_t groups = entry.Add(12 + 4 * file.groups.size());
        entry.Ref(groups, 0x0100, groups + 8, location);
        entry.U32(groups + 8, static_cast<uint32_t>(file.groups.size()));
        for (std::size_t k = 0; k < file.groups.size(); ++k)
        {
            entry.U32(groups + 12 + 4 * k, sar::ItemId(sar::kGroupItem, file.groups[k]));
        }
    }

    return entry.data;
}

// Where each internal file of `spec` starts, from the archive's start, in a FILE block at `block`. The second value is
// the archive's end.
inline std::pair<std::vector<std::size_t>, std::size_t> FilePlaces(sar::Flavor flavor,
                                                                   const sar_files::ArchiveSpec& spec,
                                                                   std::size_t block)
{
    const std::size_t alignment = flavor == sar::Flavor::kNx ? 64 : 32;
    const auto align = [alignment](std::size_t at)
    {
        return (at + alignment - 1) / alignment * alignment;
    };
    std::vector<std::size_t> starts;
    std::size_t end = align(block + 8);
    for (const sar_files::FileSpec& file : spec.files)
    {
        const bool internal = file.external.empty() && !file.group_only;
        starts.push_back(internal ? align(end) : 0);
        if (internal)
        {
            end = starts.back() + file.bytes.size();
        }
    }

    return {starts, end};
}

} // namespace detail

// A group's file: the files that it holds, each with the bytes of its copy or as a link, and the items that it lists,
// each with the bits of the parts that the group loads. A group's file of the 3DS takes the version `ctr_version`:
// 1.0.0.0, which holds no INFX block and lists no items, or 1.1.0.0, which holds INFX. Without INFX, the spec's items
// stay out.
struct GroupSpec
{
    std::vector<std::pair<uint32_t, std::optional<std::vector<uint8_t>>>> files;
    std::vector<std::pair<uint32_t, uint32_t>> items = {};
    bool padded = true; // whether zeros follow INFX up to the alignment
    uint32_t ctr_version = 0x01000000;
};

namespace detail
{

// The group's file of `spec`, laid out as the tools lay it out: a header of 64 bytes, INFO padded to 32 bytes, FILE
// with each copy on a multiple of 64 bytes on the Switch and of 32 on the other consoles, then INFX. Without INFX, FILE
// ends on a multiple of 32 bytes. The second value is the size that an archive's file table gives the file: the end of
// INFX's entries, or without INFX the end of the last copy, or the file's size without copies.
inline std::pair<std::vector<uint8_t>, std::size_t> GroupAndSize(sar::Flavor flavor, const GroupSpec& spec)
{
    const bool ctr = flavor == sar::Flavor::kCtr;
    const bool has_items = !ctr || spec.ctr_version >= 0x01010000;
    const std::size_t alignment = flavor == sar::Flavor::kNx ? 64 : 32;
    const auto align = [](std::size_t at, std::size_t to)
    {
        return (at + to - 1) / to * to;
    };
    const std::size_t files = spec.files.size();
    const std::size_t items = has_items ? spec.items.size() : 0;

    // The blocks and the copies, by hand.
    const std::size_t kInfo = kHeaderSize;
    const std::size_t file = kInfo + align(8 + 4 + 24 * files, 32);
    std::vector<std::size_t> copies;
    std::optional<std::size_t> copies_end;
    std::size_t at = align(file + 8, alignment);
    for (const auto& [id, copy] : spec.files)
    {
        copies.push_back(copy ? at : 0);
        if (copy)
        {
            copies_end = at + copy->size();
        }
        at = copy ? align(at + copy->size(), alignment) : at;
    }
    const std::size_t infx = at;
    const std::size_t entries_end = infx + 8 + 4 + 16 * items;
    const std::size_t end = !has_items ? infx : spec.padded ? align(entries_end, alignment) : entries_end;

    sar_files::Bytes group(flavor == sar::Flavor::kCafe);
    group.Add(end);
    std::memcpy(group.data.data(), ctr ? "CGRP" : "FGRP", 4);
    group.U16(4, 0xFEFF);
    group.U16(6, static_cast<uint16_t>(kHeaderSize));
    group.U32(8, ctr ? spec.ctr_version : 0x00010000);
    group.U32(0x0C, static_cast<uint32_t>(end));
    const std::size_t block_count = has_items ? 3 : 2;
    group.U16(0x10, static_cast<uint16_t>(block_count));
    const std::size_t blocks[][3] = {
        {0x7800, kInfo, file - kInfo}, {0x7801, file, infx - file}, {0x7802, infx, end - infx}};
    const char* magics[] = {"INFO", "FILE", "INFX"};
    for (std::size_t i = 0; i < block_count; ++i)
    {
        group.U16(0x14 + 12 * i, static_cast<uint16_t>(blocks[i][0]));
        group.U32(0x18 + 12 * i, static_cast<uint32_t>(blocks[i][1]));
        group.U32(0x1C + 12 * i, static_cast<uint32_t>(blocks[i][2]));
        std::memcpy(group.data.data() + blocks[i][1], magics[i], 4);
        group.U32(blocks[i][1] + 4, static_cast<uint32_t>(blocks[i][2]));
    }

    // INFO: each file's entry, with its copy's place in FILE's body, or no place and no size for a link.
    group.U32(kInfo + 8, static_cast<uint32_t>(files));
    for (std::size_t i = 0; i < files; ++i)
    {
        const std::size_t entry = kInfo + 12 + 8 * files + 16 * i;
        group.Ref(kInfo + 12 + 8 * i, 0x7900, entry, kInfo + 8);
        group.U32(entry, spec.files[i].first);
        if (const std::optional<std::vector<uint8_t>>& copy = spec.files[i].second)
        {
            group.SizedRef(entry + 4, 0x1F00, copies[i], file + 8, copy->size());
            std::memcpy(group.data.data() + copies[i], copy->data(), copy->size());
        }
        else
        {
            group.NullRef(entry + 4, 0);
            group.U32(entry + 12, kNone);
        }
    }
    if (!has_items)
    {
        return {group.data, copies_end.value_or(end)};
    }

    // INFX: each item's entry.
    group.U32(infx + 8, static_cast<uint32_t>(items));
    for (std::size_t i = 0; i < items; ++i)
    {
        const std::size_t entry = infx + 12 + 8 * items + 8 * i;
        group.Ref(infx + 12 + 8 * i, 0x7901, entry, infx + 8);
        group.U32(entry, spec.items[i].first);
        group.U32(entry + 4, spec.items[i].second);
    }

    return {group.data, entries_end};
}

} // namespace detail

// The group's file of `spec`, laid out as the tools lay it out (detail::GroupAndSize).
inline std::vector<uint8_t> Group(sar::Flavor flavor, const GroupSpec& spec)
{
    return detail::GroupAndSize(flavor, spec).first;
}

// The bytes of a group's file as an archive's FILE block holds them: up to the end of INFX's entries, or without INFX
// up to the end of the last copy. The zeros after them pad the file to the next file.
inline std::vector<uint8_t> GroupInArchive(sar::Flavor flavor, const GroupSpec& spec)
{
    auto [group, size] = detail::GroupAndSize(flavor, spec);
    group.resize(size);

    return group;
}

// The archive of `spec`, laid out as the tools lay it out. Without names, the header lists the STRG block at
// 0xFFFFFFFF, and the entries keep their string indices.
inline std::vector<uint8_t> Archive(sar::Flavor flavor, const sar_files::ArchiveSpec& spec)
{
    const bool big_endian = flavor == sar::Flavor::kCafe;

    // The entries in the order of the tables, and the names in the same order.
    detail::Names names(flavor);
    sar::InfoBlock info;
    info.flavor = flavor;
    for (std::size_t i = 0; i < spec.sounds.size(); ++i)
    {
        info.sounds.push_back(detail::SoundEntry(flavor, spec.sounds[i], i, names));
    }
    for (std::size_t i = 0; i < spec.banks.size(); ++i)
    {
        const auto& [name, file] = spec.banks[i];
        info.banks.push_back(detail::BankEntry(flavor, file, names.Option(name, sar::kBankItem, i)));
    }
    for (std::size_t i = 0; i < spec.wave_archives.size(); ++i)
    {
        const auto& [name, file] = spec.wave_archives[i];
        info.wave_archives.push_back(
            detail::PlainEntry(flavor, {file, 0}, names.Option(name, sar::kWaveArchiveItem, i)));
    }
    for (std::size_t i = 0; i < spec.groups.size(); ++i)
    {
        const auto& [name, file] = spec.groups[i];
        info.groups.push_back(detail::PlainEntry(flavor, {file}, names.Option(name, sar::kGroupItem, i)));
    }
    for (std::size_t i = 0; i < spec.players.size(); ++i)
    {
        const auto& [name, limit] = spec.players[i];
        std::vector<std::pair<int, uint32_t>> options = names.Option(name, sar::kPlayerItem, i);
        options.push_back({1, 0}); // the player's heap
        info.players.push_back(detail::PlainEntry(flavor, {limit}, options));
    }
    const uint16_t kSettings[] = {64, 64, 4, 32, 8, 64, 64};
    for (std::size_t i = 0; i < std::size(kSettings); ++i)
    {
        const std::size_t shift = big_endian ? 8 : 0;
        info.player_settings[2 * i] = static_cast<uint8_t>(kSettings[i] >> shift);
        info.player_settings[2 * i + 1] = static_cast<uint8_t>(kSettings[i] >> (8 - shift));
    }
    info.player_settings[2 * std::size(kSettings)] = 1; // the size of a stream's buffer

    // The file table, once the blocks' sizes give the files' places. The entries' sizes don't depend on the places.
    const std::vector<uint8_t> strings = spec.names ? names.block.Write() : std::vector<uint8_t>();
    std::vector<bool> group_files(spec.files.size());
    for (const auto& [name, file] : spec.groups)
    {
        if (file < group_files.size())
        {
            group_files[file] = true;
        }
    }
    for (std::size_t i = 0; i < spec.files.size(); ++i)
    {
        info.files.push_back(detail::FileEntry(flavor, spec.files[i], 0, group_files[i]));
    }
    const std::size_t block = detail::kHeaderSize + strings.size() + info.Write().size();
    const auto [starts, end] = detail::FilePlaces(flavor, spec, block);
    for (std::size_t i = 0; i < spec.files.size(); ++i)
    {
        const uint32_t offset = starts[i] != 0 ? static_cast<uint32_t>(starts[i] - block - 8) : 0;
        info.files[i] = detail::FileEntry(flavor, spec.files[i], offset, group_files[i]);
    }
    const std::vector<uint8_t> tables = info.Write();

    // The header, the blocks and the files.
    sar_files::Bytes archive(big_endian);
    archive.Add(detail::kHeaderSize);
    std::memcpy(archive.data.data(), flavor == sar::Flavor::kCtr ? "CSAR" : "FSAR", 4);
    archive.U16(4, 0xFEFF);
    archive.U16(6, static_cast<uint16_t>(detail::kHeaderSize));
    archive.U32(8, flavor == sar::Flavor::kCtr ? 0x02030000 : 0x00020200);
    archive.U16(0x10, 3);
    const uint32_t string_place = spec.names ? static_cast<uint32_t>(archive.Add(strings)) : detail::kNone;
    const std::size_t info_place = archive.Add(tables);
    archive.Add(end - block);
    std::memcpy(archive.data.data() + block, "FILE", 4);
    for (std::size_t i = 0; i < spec.files.size(); ++i)
    {
        if (starts[i] != 0)
        {
            std::memcpy(archive.data.data() + starts[i], spec.files[i].bytes.data(), spec.files[i].bytes.size());
        }
    }
    archive.U32(block + 4, static_cast<uint32_t>(end - block));
    const uint32_t block_entries[][3] = {
        {0x2000, string_place, static_cast<uint32_t>(strings.size())},
        {0x2001, static_cast<uint32_t>(info_place), static_cast<uint32_t>(tables.size())},
        {0x2002, static_cast<uint32_t>(block), static_cast<uint32_t>(end - block)},
    };
    for (std::size_t i = 0; i < 3; ++i)
    {
        archive.U16(0x14 + 12 * i, static_cast<uint16_t>(block_entries[i][0]));
        archive.U32(0x18 + 12 * i, block_entries[i][1]);
        archive.U32(0x1C + 12 * i, block_entries[i][2]);
    }
    archive.U32(0x0C, static_cast<uint32_t>(archive.Size()));

    return archive.data;
}

// `archive`, an archive of `flavor` laid out as the tools lay it out, without the options of the values in `places`, by
// the sounds' places in the sound table. It undoes sar::WithValuePlaces. An entry that holds such a value's option
// loses its bit and its value. The 3D settings and the details move 4 bytes back, and the offset of the 3D settings and
// the reference to the details follow them. A sequence sound's details lose the channel priority the same way, and the
// details' reference to the list of banks follows the list. The reader then reads what it reads without the options.
// An option that an entry leaves out stays out. Throws std::out_of_range for a sound past the sound table, and the
// exceptions of sar::WithTables.
inline std::vector<uint8_t> WithoutValuePlaces(std::span<const uint8_t> archive, sar::Flavor flavor,
                                               const std::map<uint32_t, sar::ValuePlaces>& places)
{
    const bool big_endian = flavor == sar::Flavor::kCafe;
    const sar::Tables tables = sar::TablesOf(archive, flavor);
    std::optional<sar::StringBlock> strings;
    if (tables.strings)
    {
        strings = sar::StringBlock::Read(*tables.strings, flavor);
    }
    sar::InfoBlock info = sar::InfoBlock::Read(tables.info, flavor);

    for (const auto& [sound, values] : places)
    {
        std::vector<uint8_t>& entry = info.sounds.at(sound);
        const auto load = [&entry, big_endian](std::size_t at)
        {
            return xsflib::Reader(entry, big_endian).U32(at);
        };
        const auto store = [&entry, big_endian](std::size_t at, uint32_t value)
        {
            big_endian ? xsflib::StoreBe32(&entry[at], value) : xsflib::StoreLe32(&entry[at], value);
        };
        const auto add = [&](std::size_t at, int64_t change)
        {
            store(at, static_cast<uint32_t>(load(at) + change));
        };

        // Takes out the value of option `option` of the options at `options`, if the entry holds it.
        const auto take_out = [&](std::size_t options, uint32_t option)
        {
            const uint32_t bits = load(options);
            if ((bits & option) == 0)
            {
                return false;
            }

            const std::size_t at = options + 4 + 4 * static_cast<std::size_t>(std::popcount(bits & (option - 1)));
            entry.erase(entry.begin() + static_cast<std::ptrdiff_t>(at),
                        entry.begin() + static_cast<std::ptrdiff_t>(at + 4));
            store(options, bits & ~option);

            return true;
        };

        // The sound's options start 20 bytes in. The reference to the details lies 12 bytes in, and its offset 16.
        // Bit 8 gives the offset of the 3D settings.
        const auto sound_option = [&](uint32_t option)
        {
            if (take_out(20, option))
            {
                add(16, -4);
                const uint32_t bits = load(20);
                if ((bits & 0x100) != 0)
                {
                    add(24 + 4 * static_cast<std::size_t>(std::popcount(bits & 0xFF)), -4);
                }
            }
        };
        if (values.pan)
        {
            sound_option(1u << 1);
        }
        if (values.priority)
        {
            sound_option(1u << 2);
        }

        // A sequence sound's details start with the reference to its list of banks. Its options lie 12 bytes in, and
        // bit 1 gives its channel priority.
        if (values.channel && xsflib::Reader(entry, big_endian).U16(12) == 0x2203)
        {
            const std::size_t details = load(16);
            if (take_out(details + 12, 1u << 1))
            {
                add(details + 4, -4);
            }
        }
    }

    return sar::WithTables(archive, flavor, strings, std::move(info));
}

// The bank file of `spec`, laid out as the tools lay a bank out (ntdlib/sar/bank_edit.h). The header takes 32 bytes,
// and INFO follows it. INFO's body holds the references to the table of waves and to the table of instruments, then the
// table of instruments, then the instruments, then the table of waves. The block and the file end there. A reference to
// no instrument has type 0x5903. The instruments go in the order of the programs in `order`, or else in the order of
// their programs. Their regions are those of sar_files.h's Bank.
inline std::vector<uint8_t> Bank(sar::Flavor flavor, const sar_files::BankSpec& spec,
                                 std::vector<std::size_t> order = {})
{
    const bool big_endian = sar_files::detail::BigEndian(flavor);
    const bool by_program = order.empty();
    for (std::size_t i = 0; i < spec.instruments.size(); ++i)
    {
        if (by_program && spec.instruments[i])
        {
            order.push_back(i);
        }
    }

    // INFO's body: the references, the table of instruments, the instruments and the table of waves.
    sar_files::Bytes info(big_endian);
    info.Add(16);
    const std::size_t instruments = info.Add(4 + 8 * spec.instruments.size());
    info.Ref(8, 0x0101, instruments, 0);
    info.U32(instruments, static_cast<uint32_t>(spec.instruments.size()));
    for (std::size_t i = 0; i < spec.instruments.size(); ++i)
    {
        info.NullRef(instruments + 4 + 8 * i, 0x5903);
    }
    for (const std::size_t program : order)
    {
        const std::size_t instrument = info.Add(8);
        info.Ref(instruments + 4 + 8 * program, 0x5900, instrument, instruments);
        sar_files::detail::PutTable(info, instrument, *spec.instruments[program], sar_files::detail::PutVelocityTable);
    }
    const std::size_t waves = info.Add(4 + 8 * spec.waves.size());
    info.Ref(0, 0x0100, waves, 0);
    info.U32(waves, static_cast<uint32_t>(spec.waves.size()));
    for (std::size_t i = 0; i < spec.waves.size(); ++i)
    {
        info.U32(waves + 4 + 8 * i, spec.waves[i].first);
        info.U32(waves + 8 + 8 * i, spec.waves[i].second);
    }

    // The header with its table of one block, then the block.
    sar_files::Bytes file(big_endian);
    file.Add(0x20);
    std::memcpy(file.data.data(), sar_files::detail::Magic(flavor, "BNK").c_str(), 4);
    file.U16(4, 0xFEFF);
    file.U16(6, 0x20);
    file.U32(8, sar_files::detail::Version(flavor, 0x01000100, 0x00010000));
    file.U16(0x10, 1);
    file.U16(0x14, 0x5800);
    file.U32(0x18, 0x20);
    file.U32(0x1C, static_cast<uint32_t>(8 + info.Size()));
    const std::size_t block = file.Add(8);
    std::memcpy(file.data.data() + block, "INFO", 4);
    file.U32(block + 4, static_cast<uint32_t>(8 + info.Size()));
    file.Add(info.data);
    file.U32(0x0C, static_cast<uint32_t>(file.Size()));

    return file.data;
}

} // namespace ntdlib::testing::sar_tools
