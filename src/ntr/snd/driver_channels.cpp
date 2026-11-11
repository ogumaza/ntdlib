// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "driver_shared.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr::snd
{

namespace
{

constexpr uint32_t kCaptureAddress = 0x04000510;

constexpr uint32_t kCaptureLength = 0x04000514;

// A channel's part: 0 plays waves alone, 1 plays a PSG wave too, and 2 noise too.
int PartOf(int twin)
{
    return twin >= 14 ? 2 : twin >= 8 ? 1 : 0;
}

// The channels whose volume the surround decay lowers: all but channels 1 and 3. Those two play the captures.
constexpr uint32_t kSurroundChannels = 0xFFF5;

// The envelope's attenuation at a note's start, and the level at which a release ends the channel.
constexpr int32_t kStartAttenuation = -92544;

constexpr int32_t kSilent = -723;

enum Sync : uint8_t
{
    kSyncStart = 0x1,
    kSyncStop = 0x2,
    kSyncTimer = 0x4,
    kSyncVolume = 0x8,
    kSyncPan = 0x10,
};

constexpr int kKeySplits = 8;

// The places in a bank and a wave archive.
constexpr uint32_t kBankWaveArchives = 0x18;

constexpr uint32_t kBankCount = 0x38;

constexpr uint32_t kBankOffsets = 0x3C;

constexpr uint32_t kWaveArchiveCount = 0x38;

constexpr uint32_t kWaveArchiveOffsets = 0x3C;

// The LFO's sine over a cycle of 128 steps, from the table's quarter.
int8_t Sine(int x)
{
    if (x < 0x20)
    {
        return tables::kLfoSine[x];
    }
    if (x < 0x40)
    {
        return tables::kLfoSine[0x40 - x];
    }
    if (x < 0x60)
    {
        return static_cast<int8_t>(-tables::kLfoSine[x - 0x40]);
    }

    return static_cast<int8_t>(-tables::kLfoSine[0x20 - (x - 0x60)]);
}

int32_t LfoValue(const Lfo& lfo)
{
    if (lfo.parameters.depth == 0 || lfo.delay_counter < lfo.parameters.delay)
    {
        return 0;
    }

    return Sine(lfo.counter >> 8) * lfo.parameters.depth * lfo.parameters.range;
}

void StepLfoCounter(Lfo& lfo)
{
    if (lfo.delay_counter < lfo.parameters.delay)
    {
        lfo.delay_counter++;
        return;
    }

    // The counter's low byte holds the fraction of a step, and the high byte the step of the cycle of 128.
    uint32_t step = (lfo.counter + (static_cast<uint32_t>(lfo.parameters.speed) << 6)) >> 8;
    while (step >= 0x80)
    {
        step -= 0x80;
    }
    lfo.counter = static_cast<uint16_t>(lfo.counter + (lfo.parameters.speed << 6));
    lfo.counter &= 0xFF;
    lfo.counter = static_cast<uint16_t>(lfo.counter | step << 8);
}

uint16_t DecayRate(int value)
{
    if (value == 127)
    {
        return 0xFFFF;
    }
    if (value == 126)
    {
        return 0x3C00;
    }
    if (value < 50)
    {
        return static_cast<uint16_t>(value * 2 + 1);
    }

    return static_cast<uint16_t>(0x1E00 / (126 - value));
}

// SOUNDxTMR's period for a wave's period `timer` moved by `pitch`, in 1/64 of a semitone.
uint16_t Timer(int timer, int pitch)
{
    int octave = 0;
    int fraction = -pitch;
    while (fraction < 0)
    {
        octave--;
        fraction += 768;
    }
    while (fraction >= 768)
    {
        octave++;
        fraction -= 768;
    }

    uint64_t result = (tables::kPitch[fraction] + 0x10000ull) * static_cast<uint32_t>(timer);
    int shift = octave - 16;
    if (shift <= 0)
    {
        // For a shift of 64 or more bits, the game's code returns 0.
        result = -shift >= 64 ? 0 : result >> -shift;
    }
    else if (shift < 32)
    {
        if ((result & (~0ull << (32 - shift))) != 0)
        {
            return 0xFFFF;
        }
        result <<= shift;
    }
    else
    {
        return 0xFFFF;
    }

    if (result < 0x10)
    {
        return 0x10;
    }
    if (result > 0xFFFF)
    {
        return 0xFFFF;
    }

    return static_cast<uint16_t>(result);
}

// SOUNDxCNT's volume and divider for a level in 0.1 dB.
uint16_t ChannelVolume(int level)
{
    level = std::clamp(level, kSilent, 0);
    const int volume = tables::kVolume[level - kSilent];
    const int divider = level < -240 ? 3 : level < -120 ? 2 : level < -60 ? 1 : 0;
    return static_cast<uint16_t>(volume | divider << 8);
}

} // namespace

int16_t Driver::DecibelSquare(int index) const
{
    const std::size_t at = static_cast<std::size_t>(index) * 2;
    if (index < 0 || at + 1 >= tables_.size())
    {
        return 0;
    }

    return static_cast<int16_t>(xsflib::LoadLe16(&tables_[at]));
}

uint8_t Driver::FastAttack(int index) const
{
    const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(fast_attack_) + index;
    if (at < 0 || static_cast<std::size_t>(at) >= tables_.size())
    {
        return 0;
    }

    return tables_[static_cast<std::size_t>(at)];
}

void Driver::SetUpPcmChannel(int channel, uint32_t data, int format, int loop, int loop_start, int loop_length,
                             int volume, int divider, int timer, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 4,
                      static_cast<uint32_t>(format) << 29 | static_cast<uint32_t>(loop) << 27 |
                          static_cast<uint32_t>(pan) << 16 | static_cast<uint32_t>(divider) << 8 |
                          static_cast<uint32_t>(volume));
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
    registers_->Write(ChannelRegister(channel, 0xA), 2, static_cast<uint16_t>(loop_start));
    registers_->Write(ChannelRegister(channel, 0xC), 4, static_cast<uint32_t>(loop_length));
    registers_->Write(ChannelRegister(channel, 0x4), 4, data);
}

