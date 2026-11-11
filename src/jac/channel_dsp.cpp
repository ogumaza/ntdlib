// SPDX-License-Identifier: MIT

// The DSP channels (dspdriver): their allocation, their release and their update in each frame, with the writes
// into their blocks (dspinterface) (see channel.cpp).

#include "ntdlib/jac/channel.h"

#include <cstdint>

#include "jac/channel_internal.h"
#include "ntdlib/jaudio2/calc.h"

namespace ntdlib::jac
{

namespace
{

// DSP_AllocInit: the pause, the forced stop and the start flags clear, the auto mixer off, the FIR and IIR filters
// passing the signal through, no distortion. The FIR's last 12 coefficients, the space after them and the PCM
// position keep their values.
void AllocInit(DspBlock& block)
{
    block.SetU16(DspChannelBlock::kPause, 0);
    block.SetU16(DspChannelBlock::kForcedStop, 0);
    block.SetU32(DspChannelBlock::kActive, 0);
    block.SetU16(DspChannelBlock::kAutoMixer, 0);
    ClearBlock(block, DspChannelBlock::kFirCoefficients, DspChannelBlock::kFirCoefficients + 0x10);
    block.SetU16(DspChannelBlock::kFirCoefficients, 0x7fff);
    ClearBlock(block, DspChannelBlock::kIirCoefficients, DspChannelBlock::kIirCoefficients + 8);
    block.SetU16(DspChannelBlock::kIirCoefficients, 0x7fff);
    block.SetU16(DspChannelBlock::kDist, 0);
}

} // namespace

void Channels::SetMixerLevel(float level)
{
    // Channel_SetMixerLevel (0x274240 in main).
    mixer_level_ = static_cast<uint16_t>(ToInt32(level * 32767.0f));
}

// dspdriver.

void Channels::InitDspChannels()
{
    // InitDSPchannel (0x276760 in main).
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspChannel& d = dsp_channels_[i];
        d = DspChannel{};
        d.block = static_cast<uint8_t>(i);
    }
}

uint32_t Channels::AllocDsp(uint32_t channel)
{
    // DSP_AllocDSPchannel (0x276790 in main) for a mono channel: the first free DSP channel.
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspChannel& d = dsp_channels_[i];
        if (d.state == DspChannel::kFree)
        {
            d.state = DspChannel::kMono;
            d.channel = channel;
            d.priority = 1;
            DspBlock block = Block(i);
            AllocInit(block);

            return i;
        }
    }

    return kNone;
}

int32_t Channels::DeallocDsp(uint32_t dsp, uint32_t channel)
{
    // DSP_DeAllocDSPchannel (0x276880 in main). The model allocates no stereo pairs.
    if (dsp == kNone)
    {
        return -1;
    }

    DspChannel& d = dsp_channels_[dsp];
    if (d.channel != channel)
    {
        return -2;
    }

    if (d.state >= DspChannel::kMono && d.state <= DspChannel::kStopping)
    {
        d.state = DspChannel::kFree;
    }
    d.priority = 0;
    d.channel = kNone;
    d.callback = false;

    return 0;
}

bool Channels::ForceStopDsp(uint32_t dsp)
{
    // ForceStopDSPchannel (0x276950 in main).
    DspChannel& d = dsp_channels_[dsp];
    DspBlock block = Block(d.block);
    if (d.state == DspChannel::kStopping || block.U16(DspChannelBlock::kActive) == 0)
    {
        return false;
    }

    block.SetU16(DspChannelBlock::kForcedStop, 1);
    d.state = DspChannel::kStopping;

    return true;
}

void Channels::UpdateDspChannels()
{
    // UpdateDSPchannelAll (0x276b60 in main).
    EntryCheck();
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspChannel& d = dsp_channels_[i];
        if (d.state == DspChannel::kFree)
        {
            continue;
        }

        DspBlock block = Block(d.block);
        if (block.U16(DspChannelBlock::kFinished) != 0)
        {
            if (d.callback)
            {
                d.timer = static_cast<uint16_t>(Callback(i, DspChannel::kFinish));
            }
            block.SetU32(DspChannelBlock::kActive, 0);
            if (d.state == DspChannel::kFree)
            {
                continue;
            }
        }

        if (block.U16(DspChannelBlock::kForcedStop) == 0)
        {
            const uint32_t frames = block.U32(kPlayedFrames) + 1;
            block.SetU32(kPlayedFrames, frames);
            if (frames == d.release_time && d.callback)
            {
                d.timer = static_cast<uint16_t>(Callback(i, DspChannel::kPriority));
            }
        }

        if (!d.callback)
        {
            continue;
        }

        if (d.timer != 0)
        {
            d.timer--;
            if (d.timer != 0)
            {
                continue;
            }
        }

        d.timer = static_cast<uint16_t>(Callback(i, DspChannel::kUpdate));
        if (d.timer == 0)
        {
            block.SetU32(DspChannelBlock::kActive, 0);
        }
    }
}

} // namespace ntdlib::jac
