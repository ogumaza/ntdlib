// SPDX-License-Identifier: MIT

// Entries and tables of an archive's INFO block that more than one source of the sar module writes or reads.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/archive_model.h"
#include "ntdlib/sar/reference.h"
#include "sar/format_ids.h"

namespace ntdlib::sar::detail
{

// The entry of `bank`, with the string index `name` or without one: the file, a reference to the list of wave
// archives, the options, then the list.
std::vector<uint8_t> BankEntry(const BankModel& bank, std::optional<uint32_t> name, bool big_endian);

// The entry of `wave_archive`, with the string index `name` or without one: the file, a word with the flag for loading
// the waves one at a time in its first byte, then the options.
std::vector<uint8_t> WaveArchiveEntry(const WaveArchiveModel& wave_archive, std::optional<uint32_t> name,
                                      bool big_endian);

// The entry of `group`, with the string index `name` or without one: the file, then the options.
std::vector<uint8_t> GroupEntry(const GroupModel& group, std::optional<uint32_t> name, bool big_endian);

// The tables of `info`, an InfoBlock, in the order of the block's references to them: the sounds, the sound groups, the
// banks, the wave archives, the groups, the players and the files. The entries of each table but the files hold names.
template <class Info>
auto InfoTables(Info& info)
{
    return std::array{&info.sounds, &info.sound_groups, &info.banks, &info.wave_archives,
                      &info.groups, &info.players,      &info.files};
}

// Where `entry`, a file's entry of the file table, holds the file's offset in the FILE block. The file's size follows
// the offset. Nothing for a file that the block doesn't hold: an external file, or one whose offset is none.
inline std::optional<std::size_t> InternalOffsetAt(const xsflib::Reader& entry)
{
    if (entry.U16(0) != kInternalFile)
    {
        return std::nullopt;
    }

    const std::size_t at = std::size_t{entry.U32(4)} + 4;
    if (entry.U32(at) == kNone)
    {
        return std::nullopt;
    }

    return at;
}

// The groups that the list of `entry`, a file's entry of the file table, names. Nothing for an entry that holds no
// list: an external file's. Throws xsflib::FormatError for an entry that ends before its list does.
inline std::optional<std::vector<uint32_t>> ListOf(std::span<const uint8_t> entry, bool big_endian)
{
    const xsflib::Reader r(entry, big_endian);
    if (r.U16(0) != kInternalFile)
    {
        return std::nullopt;
    }

    const std::size_t location = r.U32(4);
    const Reference list = ReadRef(r, location + 12);
    if (list.type != kListReference)
    {
        return std::nullopt;
    }

    const std::size_t at = location + static_cast<uint32_t>(list.offset);
    std::vector<uint32_t> groups;
    for (uint32_t k = 0; k < r.U32(at); ++k)
    {
        groups.push_back(r.U32(at + 4 + 4 * std::size_t{k}));
    }

    return groups;
}

} // namespace ntdlib::sar::detail
