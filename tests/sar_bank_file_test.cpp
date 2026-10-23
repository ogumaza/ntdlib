// SPDX-License-Identifier: MIT

// Tests of BankFile (ntdlib/sar/bank_file.h), in each flavor's format: the formats of the 3DS, the Wii U and the
// Switch. The writer lays banks out, and the strict reader of formats.h reads them back. A small bank's bytes are
// worked out by hand from the layout that bank_edit.h describes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sar/bank_file.h"
#include "ntdlib/sar/formats.h"

namespace
{

using ntdlib::sar::BankFile;
using ntdlib::sar::Flavor;
using ntdlib::sar::VelocityRegion;
using xsflib::testing::ErrorOf;

constexpr Flavor kFlavors[] = {Flavor::kCtr, Flavor::kCafe, Flavor::kNx};

// The readers of formats.h, stopping at the first problem.
ntdlib::sar::Options StrictOptions(Flavor flavor)
{
    ntdlib::sar::Options options;
    options.flavor = flavor;
    options.rules = ntdlib::sar::Rules::kStopAtFirstProblem;

    return options;
}

uint32_t Load32(const std::vector<uint8_t>& bytes, std::size_t at, Flavor flavor)
{
    return flavor == Flavor::kCafe ? xsflib::LoadBe32(&bytes[at]) : xsflib::LoadLe32(&bytes[at]);
}

uint16_t Load16(const std::vector<uint8_t>& bytes, std::size_t at, Flavor flavor)
{
    return flavor == Flavor::kCafe ? xsflib::LoadBe16(&bytes[at]) : xsflib::LoadLe16(&bytes[at]);
}

// Whether `call` throws an exception of type E.
template <typename E, typename Call>
bool Throws(Call call)
{
    return !ErrorOf<E>(call).empty();
}

// A velocity region over `first` to `last` that plays wave `wave` at key `key`.
BankFile::Region Region(uint8_t first, uint8_t last, uint32_t wave, uint8_t key)
{
    BankFile::Region region;
    region.first_velocity = first;
    region.last_velocity = last;
    region.values.wave_id_index = wave;
    region.values.original_key = key;

    return region;
}

// A key region over `first` to `last` with one velocity region over every velocity.
BankFile::KeyRegion KeyRegion(uint8_t first, uint8_t last, uint32_t wave)
{
    return {first, last, {Region(0, 127, wave, first)}};
}

// A bank of `flavor` with `instruments` and two waves.
BankFile Bank(Flavor flavor, std::vector<std::optional<BankFile::Instrument>> instruments)
{
    BankFile bank;
    bank.flavor = flavor;
    bank.version = flavor == Flavor::kCtr ? 0x01000100 : 0x00010000;
    bank.waves = {{0x05000002, 3}, {0x05000001, 0}};
    bank.instruments = std::move(instruments);

    return bank;
}

// The kind of the table that the reference at `at` of `bytes` leads to: 0x6000 for direct, 0x6001 for range and 0x6002
// for index.
uint16_t KindAt(const std::vector<uint8_t>& bytes, std::size_t at, Flavor flavor)
{
    return Load16(bytes, at, flavor);
}

// The place of program `program`'s instrument in a bank of one block.
std::size_t InstrumentPlace(const std::vector<uint8_t>& bytes, uint32_t program, Flavor flavor)
{
    const std::size_t table = 0x28 + Load32(bytes, 0x34, flavor);

    return table + Load32(bytes, table + 8 + 8 * std::size_t{program}, flavor);
}

// Whether the strict reader plays `key` and `velocity` of program `program` of `bytes` with the values of `expected`,
// or with none.
bool Plays(const std::vector<uint8_t>& bytes, Flavor flavor, uint32_t program, int key, int velocity,
           const std::optional<VelocityRegion>& expected)
{
    const ntdlib::sar::Bank bank = ntdlib::sar::Bank::Parse(bytes, StrictOptions(flavor));
    const VelocityRegion* found = bank.instruments[program]->Find(key, velocity);

    return expected ? found != nullptr && *found == *expected : found == nullptr;
}

// One instrument of one region over every key and velocity: the bytes that bank_edit.h's layout gives. Both tables are
// direct. The header takes 32 bytes, and INFO follows.
void TestSmallBank(Flavor flavor)
{
    const BankFile bank = Bank(flavor, {BankFile::Instrument{{KeyRegion(0, 127, 1)}}});
    std::vector<uint8_t> expected(0xA8);
    const bool big = flavor == Flavor::kCafe;
    const auto u16 = [&expected, big](std::size_t at, uint16_t value)
    {
        big ? xsflib::StoreBe16(&expected[at], value) : xsflib::StoreLe16(&expected[at], value);
    };
    const auto u32 = [&expected, big](std::size_t at, uint32_t value)
    {
        big ? xsflib::StoreBe32(&expected[at], value) : xsflib::StoreLe32(&expected[at], value);
    };
    const auto reference = [&u16, &u32](std::size_t at, uint16_t type, uint32_t offset)
    {
        u16(at, type);
        u32(at + 4, offset);
    };
    const std::string magic = flavor == Flavor::kCtr ? "CBNK" : "FBNK";
    std::copy(magic.begin(), magic.end(), expected.begin());
    u16(0x04, 0xFEFF);
    u16(0x06, 0x20);
    u32(0x08, bank.version);
    u32(0x0C, 0xA8);
    u16(0x10, 1);
    reference(0x14, 0x5800, 0x20);
    u32(0x1C, 0x88);
    std::copy_n("INFO", 4, expected.begin() + 0x20);
    u32(0x24, 0x88);
    reference(0x28, 0x0100, 0x6C); // the table of waves, at 0x94
    reference(0x30, 0x0101, 0x10); // the table of instruments, at 0x38
    u32(0x38, 1);
    reference(0x3C, 0x5900, 0x0C); // the instrument, at 0x44
    reference(0x44, 0x6000, 0x08); // its direct table of key regions, at 0x4C
    reference(0x4C, 0x5901, 0x08); // the key region, at 0x54
    reference(0x54, 0x6000, 0x08); // its direct table of velocity regions, at 0x5C
    reference(0x5C, 0x5902, 0x08); // the velocity region, at 0x64
    u32(0x64, 1);                  // its wave
    u32(0x68, 0x21F);              // its six options
    u32(0x70, 127);                // the volume, after the original key of 0
    u32(0x74, 64);                 // the pan
    u32(0x78, 0x3F800000);         // the pitch, 1.0
    u32(0x80, 0x20);               // the reference to the envelope, at 0x84, after the flags of 0
    reference(0x84, 0x0000, 0x08); // the envelope, at 0x8C, then 3 bytes of padding
    std::fill_n(expected.begin() + 0x8C, 5, 127);
    u32(0x94, 2);
    u32(0x98, 0x05000002);
    u32(0x9C, 3);
    u32(0xA0, 0x05000001);

    const std::vector<uint8_t> bytes = bank.Write();

    XSFLIB_CHECK(bytes == expected);
    XSFLIB_CHECK(BankFile::Read(bytes) == bank);
}

// The tools' choice of a table's kind: one region over every key takes a direct table, up to 11 runs a range table
// with an entry for the gap after the last region, and 12 runs or more an index table from key 0 to the last region.
void TestTableKinds(Flavor flavor)
{
    BankFile::Instrument two;
    two.key_regions = {KeyRegion(0, 59, 0), KeyRegion(60, 127, 1)};
    BankFile::Instrument middle;
    middle.key_regions = {KeyRegion(10, 20, 0)};
    BankFile::Instrument eleven;
    BankFile::Instrument twelve;
    for (uint8_t key = 10; key < 21; ++key)
    {
        if (key < 20)
        {
            eleven.key_regions.push_back(KeyRegion(key, key, key % 2));
        }
        twelve.key_regions.push_back(KeyRegion(key, key, key % 2));
    }
    BankFile::Instrument layers;
    layers.key_regions = {{0, 127, {Region(0, 63, 0, 60), Region(64, 127, 1, 61)}}};
    BankFile::Instrument gaps;
    gaps.key_regions = {KeyRegion(0, 59, 0), KeyRegion(61, 126, 1)};
    BankFile::Instrument wide = twelve;
    wide.key_regions.back().last_key = 21;
    const BankFile bank = Bank(flavor, {two, middle, eleven, twelve, layers, gaps, wide});

    const std::vector<uint8_t> bytes = bank.Write();

    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 0, flavor), flavor) == 0x6001);
    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 1, flavor), flavor) == 0x6001);
    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 2, flavor), flavor) == 0x6001);
    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 3, flavor), flavor) == 0x6002);
    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 4, flavor), flavor) == 0x6000);
    XSFLIB_CHECK(Plays(bytes, flavor, 0, 59, 100, Region(0, 127, 0, 0).values));
    XSFLIB_CHECK(Plays(bytes, flavor, 0, 60, 100, Region(0, 127, 1, 60).values));
    XSFLIB_CHECK(Plays(bytes, flavor, 1, 9, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 1, 15, 100, Region(0, 127, 0, 10).values));
    XSFLIB_CHECK(Plays(bytes, flavor, 1, 21, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 2, 19, 100, Region(0, 127, 1, 19).values));
    XSFLIB_CHECK(Plays(bytes, flavor, 2, 20, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 3, 9, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 3, 20, 100, Region(0, 127, 0, 20).values));
    XSFLIB_CHECK(Plays(bytes, flavor, 3, 21, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 4, 60, 63, Region(0, 63, 0, 60).values));
    XSFLIB_CHECK(Plays(bytes, flavor, 4, 60, 64, Region(64, 127, 1, 61).values));
    XSFLIB_CHECK(BankFile::Read(bytes) == bank);

    // A gap of one key takes an entry, and so does the gap after a region that ends at 126. An index table names a
    // region at each of its keys, and lays it out once.
    const std::size_t place = InstrumentPlace(bytes, 5, flavor);
    const std::size_t table = place + Load32(bytes, place + 4, flavor);
    XSFLIB_CHECK(Load32(bytes, table, flavor) == 4);
    XSFLIB_CHECK(std::vector<uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(table) + 4,
                                      bytes.begin() + static_cast<std::ptrdiff_t>(table) + 8) ==
                 std::vector<uint8_t>({59, 60, 126, 127}));
    XSFLIB_CHECK(Plays(bytes, flavor, 5, 60, 100, std::nullopt) && Plays(bytes, flavor, 5, 127, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 6, 21, 100, Region(0, 127, 0, 20).values));
    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 6, flavor), flavor) == 0x6002);
}

