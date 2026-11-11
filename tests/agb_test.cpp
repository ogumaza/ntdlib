// SPDX-License-Identifier: MIT

// Unit tests for the GBA's shared code: the reader of GSF files, the plan of a song's loop, and the runner of Thumb
// routines.

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

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/agb/thumb.h"
#include "ntdlib/testing/agb_cart.h"

namespace fs = std::filesystem;

namespace ntdlib::agb
{
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
    TestGsfLoading();
    TestLoopLength();
    TestRunThumb();

    return seqlib::testing::Summary("agb");
}

} // namespace
} // namespace ntdlib::agb

int main()
{
    return ntdlib::agb::RunTests();
}
