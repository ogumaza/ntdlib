// SPDX-License-Identifier: MIT

// Tests of the GBA's system for running a sound driver (ntdlib/agb/system.h), with programs that LLVM's assembler
// built. Each instruction comes with its assembly. The expected values follow GBATEK's descriptions of the GBA's
// memory map, its DMA and its BIOS.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/system.h"

namespace
{

using ntdlib::agb::kEwramBase;
using ntdlib::agb::kIwramBase;
using ntdlib::agb::kRegSoundCntH;
using ntdlib::agb::kRegVcount;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::agb::System;

// Where the cartridge holds each program and its data.
constexpr uint32_t kCpuSet = kRomBase + 0x000;     // swi 0x0B; bx lr
constexpr uint32_t kCpuFastSet = kRomBase + 0x010; // swi 0x0C; bx lr
constexpr uint32_t kDiv = kRomBase + 0x020;        // swi 6; bx lr
constexpr uint32_t kDivArm = kRomBase + 0x030;     // swi 7; bx lr
constexpr uint32_t kVBlankWait = kRomBase + 0x040; // swi 5; bx lr
constexpr uint32_t kDma = kRomBase + 0x050;        // the registers from r3 on take r0, r1 and r2
constexpr uint32_t kLoad16 = kRomBase + 0x060;     // ldrh r0, [r0]; bx lr
constexpr uint32_t kLoad32 = kRomBase + 0x070;     // ldr r0, [r0]; bx lr
constexpr uint32_t kStore16 = kRomBase + 0x080;    // strh r1, [r0]; bx lr
constexpr uint32_t kStack = kRomBase + 0x090;      // mov r0, sp; bx lr
constexpr uint32_t kArmDiv = kRomBase + 0x0A0;     // ARM: swi 0x60000; bx lr
constexpr uint32_t kArmCpsr = kRomBase + 0x0B0;    // ARM: mrs r0, cpsr; bx lr
constexpr uint32_t kChecksum = kRomBase + 0x0C0;   // swi 0x0D; bx lr
constexpr uint32_t kData = kRomBase + 0x100;       // 16 words: 0x11111111 times 1 to 16
constexpr uint32_t kImageSize = 0x1100;            // the image ends partway through a page

constexpr uint32_t kDma0 = 0x040000B0;
constexpr uint32_t kDma1 = 0x040000BC;
constexpr uint32_t kDma3 = 0x040000D4;

void Put16(std::vector<uint8_t>& image, uint32_t address, std::initializer_list<uint16_t> halfwords)
{
    for (uint16_t h : halfwords)
    {
        image[address - kRomBase] = uint8_t(h);
        image[address - kRomBase + 1] = uint8_t(h >> 8);
        address += 2;
    }
}

void Put32(std::vector<uint8_t>& image, uint32_t address, std::initializer_list<uint32_t> words)
{
    for (uint32_t w : words)
    {
        Put16(image, address, {uint16_t(w), uint16_t(w >> 16)});
        address += 4;
    }
}

Rom TestRom()
{
    std::vector<uint8_t> image(kImageSize);
    Put16(image, kCpuSet, {0xDF0B, 0x4770});
    Put16(image, kCpuFastSet, {0xDF0C, 0x4770});
    Put16(image, kDiv, {0xDF06, 0x4770});
    Put16(image, kDivArm, {0xDF07, 0x4770});
    Put16(image, kVBlankWait, {0xDF05, 0x4770});
    Put16(image, kChecksum, {0xDF0D, 0x4770});
    // str r0, [r3]; str r1, [r3, #4]; str r2, [r3, #8]; bx lr
    Put16(image, kDma, {0x6018, 0x6059, 0x609A, 0x4770});
    Put16(image, kLoad16, {0x8800, 0x4770});
    Put16(image, kLoad32, {0x6800, 0x4770});
    Put16(image, kStore16, {0x8001, 0x4770});
    Put16(image, kStack, {0x4668, 0x4770});
    Put32(image, kArmDiv, {0xEF060000, 0xE12FFF1E});
    Put32(image, kArmCpsr, {0xE10F0000, 0xE12FFF1E});
    for (uint32_t i = 0; i < 16; i++)
    {
        Put32(image, kData + 4 * i, {0x11111111 * (i + 1)});
    }

    Rom rom;
    rom.Assign(std::move(image));
    return rom;
}

uint32_t CallThumb(System& system, uint32_t address, std::initializer_list<uint32_t> args)
{
    const std::vector<uint32_t> list(args);
    return system.Call(address | 1, list);
}

void TestMemoryMap()
{
    const Rom rom = TestRom();
    System system(rom);

    // The work memories repeat through their regions.
    system.Store(kEwramBase + 0x100, 4, 0x12345678);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x40100, 4) == 0x12345678);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x102, 2) == 0x1234);
    system.Store(kIwramBase + 0x7FF0, 2, 0xBEEF);
    XSFLIB_CHECK(system.Load(kIwramBase + 0xFFF0, 2) == 0xBEEF);
    XSFLIB_CHECK(CallThumb(system, kLoad32, {kIwramBase + 0x17FF0}) == 0xBEEF);

    // The cartridge shows in each of its three regions. Past the image's end, the last page reads as 0.
    XSFLIB_CHECK(system.Load(kData, 4) == 0x11111111);
    XSFLIB_CHECK(system.Load(kData + 0x02000004, 4) == 0x22222222);
    XSFLIB_CHECK(system.Load(kData + 0x04000008, 4) == 0x33333333);
    XSFLIB_CHECK(system.Load(kRomBase + kImageSize, 4) == 0);

    // The video memories aren't modelled: their reads give 0 and count.
    const uint64_t unmapped = system.UnmappedAccesses();
    XSFLIB_CHECK(CallThumb(system, kLoad32, {0x06000000}) == 0);
    XSFLIB_CHECK(system.UnmappedAccesses() == unmapped + 1);
    system.Store(0x05000000, 2, 0x7FFF);
    XSFLIB_CHECK(system.Load(0x05000000, 2) == 0);
    XSFLIB_CHECK(system.UnmappedAccesses() == unmapped + 3);

    // An image larger than the GBA's 32 MiB.
    Rom large;
    large.Assign(std::vector<uint8_t>(ntdlib::agb::kMaxRomSize + 1));
    XSFLIB_CHECK(!xsflib::testing::ErrorOf<std::invalid_argument>([&] { System s(large); }).empty());
}

