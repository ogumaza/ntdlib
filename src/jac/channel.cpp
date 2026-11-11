// SPDX-License-Identifier: MIT

// The C JAudio's logical channels: their start, their stop, the queue of channels that wait for a DSP channel and
// the DSP channels' callback. channel_dsp.cpp, channel_lists.cpp, channel_levels.cpp and channel_oneshot.cpp
// contain the rest of Channels. The five files build as one translation unit. The compiler can then inline the
// small helpers across them.

#include "ntdlib/jac/channel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "jac/channel_internal.h"

namespace ntdlib::jac
{

namespace
{

// The DSP's buses by a mixer's bus number (0x5bf4ce in main's ro). Number 10 names 0x0a00 on the Switch. The table
// holds 25 entries before other data. The model gives 0 for a number past them.
constexpr std::array<uint16_t, 25> kBuses = {0x0000, 0x0d00, 0x0d60, 0x0dc0, 0x0e20, 0x0e80,
                                             0x0ee0, 0x0ca0, 0x0f40, 0x0fa0, 0x0a00, 0x09a0};

// By a wave's format (0 to 7, 0x5b7048 and 0x5b7000 in main's ro): the samples that the DSP's first block holds, and
// the bytes of a block of 16 samples. No wave of the bank file has a format above 3. The model writes 0 for a format
// above 7.
constexpr std::array<uint8_t, 8> kFirstBlockSamples = {0x10, 0x10, 1, 1, 1, 0x10, 0x10, 1};
constexpr std::array<uint8_t, 8> kBlockBytes = {9, 5, 8, 16, 1, 1, 1, 1};

// DSP_PlayStart: JASDsp's start, and the PCM position at 0.
void StartBlock(DspBlock& block)
{
    block.PlayStart();
    block.SetU32(DspChannelBlock::kPcmPosition, 0);
}

// DSP_SetWaveInfo: the wave's address, format and loop. The PCM position keeps its value, and no part of the wave is
// skipped.
void SetWaveInfo(DspBlock& block, const WaveRecord& record, uint32_t address)
{
    const jaudio2::WaveInfo& wave = record.info;
    const bool known = wave.format < kBlockBytes.size();
    block.SetU32(DspChannelBlock::kWaveAddress, address);
    block.SetU16(DspChannelBlock::kLeftover, known ? kFirstBlockSamples[wave.format] : 0);
    block.SetU16(DspChannelBlock::kFormat, known ? kBlockBytes[wave.format] : 0);
    if (wave.format > 3)
    {
        return;
    }

    block.SetU32(kSampleCount, wave.sample_count);
    const auto loops = static_cast<uint16_t>(wave.loop_flags);
    block.SetU16(DspChannelBlock::kLoops, loops);
    if (loops != 0)
    {
        block.SetU32(DspChannelBlock::kLoopStart, wave.loop_start);
        block.SetU32(DspChannelBlock::kLoopEnd, wave.loop_end);
        block.SetU16(DspChannelBlock::kLoopLast, static_cast<uint16_t>(wave.last));
        block.SetU16(DspChannelBlock::kLoopPenult, static_cast<uint16_t>(wave.penult));
    }
    else
    {
        block.SetU32(DspChannelBlock::kLoopEnd, wave.sample_count);
    }
    ClearBlock(block, DspChannelBlock::kSavedBlock, DspChannelBlock::kDelayHistory);
}

// DSP_SetBusConnect.
void SetBusConnect(DspBlock& block, uint32_t mixer, uint32_t bus)
{
    const uint32_t number = bus & 0xff;
    block.SetU16(MixerOffset(mixer), number < kBuses.size() ? kBuses[number] : 0);
}

} // namespace

Channels::Channels(DspMemory& memory, uint32_t blocks, const Banks& banks, Random& random)
    : memory_(memory), blocks_(blocks), banks_(banks), random_(random)
{
    // DSP_InitBuffer (0x277400 in main): each block clear, then the DSP channels and the channels.
    waits_.channels.fill(kNone);
    for (uint32_t i = 0; i < kDspChannels; i++)
    {
        DspBlock block = Block(i);
        ClearBlock(block, 0, DspChannelBlock::kBytes);
    }
    InitDspChannels();
    InitGlobalChannels();
}

bool Channels::StopLogical(uint32_t channel)
{
    // StopLogicalChannel (0x2757e0 in main).
    Channel& c = channels_[channel];
    if (c.dsp_channel == kNone)
    {
        return false;
    }

    DspChannel& d = dsp_channels_[c.dsp_channel];
    d.callback = false;
    d.timer = 0;
    Block(d.block).PlayStop();
    DeallocDsp(c.dsp_channel, channel);
    c.dsp_channel = kNone;

    return true;
}

bool Channels::CheckLogical(uint32_t channel) const
{
    // CheckLogicalChannel (0x275850 in main).
    const Channel& c = channels_[channel];

    return c.wave != nullptr || c.kind != Channel::kWave;
}

bool Channels::PlayLogical(uint32_t channel)
{
    // PlayLogicalChannel (0x275870 in main).
    Channel& c = channels_[channel];
    if (c.dsp_channel == kNone || (c.wave == nullptr && c.kind == Channel::kWave))
    {
        return false;
    }

    DspChannel& d = dsp_channels_[c.dsp_channel];
    DspBlock block = Block(d.block);
    d.callback = true;
    d.timer = 1;
    if (c.kind == Channel::kOscillator)
    {
        block.SetOscInfo(c.source);
    }
    else if (c.kind == Channel::kWave)
    {
        SetWaveInfo(block, *c.wave, c.source);
    }

    // Mono sends bus 9 to bus 2 and bus 8 to bus 11.
    for (uint32_t i = 0; i < c.mix_configs.size(); i++)
    {
        uint32_t bus = c.mix_configs[i] >> 8;
        if (kOutputMode == 0)
        {
            if (bus == 9)
            {
                bus = 2;
            }
            else if (bus == 8)
            {
                bus = 11;
            }
        }
        SetBusConnect(block, i, bus);
    }

    c.last_manager = c.manager;
    UpdateEffecterParam(channel);
    UpdateToDspInit(channel);
    d.priority = static_cast<uint8_t>(c.priority);
    d.release_time = c.release_time;
    StartBlock(block);

    return true;
}

void Channels::KillBrokenChannels(uint32_t dsp)
{
    // In CommonCallbackLogicalChannel: each channel that points at the DSP channel stops it, and goes into its
    // manager's free list.
    for (uint32_t channel = 0; channel < kChannels; channel++)
    {
        Channel& c = channels_[channel];
        if (c.dsp_channel != dsp)
        {
            continue;
        }

        StopLogical(channel);
        MoveToFree(channel);
    }
}

int32_t Channels::Callback(uint32_t dsp, uint32_t event)
{
    // CommonCallbackLogicalChannel (0x275ae0 in main). It returns the frames to the next update, or -1 after a
    // forced stop. UpdateDspChannels raises only the update, the finish and the drop in priority. The model leaves out
    // the game's work for the other events: a break (event 3, from BreakLowerDSPchannel) and any other event.
    DspChannel& d = dsp_channels_[dsp];
    const uint32_t owner = d.channel;
    if (owner == kNone)
    {
        d.callback = false;
        d.priority = 0;
        KillBrokenChannels(dsp);
        return 0;
    }

    Channel& c = channels_[owner];
    if (c.dsp_channel != dsp)
    {
        // The DSP channel's owner has another DSP channel, or none.
        if (c.dsp_channel != kNone && dsp_channels_[c.dsp_channel].channel == owner)
        {
            KillBrokenChannels(dsp);
        }
        else
        {
            StopLogical(owner);
            MoveToFree(owner);
        }
        d.priority = 0;
        d.channel = kNone;
        d.callback = false;
        return 0;
    }

    if (event == DspChannel::kFinish)
    {
        if (c.callback != Channel::kNoCallback)
        {
            RunCallback(owner, kWaveEnd);
            return 0;
        }

        StopLogical(owner);
        MoveToFree(owner);
        return 0;
    }

    // A wave whose load the game has undone (its scene closed) stops at once.
    if (c.wave != nullptr && banks_.RecordStatus(*c.wave) == 0)
    {
        ForceStopDsp(dsp);
        return -1;
    }

    if (event == DspChannel::kPriority)
    {
        d.priority = std::min(d.priority, static_cast<uint8_t>(c.priority >> 16));
        return 0;
    }

    return UpdateChannel(owner, dsp);
}

int32_t Channels::UpdateChannel(uint32_t channel, uint32_t dsp)
{
    // The update in CommonCallbackLogicalChannel: the oscillators, the levels, the sweep and the note's time.
    Channel& c = channels_[channel];
    c.pitch_modifier = 1.0f;
    c.volume_modifier = 1.0f;
    c.pans[Channel::kPan][Channel::kEffect] = 0.5f;
    c.pans[Channel::kFxmix][Channel::kEffect] = 0.0f;
    c.pans[Channel::kDolby][Channel::kEffect] = 0.0f;

    bool moving = false;
    for (std::size_t k = 0; k < c.oscillators.size(); k++)
    {
        const OscData* osc = c.oscillators[k];
        if (osc == nullptr)
        {
            continue;
        }

        const float value = OscToOfs(osc, c.osc_states[k]);
        DoEffectOsc(channel, osc->target, value);
        if (c.osc_states[k].state == OscState::kStop)
        {
            // The envelope has ended.
            if (c.callback != Channel::kNoCallback)
            {
                RunCallback(channel, kEnvelopeEnd);
                return 0;
            }

            if (c.dsp_channel != kNone)
            {
                StopLogical(channel);
            }
            else
            {
                Block(dsp_channels_[dsp].block).PlayStop();
            }
            MoveToFree(channel);
            return 0;
        }
        moving = true;
    }

    if (moving)
    {
        UpdateEffecterParam(channel);
        c.flush = 1;
    }
    if (c.sweep != Channel::kNoSweep && SweepUpdate(channel))
    {
        c.flush++;
    }
    if (c.callback == Channel::kNoCallback)
    {
        return static_cast<int32_t>(kFrameStep);
    }

    if (c.countdown >= 1)
    {
        if (static_cast<uint32_t>(c.countdown) <= kFrameStep)
        {
            c.countdown = 0;
            RunCallback(channel, kTimeOver);
            c.countdown = c.time;
        }
        else
        {
            c.countdown -= static_cast<int32_t>(kFrameStep);
        }
    }
    else if (c.countdown == 0)
    {
        RunCallback(channel, kTimeOver);
        c.countdown = c.time;
    }

    if (c.flush != 0)
    {
        UpdateToDsp(channel);
        c.flush = 0;
    }

    return static_cast<int32_t>(kFrameStep);
}

bool Channels::ResetInitialVolume(uint32_t channel)
{
    // ResetInitialVolume (0x276350 in main).
    const Channel& c = channels_[channel];
    if (c.dsp_channel == kNone || (c.wave == nullptr && c.kind == Channel::kWave))
    {
        return false;
    }

    UpdateEffecterParam(channel);
    UpdateToDspInit(channel);

    return true;
}

// driverinterface: the queue of waiting channels.

bool Channels::AddWait(uint32_t channel)
{
    // Add_WaitDSPChannel (0x2763c0 in main).
    if (waits_.count == WaitQueue::kSize)
    {
        return false;
    }

    waits_.times[waits_.tail] = 0;
    waits_.channels[waits_.tail] = channel;
    waits_.count++;
    waits_.tail++;
    if (waits_.tail == WaitQueue::kSize)
    {
        waits_.tail = 0;
    }

    return true;
}

bool Channels::DeleteWait(uint32_t channel)
{
    // Del_WaitDSPChannel (0x276420 in main).
    for (uint32_t k = 0; k < waits_.count; k++)
    {
        const uint32_t slot = (waits_.top + k) & (WaitQueue::kSize - 1);
        if (waits_.channels[slot] == channel)
        {
            waits_.channels[slot] = kNone;
            return true;
        }
    }

    return false;
}

void Channels::EntryWait()
{
    // __Entry_WaitChannel (0x276480 in main) as DSP_EntryCheck calls it: the queue's channels in order on the free DSP
    // channels, until the queue is empty or no DSP channel is free. A started channel's slot keeps its value. The game
    // can also stop after the first channel that starts. Nothing in the model asks for that.
    const auto advance = [this]
    {
        waits_.top = waits_.top + 1 == WaitQueue::kSize ? 0 : waits_.top + 1;
        waits_.count--;
    };

    while (waits_.count != 0)
    {
        const uint32_t channel = waits_.channels[waits_.top];
        if (channel == kNone)
        {
            advance();
            continue;
        }

        const uint32_t dsp = AllocDsp(channel);
        if (dsp == kNone)
        {
            return;
        }

        Channel& c = channels_[channel];
        c.dsp_channel = dsp;
        PlayLogical(channel);
        if (ListCut(channel) != -1)
        {
            ListAddTail(c.manager, ChannelManager::kActive, channel);
        }
        advance();
    }
}

void Channels::EntryCheck()
{
    // DSP_EntryCheck (0x276680 in main): the waiting channels start where they can. Then each channel that still
    // waits counts its frames. Its note's time runs down, and a note whose time is over leaves the queue.
    EntryWait();
    uint32_t count = waits_.count;
    uint32_t top = waits_.top;
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t slot = (top + k) & (WaitQueue::kSize - 1);
        const uint32_t channel = waits_.channels[slot];
        if (channel == kNone)
        {
            continue;
        }

        waits_.times[slot]++;
        Channel& c = channels_[channel];
        int32_t time = c.time;
        if (time >= 1)
        {
            time--;
            c.time = time;
        }
        if (time != 0)
        {
            continue;
        }

        RunCallback(channel, kStoppedWaiting);
        top = waits_.top;
        waits_.channels[(k + top) & (WaitQueue::kSize - 1)] = kNone;
        count = waits_.count;
    }
}

