// SPDX-License-Identifier: MIT

// Reads CPK archives and decompresses CRILAYLA data (see cpk.h). An @UTF table starts with a header of 0x20 bytes: the
// magic "@UTF", the size of the rest of the table in 32 bits, 2 bytes that ListCpk doesn't read, the rows' offset in 16
// bits, the offsets of the strings and of the data area in 32 bits, the offset of the table's name among the strings
// in 32 bits, the number of columns and the size of a row in 16 bits, and the number of rows in 32 bits. The numbers
// are big-endian. The header's offsets count from 8 bytes into the table. The columns follow the header. Each column
// starts with a flag byte. The flag byte's high 4 bits say how the column stores its value. The low 4 bits give the
// value's type. The offset of the column's name among the strings follows if the flags hold 0x10. A value that every
// row shares follows if the flags hold 0x20. With 0x40, each row holds a value for the column, after the values of the
// columns before it. A column with neither 0x20 nor 0x40 holds no value. A string is the offset of a NUL-terminated
// string among the strings. A data value is an offset in the data area and a size. A CPK may store its tables masked.

#include "ntdlib/disc/cpk.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "disc/encodings.h"
#include "disc/file_table.h"
#include "ntdlib/disc/error.h"

namespace ntdlib::disc
{
namespace
{

using detail::kMaxFileSize;

constexpr char kNotCpk[] = "the data isn't a CPK";
constexpr char kDamaged[] = "the CPK is damaged";

// A chunk's header, and an @UTF table's header. The table header's offsets count from kOffsetBase.
constexpr std::size_t kChunkHeader = 16;
constexpr std::size_t kTableHeader = 0x20;
constexpr std::size_t kOffsetBase = 8;

// How a column stores its value: the high 4 bits of its flag byte.
constexpr uint8_t kNamed = 0x10;
constexpr uint8_t kShared = 0x20;
constexpr uint8_t kPerRow = 0x40;

// The types of a column's value: the low 4 bits of its flag byte.
enum class ValueType : uint8_t
{
    kU8,
    kS8,
    kU16,
    kS16,
    kU32,
    kS32,
    kU64,
    kS64,
    kFloat,
    kDouble,
    kString,
    kData,
};

// CRILAYLA data: the magic and two sizes, the compressed stream, then kRawSize bytes stored as they are.
constexpr char kCrilaylaMagic[] = "CRILAYLA";
constexpr std::size_t kCrilaylaHeader = 0x10;
constexpr std::size_t kRawSize = 0x100;

// The bytes that a value of type `type` takes in a column or a row.
std::size_t ValueSize(ValueType type)
{
    switch (type)
    {
    case ValueType::kU8:
    case ValueType::kS8:
        return 1;
    case ValueType::kU16:
    case ValueType::kS16:
        return 2;
    case ValueType::kU32:
    case ValueType::kS32:
    case ValueType::kFloat:
    case ValueType::kString:
        return 4;
    case ValueType::kU64:
    case ValueType::kS64:
    case ValueType::kDouble:
    case ValueType::kData:
        return 8;
    }

    throw Error(kDamaged);
}

// A signed number as unsigned. A negative number in a table is damage.
uint64_t NonNegative(int64_t value)
{
    if (value < 0)
    {
        throw Error(kDamaged);
    }

    return static_cast<uint64_t>(value);
}

bool StartsWithUtf(const std::vector<uint8_t>& bytes)
{
    return bytes.size() >= 4 && std::equal(bytes.begin(), bytes.begin() + 4, "@UTF");
}

// Unmasks a table that a CPK stores masked. Each byte is XORed with the low byte of a number. The number starts at
// 0x655F and is multiplied by 0x4115 after each byte, modulo 2^32.
void Unmask(std::vector<uint8_t>& bytes)
{
    uint32_t mask = 0x655F;
    for (uint8_t& byte : bytes)
    {
        byte = static_cast<uint8_t>(byte ^ mask);
        mask *= 0x4115;
    }
}

// An @UTF table: named columns, and rows that hold the columns' values.
class UtfTable
{
public:
    // Parses `bytes`, a chunk's table, after unmasking it if it's masked. Throws Error if the table is damaged.
    explicit UtfTable(std::vector<uint8_t> bytes) : bytes_(std::move(bytes))
    {
        if (!StartsWithUtf(bytes_))
        {
            Unmask(bytes_);
        }
        if (bytes_.size() < kTableHeader || !StartsWithUtf(bytes_))
        {
            throw Error(kDamaged);
        }

        const uint64_t end = kOffsetBase + uint64_t{xsflib::LoadBe32(&bytes_[0x04])};
        const uint64_t rows = kOffsetBase + uint64_t{xsflib::LoadBe16(&bytes_[0x0A])};
        const uint64_t strings = kOffsetBase + uint64_t{xsflib::LoadBe32(&bytes_[0x0C])};
        const uint64_t data = kOffsetBase + uint64_t{xsflib::LoadBe32(&bytes_[0x10])};
        row_size_ = xsflib::LoadBe16(&bytes_[0x1A]);
        row_count_ = xsflib::LoadBe32(&bytes_[0x1C]);
        if (end < kTableHeader || end > bytes_.size() || rows > end || strings > end || data > end ||
            uint64_t{row_count_} * row_size_ > end - rows)
        {
            throw Error(kDamaged);
        }

        end_ = static_cast<std::size_t>(end);
        rows_ = static_cast<std::size_t>(rows);
        strings_ = static_cast<std::size_t>(strings);
        ReadColumns(xsflib::LoadBe16(&bytes_[0x18]));
    }

