// SPDX-License-Identifier: MIT

// Tests of WithInstruments (ntdlib/sar/bank_edit.h), in each flavor's format: the formats of the 3DS, the Wii U and the
// Switch. Banks laid out as the tools lay them out (ntdlib/testing/sar_tools.h) take copies of instruments. The tests
// compare each result with the bank that the tools would write for the same instruments, with the copies last. The
// strict reader of formats.h reads every instrument back.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/bank_edit.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sar_tools.h"

namespace
{

namespace sar_files = ntdlib::testing::sar_files;
namespace sar_tools = ntdlib::testing::sar_tools;
using ntdlib::sar::Flavor;
using ntdlib::sar::InstrumentCopy;
using ntdlib::sar::WithInstruments;
using xsflib::testing::ErrorOf;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};

// The start of INFO's body in a bank of one block.
constexpr std::size_t kBody = 0x28;

// The readers of formats.h, stopping at the first problem.
ntdlib::sar::Options StrictOptions(Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

    return options;
}

// WithInstruments with copies given as a list.
std::vector<uint8_t> With(std::span<const uint8_t> bank, Flavor flavor, const std::vector<InstrumentCopy>& copies)
{
    return WithInstruments(bank, flavor, copies);
}

// Whether `call` throws an exception of type E.
template <typename E, typename Call>
bool Throws(Call call)
{
    return !ErrorOf<E>(call).empty();
}

uint32_t Load32(const std::vector<uint8_t>& bytes, std::size_t at, Flavor flavor)
{
    return flavor == Flavor::kCafe ? xsflib::LoadBe32(&bytes[at]) : xsflib::LoadLe32(&bytes[at]);
}

void Store16(std::vector<uint8_t>& bytes, std::size_t at, uint16_t value, Flavor flavor)
{
    flavor == Flavor::kCafe ? xsflib::StoreBe16(&bytes[at], value) : xsflib::StoreLe16(&bytes[at], value);
}

void Store32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value, Flavor flavor)
{
    flavor == Flavor::kCafe ? xsflib::StoreBe32(&bytes[at], value) : xsflib::StoreLe32(&bytes[at], value);
}

// The item ID of wave archive `index`.
uint32_t WaveArchive(uint32_t index)
{
    return ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, index);
}

// A text of the instrument at `program` of `file` that the strict reader reads: each key region's keys, and each
// velocity region's velocities, wave and values. A wave goes by its wave archive's item ID and its index there.
std::string Describe(std::span<const uint8_t> file, Flavor flavor, uint32_t program)
{
    const ntdlib::sar::Bank bank = ntdlib::sar::Bank::Parse(file, StrictOptions(flavor));
    if (program >= bank.instruments.size() || !bank.instruments[program])
    {
        return "none";
    }

    std::string text;
    for (const ntdlib::sar::KeyRegion& key : bank.instruments[program]->keys)
    {
        text += "keys " + std::to_string(key.lo) + "-" + std::to_string(key.hi) + ":";
        for (const ntdlib::sar::KeyRegion::Vel& velocity : key.velocities)
        {
            text += " velocities " + std::to_string(velocity.lo) + "-" + std::to_string(velocity.hi);
            if (velocity.region)
            {
                const ntdlib::sar::VelocityRegion& region = *velocity.region;
                const ntdlib::sar::WaveId& wave = bank.waves.at(region.wave_id_index);
                const ntdlib::sar::Adshr& adshr = region.adshr;
                char values[192];
                std::snprintf(values, sizeof(values),
                              " wave %08X/%u key %d volume %d pan %d pitch %g flags %d/%d/%d envelope %d/%d/%d/%d/%d",
                              wave.wave_archive_item, wave.index, region.original_key, region.volume, region.pan,
                              region.pitch, region.ignore_note_off ? 1 : 0, region.key_group, region.interpolation,
                              adshr.attack, adshr.decay, adshr.sustain, adshr.hold, adshr.release);
                text += values;
            }
        }
        text += "\n";
    }

    return text;
}

// A region on wave `wave` with each option that the reader reads.
sar_files::Region Full(uint32_t wave)
{
    sar_files::Region region;
    region.wave = wave;
    region.original_key = 60;
    region.volume = 100;
    region.pan = 40;
    region.pitch = 1.5f;
    region.flags = 0x00010201;
    region.adshr = std::array<uint8_t, 5>{10, 20, 30, 40, 50};

    return region;
}

