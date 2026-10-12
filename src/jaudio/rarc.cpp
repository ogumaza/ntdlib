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

std::vector<ArchiveFile> ReadRarc(std::span<const uint8_t> rarc)
{
    const xsflib::Reader r(rarc, true, kReaderOptions);
    if (r.Size() < 0x40 || !r.Magic(0, "RARC"))
    {
        throw xsflib::FormatError("not an RARC archive");
    }

    // Offsets in the info block count from its start at 0x20, and file data from the data offset.
    const std::size_t data = std::size_t{r.U32(0x0c)} + 0x20;
    const uint32_t entries = r.U32(0x28);
    const std::size_t entry_table = std::size_t{r.U32(0x2c)} + 0x20;
    const std::size_t strings = std::size_t{r.U32(0x34)} + 0x20;
    std::vector<ArchiveFile> files;
    for (uint32_t i = 0; i < entries; i++)
    {
        const std::size_t entry = entry_table + 20 * i;
        const uint8_t kind = r.U8(entry + 4);
        if (kind & 0x02)
        {
            continue; // a folder
        }

        ArchiveFile file;
        file.id = r.U16(entry);
        file.name = r.CString(strings + r.U16(entry + 6), 0x100);
        file.data = r.Sub(data + r.U32(entry + 8), r.U32(entry + 12));
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

std::vector<uint8_t> RepackSequenceTable(std::span<const uint8_t> rarc, std::span<const uint8_t> table)
{
    if (IsYaz0(rarc))
    {
        const std::vector<uint8_t> decoded = DecodeYaz0(rarc);
        return RepackSequenceTable(decoded, table);
    }

    const xsflib::Reader r(rarc, true, kReaderOptions);
    const xsflib::Reader sequences(table, true, kReaderOptions);
    if (r.Size() < 0x40 || !r.Magic(0, "RARC"))
    {
        throw xsflib::FormatError("not an RARC archive");
    }

    const std::size_t data = std::size_t{r.U32(0x0c)} + 0x20;
    const uint32_t entries = r.U32(0x28);
    const std::size_t entry_table = std::size_t{r.U32(0x2c)} + 0x20;
    const uint32_t count = sequences.U32(0);
    if (data > rarc.size() || entry_table > data || (data - entry_table) / 20 < entries)
    {
        throw xsflib::FormatError("the sequence archive is damaged");
    }

    // The archive up to its data, then each file's sequence from the table, in the order of the file entries. A table's
    // entries start at 0x20.
    std::vector<uint8_t> out(rarc.begin(), rarc.begin() + static_cast<std::ptrdiff_t>(data));
    for (uint32_t i = 0; i < entries; i++)
    {
        const std::size_t entry = entry_table + 20 * i;
        if (r.U8(entry + 4) & 0x02)
        {
            continue; // a folder
        }

        const uint16_t id = r.U16(entry);
        if (id >= count)
        {
            throw xsflib::FormatError("the sequence table has no sequence for file " + std::to_string(id) +
                                      " of the sequence archive");
        }

        const std::size_t at = 0x20 + std::size_t{id} * 8;
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

} // namespace ntdlib::jaudio
