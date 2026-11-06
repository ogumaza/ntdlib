// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/machine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "spi_devices.h"

namespace ntdlib::ntr
{

namespace
{

constexpr uint32_t kMainBase = 0x02000000;
constexpr uint32_t kMainSize = 0x400000;
constexpr uint32_t kBiosBase = 0xFFFF0000;
constexpr uint32_t kBiosSize = 0x8000;
constexpr uint32_t kSmallMemory = 0x1000; // the palettes and the OAM: 2 KiB each, kept in a page each
constexpr uint32_t kIo = 0x04000000;
constexpr uint32_t kIpcFifoReceive = 0x04100000;
constexpr uint32_t kCardData = 0x04100010;
constexpr std::size_t kFifoWords = 16;

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
    kSoftReset = 0x00,
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

constexpr uint16_t kKeysReleased = 0x03FF;

// The interrupts of the ARM9 alone.
constexpr uint32_t kGxFifoIrq = 1u << 21;

// DISPSTAT's bits.
constexpr uint16_t kVblankFlag = 1u << 0;
constexpr uint16_t kHblankFlag = 1u << 1;
constexpr uint16_t kVcountFlag = 1u << 2;
constexpr uint16_t kVblankIrq = 1u << 3;
constexpr uint16_t kHblankIrq = 1u << 4;
constexpr uint16_t kVcountIrq = 1u << 5;
constexpr uint16_t kDispstatWritable = 0xFFB8;
constexpr uint64_t kHblankCycle = 256 * 6;

// DISPCAPCNT's busy bit, in the register's high half.
constexpr uint8_t kCaptureBusy = 0x80;

// Timer control bits.
constexpr uint16_t kTimerCascade = 1u << 2;
constexpr uint16_t kTimerIrq = 1u << 6;
constexpr uint16_t kTimerStartBit = 1u << 7;
constexpr uint16_t kTimerWritable = 0x00C7;
constexpr std::array<uint64_t, 4> kPrescales = {1, 64, 256, 1024};

// IPCFIFOCNT's bits.
constexpr uint16_t kSendEmpty = 1u << 0;
constexpr uint16_t kSendFull = 1u << 1;
constexpr uint16_t kSendEmptyIrq = 1u << 2;
constexpr uint16_t kSendClear = 1u << 3;
constexpr uint16_t kReceiveEmpty = 1u << 8;
constexpr uint16_t kReceiveFull = 1u << 9;
constexpr uint16_t kReceiveIrq = 1u << 10;
constexpr uint16_t kFifoError = 1u << 14;
constexpr uint16_t kFifoEnable = 1u << 15;
constexpr uint16_t kFifoWritable = kSendEmptyIrq | kReceiveIrq | kFifoEnable;

// IPCSYNC's bits.
constexpr uint16_t kSyncOutput = 0x0F00;
constexpr uint16_t kSyncRequest = 1u << 13;
constexpr uint16_t kSyncIrqEnable = 1u << 14;

// DMA control bits.
constexpr uint32_t kDmaEnable = 1u << 31;
constexpr uint32_t kDmaIrq = 1u << 30;
constexpr uint32_t kDmaWords = 1u << 26;
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

uint64_t VcountSetting(uint16_t dispstat)
{
    return static_cast<uint64_t>(dispstat >> 8) | static_cast<uint64_t>(dispstat & 0x80) << 1;
}

std::string Hex(uint32_t value)
{
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "0x%08X", value);
    return text.data();
}

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

Machine::Machine(const Rom& rom, const BackupMemory& backup)
    : image_(rom.Image()),
      arm7_(std::make_unique<Arm7System>(rom, static_cast<Arm9Link&>(*this))),
      arm9_(std::make_unique<arm::Cpu>(*this, arm::Architecture::kArmV5TE)),
      bios_(kBiosSize),
      palette_(kSmallMemory),
      oam_(kSmallMemory)
{
    if (backup.address_bytes != 0)
    {
        const std::size_t largest = backup.address_bytes == 1 ? 512 : backup.address_bytes == 2 ? 0x10000 : 0x1000000;
        if (backup.address_bytes < 1 || backup.address_bytes > 3 || !std::has_single_bit(backup.size) ||
            backup.size > largest || (backup.address_bytes == 1 && backup.size != largest))
        {
            throw std::invalid_argument("a backup memory with " + std::to_string(backup.address_bytes) +
                                        " address bytes can't hold " + std::to_string(backup.size) + " bytes");
        }
        backup_ = std::make_unique<SpiMemory>(backup.address_bytes, std::vector<uint8_t>(backup.size, backup.blank));
    }
    card_.infrared = backup.infrared;
    for (std::size_t i = 0; i < vram_.size(); i++)
    {
        vram_[i].assign(kVramSizes[i], 0);
    }
    for (std::size_t i = 0; i < kBios.size(); i++)
    {
        xsflib::StoreLe32(&bios_[4 * i], kBios[i]);
    }
    Boot(rom);
}

Machine::~Machine() = default;

std::span<uint8_t> Machine::Backup()
{
    return backup_ == nullptr ? std::span<uint8_t>() : backup_->Data();
}

arm::Cpu& Machine::Arm9()
{
    return *arm9_;
}

Arm7System& Machine::Arm7()
{
    return *arm7_;
}

std::span<uint8_t> Machine::MainMemory()
{
    return arm7_->MainMemory();
}

const MachineCounts& Machine::Counts() const
{
    return counts_;
}

uint64_t Machine::Clock() const
{
    return arm7_->Clock();
}

void Machine::Boot(const Rom& rom)
{
    LoadArm9(rom);

    const std::span<uint8_t> main = MainMemory();
    const auto put = [&main](uint32_t address, int size, uint32_t value)
    {
        for (int i = 0; i < size; i++)
        {
            main[(address - kMainBase + static_cast<uint32_t>(i)) & (kMainSize - 1)] =
                static_cast<uint8_t>(value >> (8 * i));
        }
    };
    chip_id_ = ChipId(image_.size());
    const auto header16 = [this](std::size_t at)
    {
        return at + 2 <= image_.size() ? static_cast<uint32_t>(xsflib::LoadLe16(&image_[at])) : 0u;
    };
    for (const uint32_t area : {kBootArea, kBootAreaCopy})
    {
        put(area, 4, chip_id_);
        put(area + 4, 4, chip_id_);
        put(area + 8, 2, header16(0x15E));
        put(area + 0xA, 2, header16(0x6C));
    }
    put(kBootArea + 0x50, 2, kArm7BiosCrc);
    put(kBootAreaCopy + 0x10, 2, kArm7BiosCrc);
    put(kBootIndicator, 2, 1);
    const std::array<uint8_t, 0x70> settings = FirmwareUserSettings();
    for (std::size_t i = 0; i < settings.size(); i++)
    {
        put(kUserSettings + static_cast<uint32_t>(i), 1, settings[i]);
    }

    io_[kPostFlag] = 1;
    arm9_->SetSwiHandler([this](arm::Cpu& cpu, uint32_t comment) { return Swi(cpu, comment); });
    arm9_->Jump(rom.Header().arm9.entry, arm::Mode::kSupervisor);
    ScheduleDisplay();
}

void Machine::LoadArm9(const Rom& rom)
{
    const Binary& arm9 = rom.Header().arm9;
    const std::span<const uint8_t> binary = rom.Arm9();
    if (arm9.ram_address < kMainBase || arm9.ram_address - kMainBase + uint64_t{binary.size()} > kMainSize)
    {
        throw std::runtime_error("the ARM9 binary doesn't fit in main memory at " + Hex(arm9.ram_address));
    }

    const std::span<uint8_t> main = MainMemory();
    uint8_t* at = &main[arm9.ram_address - kMainBase];
    std::memcpy(at, binary.data(), binary.size());

    // The firmware decrypts the secure area and marks its start.
    if (arm9.rom_offset == kSecureArea && binary.size() >= kSecureAreaId.size() &&
        std::equal(kSecureAreaId.begin(), kSecureAreaId.end(), at))
    {
        xsflib::StoreLe32(at, kSecureAreaMark);
        xsflib::StoreLe32(at + 4, kSecureAreaMark);
    }
}

bool Machine::Run(uint64_t until, std::vector<SoundOutput>& out)
{
    bool stopped = false;
    while (Clock() < until && error_.empty())
    {
        if (arm9_->Halted() && !irq_line_)
        {
            // The ARM9 waits for an interrupt: the system runs to the ARM9's next event, or until the ARM7 wakes it.
            arm7_->Run(std::min(until, NextEvent()), out);
            Events();
            continue;
        }

        polls_ = 0;
        arm9_running_ = true;
        const uint64_t start = arm9_->Cycles();
        const arm::StopReason reason = arm9_->Run(kSlice);
        arm9_running_ = false;
        if (reason == arm::StopReason::kStopAddress)
        {
            stopped = true;
            break;
        }

        // A burst that ran for a slice or polled passes its time, until the ARM7 makes a change that the ARM9 can
        // see. The ARM9 can't see one in WaitByLoop.
        uint64_t cycles = 0;
        if (reason == arm::StopReason::kCycles)
        {
            counts_.slices++;
            cycles = arm9_->Cycles() - start;
        }
        if (polled_)
        {
            polled_ = false;
            counts_.polls++;
            cycles = arm9_->Cycles() - start;
        }
        if (reason == arm::StopReason::kHalted && irq_line_)
        {
            // The IRQ line ends the halt at once while the CPSR masks the IRQ: the ARM9 spins, and its loops take the
            // time of their instructions.
            spin_cycles_ += arm9_->Cycles() - start;
            if (spin_cycles_ >= kSpinCycles)
            {
                cycles = spin_cycles_;
                spin_cycles_ = 0;
            }
        }
        if (cycles > 0 && error_.empty())
        {
            Pass(Clock() + (cycles + 1) / 2, true, out);
        }
        if (wait_cycles_ > 0 && error_.empty())
        {
            Pass(Clock() + (wait_cycles_ + 1) / 2, false, out);
        }
        wait_cycles_ = 0;
    }

    if (!error_.empty())
    {
        throw std::runtime_error(error_);
    }
    return stopped;
}

uint32_t Machine::Call(uint32_t address, std::span<const uint32_t> args, uint64_t until, std::vector<SoundOutput>& out)
{
    arm::Cpu& cpu = *arm9_;
    const uint32_t saved_pc = cpu.Pc();
    const uint32_t saved_sp = cpu.Reg(13);
    const uint32_t saved_stop = cpu.StopAddress();
    const bool saved_halted = cpu.Halted();

    // r0 to r3, then the rest on the stack from SP upward, with SP kept 8-byte aligned.
    for (std::size_t i = 0; i < std::min<std::size_t>(args.size(), 4); i++)
    {
        cpu.SetReg(static_cast<int>(i), args[i]);
    }
    if (args.size() > 4)
    {
        const auto stacked = static_cast<uint32_t>(args.size() - 4);
        const uint32_t sp = (saved_sp - stacked * 4) & ~7u;
        for (uint32_t i = 0; i < stacked; i++)
        {
            cpu.Store(sp + i * 4, 4, args[4 + i]);
        }
        cpu.SetReg(13, sp);
    }
    cpu.SetReg(14, arm::Cpu::kCallReturn);
    cpu.SetStopAddress(arm::Cpu::kCallReturn);
    cpu.Jump(address, static_cast<arm::Mode>(cpu.Cpsr() & arm::psr::kModeMask));

    const bool returned = Run(until, out);
    const uint32_t result = cpu.Reg(0);

    // Go on from where the CPU was before the call.
    cpu.SetStopAddress(saved_stop);
    cpu.SetReg(13, saved_sp);
    cpu.Jump(saved_pc, static_cast<arm::Mode>(cpu.Cpsr() & arm::psr::kModeMask));
    if (saved_halted)
    {
        cpu.Halt();
    }

    if (!returned)
    {
        throw std::runtime_error("the call to " + Hex(address) + " didn't return by the clock " +
                                 std::to_string(until));
    }
    return result;
}

uint8_t* Machine::Page(uint32_t page)
{
    switch (page >> 24)
    {
    case 0x02:
        return MainMemory().data() + (page & (kMainSize - 1));
    case 0x03:
        {
            // WRAMCNT gives the ARM9 all of the shared work memory (0), its second half (1), its first half (2) or
            // none (3).
            const std::span<uint8_t> shared = arm7_->SharedWram();
            const auto half = static_cast<uint32_t>(shared.size() / 2);
            switch (wram_control_)
            {
            case 0:
                return shared.data() + (page & (shared.size() - 1));
            case 1:
                return shared.data() + half + (page & (half - 1));
            case 2:
                return shared.data() + (page & (half - 1));
            default:
                return nullptr;
            }
        }
    case 0x05:
        return palette_.data();
    case 0x06:
        {
            const uint32_t area = (page >> 21) & 7;
            if (area < 4)
            {
                return vram_[area].data() + (page & (vram_[area].size() - 1));
            }
            const uint32_t offset = page & 0x1FFFFF;
            return area == 4 && offset < vram_[4].size() ? vram_[4].data() + offset : nullptr;
        }
    case 0x07:
        return oam_.data();
    case 0xFF:
        return page >= kBiosBase && page < kBiosBase + kBiosSize ? bios_.data() + (page - kBiosBase) : nullptr;
    default:
        return nullptr;
    }
}

uint32_t Machine::Read(uint32_t address, int size)
{
    switch (address >> 24)
    {
    case 0x04:
        return ReadIo(address, size);
    case 0x08:
    case 0x09:
    case 0x0A:
        // No GBA cartridge: the slot's lines read as high.
        return size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
    default:
        Unknown(address, size, false);
        return 0;
    }
}

void Machine::Write(uint32_t address, int size, uint32_t value)
{
    switch (address >> 24)
    {
    case 0x04:
        WriteIo(address, size, value);
        return;
    case 0x08:
    case 0x09:
    case 0x0A:
        return;
    default:
        Unknown(address, size, true);
        return;
    }
}

uint16_t Machine::SyncOutput() const
{
    return static_cast<uint16_t>((ipcsync_ & kSyncOutput) >> 8);
}

void Machine::OnSyncWrite(bool irq)
{
    arm7_changed_ = true;
    if (irq && (ipcsync_ & kSyncIrqEnable) != 0)
    {
        Request(irq::kIpcSync);
    }
}

void Machine::OnFifoToArm9(uint32_t word)
{
    arm7_changed_ = true;
    if (on_fifo_word)
    {
        on_fifo_word(false, word);
    }
    if (arm7_->WordsToArm9() == 1 && (fifo_control_ & kReceiveIrq) != 0)
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

void Machine::OnFifoToArm7Empty()
{
    arm7_changed_ = true;
    if ((fifo_control_ & kSendEmptyIrq) != 0)
    {
        Request(irq::kIpcSendEmpty);
    }
}

uint32_t Machine::ReadCard(uint32_t address, int size)
{
    if (address == kCardData)
    {
        return ReadCardData();
    }

    const uint32_t offset = address - kIo;
    if (size == 4)
    {
        return CardRead16(offset) | static_cast<uint32_t>(CardRead16(offset + 2)) << 16;
    }
    const uint16_t half = CardRead16(offset & ~1u);
    return size == 2 ? half : (half >> (8 * (offset & 1))) & 0xFF;
}

void Machine::WriteCard(uint32_t address, int size, uint32_t value)
{
    if (address == kCardData)
    {
        // The card takes no writes of data.
        Unknown(address, size, true);
        return;
    }

    const uint32_t offset = address - kIo;
    if (size == 4)
    {
        CardWrite16(offset, static_cast<uint16_t>(value), 0xFFFF);
        CardWrite16(offset + 2, static_cast<uint16_t>(value >> 16), 0xFFFF);
    }
    else if (size == 2)
    {
        CardWrite16(offset, static_cast<uint16_t>(value), 0xFFFF);
    }
    else
    {
        const int shift = 8 * static_cast<int>(offset & 1);
        CardWrite16(offset & ~1u, static_cast<uint16_t>((value & 0xFF) << shift), static_cast<uint16_t>(0xFF << shift));
    }
}

uint16_t Machine::CardRead16(uint32_t offset)
{
    switch (offset)
    {
    case kSpiControl:
        return static_cast<uint16_t>(card_.spi_control & ~kSpiBusy);
    case kSpiData:
        return card_.spi_data;
    case kCardControl:
        return static_cast<uint16_t>(card_.control);
    case kCardControl + 2:
        {
            Poll();
            uint32_t control = card_.control;
            control |= card_.ready && card_.position < card_.data.size() ? kCardReady : 0;
            control |= card_.busy ? kCardStart : 0;
            return static_cast<uint16_t>(control >> 16);
        }
    default:
        if (offset >= kCardCommand && offset < kCardSeeds)
        {
            const std::size_t at = offset - kCardCommand;
            return static_cast<uint16_t>(card_.command[at] | card_.command[at + 1] << 8);
        }
        return Stored(offset);
    }
}

void Machine::CardWrite16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const auto merge = [value, mask](uint16_t old)
    {
        return static_cast<uint16_t>((old & ~mask) | (value & mask));
    };

    switch (offset)
    {
    case kSpiControl:
        card_.spi_control = static_cast<uint16_t>(merge(card_.spi_control) & ~kSpiBusy);
        if ((card_.spi_control & kSpiMode) == 0 && card_.spi_selected)
        {
            // The memory is selected only in SPI mode.
            EndSpi();
        }
        return;
    case kSpiData:
        if ((mask & 0xFF) != 0)
        {
            WriteSpiData(static_cast<uint8_t>(value));
        }
        return;
    case kCardControl:
        card_.control = (card_.control & 0xFFFF0000) | merge(static_cast<uint16_t>(card_.control));
        return;
    case kCardControl + 2:
        {
            const uint32_t control = (card_.control & 0xFFFF) |
                                     static_cast<uint32_t>(merge(static_cast<uint16_t>(card_.control >> 16))) << 16;
            card_.control = control & ~(kCardStart | kCardReady);
            if ((control & kCardStart) != 0 && !card_.busy)
            {
                StartCard();
            }
            return;
        }
    default:
        if (offset >= kCardCommand && offset < kCardSeeds)
        {
            const std::size_t at = offset - kCardCommand;
            const uint16_t old = static_cast<uint16_t>(card_.command[at] | card_.command[at + 1] << 8);
            const uint16_t merged = merge(old);
            card_.command[at] = static_cast<uint8_t>(merged);
            card_.command[at + 1] = static_cast<uint8_t>(merged >> 8);
            return;
        }
        Store16(offset, value, mask);
        return;
    }
}

void Machine::StartCard()
{
    const uint32_t code = (card_.control >> 24) & 7;
    const std::size_t bytes = code == 0 ? 0 : code == 7 ? 4 : std::size_t{0x100} << code;
    card_.data.assign(bytes, 0xFF);
    switch (card_.command[0])
    {
    case kReadData:
        {
            // Reads below 0x8000 come from 0x8000 plus the address's low 9 bits.
            const uint32_t address = static_cast<uint32_t>(card_.command[1]) << 24 |
                                     static_cast<uint32_t>(card_.command[2]) << 16 |
                                     static_cast<uint32_t>(card_.command[3]) << 8 | card_.command[4];
            for (std::size_t i = 0; i < bytes; i++)
            {
                uint64_t at = uint64_t{address} + i;
                if (at < 0x8000)
                {
                    at = 0x8000 + (at & 0x1FF);
                }
                card_.data[i] = at < image_.size() ? image_[at] : 0xFF;
            }
            counts_.card_reads++;
            counts_.card_bytes += bytes;
            break;
        }
    case kReadChipId:
        for (std::size_t i = 0; i + 4 <= bytes; i += 4)
        {
            xsflib::StoreLe32(&card_.data[i], chip_id_);
        }
        break;
    default:
        Unknown(kIo + kCardCommand, 1, true);
        break;
    }

    card_.position = 0;
    card_.busy = true;
    card_.ready = false;
    const uint64_t cycles = (card_.control & kCardSlowClock) != 0 ? kCardSlowCycles : kCardFastCycles;
    card_.ready_at = Clock() + (8 + (card_.control & kCardGap) + bytes) * cycles;
}

void Machine::CardReady()
{
    card_.ready = true;
    if (card_.data.empty())
    {
        EndCard();
        return;
    }
    RunCardDmas();
}

void Machine::EndCard()
{
    card_.busy = false;
    card_.ready = false;
    if ((card_.spi_control & kCardIrqEnable) == 0)
    {
        return;
    }

    if ((Stored(kExternalMemory) & kCardToArm7) != 0)
    {
        arm7_->RequestIrq(irq::kCard);
    }
    else
    {
        Request(irq::kCard);
    }
}

uint32_t Machine::ReadCardData()
{
    if (!card_.ready || card_.position >= card_.data.size())
    {
        return 0xFFFFFFFF;
    }

    const uint32_t word = xsflib::LoadLe32(&card_.data[card_.position]);
    card_.position += 4;
    if (card_.position >= card_.data.size())
    {
        EndCard();
    }
    return word;
}

void Machine::WriteSpiData(uint8_t value)
{
    if ((card_.spi_control & kSpiMode) == 0)
    {
        return;
    }

    if (!card_.spi_selected)
    {
        card_.spi_position = 0;
    }
    const std::size_t position = card_.spi_position++;
    if (card_.infrared)
    {
        // The infrared chip takes the transfer's first byte.
        if (position == 0)
        {
            card_.spi_command = value;
            card_.spi_data = 0xFF;
        }
        else if (card_.spi_command == kInfraredMemory && backup_ != nullptr)
        {
            card_.spi_data = backup_->Transfer(value, position == 1);
        }
        else
        {
            card_.spi_data = card_.spi_command == kInfraredVersion && position == 1 ? kInfraredChipVersion : 0;
        }
        card_.spi_selected = true;
        if ((card_.spi_control & kSpiHold) == 0)
        {
            EndSpi();
        }
        return;
    }

    if (backup_ != nullptr)
    {
        card_.spi_data = backup_->Transfer(value, position == 0);
        card_.spi_selected = true;
        if ((card_.spi_control & kSpiHold) == 0)
        {
            EndSpi();
        }
        return;
    }

    // A blank backup memory: it reads as 0xFF, and its status as 0, never busy.
    if (!card_.spi_selected)
    {
        card_.spi_selected = true;
        card_.spi_command = value;
        card_.spi_data = 0xFF;
    }
    else
    {
        card_.spi_data = card_.spi_command == kReadStatus ? 0x00 : 0xFF;
    }
    if ((card_.spi_control & kSpiHold) == 0)
    {
        card_.spi_selected = false;
    }
}

void Machine::EndSpi()
{
    if (backup_ != nullptr && (!card_.infrared || (card_.spi_command == kInfraredMemory && card_.spi_position > 1)))
    {
        backup_->End();
    }
    card_.spi_selected = false;
}

uint32_t Machine::ReadIo(uint32_t address, int size)
{
    if (address == kIpcFifoReceive)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return arm7_->WordsToArm9() == 0 ? last_received_ : arm7_->ReceiveFromArm7(false);
        }
        if (arm7_->WordsToArm9() == 0)
        {
            fifo_error_ = true;
            return last_received_;
        }
        last_received_ = arm7_->ReceiveFromArm7(true);
        return last_received_;
    }
    if (IsCardRegister(address))
    {
        return ReadCard(address, size);
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoSize)
    {
        Unknown(address, size, false);
        return 0;
    }

    if (size == 4)
    {
        return ReadIo16(offset) | static_cast<uint32_t>(ReadIo16(offset + 2)) << 16;
    }
    const uint16_t half = ReadIo16(offset & ~1u);
    return size == 2 ? half : (half >> (8 * (offset & 1))) & 0xFF;
}

