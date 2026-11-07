// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a GBA ROM of Ubisoft Milan's driver (ntdlib/ubimilan/layout.h), on small synthetic
// cartridges. A cartridge has to come back byte for byte, and a sequence that grows has to move to where the driver
// finds it through the resource table. The table holds its distance from the bank's start.

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/layout.h"
#include "ntdlib/ubimilan/output.h"
#include "xsflib/testing/check.h"

#include "ubimilan_image.h"

namespace
{

namespace ubimilan = ntdlib::ubimilan;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::testing::ubimilan_image::kBank;
using ntdlib::testing::ubimilan_image::TestImage;
using ubimilan::DriverInfo;
using ubimilan::Layout;

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    XSFLIB_CHECK(ubimilan::DetectDriver(rom, {}, info, error));
    return info;
}

template <typename Function>
bool Refuses(Function function)
{
    try
    {
        function();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

Rom RomOf(const std::vector<uint8_t>& bytes)
{
    Rom rom;
    rom.Assign(std::vector<uint8_t>(bytes));
    return rom;
}

std::vector<uint8_t> BytesOf(const Rom& rom)
{
    return std::vector<uint8_t>(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
}

// A command without an effect, with a countdown of 0 after it: it costs no time.
void Grow(ntdlib::commands::Sequence& sequence, int commands)
{
    for (int i = 0; i < commands; i++)
    {
        Command nothing;
        nothing.kind = Kind::kOther;
        nothing.encoding.opcode = 0xE0;
        Command wait;
        wait.kind = Kind::kWait;
        sequence.items.insert(sequence.items.begin() + 2, {nothing, wait});
    }
}

// Puts resource `id` at `address`, by its entry in the resource table.
void MoveResource(TestImage& image, int id, uint32_t address)
{
    image.Put32(kBank + 0x10 + 4 * uint32_t(id), address - kBank);
}

// Puts a sequence of no time at `address`: a kind, 3 bytes, a countdown of 0 and the end.
void PutSequence(TestImage& image, uint32_t address)
{
    image.Put8(address, uint8_t(ubimilan::kSequence));
    image.Put8(address + 1, 0xFF);
    image.Put8(address + 2, 0xFF);
    image.Put8(address + 3, 0xFF);
    image.Put8(address + 4, 0);
    image.Put8(address + 5, 0);
    image.Put8(address + 6, 0xFC);
}

// Two pieces of one sequence each, and a sample between the sequences.
struct TwoPieces
{
    TwoPieces()
    {
        const int sample = image.AddSample({64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64});
        image.SetKey(40, sample);
        first = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x04, 0x00, 0xFC});
        second = image.AddSample({1, 2, 3, 4});
        third = image.AddSequence({0x01, 0x00, 0x99, 40, 32, 0x02, 0x00, 0xE0, 0x00, 0x00, 0xFC});
        image.AddPiece(0, {first});
        image.AddPiece(0, {third});
    }

    TestImage image;
    int first = 0;
    int second = 0;
    int third = 0;
};

void TestComesBack()
{
    TwoPieces pieces;
    const Rom rom = pieces.image.ToRom();
    const DriverInfo info = Detect(rom);
    const Layout layout = ubimilan::Cut(rom, info);
    XSFLIB_CHECK(ubimilan::Join(layout) == BytesOf(rom));
    SEQLIB_REQUIRE(layout.sequences.size() == 2);
    XSFLIB_CHECK(layout.resources == std::vector<int>({pieces.first, pieces.third}));

    // A run, a sequence, a run, a sequence, a run.
    SEQLIB_REQUIRE(layout.pieces.size() == 5);
    XSFLIB_CHECK(std::holds_alternative<std::vector<uint8_t>>(layout.pieces[0]));
    const auto& piece = std::get<ntdlib::sar::SequencePiece>(layout.pieces[1]);
    XSFLIB_CHECK(piece.sequence == 0 && piece.room == 12);
    XSFLIB_CHECK(std::get<ntdlib::sar::SequencePiece>(layout.pieces[3]).room == 15);

    // Each sequence's entry of the resource table is a field from the bank's start.
    SEQLIB_REQUIRE(layout.fields.size() == 2);
    const ntdlib::sar::Field& field = layout.fields[0];
    XSFLIB_CHECK(field.at.piece == 0 && field.at.offset == kBank + 0x10 + 4 * uint32_t(pieces.first) - kRomBase);
    XSFLIB_CHECK(field.from.piece == 0 && field.from.offset == kBank - kRomBase);
    XSFLIB_CHECK(field.to.piece == 1 && field.to.item);
}

void TestWithoutSequences()
{
    // A ROM without a sequence is one run.
    TestImage image;
    const int sequence = image.AddSequence({0x00, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    std::vector<uint8_t> bytes = BytesOf(image.ToRom());
    const Rom rom = RomOf(bytes);
    DriverInfo info = Detect(rom);
    info.resource_count = 0;
    const Layout layout = ubimilan::Cut(rom, info);
    XSFLIB_CHECK(layout.sequences.empty() && layout.pieces.size() == 1 && layout.fields.empty());
    XSFLIB_CHECK(ubimilan::Join(layout) == bytes);
}

void TestSharedSequence()
{
    // Two resources that name one sequence share a piece, and the two fields name it.
    TestImage image;
    const int sequence = image.AddSequence({0x00, 0x00, 0xFC});
    image.AddPiece(0, {sequence});
    const int copy = image.AddSample({1, 2, 3, 4});
    image.Put32(ntdlib::testing::ubimilan_image::kBank + 0x10 + 4 * uint32_t(copy),
                image.Resource(sequence) - ntdlib::testing::ubimilan_image::kBank);
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    const Layout layout = ubimilan::Cut(rom, info);
    XSFLIB_CHECK(layout.sequences.size() == 1);
    XSFLIB_CHECK(layout.resources == std::vector<int>({sequence}));
    SEQLIB_REQUIRE(layout.fields.size() == 2);
    XSFLIB_CHECK(layout.fields[0].to.piece == layout.fields[1].to.piece);
    XSFLIB_CHECK(ubimilan::Join(layout) == BytesOf(rom));
}

void TestSequencesThatStay()
{
    // A sequence that holds another's bytes stays among the ROM's bytes, with the other: a move would take the bytes
    // from under it.
    TestImage image;
    const int outer = image.AddSequence({0x00, 0x00, 0xE0, 0x00, 0x00, 0xFC});
    image.AddPiece(0, {outer});
    const int inner = image.AddSample({1, 2, 3, 4});
    const uint32_t inner_address = image.Resource(outer) + 1;
    image.Put32(kBank + 0x10 + 4 * uint32_t(inner), inner_address - kBank);
    image.Put8(inner_address, uint8_t(ubimilan::kSequence));
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    const Layout layout = ubimilan::Cut(rom, info);
    XSFLIB_CHECK(layout.sequences.empty());
    XSFLIB_CHECK(ubimilan::Join(layout) == BytesOf(rom));

    // A sequence kept as data stays too.
    TestImage kept;
    const int unknown = kept.AddSequence({0x00, 0x00, 0xA0, 0, 0, 0xFC});
    const int fine = kept.AddSequence({0x00, 0x00, 0xFC});
    kept.AddPiece(0, {fine});
    const Rom kept_rom = kept.ToRom();
    const Layout kept_layout = ubimilan::Cut(kept_rom, Detect(kept_rom));
    XSFLIB_CHECK(kept_layout.resources == std::vector<int>({fine}));
    XSFLIB_CHECK(unknown != fine);
    XSFLIB_CHECK(ubimilan::Join(kept_layout) == BytesOf(kept_rom));

    // A sequence's bytes can read as sequences by themselves: a note's key as a kind, and a countdown's high byte as an
    // end. Each of them stays, and so does the sequence that holds them. The second lies past the first's end, and
    // still in the sequence that holds both.
    TestImage nested;
    const uint8_t kind = uint8_t(ubimilan::kSequence);
    const int holder = nested.AddSequence({
        0x00, 0x00, 0x90, kind, 0x40, 0x00, 0x00, 0xE0, 0x00, 0xFC, // the first's bytes start at the key
        0x90, kind, 0x40, 0x00, 0x00, 0xE0, 0x00, 0xFC,             // the second's too
        0xFC,
    });
    nested.AddPiece(0, {holder});
    MoveResource(nested, nested.AddSample({1, 2, 3, 4}), nested.Resource(holder) + 4 + 3);
    MoveResource(nested, nested.AddSample({1, 2, 3, 4}), nested.Resource(holder) + 4 + 11);
    const Rom nested_rom = nested.ToRom();
    const Layout nested_layout = ubimilan::Cut(nested_rom, Detect(nested_rom));
    XSFLIB_CHECK(nested_layout.sequences.empty());
    XSFLIB_CHECK(ubimilan::Join(nested_layout) == BytesOf(nested_rom));

    // A sequence that reads the bank's header stays. A move would leave zeros in the header.
    TestImage over_bank;
    const int beside_bank = over_bank.AddSequence({0x00, 0x00, 0xFC});
    over_bank.AddPiece(0, {beside_bank});
    const int on_bank = over_bank.AddSample({1, 2, 3, 4});
    PutSequence(over_bank, kBank - 6);
    MoveResource(over_bank, on_bank, kBank - 6);
    const Rom over_bank_rom = over_bank.ToRom();
    const Layout over_bank_layout = ubimilan::Cut(over_bank_rom, Detect(over_bank_rom));
    XSFLIB_CHECK(over_bank_layout.resources == std::vector<int>({beside_bank}));
    XSFLIB_CHECK(ubimilan::Join(over_bank_layout) == BytesOf(over_bank_rom));

    // So does a sequence that reads the resource table. Its first word is the table's last entry.
    TestImage over_table;
    const int beside_table = over_table.AddSequence({0x00, 0x00, 0xFC});
    over_table.AddPiece(0, {beside_table});
    const int on_table = over_table.AddSample({1, 2, 3, 4});
    const int last_entry = over_table.AddSample({1, 2, 3, 4});
    const uint32_t entry = kBank + 0x10 + 4 * uint32_t(last_entry);
    over_table.Put32(entry, kind);
    over_table.Put8(entry + 6, 0xFC);
    MoveResource(over_table, on_table, entry);
    const Rom over_table_rom = over_table.ToRom();
    const Layout over_table_layout = ubimilan::Cut(over_table_rom, Detect(over_table_rom));
    XSFLIB_CHECK(over_table_layout.resources == std::vector<int>({beside_table}));
    XSFLIB_CHECK(ubimilan::Join(over_table_layout) == BytesOf(over_table_rom));
}

void TestSequenceBeforeTheBank()
{
    // A sequence can end where the bank starts. The fields' places then lie in the run after it.
    TestImage image;
    image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0xFC})});
    const int before = image.AddSample({1, 2, 3, 4});
    PutSequence(image, kBank - 7);
    MoveResource(image, before, kBank - 7);
    const Rom rom = image.ToRom();
    const Layout layout = ubimilan::Cut(rom, Detect(rom));
    SEQLIB_REQUIRE(layout.sequences.size() == 2 && layout.pieces.size() == 5);
    XSFLIB_CHECK(std::holds_alternative<ntdlib::sar::SequencePiece>(layout.pieces[1]));
    SEQLIB_REQUIRE(!layout.fields.empty());
    for (const ntdlib::sar::Field& field : layout.fields)
    {
        XSFLIB_CHECK(field.from.piece == 2 && field.from.offset == 0);
        XSFLIB_CHECK(field.at.piece == 2);
    }

    XSFLIB_CHECK(ubimilan::Join(layout) == BytesOf(rom));
}

