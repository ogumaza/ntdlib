// SPDX-License-Identifier: MIT

// The memory that the DSP reaches on each console: WiiMemory, GameCubeMemory and WiiUMemory. Dsp takes one of them as
// its template parameter. Each provides:
// - At(address, size): main memory at the addresses that the CPU and the DSP's DMA use, or nullptr unless the whole
//   range lies in one region of it;
// - Aram(address, size): the memory behind the DSP's accelerator and the ARAM DMA, or nullptr outside it;
// - on the GameCube and the Wii, kAcceleratorReadsMem1: whether an accelerator address with bit 28 clear reads MEM1
//   rather than ARAM;
// - the DSP's layout on that console, as in DspLayout.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

#include "ntdlib/types.h"
#include "xsflib/memory_image.h"

namespace ntdlib::dsp
{

constexpr PAddr kMem1Base = 0x00000000;
constexpr std::size_t kMem1Size = 24 * 1024 * 1024;
constexpr PAddr kMem2Base = 0x10000000;
constexpr std::size_t kMem2Size = 64 * 1024 * 1024;
constexpr std::size_t kAramSize = 16 * 1024 * 1024;

// The CPU addresses memory through cached (0x80000000 and up) and uncached (0xc0000000 and up) mirrors of the physical
// address space (OSCachedToPhysical, OSPhysicalToCached).
constexpr PAddr ToPhysical(uint32_t address)
{
    return address & 0x1fffffff;
}

constexpr uint32_t ToCached(PAddr address)
{
    return address | 0x80000000;
}

namespace detail
{

struct Free
{
    void operator()(uint8_t* bytes) const
    {
        std::free(bytes);
    }
};

using Bytes = std::unique_ptr<uint8_t, Free>;

// Zeroed memory from calloc. calloc leaves the zeroing to the system's lazy mapping of pages.
inline Bytes Allocate(std::size_t size)
{
    Bytes bytes(static_cast<uint8_t*>(std::calloc(size, 1)));
    if (!bytes)
    {
        throw std::bad_alloc();
    }

    return bytes;
}

} // namespace detail

// The DSP's layout on the GameCube and the Wii: the sizes of instruction and data RAM, the coefficient ROM's place in
// data memory, whether two registers extend main-memory addresses, and whether the boot ROM's routines run.
struct DspLayout
{
    // Instruction and data RAM, in words.
    static constexpr uint16_t kIramSize = 0x1000;
    static constexpr uint16_t kDramSize = 0x1000;

    // The data addresses of the coefficient ROM. Its 0x800 words repeat to the end of the range. The range starts at a
    // multiple of 0x800.
    static constexpr uint16_t kCoefficientRomBase = 0x1000;
    static constexpr uint16_t kCoefficientRomEnd = 0x2000;

    // Whether registers 0xFFBA and 0xFFBC give the top three bits of the main-memory addresses of DMA and the
    // accelerator. Without them, DMA's addresses have 30 bits.
    static constexpr bool kAddressExtensions = false;

    // Whether the boot ROM's routines run (boot_rom.cpp). Without them, a program that leaves IRAM halts the DSP.
    static constexpr bool kBootRom = true;
};

// The Wii's main memory: MEM1 (24 MiB at physical 0) and MEM2 (64 MiB at physical 0x10000000). The CPU and the DSP
// share it. AX keeps its parameter blocks, command lists and output buffers here. The DSP's accelerator reads wave data
// from it. In archive mode the NW4R SND model stores the sound archive's waves in MEM2.
class WiiMemory : public DspLayout
{
public:
    // The memory's contents, for a snapshot of the system that holds it.
    struct Snapshot
    {
        xsflib::MemoryImage mem1;
        xsflib::MemoryImage mem2;
        std::size_t mem2_used = 0;
    };

    // On the Wii, "ARAM" is MEM2. The accelerator reads MEM1 for an address with bit 28 clear.
    static constexpr bool kAcceleratorReadsMem1 = true;

    // Both start zeroed. A model that touches little of the memory is cheap to create.
    WiiMemory() : mem1_(detail::Allocate(kMem1Size)), mem2_(detail::Allocate(kMem2Size))
    {
    }

    // Takes a snapshot. If `previous` is given, the snapshot shares unchanged pages with it.
    Snapshot Save(const Snapshot* previous = nullptr) const
    {
        return {xsflib::MemoryImage(mem1_.get(), kMem1Size, previous ? &previous->mem1 : nullptr),
                xsflib::MemoryImage(mem2_.get(), kMem2Size, previous ? &previous->mem2 : nullptr), mem2_used_};
    }

    void Restore(const Snapshot& snapshot)
    {
        snapshot.mem1.Restore(mem1_.get());
        snapshot.mem2.Restore(mem2_.get());
        mem2_used_ = snapshot.mem2_used;
    }

    // The `size` bytes at physical address `address`, or nullptr if any of them is outside MEM1 and MEM2.
    uint8_t* At(PAddr address, std::size_t size = 1)
    {
        if (address < kMem1Base + kMem1Size)
        {
            return address + size <= kMem1Base + kMem1Size ? mem1_.get() + (address - kMem1Base) : nullptr;
        }
        if (address >= kMem2Base && address - kMem2Base + size <= kMem2Size)
        {
            return mem2_.get() + (address - kMem2Base);
        }

        return nullptr;
    }

    const uint8_t* At(PAddr address, std::size_t size = 1) const
    {
        return const_cast<WiiMemory*>(this)->At(address, size);
    }

