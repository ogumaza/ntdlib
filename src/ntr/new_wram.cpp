// SPDX-License-Identifier: MIT

#include "new_wram.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "xsflib/bytes.h"

namespace ntdlib::ntr
{

namespace
{

constexpr uint32_t kWramBase = 0x03000000;
constexpr uint32_t kMemorySize = 0x40000;

// The banks of WRAM-A, B and C, and the first byte of each memory's banks in MBK1 to MBK5.
constexpr std::array<uint32_t, 3> kBanks = {4, 8, 8};
constexpr std::array<std::size_t, 3> kFirstSlotByte = {0, 4, 12};

// The fields of a bank's byte in MBK1 to MBK5: the CPU (0 the ARM9, 1 the ARM7, 2 or 3 the DSP in B and C), the slot,
// and the enable bit.
constexpr uint8_t kBankEnabled = 0x80;

// The registers' offsets: MBK1 to MBK5, the CPU's MBK6 to MBK8, and MBK9.
constexpr uint32_t kSlotRegisters = 0x40;
constexpr uint32_t kWindowRegisters = 0x54;
constexpr uint32_t kProtectRegister = 0x60;

} // namespace

NewWram::NewWram()
{
    for (std::vector<uint8_t>& memory : memory_)
    {
        memory.assign(kMemorySize, 0);
    }
}

void NewWram::SetFromHeader(const uint8_t* bytes)
{
    for (std::size_t i = 0; i < slots_.size(); i++)
    {
        slots_[i] = bytes[i];
    }
    for (std::size_t cpu = 0; cpu < 2; cpu++)
    {
        for (std::size_t i = 0; i < 3; i++)
        {
            windows_[cpu][i] = xsflib::LoadLe32(bytes + 0x14 + 12 * cpu + 4 * i);
        }
    }
    protect_ = uint32_t{bytes[0x2C]} | uint32_t{bytes[0x2D]} << 8 | uint32_t{bytes[0x2E]} << 16;

    if (on_map_change)
    {
        on_map_change();
    }
}

std::optional<uint8_t*> NewWram::Page(bool arm7, uint32_t page)
{
    for (int memory = 0; memory < 3; memory++)
    {
        const Window window = WindowOf(memory, windows_[arm7 ? 1 : 0][static_cast<std::size_t>(memory)]);
        if (page < window.start || page >= window.end)
        {
            continue;
        }

        // The slots repeat from the address space's 0x03000000, not from the window's start.
        const uint32_t slot = (page - kWramBase) / window.slot_size % window.slots;
        uint8_t* bank = Bank(memory, arm7, slot);

        return bank == nullptr ? nullptr : bank + (page - kWramBase) % window.slot_size;
    }

    return std::nullopt;
}

uint32_t NewWram::Read(bool arm7, uint32_t offset, int size) const
{
    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= uint32_t{ByteOf(arm7, offset + static_cast<uint32_t>(i))} << (8 * i);
    }

    return value;
}

void NewWram::Write(bool arm7, uint32_t offset, int size, uint32_t value)
{
    for (int i = 0; i < size; i++)
    {
        WriteByte(arm7, offset + static_cast<uint32_t>(i), static_cast<uint8_t>(value >> (8 * i)));
    }

    if (on_map_change)
    {
        on_map_change();
    }
}

NewWram::Window NewWram::WindowOf(int memory, uint32_t setting)
{
    // WRAM-A's MBK6: the start in bits 4 to 11 and the end in bits 20 to 28, in units of 64 KiB, and the image's size
    // in bits 12 and 13 (0 and 1 one slot, 2 two, 3 four). WRAM-B's MBK7 and WRAM-C's MBK8: the start in bits 3 to 11
    // and the end in bits 19 to 28, in units of 32 KiB, and the image's size (1, 2, 4 or 8 slots).
    const uint32_t size_code = (setting >> 12) & 3;
    Window window;
    if (memory == 0)
    {
        window.slot_size = 0x10000;
        window.start = kWramBase + ((setting >> 4) & 0xFF) * window.slot_size;
        window.end = kWramBase + ((setting >> 20) & 0x1FF) * window.slot_size;
        window.slots = size_code < 2 ? 1 : size_code == 2 ? 2 : 4;
    }
    else
    {
        window.slot_size = 0x8000;
        window.start = kWramBase + ((setting >> 3) & 0x1FF) * window.slot_size;
        window.end = kWramBase + ((setting >> 19) & 0x3FF) * window.slot_size;
        window.slots = 1u << size_code;
    }

    return window;
}

uint8_t* NewWram::Bank(int memory, bool arm7, uint32_t slot)
{
    const auto index = static_cast<std::size_t>(memory);
    const uint32_t cpu_mask = memory == 0 ? 1 : 3;
    const uint32_t slot_mask = memory == 0 ? 3 : 7;
    const uint32_t bank_size = kMemorySize / kBanks[index];
    for (uint32_t bank = 0; bank < kBanks[index]; bank++)
    {
        const uint8_t setting = slots_[kFirstSlotByte[index] + bank];
        if ((setting & kBankEnabled) != 0 && (setting & cpu_mask) == (arm7 ? 1u : 0u) &&
            ((setting >> 2) & slot_mask) == slot)
        {
            return memory_[index].data() + bank * bank_size;
        }
    }

    return nullptr;
}

uint8_t NewWram::ByteOf(bool arm7, uint32_t offset) const
{
    if (offset >= kSlotRegisters && offset < kWindowRegisters)
    {
        return slots_[offset - kSlotRegisters];
    }
    if (offset >= kWindowRegisters && offset < kProtectRegister)
    {
        const uint32_t at = offset - kWindowRegisters;
        return static_cast<uint8_t>(windows_[arm7 ? 1 : 0][at / 4] >> (8 * (at % 4)));
    }

    return static_cast<uint8_t>(protect_ >> (8 * (offset - kProtectRegister)));
}

void NewWram::WriteByte(bool arm7, uint32_t offset, uint8_t value)
{
    if (offset >= kSlotRegisters && offset < kWindowRegisters)
    {
        // MBK9 has a bit for each byte: WRAM-A's 4 in bits 0 to 3, B's 8 in bits 8 to 15 and C's 8 in bits 16 to 23.
        const uint32_t at = offset - kSlotRegisters;
        const uint32_t bit = at < 4 ? at : at + 4;
        if (!arm7 && (protect_ & (1u << bit)) == 0)
        {
            slots_[at] = value;
        }
        return;
    }
    if (offset >= kWindowRegisters && offset < kProtectRegister)
    {
        const uint32_t at = offset - kWindowRegisters;
        uint32_t& window = windows_[arm7 ? 1 : 0][at / 4];
        const uint32_t shift = 8 * (at % 4);
        window = (window & ~(0xFFu << shift)) | uint32_t{value} << shift;
        return;
    }
    if (arm7)
    {
        const uint32_t shift = 8 * (offset - kProtectRegister);
        protect_ = (protect_ & ~(0xFFu << shift)) | uint32_t{value} << shift;
    }
}

} // namespace ntdlib::ntr