void Machine::WriteIo(uint32_t address, int size, uint32_t value)
{
    if (address == kIo + kIpcFifoSend)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return;
        }
        if (arm7_->WordsToArm7() == kFifoWords)
        {
            fifo_error_ = true;
            return;
        }
        if (on_fifo_word)
        {
            on_fifo_word(true, value);
        }
        arm7_->SendToArm7(value);
        return;
    }
    if (IsCardRegister(address))
    {
        WriteCard(address, size, value);
        return;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoSize)
    {
        Unknown(address, size, true);
        return;
    }

    if (size == 4)
    {
        WriteIo16(offset, static_cast<uint16_t>(value), 0xFFFF);
        WriteIo16(offset + 2, static_cast<uint16_t>(value >> 16), 0xFFFF);
    }
    else if (size == 2)
    {
        WriteIo16(offset, static_cast<uint16_t>(value), 0xFFFF);
    }
    else
    {
        const int shift = 8 * static_cast<int>(offset & 1);
        WriteIo16(offset & ~1u, static_cast<uint16_t>((value & 0xFF) << shift), static_cast<uint16_t>(0xFF << shift));
    }
}

uint16_t Machine::ReadIo16(uint32_t offset)
{
    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        // Only the control half reads back.
        const Dma& dma = dmas_[(offset - kDmaStart) / 12];
        return (offset - kDmaStart) % 12 == 10 ? static_cast<uint16_t>(dma.control >> 16) : 0;
    }
    if (offset >= kDmaEnd && offset < kDmaFillEnd)
    {
        const Dma& dma = dmas_[(offset - kDmaEnd) / 4];
        return static_cast<uint16_t>(dma.fill >> (offset % 4 == 0 ? 0 : 16));
    }
    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        Poll();
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        return offset % 4 == 0 ? TimerCounter(timer) : timers_[static_cast<std::size_t>(timer)].control;
    }

    switch (offset)
    {
    case kDispstat:
        {
            Poll();
            const uint64_t line = Line();
            uint16_t flags = 0;
            flags |= line >= kVblankLine && line < kFrameLines - 1 ? kVblankFlag : 0;
            flags |= Clock() % kLineCycles >= kHblankCycle ? kHblankFlag : 0;
            flags |= line == VcountSetting(dispstat_) ? kVcountFlag : 0;
            return static_cast<uint16_t>((dispstat_ & kDispstatWritable) | flags);
        }
    case kVcount:
        Poll();
        return static_cast<uint16_t>(Line());
    case kKeyInput:
        return kKeysReleased;
    case kIpcSync:
        Poll();
        return static_cast<uint16_t>((ipcsync_ & (kSyncOutput | kSyncIrqEnable)) | arm7_->SyncOutput());
    case kIpcFifoControl:
        Poll();
        return FifoControl();
    case kIme:
        return ime_;
    case kIme + 2:
        return 0;
    case kIe:
        return static_cast<uint16_t>(ie_);
    case kIe + 2:
        return static_cast<uint16_t>(ie_ >> 16);
    case kIf:
        Poll();
        return static_cast<uint16_t>(if_);
    case kIf + 2:
        Poll();
        return static_cast<uint16_t>(if_ >> 16);
    case kWramControl - 1:
        return static_cast<uint16_t>(io_[kWramControl - 1] | wram_control_ << 8);
    case kGxStatus:
        // Never busy.
        return 0;
    case kGxStatus + 2:
        // The FIFO is empty, and so less than half full. Bits 30 and 31 keep the FIFO IRQ's condition.
        return static_cast<uint16_t>(0x0600 | (Stored(kGxStatus + 2) & 0xC000));
    case kPostFlag:
    case kCaptureControl:
    case kCaptureControl + 2:
        return Stored(offset);
    default:
        if ((offset >= kDivControl && offset < kSqrtEnd) || IsStoredRegister(offset))
        {
            return Stored(offset);
        }
        Unknown(kIo + offset, 2, false);
        return Stored(offset);
    }
}

