// SPDX-License-Identifier: MIT

// Builds small titles for the Wii's NAND, like WiiWare games, in folders. A title has a TMD, a banner, a main program
// compressed with LZ77, U8 archives and a boot program. One archive is compressed as a whole. The other holds sound
// archives compressed with LZ77 and with Huffman coding. Checks that LoadGame reads a title with its contents named by
// index and by content ID, with some contents missing, and with AppleDouble files beside its files. Checks that it
// rejects a WAD, contents that don't match the TMD, and folders without the main program or the TMD. Also checks the
// decompressors of LZ77 (types 0x10 and 0x11) and Huffman coding against data made by hand and by the compressors here.
// The compressors follow the formats' descriptions.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <optional>
#include <queue>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "ntdlib/disc/cx.h"
#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "xsflib/bytes.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

using xsflib::testing::Check;
using xsflib::testing::failures;

namespace
{

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// The made-up title's ID: the high half of a WiiWare game's title ID, and the game ID WWST.
constexpr uint64_t kTitleId = uint64_t{0x00010001} << 32 | 0x57575354;

// Fills data with a pseudorandom pattern (xorshift) so that it doesn't compress to nothing.
void Fill(Bytes& data, size_t offset, size_t size, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < size; ++i)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[offset + i] = static_cast<uint8_t>(x);
    }
}

// Data that compresses: random bytes, copies of them from 1 to 4,096 bytes back, a run of 300 bytes and a run of
// 70,000. LZ77 of type 0x11 takes each form of copy for them.
Bytes Compressible(uint32_t seed)
{
    Bytes data(0x1000);
    Fill(data, 0, data.size(), seed);
    for (const size_t distance : {1, 2, 3, 17, 300, 4096})
    {
        for (int i = 0; i < 40; ++i)
        {
            const uint8_t byte = data[data.size() - distance];
            data.push_back(byte);
        }
    }

    data.insert(data.end(), 300, 0x5A);
    data.insert(data.end(), 70000, 0xA5);
    Bytes tail(0x100);
    Fill(tail, 0, tail.size(), seed + 1);
    data.insert(data.end(), tail.begin(), tail.end());

    return data;
}

// `size` bytes from a few values, some much more frequent than others, after the magic "RSAR".
Bytes FewValues(size_t size, uint32_t seed)
{
    Bytes random(size);
    Fill(random, 0, size, seed);
    Bytes data = {'R', 'S', 'A', 'R'};
    for (const uint8_t r : random)
    {
        data.push_back(static_cast<uint8_t>(r < 128 ? 0 : r < 192 ? 0x40 : r % 13));
    }

    return data;
}

void AppendSize(Bytes& out, uint8_t type, size_t size)
{
    out.push_back(type);
    out.push_back(static_cast<uint8_t>(size));
    out.push_back(static_cast<uint8_t>(size >> 8));
    out.push_back(static_cast<uint8_t>(size >> 16));
}

// Compresses `data` with LZ77 of type 0x10, or with type 0x11's longer copies. At each byte, it takes the longest copy
// of 3 bytes or more from up to 4,096 bytes back.
Bytes CompressLz(const Bytes& data, bool long_copies)
{
    const size_t max_length = long_copies ? 65808 : 18;
    Bytes out;
    AppendSize(out, long_copies ? 0x11 : 0x10, data.size());
    size_t at = 0;
    while (at < data.size())
    {
        const size_t flags = out.size();
        out.push_back(0);
        for (int block = 0; block < 8 && at < data.size(); ++block)
        {
            size_t length = 0;
            size_t distance = 0;
            for (size_t back = 1; back <= std::min<size_t>(at, 4096) && length < max_length; ++back)
            {
                size_t n = 0;
                while (n < max_length && at + n < data.size() && data[at + n] == data[at + n - back])
                {
                    ++n;
                }
                if (n > length)
                {
                    length = n;
                    distance = back;
                }
            }

            if (length < 3)
            {
                out.push_back(data[at++]);
                continue;
            }

            out[flags] |= static_cast<uint8_t>(0x80 >> block);
            const size_t d = distance - 1;
            if (!long_copies || length <= 16)
            {
                out.push_back(static_cast<uint8_t>((length - (long_copies ? 1 : 3)) << 4 | d >> 8));
            }
            else if (length <= 272)
            {
                out.push_back(static_cast<uint8_t>((length - 17) >> 4));
                out.push_back(static_cast<uint8_t>(((length - 17) & 0xF) << 4 | d >> 8));
            }
            else
            {
                out.push_back(static_cast<uint8_t>(0x10 | (length - 273) >> 12));
                out.push_back(static_cast<uint8_t>((length - 273) >> 4));
                out.push_back(static_cast<uint8_t>(((length - 273) & 0xF) << 4 | d >> 8));
            }

            out.push_back(static_cast<uint8_t>(d));
            at += length;
        }
    }

    return out;
}

