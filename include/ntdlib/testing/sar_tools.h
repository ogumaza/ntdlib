// SPDX-License-Identifier: MIT

// Builds sound archives for tests as NintendoWare's tools lay them out (ntdlib/sar/tables.h), from the specs of
// ntdlib/testing/sar_files.h. The header takes 64 bytes. The STRG, INFO and FILE blocks follow it without gaps. The
// STRG block lists the names in the order of the INFO block's tables, and an item without a name has none. Each entry
// of the INFO block holds what the tools write. A sound's entry holds its string index, its pan, its priorities and its
// 3D settings, then its details. A sequence sound's details hold its start, its channel priority and its banks, and a
// wave sound's its index and its channel priority. A stream's details are 12 zero bytes. A file's entry on the Wii U
// and the Switch lists no groups. Each file starts on a multiple of 64 bytes from the archive's start on the Switch,
// and of 32 bytes on the other consoles. The writers of ntdlib/sar/tables.h take these archives. They refuse those of
// ntdlib/testing/sar_files.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/string_block.h"
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

    // The options: the string index, the pan, the priorities and the offset of the 3D settings, which follow them.
    std::vector<std::pair<int, uint32_t>> values = names.Option(sound.name, sar::kSoundItem, index);
    values.push_back({1, sound.pan.value_or(0)});
    values.push_back({2, sound.priority.value_or(64)});
    values.push_back({8, 0});
    const std::size_t options = entry.Options(values);
    const std::size_t settings = entry.Add(20);
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
        detail = entry.Add(12);
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

// The entry of `file`, which starts `offset` bytes after the end of the FILE block's size.
inline std::vector<uint8_t> FileEntry(sar::Flavor flavor, const sar_files::FileSpec& file, uint32_t offset)
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
        entry.SizedRef(location, 0x1F00, offset, 0, file.bytes.size());
    }

    // The groups that hold the file.
    if (flavor != sar::Flavor::kCtr)
    {
        const std::size_t groups = entry.Add(12);
        entry.Ref(groups, 0x0100, groups + 8, location);
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
    const uint16_t settings[] = {64, 64, 4, 32, 8, 64, 64, 1, 0, 0};
    for (std::size_t i = 0; i < std::size(settings); ++i)
    {
        const std::size_t shift = big_endian ? 8 : 0;
        info.player_settings[2 * i] = static_cast<uint8_t>(settings[i] >> shift);
        info.player_settings[2 * i + 1] = static_cast<uint8_t>(settings[i] >> (8 - shift));
    }

    // The file table, once the blocks' sizes give the files' places. The entries' sizes don't depend on the places.
    const std::vector<uint8_t> strings = spec.names ? names.block.Write() : std::vector<uint8_t>();
    for (const sar_files::FileSpec& file : spec.files)
    {
        info.files.push_back(detail::FileEntry(flavor, file, 0));
    }
    const std::size_t block = detail::kHeaderSize + strings.size() + info.Write().size();
    const auto [starts, end] = detail::FilePlaces(flavor, spec, block);
    for (std::size_t i = 0; i < spec.files.size(); ++i)
    {
        const uint32_t offset = starts[i] != 0 ? static_cast<uint32_t>(starts[i] - block - 8) : 0;
        info.files[i] = detail::FileEntry(flavor, spec.files[i], offset);
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

} // namespace ntdlib::testing::sar_tools
