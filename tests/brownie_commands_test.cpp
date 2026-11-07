// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the commands of Brownie Brown's driver (ntdlib/brownie/commands.h), on small
// carts built here. The expected items and bytes come from the driver's format: each command's length in the routines
// that its table names, a list's three words, and a table's words.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "seqlib/testing/check.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "brownie_cart.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/brownie/commands.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/commands/sequence.h"

namespace
{

using namespace brownie_testing;

namespace brownie = ntdlib::brownie;
using brownie::Decoded;
using brownie::DecodeRegion;
using brownie::DecodeSong;
using brownie::Encoded;
using brownie::EncodeSequence;
using brownie::FindRegions;
using brownie::kAddressWord;
using brownie::kKeptBytes;
using brownie::Region;
using brownie::TracksOfList;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::Sequence;

// The address where the second sound's list starts: sound 0's list takes a word.
constexpr uint32_t kFirstList = kSongLists + 4;

// Returns the item at `offset` of `encoded`, or the number of items.
size_t ItemAt(const Encoded& encoded, uint32_t offset)
{
    for (size_t i = 0; i + 1 < encoded.offsets.size(); i++)
    {
        if (encoded.offsets[i] == offset)
        {
            return i;
        }
    }

    return encoded.offsets.size() - 1;
}

// Returns true if the encoder gives the region's bytes back at the region's start.
bool ComesBack(const Rom& rom, const Decoded& decoded, Revision revision, const Region& region)
{
    const Encoded encoded = EncodeSequence(decoded.sequence, revision, region.start);
    if (encoded.bytes.size() != region.size)
    {
        return false;
    }

    for (uint32_t i = 0; i < region.size; i++)
    {
        if (encoded.bytes[i] != rom.U8(region.start + i))
        {
            return false;
        }
    }

    return true;
}

// Returns the region that holds sound `sound`.
Region RegionOf(const Rom& rom, const DriverInfo& info, int sound)
{
    for (const Region& region : FindRegions(rom, info))
    {
        for (const int s : region.sounds)
        {
            if (s == sound)
            {
                return region;
            }
        }
    }

    return {};
}

// Returns the value of `item` in the driver's units.
int64_t ValueOf(const Command& item)
{
    return ntdlib::commands::FromUnit(item.value.value, brownie::ScaleOf(item.kind, Revision::kSwordOfMana));
}

// Returns true if TracksOfList refuses `list`.
bool TracksOfListRefused(const Sequence& sequence, uint32_t list)
{
    try
    {
        TracksOfList(sequence, list);
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// Returns true if encoding `sequence` throws std::invalid_argument.
bool Refused(const Sequence& sequence, Revision revision, uint32_t base)
{
    try
    {
        EncodeSequence(sequence, revision, base);
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// Every command of a PSG channel and a sample channel takes the kind that the header gives it, and the bytes come back.
void TestKinds()
{
    Cart cart;
    const std::vector<uint8_t> psg = {
        0x89,       // 0: a length byte
        0xF2, 0x50, // 1: the volume
        0xF3, 0xFE, // 3: the transpose
        0xF9, 0x03, // 5: the detune
        0xF0, 0x01, // 7: the envelope
        0xF1, 0x40, // 9: NRx1
        0xFA, 0x11, // 11: the sweep
        0xFB,       // 13: no sweep
        0xFD, 0x11, // 14: the pan
        0xFC, 0x00, // 16: the wave
        0xF4, 0x02, // 18: a repeat's start
        0x30,       // 20: a note
        0xFE,       // 21: a hold
        0xF5,       // 22: a repeat's end
        0xED,       // 23: a PSG channel's rest
        0xF6, 0x00, // 24: a call
        0xF8, 0x01, // 26: a jump
        0x32,       // 28: the call's note
        0xF7,       // 29: the return
        0xE0, 0x02, // 30: the release
        0xFF,       // 32: the end
    };
    const std::vector<uint8_t> samples = {
        0xE1, 0x00, // 0: the sample set
        0xEA, 0x55, // 2: the pan
        0xE2, 0x01, // 4: the mixer's mode
        0xEE, 0x00, // 6: the set of lengths
        0x10,       // 8: a note
        0xEC,       // 9: a sample channel's rest
        0xFF,       // 10: the end
    };
    cart.Sound(1, {{0, psg, {28, 30}}, {8, samples, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    const Decoded decoded = DecodeRegion(rom, info, region);
    SEQLIB_REQUIRE(!decoded.opaque);
    SEQLIB_CHECK(ComesBack(rom, decoded, info.revision, region));

    const Encoded encoded = EncodeSequence(decoded.sequence, info.revision, region.start);
    const uint32_t data = kSequences - region.start;
    const auto item = [&](uint32_t offset) -> const Command&
    {
        const size_t i = ItemAt(encoded, data + offset);
        SEQLIB_CHECK(i < decoded.sequence.items.size());
        return decoded.sequence.items[i];
    };

    SEQLIB_CHECK(item(0).kind == Kind::kOther);
    SEQLIB_CHECK_EQ(item(0).encoding.opcode, 0x89);
    SEQLIB_CHECK(item(0).bytes.empty());
    SEQLIB_CHECK(item(1).kind == Kind::kVolume);
    SEQLIB_CHECK_EQ(ValueOf(item(1)), 0x50);
    SEQLIB_CHECK(item(3).kind == Kind::kTranspose);
    SEQLIB_CHECK_EQ(ValueOf(item(3)), -2);
    SEQLIB_CHECK(item(5).kind == Kind::kTune);
    SEQLIB_CHECK_EQ(ValueOf(item(5)), 3);
    SEQLIB_CHECK(item(7).kind == Kind::kProgram);
    SEQLIB_CHECK_EQ(item(7).index, 1);
    SEQLIB_CHECK_EQ(ValueOf(item(7)), 1);
    SEQLIB_CHECK(item(9).kind == Kind::kProgram);
    SEQLIB_CHECK_EQ(item(9).index, 2);
    SEQLIB_CHECK(item(11).kind == Kind::kSweep);
    SEQLIB_CHECK_EQ(item(11).index, 0);
    SEQLIB_CHECK_EQ(ValueOf(item(11)), 0x11);
    SEQLIB_CHECK(item(13).kind == Kind::kSweep);
    SEQLIB_CHECK_EQ(item(13).index, 1);
    SEQLIB_CHECK(item(14).kind == Kind::kPan);
    SEQLIB_CHECK_EQ(item(14).index, 1);
    SEQLIB_CHECK(item(16).kind == Kind::kProgram);
    SEQLIB_CHECK_EQ(item(16).index, 3);
    SEQLIB_CHECK(item(18).kind == Kind::kLoopStart);
    SEQLIB_CHECK_EQ(ValueOf(item(18)), 2);
    SEQLIB_CHECK(item(20).kind == Kind::kNote);
    SEQLIB_CHECK_EQ(item(20).key, 0x30);
    SEQLIB_CHECK(item(21).kind == Kind::kWait);
    SEQLIB_CHECK(item(22).kind == Kind::kLoopEnd);
    SEQLIB_CHECK(item(23).kind == Kind::kNoteOff);
    SEQLIB_CHECK_EQ(item(23).index, 1);
    SEQLIB_CHECK(item(24).kind == Kind::kCall);
    SEQLIB_CHECK_EQ(item(24).index, 0);
    SEQLIB_CHECK_EQ(encoded.offsets[item(24).target], data + 28);
    SEQLIB_CHECK(item(26).kind == Kind::kJump);
    SEQLIB_CHECK_EQ(item(26).index, 1);
    SEQLIB_CHECK_EQ(encoded.offsets[item(26).target], data + 30);
    SEQLIB_CHECK(item(29).kind == Kind::kReturn);
    SEQLIB_CHECK(item(30).kind == Kind::kRelease);
    SEQLIB_CHECK_EQ(ValueOf(item(30)), 2);
    SEQLIB_CHECK(item(32).kind == Kind::kEnd);
    SEQLIB_CHECK_EQ(item(32).index, 0);

    // The second channel's data follows the first's table of two words, on a word.
    const uint32_t second = data + ((uint32_t(psg.size()) + 3) & ~3u) + 2 * 4 + 4;
    SEQLIB_CHECK(item(second - data).kind == Kind::kProgram);
    SEQLIB_CHECK_EQ(item(second - data).index, 0);
    SEQLIB_CHECK(item(second - data + 2).kind == Kind::kPan);
    SEQLIB_CHECK_EQ(item(second - data + 2).index, 0);
    SEQLIB_CHECK(item(second - data + 4).kind == Kind::kOther);
    SEQLIB_CHECK_EQ(item(second - data + 4).encoding.opcode, 0xE2);
    SEQLIB_CHECK_EQ(item(second - data + 4).bytes.size(), 1);
    SEQLIB_CHECK(item(second - data + 6).kind == Kind::kOther);
    SEQLIB_CHECK_EQ(item(second - data + 6).encoding.opcode, 0xEE);
    SEQLIB_CHECK(item(second - data + 9).kind == Kind::kNoteOff);
    SEQLIB_CHECK_EQ(item(second - data + 9).index, 0);
}

// A list holds a channel's number and two addresses, and a word of 0xFF ends it. TracksOfList reads it back.
void TestLists()
{
    Cart cart;
    cart.Sound(1, {{3, {0x89, 0x10, 0xFF}, {}}, {9, {0x89, 0x20, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Decoded decoded = DecodeSong(rom, info, 1);
    const Region region = RegionOf(rom, info, 1);
    SEQLIB_CHECK_EQ(region.start, kFirstList);
    SEQLIB_REQUIRE(decoded.lists.size() == 1);
    SEQLIB_CHECK(!decoded.sequence.labels.empty());

    const std::vector<brownie::ListTrack> tracks = TracksOfList(decoded.sequence, decoded.lists[0]);
    SEQLIB_REQUIRE(tracks.size() == 2);
    SEQLIB_CHECK_EQ(tracks[0].channel, 3);
    SEQLIB_CHECK_EQ(tracks[1].channel, 9);

    const Encoded encoded = EncodeSequence(decoded.sequence, info.revision, region.start);
    SEQLIB_CHECK_EQ(encoded.offsets[tracks[0].entry], kSequences - region.start);
    const Command& table = decoded.sequence.items[tracks[0].table];
    SEQLIB_CHECK(table.kind == Kind::kJump);
    SEQLIB_CHECK_EQ(table.encoding.opcode, kAddressWord);
    SEQLIB_CHECK(!TracksOfListRefused(decoded.sequence, decoded.lists[0]));

    // An item that doesn't start a list.
    SEQLIB_CHECK(TracksOfListRefused(decoded.sequence, tracks[0].entry));

    // The words of the list.
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&encoded.bytes[0]), 3);
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&encoded.bytes[4]), kSequences);
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&encoded.bytes[12]), 9);
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&encoded.bytes[24]), 0xFF);

    // The same items at another address name that address's bytes, and only the addresses change.
    const uint32_t moved = region.start + 0x1000;
    const Encoded elsewhere = EncodeSequence(decoded.sequence, info.revision, moved);
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&elsewhere.bytes[4]), kSequences + 0x1000);
    SEQLIB_CHECK_EQ(elsewhere.bytes.size(), encoded.bytes.size());
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&elsewhere.bytes[12]), 9);
}