    uint32_t Rows() const
    {
        return row_count_;
    }

    // Column `name`'s number in row `row`, or nothing if the table has no such column or the column holds no value.
    // Throws Error if the column holds something other than an integer, or a negative integer.
    std::optional<uint64_t> Number(uint32_t row, std::string_view name) const
    {
        const Column* column = Find(name);
        const std::optional<std::size_t> at = column ? Place(*column, row) : std::nullopt;
        if (!at)
        {
            return std::nullopt;
        }

        const uint8_t* value = &bytes_[*at];
        switch (column->type)
        {
        case ValueType::kU8:
            return value[0];
        case ValueType::kS8:
            return NonNegative(static_cast<int8_t>(value[0]));
        case ValueType::kU16:
            return xsflib::LoadBe16(value);
        case ValueType::kS16:
            return NonNegative(static_cast<int16_t>(xsflib::LoadBe16(value)));
        case ValueType::kU32:
            return xsflib::LoadBe32(value);
        case ValueType::kS32:
            return NonNegative(static_cast<int32_t>(xsflib::LoadBe32(value)));
        case ValueType::kU64:
            return xsflib::LoadBe64(value);
        case ValueType::kS64:
            return NonNegative(static_cast<int64_t>(xsflib::LoadBe64(value)));
        default:
            throw Error(kDamaged);
        }
    }

    // Column `name`'s string in row `row`, or nothing if the table has no such column or the column holds no value.
    // Throws Error if the column holds something other than a string.
    std::optional<std::string> Text(uint32_t row, std::string_view name) const
    {
        const Column* column = Find(name);
        const std::optional<std::size_t> at = column ? Place(*column, row) : std::nullopt;
        if (!at)
        {
            return std::nullopt;
        }
        if (column->type != ValueType::kString)
        {
            throw Error(kDamaged);
        }

        return String(xsflib::LoadBe32(&bytes_[*at]));
    }

private:
    struct Column
    {
        std::string name;
        ValueType type;
        std::optional<std::size_t> shared; // the shared value's offset in the table
        std::optional<std::size_t> in_row; // the value's offset in each row
    };

    // Reads the `count` columns after the header. Throws Error if one is damaged, or if a row can't hold the values.
    void ReadColumns(uint16_t count)
    {
        std::size_t at = kTableHeader;
        std::size_t row_end = 0;
        for (uint16_t i = 0; i < count; ++i)
        {
            Need(at, 1);
            const auto storage = static_cast<uint8_t>(bytes_[at] & 0xF0);
            const auto type = static_cast<uint8_t>(bytes_[at] & 0x0F);
            ++at;
            if ((storage & ~(kNamed | kShared | kPerRow)) != 0 || ((storage & kShared) && (storage & kPerRow)) ||
                type > static_cast<uint8_t>(ValueType::kData))
            {
                throw Error(kDamaged);
            }

            Column column{{}, static_cast<ValueType>(type), std::nullopt, std::nullopt};
            if (storage & kNamed)
            {
                Need(at, 4);
                column.name = String(xsflib::LoadBe32(&bytes_[at]));
                at += 4;
            }

            const std::size_t size = ValueSize(column.type);
            if (storage & kShared)
            {
                Need(at, size);
                column.shared = at;
                at += size;
            }
            if (storage & kPerRow)
            {
                column.in_row = row_end;
                row_end += size;
            }

            columns_.push_back(std::move(column));
        }

        if (row_end > row_size_)
        {
            throw Error(kDamaged);
        }
    }

