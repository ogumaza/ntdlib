// SPDX-License-Identifier: MIT

// The ARM7's side of a DS, enough to run a cartridge's ARM7 binary and its sound driver: the ARM7TDMI, main memory, the
// shared and the ARM7's work memories, the timers, the interrupts, the display's timing, the IPC with the ARM9, the
// DMA's immediate transfers, the sound unit, and a BIOS of high-level calls in place of the console's. The devices that
// only the OS's other libraries reach (the SPI devices, the RTC, the card, the GBA slot and the wireless unit) answer
// with idle values.
//
// The system boots the ARM7 binary directly, in the state that the firmware leaves: the binary at its address in main
// memory, the cartridge's header at 0x027FFE00, and the CPU at the binary's entry. The caller plays the ARM9: it
// answers the IPC and writes main memory. A system made with an Arm9Link runs beside an ARM9 instead (ntr/machine.h).
// The ARM9 then answers the IPC and the start-up's handshakes itself.
//
// The CPU runs as if it were infinitely fast: the system's clock passes only while the CPU halts. A burst of code that
// an interrupt starts therefore runs at the interrupt's time, and every register access in it happens then. A burst
// that runs for more than kLongBurst cycles without halting advances the clock by its cycles. Code that polls a timer
// then goes on.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/ntr/timers.h"

namespace ntdlib::ntr
{

// The ARM7's interrupts: bits of IE and IF.
namespace irq
{

constexpr uint32_t kVblank = 1u << 0;
constexpr uint32_t kHblank = 1u << 1;
constexpr uint32_t kVcount = 1u << 2;
constexpr uint32_t kTimer0 = 1u << 3; // timers 1 to 3 follow
constexpr uint32_t kDma0 = 1u << 8;   // DMAs 1 to 3 follow
constexpr uint32_t kIpcSync = 1u << 16;
constexpr uint32_t kIpcSendEmpty = 1u << 17;
constexpr uint32_t kIpcReceiveNotEmpty = 1u << 18;
constexpr uint32_t kCard = 1u << 19; // the card's transfer is complete
constexpr uint32_t kSpi = 1u << 23;

} // namespace irq

// The ARM7's display timing, in cycles of its clock: 263 lines of 355 dots of 6 cycles. The vertical blank starts at
// line 192.
constexpr uint64_t kLineCycles = 2130;
constexpr uint64_t kFrameLines = 263;
constexpr uint64_t kFrameCycles = kLineCycles * kFrameLines;
constexpr uint64_t kVblankLine = 192;

class SpiDevices;

// The ARM9's side of the IPC, for a system that runs beside an ARM9. The system calls it when the ARM7's side changes.
class Arm9Link
{
public:
    virtual ~Arm9Link() = default;

    // The ARM9's IPCSYNC output: bits 8 to 11 of its IPCSYNC, shifted down to bits 0 to 3.
    virtual uint16_t SyncOutput() const = 0;

    // The ARM7 writes IPCSYNC. `irq` is set when the write asks for the ARM9's IPC sync IRQ (bit 13).
    virtual void OnSyncWrite(bool irq) = 0;

    // The ARM7 puts `word` into the FIFO to the ARM9.
    virtual void OnFifoToArm9(uint32_t word) = 0;

    // The ARM7 takes the last word of the FIFO from the ARM9.
    virtual void OnFifoToArm7Empty() = 0;

    // The card's registers, at 0x040001A0 to 0x040001BB, and its data at 0x04100010. Both CPUs reach them.
    virtual uint32_t ReadCard(uint32_t address, int size) = 0;
    virtual void WriteCard(uint32_t address, int size, uint32_t value) = 0;
};

class Arm7System final : public arm::Bus
{
public:
    // The cycles of a burst after which the clock passes with the CPU's cycles.
    static constexpr uint64_t kLongBurst = 4'000'000;

    // The same beside an ARM9. The ARM7 polls the ARM9 at start-up, and the ARM9 then runs after each such burst.
    static constexpr uint64_t kLinkedBurst = 8'192;

    // A system with `rom`'s ARM7 binary loaded and the CPU at its entry. Throws std::runtime_error when the binary
    // doesn't fit in the memory at its address.
    explicit Arm7System(const Rom& rom);

    // The same beside an ARM9, through `link`. Run returns early once the ARM7 calls `link` with a change. The ARM9
    // then takes the change at the same clock.
    Arm7System(const Rom& rom, Arm9Link& link);
    Arm7System(const Arm7System&) = delete;
    Arm7System& operator=(const Arm7System&) = delete;
    ~Arm7System() override;

    arm::Cpu& Cpu();
    SoundUnit& Sound();
    std::span<uint8_t> MainMemory();

    // The system's clock, in cycles of the ARM7's clock since the boot.
    uint64_t Clock() const;

    // Whether the CPU halts, waiting for an interrupt.
    bool Halted() const;

    // Runs the system until the clock reaches `until`, and appends the sound unit's output samples to `out`. Throws
    // std::runtime_error when the CPU stops on an undefined instruction or an SWI that the BIOS doesn't model.
    void Run(uint64_t until, std::vector<SoundOutput>& out);

