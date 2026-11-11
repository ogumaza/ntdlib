// SPDX-License-Identifier: MIT

// A DS with both CPUs: the ARM946E-S beside Arm7System (ntr/arm7.h), over one main memory. It boots a cartridge image
// and runs the game's code. The ARM9's side holds main memory and the TCMs, the shared work memory as WRAMCNT divides
// it, the timers, the interrupts, the display's timing, the IPC, DMA, the divider and the square root unit, the card,
// and a BIOS of high-level calls in place of the console's. The graphics engines, the 3D engine, the keys and the GBA
// slot answer with idle values: their registers keep what the game writes, the 3D engine is never busy, a display
// capture ends at the next vertical blank, no key is down and no GBA cartridge is in the slot.
//
// The machine boots the image as the firmware leaves a DS: the ARM9 and ARM7 binaries at their addresses, the header,
// the card's chip ID and the user's settings in the area at main memory's end, and both CPUs at their binaries'
// entries. The card answers the plain read command (B7) and the chip ID (B8) from the image. The image must be
// decrypted. The card's backup memory is of the kind that the caller gives. A game's code addresses it as one kind, and
// a cartridge's header doesn't say which.
//
// Both CPUs run as if they were infinitely fast, as Arm7System's CPU does: the clock passes while a CPU halts, and a
// burst of code takes no time. The ARM9's clock runs at twice the system's. A burst of the ARM9 that runs for more than
// kSlice of its cycles passes its cycles' time. So does a burst that polls: one that reads a register that changes with
// time kPollReads times. The BIOS's WaitByLoop passes its time too, and so do halts that a masked IRQ ends at once.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/ntr/timers.h"

namespace ntdlib::ntr
{

class SpiMemory;

// A cartridge's backup memory, by the bytes of its addresses: 1 for an EEPROM of 512 bytes, 2 for a larger EEPROM or a
// FRAM, 3 for a flash memory. Each write takes effect at once. With 0 address bytes, the memory reads as blank and
// keeps nothing.
//
// A cartridge whose game code starts with I has an infrared chip on the bus in front of the memory. Each transfer then
// starts with a command to the chip, and 00h passes the rest of the transfer to the memory. The chip gives its version
// (AAh) to command 08h, and answers every other command with zeros: it never receives anything over infrared.
struct BackupMemory
{
    int address_bytes = 0;
    std::size_t size = 0;  // a power of two: 512 with 1 address byte, up to 64 KiB with 2, up to 16 MiB with 3
    uint8_t blank = 0xFF;  // the value of each byte at the start
    bool infrared = false; // the infrared chip in front of the memory
};

// What the machine has done since its boot, for checks of a game's boot.
struct MachineCounts
{
    uint64_t vblank_acks = 0; // the ARM9's acknowledgements of its vertical blank IRQ
    uint64_t card_reads = 0;  // the card's reads (B7)
    uint64_t card_bytes = 0;  // the bytes that they gave
    uint64_t slices = 0;      // the ARM9's bursts that ran for kSlice cycles
    uint64_t polls = 0;       // the ARM9's bursts that ended as they polled
};

class Machine final : public arm::Bus, private Arm9Link
{
public:
    // The ARM9's cycles after which a burst passes its time.
    static constexpr uint64_t kSlice = 1 << 21;

    // The reads of registers that change with time after which a burst of the ARM9 passes its time.
    static constexpr int kPollReads = 64;

    // The ARM9's cycles in halts that end at once after which they pass their time.
    static constexpr uint64_t kSpinCycles = 1 << 12;

    // A machine with `rom` booted and `backup` in its card. Throws std::runtime_error when a binary doesn't fit in the
    // memory at its address, and std::invalid_argument for a backup memory of another size.
    explicit Machine(const Rom& rom, const BackupMemory& backup = {});
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    ~Machine() override;

    arm::Cpu& Arm9();
    Arm7System& Arm7();
    std::span<uint8_t> MainMemory();
    const MachineCounts& Counts() const;

    // The backup memory's bytes, empty without one.
    std::span<uint8_t> Backup();

    // The system's clock, in cycles of the ARM7's clock since the boot.
    uint64_t Clock() const;

    // Runs the machine until the clock reaches `until` or the ARM9 reaches its stop address (arm::Cpu's
    // SetStopAddress), and appends the sound unit's output samples to `out`. Returns whether the ARM9 reached the stop
    // address. Throws std::runtime_error when a CPU stops on a fault, or on an SWI that the BIOS doesn't model.
    bool Run(uint64_t until, std::vector<SoundOutput>& out);

    // Calls the ARM9's function at `address` with `args` in the registers and on the stack, like arm::Cpu's Call. The
    // machine runs meanwhile: the interrupts, the other threads of the game and the ARM7 go on until the function
    // returns. Returns r0. Throws std::runtime_error when the clock reaches `until` first.
    uint32_t Call(uint32_t address, std::span<const uint32_t> args, uint64_t until, std::vector<SoundOutput>& out);

    // arm::Bus, for the ARM9.
    uint8_t* Page(uint32_t page) override;
    uint32_t Read(uint32_t address, int size) override;
    void Write(uint32_t address, int size, uint32_t value) override;