bool Channels::ForceStopLogical(uint32_t channel)
{
    // ForceStopLogicalChannel (0x276740 in main).
    const Channel& c = channels_[channel];
    if (c.dsp_channel == kNone)
    {
        return false;
    }

    ForceStopDsp(c.dsp_channel);

    return true;
}

void Channels::MoveToFree(uint32_t channel)
{
    if (ListCut(channel) != -1)
    {
        ListAdd(channels_[channel].manager, ChannelManager::kFree, channel);
    }
}

// oneshot.

uint32_t Channels::GetLogical(uint32_t manager, const WaveHandle* handle)
{
    // Get_LogicalChannel (0x279330 in main): a channel from the manager's free list, or one more from the global
    // manager. A manager that steals then also stops a releasing or a playing channel and moves it to its waiting
    // list. The channel that it takes keeps playing.
    if (handle != nullptr && banks_.HandleStart(*handle) == 0)
    {
        return kNone;
    }

    ChannelManager& m = managers_[manager];
    uint32_t channel = ListGet(manager, ChannelManager::kFree);
    if (channel == kNone)
    {
        if (FixAlloc(manager, 1) == 0)
        {
            return kNone;
        }

        m.allocations++;
        channel = ListGet(manager, ChannelManager::kFree);
        if (channel == kNone)
        {
            return kNone;
        }

        if (m.stealing)
        {
            uint32_t victim = ListGet(manager, ChannelManager::kReleasing);
            if (victim == kNone)
            {
                victim = ListGet(manager, ChannelManager::kActive);
            }
            if (victim != kNone)
            {
                Channel& v = channels_[victim];
                v.osc_states[0].state = OscState::kForceStop;
                ListAdd(manager, ChannelManager::kWaiting, victim);
                if (v.dsp_channel != kNone)
                {
                    ForceStopDsp(v.dsp_channel);
                }
            }
        }
    }

    InitChannel(channel);
    Channel& c = channels_[channel];
    if (handle != nullptr)
    {
        c.wave = banks_.Record(*handle);
        c.source = banks_.HandleStart(*handle);
        c.kind = Channel::kWave;
    }
    c.pans[Channel::kPower] = {kThird, kThird, kThird};

    return channel;
}

} // namespace ntdlib::jac