// Compresses `data` with Huffman coding of bytes (type 0x28). The tree's nodes are laid out breadth first. With up to
// 32 different bytes, each node's children are then close enough for the 6 bits that give where they are.
Bytes CompressHuffman(const Bytes& data)
{
    // The tree: a leaf for each byte that occurs, and for at least two, joined lightest first.
    struct Node
    {
        int symbol; // or -1 for a node with children
        int children[2];
    };

    std::map<uint8_t, uint64_t> counts;
    for (const uint8_t byte : data)
    {
        ++counts[byte];
    }
    for (int symbol = 0; counts.size() < 2; ++symbol)
    {
        counts.try_emplace(static_cast<uint8_t>(symbol), 0);
    }

    using Entry = std::pair<uint64_t, int>; // a weight and its node
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> lightest;
    std::vector<Node> nodes;
    for (const auto& [symbol, count] : counts)
    {
        lightest.push({count, static_cast<int>(nodes.size())});
        nodes.push_back({symbol, {-1, -1}});
    }
    while (lightest.size() > 1)
    {
        const Entry first = lightest.top();
        lightest.pop();
        const Entry second = lightest.top();
        lightest.pop();
        lightest.push({first.first + second.first, static_cast<int>(nodes.size())});
        nodes.push_back({-1, {first.second, second.second}});
    }

    // Each byte's code: the path to its leaf, 0 for a first child and 1 for a second.
    std::map<uint8_t, std::vector<bool>> codes;
    std::vector<std::pair<int, std::vector<bool>>> walk = {{lightest.top().second, {}}};
    while (!walk.empty())
    {
        auto [node, path] = walk.back();
        walk.pop_back();
        if (nodes[node].symbol >= 0)
        {
            codes[static_cast<uint8_t>(nodes[node].symbol)] = path;
            continue;
        }

        for (const int side : {0, 1})
        {
            std::vector<bool> longer = path;
            longer.push_back(side == 1);
            walk.push_back({nodes[node].children[side], longer});
        }
    }

    // The tree: its size byte at 4, the root at 5, then pairs of children from 6.
    Bytes out;
    AppendSize(out, 0x28, data.size());
    out.push_back(0);
    out.push_back(0);
    std::queue<std::pair<int, size_t>> parents; // nodes with children, and where they are
    parents.push({lightest.top().second, 5});
    while (!parents.empty())
    {
        const auto [node, at] = parents.front();
        parents.pop();
        const size_t pair = out.size();
        out.resize(out.size() + 2);
        const size_t offset = (pair - (at & ~size_t{1}) - 2) / 2;
        Check(offset < 64, "the Huffman tree's children are close enough to their parents");
        uint8_t value = static_cast<uint8_t>(offset);
        for (const int side : {0, 1})
        {
            const int child = nodes[node].children[side];
            if (nodes[child].symbol >= 0)
            {
                value |= static_cast<uint8_t>(0x80 >> side);
                out[pair + side] = static_cast<uint8_t>(nodes[child].symbol);
            }
            else
            {
                parents.push({child, pair + side});
            }
        }

        out[at] = value;
    }

    while (out.size() % 4)
    {
        out.push_back(0);
    }
    out[4] = static_cast<uint8_t>((out.size() - 4) / 2 - 1);

    // The codes, in 32-bit little-endian words from the top bit down.
    uint32_t word = 0;
    int used = 0;
    const auto flush = [&out, &word, &used]
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            out.push_back(static_cast<uint8_t>(word >> shift));
        }

        word = 0;
        used = 0;
    };
    for (const uint8_t byte : data)
    {
        for (const bool bit : codes[byte])
        {
            word |= uint32_t{bit} << (31 - used);
            if (++used == 32)
            {
                flush();
            }
        }
    }
    if (used)
    {
        flush();
    }

    return out;
}

