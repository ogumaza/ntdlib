// SPDX-License-Identifier: MIT

#include "ntdlib/krawall/instruments.h"

#include <cstdint>
#include <optional>

#include "ntdlib/agb/rom.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"

namespace ntdlib::krawall
{
namespace
{

// C-4, from C-0.
constexpr uint32_t kC4 = 48;

// The ROM that the player reads.
const Rom& Source(const Rom& rom, const DriverInfo& info)
{
    return info.unpacked ? *info.unpacked : rom;
}

// The header of sample `number` of the game's table, or 0.
uint32_t SampleHeader(const Rom& rom, const DriverInfo& info, int number)
{
    if (!info.samples || number < 0 || !rom.Contains(info.samples + 4 * uint32_t(number), 4))
    {
        return 0;
    }

    const uint32_t header = rom.U32(info.samples + 4 * uint32_t(number));
    return rom.Contains(header, kSamplePoints) ? header : 0;
}

// The address of XM instrument `instrument`, from 1, of `module`, or 0.
uint32_t InstrumentAddress(const Rom& rom, const DriverInfo& info, const ModuleInfo& module, uint16_t instrument)
{
    const uint32_t index = uint16_t(instrument - 1);
    uint32_t address = 0;
    if (module.instruments_at)
    {
        address = module.instruments_at + kInstrumentSize * index;
    }
    else if (info.instruments && rom.Contains(info.instruments + 4 * index, 4))
    {
        address = rom.U32(info.instruments + 4 * index);
    }

    return rom.Contains(address, 2 * uint32_t(kInstrumentNotes)) ? address : 0;
}

} // namespace

std::optional<Sample> ReadSample(const Rom& rom, const DriverInfo& info, int number)
{
    const Rom& source = Source(rom, info);
    const uint32_t header = SampleHeader(source, info, number);
    if (!header)
    {
        return std::nullopt;
    }

    Sample s;
    s.header = header;
    s.data = header + kSamplePoints;
    s.end = SampleEnd(source, info.build, header);
    if (s.end < s.data || !source.Contains(s.data, s.end - s.data))
    {
        return std::nullopt;
    }

    s.loop = source.U8(header + kSampleLoop);
    s.loop_length = source.U32(header + kSampleLoopLength);
    s.fine_tune = source.S8(header + kSampleFineTune);
    s.relative_note = source.S8(header + kSampleRelativeNote);
    s.volume = source.U8(header + kSampleVolume);
    s.pan = source.S8(header + kSamplePan);
    s.interpolate = source.U8(header + kSampleInterpolate) == 1;
    const uint32_t period = AmigaPeriod(source, info, kC4, s.fine_tune);
    s.rate = period ? uint32_t(kAmigaClock) / period : 0;
    return s;
}

std::optional<Instrument> ReadInstrument(const Rom& rom, const DriverInfo& info, const ModuleInfo& module,
                                         uint16_t instrument)
{
    if (instrument == 0)
    {
        return std::nullopt;
    }

    const Rom& source = Source(rom, info);
    Instrument out;
    if (!module.instruments)
    {
        if (!SampleHeader(source, info, instrument - 1))
        {
            return std::nullopt;
        }
        out.regions.push_back({1, kInstrumentNotes, instrument - 1});
        return out;
    }

    out.address = InstrumentAddress(source, info, module, instrument);
    if (!out.address)
    {
        return std::nullopt;
    }
    for (int note = 1; note <= kInstrumentNotes; note++)
    {
        const int sample = source.U16(out.address + 2 * uint32_t(note - 1));
        if (!out.regions.empty() && out.regions.back().sample == sample)
        {
            out.regions.back().last = note;
        }
        else
        {
            out.regions.push_back({note, note, sample});
        }
    }

    return out;
}

std::optional<Lookup> LookUpInstrument(const Rom& rom, const DriverInfo& info, const ModuleInfo& module,
                                       uint16_t instrument, uint8_t note)
{
    if (instrument == 0 || note == 0 || note > kInstrumentNotes)
    {
        return std::nullopt;
    }

    const Rom& source = Source(rom, info);
    Lookup lookup;
    if (module.instruments)
    {
        lookup.instrument = InstrumentAddress(source, info, module, instrument);
        if (!lookup.instrument)
        {
            return std::nullopt;
        }
        lookup.sample = source.U16(lookup.instrument + 2 * uint32_t(note - 1));
    }
    else
    {
        lookup.sample = instrument - 1;
    }

    lookup.header = SampleHeader(source, info, lookup.sample);
    if (!lookup.header)
    {
        return std::nullopt;
    }

    // The player adds the sample's relative note to the note in a byte.
    lookup.key = uint8_t(note - 1 + source.U8(lookup.header + kSampleRelativeNote));
    return lookup;
}

} // namespace ntdlib::krawall