    // Throws Error unless the table holds `size` bytes at `at`.
    void Need(std::size_t at, std::size_t size) const
    {
        if (at > end_ || size > end_ - at)
        {
            throw Error(kDamaged);
        }
    }

    // The first column named `name`, or nullptr.
    const Column* Find(std::string_view name) const
    {
        const auto column =
            std::find_if(columns_.begin(), columns_.end(), [name](const Column& c) { return c.name == name; });

        return column == columns_.end() ? nullptr : &*column;
    }

    // The offset in the table of `column`'s value in row `row`, or nothing if the column holds no value.
    std::optional<std::size_t> Place(const Column& column, uint32_t row) const
    {
        if (column.shared)
        {
            return column.shared;
        }
        if (!column.in_row)
        {
            return std::nullopt;
        }
        if (row >= row_count_)
        {
            throw Error(kDamaged);
        }

        return rows_ + std::size_t{row} * row_size_ + *column.in_row;
    }

    // The string at `offset` among the strings, decoded from Shift-JIS. Throws Error if it doesn't end in the table.
    std::string String(uint32_t offset) const
    {
        if (offset >= end_ - strings_)
        {
            throw Error(kDamaged);
        }

        const auto start = bytes_.begin() + static_cast<std::ptrdiff_t>(strings_ + offset);
        const auto end = bytes_.begin() + static_cast<std::ptrdiff_t>(end_);
        const auto nul = std::find(start, end, uint8_t{0});
        if (nul == end)
        {
            throw Error(kDamaged);
        }

        return detail::DecodeShiftJis(std::span(start, nul));
    }

    std::vector<uint8_t> bytes_;
    std::size_t end_ = 0;     // the table's end: the size in the header, plus 8
    std::size_t rows_ = 0;    // the first row's offset
    std::size_t strings_ = 0; // the strings' offset
    uint16_t row_size_ = 0;
    uint32_t row_count_ = 0;
    std::vector<Column> columns_;
};

// Reads the table of the chunk named `magic` at `offset` in the archive of `size` bytes. Throws Error with `wrong` if
// the archive holds no chunk of that name at `offset`. Throws Error with kDamaged if the chunk's table is damaged or
// runs past the archive's end.
UtfTable ReadChunk(const CpkRead& read, uint64_t size, uint64_t offset, std::string_view magic, const char* wrong)
{
    if (offset > size || size - offset < kChunkHeader)
    {
        throw Error(wrong);
    }

    const std::vector<uint8_t> header = read(offset, kChunkHeader);
    if (header.size() != kChunkHeader || !std::equal(magic.begin(), magic.end(), header.begin()))
    {
        throw Error(wrong);
    }

    const uint64_t table_size = xsflib::LoadLe64(&header[8]);
    if (table_size > size - offset - kChunkHeader || table_size > kMaxFileSize)
    {
        throw Error(kDamaged);
    }

    return UtfTable(read(offset + kChunkHeader, static_cast<std::size_t>(table_size)));
}

// The bits of a CRILAYLA stream, read from the stream's end towards its start, and from each byte's most significant
// bit.
class BackwardBits
{
public:
    // The stream is `data`'s bytes from `start` up to `end`.
    BackwardBits(std::span<const uint8_t> data, std::size_t start, std::size_t end)
        : data_(data), start_(start), next_(end)
    {
    }

    // Reads `count` bits, at most 32. The first bit read is the most significant. Throws Error if the stream ends.
    uint32_t Read(int count)
    {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i)
        {
            if (bits_left_ == 0)
            {
                if (next_ == start_)
                {
                    throw Error("CRILAYLA data is truncated");
                }

                byte_ = data_[--next_];
                bits_left_ = 8;
            }

            --bits_left_;
            value = (value << 1) | ((byte_ >> bits_left_) & 1u);
        }

        return value;
    }

private:
    std::span<const uint8_t> data_;
    std::size_t start_;
    std::size_t next_; // the index after the next byte to read
    uint8_t byte_ = 0;
    int bits_left_ = 0; // the bits of byte_ not read yet
};

// Reads a copy's length. The length is 3 plus a number of 2 bits. Numbers of 3, 5 and 8 bits follow in turn while the
// number before has all its bits set. Then numbers of 8 bits follow while the number before is 255. The length adds
// them all.
std::size_t CopyLength(BackwardBits& bits)
{
    std::size_t length = 3;
    for (const int count : {2, 3, 5, 8})
    {
        const uint32_t part = bits.Read(count);
        length += part;
        if (part != (1u << count) - 1)
        {
            return length;
        }
    }

    uint32_t part = 0;
    do
    {
        part = bits.Read(8);
        length += part;
    } while (part == 255);

    return length;
}

} // namespace