// Sounds that read each other's bytes share a region, and a sound far from them lies in another region.
void TestRegions()
{
    Cart cart;
    cart.Sound(1, {{0, {0x89, 0x10, 0xFF}, {}}});
    cart.Sound(2, {{1, {0x89, 0x20, 0xFF}, {}}});

    // Sound 3's list lies far from sound 1's, and names sound 1's data.
    const uint32_t shared_list = kRomBase + 0x4000;
    cart.Put32(kSongTable + 4 * 3, shared_list);
    cart.Put32(shared_list, 4);
    cart.Put32(shared_list + 4, kSequences);
    cart.Put32(shared_list + 8, kSequences + 4);
    cart.Put32(shared_list + 12, 0xFF);

    cart.MoveTo(kRomBase + 0x5000, kRomBase + 0x5100);
    cart.Sound(4, {{0, {0x89, 0x30, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const std::vector<Region> regions = FindRegions(rom, info);

    SEQLIB_REQUIRE(regions.size() == 3);
    SEQLIB_CHECK_EQ(regions[0].start, kSongLists);
    SEQLIB_CHECK_EQ(regions[0].size, 4);
    SEQLIB_REQUIRE(regions[0].sounds.size() == 1);
    SEQLIB_CHECK_EQ(regions[0].sounds[0], 0);

    // From sound 1's list up to the end of sound 3's list: sound 3's channel makes the three sounds read one hull.
    SEQLIB_CHECK_EQ(regions[1].start, kFirstList);
    SEQLIB_CHECK_EQ(regions[1].start + regions[1].size, shared_list + 16);
    SEQLIB_REQUIRE(regions[1].sounds.size() == 3);
    SEQLIB_CHECK_EQ(regions[1].sounds[0], 1);
    SEQLIB_CHECK_EQ(regions[1].sounds[1], 2);
    SEQLIB_CHECK_EQ(regions[1].sounds[2], 3);

    // A list of 16 bytes and a channel's 3 bytes.
    SEQLIB_CHECK_EQ(regions[2].start, kRomBase + 0x5000);
    SEQLIB_CHECK_EQ(regions[2].size, 0x103);
    SEQLIB_REQUIRE(regions[2].sounds.size() == 1);
    SEQLIB_CHECK_EQ(regions[2].sounds[0], 4);

    for (const Region& region : regions)
    {
        const Decoded decoded = DecodeRegion(rom, info, region);
        SEQLIB_CHECK(!decoded.opaque);
        SEQLIB_CHECK(ComesBack(rom, decoded, info.revision, region));
    }

    // A label names each sound's list, and the bytes between the lists and the data stay as data.
    const Decoded decoded = DecodeRegion(rom, info, regions[1]);
    SEQLIB_REQUIRE(decoded.lists.size() == 3);
    const Encoded encoded = EncodeSequence(decoded.sequence, info.revision, regions[1].start);
    SEQLIB_CHECK_EQ(encoded.offsets[decoded.lists[0]], 0);
    SEQLIB_CHECK_EQ(encoded.offsets[decoded.lists[2]], shared_list - regions[1].start);
    SEQLIB_CHECK_EQ(decoded.sequence.labels.size(), 3);
    bool data = false;
    for (const Command& item : decoded.sequence.items)
    {
        data = data || item.kind == Kind::kData;
    }
    SEQLIB_CHECK(data);
}

// A channel's FF can also be the first byte of a list's word of 0xFF. The item serves as both, and the bytes come back.
void TestSharedEnd()
{
    Cart cart;
    cart.MoveTo(kRomBase + 0x4000, kRomBase + 0x4100);
    cart.Sound(1, {});
    cart.Put32(kRomBase + 0x4000, 2);
    cart.Put32(kRomBase + 0x4004, kRomBase + 0x400C);
    cart.Put32(kRomBase + 0x4008, kRomBase + 0x4010);
    cart.Put32(kRomBase + 0x400C, 0xFF);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    const Decoded decoded = DecodeRegion(rom, info, region);
    SEQLIB_REQUIRE(!decoded.opaque);
    SEQLIB_CHECK(ComesBack(rom, decoded, info.revision, region));

    const std::vector<brownie::ListTrack> tracks = TracksOfList(decoded.sequence, decoded.lists[0]);
    SEQLIB_REQUIRE(tracks.size() == 1);
    const Command& end = decoded.sequence.items[tracks[0].entry];
    SEQLIB_CHECK(end.kind == Kind::kEnd);
    SEQLIB_CHECK_EQ(end.index, 1);
}

// In the Magical Vacation revision, E2-E9 are F2-F9 again and EF is FF. An item that a channel reads through such a
// byte keeps the byte.
void TestVacation()
{
    Cart cart(true, Revision::kMagicalVacation);
    const std::vector<uint8_t> data = {
        0xE2, 0x40, // 0: the volume, through F2's other byte
        0xF2, 0x41, // 2: the volume
        0xE3, 0xFE, // 4: the transpose
        0x89,       // 6: a length byte
        0x10,       // 7: a note
        0xEF,       // 8: the end
    };
    cart.Sound(1, {{8, data, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    const Decoded decoded = DecodeRegion(rom, info, region);
    SEQLIB_REQUIRE(!decoded.opaque);
    SEQLIB_CHECK(ComesBack(rom, decoded, info.revision, region));

    const Encoded encoded = EncodeSequence(decoded.sequence, info.revision, region.start);
    const uint32_t at = kSequences - region.start;
    const Command& kept = decoded.sequence.items[ItemAt(encoded, at)];
    SEQLIB_CHECK(kept.kind == Kind::kVolume);
    SEQLIB_CHECK_EQ(ValueOf(kept), 0x40);
    SEQLIB_CHECK_EQ(kept.encoding.opcode, kKeptBytes | 0xE2);
    const Command& plain = decoded.sequence.items[ItemAt(encoded, at + 2)];
    SEQLIB_CHECK(plain.kind == Kind::kVolume);
    SEQLIB_CHECK_EQ(plain.encoding.opcode & kKeptBytes, 0);
    const Command& end = decoded.sequence.items[ItemAt(encoded, at + 8)];
    SEQLIB_CHECK(end.kind == Kind::kEnd);
    SEQLIB_CHECK_EQ(end.encoding.opcode, kKeptBytes | 0xEF);
}

// A new value changes the value's byte alone. A new note moves the commands after it, the words that name them follow,
// and a table's words stay on a multiple of 4.
void TestEdits()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF2, 0x10, 0x89, 0x20, 0xF8, 0x00, 0x21, 0xFF}, {6}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    Decoded decoded = DecodeRegion(rom, info, region);
    SEQLIB_REQUIRE(!decoded.opaque);
    const Encoded before = EncodeSequence(decoded.sequence, info.revision, region.start);
    const uint32_t data = kSequences - region.start;

    const size_t volume = ItemAt(before, data);
    decoded.sequence.items[volume].value.value =
        ntdlib::commands::ToUnit(0x33, brownie::ScaleOf(Kind::kVolume, info.revision));
    const size_t note = ItemAt(before, data + 3);
    decoded.sequence.items[note].key = 0x22;
    const Encoded changed = EncodeSequence(decoded.sequence, info.revision, region.start);
    SEQLIB_REQUIRE(changed.bytes.size() == before.bytes.size());
    for (size_t i = 0; i < changed.bytes.size(); i++)
    {
        const uint8_t expected = i == data + 1 ? 0x33 : i == data + 3 ? 0x22 : before.bytes[i];
        SEQLIB_CHECK_EQ(changed.bytes[i], expected);
    }

    // A note goes in before the jump. The channel's data is 8 bytes and its table takes a word after them, so the table
    // moves up by a word.
    Sequence longer = DecodeRegion(rom, info, region).sequence;
    const size_t jump = ItemAt(before, data + 4);
    Command added;
    added.kind = Kind::kNote;
    added.key = 0x24;
    longer.items.insert(longer.items.begin() + std::ptrdiff_t(jump), added);
    for (Command& item : longer.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target >= jump)
        {
            item.target++;
        }
    }
    for (ntdlib::commands::Label& label : longer.labels)
    {
        label.item += label.item >= jump ? 1 : 0;
    }
    const Encoded grown = EncodeSequence(longer, info.revision, region.start);
    SEQLIB_CHECK_EQ(grown.bytes[data + 4], 0x24);
    SEQLIB_CHECK_EQ(grown.bytes[data + 5], 0xF8);
    const uint32_t table = data + 12;
    SEQLIB_CHECK_EQ((region.start + table) % 4, 0);
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&grown.bytes[table]), region.start + data + 7);
}

// A table whose first word is no call's or jump's starts with bytes of data. A new note moves the table, and it stays
// on a multiple of 4.
void TestTableAfterEdit()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF2, 0x10, 0x89, 0x20, 0xF8, 0x01, 0x21, 0xFF}, {0, 6}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    Sequence longer = DecodeRegion(rom, info, region).sequence;
    const Encoded before = EncodeSequence(longer, info.revision, region.start);
    const uint32_t data = kSequences - region.start;

    const size_t jump = ItemAt(before, data + 4);
    Command added;
    added.kind = Kind::kNote;
    added.key = 0x24;
    longer.items.insert(longer.items.begin() + std::ptrdiff_t(jump), added);
    for (Command& item : longer.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target >= jump)
        {
            item.target++;
        }
    }
    for (ntdlib::commands::Label& label : longer.labels)
    {
        label.item += label.item >= jump ? 1 : 0;
    }

    const auto open = std::find_if(longer.items.begin(), longer.items.end(),
                                   [](const Command& item) { return item.kind == Kind::kOpenTrack; });
    SEQLIB_REQUIRE(open != longer.items.end() && open + 1 != longer.items.end());
    const uint32_t table_item = (open + 1)->target;
    SEQLIB_REQUIRE(table_item < longer.items.size() && longer.items[table_item].kind == Kind::kData);
    const Encoded grown = EncodeSequence(longer, info.revision, region.start);
    const uint32_t table = grown.offsets[table_item];
    SEQLIB_CHECK_EQ(table, data + 12);
    SEQLIB_CHECK_EQ((region.start + table) % 4, 0);
    SEQLIB_CHECK_EQ(xsflib::LoadLe32(&grown.bytes[table + 4]), region.start + data + 7);
}

