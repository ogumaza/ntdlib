// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of Krawall's patterns (ntdlib/krawall/commands.h), and of the cut and the join
// of a ROM (ntdlib/krawall/layout.h), on small carts built here.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/krawall/commands.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/layout.h"
#include "ntdlib/testing/krawall_cart.h"

namespace
{

using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::krawall::DetectDriver;
using ntdlib::krawall::DriverInfo;
using ntdlib::krawall::DriverOverrides;
using ntdlib::testing::krawall_cart::Cart;
using ntdlib::testing::krawall_cart::kInstrumentTable;
using ntdlib::testing::krawall_cart::kPan;
using ntdlib::testing::krawall_cart::kSampleData;
using ntdlib::testing::krawall_cart::kSampleTable;
using ntdlib::testing::krawall_cart::kVolumeSlide;
using ntdlib::testing::krawall_cart::LoopingCells;
using ntdlib::testing::krawall_cart::Pattern;

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    DetectDriver(rom, DriverOverrides(), info, error);

    return info;
}

// The patterns of a module decode as items, and encode back to their bytes. Pattern 0 has 8 rows: two notes with a
// volume and an effect, an instrument without a note, a note off and empty rows. Pattern 1 has 4 rows and an effect
// alone. Each pattern starts with its kOther item, a wait ends each row with items, and the waits of a pattern add up
// to its rows. A byte of padding that isn't 0 keeps the patterns as data, split at the patterns' starts.
void TestCommands()
{
    using ntdlib::commands::Kind;
    using ntdlib::commands::ToUnit;
    Cart cart;
    cart.Module(
        2,
        {{8, {{0, 0, 49, 1, 0x40}, {0, 1, 53, 2, -1, kPan, 0x20}, {2, 0, 0, 3}, {5, 0, ntdlib::krawall::kNoteOff, 0}}},
         {4, {{1, 1, -1, 0, -1, kVolumeSlide, 0x04}}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ntdlib::krawall::Traits traits = ntdlib::krawall::TraitsOf(info);
    const ntdlib::krawall::Decoded decoded = ntdlib::krawall::DecodeRomSequence(rom, info, 0);
    const std::vector<ntdlib::commands::Command>& items = decoded.sequence.items;

    SEQLIB_REQUIRE(!decoded.opaque && decoded.entries.size() == 2 && items.size() > 10);
    SEQLIB_CHECK(items[0].kind == Kind::kOther && items[0].encoding.opcode == ntdlib::krawall::kPatternOpcode);
    SEQLIB_CHECK(items[1].kind == Kind::kNote && items[1].key == 49 && items[1].sample == 1 && items[1].index == 0);
    SEQLIB_CHECK(items[2].kind == Kind::kVolume && items[2].value.value == ToUnit(0x40, {}));
    SEQLIB_CHECK(items[3].kind == Kind::kNote && items[3].key == 53 && items[3].index == 1);
    SEQLIB_CHECK(items[4].kind == Kind::kOther && items[4].encoding.opcode == ntdlib::krawall::kEffectOpcode + kPan &&
                 items[4].bytes == std::vector<uint8_t>({0x20}));
    SEQLIB_CHECK(items[5].kind == Kind::kWait && items[5].value.value == ToUnit(2, {}));
    SEQLIB_CHECK(items[6].kind == Kind::kProgram && items[6].value.value == ToUnit(3, {}));
    SEQLIB_CHECK(items[8].kind == Kind::kNoteOff && items[8].index == 0);
    int64_t rows[2] = {};
    for (size_t i = 0; i < items.size(); i++)
    {
        if (items[i].kind == Kind::kWait)
        {
            rows[i < decoded.entries[1] ? 0 : 1] += ntdlib::commands::FromUnit(items[i].value.value, {});
        }
    }
    SEQLIB_CHECK(rows[0] == 8 && rows[1] == 4);

    // The bytes come back, from the first pattern to the module's header, with each pattern where the table says.
    ntdlib::krawall::ModuleInfo module;
    SEQLIB_REQUIRE(ntdlib::krawall::ReadModule(rom, info.build, info.modules[0], module));
    const ntdlib::krawall::Encoded encoded = ntdlib::krawall::EncodeSequence(decoded.sequence, decoded.entries, traits);
    const std::vector<uint8_t> original(rom.Ptr(module.patterns[0]), rom.Ptr(info.modules[0]));
    SEQLIB_CHECK(encoded.bytes == original);
    SEQLIB_CHECK_EQ(encoded.offsets[decoded.entries[1]], module.patterns[1] - module.patterns[0]);
    SEQLIB_CHECK(ntdlib::krawall::ItemOffsets(decoded.sequence, traits) == encoded.offsets);

    // A pattern's last byte before the next that isn't 0.
    std::vector<uint8_t> bytes = original;
    const uint32_t second = module.patterns[1] - module.patterns[0];
    bytes[second - 1] = 0xFF;
    const std::vector<uint32_t> starts = {0, second};
    const ntdlib::krawall::Decoded data = ntdlib::krawall::DecodeSequence(bytes, starts, traits);
    SEQLIB_CHECK(data.opaque && !data.reason.empty());
    SEQLIB_CHECK(data.sequence.items.size() == 2 && data.sequence.items[0].kind == Kind::kData &&
                 data.entries == std::vector<uint32_t>({0, 1}));
}

// A ROM cuts into its modules' patterns and the rest, and joins back byte for byte. When the first module's patterns
// grow, they move into the zeros at the ROM's end, and its table of patterns names their new place. The second module
// stays where it was.
void TestLayout()
{
    using ntdlib::commands::Kind;
    Cart cart;
    cart.Module(2, 5, LoopingCells());
    cart.Module(1, 4, {{0, 0, 49, 1}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::vector<uint8_t> bytes(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());

    ntdlib::krawall::Layout layout = ntdlib::krawall::Cut(rom, info);
    SEQLIB_REQUIRE_EQ(layout.sequences.size(), 2u);
    SEQLIB_CHECK_EQ(layout.fields.size(), 2u);
    SEQLIB_CHECK(ntdlib::krawall::Join(layout) == bytes);

    // A volume column for the first module's second note, after item 3. Then a note of channel 1 with a volume in row
    // 1, after the row's effect of channel 0: 5 bytes or more in all, past the pattern's padding of 3 bytes or less.
    std::vector<ntdlib::commands::Command>& items = layout.sequences[0].items;
    ntdlib::commands::Command volume;
    volume.kind = Kind::kVolume;
    volume.index = 1;
    volume.value.value = ntdlib::commands::ToUnit(0x30, {});
    items.insert(items.begin() + 4, volume);
    SEQLIB_REQUIRE(items.size() > 8 && items[1].kind == Kind::kNote && items[7].index == 0 &&
                   items[8].kind == Kind::kWait);
    ntdlib::commands::Command note = items[1];
    note.index = 1;
    items.insert(items.begin() + 8, {note, volume});

    Rom grown;
    grown.Assign(ntdlib::krawall::Join(layout));
    const DriverInfo grown_info = Detect(grown);
    SEQLIB_REQUIRE_EQ(grown_info.modules.size(), 2u);
    const uint32_t before = ntdlib::krawall::SequenceAddress(rom, info, 0);
    const uint32_t after = ntdlib::krawall::SequenceAddress(grown, grown_info, 0);
    SEQLIB_CHECK(after > before && after % 4 == 0);
    SEQLIB_CHECK(ntdlib::krawall::DecodeRomSequence(grown, grown_info, 0).sequence == layout.sequences[0]);
    SEQLIB_CHECK_EQ(ntdlib::krawall::SequenceAddress(grown, grown_info, 1),
                    ntdlib::krawall::SequenceAddress(rom, info, 1));
    const std::vector<uint8_t> grown_bytes(grown.Ptr(kRomBase), grown.Ptr(kRomBase) + grown.Size());
    SEQLIB_CHECK(ntdlib::krawall::Join(ntdlib::krawall::Cut(grown, grown_info)) == grown_bytes);
}

// A note without an instrument looks its sample up again at the note plus the relative note of the channel's sample.
// Sample 0 has a relative note of 4. B-7 (96) without an instrument then reads entry 99 of instrument 1, past its map
// of 96 notes. Entry 99 names a sample whose entry of the game's table is in the zeros at the ROM's end. The free
// space starts past it. With the instrument on B-7's row, the note reads entry 95, and the free space stays where it
// was.
void TestLayoutPastInstrumentMap()
{
    constexpr uint32_t kInstrument = kRomBase + 0x3800;
    constexpr uint32_t kSize = 0x20000;
    constexpr uint32_t kFarEntry = kRomBase + 0x1F000;
    const auto free_start = [](int instrument_on_b7)
    {
        Cart cart;
        const uint32_t header = cart.Module(1, 4, {{0, 0, 49, 1}, {1, 0, 96, instrument_on_b7}});
        cart.Put8(header + ntdlib::krawall::kModuleFlags, 1);
        cart.Put32(kInstrumentTable, kInstrument);
        cart.Put16(kInstrument + 2 * 99, uint16_t((kFarEntry - kSampleTable) / 4));
        cart.Put8(kSampleData + ntdlib::krawall::kSampleRelativeNote, 4);
        cart.Resize(kSize);
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        return ntdlib::krawall::Cut(rom, info).free.start;
    };

    const uint32_t past = free_start(0);
    const uint32_t within = free_start(1);

    SEQLIB_CHECK(past > kFarEntry + 4 - kRomBase && past <= kSize);
    SEQLIB_CHECK(within < kFarEntry - kRomBase);
}

// A note off with an instrument looks its sample up at its note of 127 less 1: entry 126 of the instrument, past its
// map. Entry 126 names a sample whose entry of the game's table is in the zeros at the ROM's end. The free space
// starts past it. A note off without an instrument reads no entry, and the free space stays where it was.
void TestLayoutNoteOffPastInstrumentMap()
{
    constexpr uint32_t kInstrument = kRomBase + 0x3800;
    constexpr uint32_t kSize = 0x20000;
    constexpr uint32_t kFarEntry = kRomBase + 0x1F000;
    const auto free_start = [](int instrument_on_note_off)
    {
        Cart cart;
        const uint32_t header = cart.Module(1, 4, {{0, 0, 49, 1}, {1, 0, 127, instrument_on_note_off}});
        cart.Put8(header + ntdlib::krawall::kModuleFlags, 1);
        cart.Put32(kInstrumentTable, kInstrument);
        cart.Put16(kInstrument + 2 * 126, uint16_t((kFarEntry - kSampleTable) / 4));
        cart.Resize(kSize);
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);

        return ntdlib::krawall::Cut(rom, info).free.start;
    };

    const uint32_t with = free_start(1);
    const uint32_t without = free_start(0);

    SEQLIB_CHECK(with > kFarEntry + 4 - kRomBase && with <= kSize);
    SEQLIB_CHECK(without < kFarEntry - kRomBase);
}

// The sample that a note without an instrument picks inside the map moves the next notes by its relative note. Note 25
// with instrument 1 picks sample 0 at entry 24. B-4 (60) without an instrument then picks sample 1 at entry 59. Sample
// 1 has a relative note of 40, and the next B-4 reads entry 99, past the map. Entry 99 names a sample whose entry of
// the game's table is in the zeros at the ROM's end. The free space starts past it.
void TestLayoutChainPastInstrumentMap()
{
    constexpr uint32_t kInstrument = kRomBase + 0x3800;
    constexpr uint32_t kSize = 0x20000;
    constexpr uint32_t kFarEntry = kRomBase + 0x1F000;
    Cart cart;
    const uint32_t header = cart.Module(1, 4, {{0, 0, 25, 1}, {1, 0, 60, 0}, {2, 0, 60, 0}});
    cart.Put8(header + ntdlib::krawall::kModuleFlags, 1);
    cart.Put32(kInstrumentTable, kInstrument);
    cart.Put16(kInstrument + 2 * 59, 1);
    cart.Put16(kInstrument + 2 * 99, uint16_t((kFarEntry - kSampleTable) / 4));
    const uint32_t one_shot = cart.ToRom().U32(kSampleTable + 4);
    cart.Put8(one_shot + ntdlib::krawall::kSampleRelativeNote, 40);
    cart.Resize(kSize);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const uint32_t start = ntdlib::krawall::Cut(rom, info).free.start;

    SEQLIB_CHECK(start > kFarEntry + 4 - kRomBase && start <= kSize);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("krawall_layout",
                                    []
                                    {
                                        TestCommands();
                                        TestLayout();
                                        TestLayoutPastInstrumentMap();
                                        TestLayoutNoteOffPastInstrumentMap();
                                        TestLayoutChainPastInstrumentMap();
                                    });
}
