// SPDX-License-Identifier: MIT

// The ARM interpreter's state, and the parts that the instruction handlers share: the registers with their banks, the
// memory accesses through the page caches and the TCMs, the flags, the branches and the exceptions. arm.cpp and
// thumb.cpp hold the handlers of the two instruction sets. cp15.cpp holds the system control coprocessor and the memory
// accesses that miss the page caches.

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/arm/cpu.h"
#include "ntdlib/arm/shifter.h"

namespace ntdlib::arm::detail
{

class Core;

// An instruction's handler. It gets the instruction's word, or a Thumb instruction's halfword.
using Handler = void (*)(Core& core, uint32_t op);

// The ARM handlers by bits 27-20 and 7-4 of the instruction, for the instructions whose condition field isn't 1111b.
const std::array<Handler, 4096>& ArmHandlers();

// The Thumb handlers by bits 15-6 of the instruction.
const std::array<Handler, 1024>& ThumbHandlers();

// Runs an ARM instruction whose condition field is 1111b.
void RunUnconditional(Core& core, uint32_t op);

constexpr uint32_t kPageBits = 12;
constexpr uint32_t kPageSize = 1u << kPageBits;
constexpr uint32_t kPageOffsetMask = kPageSize - 1;
constexpr uint32_t kPageCacheSize = 1024;

// Never a page number: those have 20 bits.
constexpr uint32_t kNoPage = 0xFFFFFFFF;

// The register banks, by mode. User and system mode share bank 0. Bank 0 has no SPSR.
constexpr int kUserBank = 0;
constexpr int kFiqBank = 1;
constexpr int kIrqBank = 2;
constexpr int kSupervisorBank = 3;
constexpr int kAbortBank = 4;
constexpr int kUndefinedBank = 5;
constexpr int kBanks = 6;

// The exception vectors' offsets from the vector base.
constexpr uint32_t kUndefinedVector = 0x04;
constexpr uint32_t kSwiVector = 0x08;
constexpr uint32_t kPrefetchAbortVector = 0x0C;
constexpr uint32_t kIrqVector = 0x18;

// The CP15 control register's bits that the interpreter acts on.
namespace control
{

constexpr uint32_t kHighVectors = 1u << 13;
constexpr uint32_t kPreArmV5 = 1u << 15; // LDR, LDM and POP don't switch to Thumb state on a loaded PC
constexpr uint32_t kDtcmEnable = 1u << 16;
constexpr uint32_t kDtcmLoadMode = 1u << 17;
constexpr uint32_t kItcmEnable = 1u << 18;
constexpr uint32_t kItcmLoadMode = 1u << 19;

} // namespace control

// The kinds of memory access. On ARMv5TE the TCMs serve them differently: instruction fetches see the ITCM but not
// the DTCM, and in a TCM's load mode the data reads skip that TCM.
enum class Access
{
    kFetch,
    kRead,
    kWrite,
};

// An entry of a page cache: the host memory of a 4 KiB page, or nullptr for I/O. A page that a TCM region covers in
// part is `mixed`, and each access to it checks the TCMs.
struct PageEntry
{
    uint32_t page = kNoPage;
    bool mixed = false;
    uint8_t* host = nullptr;
};

using PageCache = std::array<PageEntry, kPageCacheSize>;

// A tightly coupled memory: its storage, and the region of the address space where it appears. The region repeats
// the storage when it's larger.
struct Tcm
{
    std::vector<uint8_t> storage;
    uint32_t setting = 0;     // the CP15 register c9,c1 that sets the region
    uint32_t base = 0;        // the region's first address
    uint32_t region_mask = 0; // the address bits that select the region
    uint32_t span_mask = 0;   // the offset bits within the region that select a byte of the storage
    bool small = false;       // the region is smaller than a page
    bool enabled = false;
    bool load_mode = false; // data reads go to the bus, writes to the TCM
};

inline bool TcmContains(const Tcm& tcm, uint32_t address)
{
    return ((address ^ tcm.base) & tcm.region_mask) == 0;
}

inline uint8_t* TcmAt(Tcm& tcm, uint32_t address)
{
    return tcm.storage.data() + ((address - tcm.base) & tcm.span_mask);
}

inline uint32_t LoadHost(const uint8_t* p, int size)
{
    if (size == 4)
    {
        return xsflib::LoadLe32(p);
    }

    return size == 2 ? xsflib::LoadLe16(p) : *p;
}

inline void StoreHost(uint8_t* p, int size, uint32_t value)
{
    if (size == 4)
    {
        xsflib::StoreLe32(p, value);
    }
    else if (size == 2)
    {
        xsflib::StoreLe16(p, static_cast<uint16_t>(value));
    }
    else
    {
        *p = static_cast<uint8_t>(value);
    }
}

// a + b + carry_in, with the adder's carry and signed overflow. A subtraction a - b adds ~b with a carry in of 1.
inline uint32_t AddWithCarry(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t& carry, uint32_t& overflow)
{
    const uint64_t sum = static_cast<uint64_t>(a) + b + carry_in;
    const uint32_t result = static_cast<uint32_t>(sum);
    carry = static_cast<uint32_t>(sum >> 32);
    overflow = ((a ^ result) & (b ^ result)) >> 31;

    return result;
}

// Rotates a word that an unaligned LDR or SWP read: the byte at the address goes to the low byte.
inline uint32_t RotateLoaded(uint32_t word, uint32_t address)
{
    return std::rotr(word, static_cast<int>((address & 3) * 8));
}

// Whether condition `cond` (0 to 14) passes, by bit (NZCV flags) of each entry.
constexpr std::array<uint16_t, 16> ConditionTable()
{
    std::array<uint16_t, 16> table{};
    for (uint32_t cond = 0; cond < 16; ++cond)
    {
        for (uint32_t flags = 0; flags < 16; ++flags)
        {
            const bool n = (flags & 8) != 0;
            const bool z = (flags & 4) != 0;
            const bool c = (flags & 2) != 0;
            const bool v = (flags & 1) != 0;
            const bool passes[16] = {z,       !z,     c,      !c,           n,           !n,   v,    !v, c && !z,
                                     !c || z, n == v, n != v, !z && n == v, z || n != v, true, false};
            if (passes[cond])
            {
                table[cond] = static_cast<uint16_t>(table[cond] | (1u << flags));
            }
        }
    }

    return table;
}

inline constexpr std::array<uint16_t, 16> kConditions = ConditionTable();

// The interpreter's state. The members are public: the handlers in arm.cpp and thumb.cpp work on the registers and the
// flags directly.
class Core
{
public:
    Core(Cpu& cpu, Bus& bus, Architecture architecture);

