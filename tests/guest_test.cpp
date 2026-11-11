// SPDX-License-Identifier: MIT

// Tests of the guest (ntdlib/testing/guest.h) on a main program made up here: the small-data bases, calls with
// arguments and a return value, the stack, the allocation hooks, floats in memory, a program compressed with LZ77, and
// the errors for a file that isn't a DOL, a DOL without __init_registers, an instruction that the interpreter doesn't
// support, a call that doesn't return and an address outside memory. Tests of LoadRel on a module made up here: the
// order of its sections, each type of relocation against the module and against the main program, and the errors for
// a module that ends early, imports another module or has another type of relocation. The programs go in a temporary
// folder.

#include "ntdlib/testing/guest.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "ntdlib/testing/dol_files.h"

namespace
{

using ntdlib::testing::Guest;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// The main program's text section and its functions.
constexpr uint32_t kText = 0x80004000;
constexpr uint32_t kAdd = kText + 0x14;      // r3 + r4
constexpr uint32_t kSda2 = kText + 0x1c;     // r2
constexpr uint32_t kSda = kText + 0x24;      // r13
constexpr uint32_t kSetHooks = kText + 0x2c; // stores r3 and r4 at r13 + 16 and r13 + 20
constexpr uint32_t kCallHook = kText + 0x38; // calls the function whose address is at r13 + r4, with r3
constexpr uint32_t kDouble = kText + 0x60;   // stores twice the float at r3 at r4
constexpr uint32_t kLoop = kText + 0x70;     // b .
constexpr uint32_t kIllegal = kText + 0x74;  // a word of 0

// The small-data bases that the program's __init_registers sets.
constexpr uint32_t kSda2Base = 0x804ffff0;
constexpr uint32_t kSdaBase = 0x80401234;

// The program's code, from kText.
const std::vector<uint32_t> kCode = {
    0x3c408050, 0x3842fff0, 0x3da08040, 0x61ad1234, 0x4e800020, // lis r2; addi r2; lis r13; ori r13; blr
    0x7c632214, 0x4e800020,                                     // add r3, r3, r4
    0x7c431378, 0x4e800020,                                     // mr r3, r2
    0x7da36b78, 0x4e800020,                                     // mr r3, r13
    0x906d0010, 0x908d0014, 0x4e800020,                         // stw r3, 16(r13); stw r4, 20(r13)
    0x7c0802a6, 0x90010004, 0x9421fff0,                         // mflr r0; stw r0, 4(r1); stwu r1, -16(r1)
    0x7d8d202e, 0x7d8903a6, 0x4e800421,                         // lwzx r12, r13, r4; mtctr r12; bctrl
    0x80010014, 0x38210010, 0x7c0803a6, 0x4e800020,             // lwz r0, 20(r1); addi r1, r1, 16; mtlr r0; blr
    0xc0230000, 0xec21082a, 0xd0240000, 0x4e800020,             // lfs f1, 0(r3); fadds f1, f1, f1; stfs f1, 0(r4)
    0x48000000,                                                 // b .
    0x00000000,
};

// A DOL with `code` in its first text section, at kText.
std::vector<uint8_t> MakeDol(const std::vector<uint32_t>& code)
{
    std::vector<uint8_t> dol(0x100 + code.size() * 4);
    ntdlib::testing::SetDolSection(dol, 0, 0x100, kText, static_cast<uint32_t>(code.size() * 4));
    for (std::size_t i = 0; i < code.size(); i++)
    {
        xsflib::StoreBe32(&dol[0x100 + 4 * i], code[i]);
    }

    return dol;
}

// `data` compressed with the CX library's LZ77 (type 0x10), as literals only: a flag byte of 0 before each 8 bytes.
std::vector<uint8_t> Lz77Literals(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out = {0x10, static_cast<uint8_t>(data.size()), static_cast<uint8_t>(data.size() >> 8),
                                static_cast<uint8_t>(data.size() >> 16)};
    for (std::size_t i = 0; i < data.size(); i++)
    {
        if (i % 8 == 0)
        {
            out.push_back(0);
        }

        out.push_back(data[i]);
    }

    return out;
}

std::filesystem::path WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

    return path;
}

void TestCalls(const std::filesystem::path& dol)
{
    Guest guest(dol);

    Check(guest.Read32(kText) == 0x3c408050, "the program is in MEM1");
    Check(guest.Call(kSda2) == kSda2Base, "r2: lis and addi");
    Check(guest.Call(kSda) == kSdaBase, "r13: lis and ori");
    Check(guest.Call(kAdd, {2, 3}) == 5, "a call with two arguments");
    Check(guest.LastCycles() > 0, "the instructions of the last call");
}