void Machine::WriteIo16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const auto merge = [value, mask](uint16_t old)
    {
        return static_cast<uint16_t>((old & ~mask) | (value & mask));
    };

    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        const std::size_t channel = (offset - kDmaStart) / 12;
        Dma& dma = dmas_[channel];
        const uint32_t field = (offset - kDmaStart) % 12;
        const auto merge32 = [&merge](uint32_t old, bool high)
        {
            const int shift = high ? 16 : 0;
            return (old & ~(0xFFFFu << shift)) | static_cast<uint32_t>(merge(static_cast<uint16_t>(old >> shift)))
                                                     << shift;
        };
        if (field < 4)
        {
            dma.source = merge32(dma.source, field == 2);
        }
        else if (field < 8)
        {
            dma.destination = merge32(dma.destination, field == 6);
        }
        else if (field == 8)
        {
            dma.control = merge32(dma.control, false);
        }
        else
        {
            WriteDmaControl(static_cast<int>(channel), merge32(dma.control, true));
        }
        return;
    }
    if (offset >= kDmaEnd && offset < kDmaFillEnd)
    {
        Dma& dma = dmas_[(offset - kDmaEnd) / 4];
        const int shift = offset % 4 == 0 ? 0 : 16;
        dma.fill = (dma.fill & ~(0xFFFFu << shift)) |
                   static_cast<uint32_t>(merge(static_cast<uint16_t>(dma.fill >> shift))) << shift;
        return;
    }
    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        Timer& t = timers_[static_cast<std::size_t>(timer)];
        if (offset % 4 == 0)
        {
            t.reload = merge(t.reload);
        }
        else
        {
            WriteTimerControl(timer, merge(t.control));
        }
        return;
    }
    if (offset >= kDivControl && offset < kDivResult)
    {
        Store16(offset, value, mask);
        Divide();
        return;
    }
    if (offset >= kSqrtControl && offset < kSqrtEnd)
    {
        if (offset < kSqrtResult || offset >= kSqrtParameter)
        {
            Store16(offset, value, mask);
            SquareRoot();
        }
        return;
    }

    switch (offset)
    {
    case kDispstat:
        dispstat_ = static_cast<uint16_t>(merge(dispstat_) & kDispstatWritable);
        ScheduleDisplay();
        return;
    case kIpcSync:
        ipcsync_ = static_cast<uint16_t>(merge(ipcsync_) & (kSyncOutput | kSyncIrqEnable));
        if ((value & mask & kSyncRequest) != 0)
        {
            arm7_->RequestSyncIrq();
        }
        return;
    case kIpcFifoControl:
        WriteFifoControl(static_cast<uint16_t>(value & mask));
        return;
    case kIme:
        ime_ = static_cast<uint16_t>(merge(ime_) & 1);
        UpdateIrq();
        return;
    case kIe:
    case kIe + 2:
        {
            const int shift = offset == kIe ? 0 : 16;
            ie_ = (ie_ & ~(0xFFFFu << shift)) | static_cast<uint32_t>(merge(static_cast<uint16_t>(ie_ >> shift)))
                                                    << shift;
            UpdateIrq();
            return;
        }
    case kIf:
    case kIf + 2:
        {
            const uint32_t acks = static_cast<uint32_t>(value & mask) << (offset == kIf ? 0 : 16);
            if ((acks & if_ & irq::kVblank) != 0)
            {
                counts_.vblank_acks++;
            }
            if_ &= ~acks;
            UpdateIrq();
            return;
        }
    case kWramControl - 1:
        Store16(offset, value, mask & 0x00FF);
        if ((mask & 0xFF00) != 0)
        {
            wram_control_ = static_cast<uint8_t>((value >> 8) & 3);
            arm7_->SetWramControl(wram_control_);
            arm9_->InvalidatePages();
        }
        return;
    case kGxStatus + 2:
        Store16(offset, value, mask & 0xC000);
        UpdateIrq();
        return;
    case kPostFlag:
        if ((mask & 0x00FF) != 0)
        {
            io_[kPostFlag] |= static_cast<uint8_t>(value & 1);
        }
        return;
    case kVcount:
    case kKeyInput:
    case kIme + 2:
    case kGxStatus:
        return;
    default:
        if (offset >= kDivResult && offset < kDivControl + 0x30)
        {
            return;
        }
        if (!IsStoredRegister(offset))
        {
            Unknown(kIo + offset, 2, true);
        }
        Store16(offset, value, mask);
        return;
    }
}