    StopReason Run(uint64_t cycles);

    // The flags and the modes.
    uint32_t Carry() const;
    bool ConditionPassed(uint32_t cond) const;
    void SetNz(uint32_t result);
    void SetNzc(uint32_t result, uint32_t carry);
    void SetNzcv(uint32_t result, uint32_t carry, uint32_t overflow);
    bool Privileged() const;
    uint32_t Spsr() const;
    void WriteSpsr(uint32_t value, uint32_t mask);

    // Writes the CPSR, switching the register bank when the mode changes and the state when T changes.
    void WriteCpsr(uint32_t value);

    // Copies the SPSR to the CPSR to return from an exception. Nothing happens in a mode without an SPSR.
    void RestoreCpsr();

    // The registers r8 to r14 of user mode, whatever the mode is: LDM and STM with the S bit transfer them.
    uint32_t UserRegister(int index) const;
    void SetUserRegister(int index, uint32_t value);

    // Branches. A data-processing instruction or a load in ARMv4T stays in the current state, BX and BLX take the state
    // from bit 0, and a load of the PC on ARMv5TE does what BX does (LoadPc).
    void BranchInState(uint32_t target);
    void BranchExchange(uint32_t target);
    void LoadPc(uint32_t value);

    // The exceptions that instructions raise. Each takes the instruction's address from r15.
    void Undefined();
    void SoftwareInterrupt(uint32_t comment);
    void Breakpoint();

    void Halt();
    void UpdateAttention();

    // Memory. The addresses are aligned to the size, and the reads return the value zero-extended. Each access counts
    // a cycle.
    uint32_t Fetch32(uint32_t address);
    uint32_t Fetch16(uint32_t address);
    uint32_t Read32(uint32_t address);
    uint32_t Read16(uint32_t address);
    uint32_t Read8(uint32_t address);
    void Write32(uint32_t address, uint32_t value);
    void Write16(uint32_t address, uint32_t value);
    void Write8(uint32_t address, uint32_t value);

