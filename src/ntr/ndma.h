// SPDX-License-Identifier: MIT

// The DSi's new DMA (NDMA, GBATEK's "DSi New DMA (NDMA)") of one CPU: four channels at 0x04004104 + 0x1C * n, and the
// global control at 0x04004100. A channel starts when its control's bit 31 goes from 0 to 1. It moves words: from the
// source address, or the fill data when the source mode is 3, to the destination, with each address stepping up, down
// or not at all. A transfer that starts at once (modes 0x10 to 0x1F) moves one logical block and stops. A transfer that
// the card starts (mode 4) moves a word for each word that the card gives. It repeats its logical blocks forever with
// bit 29, or else until the total length. The model doesn't time the transfers: each unit moves at the moment that
// starts it. The other start modes aren't modelled, and a channel that starts in one fails.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "ntdlib/arm/cpu.h"

namespace ntdlib::ntr
{

class Ndma
{
public:
    // The registers' offsets from 0x04004000.
    static constexpr uint32_t kFirstRegister = 0x100;
    static constexpr uint32_t kRegistersEnd = 0x174;

    // The IF bit of channel 0. Channels 1 to 3 follow.
    static constexpr uint32_t kIrq0 = 1u << 28;

    // The start mode of the card's transfers.
    static constexpr uint32_t kCardMode = 0x04;

    // Channels that move words on `bus`. `request` requests the CPU's interrupts, and `fail` stops the machine with a
    // message.
    Ndma(arm::Bus& bus, std::function<void(uint32_t irq)> request, std::function<void(std::string)> fail);

    uint32_t Read(uint32_t offset, int size) const;
    void Write(uint32_t offset, int size, uint32_t value);

    // Whether a channel waits for the card's words.
    bool WaitsForCard() const;

    // Moves one unit of each channel that waits for the card's words.
    void CardWord();

    // Called when a channel starts in the card's mode. The card then gives it the words that are ready.
    std::function<void()> on_card_start;

private:
    struct Channel
    {
        uint32_t source = 0;
        uint32_t destination = 0;
        uint32_t total_left = 0; // the words left of a transfer that repeats until its total length
        uint32_t block_left = 0; // the words left of the logical block
    };

    uint32_t Register(int channel, uint32_t field) const;
    void SetRegister(int channel, uint32_t field, uint32_t value);
    void Start(int channel);
    void Unit(int channel);
    void EndBlock(int channel);

    arm::Bus& bus_;
    std::function<void(uint32_t)> request_;
    std::function<void(std::string)> fail_;
    std::array<uint8_t, kRegistersEnd - kFirstRegister> bytes_ = {};
    std::array<Channel, 4> channels_ = {};
};

} // namespace ntdlib::ntr