// A region on wave `wave` with its volume, and with an envelope if `envelope`.
sar_files::Region Plain(uint32_t wave, int volume, bool envelope)
{
    sar_files::Region region;
    region.wave = wave;
    region.volume = volume;
    if (envelope)
    {
        region.adshr = std::array<uint8_t, 5>{1, 2, 3, 4, 5};
    }

    return region;
}

// An instrument of one region, through direct tables.
sar_files::KeyTable Direct(const sar_files::Region& region)
{
    sar_files::VelocityTable velocities;
    velocities.entries = {{127, region}};
    sar_files::KeyTable keys;
    keys.entries = {{127, velocities}};

    return keys;
}

// An instrument of range tables. The keys up to 40 play `low`, the keys up to 80 play nothing, and the keys above
// play `high` up to velocity 63 and `low` above it. The last region has no envelope.
sar_files::KeyTable Ranges(uint32_t low, uint32_t high)
{
    sar_files::VelocityTable soft;
    soft.kind = sar_files::VelocityTable::kRange;
    soft.entries = {{127, Full(low)}};
    sar_files::VelocityTable split;
    split.kind = sar_files::VelocityTable::kRange;
    split.entries = {{63, Full(high)}, {127, Plain(low, 50, false)}};
    sar_files::KeyTable keys;
    keys.kind = sar_files::KeyTable::kRange;
    keys.entries = {{40, soft}, {80, std::nullopt}, {127, split}};

    return keys;
}

// An instrument of index tables. Key 60 plays `first` at velocity 100, nothing at 101 and `second` at 102. Key 61
// plays `second` at every velocity, and key 62 plays nothing.
sar_files::KeyTable Indices(uint32_t first, uint32_t second)
{
    sar_files::VelocityTable velocities;
    velocities.kind = sar_files::VelocityTable::kIndex;
    velocities.first = 100;
    velocities.entries = {{100, Plain(first, 90, true)}, {101, std::nullopt}, {102, Full(second)}};
    sar_files::VelocityTable every;
    every.entries = {{127, Plain(second, 80, false)}};
    sar_files::KeyTable keys;
    keys.kind = sar_files::KeyTable::kIndex;
    keys.first = 60;
    keys.entries = {{60, velocities}, {61, every}, {62, std::nullopt}};

    return keys;
}

// `keys` with the waves of its regions mapped through `waves`.
sar_files::KeyTable Remapped(sar_files::KeyTable keys, const std::map<uint32_t, uint32_t>& waves)
{
    for (auto& [key, velocities] : keys.entries)
    {
        if (!velocities)
        {
            continue;
        }

        for (auto& [velocity, region] : velocities->entries)
        {
            if (region)
            {
                region->wave = waves.at(region->wave);
            }
        }
    }

    return keys;
}

// A bank with waves 0 and 1 of wave archive 0, and instruments at programs 0 and 2.
sar_files::BankSpec TargetSpec()
{
    sar_files::BankSpec spec;
    spec.waves = {{WaveArchive(0), 0}, {WaveArchive(0), 1}};
    spec.instruments = {Direct(Full(0)), std::nullopt, Ranges(1, 0)};

    return spec;
}

// A bank with wave 5 of wave archive 1, wave 1 of wave archive 0 and wave 6 of wave archive 1, and instruments at
// programs 0 to 2.
sar_files::BankSpec SourceSpec()
{
    sar_files::BankSpec spec;
    spec.waves = {{WaveArchive(1), 5}, {WaveArchive(0), 1}, {WaveArchive(1), 6}};
    spec.instruments = {Indices(0, 1), Direct(Plain(2, 70, true)), Ranges(0, 1)};

    return spec;
}

// The place of the instrument at `program` of a bank of one block.
std::size_t InstrumentPlace(const std::vector<uint8_t>& bank, Flavor flavor, uint32_t program)
{
    const std::size_t table = kBody + Load32(bank, kBody + 12, flavor);

    return table + Load32(bank, table + 4 + 8 * program + 4, flavor);
}

