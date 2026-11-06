// SPDX-License-Identifier: MIT

// Unit tests for the GBA's shared code: the reader of GSF files and its inflate, the plan of a song's loop, and the
// runner of Thumb routines.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/testing/check.h"

#include "agb/inflate.h"
#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/agb/thumb.h"
#include "ntdlib/testing/agb_cart.h"

namespace fs = std::filesystem;

namespace ntdlib::agb
{
using ntdlib::testing::agb_files::MakeTempFolder;
using ntdlib::testing::agb_files::TempPath;

namespace
{

// Returns a zlib stream holding `data` in stored blocks.
std::vector<uint8_t> ZlibStored(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t pos = 0;
    do
    {
        const size_t n = std::min<size_t>(data.size() - pos, 0xFFFF);
        const bool last = pos + n == data.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), data.begin() + long(pos), data.begin() + long(pos + n));
        pos += n;
    } while (pos < data.size());

    uint32_t a = 1, b = 0;
    for (uint8_t c : data)
    {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8)
    {
        z.push_back(uint8_t(adler >> s));
    }

    return z;
}

// Writes a GSF file whose program puts `data` at `address`, with the given tags.
void WriteGsf(const fs::path& path, uint32_t address, const std::vector<uint8_t>& data, const std::string& tags)
{
    std::vector<uint8_t> program;
    for (uint32_t v : {kRomBase, address, uint32_t(data.size())})
    {
        for (int i = 0; i < 4; i++)
        {
            program.push_back(uint8_t(v >> (8 * i)));
        }
    }
    program.insert(program.end(), data.begin(), data.end());

    const std::vector<uint8_t> z = ZlibStored(program);
    std::vector<uint8_t> f = {'P', 'S', 'F', 0x22};
    for (uint32_t v : {0u, uint32_t(z.size()), 0u})
    {
        for (int i = 0; i < 4; i++)
        {
            f.push_back(uint8_t(v >> (8 * i)));
        }
    }
    f.insert(f.end(), z.begin(), z.end());
    if (!tags.empty())
    {
        const std::string t = "[TAG]" + tags;
        f.insert(f.end(), t.begin(), t.end());
    }

    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), std::streamsize(f.size()));
}

