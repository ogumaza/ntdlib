// SPDX-License-Identifier: MIT

// The audio interface's registers (physical 0x0C006C00): the control register (AICR), the volume, the sample counter
// and its interrupt timing. The counter counts the samples that the interface would stream from the disc. The OS reads
// it to time audio. The DMA that feeds the interface from main memory belongs to the DSP interface.

#pragma once

#include <cstdint>
#include <optional>

namespace ntdlib::flipper
{

// What differs between the GameCube's and the Wii's audio interfaces: the sample counter's rates. The counter counts
// once every clock * multiplier / divisor CPU cycles, at 32 kHz (AISFR clear) or at 48 kHz (AISFR set).
struct AudioInterfaceOptions
{
    double clock; // the CPU's clock in Hz
    double multiplier_32k;
    double divisor_32k;
    double multiplier_48k;
    double divisor_48k;
};

// The options for each console. The GameCube's rates are 108 MHz / 3,372 and 108 MHz / 2,248.
inline constexpr AudioInterfaceOptions kGameCubeAudioInterface{486'000'000.0, 3'372.0, 108'000'000.0, 2'248.0,
                                                               108'000'000.0};
inline constexpr AudioInterfaceOptions kWiiAudioInterface{729'000'000.0, 1.0, 32'000.0, 1.0, 48'000.0};

// A value type. A machine keeps it with its other registers and copies it in its snapshots.
class AudioInterface
{
public:
    explicit AudioInterface(const AudioInterfaceOptions& options) : options_(&options)
    {
    }

    // The 32-bit register at `offset`, at CPU cycle `now`.
    uint32_t Read(uint32_t offset, uint64_t now);

    // Writes the 32-bit register at `offset` at CPU cycle `now`. A write to AICR returns the DMA rate that its AIDFR
    // bit selects: true for 32 kHz. The machine passes it to the DSP interface.
    std::optional<bool> Write(uint32_t offset, uint32_t value, uint64_t now);

private:
    // Counts the samples up to `now`.
    void AdvanceSampleCounter(uint64_t now);

    const AudioInterfaceOptions* options_;
    uint32_t control_ = 0;
    uint32_t volume_ = 0;
    uint32_t interrupt_timing_ = 0;
    uint32_t samples_ = 0;
    double next_count_ = 0.0; // the CPU cycle of the counter's next count
    double period_ = 0.0;     // and the cycles between counts
};

} // namespace ntdlib::flipper