// The place of the velocity region of the instrument at `program`, an instrument of Direct. The instrument, its key
// table, its key region and its velocity table take 8 bytes each before it.
std::size_t DirectRegionPlace(const std::vector<uint8_t>& bank, Flavor flavor, uint32_t program)
{
    return InstrumentPlace(bank, flavor, program) + 32;
}

// The place of the region that entry `index` of the region table at `table` names.
std::size_t RegionPlace(const std::vector<uint8_t>& bank, Flavor flavor, std::size_t table, std::size_t first,
                        std::size_t index)
{
    return table + Load32(bank, table + first + 8 * index + 4, flavor);
}

// With no copies, a bank comes back unchanged, even with unknown options.
void TestNoCopies(Flavor flavor)
{
    const std::vector<uint8_t> tools = sar_tools::Bank(flavor, TargetSpec());
    const std::vector<uint8_t> other = sar_files::Bank(flavor, TargetSpec());
    std::vector<uint8_t> unknown = tools;
    Store32(unknown, DirectRegionPlace(unknown, flavor, 0) + 4, 0x23F, flavor);

    XSFLIB_CHECK(WithInstruments(tools, flavor, {}) == tools);
    XSFLIB_CHECK(WithInstruments(other, flavor, {}) == other);
    XSFLIB_CHECK(WithInstruments(unknown, flavor, {}) == unknown);
}

// Copies go into an empty program and past the table's end, in the order of their programs. The bank keeps its
// instruments and its waves. A copy's wave takes the bank's entry for the same wave, or else a new entry after the
// last. The result is the bank that the tools write for the same instruments, with the copies last.
void TestCopies(Flavor flavor)
{
    const sar_files::BankSpec target_spec = TargetSpec();
    const sar_files::BankSpec source_spec = SourceSpec();
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, target_spec);
    const std::vector<uint8_t> source = sar_tools::Bank(flavor, source_spec);
    const std::vector<InstrumentCopy> copies = {{source, 2, 5}, {source, 1, 1}, {source, 0, 3}, {source, 2, 6}};
    const std::map<uint32_t, uint32_t> kWaves = {{0, 3}, {1, 1}, {2, 2}};
    sar_files::BankSpec spec = target_spec;
    spec.waves.push_back({WaveArchive(1), 6});
    spec.waves.push_back({WaveArchive(1), 5});
    spec.instruments.resize(7);
    spec.instruments[1] = Remapped(*source_spec.instruments[1], kWaves);
    spec.instruments[3] = Remapped(*source_spec.instruments[0], kWaves);
    spec.instruments[5] = Remapped(*source_spec.instruments[2], kWaves);
    spec.instruments[6] = spec.instruments[5];

    const std::vector<uint8_t> bank = WithInstruments(target, flavor, copies);

    XSFLIB_CHECK(bank == sar_tools::Bank(flavor, spec, {0, 2, 1, 3, 5, 6}));
    XSFLIB_CHECK(Describe(bank, flavor, 0) == Describe(target, flavor, 0));
    XSFLIB_CHECK(Describe(bank, flavor, 2) == Describe(target, flavor, 2));
    XSFLIB_CHECK(Describe(bank, flavor, 4) == "none");
    for (const InstrumentCopy& copy : copies)
    {
        XSFLIB_CHECK(Describe(bank, flavor, copy.to) == Describe(source, flavor, copy.program));
    }
}

// Instruments of the bank itself go in again with the bank's waves, and the table of waves stays. A copy into an empty
// program keeps the table's length. The copies go after the padding of an envelope that ends the bank's instruments.
void TestSameBank(Flavor flavor)
{
    const sar_files::BankSpec target_spec = TargetSpec();
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, target_spec);
    sar_files::BankSpec spec = target_spec;
    spec.instruments = {target_spec.instruments[0], target_spec.instruments[2], target_spec.instruments[2],
                        target_spec.instruments[0]};
    sar_files::BankSpec gap_spec = target_spec;
    gap_spec.instruments[1] = target_spec.instruments[0];
    sar_files::BankSpec envelope_spec = target_spec;
    envelope_spec.instruments = {target_spec.instruments[2], std::nullopt, target_spec.instruments[0]};
    const std::vector<uint8_t> envelope_last = sar_tools::Bank(flavor, envelope_spec);
    sar_files::BankSpec envelope_gap_spec = envelope_spec;
    envelope_gap_spec.instruments[1] = envelope_spec.instruments[2];

    const std::vector<uint8_t> bank = With(target, flavor, {{target, 2, 1}, {target, 0, 3}});
    const std::vector<uint8_t> gap = With(target, flavor, {{target, 0, 1}});
    const std::vector<uint8_t> envelope_gap = With(envelope_last, flavor, {{envelope_last, 2, 1}});

    XSFLIB_CHECK(bank == sar_tools::Bank(flavor, spec, {0, 2, 1, 3}));
    XSFLIB_CHECK(gap == sar_tools::Bank(flavor, gap_spec, {0, 2, 1}));
    XSFLIB_CHECK(envelope_gap == sar_tools::Bank(flavor, envelope_gap_spec, {0, 2, 1}));
}

