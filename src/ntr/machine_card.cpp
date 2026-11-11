// SPDX-License-Identifier: MIT

// The card and the SPI bus of the DS machine: the registers of the card, its commands and data, and the SPI devices.

#include "machine_internal.h"

namespace ntdlib::ntr
{

uint32_t Machine::ReadCard(uint32_t address, int size)
{
    if (address == kCardData)
    {
        return ReadCardData();
    }

    const uint32_t offset = address - kIo;
    if (size == 4)
    {
        return CardRead16(offset) | static_cast<uint32_t>(CardRead16(offset + 2)) << 16;
    }

    const uint16_t half = CardRead16(offset & ~1u);
    return size == 2 ? half : (half >> (8 * (offset & 1))) & 0xFF;
}

void Machine::WriteCard(uint32_t address, int size, uint32_t value)
{
    if (address == kCardData)
    {
        // The card takes no writes of data.
        Unknown(address, size, true);
        return;
    }

    const uint32_t offset = address - kIo;
    if (size == 4)
    {
        CardWrite16(offset, static_cast<uint16_t>(value), 0xFFFF);
        CardWrite16(offset + 2, static_cast<uint16_t>(value >> 16), 0xFFFF);
    }
    else if (size == 2)
    {
        CardWrite16(offset, static_cast<uint16_t>(value), 0xFFFF);
    }
    else
    {
        const int shift = 8 * static_cast<int>(offset & 1);
        CardWrite16(offset & ~1u, static_cast<uint16_t>((value & 0xFF) << shift), static_cast<uint16_t>(0xFF << shift));
    }
}

uint16_t Machine::CardRead16(uint32_t offset)
{
    switch (offset)
    {
    case kSpiControl:
        return static_cast<uint16_t>(card_.spi_control & ~kSpiBusy);
    case kSpiData:
        return card_.spi_data;
    case kCardControl:
        return static_cast<uint16_t>(card_.control);
    case kCardControl + 2:
        {
            Poll();
            uint32_t control = card_.control;
            control |= card_.ready && card_.position < card_.data.size() ? kCardReady : 0;
            control |= card_.busy ? kCardStart : 0;
            return static_cast<uint16_t>(control >> 16);
        }
    default:
        if (offset >= kCardCommand && offset < kCardSeeds)
        {
            const std::size_t at = offset - kCardCommand;
            return xsflib::LoadLe16(&card_.command[at]);
        }
        return Stored(offset);
    }
}

void Machine::CardWrite16(uint32_t offset, uint16_t value, uint16_t mask)
{
    const auto merge = [value, mask](uint16_t old)
    {
        return static_cast<uint16_t>((old & ~mask) | (value & mask));
    };

    switch (offset)
    {
    case kSpiControl:
        card_.spi_control = static_cast<uint16_t>(merge(card_.spi_control) & ~kSpiBusy);
        if ((card_.spi_control & kSpiMode) == 0 && card_.spi_selected)
        {
            // The memory is selected only in SPI mode.
            EndSpi();
        }
        return;
    case kSpiData:
        if ((mask & 0xFF) != 0)
        {
            WriteSpiData(static_cast<uint8_t>(value));
        }
        return;
    case kCardControl:
        card_.control = (card_.control & 0xFFFF0000) | merge(static_cast<uint16_t>(card_.control));
        return;
    case kCardControl + 2:
        {
            const uint32_t control = (card_.control & 0xFFFF) |
                                     static_cast<uint32_t>(merge(static_cast<uint16_t>(card_.control >> 16))) << 16;
            card_.control = control & ~(kCardStart | kCardReady);
            if ((control & kCardStart) != 0 && !card_.busy)
            {
                StartCard();
            }
            return;
        }
    default:
        if (offset >= kCardCommand && offset < kCardSeeds)
        {
            const std::size_t at = offset - kCardCommand;
            const uint16_t old = xsflib::LoadLe16(&card_.command[at]);
            const uint16_t merged = merge(old);
            xsflib::StoreLe16(&card_.command[at], merged);
            return;
        }
        Store16(offset, value, mask);
        return;
    }
}

void Machine::StartCard()
{
    const uint32_t code = (card_.control >> 24) & 7;
    const std::size_t bytes = code == 0 ? 0 : code == 7 ? 4 : std::size_t{0x100} << code;
    card_.data.assign(bytes, 0xFF);

    switch (card_.command[0])
    {
    case kReadData:
        {
            // Reads below 0x8000 come from 0x8000 plus the address's low 9 bits.
            const uint32_t address = static_cast<uint32_t>(card_.command[1]) << 24 |
                                     static_cast<uint32_t>(card_.command[2]) << 16 |
                                     static_cast<uint32_t>(card_.command[3]) << 8 | card_.command[4];
            for (std::size_t i = 0; i < bytes; i++)
            {
                uint64_t at = uint64_t{address} + i;
                if (at < 0x8000)
                {
                    at = 0x8000 + (at & 0x1FF);
                }
                card_.data[i] = at < image_.size() ? image_[at] : 0xFF;
            }

            counts_.card_reads++;
            counts_.card_bytes += bytes;
            break;
        }
    case kReadChipId:
        for (std::size_t i = 0; i + 4 <= bytes; i += 4)
        {
            xsflib::StoreLe32(&card_.data[i], chip_id_);
        }
        break;
    default:
        Unknown(kIo + kCardCommand, 1, true);
        break;
    }

    card_.position = 0;
    card_.busy = true;
    card_.ready = false;
    const uint64_t cycles = (card_.control & kCardSlowClock) != 0 ? kCardSlowCycles : kCardFastCycles;
    card_.ready_at = Clock() + (8 + (card_.control & kCardGap) + bytes) * cycles;
}

void Machine::CardReady()
{
    card_.ready = true;

    if (card_.data.empty())
    {
        EndCard();
        return;
    }

    RunCardDmas();
}

void Machine::EndCard()
{
    card_.busy = false;
    card_.ready = false;

    if ((card_.spi_control & kCardIrqEnable) == 0)
    {
        return;
    }

    if ((Stored(kExternalMemory) & kCardToArm7) != 0)
    {
        arm7_->RequestIrq(irq::kCard);
    }
    else
    {
        Request(irq::kCard);
    }
}

uint32_t Machine::ReadCardData()
{
    if (!card_.ready || card_.position >= card_.data.size())
    {
        return 0xFFFFFFFF;
    }

    const uint32_t word = xsflib::LoadLe32(&card_.data[card_.position]);
    card_.position += 4;
    if (card_.position >= card_.data.size())
    {
        EndCard();
    }

    return word;
}

void Machine::WriteSpiData(uint8_t value)
{
    if ((card_.spi_control & kSpiMode) == 0)
    {
        return;
    }

    if (!card_.spi_selected)
    {
        card_.spi_position = 0;
    }
    const std::size_t position = card_.spi_position++;

    if (card_.infrared)
    {
        // The infrared chip takes the transfer's first byte.
        if (position == 0)
        {
            card_.spi_command = value;
            card_.spi_data = 0xFF;
        }
        else if (card_.spi_command == kInfraredMemory && backup_ != nullptr)
        {
            card_.spi_data = backup_->Transfer(value, position == 1);
        }
        else
        {
            card_.spi_data = card_.spi_command == kInfraredVersion && position == 1 ? kInfraredChipVersion : 0;
        }

        card_.spi_selected = true;
        if ((card_.spi_control & kSpiHold) == 0)
        {
            EndSpi();
        }
        return;
    }

    if (backup_ != nullptr)
    {
        card_.spi_data = backup_->Transfer(value, position == 0);
        card_.spi_selected = true;
        if ((card_.spi_control & kSpiHold) == 0)
        {
            EndSpi();
        }
        return;
    }

    // A blank backup memory: it reads as 0xFF, and its status as 0, never busy.
    if (!card_.spi_selected)
    {
        card_.spi_selected = true;
        card_.spi_command = value;
        card_.spi_data = 0xFF;
    }
    else
    {
        card_.spi_data = card_.spi_command == kReadStatus ? 0x00 : 0xFF;
    }

    if ((card_.spi_control & kSpiHold) == 0)
    {
        card_.spi_selected = false;
    }
}

void Machine::EndSpi()
{
    if (backup_ != nullptr && (!card_.infrared || (card_.spi_command == kInfraredMemory && card_.spi_position > 1)))
    {
        backup_->End();
    }

    card_.spi_selected = false;
}

} // namespace ntdlib::ntr
