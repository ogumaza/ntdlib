// SPDX-License-Identifier: MIT

// Calls a Wii game's functions in its main program on ntdlib's Broadway interpreter, one at a time, outside game mode.
// There's no OS and no device: the functions must use only memory. A test can compare a model with the game's code this
// way, function by function, with the same inputs.
//
// The DOL's sections are loaded into MEM1. The CPU runs as the game's OS leaves it: MEM1 and MEM2 mapped by the BATs at
// their cached addresses, floating point and paired singles on, every GQR 0, round to nearest. r2 and r13 get the
// small-data bases that the game's start-up code sets. Each call starts with an empty stack at the top of MEM1 and
// returns to a stop address. Two stub functions serve as a game's allocation hooks, such as AXFXSetHooks takes: the
// first takes a size and returns zeroed memory in MEM2, the second frees nothing.
//
// The header also defines the PowerPC encodings for a test's stand-ins of the game's functions, and a loader of the REL
// modules that a game loads after its main program. A test that includes it links ntdlib::ppc, ntdlib::dsp and
// ntdlib::disc.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/files.h"

#include "ntdlib/disc/cx.h"
#include "ntdlib/dol.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/ppc/cpu.h"
#include "ntdlib/types.h"

namespace ntdlib::testing
{

// blr: the return from a function.
constexpr uint32_t kBlr = 0x4e800020;

// The upper half of `address` for a lis, adjusted for the sign of the lower half (@ha). An addi or a load with
// Low(address) then reaches `address`.
inline uint32_t High(uint32_t address)
{
    return (address + 0x8000) >> 16;
}

// The lower half of `address` (@l).
inline uint32_t Low(uint32_t address)
{
    return address & 0xffff;
}

// A branch (b) at `from` to `to`. The two addresses must be within 32 MiB of each other.
inline uint32_t Branch(uint32_t from, uint32_t to)
{
    return 0x48000000 | ((to - from) & 0x03fffffc);
}

class Guest : public ppc::Bus
{
public:
    // The address that calls return to, and the two allocation stubs. They're in MEM1 above the stack.
    static constexpr uint32_t kReturn = 0x817ff000;
    static constexpr uint32_t kAllocHook = 0x817ff010;
    static constexpr uint32_t kFreeHook = 0x817ff020;

    // The largest decompressed main program that the guest loads: MEM1's size.
    static constexpr std::size_t kMaxProgramSize = 24u << 20;

    // Loads `dol`: a main.dol, or a title's main program. A title may store the program compressed with LZ77. The
    // guest then loads it decompressed. Throws std::runtime_error if the file can't be read or isn't a DOL.
    explicit Guest(const std::filesystem::path& dol) : cpu_(*this)
    {
        if (!xsflib::ReadFile(dol, file_))
        {
            throw std::runtime_error("can't read " + xsflib::Utf8(dol));
        }
        if (disc::IsCx(file_))
        {
            file_ = disc::DecompressCx(file_, kMaxProgramSize);
        }

        const std::optional<Dol> parsed = Dol::Parse(file_);
        if (!parsed)
        {
            throw std::runtime_error(dol.string() + " isn't a DOL");
        }

        for (const Dol::Section& section : parsed->Sections())
        {
            std::memcpy(Bytes(section.address, static_cast<uint32_t>(section.data.size())), section.data.data(),
                        section.data.size());
        }

        FindSmallDataBases(*parsed);
        Write32(kReturn, kBranchToSelf);
        Write32(kAllocHook, kBlr);
        Write32(kFreeHook, kBlr);
        SetUpCpu();
    }

