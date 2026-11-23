// SPDX-License-Identifier: MIT

#include "dsi_io.h"

#include <cstdint>
#include <utility>

#include "xsflib/bytes.h"

#include "ndma.h"
#include "new_wram.h"

namespace ntdlib::ntr
{

namespace
{

// The registers' offsets from 0x04004000.
constexpr uint32_t kRom = 0x000;        // SCFG_A9ROM on the ARM9; SCFG_ROM on the ARM7, whose bits can only be set
constexpr uint32_t kMemoryCard = 0x010; // SCFG_MC
constexpr uint32_t kCardDelays = 0x012; // the ARM7's SCFG_CARD_INSERT_DELAY and SCFG_CARD_PWROFF_DELAY
constexpr uint32_t kDebugger = 0x024;   // the ARM7's SCFG_OP
constexpr uint32_t kGatedEnd = 0x064;   // the end of the registers that SCFG_EXT's bit 31 gates
constexpr uint32_t kI2cData = 0x500;
constexpr uint32_t kI2cControl = 0x501;
constexpr uint32_t kSoundExtension = 0x700; // SNDEXCNT
constexpr uint32_t kFirstNdmaChannel = 0x104;
constexpr uint32_t kGatingBit = 1u << 31;

// I2C_CNT's bits.
constexpr uint8_t kI2cStart = 0x02;
constexpr uint8_t kI2cSignalOnly = 0x04;
constexpr uint8_t kI2cAck = 0x10;
constexpr uint8_t kI2cRead = 0x20;
constexpr uint8_t kI2cBusy = 0x80;

constexpr uint8_t kBptwl = 0x4A;

// The launcher's values: the BIOSes' upper halves and the console ID off on both CPUs; the ARM9 at its DS clock with
// the new shared RAM's clock on; the card in the slot, powered (state 2); the card's delays that the firmware sets.
constexpr uint8_t kArm9RomBits = 0x01;
constexpr uint8_t kArm7RomBits = 0x05;
constexpr uint16_t kArm9Clock = 0x0084;
constexpr uint16_t kCardPowered = 0x0008;
constexpr uint16_t kCardInsertDelay = 0x1988;
constexpr uint16_t kCardPowerOffDelay = 0x264C;

} // namespace

DsiIo::DsiIo(bool arm7, uint32_t extended, NewWram& wram, Ndma* ndma) : arm7_(arm7), wram_(wram), ndma_(ndma)
{
    bytes_[kRom] = kArm9RomBits;
    if (arm7_)
    {
        bytes_[kRom + 1] = kArm7RomBits;
        xsflib::StoreLe16(&bytes_[kCardDelays], kCardInsertDelay);
        xsflib::StoreLe16(&bytes_[kCardDelays + 2], kCardPowerOffDelay);
    }
    else
    {
        xsflib::StoreLe16(&bytes_[kClock], kArm9Clock);
    }
    xsflib::StoreLe32(&bytes_[kExtended], extended);
    xsflib::StoreLe16(&bytes_[kMemoryCard], kCardPowered);

    // The BPTWL chip's registers (GBATEK's "DSi I2C Device 4Ah"): the reserved ones read 0x5A.
    bptwl_.fill(0x5A);
    const std::array<std::pair<uint8_t, uint8_t>, 22> kBptwlRegisters = {{
        {0x00, 0x33},                                                         // the version: fast
        {0x01, 0x00}, {0x02, 0x50}, {0x10, 0x00},                             // no button down
        {0x11, 0x00}, {0x12, 0x00}, {0x20, 0x0F},                             // the battery full, not charging
        {0x21, 0x07}, {0x30, 0x00}, {0x31, 0x00},                             // the camera's LED off
        {0x40, 0x1F},                                                         // the volume
        {0x41, 0x04},                                                         // the backlight
        {0x60, 0x00}, {0x61, 0x01}, {0x62, 0x50}, {0x63, 0x00}, {0x70, 0x00}, // a cold boot
        {0x71, 0x00}, {0x72, 0x00}, {0x76, 0x00}, {0x80, 0x10}, {0x81, 0x64},
    }};
    for (const auto& [index, value] : kBptwlRegisters)
    {
        bptwl_[index] = value;
    }
    for (uint32_t index = 0x73; index <= 0x75; index++)
    {
        bptwl_[index] = 0;
    }
}

bool DsiIo::Contains(uint32_t address)
{
    return address >= kBase && address < kBase + kSize;
}

uint32_t DsiIo::Read(uint32_t address, int size)
{
    const uint32_t offset = address - kBase;
    if (!Modelled(offset) && on_unknown)
    {
        on_unknown(address, size, false);
    }

    return Peek(offset, size);
}

void DsiIo::Write(uint32_t address, int size, uint32_t value)
{
    const uint32_t offset = address - kBase;
    if (!Modelled(offset) && on_unknown)
    {
        on_unknown(address, size, true);
    }
    if (Gated(offset))
    {
        return;
    }
    if (offset >= NewWram::kFirstRegister && offset < NewWram::kRegistersEnd)
    {
        wram_.Write(arm7_, offset, size, value);
        return;
    }
    if (ndma_ != nullptr && offset >= Ndma::kFirstRegister && offset < Ndma::kRegistersEnd)
    {
        ndma_->Write(offset, size, value);
        return;
    }

    for (int i = 0; i < size; i++)
    {
        WriteByte(offset + static_cast<uint32_t>(i), static_cast<uint8_t>(value >> (8 * i)));
    }
}

uint32_t DsiIo::Peek(uint32_t offset, int size) const
{
    if (Gated(offset))
    {
        return 0;
    }
    if (offset >= NewWram::kFirstRegister && offset < NewWram::kRegistersEnd)
    {
        return wram_.Read(arm7_, offset, size);
    }
    if (ndma_ != nullptr && offset >= Ndma::kFirstRegister && offset < Ndma::kRegistersEnd)
    {
        return ndma_->Read(offset, size);
    }

    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= uint32_t{bytes_[(offset + static_cast<uint32_t>(i)) & (kSize - 1)]} << (8 * i);
    }