std::string Hex8(uint32_t value)
{
    char text[9];
    std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(value));

    return text;
}

void WriteFile(const fs::path& path, const Bytes& data)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// A U8 archive that holds `files`, by path. With `dot`, the files are in a folder named "." in the root, as in some of
// the SDK's archives.
Bytes BuildU8(const std::map<std::string, Bytes>& files, bool dot)
{
    struct Node
    {
        bool folder;
        std::string name;
        uint32_t parent; // a folder's parent
        uint32_t end;    // the index after a folder's last node
        const Bytes* data;
    };

    // The nodes in order: each folder, then what it holds. The open folders' paths end in '/'.
    std::vector<Node> nodes = {{true, "", 0, 0, nullptr}};
    std::vector<std::pair<std::string, size_t>> open = {{"", 0}};
    if (dot)
    {
        nodes.push_back({true, ".", 0, 0, nullptr});
        open.push_back({"", 1});
    }

    const size_t base = open.size();
    const auto close = [&nodes, &open]
    {
        nodes[open.back().second].end = static_cast<uint32_t>(nodes.size());
        open.pop_back();
    };
    for (const auto& [path, data] : files)
    {
        while (open.size() > base && !path.starts_with(open.back().first))
        {
            close();
        }

        size_t start = open.back().first.size();
        for (size_t slash = path.find('/', start); slash != std::string::npos; slash = path.find('/', start))
        {
            nodes.push_back(
                {true, path.substr(start, slash - start), static_cast<uint32_t>(open.back().second), 0, nullptr});
            open.push_back({path.substr(0, slash + 1), nodes.size() - 1});
            start = slash + 1;
        }

        nodes.push_back({false, path.substr(start), 0, 0, &data});
    }
    while (!open.empty())
    {
        close();
    }

    // The header, the nodes, their names, then the files' data, each at a multiple of 0x20.
    std::string names;
    for (const Node& node : nodes)
    {
        names += node.name;
        names += '\0';
    }

    const size_t table_size = nodes.size() * 12 + names.size();
    Bytes out((0x20 + table_size + 0x1F) & ~size_t{0x1F}, 0);
    xsflib::StoreBe32(&out[0], 0x55AA382D);
    xsflib::StoreBe32(&out[4], 0x20);
    xsflib::StoreBe32(&out[8], static_cast<uint32_t>(table_size));
    xsflib::StoreBe32(&out[12], static_cast<uint32_t>(out.size()));
    std::copy(names.begin(), names.end(), out.begin() + 0x20 + static_cast<std::ptrdiff_t>(nodes.size() * 12));
    size_t name = 0;
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const Node& node = nodes[i];
        uint8_t* entry = &out[0x20 + i * 12];
        xsflib::StoreBe32(entry, static_cast<uint32_t>(name));
        entry[0] = node.folder ? 1 : 0;
        name += node.name.size() + 1;
        if (node.folder)
        {
            xsflib::StoreBe32(entry + 4, node.parent);
            xsflib::StoreBe32(entry + 8, node.end);
            continue;
        }

        const size_t offset = out.size();
        out.insert(out.end(), node.data->begin(), node.data->end());
        out.resize((out.size() + 0x1F) & ~size_t{0x1F}, 0);
        entry = &out[0x20 + i * 12];
        xsflib::StoreBe32(entry + 4, static_cast<uint32_t>(offset));
        xsflib::StoreBe32(entry + 8, static_cast<uint32_t>(node.data->size()));
    }

    return out;
}

