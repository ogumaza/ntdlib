// SPDX-License-Identifier: MIT

#include "ntdlib/quintet/instruments.h"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "ntdlib/agb/rom.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/quintet/song.h"

namespace ntdlib::quintet
{
namespace
{

// The bytes of a sample's header: a word of its size, then a halfword of its rate.
constexpr uint32_t kSampleHeader = 6;

} // namespace

Instrument LookUpInstrument(const Rom& rom, const DriverInfo& info, int channel, int number, int bank)
{
    if (channel < 0 || channel >= kChannels)
    {
        throw std::out_of_range("no channel " + std::to_string(channel));
    }

    Instrument instrument;
    instrument.number = number;
    switch (channel)
    {
    case kWave:
        instrument.kind = Instrument::Kind::kWave;
        instrument.address = info.waves ? info.waves + uint32_t(number & 0x7F) * 16 + 2 : 0;
        break;

    case kNoise:
        {
            // The table's first halfword comes before the offsets. An offset counts from the table's start.
            instrument.kind = Instrument::Kind::kNoise;
            instrument.bank = bank & 1;
            const uint32_t table = info.macros[size_t(bank & 1)];
            const uint32_t entry = table + 2 * uint32_t(number & 0xFF) + 2;
            instrument.address = table && number != 0 && rom.Contains(entry, 2) ? table + rom.U16(entry) : 0;
            break;
        }

    case kPcmA:
    case kPcmB:
        {
            instrument.kind = Instrument::Kind::kPcm;
            instrument.bank = bank & 1;
            const auto& list = info.sample_addresses[size_t(bank & 1)];
            instrument.address = number >= 0 && size_t(number) < list.size() ? list[size_t(number)] : 0;
            break;
        }

    default:
        instrument.kind = Instrument::Kind::kSquare;
        break;
    }

    return instrument;
}

std::optional<std::array<uint8_t, 16>> ReadWave(const Rom& rom, const DriverInfo& info, int wave)
{
    const uint32_t data = info.waves + uint32_t(wave & 0x7F) * 16 + 2;
    if (!info.waves || !rom.Contains(data, 16))
    {
        return std::nullopt;
    }

    std::array<uint8_t, 16> bytes = {};
    for (uint32_t i = 0; i < 16; i++)
    {
        bytes[i] = rom.U8(data + i);
    }

    return bytes;
}

std::optional<Sample> ReadSample(const Rom& rom, const DriverInfo& info, int bank, int number)
{
    const auto& list = info.sample_addresses[size_t(bank & 1)];
    if (number < 0 || size_t(number) >= list.size() || !rom.Contains(list[size_t(number)], kSampleHeader))
    {
        return std::nullopt;
    }

    // The driver plays the size with the header, less the revision's trim, less 0.6%.
    Sample sample;
    sample.header = list[size_t(number)];
    sample.data = sample.header + kSampleHeader;
    const uint32_t size = rom.U32(sample.header);
    sample.size = size > kSampleHeader ? size - kSampleHeader : 0;
    const uint32_t trim = TraitsOf(info.revision).sample_trim;
    sample.played = size > trim ? (size - trim) * 994 / 1000 : 0;
    sample.rate = rom.U16(sample.header + 4);

    return sample;
}

} // namespace ntdlib::quintet
