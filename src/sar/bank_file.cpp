// SPDX-License-Identifier: MIT

#include "ntdlib/sar/bank_file.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "ntdlib/sar/formats.h"
#include "sar/file_bytes.h"

namespace ntdlib::sar
{
namespace
{

constexpr uint16_t kInfoBlock = 0x5800;
constexpr uint16_t kWaveTableReference = 0x0100;       // from INFO to the table of waves
constexpr uint16_t kInstrumentTableReference = 0x0101; // from INFO to the table of instruments
constexpr uint16_t kInstrumentReference = 0x5900;
constexpr uint16_t kKeyRegionReference = 0x5901;
constexpr uint16_t kVelocityRegionReference = 0x5902;
constexpr uint16_t kNoItemReference = 0x5903;
constexpr uint16_t kEnvelopeReference = 0x0000;
constexpr uint16_t kDirectTable = 0x6000;
constexpr uint16_t kRangeTable = 0x6001;
constexpr uint16_t kIndexTable = 0x6002;
constexpr uint32_t kNoOffset = 0xFFFFFFFF;
constexpr std::size_t kHeaderSize = 0x20;
constexpr std::size_t kEnvelopeBytes = 5;
constexpr std::size_t kAlignment = 4;
constexpr int kLastValue = 127;

// The most runs that a range table holds, not counting the gap after the last region.
constexpr std::size_t kMostRangeRuns = 11;

// The options of a velocity region: the original key, the volume, the pan, the pitch, the flags and the envelope. Each
// has a value of 4 bytes, in the order of the bits. The flags hold whether the region ignores a note-off, its key group
// and its interpolation.
constexpr uint32_t kRegionOptions = 0x21F;

using detail::Out;

// A run of keys or velocities that a table names one item for: the item's place, and the run's first and last value.
struct Run
{
    std::size_t place = 0;
    int first = 0;
    int last = 0;
};

// The runs of the table that the reference at `owner` leads to, in the order of their values. A run lies at the entries
// that name one item, one after another. Throws xsflib::FormatError for a table of an unknown kind, and for an item
// that two runs name.
std::vector<Run> RunsOf(const xsflib::Reader& r, std::size_t owner)
{
    const std::size_t table = owner + static_cast<std::size_t>(r.S32(owner + 4));
    std::size_t references = table;
    std::size_t count = 1;
    switch (r.U16(owner))
    {
    case kDirectTable:
        break;

    case kRangeTable:
        count = r.U32(table);
        references = table + 4 + (count + 3) / 4 * 4;
        break;

    case kIndexTable:
        count = static_cast<std::size_t>(std::max(0, r.U8(table + 1) - r.U8(table) + 1));
        references = table + 4;
        break;

    default:
        throw xsflib::FormatError("a table of regions of an unknown kind");
    }

    // Each entry's values, and the item that it names.
    std::vector<Run> runs;
    std::set<std::size_t> named;
    int next = r.U16(owner) == kIndexTable ? r.U8(table) : 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const int first = next;
        const int last = r.U16(owner) == kRangeTable    ? r.U8(table + 4 + i)
                         : r.U16(owner) == kDirectTable ? kLastValue
                                                        : first;
        next = last + 1;
        const std::size_t reference = references + i * 8;
        const int32_t offset = r.S32(reference + 4);
        if (r.U16(reference) == kNoItemReference || offset == -1)
        {
            continue;
        }

        const std::size_t place = table + static_cast<std::size_t>(offset);
        if (!runs.empty() && runs.back().place == place && runs.back().last + 1 == first)
        {
            runs.back().last = last;
        }
        else if (named.insert(place).second)
        {
            runs.push_back({place, first, last});
        }
        else
        {
            throw xsflib::FormatError("a region over runs of keys or velocities that don't follow one another");
        }
    }