// A bank of one instrument, Direct(Full(0)), whose structures lie out of the tools' order, with the tables of waves and
// of instruments among them. The region, the envelope's bytes, the key region and the table of waves come first. The
// reference to the envelope, the velocity table, the key table and the table of instruments follow, then the
// instrument. Each offset of the instrument then crosses the end of a table. WithInstruments puts new bytes there.
std::vector<uint8_t> Scrambled(Flavor flavor)
{
    constexpr std::size_t kRegion = 16;
    constexpr std::size_t kEnvelope = 48;
    constexpr std::size_t kKeyRegion = 56;
    constexpr std::size_t kWaves = 64;
    constexpr std::size_t kEnvelopeReference = 76;
    constexpr std::size_t kVelocities = 84;
    constexpr std::size_t kKeys = 92;
    constexpr std::size_t kInstruments = 100;
    constexpr std::size_t kInstrument = 112;
    sar_files::Bytes body(flavor == Flavor::kCafe);
    body.Add(kInstrument + 8);
    body.Ref(0, 0x0100, kWaves, 0);
    body.Ref(8, 0x0101, kInstruments, 0);
    const std::array<uint32_t, 8> region = {
        0, 0x21F, 60, 100, 40, std::bit_cast<uint32_t>(1.5f), 0x00010201, kEnvelopeReference - kRegion};
    for (std::size_t i = 0; i < region.size(); ++i)
    {
        body.U32(kRegion + 4 * i, region[i]);
    }
    for (uint8_t i = 0; i < 5; ++i)
    {
        body.U8(kEnvelope + i, static_cast<uint8_t>(10 * (i + 1)));
    }
    body.Ref(kKeyRegion, 0x6000, kVelocities, kKeyRegion);
    body.U32(kWaves, 1);
    body.U32(kWaves + 4, WaveArchive(0));
    body.Ref(kEnvelopeReference, 0x0000, kEnvelope, kEnvelopeReference);
    body.Ref(kVelocities, 0x5901, kRegion, kVelocities);
    body.Ref(kKeys, 0x5901, kKeyRegion, kKeys);
    body.U32(kInstruments, 1);
    body.Ref(kInstruments + 4, 0x5900, kInstrument, kInstruments);
    body.Ref(kInstrument, 0x6000, kKeys, kInstrument);

    return sar_files::File(flavor == Flavor::kCafe, sar_files::detail::Magic(flavor, "BNK").c_str(), 0x00010000,
                           {{0x5800, "INFO", body.data}});
}

// The scrambled bank's offsets follow the new bytes. Its instrument goes into another bank laid out as the tools lay
// it out.
void TestScrambled(Flavor flavor)
{
    const std::vector<uint8_t> scrambled = Scrambled(flavor);
    const std::vector<uint8_t> source = sar_tools::Bank(flavor, SourceSpec());
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, TargetSpec());
    sar_files::BankSpec spec = TargetSpec();
    spec.instruments.push_back(Direct(Full(0)));

    const std::vector<uint8_t> bank = With(scrambled, flavor, {{source, 1, 1}});
    const std::vector<uint8_t> copy = With(target, flavor, {{scrambled, 0, 3}});

    XSFLIB_CHECK(Describe(scrambled, flavor, 0) == Describe(target, flavor, 0));
    XSFLIB_CHECK(Describe(bank, flavor, 0) == Describe(scrambled, flavor, 0));
    XSFLIB_CHECK(Describe(bank, flavor, 1) == Describe(source, flavor, 1));
    XSFLIB_CHECK(copy == sar_tools::Bank(flavor, spec));
}

