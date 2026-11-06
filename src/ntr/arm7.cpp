// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/arm7.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
#include "spi_devices.h"

namespace ntdlib::ntr
{

namespace
{

constexpr uint32_t kBiosSize = 0x4000;
constexpr uint32_t kMainSize = 0x400000;
constexpr uint32_t kSharedWramSize = 0x8000;
constexpr uint32_t kWramSize = 0x10000;
constexpr uint32_t kVramSize = 0x40000;

constexpr uint32_t kIo = 0x04000000;
constexpr uint32_t kIpcFifoReceive = 0x04100000;
constexpr uint32_t kIpcFifoSend = 0x04000188;
constexpr uint32_t kCardData = 0x04100010;
constexpr std::size_t kFifoWords = 16;

// Where the firmware leaves the cartridge's header, and the lock that the ARM9's OS_InitLock takes before the ARM7's
// OS_InitLock goes on: the lock word's owner, the ARM9's initial ID.
constexpr uint32_t kHeaderCopy = 0x027FFE00;
constexpr uint32_t kHeaderCopySize = 0x170;
constexpr uint32_t kInitLockOwner = 0x027FFFF4;
constexpr uint16_t kArm9InitLockId = 0x7F;

// Main memory's last page holds the area that the firmware and both CPUs share at start-up. The CPU reaches it through
// Read and Write. The system's ARM9 can then answer the ARM7 there.
constexpr uint32_t kBootArea = kMainSize - 0x1000;

// The handshakes that a DSi-enhanced cartridge's binaries make at start-up. The system's ARM9 answers the ARM7's writes
// (Arm7System::AnswerArm9).
//
// In the first, each CPU increments a 16-bit counter, waits for the other's to change, and increments its counter
// again: the ARM9's, then the ARM7's.
constexpr uint32_t kArm9StartCounter = 0x027FFC24;
constexpr uint32_t kArm7StartCounter = 0x027FFC26;

// The second takes 4 bytes. One CPU writes 0x80 and a count in byte 0, waits until the other has copied it into byte 1,
// and writes the next count, until the other sets byte 2. It then writes 1 in byte 3. The other CPU copies each new
// value of byte 0 and adds it up. Once the sum reaches 768, it writes 0 in byte 3 and 1 in byte 2, and waits for byte
// 3. The ARM7 first copies, then writes, once each. The system's ARM9 starts its count and its sum at 0 and takes each
// side once. The ARM9's code does the same. The same bytes hold the lock word of OS_InitLock. The CPUs write the lock
// word 32 bits at a time, and the system's ARM9 answers only the writes of single bytes.
constexpr uint32_t kHandshake = 0x027FFFF0;
constexpr uint32_t kHandshakeSum = 768;

// The IPC's words of the tags whose handlers each CPU has set: the ARM9's, then the ARM7's. The ARM7's libraries wait
// for the ARM9's handlers of their tags. The system's ARM9 has a handler for every tag.
constexpr uint32_t kArm9ReadyTags = 0x027FFF88;

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

// The BIOS's SWIs, by number.
enum Swi : uint32_t
{
    kWaitByLoop = 0x03,
    kIntrWait = 0x04,
    kVBlankIntrWait = 0x05,
    kHalt = 0x06,
    kSoundBiasSwi = 0x08,
    kDiv = 0x09,
    kCpuSet = 0x0B,
    kCpuFastSet = 0x0C,
    kSqrt = 0x0D,
    kGetCrc16 = 0x0E,
    kIsDebugger = 0x0F,
    kGetPitchTable = 0x1B,
    kGetVolumeTable = 0x1C,
    kCustomHalt = 0x1F,
};

// The I/O registers' offsets from 0x04000000.
enum Io : uint32_t
{
    kDispstat = 0x004,
    kVcount = 0x006,
    kDmaStart = 0x0B0,
    kDmaEnd = 0x0E0,
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
    kIme = 0x208,
    kIe = 0x210,
    kIf = 0x214,
    kMemoryStatus = 0x240,
    kPostFlag = 0x300,
    kPowerControl = 0x304,
    kBiosProtection = 0x308,
    kIoEnd = 0x400,
};

constexpr uint16_t kKeysReleased = 0x03FF;
constexpr uint16_t kExtendedKeysIdle = 0x007F; // X and Y released, the pen up, the hinge open

// DISPSTAT's bits.
constexpr uint16_t kVblankFlag = 1u << 0;
constexpr uint16_t kHblankFlag = 1u << 1;
constexpr uint16_t kVcountFlag = 1u << 2;
constexpr uint16_t kVblankIrq = 1u << 3;
constexpr uint16_t kHblankIrq = 1u << 4;
constexpr uint16_t kVcountIrq = 1u << 5;
constexpr uint16_t kDispstatWritable = 0xFFB8;

// The horizontal blank starts at dot 256.
constexpr uint64_t kHblankCycle = 256 * 6;

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

// SPICNT's bits.
constexpr uint16_t kSpiHold = 1u << 11;
constexpr uint16_t kSpiIrq = 1u << 14;
constexpr uint16_t kSpiEnable = 1u << 15;

// HALTCNT's modes.
constexpr uint8_t kHaltMode = 0x80;
constexpr uint8_t kHaltModeMask = 0xC0;

// DMA control bits.
constexpr uint32_t kDmaEnable = 1u << 31;
constexpr uint32_t kDmaIrq = 1u << 30;
constexpr uint32_t kDmaWords = 1u << 26;

// The line of DISPSTAT's V-count match: bits 8 to 15, with bit 8 of the line in bit 7.
uint64_t VcountSetting(uint16_t dispstat)
{
    return static_cast<uint64_t>(dispstat >> 8) | static_cast<uint64_t>(dispstat & 0x80) << 1;
}

bool IsCardRegister(uint32_t address)
{
    return (address >= kIo + kCardStart && address < kIo + kCardEnd - 4) || address == kCardData;
}

std::string Hex(uint32_t value)
{
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "0x%08X", value);
    return text.data();
}

} // namespace