    return runs;
}

// Holds a run's first and last value in bytes. Throws xsflib::FormatError for a run past 255.
std::pair<uint8_t, uint8_t> RunBytes(const Run& run)
{
    if (run.first > 0xFF || run.last > 0xFF || run.last < run.first)
    {
        throw xsflib::FormatError("a table of regions with a run past 255");
    }

    return {static_cast<uint8_t>(run.first), static_cast<uint8_t>(run.last)};
}

// The values of the velocity region at `at`. Throws xsflib::FormatError for a region without all six values.
VelocityRegion ReadValues(const xsflib::Reader& r, std::size_t at)
{
    if (r.U32(at + 4) != kRegionOptions)
    {
        throw xsflib::FormatError("a velocity region without all six values");
    }

    const uint32_t flags = r.U32(at + 24);
    const std::size_t envelope = at + r.U32(at + 28);
    const std::size_t bytes = envelope + static_cast<std::size_t>(r.S32(envelope + 4));
    VelocityRegion values;
    values.wave_id_index = r.U32(at);
    values.original_key = static_cast<uint8_t>(r.U32(at + 8));
    values.volume = static_cast<uint8_t>(r.U32(at + 12));
    values.pan = static_cast<uint8_t>(r.U32(at + 16));
    values.pitch = std::bit_cast<float>(r.U32(at + 20));
    values.ignore_note_off = (flags & 0xFF) != 0;
    values.key_group = static_cast<uint8_t>(flags >> 8);
    values.interpolation = static_cast<uint8_t>(flags >> 16);
    values.adshr = Adshr{r.U8(bytes), r.U8(bytes + 1), r.U8(bytes + 2), r.U8(bytes + 3), r.U8(bytes + 4)};

    return values;
}

// The instrument at `at`.
BankFile::Instrument ReadInstrument(const xsflib::Reader& r, std::size_t at)
{
    BankFile::Instrument instrument;
    for (const Run& key_run : RunsOf(r, at))
    {
        BankFile::KeyRegion key_region;
        std::tie(key_region.first_key, key_region.last_key) = RunBytes(key_run);
        for (const Run& velocity_run : RunsOf(r, key_run.place))
        {
            BankFile::Region region;
            std::tie(region.first_velocity, region.last_velocity) = RunBytes(velocity_run);
            region.values = ReadValues(r, velocity_run.place);
            key_region.regions.push_back(region);
        }
        instrument.key_regions.push_back(std::move(key_region));
    }

    return instrument;
}

// The bytes of a bank file of `flavor`, laid out as NintendoWare's tools lay a bank out (see bank_file.h and
// bank_edit.h).
class Writer
{
public:
    explicit Writer(Flavor flavor) : out_(flavor == Flavor::kCafe)
    {
    }

    // Lays out `instrument`. Returns its place.
    std::size_t Instrument(const BankFile::Instrument& instrument)
    {
        std::vector<std::pair<int, int>> keys;
        for (const BankFile::KeyRegion& key_region : instrument.key_regions)
        {
            keys.emplace_back(key_region.first_key, key_region.last_key);
        }
        const auto write_key_region = [this, &instrument](std::size_t i)
        {
            return KeyRegion(instrument.key_regions[i]);
        };

        const std::size_t place = out_.Zeros(8);
        Table(place, keys, kKeyRegionReference, write_key_region);

        return place;
    }

    Out& Bytes()
    {
        return out_;
    }

private:
    // An entry of a table: the last key or velocity of its run, and the region that it names, if any.
    struct Entry
    {
        int last = 0;
        std::optional<std::size_t> region;
    };

    // Lays out the table of regions over `runs`, the first and last key or velocity of each region, after the reference
    // at `owner`. Lays out each region at its first entry with `write_region`, and gives the entries' references type
    // `type`. Throws std::invalid_argument for runs that overlap, don't follow the order of their values, end before
    // they start or pass 127.
    template <typename WriteRegion>
    void Table(std::size_t owner, const std::vector<std::pair<int, int>>& runs, uint16_t type, WriteRegion write_region)
    {
        // The runs from 0 to the last region's last: each region, and the gaps before them.
        std::vector<Entry> entries;
        int next = 0;
        for (std::size_t i = 0; i < runs.size(); ++i)
        {
            const auto [first, last] = runs[i];
            if (first < next || last < first || last > kLastValue)
            {
                throw std::invalid_argument(
                    "regions over runs of keys or velocities that overlap, run backwards or "
                    "pass 127");
            }
            if (first > next)
            {
                entries.push_back({first - 1, std::nullopt});
            }
            entries.push_back({last, i});
            next = last + 1;
        }

        // The table: its reference, its kind's fields and room for its references.
        std::size_t at = 0;
        std::size_t references = 0;
        if (runs.size() == 1 && runs[0] == std::pair(0, kLastValue))
        {
            at = references = out_.Zeros(8);
            out_.Reference(owner, kDirectTable, static_cast<uint32_t>(at - owner));
        }
        else if (entries.size() <= kMostRangeRuns)
        {
            if (next <= kLastValue)
            {
                entries.push_back({kLastValue, std::nullopt});
            }
            at = out_.Zeros(4 + (entries.size() + 3) / 4 * 4 + entries.size() * 8);
            references = at + 4 + (entries.size() + 3) / 4 * 4;
            out_.Reference(owner, kRangeTable, static_cast<uint32_t>(at - owner));
            out_.U32(at, static_cast<uint32_t>(entries.size()));
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                out_.U8(at + 4 + i, static_cast<uint8_t>(entries[i].last));
            }
        }
        else
        {
            std::vector<Entry> keys;
            for (const Entry& entry : entries)
            {
                while (static_cast<int>(keys.size()) <= entry.last)
                {
                    keys.push_back({static_cast<int>(keys.size()), entry.region});
                }
            }
            entries = std::move(keys);
            at = out_.Zeros(4 + entries.size() * 8);
            references = at + 4;
            out_.Reference(owner, kIndexTable, static_cast<uint32_t>(at - owner));
            out_.U8(at + 1, static_cast<uint8_t>(entries.size() - 1));
        }

