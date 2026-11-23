// SPDX-License-Identifier: MIT

// The parts of the DSE driver model that its state is built from: the driver's arithmetic and tables of times, the
// fades, the envelopes and the LFOs. driver_state.h includes this header.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "xsflib/bytes.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/tables.h"

namespace ntdlib::dse
{

// The driver's arithmetic: the ARM's 32-bit operations, with wrapping.
inline int32_t Wrap(int64_t value)
{
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
}

inline int32_t Divide(int32_t dividend, int32_t divisor)
{
    if (divisor == 0)
    {
        return 0;
    }
    if (dividend == INT32_MIN && divisor == -1)
    {
        return INT32_MIN;
    }
    return dividend / divisor;
}

inline uint32_t DivideUnsigned(uint32_t dividend, uint32_t divisor)
{
    return divisor == 0 ? 0 : dividend / divisor;
}

// Whether a channel that selects a program takes the program's LFO `entry` (its 16 bytes): one of any type but 0. The
// oldest build takes only the LFOs of type 1 or 3 whose output is the pitch, the volume or the pan (0x020C40B4 in
// Luminous Arc's ARM9 binary).
inline bool TakesLfo(Build build, std::span<const uint8_t> entry)
{
    if (build == Build::kOldest)
    {
        return (entry[1] == 1 || entry[1] == 3) && entry[2] >= 1 && entry[2] <= 3;
    }

    return entry[1] != 0;
}

// The callbacks of the LFOs' waveforms (0x020B1250). The times of an envelope without a multiplier read them past the
// end of their table.
constexpr std::array<uint32_t, 16> kWaveformCallbacks = {
    0x020753C0, 0x020753FC, 0x02075444, 0x02075498, 0x020754FC, 0x02075538, 0x02075574, 0x020755B8,
    0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC, 0x020753AC,
};

// An entry of the table of an envelope's times with a multiplier (0x020B0F50). The driver reads entries past the
// table's 128 from the table that follows it.
inline uint32_t ScaledTime(uint8_t index)
{
    if (index < 128)
    {
        return tables::kEnvelopeTimes[index];
    }
    const uint32_t word = tables::kEnvelopeTimesUnscaled[(index - 128) / 2];
    return index % 2 == 0 ? word & 0xFFFF : word >> 16;
}

// An entry of the table of an envelope's times without a multiplier (0x020B1050). The driver reads entries past the
// table's 128 from the tables that follow it: the callbacks of the LFOs' waveforms, the pan curve, the keys' places,
// a word of 8 (0x020B1390) and the frequencies.
inline uint32_t UnscaledTime(uint8_t index)
{
    if (index < 128)
    {
        return tables::kEnvelopeTimesUnscaled[index];
    }
    std::size_t word = index - 128;
    if (word < kWaveformCallbacks.size())
    {
        return kWaveformCallbacks[word];
    }
    word -= kWaveformCallbacks.size();
    if (word < 32)
    {
        return xsflib::LoadLe32(&tables::kPanCurve[4 * word]);
    }
    word -= 32;
    if (word < 32)
    {
        return xsflib::LoadLe32(&tables::kKeyPlaces[4 * word]);
    }
    word -= 32;
    if (word == 0)
    {
        return 8;
    }
    word -= 1;
    return static_cast<uint32_t>(tables::kSemitoneFrequencies[2 * word]) |
           static_cast<uint32_t>(tables::kSemitoneFrequencies[2 * word + 1]) << 16;
}

// The ticks of a stage of an envelope (0x02074D58), without the driver's test of 0x7F.
inline int32_t SlideTicks(uint8_t multiplier, uint8_t index, int16_t tick)
{
    if (multiplier == 0)
    {
        return static_cast<int32_t>(DivideUnsigned(UnscaledTime(index) * 1000, static_cast<uint32_t>(tick)));
    }
    return Divide(Wrap(int64_t{multiplier} * ScaledTime(index) * 1000), tick);
}

struct Fade
{
    int32_t current = 0;
    int32_t delta = 0;
    int32_t target = 0;
    uint16_t ticks = 0;
};

// An envelope's parameters: 16 bytes of a split, or a channel's override.
struct EnvelopeParameters
{
    static EnvelopeParameters Read(std::span<const uint8_t> bytes)
    {
        return {
            .use = bytes[0],
            .multiplier = bytes[1],
            .unknown2 = xsflib::LoadLe16(&bytes[2]),
            .unknown4 = xsflib::LoadLe32(&bytes[4]),
            .attack_begin = bytes[8],
            .attack_time = bytes[9],
            .decay_time = bytes[10],
            .sustain_level = bytes[11],
            .hold_time = bytes[12],
            .sustain_time = bytes[13],
            .release_time = bytes[14],
            .unknown15 = bytes[15],
        };
    }

