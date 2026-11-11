// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a GBA ROM of Brownie Brown's driver (ntdlib/brownie/layout.h), on small carts built
// here. The expected ROMs come from the driver's rules: the song table holds each sound's list, a list holds its
// channels' data and tables as absolute addresses, and nothing else names a sound's bytes.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/brownie/layout.h"
#include "ntdlib/brownie/sequencer.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/brownie_cart.h"

namespace
{

using namespace ntdlib::testing::brownie_cart;

namespace brownie = ntdlib::brownie;
namespace sar = ntdlib::sar;
using brownie::Layout;
using brownie::Sequencer;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;

// Sound 2's list and data, far from sound 1's.
constexpr uint32_t kList2 = kRomBase + 0x4000;
constexpr uint32_t kData2 = kRomBase + 0x4010;

Cart TwoRegions(Revision revision = Revision::kSwordOfMana)
{
    Cart cart(true, revision);
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0xF1, 0x80, 0x89, 24, 26, 0xFF}, {}}});
    cart.MoveTo(kList2, kData2);
    cart.Sound(2, {{1, {0xF0, kPsgHold, 0xF1, 0x80, 0xF6, 0x00, 0xFF, 0x89, 36, 0xF7}, {7}}});
    return cart;
}

// Grows sequence `s` of `layout` by `bytes` bytes that no command reaches.
void Grow(Layout& layout, size_t s, size_t bytes)
{
    Command data;
    data.kind = Kind::kData;
    data.bytes.assign(bytes, 0xFD);
    layout.sequences[s].items.push_back(std::move(data));
}

// Takes item `item` out of `sequence`.
void Erase(ntdlib::commands::Sequence& sequence, size_t item)
{
    sequence.items.erase(sequence.items.begin() + std::ptrdiff_t(item));
    for (Command& other : sequence.items)
    {
        if (ntdlib::commands::HasTarget(other.kind) && !other.target_outside && other.target > item)
        {
            other.target--;
        }
    }
    for (ntdlib::commands::Label& label : sequence.labels)
    {
        label.item -= label.item > item ? 1 : 0;
    }
}

// The sequence that holds sound `sound`'s list.
uint32_t SequenceOf(const Layout& layout, const DriverInfo& info, int sound)
{
    return brownie::SoundPlaces(layout, info).at(sound).sequence;
}

// Returns true if sound `sound` gives the same stores in each frame of its first `frames` frames from both ROMs.
bool PlaysTheSame(const Rom& a, const Rom& b, const DriverInfo& info, const DriverInfo& other, int sound, int frames)
{
    Sequencer one(a, info, sound);
    Sequencer two(b, other, sound);
    for (int f = 0; f < frames; f++)
    {
        const auto& x = one.Step();
        const auto& y = two.Step();
        if (x.size() != y.size())
        {
            return false;
        }
        for (size_t i = 0; i < x.size(); i++)
        {
            if (x[i].address != y[i].address || x[i].size != y[i].size || x[i].value != y[i].value)
            {
                return false;
            }
        }
    }

    return true;
}

// A ROM's pieces join to give it back byte for byte, its song table stays in a run, and the fields name each sound's
// list.
void TestRoundTrip()
{
    for (const Revision revision : {Revision::kSwordOfMana, Revision::kMagicalVacation})
    {
        const Rom rom = TwoRegions(revision).ToRom();
        const DriverInfo info = Detect(rom);
        const Layout layout = brownie::Cut(rom, info);
        const std::vector<uint8_t> joined = brownie::Join(layout);
        SEQLIB_REQUIRE(joined.size() == rom.Size());
        SEQLIB_CHECK(std::equal(joined.begin(), joined.end(), rom.Ptr(kRomBase)));

        // Sound 0's list, then the regions of sound 1 and of sound 2.
        SEQLIB_CHECK_EQ(layout.sequences.size(), 3);
        SEQLIB_CHECK_EQ(layout.fields.size(), 3);
        const std::map<int, brownie::SoundPlace> places = brownie::SoundPlaces(layout, info);
        SEQLIB_CHECK_EQ(places.size(), 3);
        for (const auto& [sound, place] : places)
        {
            const std::vector<brownie::ListTrack> tracks =
                brownie::TracksOfList(layout.sequences[place.sequence], place.list);
            SEQLIB_CHECK_EQ(tracks.size(), sound == 0 ? 0 : 1);
        }

        // Each field holds its table entry's address and the place of the list.
        for (const sar::Field& field : layout.fields)
        {
            SEQLIB_CHECK(!field.at.item);
            SEQLIB_CHECK(field.to.item);
            SEQLIB_CHECK(std::holds_alternative<std::vector<uint8_t>>(layout.pieces[field.at.piece]));
        }
    }
}