Arm7System::Arm7System(const Rom& rom) : Arm7System(rom, nullptr)
{
}

Arm7System::Arm7System(const Rom& rom, Arm9Link& link) : Arm7System(rom, &link)
{
}

Arm7System::Arm7System(const Rom& rom, Arm9Link* link)
    : bios_(kBiosSize),
      main_(kMainSize),
      shared_wram_(kSharedWramSize),
      wram_(kWramSize),
      vram_(kVramSize),
      cpu_(std::make_unique<arm::Cpu>(*this, arm::Architecture::kArmV4T)),
      sound_(main_),
      link_(link),
      spi_(link == nullptr ? nullptr : std::make_unique<SpiDevices>())
{
    for (std::size_t i = 0; i < kBios.size(); i++)
    {
        for (int byte = 0; byte < 4; byte++)
        {
            bios_[4 * i + static_cast<std::size_t>(byte)] = static_cast<uint8_t>(kBios[i] >> (8 * byte));
        }
    }

    const Binary& arm7 = rom.Header().arm7;
    const std::span<const uint8_t> binary = rom.Arm7();
    for (uint32_t i = 0; i < binary.size(); i++)
    {
        const uint32_t address = arm7.ram_address + i;
        if (address < arm7.ram_address || (Page(address & ~0xFFFu) == nullptr && address >> 24 != 0x02))
        {
            throw std::runtime_error("the ARM7 binary doesn't fit in the memory at " + Hex(arm7.ram_address));
        }
        Store(address, 1, binary[i]);
    }

    const std::span<const uint8_t> image = rom.Image();
    for (uint32_t i = 0; i < kHeaderCopySize && i < image.size(); i++)
    {
        Store(kHeaderCopy + i, 1, image[i]);
    }
    if (link_ == nullptr)
    {
        Store(kInitLockOwner, 2, kArm9InitLockId);
        Store(kArm9ReadyTags, 4, 0xFFFFFFFF);
    }

    io_[kPostFlag] = 1;
    io_[kPowerControl] = 1;
    sound_.Write(kSoundBias, 2, kSoundBiasMiddle);

    cpu_->SetSwiHandler([this](arm::Cpu& cpu, uint32_t comment) { return Swi(cpu, comment); });
    cpu_->SetStopAddress(kUndefinedVector);
    cpu_->SetCpsr(static_cast<uint32_t>(arm::Mode::kIrq) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kIrqStack);
    cpu_->SetCpsr(static_cast<uint32_t>(arm::Mode::kSupervisor) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kSupervisorStack);
    cpu_->SetCpsr(static_cast<uint32_t>(arm::Mode::kSystem) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kSystemStack);
    cpu_->Jump(arm7.entry, arm::Mode::kSystem);

    ScheduleDisplay();
}

