// SPDX-License-Identifier: MIT

// Tests of the ARM7's side of a DS (ntdlib/ntr/arm7.h), on cartridge images with small ARM7 programs. The expected
// values follow GBATEK's descriptions of the hardware and the BIOS, worked out by hand. The handshakes at start-up
// follow the routines of a DSi-enhanced cartridge's ARM7 binary.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/testing/nds_files.h"

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
using ntdlib::ntr::Arm7System;
using ntdlib::ntr::Rom;
using ntdlib::ntr::SoundOutput;

constexpr uint32_t kResults = 0x02300000;
constexpr uint32_t kIrqCount = 0x02300060;

// The program, at 0x02380000. It sets its IRQ handler and starts timer 0 with a reload of 0xFF9C and a prescaler of 64,
// with its IRQ. It waits for the timer's IRQ in IntrWait, then writes SOUNDCNT. It calls the BIOS's Div, Sqrt,
// GetPitchTable, GetVolumeTable, GetCRC16 and CpuSet, and stores their results from 0x02300000. Then it waits for a
// word from the ARM9 in the IPC's FIFO, and sends it back plus 1. The IRQ handler counts the IRQs at 0x02300060.
const std::vector<uint32_t> kProgram = {
    0xE59F010C, // 0x000 ldr r0, =irq_handler
    0xE59F110C, // 0x004 ldr r1, =0x0380FFFC
    0xE5810000, // 0x008 str r0, [r1]
    0xE3A04301, // 0x00C mov r4, #0x04000000
    0xE59F0104, // 0x010 ldr r0, =0x00C1FF9C
    0xE5840100, // 0x014 str r0, [r4, #0x100]: TM0CNT
    0xE3A00008, // 0x018 mov r0, #8
    0xE5840210, // 0x01C str r0, [r4, #0x210]: IE, timer 0
    0xE10F0000, // 0x020 mrs r0, cpsr
    0xE3C00080, // 0x024 bic r0, r0, #0x80
    0xE121F000, // 0x028 msr cpsr_c, r0
    0xE3A00001, // 0x02C mov r0, #1
    0xE3A01008, // 0x030 mov r1, #8
    0xEF040000, // 0x034 swi 0x040000: IntrWait
    0xE59F00E0, // 0x038 ldr r0, =0x807F
    0xE59F10E0, // 0x03C ldr r1, =0x04000500
    0xE1C100B0, // 0x040 strh r0, [r1]: SOUNDCNT
    0xE3A05623, // 0x044 mov r5, #0x02300000
    0xE3E00006, // 0x048 mvn r0, #6
    0xE3A01002, // 0x04C mov r1, #2
    0xEF090000, // 0x050 swi 0x090000: Div
    0xE8A5000B, // 0x054 stmia r5!, {r0, r1, r3}
    0xE3A00FFA, // 0x058 mov r0, #1000
    0xEF0D0000, // 0x05C swi 0x0D0000: Sqrt
    0xE4850004, // 0x060 str r0, [r5], #4
    0xE59F00BC, // 0x064 ldr r0, =767
    0xEF1B0000, // 0x068 swi 0x1B0000: GetPitchTable
    0xE4850004, // 0x06C str r0, [r5], #4
    0xE59F00B4, // 0x070 ldr r0, =723
    0xEF1C0000, // 0x074 swi 0x1C0000: GetVolumeTable
    0xE4850004, // 0x078 str r0, [r5], #4
    0xE59F00AC, // 0x07C ldr r0, =0xFFFF
    0xE59F10AC, // 0x080 ldr r1, =text
    0xE3A02008, // 0x084 mov r2, #8
    0xEF0E0000, // 0x088 swi 0x0E0000: GetCRC16
    0xE8A50009, // 0x08C stmia r5!, {r0, r3}
    0xE59F00A0, // 0x090 ldr r0, =fill
    0xE59F10A0, // 0x094 ldr r1, =0x02300040
    0xE59F20A0, // 0x098 ldr r2, =0x05000004
    0xEF0B0000, // 0x09C swi 0x0B0000: CpuSet
    0xE59F009C, // 0x0A0 ldr r0, =0x8404
    0xE2841F61, // 0x0A4 add r1, r4, #0x184
    0xE1C100B0, // 0x0A8 strh r0, [r1]: IPCFIFOCNT
    0xE3A00701, // 0x0AC mov r0, #0x40000
    0xE5840210, // 0x0B0 str r0, [r4, #0x210]: IE, the IPC's receive FIFO
    0xE3A00001, // 0x0B4 mov r0, #1
    0xE3A01701, // 0x0B8 mov r1, #0x40000
    0xEF040000, // 0x0BC swi 0x040000: IntrWait
    0xE3A01641, // 0x0C0 mov r1, #0x04100000
    0xE5910000, // 0x0C4 ldr r0, [r1]: IPCFIFORECV
    0xE2800001, // 0x0C8 add r0, r0, #1
    0xE5840188, // 0x0CC str r0, [r4, #0x188]: IPCFIFOSEND
    0xEF060000, // 0x0D0 idle: swi 0x060000: Halt
    0xEAFFFFFD, // 0x0D4 b idle
    0xE3A00301, // 0x0D8 irq_handler: mov r0, #0x04000000
    0xE5901214, // 0x0DC ldr r1, [r0, #0x214]: IF
    0xE5801214, // 0x0E0 str r1, [r0, #0x214]
    0xE59F205C, // 0x0E4 ldr r2, =0x0380FFF8
    0xE5923000, // 0x0E8 ldr r3, [r2]
    0xE1833001, // 0x0EC orr r3, r3, r1
    0xE5823000, // 0x0F0 str r3, [r2]: the BIOS's flags for IntrWait
    0xE59F2050, // 0x0F4 ldr r2, =0x02300060
    0xE5923000, // 0x0F8 ldr r3, [r2]
    0xE2833001, // 0x0FC add r3, r3, #1
    0xE5823000, // 0x100 str r3, [r2]
    0xE12FFF1E, // 0x104 bx lr
    0x34333231, // 0x108 text: "12345678"
    0x38373635, // 0x10C
    0xA5A5A5A5, // 0x110 fill
    0x023800D8, // 0x114 the literals
    0x0380FFFC, // 0x118
    0x00C1FF9C, // 0x11C
    0x0000807F, // 0x120
    0x04000500, // 0x124
    0x000002FF, // 0x128
    0x000002D3, // 0x12C
    0x0000FFFF, // 0x130
    0x02380108, // 0x134
    0x02380110, // 0x138
    0x02300040, // 0x13C
    0x05000004, // 0x140
    0x00008404, // 0x144
    0x0380FFF8, // 0x148
    0x02300060, // 0x14C
};