// A region that shrinks stays in its place, with zeros after its bytes, and the addresses inside it follow its items.
void TestShrink()
{
    const Rom rom = TwoRegions().ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = brownie::Cut(rom, info);
    const uint32_t s = SequenceOf(layout, info, 2);
    ntdlib::commands::Sequence& sequence = layout.sequences[s];
    const brownie::Encoded encoded = brownie::EncodeSequence(sequence, info.revision, kList2);

    // F1's 2 bytes go, and so do the 2 bytes of zeros after the channel. The channel's 8 bytes then reach a word, and
    // the table follows them there.
    size_t duty = 0;
    while (encoded.offsets[duty] != kData2 - kList2 + 2)
    {
        duty++;
    }
    size_t zeros = duty;
    while (sequence.items[zeros].kind != Kind::kData)
    {
        zeros++;
    }
    Erase(sequence, zeros);
    Erase(sequence, duty);

    const std::vector<uint8_t> image = brownie::Join(layout);
    SEQLIB_CHECK_EQ(image.size(), rom.Size());
    Rom shrunk;
    shrunk.Assign(image);
    SEQLIB_CHECK_EQ(shrunk.U32(kSongTable + 4 * 2), kList2);
    SEQLIB_CHECK_EQ(shrunk.U32(kList2 + 4), kData2);
    SEQLIB_CHECK_EQ(shrunk.U32(kList2 + 8), kData2 + 8);
    SEQLIB_CHECK_EQ(shrunk.U8(kData2 + 2), 0xF6);
    SEQLIB_CHECK_EQ(shrunk.U32(kData2 + 8), kData2 + 5);
    SEQLIB_CHECK_EQ(shrunk.U32(kData2 + 12), 0);
}

// A region that outgrows its place moves whole. The song table's entry, its list's addresses and its table's words name
// the new place. Its old place holds zeros, and the sound plays as before.
void TestMove()
{
    const Rom rom = TwoRegions().ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = brownie::Cut(rom, info);
    const uint32_t s = SequenceOf(layout, info, 2);
    const auto size = uint32_t(brownie::EncodeSequence(layout.sequences[s], info.revision, kList2).bytes.size());

    Grow(layout, s, 9);
    const std::vector<uint8_t> image = brownie::Join(layout);
    Rom grown;
    grown.Assign(image);
    DriverInfo grown_info;
    std::string error;
    SEQLIB_REQUIRE(brownie::DetectDriver(grown, DriverOverrides(), grown_info, error));
    SEQLIB_CHECK_EQ(grown_info.song_table, info.song_table);
    SEQLIB_CHECK_EQ(grown_info.song_count, info.song_count);

    // The cart's ROM ends with zeros after its last sample, at 0x8B40. The moved region starts 256 bytes or more into
    // them, on a multiple of 256.
    const uint32_t list = grown.U32(kSongTable + 4 * 2);
    SEQLIB_CHECK_EQ(list, kRomBase + 0x8D00);
    SEQLIB_CHECK_EQ(grown.U32(list), 1);
    const uint32_t data = list + (kData2 - kList2);
    SEQLIB_CHECK_EQ(grown.U32(list + 4), data);
    const uint32_t table = grown.U32(list + 8);
    SEQLIB_CHECK_EQ(table, data + 12);
    SEQLIB_CHECK_EQ(grown.U32(table), data + 7);

    // The old place holds zeros.
    for (uint32_t i = 0; i < size; i++)
    {
        SEQLIB_CHECK_EQ(grown.U8(kList2 + i), 0);
    }

    // Sound 1's entry and bytes stay.
    SEQLIB_CHECK_EQ(grown.U32(kSongTable + 4), rom.U32(kSongTable + 4));
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 1, 40));
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 2, 60));

    // The grown ROM's pieces give it back.
    const Layout again = brownie::Cut(grown, grown_info);
    SEQLIB_CHECK(brownie::Join(again) == image);
}

