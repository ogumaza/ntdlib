// SPDX-License-Identifier: MIT

// The ARM7's registers and events: the I/O registers, the BIOS's calls, the display's timing, the DMA and the IPC's
// FIFO.

#include "arm7_internal.h"

namespace ntdlib::ntr
{

uint32_t Arm7System::ReadIo(uint32_t address, int size)
{
    if (link_ != nullptr && IsCardRegister(address))
    {
        return link_->ReadCard(address, size);
    }

    if (address >= kSoundRegisters && address < kSoundRegistersEnd)
    {
        SyncSound();
        const uint32_t value = sound_.Read(address, size);
        if (on_sound_access)
        {
            on_sound_access(address, size, value, false);
        }
        return value;
    }

    if (address == kIpcFifoReceive)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return to_arm7_.empty() ? last_received_ : to_arm7_.front();
        }
        if (to_arm7_.empty())
        {
            fifo_error_ = true;
            return last_received_;
        }
        last_received_ = to_arm7_.front();
        to_arm7_.pop_front();
        if (to_arm7_.empty() && link_ != nullptr)
        {
            link_->OnFifoToArm7Empty();
            LinkEvent();
        }
        return last_received_;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoEnd)
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

void Arm7System::WriteIo(uint32_t address, int size, uint32_t value)
{
    if (link_ != nullptr && IsCardRegister(address))
    {
        link_->WriteCard(address, size, value);
        return;
    }

    if (address >= kSoundRegisters && address < kSoundRegistersEnd)
    {
        SyncSound();
        sound_.Write(address, size, value);
        if (on_sound_access)
        {
            on_sound_access(address, size, value, true);
        }
        return;
    }

    if (address == kIpcFifoSend)
    {
        if ((fifo_control_ & kFifoEnable) == 0)
        {
            return;
        }
        if (to_arm9_.size() == kFifoWords)
        {
            fifo_error_ = true;
            return;
        }
        to_arm9_.push_back(value);
        if (link_ != nullptr)
        {
            link_->OnFifoToArm9(value);
            LinkEvent();
        }
        return;
    }

    const uint32_t offset = address - kIo;
    if (offset >= kIoEnd)
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

uint16_t Arm7System::ReadIo16(uint32_t offset)
{
    const auto stored = [this](uint32_t at)
    {
        return xsflib::LoadLe16(&io_[at]);
    };

    if (offset >= kDmaStart && offset < kDmaEnd)
    {
        return ReadDmaRegister(dmas_, offset);
    }

    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        return offset % 4 == 0 ? timers_.Counter(timer, clock_) : timers_.Control(timer);
    }

    switch (offset)
    {
    case kDispstat:
        {
            const uint64_t line = Line();
            const uint64_t vcount_setting = VcountSetting(dispstat_);
            uint16_t flags = 0;
            flags |= line >= kVblankLine && line < kFrameLines - 1 ? kVblankFlag : 0;
            flags |= clock_ % kLineCycles >= kHblankCycle ? kHblankFlag : 0;
            flags |= line == vcount_setting ? kVcountFlag : 0;
            return static_cast<uint16_t>((dispstat_ & kDispstatWritable) | flags);
        }
    case kVcount:
        return static_cast<uint16_t>(Line());
    case kKeyInput:
        return kKeysReleased;
    case kExtendedKeys:
        return kExtendedKeysIdle;
    case kIpcSync:
        // Without a link, the ARM9's output follows the ARM7's.
        return static_cast<uint16_t>((ipcsync_ & (kSyncOutput | kSyncIrqEnable)) |
                                     (link_ != nullptr ? link_->SyncOutput() & 0xF : (ipcsync_ & kSyncOutput) >> 8));
    case kIpcFifoControl:
        return FifoControl();
    case kIme:
    case kIme + 2:
    case kIe:
    case kIe + 2:
    case kIf:
    case kIf + 2:
        return ReadInterruptRegister(offset, ime_, ie_, if_);
    case kMemoryStatus:
        return static_cast<uint16_t>(wram_control_ << 8);
    case kPostFlag:
        return io_[kPostFlag];
    case kKeyControl:
    case kExternalMemory:
    case kWifiWait:
    case kPowerControl:
    case kBiosProtection:
    case kBiosProtection + 2:
        return stored(offset);
    case kSpiControl:
        // Never busy.
        if (spi_ == nullptr)
        {
            Unknown(kIo + offset, 2, false);
        }
        return static_cast<uint16_t>(stored(offset) & 0xCF03);
    case kSpiData:
        if (spi_ == nullptr)
        {
            Unknown(kIo + offset, 2, false);
            return 0;
        }
        return spi_answer_;
    default:
        Unknown(kIo + offset, 2, false);
        return stored(offset);
    }
}

