// SPDX-License-Identifier: MIT

// The LFOs of DSE's voices: their start from a program's settings, their waveforms and their steps.

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver_state.h"
#include "ntdlib/dse/driver.h"

namespace ntdlib::dse
{

namespace
{

// The flags of the voice's parameters that each of an LFO's outputs changes (0x020A9FA0).
constexpr std::array<uint16_t, 8> kOutputFlags = {0, 0x10, 0x20, 0x40, 0, 0, 0, 0};

} // namespace

void Driver::State::StartLfos(LfoBank& bank, const std::array<LfoSettings, 4>& settings, int8_t level)
{
    const int32_t constant = Wrap(int64_t{Divide(int32_t{level} << 8, 127)} << 16);
    bank.count = 0;
    for (std::size_t k = 0; k < 4; k++)
    {
        const LfoSettings& s = settings[k];
        if (s.type == 0)
        {
            continue;
        }
        Lfo& lfo = bank.lfos[k];
        lfo.type = s.type;
        lfo.phase_flags = 0;
        lfo.output = 0;
        if (s.type == 1)
        {
            lfo.delay = static_cast<uint16_t>(Divide(int32_t{s.delay_ms} * 1000, tick));
            const int32_t ticks = Divide(int32_t{s.envelope_ms} * 1000, tick);
            lfo.envelope_ticks = static_cast<uint16_t>(ticks);
            if (ticks != 0)
            {
                lfo.envelope_level = 0;
                lfo.envelope_delta = Divide(0x1000000, ticks);
            }
            else
            {
                lfo.envelope_level = 0x1000000;
                lfo.envelope_delta = 0;
            }
        }
        else
        {
            lfo.delay = 0;
            lfo.envelope_ticks = 0;
            lfo.envelope_level = constant;
        }
        lfo.ticks_until_phase = 0;
        lfo.delta = 0;
        if (s.phase_ms == 0)
        {
            lfo.ticks_until_phase = 0;
            lfo.ticks_per_phase = 0;
        }
        else
        {
            int32_t per = Divide(int32_t{s.phase_ms} * 1000, tick);
            if (per == 0)
            {
                per = 1;
            }
            lfo.ticks_per_phase = static_cast<uint16_t>(per);
            // The oldest build waits a whole phase before the first (0x020C5478 in Luminous Arc's ARM9 binary).
            lfo.ticks_until_phase = build == Build::kOldest ? static_cast<uint16_t>(per) : 0;
            lfo.amplitude = Wrap(int64_t{s.amplitude} << 10);
            lfo.delta = Divide(lfo.amplitude, per);
        }
        lfo.waveform = s.waveform;
        lfo.output_index = s.output;
        lfo.update_flags = s.output < kOutputFlags.size() ? kOutputFlags[s.output] : 0;
        bank.list[static_cast<std::size_t>(bank.count++)] = static_cast<uint8_t>(k);
    }
}

void Driver::State::SetConstantLevels(LfoBank& bank, int8_t level)
{
    for (int i = 0; i < bank.count; i++)
    {
        Lfo& lfo = bank.lfos[bank.list[static_cast<std::size_t>(i)]];
        if (lfo.type == 3)
        {
            lfo.envelope_level = Wrap(int64_t{Divide(int32_t{level} << 8, 127)} * 0x10000);
        }
    }
}

int32_t Driver::State::Waveform(Lfo& lfo)
{
    switch (lfo.waveform)
    {
    case 0:
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            lfo.output = lfo.output != 0 ? 0 : lfo.amplitude;
        }
        lfo.ticks_until_phase--;
        return lfo.output;
    case 1:
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            lfo.output = (lfo.phase_flags & 2) != 0 ? Wrap(-int64_t{lfo.amplitude}) : lfo.amplitude;
            lfo.phase_flags ^= 2;
        }
        lfo.ticks_until_phase--;
        return lfo.output;
    case 2:
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            // The oldest build turns at every phase (0x020C56DC).
            if ((lfo.phase_flags & 1) || build == Build::kOldest)
            {
                lfo.delta = Wrap(-int64_t{lfo.delta});
            }
            lfo.phase_flags |= 1;
        }
        lfo.ticks_until_phase--;
        lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
        return lfo.output;
    case 3:
        if (build == Build::kOldest)
        {
            // The oldest build turns at every phase, and makes each phase after the first twice as long (0x020C572C).
            uint16_t left = lfo.ticks_until_phase;
            if (left == 0)
            {
                left = lfo.ticks_per_phase;
                if (lfo.phase_flags & 1)
                {
                    left = static_cast<uint16_t>(left * 2);
                }
                lfo.delta = Wrap(-int64_t{lfo.delta});
                lfo.phase_flags = static_cast<uint8_t>((lfo.phase_flags | 1) ^ 2);
            }
            lfo.ticks_until_phase = static_cast<uint16_t>(left - 1u);
            lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
            return lfo.output;
        }
        else
        {
            uint16_t left = lfo.ticks_until_phase;
            if (left == 0)
            {
                left = lfo.ticks_per_phase;
                if (lfo.phase_flags & 1)
                {
                    lfo.delta = Wrap(-int64_t{lfo.delta});
                }
                else
                {
                    left /= 2;
                    if (left < 1)
                    {
                        left = 1;
                    }
                }
                lfo.phase_flags |= 1;
            }
            lfo.ticks_until_phase = static_cast<uint16_t>(left - 1u);
            lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
            return lfo.output;
        }
    case 4:
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            lfo.output = 0;
        }
        else
        {
            lfo.output = Wrap(int64_t{lfo.output} + lfo.delta);
        }
        lfo.ticks_until_phase--;
        return lfo.output;
    case 5:
        if (build == Build::kOldest)
        {
            // The oldest build's table gives waveform 5 the function of waveform 4 (0x02112A74).
            lfo.waveform = 4;
            const int32_t output = Waveform(lfo);
            lfo.waveform = 5;
            return output;
        }
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            lfo.output = lfo.amplitude;
        }
        else
        {
            lfo.output = Wrap(int64_t{lfo.output} - lfo.delta);
        }
        lfo.ticks_until_phase--;
        return lfo.output;
    case 6:
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            lfo.output = Wrap(int64_t{lfo.amplitude >> 16} * Random());
        }
        lfo.ticks_until_phase--;
        return lfo.output;
    case 7:
        if (lfo.ticks_until_phase == 0)
        {
            lfo.ticks_until_phase = lfo.ticks_per_phase;
            const int32_t amplitude = lfo.amplitude;
            // The oldest build subtracts the whole amplitude (0x020C5808).
            const int32_t offset = build == Build::kOldest ? amplitude : amplitude >> 1;
            lfo.output = Wrap(int64_t{amplitude >> 15} * Random() - offset);
        }
        lfo.ticks_until_phase--;
        return lfo.output;
    default:
        lfo.phase_flags = 0;
        lfo.type = 0;
        return 0;
    }
}

