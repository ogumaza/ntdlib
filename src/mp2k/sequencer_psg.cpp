// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/wave/sinc.h"
#include "sequencer_shared.h"

namespace ntdlib::mp2k
{

namespace
{

constexpr uint32_t kRegNr51 =
    0x04000081; // each channel's outputs: the right side in the low 4 bits, the left in the high

constexpr uint32_t kRegWaveRam = 0x04000090;

// NRx2's direction bit: the hardware's envelope rises with it, and falls without it.
constexpr uint8_t kEnvelopeUp = 0x08;

// The wave channel's NR32 for each level of the envelope: mute, then 25%, 50%, 75% and the full volume. The driver's
// table holds 16 levels. The SDK's tables put the numbers 0 to 15 after it, so a level of 16 to 31 mutes the channel.
constexpr uint8_t kWaveVolumes[32] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40, 0x40, 0x40, 0x80,
                                      0x80, 0x80, 0x80, 0x20, 0x20, 0,    1,    2,    3,    4,    5,
                                      6,    7,    8,    9,    10,   11,   12,   13,   14,   15};

// Whether a PSG channel is the wave channel. The wave channel has no envelope in the hardware: NR32 sets its volume.
bool IsWaveChannel(const Channel& c)
{
    return (c.type & kVoicePsg) == 3;
}

// Asks for the frame's write of the channel's volume, with the hardware envelope's step time and direction `step`.
void SetEnvelopeStep(Channel& c, uint8_t step)
{
    c.modify |= kModifyVolume;
    if (!IsWaveChannel(c))
    {
        c.step = step;
    }
}

// Sets a PSG channel's pan and its envelope's full and sustain levels from its volumes.
void SetPsgVolume(Channel& c)
{
    // A note panned at least two to one plays on one side only, at up to the full level. Otherwise it plays on both.
    bool one_side = false;
    if (c.right >= c.left && c.right / 2 >= c.left)
    {
        c.pan = 0x0F;
        one_side = true;
    }
    else if (c.right < c.left && c.left / 2 >= c.right)
    {
        c.pan = 0xF0;
        one_side = true;
    }

    const uint32_t sum = uint32_t(c.left) + c.right;
    c.goal = uint8_t(sum / 16);
    if (!one_side)
    {
        c.pan = 0xFF;
    }
    else if (c.goal > 15)
    {
        c.goal = 15;
    }

    c.sustain_goal = uint8_t((c.goal * c.sustain + 15) >> 4);
    c.pan &= c.pan_mask;
}

// Each part of a PSG channel's envelope ends in a step of its counter, the end of the frame's envelope work, or the
// channel's end.
enum class Next
{
    kStep,
    kDone,
    kOff
};

// Starts the echo after a PSG note's release. A silent echo ends the channel.
Next StartEcho(Channel& c)
{
    c.envelope = uint8_t((c.goal * c.echo_volume + 0xFF) >> 8);
    if (!c.envelope)
    {
        return Next::kOff;
    }

    c.status |= kStatusEcho;
    SetEnvelopeStep(c, kEnvelopeUp);

    return Next::kDone;
}

// Starts a PSG channel's sustain, or its echo if the voice's sustain is 0.
Next StartSustain(Channel& c)
{
    if (c.sustain == 0)
    {
        c.status &= uint8_t(~kStatusEnvelope);
        return StartEcho(c);
    }

    c.status--;
    SetEnvelopeStep(c, kEnvelopeUp);
    c.envelope = c.sustain_goal;
    c.counter = 7;

    return Next::kStep;
}

// Starts a PSG channel's decay, or its sustain if the voice's decay is 0.
Next StartDecay(Channel& c)
{
    c.status--;
    c.counter = c.decay;
    if (!c.counter)
    {
        return StartSustain(c);
    }

    SetEnvelopeStep(c, c.decay);
    c.envelope = c.goal;

    return Next::kStep;
}

// Moves a PSG channel's envelope a level on in its phase once the counter has run out, and sets the counter again.
Next StepEnvelope(Channel& c)
{
    if (c.counter != 0)
    {
        return Next::kStep;
    }

    if (IsWaveChannel(c))
    {
        c.modify |= kModifyVolume;
    }
    SetPsgVolume(c);

    const uint8_t phase = c.status & kStatusEnvelope;
    if (phase == kPhaseRelease)
    {
        c.envelope--;
        if (int8_t(c.envelope) <= 0)
        {
            return StartEcho(c);
        }

        c.counter = c.release;
    }
    else if (phase == kPhaseSustain)
    {
        c.envelope = c.sustain_goal;
        c.counter = 7;
    }
    else if (phase == kPhaseDecay)
    {
        c.envelope--;
        if (int8_t(c.envelope) <= int8_t(c.sustain_goal))
        {
            return StartSustain(c);
        }

        c.counter = c.decay;
    }
    else
    {
        c.envelope++;
        if (c.envelope >= c.goal)
        {
            return StartDecay(c);
        }

        c.counter = c.attack;
    }

    return Next::kStep;
}

// The Metroid games' PSG routine (0x080011DC in Metroid Fusion) counts a channel's envelope a level at a time, once
// every `attack`, `decay` or `release` frames, with no extra step every 15th frame. It writes a square's or the noise
// channel's envelope only when a phase starts or the track's volume changes, and restarts the channel each time. The
// hardware envelope moves the level in between. The wave channel takes its volume at each step of the count, and
// restarts only when its note starts or its volume changes.

// Asks for the frame's write of the channel's level at the start of a phase, with the hardware envelope's step time and
// direction `step`. A square or the noise channel restarts. The wave channel restarts only if `restart`.
void MetroidPhase(Channel& c, uint8_t step, bool restart = false)
{
    c.modify |= kModifyVolume;
    c.lag = 0;
    if (!IsWaveChannel(c))
    {
        c.step = step;
    }
    else if (restart)
    {
        c.n4 |= 0x80;
    }
}

// Asks for the frame's write of the wave channel's level after a step of the count. The hardware envelope moves the
// other channels' levels.
void MetroidStep(Channel& c)
{
    if (IsWaveChannel(c))
    {
        c.modify |= kModifyVolume;
    }
}

// Starts the echo after a PSG note's release, at its level with a step time of 0. Without both an echo volume and an
// echo length, the note ends (0x080014F0).
Next MetroidEcho(Channel& c)
{
    c.status &= uint8_t(~kStatusEnvelope);
    if (!c.echo_volume || !c.echo_length)
    {
        return Next::kOff;
    }

    c.envelope = uint8_t((c.goal * c.echo_volume + 0xFF) >> 8);
    c.counter = c.echo_length;
    c.status |= kStatusEcho;
    MetroidPhase(c, 0);

    return Next::kStep;
}

// Starts a PSG channel's sustain, at the sustain level with a step time of 0 (0x08001474). The count goes back to the
// sustain level every frame.
Next MetroidSustain(Channel& c)
{
    c.status = uint8_t((c.status & ~kStatusEnvelope) | kPhaseSustain);
    c.envelope = c.sustain_goal;
    c.counter = 1;
    MetroidPhase(c, 0);

    return Next::kStep;
}

// Counts a level down in a PSG channel's decay. The sustain starts once the count reaches the sustain level
// (0x08001452).
Next MetroidDecayStep(Channel& c)
{
    c.envelope--;
    if (int8_t(c.envelope) <= int8_t(c.sustain_goal))
    {
        return MetroidSustain(c);
    }

    c.counter = c.decay;
    MetroidStep(c);

    return Next::kStep;
}

// Starts a PSG channel's decay, or its sustain if the voice's decay is 0. The count goes to the full level, then a
// level down at once, so the hardware restarts a level below the full level and falls (0x08001414).
Next MetroidDecay(Channel& c)
{
    if (!c.decay)
    {
        return MetroidSustain(c);
    }

    c.status = uint8_t((c.status & ~kStatusEnvelope) | kPhaseDecay);
    c.envelope = c.goal;
    MetroidPhase(c, c.decay);

    return MetroidDecayStep(c);
}

// Counts a level up in a PSG channel's attack. The decay starts once the count reaches the full level (0x08001402).
Next MetroidAttackStep(Channel& c)
{
    c.envelope++;
    if (int8_t(c.envelope) >= int8_t(c.goal))
    {
        return MetroidDecay(c);
    }

    c.counter = c.attack;
    MetroidStep(c);

    return Next::kStep;
}

// Counts a level down in a PSG channel's release. The echo starts once the count reaches 0 (0x080014D2).
Next MetroidReleaseStep(Channel& c)
{
    c.envelope--;
    if (int8_t(c.envelope) <= 0)
    {
        return MetroidEcho(c);
    }

    c.counter = c.release;
    MetroidStep(c);

    return Next::kStep;
}

// Starts a PSG note in the Metroid games (0x08001362). A voice with a sound length plays at the sustain level until
// the hardware's length counter stops it. Otherwise an attack restarts the hardware at 0, rising, while the count
// starts at 1, a level ahead of the hardware. Without an attack, the decay starts at once.
Next MetroidStart(Channel& c)
{
    if (c.length)
    {
        c.envelope = c.sustain_goal;
        c.status = uint8_t((c.status & ~kStatusEnvelope) | kPhaseSustain);
        c.counter = 0xFF;
        MetroidPhase(c, 0);
        return Next::kStep;
    }
    if (!c.attack)
    {
        return MetroidDecay(c);
    }

    c.envelope = 0;
    MetroidPhase(c, uint8_t(c.attack + kEnvelopeUp));
    if (!IsWaveChannel(c))
    {
        c.lag = 1;
    }

    return MetroidAttackStep(c);
}

// Moves a PSG channel's envelope on by a frame in the Metroid games, after its first. A change of the track's volume
// restarts the hardware at the count, with a step time of 0. A release restarts the hardware a level below the count,
// falling. A voice with a sound length has no release (0x080011DC).
Next StepMetroidEnvelope(Channel& c)
{
    if (c.modify & kModifyVolume)
    {
        SetPsgVolume(c);
        MetroidPhase(c, 0, true);
    }

    if (c.status & kStatusEcho)
    {
        return c.counter ? Next::kStep : Next::kOff;
    }
    if ((c.status & kStatusStop) && (c.status & kStatusEnvelope))
    {
        if (!c.release || c.length)
        {
            return MetroidEcho(c);
        }

        c.status &= uint8_t(~kStatusEnvelope);
        MetroidPhase(c, c.release);
        return MetroidReleaseStep(c);
    }
    if (c.counter)
    {
        return Next::kStep;
    }

    switch (c.status & kStatusEnvelope)
    {
    case kPhaseAttack:
        return MetroidAttackStep(c);
    case kPhaseDecay:
        return MetroidDecayStep(c);
    case kPhaseSustain:
        if (c.length)
        {
            c.counter = 0xFF;
        }
        else
        {
            c.envelope = c.sustain_goal;
            c.counter = 1;
        }
        return Next::kStep;
    default:
        return MetroidReleaseStep(c);
    }
}

} // namespace

