// SPDX-License-Identifier: MIT

// Definitions that the parts of the ARM7's side of a DS share (arm7.cpp and arm7_io.cpp): the memory map, the
// firmware's area, the handshakes, the BIOS and the registers. The two files build as one translation unit. The
// definitions therefore keep internal linkage. Only those files include this header.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "bios.h"
#include "dsi_bios.h"
#include "dsi_io.h"
#include "io.h"
#include "new_wram.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/ntr/timers.h"
#include "spi_devices.h"

namespace ntdlib::ntr
{

namespace
{

constexpr uint32_t kBiosSize = 0x4000;
constexpr uint32_t kSharedWramSize = 0x8000;
constexpr uint32_t kWramSize = 0x10000;
constexpr uint32_t kVramSize = 0x40000;

constexpr uint32_t kIpcFifoSend = 0x04000188;

// Where the firmware leaves the cartridge's header, and the lock that the ARM9's OS_InitLock takes before the ARM7's
// OS_InitLock goes on: the lock word's owner, the ARM9's initial ID. The places in main memory's last page count from
// the memory's end: on a DS 0x027FFE00 and 0x027FFFF4, in DSi mode 0x02FFFE00 and 0x02FFFFF4. The header's copy is
// 0x170 bytes on a DS and 0x160 in DSi mode (GBATEK's "DSi BIOS RAM").
constexpr uint32_t kHeaderCopy = 0x200;
constexpr uint32_t kHeaderCopySize = 0x170;
constexpr uint32_t kDsiHeaderCopySize = 0x160;
constexpr uint32_t kInitLockOwner = 0x00C;
constexpr uint16_t kArm9InitLockId = 0x7F;

// Main memory's last page holds the area that the firmware and both CPUs share at start-up. The CPU reaches it through
// Read and Write. The system's ARM9 can then answer the ARM7 there.
constexpr uint32_t kBootArea = 0x1000;

// The handshakes that a DSi-enhanced cartridge's binaries make at start-up. The system's ARM9 answers the ARM7's writes
// (Arm7System::AnswerArm9).
//
// In the first, each CPU increments a 16-bit counter, waits for the other's to change, and increments its counter
// again: the ARM9's, then the ARM7's. On a DS they are at 0x027FFC24 and 0x027FFC26.
constexpr uint32_t kArm9StartCounter = 0x3DC;
constexpr uint32_t kArm7StartCounter = 0x3DA;

// The second takes 4 bytes, at 0x027FFFF0 on a DS. One CPU writes 0x80 and a count in byte 0, waits until the other
// has copied it into byte 1, and writes the next count, until the other sets byte 2. It then writes 1 in byte 3. The
// other CPU copies each new value of byte 0 and adds it up. Once the sum reaches 768, it writes 0 in byte 3 and 1 in
// byte 2, and waits for byte 3. The ARM7 first copies, then writes, once each. The system's ARM9 starts its count and
// its sum at 0 and takes each side once. The ARM9's code does the same. The same bytes hold the lock word of
// OS_InitLock. The CPUs write the lock word 32 bits at a time, and the system's ARM9 answers only the writes of single
// bytes.
constexpr uint32_t kHandshake = 0x010;
constexpr uint32_t kHandshakeSum = 768;

// The IPC's words of the tags whose handlers each CPU has set: the ARM9's, at 0x027FFF88 on a DS, then the ARM7's. The
// ARM7's libraries wait for the ARM9's handlers of their tags. The system's ARM9 has a handler for every tag.
constexpr uint32_t kArm9ReadyTags = 0x078;

// DSi mode: the BIOS's GetPitchTable reads from the sine table, 0x46A entries before the pitch table. The SDK passes
// the index less 0x46A (GBATEK's "GetPitchTable (DSi7: bugged)").
constexpr uint32_t kDsiPitchTableShift = 0x46A;

// DSi mode: SCFG_EXT7 as the launcher leaves it for a game.
constexpr uint32_t kDsiArm7Extended = 0x93FFFB06;

// DSi mode: the header's new shared WRAM settings and the bits of SCFG_EXT7 that the header gives (GBATEK's "DSi
// Cartridge Header"), and where the launcher leaves the game's SCFG_EXT7.
constexpr std::size_t kHeaderWramSettings = 0x180;
constexpr std::size_t kHeaderExtended = 0x1B8;
constexpr uint32_t kDsiExtendedFromHeader = 0x80040407;
constexpr uint32_t kDsiExtendedCopy = 0x0380FFC4;

// The stack pointers that the BIOS gives the ARM7's modes before it starts a game.
constexpr uint32_t kSupervisorStack = 0x0380FFDC;
constexpr uint32_t kIrqStack = 0x0380FFB0;
constexpr uint32_t kSystemStack = 0x0380FF00;

// The BIOS's vector of undefined instructions. The CPU stops there.
constexpr uint32_t kUndefinedVector = 0x04;

constexpr uint16_t kSoundBiasMiddle = 0x200;
constexpr uint32_t kSoundBias = 0x04000504;

// The BIOS in place of the console's: the exception vectors, the IRQ handler, and IntrWait in ARM code. They run as the
// console's BIOS runs them. The IRQ handler calls the program's handler at 0x0380FFFC. The SWI vector serves only
// IntrWait and VBlankIntrWait: it runs IntrWait in system mode with the caller's IRQ mask. The console's BIOS runs its
// functions that way too. IntrWait sets IME, clears the flags of r1 at 0x0380FFF8 first when r0 isn't 0, then halts
// until the program's handler sets one of them, and clears them. The system answers the other SWIs itself
// (Arm7System::Swi).
constexpr std::array<uint32_t, 48> kBios = {
    0xEAFFFFFE, // 0x00 reset: b 0x00
    0xEAFFFFFE, // 0x04 undefined instruction: b 0x04
    0xEA000009, // 0x08 SWI: b 0x34
    0xEAFFFFFE, // 0x0C prefetch abort: b 0x0C
    0xEAFFFFFE, // 0x10 data abort: b 0x10
    0xEAFFFFFE, // 0x14 reserved: b 0x14
    0xE92D500F, // 0x18 IRQ: stmfd sp!, {r0-r3, r12, lr}
    0xE3A00301, // 0x1C mov r0, #0x04000000
    0xE28FE000, // 0x20 add lr, pc, #0
    0xE510F004, // 0x24 ldr pc, [r0, #-4]
    0xE8BD500F, // 0x28 ldmfd sp!, {r0-r3, r12, lr}
    0xE25EF004, // 0x2C subs pc, lr, #4
    0xEAFFFFFE, // 0x30 FIQ: b 0x30
    0xE92D5800, // 0x34 stmfd sp!, {r11, r12, lr}
    0xE14FB000, // 0x38 mrs r11, spsr
    0xE92D0800, // 0x3C stmfd sp!, {r11}
    0xE20BB080, // 0x40 and r11, r11, #0x80
    0xE38BB01F, // 0x44 orr r11, r11, #0x1F
    0xE129F00B, // 0x48 msr cpsr_fc, r11
    0xE92D400C, // 0x4C stmfd sp!, {r2, r3, lr}
    0xEB000006, // 0x50 bl 0x70
    0xE8BD400C, // 0x54 ldmfd sp!, {r2, r3, lr}
    0xE3A0C0D3, // 0x58 mov r12, #0xD3
    0xE129F00C, // 0x5C msr cpsr_fc, r12
    0xE8BD0800, // 0x60 ldmfd sp!, {r11}
    0xE169F00B, // 0x64 msr spsr_fc, r11
    0xE8BD5800, // 0x68 ldmfd sp!, {r11, r12, lr}
    0xE1B0F00E, // 0x6C movs pc, lr
    0xE3A03301, // 0x70 IntrWait: mov r3, #0x04000000
    0xE3A02001, // 0x74 mov r2, #1
    0xE5C32208, // 0x78 strb r2, [r3, #0x208]
    0xE3500000, // 0x7C cmp r0, #0
    0x0A000002, // 0x80 beq 0x90
    0xE5132008, // 0x84 ldr r2, [r3, #-8]
    0xE1C22001, // 0x88 bic r2, r2, r1
    0xE5032008, // 0x8C str r2, [r3, #-8]
    0xE3A02000, // 0x90 mov r2, #0
    0xE5C32208, // 0x94 strb r2, [r3, #0x208]
    0xE5132008, // 0x98 ldr r2, [r3, #-8]
    0xE0120001, // 0x9C ands r0, r2, r1
    0x11C22001, // 0xA0 bicne r2, r2, r1
    0x15032008, // 0xA4 strne r2, [r3, #-8]
    0xE3A02001, // 0xA8 mov r2, #1
    0xE5C32208, // 0xAC strb r2, [r3, #0x208]
    0x112FFF1E, // 0xB0 bxne lr
    0xE3A02080, // 0xB4 mov r2, #0x80
    0xE5C32301, // 0xB8 strb r2, [r3, #0x301]
    0xEAFFFFF3, // 0xBC b 0x90
};

// The BIOS's other SWIs, by number (bios::Swi lists the rest).
enum Swi : uint32_t
{
    kSoundBiasSwi = 0x08,
    kGetPitchTable = 0x1B,
    kGetVolumeTable = 0x1C,
    kCustomHalt = 0x1F,
};

// The I/O registers' offsets from 0x04000000.
enum Io : uint32_t
{
    kDispstat = 0x004,
    kVcount = 0x006,
    kTimerStart = 0x100,
    kTimerEnd = 0x110,
    kKeyInput = 0x130,
    kKeyControl = 0x132,
    kSerialControl = 0x134,
    kExtendedKeys = 0x136,
    kRtc = 0x138,
    kIpcSync = 0x180,
    kIpcFifoControl = 0x184,
    kCardStart = 0x1A0,
    kCardEnd = 0x1C0,
    kSpiControl = 0x1C0,
    kSpiData = 0x1C2,
    kExternalMemory = 0x204,
    kWifiWait = 0x206,
    kMemoryStatus = 0x240,
    kPostFlag = 0x300,
    kPowerControl = 0x304,
    kBiosProtection = 0x308,
    kIoEnd = 0x400,
};

constexpr uint16_t kExtendedKeysIdle = 0x007F; // X and Y released, the pen up, the hinge open

// SPICNT's bits.
constexpr uint16_t kSpiHold = 1u << 11;
constexpr uint16_t kSpiIrq = 1u << 14;
constexpr uint16_t kSpiEnable = 1u << 15;

// HALTCNT's modes.
constexpr uint8_t kHaltMode = 0x80;
constexpr uint8_t kHaltModeMask = 0xC0;

bool IsCardRegister(uint32_t address)
{
    return (address >= kIo + kCardStart && address < kIo + kCardEnd - 4) || address == kCardData;
}

} // namespace

} // namespace ntdlib::ntr
