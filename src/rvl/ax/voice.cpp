// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/ax/voice.h"

#include <cstdint>

#include "ntdlib/rvl/ax/pb.h"

namespace ntdlib::rvl::ax
{

void Voice::SetState(VoiceState state)
{
    if (pb.state == static_cast<uint16_t>(state))
    {
        return;
    }

    pb.state = static_cast<uint16_t>(state);
    sync |= sync::kState;
    if (state == VoiceState::kStop)
    {
        depop = true;
    }
}

void Voice::SetAddr(const PbAddr& address)
{
    pb.addr = address;

    // PCM has no frames to decode. The decoder gets no coefficients and a gain that scales the samples to 16 bits.
    switch (static_cast<SampleFormat>(address.format))
    {
    case SampleFormat::kPcm16:
        pb.adpcm = PbAdpcm{};
        pb.adpcm.gain = kPcm16Gain;
        break;

    case SampleFormat::kPcm8:
        pb.adpcm = PbAdpcm{};
        pb.adpcm.gain = kPcm8Gain;
        break;

    default:
        break;
    }

    sync &= ~(sync::kLoopFlag | sync::kLoopAddr | sync::kEndAddr | sync::kCurrAddr);
    sync |= sync::kAddr | sync::kAdpcm;
}

} // namespace ntdlib::rvl::ax