void Driver::SetUpPsgChannel(int channel, int duty, int volume, int divider, int timer, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 4,
                      0x60000000u | static_cast<uint32_t>(duty) << 24 | static_cast<uint32_t>(pan) << 16 |
                          static_cast<uint32_t>(divider) << 8 | static_cast<uint32_t>(volume));
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
}

void Driver::SetUpNoiseChannel(int channel, int volume, int divider, int timer, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 4,
                      0x60000000u | static_cast<uint32_t>(pan) << 16 | static_cast<uint32_t>(divider) << 8 |
                          static_cast<uint32_t>(volume));
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
}

void Driver::StopChannel(int channel, int hold)
{
    uint32_t control = registers_->Read(ChannelRegister(channel, 0x0), 4);
    control &= ~0x80000000u;
    if (hold & 1)
    {
        control |= 0x8000;
    }
    registers_->Write(ChannelRegister(channel, 0x0), 4, control);
}

void Driver::SetChannelVolume(int channel, int volume, int divider)
{
    original_volumes_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(volume);
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        const int pan = static_cast<int>(registers_->Read(ChannelRegister(channel, 0x2), 1));
        volume = SurroundDecay(volume, pan);
    }

    registers_->Write(ChannelRegister(channel, 0x0), 2, static_cast<uint16_t>(divider << 8 | volume));
}

void Driver::SetChannelTimer(int channel, int timer)
{
    registers_->Write(ChannelRegister(channel, 0x8), 2, static_cast<uint16_t>(0x10000 - timer));
}

void Driver::SetChannelPan(int channel, int pan)
{
    original_pans_[static_cast<std::size_t>(channel)] = static_cast<uint8_t>(pan);
    if (master_pan_ >= 0)
    {
        pan = master_pan_;
    }
    registers_->Write(ChannelRegister(channel, 0x2), 1, static_cast<uint8_t>(pan));
    if (surround_decay_ > 0 && Surrounded(channel))
    {
        registers_->Write(
            ChannelRegister(channel, 0x0), 1,
            static_cast<uint8_t>(SurroundDecay(original_volumes_[static_cast<std::size_t>(channel)], pan)));
    }
}

bool Driver::IsChannelActive(int channel)
{
    return (registers_->Read(ChannelRegister(channel, 0x3), 1) & 0x80) != 0;
}

bool Driver::Surrounded(int channel) const
{
    return ((1u << channels_[static_cast<std::size_t>(channel)].twin) & kSurroundChannels) != 0;
}

