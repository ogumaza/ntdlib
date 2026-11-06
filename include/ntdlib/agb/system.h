// SPDX-License-Identifier: MIT

// Enough of a GBA to call a game's sound driver: the ARM7TDMI, the work memories, the cartridge's ROM, the I/O
// registers, the DMA's immediate transfers and a BIOS of high-level calls in place of the console's. The caller calls
// the game's routines one at a time, in the order of the game's interrupts. Nothing runs between the calls: no
// interrupt, timer, sound DMA or display. The I/O registers hold what the code writes to them. The vertical count is
// the exception. It reads as a line that the caller holds, or else moves on a line with each read. Code that waits for
// a line then goes on. The video memories and the save memory aren't modelled. Their accesses count as unmapped ones.
// The cartridge's ROM is a copy, and a write there changes the copy.
//
// The BIOS's calls follow GBATEK's description of the GBA's BIOS.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/arm/cpu.h"

namespace ntdlib::agb
{

// The GBA's memory map.
constexpr uint32_t kBiosSize = 0x4000;
constexpr uint32_t kEwramBase = 0x02000000;
constexpr uint32_t kEwramSize = 0x40000;
constexpr uint32_t kIwramBase = 0x03000000;
constexpr uint32_t kIwramSize = 0x8000;
constexpr uint32_t kIoBase = 0x04000000;
constexpr uint32_t kIoSize = 0x400;
constexpr uint32_t kMaxRomSize = 0x2000000;

// The I/O registers that the sound drivers use.
constexpr uint32_t kRegVcount = 0x04000006;
constexpr uint32_t kRegSoundCntH = 0x04000082; // the DirectSound outputs' volumes and sides
constexpr uint32_t kRegSoundBias = 0x04000088;
constexpr uint32_t kRegDma0Source = 0x040000B0; // DMAs 1 to 3 follow, 12 bytes apart
constexpr uint32_t kRegTimer0Reload = 0x04000100;

// The lines of a frame, and the first line of the vertical blank.
constexpr int kFrameLines = 228;
constexpr int kVblankLine = 160;

class System final : public arm::Bus
{
public:
    // The stack pointer that each call starts with. The BIOS gives the system mode this stack.
    static constexpr uint32_t kStack = 0x03007F00;

    // The cycles after which a call fails.
    static constexpr uint64_t kCallCycles = 400'000'000;

    // A system with a copy of `rom` as its cartridge. Throws std::invalid_argument when the image is larger than the
    // GBA's 32 MiB.
    explicit System(const Rom& rom);
    System(const System&) = delete;
    System& operator=(const System&) = delete;
    ~System() override;

    arm::Cpu& Cpu();

    // Calls the routine at `address` (Thumb when bit 0 is set) in system mode with `args` in r0 to r3, from kStack, and
    // returns r0. Throws std::runtime_error when the routine makes a BIOS call that the system doesn't model, runs an
    // undefined instruction or doesn't return within kCallCycles.
    uint32_t Call(uint32_t address, std::span<const uint32_t> args = {});

    // Reads and writes memory as the CPU does. Unmapped addresses read as 0 and ignore writes.
    uint32_t Load(uint32_t address, int size);
    void Store(uint32_t address, int size, uint32_t value);

    // Holds the vertical count at `line`, or with std::nullopt lets it move on a line with each read.
    void HoldVcount(std::optional<int> line);

    // The count of reads and writes of addresses that the system doesn't map.
    uint64_t UnmappedAccesses() const;

    // Called after each write to an I/O register, with the register's address and the written bytes.
    std::function<void(uint32_t address, int size, uint32_t value)> on_io_write;

    // arm::Bus.
    uint8_t* Page(uint32_t page) override;
    uint32_t Read(uint32_t address, int size) override;
    void Write(uint32_t address, int size, uint32_t value) override;

private:
    uint32_t ReadIo(uint32_t offset, int size);
    void WriteIo(uint32_t offset, int size, uint32_t value);

    // Runs DMA `channel`'s transfer if its control starts one at once.
    void StartDma(int channel);

    // The BIOS's calls: Div, CpuSet, CpuFastSet and GetBiosChecksum. Returns false for a call that the system doesn't
    // model.
    bool Swi(arm::Cpu& cpu, uint32_t comment);
    void CpuSet(uint32_t source, uint32_t destination, uint32_t control, bool fast);

    std::vector<uint8_t> bios_;
    std::vector<uint8_t> ewram_;
    std::vector<uint8_t> iwram_;
    std::vector<uint8_t> io_;
    std::vector<uint8_t> rom_;
    std::optional<int> held_vcount_;
    int vcount_ = 0;
    uint64_t unmapped_ = 0;
    std::string failure_;
    std::unique_ptr<arm::Cpu> cpu_;
};

} // namespace ntdlib::agb