// Programs without instruments, instruments laid out in another order than their programs', and instruments without
// regions come back. An instrument without key regions and a key region without velocity regions take a range table
// with one entry that names nothing.
void TestOrderAndGaps(Flavor flavor)
{
    BankFile::Instrument empty;
    BankFile::Instrument silent;
    silent.key_regions = {{0, 127, {}}};
    BankFile bank = Bank(flavor, {BankFile::Instrument{{KeyRegion(0, 127, 1)}}, std::nullopt, empty, silent});
    bank.order = {3, 0, 2};

    const std::vector<uint8_t> bytes = bank.Write();

    XSFLIB_CHECK(BankFile::Read(bytes) == bank);
    XSFLIB_CHECK(InstrumentPlace(bytes, 3, flavor) < InstrumentPlace(bytes, 0, flavor));
    XSFLIB_CHECK(KindAt(bytes, InstrumentPlace(bytes, 2, flavor), flavor) == 0x6001);
    XSFLIB_CHECK(Plays(bytes, flavor, 2, 60, 100, std::nullopt));
    XSFLIB_CHECK(Plays(bytes, flavor, 3, 60, 100, std::nullopt));
    XSFLIB_CHECK(!ntdlib::sar::Bank::Parse(bytes, StrictOptions(flavor)).instruments[1]);
}

