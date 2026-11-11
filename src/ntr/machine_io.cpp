// SPDX-License-Identifier: MIT

// The registers of the DS machine: the reads and writes of the I/O area, and the registers that it stores.

#include "machine_internal.h"

namespace ntdlib::ntr
{

uint32_t Machine::ReadIo(uint32_t address, int size)
{
    if (address == kIpcFifoReceive)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return arm7_->WordsToArm9() == 0 ? last_received_ : arm7_->ReceiveFromArm7(false);
        }
        if (arm7_->WordsToArm9() == 0)
        {
            fifo_error_ = true;
            return last_received_;
        }

        last_received_ = arm7_->ReceiveFromArm7(true);
        return last_received_;
    }
    if (IsCardRegister(address))
    {
        return ReadCard(address, size);
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoSize)
    {
        Unknown(address, size, false);
        return 0;
    }

    if (size == 4)
    {
        return ReadIo16(offset) | static_cast<uint32_t>(ReadIo16(offset + 2)) << 16;
    }

    const uint16_t half = ReadIo16(offset & ~1u);
    return size == 2 ? half : (half >> (8 * (offset & 1))) & 0xFF;
}

void Machine::WriteIo(uint32_t address, int size, uint32_t value)
{
    if (address == kIo + kIpcFifoSend)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return;
        }
        if (arm7_->WordsToArm7() == kFifoWords)
        {
            fifo_error_ = true;
            return;
        }

        if (on_fifo_word)
        {
            on_fifo_word(true, value);
        }

        arm7_->SendToArm7(value);
        return;
    }
    if (IsCardRegister(address))
    {
        WriteCard(address, size, value);
        return;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoSize)
    {
        Unknown(address, size, true);
        return;
    }

    if (size == 4)
    {
        WriteIo16(offset, static_cast<uint16_t>(value), 0xFFFF);
        WriteIo16(offset + 2, static_cast<uint16_t>(value >> 16), 0xFFFF);
    }
    else if (size == 2)
    {
        WriteIo16(offset, static_cast<uint16_t>(value), 0xFFFF);
    }
    else
    {
        const int shift = 8 * static_cast<int>(offset & 1);
        WriteIo16(offset & ~1u, static_cast<uint16_t>((value & 0xFF) << shift), static_cast<uint16_t>(0xFF << shift));
    }
}

uint16_t Machine::ReadIo16(uint32_t offset)
{
    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        // Only the control half reads back.
        const Dma& dma = dmas_[(offset - kDmaStart) / 12];
        return (offset - kDmaStart) % 12 == 10 ? static_cast<uint16_t>(dma.control >> 16) : 0;
    }
    if (offset >= kDmaEnd && offset < kDmaFillEnd)
    {
        const Dma& dma = dmas_[(offset - kDmaEnd) / 4];
        return static_cast<uint16_t>(dma.fill >> (offset % 4 == 0 ? 0 : 16));
    }
    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        Poll();
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        return offset % 4 == 0 ? timers_.Counter(timer, Clock()) : timers_.Control(timer);
    }

    switch (offset)
    {
    case kDispstat:
        {
            Poll();
            const uint64_t line = Line();
            uint16_t flags = 0;
            flags |= line >= kVblankLine && line < kFrameLines - 1 ? kVblankFlag : 0;
            flags |= Clock() % kLineCycles >= kHblankCycle ? kHblankFlag : 0;
            flags |= line == VcountSetting(dispstat_) ? kVcountFlag : 0;
            return static_cast<uint16_t>((dispstat_ & kDispstatWritable) | flags);
        }
    case kVcount:
        Poll();
        return static_cast<uint16_t>(Line());
    case kKeyInput:
        return kKeysReleased;
    case kIpcSync:
        Poll();
        return static_cast<uint16_t>((ipcsync_ & (kSyncOutput | kSyncIrqEnable)) | arm7_->SyncOutput());
    case kIpcFifoControl:
        Poll();
        return FifoControl();
    case kIme:
        return ime_;
    case kIme + 2:
        return 0;
    case kIe:
        return static_cast<uint16_t>(ie_);
    case kIe + 2:
        return static_cast<uint16_t>(ie_ >> 16);
    case kIf:
        Poll();
        return static_cast<uint16_t>(if_);
    case kIf + 2:
        Poll();
        return static_cast<uint16_t>(if_ >> 16);
    case kWramControl - 1:
        return static_cast<uint16_t>(io_[kWramControl - 1] | wram_control_ << 8);
    case kGxStatus:
        // Never busy.
        return 0;
    case kGxStatus + 2:
        // The FIFO is empty, and so less than half full. Bits 30 and 31 keep the FIFO IRQ's condition.
        return static_cast<uint16_t>(0x0600 | (Stored(kGxStatus + 2) & 0xC000));
    case kPostFlag:
    case kCaptureControl:
    case kCaptureControl + 2:
        return Stored(offset);
    default:
        if ((offset >= kDivControl && offset < kSqrtEnd) || IsStoredRegister(offset))
        {
            return Stored(offset);
        }
        Unknown(kIo + offset, 2, false);
        return Stored(offset);
    }
}