std::vector<uint8_t> Image(const std::vector<uint32_t>& program)
{
    nds_files::Image spec;
    spec.arm7.clear();
    for (const uint32_t word : program)
    {
        for (int byte = 0; byte < 4; byte++)
        {
            spec.arm7.push_back(static_cast<uint8_t>(word >> (8 * byte)));
        }
    }
    return nds_files::BuildImage(spec).first;
}

uint32_t Word(Arm7System& system, uint32_t address)
{
    const auto memory = system.MainMemory();
    // Main memory repeats every 4 MiB.
    const uint32_t at = (address - 0x02000000) & 0x3FFFFF;
    return static_cast<uint32_t>(memory[at] | memory[at + 1] << 8 | memory[at + 2] << 16 | memory[at + 3] << 24);
}

void TestBoot()
{
    const std::vector<uint8_t> image = Image(kProgram);
    const Rom rom = Rom::Parse(image);
    Arm7System system(rom);

    // The binary at its address, the header's copy, and the ARM9's side of the start-up.
    XSFLIB_CHECK(Word(system, 0x02380000) == kProgram[0]);
    const auto memory = system.MainMemory();
    XSFLIB_CHECK(std::string(memory.begin() + 0x3FFE0C, memory.begin() + 0x3FFE10) == "ATST");
    XSFLIB_CHECK(Word(system, 0x027FFFF4) == 0x7F);
    XSFLIB_CHECK(Word(system, 0x027FFF88) == 0xFFFFFFFF);
    XSFLIB_CHECK(system.Cpu().Pc() == 0x02380000);
    XSFLIB_CHECK(system.Sound().Read(0x04000504, 2) == 0x200);
}