void TestGrowth()
{
    TwoPieces pieces;
    const Rom rom = pieces.image.ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = ubimilan::Cut(rom, info);
    Grow(layout.sequences[0], 4);
    const std::vector<uint8_t> grown_bytes = ubimilan::Join(layout);
    const Rom grown = RomOf(grown_bytes);
    const DriverInfo grown_info = Detect(grown);

    // The table's entry names the sequence's new place. The ROM's zeros don't end with 0xFF. The last bytes of the
    // image are zeros. The free space therefore lies inside them.
    const uint32_t address = ubimilan::ResourceAddress(grown, grown_info, pieces.first);
    XSFLIB_CHECK(address != ubimilan::ResourceAddress(rom, info, pieces.first));
    XSFLIB_CHECK(address == kRomBase + layout.free.start);
    XSFLIB_CHECK(grown.Size() == rom.Size());
    XSFLIB_CHECK(ubimilan::ResourceAddress(grown, grown_info, pieces.third) ==
                 ubimilan::ResourceAddress(rom, info, pieces.third));

    // The old place holds zeros.
    const uint32_t old_address = ubimilan::ResourceAddress(rom, info, pieces.first);
    for (uint32_t i = 0; i < 12; i++)
    {
        XSFLIB_CHECK(grown.U8(old_address + i) == 0);
    }

    // The grown ROM comes back from its cut, and its pieces play as before: the commands cost no time.
    XSFLIB_CHECK(ubimilan::Join(ubimilan::Cut(grown, grown_info)) == grown_bytes);
    ubimilan::Output before(rom, info, 0);
    ubimilan::Output after(grown, grown_info, 0);
    SEQLIB_REQUIRE(before.Valid() && after.Valid());
    bool same = true;
    bool sounded = false;
    for (int frame = 0; frame < 30; frame++)
    {
        before.Frame();
        after.Frame();
        same = same && before.Fifo() == after.Fifo();
        sounded = sounded || std::any_of(before.Fifo().begin(), before.Fifo().end(), [](int8_t b) { return b != 0; });
    }

    XSFLIB_CHECK(same && sounded);
}