Arm7System::~Arm7System() = default;

arm::Cpu& Arm7System::Cpu()
{
    return *cpu_;
}

SoundUnit& Arm7System::Sound()
{
    return sound_;
}

std::span<uint8_t> Arm7System::MainMemory()
{
    return main_;
}

uint64_t Arm7System::Clock() const
{
    return clock_;
}

bool Arm7System::Halted() const
{
    return halted_;
}

void Arm7System::Run(uint64_t until, std::vector<SoundOutput>& out)
{
    out_ = &out;
    while (clock_ < until && error_.empty())
    {
        if (halted_)
        {
            // The ARM7 leaves a halt once an enabled interrupt is requested, whatever IME holds.
            if ((ie_ & if_) != 0)
            {
                halted_ = false;
                continue;
            }

            Advance(std::min(NextEvent(), until));
            continue;
        }

        // A burst lasts from the CPU's last halt. Beside an ARM9, a burst that a link event ends goes on in the next
        // run. The event can come in the instruction that ends the burst.
        const uint64_t burst = link_ == nullptr ? kLongBurst : kLinkedBurst;
        const uint64_t start = cpu_->Cycles();
        const arm::StopReason reason = cpu_->Run(burst - burst_cycles_);
        burst_cycles_ += cpu_->Cycles() - start;
        if (reason == arm::StopReason::kStopAddress)
        {
            // The undefined mode's LR holds the address after the instruction.
            Fail("an undefined instruction before " + Hex(cpu_->Reg(14)));
        }
        else if (reason == arm::StopReason::kHalted)
        {
            halted_ = true;
        }
        if (halted_ && (ie_ & if_) != 0)
        {
            // A halt with an enabled interrupt requested ends at once, and the burst goes on: a loop of such halts
            // takes the time of its instructions.
            halted_ = false;
        }
        if (halted_)
        {
            burst_cycles_ = 0;
        }
        else if (burst_cycles_ >= burst)
        {
            Advance(clock_ + burst_cycles_);
            burst_cycles_ = 0;
        }

        if (link_event_)
        {
            link_event_ = false;
            break;
        }
    }

    SyncSound();
    out_ = nullptr;
    if (!error_.empty())
    {
        throw std::runtime_error(error_);
    }
}