// Every value of a region comes back, and the strict reader reads it.
void TestValues(Flavor flavor)
{
    BankFile::Region region = Region(0, 127, 1, 72);
    region.values.volume = 100;
    region.values.pan = 20;
    region.values.pitch = 1.5f;
    region.values.ignore_note_off = true;
    region.values.key_group = 3;
    region.values.interpolation = 1;
    region.values.adshr = {1, 2, 3, 4, 5};
    const BankFile bank = Bank(flavor, {BankFile::Instrument{{{0, 127, {region}}}}});

    const std::vector<uint8_t> bytes = bank.Write();

    XSFLIB_CHECK(Plays(bytes, flavor, 0, 0, 1, region.values));
    XSFLIB_CHECK(BankFile::Read(bytes) == bank);
}

// The writer refuses runs that overlap, run backwards, pass 127 or come out of order, and an order that doesn't name
// each instrument once. The reader refuses other bytes, and a bank that the tools didn't lay out.
void TestRefusals(Flavor flavor)
{
    const auto refuses = [flavor](std::vector<BankFile::KeyRegion> key_regions)
    {
        const BankFile bank = Bank(flavor, {BankFile::Instrument{std::move(key_regions)}});

        return Throws<std::invalid_argument>([&] { bank.Write(); });
    };
    BankFile unordered = Bank(flavor, {BankFile::Instrument{{KeyRegion(0, 127, 0)}}, std::nullopt});
    unordered.order = {1};
    const std::vector<uint8_t> bytes = Bank(flavor, {BankFile::Instrument{{KeyRegion(0, 127, 0)}}}).Write();
    std::vector<uint8_t> fewer = bytes;
    fewer[flavor == Flavor::kCafe ? 0x6A : 0x69] = 0; // options 0x1F, without the envelope
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    const std::string past_end =
        "a bank file that NintendoWare's tools didn't lay out: byte " + std::to_string(bytes.size()) + " differs";

    XSFLIB_CHECK(refuses({KeyRegion(0, 60, 0), KeyRegion(60, 127, 1)}));
    XSFLIB_CHECK(refuses({KeyRegion(60, 127, 0), KeyRegion(0, 59, 1)}));
    XSFLIB_CHECK(refuses({KeyRegion(61, 60, 0)}));
    XSFLIB_CHECK(refuses({KeyRegion(0, 128, 0)}));
    XSFLIB_CHECK(refuses({{0, 127, {Region(0, 64, 0, 60), Region(64, 127, 0, 60)}}}));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { unordered.Write(); }));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { BankFile::Read(fewer); }) ==
                 "a velocity region without all six values");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { BankFile::Read(longer); }) == past_end);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([] { BankFile::Read(std::vector<uint8_t>(0x40)); }) ==
                 "not a bank file (CBNK or FBNK)");
}

} // namespace

int main()
{
    for (const Flavor flavor : kFlavors)
    {
        TestSmallBank(flavor);
        TestTableKinds(flavor);
        TestOrderAndGaps(flavor);
        TestValues(flavor);
        TestRefusals(flavor);
    }

    return xsflib::testing::Summary("bank file");
}