void Machine::WriteIo16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const auto merge = [value, mask](uint16_t old)
    {
        return static_cast<uint16_t>((old & ~mask) | (value & mask));
    };

    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        const std::size_t channel = (offset - kDmaStart) / 12;
        Dma& dma = dmas_[channel];
        const uint32_t field = (offset - kDmaStart) % 12;

        const auto merge32 = [&merge](uint32_t old, bool high)
        {
            const int shift = high ? 16 : 0;
            return (old & ~(0xFFFFu << shift)) | static_cast<uint32_t>(merge(static_cast<uint16_t>(old >> shift)))
                                                     << shift;
        };

        if (field < 4)
        {
            dma.source = merge32(dma.source, field == 2);
        }
        else if (field < 8)
        {
            dma.destination = merge32(dma.destination, field == 6);
        }
        else if (field == 8)
        {
            dma.control = merge32(dma.control, false);
        }
        else
        {
            WriteDmaControl(static_cast<int>(channel), merge32(dma.control, true));
        }
        return;
    }
    if (offset >= kDmaEnd && offset < kDmaFillEnd)
    {
        Dma& dma = dmas_[(offset - kDmaEnd) / 4];
        const int shift = offset % 4 == 0 ? 0 : 16;
        dma.fill = (dma.fill & ~(0xFFFFu << shift)) |
                   static_cast<uint32_t>(merge(static_cast<uint16_t>(dma.fill >> shift))) << shift;
        return;
    }
    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        if (offset % 4 == 0)
        {
            timers_.SetReload(timer, merge(timers_.Reload(timer)));
        }
        else
        {
            timers_.SetControl(timer, merge(timers_.Control(timer)), Clock());
        }
        return;
    }
    if (offset >= kDivControl && offset < kDivResult)
    {
        Store16(offset, value, mask);
        Divide();
        return;
    }
    if (offset >= kSqrtControl && offset < kSqrtEnd)
    {
        if (offset < kSqrtResult || offset >= kSqrtParameter)
        {
            Store16(offset, value, mask);
            SquareRoot();
        }
        return;
    }

    switch (offset)
    {
    case kDispstat:
        dispstat_ = static_cast<uint16_t>(merge(dispstat_) & kDispstatWritable);
        ScheduleDisplay();
        return;
    case kIpcSync:
        ipcsync_ = static_cast<uint16_t>(merge(ipcsync_) & (kSyncOutput | kSyncIrqEnable));
        if ((value & mask & kSyncRequest) != 0)
        {
            arm7_->RequestSyncIrq();
        }
        return;
    case kIpcFifoControl:
        WriteFifoControl(static_cast<uint16_t>(value & mask));
        return;
    case kIme:
        ime_ = static_cast<uint16_t>(merge(ime_) & 1);
        UpdateIrq();
        return;
    case kIe:
    case kIe + 2:
        {
            const int shift = offset == kIe ? 0 : 16;
            ie_ = (ie_ & ~(0xFFFFu << shift)) | static_cast<uint32_t>(merge(static_cast<uint16_t>(ie_ >> shift)))
                                                    << shift;
            UpdateIrq();
            return;
        }
    case kIf:
    case kIf + 2:
        {
            const uint32_t acks = static_cast<uint32_t>(value & mask) << (offset == kIf ? 0 : 16);
            if ((acks & if_ & irq::kVblank) != 0)
            {
                counts_.vblank_acks++;
            }
            if_ &= ~acks;
            UpdateIrq();
            return;
        }
    case kWramControl - 1:
        Store16(offset, value, mask & 0x00FF);
        if ((mask & 0xFF00) != 0)
        {
            wram_control_ = static_cast<uint8_t>((value >> 8) & 3);
            arm7_->SetWramControl(wram_control_);
            arm9_->InvalidatePages();
        }
        return;
    case kGxStatus + 2:
        Store16(offset, value, mask & 0xC000);
        UpdateIrq();
        return;
    case kPostFlag:
        if ((mask & 0x00FF) != 0)
        {
            io_[kPostFlag] |= static_cast<uint8_t>(value & 1);
        }
        return;
    case kVcount:
    case kKeyInput:
    case kIme + 2:
    case kGxStatus:
        return;
    default:
        if (offset >= kDivResult && offset < kDivControl + 0x30)
        {
            return;
        }
        if (!IsStoredRegister(offset))
        {
            Unknown(kIo + offset, 2, true);
        }
        Store16(offset, value, mask);
        return;
    }
}

uint16_t Machine::Stored(uint32_t offset) const
{
    return xsflib::LoadLe16(&io_[offset]);
}

void Machine::Store16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const uint16_t merged = static_cast<uint16_t>((Stored(offset) & ~mask) | (value & mask));
    xsflib::StoreLe16(&io_[offset], merged);
}

void Machine::Unknown(uint32_t address, int size, bool write)
{
    if (on_unknown_io)
    {
        on_unknown_io(address, size, write);
    }
}

} // namespace ntdlib::ntr
