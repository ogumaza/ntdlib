// SPDX-License-Identifier: MIT

// The channels that play: their envelopes, their pitch and the sound unit's registers. The addresses in the comments
// are those of the driver's code in Super Princess Peach (USA), on the ARM7.

#include "player_internal.h"

namespace ntdlib::spd
{

// 0x037F90D8: a channel that plays, in a frame.
void Player::Update(Channel& channel)
{
    PlayerState& player = players_[channel.player];
    Track& track = player.tracks[channel.track];
    uint32_t instrument = track.instrument;
    uint32_t settings = Read32(instrument + 8);
    uint32_t set = 0;
    int key = channel.key;
    int pan = 0;
    if ((settings & 7) == kKindKit)
    {
        const uint32_t kit = Bits(settings, 3, 7);
        set = SetAddress(kit >= kSetBSound);
        const int index = channel.key - 36;
        const uint32_t pair = Read16(set + kKitMapsOffset + ((kit % kSetBSound) * 36 + (index >> 1)) * 2);
        const uint32_t entry = (index & 1) != 0 ? pair >> 8 : pair & 0xFF;
        instrument = set + Read32(set + Read32(set + 8) + (kit % kSetBSound) * 4) + entry * 12;
        settings = Read32(instrument + 8);
        const int fixed = SignedBits(settings, 17, 8);
        if (fixed < 0)
        {
            pan = track.pan + player.pan;
        }
        else
        {
            key = static_cast<int16_t>(fixed);
            pan = static_cast<int>(settings >> 25) + player.pan;
        }
    }
    else
    {
        set = SetAddress(track.program >= kSetBSound);
        pan = track.pan + player.pan;
    }
    channel.pan = static_cast<uint8_t>(std::clamp(pan, 0, 127));

    bool start = false;
    if (channel.state == 2)
    {
        channel.state = 1;
        channel.modulation = 5;
        channel.envelope = 5;
        channel.modulation_frames = 0;
        channel.envelope_frames = 0;
    }
    else if (channel.state == 4)
    {
        channel.state = 3;
        start = true;
        channel.modulation = 1;
        channel.envelope = 1;
        channel.modulation_frames = 0;
        channel.envelope_frames = 0;
        channel.sample = Read32(instrument) + set;
        channel.started = true;
    }

    if (channel.frames != 0 && Bits(track.flags, 2, 2) == 0)
    {
        --channel.frames;
        if (channel.frames == 0)
        {
            NoteTicks(channel);
        }
    }

    channel.volume = Envelope(channel, instrument);
    channel.pitch = Pitch(channel, instrument, key);
    WriteChannel(channel, settings, start);
}

// 0x037F9AAC: the envelope, linear in each phase. The release falls from the sustain level.
uint8_t Player::Envelope(Channel& channel, uint32_t instrument)
{
    const uint32_t envelope = Read32(instrument + 4);
    const PlayerState& player = players_[channel.player];
    const Track& track = player.tracks[channel.track];
    const int32_t peak = (channel.velocity * track.volume * player.volume) >> 14;
    const int32_t sustain =
        static_cast<int32_t>(static_cast<uint32_t>(peak * static_cast<int32_t>(Bits(envelope, 19, 6) + 1)) >> 6);
    int32_t value = 0;
    switch (channel.envelope)
    {
    case 0:
        channel.state = 0;
        return 0;

    case 1:
        if (channel.envelope_frames == 0)
        {
            channel.length_a = static_cast<uint8_t>(Bits(envelope, 5, 7));
        }
        if (channel.envelope_frames != channel.length_a)
        {
            const int count = channel.envelope_frames++;
            return static_cast<uint8_t>(Divide(peak * count, channel.length_a));
        }
        ++channel.envelope;
        channel.envelope_frames = 0;
        [[fallthrough]];

    case 2:
        if (channel.envelope_frames == 0)
        {
            channel.length_a = static_cast<uint8_t>(Bits(envelope, 12, 7));
        }
        if (channel.envelope_frames != channel.length_a)
        {
            const int count = channel.envelope_frames++;
            return static_cast<uint8_t>(peak - Divide((peak - sustain) * count, channel.length_a));
        }
        ++channel.envelope;
        channel.envelope_frames = 0;
        return static_cast<uint8_t>(sustain);

    case 3:
        return static_cast<uint8_t>(sustain);

    case 5:
        if (channel.envelope_frames == 0)
        {
            channel.length_b = static_cast<uint8_t>(envelope >> 25);
            channel.length_a = channel.length_b;
        }
        if (channel.envelope_frames == channel.length_a)
        {
            channel.envelope_frames = 0;
            channel.envelope = 0;
            channel.state = 0;
            return 0;
        }
        {
            const int count = channel.envelope_frames++;
            value = sustain - Divide(sustain * count, channel.length_a);
        }
        return static_cast<uint8_t>(value);

    default:
        return static_cast<uint8_t>(value);
    }
}

// 0x037F9C70: the pitch in 1/96 octave from the base timer, with the bend, the player's pitch and the instrument's
// modulation: a sweep, a delay, a triangle vibrato, and a sweep at the release.
int16_t Player::Pitch(Channel& channel, uint32_t instrument, int key)
{
    PlayerState& player = players_[channel.player];
    Track& track = player.tracks[channel.track];
    const uint32_t settings = Read32(instrument + 8);
    const uint32_t envelope = Read32(instrument + 4);
    const int kind = static_cast<int>(settings & 7);
    const uint32_t set =
        kind == kKindKit ? SetAddress(Bits(settings, 3, 7) >= kSetBSound) : SetAddress(track.program >= kSetBSound);

    int32_t pitch = 0;
    if (kind == kKindSample)
    {
        pitch = (key - Read16(channel.sample + 2)) * 8;
    }
    else
    {
        pitch = (key - 64) * 8;
    }
    const int32_t bend =
        static_cast<int32_t>(static_cast<uint32_t>(track.bend * static_cast<int32_t>(envelope & 0x1F)) >> 4);
    pitch += player.pitch * 2 + bend;

    const int index = SignedBits(settings, 10, 7);
    const uint32_t modulation = index < 0 || index > 15 ? 0 : Read32(set + kModulationsOffset + index * 4u);
    switch (channel.modulation)
    {
    case 1:
        if (channel.modulation_frames == 0)
        {
            channel.quadrant = 0;
            channel.length_b = static_cast<uint8_t>(modulation & 0x3F);
            if (pitch_mode_ == 1)
            {
                counter_ = counter_ + 1 > 96 ? 0 : counter_ + 1;
            }
            track.flags &= ~0xF0u;
        }
        if (channel.modulation_frames != channel.length_b)
        {
            const int count = channel.modulation_frames++;
            pitch += Divide(SignedBits(modulation, 6, 5) * (channel.length_b - count), channel.length_b);
            break;
        }
        ++channel.modulation;
        channel.modulation_frames = 0;
        [[fallthrough]];

    case 2:
        if (channel.modulation_frames == 0)
        {
            channel.length_b = static_cast<uint8_t>(Bits(modulation, 11, 6));
        }
        if (channel.modulation_frames != channel.length_b)
        {
            ++channel.modulation_frames;
            break;
        }
        ++channel.modulation;
        channel.modulation_frames = 0;
        [[fallthrough]];

    case 3:
        {
            const uint32_t rate = Bits(modulation, 17, 4);
            if (rate == 0)
            {
                channel.modulation_frames = 0;
                channel.modulation = 0;
                break;
            }
            if (channel.modulation_frames == rate)
            {
                channel.quadrant = static_cast<uint8_t>((channel.quadrant + 1) & 3);
                channel.modulation_frames = 0;
            }
            uint32_t step = 0;
            if ((channel.quadrant & 1) == 0)
            {
                step = channel.modulation_frames++;
            }
            else
            {
                const uint32_t count = channel.modulation_frames++;
                step = rate - count;
            }
            const uint32_t depth = Bits(modulation, 21, 6) * step / rate;
            pitch += (1 - (channel.quadrant & 2)) * static_cast<int32_t>(depth);
            break;
        }

    case 5:
        if (channel.modulation_frames == channel.length_b)
        {
            channel.modulation_frames = 0;
            channel.modulation = 0;
            break;
        }
        {
            const int count = channel.modulation_frames++;
            pitch += Divide(SignedBits(modulation, 27, 5) * count, channel.length_b);
        }
        break;

    default:
        break;
    }

    if (channel.player == 0)
    {
        pitch += pitch_offset_ + static_cast<int32_t>(Bits(track.flags, 4, 4));
    }
    return static_cast<int16_t>(pitch);
}

// 0x037FA1F8: the registers of a channel that plays.
void Player::WriteChannel(const Channel& channel, uint32_t settings, bool start)
{
    int32_t pitch = channel.pitch;
    int shift = 0;
    if (pitch < 0)
    {
        shift = 16;
        while (pitch < 0)
        {
            --shift;
            pitch += 96;
        }
    }
    else
    {
        shift = 15;
        while (pitch >= 0)
        {
            ++shift;
            pitch -= 96;
        }
        pitch += 96;
    }

    const bool sample = (settings & 7) == kKindSample;
    const uint32_t base = sample ? Read16(channel.sample + 4) : kToneTimer;
    const uint32_t registers = kChannelRegisters + channel.hardware * 16u;
    Write(registers + kCnt, 2, channel.volume);
    const int32_t product = static_cast<int32_t>(base * PitchTable()[pitch]);
    const int amount = shift & 0xFF;
    const int32_t scaled = amount >= 32 ? (product < 0 ? -1 : 0) : product >> amount;
    Write(registers + kTmr, 2, static_cast<uint32_t>(65536 - scaled) & 0xFFFC);
    Write(registers + kPanByte, 1, channel.pan);
    if (!start)
    {
        return;
    }

    Write(registers + kStartByte, 1, 0);
    if (!sample)
    {
        Write(registers + kStartByte, 1, Bits(settings, 3, 7) | 0xE0);
        return;
    }
    Write(registers + kSad, 4, channel.sample + 10);
    Write(registers + kLen, 4, Read16(channel.sample + 8));
    Write(registers + kPnt, 2, Read16(channel.sample + 6));
    Write(registers + kStartByte, 1, (Read16(channel.sample) >> 8) | 0x80);
}

// 0x037FA1AC: the registers of a free channel.
void Player::ClearChannel(const Channel& channel)
{
    const uint32_t registers = kChannelRegisters + channel.hardware * 16u;
    Write(registers + kCnt, 4, 0);
    Write(registers + kTmr, 2, 0);
    Write(registers + kSad, 4, 0);
    Write(registers + kLen, 4, 0);
    Write(registers + kPnt, 2, 0);
}

} // namespace ntdlib::spd