void Arm7System::WriteIo16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const auto store = [this, value, mask](uint32_t at)
    {
        const uint16_t merged = Merge16(xsflib::LoadLe16(&io_[at]), value, mask);
        xsflib::StoreLe16(&io_[at], merged);
        return merged;
    };
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

    if (offset >= kTimerStart && offset < kTimerEnd)
    {
        const int timer = static_cast<int>((offset - kTimerStart) / 4);
        if (offset % 4 == 0)
        {
            timers_.SetReload(timer, merge(timers_.Reload(timer)));
        }
        else
        {
            timers_.SetControl(timer, merge(timers_.Control(timer)), clock_);
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
        if (link_ != nullptr)
        {
            // The ARM9 reads the new output before the ARM7 goes on.
            link_->OnSyncWrite((value & mask & kSyncRequest) != 0);
            LinkEvent();
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
        WriteInterruptRegister(offset, value, mask, ime_, ie_, if_);
        UpdateIrq();
        return;
    case kPostFlag:
        if ((mask & 0x00FF) != 0)
        {
            io_[kPostFlag] |= static_cast<uint8_t>(value & 1);
        }
        if ((mask & 0xFF00) != 0)
        {
            const auto halt = static_cast<uint8_t>(value >> 8);
            if ((halt & kHaltModeMask) == kHaltMode)
            {
                Halt();
            }
            else if ((halt & kHaltModeMask) != 0)
            {
                Fail("HALTCNT " + xsflib::Hex(halt, 8, true) + ": a mode that the system doesn't model");
            }
        }
        return;
    case kIme + 2:
    case kMemoryStatus:
    case kKeyInput:
    case kExtendedKeys:
    case kVcount:
        return;
    case kKeyControl:
    case kExternalMemory:
    case kWifiWait:
    case kPowerControl:
    case kBiosProtection:
    case kBiosProtection + 2:
        store(offset);
        return;
    case kSpiControl:
        if (spi_ == nullptr)
        {
            Unknown(kIo + offset, 2, true);
        }
        else if ((store(offset) & kSpiEnable) == 0)
        {
            spi_->Deselect();
            return;
        }
        store(offset);
        return;
    case kSpiData:
        {
            const uint16_t control = xsflib::LoadLe16(&io_[kSpiControl]);
            if (spi_ == nullptr)
            {
                Unknown(kIo + offset, 2, true);
            }
            else if ((control & kSpiEnable) != 0)
            {
                spi_answer_ = spi_->Transfer(control >> 8 & 3, static_cast<uint8_t>(value), (control & kSpiHold) != 0);
            }
            if ((control & (kSpiEnable | kSpiIrq)) == (kSpiEnable | kSpiIrq))
            {
                Request(irq::kSpi);
            }
            return;
        }
    default:
        Unknown(kIo + offset, 2, true);
        store(offset);
        return;
    }
}

void Arm7System::Unknown(uint32_t address, int size, bool write)
{
    if (on_unknown_io)
    {
        on_unknown_io(address, size, write);
    }
}

bool Arm7System::Swi(arm::Cpu& cpu, uint32_t comment)
{
    // In ARM state, the BIOS takes the number from bits 16 to 23 of the comment.
    const uint32_t number = (cpu.Cpsr() & arm::psr::kT) != 0 ? comment : (comment >> 16) & 0xFF;
    const auto load = [this](uint32_t address, int size)
    {
        return LoadFromBus(*this, address, size);
    };
    const auto store = [this](uint32_t address, int size, uint32_t value)
    {
        StoreToBus(*this, address, size, value);
    };
    const auto fail = [this](std::string message)
    {
        Fail(std::move(message));
    };
    if (const std::optional<bool> done = bios::SharedCall(cpu, number, dsi_io_ != nullptr, load, store, fail))
    {
        return *done;
    }

    switch (number)
    {
    case bios::kWaitByLoop:
        return true;
    case bios::kHalt:
        Halt();
        return true;
    case kSoundBiasSwi:
        {
            // The BIOS moves the bias one step each r1 loops. The system moves it at once.
            const uint32_t bias = cpu.Reg(0) != 0 ? kSoundBiasMiddle : 0;
            WriteIo(kSoundBias, 2, bias);
            return true;
        }
    case kGetPitchTable:
    case kGetVolumeTable:
        {
            const uint32_t index =
                number == kGetPitchTable && dsi_io_ != nullptr ? cpu.Reg(0) + kDsiPitchTableShift : cpu.Reg(0);
            const std::size_t count =
                number == kGetPitchTable ? std::size(snd::tables::kPitch) : std::size(snd::tables::kVolume);
            if (index >= count)
            {
                Fail("a read past the BIOS's table at " + xsflib::Hex(index, 8, true));
                return true;
            }
            cpu.SetReg(0, number == kGetPitchTable ? snd::tables::kPitch[index] : snd::tables::kVolume[index]);
            return true;
        }
    case kCustomHalt:
        WriteIo(kIo + kPostFlag + 1, 1, cpu.Reg(2) & 0xFF);
        return true;
    default:
        Fail("SWI " + xsflib::Hex(number, 8, true) + ", which the BIOS doesn't model, at " +
             xsflib::Hex(cpu.Pc(), 8, true));
        return true;
    }
}

void Arm7System::Fail(std::string message)
{
    if (error_.empty())
    {
        error_ = std::move(message);
    }
    cpu_->Stop();
}

void Arm7System::SyncSound()
{
    const uint64_t ticks = clock_ / 2;
    if (ticks <= sound_ticks_)
    {
        return;
    }

    sound_.Run(ticks - sound_ticks_, out_ != nullptr ? *out_ : discarded_);
    sound_ticks_ = ticks;
    discarded_.clear();
}

uint64_t Arm7System::NextEvent() const
{
    uint64_t next = timers_.NextOverflow(next_display_);
    return next;
}

void Arm7System::Advance(uint64_t until)
{
    for (;;)
    {
        const uint64_t next = NextEvent();
        if (next > until)
        {
            break;
        }

        clock_ = next;
        if (next_display_ == next)
        {
            DisplayEvent(next);
        }
        // Timer n's IRQ is irq::kTimer0 << n.
        if (const uint32_t timers = timers_.Overflow(next); timers != 0)
        {
            Request(irq::kTimer0 * timers);
        }
    }
    clock_ = std::max(clock_, until);
}

void Arm7System::Request(uint32_t irq)
{
    if_ |= irq;
    UpdateIrq();
}

void Arm7System::UpdateIrq()
{
    cpu_->SetIrq((ime_ & 1) != 0 && (ie_ & if_) != 0);
}

void Arm7System::Halt()
{
    halted_ = true;
    cpu_->Stop();
}

uint64_t Arm7System::Line() const
{
    return clock_ / kLineCycles % kFrameLines;
}

void Arm7System::ScheduleDisplay()
{
    // The next start of a line, or of a line's horizontal blank, after the clock.
    const auto next = [this](uint64_t line, uint64_t cycle)
    {
        const uint64_t at = clock_ / kFrameCycles * kFrameCycles + line * kLineCycles + cycle;
        return at > clock_ ? at : at + kFrameCycles;
    };

    next_display_ = std::numeric_limits<uint64_t>::max();
    if ((dispstat_ & kVblankIrq) != 0)
    {
        next_display_ = std::min(next_display_, next(kVblankLine, 0));
    }
    if ((dispstat_ & kVcountIrq) != 0)
    {
        const uint64_t setting = VcountSetting(dispstat_);
        if (setting < kFrameLines)
        {
            next_display_ = std::min(next_display_, next(setting, 0));
        }
    }
    if ((dispstat_ & kHblankIrq) != 0)
    {
        const uint64_t at = clock_ / kLineCycles * kLineCycles + kHblankCycle;
        next_display_ = std::min(next_display_, at > clock_ ? at : at + kLineCycles);
    }
}

void Arm7System::DisplayEvent(uint64_t at)
{
    const uint64_t line = at / kLineCycles % kFrameLines;
    const uint64_t cycle = at % kLineCycles;
    if (cycle == 0 && line == kVblankLine && (dispstat_ & kVblankIrq) != 0)
    {
        Request(irq::kVblank);
    }
    if (cycle == 0 && (dispstat_ & kVcountIrq) != 0 && line == VcountSetting(dispstat_))
    {
        Request(irq::kVcount);
    }
    if (cycle == kHblankCycle && (dispstat_ & kHblankIrq) != 0)
    {
        Request(irq::kHblank);
    }
    ScheduleDisplay();
}

void Arm7System::WriteDmaControl(int channel, uint32_t control)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const bool starts = (dma.control & kDmaEnable) == 0 && (control & kDmaEnable) != 0;
    dma.control = control;
    if (!starts)
    {
        return;
    }

    // Only the immediate start is modelled. The transfer happens at once.
    const uint32_t timing = (control >> 28) & 3;
    if (timing != 0)
    {
        Fail("a DMA that starts on an event (" + xsflib::Hex(control, 8, true) + ")");
        return;
    }

    const uint32_t mask = channel == 3 ? 0xFFFF : 0x3FFF;
    uint32_t count = control & mask;
    if (count == 0)
    {
        count = mask + 1;
    }
    uint32_t source = dma.source;
    uint32_t destination = dma.destination;
    CopyDmaUnits(*this, control, count, source, destination);

    dma.control &= ~kDmaEnable;
    if ((control & kDmaIrq) != 0)
    {
        Request(irq::kDma0 << channel);
    }
}

uint16_t Arm7System::FifoControl() const
{
    return FifoStatus(fifo_control_, to_arm9_.size(), to_arm7_.size(), fifo_error_);
}

void Arm7System::WriteFifoControl(uint16_t value)
{
    if ((value & kSendClear) != 0)
    {
        to_arm9_.clear();
    }
    if ((value & kFifoError) != 0)
    {
        fifo_error_ = false;
    }

    const uint16_t old = fifo_control_;
    fifo_control_ = static_cast<uint16_t>(value & kFifoWritable);
    if ((old & kSendEmptyIrq) == 0 && (fifo_control_ & kSendEmptyIrq) != 0 && to_arm9_.empty())
    {
        Request(irq::kIpcSendEmpty);
    }
    if ((old & kReceiveIrq) == 0 && (fifo_control_ & kReceiveIrq) != 0 && !to_arm7_.empty())
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

} // namespace ntdlib::ntr