void TestInflate()
{
    static const uint8_t kStored[] = {0x78, 0x01, 0x01, 0x15, 0x00, 0xEA, 0xFF, 0x47, 0x42, 0x41, 0x20,
                                      0x73, 0x6F, 0x75, 0x6E, 0x64, 0x20, 0x64, 0x72, 0x69, 0x76, 0x65,
                                      0x72, 0x20, 0x74, 0x65, 0x73, 0x74, 0x4E, 0xB2, 0x07, 0xA0};
    static const uint8_t kFixed[] = {0x78, 0x01, 0x73, 0x77, 0x72, 0x54, 0x28, 0xCE, 0x2F, 0xCD,
                                     0x4B, 0x51, 0x48, 0x29, 0xCA, 0x2C, 0x4B, 0x2D, 0x52, 0x28,
                                     0x49, 0x2D, 0x2E, 0x01, 0x00, 0x4E, 0xB2, 0x07, 0xA0};
    static const uint8_t kDynamic[] = {
        0x78, 0xDA, 0x4D, 0xCD, 0x41, 0x0E, 0x80, 0x30, 0x08, 0x04, 0xC0, 0xAF, 0xEC, 0xD7, 0x6A, 0xE5, 0x60,
        0x6C, 0x0B, 0x51, 0xE2, 0xFB, 0x8D, 0x0B, 0x89, 0x5C, 0x08, 0x81, 0x61, 0x59, 0xEA, 0x02, 0xBF, 0x5A,
        0x3F, 0x71, 0xB7, 0x69, 0x43, 0xF0, 0xE8, 0xD1, 0x05, 0x43, 0xD5, 0xE0, 0x32, 0x4D, 0xB1, 0x3E, 0xB2,
        0xC9, 0xDA, 0xD3, 0x05, 0x48, 0xCD, 0x25, 0x31, 0x3B, 0x96, 0x38, 0xE3, 0x45, 0x0D, 0xAB, 0x4F, 0x4A,
        0x26, 0x71, 0x38, 0x4E, 0x53, 0x04, 0xFF, 0x93, 0x4B, 0x5C, 0x88, 0x17, 0x22, 0x42, 0x44, 0xB8};
    const std::string kText = "GBA sound driver test";
    const std::string kWords =
        "note track sample voice loop tempo note bend track voice sample note loop note note tempo bend voice loop "
        "track sample note bend tempo voice note sample track loop note bend voice sample";

    // A dynamic block holding "A", with a distance code that no symbol uses: a single code, one bit long.
    static const uint8_t kOneBitDistance[] = {0x78, 0x01, 0x05, 0xC0, 0x01, 0x09, 0x00, 0x00, 0x00, 0x80,
                                              0xA0, 0x6D, 0xFE, 0x3F, 0xA5, 0x02, 0x00, 0x42, 0x00, 0x42};

    std::vector<uint8_t> out;
    std::string err;

    SEQLIB_CHECK(ZlibDecompress(kStored, sizeof kStored, out, err));
    SEQLIB_CHECK(std::string(out.begin(), out.end()) == kText);
    SEQLIB_CHECK(ZlibDecompress(kFixed, sizeof kFixed, out, err));
    SEQLIB_CHECK(std::string(out.begin(), out.end()) == kText);
    SEQLIB_CHECK(ZlibDecompress(kDynamic, sizeof kDynamic, out, err));
    SEQLIB_CHECK(std::string(out.begin(), out.end()) == kWords);

    std::vector<uint8_t> bad(kDynamic, kDynamic + sizeof kDynamic);
    bad.back() ^= 1; // Adler-32 mismatch

    SEQLIB_CHECK(!ZlibDecompress(bad.data(), bad.size(), out, err));
    SEQLIB_CHECK(!ZlibDecompress(kDynamic, 20, out, err));                  // truncated
    SEQLIB_CHECK(!ZlibDecompress(kDynamic, sizeof kDynamic - 4, out, err)); // no Adler-32

    // A single code that doesn't fill its code space has to be one bit long, as zlib and puff require. The same block
    // with a distance code of two bits is corrupt.
    std::vector<uint8_t> two_bit(kOneBitDistance, kOneBitDistance + sizeof kOneBitDistance);
    two_bit[14] = 0x65;

    SEQLIB_CHECK(ZlibDecompress(kOneBitDistance, sizeof kOneBitDistance, out, err));
    SEQLIB_CHECK(std::string(out.begin(), out.end()) == "A");
    SEQLIB_CHECK(!ZlibDecompress(two_bit.data(), two_bit.size(), out, err));
}

void TestGsfLoading()
{
    // A GSF set in a folder whose name isn't ASCII: the library holds the cartridge, and the mini-GSF names it and
    // patches one byte.
    std::error_code ec;
    const fs::path dir = TempPath("gsf_\xC3\xA9\xE3\x81\x82");
    fs::create_directories(dir, ec);
    SEQLIB_CHECK(!ec);
    if (ec)
    {
        return;
    }

    std::vector<uint8_t> cart(0x1000, 0);
    const uint8_t kMarker[] = {0x78, 0x56, 0x34, 0x12};
    std::copy(std::begin(kMarker), std::end(kMarker), cart.begin() + 0x100);
    WriteGsf(dir / "set.gsflib", kRomBase, cart, "");
    WriteGsf(dir / "set-01.minigsf", kRomBase + 0xF00, {0x5A}, "title=Song 1\n_lib=set.gsflib\n");

    Rom rom;
    std::string err;

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-01.minigsf"), err));

    SEQLIB_CHECK(rom.FromGsf());
    SEQLIB_CHECK(rom.GsfLibrary() == Utf8(dir / "set.gsflib"));
    SEQLIB_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678); // from the library
    SEQLIB_CHECK_EQ(rom.U8(kRomBase + 0xF00), 0x5A);        // from the mini-GSF

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set.gsflib"), err));

    SEQLIB_CHECK(rom.GsfLibrary().empty());

    // _lib2 goes on top of the file's program.
    WriteGsf(dir / "patch.gsflib", kRomBase + 0xF00, {0xA5}, "");
    WriteGsf(dir / "set-02.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=set.gsflib\n_lib2=patch.gsflib\n");

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-02.minigsf"), err));

    SEQLIB_CHECK_EQ(rom.U8(kRomBase + 0xF00), 0xA5);
    SEQLIB_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678);

    // A _lib tag whose case differs from the library's name finds the library.
    WriteGsf(dir / "set-03.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=SET.GSFLIB\n");

    SEQLIB_CHECK(rom.Load(Utf8(dir / "set-03.minigsf"), err));

    SEQLIB_CHECK_EQ(rom.U32(kRomBase + 0x100), 0x12345678);

    WriteGsf(dir / "orphan.minigsf", kRomBase + 0xF00, {0x5A}, "_lib=missing.gsflib\n");

    SEQLIB_CHECK(!rom.Load(Utf8(dir / "orphan.minigsf"), err));

    SEQLIB_CHECK(err == "the library missing.gsflib can't be read");

    // A folder dropped on the program by mistake.
    SEQLIB_CHECK(!rom.Load(Utf8(dir), err));

    SEQLIB_CHECK(err == "it's a folder, not a ROM or GSF file");

    // A zipped ROM, which has to be taken out of its archive first.
    std::vector<uint8_t> zip(0x200, 0);
    std::memcpy(zip.data(), "PK\x03\x04", 4);
    std::string write_error;
    SEQLIB_CHECK(WriteFile(Utf8(dir / "game.zip"), zip, write_error));

    SEQLIB_CHECK(!rom.Load(Utf8(dir / "game.zip"), err));

    SEQLIB_CHECK(err.find("it's an archive") == 0);

    fs::remove_all(dir, ec);
}