uint16_t Machine::Stored(uint32_t offset) const
{
    return static_cast<uint16_t>(io_[offset] | io_[offset + 1] << 8);
}

void Machine::Store16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const uint16_t merged = static_cast<uint16_t>((Stored(offset) & ~mask) | (value & mask));
    io_[offset] = static_cast<uint8_t>(merged);
    io_[offset + 1] = static_cast<uint8_t>(merged >> 8);
}

void Machine::Unknown(uint32_t address, int size, bool write)
{
    if (on_unknown_io)
    {
        on_unknown_io(address, size, write);
    }
}

void Machine::Poll()
{
    if (arm9_running_ && ++polls_ == kPollReads)
    {
        polled_ = true;
        arm9_->Stop();
    }
}

uint32_t Machine::Load(uint32_t address, int size)
{
    address &= ~static_cast<uint32_t>(size - 1);
    const uint8_t* page = Page(address & ~0xFFFu);
    if (page == nullptr)
    {
        return Read(address, size);
    }

    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= static_cast<uint32_t>(page[(address & 0xFFF) + static_cast<uint32_t>(i)]) << (8 * i);
    }
    return value;
}

void Machine::Store(uint32_t address, int size, uint32_t value)
{
    address &= ~static_cast<uint32_t>(size - 1);
    uint8_t* page = Page(address & ~0xFFFu);
    if (page == nullptr)
    {
        Write(address, size, value);
        return;
    }

    for (int i = 0; i < size; i++)
    {
        page[(address & 0xFFF) + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
    }
}

bool Machine::Swi(arm::Cpu& cpu, uint32_t comment)
{
    if (on_arm9_swi && on_arm9_swi(cpu, comment))
    {
        return true;
    }

    const bool thumb = (cpu.Cpsr() & arm::psr::kT) != 0;
    if (!thumb && comment >> 16 == kFaultSwi && (comment & 0xFFFF) < kFaults.size())
    {
        // The vector's mode holds the address after the instruction that faulted in LR.
        Fail(std::string("the ARM9 took ") + kFaults[comment & 0xFFFF] + " before " + Hex(cpu.Reg(14)));
        return true;
    }

    // In ARM state, the BIOS takes the number from bits 16 to 23 of the comment.
    const uint32_t number = thumb ? comment : (comment >> 16) & 0xFF;
    switch (number)
    {
    case kWaitByLoop:
        {
            const auto loops = static_cast<int32_t>(cpu.Reg(0));
            if (loops > 0)
            {
                wait_cycles_ += static_cast<uint64_t>(loops) * kWaitLoopCycles;
                cpu.Stop();
            }
            return true;
        }
    case kIntrWait:
        return false;
    case kVBlankIntrWait:
        cpu.SetReg(0, 1);
        cpu.SetReg(1, irq::kVblank);
        return false;
    case kHalt:
        cpu.Halt();
        return true;
    case kDiv:
        {
            const auto numerator = static_cast<int32_t>(cpu.Reg(0));
            const auto denominator = static_cast<int32_t>(cpu.Reg(1));
            if (denominator == 0 || (numerator == std::numeric_limits<int32_t>::min() && denominator == -1))
            {
                Fail("Div with " + Hex(cpu.Reg(0)) + " / " + Hex(cpu.Reg(1)));
                return true;
            }
            const int32_t quotient = numerator / denominator;
            cpu.SetReg(0, static_cast<uint32_t>(quotient));
            cpu.SetReg(1, static_cast<uint32_t>(numerator % denominator));
            cpu.SetReg(3, static_cast<uint32_t>(quotient < 0 ? -static_cast<int64_t>(quotient) : quotient));
            return true;
        }
    case kCpuSet:
    case kCpuFastSet:
        {
            const uint32_t control = cpu.Reg(2);
            const bool fill = (control & (1u << 24)) != 0;
            const bool fast = number == kCpuFastSet;
            const int size = fast || (control & (1u << 26)) != 0 ? 4 : 2;
            uint32_t count = control & 0x1FFFFF;
            if (fast)
            {
                count = (count + 7) & ~7u;
            }
            uint32_t source = cpu.Reg(0) & ~static_cast<uint32_t>(size - 1);
            uint32_t destination = cpu.Reg(1) & ~static_cast<uint32_t>(size - 1);
            const uint32_t first = cpu.Load(source, size);
            for (uint32_t i = 0; i < count; i++)
            {
                cpu.Store(destination, size, fill ? first : cpu.Load(source, size));
                destination += static_cast<uint32_t>(size);
                source += fill ? 0 : static_cast<uint32_t>(size);
            }
            return true;
        }
    case kSqrt:
        {
            const uint32_t value = cpu.Reg(0);
            uint32_t root = 0;
            for (uint32_t bit = 1u << 15; bit != 0; bit >>= 1)
            {
                const uint32_t trial = root | bit;
                if (static_cast<uint64_t>(trial) * trial <= value)
                {
                    root = trial;
                }
            }
            cpu.SetReg(0, root);
            return true;
        }
    case kGetCrc16:
        {
            // GBATEK's CRC-16, a byte at a time. r3 returns the last halfword read.
            constexpr std::array<uint16_t, 8> kValues = {0xC0C1, 0xC181, 0xC301, 0xC601,
                                                         0xCC01, 0xD801, 0xF001, 0xA001};
            uint32_t crc = cpu.Reg(0) & 0xFFFF;
            const uint32_t start = cpu.Reg(1) & ~1u;
            const uint32_t length = cpu.Reg(2) & ~1u;
            uint32_t last = 0;
            for (uint32_t at = start; at < start + length; at += 2)
            {
                last = cpu.Load(at, 2);
                for (const uint32_t byte : {last & 0xFF, last >> 8})
                {
                    crc ^= byte;
                    for (std::size_t j = 0; j < kValues.size(); j++)
                    {
                        const bool carry = (crc & 1) != 0;
                        crc >>= 1;
                        if (carry)
                        {
                            crc ^= static_cast<uint32_t>(kValues[j]) << (7 - j);
                        }
                    }
                }
            }
            cpu.SetReg(0, crc & 0xFFFF);
            cpu.SetReg(3, last);
            return true;
        }
    case kIsDebugger:
        cpu.SetReg(0, 0);
        return true;
    case kBitUnPack:
        {
            // r2 points at the source's length in bytes (16 bits), the source's and the destination's widths in bits,
            // and an offset added to each unit, to zeros too when bit 31 is set.
            const uint32_t info = cpu.Reg(2);
            const uint32_t length = cpu.Load(info, 2);
            const uint32_t source_width = cpu.Load(info + 2, 1);
            const uint32_t destination_width = cpu.Load(info + 3, 1);
            const uint32_t offset_word = cpu.Load(info + 4, 4);
            if (source_width == 0 || source_width > 8 || destination_width == 0 || destination_width > 32)
            {
                Fail("BitUnPack with widths of " + std::to_string(source_width) + " and " +
                     std::to_string(destination_width) + " bits");
                return true;
            }
            uint32_t source = cpu.Reg(0);
            uint32_t destination = cpu.Reg(1);
            uint32_t word = 0;
            uint32_t filled = 0;
            for (uint32_t i = 0; i < length; i++)
            {
                const uint32_t byte = cpu.Load(source + i, 1);
                for (uint32_t bit = 0; bit < 8; bit += source_width)
                {
                    uint32_t unit = (byte >> bit) & ((1u << source_width) - 1);
                    if (unit != 0 || (offset_word & 0x80000000) != 0)
                    {
                        unit += offset_word & 0x7FFFFFFF;
                    }
                    word |= (destination_width == 32 ? unit : unit & ((1u << destination_width) - 1)) << filled;
                    filled += destination_width;
                    if (filled >= 32)
                    {
                        cpu.Store(destination, 4, word);
                        destination += 4;
                        word = 0;
                        filled = 0;
                    }
                }
            }
            return true;
        }
    case kLz77Uncompress:
    case kRlUncompress:
        {
            uint32_t source = cpu.Reg(0);
            uint32_t destination = cpu.Reg(1);
            const uint32_t size = cpu.Load(source, 4) >> 8;
            source += 4;
            const uint32_t end = destination + size;
            while (destination < end)
            {
                const uint32_t flags = cpu.Load(source++, 1);
                if (number == kRlUncompress)
                {
                    // Bit 7 set: the next byte, (flags & 0x7F) + 3 times. Clear: flags + 1 bytes as they are.
                    const bool run = (flags & 0x80) != 0;
                    const uint32_t count = run ? (flags & 0x7F) + 3 : flags + 1;
                    const uint32_t byte = run ? cpu.Load(source++, 1) : 0;
                    for (uint32_t i = 0; i < count && destination < end; i++)
                    {
                        cpu.Store(destination++, 1, run ? byte : cpu.Load(source++, 1));
                    }
                    continue;
                }

                // Each flag bit from bit 7: clear, a byte as it is; set, 3 to 18 bytes copied from 1 to 4096 bytes
                // back.
                for (int bit = 7; bit >= 0 && destination < end; bit--)
                {
                    if ((flags & (1u << bit)) == 0)
                    {
                        cpu.Store(destination++, 1, cpu.Load(source++, 1));
                        continue;
                    }
                    const uint32_t high = cpu.Load(source++, 1);
                    const uint32_t low = cpu.Load(source++, 1);
                    const uint32_t count = (high >> 4) + 3;
                    const uint32_t distance = ((high & 0xF) << 8 | low) + 1;
                    for (uint32_t i = 0; i < count && destination < end; i++)
                    {
                        cpu.Store(destination, 1, cpu.Load(destination - distance, 1));
                        destination++;
                    }
                }
            }
            return true;
        }
    case kDiff8bitUnFilter:
    case kDiff16bitUnFilter:
        {
            const int size = number == kDiff8bitUnFilter ? 1 : 2;
            uint32_t source = cpu.Reg(0);
            uint32_t destination = cpu.Reg(1);
            const uint32_t length = cpu.Load(source, 4) >> 8;
            source += 4;
            uint32_t sum = 0;
            for (uint32_t i = 0; i < length; i += static_cast<uint32_t>(size))
            {
                sum += cpu.Load(source + i, size);
                cpu.Store(destination + i, size, sum);
            }
            return true;
        }
    default:
        Fail("ARM9 SWI " + Hex(number) + ", which the BIOS doesn't model, at " + Hex(cpu.Pc()));
        return true;
    }
}

void Machine::Fail(std::string message)
{
    if (error_.empty())
    {
        error_ = std::move(message);
    }
    arm9_->Stop();
}

void Machine::Pass(uint64_t until, bool to_arm7_change, std::vector<SoundOutput>& out)
{
    arm7_changed_ = false;
    while (Clock() < until && error_.empty() && !(to_arm7_change && arm7_changed_))
    {
        arm7_->Run(std::min(until, NextEvent()), out);
        Events();
    }
}

uint64_t Machine::NextEvent() const
{
    uint64_t next = next_display_;
    for (int timer = 0; timer < 4; timer++)
    {
        if (Counting(timer))
        {
            next = std::min(next, TimerOverflow(timer));
        }
    }
    if (card_.busy && !card_.ready)
    {
        next = std::min(next, card_.ready_at);
    }
    return next;
}

void Machine::Events()
{
    const uint64_t now = Clock();
    for (;;)
    {
        const uint64_t next = NextEvent();
        if (next > now)
        {
            break;
        }

        if (next_display_ == next)
        {
            DisplayEvent(next);
        }
        for (int timer = 0; timer < 4; timer++)
        {
            if (Counting(timer) && TimerOverflow(timer) == next)
            {
                OverflowTimer(timer, next);
            }
        }
        if (card_.busy && !card_.ready && card_.ready_at == next)
        {
            CardReady();
        }
    }
}

void Machine::Request(uint32_t irq)
{
    if_ |= irq;
    UpdateIrq();
}

void Machine::UpdateIrq()
{
    // The GX FIFO's IRQ follows its condition: the FIFO is always empty.
    if ((Stored(kGxStatus + 2) & 0xC000) != 0)
    {
        if_ |= kGxFifoIrq;
    }
    irq_line_ = (ime_ & 1) != 0 && (ie_ & if_) != 0;
    arm9_->SetIrq(irq_line_);
}

uint64_t Machine::Prescale(int timer) const
{
    return kPrescales[timers_[static_cast<std::size_t>(timer)].control & 3];
}

bool Machine::Counting(int timer) const
{
    const uint16_t control = timers_[static_cast<std::size_t>(timer)].control;
    return (control & kTimerStartBit) != 0 && (timer == 0 || (control & kTimerCascade) == 0);
}

uint16_t Machine::TimerCounter(int timer) const
{
    const Timer& t = timers_[static_cast<std::size_t>(timer)];
    if (!Counting(timer))
    {
        return t.counter;
    }
    return static_cast<uint16_t>(t.counter + (Clock() - t.base) / Prescale(timer));
}

uint64_t Machine::TimerOverflow(int timer) const
{
    const Timer& t = timers_[static_cast<std::size_t>(timer)];
    return t.base + (0x10000 - static_cast<uint64_t>(t.counter)) * Prescale(timer);
}

void Machine::RebaseTimer(int timer)
{
    if (!Counting(timer))
    {
        return;
    }

    Timer& t = timers_[static_cast<std::size_t>(timer)];
    const uint64_t counts = (Clock() - t.base) / Prescale(timer);
    t.counter = static_cast<uint16_t>(t.counter + counts);
    t.base += counts * Prescale(timer);
}

void Machine::OverflowTimer(int timer, uint64_t at)
{
    Timer& t = timers_[static_cast<std::size_t>(timer)];
    t.counter = t.reload;
    t.base = at;
    if ((t.control & kTimerIrq) != 0)
    {
        Request(irq::kTimer0 << timer);
    }

    if (timer < 3)
    {
        Timer& next = timers_[static_cast<std::size_t>(timer) + 1];
        if ((next.control & (kTimerStartBit | kTimerCascade)) == (kTimerStartBit | kTimerCascade))
        {
            if (next.counter == 0xFFFF)
            {
                OverflowTimer(timer + 1, at);
            }
            else
            {
                next.counter++;
            }
        }
    }
}

void Machine::WriteTimerControl(int timer, uint16_t control)
{
    RebaseTimer(timer);
    Timer& t = timers_[static_cast<std::size_t>(timer)];
    const uint16_t old = t.control;
    t.control = static_cast<uint16_t>(control & kTimerWritable);
    if ((old & kTimerStartBit) == 0 && (control & kTimerStartBit) != 0)
    {
        t.counter = t.reload;
        t.base = Clock();
    }
    else if ((old & 3) != (control & 3))
    {
        t.base = Clock();
    }
}

uint64_t Machine::Line() const
{
    return Clock() / kLineCycles % kFrameLines;
}

void Machine::ScheduleDisplay()
{
    ScheduleDisplayAfter(Clock());
}

void Machine::ScheduleDisplayAfter(uint64_t after)
{
    // The next start of a line, or of a line's horizontal blank, after `after`. The vertical blank's start is always an
    // event: it ends a display capture and starts DMA.
    const auto next = [after](uint64_t line, uint64_t cycle)
    {
        const uint64_t at = after / kFrameCycles * kFrameCycles + line * kLineCycles + cycle;
        return at > after ? at : at + kFrameCycles;
    };

    next_display_ = next(kVblankLine, 0);
    if ((dispstat_ & kVcountIrq) != 0)
    {
        const uint64_t setting = VcountSetting(dispstat_);
        if (setting < kFrameLines)
        {
            next_display_ = std::min(next_display_, next(setting, 0));
        }
    }
    const bool hblank_dma =
        std::any_of(dmas_.begin(), dmas_.end(), [](const Dma& dma)
                    { return (dma.control & kDmaEnable) != 0 && DmaTimingOf(dma.control) == kDmaHblank; });
    if ((dispstat_ & kHblankIrq) != 0 || hblank_dma)
    {
        const uint64_t at = after / kLineCycles * kLineCycles + kHblankCycle;
        next_display_ = std::min(next_display_, at > after ? at : at + kLineCycles);
    }
}

void Machine::DisplayEvent(uint64_t at)
{
    const uint64_t line = at / kLineCycles % kFrameLines;
    const uint64_t cycle = at % kLineCycles;
    if (cycle == 0 && line == kVblankLine)
    {
        if ((dispstat_ & kVblankIrq) != 0)
        {
            Request(irq::kVblank);
        }
        io_[kCaptureControl + 3] &= static_cast<uint8_t>(~kCaptureBusy);
        for (int channel = 0; channel < 4; channel++)
        {
            const uint32_t control = dmas_[static_cast<std::size_t>(channel)].control;
            if ((control & kDmaEnable) != 0 && DmaTimingOf(control) == kDmaVblank)
            {
                RunDma(channel);
            }
        }
    }
    if (cycle == 0 && (dispstat_ & kVcountIrq) != 0 && line == VcountSetting(dispstat_))
    {
        Request(irq::kVcount);
    }
    if (cycle == kHblankCycle)
    {
        if ((dispstat_ & kHblankIrq) != 0)
        {
            Request(irq::kHblank);
        }
        // DMA starts on the horizontal blanks of the lines that the screens show.
        for (int channel = 0; channel < 4 && line < kVblankLine; channel++)
        {
            const uint32_t control = dmas_[static_cast<std::size_t>(channel)].control;
            if ((control & kDmaEnable) != 0 && DmaTimingOf(control) == kDmaHblank)
            {
                RunDma(channel);
            }
        }
    }
    ScheduleDisplayAfter(at);
}

void Machine::WriteDmaControl(int channel, uint32_t control)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const bool starts = (dma.control & kDmaEnable) == 0 && (control & kDmaEnable) != 0;
    dma.control = control;
    if (!starts)
    {
        return;
    }

    dma.next_source = dma.source;
    dma.next_destination = dma.destination;
    dma.left = DmaCount(control);
    switch (DmaTimingOf(control))
    {
    case kDmaImmediate:
    case kDmaGxFifo:
        // The 3D engine takes the commands at once.
        RunDma(channel);
        break;
    case kDmaCard:
        RunCardDmas();
        break;
    case kDmaHblank:
        ScheduleDisplay();
        break;
    default:
        // The vertical blank's DMA waits for its event. The display's DMA never starts.
        break;
    }
}