void Driver::SetMasterPan(int pan)
{
    master_pan_ = pan;
    for (int i = 0; i < ChannelCount(); i++)
    {
        registers_->Write(ChannelRegister(i, 0x2), 1,
                          pan >= 0 ? static_cast<uint8_t>(pan) : original_pans_[static_cast<std::size_t>(i)]);
    }
}

void Driver::SetSurroundDecay(int decay)
{
    surround_decay_ = decay;
    for (int i = 0; i < ChannelCount(); i++)
    {
        if (Surrounded(i))
        {
            const int pan = static_cast<int>(registers_->Read(ChannelRegister(i, 0x2), 1));
            registers_->Write(ChannelRegister(i, 0x0), 1,
                              static_cast<uint8_t>(SurroundDecay(original_volumes_[static_cast<std::size_t>(i)], pan)));
        }
    }
}

int Driver::SurroundDecay(int volume, int pan) const
{
    if (pan < 24)
    {
        return volume * (surround_decay_ * (pan + 40) + ((0x7FFF - surround_decay_) << 6)) >> 21;
    }
    if (pan <= 104)
    {
        return volume;
    }

    return volume * (-surround_decay_ * (pan - 40) + ((surround_decay_ + 0x7FFF) << 6)) >> 21;
}

void Driver::SetOutputSelector(int left, int right, int channel1, int channel3)
{
    const int enable = (registers_->Read(kMixRegister, 1) & 0x80) ? 1 : 0;
    registers_->Write(kMixRegister, 1,
                      static_cast<uint8_t>(enable << 7 | channel3 << 5 | channel1 << 4 | right << 2 | left));
}

void Driver::SetUpCapture(int capture, int format, uint32_t address, int size, bool loop, int source, int add)
{
    const uint32_t offset = static_cast<uint32_t>(capture) * 8;
    registers_->Write(kCaptureControl + static_cast<uint32_t>(capture), 1,
                      static_cast<uint8_t>(format << 3 | (loop ? 0 : 1) << 2 | source << 1 | add));
    registers_->Write(kCaptureAddress + offset, 4, address);
    registers_->Write(kCaptureLength + offset, 2, static_cast<uint16_t>(size));
}

bool Driver::IsCaptureActive(int capture)
{
    return (registers_->Read(kCaptureControl + static_cast<uint32_t>(capture), 1) & 0x80) != 0;
}

void Driver::UpdateChannels()
{
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.sync == 0)
        {
            continue;
        }

        if (channel.sync & kSyncStop)
        {
            StopChannel(i, 0);
        }

        if (channel.sync & kSyncStart)
        {
            switch (channel.type)
            {
            case kPcm:
                SetUpPcmChannel(i, channel.data, channel.wave.format, channel.wave.loop ? 1 : 2,
                                channel.wave.loop_start, static_cast<int>(channel.wave.loop_length),
                                channel.volume & 0xFF, channel.volume >> 8, channel.timer, channel.pan);
                break;
            case kPsg:
                SetUpPsgChannel(i, static_cast<int>(channel.data), channel.volume & 0xFF, channel.volume >> 8,
                                channel.timer, channel.pan);
                break;
            case kNoise:
                SetUpNoiseChannel(i, channel.volume & 0xFF, channel.volume >> 8, channel.timer, channel.pan);
                break;
            default:
                break;
            }
        }
        else
        {
            if (channel.sync & kSyncTimer)
            {
                SetChannelTimer(i, channel.timer);
            }
            if (channel.sync & kSyncVolume)
            {
                SetChannelVolume(i, channel.volume & 0xFF, channel.volume >> 8);
            }
            if (channel.sync & kSyncPan)
            {
                SetChannelPan(i, channel.pan);
            }
        }
    }

    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.sync == 0)
        {
            continue;
        }

        if (channel.sync & kSyncStart)
        {
            registers_->Write(ChannelRegister(i, 0x3), 1, registers_->Read(ChannelRegister(i, 0x3), 1) | 0x80);
        }
        channel.sync = 0;
    }
}