void TestProgram()
{
    const std::vector<uint8_t> image = Image(kProgram);
    const Rom rom = Rom::Parse(image);
    Arm7System system(rom);
    std::vector<uint64_t> sound_clocks;
    int unknown = 0;
    system.on_sound_access = [&](uint32_t address, int, uint32_t value, bool write)
    {
        XSFLIB_CHECK(address == 0x04000500 && value == 0x807F && write);
        sound_clocks.push_back(system.Clock());
    };
    system.on_unknown_io = [&unknown](uint32_t, int, bool)
    {
        unknown++;
    };
    std::vector<SoundOutput> out;

    // The timer counts 100 times at 64 cycles each: it overflows at 6,400 cycles. The CPU is infinitely fast. The write
    // after the wait therefore comes at the same clock.
    system.Run(6400, out);
    XSFLIB_CHECK(sound_clocks.empty());
    XSFLIB_CHECK(system.Halted());
    system.Run(6401, out);
    XSFLIB_CHECK(sound_clocks == std::vector<uint64_t>{6400});
    XSFLIB_CHECK(system.Sound().Read(0x04000500, 2) == 0x807F);
    XSFLIB_CHECK(Word(system, kIrqCount) == 1);

    // Div(-7, 2): -3, remainder -1, and |-3|. Sqrt(1000): 31. The pitch table's last entry: round(65536 *
    // (2^(767/768) - 1)) = 65418. The volume table's last entry: 127. The CRC-16 of "12345678" from 0xFFFF:
    // 0x37DD, with the last halfword, "78". CpuSet fills 4 words.
    const std::vector<uint32_t> expected = {0xFFFFFFFD, 0xFFFFFFFF, 3, 31, 65418, 127, 0x37DD, 0x3837};
    for (uint32_t i = 0; i < expected.size(); i++)
    {
        XSFLIB_CHECK(Word(system, kResults + 4 * i) == expected[i]);
    }
    for (uint32_t i = 0; i < 4; i++)
    {
        XSFLIB_CHECK(Word(system, kResults + 0x40 + 4 * i) == 0xA5A5A5A5);
    }
    XSFLIB_CHECK(Word(system, kResults + 0x50) == 0);

    // The timer's later IRQs aren't enabled. A word from the ARM9 wakes the program. The program sends the word back
    // plus 1.
    system.Run(64001, out);
    XSFLIB_CHECK(Word(system, kIrqCount) == 1);
    XSFLIB_CHECK(system.TakeFromArm7().empty());
    system.SendToArm7(41);
    system.Run(70000, out);
    XSFLIB_CHECK(system.TakeFromArm7() == std::vector<uint32_t>{42});
    XSFLIB_CHECK(Word(system, kIrqCount) == 2);
    XSFLIB_CHECK(system.Halted());
    XSFLIB_CHECK(unknown == 0);

    // The sound unit makes a sample each 1,024 cycles: 68 by 70,000 cycles.
    XSFLIB_CHECK(out.size() == 68);
}

