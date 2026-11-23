// SPDX-License-Identifier: MIT

#include "ndma.h"

#include <cstdint>
#include <string>
#include <utility>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "io.h"

namespace ntdlib::ntr
{

namespace
{

// A channel's registers, from its first: the source and destination addresses, the total length, the logical block's
// size, the interval, the fill data and the control.
constexpr uint32_t kChannelBase = 0x04;
constexpr uint32_t kChannelStride = 0x1C;
constexpr uint32_t kSource = 0x00;
constexpr uint32_t kDestination = 0x04;
constexpr uint32_t kTotal = 0x08;
constexpr uint32_t kBlock = 0x0C;
constexpr uint32_t kFill = 0x14;
constexpr uint32_t kControl = 0x18;

// The control's fields.
constexpr uint32_t kEnable = 1u << 31;
constexpr uint32_t kIrqEnable = 1u << 30;
constexpr uint32_t kRepeatForever = 1u << 29;
constexpr uint32_t kSourceReload = 1u << 15;
constexpr uint32_t kDestinationReload = 1u << 12;
constexpr uint32_t kFirstImmediateMode = 0x10;

uint32_t ModeOf(uint32_t control)
{
    return (control >> 24) & 0x1F;
}

// The step of an address mode: 0 increments, 1 decrements, 2 and 3 stay.
uint32_t Step(uint32_t mode)
{
    return mode == 0 ? 4 : mode == 1 ? static_cast<uint32_t>(-4) : 0;
}

uint32_t TotalWords(uint32_t value)
{
    const uint32_t words = value & 0x0FFFFFFF;
    return words == 0 ? 0x10000000 : words;
}

uint32_t BlockWords(uint32_t value)
{
    const uint32_t words = value & 0xFFFFFF;
    return words == 0 ? 0x1000000 : words;
}

} // namespace

Ndma::Ndma(arm::Bus& bus, std::function<void(uint32_t irq)> request, std::function<void(std::string)> fail)
    : bus_(bus), request_(std::move(request)), fail_(std::move(fail))
{
}

uint32_t Ndma::Read(uint32_t offset, int size) const
{
    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= uint32_t{bytes_[offset - kFirstRegister + static_cast<uint32_t>(i)]} << (8 * i);
    }

    return value;
}

void Ndma::Write(uint32_t offset, int size, uint32_t value)
{
    std::array<bool, 4> was_enabled = {};
    for (int channel = 0; channel < 4; channel++)
    {
        was_enabled[static_cast<std::size_t>(channel)] = (Register(channel, kControl) & kEnable) != 0;
    }

    for (int i = 0; i < size; i++)
    {
        bytes_[offset - kFirstRegister + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
    }

    for (int channel = 0; channel < 4; channel++)
    {
        if (!was_enabled[static_cast<std::size_t>(channel)] && (Register(channel, kControl) & kEnable) != 0)
        {
            Start(channel);
        }
    }
}

bool Ndma::WaitsForCard() const
{
    for (int channel = 0; channel < 4; channel++)
    {
        const uint32_t control = Register(channel, kControl);
        if ((control & kEnable) != 0 && ModeOf(control) == kCardMode)
        {
            return true;
        }
    }

    return false;
}

void Ndma::CardWord()
{
    for (int channel = 0; channel < 4; channel++)
    {
        const uint32_t control = Register(channel, kControl);
        if ((control & kEnable) != 0 && ModeOf(control) == kCardMode)
        {
            Unit(channel);
        }
    }
}

uint32_t Ndma::Register(int channel, uint32_t field) const
{
    return xsflib::LoadLe32(&bytes_[kChannelBase + kChannelStride * static_cast<uint32_t>(channel) + field]);
}

void Ndma::SetRegister(int channel, uint32_t field, uint32_t value)
{
    xsflib::StoreLe32(&bytes_[kChannelBase + kChannelStride * static_cast<uint32_t>(channel) + field], value);
}

void Ndma::Start(int channel)
{
    Channel& state = channels_[static_cast<std::size_t>(channel)];
    const uint32_t control = Register(channel, kControl);
    const uint32_t mode = ModeOf(control);
    state.source = Register(channel, kSource) & ~3u;
    state.destination = Register(channel, kDestination) & ~3u;
    state.block_left = BlockWords(Register(channel, kBlock));
    state.total_left = TotalWords(Register(channel, kTotal));

    if (mode >= kFirstImmediateMode)
    {
        // One logical block, with no repeat.
        state.total_left = state.block_left;
        while ((Register(channel, kControl) & kEnable) != 0)
        {
            Unit(channel);
        }
        return;
    }
    if (mode == kCardMode)
    {
        if (on_card_start)
        {
            on_card_start();
        }
        return;
    }

    fail_("NDMA channel " + std::to_string(channel) + " starts in mode " + xsflib::Hex(mode, 2, true) +
          ", which the machine doesn't model");
}

void Ndma::Unit(int channel)
{
    Channel& state = channels_[static_cast<std::size_t>(channel)];
    const uint32_t control = Register(channel, kControl);
    const uint32_t source_mode = (control >> 13) & 3;
    const uint32_t destination_mode = (control >> 10) & 3;

    const uint32_t word = source_mode == 3 ? Register(channel, kFill) : LoadFromBus(bus_, state.source, 4);
    StoreToBus(bus_, state.destination, 4, word);
    state.source += Step(source_mode);
    state.destination += Step(destination_mode);
    state.total_left--;
    state.block_left--;
    if (state.block_left == 0 || state.total_left == 0)
    {
        EndBlock(channel);
    }
}

void Ndma::EndBlock(int channel)
{
    Channel& state = channels_[static_cast<std::size_t>(channel)];
    uint32_t control = Register(channel, kControl);
    state.block_left = BlockWords(Register(channel, kBlock));

    // A transfer that repeats until its total length goes on with its next block, with no reload and no IRQ.
    const bool forever = (control & kRepeatForever) != 0 && ModeOf(control) < kFirstImmediateMode;
    if (!forever && state.total_left != 0)
    {
        return;
    }

    if ((control & kSourceReload) != 0)
    {
        state.source = Register(channel, kSource) & ~3u;
    }
    if ((control & kDestinationReload) != 0)
    {
        state.destination = Register(channel, kDestination) & ~3u;
    }
    if (!forever)
    {
        control &= ~kEnable;
        SetRegister(channel, kControl, control);
    }
    if ((control & kIrqEnable) != 0)
    {
        request_(kIrq0 << channel);
    }
}

} // namespace ntdlib::ntr