    // LDRH and LDRSH at any address, with the architecture's treatment of an odd one.
    uint32_t ReadHalfword(uint32_t address);
    uint32_t ReadSignedHalfword(uint32_t address);

    void InvalidatePages();
    uint32_t VectorBase() const;

    // CP15 register transfers. A read returns false for a register that raises the undefined instruction exception.
    bool ReadCp15(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t& value);
    bool WriteCp15(uint32_t opcode1, uint32_t crn, uint32_t crm, uint32_t opcode2, uint32_t value);

    Cpu& cpu_;
    Bus& bus_;
    const Architecture architecture_;
    const bool v5_;
    const std::array<Handler, 4096>& arm_handlers_;
    const std::array<Handler, 1024>& thumb_handlers_;

    // The registers of the current mode, the other banks, and the PSRs. While an instruction runs, r15 holds its
    // address plus 8 (ARM) or 4 (Thumb). next_pc_ holds the address of the next instruction, and a branch sets it.
    // psr_bits_ holds the PSR bits that exist: NZCV, the control bits, and the Q flag on ARMv5TE.
    std::array<uint32_t, 16> regs_{};
    uint32_t next_pc_ = 0;
    uint32_t cpsr_ = 0;
    uint32_t psr_bits_ = 0;
    bool thumb_ = false;
    std::array<std::array<uint32_t, 2>, kBanks> r13_r14_{};
    std::array<uint32_t, 5> user_r8_r12_{};
    std::array<uint32_t, 5> fiq_r8_r12_{};
    std::array<uint32_t, kBanks> spsr_{};
    uint64_t cycles_ = 0;

    // Run control. `attention_` is set while any of the flags after it is.
    bool attention_ = false;
    bool irq_line_ = false;
    bool halted_ = false;
    bool stop_requested_ = false;
    bool undefined_stop_ = false;
    uint32_t stop_address_ = 0xFFFFFFFF;
    bool vectors_handled_ = true;
    std::function<bool(Cpu&, uint32_t)> swi_handler_;

    // The page caches, and the page of the last instruction fetch.
    uint32_t fetch_page_ = kNoPage;
    uint8_t* fetch_host_ = nullptr;
    PageCache fetch_pages_;
    PageCache read_pages_;
    PageCache write_pages_;

    // CP15 and the TCMs (ARMv5TE).
    uint32_t control_ = 0;
    bool load_interworks_ = false;
    Tcm itcm_;
    Tcm dtcm_;
    std::array<uint32_t, 2> cachable_{};
    uint32_t write_bufferable_ = 0;
    std::array<uint32_t, 4> permissions_{};
    std::array<uint32_t, 8> regions_{};
    std::array<uint32_t, 2> lockdown_{};
    uint32_t trace_process_id_ = 0;

private:
    int BankOf(uint32_t mode) const;
    void SwitchBank(int from, int to);
    void EnterException(Mode mode, uint32_t vector, uint32_t return_address);
    void TakeIrq();
    void Step();