    // Called for each SWI of the ARM9 with its comment field, before the BIOS. Returns true when it handled the SWI.
    std::function<bool(arm::Cpu&, uint32_t comment)> on_arm9_swi;

    // Called for each word that a CPU puts into the IPC's FIFO to the other: `to_arm7` is set for the ARM9's words.
    std::function<void(bool to_arm7, uint32_t word)> on_fifo_word;

    // Called for each access of the ARM9 to an I/O register or a device that the machine doesn't model.
    std::function<void(uint32_t address, int size, bool write)> on_unknown_io;

private:
    struct Dma
    {
        uint32_t source = 0;
        uint32_t destination = 0;
        uint32_t control = 0;
        uint32_t fill = 0;
        uint32_t next_source = 0; // the transfer's places and the units left in its block, once it starts
        uint32_t next_destination = 0;
        uint32_t left = 0;
    };

    struct Card
    {
        uint16_t spi_control = 0;
        uint8_t spi_data = 0xFF;
        bool spi_selected = false;
        uint8_t spi_command = 0; // the command to a blank backup memory, or to the infrared chip
        bool infrared = false;
        std::size_t spi_position = 0; // the bytes of the transfer so far
        uint32_t control = 0;         // ROMCTRL, without its status bits
        std::array<uint8_t, 8> command = {};
        std::vector<uint8_t> data;
        std::size_t position = 0;
        bool busy = false;
        bool ready = false;
        uint64_t ready_at = 0;
    };

    // Arm9Link.
    uint16_t SyncOutput() const override;
    void OnSyncWrite(bool irq) override;
    void OnFifoToArm9(uint32_t word) override;
    void OnFifoToArm7Empty() override;
    uint32_t ReadCard(uint32_t address, int size) override;
    void WriteCard(uint32_t address, int size, uint32_t value) override;

    void Boot(const Rom& rom);
    void LoadArm9(const Rom& rom);

    uint32_t ReadIo(uint32_t address, int size);
    void WriteIo(uint32_t address, int size, uint32_t value);
    uint16_t ReadIo16(uint32_t offset);
    void WriteIo16(uint32_t offset, uint16_t value, uint16_t mask);
    uint16_t Stored(uint32_t offset) const;
    void Store16(uint32_t offset, uint16_t value, uint16_t mask);
    void Unknown(uint32_t address, int size, bool write);
    void Poll();

    bool Swi(arm::Cpu& cpu, uint32_t comment);
    void Fail(std::string message);

    // The clock, the events and the interrupts.
    void Pass(uint64_t until, bool to_arm7_change, std::vector<SoundOutput>& out);
    uint64_t NextEvent() const;
    void Events();
    void Request(uint32_t irq);
    void UpdateIrq();
    uint64_t Line() const;
    void ScheduleDisplay();
    void ScheduleDisplayAfter(uint64_t after);
    void DisplayEvent(uint64_t at);

    // DMA.
    void WriteDmaControl(int channel, uint32_t control);
    void RunDma(int channel);
    void EndDmaBlock(int channel);
    void RunCardDmas();

    // The card.
    uint16_t CardRead16(uint32_t offset);
    void CardWrite16(uint32_t offset, uint16_t value, uint16_t mask);
    void StartCard();
    void CardReady();
    void EndCard();
    uint32_t ReadCardData();
    void WriteSpiData(uint8_t value);
    void EndSpi();

    // The IPC.
    uint16_t FifoControl() const;
    void WriteFifoControl(uint16_t value);

    // The divider and the square root unit.
    void Divide();
    void SquareRoot();

    std::span<const uint8_t> image_;
    std::unique_ptr<Arm7System> arm7_;
    std::unique_ptr<arm::Cpu> arm9_;
    std::unique_ptr<SpiMemory> backup_;
    std::vector<uint8_t> bios_;
    std::vector<uint8_t> palette_;
    std::vector<uint8_t> oam_;
    std::array<std::vector<uint8_t>, 5> vram_;
    std::array<uint8_t, 0x2000> io_ = {}; // the registers that the machine stores without a meaning
    MachineCounts counts_;
    std::string error_;

    uint64_t next_display_ = 0;
    uint64_t wait_cycles_ = 0; // the ARM9's cycles in WaitByLoop, whose time is to pass
    uint64_t spin_cycles_ = 0; // the ARM9's cycles in halts that the IRQ line ended at once, whose time is to pass
    bool arm9_running_ = false;
    int polls_ = 0;       // the burst's reads of registers that change with time
    bool polled_ = false; // the burst ended as it polled
    bool irq_line_ = false;
    bool arm7_changed_ = false; // the ARM7 changed the IPC since the last run of the ARM7

    uint16_t ime_ = 0;
    uint32_t ie_ = 0;
    uint32_t if_ = 0;
    uint16_t dispstat_ = 0;
    Timers timers_;
    std::array<Dma, 4> dmas_ = {};
    uint8_t wram_control_ = 3;
    uint32_t chip_id_ = 0;
    Card card_;

    uint16_t ipcsync_ = 0; // the ARM9's IPCSYNC: its output in bits 8 to 11 and the IRQ enable in bit 14
    uint16_t fifo_control_ = 0;
    uint32_t last_received_ = 0;
    bool fifo_error_ = false;
};

} // namespace ntdlib::ntr
