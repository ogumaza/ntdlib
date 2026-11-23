// SPDX-License-Identifier: MIT

// The players' fades and tracks: the frames of waits and notes, the events and the start of notes, and the allocation
// of channels. The addresses in the comments are those of the driver's code in Super Princess Peach (USA), on the ARM7.

#include "player_internal.h"

namespace ntdlib::spd
{

// 0x037FA344: the fades.
void Player::Fade(int p)
{
    PlayerState& player = players_[p];
    if (player.level == 1)
    {
        if (player.volume == 0)
        {
            return;
        }

        uint8_t volume = 0;
        if (player.level_length != 0)
        {
            const int count = player.level_count++;
            volume = static_cast<uint8_t>(Divide((player.level_length - count) * player.saved, player.level_length));
        }
        if (volume == 0)
        {
            FreeChannels(p);
            player.state = static_cast<uint8_t>((player.state & ~0xF) | 2);
        }
        player.volume = volume;
        return;
    }

    if (player.level == 2)
    {
        if (player.level_length == 0)
        {
            player.volume = player.saved;
        }
        else
        {
            const int count = player.level_count++;
            player.volume = static_cast<uint8_t>(Divide(player.saved * count, player.level_length));
        }
        if (player.volume < player.saved)
        {
            return;
        }
        if (player.next_length == 0)
        {
            player.volume = player.saved;
            player.level = 0;
            return;
        }
        player.volume = player.saved;
        player.saved = player.target;
        player.level_length = player.next_length;
        player.level = 3;
        player.next_length = 0;
        return;
    }

    if (player.level == 3)
    {
        if (player.fade != 0)
        {
            player.level = 0;
        }
        else
        {
            player.volume = static_cast<uint8_t>((player.target * player.level_length) >> 7);
        }
        return;
    }

    if (player.fade == 1)
    {
        uint8_t volume = 0;
        if (player.fade_length != 0)
        {
            const int count = player.fade_count++;
            volume = static_cast<uint8_t>(Divide((player.fade_length - count) * player.target, player.fade_length));
        }
        if (volume == 0)
        {
            player.fade = 0;
            for (Track& track : player.tracks)
            {
                track.position = 0;
            }
            FreeChannels(p);
            player.state = static_cast<uint8_t>(player.state & ~0xF);
        }
        player.volume = volume;
    }
    else if (player.fade == 2)
    {
        if (player.fade_length == 0)
        {
            player.volume = player.target;
        }
        else
        {
            const int count = player.fade_count++;
            player.volume = static_cast<uint8_t>(Divide(player.target * count, player.fade_length));
        }
        if (player.volume == player.target)
        {
            player.fade = 0;
        }
    }
}

void Player::FreeChannels(int p)
{
    for (Channel& channel : channels_)
    {
        if (channel.player == p)
        {
            channel.state = 0;
        }
    }
}

// The tempo after the player's tempo offset, in 1/96 octave (0x037FA0E4).
uint32_t Player::ScaledTempo(uint16_t tempo, const PlayerState& player) const
{
    const int32_t shifted = player.tempo + 96;
    const int32_t octaves = Divide(shifted, 96);
    const int32_t rest = Remainder(shifted, 96);
    const int index = 96 - rest;
    const uint32_t factor = index >= 0 && index <= 96 ? PitchTable()[index] : 0;
    const int32_t product = static_cast<int32_t>(static_cast<uint32_t>(tempo) * factor);
    const int shift = 16 - octaves;
    return static_cast<uint32_t>(shift >= 32 ? (product < 0 ? -1 : 0) : product >> shift);
}

// 0x037FA0E4: the frames of the next ticks of a wait.
uint16_t Player::WaitFrames(Track& track, const PlayerState& player)
{
    const int32_t tempo = static_cast<int32_t>(ScaledTempo(track.tempo, player));
    int32_t frames = Divide(160, tempo);
    const int32_t rest = 160 - frames * tempo;
    do
    {
        track.carry = static_cast<uint16_t>(track.carry + rest);
        if (track.carry >= tempo)
        {
            ++frames;
            track.carry = static_cast<uint16_t>(track.carry - tempo);
        }
        --track.wait;
    } while (track.wait != 0 && frames == 0);
    track.frames = static_cast<uint16_t>(frames & 0xFF);
    return static_cast<uint16_t>(frames);
}

// 0x037F9FFC: the frames of a note's next ticks.
void Player::NoteTicks(Channel& channel)
{
    const Track& track = players_[channel.player].tracks[channel.track];
    const int32_t tempo = static_cast<int32_t>(ScaledTempo(track.tempo, players_[channel.player]));
    if (channel.length == 0)
    {
        if (channel.envelope != 0 && channel.envelope != 5)
        {
            channel.state = 2;
        }
        return;
    }

    int32_t carry = track.carry;
    int32_t frames = Divide(160, tempo);
    const int32_t rest = 160 - frames * tempo;
    do
    {
        carry += rest;
        if (carry >= tempo)
        {
            ++frames;
            carry -= tempo;
        }
        --channel.length;
    } while (channel.length != 0 && frames == 0);
    if (frames == 0)
    {
        channel.state = 2;
    }
    channel.frames = static_cast<uint8_t>(frames);
}

// 0x037F9328: a player's tracks in a frame.
void Player::Step(int p)
{
    PlayerState& player = players_[p];
    if (player.delay != 0)
    {
        --player.delay;
        return;
    }

    int active = 0;
    for (int t = 0; t < player.max_tracks; ++t)
    {
        if (player.tracks[t].position == 0)
        {
            continue;
        }
        if (!PlayEvents(p, t, active++))
        {
            break;
        }
    }

    if (active == 0)
    {
        player.state = static_cast<uint8_t>(player.state & ~0xF);
        const uint32_t block = SetAddress(player.set_b);
        const uint32_t table = block + Read32(block + 0xC);
        RunAction(p, Read32(table + static_cast<uint32_t>(player.sound - 1) * 8 + 4), true);
    }
}

// A track's events in a frame. Returns false when the track's player no longer plays. The driver can't see that, but a
// message from an action can't cause it either.
bool Player::PlayEvents(int p, int t, int before)
{
    PlayerState& player = players_[p];
    Track& track = player.tracks[t];
    const auto hold = [&track]
    {
        return Bits(track.flags, 2, 2) != 0;
    };

    if (track.frames != 0 || (track.wait != 0 && WaitFrames(track, player) != 0))
    {
        if (!hold())
        {
            --track.frames;
        }
        return true;
    }

    uint32_t position = track.position;
    uint32_t instrument = track.instrument;
    for (int guard = 0; guard < 0x100000; ++guard)
    {
        const uint16_t event = Read16(position);
        const uint8_t code = static_cast<uint8_t>(event);
        const uint8_t argument = static_cast<uint8_t>(event >> 8);
        if (code <= 0x7F)
        {
            StartNotes(p, t, before, position, instrument);
        }
        else if (code >= 0xF0)
        {
            position += 2;
            const uint32_t set = SetAddress(track.program >= kSetBSound);
            switch (code)
            {
            case 0xF0:
                track.tempo = argument;
                break;
            case 0xF1:
                {
                    for (Channel& channel : channels_)
                    {
                        if (channel.player == p && channel.track == t)
                        {
                            channel.state = 0;
                        }
                    }
                    const uint32_t program = track.program % kSetBSound;
                    const uint8_t entry = Read8(set + kProgramMapsOffset + program * kProgramMapSize + argument);
                    if (entry == 0xFF)
                    {
                        track.instrument = 0;
                    }
                    else
                    {
                        track.instrument = set + Read32(set + Read32(set + 4) + program * 4) + entry * 12u;
                    }
                    instrument = track.instrument;
                    break;
                }
            case 0xF2:
                track.program = argument;
                break;
            case 0xF3:
                track.pan = argument;
                break;
            case 0xF4:
                track.flags = (track.flags & ~0xCu) | 4;
                break;
            case 0xF5:
                track.bend = static_cast<int8_t>(argument);
                break;
            case 0xF6:
                track.volume = argument;
                break;
            case 0xF8:
                track.loop = position;
                loop_marks_.push_back({p, t, false});
                break;
            case 0xF9:
                position = track.loop;
                loop_marks_.push_back({p, t, true});
                break;
            case 0xFA:
                track.flags = (track.flags & ~3u) | 1;
                break;
            case 0xFB:
                track.flags &= ~3u;
                break;
            case 0xFC:
            case 0xFD:
                if (p == 0 && (branch_flag_ != 0) == (code == 0xFC))
                {
                    const uint32_t block = SetAddress(player.set_b);
                    const uint32_t table = block + Read32(block + 0xC);
                    const uint32_t header = block + Read32(table + (argument - 1u) * 8);
                    position = Read32(header + 4 + static_cast<uint32_t>(t) * 4) + block;
                }
                break;
            case 0xFF:
                track.position = 0;
                return true;
            default:
                break;
            }
            if (position == 0)
            {
                // F9 without F8: the driver reads from address 0, where it finds no events of its own.
                track.position = 0;
                return true;
            }
            continue;
        }

        // A wait: the event after a run of notes too, whatever its code (0x037F99C4).
        const uint16_t wait = Read16(position);
        position += 2;
        if ((wait & 0xFF) == 0)
        {
            continue;
        }
        track.wait = static_cast<uint8_t>(wait >> 8);
        track.frames = 0;
        if (WaitFrames(track, player) == 0)
        {
            continue;
        }
        track.position = position;
        if (!hold())
        {
            --track.frames;
        }
        return true;
    }
    throw std::runtime_error("a track runs a million events in a frame");
}

// A run of notes (0x037F9724). Each note takes a channel by the kind of its instrument. A run that finds no
// instrument or no channel for a note skips its other notes. The priority counts the tracks that the frame visited
// before this one while they played (0x037F93D0), then the track's priority and the sound's. Each note of a square
// wave or noise adds 0x8000 to it for the rest of the run, as the driver's register does.
void Player::StartNotes(int p, int t, int before, uint32_t& position, uint32_t& instrument)
{
    PlayerState& player = players_[p];
    Track& track = player.tracks[t];
    uint32_t priority =
        static_cast<uint32_t>(16 - before) + ((track.flags & 3) << 10) + (static_cast<uint32_t>(player.priority) << 5);

    while ((Read16(position) & 0xFF) <= 0x7F)
    {
        const uint16_t event = Read16(position);
        const uint8_t key = static_cast<uint8_t>(event);
        const uint8_t velocity = static_cast<uint8_t>(event >> 8);
        if (instrument == 0)
        {
            break;
        }

        uint32_t settings = Read32(track.instrument + 8);
        int kind = static_cast<int>(settings & 7);
        if (kind == kKindKit)
        {
            const int index = key - 36;
            if (index < 0 || index > 71)
            {
                break;
            }
            const uint32_t kit = Bits(settings, 3, 7);
            const uint32_t set = SetAddress(kit >= kSetBSound);
            const uint8_t entry = Read8(set + kKitMapsOffset + (kit % kSetBSound) * kKitKeys + index);
            if (entry == 0xFF)
            {
                break;
            }
            instrument = set + Read32(set + Read32(set + 8) + (kit % kSetBSound) * 4) + entry * 12u;
            settings = Read32(instrument + 8);
            kind = static_cast<int>(settings & 7);
        }

        if (kind == kKindSquare || kind == kKindNoise)
        {
            priority += kLowPriority;
        }
        const int found = Allocate(kind, priority);
        if (found < 0)
        {
            break;
        }

        Channel& channel = channels_[found];
        channel.player = p;
        channel.track = t;
        channel.priority = static_cast<uint16_t>(priority);
        channel.state = 4;
        channel.key = static_cast<int8_t>(key);
        channel.velocity = velocity;
        channel.length = Read16(position + 2);
        channel.frames = 0;
        NoteTicks(channel);
        position += 4;
    }

    // The rest of the run, when a note broke it off.
    while ((Read16(position) & 0xFF) <= 0x7F)
    {
        position += 4;
    }
}

// The channel for a note (0x037F9844): a free one, else one that releases, else the one with the lowest priority
// when the note's is at least as high. A sample that finds none tries the squares' and the noise's channels.
int Player::Allocate(int kind, uint32_t priority)
{
    std::size_t first = 0;
    std::size_t end = 0;
    if (kind == kKindSquare)
    {
        first = 0;
        end = squares_end_;
    }
    else if (kind == kKindNoise)
    {
        first = squares_end_;
        end = noise_end_;
    }
    else if (kind == kKindSample)
    {
        first = noise_end_;
        end = active_end_;
    }
    else
    {
        throw std::runtime_error("a note plays an instrument of kind " + std::to_string(kind));
    }

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        for (std::size_t i = first; i < end; ++i)
        {
            if (channels_[i].state == 0)
            {
                return static_cast<int>(i);
            }
        }
        for (std::size_t i = first; i < end; ++i)
        {
            if (channels_[i].state < 3)
            {
                return static_cast<int>(i);
            }
        }

        std::size_t lowest = first;
        uint32_t lowest_priority = 0xFFFF;
        for (std::size_t i = first; i < end; ++i)
        {
            if (channels_[i].priority < lowest_priority)
            {
                lowest = i;
                lowest_priority = channels_[i].priority;
            }
        }
        if (first != end && static_cast<int32_t>(priority) >= channels_[lowest].priority)
        {
            return static_cast<int>(lowest);
        }
        if (kind != kKindSample || attempt == 1)
        {
            return -1;
        }
        first = 0;
        end = noise_end_;
    }
    return -1;
}

} // namespace ntdlib::spd