    // The paths of cp15.cpp for an access that misses the page caches, or whose page has no host memory.
    uint32_t SlowFetch(uint32_t address, int size);
    uint32_t SlowRead(uint32_t address, int size);
    void SlowWrite(uint32_t address, int size, uint32_t value);
    void Fill(PageEntry& entry, uint32_t page, Access access);
    uint8_t* TcmFor(uint32_t address, Access access);
    uint32_t ReadUncached(const PageEntry& entry, uint32_t address, int size, Access access);
    void UpdateTcms();
};

inline uint32_t Core::Carry() const
{
    return (cpsr_ >> 29) & 1;
}

inline bool Core::ConditionPassed(uint32_t cond) const
{
    return ((kConditions[cond] >> (cpsr_ >> 28)) & 1) != 0;
}

inline void Core::SetNz(uint32_t result)
{
    cpsr_ = (cpsr_ & 0x3FFFFFFF) | (result & psr::kN) | (result == 0 ? psr::kZ : 0);
}

inline void Core::SetNzc(uint32_t result, uint32_t carry)
{
    cpsr_ = (cpsr_ & 0x1FFFFFFF) | (result & psr::kN) | (result == 0 ? psr::kZ : 0) | (carry << 29);
}

inline void Core::SetNzcv(uint32_t result, uint32_t carry, uint32_t overflow)
{
    cpsr_ = (cpsr_ & 0x0FFFFFFF) | (result & psr::kN) | (result == 0 ? psr::kZ : 0) | (carry << 29) | (overflow << 28);
}

inline void Core::BranchInState(uint32_t target)
{
    next_pc_ = target & (thumb_ ? ~1u : ~3u);
}

inline uint32_t Core::Fetch32(uint32_t address)
{
    if ((address >> kPageBits) == fetch_page_)
    {
        return xsflib::LoadLe32(fetch_host_ + (address & kPageOffsetMask));
    }

    return SlowFetch(address, 4);
}

inline uint32_t Core::Fetch16(uint32_t address)
{
    if ((address >> kPageBits) == fetch_page_)
    {
        return xsflib::LoadLe16(fetch_host_ + (address & kPageOffsetMask));
    }

    return SlowFetch(address, 2);
}

inline uint32_t Core::Read32(uint32_t address)
{
    ++cycles_;
    const uint32_t page = address >> kPageBits;
    const PageEntry& entry = read_pages_[page % kPageCacheSize];
    if (entry.page == page && entry.host != nullptr)
    {
        return xsflib::LoadLe32(entry.host + (address & kPageOffsetMask));
    }

    return SlowRead(address, 4);
}

inline uint32_t Core::Read16(uint32_t address)
{
    ++cycles_;
    const uint32_t page = address >> kPageBits;
    const PageEntry& entry = read_pages_[page % kPageCacheSize];
    if (entry.page == page && entry.host != nullptr)
    {
        return xsflib::LoadLe16(entry.host + (address & kPageOffsetMask));
    }

    return SlowRead(address, 2);
}

inline uint32_t Core::Read8(uint32_t address)
{
    ++cycles_;
    const uint32_t page = address >> kPageBits;
    const PageEntry& entry = read_pages_[page % kPageCacheSize];
    if (entry.page == page && entry.host != nullptr)
    {
        return entry.host[address & kPageOffsetMask];
    }

    return SlowRead(address, 1);
}

inline void Core::Write32(uint32_t address, uint32_t value)
{
    ++cycles_;
    const uint32_t page = address >> kPageBits;
    const PageEntry& entry = write_pages_[page % kPageCacheSize];
    if (entry.page == page && entry.host != nullptr)
    {
        xsflib::StoreLe32(entry.host + (address & kPageOffsetMask), value);
        return;
    }

    SlowWrite(address, 4, value);
}

inline void Core::Write16(uint32_t address, uint32_t value)
{
    ++cycles_;
    const uint32_t page = address >> kPageBits;
    const PageEntry& entry = write_pages_[page % kPageCacheSize];
    if (entry.page == page && entry.host != nullptr)
    {
        xsflib::StoreLe16(entry.host + (address & kPageOffsetMask), static_cast<uint16_t>(value));
        return;
    }

    SlowWrite(address, 2, value & 0xFFFF);
}

inline void Core::Write8(uint32_t address, uint32_t value)
{
    ++cycles_;
    const uint32_t page = address >> kPageBits;
    const PageEntry& entry = write_pages_[page % kPageCacheSize];
    if (entry.page == page && entry.host != nullptr)
    {
        entry.host[address & kPageOffsetMask] = static_cast<uint8_t>(value);
        return;
    }

    SlowWrite(address, 1, value & 0xFF);
}

inline uint32_t Core::ReadHalfword(uint32_t address)
{
    const uint32_t value = Read16(address & ~1u);
    if (v5_ || (address & 1) == 0)
    {
        return value;
    }

    // The ARM7TDMI reads the aligned halfword and rotates the word right by 8.
    return std::rotr(value, 8);
}

inline uint32_t Core::ReadSignedHalfword(uint32_t address)
{
    if (!v5_ && (address & 1) != 0)
    {
        // The ARM7TDMI loads the signed byte at an odd address.
        return static_cast<uint32_t>(static_cast<int8_t>(Read8(address)));
    }

    return static_cast<uint32_t>(static_cast<int16_t>(Read16(address & ~1u)));
}

} // namespace ntdlib::arm::detail