void Arm7System::SendToArm7(uint32_t word)
{
    if (to_arm7_.size() == kFifoWords)
    {
        return;
    }

    const bool was_empty = to_arm7_.empty();
    to_arm7_.push_back(word);
    if (was_empty && (fifo_control_ & kReceiveIrq) != 0)
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

std::vector<uint32_t> Arm7System::TakeFromArm7()
{
    std::vector<uint32_t> words(to_arm9_.begin(), to_arm9_.end());
    to_arm9_.clear();
    if (!words.empty() && (fifo_control_ & kSendEmptyIrq) != 0)
    {
        Request(irq::kIpcSendEmpty);
    }
    return words;
}

uint32_t Arm7System::ReceiveFromArm7(bool take)
{
    const uint32_t word = to_arm9_.front();
    if (take)
    {
        to_arm9_.pop_front();
        if (to_arm9_.empty() && (fifo_control_ & kSendEmptyIrq) != 0)
        {
            Request(irq::kIpcSendEmpty);
        }
    }
    return word;
}

std::size_t Arm7System::WordsToArm9() const
{
    return to_arm9_.size();
}

std::size_t Arm7System::WordsToArm7() const
{
    return to_arm7_.size();
}

void Arm7System::ClearToArm7()
{
    to_arm7_.clear();
}

void Arm7System::RequestIrq(uint32_t irq)
{
    Request(irq);
}

void Arm7System::RequestSyncIrq()
{
    if ((ipcsync_ & kSyncIrqEnable) != 0)
    {
        Request(irq::kIpcSync);
    }
}

uint16_t Arm7System::SyncOutput() const
{
    return static_cast<uint16_t>((ipcsync_ & kSyncOutput) >> 8);
}

void Arm7System::SetWramControl(uint8_t value)
{
    wram_control_ = value & 3;
    cpu_->InvalidatePages();
}

std::span<uint8_t> Arm7System::SharedWram()
{
    return shared_wram_;
}

void Arm7System::LinkEvent()
{
    link_event_ = true;
    cpu_->Stop();
}

uint8_t* Arm7System::Page(uint32_t page)
{
    switch (page >> 24)
    {
    case 0x00:
        return page < kBiosSize ? bios_.data() + page : nullptr;
    case 0x02:
        if ((page & (kMainSize - 1)) == kBootArea && link_ == nullptr)
        {
            return nullptr;
        }
        return main_.data() + (page & (kMainSize - 1));
    case 0x03:
        // The ARM7 reaches the shared work memory up to 0x037FFFFF, and its work memory from 0x03800000. WRAMCNT
        // gives it all of the shared memory (3), one half (1 the first, 2 the second) or none. With none, the ARM7's
        // work memory takes the place.
        if (page < 0x03800000 && wram_control_ != 0)
        {
            const uint32_t half = kSharedWramSize / 2;
            switch (wram_control_)
            {
            case 1:
                return shared_wram_.data() + (page & (half - 1));
            case 2:
                return shared_wram_.data() + half + (page & (half - 1));
            default:
                return shared_wram_.data() + (page & (kSharedWramSize - 1));
            }
        }
        return wram_.data() + (page & (kWramSize - 1));
    case 0x06:
        return vram_.data() + (page & (kVramSize - 1));
    default:
        return nullptr;
    }
}

uint32_t Arm7System::Read(uint32_t address, int size)
{
    if (address >> 24 == 0x02)
    {
        uint32_t value = 0;
        for (int i = 0; i < size; i++)
        {
            value |= static_cast<uint32_t>(main_[(address & (kMainSize - 1)) + static_cast<uint32_t>(i)]) << (8 * i);
        }
        return value;
    }
    if (address >> 24 == 0x04)
    {
        return ReadIo(address, size);
    }

    Unknown(address, size, false);
    return 0;
}

void Arm7System::Write(uint32_t address, int size, uint32_t value)
{
    if (address >> 24 == 0x02)
    {
        const uint32_t offset = address & (kMainSize - 1);
        for (int i = 0; i < size; i++)
        {
            main_[offset + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
        }

        if (link_ == nullptr)
        {
            AnswerArm9(offset, size);
        }
        return;
    }
    if (address >> 24 == 0x04)
    {
        WriteIo(address, size, value);
        return;
    }

    Unknown(address, size, true);
}

void Arm7System::AnswerArm9(uint32_t offset, int size)
{
    const uint32_t arm7_counter = kArm7StartCounter & (kMainSize - 1);
    if (offset < arm7_counter + 2 && arm7_counter < offset + static_cast<uint32_t>(size))
    {
        const auto count = static_cast<uint16_t>(main_[arm7_counter] | main_[arm7_counter + 1] << 8);
        if (count != arm7_start_counter_)
        {
            arm7_start_counter_ = count;
            const uint32_t arm9_counter = kArm9StartCounter & (kMainSize - 1);
            const auto answer = static_cast<uint16_t>((main_[arm9_counter] | main_[arm9_counter + 1] << 8) + 1);
            main_[arm9_counter] = static_cast<uint8_t>(answer);
            main_[arm9_counter + 1] = static_cast<uint8_t>(answer >> 8);
        }
        return;
    }

    const uint32_t bytes = kHandshake & (kMainSize - 1);
    if (size != 1 || offset < bytes || offset > bytes + 2)
    {
        return;
    }
    uint8_t* handshake = &main_[bytes];
    switch (offset - bytes)
    {
    case 0:
        // The ARM7 writes: the ARM9 copies.
        if (handshake[0] != handshake[1] && handshake[2] == 0)
        {
            handshake[1] = handshake[0];
            arm9_handshake_sum_ += handshake[1];
            if (arm9_handshake_sum_ >= kHandshakeSum)
            {
                handshake[3] = 0;
                handshake[2] = 1;
            }
        }
        break;
    case 1:
        // The ARM7 copies: the ARM9 writes the next count.
        if (handshake[0] == handshake[1] && handshake[2] == 0)
        {
            handshake[0] = static_cast<uint8_t>(0x80 | (arm9_handshake_count_++ & 0xF));
        }
        break;
    default:
        // The ARM7's sum is complete.
        if (handshake[2] != 0)
        {
            handshake[3] = 1;
        }
        break;
    }
}

uint32_t Arm7System::ReadIo(uint32_t address, int size)
{
    if (link_ != nullptr && IsCardRegister(address))
    {
        return link_->ReadCard(address, size);
    }

    if (address >= kSoundRegisters && address < kSoundRegistersEnd)
    {
        SyncSound();
        const uint32_t value = sound_.Read(address, size);
        if (on_sound_access)
        {
            on_sound_access(address, size, value, false);
        }
        return value;
    }

    if (address == kIpcFifoReceive)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return to_arm7_.empty() ? last_received_ : to_arm7_.front();
        }
        if (to_arm7_.empty())
        {
            fifo_error_ = true;
            return last_received_;
        }
        last_received_ = to_arm7_.front();
        to_arm7_.pop_front();
        if (to_arm7_.empty() && link_ != nullptr)
        {
            link_->OnFifoToArm7Empty();
            LinkEvent();
        }
        return last_received_;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoEnd)
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

void Arm7System::WriteIo(uint32_t address, int size, uint32_t value)
{
    if (link_ != nullptr && IsCardRegister(address))
    {
        link_->WriteCard(address, size, value);
        return;
    }

    if (address >= kSoundRegisters && address < kSoundRegistersEnd)
    {
        SyncSound();
        sound_.Write(address, size, value);
        if (on_sound_access)
        {
            on_sound_access(address, size, value, true);
        }
        return;
    }

    if (address == kIpcFifoSend)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return;
        }
        if (to_arm9_.size() == kFifoWords)
        {
            fifo_error_ = true;
            return;
        }
        to_arm9_.push_back(value);
        if (link_ != nullptr)
        {
            link_->OnFifoToArm9(value);
            LinkEvent();
        }
        return;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoEnd)
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