// A banner, a title's content 0: a build tag, then at 0x80 an IMET header with a Japanese and an English name in
// UTF-16BE.
Bytes BuildBanner(std::span<const uint16_t> english)
{
    Bytes banner(0x80 + 0x600, 0);
    const std::string kTag = "WWST_TEST_BUILD";
    std::copy(kTag.begin(), kTag.end(), banner.begin());
    xsflib::StoreBe32(&banner[0x80], 0x494D4554); // "IMET"
    xsflib::StoreBe32(&banner[0x84], 0x600);
    xsflib::StoreBe32(&banner[0x88], 3);
    const std::vector<uint16_t> kJapanese = {0x30C6, 0x30B9, 0x30C8};
    for (size_t i = 0; i < kJapanese.size(); ++i)
    {
        xsflib::StoreBe16(&banner[0x9C + i * 2], kJapanese[i]);
    }
    for (size_t i = 0; i < english.size(); ++i)
    {
        xsflib::StoreBe16(&banner[0x9C + 0x54 + i * 2], english[i]);
    }

    return banner;
}

// A DOL with one text section that holds `code`.
Bytes BuildDol(const Bytes& code)
{
    Bytes dol(0x100, 0);
    xsflib::StoreBe32(&dol[0x00], 0x100);      // the section's offset in the file
    xsflib::StoreBe32(&dol[0x48], 0x80004000); // its address
    xsflib::StoreBe32(&dol[0x90], static_cast<uint32_t>(code.size()));
    xsflib::StoreBe32(&dol[0xE0], 0x80004000); // the entry point
    dol.insert(dol.end(), code.begin(), code.end());

    return dol;
}

// A content of a test title: its record in the TMD, and its data as the title stores it.
struct TestContent
{
    uint32_t id;
    uint16_t index;
    uint16_t type;
    Bytes data;
};

// A test title, and what LoadGame should read from it.
struct TestTitle
{
    uint16_t boot_index;
    std::vector<TestContent> contents;
    Bytes dol;                          // the main program
    std::map<std::string, Bytes> files; // the files of its archives in the form that LoadGame reads
};

// The title's TMD, without a signature. Each content's record gives its ID, index, type, size and SHA-1.
Bytes BuildTmd(const TestTitle& title)
{
    Bytes tmd(0x1E4 + title.contents.size() * 36, 0);
    xsflib::StoreBe32(&tmd[0], 0x00010001); // RSA-2048
    xsflib::StoreBe64(&tmd[0x18C], kTitleId);
    xsflib::StoreBe16(&tmd[0x1DE], static_cast<uint16_t>(title.contents.size()));
    xsflib::StoreBe16(&tmd[0x1E0], title.boot_index);
    for (size_t i = 0; i < title.contents.size(); ++i)
    {
        const TestContent& content = title.contents[i];
        uint8_t* record = &tmd[0x1E4 + i * 36];
        xsflib::StoreBe32(record, content.id);
        xsflib::StoreBe16(record + 4, content.index);
        xsflib::StoreBe16(record + 6, content.type);
        xsflib::StoreBe64(record + 8, content.data.size());
        const xsflib::Sha1::Digest hash = xsflib::Sha1::Of(content.data);
        std::copy(hash.begin(), hash.end(), record + 16);
    }

    return tmd;
}

// The English name in the test title's banner, with a line break, spaces at its ends, a character outside the BMP in
// UTF-16's surrogates and a surrogate without its pair. Then what LoadGame makes of it.
constexpr uint16_t kEnglishName[] = {' ', 'N', 'A', 'N', 'D', ' ', 'T',    'e',    's',    't', '\n',
                                     'T', 'i', 't', 'l', 'e', ' ', 0xD834, 0xDD1E, 0xDC00, ' '};
