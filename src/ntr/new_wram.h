// SPDX-License-Identifier: MIT

// The DSi's new shared work memory: WRAM-A (4 banks of 64 KiB), WRAM-B and WRAM-C (8 banks of 32 KiB each), and the
// registers that map them (MBK1 to MBK9, GBATEK's "DSi New Shared WRAM"). MBK1 to MBK5 give each bank to a CPU and a
// slot. Each CPU has a separate MBK6 to MBK8. They place the slots of WRAM-A, B and C in the CPU's address space. The
// slots repeat through a window from its start to its end. A slot that no bank of that CPU fills reads as nothing.
// WRAM-A comes before B, and B before C, where windows overlap. The ARM9 writes MBK1 to MBK5 unless MBK9 protects them,
// and only the ARM7 writes MBK9. Banks that the DSP takes aren't mapped for either CPU.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace ntdlib::ntr
{

class NewWram
{
public:
    // The registers' offsets from 0x04004000.
    static constexpr uint32_t kFirstRegister = 0x40;
    static constexpr uint32_t kRegistersEnd = 0x64;

    NewWram();

    // Sets the registers as the launcher leaves them from a DSi header's bytes 0x180 to 0x1AE: MBK1 to MBK5, the
    // ARM9's MBK6 to MBK8, the ARM7's MBK6 to MBK8, and MBK9's 3 bytes.
    void SetFromHeader(const uint8_t* bytes);

    // The host memory of the 4 KiB page at `page`, in 0x03000000 to 0x03FFFFFF, for the ARM7 or the ARM9. Returns
    // nothing where no window of the CPU covers the page, and nullptr where a window covers it with no bank of the CPU
    // in the slot.
    std::optional<uint8_t*> Page(bool arm7, uint32_t page);

    // A CPU's access to a register, at `offset` from 0x04004000 (kFirstRegister to kRegistersEnd).
    uint32_t Read(bool arm7, uint32_t offset, int size) const;
    void Write(bool arm7, uint32_t offset, int size, uint32_t value);

    // Called after a write that changes where the banks are mapped.
    std::function<void()> on_map_change;

private:
    struct Window
    {
        uint32_t start = 0; // the first address
        uint32_t end = 0;   // the address after the last
        uint32_t slot_size = 0;
        uint32_t slots = 0; // the slots of the image that repeats through the window
    };

    static Window WindowOf(int memory, uint32_t setting);
    uint8_t* Bank(int memory, bool arm7, uint32_t slot);
    uint8_t ByteOf(bool arm7, uint32_t offset) const;
    void WriteByte(bool arm7, uint32_t offset, uint8_t value);

    std::array<uint8_t, 20> slots_ = {};                  // MBK1 to MBK5: a byte for each bank of A, B and C
    std::array<std::array<uint32_t, 3>, 2> windows_ = {}; // MBK6 to MBK8 of the ARM9, then of the ARM7
    uint32_t protect_ = 0;                                // MBK9
    std::array<std::vector<uint8_t>, 3> memory_;          // WRAM-A, B and C
};

} // namespace ntdlib::ntr
