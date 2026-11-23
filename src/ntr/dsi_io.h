// SPDX-License-Identifier: MIT

// The DSi's registers from 0x04004000 to 0x04004FFF, of one CPU in DSi mode (GBATEK's "DSi I/O Map"). The model covers
// the system control registers (SCFG), the new shared WRAM's registers (NewWram), the ARM9's NDMA (Ndma), and on the
// ARM7 the I2C bus with the BPTWL chip. While bit 31 of the CPU's SCFG_EXT is clear, the SCFG and MBK registers
// (0x04004000 to 0x04004063) read as 0 and ignore writes. The other registers keep what the CPU writes, as does
// SNDEXCNT (0x04004700): the sound unit plays as a DS's whatever the register contains. An access to a register of a
// device without a model (the cameras, the DSP, AES, the microphone, SD/MMC, SDIO, GPIO and the console ID) is
// reported. The ARM7's NDMA keeps its registers, and a start of one of its channels fails.
//
// The I2C bus moves a byte at once when I2C_CNT's bit 7 is set. Bit 1 sends the start condition with a device's byte,
// bit 5 reads a byte, and bit 4 is the device's acknowledgement of a written byte. Bit 2 sends only the start or
// stop condition. The BPTWL chip (device 0x4A) answers with a register index, then reads or writes its registers from
// that index on. Its registers contain GBATEK's values for a DSi with a full battery that isn't charging and no button
// down. No other device answers: a read gives 0xFF. The bus requests no interrupt.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace ntdlib::ntr
{

class Ndma;
class NewWram;

class DsiIo
{
public:
    static constexpr uint32_t kBase = 0x04004000;
    static constexpr uint32_t kSize = 0x1000;

    // The registers' offsets from kBase that other parts read.
    static constexpr uint32_t kClock = 0x004;    // SCFG_CLK: bit 0 runs the ARM9 at twice its DS clock
    static constexpr uint32_t kExtended = 0x008; // SCFG_EXT

    // The registers of the ARM7 or of the ARM9, with the new shared WRAM's registers in `wram` and, on the ARM9, the
    // NDMA's in `ndma`. They start as the launcher leaves them for a game that runs in DSi mode. `extended` is the
    // CPU's SCFG_EXT.
    DsiIo(bool arm7, uint32_t extended, NewWram& wram, Ndma* ndma);

    static bool Contains(uint32_t address);

    uint32_t Read(uint32_t address, int size);
    void Write(uint32_t address, int size, uint32_t value);

    // A register's value, as a read by the CPU would give it, with no side effects.
    uint32_t Peek(uint32_t offset, int size) const;

    // Whether SCFG_CLK runs the ARM9 at twice its DS clock, whatever SCFG_EXT gates.
    bool FastArm9() const;

    // Called for each access to a register of a device without a model.
    std::function<void(uint32_t address, int size, bool write)> on_unknown;

    // Called to stop the machine with a message.
    std::function<void(std::string)> on_fail;

private:
    bool Gated(uint32_t offset) const;
    bool Modelled(uint32_t offset) const;
    void WriteByte(uint32_t offset, uint8_t value);
    void I2cTransfer();

    bool arm7_;
    NewWram& wram_;
    Ndma* ndma_;
    std::array<uint8_t, kSize> bytes_ = {};

    // The I2C bus and the BPTWL chip.
    std::array<uint8_t, 256> bptwl_ = {};
    uint8_t device_ = 0;
    uint8_t index_ = 0;
    bool index_pending_ = false; // the next byte written is the register's index
};

} // namespace ntdlib::ntr
