// SPDX-License-Identifier: MIT

// Tests of the FSYS reader and of LZSS. The LZSS streams are written field by field, as fsys.h describes them: bytes
// stored as they are, copies of earlier output, copies that repeat the bytes that they write, copies from the window's
// first zeros, copies across the window's end, and copies that the data's size cuts short. The FSYS archive is made up
// here. It holds a file stored as it is and one stored with LZSS. The test also checks damaged streams and archives.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/disc/fsys.h"
#include "ntdlib/testing/big_endian_bytes.h"

namespace
{

namespace disc = ntdlib::disc;
using Bytes = std::vector<uint8_t>;
using ntdlib::testing::BigEndianBytes;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// The place of the first byte of output in the window.
constexpr uint32_t kFirstPlace = 0xfee;

// LZSS data that decompresses to `size` bytes: the header, with the stored size of `stream` and the header, then
// `stream`.
Bytes Lzss(uint32_t size, const Bytes& stream)
{
    BigEndianBytes b;
    b.Tag("LZSS").U32(size).U32(static_cast<uint32_t>(16 + stream.size())).U32(0).Append(stream);

    return b.Data();
}

// A copy's two bytes: `length` bytes from place `from` of the window.
Bytes Copy(uint32_t from, uint32_t length)
{
    return {static_cast<uint8_t>(from), static_cast<uint8_t>((from >> 4 & 0xf0) | (length - 3))};
}

// `first`, then each of `more` after it.
Bytes Join(Bytes first, const std::vector<Bytes>& more)
{
    for (const Bytes& part : more)
    {
        first.insert(first.end(), part.begin(), part.end());
    }

    return first;
}

Bytes Text(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

// Bytes stored as they are: a flag byte of eight 1s with eight bytes, then a flag byte whose first two items are bytes.
// The data ends there. The reader stops at the data's size and reads none of the second flag byte's other items. Data
// of no bytes is a header alone.
void TestStoredBytes()
{
    const Bytes data = Lzss(10, Join({0xff}, {Text("abcdefgh"), {0x03}, Text("ij")}));
    const Bytes empty = Lzss(0, {});

    const Bytes out = disc::DecompressLzss(data);

    Check(out == Text("abcdefghij"), "a stream of bytes stored as they are");
    Check(disc::DecompressLzss(empty).empty(), "a header alone decompresses to no bytes");
}

// Copies, each in a stream of its own.
void TestCopies()
{
    // Three bytes, then a copy of them from their places in the window.
    const Bytes again = Lzss(6, Join({0x07}, {Text("abc"), Copy(kFirstPlace, 3)}));
    // A byte, then a copy that starts at it. Each byte that the copy writes is the next byte that it reads.
    const Bytes repeat = Lzss(7, Join({0x01}, {Text("x"), Copy(kFirstPlace, 6)}));
    // A copy from the window's first bytes, which start as zeros, then a byte.
    const Bytes zeros = Lzss(6, Join({0x02}, {Copy(0, 5), Text("z")}));
    // Twenty bytes. The 19th and the 20th go to places 0 and 1 of the window. The copy reads the 17th to the 20th, from
    // place 0xffe on, across the window's end.
    const Bytes across =
        Lzss(24, Join({0xff}, {Text("ABCDEFGH"), {0xff}, Text("IJKLMNOP"), {0x0f}, Text("QRST"), Copy(0xffe, 4)}));
    // The longest copy, 18 bytes, after one byte.
    const Bytes longest = Lzss(19, Join({0x01}, {Text("a"), Copy(kFirstPlace, 18)}));
    // The same stream with a size of 5. The size cuts the copy short.
    const Bytes cut = Lzss(5, Join({0x01}, {Text("a"), Copy(kFirstPlace, 18)}));

    Check(disc::DecompressLzss(again) == Text("abcabc"), "a copy of earlier output");
    Check(disc::DecompressLzss(repeat) == Text("xxxxxxx"), "a copy that repeats the bytes that it writes");
    Check(disc::DecompressLzss(zeros) == Bytes{0, 0, 0, 0, 0, 'z'}, "a copy from the window's first zeros");
    Check(disc::DecompressLzss(across) == Text("ABCDEFGHIJKLMNOPQRSTQRST"), "a copy across the window's end");
    Check(disc::DecompressLzss(longest) == Bytes(19, 'a'), "a copy of 18 bytes");
    Check(disc::DecompressLzss(cut) == Bytes(5, 'a'), "the data's size cuts a copy short");
}

// LZSS data that is damaged or truncated.
void TestDamagedLzss()
{
    Bytes not_lzss = Lzss(1, Join({0x01}, {Text("a")}));
    not_lzss[3] = 'X';
    const Bytes good = Lzss(2, Join({0x03}, {Text("ab")}));
    const Bytes short_header(good.begin(), good.begin() + 15);
    Bytes small_stored = good;
    small_stored[11] = 15;
    Bytes past_end = good;
    past_end[11] = static_cast<uint8_t>(good.size() + 1);
    // The stored size leaves out the second byte. The reader reads no further than the stored size.
    Bytes cut_stream = good;
    cut_stream[11] = static_cast<uint8_t>(good.size() - 1);
    const Bytes half_copy = Lzss(3, {0x00, 0xee});
    // Three bytes of stream give at most 27 bytes.
    const Bytes too_large = Lzss(28, Join({0x00}, {Copy(kFirstPlace, 18)}));
    const Bytes largest = Lzss(27, Join({0x00}, {Copy(kFirstPlace, 18)}));

    Check(ErrorOf([&] { disc::DecompressLzss(not_lzss); }) == "LZSS data doesn't start with \"LZSS\"",
          "data without the magic");
    Check(ErrorOf([&] { disc::DecompressLzss(short_header); }) == "LZSS data's header runs past the end of its data",
          "a header cut short");
    Check(ErrorOf([&] { disc::DecompressLzss(small_stored); }) == "LZSS data's stored size is smaller than its header",
          "a stored size smaller than the header");
    Check(ErrorOf([&] { disc::DecompressLzss(past_end); }) == "LZSS data runs past the end of its data",
          "a stored size past the data's end");
    Check(ErrorOf([&] { disc::DecompressLzss(cut_stream); }) == "LZSS data's stream runs past the end of its data",
          "a stream that ends before the data's size");
    Check(ErrorOf([&] { disc::DecompressLzss(half_copy); }) == "LZSS data's stream runs past the end of its data",
          "a copy cut in half");
    Check(ErrorOf([&] { disc::DecompressLzss(too_large); }) == "LZSS data gives a size that its stream can't hold",
          "a size that the stream can't hold");
    Check(ErrorOf([&] { disc::DecompressLzss(largest); }) == "LZSS data's stream runs past the end of its data",
          "the largest size that the stream may hold is read until the stream ends");
}

// Where the made-up archive holds its parts.
constexpr std::size_t kInformation = 0x40;
constexpr std::size_t kTable = 0x60;
constexpr std::size_t kStoredEntry = 0x70;
constexpr std::size_t kPackedEntry = 0x98;
constexpr std::size_t kNames = 0xc0;
constexpr std::size_t kStoredData = 0x100;
constexpr std::size_t kPackedData = 0x108;

// An FSYS archive with two files. The table lists "packed" first, then "stored". "stored" holds five bytes as they are.
// "packed" holds seven bytes with LZSS, and its entry gives the LZSS data's stored size. The archive ends with the LZSS
// data. Its last byte isn't zero.
Bytes MakeArchive()
{
    const Bytes packed = Lzss(7, Join({0x01}, {Text("x"), Copy(kFirstPlace, 6)}));

    BigEndianBytes b;
    b.Tag("FSYS").U32(0x104).U32(0).U32(2).U32(0x80000000).U32(3).U32(kInformation).U32(kStoredData);
    b.PadTo(kInformation).U32(kTable).U32(kNames).U32(kStoredData);
    b.PadTo(kTable).U32(kPackedEntry).U32(kStoredEntry);
    b.PadTo(kStoredEntry).U32(0x00100800).U32(kStoredData).U32(5).U32(0).U32(0).U32(5).U32(0).U32(0).U32(4);
    b.U32(kNames);
    b.U32(0x01c71c00).U32(kPackedData).U32(7).U32(0x80000000).U32(0).U32(static_cast<uint32_t>(packed.size()));
    b.U32(0).U32(0).U32(0xe).U32(kNames + 7);
    b.Append(Text("stored")).U8(0).Append(Text("packed")).U8(0);
    b.PadTo(kStoredData).Append({1, 2, 3, 4, 5});
    b.PadTo(kPackedData).Append(packed);

    return b.Data();
}

void TestArchive()
{
    const Bytes archive = MakeArchive();

    const std::vector<disc::FsysFile> files = disc::ReadFsys(archive);

    Check(files.size() == 2, "the archive lists two files");
    if (files.size() == 2)
    {
        Check(files[0].id == 0x01c71c00 && files[0].name == "packed" && files[0].data == Text("xxxxxxx"),
              "the table's first file, decompressed");
        Check(files[1].id == 0x00100800 && files[1].name == "stored" && files[1].data == Bytes{1, 2, 3, 4, 5},
              "the table's second file, as it is stored");
    }
}

// The archive of MakeArchive with the word at `at` set to `value`.
Bytes WithWord(std::size_t at, uint32_t value)
{
    BigEndianBytes b;
    b.Append(MakeArchive());
    b.Put32(at, value);

    return b.Data();
}

// The message of the error that reading `archive` throws, or "" if it throws none.
std::string ReadError(const Bytes& archive)
{
    return ErrorOf([&archive] { disc::ReadFsys(archive); });
}

void TestDamagedArchives()
{
    const Bytes archive = MakeArchive();
    const auto size = static_cast<uint32_t>(archive.size());
    Bytes not_fsys = archive;
    not_fsys[3] = 'X';
    const Bytes short_header(archive.begin(), archive.begin() + 0x18);
    Bytes bad_lzss = archive;
    bad_lzss[kPackedData + 3] = 'X';

    Check(ReadError(not_fsys) == "the archive doesn't start with \"FSYS\"", "data without the magic");
    Check(ReadError(short_header) == "an FSYS archive's header runs past the end of its data", "a header cut short");
    Check(ReadError(WithWord(0x18, size)) == "an FSYS archive's information runs past the end of its data",
          "an information block past the archive's end");
    Check(ReadError(WithWord(kInformation, size)) == "an FSYS archive's table runs past the end of its data",
          "a table past the archive's end");
    Check(ReadError(WithWord(kTable, size - 0x27)) == "an FSYS archive's entry runs past the end of its data",
          "an entry that runs past the archive's end");
    Check(ReadError(WithWord(kStoredEntry + 0x24, size + 1)) == "an FSYS file's name runs past the end of its data",
          "a name past the archive's end");
    Check(ReadError(WithWord(kPackedEntry + 0x24, size - 2)) == "an FSYS file's name has no end",
          "a name without a zero byte before the archive's end");
    Check(ReadError(WithWord(kStoredEntry + 8, size)) == "an FSYS archive's file runs past the end of its data",
          "a file stored as it is that runs past the archive's end");
    Check(ReadError(WithWord(kPackedEntry + 0x14, size)) == "an FSYS archive's file runs past the end of its data",
          "LZSS data that runs past the archive's end");
    Check(ReadError(WithWord(kPackedEntry + 8, 8)) == "FSYS file packed decompresses to a size other than its entry's",
          "a file that decompresses to a size other than its entry's");
    Check(ReadError(bad_lzss) == "LZSS data doesn't start with \"LZSS\"", "a file with damaged LZSS data");
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
    Run("TestStoredBytes", TestStoredBytes);
    Run("TestCopies", TestCopies);
    Run("TestDamagedLzss", TestDamagedLzss);
    Run("TestArchive", TestArchive);
    Run("TestDamagedArchives", TestDamagedArchives);

    return xsflib::testing::Summary("disc FSYS");
}
