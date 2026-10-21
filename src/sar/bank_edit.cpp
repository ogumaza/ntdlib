// SPDX-License-Identifier: MIT

#include "ntdlib/sar/bank_edit.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
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
constexpr uint16_t kNoRegionReference = 0x5903; // the tools give a reference to no instrument this type too
constexpr uint16_t kDirectTable = 0x6000;
constexpr uint16_t kRangeTable = 0x6001;
constexpr uint16_t kIndexTable = 0x6002;
constexpr int32_t kNoOffset = -1;
constexpr uint32_t kLastProgram = 0xFFFF;

// The options of a velocity region that ntdlib reads: the original key, the volume, the pan, the pitch, the flags and
// the envelope. Each set bit has a value of 4 bytes, in the order of the bits.
constexpr uint32_t kRegionOptions = 0x21F;
constexpr int kEnvelopeOption = 9;
constexpr std::size_t kEnvelopeBytes = 5;

// The boundary that each structure of INFO starts on.
constexpr std::size_t kAlignment = 4;

using detail::Out;

// The kinds of an instrument's structures, for the record of the structures that a walk has reached.
enum Kind
{
    kInstrument,
    kKeyTable,
    kKeyRegion,
    kVelocityTable,
    kVelocityRegion,
};

// A bank file: a reader in its byte order, the place of its INFO block, the entry of the table of blocks for INFO and
// the places of its two tables. Each table starts with its count.
struct BankFile
{
    xsflib::Reader r;
    std::size_t info = 0;
    std::size_t info_entry = 0;
    std::size_t waves = 0;
    std::size_t instruments = 0;
};

// Opens a bank file of `flavor`. Throws std::invalid_argument for a bank file of another flavor. Throws
// xsflib::FormatError for other bytes, for an INFO block past the file's end and for an unknown reference of INFO's
// body.
BankFile Open(std::span<const uint8_t> file, Flavor flavor)
{
    if (file.size() < 6 || (file[0] != 'C' && file[0] != 'F') || std::memcmp(file.data() + 1, "BNK", 3) != 0 ||
        !((file[4] == 0xFF && file[5] == 0xFE) || (file[4] == 0xFE && file[5] == 0xFF)))
    {
        throw xsflib::FormatError("not a bank file (CBNK or FBNK)");
    }
    const bool big_endian = flavor == Flavor::kCafe;
    if (file[0] != (flavor == Flavor::kCtr ? 'C' : 'F') || (file[4] == 0xFE) != big_endian)
    {
        throw std::invalid_argument("a bank file of another console");
    }

    BankFile bank{xsflib::Reader(file, big_endian)};
    for (uint16_t i = 0; i < bank.r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        if (bank.r.U16(entry) == kInfoBlock)
        {
            bank.info_entry = entry;
            bank.info = bank.r.U32(entry + 4);
        }
    }
    if (bank.info_entry == 0)
    {
        throw xsflib::FormatError("a bank file without an INFO block");
    }
    bank.r.Sub(bank.info, bank.r.U32(bank.info_entry + 8));

    const std::size_t body = bank.info + 8;
    for (std::size_t i = 0; i < 2; ++i)
    {
        const std::size_t reference = body + i * 8;
        const std::size_t table = body + static_cast<std::size_t>(bank.r.S32(reference + 4));
        switch (bank.r.U16(reference))
        {
        case kWaveTableReference:
            bank.waves = table;
            break;

        case kInstrumentTableReference:
            bank.instruments = table;
            break;

        default:
            throw xsflib::FormatError("a bank's INFO block with an unknown reference");
        }
    }

    return bank;
}

// The copies of `copies`, by their programs in `bank`. Throws std::invalid_argument for a program past 0xFFFF and for
// a program that the bank or another copy takes already.
std::map<uint32_t, const InstrumentCopy*> Programs(const BankFile& bank, std::span<const InstrumentCopy> copies)
{
    const uint32_t count = bank.r.U32(bank.instruments);
    std::map<uint32_t, const InstrumentCopy*> programs;
    for (const InstrumentCopy& copy : copies)
    {
        if (copy.to > kLastProgram)
        {
            throw std::invalid_argument("program " + std::to_string(copy.to) + " lies past 0xFFFF");
        }
        if ((copy.to < count && bank.r.U16(bank.instruments + 4 + std::size_t{copy.to} * 8) == kInstrumentReference) ||
            !programs.emplace(copy.to, &copy).second)
        {
            throw std::invalid_argument("program " + std::to_string(copy.to) + " holds an instrument already");
        }
    }

    return programs;
}

