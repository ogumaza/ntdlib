// SPDX-License-Identifier: MIT

#include "ntdlib/jaudio/bx.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"

#include "jaudio/bank_parse.h"
#include "ntdlib/jaudio/bank.h"
#include "ntdlib/jaudio/wave_system.h"

namespace ntdlib::jaudio
{
namespace
{

// The header: the offset and the number of entries of the wave systems' table, then of the banks' table. An entry
// takes 8 bytes: a block's offset and its size.
constexpr std::size_t kSystemTable = 0;
constexpr std::size_t kBankTable = 8;
constexpr std::size_t kEntryBytes = 8;

// A wave id whose high half is this plays from the system at the bank's place in the table.
constexpr uint16_t kBankPlace = 0xffff;

// The game maps the numbers of wave systems and banks up to this.
constexpr uint32_t kLastNumber = 0xff;

// Every read past the end of the file gives one message.
constexpr xsflib::ReaderOptions kBxReaderOptions{.bounds_message = xsflib::BoundsMessage::kFixed,
                                                 .fixed_message = "the bank file is damaged"};

// A reader of the file in its byte order: little-endian if its first word, the offset of the wave systems' table, is
// smaller read that way.
xsflib::Reader BxReader(std::span<const uint8_t> bx)
{
    const bool big_endian = bx.size() < 4 || xsflib::LoadBe32(bx.data()) <= xsflib::LoadLe32(bx.data());
    return xsflib::Reader(bx, big_endian, kBxReaderOptions);
}

// A block of a table: its place in the table, and its offset in the file.
struct Block
{
    uint32_t place = 0;
    uint32_t offset = 0;
};

// The blocks of the table whose offset and number of entries the header gives at `header`, in the table's order. An
// entry of size 0 is an empty place. The game skips it.
std::vector<Block> Blocks(const xsflib::Reader& r, std::size_t header)
{
    const uint32_t table = r.U32(header);
    const uint32_t entries = r.U32(header + 4);
    std::vector<Block> blocks;
    for (uint32_t place = 0; place < entries; place++)
    {
        const std::size_t entry = table + std::size_t{place} * kEntryBytes;
        if (r.U32(entry + 4) != 0)
        {
            blocks.push_back({place, r.U32(entry)});
        }
    }

    return blocks;
}

// The place of the wave system that `named`, a wave id's high half, names for a bank at `bank_place`. `places` maps
// the systems' numbers to their places. Returns nothing if no system gives the number.
std::optional<uint32_t> NamedPlace(const std::map<uint32_t, uint32_t>& places, uint16_t named, uint32_t bank_place)
{
    if (named == kBankPlace)
    {
        return bank_place;
    }

    const auto found = places.find(named);
    if (found == places.end())
    {
        return std::nullopt;
    }

    return found->second;
}

} // namespace

std::vector<WaveSystem> ReadBxWaveSystems(std::span<const uint8_t> bx)
{
    const xsflib::Reader r = BxReader(bx);
    const std::size_t wave_count_at = r.BigEndian() ? kWaveCountAt : kWideWaveCountAt;
    std::vector<WaveSystem> systems;
    for (const Block& block : Blocks(r, kSystemTable))
    {
        r.Sub(block.offset, 1);
        systems.push_back(ReadWaveSystem(bx.subspan(block.offset), block.place, wave_count_at));
    }

    return systems;
}

std::vector<Bank> ReadBxBanks(std::span<const uint8_t> bx, std::vector<std::string>& warnings)
{
    const xsflib::Reader r = BxReader(bx);

    // Each wave system's number, from its header, maps to its place in the table. The first system that gives a
    // number takes it.
    std::map<uint32_t, uint32_t> places;
    std::set<uint32_t> held;
    for (const Block& block : Blocks(r, kSystemTable))
    {
        held.insert(block.place);
        const uint32_t number = r.U32(std::size_t{block.offset} + 8);
        if (number <= kLastNumber)
        {
            places.try_emplace(number, block.place);
        }
    }

    std::vector<Bank> banks;
    for (const Block& block : Blocks(r, kBankTable))
    {
        try
        {
            r.Sub(block.offset, 1);
            detail::ParsedBank parsed = detail::ParseBank(bx.subspan(block.offset), block.place);

            // The places of the systems that the bank's waves name.
            std::set<uint32_t> systems;
            for (const uint16_t named : parsed.wave_systems)
            {
                const std::optional<uint32_t> place = NamedPlace(places, named, block.place);
                if (!place || !held.contains(*place))
                {
                    throw xsflib::FormatError("bank plays a wave system that the file lacks");
                }

                systems.insert(*place);
            }

            if (systems.size() > 1)
            {
                throw xsflib::FormatError("bank plays the waves of more than one wave system");
            }

            if (!systems.empty())
            {
                parsed.bank.wave_system = *systems.begin();
            }

            banks.push_back(std::move(parsed.bank));
        }
        catch (const xsflib::FormatError& e)
        {
            warnings.push_back("bank at offset " + std::to_string(block.offset) + ": " + e.what());
        }
    }

    return banks;
}

} // namespace ntdlib::jaudio