void TestHooks(const std::filesystem::path& dol)
{
    Guest guest(dol);

    guest.SetHooks(kSetHooks);
    const uint32_t first = guest.Call(kCallHook, {40, 16});
    const uint32_t second = guest.Call(kCallHook, {8, 16});
    const uint32_t freed = guest.Call(kCallHook, {first, 20});
    const uint32_t third = guest.Alloc(1);

    Check(guest.Read32(kSdaBase + 16) == Guest::kAllocHook && guest.Read32(kSdaBase + 20) == Guest::kFreeHook,
          "SetHooks passes the two stubs");
    Check(first == 0x90100000 && second == 0x90100040 && third == 0x90100060,
          "the allocation stub returns 32-byte aligned memory in MEM2");
    Check(freed == first, "the free stub returns");
    bool zero = true;
    for (uint32_t i = 0; i < 40; i += 4)
    {
        zero = zero && guest.Read32(first + i) == 0;
    }

    Check(zero, "allocated memory is zeroed");
}

void TestMemory(const std::filesystem::path& dol)
{
    Guest guest(dol);
    const uint32_t from = guest.Alloc(4);
    const uint32_t to = guest.Alloc(4);

    guest.WriteFloat(from, 1.5f);
    guest.Call(kDouble, {from, to});
    guest.Write32(0x817f0000, 0x12345678);

    Check(guest.ReadFloat(to) == 3.0f, "floats in memory");
    Check(guest.Read32(0x817f0000) == 0x12345678 && xsflib::LoadBe32(guest.Bytes(0x817f0000, 4)) == 0x12345678,
          "a word written in MEM1");
    Check(ErrorOf<std::runtime_error>([&guest] { guest.Bytes(0x88000000, 4); }) == "no memory at 0x88000000",
          "an address outside MEM1 and MEM2");
}

void TestErrors(const std::filesystem::path& folder)
{
    Guest guest(folder / "main.dol");
    const std::string loop = ErrorOf<std::runtime_error>([&guest] { guest.Call(kLoop, {}, 1000); });
    const std::string illegal = ErrorOf<std::runtime_error>([&guest] { guest.Call(kIllegal); });

    Check(loop.starts_with("the call of 0x80004070 stopped at "), "a call that doesn't return: " + loop);
    Check(illegal.starts_with("the call of 0x80004074 stopped at 0x80004074"),
          "an instruction that the interpreter doesn't support: " + illegal);
    Check(guest.Call(kAdd, {4, 5}) == 9, "a call after the errors");

    const std::filesystem::path short_file = WriteFile(folder / "short.dol", std::vector<uint8_t>(16));
    Check(
        ErrorOf<std::runtime_error>([&short_file] { Guest other(short_file); }) == short_file.string() + " isn't a DOL",
        "a file that isn't a DOL");
    const std::vector<uint32_t> no_init(kCode.begin() + 4, kCode.end());
    const std::filesystem::path no_init_file = WriteFile(folder / "no_init.dol", MakeDol(no_init));
    Check(ErrorOf<std::runtime_error>([&no_init_file] { Guest other(no_init_file); }) ==
              "the DOL doesn't set r2 and r13 as __init_registers does",
          "a DOL without __init_registers");
}