void TestShrinking()
{
    // A sequence that fits its place stays there, with zeros after its bytes.
    TwoPieces pieces;
    const Rom rom = pieces.image.ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = ubimilan::Cut(rom, info);
    ntdlib::commands::Sequence& sequence = layout.sequences[1];
    sequence.items.erase(sequence.items.begin() + 4, sequence.items.begin() + 6);
    const Rom shrunk = RomOf(ubimilan::Join(layout));
    const uint32_t address = ubimilan::ResourceAddress(rom, info, pieces.third);
    const DriverInfo shrunk_info = Detect(shrunk);
    XSFLIB_CHECK(ubimilan::ResourceAddress(shrunk, shrunk_info, pieces.third) == address);
    XSFLIB_CHECK(shrunk.U8(address + 11) == 0xFC);
    XSFLIB_CHECK(shrunk.U8(address + 12) == 0 && shrunk.U8(address + 13) == 0 && shrunk.U8(address + 14) == 0);
}

void TestPlaces()
{
    // Each sequence's 16 bytes end on a word. The two places therefore lie side by side.
    TestImage image;
    const std::vector<uint8_t> commands = {0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xFC};
    const int a = image.AddSequence(commands);
    const int b = image.AddSequence(commands);
    const int c = image.AddSequence({0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00,
                                     0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xE0, 0x00, 0x00, 0xFC});
    image.AddPiece(0, {a});
    image.AddPiece(0, {b});
    image.AddPiece(0, {c});
    const Rom rom = image.ToRom();
    const DriverInfo info = Detect(rom);
    const uint32_t place_a = ubimilan::ResourceAddress(rom, info, a);
    const uint32_t place_b = ubimilan::ResourceAddress(rom, info, b);
    const uint32_t place_c = ubimilan::ResourceAddress(rom, info, c);
    XSFLIB_CHECK(place_b == place_a + 16);
    const auto join = [&](int grow_a, int grow_b, int grow_c)
    {
        Layout layout = ubimilan::Cut(rom, info);
        Grow(layout.sequences[0], grow_a);
        Grow(layout.sequences[1], grow_b);
        Grow(layout.sequences[2], grow_c);
        return RomOf(ubimilan::Join(layout));
    };
    const uint32_t free = kRomBase + ubimilan::Cut(rom, info).free.start;

    // A sequence that outgrows its place goes to the free space. The place of a sequence that stays takes nothing.
    Rom joined = join(1, 0, 0);
    DriverInfo joined_info = Detect(joined);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, a) == free);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, b) == place_b);

    // Neighbouring places count as one: a's 25 bytes fit neither place, but they fit both. b's 19 bytes then find no
    // room there, and go to the free space.
    joined = join(3, 1, 0);
    joined_info = Detect(joined);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, a) == place_a);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, b) == free);

    // The largest goes first. All three places lie side by side and hold 63 bytes: c's 34 bytes go first, a's 19 after
    // them, and b's 19 find no room.
    joined = join(1, 1, 1);
    joined_info = Detect(joined);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, c) == place_a);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, a) == place_a + 34);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, b) == free);

    // With b in its place, a's and c's places don't touch. c's 121 bytes take the free space, and a's 28 bytes c's
    // place of 31.
    joined = join(4, 0, 30);
    joined_info = Detect(joined);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, c) == free);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, a) == place_c);

    // In the free space, one sequence follows another.
    joined = join(30, 0, 30);
    joined_info = Detect(joined);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, c) == free);
    XSFLIB_CHECK(ubimilan::ResourceAddress(joined, joined_info, a) == free + 121);

    // Each ROM comes back from its cut.
    XSFLIB_CHECK(ubimilan::Join(ubimilan::Cut(joined, joined_info)) == BytesOf(joined));
}