// The encoder refuses what the driver wouldn't read as the items say.
void TestRefusals()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF2, 0x10, 0x89, 0x20, 0xF8, 0x00, 0x21, 0xFF}, {6}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    const Decoded decoded = DecodeRegion(rom, info, region);
    const Encoded encoded = EncodeSequence(decoded.sequence, info.revision, region.start);
    const uint32_t data = kSequences - region.start;
    const size_t volume = ItemAt(encoded, data);
    const size_t note = ItemAt(encoded, data + 3);
    const size_t jump = ItemAt(encoded, data + 4);
    const size_t end = ItemAt(encoded, data + 7);

    // A note past the driver's notes, and a volume past its byte.
    Sequence bad = decoded.sequence;
    bad.items[note].key = 0x80;
    SEQLIB_CHECK(Refused(bad, info.revision, region.start));
    bad = decoded.sequence;
    bad.items[volume].value.value = ntdlib::commands::ToUnit(0x100, brownie::ScaleOf(Kind::kVolume, info.revision));
    SEQLIB_CHECK(Refused(bad, info.revision, region.start));

    // A jump that names another place than its table's word.
    bad = decoded.sequence;
    bad.items[jump].target = uint32_t(note);
    SEQLIB_CHECK(Refused(bad, info.revision, region.start));

    // A jump to an item that doesn't exist.
    bad = decoded.sequence;
    bad.items[jump].target = uint32_t(bad.items.size());
    SEQLIB_CHECK(Refused(bad, info.revision, region.start));

    // A channel that runs on into its table, without its end.
    bad = decoded.sequence;
    bad.items.erase(bad.items.begin() + std::ptrdiff_t(end));
    for (Command& item : bad.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside && item.target > end)
        {
            item.target--;
        }
    }
    SEQLIB_CHECK(Refused(bad, info.revision, region.start));

    // A list's channel without the word that gives its table's address.
    bad = decoded.sequence;
    const std::vector<brownie::ListTrack> tracks = TracksOfList(bad, decoded.lists[0]);
    bad.items[tracks[0].table].kind = Kind::kOther;
    SEQLIB_CHECK(Refused(bad, info.revision, region.start));
}