// A song's loop lasts until every track's loop is back where it started: loops of 3 and 4 bars make one of 12, and a
// loop that divides the longest leaves it as it is. A loop more than 8 times the longest isn't used: 72 ticks for loops
// of 8 and 9 is, but 90 for loops of 9 and 10 isn't, and the longest loop is the song's.
void TestLoopLength()
{
    SEQLIB_CHECK_EQ(LoopLength({7}), uint64_t(7));
    SEQLIB_CHECK_EQ(LoopLength({960, 480}), uint64_t(960));
    SEQLIB_CHECK_EQ(LoopLength({9216, 12288}), uint64_t(36864));
    SEQLIB_CHECK_EQ(LoopLength({8, 9}), uint64_t(72));
    SEQLIB_CHECK_EQ(LoopLength({9, 10}), uint64_t(10));
    SEQLIB_CHECK_EQ(LoopLength({6138, 6144}), uint64_t(6144));
}

// RunThumb() runs ldmia and stmia: one routine loads two words from the ROM with ldmia and adds them, and another
// stores its arguments on the stack with stmia and loads the second one back. A routine that stores a byte at the top
// of the address space, which isn't on the stack, gets no result.
void TestRunThumb()
{
    // The first routine is ldr r1, =words; ldmia r1!, {r0, r2}; adds r0, r0, r2; bx lr, followed by the words' address,
    // 5 and 7. The second, at 0x14, is sub sp, #8; mov r3, sp; stmia r3!, {r0, r1}; ldr r0, [sp, #4]; add sp, #8;
    // bx lr. The third, at 0x20, is movs r0, #0; subs r0, #1; strb r0, [r0]; ldrb r0, [r0]; bx lr.
    Rom rom;
    rom.Assign({0x01, 0x49, 0x05, 0xC9, 0x80, 0x18, 0x70, 0x47, 0x0C, 0x00, 0x00, 0x08, 0x05, 0x00,
                0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x82, 0xB0, 0x6B, 0x46, 0x03, 0xC3, 0x01, 0x98,
                0x02, 0xB0, 0x70, 0x47, 0x00, 0x20, 0x01, 0x38, 0x00, 0x70, 0x00, 0x78, 0x70, 0x47});

    const std::optional<uint32_t> sum = RunThumb(rom, kRomBase, {0, 0, 0, 0});
    const std::optional<uint32_t> second = RunThumb(rom, kRomBase + 0x14, {3, 9, 0, 0});
    const std::optional<uint32_t> top = RunThumb(rom, kRomBase + 0x20, {0, 0, 0, 0});

    SEQLIB_CHECK(sum && *sum == 12);
    SEQLIB_CHECK(second && *second == 9);
    SEQLIB_CHECK(!top);
}

// Runs every test. Returns the exit code.
int RunTests()
{
    if (!ntdlib::testing::agb_files::MakeTempFolder())
    {
        std::fprintf(stderr, "can't create a temporary folder for the test output\n");
        return 1;
    }

    TestInflate();
    TestGsfLoading();
    TestLoopLength();
    TestRunThumb();

    std::error_code ec;
    std::filesystem::remove_all(ntdlib::testing::agb_files::g_temp, ec);
    return seqlib::testing::Summary("agb");
}

} // namespace
} // namespace ntdlib::agb

int main()
{
    return ntdlib::agb::RunTests();
}
