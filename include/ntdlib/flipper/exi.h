// SPDX-License-Identifier: MIT

// The external interface (EXI), at physical 0x0C006800 (the Wii mirrors it at 0x0D006800): three channels of serial
// transfers, immediate or by DMA. The only device is the one that the OS reads at boot: device 1 on channel 0. It holds
// the real-time clock and the 64-byte SRAM of console settings. The memory card slots and the other devices are empty.
// Transfers finish at once.

#pragma once

#include <array>
#include <cstdint>

namespace ntdlib::flipper
{

// The channels' registers and the device's state. A snapshot of the system copies them (see Exi::Save).
class ExiState
{
protected:
    struct Channel
    {
        uint32_t status = 0; // CSR
        uint32_t dma_address = 0;
        uint32_t dma_length = 0;
        uint32_t control = 0;
        uint32_t data = 0;
    };

    std::array<Channel, 3> channels_{};

    // The RTC/SRAM device: the command of the transfer in progress, and where its data goes.
    bool selected_ = false;
    bool have_command_ = false;
    uint32_t command_ = 0;
    uint32_t position_ = 0;
    std::array<uint8_t, 64> sram_{};
    uint32_t rtc_ = 0;
};

// What differs between the GameCube's and the Wii's interfaces.
struct ExiOptions
{
    // Whether SRAM's settings select stereo sound (bit 2 of the flags at 0x13). The OS's OSGetSoundMode reads that bit.
    // Without it, the GameCube's sound libraries mix in mono.
    bool stereo;
};

// The options for each console.
inline constexpr ExiOptions kGameCubeExi{true};
inline constexpr ExiOptions kWiiExi{false};

// The external interface with main memory of type `Memory` (dsp::GameCubeMemory or dsp::WiiMemory) for its DMA.
template <typename Memory>
class Exi : private ExiState
{
public:
    // The interface's state as Save captures it.
    using Snapshot = ExiState;

    Exi(Memory& memory, const ExiOptions& options);

    // A 32-bit register at `offset` from 0x0C006800.
    uint32_t Read(uint32_t offset) const;
    void Write(uint32_t offset, uint32_t value);

    // Whether a channel's transfer-complete or external interrupt is pending and enabled: the EXI line to the processor
    // interface.
    bool InterruptPending() const;

    // The interface's state between runs of the CPU, and its restoration.
    Snapshot Save() const
    {
        return *this;
    }

    void Restore(const Snapshot& snapshot)
    {
        ExiState::operator=(snapshot);
    }

private:
    // The RTC/SRAM device's side of a transfer of `size` bytes. For a read, the device's reply replaces `bytes`. The
    // reply depends on the bytes that the device received.
    void DeviceTransfer(uint8_t* bytes, uint32_t size, bool write);
    void Transfer(Channel& channel, int index);

    Memory& memory_;
};

} // namespace ntdlib::flipper