constexpr char kTitleName[] = "NAND Test Title \xF0\x9D\x84\x9E\xEF\xBF\xBD";

// The test title. Content 0 is the banner. Content 1 holds the main program, compressed with LZ77 of type 0x11. Content
// 2 is a U8 archive with its files in a "." folder: sound archives stored as they are, compressed with Huffman coding
// and compressed with LZ77, and another compressed file. Content 3 is a U8 archive compressed with LZ77 as a whole.
// Content 4, the boot content, holds a boot program. Each content's ID differs from its index.
TestTitle BuildTitle()
{
    TestTitle title;
    title.boot_index = 4;
    title.dol = BuildDol(Compressible(1));
    const Bytes plain = FewValues(0x1000, 2);
    const Bytes huffman = FewValues(0x3000, 3);
    const Bytes lz = Compressible(4);
    const Bytes other = CompressLz(Compressible(5), false);
    const Bytes music = FewValues(0x800, 6);
    title.files = {{"misc/other.bin", other},
                   {"music/b.brsar", music},
                   {"sound/a.brsar", plain},
                   {"sound/huff.brsar", huffman},
                   {"sound/lz.brsar", lz}};

    const Bytes archive = BuildU8({{"misc/other.bin", other},
                                   {"sound/a.brsar", plain},
                                   {"sound/huff.brsar", CompressHuffman(huffman)},
                                   {"sound/lz.brsar", CompressLz(lz, false)}},
                                  true);
    title.contents = {
        {0x1000, 0, 1, BuildBanner(kEnglishName)},
        {0x1001, 1, 1, CompressLz(title.dol, true)},
        {0x1002, 2, 1, archive},
        {0x1003, 3, 0x8001, CompressLz(BuildU8({{"music/b.brsar", music}}, false), false)},
        {0x1004, 4, 1, BuildDol(Bytes(0x400, 0x60))},
    };

    return title;
}

enum class Naming
{
    kIndex,
    kId,
};

std::string FileOf(const TestContent& content, Naming naming)
{
    return Hex8(naming == Naming::kIndex ? content.index : content.id) + ".app";
}

// Writes the title into `folder`: its TMD as `tmd_name`, and each content in a file named by its index or its ID.
void WriteTitle(const fs::path& folder, const TestTitle& title, Naming naming, const std::string& tmd_name)
{
    WriteFile(folder / tmd_name, BuildTmd(title));
    for (const TestContent& content : title.contents)
    {
        WriteFile(folder / FileOf(content, naming), content.data);
    }
}

// The options of a program that loads titles.
ntdlib::disc::GameOptions TitleOptions()
{
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kWii, "titletool"};
    options.titles = true;

    return options;
}

// Checks that LoadGame reads `title` from `folder` with the files in `files` and the title `name`.
void CheckTitle(const std::string& what, const fs::path& folder, const TestTitle& title,
                const std::map<std::string, Bytes>& files, const std::string& name)
{
    ntdlib::disc::GameFiles game;
    const auto error = ntdlib::disc::LoadGame(folder, game, TitleOptions());
    Check(!error, what + ": " + error.value_or(""));
    if (error)
    {
        return;
    }

    Check(game.IsTitle() && game.source == "unpacked title", what + ": an unpacked title");
    Check(game.GameId() == "WWST" && game.Title() == name,
          what + ": the ID and title, got " + game.GameId() + " \"" + game.Title() + "\"");
    Check(game.dol == title.dol, what + ": the main program");
    Check(game.tmd == BuildTmd(title), what + ": the TMD");
    Check(game.boot.empty() && game.fst.empty(), what + ": no disc header or file system table");
    Check(game.files.size() == files.size(), what + ": the number of files");
    for (const ntdlib::disc::GameFile& file : game.files)
    {
        const auto expected = files.find(file.path);
        Bytes data;
        Check(expected != files.end() && file.size == expected->second.size(), what + ": listing " + file.path);
        Check(game.read_file(file.path, data) && expected != files.end() && data == expected->second,
              what + ": reading " + file.path);
    }
}