// The place of the instrument at `program` of `bank`. Throws std::invalid_argument for a program without an instrument.
std::size_t InstrumentAt(const BankFile& bank, uint32_t program)
{
    const std::size_t entry = bank.instruments + 4 + std::size_t{program} * 8;
    if (program >= bank.r.U32(bank.instruments) || bank.r.U16(entry) != kInstrumentReference)
    {
        throw std::invalid_argument("program " + std::to_string(program) + " of a source holds no instrument");
    }

    return bank.instruments + static_cast<std::size_t>(bank.r.S32(entry + 4));
}

// A region table at the place that the reference at `owner` gives: where it starts, the place of its first reference
// from its start, and its count of references. A direct table holds one reference. A range table holds a count, the
// last key or velocity of each region in a byte, padded to 4 bytes, then the references. An index table holds its first
// and last key and 2 more bytes, then a reference for each key from the first to the last.
struct RegionTable
{
    std::size_t Size() const
    {
        return first + count * 8;
    }

    std::size_t at = 0;
    std::size_t first = 0;
    std::size_t count = 1;
};

// Throws xsflib::FormatError for a table of another type.
RegionTable TableAt(const xsflib::Reader& r, std::size_t owner)
{
    RegionTable table;
    table.at = owner + static_cast<std::size_t>(r.S32(owner + 4));
    switch (r.U16(owner))
    {
    case kDirectTable:
        break;

    case kRangeTable:
        table.count = r.U32(table.at);
        table.first = 4 + (table.count + 3) / 4 * 4;
        break;

    case kIndexTable:
        table.count = static_cast<std::size_t>(std::max(0, r.U8(table.at + 1) - r.U8(table.at) + 1));
        table.first = 4;
        break;

    default:
        throw xsflib::FormatError("a region table of an unknown type");
    }

    return table;
}

// The region that the reference at `at` names, from `table`. A reference of kNoRegionReference or without an offset
// names none.
std::optional<std::size_t> RegionAt(const xsflib::Reader& r, std::size_t table, std::size_t at)
{
    const int32_t offset = r.S32(at + 4);
    if (r.U16(at) == kNoRegionReference || offset == kNoOffset)
    {
        return std::nullopt;
    }

    return table + static_cast<std::size_t>(offset);
}

// The size of the velocity region at `at` with the values of its options, and the place of its envelope's value from
// its start, if it has an envelope. Throws xsflib::FormatError for an unknown option.
std::pair<std::size_t, std::optional<std::size_t>> RegionSize(const xsflib::Reader& r, std::size_t at)
{
    const uint32_t bits = r.U32(at + 4);
    if ((bits & ~kRegionOptions) != 0)
    {
        throw xsflib::FormatError("a velocity region with unknown options");
    }

    std::optional<std::size_t> envelope;
    if ((bits >> kEnvelopeOption & 1) != 0)
    {
        envelope = 8 + 4 * static_cast<std::size_t>(std::popcount(bits & ((1u << kEnvelopeOption) - 1)));
    }

    return {8 + 4 * static_cast<std::size_t>(std::popcount(bits)), envelope};
}

// A field of 4 bytes that holds an offset: its place, and the places that the offset leads from and to.
struct Offset
{
    std::size_t at = 0;
    std::size_t base = 0;
    std::size_t target = 0;
};