void Sequencer::SetEnvelopeCounter(int counter)
{
    if (counter < 0 || counter > 14)
    {
        throw std::invalid_argument("the PSG envelopes' frame counter runs from 0 to 14");
    }
    c15_ = uint8_t(counter);
}

void Sequencer::RunPsg()
{
    // The steps of the hardware's envelopes are 64 a second, so every 15th frame takes two.
    c15_ = c15_ ? uint8_t(c15_ - 1) : 14;

    // The console's PSG channels, then the grown ones.
    for (int index = kFirstPsgChannel; index < ChannelCount(); index++)
    {
        Channel& c = channels_[size_t(index)];
        const int n = c.part;
        if (!n || !(c.status & kStatusOn))
        {
            continue;
        }

        // The hardware envelope's step time and direction stay as the last write left them, unless the frame changes
        // them.
        c.step = SoundRegister(UnitAddress(c.unit, kPsgRegisters[n - 1].nrx2));

        // A note released before the channel started it never sounds.
        Next next = Next::kStep;
        const bool never_started = (c.status & kStatusStart) && (c.status & kStatusStop);
        if (never_started)
        {
            next = Next::kOff;
        }
        else if (c.status & kStatusStart)
        {
            c.status = kPhaseAttack;
            c.modify = kModifyVolume | kModifyPitch;
            SetPsgVolume(c);
            StartPsgRegisters(n, c);
            c.counter = c.attack;
            c.lag = 0;
            if (info_.metroid_sequencer)
            {
                next = MetroidStart(c);
            }
            else if (c.attack)
            {
                c.envelope = 0;
            }
            else
            {
                next = StartDecay(c);
            }
        }
        else if (info_.metroid_sequencer)
        {
            next = StepMetroidEnvelope(c);
        }
        else if (c.status & kStatusEcho)
        {
            c.echo_length--;
            next = int8_t(c.echo_length) <= 0 ? Next::kOff : Next::kDone;
        }
        else if ((c.status & kStatusStop) && (c.status & kStatusEnvelope))
        {
            c.status &= uint8_t(~kStatusEnvelope);
            c.counter = c.release;
            if (!c.release)
            {
                next = StartEcho(c);
            }
            else
            {
                SetEnvelopeStep(c, c.release);
            }
        }
        else
        {
            next = StepEnvelope(c);
        }

        // The counter steps once a frame, and twice every 15th frame. The Metroid games' counter steps once a frame.
        int c15 = c15_;
        if (info_.metroid_sequencer && next == Next::kStep)
        {
            c.counter--;
            next = Next::kDone;
        }
        while (next == Next::kStep)
        {
            c.counter--;
            if (c15 != 0)
            {
                next = Next::kDone;
                break;
            }

            c15--;
            next = StepEnvelope(c);
        }
        if (next == Next::kOff)
        {
            StopPsg(n, c.unit);
            c.status = 0;
            c.modify = 0;
            EndNote(index, !never_started);
            continue;
        }

        CheckAudible(index);

        // A fixed-pitch voice's frequency is rounded to suit the output's resolution. The driver rounds it when it
        // changes, and rounding it again changes nothing.
        if (n < 4 && (c.type & kVoiceFixed))
        {
            if (info_.dac_bits < 0x40)
            {
                c.frequency = (c.frequency + 2) & 0x7FC;
            }
            else if (info_.dac_bits < 0x80)
            {
                c.frequency = (c.frequency + 1) & 0x7FE;
            }
        }

        WritePsgRegisters(n, c);
        c.modify = 0;
    }
}