void TestFreeSpace()
{
    // Without a run of 0xFF or of zeros at its end, the free space lies at the ROM's end, on a multiple of 4 bytes, and
    // the ROM grows to hold the sequence.
    TwoPieces pieces;
    std::vector<uint8_t> bytes = BytesOf(pieces.image.ToRom());
    bytes.push_back(0x11);
    bytes.push_back(0x22);
    const Rom rom = RomOf(bytes);
    const DriverInfo info = Detect(rom);
    Layout layout = ubimilan::Cut(rom, info);
    XSFLIB_CHECK(layout.free.start == ((bytes.size() + 3) & ~size_t(3)));
    XSFLIB_CHECK(ubimilan::Join(layout) == bytes);
    Grow(layout.sequences[0], 4);
    const Rom grown = RomOf(ubimilan::Join(layout));
    const DriverInfo grown_info = Detect(grown);
    XSFLIB_CHECK(ubimilan::ResourceAddress(grown, grown_info, pieces.first) == kRomBase + layout.free.start);

    // In a run of 0xFF, it lies 256 bytes or more into the run, on a multiple of 256 bytes, and the bytes that the ROM
    // grows by hold 0xFF up to a sequence that lies past its end.
    bytes = BytesOf(pieces.image.ToRom());
    const uint32_t end = uint32_t(bytes.size());
    bytes.resize(end + 0x10, 0xFF);
    const Rom padded = RomOf(bytes);
    const Layout padded_layout = ubimilan::Cut(padded, Detect(padded));
    XSFLIB_CHECK(padded_layout.free.padding == 0xFF);
    XSFLIB_CHECK(padded_layout.free.start % 256 == 0);
    XSFLIB_CHECK(padded_layout.free.start >= end + 256);
    Layout padded_grown = padded_layout;
    Grow(padded_grown.sequences[0], 4);
    const std::vector<uint8_t> grown_bytes = ubimilan::Join(padded_grown);
    XSFLIB_CHECK(grown_bytes.size() == padded_layout.free.start + 12 + 4 * 3);
    XSFLIB_CHECK(grown_bytes[end + 0x10] == 0xFF);

    // In a run of zeros, the bytes that the ROM grows by hold zeros.
    const Rom zeros = pieces.image.ToRom();
    XSFLIB_CHECK(ubimilan::Cut(zeros, Detect(zeros)).free.padding == 0);
}