void Machine::RunDma(int channel)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const int size = (dma.control & kDmaWords) != 0 ? 4 : 2;
    const auto step = [size](uint32_t mode)
    {
        return mode == 1 ? -size : mode == 2 ? 0 : size;
    };
    const int destination_step = step((dma.control >> 21) & 3);
    const int source_step = step((dma.control >> 23) & 3);
    uint32_t source = dma.next_source & ~static_cast<uint32_t>(size - 1);
    uint32_t destination = dma.next_destination & ~static_cast<uint32_t>(size - 1);
    for (uint32_t i = 0; i < dma.left; i++)
    {
        Store(destination, size, Load(source, size));
        source += static_cast<uint32_t>(source_step);
        destination += static_cast<uint32_t>(destination_step);
    }
    dma.next_source = source;
    dma.next_destination = destination;
    dma.left = 0;
    EndDmaBlock(channel);
}

void Machine::EndDmaBlock(int channel)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const uint32_t timing = DmaTimingOf(dma.control);
    if ((dma.control & kDmaRepeat) != 0 && timing != kDmaImmediate && timing != kDmaGxFifo)
    {
        dma.left = DmaCount(dma.control);
        if (((dma.control >> 21) & 3) == 3)
        {
            dma.next_destination = dma.destination;
        }
    }
    else
    {
        dma.control &= ~kDmaEnable;
    }
    if ((dma.control & kDmaIrq) != 0)
    {
        Request(irq::kDma0 << channel);
    }
}