void TestStartHandshakes()
{
    const std::vector<uint8_t> image = Image(kProgram);
    const Rom rom = Rom::Parse(image);
    Arm7System system(rom);

    // The counters: each change of the ARM7's counter changes the ARM9's. A write of the same value isn't a change.
    system.Write(0x02FFFC26, 2, 1);
    XSFLIB_CHECK(system.Read(0x02FFFC24, 2) == 1);
    system.Write(0x02FFFC26, 2, 1);
    XSFLIB_CHECK(system.Read(0x02FFFC24, 2) == 1);
    system.Write(0x02FFFC27, 1, 1);
    XSFLIB_CHECK(system.Read(0x02FFFC24, 2) == 2);
    system.Write(0x02FFFC28, 2, 5);
    XSFLIB_CHECK(system.Read(0x02FFFC24, 2) == 2);

    // The bytes, with the ARM7 as the side that copies: 0x80 to 0x85 add up to 783.
    system.Write(0x02FFFFF1, 1, 0);
    std::vector<uint32_t> copied;
    uint32_t sum = 0;
    while (sum < 768)
    {
        const uint32_t value = system.Read(0x02FFFFF0, 1);
        system.Write(0x02FFFFF1, 1, value);
        copied.push_back(value);
        sum += value;
    }
    XSFLIB_CHECK(copied == (std::vector<uint32_t>{0x80, 0x81, 0x82, 0x83, 0x84, 0x85}));
    system.Write(0x02FFFFF3, 1, 0);
    XSFLIB_CHECK(system.Read(0x02FFFFF3, 1) == 0);
    system.Write(0x02FFFFF2, 1, 1);
    XSFLIB_CHECK(system.Read(0x02FFFFF3, 1) == 1);

    // Once byte 2 is set, the ARM9 writes no other count.
    system.Write(0x02FFFFF1, 1, system.Read(0x02FFFFF0, 1));
    XSFLIB_CHECK(system.Read(0x02FFFFF0, 1) == 0x86);

    // Then with the ARM7 as the side that writes. The ARM9 copies each value, and sets byte 2 once they add up to 768.
    // Then it copies no more. Clearing byte 2 isn't the end of a sum.
    system.Write(0x02FFFFF3, 1, 0);
    system.Write(0x02FFFFF2, 1, 0);
    XSFLIB_CHECK(system.Read(0x02FFFFF3, 1) == 0);
    const std::vector<uint32_t> values = {0x80, 0x81, 0x7F, 0x80, 0x81, 0x7F};
    for (const uint32_t value : values)
    {
        XSFLIB_CHECK(system.Read(0x02FFFFF2, 1) == 0);
        system.Write(0x02FFFFF0, 1, value);
        XSFLIB_CHECK(system.Read(0x02FFFFF1, 1) == value);
    }
    XSFLIB_CHECK(system.Read(0x02FFFFF2, 1) == 1 && system.Read(0x02FFFFF3, 1) == 0);
    system.Write(0x02FFFFF0, 1, 0x82);
    XSFLIB_CHECK(system.Read(0x02FFFFF1, 1) == 0x7F);

    // The lock word in the same place takes words. The ARM9 doesn't answer them.
    system.Write(0x027FFFF0, 4, 0);
    system.Write(0x027FFFF0, 4, 0x80);
    XSFLIB_CHECK(system.Read(0x027FFFF0, 4) == 0x80);
}

// Runs a program that has to stop the system, and returns the message.
std::string Failure(const std::vector<uint32_t>& program)
{
    const std::vector<uint8_t> image = Image(program);
    const Rom rom = Rom::Parse(image);
    Arm7System system(rom);
    std::vector<SoundOutput> out;
    try
    {
        system.Run(1000, out);
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }
    return {};
}

void TestFailures()
{
    // An undefined instruction, and Sleep. The BIOS doesn't model Sleep.
    XSFLIB_CHECK(Failure({0xE7F000F0}) == "an undefined instruction before 0x02380004");
    XSFLIB_CHECK(Failure({0xEF070000}) == "SWI 0x00000007, which the BIOS doesn't model, at 0x02380004");
}

} // namespace

int main()
{
    TestBoot();
    TestProgram();
    TestStartHandshakes();
    TestFailures();

    return xsflib::testing::Summary("DS ARM7 system");
}