uint16_t Arm7System::ReadIo16(uint32_t offset)
{
    const auto stored = [this](uint32_t at)
    {
        return static_cast<uint16_t>(io_[at] | io_[at + 1] << 8);
    };

    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        // Only the control half reads back.
        const Dma& dma = dmas_[(offset - kDmaStart) / 12];
        return (offset - kDmaStart) % 12 == 10 ? static_cast<uint16_t>(dma.control >> 16) : 0;
    }

    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        return offset % 4 == 0 ? TimerCounter(timer) : timers_[static_cast<std::size_t>(timer)].control;
    }

    switch (offset)
    {
    case kDispstat:
        {
            const uint64_t line = Line();
            const uint64_t vcount_setting = VcountSetting(dispstat_);
            uint16_t flags = 0;
            flags |= line >= kVblankLine && line < kFrameLines - 1 ? kVblankFlag : 0;
            flags |= clock_ % kLineCycles >= kHblankCycle ? kHblankFlag : 0;
            flags |= line == vcount_setting ? kVcountFlag : 0;
            return static_cast<uint16_t>((dispstat_ & kDispstatWritable) | flags);
        }
    case kVcount:
        return static_cast<uint16_t>(Line());
    case kKeyInput:
        return kKeysReleased;
    case kExtendedKeys:
        return kExtendedKeysIdle;
    case kIpcSync:
        // Without a link, the ARM9's output follows the ARM7's.
        return static_cast<uint16_t>((ipcsync_ & (kSyncOutput | kSyncIrqEnable)) |
                                     (link_ != nullptr ? link_->SyncOutput() & 0xF : (ipcsync_ & kSyncOutput) >> 8));
    case kIpcFifoControl:
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
        return static_cast<uint16_t>(if_);
    case kIf + 2:
        return static_cast<uint16_t>(if_ >> 16);
    case kMemoryStatus:
        return static_cast<uint16_t>(wram_control_ << 8);
    case kPostFlag:
        return io_[kPostFlag];
    case kKeyControl:
    case kExternalMemory:
    case kWifiWait:
    case kPowerControl:
    case kBiosProtection:
    case kBiosProtection + 2:
        return stored(offset);
    case kSpiControl:
        // Never busy.
        if (spi_ == nullptr)
        {
            Unknown(kIo + offset, 2, false);
        }
        return static_cast<uint16_t>(stored(offset) & 0xCF03);
    case kSpiData:
        if (spi_ == nullptr)
        {
            Unknown(kIo + offset, 2, false);
            return 0;
        }
        return spi_answer_;
    default:
        Unknown(kIo + offset, 2, false);
        return stored(offset);
    }
}