// A byte that reads both as a command and as an argument keeps the whole region as data, with the reason.
void TestOpaque()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF2, 0x10, 0x89, 0x20, 0xF8, 0x00, 0xFF}, {1}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Region region = RegionOf(rom, info, 1);
    const Decoded decoded = DecodeRegion(rom, info, region);
    SEQLIB_CHECK(decoded.opaque);
    SEQLIB_CHECK(!decoded.reason.empty());
    SEQLIB_REQUIRE(decoded.sequence.items.size() == 1);
    SEQLIB_CHECK(decoded.sequence.items[0].kind == Kind::kData);
    SEQLIB_CHECK(ComesBack(rom, decoded, info.revision, region));
}

// A sound that lies in no region can't be decoded.
void TestNoRegion()
{
    Cart cart;
    cart.Sound(1, {{0, {0x89, 0x10, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    bool refused = false;
    try
    {
        DecodeSong(rom, info, info.song_count);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused);
}

// A transpose's value takes 100 cents to the driver's step, and the others stay in the driver's units.
void TestScale()
{
    const ntdlib::commands::Scale cents = brownie::ScaleOf(Kind::kTranspose, Revision::kSwordOfMana);
    const ntdlib::commands::Scale raw = brownie::ScaleOf(Kind::kVolume, Revision::kSwordOfMana);
    SEQLIB_CHECK_EQ(ntdlib::commands::ToUnit(-2, cents), -200 * ntdlib::commands::kOne);
    SEQLIB_CHECK_EQ(ntdlib::commands::FromUnit(ntdlib::commands::ToUnit(0x50, raw), raw), 0x50);
}

} // namespace

int main()
{
    TestKinds();
    TestLists();
    TestRegions();
    TestSharedEnd();
    TestVacation();
    TestEdits();
    TestTableAfterEdit();
    TestRefusals();
    TestOpaque();
    TestNoRegion();
    TestScale();
    return xsflib::testing::Summary("brownie_commands");
}