void TestFreeSpacePastWhatTheDriverReads()
{
    // The run at the ROM's end can hold bytes that the driver reads. The free space lies past them, by the margin.
    const auto base = []
    {
        TestImage image;
        image.AddPiece(0, {image.AddSequence({0x00, 0x00, 0xFC})});
        return image;
    };

    // A sample's zeros. Its header holds its length of 0x400, and its points follow the header's 16 bytes.
    TestImage sample = base();
    const int moved_sample = sample.AddSample({1, 2, 3, 4});
    const uint32_t size = uint32_t(sample.ToRom().Size());
    const uint32_t at = kRomBase + size - 0x800;
    sample.Put8(at, uint8_t(ubimilan::kSample));
    sample.Put32(at + 4, 0x400);
    MoveResource(sample, moved_sample, at);
    const Rom sample_rom = sample.ToRom();
    XSFLIB_CHECK(ubimilan::Cut(sample_rom, Detect(sample_rom)).free.start >= at - kRomBase + 16 + 0x400 + 256);

    // A list of 300 sequences of resource 0, a piece's zeros. The header takes 8 bytes.
    TestImage piece = base();
    const int moved_piece = piece.AddPiece(0, {0});
    piece.Put8(at, uint8_t(ubimilan::kMusic));
    piece.Put16(at + 4, 300);
    MoveResource(piece, moved_piece, at);
    const Rom piece_rom = piece.ToRom();
    const DriverInfo piece_info = Detect(piece_rom);
    XSFLIB_CHECK(ubimilan::Cut(piece_rom, piece_info).free.start >= at - kRomBase + 8 + 2 * 300 + 256);

    // The kit's entries and the kit table's. Where they run past the ROM's end, the free space lies past its end too.
    const TestImage plain = base();
    const Rom plain_rom = plain.ToRom();
    DriverInfo kit_info = Detect(plain_rom);
    kit_info.kit = kRomBase + size - 0x100;
    XSFLIB_CHECK(ubimilan::Cut(plain_rom, kit_info).free.start >= size + 256);
    DriverInfo table_info = Detect(plain_rom);
    table_info.kit_table = kRomBase + size - 2;
    XSFLIB_CHECK(ubimilan::Cut(plain_rom, table_info).free.start >= size + 256);
}

