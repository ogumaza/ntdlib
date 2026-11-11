// SPDX-License-Identifier: MIT

// Definitions that the parts of the DS machine share (machine.cpp, machine_card.cpp and machine_io.cpp): the memory
// map, the BIOS, the calls and the registers. The three files build as one translation unit. The definitions therefore
// keep internal linkage. Only those files include this header.

#pragma once

#include "ntdlib/ntr/machine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "bios.h"
#include "io.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/ntr/timers.h"
#include "spi_devices.h"

namespace ntdlib::ntr
{
namespace
{

constexpr uint32_t kMainBase = 0x02000000;
constexpr uint32_t kBiosBase = 0xFFFF0000;
constexpr uint32_t kBiosSize = 0x8000;
constexpr uint32_t kSmallMemory = 0x1000; // the palettes and the OAM: 2 KiB each, kept in a page each

// VRAM's areas, by bits 21 to 23 of the address: the backgrounds and the objects of engines A and B, then the LCDC
// area. The banks that VRAMCNT maps aren't modelled: each area has memory of its own, mirrored.
constexpr std::array<uint32_t, 5> kVramSizes = {0x80000, 0x20000, 0x40000, 0x20000, 0xA4000};

// The area at main memory's end that the firmware fills before it starts a game: the card's chip ID twice, the CRCs of
// the header and the secure area that the firmware checked, and the ARM7 BIOS's CRC, each at 0x027FF800 and copied
// from 0x027FFC00. The boot indicator gives a boot from the card, and the user's settings follow.
constexpr uint32_t kBootArea = 0x027FF800;
constexpr uint32_t kBootAreaCopy = 0x027FFC00;
constexpr uint32_t kBootIndicator = 0x027FFC40;
constexpr uint32_t kUserSettings = 0x027FFC80;
constexpr uint16_t kArm7BiosCrc = 0x5835;

// The secure area's first 8 bytes in a decrypted image, and what the firmware writes in their place.
constexpr std::array<uint8_t, 8> kSecureAreaId = {'e', 'n', 'c', 'r', 'y', 'O', 'b', 'j'};
constexpr uint32_t kSecureAreaMark = 0xE7FFDEFF;
constexpr uint32_t kSecureArea = 0x4000;

// The BIOS in place of the console's: the exception vectors, the IRQ handler, and IntrWait in ARM code. The IRQ
// handler calls the program's handler at the end of the data TCM, at its base plus 0x3FFC. The SWI vector serves only
// IntrWait and VBlankIntrWait: it runs IntrWait in system mode with the caller's IRQ mask. IntrWait sets IME, clears
// the flags of r1 at the data TCM's base plus 0x3FF8 first when r0 isn't 0, then waits until the program's handler
// sets one of them, and clears them. It waits for each interrupt with the CPSR's IRQ mask set, then takes the
// interrupt with the caller's mask. The other vectors make SWIs that the system takes as faults. The system answers
// the other SWIs itself (Machine::Swi).
constexpr std::array<uint32_t, 57> kBios = {
    0xEFFE0000, // 0x00 reset: swi 0xFE0000
    0xEFFE0001, // 0x04 undefined: swi 0xFE0001
    0xEA00000D, // 0x08 swi: b swi_handler
    0xEFFE0002, // 0x0C prefetch_abort: swi 0xFE0002
    0xEFFE0003, // 0x10 data_abort: swi 0xFE0003
    0xEFFE0004, // 0x14 reserved: swi 0xFE0004
    0xEA000000, // 0x18 irq: b irq_handler
    0xEFFE0005, // 0x1C fiq: swi 0xFE0005
    0xE92D500F, // 0x20 irq_handler: stmfd sp!, {r0-r3, r12, lr}
    0xEE190F11, // 0x24 mrc p15, 0, r0, c9, c1, 0
    0xE1A00620, // 0x28 mov r0, r0, lsr #12
    0xE1A00600, // 0x2C mov r0, r0, lsl #12
    0xE2800901, // 0x30 add r0, r0, #0x4000
    0xE28FE000, // 0x34 add lr, pc, #0
    0xE510F004, // 0x38 ldr pc, [r0, #-4]
    0xE8BD500F, // 0x3C ldmfd sp!, {r0-r3, r12, lr}
    0xE25EF004, // 0x40 subs pc, lr, #4
    0xE92D5800, // 0x44 swi_handler: stmfd sp!, {r11, r12, lr}
    0xE14FB000, // 0x48 mrs r11, spsr
    0xE92D0800, // 0x4C stmfd sp!, {r11}
    0xE20BB080, // 0x50 and r11, r11, #0x80
    0xE38BB01F, // 0x54 orr r11, r11, #0x1F
    0xE129F00B, // 0x58 msr cpsr_fc, r11
    0xE92D400C, // 0x5C stmfd sp!, {r2, r3, lr}
    0xEB000006, // 0x60 bl intr_wait
    0xE8BD400C, // 0x64 ldmfd sp!, {r2, r3, lr}
    0xE3A0C0D3, // 0x68 mov r12, #0xD3
    0xE129F00C, // 0x6C msr cpsr_fc, r12
    0xE8BD0800, // 0x70 ldmfd sp!, {r11}
    0xE169F00B, // 0x74 msr spsr_fc, r11
    0xE8BD5800, // 0x78 ldmfd sp!, {r11, r12, lr}
    0xE1B0F00E, // 0x7C movs pc, lr
    0xEE193F11, // 0x80 intr_wait: mrc p15, 0, r3, c9, c1, 0
    0xE1A03623, // 0x84 mov r3, r3, lsr #12
    0xE1A03603, // 0x88 mov r3, r3, lsl #12
    0xE2833901, // 0x8C add r3, r3, #0x4000
    0xE3A0C301, // 0x90 mov r12, #0x04000000
    0xE3500000, // 0x94 cmp r0, #0
    0x0A000002, // 0x98 beq check
    0xE5132008, // 0x9C ldr r2, [r3, #-8]
    0xE1C22001, // 0xA0 bic r2, r2, r1
    0xE5032008, // 0xA4 str r2, [r3, #-8]
    0xE3A02000, // 0xA8 check: mov r2, #0
    0xE5CC2208, // 0xAC strb r2, [r12, #0x208]
    0xE5132008, // 0xB0 ldr r2, [r3, #-8]
    0xE0120001, // 0xB4 ands r0, r2, r1
    0x11C22001, // 0xB8 bicne r2, r2, r1
    0x15032008, // 0xBC strne r2, [r3, #-8]
    0xE3A02001, // 0xC0 mov r2, #1
    0xE5CC2208, // 0xC4 strb r2, [r12, #0x208]
    0x112FFF1E, // 0xC8 bxne lr
    0xE10F2000, // 0xCC mrs r2, cpsr
    0xE3820080, // 0xD0 orr r0, r2, #0x80
    0xE121F000, // 0xD4 msr cpsr_c, r0
    0xEE070F90, // 0xD8 mcr p15, 0, r0, c7, c0, 4
    0xE121F002, // 0xDC msr cpsr_c, r2
    0xEAFFFFF0, // 0xE0 b check
};

// The SWIs of the stub's vectors, by the low byte of their comment.
constexpr uint32_t kFaultSwi = 0xFE;
constexpr std::array<const char*, 6> kFaults = {"a reset",      "an undefined instruction", "a prefetch abort",
                                                "a data abort", "the reserved vector",      "an FIQ"};

// The BIOS's SWIs, by number.
enum Swi : uint32_t
{
    kWaitByLoop = 0x03,
    kIntrWait = 0x04,
    kVBlankIntrWait = 0x05,
    kHalt = 0x06,
    kDiv = 0x09,
    kCpuSet = 0x0B,
    kCpuFastSet = 0x0C,
    kSqrt = 0x0D,
    kGetCrc16 = 0x0E,
    kIsDebugger = 0x0F,
    kBitUnPack = 0x10,
    kLz77Uncompress = 0x11, // the version that reads and writes bytes
    kRlUncompress = 0x14,   // the same
    kDiff8bitUnFilter = 0x16,
    kDiff16bitUnFilter = 0x18,
};

// The ARM9's cycles of a loop of WaitByLoop.
constexpr uint64_t kWaitLoopCycles = 4;

// The I/O registers' offsets from 0x04000000.
enum Io : uint32_t
{
    kDispstat = 0x004,
    kVcount = 0x006,
    kCaptureControl = 0x064, // DISPCAPCNT
    kGraphicsAEnd = 0x070,
    kDmaStart = 0x0B0,
    kDmaEnd = 0x0E0,
    kDmaFillEnd = 0x0F0,
    kTimerStart = 0x100,
    kTimerEnd = 0x110,
    kKeyInput = 0x130,
    kKeyControl = 0x132,
    kIpcSync = 0x180,
    kIpcFifoControl = 0x184,
    kIpcFifoSend = 0x188,
    kSpiControl = 0x1A0, // AUXSPICNT
    kSpiData = 0x1A2,
    kCardControl = 0x1A4, // ROMCTRL
    kCardCommand = 0x1A8,
    kCardSeeds = 0x1B0,
    kCardEnd = 0x1BC,
    kExternalMemory = 0x204,
    kIme = 0x208,
    kIe = 0x210,
    kIf = 0x214,
    kVramControl = 0x240, // VRAMCNT_A to _G, then WRAMCNT at 0x247, then VRAMCNT_H and _I
    kWramControl = 0x247,
    kVramControlEnd = 0x24A,
    kDivControl = 0x280,
    kDivNumerator = 0x290,
    kDivDenominator = 0x298,
    kDivResult = 0x2A0,
    kDivRemainder = 0x2A8,
    kSqrtControl = 0x2B0,
    kSqrtResult = 0x2B4,
    kSqrtParameter = 0x2B8,
    kSqrtEnd = 0x2C0,
    kPostFlag = 0x300,
    kPowerControl = 0x304,
    k3dStart = 0x320,
    kGxStatus = 0x600,
    k3dEnd = 0x6A4,
    kGraphicsB = 0x1000,
    kGraphicsBEnd = 0x1070,
    kIoSize = 0x2000,
};

// The interrupts of the ARM9 alone.
constexpr uint32_t kGxFifoIrq = 1u << 21;

// DISPCAPCNT's busy bit, in the register's high half.
constexpr uint8_t kCaptureBusy = 0x80;

// The ARM9's other DMA control bits.
constexpr uint32_t kDmaRepeat = 1u << 25;
constexpr uint32_t kDmaCountMask = 0x1FFFFF;

// DMA's start timings: bits 27 to 29 of the control.
enum DmaTiming : uint32_t
{
    kDmaImmediate = 0,
    kDmaVblank = 1,
    kDmaHblank = 2,
    kDmaCard = 5,
    kDmaGxFifo = 7,
};

// AUXSPICNT's bits.
constexpr uint16_t kSpiBusy = 1u << 7;
constexpr uint16_t kSpiHold = 1u << 6;
constexpr uint16_t kSpiMode = 1u << 13;
constexpr uint16_t kCardIrqEnable = 1u << 14;

// The backup memory's command that reads its status register.
constexpr uint8_t kReadStatus = 0x05;

// The infrared chip's commands that pass a transfer to the memory and that give the chip's version, and the version.
constexpr uint8_t kInfraredMemory = 0x00;
constexpr uint8_t kInfraredVersion = 0x08;
constexpr uint8_t kInfraredChipVersion = 0xAA;

// ROMCTRL's bits.
constexpr uint32_t kCardReady = 1u << 23;
constexpr uint32_t kCardStart = 1u << 31;
constexpr uint32_t kCardSlowClock = 1u << 27;
constexpr uint32_t kCardGap = 0x1FFF;

// The card's commands.
constexpr uint8_t kReadData = 0xB7;
constexpr uint8_t kReadChipId = 0xB8;

// The card's clock: 5 or 8 of the system's cycles a byte.
constexpr uint64_t kCardFastCycles = 5;
constexpr uint64_t kCardSlowCycles = 8;

// EXMEMCNT: the card belongs to the ARM7.
constexpr uint16_t kCardToArm7 = 1u << 11;

bool IsCardRegister(uint32_t address)
{
    return (address >= kIo + kSpiControl && address < kIo + kCardEnd) || address == kCardData;
}

// The registers that the machine keeps with no meaning of their own: the graphics engines', the 3D engine's, KEYCNT,
// EXMEMCNT, VRAMCNT and POWCNT1.
bool IsStoredRegister(uint32_t offset)
{
    return offset < kGraphicsAEnd || (offset >= k3dStart && offset < k3dEnd) ||
           (offset >= kGraphicsB && offset < kGraphicsBEnd) || (offset >= kVramControl && offset < kVramControlEnd) ||
           offset == kKeyControl || offset == kExternalMemory || offset == kPowerControl;
}

// The card's chip ID for an image of `size` bytes: Macronix's maker code in the first byte, and the size in the
// second: (N + 1) MiB up to 0x7F, and (0x100 - N) * 256 MiB from 0xF0.
uint32_t ChipId(std::size_t size)
{
    uint64_t megabytes = 1;
    while (megabytes << 20 < size)
    {
        megabytes <<= 1;
    }

    const uint64_t code = megabytes <= 128 ? megabytes - 1 : 0x100 - (megabytes >> 8);
    return 0xC2 | static_cast<uint32_t>(code & 0xFF) << 8;
}

uint32_t DmaCount(uint32_t control)
{
    const uint32_t count = control & kDmaCountMask;
    return count == 0 ? kDmaCountMask + 1 : count;
}

uint32_t DmaTimingOf(uint32_t control)
{
    return (control >> 27) & 7;
}

} // namespace
} // namespace ntdlib::ntr