void Driver::StepChannels(bool step)
{
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (!channel.active)
        {
            continue;
        }

        if (channel.start)
        {
            channel.sync |= kSyncStart;
            channel.start = false;
        }
        else if (!IsChannelActive(i))
        {
            // The sound unit has stopped the channel: a one-shot wave has ended.
            if (channel.owner >= 0)
            {
                Callback(channel, 1);
            }
            else
            {
                channel.priority = 0;
            }
            channel.volume = 0;
            channel.active = false;
            continue;
        }

        int32_t volume = DecibelSquare(channel.velocity);
        int32_t pitch = (channel.key - channel.root_key) * 0x40;
        int32_t pan = 0;
        volume += StepEnvelope(channel, step);
        pitch += StepSweep(channel, step);
        volume += channel.user_decay;
        volume += channel.user_decay2;
        pitch += channel.user_pitch;

        const int32_t lfo = StepLfo(channel, step);
        switch (channel.lfo.parameters.target)
        {
        case kLfoVolume:
            if (volume > -0x8000)
            {
                volume += lfo;
            }
            break;
        case kLfoPan:
            pan += lfo;
            break;
        case kLfoPitch:
            pitch += lfo;
            break;
        default:
            break;
        }

        pan += channel.initial_pan;
        if (channel.pan_range != 127)
        {
            pan = (pan * channel.pan_range + 0x40) >> 7;
        }
        pan += channel.user_pan;

        if (channel.envelope_stage == kRelease && volume <= kSilent)
        {
            channel.sync = kSyncStop;
            if (channel.owner >= 0)
            {
                Callback(channel, 1);
            }
            else
            {
                channel.priority = 0;
            }
            channel.volume = 0;
            channel.active = false;
            continue;
        }

        const uint16_t new_volume = ChannelVolume(volume);
        uint16_t timer = Timer(channel.wave.timer, pitch);
        if (channel.type == kPsg)
        {
            timer &= 0xFFFC;
        }
        pan = std::clamp(pan + 0x40, 0, 127);

        if (new_volume != channel.volume)
        {
            channel.volume = new_volume;
            channel.sync |= kSyncVolume;
        }
        if (timer != channel.timer)
        {
            channel.timer = timer;
            channel.sync |= kSyncTimer;
        }
        if (pan != channel.pan)
        {
            channel.pan = static_cast<uint8_t>(pan);
            channel.sync |= kSyncPan;
        }
    }
}

int32_t Driver::StepEnvelope(Channel& channel, bool step)
{
    if (step)
    {
        switch (channel.envelope_stage)
        {
        case kAttack:
            channel.attenuation = -((-channel.attenuation * channel.attack) >> 8);
            if (channel.attenuation == 0)
            {
                channel.envelope_stage = kDecay;
            }
            break;
        case kDecay:
            {
                const int32_t sustain = DecibelSquare(channel.sustain) * 128;
                channel.attenuation -= channel.decay;
                if (channel.attenuation <= sustain)
                {
                    channel.attenuation = sustain;
                    channel.envelope_stage = kSustain;
                }
                break;
            }
        case kRelease:
            channel.attenuation -= channel.release;
            break;
        default:
            break;
        }
    }

    return channel.attenuation >> 7;
}

int32_t Driver::StepSweep(Channel& channel, bool step)
{
    if (channel.sweep_pitch == 0 || channel.sweep_counter >= channel.sweep_length)
    {
        return 0;
    }

    const int64_t result = static_cast<int64_t>(channel.sweep_pitch) * (channel.sweep_length - channel.sweep_counter) /
                           channel.sweep_length;
    if (step && channel.auto_sweep)
    {
        channel.sweep_counter++;
    }

    return static_cast<int32_t>(result);
}

int32_t Driver::StepLfo(Channel& channel, bool step)
{
    int64_t result = LfoValue(channel.lfo);
    if (result != 0)
    {
        switch (channel.lfo.parameters.target)
        {
        case kLfoVolume:
            result *= 60;
            break;
        case kLfoPitch:
        case kLfoPan:
            result *= 64;
            break;
        default:
            break;
        }
        result >>= 14;
    }

    if (step)
    {
        StepLfoCounter(channel.lfo);
    }

    return static_cast<int32_t>(result);
}

void Driver::SetAttack(Channel& channel, int attack)
{
    channel.attack = attack < 109 ? static_cast<uint8_t>(255 - attack) : FastAttack(127 - attack);
}

void Driver::SetDecay(Channel& channel, int decay)
{
    channel.decay = DecayRate(decay);
}