// A region that moves goes into another moved region's place when it fits there, and keeps its remainder modulo 4.
void TestMoveIntoPlace()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}}});
    cart.MoveTo(kRomBase + 0x4000, kRomBase + 0x4010);
    cart.Sound(2, {{1, {0xF0, kPsgHold, 0x89, 26, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = brownie::Cut(rom, info);

    // Sound 1's region is far larger than sound 2's. Both grow. The larger moves first, to the free space, and the
    // smaller then finds the larger's old place.
    const uint32_t larger = SequenceOf(layout, info, 1);
    const uint32_t smaller = SequenceOf(layout, info, 2);
    Grow(layout, larger, 3);
    Grow(layout, smaller, 3);
    const std::vector<uint8_t> image = brownie::Join(layout);
    Rom grown;
    grown.Assign(image);
    const uint32_t larger_list = grown.U32(kSongTable + 4);
    const uint32_t smaller_list = grown.U32(kSongTable + 8);
    SEQLIB_CHECK_EQ(larger_list, kRomBase + 0x8D00);
    SEQLIB_CHECK_EQ(smaller_list, kSongLists + 4);
    DriverInfo grown_info;
    std::string error;
    SEQLIB_REQUIRE(brownie::DetectDriver(grown, DriverOverrides(), grown_info, error));
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 1, 30));
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 2, 30));
}

// Two sounds of a region keep their lists when the region moves: the song table's fields name each list.
void TestSharedRegionMoves()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}}});
    cart.Sound(2, {{1, {0xF0, kPsgHold, 0x89, 26, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = brownie::Cut(rom, info);
    const uint32_t s = SequenceOf(layout, info, 1);
    SEQLIB_CHECK_EQ(SequenceOf(layout, info, 2), s);

    Grow(layout, s, 9);
    Rom grown;
    grown.Assign(brownie::Join(layout));
    DriverInfo grown_info;
    std::string error;
    SEQLIB_REQUIRE(brownie::DetectDriver(grown, DriverOverrides(), grown_info, error));
    SEQLIB_CHECK(grown.U32(kSongTable + 4) != rom.U32(kSongTable + 4));
    SEQLIB_CHECK_EQ(grown.U32(kSongTable + 8) - grown.U32(kSongTable + 4),
                    rom.U32(kSongTable + 8) - rom.U32(kSongTable + 4));
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 1, 30));
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 2, 30));
}

// A region that moves past the ROM's end keeps its remainder modulo 4, and the ROM's padding fills the bytes before it.
void TestMovePastEnd()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}}});

    // Sound 2's data lies before its list, at an address with a remainder of 1.
    cart.MoveTo(kRomBase + 0x4000, kRomBase + 0x3FF1);
    cart.Sound(2, {{1, {0xF0, kPsgHold, 0x89, 26, 0xFF}, {}}});
    const Rom base = cart.ToRom();

    // The ROM ends 8 bytes of 0xFF after its last byte that isn't 0.
    std::vector<uint8_t> bytes(base.Ptr(kRomBase), base.Ptr(kRomBase) + base.Size());
    while (bytes.back() == 0)
    {
        bytes.pop_back();
    }
    bytes.insert(bytes.end(), 8, 0xFF);
    const size_t old_size = bytes.size();
    Rom rom;
    rom.Assign(bytes);
    const DriverInfo info = Detect(rom);
    Layout layout = brownie::Cut(rom, info);
    SEQLIB_CHECK_EQ(layout.free.padding, 0xFF);

    Grow(layout, SequenceOf(layout, info, 2), 40);
    Rom grown;
    grown.Assign(brownie::Join(layout));
    DriverInfo grown_info;
    std::string error;
    SEQLIB_REQUIRE(brownie::DetectDriver(grown, DriverOverrides(), grown_info, error));
    const uint32_t list = grown.U32(kSongTable + 8);
    const uint32_t data = grown.U32(list + 4);
    SEQLIB_CHECK_EQ(data % 4, 1);
    SEQLIB_CHECK_EQ(list - data, 0x4000 - 0x3FF1);
    SEQLIB_CHECK(data > kRomBase + old_size);
    for (uint32_t at = kRomBase + uint32_t(old_size); at < data; at++)
    {
        SEQLIB_CHECK_EQ(grown.U8(at), 0xFF);
    }
    SEQLIB_CHECK(PlaysTheSame(rom, grown, info, grown_info, 2, 30));
}