    // Calls the function at `address` with up to eight integer arguments in r3 to r10. Returns r3. Throws
    // std::runtime_error if the function faults, runs an instruction that the interpreter doesn't support, or doesn't
    // return within `budget` instructions.
    uint32_t Call(uint32_t address, std::initializer_list<uint32_t> args = {}, uint64_t budget = 100'000'000)
    {
        ppc::State& s = cpu_.state_;
        int reg = 3;
        for (const uint32_t arg : args)
        {
            s.gpr[reg++] = arg;
        }

        s.gpr[1] = kStackTop;
        s.gpr[2] = sda2_base_;
        s.gpr[13] = sda_base_;
        s.lr = kReturn;
        s.pc = address;
        Write32(kStackTop, 0);
        Write32(kStackTop + 4, 0);

        const auto on_watch = [this](uint32_t pc)
        {
            ppc::State& state = cpu_.state_;
            if (pc == kReturn)
            {
                cpu_.LimitRun(cpu_.Cycles());
            }
            else if (pc == kAllocHook)
            {
                state.gpr[3] = Alloc(state.gpr[3]);
                state.pc = state.lr;
            }
            else if (pc == kFreeHook)
            {
                state.pc = state.lr;
            }
        };
        cpu_.Watch({kReturn, kAllocHook, kFreeHook}, on_watch);

        const uint64_t start = cpu_.Cycles();
        const ppc::StopReason reason = cpu_.Run(budget);
        last_cycles_ = cpu_.Cycles() - start;
        if (reason != ppc::StopReason::kBudget || s.pc != kReturn)
        {
            throw std::runtime_error("the call of " + Hex(address) + " stopped at " + Hex(cpu_.stop_pc_) + ": " +
                                     cpu_.Describe());
        }

        return s.gpr[3];
    }

    // Calls the game's function at `set_hooks`, such as AXFXSetHooks, with the two allocation stubs.
    void SetHooks(uint32_t set_hooks)
    {
        Call(set_hooks, {kAllocHook, kFreeHook});
    }

    // The instructions that the last call ran.
    uint64_t LastCycles() const
    {
        return last_cycles_;
    }

    // Allocates `size` bytes of zeroed memory in MEM2, 32-byte aligned. Returns its cached address.
    uint32_t Alloc(uint32_t size)
    {
        const uint32_t address = heap_;
        heap_ += (size + 31) & ~31u;
        if (heap_ > kHeapEnd)
        {
            throw std::runtime_error("the guest heap is full");
        }

        return address;
    }

    // Big-endian reads and writes of memory at a cached address.
    uint32_t Read32(uint32_t address)
    {
        return xsflib::LoadBe32(Bytes(address, 4));
    }

    void Write32(uint32_t address, uint32_t value)
    {
        xsflib::StoreBe32(Bytes(address, 4), value);
    }

    // The `size` bytes at a cached address. Throws std::runtime_error outside MEM1 and MEM2.
    uint8_t* Bytes(uint32_t address, uint32_t size)
    {
        uint8_t* bytes = memory_.At(dsp::ToPhysical(address), size);
        if (bytes == nullptr)
        {
            throw std::runtime_error("no memory at " + Hex(address));
        }

        return bytes;
    }

    float ReadFloat(uint32_t address)
    {
        const uint32_t bits = Read32(address);
        float value = 0.0f;
        std::memcpy(&value, &bits, 4);

        return value;
    }

    void WriteFloat(uint32_t address, float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        Write32(address, bits);
    }

    ppc::Cpu& Cpu()
    {
        return cpu_;
    }

    // Bus.
    uint8_t* RamPage(PAddr page) override
    {
        if (page < dsp::kMem1Size || (page >= dsp::kMem2Base && page < dsp::kMem2Base + dsp::kMem2Size))
        {
            return memory_.At(page, 0x1000);
        }

        return nullptr;
    }

    bool ReadIo(PAddr /*address*/, int /*size*/, uint32_t& /*value*/) override
    {
        return false;
    }