// The fields of a bank's INFO block that hold offsets: the references to the two tables, those of the table of
// instruments, and those inside the instruments. Also the end of the instruments' last structure.
class Offsets
{
public:
    explicit Offsets(const BankFile& bank) : r_(bank.r)
    {
        const std::size_t body = bank.info + 8;
        fields_.push_back({body + 4, body, body + static_cast<std::size_t>(r_.S32(body + 4))});
        fields_.push_back({body + 12, body, body + static_cast<std::size_t>(r_.S32(body + 12))});

        const uint32_t count = r_.U32(bank.instruments);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::size_t entry = bank.instruments + 4 + std::size_t{i} * 8;
            if (r_.U16(entry) == kInstrumentReference)
            {
                const std::size_t instrument = bank.instruments + static_cast<std::size_t>(r_.S32(entry + 4));
                fields_.push_back({entry + 4, bank.instruments, instrument});
                Instrument(instrument);
            }
        }
    }

    const std::vector<Offset>& Fields() const
    {
        return fields_;
    }

    std::size_t End() const
    {
        return end_;
    }

private:
    // Whether the walk reaches the structure of `kind` at `at` for the first time. Moves the end past its `size` bytes.
    bool First(Kind kind, std::size_t at, std::size_t size)
    {
        end_ = std::max(end_, at + size);

        return walked_.insert({kind, at}).second;
    }

    void Instrument(std::size_t at)
    {
        if (First(kInstrument, at, 8))
        {
            Table(kKeyTable, at, &Offsets::KeyRegion);
        }
    }

    // The region table that the reference at `owner` gives, and each region that it names.
    void Table(Kind kind, std::size_t owner, void (Offsets::*region)(std::size_t))
    {
        const RegionTable table = TableAt(r_, owner);
        fields_.push_back({owner + 4, owner, table.at});
        if (!First(kind, table.at, table.Size()))
        {
            return;
        }

        for (std::size_t i = 0; i < table.count; ++i)
        {
            const std::size_t reference = table.first + i * 8;
            if (const std::optional<std::size_t> at = RegionAt(r_, table.at, table.at + reference))
            {
                fields_.push_back({table.at + reference + 4, table.at, *at});
                (this->*region)(*at);
            }
        }
    }

    void KeyRegion(std::size_t at)
    {
        if (First(kKeyRegion, at, 8))
        {
            Table(kVelocityTable, at, &Offsets::VelocityRegion);
        }
    }

    void VelocityRegion(std::size_t at)
    {
        const auto [size, envelope] = RegionSize(r_, at);
        if (!First(kVelocityRegion, at, size) || !envelope)
        {
            return;
        }

        const std::size_t reference = at + r_.U32(at + *envelope);
        const std::size_t bytes = reference + static_cast<std::size_t>(r_.S32(reference + 4));
        fields_.push_back({at + *envelope, at, reference});
        fields_.push_back({reference + 4, reference, bytes});
        end_ = std::max({end_, reference + 8, bytes + kEnvelopeBytes});
    }

    const xsflib::Reader& r_;
    std::vector<Offset> fields_;
    std::size_t end_ = 0;
    std::set<std::pair<Kind, std::size_t>> walked_;
};

// A bank's table of waves, and the entries that the copies add after its last.
class WaveTable
{
public:
    explicit WaveTable(const BankFile& bank) : count_(bank.r.U32(bank.waves))
    {
        for (uint32_t i = 0; i < count_; ++i)
        {
            const std::size_t entry = bank.waves + 4 + std::size_t{i} * 8;
            indices_.emplace(std::pair(bank.r.U32(entry), bank.r.U32(entry + 4)), i);
        }
    }

    // The index in the table of the wave that entry `index` of the table of waves of `from` names: the first entry
    // that names the same wave archive and wave, or else a new one. Throws xsflib::FormatError for an index past the
    // table of `from`.
    uint32_t IndexOf(const BankFile& from, uint32_t index)
    {
        if (index >= from.r.U32(from.waves))
        {
            throw xsflib::FormatError("a velocity region whose wave lies past the table of waves");
        }

        const std::size_t entry = from.waves + 4 + std::size_t{index} * 8;
        const std::pair wave(from.r.U32(entry), from.r.U32(entry + 4));
        const auto [found, is_new] = indices_.emplace(wave, count_ + static_cast<uint32_t>(added_.size()));
        if (is_new)
        {
            added_.push_back(wave);
        }

        return found->second;
    }

    uint32_t Count() const
    {
        return count_;
    }

    // The new entries: a wave archive's item ID and a wave's index.
    const std::vector<std::pair<uint32_t, uint32_t>>& Added() const
    {
        return added_;
    }

private:
    uint32_t count_ = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> indices_;
    std::vector<std::pair<uint32_t, uint32_t>> added_;
};

