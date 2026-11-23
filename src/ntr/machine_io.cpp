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

    if (dsi_io_ != nullptr && DsiIo::Contains(address))
    {
        return dsi_io_->Read(address, size);
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoSize)
    {
        Unknown(address, size, false);
        return 0;
    }

    const auto read16 = [this](uint32_t at)
    {
        return ReadIo16(at);
    };
    return ReadIoBySize(offset, size, read16);
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

    if (dsi_io_ != nullptr && DsiIo::Contains(address))
    {
        dsi_io_->Write(address, size, value);
        return;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoSize)
    {
        Unknown(address, size, true);
        return;
    }

    const auto write16 = [this](uint32_t at, uint16_t half, uint16_t mask)
    {
        WriteIo16(at, half, mask);
    };
    WriteIoBySize(offset, size, value, write16);
}

uint16_t Machine::ReadIo16(uint32_t offset)
{
    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        return ReadDmaRegister(dmas_, offset);
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
    case kIf:
    case kIf + 2:
        Poll();
        [[fallthrough]];
    case kIme:
    case kIme + 2:
    case kIe:
    case kIe + 2:
        return ReadInterruptRegister(offset, ime_, ie_, if_);
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
        return Merge16(old, value, mask);
    };

    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        if (const std::optional<uint32_t> control = WriteDmaRegister(dmas_, offset, value, mask))
        {
            WriteDmaControl(DmaChannelOf(offset), *control);
        }
        return;
    }
    if (offset >= kDmaEnd && offset < kDmaFillEnd)
    {
        Dma& dma = dmas_[(offset - kDmaEnd) / 4];
        dma.fill = MergeHalf(dma.fill, offset % 4 == 0 ? 0 : 16, value, mask);
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
            EventScheduled();
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
        EventScheduled();
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
    case kIe:
    case kIe + 2:
    case kIf:
    case kIf + 2:
        if ((WriteInterruptRegister(offset, value, mask, ime_, ie_, if_) & irq::kVblank) != 0)
        {
            counts_.vblank_acks++;
        }
        UpdateIrq();
        return;
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
    xsflib::StoreLe16(&io_[offset], Merge16(Stored(offset), value, mask));
}

void Machine::Unknown(uint32_t address, int size, bool write)
{
    if (on_unknown_io)
    {
        on_unknown_io(address, size, write);
    }
}

} // namespace ntdlib::ntr