void TestCall()
{
    const Rom rom = TestRom();
    System system(rom);

    // Each call starts in system mode, with IRQs and FIQs disabled and the system mode's stack.
    XSFLIB_CHECK(CallThumb(system, kStack, {}) == System::kStack);
    XSFLIB_CHECK((system.Call(kArmCpsr) & 0xFF) == 0xDF);

    // A BIOS call that isn't modelled stops the call.
    const std::string error = xsflib::testing::ErrorOf<std::runtime_error>([&] { CallThumb(system, kVBlankWait, {}); });
    XSFLIB_CHECK(error == "BIOS call 5 isn't modelled");

    // The system takes calls after a failure.
    XSFLIB_CHECK(CallThumb(system, kStack, {}) == System::kStack);
}

void TestDiv()
{
    const Rom rom = TestRom();
    System system(rom);

    // Div: r0 / r1, rounded toward zero. r1 takes the remainder, with the numerator's sign, and r3 the quotient's
    // magnitude.
    XSFLIB_CHECK(CallThumb(system, kDiv, {7, 2}) == 3);
    XSFLIB_CHECK(system.Cpu().Reg(1) == 1 && system.Cpu().Reg(3) == 3);
    XSFLIB_CHECK(CallThumb(system, kDiv, {uint32_t(-7), 2}) == uint32_t(-3));
    XSFLIB_CHECK(system.Cpu().Reg(1) == uint32_t(-1) && system.Cpu().Reg(3) == 3);
    XSFLIB_CHECK(CallThumb(system, kDiv, {7, uint32_t(-2)}) == uint32_t(-3));
    XSFLIB_CHECK(system.Cpu().Reg(1) == 1 && system.Cpu().Reg(3) == 3);
    XSFLIB_CHECK(CallThumb(system, kDiv, {0x80000000, 1}) == 0x80000000);
    XSFLIB_CHECK(system.Cpu().Reg(3) == 0x80000000);

    // DivArm: r1 / r0.
    XSFLIB_CHECK(CallThumb(system, kDivArm, {3, 100}) == 33);
    XSFLIB_CHECK(system.Cpu().Reg(1) == 1);

    // From ARM code, the comment's bits 16 to 23 give the call.
    XSFLIB_CHECK(system.Call(kArmDiv, std::array<uint32_t, 2>{100, 7}) == 14);
    XSFLIB_CHECK(system.Cpu().Reg(1) == 2);

    const std::string error = xsflib::testing::ErrorOf<std::runtime_error>([&] { CallThumb(system, kDiv, {1, 0}); });
    XSFLIB_CHECK(error == "the BIOS's Div was called with a denominator of 0");
}

void TestBiosChecksum()
{
    // GBATEK gives the sum of the GBA's BIOS as 0xBAAE187F.
    const Rom rom = TestRom();
    System system(rom);
    XSFLIB_CHECK(CallThumb(system, kChecksum, {}) == 0xBAAE187F);
}