void Driver::SetRelease(Channel& channel, int release)
{
    channel.release = DecayRate(release);
}

void Driver::StartChannel(Channel& channel, int32_t length)
{
    channel.attenuation = kStartAttenuation;
    channel.envelope_stage = kAttack;
    channel.length = length;
    channel.lfo.counter = 0;
    channel.lfo.delay_counter = 0;
    channel.start = true;
    channel.active = true;
}

int Driver::Allocate(uint32_t mask, int priority, bool weak_locks_too, int owner)
{
    mask &= ~locked_;
    if (!weak_locks_too)
    {
        mask &= ~weakly_locked_;
    }

    // The channel of the lowest priority, and of those the quietest, in the driver's order.
    const auto loudness = [this](const Channel& channel)
    {
        const std::size_t shift = shifts_ + static_cast<std::size_t>(channel.volume >> 8);
        return ((channel.volume & 0xFF) << 4) >> (shift < tables_.size() ? tables_[shift] : 0);
    };
    int chosen = -1;
    for (const uint8_t candidate : tables::kChannelOrder)
    {
        if (!(mask & (1u << candidate)))
        {
            continue;
        }
        const Channel& channel = channels_[candidate];
        if (chosen < 0)
        {
            chosen = candidate;
            continue;
        }
        const Channel& best = channels_[static_cast<std::size_t>(chosen)];
        if (channel.priority > best.priority)
        {
            continue;
        }
        if (channel.priority != best.priority || loudness(best) > loudness(channel))
        {
            chosen = candidate;
        }
    }

    if (chosen < 0)
    {
        return -1;
    }
    // A driver that grows its channels gives the note another channel in place of a channel whose note sounds before
    // its release, or where the note's priority takes no channel. A note in its release gives way, as on the console.
    const Channel& console_choice = channels_[static_cast<std::size_t>(chosen)];
    const bool sounding = console_choice.active && console_choice.envelope_stage != kRelease;
    if (growing_ && (sounding || priority < console_choice.priority))
    {
        if (const int grown = GrownChannel(chosen); grown >= 0)
        {
            chosen = grown;
        }
    }
    Channel& channel = channels_[static_cast<std::size_t>(chosen)];
    if (priority < channel.priority)
    {
        return -1;
    }

    if (channel.owner >= 0)
    {
        Callback(channel, 0);
    }
    channel.sync = kSyncStop;
    channel.active = false;

    channel.next = -1;
    channel.owner = owner;
    channel.length = 0;
    channel.priority = static_cast<uint8_t>(priority);
    channel.volume = 127;
    channel.start = false;
    channel.auto_sweep = true;
    channel.key = 60;
    channel.root_key = 60;
    channel.velocity = 127;
    channel.initial_pan = 0;
    channel.user_decay = 0;
    channel.user_decay2 = 0;
    channel.user_pitch = 0;
    channel.user_pan = 0;
    channel.pan_range = 127;
    channel.sweep_pitch = 0;
    channel.sweep_length = 0;
    channel.sweep_counter = 0;
    SetAttack(channel, 127);
    SetDecay(channel, 127);
    channel.sustain = 127;
    SetRelease(channel, 127);
    channel.lfo.parameters = DefaultLfo();
    return chosen;
}

int Driver::GrownChannel(int chosen)
{
    // A grown channel of the same part that plays no more, or else a new one.
    const int part = PartOf(chosen);
    for (int i = kChannelCount; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (PartOf(channel.twin) == part && !channel.active && channel.owner < 0 && channel.sync == 0 &&
            !IsChannelActive(i))
        {
            channel.twin = static_cast<uint16_t>(chosen);
            registers_->Write(ChannelRegister(i, kGrownChannelTwin), 4, static_cast<uint32_t>(chosen));
            return i;
        }
    }
    if (ChannelCount() >= kMostChannels)
    {
        return -1;
    }

    const int index = ChannelCount();
    Channel& channel = channels_.emplace_back();
    channel.id = static_cast<uint16_t>(index);
    channel.twin = static_cast<uint16_t>(chosen);
    original_volumes_.push_back(0);
    original_pans_.push_back(0);
    registers_->Write(ChannelRegister(index, kGrownChannelTwin), 4, static_cast<uint32_t>(chosen));
    return index;
}

void Driver::Free(Channel& channel)
{
    channel.owner = -1;
}