void Sequencer::StartPsgRegisters(int n, Channel& c)
{
    // A square channel takes its duty in NRx1's top 2 bits. The wave channel takes its pattern when another pattern is
    // in its RAM: NR30's bit 6 has the channel play the other bank meanwhile. The noise channel takes its type in
    // NR43's bit 3. The length's enable goes in NRx4 with the pitch.
    const PsgRegisters& r = kPsgRegisters[n - 1];
    const auto at = [&c](uint32_t address)
    {
        return UnitAddress(c.unit, address);
    };
    switch (n)
    {
    case 3:
        if (c.wave != c.pattern)
        {
            WriteSound(at(r.nrx0), 1, 0x40);
            for (uint32_t i = 0; i < 4; i++)
            {
                WriteSound(at(kRegWaveRam + 4 * i), 4, rom_.U32(c.wave + 4 * i));
            }
            c.pattern = c.wave;
        }
        WriteSound(at(r.nrx0), 1, 0);
        WriteSound(at(r.nrx1), 1, c.length);
        c.n4 = c.length ? 0xC0 : 0x80;
        return;
    case 4:
        WriteSound(at(r.nrx1), 1, c.length);
        WriteSound(at(r.nrx3), 1, c.wave << 3);
        break;
    default:
        if (n == 1)
        {
            WriteSound(at(r.nrx0), 1, c.sweep);
        }
        WriteSound(at(r.nrx1), 1, (c.wave << 6) + c.length);
        break;
    }

    c.step = uint8_t(c.attack + kEnvelopeUp);
    c.n4 = c.length ? 0x40 : 0x00;
}

