// SPDX-License-Identifier: MIT

#include "ntdlib/flipper/audio_interface.h"

#include <cmath>
#include <cstdint>
#include <optional>

namespace ntdlib::flipper
{
namespace
{

// AICR's bits: PSTAT (0) runs the sample counter. AISFR (1) selects its rate (48 kHz when set). AIINT (3) clears when
// written with 1. SCRESET (5) resets the counter and reads back 0. AIDFR (6) selects the DMA's rate (32 kHz when set).
constexpr uint32_t kPlay = 1u << 0;
constexpr uint32_t kRate48k = 1u << 1;
constexpr uint32_t kDmaRate32k = 1u << 6;

} // namespace

uint32_t AudioInterface::Read(uint32_t offset, uint64_t now)
{
    switch (offset)
    {
    case 0x00:
        return control_;

    case 0x04:
        return volume_;

    case 0x08:
        AdvanceSampleCounter(now);
        return samples_;

    case 0x0c:
        return interrupt_timing_;

    default:
        return 0;
    }
}

std::optional<bool> AudioInterface::Write(uint32_t offset, uint32_t value, uint64_t now)
{
    switch (offset)
    {
    case 0x00:
        {
            AdvanceSampleCounter(now);
            const uint32_t old = control_;
            control_ = (control_ & 0x08 & ~value) | (value & ~0x28u);
            if (value & 0x20)
            {
                samples_ = 0;
            }

            if ((control_ & kPlay) && (!(old & kPlay) || ((control_ ^ old) & kRate48k)))
            {
                const bool rate_48k = (control_ & kRate48k) != 0;
                period_ = options_->clock * (rate_48k ? options_->multiplier_48k : options_->multiplier_32k) /
                          (rate_48k ? options_->divisor_48k : options_->divisor_32k);
                next_count_ = static_cast<double>(now) + period_;
            }

            return (value & kDmaRate32k) != 0;
        }

    case 0x04:
        volume_ = value;
        break;

    case 0x08:
        AdvanceSampleCounter(now);
        samples_ = value & 0x7fffffff;
        break;

    case 0x0c:
        interrupt_timing_ = value;
        break;

    default:
        break;
    }

    return std::nullopt;
}

void AudioInterface::AdvanceSampleCounter(uint64_t now)
{
    const auto cycle = static_cast<double>(now);
    if (!(control_ & kPlay) || cycle < next_count_)
    {
        return;
    }

    const double counts = std::floor((cycle - next_count_) / period_) + 1.0;
    samples_ = (samples_ + static_cast<uint32_t>(counts)) & 0x7fffffff;
    next_count_ += counts * period_;
}

} // namespace ntdlib::flipper