// A bank of sar_files.h's layout: the table of waves first, references of type 0x5902 to no instrument, and INFO
// padded to 32 bytes. The copies go after the last instrument, before the padding. Every instrument reads as before.
void TestOtherLayout(Flavor flavor)
{
    const std::vector<uint8_t> target = sar_files::Bank(flavor, TargetSpec());
    const std::vector<uint8_t> source = sar_files::Bank(flavor, SourceSpec());
    const std::vector<InstrumentCopy> copies = {{source, 0, 1}, {source, 2, 4}};

    const std::vector<uint8_t> bank = WithInstruments(target, flavor, copies);

    const std::vector<ntdlib::sar::WaveId> waves = ntdlib::sar::Bank::Parse(bank, StrictOptions(flavor)).waves;
    XSFLIB_CHECK(waves.size() == 3 && waves[2].wave_archive_item == WaveArchive(1) && waves[2].index == 5);
    XSFLIB_CHECK(Describe(bank, flavor, 0) == Describe(target, flavor, 0));
    XSFLIB_CHECK(Describe(bank, flavor, 2) == Describe(target, flavor, 2));
    XSFLIB_CHECK(Describe(bank, flavor, 3) == "none");
    for (const InstrumentCopy& copy : copies)
    {
        XSFLIB_CHECK(Describe(bank, flavor, copy.to) == Describe(source, flavor, copy.program));
    }
}

// A key region that two keys name goes in once. Both keys of the copy name it. Here the source's second key names the
// first key's region, and the region that it named before lies unused.
void TestSharedKeyRegion(Flavor flavor)
{
    sar_files::VelocityTable a;
    a.entries = {{127, Full(0)}};
    sar_files::VelocityTable b;
    b.entries = {{127, Plain(1, 80, false)}};
    sar_files::KeyTable keys;
    keys.kind = sar_files::KeyTable::kIndex;
    keys.first = 60;
    keys.entries = {{60, a}, {61, a}, {62, b}};
    sar_files::BankSpec spec = TargetSpec();
    spec.instruments = {keys};
    const std::vector<uint8_t> source = sar_tools::Bank(flavor, spec);
    std::vector<uint8_t> shared = source;
    const std::size_t table = InstrumentPlace(source, flavor, 0) + 8;
    const std::size_t second = RegionPlace(source, flavor, table, 4, 1);
    const std::size_t third = RegionPlace(source, flavor, table, 4, 2);
    Store32(shared, table + 4 + 8 + 4, static_cast<uint32_t>(RegionPlace(source, flavor, table, 4, 0) - table), flavor);
    spec.instruments.clear();
    const std::vector<uint8_t> empty = sar_tools::Bank(flavor, spec);

    const std::vector<uint8_t> each = With(empty, flavor, {{source, 0, 0}});
    const std::vector<uint8_t> once = With(empty, flavor, {{shared, 0, 0}});
    const std::vector<uint8_t> into_shared = With(shared, flavor, {{source, 0, 1}});

    XSFLIB_CHECK(Describe(once, flavor, 0) == Describe(shared, flavor, 0));
    XSFLIB_CHECK(once.size() == each.size() - (third - second));
    XSFLIB_CHECK(Describe(into_shared, flavor, 0) == Describe(shared, flavor, 0));
    XSFLIB_CHECK(Describe(into_shared, flavor, 1) == Describe(source, flavor, 0));
}

// A reference of type 0x5903 names no region whatever its offset, and a reference without an offset names none
// whatever its type.
void TestEmptyReferences(Flavor flavor)
{
    std::vector<uint8_t> source = sar_tools::Bank(flavor, SourceSpec());
    const std::size_t keys = InstrumentPlace(source, flavor, 2) + 8;
    Store32(source, keys + 8 + 8 + 4, 0, flavor);
    const std::size_t velocities = RegionPlace(source, flavor, InstrumentPlace(source, flavor, 0) + 8, 4, 0) + 8;
    Store16(source, velocities + 4 + 8, 0x5902, flavor);
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, TargetSpec());

    const std::vector<uint8_t> bank = With(target, flavor, {{source, 2, 3}, {source, 0, 4}});

    XSFLIB_CHECK(Describe(bank, flavor, 3) == Describe(source, flavor, 2));
    XSFLIB_CHECK(Describe(bank, flavor, 4) == Describe(source, flavor, 0));
}

