// SPDX-License-Identifier: MIT

// What the tests of the ARM interpreter share: the test machine's memory and bus, and the runs of ARM and Thumb code.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "ntdlib/arm/cpu.h"

namespace arm_tests
{


using ntdlib::arm::Architecture;
using ntdlib::arm::Bus;
using ntdlib::arm::Cpu;
using ntdlib::arm::Mode;
using ntdlib::arm::StopReason;
namespace psr = ntdlib::arm::psr;

constexpr Architecture kV4 = Architecture::kArmV4T;
constexpr Architecture kV5 = Architecture::kArmV5TE;

// The test machine's memory: RAM from 0, 64 KiB at the high vectors, and I/O registers at kIo.
constexpr uint32_t kRamSize = 0x40000;
constexpr uint32_t kHigh = 0xFFFF0000;
constexpr uint32_t kIo = 0x04000000;
constexpr uint32_t kIoSize = 0x1000;

// Where the tests put their code, their data and the stack.
constexpr uint32_t kCode = 0x1000;
constexpr uint32_t kData = 0x8000;
constexpr uint32_t kStack = 0x20000;

// A page that the bus can show from a second buffer.
constexpr uint32_t kSwappedPage = 0x5000;

// The CPSR that a Machine starts with: system mode with IRQs and FIQs disabled.
constexpr uint32_t kSystemCpsr = 0xDF;

// A bus with RAM, memory at the high vectors and I/O registers that log their accesses. Other addresses read as 0
// and log their accesses too.
class TestBus : public Bus
{
public:
    struct Access
    {
        uint32_t address = 0;
        int size = 0;
        uint32_t value = 0;
        bool write = false;

        bool operator==(const Access& other) const = default;
    };

    uint8_t* Page(uint32_t page) override;
    uint32_t Read(uint32_t address, int size) override;
    void Write(uint32_t address, int size, uint32_t value) override;