void Arm7System::WriteIo16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const auto store = [this, value, mask](uint32_t at)
    {
        const uint16_t old = static_cast<uint16_t>(io_[at] | io_[at + 1] << 8);
        const uint16_t merged = static_cast<uint16_t>((old & ~mask) | (value & mask));
        io_[at] = static_cast<uint8_t>(merged);
        io_[at + 1] = static_cast<uint8_t>(merged >> 8);
        return merged;
    };
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

    switch (offset)
    {
    case kDispstat:
        dispstat_ = static_cast<uint16_t>(merge(dispstat_) & kDispstatWritable);
        ScheduleDisplay();
        return;
    case kIpcSync:
        ipcsync_ = static_cast<uint16_t>(merge(ipcsync_) & (kSyncOutput | kSyncIrqEnable));
        if (link_ != nullptr)
        {
            // The ARM9 reads the new output before the ARM7 goes on.
            link_->OnSyncWrite((value & mask & kSyncRequest) != 0);
            LinkEvent();
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
        if_ &= ~(static_cast<uint32_t>(value & mask) << (offset == kIf ? 0 : 16));
        UpdateIrq();
        return;
    case kPostFlag:
        if ((mask & 0x00FF) != 0)
        {
            io_[kPostFlag] |= static_cast<uint8_t>(value & 1);
        }
        if ((mask & 0xFF00) != 0)
        {
            const auto halt = static_cast<uint8_t>(value >> 8);
            if ((halt & kHaltModeMask) == kHaltMode)
            {
                Halt();
            }
            else if ((halt & kHaltModeMask) != 0)
            {
                Fail("HALTCNT " + Hex(halt) + ": a mode that the system doesn't model");
            }
        }
        return;
    case kIme + 2:
    case kMemoryStatus:
    case kKeyInput:
    case kExtendedKeys:
    case kVcount:
        return;
    case kKeyControl:
    case kExternalMemory:
    case kWifiWait:
    case kPowerControl:
    case kBiosProtection:
    case kBiosProtection + 2:
        store(offset);
        return;
    case kSpiControl:
        if (spi_ == nullptr)
        {
            Unknown(kIo + offset, 2, true);
        }
        else if ((store(offset) & kSpiEnable) == 0)
        {
            spi_->Deselect();
            return;
        }
        store(offset);
        return;
    case kSpiData:
        {
            const uint16_t control = static_cast<uint16_t>(io_[kSpiControl] | io_[kSpiControl + 1] << 8);
            if (spi_ == nullptr)
            {
                Unknown(kIo + offset, 2, true);
            }
            else if ((control & kSpiEnable) != 0)
            {
                spi_answer_ = spi_->Transfer(control >> 8 & 3, static_cast<uint8_t>(value), (control & kSpiHold) != 0);
            }
            if ((control & (kSpiEnable | kSpiIrq)) == (kSpiEnable | kSpiIrq))
            {
                Request(irq::kSpi);
            }
            return;
        }
    default:
        Unknown(kIo + offset, 2, true);
        store(offset);
        return;
    }
}

void Arm7System::Unknown(uint32_t address, int size, bool write)
{
    if (on_unknown_io)
    {
        on_unknown_io(address, size, write);
    }
}

uint32_t Arm7System::Load(uint32_t address, int size)
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

void Arm7System::Store(uint32_t address, int size, uint32_t value)
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