    // 0x02074C84: every value unset.
    void Reset()
    {
        *this = {.use = 0,
                 .multiplier = 0xFF,
                 .unknown2 = 0xFFFF,
                 .unknown4 = 0xFFFFFFFF,
                 .attack_begin = 0xFF,
                 .attack_time = 0xFF,
                 .decay_time = 0xFF,
                 .sustain_level = 0xFF,
                 .hold_time = 0xFF,
                 .sustain_time = 0xFF,
                 .release_time = 0xFF,
                 .unknown15 = 0xFF};
    }

    // 0x02074CA0: the override applies while one of its values is set.
    void SetUse()
    {
        use = multiplier > 0x7F && attack_begin > 0x7F && attack_time > 0x7F && decay_time > 0x7F &&
                      sustain_level > 0x7F && hold_time > 0x7F && sustain_time > 0x7F && release_time > 0x7F
                  ? 0
                  : 1;
    }

    uint8_t use = 0;
    uint8_t multiplier = 0;
    uint16_t unknown2 = 0;
    uint32_t unknown4 = 0;
    uint8_t attack_begin = 0;
    uint8_t attack_time = 0;
    uint8_t decay_time = 0;
    uint8_t sustain_level = 0;
    uint8_t hold_time = 0;
    uint8_t sustain_time = 0;
    uint8_t release_time = 0;
    uint8_t unknown15 = 0;
};

enum EnvelopeState : uint8_t
{
    kOff = 0,
    kConstant = 1,
    kDone = 2,
    kAttack = 3,
    kHold = 4,
    kDecay = 5,
    kSustain = 6,
    kRelease = 7,
    kReleaseEnd = 8,
};

// A voice's envelope. Its volume is in 7.23 fixed point: 127 << 23 is full.
struct Envelope
{
    // 0x02074D58
    void SlideTo(int32_t target_volume, int32_t index, int16_t tick)
    {
        if (index == 0x7F)
        {
            delta = 0;
            ticks = 0x7FFFFFFF;
            return;
        }
        target = static_cast<uint8_t>(target_volume);
        ticks = SlideTicks(parameters.multiplier, static_cast<uint8_t>(index), tick);
        delta = ticks != 0 ? Divide(Wrap((int64_t{target_volume} << 23) - volume), ticks) : 0;
    }

    // 0x02074E0C: the envelope's start.
    void Start(int16_t tick)
    {
        if (parameters.use)
        {
            if (parameters.attack_time != 0)
            {
                volume = static_cast<int32_t>(static_cast<uint32_t>(parameters.attack_begin) << 23);
                state = kAttack;
                SlideTo(0x7F, parameters.attack_time, tick);
            }
            else
            {
                volume = 0x3F800000;
                if (parameters.hold_time != 0)
                {
                    SlideTo(0x7F, parameters.hold_time, tick);
                    state = kHold;
                }
                else if (parameters.decay_time != 0)
                {
                    SlideTo(static_cast<int8_t>(parameters.sustain_level), parameters.decay_time, tick);
                    state = kDecay;
                }
                else
                {
                    SlideTo(0, parameters.sustain_time, tick);
                    state = kSustain;
                }
            }
            active = 1;
        }
        else
        {
            if (state == kConstant)
            {
                return;
            }
            state = kOff;
            volume = 0x3F800000;
        }
    }

