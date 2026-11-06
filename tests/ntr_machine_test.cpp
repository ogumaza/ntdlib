// SPDX-License-Identifier: MIT

// Tests of the DS with both CPUs (ntdlib/ntr/machine.h), on cartridge images with small programs for both CPUs. The
// expected values follow GBATEK's descriptions of the hardware, the backup memories and the firmware, worked out by
// hand. The sound of a program that the ARM9 drives through the IPC is checked against Arm7System on its own, with the
// ARM9's words given by the host at the same clocks.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/rom_writer.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/testing/nds_files.h"

namespace
{

namespace nds_files = ntdlib::testing::nds_files;
using ntdlib::ntr::Arm7System;
using ntdlib::ntr::BackupMemory;
using ntdlib::ntr::Machine;
using ntdlib::ntr::Rom;
using ntdlib::ntr::SoundOutput;

constexpr uint32_t kData = 0x02200000;
constexpr uint32_t kReplies = 0x02200200;
constexpr uint32_t kArm7Answers = 0x02300000;

// The ARM9's program, at 0x02000000. It puts the data TCM at 0x027C0000 and its stacks in it, and sets its IRQ handler
// at the TCM's end. It reads 0x200 bytes of the card at the ARM7 binary's offset into 0x02200000, with the plain read
// command, and polls ROMCTRL for each word. Then it turns on the vertical blank's IRQ and the IPC's FIFO. For each
// note, it waits for two vertical blanks, sends the note's timer to the ARM7 and stores the ARM7's reply from
// 0x02200200. After the notes, it counts the vertical blanks at 0x0220020C.
const std::vector<uint32_t> kArm9Program = {
    0xE59F0144, // 0x000 ldr r0, l_dtcm: the data TCM at 0x027C0000, 16 KiB
    0xEE090F11, // 0x004 mcr p15, 0, r0, c9, c1, 0
    0xEE110F10, // 0x008 mrc p15, 0, r0, c1, c0, 0
    0xE3800801, // 0x00C orr r0, r0, #0x10000
    0xEE010F10, // 0x010 mcr p15, 0, r0, c1, c0, 0
    0xE321F0D2, // 0x014 msr cpsr_c, #0xD2
    0xE59FD130, // 0x018 ldr sp, l_irq_stack: the IRQ mode's stack
    0xE321F0D3, // 0x01C msr cpsr_c, #0xD3
    0xE59FD12C, // 0x020 ldr sp, l_svc_stack: the supervisor mode's stack
    0xE321F0DF, // 0x024 msr cpsr_c, #0xDF
    0xE59FD128, // 0x028 ldr sp, l_sys_stack: the system mode's stack
    0xE28F00E8, // 0x02C adr r0, irq_handler
    0xE59F1124, // 0x030 ldr r1, l_handler
    0xE5810000, // 0x034 str r0, [r1]
    0xE3A04301, // 0x038 mov r4, #0x04000000
    0xE3A00080, // 0x03C mov r0, #0x80
    0xE5C401A1, // 0x040 strb r0, [r4, #0x1A1]: AUXSPICNT: the slot on, in ROM mode
    0xE59F1114, // 0x044 ldr r1, l_arm7_offset
    0xE5911000, // 0x048 ldr r1, [r1]: the ARM7 binary's offset in the image, from the header
    0xE3A000B7, // 0x04C mov r0, #0xB7
    0xE5C401A8, // 0x050 strb r0, [r4, #0x1A8]: the command: B7, then the offset
    0xE1A00C21, // 0x054 mov r0, r1, lsr #24
    0xE5C401A9, // 0x058 strb r0, [r4, #0x1A9]
    0xE1A00821, // 0x05C mov r0, r1, lsr #16
    0xE5C401AA, // 0x060 strb r0, [r4, #0x1AA]
    0xE1A00421, // 0x064 mov r0, r1, lsr #8
    0xE5C401AB, // 0x068 strb r0, [r4, #0x1AB]
    0xE5C411AC, // 0x06C strb r1, [r4, #0x1AC]
    0xE3A00000, // 0x070 mov r0, #0
    0xE5C401AD, // 0x074 strb r0, [r4, #0x1AD]
    0xE5C401AE, // 0x078 strb r0, [r4, #0x1AE]
    0xE5C401AF, // 0x07C strb r0, [r4, #0x1AF]
    0xE3A004A1, // 0x080 mov r0, #0xA1000000
    0xE58401A4, // 0x084 str r0, [r4, #0x1A4]: ROMCTRL: a block of 0x200 bytes
    0xE59F50D4, // 0x088 ldr r5, l_data
    0xE59F60D4, // 0x08C ldr r6, l_card_data
    0xE59401A4, // 0x090 read: ldr r0, [r4, #0x1A4]
    0xE3100502, // 0x094 tst r0, #0x800000
    0x15961000, // 0x098 ldrne r1, [r6]: the card's data, once ready
    0x14851004, // 0x09C strne r1, [r5], #4
    0xE3100102, // 0x0A0 tst r0, #0x80000000
    0x1AFFFFF9, // 0x0A4 bne read
    0xE3A00008, // 0x0A8 mov r0, #8
    0xE1C400B4, // 0x0AC strh r0, [r4, #4]: DISPSTAT: the vertical blank's IRQ
    0xE3A00001, // 0x0B0 mov r0, #1
    0xE5840210, // 0x0B4 str r0, [r4, #0x210]: IE: the vertical blank
    0xE5840208, // 0x0B8 str r0, [r4, #0x208]: IME
    0xE284AF61, // 0x0BC add r10, r4, #0x184
    0xE59F00A4, // 0x0C0 ldr r0, l_fifo_control
    0xE1CA00B0, // 0x0C4 strh r0, [r10]: IPCFIFOCNT: on, the send FIFO cleared
    0xE321F05F, // 0x0C8 msr cpsr_c, #0x5F
    0xE28F7068, // 0x0CC adr r7, notes
    0xE59F8098, // 0x0D0 ldr r8, l_replies
    0xE4979004, // 0x0D4 note: ldr r9, [r7], #4
    0xE3590000, // 0x0D8 cmp r9, #0
    0x0A000009, // 0x0DC beq idle
    0xEF050000, // 0x0E0 swi 0x050000: VBlankIntrWait, twice
    0xEF050000, // 0x0E4 swi 0x050000
    0xE5849188, // 0x0E8 str r9, [r4, #0x188]: IPCFIFOSEND: the note's timer
    0xE1DA00B0, // 0x0EC reply: ldrh r0, [r10]
    0xE3100C01, // 0x0F0 tst r0, #0x100
    0x1AFFFFFC, // 0x0F4 bne reply: wait while the receive FIFO is empty
    0xE3A02641, // 0x0F8 mov r2, #0x04100000
    0xE5920000, // 0x0FC ldr r0, [r2]: IPCFIFORECV
    0xE4880004, // 0x100 str r0, [r8], #4
    0xEAFFFFF2, // 0x104 b note
    0xEF050000, // 0x108 idle: swi 0x050000
    0xE5980000, // 0x10C ldr r0, [r8]
    0xE2800001, // 0x110 add r0, r0, #1
    0xE5880000, // 0x114 str r0, [r8]: count the vertical blanks
    0xEAFFFFFA, // 0x118 b idle
    0xE3A00301, // 0x11C irq_handler: mov r0, #0x04000000
    0xE5901214, // 0x120 ldr r1, [r0, #0x214]
    0xE5801214, // 0x124 str r1, [r0, #0x214]: IF: acknowledged
    0xE59F2044, // 0x128 ldr r2, l_flags
    0xE5923000, // 0x12C ldr r3, [r2]
    0xE1833001, // 0x130 orr r3, r3, r1
    0xE5823000, // 0x134 str r3, [r2]: the BIOS's flags
    0xE12FFF1E, // 0x138 bx lr
    0x0000F000, // 0x13C notes
    0x0000F400, // 0x140
    0x0000F800, // 0x144
    0x00000000, // 0x148
    0x027C000A, // 0x14C l_dtcm
    0x027C3F00, // 0x150 l_irq_stack
    0x027C3E00, // 0x154 l_svc_stack
    0x027C3D00, // 0x158 l_sys_stack
    0x027C3FFC, // 0x15C l_handler
    0x027FFE30, // 0x160 l_arm7_offset
    0x02200000, // 0x164 l_data
    0x04100010, // 0x168 l_card_data
    0x00008008, // 0x16C l_fifo_control
    0x02200200, // 0x170 l_replies
    0x027C3FF8, // 0x174 l_flags
};

// The ARM7's program, at 0x02380000. It sets its IRQ handler, turns on the FIFO's receive IRQ and the sound unit, and
// waits for words from the ARM9 in IntrWait. It starts each word's note on channel 8: a square wave with the word as
// its timer. Then it sends the word back plus 1.
const std::vector<uint32_t> kArm7Program = {
    0xE28F006C, // 0x00 adr r0, irq_handler
    0xE59F1088, // 0x04 ldr r1, l_handler
    0xE5810000, // 0x08 str r0, [r1]
    0xE3A04301, // 0x0C mov r4, #0x04000000
    0xE2841F61, // 0x10 add r1, r4, #0x184
    0xE59F007C, // 0x14 ldr r0, l_fifo_control
    0xE1C100B0, // 0x18 strh r0, [r1]: IPCFIFOCNT: on, the receive FIFO's IRQ, the send FIFO cleared
    0xE3A00701, // 0x1C mov r0, #0x40000
    0xE5840210, // 0x20 str r0, [r4, #0x210]: IE: the receive FIFO
    0xE3A00001, // 0x24 mov r0, #1
    0xE5840208, // 0x28 str r0, [r4, #0x208]: IME
    0xE59F0068, // 0x2C ldr r0, l_sound_control
    0xE2841C05, // 0x30 add r1, r4, #0x500
    0xE1C100B0, // 0x34 strh r0, [r1]: SOUNDCNT: on, at full volume
    0xE10F0000, // 0x38 mrs r0, cpsr
    0xE3C00080, // 0x3C bic r0, r0, #0x80
    0xE121F000, // 0x40 msr cpsr_c, r0
    0xE3A00001, // 0x44 wait: mov r0, #1
    0xE3A01701, // 0x48 mov r1, #0x40000
    0xEF040000, // 0x4C swi 0x040000: IntrWait for the receive FIFO
    0xE3A01641, // 0x50 mov r1, #0x04100000
    0xE5910000, // 0x54 ldr r0, [r1]: IPCFIFORECV: the note's timer
    0xE59F1040, // 0x58 ldr r1, l_timer
    0xE1C100B0, // 0x5C strh r0, [r1]: SOUND8TMR
    0xE59F203C, // 0x60 ldr r2, l_channel_control
    0xE5012008, // 0x64 str r2, [r1, #-8]: SOUND8CNT: a square wave of duty 4/8, at the centre, at full volume
    0xE2800001, // 0x68 add r0, r0, #1
    0xE5840188, // 0x6C str r0, [r4, #0x188]: IPCFIFOSEND: the timer plus 1
    0xEAFFFFF3, // 0x70 b wait
    0xE3A00301, // 0x74 irq_handler: mov r0, #0x04000000
    0xE5901214, // 0x78 ldr r1, [r0, #0x214]
    0xE5801214, // 0x7C str r1, [r0, #0x214]: IF: acknowledged
    0xE59F2020, // 0x80 ldr r2, l_flags
    0xE5923000, // 0x84 ldr r3, [r2]
    0xE1833001, // 0x88 orr r3, r3, r1
    0xE5823000, // 0x8C str r3, [r2]: the BIOS's flags
    0xE12FFF1E, // 0x90 bx lr
    0x0380FFFC, // 0x94 l_handler
    0x00008408, // 0x98 l_fifo_control
    0x0000807F, // 0x9C l_sound_control
    0x04000488, // 0xA0 l_timer
    0xE340007F, // 0xA4 l_channel_control
    0x0380FFF8, // 0xA8 l_flags
};

// A script's halfwords: a byte to send in bits 0 to 7, kLast for a command's last byte, kStore to store the answer,
// and the ARM7 bus's device in bits 10 and 11. kEnd ends the script.
constexpr uint16_t kLast = 0x100;
constexpr uint16_t kStore = 0x200;
constexpr uint16_t kEnd = 0xFFFF;

// The ARM9's runner of a script for the backup memory, at 0x02000000, with the script after it. It sends each byte
// through AUXSPIDATA and stores the answers that the script asks for from 0x02200000. Then it stops at 0x02000050.
const std::vector<uint32_t> kArm9Runner = {
    0xE3A04301, // 0x00 mov r4, #0x04000000
    0xE2844E1A, // 0x04 add r4, r4, #0x1A0: AUXSPICNT's address
    0xE28F5054, // 0x08 adr r5, script
    0xE59F6040, // 0x0C ldr r6, l_answers
    0xE0D500B2, // 0x10 next: ldrh r0, [r5], #2
    0xE59F103C, // 0x14 ldr r1, l_end
    0xE1500001, // 0x18 cmp r0, r1
    0x0A00000B, // 0x1C beq stop
    0xE3100C01, // 0x20 tst r0, #0x100
    0x059F1030, // 0x24 ldreq r1, l_hold: AUXSPICNT: the slot on, in SPI mode, with the chip select held
    0x159F1030, // 0x28 ldrne r1, l_last: the same, released after the byte
    0xE1C410B0, // 0x2C strh r1, [r4]
    0xE5C40002, // 0x30 strb r0, [r4, #2]: AUXSPIDATA: the byte
    0xE1D410B0, // 0x34 busy: ldrh r1, [r4]
    0xE3110080, // 0x38 tst r1, #0x80
    0x1AFFFFFC, // 0x3C bne busy
    0xE3100C02, // 0x40 tst r0, #0x200
    0x15D41002, // 0x44 ldrbne r1, [r4, #2]: the answer, when the script asks for it
    0x14C61001, // 0x48 strbne r1, [r6], #1
    0xEAFFFFEF, // 0x4C b next
    0xEAFFFFFE, // 0x50 stop: b stop
    0x02200000, // 0x54 l_answers
    0x0000FFFF, // 0x58 l_end
    0x0000A040, // 0x5C l_hold
    0x0000A000, // 0x60 l_last
};
constexpr uint32_t kArm9RunnerStop = 0x02000050;

// The ARM7's runner of a script for its SPI bus, at 0x02380000, with the script after it. It sends each byte through
// SPIDATA to the script's device and stores the answers that the script asks for from 0x02300000. Then it halts.
const std::vector<uint32_t> kArm7Runner = {
    0xE3A04301, // 0x00 mov r4, #0x04000000
    0xE2844D07, // 0x04 add r4, r4, #0x1C0: SPICNT's address
    0xE28F505C, // 0x08 adr r5, script
    0xE59F6050, // 0x0C ldr r6, l_answers
    0xE0D500B2, // 0x10 next: ldrh r0, [r5], #2
    0xE59F104C, // 0x14 ldr r1, l_end
    0xE1500001, // 0x18 cmp r0, r1
    0x0A00000E, // 0x1C beq stop
    0xE2001B03, // 0x20 and r1, r0, #0xC00
    0xE1A01121, // 0x24 mov r1, r1, lsr #2: the device, in bits 8 and 9
    0xE3811902, // 0x28 orr r1, r1, #0x8000
    0xE3100C01, // 0x2C tst r0, #0x100
    0x03811B02, // 0x30 orreq r1, r1, #0x800: SPICNT: on, with the chip select held but for the last byte
    0xE1C410B0, // 0x34 strh r1, [r4]
    0xE20020FF, // 0x38 and r2, r0, #0xFF
    0xE1C420B2, // 0x3C strh r2, [r4, #2]: SPIDATA: the byte
    0xE1D410B0, // 0x40 busy: ldrh r1, [r4]
    0xE3110080, // 0x44 tst r1, #0x80
    0x1AFFFFFC, // 0x48 bne busy
    0xE3100C02, // 0x4C tst r0, #0x200
    0x11D410B2, // 0x50 ldrhne r1, [r4, #2]: the answer, when the script asks for it
    0x14C61001, // 0x54 strbne r1, [r6], #1
    0xEAFFFFEC, // 0x58 b next
    0xEF060000, // 0x5C stop: swi 0x060000: Halt
    0xEAFFFFFD, // 0x60 b stop
    0x02300000, // 0x64 l_answers
    0x0000FFFF, // 0x68 l_end
};

// An ARM9 that halts for good.
const std::vector<uint32_t> kArm9Halts = {
    0xEF060000, // 0x0 halt: swi 0x060000: Halt
    0xEAFFFFFD, // 0x4 b halt
};

std::vector<uint8_t> Bytes(const std::vector<uint32_t>& words, std::span<const uint16_t> script = {})
{
    std::vector<uint8_t> bytes;
    for (const uint32_t word : words)
    {
        for (int byte = 0; byte < 4; byte++)
        {
            bytes.push_back(static_cast<uint8_t>(word >> (8 * byte)));
        }
    }
    for (const uint16_t half : script)
    {
        bytes.push_back(static_cast<uint8_t>(half));
        bytes.push_back(static_cast<uint8_t>(half >> 8));
    }
    return bytes;
}

std::vector<uint8_t> Image(std::vector<uint8_t> arm9, std::vector<uint8_t> arm7)
{
    nds_files::Image spec;
    spec.arm9 = std::move(arm9);
    spec.arm7 = std::move(arm7);
    return nds_files::BuildImage(spec).first;
}

// Adds a command to `script`: `bytes` to device `device`, storing the answers to the last `answers` of them.
void Add(std::vector<uint16_t>& script, std::initializer_list<uint8_t> bytes, std::size_t answers, int device = 0)
{
    std::size_t i = 0;
    for (const uint8_t byte : bytes)
    {
        uint16_t half = static_cast<uint16_t>(byte | device << 10);
        if (i + 1 == bytes.size())
        {
            half |= kLast;
        }
        if (i + answers >= bytes.size())
        {
            half |= kStore;
        }
        script.push_back(half);
        i++;
    }
}

uint32_t Word(Machine& machine, uint32_t address)
{
    const auto memory = machine.MainMemory();
    const uint32_t at = (address - 0x02000000) & static_cast<uint32_t>(memory.size() - 1);
    return static_cast<uint32_t>(memory[at] | memory[at + 1] << 8 | memory[at + 2] << 16 | memory[at + 3] << 24);
}

std::vector<uint8_t> MemoryBytes(Machine& machine, uint32_t address, std::size_t size)
{
    const auto memory = machine.MainMemory();
    const std::size_t at = (address - 0x02000000) & (memory.size() - 1);
    return {memory.begin() + static_cast<std::ptrdiff_t>(at), memory.begin() + static_cast<std::ptrdiff_t>(at + size)};
}

void TestProgram()
{
    // The ARM9 binary fills 0x8000 bytes. The ARM7 binary then lies past the card's secure area.
    std::vector<uint8_t> arm9 = Bytes(kArm9Program);
    arm9.resize(0x8000, 0);
    const std::vector<uint8_t> image = Image(arm9, Bytes(kArm7Program));
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    std::vector<std::pair<uint64_t, uint32_t>> sent;
    std::vector<uint32_t> received;
    machine.on_fifo_word = [&](bool to_arm7, uint32_t word)
    {
        if (to_arm7)
        {
            sent.emplace_back(machine.Clock(), word);
        }
        else
        {
            received.push_back(word);
        }
    };
    int unknown = 0;
    machine.on_unknown_io = [&unknown](uint32_t, int, bool)
    {
        unknown++;
    };
    machine.Arm7().on_unknown_io = [&unknown](uint32_t, int, bool)
    {
        unknown++;
    };

    // The vertical blank starts at line 192 of each frame of 263 lines of 2,130 cycles: at 408,960 cycles, then every
    // 560,190. The run ends just after the twelfth.
    constexpr uint64_t kFirstBlank = 192 * 2130;
    constexpr uint64_t kFrame = 263 * 2130;
    constexpr uint64_t kRunEnd = kFirstBlank + 11 * kFrame + 1;
    std::vector<SoundOutput> out;
    XSFLIB_CHECK(!machine.Run(kRunEnd, out));
    XSFLIB_CHECK(machine.Clock() == kRunEnd);

    // The card's 0x200 bytes at the ARM7 binary's offset.
    const uint32_t offset = rom.Header().arm7.rom_offset;
    XSFLIB_CHECK(offset >= 0x8000);
    std::vector<uint8_t> expected(0x200, 0xFF);
    for (std::size_t i = 0; i < expected.size() && offset + i < image.size(); i++)
    {
        expected[i] = image[offset + i];
    }
    XSFLIB_CHECK(MemoryBytes(machine, kData, 0x200) == expected);
    XSFLIB_CHECK(Word(machine, kData) == kArm7Program[0]);
    XSFLIB_CHECK(machine.Counts().card_reads == 1);
    XSFLIB_CHECK(machine.Counts().card_bytes == 0x200);

    // A note at every second vertical blank, from the second. The ARM7 replies at once, in no time.
    const std::vector<std::pair<uint64_t, uint32_t>> notes = {
        {kFirstBlank + kFrame, 0xF000}, {kFirstBlank + 3 * kFrame, 0xF400}, {kFirstBlank + 5 * kFrame, 0xF800}};
    XSFLIB_CHECK(sent == notes);
    XSFLIB_CHECK(received == (std::vector<uint32_t>{0xF001, 0xF401, 0xF801}));
    XSFLIB_CHECK(Word(machine, kReplies) == 0xF001);
    XSFLIB_CHECK(Word(machine, kReplies + 4) == 0xF401);
    XSFLIB_CHECK(Word(machine, kReplies + 8) == 0xF801);

    // Twelve vertical blanks, each acknowledged. The program counts the six after the last note.
    XSFLIB_CHECK(machine.Counts().vblank_acks == 12);
    XSFLIB_CHECK(Word(machine, kReplies + 12) == 6);
    XSFLIB_CHECK(unknown == 0);

    // Arm7System on its own, with the ARM9's words at the same clocks, plays the same samples. The sound unit makes a
    // sample each 1,024 cycles. The first note starts with the sample after its clock.
    Arm7System alone(rom);
    std::vector<SoundOutput> alone_out;
    for (const auto& [clock, word] : sent)
    {
        alone.Run(clock, alone_out);
        alone.SendToArm7(word);
    }
    alone.Run(kRunEnd, alone_out);
    XSFLIB_CHECK(alone.TakeFromArm7() == (std::vector<uint32_t>{0xF001, 0xF401, 0xF801}));
    XSFLIB_CHECK(out.size() == kRunEnd / 1024);
    XSFLIB_CHECK(alone_out == out);
    const std::size_t first = static_cast<std::size_t>(notes[0].first / 1024);
    bool silent = true;
    for (std::size_t i = 0; i < first; i++)
    {
        silent = silent && out[i] == out[0];
    }
    XSFLIB_CHECK(silent);
    bool sounds = false;
    for (std::size_t i = first; i < out.size(); i++)
    {
        sounds = sounds || !(out[i] == out[0]);
    }
    XSFLIB_CHECK(sounds);
}

// Runs `script` on the ARM9's runner with `backup` in the card, and returns the answers that it stores.
std::vector<uint8_t> RunBackupScript(const BackupMemory& backup, std::vector<uint16_t> script,
                                     std::vector<uint8_t>* contents = nullptr)
{
    script.push_back(kEnd);
    const std::vector<uint8_t> image =
        Image(Bytes(kArm9Runner, script), Bytes(kArm7Runner, std::vector<uint16_t>{kEnd}));
    const Rom rom = Rom::Parse(image);
    Machine machine(rom, backup);
    machine.Arm9().SetStopAddress(kArm9RunnerStop);
    std::vector<SoundOutput> out;
    XSFLIB_CHECK(machine.Run(100'000, out));
    std::size_t answers = 0;
    for (const uint16_t half : script)
    {
        answers += half != kEnd && (half & kStore) != 0 ? 1 : 0;
    }
    if (contents != nullptr)
    {
        *contents = std::vector<uint8_t>(machine.Backup().begin(), machine.Backup().end());
    }
    return MemoryBytes(machine, kData, answers);
}

void TestBackupMemories()
{
    // An EEPROM with 2 address bytes. A write without the write enable command first changes nothing, and a write
    // clears the latch. The status then reads 0.
    std::vector<uint16_t> script;
    Add(script, {0x02, 0x00, 0x10, 0x77}, 0);
    Add(script, {0x06}, 0);
    Add(script, {0x05, 0x00}, 1);
    Add(script, {0x02, 0x01, 0x23, 0x5A, 0xA5}, 0);
    Add(script, {0x05, 0x00}, 1);
    Add(script, {0x03, 0x01, 0x23, 0x00, 0x00}, 2);
    Add(script, {0x03, 0x00, 0x10, 0x00}, 1);
    std::vector<uint8_t> contents;
    XSFLIB_CHECK(RunBackupScript({.address_bytes = 2, .size = 0x2000}, script, &contents) ==
                 (std::vector<uint8_t>{0x02, 0x00, 0x5A, 0xA5, 0xFF}));
    XSFLIB_CHECK(contents.size() == 0x2000 && contents[0x123] == 0x5A && contents[0x124] == 0xA5);
    XSFLIB_CHECK(contents[0x10] == 0xFF);

    // An EEPROM of 512 bytes: bit 3 of the read and write commands gives address bit 8.
    script.clear();
    Add(script, {0x06}, 0);
    Add(script, {0x0A, 0x10, 0x42}, 0);
    Add(script, {0x0B, 0x10, 0x00}, 1);
    Add(script, {0x03, 0x10, 0x00}, 1);
    XSFLIB_CHECK(RunBackupScript({.address_bytes = 1, .size = 512}, script, &contents) ==
                 (std::vector<uint8_t>{0x42, 0xFF}));
    XSFLIB_CHECK(contents[0x110] == 0x42);

    // A flash memory of 256 KiB: its ID, a page write, a page program that clears bits, a fast read with its dummy
    // byte, and a page erase.
    script.clear();
    Add(script, {0x9F, 0x00, 0x00, 0x00}, 3);
    Add(script, {0x06}, 0);
    Add(script, {0x0A, 0x00, 0x01, 0x00, 0x11, 0x22}, 0);
    Add(script, {0x06}, 0);
    Add(script, {0x02, 0x00, 0x01, 0x00, 0x0F}, 0);
    Add(script, {0x0B, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}, 2);
    Add(script, {0x06}, 0);
    Add(script, {0xDB, 0x00, 0x01, 0x80}, 0);
    Add(script, {0x03, 0x00, 0x01, 0x00, 0x00}, 1);
    XSFLIB_CHECK(RunBackupScript({.address_bytes = 3, .size = 0x40000}, script, &contents) ==
                 (std::vector<uint8_t>{0x20, 0x40, 0x12, 0x01, 0x22, 0xFF}));
    XSFLIB_CHECK(contents.size() == 0x40000 && contents[0x100] == 0xFF && contents[0x101] == 0xFF);

    // The infrared chip in front of a flash memory: its version, and the memory's status and ID through command 00h.
    // Its other commands answer zeros.
    script.clear();
    Add(script, {0x08, 0x00}, 1);
    Add(script, {0x00, 0x05, 0x00}, 1);
    Add(script, {0x00, 0x9F, 0x00, 0x00, 0x00}, 3);
    Add(script, {0x01, 0x00}, 1);
    Add(script, {0x00, 0x06}, 0);
    Add(script, {0x00, 0x05, 0x00}, 1);
    XSFLIB_CHECK(RunBackupScript({.address_bytes = 3, .size = 0x80000, .infrared = true}, script) ==
                 (std::vector<uint8_t>{0xAA, 0x00, 0x20, 0x40, 0x13, 0x00, 0x02}));

    // Without the chip, the memory takes 00h as an unknown command and answers nothing.
    script.clear();
    Add(script, {0x00, 0x05, 0x00}, 1);
    XSFLIB_CHECK(RunBackupScript({.address_bytes = 3, .size = 0x80000}, script) == std::vector<uint8_t>{0xFF});

    // A blank memory keeps nothing, and its status reads 0.
    script.clear();
    Add(script, {0x06}, 0);
    Add(script, {0x02, 0x00, 0x10, 0x77}, 0);
    Add(script, {0x05, 0x00}, 1);
    Add(script, {0x03, 0x00, 0x10, 0x00}, 1);
    XSFLIB_CHECK(RunBackupScript({}, script, &contents) == (std::vector<uint8_t>{0x00, 0xFF}));
    XSFLIB_CHECK(contents.empty());

    // Sizes that no memory of the kind has.
    const std::vector<uint8_t> image = Image(Bytes(kArm9Halts), Bytes(kArm7Runner, std::vector<uint16_t>{kEnd}));
    const Rom rom = Rom::Parse(image);
    for (const BackupMemory& backup :
         {BackupMemory{.address_bytes = 1, .size = 0x2000}, BackupMemory{.address_bytes = 2, .size = 0x3000},
          BackupMemory{.address_bytes = 2, .size = 0x20000}, BackupMemory{.address_bytes = 4, .size = 0x40000}})
    {
        bool refused = false;
        try
        {
            Machine machine(rom, backup);
        }
        catch (const std::invalid_argument&)
        {
            refused = true;
        }
        XSFLIB_CHECK(refused);
    }
}

void TestArm7Devices()
{
    // The firmware's flash memory (device 1): its ID, the header's place of the user's settings (0x3FE00 / 8), and the
    // first copy of the settings: version 5, then the update count 0 and the CRC-16 of the 0x70 bytes. The power
    // management device (0) keeps its registers. Register 0 starts with the amplifier and both backlights on. The
    // touch screen's controller (2) answers 0.
    std::vector<uint16_t> script;
    Add(script, {0x9F, 0x00, 0x00, 0x00}, 3, 1);
    Add(script, {0x03, 0x00, 0x00, 0x20, 0x00, 0x00}, 2, 1);
    Add(script, {0x03, 0x03, 0xFE, 0x00}, 0, 1);
    script.back() &= static_cast<uint16_t>(~kLast);
    for (int i = 0; i < 0x74; i++)
    {
        script.push_back(static_cast<uint16_t>(kStore | 1 << 10 | (i + 1 == 0x74 ? kLast : 0)));
    }
    Add(script, {0x80, 0x00}, 1, 0);
    Add(script, {0x04, 0x3A}, 0, 0);
    Add(script, {0x84, 0x00}, 1, 0);
    Add(script, {0xD0, 0x00, 0x00}, 2, 2);
    script.push_back(kEnd);

    const std::vector<uint8_t> image = Image(Bytes(kArm9Halts), Bytes(kArm7Runner, script));
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    std::vector<SoundOutput> out;
    machine.Run(1000, out);
    XSFLIB_CHECK(machine.Arm7().Halted());

    const std::vector<uint8_t> answers = MemoryBytes(machine, kArm7Answers, 3 + 2 + 0x74 + 1 + 1 + 2);
    XSFLIB_CHECK(std::vector<uint8_t>(answers.begin(), answers.begin() + 5) ==
                 (std::vector<uint8_t>{0x20, 0x40, 0x12, 0xC0, 0x7F}));
    const std::span<const uint8_t> settings = std::span(answers).subspan(5, 0x74);
    XSFLIB_CHECK(settings[0] == 5 && settings[1] == 0);
    XSFLIB_CHECK(settings[0x70] == 0 && settings[0x71] == 0);
    const uint16_t crc = ntdlib::ntr::Crc16(settings.first(0x70));
    XSFLIB_CHECK(settings[0x72] == (crc & 0xFF) && settings[0x73] == crc >> 8);
    XSFLIB_CHECK(std::vector<uint8_t>(answers.begin() + 5 + 0x74, answers.end()) ==
                 (std::vector<uint8_t>{0x0D, 0x3A, 0x00, 0x00}));

    // The settings in main memory, as the firmware leaves them.
    XSFLIB_CHECK(MemoryBytes(machine, 0x027FFC80, 0x70) ==
                 std::vector<uint8_t>(settings.begin(), settings.begin() + 0x70));
}

} // namespace

int main()
{
    TestProgram();
    TestBackupMemories();
    TestArm7Devices();
    return xsflib::testing::Summary("DS machine");
}
