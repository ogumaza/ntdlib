// SPDX-License-Identifier: MIT

// The DMAs, the IPC's FIFO and the maths unit (the divider and the square root) of the DS machine.

#include "machine_internal.h"

namespace ntdlib::ntr
{

void Machine::WriteDmaControl(int channel, uint32_t control)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const bool starts = (dma.control & kDmaEnable) == 0 && (control & kDmaEnable) != 0;
    dma.control = control;
    if (!starts)
    {
        return;
    }

    dma.next_source = dma.source;
    dma.next_destination = dma.destination;
    dma.left = DmaCount(control);

    switch (DmaTimingOf(control))
    {
    case kDmaImmediate:
    case kDmaGxFifo:
        // The 3D engine takes the commands at once.
        RunDma(channel);
        break;
    case kDmaCard:
        RunCardDmas();
        break;
    case kDmaHblank:
        ScheduleDisplay();
        EventScheduled();
        break;
    default:
        // The vertical blank's DMA waits for its event. The display's DMA never starts.
        break;
    }
}

void Machine::RunDma(int channel)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    uint32_t source = dma.next_source;
    uint32_t destination = dma.next_destination;
    CopyDmaUnits(*this, dma.control, dma.left, source, destination);

    dma.next_source = source;
    dma.next_destination = destination;
    dma.left = 0;
    EndDmaBlock(channel);
}

void Machine::EndDmaBlock(int channel)
{
    Dma& dma = dmas_[static_cast<std::size_t>(channel)];
    const uint32_t timing = DmaTimingOf(dma.control);
    if ((dma.control & kDmaRepeat) != 0 && timing != kDmaImmediate && timing != kDmaGxFifo)
    {
        dma.left = DmaCount(dma.control);
        if (((dma.control >> 21) & 3) == 3)
        {
            dma.next_destination = dma.destination;
        }
    }
    else
    {
        dma.control &= ~kDmaEnable;
    }

    if ((dma.control & kDmaIrq) != 0)
    {
        Request(irq::kDma0 << channel);
    }
}

void Machine::RunCardDmas()
{
    for (int channel = 0; channel < 4; channel++)
    {
        Dma& dma = dmas_[static_cast<std::size_t>(channel)];
        const int size = DmaUnitSize(dma.control);
        while ((dma.control & kDmaEnable) != 0 && DmaTimingOf(dma.control) == kDmaCard && card_.ready &&
               card_.position < card_.data.size())
        {
            // Each word that the card gives starts a unit of the transfer.
            const int destination_step = DmaStep(dma.control, 21);
            StoreToBus(*this, dma.next_destination & ~static_cast<uint32_t>(size - 1), size,
                       LoadFromBus(*this, dma.next_source, size));
            dma.next_destination += static_cast<uint32_t>(destination_step);
            if (--dma.left == 0)
            {
                EndDmaBlock(channel);
            }
        }
    }

    // NDMA in the card's mode takes each word that the card gives.
    while (ndma_ != nullptr && ndma_->WaitsForCard() && card_.ready && card_.position < card_.data.size())
    {
        // A channel that doesn't read the card's data waits for it forever.
        const std::size_t position = card_.position;
        ndma_->CardWord();
        if (card_.position == position)
        {
            break;
        }
    }
}

uint16_t Machine::FifoControl() const
{
    return FifoStatus(fifo_control_, arm7_->WordsToArm7(), arm7_->WordsToArm9(), fifo_error_);
}

void Machine::WriteFifoControl(uint16_t value)
{
    if ((value & kSendClear) != 0)
    {
        arm7_->ClearToArm7();
    }
    if ((value & kFifoError) != 0)
    {
        fifo_error_ = false;
    }

    const uint16_t old = fifo_control_;
    fifo_control_ = static_cast<uint16_t>(value & kFifoWritable);
    if ((old & kSendEmptyIrq) == 0 && (fifo_control_ & kSendEmptyIrq) != 0 && arm7_->WordsToArm7() == 0)
    {
        Request(irq::kIpcSendEmpty);
    }
    if ((old & kReceiveIrq) == 0 && (fifo_control_ & kReceiveIrq) != 0 && arm7_->WordsToArm9() != 0)
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

void Machine::Divide()
{
    // DIVCNT's mode: 32 bits by 32 (0), 64 by 32 (1 and 3) or 64 by 64 (2).
    const uint16_t control = Stored(kDivControl);
    const auto numerator64 = static_cast<int64_t>(xsflib::LoadLe64(&io_[kDivNumerator]));
    const auto denominator64 = static_cast<int64_t>(xsflib::LoadLe64(&io_[kDivDenominator]));
    const uint32_t mode = control & 3;
    const int64_t numerator = mode == 0 ? static_cast<int32_t>(numerator64) : numerator64;
    const int64_t denominator = mode == 2 ? denominator64 : static_cast<int32_t>(denominator64);

    // A denominator of 0 gives the numerator as the remainder and -1 or 1 as the result, with the high word of the
    // result inverted in mode 0. The flag in bit 14 looks at all 64 bits of the denominator.
    int64_t result = 0;
    int64_t remainder = 0;
    if (denominator == 0)
    {
        remainder = numerator;
        result = numerator >= 0 ? -1 : 1;
        if (mode == 0)
        {
            result = static_cast<int64_t>(static_cast<uint64_t>(result) ^ 0xFFFFFFFF00000000);
        }
    }
    else if (numerator == std::numeric_limits<int64_t>::min() && denominator == -1)
    {
        result = numerator;
    }
    else
    {
        result = numerator / denominator;
        remainder = numerator % denominator;
    }

    xsflib::StoreLe64(&io_[kDivResult], static_cast<uint64_t>(result));
    xsflib::StoreLe64(&io_[kDivRemainder], static_cast<uint64_t>(remainder));
    const uint16_t flags = static_cast<uint16_t>((control & 3) | (denominator64 == 0 ? 1u << 14 : 0u));
    xsflib::StoreLe16(&io_[kDivControl], flags);
}

void Machine::SquareRoot()
{
    // SQRTCNT's mode: a 32-bit parameter (0) or a 64-bit one (1).
    const uint64_t parameter = (Stored(kSqrtControl) & 1) != 0 ? xsflib::LoadLe64(&io_[kSqrtParameter])
                                                               : xsflib::LoadLe32(&io_[kSqrtParameter]);

    uint64_t root = 0;
    for (uint64_t bit = uint64_t{1} << 31; bit != 0; bit >>= 1)
    {
        const uint64_t trial = root | bit;
        if (trial * trial <= parameter)
        {
            root = trial;
        }
    }

    xsflib::StoreLe32(&io_[kSqrtResult], static_cast<uint32_t>(root));
}

} // namespace ntdlib::ntr