    return value;
}

bool DsiIo::FastArm9() const
{
    return !arm7_ && (bytes_[kClock] & 1) != 0;
}

bool DsiIo::Gated(uint32_t offset) const
{
    return offset < kGatedEnd && (xsflib::LoadLe32(&bytes_[kExtended]) & kGatingBit) == 0;
}

bool DsiIo::Modelled(uint32_t offset) const
{
    return offset < kGatedEnd || (offset >= Ndma::kFirstRegister && offset < Ndma::kRegistersEnd) ||
           (arm7_ && (offset == kI2cData || offset == kI2cControl || offset == kSoundExtension ||
                      offset == kSoundExtension + 1));
}

void DsiIo::WriteByte(uint32_t offset, uint8_t value)
{
    if (offset < kGatedEnd)
    {
        // Read-only: the ARM9's SCFG_A9ROM and SCFG_MC, and the ARM7's SCFG_OP. The ARM7's SCFG_ROM only sets bits.
        if ((!arm7_ && (offset < kRom + 4 || offset == kMemoryCard || offset == kMemoryCard + 1)) ||
            (arm7_ && (offset == kDebugger || offset == kDebugger + 1)))
        {
            return;
        }
        bytes_[offset] = arm7_ && offset < kRom + 2 ? static_cast<uint8_t>(bytes_[offset] | value) : value;
        return;
    }

    if (arm7_ && offset >= Ndma::kFirstRegister && offset < Ndma::kRegistersEnd)
    {
        // A byte with a channel's enable bit: the top byte of NDMAxCNT, 0x1B bytes after the channel's first register.
        bytes_[offset] = value;
        if ((value & 0x80) != 0 && offset >= kFirstNdmaChannel && (offset - kFirstNdmaChannel) % 0x1C == 0x1B &&
            on_fail)
        {
            on_fail("the ARM7 starts an NDMA channel, which the machine doesn't model");
        }
        return;
    }

    bytes_[offset] = value;
    if (arm7_ && offset == kI2cControl && (value & kI2cBusy) != 0)
    {
        I2cTransfer();
    }
}

void DsiIo::I2cTransfer()
{
    uint8_t control = bytes_[kI2cControl];
    const uint8_t data = bytes_[kI2cData];
    bool ack = false;
    if ((control & kI2cSignalOnly) != 0)
    {
        ack = true;
    }
    else if ((control & kI2cStart) != 0)
    {
        // The device's byte: its number in bits 1 to 7, and a read in bit 0.
        device_ = data & 0xFE;
        index_pending_ = (data & 1) == 0;
        ack = device_ == kBptwl;
    }
    else if ((control & kI2cRead) == 0)
    {
        ack = device_ == kBptwl;
        if (index_pending_)
        {
            index_ = data;
            index_pending_ = false;
        }
        else
        {
            if (device_ == kBptwl)
            {
                bptwl_[index_] = data;
            }
            index_++;
        }
    }
    else
    {
        bytes_[kI2cData] = device_ == kBptwl ? bptwl_[index_] : 0xFF;
        index_++;
    }

    // A written byte gets the device's acknowledgement. On a read, the bit is the CPU's.
    if ((control & kI2cRead) == 0)
    {
        control = static_cast<uint8_t>(ack ? control | kI2cAck : control & ~kI2cAck);
    }
    bytes_[kI2cControl] = static_cast<uint8_t>(control & ~kI2cBusy);
}

} // namespace ntdlib::ntr