uint16_t Driver::State::StepLfos(LfoBank& bank)
{
    uint16_t flags = 0;
    bank.outputs = {};
    for (int i = 0; i < bank.count; i++)
    {
        Lfo& lfo = bank.lfos[bank.list[static_cast<std::size_t>(i)]];
        if (lfo.delay != 0)
        {
            lfo.delay--;
            continue;
        }
        const int32_t output = Waveform(lfo) >> 8;
        if (lfo.envelope_ticks != 0)
        {
            lfo.envelope_ticks--;
            if (lfo.envelope_ticks != 0)
            {
                lfo.envelope_level = Wrap(int64_t{lfo.envelope_level} + lfo.envelope_delta);
            }
            else
            {
                lfo.envelope_level = 0x1000000;
            }
        }
        const uint32_t level = static_cast<uint32_t>(output) * (static_cast<uint32_t>(lfo.envelope_level) >> 8);
        if (lfo.output_index < bank.outputs.size())
        {
            // The oldest build writes the output in place of adding to it (0x020C5624).
            int16_t& out = bank.outputs[lfo.output_index];
            const int16_t before = build == Build::kOldest ? int16_t{0} : out;
            out = static_cast<int16_t>(before + (static_cast<int32_t>(level) >> 16));
        }
        flags |= lfo.update_flags;
    }
    return flags;
}

} // namespace ntdlib::dse
