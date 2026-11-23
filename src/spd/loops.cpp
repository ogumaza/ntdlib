// SPDX-License-Identifier: MIT

#include "ntdlib/spd/loops.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ntdlib::spd
{

namespace
{

// The envelope's attack and sustain (Channel::envelope). A channel at a volume of 0 in its attack still rises, and a
// channel in its sustain keeps its volume.
constexpr uint8_t kAttack = 1;
constexpr uint8_t kSustain = 3;

// SOUNDxCNT's top byte: the repeat mode in bits 3-4, the format in bits 5-6.
constexpr uint32_t kChannelRegisters = 0x04000400;
constexpr uint32_t kRegisterBytes = 16;
constexpr uint32_t kControlTop = 3;
constexpr uint8_t kRepeatLoop = 1;
constexpr uint8_t kFormatPsg = 3;

// Whether the top byte of SOUNDxCNT plays a wave without end: a loop, or a square wave or noise.
bool PlaysOn(uint8_t top)
{
    return ((top >> 3) & 3) == kRepeatLoop || ((top >> 5) & 3) == kFormatPsg;
}

// Calls `f` with the player and the track of each track that reads events.
template <typename F>
void EachReadingTrack(const Player& player, F f)
{
    for (int p = 0; p < kPlayers; p++)
    {
        const PlayerState& state = player.Players()[static_cast<std::size_t>(p)];
        if ((state.state & 0xF) == 0)
        {
            continue;
        }

        for (int t = 0; t < state.max_tracks; t++)
        {
            const Track& track = state.tracks[static_cast<std::size_t>(t)];
            if (track.position != 0 && ((track.flags >> 2) & 3) == 0)
            {
                f(p, t);
            }
        }
    }
}

} // namespace

void LoopWatch::Note(const Player& player, uint32_t frame)
{
    for (const LoopMark& mark : player.LoopMarks())
    {
        TrackLoop& loop = loops_[{mark.player, mark.track}];
        if (mark.jump)
        {
            loop.jumps.push_back(frame);
        }
        else if (!loop.start)
        {
            loop.start = frame;
        }
    }
}

bool LoopWatch::Reading(const Player& player)
{
    bool reading = false;
    EachReadingTrack(player, [&](int, int) { reading = true; });

    return reading;
}

bool LoopWatch::Fading(const Player& player)
{
    const auto fading = [](const PlayerState& state)
    {
        return (state.state & 0xF) != 0 && (state.fade != 0 || state.level != 0);
    };
    return std::any_of(player.Players().begin(), player.Players().end(), fading);
}

bool LoopWatch::Looped(const Player& player, uint32_t passes) const
{
    bool reading = false;
    bool looped = true;
    EachReadingTrack(player,
                     [&](int p, int t)
                     {
                         reading = true;
                         const auto loop = loops_.find({p, t});
                         looped = looped && loop != loops_.end() && loop->second.jumps.size() >= passes;
                     });

    return reading && looped;
}

std::optional<TrackLoop> LoopWatch::Longest() const
{
    std::optional<TrackLoop> longest;
    for (const auto& [track, loop] : loops_)
    {
        if (!loop.start || loop.jumps.empty() || loop.jumps[0] <= *loop.start)
        {
            continue;
        }

        if (!longest || loop.jumps[0] - *loop.start > longest->jumps[0] - *longest->start)
        {
            longest = loop;
        }
    }

    return longest;
}

SoundLength MeasureSound(Player& player, int slot, int sound, uint32_t max_frames)
{
    SoundLength length;
    LoopWatch loops;
    uint32_t last_sound = 0;
    std::array<bool, kChannels> plays_on = {};
    player.Post(StartMessage(slot, sound));
    for (uint32_t frame = 0; frame < max_frames; frame++)
    {
        for (const RegisterWrite& write : player.Frame())
        {
            const uint32_t offset = write.address - kChannelRegisters;
            if (write.address >= kChannelRegisters && offset < kChannels * kRegisterBytes &&
                offset % kRegisterBytes + write.size > kControlTop && offset % kRegisterBytes <= kControlTop)
            {
                const uint32_t shift = 8 * (kControlTop - offset % kRegisterBytes);
                plays_on[offset / kRegisterBytes] = PlaysOn(static_cast<uint8_t>(write.value >> shift));
            }
        }
        loops.Note(player, frame);

        // The channels of notes. The capture reverb's channels play only what the others played. A channel may sound
        // again while its volume is above 0 or rises. The sound is steady when no channel changes its volume: each one
        // holds its sustain or stays at 0 outside its attack.
        bool may_sound = false;
        bool steady = true;
        bool held = false;
        for (const Channel& channel : player.Channels())
        {
            if (channel.state == 0)
            {
                continue;
            }

            if (channel.volume > 0)
            {
                length.audible = true;
                last_sound = frame;
                held = held || plays_on[channel.hardware];
            }
            may_sound = may_sound || channel.volume > 0 || channel.envelope == kAttack;
            steady = steady && (channel.envelope == kSustain || (channel.volume == 0 && channel.envelope != kAttack));
        }

        // With no track that reads events and no fade, a steady sound stays as it is: it holds a wave that plays on,
        // or it ends once its waves end.
        const bool still = !LoopWatch::Reading(player) && !LoopWatch::Fading(player);
        if (still && steady && held)
        {
            length.held = frame;
            return length;
        }

        if (!player.Busy() || (still && (!may_sound || steady)))
        {
            length.end = length.audible ? last_sound + 1 : frame + 1;
            return length;
        }

        if (loops.Looped(player, 1))
        {
            const std::optional<TrackLoop> loop = loops.Longest();
            if (loop)
            {
                length.loop_start = *loop->start;
                length.loop_frames = loop->jumps[0] - *loop->start;
            }
            return length;
        }
    }

    return length;
}

} // namespace ntdlib::spd
