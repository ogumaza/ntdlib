// SPDX-License-Identifier: MIT

// Checks the decoder of LZ4 blocks and the reader of NSO executables on blocks and files built here: copies that
// overlap their output, numbers that continue in more bytes, the segments of an NSO, and the messages for damaged
// data.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nx/lz4.h"
#include "ntdlib/nx/nso.h"
#include "ntdlib/testing/nso_files.h"

namespace nx = ntdlib::nx;

using ntdlib::testing::Lz4Literals;
using ntdlib::testing::NsoFile;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

namespace
{

// "a", then a copy of 280 bytes from 1 byte back (4 + 15 + 255 + 6), then "xyz".
const std::vector<uint8_t> kRun = {0x1f, 'a', 1, 0, 255, 6, 0x30, 'x', 'y', 'z'};

// The text that `block` holds, or the message with which DecompressLz4Block rejects it.
std::string Decompressed(const std::vector<uint8_t>& block, std::size_t size)
{
    std::string text;
    const auto decompress = [&]
    {
        const std::vector<uint8_t> bytes = nx::DecompressLz4Block(block, size);
        text.assign(bytes.begin(), bytes.end());
    };
    const std::string error = ErrorOf(decompress);

    return error.empty() ? text : error;
}

// `size` bytes in which byte i holds i * `step`.
std::vector<uint8_t> Pattern(std::size_t size, std::size_t step)
{
    std::vector<uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; i++)
    {
        bytes[i] = static_cast<uint8_t>(i * step);
    }

    return bytes;
}

// Literals, and copies of earlier output. A copy may overlap the bytes that it writes.
void TestLz4Sequences()
{
    // "ab", then a copy of 6 bytes from 2 bytes back, then the last sequence's 5 literals.
    const std::vector<uint8_t> kCopies = {0x22, 'a', 'b', 2, 0, 0x50, 'x', 'y', 'z', '1', '2'};

    Check(Decompressed({0x50, 'h', 'e', 'l', 'l', 'o'}, 5) == "hello", "a block of literals");
    Check(Decompressed(kCopies, 13) == "ababababxyz12", "a copy that overlaps the bytes that it writes");
    Check(Decompressed(kRun, 284) == std::string(281, 'a') + "xyz", "a copy whose length continues in more bytes");
}

// A number of literals of 15 continues in a byte of 0, and one of 270 in a byte of 255 and a byte of 0.
void TestLz4LongLiterals()
{
    const std::vector<uint8_t> fifteen = Lz4Literals(Pattern(15, 1));
    const std::vector<uint8_t> long_run = Lz4Literals(Pattern(270, 1));
    Check(fifteen.size() == 17 && fifteen[0] == 0xf0 && fifteen[1] == 0, "the bytes of 15 literals");
    Check(long_run.size() == 273 && long_run[0] == 0xf0 && long_run[1] == 255 && long_run[2] == 0,
          "the bytes of 270 literals");

    for (const std::size_t count : {14, 15, 16, 269, 270, 271, 600})
    {
        const std::vector<uint8_t> bytes = Pattern(count, 7);
        Check(nx::DecompressLz4Block(Lz4Literals(bytes), count) == bytes, std::to_string(count) + " literals");
    }
}

// A damaged block, and blocks that hold more or fewer bytes than the size given.
void TestLz4Refusals()
{
    const std::string kEndsEarly = "the LZ4 block ends early";
    const std::string kUnwritten = "the LZ4 block copies bytes that it hasn't written";
    const std::vector<uint8_t> kHello = {0x50, 'h', 'e', 'l', 'l', 'o'};

    Check(Decompressed({}, 0) == kEndsEarly, "an empty block");
    Check(Decompressed({0x50, 'h', 'e', 'l'}, 5) == kEndsEarly, "literals cut short");
    Check(Decompressed({0xf0, 255}, 300) == kEndsEarly, "a number of literals cut short");
    Check(Decompressed({0x10, 'a', 1}, 5) == kEndsEarly, "an offset cut short");
    Check(Decompressed({0x1f, 'a', 1, 0, 255}, 300) == kEndsEarly, "a copy's length cut short");
    Check(Decompressed({0x10, 'a', 0, 0, 0x10, 'b'}, 6) == kUnwritten, "a copy from 0 bytes back");
    Check(Decompressed({0x10, 'a', 2, 0, 0x10, 'b'}, 6) == kUnwritten, "a copy from before the start");
    Check(Decompressed(kHello, 4) == "the LZ4 block doesn't hold 4 bytes", "literals past the size");
    Check(Decompressed(kHello, 6) == "the LZ4 block doesn't hold 6 bytes", "a block that ends before the size");
    Check(Decompressed(kRun, 100) == "the LZ4 block doesn't hold 100 bytes", "a copy past the size");
}

// Each segment comes back at its address with its bytes, stored compressed or as it is, with or without a hash.
void TestNso()
{
    const std::vector<uint8_t> text = Pattern(0x1234, 3);
    const std::vector<uint8_t> rodata = Pattern(0x800, 5);
    const std::vector<uint8_t> data = Pattern(0x2001, 7);
    const std::vector<uint8_t> file = NsoFile({text}, {rodata, false}, {data, true, false});

    const nx::Nso nso = nx::ReadNso(file);

    Check(nso.text.address == 0 && nso.text.bytes == text, "the text segment");
    Check(nso.rodata.address == 0x2000 && nso.rodata.bytes == rodata, "the rodata segment, stored as it is");
    Check(nso.data.address == 0x3000 && nso.data.bytes == data, "the data segment, without a hash");
}

// A file that isn't an NSO, a file cut short, a segment that doesn't match its hash and a damaged LZ4 block.
void TestNsoRefusals()
{
    const std::vector<uint8_t> file = NsoFile({Pattern(0x100, 3)}, {Pattern(0x10, 5)}, {Pattern(0x20, 7)});
    const auto refusal = [](std::span<const uint8_t> nso)
    {
        return ErrorOf([&] { nx::ReadNso(nso); });
    };

    // `changed` flips a bit of the text's second literal, after the token and a byte of the number of literals.
    // `damaged` adds 1 to the data's byte of the number of literals. The block then ends early.
    std::vector<uint8_t> other = file;
    other[3] = '1';
    std::vector<uint8_t> changed = file;
    changed[xsflib::LoadLe32(&file[0x10]) + 3] ^= 1;
    std::vector<uint8_t> damaged = file;
    damaged[xsflib::LoadLe32(&file[0x30]) + 1]++;

    Check(refusal(file).empty(), "the file as it was built");
    Check(refusal(other) == "not an NSO" && refusal(std::span(file).first(3)) == "not an NSO",
          "a file without the tag");
    Check(refusal(std::span(file).first(0x80)) == "the NSO is damaged", "a header cut short");
    Check(refusal(std::span(file).first(file.size() - 1)) == "the NSO is damaged", "a segment cut short");
    Check(refusal(changed) == "the NSO's text segment doesn't match its hash", "a segment that doesn't match its hash");
    Check(refusal(damaged) == "the NSO's data segment: the LZ4 block ends early", "a damaged block");
}

} // namespace

int main()
{
    TestLz4Sequences();
    TestLz4LongLiterals();
    TestLz4Refusals();
    TestNso();
    TestNsoRefusals();

    return xsflib::testing::Summary("nx_nso");
}