// Checks that LoadGame rejects `path` with a message that contains `words`.
void CheckRefused(const std::string& what, const fs::path& path, const std::string& words)
{
    ntdlib::disc::GameFiles game;
    const auto error = ntdlib::disc::LoadGame(path, game, TitleOptions());
    Check(error && error->find(words) != std::string::npos,
          what + ": rejected with \"" + words + "\", got \"" + error.value_or("success") + "\"");
}

// Decompresses `data`, or returns nothing if the decompressor throws.
std::optional<Bytes> Decompress(const Bytes& data, size_t max_size = 1 << 20)
{
    try
    {
        return ntdlib::disc::DecompressCx(data, max_size);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

Bytes Text(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

void TestLz()
{
    // Type 0x10: three literal bytes, a copy of 6 bytes from 3 back, and a literal. With a 32-bit size, and with the
    // magic "LZ77".
    const Bytes kLz = {0x10, 0x0A, 0x00, 0x00, 0x10, 'a', 'b', 'c', 0x30, 0x02, 'd'};
    const Bytes kWide = {0x10, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x10, 'a', 'b', 'c', 0x30, 0x02, 'd'};
    Bytes magic = Text("LZ77");
    magic.insert(magic.end(), kLz.begin(), kLz.end());

    // Type 0x11: a literal byte, then copies of 16, 17 and 273 bytes from 1 back, one of each form.
    const Bytes kLongCopies = {0x11, 0x33, 0x01, 0x00, 0x70, 'a', 0xF0, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};

    XSFLIB_CHECK(ntdlib::disc::IsCx(kLz) && ntdlib::disc::IsCx(kWide) && ntdlib::disc::IsCx(magic) &&
                 ntdlib::disc::IsCx(kLongCopies));
    XSFLIB_CHECK(Decompress(kLz) == Text("abcabcabcd"));
    XSFLIB_CHECK(Decompress(kWide) == Text("abcabcabcd"));
    XSFLIB_CHECK(Decompress(magic) == Text("abcabcabcd"));
    XSFLIB_CHECK(Decompress(kLongCopies) == Bytes(307, 'a'));

    // Round trips through the compressor, with every form of copy.
    for (const uint32_t seed : {7u, 8u})
    {
        const Bytes data = Compressible(seed);
        Check(Decompress(CompressLz(data, false)) == data, "LZ77 of type 0x10, seed " + std::to_string(seed));
        Check(Decompress(CompressLz(data, true)) == data, "LZ77 of type 0x11, seed " + std::to_string(seed));
    }

    // Damaged data: a copy from before the start, data that ends early, and a size over the limit.
    XSFLIB_CHECK(!Decompress({0x10, 0x04, 0x00, 0x00, 0x80, 0x30, 0x05}));
    XSFLIB_CHECK(!Decompress({0x10, 0x0A, 0x00, 0x00, 0x10, 'a'}));
    XSFLIB_CHECK(!Decompress(kLz, 9));
}

void TestHuffman()
{
    // Two symbols. The root at 5 has both its children at 6 and 7, and both are symbols. The bits 0110 give ABBA.
    const Bytes kAbba = {0x28, 0x04, 0x00, 0x00, 0x01, 0xC0, 'A', 'B', 0x00, 0x00, 0x00, 0x60};
    XSFLIB_CHECK(ntdlib::disc::IsCx(kAbba));
    XSFLIB_CHECK(Decompress(kAbba) == Text("ABBA"));

    // Round trips through the compressor: a few values, and one value alone.
    const Bytes data = FewValues(0x2345, 9);
    XSFLIB_CHECK(Decompress(CompressHuffman(data)) == data);
    XSFLIB_CHECK(Decompress(CompressHuffman(Bytes(1000, 0x33))) == Bytes(1000, 0x33));

    // Damaged data: a child past the tree, and bits that end early.
    XSFLIB_CHECK(!Decompress({0x28, 0x04, 0x00, 0x00, 0x01, 0x3F, 'A', 'B', 0x00, 0x00, 0x00, 0x60}));
    XSFLIB_CHECK(!Decompress({0x28, 0x40, 0x00, 0x00, 0x01, 0xC0, 'A', 'B', 0x00, 0x00, 0x00, 0x60}));

    // What isn't in a format that the decompressors read: a U8 archive, Huffman coding of 4-bit symbols (type 0x24),
    // the SDK's other formats (0x30, 0x40), a short header, and Huffman coding after the magic "LZ77".
    XSFLIB_CHECK(!ntdlib::disc::IsCx(Bytes{0x55, 0xAA, 0x38, 0x2D, 0x00, 0x00, 0x00, 0x20}));
    XSFLIB_CHECK(!ntdlib::disc::IsCx(Bytes{0x24, 0x04, 0x00, 0x00}));
    XSFLIB_CHECK(!ntdlib::disc::IsCx(Bytes{0x30, 0x04, 0x00, 0x00}) &&
                 !ntdlib::disc::IsCx(Bytes{0x40, 0x04, 0x00, 0x00}));
    XSFLIB_CHECK(!ntdlib::disc::IsCx(Bytes{0x10, 0x04, 0x00}));
    XSFLIB_CHECK(!ntdlib::disc::IsCx(Bytes{'L', 'Z', '7', '7', 0x28, 0x04, 0x00, 0x00}));
}

// An AppleDouble file with no entries: the magic 0x00051607, version 2, 16 bytes of filler and an entry count of 0
// (RFC 1740).
Bytes AppleDouble()
{
    Bytes file(26, 0);
    xsflib::StoreBe32(&file[0], 0x00051607);
    xsflib::StoreBe32(&file[4], 0x00020000);

    return file;
}

// The title read from its folder: with its contents named by index, by ID (with a TMD named title.tmd and padding after
// a content), with only some contents, and with AppleDouble files beside its files.
void TestTitles(const fs::path& dir)
{
    const TestTitle title = BuildTitle();
    WriteTitle(dir / "by_index", title, Naming::kIndex, "game.tmd");
    CheckTitle("contents by index", dir / "by_index", title, title.files, kTitleName);

    WriteTitle(dir / "by_id", title, Naming::kId, "title.tmd");
    Bytes padded = title.contents[2].data;
    padded.resize(padded.size() + 0x10, 0);
    WriteFile(dir / "by_id" / FileOf(title.contents[2], Naming::kId), padded);
    CheckTitle("contents by ID", dir / "by_id", title, title.files, kTitleName);

    // The main program and the archive compressed as a whole, but no banner. The title is then the game ID.
    WriteTitle(dir / "some", title, Naming::kId, "title.tmd");
    for (const size_t i : {0, 2, 4})
    {
        fs::remove(dir / "some" / FileOf(title.contents[i], Naming::kId));
    }

    CheckTitle("some contents", dir / "some", title, {{"music/b.brsar", title.files.at("music/b.brsar")}}, "WWST");

    // macOS writes an AppleDouble file, named "._" and the file's name, beside each file that it copies to a FAT or
    // exFAT drive. Neither the TMD's nor the contents' AppleDouble files count as a TMD or a content.
    WriteTitle(dir / "apple_double", title, Naming::kIndex, "game.tmd");
    WriteFile(dir / "apple_double" / "._game.tmd", AppleDouble());
    for (const TestContent& content : title.contents)
    {
        WriteFile(dir / "apple_double" / ("._" + FileOf(content, Naming::kIndex)), AppleDouble());
    }

    CheckTitle("AppleDouble files beside the title's files", dir / "apple_double", title, title.files, kTitleName);
}

// A title whose content 1 isn't a DOL boots its main program itself: the boot content is the main program. Two archives
// with a file at the same path each get a folder named after their contents.
void TestOtherTitles(const fs::path& dir)
{
    TestTitle title;
    title.boot_index = 2;
    title.dol = BuildDol(Bytes(0x200, 0x48));
    const Bytes first = FewValues(0x100, 10);
    const Bytes second = FewValues(0x100, 11);
    title.files = {{"content1/sound/a.brsar", first}, {"content3/sound/a.brsar", second}};
    title.contents = {
        {0x2000, 0, 1, BuildBanner(kEnglishName)},
        {0x2001, 1, 1, BuildU8({{"sound/a.brsar", first}}, true)},
        {0x2002, 2, 1, title.dol},
        {0x2003, 3, 1, BuildU8({{"sound/a.brsar", second}}, false)},
    };
    WriteTitle(dir / "boot", title, Naming::kIndex, "boot.tmd");

    CheckTitle("the boot content as the main program", dir / "boot", title, title.files, kTitleName);
}

// What LoadGame rejects: contents that don't match the TMD, a folder without the main program or the TMD or with two
// TMDs, a folder that holds no game, a WAD, and a title's files given on their own.
void TestRefusals(const fs::path& dir)
{
    const TestTitle title = BuildTitle();
    WriteTitle(dir / "encrypted", title, Naming::kIndex, "title.tmd");
    Bytes scrambled = title.contents[2].data;
    for (uint8_t& byte : scrambled)
    {
        byte ^= 0x5C;
    }

    WriteFile(dir / "encrypted" / FileOf(title.contents[2], Naming::kIndex), scrambled);
    CheckRefused("an encrypted content", dir / "encrypted", "encrypted or damaged");

    WriteTitle(dir / "no_program", title, Naming::kIndex, "title.tmd");
    fs::remove(dir / "no_program" / FileOf(title.contents[1], Naming::kIndex));
    CheckRefused("no main program", dir / "no_program", "main program");

    WriteTitle(dir / "no_tmd", title, Naming::kIndex, "title.tmd");
    fs::remove(dir / "no_tmd" / "title.tmd");
    CheckRefused("no TMD", dir / "no_tmd", "but not its TMD");

    WriteTitle(dir / "two_tmds", title, Naming::kIndex, "title.tmd");
    WriteFile(dir / "two_tmds" / "copy.tmd", BuildTmd(title));
    CheckRefused("two TMDs", dir / "two_tmds", "more than one TMD");

    fs::create_directories(dir / "empty");
    CheckRefused("an empty folder", dir / "empty", "holds no game");

    // An installable WAD's header: its size, its type, and the sizes of its parts. Then a WAD by its name alone, and a
    // title's TMD and content given on their own.
    Bytes wad(0x40, 0);
    xsflib::StoreBe32(&wad[0], 0x20);
    wad[4] = 'I';
    wad[5] = 's';
    xsflib::StoreBe32(&wad[0x14], 0x1E4);
    WriteFile(dir / "title.bin", wad);
    WriteFile(dir / "empty.wad", {});
    CheckRefused("a WAD", dir / "title.bin", "decrypted contents (.app)");
    CheckRefused("a file named as a WAD", dir / "empty.wad", "it's a WAD");
    CheckRefused("a TMD", dir / "encrypted" / "title.tmd", "one of a title's files");
    CheckRefused("a content", dir / "encrypted" / FileOf(title.contents[0], Naming::kIndex), "one of a title's files");
}

} // namespace

// usage: title_test [folder]. With a folder, the titles are written there and kept.
int main(int argc, char** argv)
{
    const bool keep = argc > 1;
    const fs::path dir =
        keep ? fs::path(argv[1])
             : fs::temp_directory_path() / ("ntdlib_title_test_" + std::to_string(std::random_device{}()));
    try
    {
        fs::create_directories(dir);
        TestLz();
        TestHuffman();
        TestTitles(dir);
        TestOtherTitles(dir);
        TestRefusals(dir);
    }
    catch (const std::exception& e)
    {
        Check(false, std::string("unexpected error: ") + e.what());
    }

    if (!keep)
    {
        std::error_code error;
        fs::remove_all(dir, error);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
