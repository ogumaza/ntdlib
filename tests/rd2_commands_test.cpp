// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the commands of Nintendo R&D2's GBA driver (ntdlib/rd2/commands.h). The
// sequences are written by hand, and the expected values come from the driver's format.

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd2/commands.h"
#include "ntdlib/rd2/driver.h"
#include "rd2_cart.h"

using xsflib::testing::Check;

namespace
{

namespace rd2 = ntdlib::rd2;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using rd2::Decoded;
using rd2::DecodeSequence;
using rd2::EncodeSequence;
using rd2::Revision;

constexpr Revision kPast = Revision::kLinkToThePast;
constexpr Revision kMario2 = Revision::kSuperMarioAdvance2;

// A sequence of one track, at offset 4, with `track` as its commands.
std::vector<uint8_t> OneTrack(std::vector<uint8_t> track)
{
    std::vector<uint8_t> bytes = {1, 0, 4, 0};
    bytes.insert(bytes.end(), track.begin(), track.end());
    return bytes;
}

// The commands of a sequence: its items after the header.
std::vector<Command> CommandsOf(const Decoded& decoded)
{
    std::vector<Command> commands;
    for (const Command& item : decoded.sequence.items)
    {
        if (item.kind != Kind::kData && item.encoding.opcode != rd2::kHeaderEntry)
        {
            commands.push_back(item);
        }
    }
    return commands;
}

// Checks that `bytes` decode without a problem and encode back.
Decoded Decodes(const std::vector<uint8_t>& bytes, Revision revision = kPast, bool effects = false)
{
    const Decoded decoded = DecodeSequence(bytes, revision, effects);
    Check(!decoded.opaque, decoded.reason.c_str());
    Check(EncodeSequence(decoded.sequence, revision).bytes == bytes, "the items encode back to the bytes");
    return decoded;
}

// A command of `kind` with `value` in the console's values, for the encoder.
Command NewCommand(Kind kind, int64_t value = 0)
{
    Command item;
    item.kind = kind;
    item.value.value = ntdlib::commands::ToUnit(value, rd2::ScaleOf(kind));
    return item;
}

// The bytes of `item` alone, in a track that ends after it.
std::vector<uint8_t> BytesOf(const Command& item, Revision revision = kPast)
{
    Command count;
    count.kind = Kind::kData;
    count.bytes = {1, 0};
    Command entry;
    entry.kind = Kind::kOpenTrack;
    entry.encoding.opcode = rd2::kHeaderEntry;
    entry.target = 2;

    ntdlib::commands::Sequence sequence;
    sequence.items = {count, entry, item, NewCommand(Kind::kEnd)};
    const std::vector<uint8_t> bytes = EncodeSequence(sequence, revision).bytes;
    return {bytes.begin() + 4, bytes.end() - 1};
}

// Whether `body` throws an exception of type `E`.
template <typename E, typename Body>
bool Throws(Body body)
{
    try
    {
        body();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

void TestHeader()
{
    // Two tracks at 6 and 7, then a track that the header leaves out with an offset of 0.
    const Decoded decoded = Decodes({3, 0, 6 + 2, 0, 9, 0, 0, 0, 0xFF, 0xFF});
    Check(decoded.entries.size() == 3 && decoded.entries[2] == rd2::kNoEntry, "the empty track has no entry");
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items[0].kind == Kind::kData && items[0].bytes == std::vector<uint8_t>({3, 0}), "the count stays as data");
    for (int t = 0; t < 3; t++)
    {
        const Command& entry = items[size_t(t) + 1];
        Check(entry.kind == Kind::kOpenTrack && entry.index == t && entry.encoding.opcode == rd2::kHeaderEntry,
              "each halfword is an entry of its track");
    }
    Check(items[3].target_outside && items[3].target == 0, "an offset of 0 names no item");
    Check(!items[1].target_outside && items[items[1].target].kind == Kind::kEnd,
          "an entry names its track's first item");
    Check(decoded.entries[0] == items[1].target && decoded.entries[1] == items[2].target,
          "entries name the same items");
}

void TestEveryCommand()
{
    const Decoded decoded = Decodes(OneTrack({
        0x60 + 40, 0x18, 0x50,    // a note with a length and a velocity
        41,                       // a note that takes both from the last
        0xC1,      0x20,          // a wait
        0xC0,                     // a wait as long as the last
        0xC2,      7,             // an instrument
        0xC3,      0x20,          // a pan
        0xC4,      12,            // a priority
        0xC5,      0xC6,          // legato on and off
        0xC7,      1,             // a bank
        0xC8,      0xC9,          // notes that wait and notes that don't
        0xCA,      9,             // a call to the game's code
        0xD2,      50,   0x80,    // a slide with even flags: a note and a length
        0xD3,      51,   0x40, 6, // a slide with odd flags: and a delay
        0xE0,      0x64,          // a volume
        0xE1,      0xC0,          // a bend of -64
        0xE2,      12,            // a bend's range
        0xE3,      0x30,          // the echo send
        0xE4,      0x96,          // a tempo: two bytes, as a length of 0x1600 + 0x00
        0x00,                     //
        0xE5,      3,             // the LFO's delay
        0xE6,      4,             // its speed
        0xE7,      5,             // its depth
        0xE8,                     // no slide
        0xE9,      0xFE,          // a transpose of -2
        0xEA,      0x70,          // the player's volume
        0xCB,                     // a command that does nothing
        0xFF,                     //
    }));
    const std::vector<Command> c = CommandsOf(decoded);
    Check(c.size() == 28, "every command has an item");
    using ntdlib::commands::FromUnit;
    const auto value = [&c](size_t i)
    {
        return FromUnit(c[i].value.value, rd2::ScaleOf(c[i].kind));
    };

    Check(c[0].kind == Kind::kNote && c[0].key == 40 && value(0) == 0x18 && !c[0].encoding.repeated, "a whole note");
    Check(c[0].velocity == 0x50 * kOne, "a note's velocity");
    Check(c[1].kind == Kind::kNote && c[1].key == 41 && value(1) == 0x18 && c[1].velocity == 0x50 * kOne &&
              c[1].encoding.repeated,
          "a note with the track's values");
    Check(c[2].kind == Kind::kWait && value(2) == 0x20 && !c[2].encoding.repeated, "a wait");
    Check(c[3].kind == Kind::kWait && value(3) == 0x20 && c[3].encoding.repeated, "a wait with the track's value");
    Check(c[4].kind == Kind::kProgram && value(4) == 7, "an instrument");
    Check(c[5].kind == Kind::kPan && value(5) == 0x20 && c[5].value.value == (0x20 - 64) * kOne,
          "a pan from the centre");
    Check(c[6].kind == Kind::kPriority && value(6) == 12, "a priority");
    Check(c[7].kind == Kind::kTie && value(7) == 1 && c[8].kind == Kind::kTie && value(8) == 0, "legato");
    Check(c[9].kind == Kind::kBankSelect && value(9) == 1, "a bank");
    Check(c[10].kind == Kind::kNoteWait && value(10) == 1 && c[11].kind == Kind::kNoteWait && value(11) == 0,
          "notes that wait");
    Check(c[12].kind == Kind::kUserProcedure && value(12) == 9, "a call to the game's code");
    Check(c[13].kind == Kind::kPortamentoFrom && c[13].index == 2 && c[13].key == 50 && value(13) == 0x80 &&
              c[13].velocity == 0,
          "a slide without a delay");
    Check(c[14].index == 3 && c[14].key == 51 && value(14) == 0x40 && c[14].velocity == 6 * kOne,
          "a slide with a delay");
    Check(c[15].kind == Kind::kVolume && value(15) == 0x64, "a volume");
    Check(c[16].kind == Kind::kPitchBend && value(16) == -64 && c[16].value.value == -kOne / 2, "half a bend's range");
    Check(c[17].kind == Kind::kBendRange && value(17) == 12 && c[17].value.value == 1200 * kOne, "a range in cents");
    Check(c[18].kind == Kind::kSend && value(18) == 0x30, "the echo send");
    Check(c[19].kind == Kind::kTempo && value(19) == 0x1600, "a tempo of two bytes");
    Check(c[20].kind == Kind::kLfoDelay && value(20) == 3, "the LFO's delay");
    Check(c[21].kind == Kind::kLfoSpeed && value(21) == 4, "the LFO's speed");
    Check(c[22].kind == Kind::kLfoDepth && value(22) == 5, "the LFO's depth");
    Check(c[23].kind == Kind::kPortamento, "no slide");
    Check(c[24].kind == Kind::kTranspose && value(24) == -2 && c[24].value.value == -200 * kOne,
          "a transpose in cents");
    Check(c[25].kind == Kind::kMainVolume && value(25) == 0x70, "the player's volume");
    Check(c[26].kind == Kind::kOther && c[26].encoding.opcode == 0xCB, "a command that does nothing");
    Check(c[27].kind == Kind::kEnd, "the track's end");
}

void TestLengths()
{
    // A length takes a byte below 0x80, or two with the top bit of the first set.
    const std::vector<Command> c = CommandsOf(Decodes(OneTrack({0xC1, 0x81, 0x02, 0xC1, 0x80, 0x05, 0xC1, 5, 0xFF})));
    Check(c[0].value.value == 0x102 * kOne && c[0].encoding.value_bytes == 0, "two bytes that it requires");
    Check(c[1].value.value == 5 * kOne && c[1].encoding.value_bytes == 2, "two bytes that it doesn't require");
    Check(c[2].value.value == 5 * kOne && c[2].encoding.value_bytes == 0, "one byte");

    Check(BytesOf(NewCommand(Kind::kWait, 0x7F)) == std::vector<uint8_t>({0xC1, 0x7F}), "the longest length in a byte");
    Check(BytesOf(NewCommand(Kind::kWait, 0x80)) == std::vector<uint8_t>({0xC1, 0x80, 0x80}), "the shortest in two");
    Check(BytesOf(NewCommand(Kind::kWait, 0x7FFF)) == std::vector<uint8_t>({0xC1, 0xFF, 0xFF}), "the longest");
    Check(Throws<std::invalid_argument>([] { BytesOf(NewCommand(Kind::kWait, 0x8000)); }), "a length past 15 bits");
}

void TestRevisions()
{
    // Super Mario Advance 2 reads a byte for a tempo, and nothing for EA.
    const std::vector<Command> older = CommandsOf(Decodes(OneTrack({0xE4, 0x96, 0xEA, 0xFF}), kMario2));
    Check(older[0].kind == Kind::kTempo && older[0].value.value == 0x96 * kOne, "a tempo of a byte");
    Check(older[1].kind == Kind::kOther && older[1].encoding.opcode == 0xEA, "EA does nothing");
    Check(older[2].kind == Kind::kEnd, "the end follows at once");
    Check(Throws<std::invalid_argument>([] { BytesOf(NewCommand(Kind::kMainVolume, 1), kMario2); }),
          "the revision holds no player's volume");
    Check(Throws<std::invalid_argument>([] { BytesOf(NewCommand(Kind::kTempo, 0x100), kMario2); }),
          "a tempo past a byte");

    Check(BytesOf(NewCommand(Kind::kTempo, 0x96)) == std::vector<uint8_t>({0xE4, 0x80, 0x96}), "a tempo as a length");
}

void TestFlow()
{
    // The track: a call of a subroutine that calls another, a jump over bytes that nothing reaches, the end. Offsets
    // count from the sequence's start. F8 starts a second track at 0x18.
    const std::vector<uint8_t> bytes = {
        1,    0,    4,    0,       // the header
        0xF4, 0x10, 0,             // 04: a call of 10
        0xF8, 1,    0x18, 0,       // 07: starts track 1 at 18
        0xF0, 0x15, 0,             // 0B: a jump to 15
        0xAA, 0xBB,                // 0E: no command reaches these
        0xF4, 0x14, 0,             // 10: a call of 14
        0xFF,                      // 13: a return
        0xFF,                      // 14: a return from the subroutine's subroutine
        0xFF,                      // 15: the first track's end
        0xCC, 0xDD,                // 16: no command reaches these either
        0xC1, 2,    0xF0, 0x18, 0, // 18: track 1 loops for ever
        0xEE,                      // 1D: nor this
    };
    const Decoded decoded = Decodes(bytes);
    const std::vector<Command>& items = decoded.sequence.items;

    std::vector<Kind> kinds;
    for (const Command& item : items)
    {
        kinds.push_back(item.kind);
    }
    const std::vector<Kind> expected = {Kind::kData, Kind::kOpenTrack, Kind::kCall,   Kind::kOpenTrack, Kind::kJump,
                                        Kind::kData, Kind::kCall,      Kind::kReturn, Kind::kReturn,    Kind::kEnd,
                                        Kind::kData, Kind::kWait,      Kind::kJump,   Kind::kData};
    Check(kinds == expected, "each byte reads as the driver reads it");
    Check(items[2].target == 6 && !items[2].target_outside, "a call's target is an item");
    Check(items[3].index == 1 && items[3].target == 11, "F8's track and target");
    Check(items[4].target == 9, "a jump's target");
    Check(items[6].target == 8, "the inner call's target");
    Check(items[12].target == 11, "a loop's target");

    // An FF that a track reaches both inside and outside a call is the track's end.
    const std::vector<uint8_t> both = {1, 0, 4, 0, 0xF4, 8, 0, 0xFF, 0xFF};
    const std::vector<Command> shared = CommandsOf(Decodes(both));
    Check(shared[1].kind == Kind::kEnd && shared[2].kind == Kind::kReturn, "a call's return, and a track's end");
    const std::vector<uint8_t> falls = {1, 0, 4, 0, 0xF4, 7, 0, 0xFF};
    Check(CommandsOf(Decodes(falls))[1].kind == Kind::kEnd, "the same FF, reached both ways, is an end");

    // The driver keeps 3 places, and frees a track that nests deeper. The fourth call's target stays out of the flow.
    const std::vector<uint8_t> deep = {1, 0, 4, 0, 0xF4, 7, 0, 0xF4, 10, 0, 0xF4, 13, 0, 0xF4, 17, 0, 0xFF, 0xAB};
    const Decoded nested = Decodes(deep);
    Check(nested.sequence.items.back().kind == Kind::kData, "the track never reads the fourth call's target");
    const Command& fourth = nested.sequence.items[5];
    Check(fourth.kind == Kind::kCall && fourth.target_outside && fourth.target == 17, "a target that no track reads");

    // F8 past a player's 10 tracks starts nothing, and the target stays a place.
    const std::vector<uint8_t> past = {1, 0, 4, 0, 0xF8, 10, 9, 0, 0xFF, 0xAB};
    const std::vector<Command> started = CommandsOf(Decodes(past));
    Check(started[0].index == 10 && started[0].target_outside && started[0].target == 9, "no track past the tenth");
}

void TestEffects()
{
    // A sound effects' sequence: halfwords up to its first track. The track at 0 is empty.
    const std::vector<uint8_t> bytes = {0, 0, 8, 0, 9, 0, 0, 0, 0xFF, 0xC1, 1, 0xFF};
    const Decoded decoded = Decodes(bytes, kPast, true);
    Check(decoded.entries.size() == 4, "the table ends where its first track starts");
    Check(decoded.entries[0] == rd2::kNoEntry && decoded.entries[3] == rd2::kNoEntry,
          "an offset of 0 is an empty track");
    Check(decoded.sequence.items[0].kind == Kind::kOpenTrack, "the table has no count");
    Check(decoded.sequence.items[decoded.entries[1]].kind == Kind::kEnd, "the first track");
    Check(decoded.sequence.items[decoded.entries[2]].kind == Kind::kWait, "the second track");

    // A table that names no track, and one that a track starts inside.
    Check(DecodeSequence(std::vector<uint8_t>{0, 0, 0, 0}, kPast, true).opaque, "no track");
    const Decoded odd = DecodeSequence(std::vector<uint8_t>{0, 0, 3, 0, 0xFF}, kPast, true);
    Check(odd.opaque, "a track inside the table");
    Check(EncodeSequence(odd.sequence, kPast).bytes == std::vector<uint8_t>({0, 0, 3, 0, 0xFF}), "its bytes come back");
}

void TestOpaque()
{
    const auto opaque = [](const std::vector<uint8_t>& bytes, const char* what)
    {
        const Decoded decoded = DecodeSequence(bytes, kPast);
        Check(decoded.opaque, what);
        Check(EncodeSequence(decoded.sequence, kPast).bytes == bytes, "a sequence kept as data comes back");
        for (const Command& item : decoded.sequence.items)
        {
            Check(item.kind == Kind::kData, "a sequence kept as data holds no commands");
        }
    };

    opaque({11, 0}, "more tracks than a player holds");
    opaque({0xFF, 0}, "a negative count of tracks");
    opaque({2, 0, 6, 0}, "a header that runs past the bytes");
    opaque({1, 0, 9, 0, 0xFF}, "a track that starts past the bytes");
    opaque({1, 0, 4, 0, 0xC1}, "a command that runs past the bytes");
    opaque({1, 0, 4, 0, 0xF0, 1, 0}, "a jump into the header");
    opaque({1, 0, 4, 0, 0xF0, 5, 0}, "a jump into a command");
    Check(Throws<std::invalid_argument>([] { DecodeSequence({}, kPast); }), "no bytes");

    // The entries of a sequence kept as data name its pieces.
    const Decoded split = DecodeSequence(std::vector<uint8_t>{2, 0, 6, 0, 7, 0, 0xFF, 0xF0, 1, 0}, kPast);
    Check(split.opaque && split.sequence.items.size() == 3, "the data splits at the tracks");
    Check(split.entries[0] == 1 && split.entries[1] == 2, "each track's entry names its piece");
}

void TestEdits()
{
    // Clearing `repeated` gives a note and a wait their values.
    Decoded decoded = Decodes(OneTrack({0x60 + 40, 0x18, 0x50, 41, 0xC1, 0x20, 0xC0, 0xFF}));
    for (Command& item : decoded.sequence.items)
    {
        item.encoding.repeated = false;
    }
    const std::vector<uint8_t> whole = EncodeSequence(decoded.sequence, kPast).bytes;
    Check(whole == OneTrack({0x60 + 40, 0x18, 0x50, 0x60 + 41, 0x18, 0x50, 0xC1, 0x20, 0xC1, 0x20, 0xFF}),
          "the usual form of each");

    // A longer command moves the targets after it.
    Decoded jump = Decodes({1, 0, 4, 0, 0xC1, 5, 0xF0, 9, 0, 0xFF});
    jump.sequence.items[2].value.value = 0x100 * kOne;
    Check(EncodeSequence(jump.sequence, kPast).bytes ==
              std::vector<uint8_t>({1, 0, 4, 0, 0xC1, 0x81, 0, 0xF0, 10, 0, 0xFF}),
          "the jump follows its target");

    // What the bytes can't hold.
    const auto refused = [](Command item, const char* what)
    {
        Check(Throws<std::invalid_argument>([&item] { BytesOf(item); }), what);
    };
    refused(NewCommand(Kind::kVolume, 256), "a volume past a byte");
    refused(NewCommand(Kind::kTranspose, 128), "a transpose past a signed byte");
    refused(NewCommand(Kind::kPitchBend, -129), "a bend past a signed byte");
    refused(NewCommand(Kind::kLoopStart), "a command that the driver lacks");

    Command key = NewCommand(Kind::kNote, 1);
    key.key = 0x60;
    refused(key, "a key past the driver's notes");

    Command opcode = NewCommand(Kind::kOther);
    opcode.encoding.opcode = 0xC2;
    refused(opcode, "an opcode that reads as a command");

    Command even = NewCommand(Kind::kPortamentoFrom, 1);
    even.index = 2;
    even.velocity = kOne;
    refused(even, "a delay on a slide that reads none");

    Command random = NewCommand(Kind::kVolume, 1);
    random.value.source = ntdlib::commands::Source::kRandom;
    refused(random, "a random value");

    // A target past the driver's 16 bits.
    Decoded far = Decodes({1, 0, 4, 0, 0xF0, 4, 0});
    Command data;
    data.kind = Kind::kData;
    data.bytes.assign(0x10000, 0xAB);
    far.sequence.items.insert(far.sequence.items.begin() + 2, data);
    far.sequence.items[1].target = 3;
    far.sequence.items[3].target = 3;
    Check(Throws<std::invalid_argument>([&far] { EncodeSequence(far.sequence, kPast); }), "a target past 16 bits");

    // A track that an edit leads into data, a place inside a command, and a header that doesn't read back.
    Decoded wild = Decodes({1, 0, 4, 0, 0xF0, 7, 0, 0xFF});
    wild.sequence.items[2].target = 1;
    Check(Throws<std::invalid_argument>([&wild] { EncodeSequence(wild.sequence, kPast); }),
          "a track that reaches the header");
    Decoded inside = Decodes({1, 0, 4, 0, 0xF0, 7, 0, 0xFF});
    inside.sequence.items[2].target_outside = true;
    inside.sequence.items[2].target = 5;
    Check(Throws<std::invalid_argument>([&inside] { EncodeSequence(inside.sequence, kPast); }),
          "a track that reaches a place inside a command");
    Decoded count = Decodes(OneTrack({0xFF}));
    count.sequence.items[0].bytes[0] = 2;
    Check(Throws<std::invalid_argument>([&count] { EncodeSequence(count.sequence, kPast); }),
          "a count that the entries don't give");
}

void TestItemOffsets()
{
    // The header's count and entry of 2 bytes each, a note of 3 bytes, a repeated note of 1, a wait of 2 and an end
    // of 1.
    const Decoded decoded = Decodes(OneTrack({0x60 + 40, 0x18, 0x50, 41, 0xC1, 0x20, 0xFF}));
    const std::vector<uint32_t> offsets = rd2::ItemOffsets(decoded.sequence, kPast);
    Check(offsets == std::vector<uint32_t>{0, 2, 4, 7, 8, 10, 11}, "each item's offset, then the end");
    Check(offsets == EncodeSequence(decoded.sequence, kPast).offsets, "the offsets are the encoder's");

    // Items that don't read back as a sequence: no header, no end, and data that a track would reach.
    Command data;
    data.kind = Kind::kData;
    data.bytes = {1, 2, 3};
    const ntdlib::commands::Sequence loose = {{NewCommand(Kind::kVolume, 5), data, NewCommand(Kind::kVolume, 6)}, {}};
    Check(rd2::ItemOffsets(loose, kPast) == std::vector<uint32_t>{0, 2, 5, 7}, "items without a header");

    const ntdlib::commands::Sequence too_loud = {{NewCommand(Kind::kVolume, 256)}, {}};
    Check(Throws<std::invalid_argument>([&too_loud] { rd2::ItemOffsets(too_loud, kPast); }),
          "a value that the bytes can't hold");
}

void TestRomSequences()
{
    using namespace ntdlib::testing::rd2_cart;
    Cart cart;
    cart.Sequences({{TrackData().Wait(4).End()}, {TrackData().End(), TrackData().End()}});
    cart.Effects({{TrackData().End(), {}, TrackData().Wait(1).End()}});
    const rd2::Rom rom = cart.ToRom();
    rd2::DriverInfo info;
    std::string error;
    Check(rd2::DetectDriver(rom, {}, info, error), error.c_str());

    Check(rd2::SequenceCount(info) == 3, "the sequences, then the sound effects' sequence");
    Check(!rd2::IsEffectSequence(info, 1) && rd2::IsEffectSequence(info, 2), "which sequences are the effects'");
    Check(rd2::SequenceAddress(info, 2) == kEffectData, "the effects' sequence's address");
    Check(Throws<std::out_of_range>([&info] { rd2::SequenceAddress(info, 3); }), "no fourth sequence");
    Check(rd2::SequenceOfSong(info, 1) == 1 && rd2::SequenceOfSong(info, 2) == 2 && rd2::SequenceOfSong(info, 4) == 2,
          "a sound effect's sequence");
    Check(Throws<std::out_of_range>([&info] { rd2::SequenceOfSong(info, 5); }), "no sixth song");

    // The first sequence's 8 bytes, and the padding up to the next on a multiple of 4.
    const Decoded first = rd2::DecodeRomSequence(rom, info, 0);
    const std::vector<uint8_t> bytes = EncodeSequence(first.sequence, info.revision).bytes;
    Check(bytes.size() == 8, "a sequence runs on to a next sequence close after it");
    Check(first.sequence.items.back().kind == Kind::kData && first.sequence.items.back().bytes.size() == 1,
          "the padding is data");

    // The last music sequence ends with its last command: the effects' sequence lies far off.
    const Decoded second = rd2::DecodeRomSequence(rom, info, 1);
    Check(EncodeSequence(second.sequence, info.revision).bytes.size() == 8, "a sequence ends with its tracks");
    Check(rd2::DecodeRomSequence(rom, info, 2).entries.size() == 3, "the effects' table");
}

} // namespace

int main()
{
    TestHeader();
    TestEveryCommand();
    TestLengths();
    TestRevisions();
    TestFlow();
    TestEffects();
    TestOpaque();
    TestEdits();
    TestItemOffsets();
    TestRomSequences();

    return xsflib::testing::Summary("rd2_commands");
}