void Machine::RunCardDmas()
{
    for (int channel = 0; channel < 4; channel++)
    {
        Dma& dma = dmas_[static_cast<std::size_t>(channel)];
        const int size = (dma.control & kDmaWords) != 0 ? 4 : 2;
        while ((dma.control & kDmaEnable) != 0 && DmaTimingOf(dma.control) == kDmaCard && card_.ready &&
               card_.position < card_.data.size())
        {
            // Each word that the card gives starts a unit of the transfer.
            const int destination_step = ((dma.control >> 21) & 3) == 1   ? -size
                                         : ((dma.control >> 21) & 3) == 2 ? 0
                                                                          : size;
            Store(dma.next_destination & ~static_cast<uint32_t>(size - 1), size, Load(dma.next_source, size));
            dma.next_destination += static_cast<uint32_t>(destination_step);
            if (--dma.left == 0)
            {
                EndDmaBlock(channel);
            }
        }
    }
}

uint16_t Machine::FifoControl() const
{
    uint16_t value = fifo_control_;
    value |= arm7_->WordsToArm7() == 0 ? kSendEmpty : 0;
    value |= arm7_->WordsToArm7() == kFifoWords ? kSendFull : 0;
    value |= arm7_->WordsToArm9() == 0 ? kReceiveEmpty : 0;
    value |= arm7_->WordsToArm9() == kFifoWords ? kReceiveFull : 0;
    value |= fifo_error_ ? kFifoError : 0;
    return value;
}