    // The ARM9's side of the IPC. Without an Arm9Link, the ARM9's IPCSYNC output follows the ARM7's output. Both sides
    // of the SDK's handshake then end alike. SendToArm7 drops a word when the FIFO is full.
    void SendToArm7(uint32_t word);
    std::vector<uint32_t> TakeFromArm7();

    // The rest of the ARM9's side, for an Arm9Link. ReceiveFromArm7 gives the ARM9 the first word of the FIFO, and
    // takes the word out when `take` is set. The FIFO must hold a word.
    uint32_t ReceiveFromArm7(bool take);
    std::size_t WordsToArm9() const;
    std::size_t WordsToArm7() const;
    void ClearToArm7();

    // The ARM9 asks for the ARM7's IPC sync IRQ.
    void RequestSyncIrq();

    // Requests the ARM7's interrupts `irq`: the card's, when the ARM7 holds the card.
    void RequestIrq(uint32_t irq);

    // The ARM7's IPCSYNC output, in bits 0 to 3.
    uint16_t SyncOutput() const;

    // WRAMCNT: how the shared work memory divides between the CPUs. The ARM9 writes the register, and reaches its part
    // of the memory through SharedWram.
    void SetWramControl(uint8_t value);
    std::span<uint8_t> SharedWram();

    // Called for each access to the sound unit's registers, with the clock of the access.
    std::function<void(uint32_t address, int size, uint32_t value, bool write)> on_sound_access;

    // Called for each access to an I/O register or a device that the system doesn't model.
    std::function<void(uint32_t address, int size, bool write)> on_unknown_io;

    // arm::Bus.
    uint8_t* Page(uint32_t page) override;
    uint32_t Read(uint32_t address, int size) override;
    void Write(uint32_t address, int size, uint32_t value) override;

private:
    struct Dma
    {
        uint32_t source = 0;
        uint32_t destination = 0;
        uint32_t control = 0;
    };

    uint32_t ReadIo(uint32_t address, int size);
    void WriteIo(uint32_t address, int size, uint32_t value);
    uint16_t ReadIo16(uint32_t address);
    void WriteIo16(uint32_t address, uint16_t value, uint16_t mask);
    void Unknown(uint32_t address, int size, bool write);

    // Memory through the bus, for the BIOS and the DMA.
    bool Swi(arm::Cpu& cpu, uint32_t number);
    void Fail(std::string message);

    // The clock, the events and the interrupts.
    void SyncSound();
    uint64_t NextEvent() const;
    void Advance(uint64_t until);
    void Request(uint32_t irq);
    void UpdateIrq();
    void Halt();
    uint64_t Line() const;
    void ScheduleDisplay();
    void DisplayEvent(uint64_t at);
    void WriteDmaControl(int channel, uint32_t control);

    Arm7System(const Rom& rom, Arm9Link* link);

    // The ARM9's answers to the ARM7's writes in the area at main memory's end.
    void AnswerArm9(uint32_t offset, int size);

    // Tells the ARM9 of a change on the ARM7's side, and ends Run's burst.
    void LinkEvent();

    // The IPC.
    uint16_t FifoControl() const;
    void WriteFifoControl(uint16_t value);

    std::vector<uint8_t> bios_;
    std::vector<uint8_t> main_;
    std::vector<uint8_t> shared_wram_;
    std::vector<uint8_t> wram_;
    std::vector<uint8_t> vram_;
    std::unique_ptr<arm::Cpu> cpu_;
    SoundUnit sound_;
    Arm9Link* link_ = nullptr;
    bool link_event_ = false;
    uint8_t wram_control_ = 3;        // WRAMCNT: 3 gives all of the shared work memory to the ARM7
    std::unique_ptr<SpiDevices> spi_; // the SPI bus's devices, beside an ARM9 only
    uint8_t spi_answer_ = 0;

    uint64_t clock_ = 0;
    uint64_t sound_ticks_ = 0; // the sound unit's counts so far, at half the clock
    std::vector<SoundOutput>* out_ = nullptr;
    std::vector<SoundOutput> discarded_;
    uint64_t next_display_ = 0;
    bool halted_ = false;
    uint64_t burst_cycles_ = 0;         // the CPU's cycles since it last halted or the clock passed
    uint16_t arm7_start_counter_ = 0;   // the ARM7's counter of the first handshake at the ARM9's last read
    uint32_t arm9_handshake_count_ = 0; // the ARM9's count as the side of the byte handshake that writes byte 0
    uint32_t arm9_handshake_sum_ = 0;   // the sum of the bytes that the ARM9 copies as the other side
    std::string error_;

    uint16_t ime_ = 0;
    uint32_t ie_ = 0;
    uint32_t if_ = 0;
    uint16_t dispstat_ = 0;
    Timers timers_;
    std::array<Dma, 4> dmas_ = {};

    uint16_t ipcsync_ = 0; // the ARM7's IPCSYNC: its output in bits 8 to 11 and the IRQ enable in bit 14
    uint16_t fifo_control_ = 0;
    std::deque<uint32_t> to_arm9_;
    std::deque<uint32_t> to_arm7_;
    uint32_t last_received_ = 0;
    bool fifo_error_ = false;

    std::array<uint8_t, 0x400> io_ = {}; // the plain registers that the system stores without a meaning
};

} // namespace ntdlib::ntr