void Sequencer::WritePsgRegisters(int n, Channel& c)
{
    const PsgRegisters& r = kPsgRegisters[n - 1];
    const auto at = [&c](uint32_t address)
    {
        return UnitAddress(c.unit, address);
    };
    if (c.modify & kModifyPitch)
    {
        // The noise channel keeps its type in NR43's bit 3. The frequency's high bits go in NRx4.
        WriteSound(at(r.nrx3), 1, n == 4 ? (SoundRegister(at(r.nrx3)) & 0x08) | c.frequency : c.frequency);
        c.n4 = uint8_t((c.n4 & 0xC0) + ((c.frequency >> 8) & 0xFF));
        WriteSound(at(r.nrx4), 1, c.n4);
    }

    if (c.modify & kModifyVolume)
    {
        WriteSound(at(kRegNr51), 1, (SoundRegister(at(kRegNr51)) & ~c.pan_mask) | c.pan);
        if (n == 3)
        {
            // An envelope above 31 needs a wave voice whose sustain is above 16. The driver then reads further past its
            // table than kWaveVolumes covers, and the model wraps into kWaveVolumes. No song of the test images has
            // one. The first volume after a note starts restarts the wave channel.
            WriteSound(at(r.nrx2), 1, kWaveVolumes[c.envelope & 31]);
            if (c.n4 & 0x80)
            {
                WriteSound(at(r.nrx0), 1, 0x80);
                WriteSound(at(r.nrx4), 1, c.n4);
                c.n4 &= 0x7F;
            }
        }
        else
        {
            // The other channels restart at the envelope's level, and the hardware envelope moves on from there. The
            // Metroid games' count can run a level ahead of the hardware (see MetroidStart()).
            // Channel 1 restarts a second time unless NR10 has its sweep lower the pitch.
            WriteSound(at(r.nrx2), 1, (c.step & 0x0F) + (uint32_t(c.envelope - c.lag) << 4));
            WriteSound(at(r.nrx4), 1, c.n4 | 0x80);
            if (n == 1 && !(SoundRegister(at(r.nrx0)) & 0x08))
            {
                WriteSound(at(r.nrx4), 1, c.n4 | 0x80);
            }
        }
    }
}