void Machine::WriteFifoControl(uint16_t value)
{
    if ((value & kSendClear) != 0)
    {
        arm7_->ClearToArm7();
    }
    if ((value & kFifoError) != 0)
    {
        fifo_error_ = false;
    }

    const uint16_t old = fifo_control_;
    fifo_control_ = static_cast<uint16_t>(value & kFifoWritable);
    if ((old & kSendEmptyIrq) == 0 && (fifo_control_ & kSendEmptyIrq) != 0 && arm7_->WordsToArm7() == 0)
    {
        Request(irq::kIpcSendEmpty);
    }
    if ((old & kReceiveIrq) == 0 && (fifo_control_ & kReceiveIrq) != 0 && arm7_->WordsToArm9() != 0)
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

void Machine::Divide()
{
    // DIVCNT's mode: 32 bits by 32 (0), 64 by 32 (1 and 3) or 64 by 64 (2).
    const uint16_t control = Stored(kDivControl);
    const auto numerator64 = static_cast<int64_t>(xsflib::LoadLe64(&io_[kDivNumerator]));
    const auto denominator64 = static_cast<int64_t>(xsflib::LoadLe64(&io_[kDivDenominator]));
    const uint32_t mode = control & 3;
    const int64_t numerator = mode == 0 ? static_cast<int32_t>(numerator64) : numerator64;
    const int64_t denominator = mode == 2 ? denominator64 : static_cast<int32_t>(denominator64);

    // A denominator of 0 gives the numerator as the remainder and -1 or 1 as the result, with the high word of the
    // result inverted in mode 0. The flag in bit 14 looks at all 64 bits of the denominator.
    int64_t result = 0;
    int64_t remainder = 0;
    if (denominator == 0)
    {
        remainder = numerator;
        result = numerator >= 0 ? -1 : 1;
        if (mode == 0)
        {
            result = static_cast<int64_t>(static_cast<uint64_t>(result) ^ 0xFFFFFFFF00000000);
        }
    }
    else if (numerator == std::numeric_limits<int64_t>::min() && denominator == -1)
    {
        result = numerator;
    }
    else
    {
        result = numerator / denominator;
        remainder = numerator % denominator;
    }
    xsflib::StoreLe64(&io_[kDivResult], static_cast<uint64_t>(result));
    xsflib::StoreLe64(&io_[kDivRemainder], static_cast<uint64_t>(remainder));
    const uint16_t flags = static_cast<uint16_t>((control & 3) | (denominator64 == 0 ? 1u << 14 : 0u));
    io_[kDivControl] = static_cast<uint8_t>(flags);
    io_[kDivControl + 1] = static_cast<uint8_t>(flags >> 8);
}

void Machine::SquareRoot()
{
    // SQRTCNT's mode: a 32-bit parameter (0) or a 64-bit one (1).
    const uint64_t parameter = (Stored(kSqrtControl) & 1) != 0 ? xsflib::LoadLe64(&io_[kSqrtParameter])
                                                               : xsflib::LoadLe32(&io_[kSqrtParameter]);
    uint64_t root = 0;
    for (uint64_t bit = uint64_t{1} << 31; bit != 0; bit >>= 1)
    {
        const uint64_t trial = root | bit;
        if (trial * trial <= parameter)
        {
            root = trial;
        }
    }
    xsflib::StoreLe32(&io_[kSqrtResult], static_cast<uint32_t>(root));
}

} // namespace ntdlib::ntr