// A region that the decoder keeps as data stays among the ROM's bytes, and the pieces still give the ROM back. A cut of
// a ROM whose regions all stay gives one run and no fields.
void TestOpaqueRegion()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF2, 0x10, 0x89, 0x20, 0xF8, 0x00, 0xFF}, {1}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Layout layout = brownie::Cut(rom, info);
    SEQLIB_CHECK(brownie::Join(layout) == std::vector<uint8_t>(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size()));
    const std::map<int, brownie::SoundPlace> places = brownie::SoundPlaces(layout, info);
    SEQLIB_CHECK(!places.contains(1));
    SEQLIB_CHECK(places.contains(0));
}

// Join refuses what the driver's ROM can't hold.
void TestRefusals()
{
    const Rom rom = TwoRegions().ToRom();
    const DriverInfo info = Detect(rom);
    const Layout layout = brownie::Cut(rom, info);
    const auto refused = [](const Layout& l)
    {
        try
        {
            brownie::Join(l);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }

        return false;
    };

    // A piece of padding.
    Layout bad = layout;
    bad.pieces.emplace_back(sar::Padding{4});
    SEQLIB_CHECK(refused(bad));

    // A sequence piece without its sequence.
    bad = layout;
    bad.pieces.emplace_back(sar::SequencePiece{uint32_t(bad.sequences.size()), 0, 4});
    SEQLIB_CHECK(refused(bad));

    // A field whose place lies past its run, and a field whose item the region lacks.
    bad = layout;
    bad.fields[0].at.offset = 0x7FFFFFFF;
    SEQLIB_CHECK(refused(bad));
    bad = layout;
    bad.fields[0].to.offset = 1000000;
    SEQLIB_CHECK(refused(bad));

    // A run past the largest ROM that the GBA maps.
    bad = layout;
    bad.pieces.emplace_back(std::vector<uint8_t>(brownie::kLargestRom, 0));
    SEQLIB_CHECK(refused(bad));

    // A region that outgrows its place and the largest ROM.
    bad = layout;
    Grow(bad, SequenceOf(bad, info, 2), brownie::kLargestRom);
    SEQLIB_CHECK(refused(bad));
}

// The free space lies past the last byte that the driver reads, in the run at the ROM's end. A ROM that ends with other
// bytes puts it at its end, on a multiple of 4.
void TestFreeSpace()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0x89, 24, 0xFF}, {}}});
    Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Layout zeros = brownie::Cut(rom, info);
    SEQLIB_CHECK_EQ(zeros.free.padding, 0);
    SEQLIB_CHECK_EQ(zeros.free.start, 0x8D00);

    // A last byte other than 0xFF and 0 leaves no run.
    std::vector<uint8_t> image(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    image.back() = 0x12;
    image.push_back(0x34);
    rom.Assign(image);
    const DriverInfo other = Detect(rom);
    const Layout none = brownie::Cut(rom, other);
    SEQLIB_CHECK_EQ(none.free.start, (image.size() + 3) / 4 * 4);
}

} // namespace

int main()
{
    TestRoundTrip();
    TestShrink();
    TestMove();
    TestMoveIntoPlace();
    TestSharedRegionMoves();
    TestMovePastEnd();
    TestOpaqueRegion();
    TestRefusals();
    TestFreeSpace();
    return xsflib::testing::Summary("brownie_layout");
}
