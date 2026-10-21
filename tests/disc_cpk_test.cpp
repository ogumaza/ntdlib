// SPDX-License-Identifier: MIT

// Tests of the CPK reader, and of the sound archives that LoadGame lists inside CPKs (GameOptions::cpk_sound_archives).
// The CPKs are made up here. Their tables are stored masked and unmasked, with the TOC before the files and after
// them, and with numbers of several widths. Their files are stored as they are or with CRILAYLA, in the root folder and
// in nested folders. LoadGame reads the CPKs in an extracted disc, in a title's U8 archive and in a GameCube disc
// image. The CRILAYLA streams are written field by field, as the format describes them: with each form of a copy's
// length, with near and far copies, and with literals only. The test also checks damaged CPKs, tables and streams.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

#include "disc_files.h"
#include "ntdlib/disc/cpk.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "temp_folder.h"

namespace
{

namespace fs = std::filesystem;
namespace disc = ntdlib::disc;
using Bytes = std::vector<uint8_t>;
using disc_files::Fill;
using disc_files::StoreBe32;
using disc_files::StoreBe64;
using disc_files::Text;
using disc_files::WriteFile;
using ntdlib::testing::TempFolder;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// The bytes that CRILAYLA data stores as they are, before the decompressed part.
constexpr size_t kRawSize = 0x100;

// The flags of a made-up table's column: how the column stores its value, in the high 4 bits, and the value's type, in
// the low 4 bits.
constexpr uint8_t kNamed = 0x10;
constexpr uint8_t kShared = 0x20;
constexpr uint8_t kPerRow = 0x40;
constexpr uint8_t kU16 = 0x2;
constexpr uint8_t kU32 = 0x4;
constexpr uint8_t kS32 = 0x5;
constexpr uint8_t kU64 = 0x6;
constexpr uint8_t kFloat = 0x8;
constexpr uint8_t kString = 0xA;
constexpr uint8_t kData = 0xB;

void AppendBe(Bytes& out, uint64_t value, size_t size)
{
    for (size_t i = size; i-- > 0;)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void AppendLe(Bytes& out, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void Place(Bytes& out, size_t at, const Bytes& data)
{
    if (out.size() < at + data.size())
    {
        out.resize(at + data.size());
    }

    std::copy(data.begin(), data.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
}

// `size` bytes of a pseudorandom pattern (see Fill).
Bytes Pattern(size_t size, uint32_t seed)
{
    Bytes data(size);
    Fill(data, 0, size, seed);

    return data;
}

// A field of a CRILAYLA stream: `count` bits of `value`, most significant first, in the order that the decompressor
// reads them.
struct Field
{
    uint32_t value;
    int count;
};

// CRILAYLA data whose compressed stream holds `fields`, and whose decompressed part is `size` bytes after the bytes
// `raw`. The decompressor reads the stream from its last byte towards its first, and each byte from its most
// significant bit. The fields' bits are therefore packed from the most significant bit, and the bytes stored in
// reverse.
Bytes Crilayla(const std::vector<Field>& fields, size_t size, const Bytes& raw)
{
    Bytes packed;
    int used = 8;
    for (const Field& field : fields)
    {
        for (int i = field.count; i-- > 0;)
        {
            if (used == 8)
            {
                packed.push_back(0);
                used = 0;
            }

            packed.back() = static_cast<uint8_t>(packed.back() | ((field.value >> i) & 1) << (7 - used));
            ++used;
        }
    }

    Bytes data = Text("CRILAYLA");
    AppendLe(data, size, 4);
    AppendLe(data, packed.size(), 4);
    data.insert(data.end(), packed.rbegin(), packed.rend());
    data.insert(data.end(), raw.begin(), raw.end());

    return data;
}

// The bytes that the made-up CRILAYLA data stores as they are.
Bytes Raw()
{
    return Pattern(kRawSize, 99);
}

// Compresses `data` with literals only: a 0 bit and the byte, for each byte after the first kRawSize, from the last.
Bytes CompressLiterals(const Bytes& data)
{
    std::vector<Field> fields;
    for (size_t i = data.size(); i-- > kRawSize;)
    {
        fields.push_back({0, 1});
        fields.push_back({data[i], 8});
    }

    return Crilayla(fields, data.size() - kRawSize, Bytes(data.begin(), data.begin() + kRawSize));
}

// Three literals, x, y and z, then copies from 3 bytes back (distance 0) with each form of the length. The length is 3
// and a number of 2 bits below 3, or 3 and then a number of 3 bits below 7, or 7 and then one of 5 bits below 31, or 31
// and then one of 8 bits below 255, or 255 and then more numbers of 8 bits while each is 255. The lengths are 3, 5, 6,
// 12, 13, 43, 44, 298, 299 and 559.
std::vector<Field> LengthFields()
{
    return {
        {0, 1}, {'x', 8}, {0, 1}, {'y', 8}, {0, 1},  {'z', 8},                   // the literals
        {1, 1}, {0, 13},  {0, 2},                                                // 3 + 0
        {1, 1}, {0, 13},  {2, 2},                                                // 3 + 2
        {1, 1}, {0, 13},  {3, 2}, {0, 3},                                        // 3 + 3 + 0
        {1, 1}, {0, 13},  {3, 2}, {6, 3},                                        // 3 + 3 + 6
        {1, 1}, {0, 13},  {3, 2}, {7, 3},   {0, 5},                              // 3 + 3 + 7 + 0
        {1, 1}, {0, 13},  {3, 2}, {7, 3},   {30, 5},                             // 3 + 3 + 7 + 30
        {1, 1}, {0, 13},  {3, 2}, {7, 3},   {31, 5}, {0, 8},                     // 3 + 3 + 7 + 31 + 0
        {1, 1}, {0, 13},  {3, 2}, {7, 3},   {31, 5}, {254, 8},                   // 3 + 3 + 7 + 31 + 254
        {1, 1}, {0, 13},  {3, 2}, {7, 3},   {31, 5}, {255, 8}, {0, 8},           // 3 + 3 + 7 + 31 + 255 + 0
        {1, 1}, {0, 13},  {3, 2}, {7, 3},   {31, 5}, {255, 8}, {255, 8}, {5, 8}, // ... + 255 + 255 + 5
    };
}

// What LengthFields decompress to after Raw: the decompressor writes x, y and z at the end, and each copy repeats the
// 3 bytes above it. The decompressed part therefore repeats "zyx", ending at its last byte.
Bytes LengthOutput()
{
    constexpr size_t kSize = 3 + 3 + 5 + 6 + 12 + 13 + 43 + 44 + 298 + 299 + 559;
    Bytes out = Raw();
    for (size_t i = 0; i < kSize; ++i)
    {
        out.push_back(static_cast<uint8_t>("xyz"[(kSize - 1 - i) % 3]));
    }

    return out;
}

// Literals and copies from further back. From the end: the literals H, G, F, E and D, a copy of 3 bytes from 5 bytes
// back (distance 2), the literal A, and a copy of 4 bytes from 8 bytes back (distance 5).
std::vector<Field> DistanceFields()
{
    return {
        {0, 1}, {'H', 8}, {0, 1}, {'G', 8}, {0, 1}, {'F', 8}, {0, 1}, {'E', 8}, {0, 1}, {'D', 8}, // the literals
        {1, 1}, {2, 13},  {0, 2},                                                                 // 3 bytes, distance 2
        {0, 1}, {'A', 8},                                                                         // a literal
        {1, 1}, {5, 13},  {1, 2},                                                                 // 4 bytes, distance 5
    };
}

// A copy from the furthest distance: 8,194 literals, then a copy of 3 bytes from 8,194 bytes back (distance 8,191).
// Returns the fields, and the decompressed part in `out`.
std::vector<Field> FarFields(Bytes& out)
{
    const Bytes literals = Pattern(8194, 7);
    std::vector<Field> fields;
    for (const uint8_t byte : literals)
    {
        fields.push_back({0, 1});
        fields.push_back({byte, 8});
    }

    fields.push_back({1, 1});
    fields.push_back({8191, 13});
    fields.push_back({0, 2});

    // The decompressor writes the literals from the end, then the copy below them.
    out.assign({literals[2], literals[1], literals[0]});
    out.insert(out.end(), literals.rbegin(), literals.rend());

    return fields;
}

// A value in a made-up table: a number, or text for a string column.
struct Value
{
    uint64_t number = 0;
    std::string text;
};

// A column of a made-up table: its name, its flags, and its shared value or a value for each row.
struct Column
{
    std::string name;
    uint8_t flags;
    std::vector<Value> values;
};

uint8_t Flags(uint8_t storage, uint8_t type)
{
    return static_cast<uint8_t>(storage | type);
}

size_t TypeSize(uint8_t type)
{
    switch (type)
    {
    case kU16:
        return 2;
    case kU64:
    case kData:
        return 8;
    default:
        return 4;
    }
}

// An @UTF table named `name`, with `rows` rows. The columns follow the header, then the rows, then the strings, with
// the table's name first. The data area is empty, at the end.
Bytes UtfTable(const std::string& name, uint32_t rows, const std::vector<Column>& columns)
{
    Bytes strings;
    const auto add_string = [&strings](const std::string& text)
    {
        const size_t offset = strings.size();
        strings.insert(strings.end(), text.begin(), text.end());
        strings.push_back(0);

        return offset;
    };
    const size_t name_offset = add_string(name);
    const auto append_value = [&add_string](Bytes& out, uint8_t type, const Value& value)
    {
        if (type == kString)
        {
            AppendBe(out, add_string(value.text), 4);
        }
        else if (type == kData)
        {
            AppendBe(out, 0, 8);
        }
        else
        {
            AppendBe(out, value.number, TypeSize(type));
        }
    };

    Bytes schema;
    size_t row_size = 0;
    for (const Column& column : columns)
    {
        const auto type = static_cast<uint8_t>(column.flags & 0x0F);
        schema.push_back(column.flags);
        if (column.flags & kNamed)
        {
            AppendBe(schema, add_string(column.name), 4);
        }
        if (column.flags & kShared)
        {
            append_value(schema, type, column.values.at(0));
        }
        if (column.flags & kPerRow)
        {
            row_size += TypeSize(type);
        }
    }

    Bytes row_data;
    for (uint32_t row = 0; row < rows; ++row)
    {
        for (const Column& column : columns)
        {
            if (column.flags & kPerRow)
            {
                append_value(row_data, static_cast<uint8_t>(column.flags & 0x0F), column.values.at(row));
            }
        }
    }

    const size_t rows_offset = 0x20 + schema.size();
    const size_t strings_offset = rows_offset + row_data.size();
    const size_t end = strings_offset + strings.size();
    Bytes table = Text("@UTF");
    AppendBe(table, end - 8, 4);
    AppendBe(table, 0, 2);
    AppendBe(table, rows_offset - 8, 2);
    AppendBe(table, strings_offset - 8, 4);
    AppendBe(table, end - 8, 4);
    AppendBe(table, name_offset, 4);
    AppendBe(table, columns.size(), 2);
    AppendBe(table, row_size, 2);
    AppendBe(table, rows, 4);
    table.insert(table.end(), schema.begin(), schema.end());
    table.insert(table.end(), row_data.begin(), row_data.end());
    table.insert(table.end(), strings.begin(), strings.end());

    return table;
}

// Masks a table, or unmasks it: each byte is XORed with the low byte of a number that starts at 0x655F and is
// multiplied by 0x4115 after each byte.
Bytes Masked(Bytes table)
{
    uint32_t mask = 0x655F;
    for (uint8_t& byte : table)
    {
        byte = static_cast<uint8_t>(byte ^ mask);
        mask *= 0x4115;
    }

    return table;
}

// A chunk: its magic, the bytes FF 00 00 00, the table's size in 64 bits (little-endian), then the table.
Bytes Chunk(const std::string& magic, const Bytes& table, bool masked)
{
    Bytes chunk = Text(magic);
    chunk.insert(chunk.end(), {0xFF, 0, 0, 0});
    AppendLe(chunk, table.size(), 8);
    const Bytes stored = masked ? Masked(table) : table;
    chunk.insert(chunk.end(), stored.begin(), stored.end());

    return chunk;
}

// A file of a made-up CPK: its folder and name, the bytes that the CPK stores, and the TOC's ExtractSize. The TOC's
// FileSize is the stored size plus `extra`.
struct Entry
{
    std::string folder;
    std::string name;
    Bytes stored;
    uint64_t extract_size;
    uint64_t extra = 0;
};

// How a made-up CPK lays out its tables.
struct Layout
{
    bool masked = false;        // the tables are stored masked
    bool toc_last = false;      // the files start at 0x600, and the TOC follows them
    bool content_offset = true; // the CPK's table gives ContentOffset
    bool other_widths = false;  // FileSize in 64 bits, ExtractSize signed and FileOffset in 32 bits
    bool no_toc = false;        // TocOffset is 0, as in a CPK that lists its files by ID only
    bool shared = false; // ContentOffset and DirName are values that every row shares: DirName is the first file's
};

// The CPK's first table: one row with the TOC's offset and the files' offset, and columns that the reader skips. One
// column has no value, and another holds data. With `shared`, ContentOffset is a value that every row shares.
Bytes HeaderTable(uint64_t toc, std::optional<uint64_t> content, uint64_t size, size_t files, bool shared)
{
    std::vector<Column> columns = {
        {"UpdateDateTime", Flags(kNamed | kPerRow, kU64), {{1, ""}}},
        {"FileSize", Flags(kNamed | kPerRow, kU64), {{size, ""}}},
        {"TocOffset", Flags(kNamed | kPerRow, kU64), {{toc, ""}}},
        {"ItocOffset", Flags(kNamed, kU64), {}},
        {"Files", Flags(kNamed | kPerRow, kU32), {{files, ""}}},
        {"Align", Flags(kNamed | kPerRow, kU16), {{0x40, ""}}},
        {"Scale", Flags(kNamed | kShared, kFloat), {{0x3F800000, ""}}},
        {"Info", Flags(kNamed | kPerRow, kData), {{0, ""}}},
        {"Tvers", Flags(kNamed | kPerRow, kString), {{0, "CPKMC2.10.00"}}},
    };
    if (content)
    {
        const uint8_t storage = shared ? kShared : kPerRow;
        columns.insert(columns.begin() + 2, {"ContentOffset", Flags(kNamed | storage, kU64), {{*content, ""}}});
    }

    return UtfTable("CpkHeader", 1, columns);
}

// The TOC: a row for each file. Each file's FileOffset counts from `base`. `offsets` gives each file's offset in the
// CPK.
Bytes TocTable(const std::vector<Entry>& entries, const std::vector<uint64_t>& offsets, uint64_t base,
               const Layout& layout)
{
    std::vector<Column> columns = {
        {"DirName", Flags(kNamed | (layout.shared ? kShared : kPerRow), kString), {}},
        {"FileName", Flags(kNamed | kPerRow, kString), {}},
        {"FileSize", Flags(kNamed | kPerRow, layout.other_widths ? kU64 : kU32), {}},
        {"ExtractSize", Flags(kNamed | kPerRow, layout.other_widths ? kS32 : kU32), {}},
        {"FileOffset", Flags(kNamed | kPerRow, layout.other_widths ? kU32 : kU64), {}},
        {"ID", Flags(kNamed | kPerRow, kU32), {}},
        {"UserString", Flags(kNamed | kShared, kString), {{0, "<NULL>"}}},
    };
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (!layout.shared || i == 0)
        {
            columns[0].values.push_back({0, entries[i].folder});
        }

        columns[1].values.push_back({0, entries[i].name});
        columns[2].values.push_back({entries[i].stored.size() + entries[i].extra, ""});
        columns[3].values.push_back({entries[i].extract_size, ""});
        columns[4].values.push_back({offsets[i] - base, ""});
        columns[5].values.push_back({i, ""});
    }

    return UtfTable("CpkTocInfo", static_cast<uint32_t>(entries.size()), columns);
}

// A CPK that holds `entries`, each at a multiple of 0x40. Its first chunk lies at 0. By default, the TOC lies at 0x800
// and the files from 0x2000, and the files' offsets count from the TOC's offset.
Bytes Cpk(const std::vector<Entry>& entries, const Layout& layout)
{
    const uint64_t content = layout.toc_last ? 0x600 : 0x2000;
    std::vector<uint64_t> offsets;
    Bytes files;
    for (const Entry& entry : entries)
    {
        files.resize((files.size() + 0x3F) & ~size_t{0x3F});
        offsets.push_back(content + files.size());
        files.insert(files.end(), entry.stored.begin(), entry.stored.end());
    }

    const uint64_t toc = layout.toc_last ? (content + files.size() + 0x7FF) & ~uint64_t{0x7FF} : 0x800;
    const uint64_t base = layout.content_offset ? std::min(toc, content) : toc;
    const Bytes toc_chunk = Chunk("TOC ", TocTable(entries, offsets, base, layout), layout.masked);
    const uint64_t size = layout.toc_last ? toc + toc_chunk.size() : content + files.size();
    const std::optional<uint64_t> content_offset = layout.content_offset ? std::optional(content) : std::nullopt;
    const Bytes header =
        Chunk("CPK ", HeaderTable(layout.no_toc ? 0 : toc, content_offset, size, entries.size(), layout.shared),
              layout.masked);
    Check(header.size() <= 0x600 && (layout.toc_last || toc_chunk.size() <= content - toc),
          "the made-up CPK's chunks fit their places");

    Bytes cpk(static_cast<size_t>(size));
    Place(cpk, 0, header);
    Place(cpk, static_cast<size_t>(toc), toc_chunk);
    Place(cpk, static_cast<size_t>(content), files);

    return cpk;
}

// A file of the made-up CPKs, and what reading it gives.
struct TestFile
{
    Entry entry;
    Bytes data;
};

// The path of `file` in the CPK: the folder, then the name.
std::string PathInCpk(const TestFile& file)
{
    return file.entry.folder.empty() ? file.entry.name : file.entry.folder + "/" + file.entry.name;
}

// The files of the made-up CPKs: a sound archive stored as it is, one compressed with literals only in the root folder,
// one compressed with each form of a copy's length in a nested folder, a file that isn't a sound archive, and a sound
// archive whose ExtractSize is 0.
std::vector<TestFile> TestFiles()
{
    const Bytes plain = Pattern(0x180, 1);
    const Bytes literal = Pattern(0x300, 2);
    const Bytes lengths = LengthOutput();
    const Bytes effects = Pattern(0x40, 3);
    const Bytes zero = Pattern(0x50, 4);

    return {
        {{"SOUND", "plain.brsar", plain, plain.size()}, plain},
        {{"", "literal.brsar", CompressLiterals(literal), literal.size()}, literal},
        {{"A/B", "Lengths.BRSAR", Crilayla(LengthFields(), lengths.size() - kRawSize, Raw()), lengths.size()}, lengths},
        {{"SOUND", "effects.bin", effects, effects.size()}, effects},
        {{"SOUND", "zero.brsar", zero, 0}, zero},
    };
}

std::vector<Entry> EntriesOf(const std::vector<TestFile>& files)
{
    std::vector<Entry> entries;
    for (const TestFile& file : files)
    {
        entries.push_back(file.entry);
    }

    return entries;
}

// Reads a CPK in memory. Every read must lie inside the CPK.
disc::CpkRead MemoryRead(const Bytes& cpk)
{
    return [&cpk](uint64_t offset, size_t size)
    {
        Check(offset <= cpk.size() && size <= cpk.size() - offset, "the reads stay inside the CPK");
        const auto start = cpk.begin() + static_cast<std::ptrdiff_t>(std::min<uint64_t>(offset, cpk.size()));

        return Bytes(start, start + static_cast<std::ptrdiff_t>(std::min<uint64_t>(size, cpk.end() - start)));
    };
}

std::vector<std::string> Paths(const disc::GameFiles& game)
{
    std::vector<std::string> paths;
    for (const disc::GameFile& file : game.files)
    {
        paths.push_back(file.path);
    }

    return paths;
}

// Decompresses the streams written field by field, and checks each against the output that the format gives.
void TestStreams()
{
    const Bytes lengths = LengthOutput();
    Bytes far;
    const std::vector<Field> far_fields = FarFields(far);
    Bytes far_output = Raw();
    far_output.insert(far_output.end(), far.begin(), far.end());
    Bytes distance_output = Raw();
    const Bytes distance_text = Text("DEFGAFGHDEFGH");
    distance_output.insert(distance_output.end(), distance_text.begin(), distance_text.end());

    const Bytes lengths_result =
        disc::DecompressCrilayla(Crilayla(LengthFields(), lengths.size() - kRawSize, Raw()), 1 << 20);
    const Bytes distance_result = disc::DecompressCrilayla(Crilayla(DistanceFields(), 13, Raw()), 1 << 20);
    const Bytes far_result = disc::DecompressCrilayla(Crilayla(far_fields, far.size(), Raw()), 1 << 20);
    const Bytes raw_only = disc::DecompressCrilayla(Crilayla({}, 0, Raw()), 1 << 20);

    Check(lengths_result == lengths, "each form of a copy's length, from 3 to 559 bytes");
    Check(distance_result == distance_output, "literals between copies from 5 and 8 bytes back");
    Check(far_result == far_output, "a copy from 8,194 bytes back, the 13-bit distance's largest");
    Check(raw_only == Raw(), "data with nothing to decompress gives the bytes stored as they are");
}

// Compresses data with literals only, and decompresses it.
void TestLiterals()
{
    const Bytes data = Pattern(0x1234, 5);

    const Bytes compressed = CompressLiterals(data);
    const Bytes result = disc::DecompressCrilayla(compressed, data.size());

    Check(disc::IsCrilayla(compressed) && !disc::IsCrilayla(data), "IsCrilayla finds the magic");
    Check(result == data, "literals only, decompressed");
}

// Damaged streams and data: a copy from bytes not yet written, a copy past the decompressed part's start, a stream
// that ends early, stored bytes past the end, a size over the limit and data without the magic.
void TestDamagedStreams()
{
    const Bytes copy_first = Crilayla({{1, 1}, {0, 13}, {0, 2}}, 3, Raw());
    const Bytes copy_past =
        Crilayla({{0, 1}, {'x', 8}, {0, 1}, {'y', 8}, {0, 1}, {'z', 8}, {1, 1}, {0, 13}, {0, 2}}, 4, Raw());
    const Bytes ends_early = Crilayla({{0, 1}, {'x', 8}, {0, 1}, {'y', 8}}, 10, Raw());
    Bytes raw_past = Crilayla({{0, 1}, {'x', 8}}, 1, Raw());
    raw_past.pop_back();
    Bytes no_magic = Crilayla({{0, 1}, {'x', 8}}, 1, Raw());
    no_magic[0] = 'X';
    const auto decompress = [](const Bytes& data, size_t max_size)
    {
        return ErrorOf([&data, max_size] { disc::DecompressCrilayla(data, max_size); });
    };

    Check(decompress(copy_first, 1 << 20) == "CRILAYLA data is damaged", "a copy from bytes not yet written");
    Check(decompress(copy_past, 1 << 20) == "CRILAYLA data is damaged", "a copy past the decompressed part's start");
    Check(decompress(ends_early, 1 << 20) == "CRILAYLA data is truncated", "a stream that ends early");
    Check(decompress(raw_past, 1 << 20) == "CRILAYLA data is truncated", "stored bytes that run past the end");
    Check(decompress(copy_first, kRawSize + 2) == "CRILAYLA data is too large", "a size over the limit");
    Check(decompress(no_magic, 1 << 20) == "the data isn't compressed with CRILAYLA", "data without the magic");
}

// Lists and reads the made-up CPK in each layout.
void TestLayouts()
{
    const std::vector<TestFile> files = TestFiles();
    const std::vector<std::string> kPaths = {"SOUND/plain.brsar", "literal.brsar", "A/B/Lengths.BRSAR",
                                             "SOUND/effects.bin", "SOUND/zero.brsar"};
    const std::vector<std::pair<std::string, Layout>> kLayouts = {
        {"unmasked", {}},
        {"masked", {.masked = true}},
        {"with the TOC last", {.toc_last = true}},
        {"masked, with the TOC last", {.masked = true, .toc_last = true}},
        {"without ContentOffset", {.content_offset = false}},
        {"with other widths", {.other_widths = true}},
    };

    for (const auto& [name, layout] : kLayouts)
    {
        const Bytes cpk = Cpk(EntriesOf(files), layout);
        const disc::CpkRead read = MemoryRead(cpk);

        const std::vector<disc::CpkFile> listed = disc::ListCpk(read, cpk.size());

        Check(listed.size() == files.size(), "a CPK " + name + " lists every file");
        for (size_t i = 0; i < std::min(listed.size(), files.size()); ++i)
        {
            const std::string what = name + ": " + kPaths[i];
            Check(listed[i].path == kPaths[i], what + " has its path");
            Check(listed[i].size == files[i].data.size(), what + " has its size");
            Check(listed[i].stored_size == files[i].entry.stored.size(), what + " has its stored size");
            Check(disc::ReadCpkFile(read, listed[i], 1 << 20) == files[i].data, what + " reads");
        }
    }

    // A CPK whose ContentOffset and DirName are values that every row shares, with the TOC after the files.
    const std::vector<Entry> one_folder = {{"BGM", "a.brsar", Pattern(0x60, 11), 0x60},
                                           {"BGM", "b.brsar", Pattern(0x70, 12), 0x70}};
    const Bytes shared = Cpk(one_folder, {.toc_last = true, .shared = true});
    const disc::CpkRead shared_read = MemoryRead(shared);
    const std::vector<disc::CpkFile> shared_files = disc::ListCpk(shared_read, shared.size());
    Check(shared_files.size() == 2 && shared_files[0].path == "BGM/a.brsar" && shared_files[1].path == "BGM/b.brsar",
          "a CPK lists the folder that every row shares");
    for (size_t i = 0; i < std::min<size_t>(shared_files.size(), 2); ++i)
    {
        Check(disc::ReadCpkFile(shared_read, shared_files[i], 1 << 20) == one_folder[i].stored,
              shared_files[i].path + " reads from the content's offset that every row shares");
    }

    const Bytes masked = Cpk(EntriesOf(files), {.masked = true});
    Check(!std::equal(masked.begin() + 0x10, masked.begin() + 0x14, "@UTF"), "the masked CPK stores its tables masked");
}

// CPKs that ListCpk rejects or reads in part: a TOC cut off at the CPK's end, data that isn't a CPK, a CPK without a
// TOC, and files that end past the CPK's end.
void TestDamagedCpks()
{
    const std::vector<TestFile> files = TestFiles();
    Bytes cut = Cpk(EntriesOf(files), {});
    cut.resize(0x840);
    const Bytes junk = Pattern(0x1000, 6);
    const Bytes no_toc = Cpk(EntriesOf(files), {.no_toc = true});
    std::vector<Entry> outside = EntriesOf(files);
    outside[1].extra = 0x100000;
    const Bytes outside_cpk = Cpk(outside, {});
    const auto list_error = [](const Bytes& cpk)
    {
        return ErrorOf([&cpk] { disc::ListCpk(MemoryRead(cpk), cpk.size()); });
    };

    const std::vector<disc::CpkFile> no_toc_files = disc::ListCpk(MemoryRead(no_toc), no_toc.size());
    const std::vector<disc::CpkFile> inside = disc::ListCpk(MemoryRead(outside_cpk), outside_cpk.size());

    Check(list_error(cut) == "the CPK is damaged", "a TOC cut off at the CPK's end");
    Check(list_error(junk) == "the data isn't a CPK", "data that isn't a CPK");
    Check(list_error(Bytes(8, 0)) == "the data isn't a CPK", "data shorter than a chunk's header");
    Check(no_toc_files.empty(), "a CPK without a TOC lists no files");
    Check(inside.size() == files.size() - 1 && inside[0].path == "SOUND/plain.brsar" &&
              inside[1].path == "A/B/Lengths.BRSAR",
          "a file that ends past the CPK's end is left out");
}

// A GameCube disc's header: the magic, and the file system table's offset and size.
Bytes GameCubeHeader(uint32_t fst_offset, uint32_t fst_size)
{
    Bytes header(0x440);
    std::copy_n("GCPKZZ", 6, header.begin());
    StoreBe32(header, 0x1C, 0xC2339F3D);
    StoreBe32(header, 0x424, fst_offset);
    StoreBe32(header, 0x428, fst_size);

    return header;
}

// A file system table of files in the root folder: the root's entry, each file's entry (its name's offset, its offset
// and its size), then the names.
Bytes FileTable(const std::vector<std::pair<std::string, Bytes>>& files, const std::vector<uint32_t>& offsets)
{
    Bytes table(12 * (files.size() + 1));
    Bytes names;
    table[0] = 1;
    StoreBe32(table, 8, static_cast<uint32_t>(files.size() + 1));
    for (size_t i = 0; i < files.size(); ++i)
    {
        StoreBe32(table, 12 * (i + 1), static_cast<uint32_t>(names.size()));
        StoreBe32(table, 12 * (i + 1) + 4, offsets[i]);
        StoreBe32(table, 12 * (i + 1) + 8, static_cast<uint32_t>(files[i].second.size()));
        names.insert(names.end(), files[i].first.begin(), files[i].first.end());
        names.push_back(0);
    }

    table.insert(table.end(), names.begin(), names.end());

    return table;
}

// A GameCube disc image with `files` in its root folder, from 0x4000.
Bytes GameCubeImage(const std::vector<std::pair<std::string, Bytes>>& files)
{
    std::vector<uint32_t> offsets;
    uint32_t offset = 0x4000;
    for (const auto& [name, data] : files)
    {
        offsets.push_back(offset);
        offset = (offset + static_cast<uint32_t>(data.size()) + 0x7FFF) & ~uint32_t{0x7FFF};
    }

    const Bytes table = FileTable(files, offsets);
    Bytes image = GameCubeHeader(0x2800, static_cast<uint32_t>(table.size()));
    Place(image, 0x2800, table);
    for (size_t i = 0; i < files.size(); ++i)
    {
        Place(image, offsets[i], files[i].second);
    }

    return image;
}

// An extracted GameCube disc with `files` under files/, each at its path.
void WriteExtractedDisc(const fs::path& root, const std::vector<std::pair<std::string, Bytes>>& files)
{
    WriteFile(root / "sys" / "boot.bin", GameCubeHeader(0, 0));
    WriteFile(root / "sys" / "main.dol", {0});
    WriteFile(root / "sys" / "fst.bin", {0});
    for (const auto& [path, data] : files)
    {
        WriteFile(root / "files" / fs::path(path), data);
    }
}

// A U8 archive with `files` in its root folder.
Bytes U8Archive(const std::vector<std::pair<std::string, Bytes>>& files)
{
    std::vector<uint32_t> offsets;
    const Bytes sizing = FileTable(files, std::vector<uint32_t>(files.size()));
    uint32_t offset = (0x20 + static_cast<uint32_t>(sizing.size()) + 0x1F) & ~uint32_t{0x1F};
    const uint32_t data_offset = offset;
    for (const auto& [name, data] : files)
    {
        offsets.push_back(offset);
        offset = (offset + static_cast<uint32_t>(data.size()) + 0x1F) & ~uint32_t{0x1F};
    }

    const Bytes table = FileTable(files, offsets);
    Bytes archive(0x20);
    StoreBe32(archive, 0, 0x55AA382D);
    StoreBe32(archive, 4, 0x20);
    StoreBe32(archive, 8, static_cast<uint32_t>(table.size()));
    StoreBe32(archive, 12, data_offset);
    Place(archive, 0x20, table);
    for (size_t i = 0; i < files.size(); ++i)
    {
        Place(archive, offsets[i], files[i].second);
    }

    return archive;
}

// A title's folder: a TMD and three contents, named by index. Content 0 is a stand-in for the banner, content 1 the
// main program (a DOL with one section) and content 2 `archive`.
void WriteTitle(const fs::path& folder, const Bytes& archive)
{
    Bytes dol(0x120);
    StoreBe32(dol, 0x00, 0x100);
    StoreBe32(dol, 0x48, 0x80003100);
    StoreBe32(dol, 0x90, 0x20);
    StoreBe32(dol, 0xE0, 0x80003100);
    const std::vector<Bytes> contents = {Bytes(0x40), dol, archive};

    Bytes tmd(0x1E4 + 36 * contents.size());
    StoreBe32(tmd, 0, 0x00010001);
    StoreBe64(tmd, 0x18C, uint64_t{0x00010001} << 32 | 0x57435043);
    tmd[0x1DF] = static_cast<uint8_t>(contents.size());
    fs::create_directories(folder);
    for (size_t i = 0; i < contents.size(); ++i)
    {
        const size_t record = 0x1E4 + 36 * i;
        StoreBe32(tmd, record, static_cast<uint32_t>(i));
        tmd[record + 5] = static_cast<uint8_t>(i);
        StoreBe64(tmd, record + 8, contents[i].size());
        const xsflib::Sha1::Digest hash = xsflib::Sha1::Of(contents[i]);
        std::copy(hash.begin(), hash.end(), tmd.begin() + static_cast<std::ptrdiff_t>(record + 16));

        char name[16];
        std::snprintf(name, sizeof(name), "%08zx.app", i);
        WriteFile(folder / name, contents[i]);
    }

    WriteFile(folder / "title.tmd", tmd);
}

// What LoadGame lists and reads with and without the option, for a game whose files include `cpk` at `cpk_path`. Every
// sound archive inside the CPK reads as `files` give it.
void CheckGame(const std::string& what, const fs::path& path, disc::GameOptions options, const std::string& cpk_path,
               const Bytes& cpk, const std::vector<TestFile>& files, const std::vector<std::string>& other_paths)
{
    disc::GameFiles without;
    disc::GameFiles with;
    const std::optional<std::string> without_error = disc::LoadGame(path, without, options);
    options.cpk_sound_archives = true;
    const std::optional<std::string> with_error = disc::LoadGame(path, with, options);

    std::vector<std::string> expected = other_paths;
    expected.push_back(cpk_path);
    std::vector<std::string> expected_with = expected;
    for (const TestFile& file : files)
    {
        const std::string inner = PathInCpk(file);
        if (inner.ends_with(".brsar") || inner.ends_with(".BRSAR"))
        {
            expected_with.push_back(cpk_path + "/" + inner);
        }
    }

    std::sort(expected.begin(), expected.end());
    std::sort(expected_with.begin(), expected_with.end());
    Check(!without_error && !with_error, what + " loads: " + without_error.value_or("") + with_error.value_or(""));
    Check(Paths(without) == expected, what + ": without the option, the CPK's sound archives aren't listed");
    Check(Paths(with) == expected_with, what + ": with the option, the CPK's sound archives are listed");

    uint32_t cpk_index = 0;
    for (const disc::GameFile& file : with.files)
    {
        if (file.path == cpk_path)
        {
            cpk_index = file.index;
        }
    }

    for (const disc::GameFile& file : with.files)
    {
        Bytes data;
        const bool read = with.read_file(file.path, data);
        if (file.path == cpk_path)
        {
            Check(read && data == cpk, what + ": the CPK reads as a file");
        }

        for (const TestFile& test : files)
        {
            if (file.path == cpk_path + "/" + PathInCpk(test))
            {
                Check(read && data == test.data && file.size == test.data.size(), what + ": " + file.path + " reads");
                Check(file.index == cpk_index, what + ": " + file.path + " takes the CPK's index");
            }
        }
    }
}

// The CPK in an extracted disc, in a title's U8 archive and in a disc image.
void TestGames()
{
    const TempFolder folder("disc_cpk_test");
    const std::vector<TestFile> files = TestFiles();
    const Bytes cpk = Cpk(EntriesOf(files), {});
    const Bytes other = Pattern(0x30, 8);

    WriteExtractedDisc(folder.Path() / "disc", {{"Sound/data.CPK", cpk}, {"other.bin", other}});
    WriteTitle(folder.Path() / "title", U8Archive({{"data.cpk", cpk}, {"other.bin", other}}));
    WriteFile(folder.Path() / "game.iso", GameCubeImage({{"data.cpk", cpk}, {"other.bin", other}}));
    disc::GameOptions disc_options{disc::Console::kGameCube, "xyz"};
    disc::GameOptions image_options = disc_options;
    image_options.system_files = false;
    disc::GameOptions title_options{disc::Console::kWii, "xyz"};
    title_options.titles = true;

    CheckGame("an extracted disc", folder.Path() / "disc", disc_options, "Sound/data.CPK", cpk, files, {"other.bin"});
    CheckGame("a title", folder.Path() / "title", title_options, "data.cpk", cpk, files, {"other.bin"});
    CheckGame("a disc image", folder.Path() / "game.iso", image_options, "data.cpk", cpk, files, {"other.bin"});
}

// Damaged CPKs in an extracted disc: one whose TOC is cut off and one that isn't a CPK list nothing. In a third, a
// sound archive with a copy from bytes not yet written and one whose ExtractSize is wrong are listed, and their reads
// throw.
void TestDamagedGames()
{
    const TempFolder folder("disc_cpk_test_damaged");
    Bytes cut = Cpk(EntriesOf(TestFiles()), {});
    cut.resize(0x840);
    const Bytes bad_copy = Crilayla({{1, 1}, {0, 13}, {0, 2}}, 3, Raw());
    const Bytes literal = Pattern(0x120, 9);
    const Bytes bad = Cpk(
        {{"", "copy.brsar", bad_copy, kRawSize + 3}, {"", "size.brsar", CompressLiterals(literal), literal.size() + 1}},
        {});
    WriteExtractedDisc(folder.Path() / "disc", {{"bad.cpk", bad}, {"cut.cpk", cut}, {"junk.cpk", Pattern(0x900, 10)}});
    disc::GameOptions options{disc::Console::kGameCube, "xyz"};
    options.cpk_sound_archives = true;

    disc::GameFiles game;
    const std::optional<std::string> error = disc::LoadGame(folder.Path() / "disc", game, options);
    Bytes data;
    const std::string copy_error = ErrorOf([&game, &data] { game.read_file("bad.cpk/copy.brsar", data); });
    const std::string size_error = ErrorOf([&game, &data] { game.read_file("bad.cpk/size.brsar", data); });

    Check(!error, "a game with damaged CPKs loads: " + error.value_or(""));
    Check(Paths(game) ==
              std::vector<std::string>{"bad.cpk", "bad.cpk/copy.brsar", "bad.cpk/size.brsar", "cut.cpk", "junk.cpk"},
          "damaged CPKs list nothing, and damaged files are listed");
    Check(copy_error == "bad.cpk/copy.brsar: CRILAYLA data is damaged", "a damaged stream's read throws");
    Check(size_error == "bad.cpk/size.brsar: the file's size doesn't match the CPK's TOC",
          "a read whose size isn't ExtractSize throws");
}

// Runs `test`, and counts an exception that escapes it as a failed check.
void Run(const std::string& name, void (*test)())
{
    const std::string error = ErrorOf(test);
    Check(error.empty(), name + " throws: " + error);
}

} // namespace

int main()
{
    Run("TestStreams", TestStreams);
    Run("TestLiterals", TestLiterals);
    Run("TestDamagedStreams", TestDamagedStreams);
    Run("TestLayouts", TestLayouts);
    Run("TestDamagedCpks", TestDamagedCpks);
    Run("TestGames", TestGames);
    Run("TestDamagedGames", TestDamagedGames);

    return xsflib::testing::Summary("disc CPK");
}