// A REL module with the ID 7 and four sections: none, .text (6 words at 0x80), .bss (0x40 bytes) and .data (2 words at
// 0xa0). Its imports at 0xc0 list its relocations against itself at 0xd0 and those against `other` (the main program)
// at 0x100. `type` is the type of the relocation at the main program's .text + 0x10.
std::vector<uint8_t> MakeRel(uint32_t other, uint8_t type)
{
    std::vector<uint8_t> rel(0x138);
    const auto word = [&rel](std::size_t at, uint32_t value)
    {
        xsflib::StoreBe32(&rel[at], value);
    };
    const auto relocation = [&rel](std::size_t at, uint16_t offset, uint8_t kind, uint8_t section, uint32_t addend)
    {
        xsflib::StoreBe16(&rel[at], offset);
        rel[at + 2] = kind;
        rel[at + 3] = section;
        xsflib::StoreBe32(&rel[at + 4], addend);
    };

    word(0x00, 7);
    word(0x0c, 4);
    word(0x10, 0x40);
    word(0x28, 0xc0);
    word(0x2c, 16);
    word(0x48, 0x81);
    word(0x4c, 0x18);
    word(0x54, 0x40);
    word(0x58, 0xa0);
    word(0x5c, 0x08);

    // .text: an ADDR32 site, lis r3, addi r3, lis r4, bl and beq. .data: an ADDR32 site and a word.
    word(0x84, 0x3c600000);
    word(0x88, 0x38630000);
    word(0x8c, 0x3c800000);
    word(0x90, 0x48000001);
    word(0x94, 0x41820000);
    word(0xa4, 0x11223344);

    word(0xc0, 7);
    word(0xc4, 0xd0);
    word(0xc8, other);
    word(0xcc, 0x100);

    relocation(0xd0, 0, 202, 1, 0);         // R_DOLPHIN_SECTION: .text
    relocation(0xd8, 0, 1, 3, 4);           // +0x0: R_PPC_ADDR32, .data + 4
    relocation(0xe0, 0xe, 5, 2, 0);         // +0xe: R_PPC_ADDR16_HI, .bss
    relocation(0xe8, 4, 201, 0, 0);         // R_DOLPHIN_NOP: +0x12
    relocation(0xf0, 2, 11, 1, 0);          // +0x14: R_PPC_REL14, .text
    relocation(0xf8, 0, 203, 0, 0);         // R_DOLPHIN_END
    relocation(0x100, 0, 202, 1, 0);        // R_DOLPHIN_SECTION: .text
    relocation(0x108, 6, 6, 0, 0x80019000); // +0x6: R_PPC_ADDR16_HA
    relocation(0x110, 4, 4, 0, 0x80019000); // +0xa: R_PPC_ADDR16_LO
    relocation(0x118, 6, type, 0, kAdd);    // +0x10: R_PPC_REL24
    relocation(0x120, 0, 202, 3, 0);        // R_DOLPHIN_SECTION: .data
    relocation(0x128, 0, 1, 0, kText);      // +0x0: R_PPC_ADDR32
    relocation(0x130, 0, 203, 0, 0);        // R_DOLPHIN_END

    return rel;
}

void TestRel(const std::filesystem::path& dol)
{
    constexpr uint32_t kModule = 0x80400000;
    Guest guest(dol);

    const std::vector<uint32_t> bases = ntdlib::testing::LoadRel(guest, MakeRel(0, 10), kModule);

    Check(bases == std::vector<uint32_t>{0, kModule, kModule + 0x40, kModule + 0x20},
          ".text first, then .data, then .bss, each 32-byte aligned");
    Check(guest.Read32(kModule) == kModule + 0x24, "R_PPC_ADDR32 against the module");
    Check(guest.Read32(kModule + 0x4) == 0x3c608002, "R_PPC_ADDR16_HA against the main program");
    Check(guest.Read32(kModule + 0x8) == 0x38639000, "R_PPC_ADDR16_LO against the main program");
    Check(guest.Read32(kModule + 0xc) == 0x3c808040, "R_PPC_ADDR16_HI against the module");
    Check(guest.Read32(kModule + 0x10) == 0x4bc04005, "R_PPC_REL24 against the main program");
    Check(guest.Read32(kModule + 0x14) == 0x4182ffec, "R_PPC_REL14 against the module");
    Check(guest.Read32(kModule + 0x20) == kText && guest.Read32(kModule + 0x24) == 0x11223344,
          "R_PPC_ADDR32 against the main program, in .data");
}

void TestRelErrors(const std::filesystem::path& dol)
{
    Guest guest(dol);
    const std::vector<uint8_t> rel = MakeRel(0, 10);
    const std::vector<uint8_t> short_rel(rel.begin(), rel.begin() + 0x20);
    const std::vector<uint8_t> imports = MakeRel(5, 10);
    const std::vector<uint8_t> addr24 = MakeRel(0, 2);

    const auto error = [&guest](const std::vector<uint8_t>& module)
    {
        return ErrorOf<std::runtime_error>([&] { ntdlib::testing::LoadRel(guest, module, 0x80400000); });
    };
    Check(error(short_rel) == "the module ends early", "a module that ends early");
    Check(error(imports) == "the module imports another module", "a module that imports another module");
    Check(error(addr24) == "the module has a relocation of type 2", "R_PPC_ADDR24");
}

} // namespace

int main()
{
    const auto tests = []
    {
        const xsflib::testing::TempFolder folder("ntdlib_guest_test");
        const std::filesystem::path dol = WriteFile(folder.Path() / "main.dol", MakeDol(kCode));
        const std::filesystem::path compressed = WriteFile(folder.Path() / "lz77.dol", Lz77Literals(MakeDol(kCode)));

        TestCalls(dol);
        TestCalls(compressed);
        TestHooks(dol);
        TestMemory(dol);
        TestErrors(folder.Path());
        TestRel(dol);
        TestRelErrors(dol);
    };

    return xsflib::testing::RunTest("guest", tests);
}