// An index table whose last key lies before its first holds no references. Its instrument plays nothing.
void TestEmptyIndexTable(Flavor flavor)
{
    std::vector<uint8_t> source = sar_tools::Bank(flavor, SourceSpec());
    source[InstrumentPlace(source, flavor, 0) + 8 + 1] = 50;
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, TargetSpec());

    const std::vector<uint8_t> bank = With(target, flavor, {{source, 0, 3}});

    XSFLIB_CHECK(Describe(bank, flavor, 3).empty());
}

// A block after INFO moves past the bytes that go into INFO. It keeps its bytes.
void TestBlocks(Flavor flavor)
{
    const std::vector<uint8_t> one = sar_files::Bank(flavor, TargetSpec());
    const std::vector<uint8_t> body(one.begin() + kBody, one.end());
    const std::vector<uint8_t> target =
        sar_files::File(flavor == Flavor::kCafe, flavor == Flavor::kCtr ? "CBNK" : "FBNK", 0x00010000,
                        {{0x5800, "INFO", body}, {0x5801, "XTRA", {1, 2, 3, 4}}});
    const std::vector<uint8_t> source = sar_tools::Bank(flavor, SourceSpec());

    const std::vector<uint8_t> bank = With(target, flavor, {{source, 1, 3}});

    const std::size_t grown = bank.size() - target.size();
    const std::size_t block = Load32(bank, 0x14 + 12 + 4, flavor);
    XSFLIB_CHECK(grown > 0 && block == Load32(target, 0x14 + 12 + 4, flavor) + grown);
    XSFLIB_CHECK(std::memcmp(&bank[block], "XTRA", 4) == 0 && bank[block + 8] == 1 && bank[block + 11] == 4);
    XSFLIB_CHECK(Load32(bank, 0x14 + 8, flavor) == Load32(target, 0x14 + 8, flavor) + grown);
    XSFLIB_CHECK(Describe(bank, flavor, 3) == Describe(source, flavor, 1));
}

// Program 0xFFFF takes a copy. The table of instruments grows to 65,536 entries. A program change selects no program
// past it.
void TestLastProgram(Flavor flavor)
{
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, TargetSpec());
    const std::vector<uint8_t> source = sar_tools::Bank(flavor, SourceSpec());

    const std::vector<uint8_t> bank = With(target, flavor, {{source, 1, 0xFFFF}});

    XSFLIB_CHECK(ntdlib::sar::Bank::Parse(bank, StrictOptions(flavor)).instruments.size() == 0x10000);
    XSFLIB_CHECK(Describe(bank, flavor, 0xFFFF) == Describe(source, flavor, 1));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { With(target, flavor, {{source, 1, 0x10000}}); }));
}

// WithInstruments refuses programs that the bank or another copy takes, programs of a source without an instrument,
// and banks of another flavor.
void TestRefusals(Flavor flavor)
{
    const std::vector<uint8_t> target = sar_tools::Bank(flavor, TargetSpec());
    const std::vector<uint8_t> source = sar_tools::Bank(flavor, SourceSpec());
    const Flavor other = flavor == Flavor::kNx ? Flavor::kCafe : Flavor::kNx;
    const std::vector<uint8_t> other_source = sar_tools::Bank(other, SourceSpec());
    const auto refused =
        [](std::span<const uint8_t> bank, Flavor bank_flavor, const std::vector<InstrumentCopy>& copies)
    {
        return Throws<std::invalid_argument>([&] { With(bank, bank_flavor, copies); });
    };

    XSFLIB_CHECK(refused(target, flavor, {{source, 0, 0}}));
    XSFLIB_CHECK(refused(target, flavor, {{source, 0, 2}}));
    XSFLIB_CHECK(refused(target, flavor, {{source, 0, 3}, {source, 1, 3}}));
    XSFLIB_CHECK(refused(target, flavor, {{target, 1, 3}}));
    XSFLIB_CHECK(refused(target, flavor, {{target, 3, 3}}));
    XSFLIB_CHECK(refused(target, flavor, {{other_source, 0, 3}}));
    XSFLIB_CHECK(refused(target, other, {}));
    XSFLIB_CHECK(!refused(target, flavor, {{source, 0, 1}}));
}