void TestRefusals()
{
    TwoPieces pieces;
    const Rom rom = pieces.image.ToRom();
    const Layout layout = ubimilan::Cut(rom, Detect(rom));

    // A piece that names a sequence that the layout lacks.
    Layout bad = layout;
    bad.sequences.clear();
    bad.sequences.shrink_to_fit();
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // A piece of padding.
    bad = layout;
    bad.pieces.push_back(ntdlib::sar::Padding{4});
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // A field outside a run.
    bad = layout;
    bad.fields[0].at.piece = 1;
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));
    bad = layout;
    bad.fields[0].to.piece = 100;
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // A field that runs past its run's end.
    bad = layout;
    bad.fields[0].at.offset = uint32_t(std::get<std::vector<uint8_t>>(bad.pieces[0]).size()) - 2;
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // A place in a sequence, in a GBA ROM's pieces, which have no such places.
    bad = layout;
    bad.fields[0].from.sequence = true;
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // An item of a run, and an item past a sequence's first.
    bad = layout;
    bad.fields[0].to.piece = 0;
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));
    bad = layout;
    bad.fields[0].to.offset = 1;
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // A ROM past 32 MiB.
    bad = layout;
    bad.pieces.push_back(std::vector<uint8_t>(ubimilan::kLargestRom, 0));
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // Sequences that don't fit in 32 MiB.
    bad = layout;
    bad.free.start = ubimilan::kLargestRom - 4;
    Grow(bad.sequences[0], 4);
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));

    // An item that the driver doesn't have.
    bad = layout;
    bad.sequences[0].items.pop_back();
    XSFLIB_CHECK(Refuses([&] { ubimilan::Join(bad); }));
}

} // namespace

int main()
{
    TestComesBack();
    TestWithoutSequences();
    TestSharedSequence();
    TestSequencesThatStay();
    TestSequenceBeforeTheBank();
    TestGrowth();
    TestShrinking();
    TestPlaces();
    TestFreeSpace();
    TestFreeSpacePastWhatTheDriverReads();
    TestRefusals();
    return xsflib::testing::Summary("Ubisoft Milan's layout");
}