    // 0x02074ED8
    void Release(int16_t tick)
    {
        if (state == kOff)
        {
            return;
        }
        SlideTo(0, parameters.release_time, tick);
        state = kRelease;
    }

    // 0x02074F04 and 0x02074F3C
    void Stop()
    {
        state = kOff;
        volume = 0;
        ticks = 0;
        active = 0;
    }

    // 0x02074F54: the level of the tick.
    int8_t Tick(int16_t tick)
    {
        if (state > kDone)
        {
            if (ticks == 0)
            {
                volume = static_cast<int32_t>(static_cast<uint32_t>(target) << 23);
                switch (state)
                {
                case kAttack:
                    if (parameters.hold_time != 0)
                    {
                        SlideTo(0x7F, parameters.hold_time, tick);
                        state = kHold;
                        break;
                    }
                    [[fallthrough]];
                case kHold:
                    if (parameters.decay_time != 0)
                    {
                        SlideTo(static_cast<int8_t>(parameters.sustain_level), parameters.decay_time, tick);
                        state = kDecay;
                        break;
                    }
                    volume = static_cast<int32_t>(static_cast<uint32_t>(parameters.sustain_level) << 23);
                    [[fallthrough]];
                case kDecay:
                    if (parameters.sustain_time != 0)
                    {
                        SlideTo(0, parameters.sustain_time, tick);
                        state = kSustain;
                        break;
                    }
                    [[fallthrough]];
                case kSustain:
                    SlideTo(0, 0, tick);
                    state = kDone;
                    break;
                case kRelease:
                    state = kReleaseEnd;
                    volume = 0;
                    ticks = 0;
                    break;
                default:
                    break;
                }
            }
            else
            {
                int32_t next = Wrap(int64_t{volume} + delta);
                next = std::clamp(next, 0, 0x3FFFFFFF);
                ticks--;
                volume = next;
            }
        }
        return static_cast<int8_t>(volume >> 23);
    }

    EnvelopeParameters parameters;
    int32_t volume = 0;
    int32_t delta = 0;
    int32_t ticks = 0;
    uint8_t state = kOff;
    uint8_t target = 0;
    uint8_t active = 0;
};

// An LFO's settings: 16 bytes of a program, or of a channel.
struct LfoSettings
{
    static LfoSettings Read(std::span<const uint8_t> bytes)
    {
        return {
            .unknown0 = bytes[0],
            .type = bytes[1],
            .output = bytes[2],
            .waveform = bytes[3],
            .amplitude = static_cast<int32_t>(xsflib::LoadLe32(&bytes[4])),
            .phase_ms = xsflib::LoadLe16(&bytes[8]),
            .delay_ms = xsflib::LoadLe16(&bytes[10]),
            .envelope_ms = xsflib::LoadLe16(&bytes[12]),
            .unknown14 = bytes[14],
            .unknown15 = bytes[15],
        };
    }

    uint8_t unknown0 = 0;
    uint8_t type = 0;
    uint8_t output = 0;
    uint8_t waveform = 0;
    int32_t amplitude = 0;
    uint16_t phase_ms = 0;
    uint16_t delay_ms = 0;
    uint16_t envelope_ms = 0;
    uint8_t unknown14 = 0;
    uint8_t unknown15 = 0;
};

struct Lfo
{
    uint8_t phase_flags = 0;
    uint8_t type = 0;
    uint16_t update_flags = 0;
    uint16_t ticks_per_phase = 0;
    uint16_t ticks_until_phase = 0;
    int32_t output = 0;
    int32_t amplitude = 0;
    int32_t delta = 0;
    uint16_t delay = 0;
    uint16_t envelope_ticks = 0;
    int32_t envelope_level = 0;
    int32_t envelope_delta = 0;
    uint8_t output_index = 0;
    uint8_t waveform = 0;
};

// A voice's LFOs. The driver links the LFOs whose settings give a type, in their order.
struct LfoBank
{
    std::array<int16_t, 6> outputs = {};
    std::array<Lfo, 4> lfos = {};
    std::array<uint8_t, 4> list = {};
    int count = 0;
};

} // namespace ntdlib::dse