    // The `size` bytes at `address` in MEM2, or nullptr if any of them is outside it.
    uint8_t* Aram(uint32_t address, std::size_t size = 1)
    {
        return At(kMem2Base + address, size);
    }

    const uint8_t* Aram(uint32_t address, std::size_t size = 1) const
    {
        return const_cast<WiiMemory*>(this)->Aram(address, size);
    }

    // Copies `size` bytes into the next free 32-byte aligned space in MEM2 and returns their physical address. This is
    // bump allocation, for the waves that the NW4R SND model loads. Throws when MEM2 is full.
    PAddr Store(const void* data, std::size_t size)
    {
        const std::size_t offset = (mem2_used_ + 31) / 32 * 32;
        if (offset + size > kMem2Size)
        {
            throw std::runtime_error("MEM2 model exhausted");
        }

        std::memcpy(mem2_.get() + offset, data, size);
        mem2_used_ = offset + size;

        return kMem2Base + static_cast<PAddr>(offset);
    }

private:
    detail::Bytes mem1_;
    detail::Bytes mem2_;
    std::size_t mem2_used_ = 0;
};

// The GameCube's memory: main memory (24 MiB at physical 0) and ARAM (16 MiB of auxiliary memory). The CPU and the
// DSP's DMA share main memory. The DSP's accelerator reads samples from ARAM, and the CPU moves data in and out of ARAM
// with the ARAM DMA. JAudio keeps its wave archives in ARAM.
class GameCubeMemory : public DspLayout
{
public:
    // The memory's contents, for a snapshot of the system that holds it.
    struct Snapshot
    {
        xsflib::MemoryImage mem1;
        xsflib::MemoryImage aram;
    };

    // The accelerator reads ARAM only.
    static constexpr bool kAcceleratorReadsMem1 = false;

    // Both start zeroed. A system that touches little of the memory is cheap to create.
    GameCubeMemory() : mem1_(detail::Allocate(kMem1Size)), aram_(detail::Allocate(kAramSize))
    {
    }

    // Takes a snapshot. If `previous` is given, the snapshot shares unchanged pages with it.
    Snapshot Save(const Snapshot* previous = nullptr) const
    {
        return {xsflib::MemoryImage(mem1_.get(), kMem1Size, previous ? &previous->mem1 : nullptr),
                xsflib::MemoryImage(aram_.get(), kAramSize, previous ? &previous->aram : nullptr)};
    }

    void Restore(const Snapshot& snapshot)
    {
        snapshot.mem1.Restore(mem1_.get());
        snapshot.aram.Restore(aram_.get());
    }

    // The `size` bytes at physical address `address` of main memory, or nullptr if any of them lies outside it.
    uint8_t* At(PAddr address, std::size_t size = 1)
    {
        static_assert(kMem1Base == 0);
        if (address > kMem1Size || size > kMem1Size - address)
        {
            return nullptr;
        }

        return mem1_.get() + address;
    }

    const uint8_t* At(PAddr address, std::size_t size = 1) const
    {
        return const_cast<GameCubeMemory*>(this)->At(address, size);
    }

    // The `size` bytes at `address` in ARAM, or nullptr if any of them lies outside it.
    uint8_t* Aram(uint32_t address, std::size_t size = 1)
    {
        if (address > kAramSize || size > kAramSize - address)
        {
            return nullptr;
        }

        return aram_.get() + address;
    }

    const uint8_t* Aram(uint32_t address, std::size_t size = 1) const
    {
        return const_cast<GameCubeMemory*>(this)->Aram(address, size);
    }

private:
    detail::Bytes mem1_;
    detail::Bytes aram_;
};

// The Wii U's memory at its DSP's addresses: one flat range of physical addresses from 0, of the size the caller gives.
// The Wii U's DSP is the Wii's DSP, extended: 0x2000 words of instruction RAM and 0x3000 of data RAM, the coefficient
// ROM at data address 0x3000, and two registers that give the top three bits of a main-memory address, 0xFFBA for DMA
// and 0xFFBC for the accelerator. The accelerator reads main memory, and there's no ARAM. No boot ROM is modelled.
class WiiUMemory
{
public:
    static constexpr uint16_t kIramSize = 0x2000;
    static constexpr uint16_t kDramSize = 0x3000;
    static constexpr uint16_t kCoefficientRomBase = 0x3000;
    static constexpr uint16_t kCoefficientRomEnd = 0x3800;
    static constexpr bool kAddressExtensions = true;
    static constexpr bool kBootRom = false;

    // Starts zeroed.
    explicit WiiUMemory(std::size_t size) : bytes_(detail::Allocate(size)), size_(size)
    {
    }

    // The `size` bytes at physical address `address`, or nullptr if any of them lies outside the memory.
    uint8_t* At(PAddr address, std::size_t size = 1)
    {
        if (address > size_ || size > size_ - address)
        {
            return nullptr;
        }

        return bytes_.get() + address;
    }

    const uint8_t* At(PAddr address, std::size_t size = 1) const
    {
        return const_cast<WiiUMemory*>(this)->At(address, size);
    }

    // There's no ARAM.
    uint8_t* Aram(uint32_t /*address*/, std::size_t /*size*/ = 1)
    {
        return nullptr;
    }

    const uint8_t* Aram(uint32_t /*address*/, std::size_t /*size*/ = 1) const
    {
        return nullptr;
    }

private:
    detail::Bytes bytes_;
    std::size_t size_;
};

} // namespace ntdlib::dsp