void TestCpuSet()
{
    const Rom rom = TestRom();
    System system(rom);

    // A copy of 3 words.
    CallThumb(system, kCpuSet, {kData, kEwramBase, 3 | 1u << 26});
    XSFLIB_CHECK(system.Load(kEwramBase, 4) == 0x11111111);
    XSFLIB_CHECK(system.Load(kEwramBase + 8, 4) == 0x33333333);
    XSFLIB_CHECK(system.Load(kEwramBase + 12, 4) == 0);

    // A fill of 3 halfwords from the source's first halfword.
    CallThumb(system, kCpuSet, {kData + 4, kIwramBase + 0x10, 3 | 1u << 24});
    XSFLIB_CHECK(system.Load(kIwramBase + 0x10, 4) == 0x22222222);
    XSFLIB_CHECK(system.Load(kIwramBase + 0x14, 4) == 0x2222);

    // CpuFastSet copies whole blocks of 8 words: a count of 9 copies 16.
    CallThumb(system, kCpuFastSet, {kData, kEwramBase + 0x100, 9});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x13C, 4) == 0x11111111u * 16);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x140, 4) == 0);

    // A fill of 1 word fills 8.
    CallThumb(system, kCpuFastSet, {kData + 8, kEwramBase + 0x200, 1 | 1u << 24});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x21C, 4) == 0x33333333);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x220, 4) == 0);
}

void TestDma()
{
    const Rom rom = TestRom();
    System system(rom);

    // DMA 3 copies 4 words at once, and clears its enable bit.
    CallThumb(system, kDma, {kData, kEwramBase, 4 | 0x8400u << 16, kDma3});
    XSFLIB_CHECK(system.Load(kEwramBase + 12, 4) == 0x44444444);
    XSFLIB_CHECK(system.Load(kEwramBase + 16, 4) == 0);
    XSFLIB_CHECK(system.Load(kDma3 + 10, 2) == 0x0400);

    // DMA 3 copies 3 halfwords down from the source into a fixed destination: the last write is the source's first
    // halfword less two.
    CallThumb(system, kDma, {kData + 4, kIwramBase, 3 | 0x80C0u << 16, kDma3});
    XSFLIB_CHECK(system.Load(kIwramBase, 4) == 0x1111);

    // DMA 0 reads only the internal memories. Its source loses its top bits, and the cartridge's address reads the
    // BIOS's memory. That memory holds zeros here.
    CallThumb(system, kDma, {kData, kEwramBase + 0x20, 2 | 0x8400u << 16, kDma0});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x20, 4) == 0);

    // A transfer at the sound FIFO's requests doesn't start here, and stays enabled.
    CallThumb(system, kDma, {kData, kEwramBase + 0x100, 4 | 0xB600u << 16, kDma1});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x100, 4) == 0);
    XSFLIB_CHECK(system.Load(kDma1 + 10, 2) == 0xB600);

    // A write that leaves the enable bit set starts nothing.
    CallThumb(system, kDma, {kData, kEwramBase + 0x100, 4 | 0x8400u << 16, kDma1});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x100, 4) == 0);

    // A count of 0 is 0x4000 units on DMA 0, and 0x10000 on DMA 3. These fill from a fixed source.
    CallThumb(system, kDma, {kData, kEwramBase + 0x10000, 0x8100u << 16, kDma3});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x10000 + 2 * 0xFFFF, 2) == 0x1111);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x10000 + 2 * 0x10000, 2) == 0);
    system.Store(kIwramBase + 0x4000, 2, 0x5555);
    CallThumb(system, kDma, {kIwramBase + 0x4000, kEwramBase + 0x30000, 0x8100u << 16, kDma0});
    XSFLIB_CHECK(system.Load(kEwramBase + 0x30000 + 2 * 0x3FFF, 2) == 0x5555);
    XSFLIB_CHECK(system.Load(kEwramBase + 0x30000 + 2 * 0x4000, 2) == 0);
}

void TestIo()
{
    const Rom rom = TestRom();
    System system(rom);

    // Each read of the vertical count moves it on a line, through the frame's 228 lines.
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegVcount}) == 1);
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegVcount}) == 2);
    for (int i = 0; i < ntdlib::agb::kFrameLines - 3; i++)
    {
        CallThumb(system, kLoad16, {kRegVcount});
    }
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegVcount}) == 0);

    // A held count stays.
    system.HoldVcount(ntdlib::agb::kVblankLine);
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegVcount}) == 160);
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegVcount}) == 160);
    system.HoldVcount(std::nullopt);
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegVcount}) == 161);

    // The other registers hold what the code writes, and the callback sees each write.
    std::vector<std::array<uint32_t, 3>> writes;
    system.on_io_write = [&](uint32_t address, int size, uint32_t value)
    {
        writes.push_back({address, uint32_t(size), value});
    };
    CallThumb(system, kStore16, {kRegSoundCntH, 0x9A0C});
    XSFLIB_CHECK(writes.size() == 1 && writes[0] == (std::array<uint32_t, 3>{kRegSoundCntH, 2, 0x9A0C}));
    XSFLIB_CHECK(CallThumb(system, kLoad16, {kRegSoundCntH}) == 0x9A0C);
}

} // namespace

int main()
{
    TestMemoryMap();
    TestCall();
    TestDiv();
    TestBiosChecksum();
    TestCpuSet();
    TestDma();
    TestIo();

    return xsflib::testing::Summary("GBA system");
}
