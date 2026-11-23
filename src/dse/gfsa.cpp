// SPDX-License-Identifier: MIT

#include "gfsa.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"

#include "common/magic.h"
#include "ntdlib/disc/cx.h"
#include "ntdlib/disc/error.h"

namespace ntdlib::dse
{

using ntdlib::detail::HasMagic;

namespace
{

// A GFSA archive (Professor Layton and the Last Specter, North America, NTR-BLFE-USA: lt4_main_en.fa) starts with a
// header of 0x28 bytes: the magic word, the offsets of the folders' table, of the files' table, of a table that the
// reader doesn't use, of the names and of the files' data, then the count of folders and of files. Each table is data
// in Level-5's compression.
constexpr std::size_t kHeaderSize = 0x28;
constexpr std::size_t kFoldersAt = 0x04;
constexpr std::size_t kFilesAt = 0x08;
constexpr std::size_t kNamesAt = 0x10;
constexpr std::size_t kDataAt = 0x14;
constexpr std::size_t kFolderCountAt = 0x18;
constexpr std::size_t kFileCountAt = 0x1C;

// A folder's entry: the hash of its name, the count of its files and the index of its first file, in 16 bits each,
// then 16 bits of 0. A file's entry: the hash of its name and bits 2 to 17 of its offset from the files' data, in 16
// bits each, then a word: the low 16 bits of its size, bits 18 to 29 of its offset, and bits 16 to 19 of its size.
constexpr std::size_t kEntrySize = 8;

// The most bytes that a table decompresses to.
constexpr std::size_t kMostTable = 4 << 20;

// The hash of a name in the tables: CRC-16/X.25 of its bytes, a folder's with its last '/'.
uint16_t NameHash(const std::string& name)
{
    uint16_t crc = 0xFFFF;
    for (const char c : name)
    {
        crc ^= static_cast<uint8_t>(c);
        for (int bit = 0; bit < 8; bit++)
        {
            crc = (crc & 1) != 0 ? static_cast<uint16_t>(crc >> 1 ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
        }
    }
    return static_cast<uint16_t>(~crc);
}

// The table at the offset that the header gives at `at`, decompressed, when it holds at least `size` bytes.
std::optional<std::vector<uint8_t>> Table(std::span<const uint8_t> archive, std::size_t at, std::size_t size)
{
    const std::size_t offset = xsflib::LoadLe32(&archive[at]);
    if (offset >= archive.size())
    {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> table = DecompressLevel5(archive.subspan(offset), kMostTable);
    if (!table || table->size() < size)
    {
        return std::nullopt;
    }
    return table;
}

// The names of a table of names: strings that end with a zero byte.
std::vector<std::string> Names(const std::vector<uint8_t>& table)
{
    std::vector<std::string> names;
    std::size_t start = 0;
    for (std::size_t at = 0; at < table.size(); at++)
    {
        if (table[at] == 0)
        {
            names.emplace_back(reinterpret_cast<const char*>(&table[start]), at - start);
            start = at + 1;
        }
    }
    return names;
}

} // namespace

std::optional<std::vector<uint8_t>> DecompressLevel5(std::span<const uint8_t> data, std::size_t max_size)
{
    if (data.size() < 4)
    {
        return std::nullopt;
    }
    const uint32_t word = xsflib::LoadLe32(data.data());
    const std::size_t size = word >> 3;
    const std::span<const uint8_t> body = data.subspan(4);
    if (size > max_size)
    {
        return std::nullopt;
    }

    try
    {
        switch (word & 7)
        {
        case 0:
            if (body.size() < size)
            {
                return std::nullopt;
            }
            return std::vector<uint8_t>(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(size));
        case 1:
            return disc::DecompressBody(body, disc::BodyFormat::kLz77, size);
        case 2:
            return disc::DecompressBody(body, disc::BodyFormat::kHuffman4, size);
        case 3:
            return disc::DecompressBody(body, disc::BodyFormat::kHuffman8, size);
        case 4:
            return disc::DecompressBody(body, disc::BodyFormat::kRunLength, size);
        default:
            return std::nullopt;
        }
    }
    catch (const disc::Error&)
    {
        return std::nullopt;
    }
}

std::optional<std::vector<GfsaMember>> GfsaMembers(std::span<const uint8_t> archive)
{
    if (archive.size() < kHeaderSize || !HasMagic(archive, "GFSA"))
    {
        return std::nullopt;
    }
    const std::size_t folder_count = xsflib::LoadLe32(&archive[kFolderCountAt]);
    const std::size_t file_count = xsflib::LoadLe32(&archive[kFileCountAt]);
    const std::size_t data_at = xsflib::LoadLe32(&archive[kDataAt]);
    if (folder_count > kMostTable / kEntrySize || file_count > kMostTable / kEntrySize || data_at > archive.size())
    {
        return std::nullopt;
    }
    const std::optional<std::vector<uint8_t>> folders = Table(archive, kFoldersAt, folder_count * kEntrySize);
    const std::optional<std::vector<uint8_t>> files = Table(archive, kFilesAt, file_count * kEntrySize);
    const std::optional<std::vector<uint8_t>> name_table = Table(archive, kNamesAt, 0);
    if (!folders || !files || !name_table)
    {
        return std::nullopt;
    }

    // The folders by their names' hashes.
    std::map<uint16_t, std::size_t> folder_of_hash;
    for (std::size_t folder = 0; folder < folder_count; folder++)
    {
        folder_of_hash[xsflib::LoadLe16(&(*folders)[kEntrySize * folder])] = folder;
    }

    // The names: each folder's, then the names of each folder's files, the folders in the same order.
    const std::vector<std::string> names = Names(*name_table);
    if (names.size() < folder_count + file_count)
    {
        return std::nullopt;
    }
    std::vector<GfsaMember> members;
    std::size_t next_name = folder_count;
    for (std::size_t n = 0; n < folder_count; n++)
    {
        const auto found = folder_of_hash.find(NameHash(names[n]));
        if (found == folder_of_hash.end())
        {
            return std::nullopt;
        }
        const uint8_t* folder = &(*folders)[kEntrySize * found->second];
        const std::size_t count = xsflib::LoadLe16(folder + 2);
        const std::size_t first = xsflib::LoadLe16(folder + 4);
        if (first + count > file_count || next_name + count > names.size())
        {
            return std::nullopt;
        }

        // The folder's files by their names' hashes.
        std::map<uint16_t, std::size_t> file_of_hash;
        for (std::size_t file = first; file < first + count; file++)
        {
            file_of_hash[xsflib::LoadLe16(&(*files)[kEntrySize * file])] = file;
        }
        for (std::size_t i = 0; i < count; i++)
        {
            const std::string& name = names[next_name++];
            const auto file = file_of_hash.find(NameHash(name));
            if (file == file_of_hash.end())
            {
                return std::nullopt;
            }
            const uint8_t* entry = &(*files)[kEntrySize * file->second];
            const uint32_t word = xsflib::LoadLe32(entry + 4);
            const std::size_t offset = std::size_t{xsflib::LoadLe16(entry + 2)} << 2 | std::size_t{word >> 16 & 0xFFF}
                                                                                           << 18;
            const std::size_t size = (word & 0xFFFF) | std::size_t{word >> 28} << 16;
            if (offset > archive.size() - data_at || size > archive.size() - data_at - offset)
            {
                return std::nullopt;
            }
            members.push_back({names[n] + name, archive.subspan(data_at + offset, size)});
        }
    }
    return members;
}

} // namespace ntdlib::dse
