// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a GBA ROM of Nintendo R&D2's driver (ntdlib/rd2/layout.h), on small carts built
// here. The expected ROMs come from the driver's rules: a table's entry holds a sequence's offset from the table's
// start, and nothing else names a sequence's bytes.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/rd2/layout.h"
#include "ntdlib/rd2/output.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/rd2_cart.h"

using xsflib::testing::Check;

namespace
{

namespace rd2 = ntdlib::rd2;
namespace sar = ntdlib::sar;
using namespace ntdlib::testing::rd2_cart;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using rd2::Layout;
using rd2::Rom;

constexpr Revision kRevision = Revision::kLinkToThePast;

// Where a run of 0xFF at a cart's end starts. It lies past the 64 KiB that each bank's offsets reach. The cart's table
// of banks ends where its first bank starts, and reads the bytes after the table as more banks.
constexpr uint32_t kRunStart = 0x30000;
constexpr uint32_t kRunEnd = 0x40000;

// A cart with two sequences side by side, and a sound effects' sequence: 8 bytes, 8 bytes and 8 bytes. `first_wait`
// sets the first sequence's wait.
Cart TwoSequences(uint16_t first_wait = 4)
{
    Cart cart;
    cart.Sequences({{TrackData().Wait(first_wait).End()}, {TrackData().End(), TrackData().End()}});
    cart.Effects({{TrackData().Wait(1).End(), {}}});
    return cart;
}

rd2::DriverInfo Detect(const Rom& rom)
{
    rd2::DriverInfo info;
    std::string error;
    Check(rd2::DetectDriver(rom, {}, info, error), error.c_str());
    return info;
}

std::vector<uint8_t> BytesOf(const Rom& rom)
{
    return std::vector<uint8_t>(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
}

Rom RomOf(std::vector<uint8_t> image)
{
    Rom rom;
    rom.Assign(std::move(image));
    return rom;
}

// Adds `size` bytes after the last item of `sequence`. No command reaches them.
void Grow(ntdlib::commands::Sequence& sequence, size_t size)
{
    Command data;
    data.kind = Kind::kData;
    data.bytes.assign(size, 0xFD);
    sequence.items.push_back(std::move(data));
}

// Whether `body` throws std::invalid_argument.
template <typename Body>
bool Refuses(Body body)
{
    try
    {
        body();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

void TestCut()
{
    const Rom rom = TwoSequences().ToRom();
    const rd2::DriverInfo info = Detect(rom);
    const Layout layout = rd2::Cut(rom, info);

    Check(layout.revision == kRevision, "the layout keeps the revision");
    Check(layout.sequences.size() == 3, "each sequence becomes a piece");
    Check(layout.pieces.size() == 6, "a run, two sequences side by side, a run, a sequence and a run");
    Check(std::holds_alternative<std::vector<uint8_t>>(layout.pieces[0]), "a run first");
    const auto& first = std::get<sar::SequencePiece>(layout.pieces[1]);
    const auto& second = std::get<sar::SequencePiece>(layout.pieces[2]);
    Check(first.sequence == 0 && first.room == 8 && second.sequence == 1 && second.room == 8, "each piece's room");
    Check(std::get<std::vector<uint8_t>>(layout.pieces[0]).size() == kSequenceData - kRomBase, "the run up to them");

    const uint32_t table = kSequences - kRomBase;
    Check(layout.fields.size() == 3, "a field for each table's entry");
    Check(layout.fields[0].at.piece == 0 && layout.fields[0].at.offset == table, "the first entry");
    Check(layout.fields[0].from.piece == 0 && layout.fields[0].from.offset == table, "counts from its table");
    Check(layout.fields[0].to.piece == 1 && layout.fields[0].to.offset == 0 && layout.fields[0].to.item,
          "and names the sequence's first item");
    Check(layout.fields[1].at.offset == table + 4 && layout.fields[1].from.offset == table &&
              layout.fields[1].to.piece == 2,
          "the second entry");
    Check(layout.fields[2].at.offset == kEffects - kRomBase && layout.fields[2].from.offset == kEffects - kRomBase &&
              layout.fields[2].to.piece == 4,
          "the effects' table counts from its start");

    Check(rd2::Join(layout) == BytesOf(rom), "the join gives the ROM back");
}

void TestSequenceMoves()
{
    const Rom rom = TwoSequences().ToRom();
    const rd2::DriverInfo info = Detect(rom);
    const std::vector<uint8_t> bytes = BytesOf(rom);

    // A sequence that takes less than its room stays there, with zeros after it.
    Layout layout = rd2::Cut(rom, info);
    layout.sequences[0].items.pop_back();
    Check(layout.sequences[0].items.back().kind == Kind::kEnd, "the padding goes");
    std::vector<uint8_t> expected = bytes;
    expected[kSequenceData - kRomBase + 7] = 0;
    Check(rd2::Join(layout) == expected, "a sequence that fits its place stays");

    // A sequence that outgrows its place goes to the free space, and its place holds zeros.
    layout = rd2::Cut(rom, info);
    Grow(layout.sequences[0], 9);
    Check(layout.free.start == 0x10100,
          "the free space starts past the ROM's end, as the bank's reach covers the rest");
    const Rom moved = RomOf(rd2::Join(layout));
    const uint32_t place = kRomBase + layout.free.start;
    Check(moved.Size() == layout.free.start + 17, "the ROM grows to hold it");
    Check(moved.U32(kSequences) == place - kSequences, "the table names its new place");
    Check(moved.U32(kSequences + 4) == kSequenceData + 8 - kSequences, "the other sequence's entry stays");
    for (uint32_t i = 0; i < 8; i++)
    {
        Check(moved.U8(kSequenceData + i) == 0, "its place holds zeros");
        Check(moved.U8(kSequenceData + 8 + i) == rom.U8(kSequenceData + 8 + i), "the next sequence stays");
    }
    for (uint32_t i = 0; i < 8; i++)
    {
        Check(moved.U8(place + i) == rom.U8(kSequenceData + i), "its bytes lie in the new place");
    }
    Check(moved.U8(place + 8) == 0xFD && moved.U8(place + 16) == 0xFD, "with the new bytes");

    const rd2::DriverInfo found = Detect(moved);
    Check(found.sequence_addresses.size() == 2 && found.sequence_addresses[0] == place, "detection finds it there");
    Check(found.sound_effects.size() == info.sound_effects.size(), "and the same sound effects");

    // Its cut and its join give the new ROM back.
    Check(rd2::Join(rd2::Cut(moved, found)) == BytesOf(moved), "the new ROM cuts and joins");

    // A sound effects' sequence moves through its table.
    layout = rd2::Cut(rom, info);
    Grow(layout.sequences[2], 16);
    const Rom effects = RomOf(rd2::Join(layout));
    const rd2::DriverInfo with_effects = Detect(effects);
    Check(with_effects.effect_sequence_addresses[0] == kRomBase + layout.free.start, "the effects' sequence moves");
    Check(with_effects.sound_effects[0].address == kRomBase + layout.free.start + 4, "its first track moves with it");
    Check(with_effects.sound_effects[1].address == 0, "its empty track stays empty");
}

void TestNeighbours()
{
    const Rom rom = TwoSequences().ToRom();
    const rd2::DriverInfo info = Detect(rom);

    // Two sequences side by side that outgrow their places. Their 16 bytes count as one place. The earlier sequence
    // goes there, and the next multiple of 4 leaves too little room for the other.
    Layout layout = rd2::Cut(rom, info);
    Grow(layout.sequences[0], 2);
    Grow(layout.sequences[1], 2);
    const Rom moved = RomOf(rd2::Join(layout));
    Check(moved.U32(kSequences) == kSequenceData - kSequences, "the first takes the places");
    Check(moved.U32(kSequences + 4) == kRomBase + layout.free.start - kSequences, "the second goes to the free space");

    // Of different sizes, the largest goes first. The next goes after it on a multiple of 4, where it has room.
    Cart three;
    TrackData waits;
    for (int i = 0; i < 13; i++)
    {
        waits.Wait(1);
    }
    three.Sequences({{waits.End()}, {TrackData().Wait(4).End()}, {TrackData().Wait(5).End()}});
    const Rom rom_of_three = three.ToRom();
    Layout of_three = rd2::Cut(rom_of_three, Detect(rom_of_three));
    Grow(of_three.sequences[0], 1);
    Grow(of_three.sequences[1], 1);
    Grow(of_three.sequences[2], 2);
    const Rom sized = RomOf(rd2::Join(of_three));
    Check(sized.U32(kSequences) == kSequenceData - kSequences, "the largest takes the places first");
    Check(sized.U32(kSequences + 4) == kSequenceData + 36 - kSequences, "the next lies after it, on a multiple of 4");
    Check(sized.U32(kSequences + 8) == kRomBase + of_three.free.start - kSequences, "the last goes to the free space");
}

void TestStaysAfterItsTable()
{
    // A table that lies between two sequences. A sequence that moves can't go before its table: a table's offsets are
    // positive, and detection reads the first entry for the table's size.
    Cart cart = TwoSequences();
    const Rom rom = cart.ToRom();
    const rd2::DriverInfo info = Detect(rom);
    const Layout cut = rd2::Cut(rom, info);

    const ntdlib::commands::Sequence small = cut.sequences[1];
    ntdlib::commands::Sequence large = small;
    Grow(large, 32);
    ntdlib::commands::Sequence bigger = small;
    Grow(bigger, 4);

    const std::vector<ntdlib::commands::Sequence> sequences = {large, bigger};
    const std::vector<sar::Piece> pieces = {
        sar::SequencePiece{0, 0, 32},
        std::vector<uint8_t>(16, 0xAB),
        sar::SequencePiece{1, 0, 8},
        std::vector<uint8_t>(16, 0xAB),
    };
    const std::vector<sar::Field> fields = {
        {{1, 0}, {1, 0}, {0, 0, true}},
        {{1, 4}, {1, 0}, {2, 0, true}},
    };
    rd2::FreeSpace free;
    free.start = 72;
    free.padding = 0xFF;
    const std::vector<uint8_t> image = rd2::Join(kRevision, sequences, pieces, fields, free);

    // Both outgrow their places. The large one goes to the free space. The smaller would fit in the first place, before
    // its table, and goes after the large one in the free space instead.
    const auto word = [&image](size_t at)
    {
        return uint32_t(image[at]) | uint32_t(image[at + 1]) << 8 | uint32_t(image[at + 2]) << 16 |
               uint32_t(image[at + 3]) << 24;
    };
    Check(word(32) == 72 - 32, "the large sequence lies in the free space");
    Check(word(36) == 72 + 40 - 32, "the smaller lies after its table");
    Check(image[32 + 16 + 8 + 16] == 0x02, "the free space starts where the layout says");
}

void TestFreeSpace()
{
    // A run of 0xFF at the ROM's end, past the banks' reach.
    Cart cart = TwoSequences();
    cart.Resize(kRunEnd);
    for (uint32_t a = 0x10000; a < kRunStart; a++)
    {
        cart.Put8(kRomBase + a, 0xAB);
    }
    for (uint32_t a = kRunStart; a < kRunEnd; a++)
    {
        cart.Put8(kRomBase + a, 0xFF);
    }
    Rom rom = cart.ToRom();
    Layout layout = rd2::Cut(rom, Detect(rom));
    Check(layout.free.start == kRunStart + 0x100 && layout.free.padding == 0xFF, "the free space lies in the run");
    Grow(layout.sequences[0], 9);
    const Rom inside = RomOf(rd2::Join(layout));
    Check(inside.Size() == kRunEnd, "a sequence in the run keeps the ROM's size");
    Check(inside.U32(kSequences) == kRomBase + layout.free.start - kSequences, "the sequence lies in the run");
    Check(inside.U8(kRomBase + layout.free.start - 1) == 0xFF, "the run stays before it");

    // A sequence past the ROM's end: 0xFF fills the bytes up to it.
    Layout past = layout;
    Grow(past.sequences[1], 0x10000);
    const Rom grown = RomOf(rd2::Join(past));
    Check(grown.Size() > kRunEnd, "the ROM grows");

    // No run at the ROM's end: the free space starts at its end, on a multiple of 4.
    cart.Resize(kRunEnd + 1);
    cart.Put8(kRomBase + kRunEnd, 0xAB);
    rom = cart.ToRom();
    layout = rd2::Cut(rom, Detect(rom));
    Check(layout.free.start == kRunEnd + 4, "the free space starts past the ROM's end");
    Grow(layout.sequences[0], 9);
    const Rom after = RomOf(rd2::Join(layout));
    Check(after.U32(kSequences) == kRomBase + kRunEnd + 4 - kSequences, "the sequence lies on a multiple of 4");
}

void TestFiller()
{
    // A ROM of 32 MiB with a run of 0xFF and a run of zeros inside, and a run that's too short.
    Cart cart = TwoSequences();
    cart.Resize(rd2::kLargestRom);
    for (uint32_t a = 0x10000; a < rd2::kLargestRom; a++)
    {
        cart.Put8(kRomBase + a, 0xAB);
    }
    const auto fill = [&cart](uint32_t from, uint32_t size, uint8_t byte)
    {
        for (uint32_t i = 0; i < size; i++)
        {
            cart.Put8(kRomBase + from + i, byte);
        }
    };
    fill(0x200000, 0x3000, 0);
    fill(0x300000, 0x2000, 0xFF);
    fill(0x400000, 0xFFF, 0xFF);
    const Rom rom = cart.ToRom();
    const rd2::DriverInfo info = Detect(rom);

    const std::vector<rd2::Span> runs = rd2::FillerRuns(rom, info);
    Check(runs.size() == 2, "two runs are long enough");
    Check(runs[0] == rd2::Span{0x300100, 0x2000 - 0x200}, "the run of 0xFF comes first");
    Check(runs[1] == rd2::Span{0x200100, 0x3000 - 0x200}, "the run of zeros follows");

    Layout layout = rd2::Cut(rom, info);
    Check(layout.free.filler == runs, "the cut keeps the filler runs");
    Grow(layout.sequences[0], 9);
    Check(Refuses([&layout] { rd2::Join(layout); }), "no room without the filler runs");

    rd2::JoinOptions options;
    options.filler = true;
    const Rom joined = RomOf(rd2::Join(layout, options));
    Check(joined.Size() == rom.Size(), "the ROM keeps its size");
    Check(joined.U32(kSequences) == kRomBase + 0x300100 - kSequences, "the sequence lies in the run of 0xFF");

    Grow(layout.sequences[0], 0x4000);
    Check(Refuses([&layout, &options] { rd2::Join(layout, options); }), "no filler run with room");
}

void TestSequencesThatStay()
{
    // Two entries that name one place share a piece.
    Cart shared = TwoSequences();
    shared.Put32(kSequences + 4, kSequenceData - kSequences);
    Rom rom = shared.ToRom();
    Layout layout = rd2::Cut(rom, Detect(rom));
    Check(layout.sequences.size() == 2, "entries that name one place share a piece");
    Check(layout.fields[0].to.piece == layout.fields[1].to.piece, "both fields name it");
    Check(rd2::Join(layout) == BytesOf(rom), "the join gives the ROM back");

    // A sequence whose track jumps into another's bytes: neither can move.
    Cart reading;
    reading.Sequences({{{0xF0, 0x0C, 0x00}}, {TrackData().Wait(1).End()}});
    rom = reading.ToRom();
    layout = rd2::Cut(rom, Detect(rom));
    Check(layout.sequences.empty(), "two sequences that read one place stay among the bytes");
    Check(layout.pieces.size() == 1 && layout.fields.empty(), "the ROM is one run");
    Check(rd2::Join(layout) == BytesOf(rom), "the join gives the ROM back");

    // A sequence kept as data, and a sequence off a multiple of 4.
    Cart data = TwoSequences();
    data.Put8(kSequenceData, 11);
    rom = data.ToRom();
    layout = rd2::Cut(rom, Detect(rom));
    Check(layout.sequences.size() == 2, "the decoder's sequence kept as data stays among the bytes");
    Check(rd2::Join(layout) == BytesOf(rom), "the join gives the ROM back");

    Cart odd = TwoSequences();
    odd.Put32(kSequences + 4, kSequenceData + 9 - kSequences);
    odd.Put8(kSequenceData + 9, 0);
    rom = odd.ToRom();
    layout = rd2::Cut(rom, Detect(rom));
    Check(layout.sequences.size() == 2, "a sequence off a multiple of 4 stays among the bytes");

    // A sequence that holds the start of a list of banks.
    Cart lists = TwoSequences();
    lists.Put32(kBankLists + 4, kSequenceData + 2 - kBankLists);
    rom = lists.ToRom();
    layout = rd2::Cut(rom, Detect(rom));
    Check(layout.sequences.size() == 2, "a sequence over the driver's data stays among the bytes");
    Check(rd2::Join(layout) == BytesOf(rom), "the join gives the ROM back");
}

void TestRefusals()
{
    const Rom rom = TwoSequences().ToRom();
    const Layout cut = rd2::Cut(rom, Detect(rom));

    Layout padding = cut;
    padding.pieces.emplace_back(sar::Padding{4});
    Check(Refuses([&padding] { rd2::Join(padding); }), "a piece of padding");

    Layout missing = cut;
    missing.sequences.pop_back();
    Check(Refuses([&missing] { rd2::Join(missing); }), "a piece without a sequence");

    Layout field = cut;
    field.fields[0].at = {1, 0};
    Check(Refuses([&field] { rd2::Join(field); }), "a field in a sequence");

    Layout outside = cut;
    outside.fields[0].to.offset = 1000;
    Check(Refuses([&outside] { rd2::Join(outside); }), "a field that names an item that a sequence lacks");

    Layout large = cut;
    std::get<std::vector<uint8_t>>(large.pieces[0]).resize(rd2::kLargestRom);
    Check(Refuses([&large] { rd2::Join(large); }), "a ROM past 32 MiB");
}

void TestPlaysTheSame()
{
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xC2, kLooped}).Note(60, 48, 0x7F).Wait(48).End(),
                     TrackData().Bytes({0xC2, kSquare}).Note(64, 24, 0x60).Wait(24).End()}});
    const Rom rom = cart.ToRom();
    const rd2::DriverInfo info = Detect(rom);
    Layout layout = rd2::Cut(rom, info);
    Grow(layout.sequences[0], 16);
    const Rom moved = RomOf(rd2::Join(layout));
    const rd2::DriverInfo found = Detect(moved);

    rd2::Output before(rom, info);
    rd2::Output after(moved, found);
    before.Play(0, 0);
    after.Play(0, 0);
    int sounding = 0;
    for (int frame = 0; frame < 60; frame++)
    {
        before.Frame();
        after.Frame();
        Check(before.FifoA() == after.FifoA() && before.FifoB() == after.FifoB(), "the moved sequence mixes the same");
        Check(before.SoundWrites().size() == after.SoundWrites().size(), "and stores the same");
        sounding += std::any_of(before.FifoA().begin(), before.FifoA().end(), [](int8_t b) { return b != 0; });
    }
    Check(sounding > 30, "the song sounds");
}

} // namespace

int main()
{
    TestCut();
    TestSequenceMoves();
    TestNeighbours();
    TestStaysAfterItsTable();
    TestFreeSpace();
    TestFiller();
    TestSequencesThatStay();
    TestRefusals();
    TestPlaysTheSame();

    return xsflib::testing::Summary("rd2_layout");
}