    void Put32(uint32_t address, std::initializer_list<uint32_t> words);
    void Put16(uint32_t address, std::initializer_list<uint16_t> halfwords);
    uint32_t Get32(uint32_t address);
    void SetIo32(uint32_t address, uint32_t value);
    const std::vector<Access>& Accesses() const;
    void ClearAccesses();
    void SwapPage();

private:
    std::vector<uint8_t> ram_ = std::vector<uint8_t>(kRamSize);
    std::vector<uint8_t> high_ = std::vector<uint8_t>(0x10000);
    std::vector<uint8_t> io_ = std::vector<uint8_t>(kIoSize);
    std::vector<uint8_t> swapped_ = std::vector<uint8_t>(0x1000);
    bool swap_ = false;
    std::vector<Access> accesses_;
};

inline uint8_t* TestBus::Page(uint32_t page)
{
    if (page == kSwappedPage && swap_)
    {
        return swapped_.data();
    }

    if (page < kRamSize)
    {
        return ram_.data() + page;
    }

    return page >= kHigh ? high_.data() + (page - kHigh) : nullptr;
}

inline uint32_t TestBus::Read(uint32_t address, int size)
{
    uint32_t value = 0;
    if (address - kIo < kIoSize)
    {
        for (int i = 0; i < size; ++i)
        {
            value |= static_cast<uint32_t>(io_[address - kIo + i]) << (8 * i);
        }
    }

    accesses_.push_back({address, size, value, false});

    return value;
}

inline void TestBus::Write(uint32_t address, int size, uint32_t value)
{
    if (address - kIo < kIoSize)
    {
        for (int i = 0; i < size; ++i)
        {
            io_[address - kIo + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }

    accesses_.push_back({address, size, value, true});
}

inline void TestBus::Put32(uint32_t address, std::initializer_list<uint32_t> words)
{
    for (const uint32_t word : words)
    {
        xsflib::StoreLe32(Page(address & ~0xFFFu) + (address & 0xFFF), word);
        address += 4;
    }
}

inline void TestBus::Put16(uint32_t address, std::initializer_list<uint16_t> halfwords)
{
    for (const uint16_t halfword : halfwords)
    {
        xsflib::StoreLe16(Page(address & ~0xFFFu) + (address & 0xFFF), halfword);
        address += 2;
    }
}

inline uint32_t TestBus::Get32(uint32_t address)
{
    return xsflib::LoadLe32(Page(address & ~0xFFFu) + (address & 0xFFF));
}

inline void TestBus::SetIo32(uint32_t address, uint32_t value)
{
    xsflib::StoreLe32(&io_[address - kIo], value);
}

inline const std::vector<TestBus::Access>& TestBus::Accesses() const
{
    return accesses_;
}

inline void TestBus::ClearAccesses()
{
    accesses_.clear();
}

inline void TestBus::SwapPage()
{
    swap_ = true;
}

// A CPU on a TestBus, in system mode at kCode with SP at kStack. kData holds the bytes 11 22 33 ... FF 00.
struct Machine
{
    explicit Machine(Architecture architecture);

    TestBus bus;
    Cpu cpu;
};

inline Machine::Machine(Architecture architecture) : cpu(bus, architecture)
{
    cpu.Jump(kCode, Mode::kSystem);
    cpu.SetReg(13, kStack);
    bus.Put32(kData, {0x44332211, 0x88776655, 0xCCBBAA99, 0x00FFEEDD});
}

inline Mode CurrentMode(const Cpu& cpu)
{
    return static_cast<Mode>(cpu.Cpsr() & psr::kModeMask);
}

// The flags as a 4-bit number: N, Z, C and V from the top bit down.
inline uint32_t Nzcv(const Cpu& cpu)
{
    return cpu.Cpsr() >> 28;
}

inline void SetNzcv(Cpu& cpu, uint32_t flags)
{
    cpu.SetCpsr((cpu.Cpsr() & 0x0FFFFFFF) | (flags << 28));
}

// Puts ARM code at `address` and runs it from there in the current mode until the CPU reaches `stop`.
inline StopReason RunArmAt(Machine& m, uint32_t address, std::initializer_list<uint32_t> program, uint32_t stop)
{
    m.bus.Put32(address, program);
    m.cpu.Jump(address, CurrentMode(m.cpu));
    m.cpu.SetStopAddress(stop);

    return m.cpu.Run(1000);
}

// Runs ARM code at kCode until it reaches `stop`.
inline StopReason RunArmTo(Machine& m, std::initializer_list<uint32_t> program, uint32_t stop)
{
    return RunArmAt(m, kCode, program, stop);
}

// Runs ARM code at kCode until it passes its last instruction.
inline void RunArm(Machine& m, std::initializer_list<uint32_t> program)
{
    const uint32_t end = kCode + static_cast<uint32_t>(program.size()) * 4;

    const StopReason reason = RunArmTo(m, program, end);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == end);
}

// Runs Thumb code at `address` until the CPU reaches `stop`.
inline StopReason RunThumbAt(Machine& m, uint32_t address, std::initializer_list<uint16_t> program, uint32_t stop)
{
    m.bus.Put16(address, program);
    m.cpu.Jump(address | 1, CurrentMode(m.cpu));
    m.cpu.SetStopAddress(stop);

    return m.cpu.Run(1000);
}

inline StopReason RunThumbTo(Machine& m, std::initializer_list<uint16_t> program, uint32_t stop)
{
    return RunThumbAt(m, kCode, program, stop);
}

// Runs Thumb code at kCode until it passes its last instruction.
inline void RunThumb(Machine& m, std::initializer_list<uint16_t> program)
{
    const uint32_t end = kCode + static_cast<uint32_t>(program.size()) * 2;

    const StopReason reason = RunThumbTo(m, program, end);

    XSFLIB_CHECK(reason == StopReason::kStopAddress && m.cpu.Pc() == (end | 1));
}

// Whether `op` at kCode, in system mode, enters the undefined instruction exception with the address after it in LR
// and the old CPSR in the SPSR.
inline bool RaisesUndefined(Architecture architecture, uint32_t op, bool thumb)
{
    Machine m(architecture);
    const uint32_t vector = (architecture == kV5 ? kHigh : 0) + 0x04;
    const uint32_t old_cpsr = kSystemCpsr | (thumb ? psr::kT : 0);

    const StopReason reason = thumb ? RunThumbTo(m, {static_cast<uint16_t>(op)}, vector) : RunArmTo(m, {op}, vector);

    return reason == StopReason::kStopAddress && m.cpu.Pc() == vector && m.cpu.Cpsr() == 0xDB &&
           m.cpu.Reg(14) == kCode + (thumb ? 2 : 4) && m.cpu.Spsr() == old_cpsr;
}

} // namespace arm_tests