// WithInstruments throws xsflib::FormatError for damaged files: here a bank of the Switch's flavor.
void TestDamage()
{
    constexpr Flavor kNx = Flavor::kNx;
    const std::vector<uint8_t> target = sar_tools::Bank(kNx, TargetSpec());
    const std::vector<uint8_t> source = sar_tools::Bank(kNx, SourceSpec());
    const auto damaged = [](const std::vector<uint8_t>& bank, const std::vector<uint8_t>& from)
    {
        return Throws<xsflib::FormatError>([&] { With(bank, kNx, {{from, 1, 3}}); });
    };
    const auto patched = [](std::vector<uint8_t> bank, std::size_t at, uint32_t value, std::size_t size)
    {
        size == 2 ? Store16(bank, at, static_cast<uint16_t>(value), kNx) : Store32(bank, at, value, kNx);

        return bank;
    };
    std::vector<uint8_t> magic = target;
    magic[0] = 'X';
    std::vector<uint8_t> kind = target;
    kind[3] = 'X';
    std::vector<uint8_t> bom = target;
    bom[4] = 0;
    std::vector<uint8_t> half_bom = target;
    half_bom[5] = 0;
    const std::vector<uint8_t> other = sar_files::Bank(kNx, SourceSpec());
    std::vector<uint8_t> before =
        sar_files::File(false, "FBNK", 0x00010000,
                        {{0x5801, "ZERO", std::vector<uint8_t>(32)},
                         {0x5800, "INFO", std::vector<uint8_t>(target.begin() + kBody, target.end())}});
    const std::size_t body = Load32(before, 0x14 + 12 + 4, kNx) + 8;
    Store32(before, body + 4, static_cast<uint32_t>(Load32(before, 0x18, kNx) + 8 - body), kNx);

    XSFLIB_CHECK(damaged(sar_files::WaveArchive(kNx, {}), source));
    XSFLIB_CHECK(damaged(std::vector<uint8_t>(target.begin(), target.begin() + 5), source));
    XSFLIB_CHECK(damaged(magic, source));
    XSFLIB_CHECK(damaged(kind, source));
    XSFLIB_CHECK(damaged(bom, source));
    XSFLIB_CHECK(damaged(half_bom, source));
    XSFLIB_CHECK(damaged(patched(target, 0x14, 0x5801, 2), source));
    XSFLIB_CHECK(damaged(patched(target, 0x1C, 0x10000, 4), source));
    XSFLIB_CHECK(damaged(patched(target, 0x1C, Load32(target, 0x1C, kNx) - 8, 4), source));
    XSFLIB_CHECK(damaged(before, source));
    XSFLIB_CHECK(damaged(patched(target, kBody, 0x0102, 2), source));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { WithInstruments(patched(target, kBody, 0x0102, 2), kNx, {}); }));
    XSFLIB_CHECK(damaged(patched(target, InstrumentPlace(target, kNx, 0), 0x6003, 2), source));
    XSFLIB_CHECK(damaged(patched(target, DirectRegionPlace(target, kNx, 0) + 4, 0x23F, 4), source));
    XSFLIB_CHECK(damaged(patched(target, DirectRegionPlace(target, kNx, 0) + 4, 0x8000021F, 4), source));
    XSFLIB_CHECK(damaged(target, patched(source, DirectRegionPlace(source, kNx, 1) + 4, 0x23F, 4)));
    XSFLIB_CHECK(damaged(target, patched(source, DirectRegionPlace(source, kNx, 1) + 4, 0x80000202, 4)));
    XSFLIB_CHECK(damaged(target, patched(source, InstrumentPlace(source, kNx, 1), 0x6003, 2)));
    XSFLIB_CHECK(damaged(target, patched(other, DirectRegionPlace(other, kNx, 1), 3, 4)));
    XSFLIB_CHECK(!damaged(target, source));
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestNoCopies(flavor);
        TestCopies(flavor);
        TestSameBank(flavor);
        TestOtherLayout(flavor);
        TestScrambled(flavor);
        TestSharedKeyRegion(flavor);
        TestEmptyReferences(flavor);
        TestEmptyIndexTable(flavor);
        TestBlocks(flavor);
        TestLastProgram(flavor);
        TestRefusals(flavor);
    }
    TestDamage();

    return xsflib::testing::Summary("bank edit");
}
