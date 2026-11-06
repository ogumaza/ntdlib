// SPDX-License-Identifier: MIT

// An interpreter of the DS's two CPUs: the ARM7TDMI (ARMv4T) and the ARM946E-S (ARMv5TE). It runs the ARM and Thumb
// instruction sets, the exceptions that a DS program takes (IRQ, SWI and undefined instructions), and on ARMv5TE the
// system control coprocessor (CP15) with the tightly coupled memories (ITCM and DTCM).
//
// Written to run a game's sound driver. The interpreter follows ARM's architecture reference manual and GBATEK's ARM
// CPU reference. Its timing is a rough count: each instruction takes one cycle, plus one for each memory access.
// Nothing that this library runs depends on the count beyond the order of events. The caches and the ARMv5TE protection
// unit aren't modelled: the CPU accepts their CP15 operations and every access goes through.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace ntdlib::arm
{

enum class Architecture
{
    kArmV4T,  // the ARM7TDMI
    kArmV5TE, // the ARM946E-S
};

// The processor modes, by the value of the CPSR's low 5 bits.
enum class Mode : uint32_t
{
    kUser = 0x10,
    kFiq = 0x11,
    kIrq = 0x12,
    kSupervisor = 0x13,
    kAbort = 0x17,
    kUndefined = 0x1B,
    kSystem = 0x1F,
};

// CPSR bits.
namespace psr
{

constexpr uint32_t kN = 1u << 31;
constexpr uint32_t kZ = 1u << 30;
constexpr uint32_t kC = 1u << 29;
constexpr uint32_t kV = 1u << 28;
constexpr uint32_t kQ = 1u << 27; // ARMv5TE: sticky overflow of the saturating instructions
constexpr uint32_t kI = 1u << 7;  // IRQs disabled
constexpr uint32_t kF = 1u << 6;  // FIQs disabled
constexpr uint32_t kT = 1u << 5;  // Thumb state
constexpr uint32_t kModeMask = 0x1F;

} // namespace psr

// The memory and devices that the CPU accesses. The CPU passes aligned addresses: a word's address is a multiple of 4,
// a halfword's of 2. It does the rotations of unaligned loads itself.
class Bus
{
public:
    virtual ~Bus() = default;

    // The host memory of the 4 KiB page at `page` (a multiple of 0x1000), or nullptr for I/O or an unmapped address.
    // The CPU reads and writes such a page in place, in little-endian order. A page's host memory must not move while
    // the CPU runs. The CPU calls Page again after InvalidatePages().
    virtual uint8_t* Page(uint32_t page) = 0;

    // A read or write of 1, 2 or 4 bytes at an address whose page has no host memory.
    virtual uint32_t Read(uint32_t address, int size) = 0;
    virtual void Write(uint32_t address, int size, uint32_t value) = 0;
};

// Why Run returned.
enum class StopReason
{
    kCycles,      // the cycles ran out
    kHalted,      // the CPU halted. An IRQ wakes it, whether or not the CPSR's I bit masks it
    kStopAddress, // the CPU reached the stop address (SetStopAddress)
    kStopped,     // a handler called Stop()
    kUndefined,   // an undefined instruction, with no handler for its exception (see SetVectorsHandled)
};

class Cpu
{
public:
    // The return address that Call gives the function it calls. Nothing may run there.
    static constexpr uint32_t kCallReturn = 0xFFFFFFF0;

    // A CPU in supervisor mode with IRQs and FIQs disabled, at address 0 in ARM state. An ARMv5TE CPU starts with its
    // TCMs disabled and the exception vectors at 0xFFFF0000. A reset leaves a CPU in that state.
    Cpu(Bus& bus, Architecture architecture);
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    ~Cpu();

    Architecture GetArchitecture() const;

    // Starts the CPU at `pc` in `mode`, in Thumb state when bit 0 of `pc` is set. The other registers keep their
    // values.
    void Jump(uint32_t pc, Mode mode);

    // Runs instructions until about `cycles` cycles have passed, or until one of the other StopReasons. Returns why.
    StopReason Run(uint64_t cycles);

    // Calls the function at `address` (Thumb when bit 0 is set) with `args` in r0 to r3, and the rest on the stack.
    // ARM's procedure call standard passes arguments that way. Runs until the function returns or `max_cycles` pass.
    // Returns r0, or throws std::runtime_error when the function doesn't return in time or the CPU stops for another
    // reason. The call takes the current mode and SP. Afterwards the PC, SP, the stop address and a halt are as they
    // were before the call. The other registers keep the function's values.
    uint32_t Call(uint32_t address, std::span<const uint32_t> args, uint64_t max_cycles);

    // The cycles counted since the CPU was made.
    uint64_t Cycles() const;

    // Asks Run to return after the current instruction (StopReason::kStopped).
    void Stop();

    // The IRQ line: while asserted and the CPSR's I bit is clear, the CPU takes the IRQ exception before its next
    // instruction. An asserted line wakes a halted CPU.
    void SetIrq(bool asserted);

    // Halts the CPU until an IRQ. The ARM7's HALTCNT and the halt SWIs halt it that way. Run returns kHalted.
    void Halt();
    bool Halted() const;

    // Run returns kStopAddress before it runs the instruction at `address`. 0xFFFFFFFF: none.
    void SetStopAddress(uint32_t address);
    uint32_t StopAddress() const;

    // A handler of SWIs in place of the exception: it gets the CPU and the SWI's comment field (the low 24 bits in ARM
    // state, the low 8 in Thumb). It returns true when it handled the SWI, and the CPU goes on after it. Otherwise the
    // CPU takes the exception. A program's high-level BIOS sets this.
    void SetSwiHandler(std::function<bool(Cpu&, uint32_t)> handler);

    // Whether the exception vectors hold code. Without them, an undefined instruction stops Run (kUndefined), and an
    // SWI that the handler leaves stops it too. On by default.
    void SetVectorsHandled(bool handled);

    // The registers of the current mode: r0 to r15. r15 reads as the address of the next instruction to run.
    uint32_t Reg(int index) const;
    void SetReg(int index, uint32_t value);

    uint32_t Cpsr() const;

    // Writes the whole CPSR, switching the register bank when the mode changes.
    void SetCpsr(uint32_t value);

    // The SPSR of the current mode (none in user and system mode: 0).
    uint32_t Spsr() const;

    // The address of the next instruction, with bit 0 set in Thumb state.
    uint32_t Pc() const;

    // ARMv5TE: CP15 register c1 (the control register) and the TCMs. The data TCM's base and the sizes follow the
    // c9 writes. The memories are part of the CPU: they take precedence over the bus where they are enabled.
    uint32_t Cp15Control() const;
    std::span<uint8_t> Itcm();
    std::span<uint8_t> Dtcm();
    uint32_t DtcmBase() const;

    // A data access as a load or a store makes it: in a TCM where one serves the address, otherwise on the bus. The
    // address is aligned to the size of 1, 2 or 4 bytes. A high-level BIOS reaches memory this way.
    uint32_t Load(uint32_t address, int size);
    void Store(uint32_t address, int size, uint32_t value);

    // Forgets the pages that the bus gave. The bus calls this when a page's mapping changes.
    void InvalidatePages();

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace ntdlib::arm