    bool WriteIo(PAddr /*address*/, int /*size*/, uint32_t /*value*/) override
    {
        return false;
    }

private:
    static constexpr uint32_t kBranchToSelf = 0x48000000;
    static constexpr uint32_t kStackTop = 0x817fe000;
    static constexpr uint32_t kHeapStart = 0x90100000;
    static constexpr uint32_t kHeapEnd = 0x93000000;

    static std::string Hex(uint32_t value)
    {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08x", static_cast<unsigned>(value));

        return text;
    }

    // __init_registers sets r2 and r13 with a lis and an ori (or addi) each. The first such pair in the text sections
    // is that function's.
    void FindSmallDataBases(const Dol& dol)
    {
        bool found_r2 = false;
        bool found_r13 = false;
        for (const Dol::Section& section : dol.Sections())
        {
            for (std::size_t i = 0; i + 8 <= section.data.size() && !(found_r2 && found_r13); i += 4)
            {
                const uint32_t high = xsflib::LoadBe32(&section.data[i]);
                const uint32_t low = xsflib::LoadBe32(&section.data[i + 4]);
                const auto base = [&](uint32_t lis, uint32_t ori, uint32_t addi) -> std::optional<uint32_t>
                {
                    if (high >> 16 != lis)
                    {
                        return std::nullopt;
                    }

                    if (low >> 16 == ori)
                    {
                        return (high << 16) | (low & 0xffff);
                    }

                    if (low >> 16 == addi)
                    {
                        return (high << 16) + static_cast<uint32_t>(static_cast<int16_t>(low & 0xffff));
                    }

                    return std::nullopt;
                };

                if (const auto r2 = base(0x3c40, 0x6042, 0x3842); r2 && !found_r2)
                {
                    sda2_base_ = *r2;
                    found_r2 = true;
                }

                if (const auto r13 = base(0x3da0, 0x61ad, 0x39ad); r13 && !found_r13)
                {
                    sda_base_ = *r13;
                    found_r13 = true;
                }
            }
        }

        if (!found_r2 || !found_r13)
        {
            throw std::runtime_error("the DOL doesn't set r2 and r13 as __init_registers does");
        }
    }

    // The state that the game's OS leaves: the BATs for MEM1 (cached at 0x80000000, uncached at 0xc0000000) and MEM2
    // (0x90000000 and 0xd0000000), translation and floating point on, paired singles and quantized loads on.
    void SetUpCpu()
    {
        ppc::State& s = cpu_.state_;
        s.msr = ppc::msr::kIr | ppc::msr::kDr | ppc::msr::kRi | ppc::msr::kFp;
        s.spr[ppc::spr::kHid2] = 0xa0000000; // LSQE and PSE
        s.spr[ppc::spr::kHid4] = 0x82000000; // SBE: BATs 4-7 on
        SetBat(s, ppc::spr::kIbat0u, 0x80001fff, 0x00000002);
        SetBat(s, ppc::spr::kIbat0u + 8, 0x80001fff, 0x00000002);
        SetBat(s, ppc::spr::kIbat0u + 10, 0xc0001fff, 0x0000002a);
        SetBat(s, ppc::spr::kIbat4u, 0x90001fff, 0x10000002);
        SetBat(s, ppc::spr::kIbat4u + 8, 0x90001fff, 0x10000002);
        SetBat(s, ppc::spr::kIbat4u + 10, 0xd0001fff, 0x1000002a);
        cpu_.InvalidateTranslation();
    }

    static void SetBat(ppc::State& s, int base, uint32_t upper, uint32_t lower)
    {
        s.spr[base] = upper;
        s.spr[base + 1] = lower;
    }