        // The references, and each region at its first entry.
        std::vector<std::optional<std::size_t>> places(runs.size());
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const std::size_t reference = references + i * 8;
            const std::optional<std::size_t> region = entries[i].region;
            if (!region)
            {
                out_.Reference(reference, kNoItemReference, kNoOffset);
                continue;
            }

            if (!places[*region])
            {
                places[*region] = write_region(*region);
            }
            out_.Reference(reference, type, static_cast<uint32_t>(*places[*region] - at));
        }
    }

    // Lays out `key_region` and its velocity regions. Returns its place.
    std::size_t KeyRegion(const BankFile::KeyRegion& key_region)
    {
        std::vector<std::pair<int, int>> velocities;
        for (const BankFile::Region& region : key_region.regions)
        {
            velocities.emplace_back(region.first_velocity, region.last_velocity);
        }
        const auto write_region = [this, &key_region](std::size_t i)
        {
            return Region(key_region.regions[i].values);
        };

        const std::size_t place = out_.Zeros(8);
        Table(place, velocities, kVelocityRegionReference, write_region);

        return place;
    }

    // Lays out a velocity region with `values`, then the reference to its envelope and the envelope. Returns its place.
    std::size_t Region(const VelocityRegion& values)
    {
        const std::size_t place = out_.Zeros(32);
        out_.U32(place, values.wave_id_index);
        out_.U32(place + 4, kRegionOptions);
        out_.U32(place + 8, values.original_key);
        out_.U32(place + 12, values.volume);
        out_.U32(place + 16, values.pan);
        out_.U32(place + 20, std::bit_cast<uint32_t>(values.pitch));
        out_.U32(place + 24, uint32_t{values.ignore_note_off} | uint32_t{values.key_group} << 8 |
                                 uint32_t{values.interpolation} << 16);

        const std::size_t reference = out_.Zeros(8);
        const std::size_t bytes = out_.Zeros(kEnvelopeBytes);
        out_.Align(kAlignment);
        out_.U32(place + 28, static_cast<uint32_t>(reference - place));
        out_.Reference(reference, kEnvelopeReference, static_cast<uint32_t>(bytes - reference));
        out_.U8(bytes, values.adshr.attack);
        out_.U8(bytes + 1, values.adshr.decay);
        out_.U8(bytes + 2, values.adshr.sustain);
        out_.U8(bytes + 3, values.adshr.hold);
        out_.U8(bytes + 4, values.adshr.release);

        return place;
    }

    Out out_;
};

// The programs of `bank`'s instruments in the order that the file lays them out in. Throws std::invalid_argument for an
// order that doesn't name each program with an instrument once.
std::vector<uint32_t> LaidOrder(const BankFile& bank)
{
    std::vector<uint32_t> programs;
    for (std::size_t i = 0; i < bank.instruments.size(); ++i)
    {
        if (bank.instruments[i])
        {
            programs.push_back(static_cast<uint32_t>(i));
        }
    }
    if (bank.order.empty())
    {
        return programs;
    }

    std::vector<uint32_t> sorted = bank.order;
    std::ranges::sort(sorted);
    if (sorted != programs)
    {
        throw std::invalid_argument(
            "an order of the instruments that doesn't name each program with an instrument once");
    }

    return bank.order;
}

} // namespace