void Sequencer::StopPsg(int n, int unit)
{
    const PsgRegisters& r = kPsgRegisters[n - 1];
    if (n == 3)
    {
        WriteSound(UnitAddress(unit, r.nrx0), 1, 0);
        return;
    }

    // A rising envelope with a step time of 0 stays at its level of 0.
    WriteSound(UnitAddress(unit, r.nrx2), 1, kEnvelopeUp);
    WriteSound(UnitAddress(unit, r.nrx4), 1, 0x80);
}

uint32_t Sequencer::UnitAddress(int unit, uint32_t address)
{
    return unit == 0
               ? address
               : agb::kGrownPsgRegisters + uint32_t(unit - 1) * agb::kGrownPsgBytes + (address - agb::kRegSound1CntL);
}

void Sequencer::SetSoundRegisters(std::span<const uint8_t> registers)
{
    if (registers.size() != sound_registers_.size())
    {
        throw std::invalid_argument("the sound registers take " + std::to_string(sound_registers_.size()) + " bytes");
    }

    std::copy(registers.begin(), registers.end(), sound_registers_.begin());
}

void Sequencer::WriteSound(uint32_t address, int size, uint32_t value)
{
    if (size < 4)
    {
        value &= (1u << (8 * size)) - 1;
    }
    for (int i = 0; i < size; i++)
    {
        const uint32_t byte = address + uint32_t(i);
        if (byte >= agb::kGrownPsgRegisters)
        {
            const uint32_t offset = byte - agb::kGrownPsgRegisters;
            grown_registers_[offset / agb::kGrownPsgBytes][offset % agb::kGrownPsgBytes] = uint8_t(value >> (8 * i));
        }
        else
        {
            sound_registers_[byte - agb::kRegSound1CntL] = uint8_t(value >> (8 * i));
        }
    }
    sound_writes_.push_back({address, uint8_t(size), value});
}

uint8_t Sequencer::SoundRegister(uint32_t address) const
{
    if (address >= agb::kGrownPsgRegisters)
    {
        const uint32_t offset = address - agb::kGrownPsgRegisters;
        return grown_registers_[offset / agb::kGrownPsgBytes][offset % agb::kGrownPsgBytes];
    }
    return sound_registers_[address - agb::kRegSound1CntL];
}

} // namespace ntdlib::mp2k