std::vector<CpkFile> ListCpk(const CpkRead& read, uint64_t size)
{
    const UtfTable header = ReadChunk(read, size, 0, "CPK ", kNotCpk);
    if (header.Rows() == 0)
    {
        throw Error(kDamaged);
    }

    // An archive without a TOC lists its files by ID only.
    const std::optional<uint64_t> toc = header.Number(0, "TocOffset");
    if (!toc || *toc == 0)
    {
        return {};
    }

    // The files' offsets count from the lower of the TOC's offset and the content's offset.
    const std::optional<uint64_t> content = header.Number(0, "ContentOffset");
    const uint64_t base = content && *content ? std::min(*toc, *content) : *toc;
    const UtfTable table = ReadChunk(read, size, *toc, "TOC ", kDamaged);
    std::vector<CpkFile> files;
    for (uint32_t row = 0; row < table.Rows(); ++row)
    {
        const std::optional<std::string> name = table.Text(row, "FileName");
        const std::optional<uint64_t> stored = table.Number(row, "FileSize");
        const std::optional<uint64_t> offset = table.Number(row, "FileOffset");
        if (!name || !stored || !offset)
        {
            throw Error(kDamaged);
        }

        // A file whose stored bytes lie outside the archive is left out.
        if (base > size || *offset > size - base || *stored > size - base - *offset)
        {
            continue;
        }

        const std::string folder = table.Text(row, "DirName").value_or("");
        const uint64_t extracted = table.Number(row, "ExtractSize").value_or(0);
        files.push_back(
            {folder.empty() ? *name : folder + "/" + *name, base + *offset, *stored, extracted ? extracted : *stored});
    }

    return files;
}

std::vector<uint8_t> ReadCpkFile(const CpkRead& read, const CpkFile& file, std::size_t max_size)
{
    if (file.stored_size > max_size || file.size > max_size)
    {
        throw Error("the file is too large");
    }

    std::vector<uint8_t> data = read(file.offset, static_cast<std::size_t>(file.stored_size));
    if (IsCrilayla(data))
    {
        data = DecompressCrilayla(data, max_size);
    }
    if (data.size() != file.size)
    {
        throw Error("the file's size doesn't match the CPK's TOC");
    }

    return data;
}

bool IsCrilayla(std::span<const uint8_t> data)
{
    return data.size() >= 8 && std::equal(data.begin(), data.begin() + 8, kCrilaylaMagic);
}

std::vector<uint8_t> DecompressCrilayla(std::span<const uint8_t> data, std::size_t max_size)
{
    if (!IsCrilayla(data))
    {
        throw Error("the data isn't compressed with CRILAYLA");
    }
    if (data.size() < kCrilaylaHeader)
    {
        throw Error("CRILAYLA data is truncated");
    }

    // The bytes stored as they are follow the compressed stream.
    const uint64_t size = xsflib::LoadLe32(&data[8]);
    const uint64_t raw = kCrilaylaHeader + uint64_t{xsflib::LoadLe32(&data[12])};
    if (raw + kRawSize > data.size())
    {
        throw Error("CRILAYLA data is truncated");
    }
    if (size > max_size || kRawSize > max_size - size)
    {
        throw Error("CRILAYLA data is too large");
    }

    std::vector<uint8_t> out(static_cast<std::size_t>(kRawSize + size));
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(raw), kRawSize, out.begin());

    // Each step writes a byte or copies bytes from those already written, from the end towards kRawSize.
    BackwardBits bits(data, kCrilaylaHeader, static_cast<std::size_t>(raw));
    std::size_t left = static_cast<std::size_t>(size); // the bytes still to write, below kRawSize + left
    while (left > 0)
    {
        std::size_t to = kRawSize + left - 1;
        if (bits.Read(1) == 0)
        {
            out[to] = static_cast<uint8_t>(bits.Read(8));
            --left;
            continue;
        }

        std::size_t from = to + bits.Read(13) + 3;
        const std::size_t length = CopyLength(bits);
        if (from >= out.size() || length > left)
        {
            throw Error("CRILAYLA data is damaged");
        }

        for (std::size_t i = 0; i < length; ++i)
        {
            out[to--] = out[from--];
        }

        left -= length;
    }

    return out;
}

} // namespace ntdlib::disc
