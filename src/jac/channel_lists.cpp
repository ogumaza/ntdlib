// SPDX-License-Identifier: MIT

// The channel managers and their lists of channels: free, playing, releasing and waiting (see channel.cpp).

#include "ntdlib/jac/channel.h"

#include <cstdint>

#include "jac/channel_internal.h"

namespace ntdlib::jac
{

// driverinterface: the managers and the lists.

void Channels::InitGlobalChannels()
{
    // InitGlobalChannel (0x2748e0 in main): InitJcs on the global manager, then each channel in its free list, the
    // last channel first.
    managers_[kGlobal] = ChannelManager{};
    for (uint32_t i = 0; i < kChannels; i++)
    {
        Channel& c = channels_[i];
        c.id = c.id == 0xffff ? 1 : static_cast<uint16_t>(c.id + 1);
        c.wave = nullptr;
        c.source = 0;
        c.mix_configs = kDefaultMixConfigs;
        c.dsp_channel = kNone;
        c.next = i == 0 ? kNone : i - 1;
        c.kind = Channel::kWave;
        c.moved = false;
        c.priority = kDefaultPriority;
        c.release_time = kDefaultReleaseTime;
        c.pan_calcs = kDefaultPanCalcs;
        c.time = 0;
        c.countdown = 0;
        c.pause = false;
        c.callback = Channel::kNoCallback;
        c.sweep = Channel::kNoSweep;
        c.oscillators = {};
        c.manager = kGlobal;
        c.list_manager = kGlobal;
        c.list = ChannelManager::kFree;
    }

    managers_[kGlobal].lists[ChannelManager::kFree] = kChannels - 1;
    managers_[kGlobal].count = kChannels;
}

void Channels::InitManager(uint32_t manager)
{
    // InitJcs (0x274780 in main).
    managers_[manager] = ChannelManager{};
}

void Channels::InitOneShot(uint32_t manager, uint32_t count)
{
    // Init_1shot (0x2783c0 in main).
    if (managers_[manager].count != 0)
    {
        FixReleaseAll(manager);
    }
    InitManager(manager);
    FixAlloc(manager, count);
    managers_[manager].stealing = count != 0;
}

uint32_t Channels::FixAlloc(uint32_t manager, uint32_t count)
{
    // FixAllocChannel (0x274370 in main): up to `count` channels from the global manager's free list.
    ChannelManager& global = managers_[kGlobal];
    uint32_t moved = 0;
    while (moved < count)
    {
        const uint32_t channel = global.lists[ChannelManager::kFree];
        if (channel == kNone)
        {
            break;
        }

        Channel& c = channels_[channel];
        global.lists[ChannelManager::kFree] = c.next;
        ListAdd(manager, ChannelManager::kFree, channel);
        c.manager = manager;
        moved++;
    }

    managers_[manager].count += moved;
    global.count -= moved;

    return moved;
}

void Channels::FixRelease(uint32_t channel)
{
    // FixReleaseChannel (0x2743e0 in main): into the global manager's free list. The game doesn't cut the channel
    // from its list first.
    Channel& c = channels_[channel];
    ListAdd(kGlobal, ChannelManager::kFree, channel);
    managers_[c.manager].count--;
    managers_[kGlobal].count++;
    c.manager = kGlobal;
}

void Channels::FixReleaseAll(uint32_t manager)
{
    // FixReleaseChannelAll (0x274430 in main). The game never ends the move of the global manager onto itself.
    if (manager == kGlobal)
    {
        return;
    }

    FixMove(manager, kGlobal);
    managers_[kGlobal].count += managers_[manager].count;
    managers_[manager].count = 0;
}

void Channels::FixMoveAll(uint32_t from, uint32_t to)
{
    // FixMoveChannelAll (0x2745e0 in main).
    if (from == to)
    {
        return;
    }

    FixMove(from, to);
    managers_[to].count += managers_[from].count;
    managers_[from].count = 0;
    managers_[to].allocations += managers_[from].allocations;
    managers_[from].allocations = 0;
}

void Channels::FixMove(uint32_t from, uint32_t to)
{
    // Each list's channels in turn, each at the head of the same list of `to`. A channel that plays or releases is
    // marked as moved. A waiting channel that holds a slot of the queue loses the slot and goes into the free list.
    ChannelManager& m = managers_[from];
    for (const uint8_t list : {ChannelManager::kFree, ChannelManager::kActive, ChannelManager::kReleasing})
    {
        while (m.lists[list] != kNone)
        {
            const uint32_t channel = m.lists[list];
            Channel& c = channels_[channel];
            m.lists[list] = c.next;
            ListAdd(to, list, channel);
            c.manager = to;
            if (list != ChannelManager::kFree)
            {
                c.moved = true;
            }
        }
    }

    const uint32_t count = waits_.count;
    const uint32_t top = waits_.top;
    while (m.lists[ChannelManager::kWaiting] != kNone)
    {
        const uint32_t channel = m.lists[ChannelManager::kWaiting];
        Channel& c = channels_[channel];
        m.lists[ChannelManager::kWaiting] = c.next;
        uint8_t list = ChannelManager::kWaiting;
        for (uint32_t k = 0; k < count; k++)
        {
            const uint32_t slot = (top + k) & (WaitQueue::kSize - 1);
            if (waits_.channels[slot] == channel)
            {
                waits_.channels[slot] = kNone;
                list = ChannelManager::kFree;
                break;
            }
        }
        ListAdd(to, list, channel);
        c.manager = to;
    }
}

uint32_t Channels::ListCount(uint32_t manager, uint8_t list) const
{
    // List_CountChannel (0x274270 in main).
    uint32_t count = 0;
    for (uint32_t channel = managers_[manager].lists[list]; channel != kNone; channel = channels_[channel].next)
    {
        count++;
    }

    return count;
}

int32_t Channels::ListCut(uint32_t channel)
{
    // List_CutChannel (0x2742a0 in main). The game reads through a null list and crashes. The model gives -1.
    Channel& c = channels_[channel];
    if (c.list_manager == kNone)
    {
        return -1;
    }

    uint32_t& head = managers_[c.list_manager].lists[c.list];
    if (head == channel)
    {
        head = c.next;
        c.list_manager = kNone;
        return 0;
    }

    int32_t place = 0;
    for (uint32_t before = head; before != kNone; before = channels_[before].next)
    {
        place++;
        if (channels_[before].next == channel)
        {
            channels_[before].next = c.next;
            c.list_manager = kNone;
            return place;
        }
    }

    return -1;
}

uint32_t Channels::ListGet(uint32_t manager, uint8_t list)
{
    // List_GetChannel (0x274300 in main).
    uint32_t& head = managers_[manager].lists[list];
    const uint32_t channel = head;
    if (channel != kNone)
    {
        head = channels_[channel].next;
        channels_[channel].list_manager = kNone;
    }

    return channel;
}

void Channels::ListAddTail(uint32_t manager, uint8_t list, uint32_t channel)
{
    // List_AddChannelTail (0x274320 in main).
    Channel& c = channels_[channel];
    c.list_manager = manager;
    c.list = list;
    uint32_t& head = managers_[manager].lists[list];
    if (head == kNone)
    {
        head = channel;
    }
    else
    {
        uint32_t last = head;
        while (channels_[last].next != kNone)
        {
            last = channels_[last].next;
        }
        channels_[last].next = channel;
    }
    c.next = kNone;
}

void Channels::ListAdd(uint32_t manager, uint8_t list, uint32_t channel)
{
    // List_AddChannel (0x274350 in main).
    Channel& c = channels_[channel];
    uint32_t& head = managers_[manager].lists[list];
    c.list_manager = manager;
    c.list = list;
    c.next = head;
    head = channel;
}

// driverinterface: the channels' levels and their DSP channels.

void Channels::InitChannel(uint32_t channel)
{
    // Channel_Init (0x274800 in main). The settings of the mixers, the priority, the release and the pans' types come
    // from the manager.
    Channel& c = channels_[channel];
    c.kind = Channel::kWave;
    c.moved = false;
    c.time = 0;
    c.countdown = 0;
    c.wave = nullptr;
    c.source = 0;
    c.callback = Channel::kNoCallback;
    c.sweep = Channel::kNoSweep;
    if (c.manager != kNone)
    {
        const ChannelManager& m = managers_[c.manager];
        c.mix_configs = m.mix_configs;
        c.priority = m.priority;
        c.release_time = m.release_time;
        c.pan_calcs = m.pan_calcs;
    }
    else
    {
        c.mix_configs = kDefaultMixConfigs;
        c.release_time = kDefaultReleaseTime;
        c.priority = kDefaultPriority;
        c.pan_calcs = kDefaultPanCalcs;
    }
    c.id = c.id == 0xffff ? 1 : static_cast<uint16_t>(c.id + 1);
    c.oscillators = {};
    c.pause = false;
}

} // namespace ntdlib::jac