    std::vector<uint8_t> file_;
    dsp::WiiMemory memory_;
    ppc::Cpu cpu_;
    uint32_t sda_base_ = 0;
    uint32_t sda2_base_ = 0;
    uint32_t heap_ = kHeapStart;
    uint64_t last_cycles_ = 0;
};

// Loads the REL module `rel` into `guest` from `address` and applies its relocations against itself and the main
// program, as the game's loader does. The executable sections come first, then the other sections with data, then those
// without data (.bss). Each starts on a 32-byte boundary. Returns each section's address, or 0 for an empty section.
// Throws std::runtime_error if the module ends early, imports another module or has a relocation of another type than
// R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA, R_PPC_REL24 and R_PPC_REL14.
inline std::vector<uint32_t> LoadRel(Guest& guest, std::span<const uint8_t> rel, uint32_t address)
{
    const auto u32 = [&rel](std::size_t at)
    {
        if (at + 4 > rel.size())
        {
            throw std::runtime_error("the module ends early");
        }

        return xsflib::LoadBe32(&rel[at]);
    };
    const uint32_t id = u32(0);
    const uint32_t count = u32(0xc);
    const uint32_t table = u32(0x10);
    const uint32_t imports = u32(0x28);
    const uint32_t imports_size = u32(0x2c);

    std::vector<uint32_t> bases(count, 0);
    uint32_t next = address;
    for (int pass = 0; pass < 3; pass++)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t offset = u32(table + 8 * i);
            const uint32_t size = u32(table + 8 * i + 4);
            const bool executable = (offset & 1) != 0;
            const bool has_data = (offset & ~1u) != 0;
            const int section_pass = executable ? 0 : has_data ? 1 : 2;
            if (size == 0 || section_pass != pass)
            {
                continue;
            }

            next = (next + 0x1f) & ~0x1fu;
            bases[i] = next;
            if (has_data)
            {
                const uint32_t from = offset & ~1u;
                if (from + size > rel.size())
                {
                    throw std::runtime_error("a section of the module ends past the file");
                }

                std::copy(rel.begin() + from, rel.begin() + from + size, guest.Bytes(next, size));
            }

            next += size;
        }
    }

    for (uint32_t k = 0; k < imports_size / 8; k++)
    {
        const uint32_t module = u32(imports + 8 * k);
        uint32_t at = u32(imports + 8 * k + 4);
        uint32_t section = 0;
        uint32_t position = 0;
        for (;; at += 8)
        {
            const uint32_t offset = u32(at) >> 16;
            const uint32_t type = (u32(at) >> 8) & 0xff;
            const uint32_t target_section = u32(at) & 0xff;
            const uint32_t addend = u32(at + 4);
            if (type == 203) // R_DOLPHIN_END
            {
                break;
            }

            if (type == 202) // R_DOLPHIN_SECTION
            {
                section = target_section;
                position = 0;
                continue;
            }

            position += offset;
            if (type == 201) // R_DOLPHIN_NOP
            {
                continue;
            }

            if (module != 0 && module != id)
            {
                throw std::runtime_error("the module imports another module");
            }

            const uint32_t target = module == 0 ? addend : bases.at(target_section) + addend;
            const uint32_t place = bases.at(section) + position;
            const uint32_t word = guest.Read32(place & ~3u);
            switch (type)
            {
            case 1: // R_PPC_ADDR32
                guest.Write32(place, target);
                break;
            case 4: // R_PPC_ADDR16_LO
            case 5: // R_PPC_ADDR16_HI
            case 6: // R_PPC_ADDR16_HA
                {
                    const uint32_t half = type == 4 ? target & 0xffff : type == 5 ? target >> 16 : High(target);
                    xsflib::StoreBe16(guest.Bytes(place, 2), static_cast<uint16_t>(half));
                    break;
                }
            case 10: // R_PPC_REL24
                guest.Write32(place, (word & 0xfc000003) | ((target - place) & 0x03fffffc));
                break;
            case 11: // R_PPC_REL14
                guest.Write32(place, (word & 0xffff0003) | ((target - place) & 0xfffc));
                break;
            default:
                throw std::runtime_error("the module has a relocation of type " + std::to_string(type));
            }
        }
    }

    return bases;
}

} // namespace ntdlib::testing