// The callback of a sequence's channel: status 0 when another note takes the channel, 1 when the channel ends.
void Driver::Callback(Channel& channel, int status)
{
    const int owner = channel.owner;
    if (status == 1)
    {
        channel.priority = 0;
        Free(channel);
    }

    Track& track = tracks_[static_cast<std::size_t>(owner)];
    const int index = channel.id;
    if (track.channels == index)
    {
        track.channels = channel.next;
        return;
    }
    for (int at = track.channels; at >= 0; at = channels_[static_cast<std::size_t>(at)].next)
    {
        Channel& previous = channels_[static_cast<std::size_t>(at)];
        if (previous.next == index)
        {
            previous.next = channel.next;
            return;
        }
    }
}

void Driver::StopUnlockedChannels(uint32_t mask)
{
    for (int i = 0; i < kChannelCount && mask != 0; i++, mask >>= 1)
    {
        if (!(mask & 1) || (locked_ & (1u << i)))
        {
            continue;
        }

        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.owner >= 0)
        {
            Callback(channel, 0);
        }
        StopChannel(i, 0);
        channel.priority = 0;
        Free(channel);
        channel.sync = 0;
        channel.active = false;
    }
}

void Driver::LockChannels(uint32_t mask, uint32_t weak)
{
    StopUnlockedChannels(mask);
    if (weak & 1)
    {
        weakly_locked_ |= mask;
    }
    else
    {
        locked_ |= mask;
    }
}

void Driver::InvalidateWave(uint32_t start, uint32_t end)
{
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        if (channel.active && channel.type == kPcm && start <= channel.data && channel.data <= end)
        {
            channel.start = false;
            StopChannel(i, 0);
        }
    }
}

bool Driver::ReadInstrument(uint32_t bank, int program, int key, Instrument& instrument) const
{
    if (program < 0 || static_cast<uint32_t>(program) >= Read32(bank + kBankCount))
    {
        return false;
    }

    const auto read_parameters = [this, &instrument](uint32_t at)
    {
        instrument.wave = {Read16(at), Read16(at + 2)};
        instrument.root_key = Read8(at + 4);
        instrument.attack = Read8(at + 5);
        instrument.decay = Read8(at + 6);
        instrument.sustain = Read8(at + 7);
        instrument.release = Read8(at + 8);
        instrument.pan = Read8(at + 9);
    };

    const uint32_t offset = Read32(bank + kBankOffsets + 4 * static_cast<uint32_t>(program));
    const uint32_t at = bank + (offset >> 8);
    instrument.type = static_cast<uint8_t>(offset);
    switch (instrument.type)
    {
    case kInstrumentPcm:
    case kInstrumentPsg:
    case kInstrumentNoise:
    case kInstrumentDirectPcm:
    case kInstrumentDummy:
        read_parameters(at);
        return true;
    case kInstrumentDrums:
        {
            const int low = Read8(at);
            const int high = Read8(at + 1);
            if (key < low || key > high)
            {
                return false;
            }
            const uint32_t region = at + 2 + 12 * static_cast<uint32_t>(key - low);
            instrument.type = Read8(region);
            read_parameters(region + 2);
            return true;
        }
    case kInstrumentKeySplit:
        {
            int i = 0;
            while (key > Read8(at + static_cast<uint32_t>(i)))
            {
                i++;
                if (i >= kKeySplits)
                {
                    return false;
                }
            }
            const uint32_t region = at + kKeySplits + 12 * static_cast<uint32_t>(i);
            instrument.type = Read8(region);
            read_parameters(region + 2);
            return true;
        }
    default:
        return false;
    }
}

uint32_t Driver::WaveAddress(uint32_t bank, int wave_archive, int wave) const
{
    const uint32_t archive = Read32(bank + kBankWaveArchives + 8 * static_cast<uint32_t>(wave_archive));
    if (archive == 0 || static_cast<uint32_t>(wave) >= Read32(archive + kWaveArchiveCount))
    {
        return 0;
    }

    // An offset below main memory counts from the wave archive's start. A larger one is the wave's address.
    uint32_t address = Read32(archive + kWaveArchiveOffsets + 4 * static_cast<uint32_t>(wave));
    if (address != 0 && address < kMainMemory)
    {
        address += archive;
    }

    return address;
}

} // namespace ntdlib::ntr::snd
