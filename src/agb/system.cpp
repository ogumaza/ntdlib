// SPDX-License-Identifier: MIT

#include "ntdlib/agb/system.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ntdlib::agb
{
namespace
{

constexpr uint32_t kPageSize = 0x1000;

// A DMA channel's registers, from kRegDma0Source: the source, the destination, the count and the control.
constexpr uint32_t kDmaStride = 12;
constexpr uint32_t kDmaControl = 10;
constexpr uint16_t kDmaEnable = 0x8000;
constexpr uint16_t kDmaTiming = 0x3000; // 0: at once
constexpr uint16_t kDmaWords = 0x0400;

// What the BIOS's GetBiosChecksum gives on the GBA, by GBATEK. The DS's mode of the GBA gives another.
constexpr uint32_t kBiosChecksum = 0xBAAE187F;

uint32_t ReadLe(const uint8_t* p, int size)
{
    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= uint32_t(p[i]) << (8 * i);
    }

    return value;
}

void WriteLe(uint8_t* p, int size, uint32_t value)
{
    for (int i = 0; i < size; i++)
    {
        p[i] = uint8_t(value >> (8 * i));
    }
}

} // namespace

System::System(const Rom& rom) : bios_(kBiosSize), ewram_(kEwramSize), iwram_(kIwramSize), io_(kIoSize)
{
    if (rom.Size() > kMaxRomSize)
    {
        throw std::invalid_argument("the ROM image is larger than the GBA's 32 MiB");
    }

    // The image fills whole pages. The rest of the last page reads as 0.
    rom_.assign(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    rom_.resize((rom_.size() + kPageSize - 1) & ~size_t(kPageSize - 1));

    cpu_ = std::make_unique<arm::Cpu>(*this, arm::Architecture::kArmV4T);
    cpu_->SetVectorsHandled(false);
    cpu_->SetSwiHandler([this](arm::Cpu& cpu, uint32_t comment) { return Swi(cpu, comment); });
}

System::~System() = default;

arm::Cpu& System::Cpu()
{
    return *cpu_;
}

uint32_t System::Call(uint32_t address, std::span<const uint32_t> args)
{
    failure_.clear();
    cpu_->SetCpsr(uint32_t(arm::Mode::kSystem) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kStack);
    try
    {
        return cpu_->Call(address, args, kCallCycles);
    }
    catch (const std::runtime_error&)
    {
        if (!failure_.empty())
        {
            throw std::runtime_error(failure_);
        }
        throw;
    }
}

uint32_t System::Load(uint32_t address, int size)
{
    address &= ~uint32_t(size - 1);
    if (uint8_t* page = Page(address & ~(kPageSize - 1)))
    {
        return ReadLe(page + (address & (kPageSize - 1)), size);
    }

    return Read(address, size);
}

void System::Store(uint32_t address, int size, uint32_t value)
{
    address &= ~uint32_t(size - 1);
    if (uint8_t* page = Page(address & ~(kPageSize - 1)))
    {
        WriteLe(page + (address & (kPageSize - 1)), size, value);
        return;
    }

    Write(address, size, value);
}

void System::HoldVcount(std::optional<int> line)
{
    held_vcount_ = line;
}

System::Snapshot System::Save(const Snapshot* previous) const
{
    Snapshot snapshot;
    size_t index = 0;
    const auto add = [&](const std::vector<uint8_t>& memory)
    {
        for (size_t at = 0; at < memory.size(); at += kPageSize, index++)
        {
            const size_t size = std::min<size_t>(kPageSize, memory.size() - at);
            const auto first = memory.begin() + ptrdiff_t(at);
            if (previous && index < previous->pages_.size() && previous->pages_[index]->size() == size &&
                std::equal(first, first + ptrdiff_t(size), previous->pages_[index]->begin()))
            {
                snapshot.pages_.push_back(previous->pages_[index]);
            }
            else
            {
                snapshot.pages_.push_back(std::make_shared<const std::vector<uint8_t>>(first, first + ptrdiff_t(size)));
            }
        }
    };

    add(ewram_);
    add(iwram_);
    add(io_);
    for (int r = 0; r < 13; r++)
    {
        snapshot.registers_[size_t(r)] = cpu_->Reg(r);
    }
    snapshot.registers_[13] = cpu_->Reg(14);
    snapshot.vcount_ = vcount_;
    snapshot.held_vcount_ = held_vcount_;
    snapshot.unmapped_ = unmapped_;

    return snapshot;
}

void System::Restore(const Snapshot& snapshot)
{
    size_t index = 0;
    const auto take = [&](std::vector<uint8_t>& memory)
    {
        for (size_t at = 0; at < memory.size(); at += kPageSize, index++)
        {
            const std::vector<uint8_t>& page = *snapshot.pages_.at(index);
            std::copy(page.begin(), page.end(), memory.begin() + ptrdiff_t(at));
        }
    };

    take(ewram_);
    take(iwram_);
    take(io_);
    cpu_->SetCpsr(uint32_t(arm::Mode::kSystem) | arm::psr::kI | arm::psr::kF);
    for (int r = 0; r < 13; r++)
    {
        cpu_->SetReg(r, snapshot.registers_[size_t(r)]);
    }
    cpu_->SetReg(14, snapshot.registers_[13]);
    vcount_ = snapshot.vcount_;
    held_vcount_ = snapshot.held_vcount_;
    unmapped_ = snapshot.unmapped_;
}

uint64_t System::UnmappedAccesses() const
{
    return unmapped_;
}

uint8_t* System::Page(uint32_t page)
{
    switch (page >> 24)
    {
    case 0x00:
        return page < kBiosSize ? &bios_[page] : nullptr;
    case 0x02:
        return &ewram_[page & (kEwramSize - 1)];
    case 0x03:
        return &iwram_[page & (kIwramSize - 1)];
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
        {
            // The three regions of different wait states each show the whole cartridge.
            const uint32_t offset = page & (kMaxRomSize - 1);
            return offset < rom_.size() ? &rom_[offset] : nullptr;
        }
    default:
        return nullptr;
    }
}

uint32_t System::Read(uint32_t address, int size)
{
    if (address >= kIoBase && address - kIoBase < kIoSize)
    {
        return ReadIo(address - kIoBase, size);
    }

    unmapped_++;
    return 0;
}

void System::Write(uint32_t address, int size, uint32_t value)
{
    if (address >= kIoBase && address - kIoBase < kIoSize)
    {
        WriteIo(address - kIoBase, size, value);
        return;
    }

    unmapped_++;
}

uint32_t System::ReadIo(uint32_t offset, int size)
{
    const uint32_t vcount = kRegVcount - kIoBase;
    if (offset <= vcount && vcount < offset + uint32_t(size))
    {
        if (held_vcount_)
        {
            vcount_ = *held_vcount_;
        }
        else
        {
            vcount_ = (vcount_ + 1) % kFrameLines;
        }
        WriteLe(&io_[vcount], 2, uint32_t(vcount_));
    }

    return ReadLe(&io_[offset], size);
}

void System::WriteIo(uint32_t offset, int size, uint32_t value)
{
    // A DMA channel starts when a write sets its control's enable bit.
    uint16_t enabled[4];
    for (int c = 0; c < 4; c++)
    {
        const uint32_t control = kRegDma0Source - kIoBase + kDmaStride * uint32_t(c) + kDmaControl;
        enabled[c] = uint16_t(ReadLe(&io_[control], 2) & kDmaEnable);
    }

    WriteLe(&io_[offset], size, value);
    if (on_io_write)
    {
        on_io_write(kIoBase + offset, size, value);
    }

    for (int c = 0; c < 4; c++)
    {
        const uint32_t control = kRegDma0Source - kIoBase + kDmaStride * uint32_t(c) + kDmaControl;
        if (offset < control + 2 && control < offset + uint32_t(size) && !enabled[c] &&
            (ReadLe(&io_[control], 2) & kDmaEnable))
        {
            StartDma(c);
        }
    }
}

void System::StartDma(int channel)
{
    const uint32_t base = kRegDma0Source - kIoBase + kDmaStride * uint32_t(channel);
    const uint16_t control = uint16_t(ReadLe(&io_[base + kDmaControl], 2));
    if (control & kDmaTiming)
    {
        // A transfer at the vertical or horizontal blank, or the sound FIFOs'. Nothing here starts one.
        return;
    }

    const int unit = control & kDmaWords ? 4 : 2;
    uint32_t source = ReadLe(&io_[base], 4) & (channel == 0 ? 0x07FFFFFF : 0x0FFFFFFF);
    uint32_t destination = ReadLe(&io_[base + 4], 4) & (channel == 3 ? 0x0FFFFFFF : 0x07FFFFFF);
    uint32_t count = ReadLe(&io_[base + 8], 2);
    if (count == 0)
    {
        count = channel == 3 ? 0x10000 : 0x4000;
    }

    // The address controls: 0 counts up, 1 down and 2 stays. 3 counts the destination up and reloads it afterwards.
    const auto step = [unit](int address_control)
    {
        return address_control == 1 ? -unit : address_control == 2 ? 0 : unit;
    };
    const int destination_step = step((control >> 5) & 3);
    const int source_step = step((control >> 7) & 3);
    for (uint32_t i = 0; i < count; i++)
    {
        Store(destination, unit, Load(source, unit));
        source += uint32_t(source_step);
        destination += uint32_t(destination_step);
    }

    WriteLe(&io_[base + kDmaControl], 2, control & ~kDmaEnable);
}

bool System::Swi(arm::Cpu& cpu, uint32_t comment)
{
    // An ARM program puts the call's number in bits 16 to 23 of the comment.
    const uint32_t number = cpu.Cpsr() & arm::psr::kT ? comment & 0xFF : (comment >> 16) & 0xFF;
    switch (number)
    {
    case 0x06: // Div: r0 / r1
    case 0x07: // DivArm: r1 / r0
        {
            const int64_t numerator = int32_t(cpu.Reg(number == 0x06 ? 0 : 1));
            const int64_t denominator = int32_t(cpu.Reg(number == 0x06 ? 1 : 0));
            if (denominator == 0)
            {
                failure_ = "the BIOS's Div was called with a denominator of 0";
                return false;
            }

            // The quotient rounds toward zero. r3 takes its magnitude.
            const int64_t quotient = numerator / denominator;
            cpu.SetReg(0, uint32_t(quotient));
            cpu.SetReg(1, uint32_t(numerator - quotient * denominator));
            cpu.SetReg(3, uint32_t(std::llabs(quotient)));
            return true;
        }
    case 0x0D: // GetBiosChecksum: the GBA's BIOS sums to this value
        cpu.SetReg(0, kBiosChecksum);
        return true;
    case 0x0B: // CpuSet
    case 0x0C: // CpuFastSet
        CpuSet(cpu.Reg(0), cpu.Reg(1), cpu.Reg(2), number == 0x0C);
        return true;
    case 0x11: // LZ77UnCompWram
    case 0x12: // LZ77UnCompVram: the same bytes, written in halfwords
        if (!Lz77(cpu.Reg(0), cpu.Reg(1)))
        {
            failure_ = "the BIOS's LZ77UnComp was called on data that isn't LZ77";
            return false;
        }
        return true;
    default:
        if (on_swi && on_swi(number))
        {
            return true;
        }

        failure_ = "BIOS call " + std::to_string(number) + " isn't modelled";
        return false;
    }
}

void System::CpuSet(uint32_t source, uint32_t destination, uint32_t control, bool fast)
{
    // r2 holds the count in bits 0 to 20, a fill from the source's first unit in bit 24, and 32-bit units in bit 26.
    // CpuFastSet copies words, 8 at a time.
    const bool fill = control & (1u << 24);
    const int unit = fast || (control & (1u << 26)) ? 4 : 2;
    uint32_t count = control & 0x1FFFFF;
    if (fast)
    {
        count = (count + 7) & ~7u;
    }

    source &= ~uint32_t(unit - 1);
    destination &= ~uint32_t(unit - 1);
    const uint32_t first = Load(source, unit);
    for (uint32_t i = 0; i < count; i++)
    {
        Store(destination + i * uint32_t(unit), unit, fill ? first : Load(source + i * uint32_t(unit), unit));
    }
}

bool System::Lz77(uint32_t source, uint32_t destination)
{
    // The header's bits 4 to 7 give the type, 1 for LZ77, and bits 8 to 31 the decompressed size. Then each flag byte
    // tells, from bit 7 down, whether each of the next 8 units is a byte to copy (0) or a back-reference (1). A
    // back-reference's two bytes give its length less 3 in the first's top 4 bits, and its distance less 1 in the rest.
    const uint32_t header = Load(source, 4);
    if ((header >> 4 & 0xF) != 1)
    {
        return false;
    }

    const uint32_t size = header >> 8;
    uint32_t in = source + 4;
    uint32_t out = 0;
    while (out < size)
    {
        const uint32_t flags = Load(in++, 1);
        for (int bit = 7; bit >= 0 && out < size; bit--)
        {
            if (!(flags >> bit & 1))
            {
                Store(destination + out++, 1, Load(in++, 1));
                continue;
            }

            const uint32_t first = Load(in++, 1);
            const uint32_t second = Load(in++, 1);
            const uint32_t length = (first >> 4) + 3;
            const uint32_t distance = ((first & 0xF) << 8 | second) + 1;
            for (uint32_t i = 0; i < length && out < size; i++, out++)
            {
                Store(destination + out, 1, Load(destination + out - distance, 1));
            }
        }
    }

    return true;
}

} // namespace ntdlib::agb