// Instruments laid out one after another in the layout of NintendoWare's tools (see bank_edit.h). Each place counts
// from the start of the bytes.
class Layout
{
public:
    Layout(bool big_endian, WaveTable& waves) : bytes_(big_endian), waves_(waves)
    {
    }

    // Lays out the instrument at `at` of `from`. Returns its place.
    std::size_t Instrument(const BankFile& from, std::size_t at)
    {
        key_regions_.clear();
        const std::size_t place = Copy(from, at, 8);
        Table(from, at, place, &Layout::KeyRegion);

        return place;
    }

    std::vector<uint8_t> Take()
    {
        return bytes_.Take();
    }

private:
    // Appends `size` bytes of `from` at `at`, and returns their place.
    std::size_t Copy(const BankFile& from, std::size_t at, std::size_t size)
    {
        return bytes_.Append(from.r.Sub(at, size));
    }

    // Lays out the region table that the reference at `owner` gives, and each region that it names. The reference's
    // copy lies at `owner_place`.
    void Table(const BankFile& from, std::size_t owner, std::size_t owner_place,
               std::size_t (Layout::*region)(const BankFile&, std::size_t))
    {
        const RegionTable table = TableAt(from.r, owner);
        const std::size_t place = Copy(from, table.at, table.Size());
        bytes_.U32(owner_place + 4, static_cast<uint32_t>(place - owner_place));
        for (std::size_t i = 0; i < table.count; ++i)
        {
            const std::size_t reference = table.first + i * 8;
            if (const std::optional<std::size_t> at = RegionAt(from.r, table.at, table.at + reference))
            {
                const std::size_t region_place = (this->*region)(from, *at);
                bytes_.U32(place + reference + 4, static_cast<uint32_t>(region_place - place));
            }
        }
    }

    // A key region that the table names again keeps the place where it went in first.
    std::size_t KeyRegion(const BankFile& from, std::size_t at)
    {
        const auto [laid, first] = key_regions_.emplace(at, bytes_.Size());
        if (first)
        {
            Copy(from, at, 8);
            Table(from, at, laid->second, &Layout::VelocityRegion);
        }

        return laid->second;
    }

    std::size_t VelocityRegion(const BankFile& from, std::size_t at)
    {
        const auto [size, envelope] = RegionSize(from.r, at);
        const std::size_t place = Copy(from, at, size);
        bytes_.U32(place, waves_.IndexOf(from, from.r.U32(at)));
        if (envelope)
        {
            const std::size_t reference = at + from.r.U32(at + *envelope);
            const std::size_t reference_place = Copy(from, reference, 8);
            const std::size_t bytes = reference + static_cast<std::size_t>(from.r.S32(reference + 4));
            const std::size_t bytes_place = Copy(from, bytes, kEnvelopeBytes);
            bytes_.Align(kAlignment);
            bytes_.U32(place + *envelope, static_cast<uint32_t>(reference_place - place));
            bytes_.U32(reference_place + 4, static_cast<uint32_t>(bytes_place - reference_place));
        }

        return place;
    }

    Out bytes_;
    WaveTable& waves_;
    std::map<std::size_t, std::size_t> key_regions_;
};

// Bytes that go in at a place of the bank.
struct Insertion
{
    std::size_t at = 0;
    std::vector<uint8_t> bytes;
};

// Where a place of the bank lies after the insertions: past every insertion at or before it.
std::size_t Moved(const std::vector<Insertion>& insertions, std::size_t at)
{
    std::size_t place = at;
    for (const Insertion& insertion : insertions)
    {
        place += insertion.at <= at ? insertion.bytes.size() : 0;
    }

    return place;
}

} // namespace