bool Arm7System::Swi(arm::Cpu& cpu, uint32_t comment)
{
    // In ARM state, the BIOS takes the number from bits 16 to 23 of the comment.
    const uint32_t number = (cpu.Cpsr() & arm::psr::kT) != 0 ? comment : (comment >> 16) & 0xFF;
    switch (number)
    {
    case kWaitByLoop:
        return true;
    case kIntrWait:
        return false;
    case kVBlankIntrWait:
        cpu.SetReg(0, 1);
        cpu.SetReg(1, irq::kVblank);
        return false;
    case kHalt:
        Halt();
        return true;
    case kSoundBiasSwi:
        {
            // The BIOS moves the bias one step each r1 loops. The system moves it at once.
            const uint32_t bias = cpu.Reg(0) != 0 ? kSoundBiasMiddle : 0;
            WriteIo(kSoundBias, 2, bias);
            return true;
        }
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
            const uint32_t first = Load(source, size);
            for (uint32_t i = 0; i < count; i++)
            {
                Store(destination, size, fill ? first : Load(source, size));
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
                last = Load(at, 2);
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
    case kGetPitchTable:
    case kGetVolumeTable:
        {
            const uint32_t index = cpu.Reg(0);
            const std::size_t count =
                number == kGetPitchTable ? std::size(snd::tables::kPitch) : std::size(snd::tables::kVolume);
            if (index >= count)
            {
                Fail("a read past the BIOS's table at " + Hex(index));
                return true;
            }
            cpu.SetReg(0, number == kGetPitchTable ? snd::tables::kPitch[index] : snd::tables::kVolume[index]);
            return true;
        }
    case kCustomHalt:
        WriteIo(kIo + kPostFlag + 1, 1, cpu.Reg(2) & 0xFF);
        return true;
    default:
        Fail("SWI " + Hex(number) + ", which the BIOS doesn't model, at " + Hex(cpu.Pc()));
        return true;
    }
}

void Arm7System::Fail(std::string message)
{
    if (error_.empty())
    {
        error_ = std::move(message);
    }
    cpu_->Stop();
}

void Arm7System::SyncSound()
{
    const uint64_t ticks = clock_ / 2;
    if (ticks <= sound_ticks_)
    {
        return;
    }

    sound_.Run(ticks - sound_ticks_, out_ != nullptr ? *out_ : discarded_);
    sound_ticks_ = ticks;
    discarded_.clear();
}

uint64_t Arm7System::NextEvent() const
{
    uint64_t next = next_display_;
    for (int timer = 0; timer < 4; timer++)
    {
        if (Counting(timer))
        {
            next = std::min(next, TimerOverflow(timer));
        }
    }
    return next;
}

void Arm7System::Advance(uint64_t until)
{
    for (;;)
    {
        const uint64_t next = NextEvent();
        if (next > until)
        {
            break;
        }

        clock_ = next;
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
    }
    clock_ = std::max(clock_, until);
}

void Arm7System::Request(uint32_t irq)
{
    if_ |= irq;
    UpdateIrq();
}

void Arm7System::UpdateIrq()
{
    cpu_->SetIrq((ime_ & 1) != 0 && (ie_ & if_) != 0);
}

void Arm7System::Halt()
{
    halted_ = true;
    cpu_->Stop();
}

uint64_t Arm7System::Prescale(int timer) const
{
    return kPrescales[timers_[static_cast<std::size_t>(timer)].control & 3];
}

bool Arm7System::Counting(int timer) const
{
    const uint16_t control = timers_[static_cast<std::size_t>(timer)].control;
    return (control & kTimerStartBit) != 0 && (timer == 0 || (control & kTimerCascade) == 0);
}

uint16_t Arm7System::TimerCounter(int timer) const
{
    const Timer& t = timers_[static_cast<std::size_t>(timer)];
    if (!Counting(timer))
    {
        return t.counter;
    }
    return static_cast<uint16_t>(t.counter + (clock_ - t.base) / Prescale(timer));
}

uint64_t Arm7System::TimerOverflow(int timer) const
{
    const Timer& t = timers_[static_cast<std::size_t>(timer)];
    return t.base + (0x10000 - static_cast<uint64_t>(t.counter)) * Prescale(timer);
}

void Arm7System::RebaseTimer(int timer)
{
    if (!Counting(timer))
    {
        return;
    }

    Timer& t = timers_[static_cast<std::size_t>(timer)];
    const uint64_t counts = (clock_ - t.base) / Prescale(timer);
    t.counter = static_cast<uint16_t>(t.counter + counts);
    t.base += counts * Prescale(timer);
}

void Arm7System::OverflowTimer(int timer, uint64_t at)
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

void Arm7System::WriteTimerControl(int timer, uint16_t control)
{
    RebaseTimer(timer);
    Timer& t = timers_[static_cast<std::size_t>(timer)];
    const uint16_t old = t.control;
    t.control = static_cast<uint16_t>(control & kTimerWritable);
    if ((old & kTimerStartBit) == 0 && (control & kTimerStartBit) != 0)
    {
        t.counter = t.reload;
        t.base = clock_;
    }
    else if ((old & 3) != (control & 3))
    {
        t.base = clock_;
    }
}

uint64_t Arm7System::Line() const
{
    return clock_ / kLineCycles % kFrameLines;
}

void Arm7System::ScheduleDisplay()
{
    // The next start of a line, or of a line's horizontal blank, after the clock.
    const auto next = [this](uint64_t line, uint64_t cycle)
    {
        const uint64_t at = clock_ / kFrameCycles * kFrameCycles + line * kLineCycles + cycle;
        return at > clock_ ? at : at + kFrameCycles;
    };

    next_display_ = std::numeric_limits<uint64_t>::max();
    if ((dispstat_ & kVblankIrq) != 0)
    {
        next_display_ = std::min(next_display_, next(kVblankLine, 0));
    }
    if ((dispstat_ & kVcountIrq) != 0)
    {
        const uint64_t setting = VcountSetting(dispstat_);
        if (setting < kFrameLines)
        {
            next_display_ = std::min(next_display_, next(setting, 0));
        }
    }
    if ((dispstat_ & kHblankIrq) != 0)
    {
        const uint64_t at = clock_ / kLineCycles * kLineCycles + kHblankCycle;
        next_display_ = std::min(next_display_, at > clock_ ? at : at + kLineCycles);
    }
}

void Arm7System::DisplayEvent(uint64_t at)
{
    const uint64_t line = at / kLineCycles % kFrameLines;
    const uint64_t cycle = at % kLineCycles;
    if (cycle == 0 && line == kVblankLine && (dispstat_ & kVblankIrq) != 0)
    {
        Request(irq::kVblank);
    }
    if (cycle == 0 && (dispstat_ & kVcountIrq) != 0 && line == VcountSetting(dispstat_))
    {
        Request(irq::kVcount);
    }
    if (cycle == kHblankCycle && (dispstat_ & kHblankIrq) != 0)
    {
        Request(irq::kHblank);
    }
    ScheduleDisplay();
}

void Arm7System::WriteDmaControl(int channel, uint32_t control)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const bool starts = (dma.control & kDmaEnable) == 0 && (control & kDmaEnable) != 0;
    dma.control = control;
    if (!starts)
    {
        return;
    }

    // Only the immediate start is modelled. The transfer happens at once.
    const uint32_t timing = (control >> 28) & 3;
    if (timing != 0)
    {
        Fail("a DMA that starts on an event (" + Hex(control) + ")");
        return;
    }

    const int size = (control & kDmaWords) != 0 ? 4 : 2;
    const uint32_t mask = channel == 3 ? 0xFFFF : 0x3FFF;
    uint32_t count = control & mask;
    if (count == 0)
    {
        count = mask + 1;
    }
    const auto step = [size](uint32_t mode)
    {
        return mode == 1 ? -size : mode == 2 ? 0 : size;
    };
    const int destination_step = step((control >> 21) & 3);
    const int source_step = step((control >> 23) & 3);
    uint32_t source = dma.source & ~static_cast<uint32_t>(size - 1);
    uint32_t destination = dma.destination & ~static_cast<uint32_t>(size - 1);
    for (uint32_t i = 0; i < count; i++)
    {
        Store(destination, size, Load(source, size));
        source += static_cast<uint32_t>(source_step);
        destination += static_cast<uint32_t>(destination_step);
    }

    dma.control &= ~kDmaEnable;
    if ((control & kDmaIrq) != 0)
    {
        Request(irq::kDma0 << channel);
    }
}

uint16_t Arm7System::FifoControl() const
{
    uint16_t value = fifo_control_;
    value |= to_arm9_.empty() ? kSendEmpty : 0;
    value |= to_arm9_.size() == kFifoWords ? kSendFull : 0;
    value |= to_arm7_.empty() ? kReceiveEmpty : 0;
    value |= to_arm7_.size() == kFifoWords ? kReceiveFull : 0;
    value |= fifo_error_ ? kFifoError : 0;
    return value;
}

void Arm7System::WriteFifoControl(uint16_t value)
{
    if ((value & kSendClear) != 0)
    {
        to_arm9_.clear();
    }
    if ((value & kFifoError) != 0)
    {
        fifo_error_ = false;
    }

    const uint16_t old = fifo_control_;
    fifo_control_ = static_cast<uint16_t>(value & kFifoWritable);
    if ((old & kSendEmptyIrq) == 0 && (fifo_control_ & kSendEmptyIrq) != 0 && to_arm9_.empty())
    {
        Request(irq::kIpcSendEmpty);
    }
    if ((old & kReceiveIrq) == 0 && (fifo_control_ & kReceiveIrq) != 0 && !to_arm7_.empty())
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

} // namespace ntdlib::ntr
