// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/rarc.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "jaudio/reader.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"

namespace ntdlib::jaudio
{

using detail::kReaderOptions;

namespace
{

// A file entry's size. An entry holds the file's id at +0, its kind at +4, its name's offset among the strings at +6,
// and its data's offset and size at +8 and +12. Bit 1 of the kind marks a folder.
constexpr uint32_t kEntrySize = 20;

// The places that an RARC archive's header gives. Offsets in the info block count from its start at 0x20, and file
// data from the data offset.
struct RarcHeader
{
    std::size_t data = 0;
    uint32_t entries = 0;
    std::size_t entry_table = 0;
    std::size_t strings = 0;
};

RarcHeader ReadHeader(const xsflib::Reader& r)
{
    if (r.Size() < 0x40 || !r.Magic(0, "RARC"))
    {
        throw xsflib::FormatError("not an RARC archive");
    }

    return {.data = std::size_t{r.U32(0x0c)} + 0x20,
            .entries = r.U32(0x28),
            .entry_table = std::size_t{r.U32(0x2c)} + 0x20,
            .strings = std::size_t{r.U32(0x34)} + 0x20};
}

bool IsFolder(const xsflib::Reader& r, std::size_t entry)
{
    return (r.U8(entry + 4) & 0x02) != 0;
}

// The place of entry `index` of a sequence table. The entries start at 0x20. Each holds a sequence's offset, then its
// size.
std::size_t TableEntry(std::size_t index)
{
    return 0x20 + index * 8;
}

} // namespace

std::vector<ArchiveFile> ReadRarc(std::span<const uint8_t> rarc)
{
    const xsflib::Reader r(rarc, true, kReaderOptions);
    const RarcHeader header = ReadHeader(r);
    std::vector<ArchiveFile> files;
    for (uint32_t i = 0; i < header.entries; i++)
    {
        const std::size_t entry = header.entry_table + kEntrySize * i;
        if (IsFolder(r, entry))
        {
            continue;
        }

        ArchiveFile file;
        file.id = r.U16(entry);
        file.name = r.CString(header.strings + r.U16(entry + 6), 0x100);
        file.data = r.Sub(header.data + r.U32(entry + 8), r.U32(entry + 12));
        files.push_back(std::move(file));
    }

    return files;
}

bool IsYaz0(std::span<const uint8_t> data)
{
    return data.size() >= 4 && data[0] == 'Y' && data[1] == 'a' && data[2] == 'z' && data[3] == '0';
}

std::vector<uint8_t> DecodeYaz0(std::span<const uint8_t> data)
{
    // A 16-byte header: "Yaz0", the decoded size and 8 bytes that are unused. The data then comes in groups of eight
    // chunks. A group's first byte has a bit for each chunk, from the top. A set bit marks a byte to copy to the
    // output. A clear bit marks a repeat of earlier output: two bytes, with the distance back less 1 in the low 12 bits
    // and the length less 2 in the top 4. A length of 0 there means that a third byte gives the length less 0x12.
    constexpr uint32_t kMaxSize = 64 << 20;
    const xsflib::Reader r(data, true, kReaderOptions);
    if (!IsYaz0(data) || r.Size() < 16)
    {
        throw xsflib::FormatError("not Yaz0 data");
    }

    const uint32_t size = r.U32(4);
    if (size > kMaxSize)
    {
        throw xsflib::FormatError("Yaz0 data decodes to more than 64 MiB");
    }

    std::vector<uint8_t> out;
    out.reserve(size);
    std::size_t at = 16;
    while (out.size() < size)
    {
        const uint8_t chunks = r.U8(at++);
        for (int bit = 7; bit >= 0 && out.size() < size; bit--)
        {
            if (chunks >> bit & 1)
            {
                out.push_back(r.U8(at++));
                continue;
            }

            const uint16_t repeat = r.U16(at);
            at += 2;
            const std::size_t distance = (repeat & 0xfffu) + std::size_t{1};
            const std::size_t length = repeat >> 12 ? (repeat >> 12) + std::size_t{2} : r.U8(at++) + std::size_t{0x12};
            if (distance > out.size())
            {
                throw xsflib::FormatError("Yaz0 data repeats bytes before its start");
            }

            for (std::size_t i = 0; i < length && out.size() < size; i++)
            {
                out.push_back(out[out.size() - distance]);
            }
        }
    }

    return out;
}

std::span<const uint8_t> DecodeIfYaz0(std::span<const uint8_t> data, std::vector<uint8_t>& decoded)
{
    if (!IsYaz0(data))
    {
        return data;
    }

    decoded = DecodeYaz0(data);

    return decoded;
}

std::vector<uint8_t> RepackSequenceTable(std::span<const uint8_t> rarc, std::span<const uint8_t> table)
{
    if (IsYaz0(rarc))
    {
        const std::vector<uint8_t> decoded = DecodeYaz0(rarc);
        return RepackSequenceTable(decoded, table);
    }

    const xsflib::Reader r(rarc, true, kReaderOptions);
    const xsflib::Reader sequences(table, true, kReaderOptions);
    const RarcHeader header = ReadHeader(r);
    const std::size_t data = header.data;
    const uint32_t count = sequences.U32(0);
    if (data > rarc.size() || header.entry_table > data || (data - header.entry_table) / kEntrySize < header.entries)
    {
        throw xsflib::FormatError("the sequence archive is damaged");
    }

    // The archive up to its data, then each file's sequence from the table, in the order of the file entries.
    std::vector<uint8_t> out(rarc.begin(), rarc.begin() + static_cast<std::ptrdiff_t>(data));
    for (uint32_t i = 0; i < header.entries; i++)
    {
        const std::size_t entry = header.entry_table + kEntrySize * i;
        if (IsFolder(r, entry))
        {
            continue;
        }

        const uint16_t id = r.U16(entry);
        if (id >= count)
        {
            throw xsflib::FormatError("the sequence table has no sequence for file " + std::to_string(id) +
                                      " of the sequence archive");
        }

        const std::size_t at = TableEntry(id);
        const std::span<const uint8_t> sequence = sequences.Sub(sequences.U32(at), sequences.U32(at + 4));
        xsflib::StoreBe32(out.data() + entry + 8, static_cast<uint32_t>(out.size() - data));
        xsflib::StoreBe32(out.data() + entry + 12, static_cast<uint32_t>(sequence.size()));
        out.insert(out.end(), sequence.begin(), sequence.end());
        out.resize(data + (out.size() - data + 0x1f) / 0x20 * 0x20);
    }

    // The header: the archive's size at +4, and the data's size at +0x10 and +0x14. The second size is the data that
    // goes in main memory: all of it in a sequence archive.
    xsflib::StoreBe32(out.data() + 4, static_cast<uint32_t>(out.size()));
    xsflib::StoreBe32(out.data() + 0x10, static_cast<uint32_t>(out.size() - data));
    xsflib::StoreBe32(out.data() + 0x14, static_cast<uint32_t>(out.size() - data));

    return out;
}

std::vector<ArchiveFile> ReadSequenceTable(std::span<const uint8_t> table)
{
    const xsflib::Reader r(table, true, kReaderOptions);
    const uint32_t count = r.U32(0);
    if (table.size() < 0x20 || (table.size() - 0x20) / 8 < count)
    {
        throw xsflib::FormatError("the sequence table is damaged");
    }

    std::vector<ArchiveFile> files;
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t at = TableEntry(i);
        const uint32_t size = r.U32(at + 4);
        if (size != 0)
        {
            files.push_back({static_cast<uint16_t>(i), {}, r.Sub(r.U32(at), size)});
        }
    }

    return files;
}

} // namespace ntdlib::jaudio