std::vector<uint8_t> WithInstruments(std::span<const uint8_t> bank, Flavor flavor,
                                     std::span<const InstrumentCopy> copies)
{
    const BankFile target = Open(bank, flavor);
    if (copies.empty())
    {
        return {bank.begin(), bank.end()};
    }

    // The copies, laid out in the order of their programs.
    const std::map<uint32_t, const InstrumentCopy*> programs = Programs(target, copies);
    const Offsets offsets(target);
    WaveTable waves(target);
    Layout layout(target.r.BigEndian(), waves);
    std::vector<std::pair<uint32_t, std::size_t>> places;
    for (const auto& [program, copy] : programs)
    {
        const BankFile from = Open(copy->from, flavor);
        places.emplace_back(program, layout.Instrument(from, InstrumentAt(from, copy->program)));
    }

    // What goes in: the new waves after the table of waves, the new entries after the table of instruments, and the
    // copies after the last instrument. Each place lies inside INFO's body.
    const xsflib::Reader& r = target.r;
    const uint32_t count = r.U32(target.instruments);
    const uint32_t new_count = std::max(count, programs.rbegin()->first + 1);
    const std::size_t info_size = r.U32(target.info_entry + 8);
    std::vector<Insertion> insertions(3);
    insertions[0].at = target.waves + 4 + std::size_t{waves.Count()} * 8;
    insertions[1].at = target.instruments + 4 + std::size_t{count} * 8;
    insertions[2].at = std::max(insertions[1].at, (offsets.End() + kAlignment - 1) / kAlignment * kAlignment);
    for (const Insertion& insertion : insertions)
    {
        if (insertion.at < target.info + 8 || insertion.at > target.info + info_size)
        {
            throw xsflib::FormatError("a bank whose tables or instruments lie outside its INFO block");
        }
    }

    Out new_waves(r.BigEndian());
    for (const auto& [wave_archive, index] : waves.Added())
    {
        const std::size_t at = new_waves.Zeros(8);
        new_waves.U32(at, wave_archive);
        new_waves.U32(at + 4, index);
    }
    Out new_entries(r.BigEndian());
    for (uint32_t i = count; i < new_count; ++i)
    {
        const std::size_t at = new_entries.Zeros(8);
        new_entries.U16(at, kNoRegionReference);
        new_entries.U32(at + 4, static_cast<uint32_t>(kNoOffset));
    }
    insertions[0].bytes = new_waves.Take();
    insertions[1].bytes = new_entries.Take();
    insertions[2].bytes = layout.Take();
    const std::size_t copies_at = insertions[2].at;
    const std::size_t copies_size = insertions[2].bytes.size();
    std::ranges::stable_sort(insertions, {}, &Insertion::at);

    // The bank with the insertions. Of the insertions at one place, the copies go in last.
    Out out(r.BigEndian());
    std::size_t copied = 0;
    for (const Insertion& insertion : insertions)
    {
        out.Append(bank.subspan(copied, insertion.at - copied));
        out.Append(insertion.bytes);
        copied = insertion.at;
    }
    out.Append(bank.subspan(copied));

    // The offsets, the counts, the copies' entries and the sizes.
    for (const Offset& offset : offsets.Fields())
    {
        const std::size_t value = Moved(insertions, offset.target) - Moved(insertions, offset.base);
        out.U32(Moved(insertions, offset.at), static_cast<uint32_t>(value));
    }
    const std::size_t table = Moved(insertions, target.instruments);
    const std::size_t copies_place = Moved(insertions, copies_at) - copies_size;
    out.U32(Moved(insertions, target.waves), waves.Count() + static_cast<uint32_t>(waves.Added().size()));
    out.U32(table, new_count);
    for (const auto& [program, place] : places)
    {
        const std::size_t entry = table + 4 + std::size_t{program} * 8;
        out.U16(entry, kInstrumentReference);
        out.U32(entry + 4, static_cast<uint32_t>(copies_place + place - table));
    }
    const std::size_t grown = out.Size() - bank.size();
    out.U32(0x0C, r.U32(0x0C) + static_cast<uint32_t>(grown));
    out.U32(target.info + 4, r.U32(target.info + 4) + static_cast<uint32_t>(grown));
    out.U32(target.info_entry + 8, static_cast<uint32_t>(info_size + grown));
    for (uint16_t i = 0; i < r.U16(0x10); ++i)
    {
        const std::size_t entry = 0x14 + std::size_t{i} * 12;
        out.U32(entry + 4, static_cast<uint32_t>(Moved(insertions, r.U32(entry + 4))));
    }

    return out.Take();
}

} // namespace ntdlib::sar