BankFile BankFile::Read(std::span<const uint8_t> file)
{
    if (file.size() < kHeaderSize || (file[0] != 'C' && file[0] != 'F') || std::memcmp(file.data() + 1, "BNK", 3) != 0)
    {
        throw xsflib::FormatError("not a bank file (CBNK or FBNK)");
    }
    const bool big_endian = file[4] == 0xFE && file[5] == 0xFF;
    if ((!big_endian && (file[4] != 0xFF || file[5] != 0xFE)) || (file[0] == 'C' && big_endian))
    {
        throw xsflib::FormatError("a bank file without the byte order of its console");
    }

    // The header and the places of INFO's two tables.
    const xsflib::Reader r(file, big_endian);
    BankFile bank;
    bank.flavor = file[0] == 'C' ? Flavor::kCtr : big_endian ? Flavor::kCafe : Flavor::kNx;
    bank.version = r.U32(8);
    std::optional<std::size_t> info;
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        if (r.U16(entry) == kInfoBlock)
        {
            info = r.U32(entry + 4);
        }
    }
    if (!info)
    {
        throw xsflib::FormatError("a bank file without an INFO block");
    }
    const std::size_t body = *info + 8;
    if (r.U16(body) != kWaveTableReference || r.U16(body + 8) != kInstrumentTableReference)
    {
        throw xsflib::FormatError("a bank's INFO block without its tables");
    }
    const std::size_t waves = body + static_cast<std::size_t>(r.S32(body + 4));
    const std::size_t instruments = body + static_cast<std::size_t>(r.S32(body + 12));

    // The waves, the instruments and the order of their places.
    for (uint32_t i = 0; i < r.U32(waves); ++i)
    {
        const std::size_t entry = waves + 4 + std::size_t{i} * 8;
        bank.waves.push_back({r.U32(entry), r.U32(entry + 4)});
    }
    std::vector<std::pair<std::size_t, uint32_t>> places;
    for (uint32_t i = 0; i < r.U32(instruments); ++i)
    {
        const std::size_t entry = instruments + 4 + std::size_t{i} * 8;
        if (r.U16(entry) != kInstrumentReference)
        {
            bank.instruments.emplace_back();
            continue;
        }

        const std::size_t at = instruments + static_cast<std::size_t>(r.S32(entry + 4));
        bank.instruments.emplace_back(ReadInstrument(r, at));
        places.emplace_back(at, i);
    }
    std::ranges::sort(places);
    for (const auto& [place, program] : places)
    {
        bank.order.push_back(program);
    }
    if (std::ranges::is_sorted(bank.order))
    {
        bank.order.clear();
    }

    // The tools' layout has to give the file back.
    std::vector<uint8_t> written;
    try
    {
        written = bank.Write();
    }
    catch (const std::invalid_argument& e)
    {
        throw xsflib::FormatError(std::string("a bank file that the model can't hold: ") + e.what());
    }
    const auto [ours, theirs] = std::ranges::mismatch(written, file);
    if (ours != written.end() || theirs != file.end())
    {
        throw xsflib::FormatError("a bank file that NintendoWare's tools didn't lay out: byte " +
                                  std::to_string(ours - written.begin()) + " differs");
    }

    return bank;
}

std::vector<uint8_t> BankFile::Write() const
{
    const std::vector<uint32_t> programs = LaidOrder(*this);

    // The header, with its table of one block.
    Writer writer(flavor);
    Out& out = writer.Bytes();
    const char* magic = flavor == Flavor::kCtr ? "CBNK" : "FBNK";
    out.Append(std::span(reinterpret_cast<const uint8_t*>(magic), 4));
    out.Zeros(kHeaderSize - 4);
    out.U16(4, 0xFEFF);
    out.U16(6, static_cast<uint16_t>(kHeaderSize));
    out.U32(8, version);
    out.U16(0x10, 1);

    // INFO: the references to the two tables, the table of instruments, the instruments and the table of waves.
    const std::size_t info = out.Append(std::span(reinterpret_cast<const uint8_t*>("INFO"), 4));
    out.Zeros(4);
    const std::size_t body = out.Zeros(16);
    const std::size_t table = out.Zeros(4 + instruments.size() * 8);
    out.Reference(body + 8, kInstrumentTableReference, static_cast<uint32_t>(table - body));
    out.U32(table, static_cast<uint32_t>(instruments.size()));
    for (std::size_t i = 0; i < instruments.size(); ++i)
    {
        out.Reference(table + 4 + i * 8, kNoItemReference, kNoOffset);
    }
    for (const uint32_t program : programs)
    {
        const std::size_t place = writer.Instrument(*instruments[program]);
        out.Reference(table + 4 + std::size_t{program} * 8, kInstrumentReference, static_cast<uint32_t>(place - table));
    }
    const std::size_t wave_table = out.Zeros(4 + waves.size() * 8);
    out.Reference(body, kWaveTableReference, static_cast<uint32_t>(wave_table - body));
    out.U32(wave_table, static_cast<uint32_t>(waves.size()));
    for (std::size_t i = 0; i < waves.size(); ++i)
    {
        out.U32(wave_table + 4 + i * 8, waves[i].wave_archive_item);
        out.U32(wave_table + 8 + i * 8, waves[i].index);
    }

    // The sizes.
    out.U32(info + 4, static_cast<uint32_t>(out.Size() - info));
    out.U32(0x0C, static_cast<uint32_t>(out.Size()));
    out.Reference(0x14, kInfoBlock, static_cast<uint32_t>(info));
    out.U32(0x1C, static_cast<uint32_t>(out.Size() - info));

    return out.Take();
}

} // namespace ntdlib::sar
