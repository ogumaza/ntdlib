// SPDX-License-Identifier: MIT

// Unit tests of the common form of the sequences' commands (ntdlib/commands/sequence.h), and of the decoder and the
// encoder of NintendoWare's sequence bytecode (ntdlib/nwsnd/commands.h). The bytes are made up. The expected commands
// are those that the sequence player's parser (SequenceTrack::Parse) reads from them.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/sound_build.h"

namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using ntdlib::nwsnd::Grammar;
using ntdlib::nwsnd::LabelOffset;
using xsflib::testing::ErrorOf;

const Grammar kSwitch = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::NxPolicy>();

// Decodes `bytes`, and checks that the encoder writes them back.
Sequence RoundTrip(const std::vector<uint8_t>& bytes, const std::vector<LabelOffset>& labels, const Grammar& grammar,
                   bool opaque = false)
{
    const ntdlib::nwsnd::Decoded decoded = ntdlib::nwsnd::DecodeSequence(bytes, labels, grammar);
    XSFLIB_CHECK(decoded.opaque == opaque);
    XSFLIB_CHECK(ntdlib::nwsnd::EncodeSequence(decoded.sequence, grammar).bytes == bytes);

    return decoded.sequence;
}

bool IsArgument(const Argument& argument, Source source, int64_t value, int64_t high = 0)
{
    return argument == Argument{source, value, high};
}

bool IsData(const Command& command, const std::vector<uint8_t>& bytes)
{
    return command.kind == Kind::kData && command.bytes == bytes;
}

void TestNames()
{
    for (int k = 0; k < ntdlib::commands::kKindCount; ++k)
    {
        const auto kind = static_cast<Kind>(k);
        XSFLIB_CHECK(ntdlib::commands::KindNamed(ntdlib::commands::Name(kind)) == kind);
    }

    XSFLIB_CHECK(ntdlib::commands::Name(Kind::kVolume) == "volume");
    XSFLIB_CHECK(!ntdlib::commands::KindNamed("loudness"));
}

// Two tracks: the first opens the second.
void TestTracks()
{
    const std::vector<uint8_t> kBytes = {
        0xFE, 0x03, 0x00,             // the tracks to allocate: 0 and 1
        0x88, 0x01, 0x0E, 0x00, 0x00, // open track 1 at 0x0E
        0xC1, 0x64,                   // volume 100
        0x3C, 0x7F, 0x30,             // note 60, velocity 127, 48 ticks
        0xFF,                         // end
        0xC3, 0xFF,                   // 0x0E: transpose -1
        0x80, 0x81, 0x00,             // wait 128
        0xFF,                         // end
    };
    const Sequence sequence = RoundTrip(kBytes, {{"Song", 0}}, kSwitch);
    const std::vector<Command>& items = sequence.items;
    XSFLIB_CHECK(items.size() == 8 && sequence.labels.size() == 1 && sequence.labels[0].item == 0);
    XSFLIB_CHECK(items[0].kind == Kind::kAllocateTracks && items[0].value.value == 3 * kOne);
    XSFLIB_CHECK(items[1].kind == Kind::kOpenTrack && items[1].index == 1 && items[1].target == 5);
    XSFLIB_CHECK(items[2].kind == Kind::kVolume && items[2].value.value == 100 * kOne);
    XSFLIB_CHECK(items[3].kind == Kind::kNote && items[3].key == 60 && items[3].velocity == 127 * kOne);
    XSFLIB_CHECK(IsArgument(items[3].value, Source::kValue, 48 * kOne));
    XSFLIB_CHECK(items[4].kind == Kind::kEnd && items[7].kind == Kind::kEnd);
    XSFLIB_CHECK(items[5].kind == Kind::kTranspose && items[5].value.value == -100 * kOne);
    XSFLIB_CHECK(items[6].kind == Kind::kWait && items[6].value.value == 128 * kOne &&
                 items[6].encoding.value_bytes == 0);
}

// The prefixes, the commands after 0xF0, and a variable-length value in more bytes than it requires.
void TestPrefixes()
{
    const std::vector<uint8_t> kBytes = {
        0xA2, 0xC1, 0x50,                               // if: volume 80
        0xA3, 0xC1, 0x64, 0x30, 0x00,                   // volume 100 over 48 ticks
        0xA4, 0xC0, 0x40, 0xF6, 0xFF, 0x0A, 0x00,       // pan 64 over -10 to 10 ticks at random
        0xA5, 0xC0, 0x40, 0x05,                         // pan 64 over the ticks in variable 5
        0xA0, 0x3C, 0x7F, 0x10, 0x00, 0x20, 0x00,       // note 60 for 16 to 32 ticks at random
        0xA1, 0xC1, 0x07,                               // the volume in variable 7
        0xA3, 0x80, 0x05,                               // wait 5, after a time prefix it reads no time for
        0xA0, 0xA1,                                     // 0xA1 after a random prefix: a command that does nothing
        0xF0, 0x80, 0x03, 0xFE, 0xFF,                   // variable 3 = -2
        0xA0, 0xF0, 0x81, 0x03, 0x01, 0x00, 0x05, 0x00, // variable 3 += 1 to 5 at random
        0xF0, 0xA0, 0x05,                               // LFO 1's curve 5
        0xF0, 0xE1, 0xE8, 0x03,                         // LFO 1's delay 1000
        0xF0, 0xE0, 0x34, 0x12,                         // user procedure 0x1234
        0xA2, 0xFF,                                     // if: end. Otherwise the track goes on.
        0x80, 0x80, 0x80, 0x80, 0x05,                   // wait 5 in 4 bytes
        0xFF,                                           // end
    };
    const std::vector<Command> items = RoundTrip(kBytes, {{"Song", 0}}, kSwitch).items;
    XSFLIB_CHECK(items.size() == 16);
    XSFLIB_CHECK(items[0].kind == Kind::kVolume && items[0].conditional && items[0].value.value == 80 * kOne);
    XSFLIB_CHECK(!items[1].conditional && items[1].time && IsArgument(*items[1].time, Source::kValue, 48 * kOne));
    XSFLIB_CHECK(items[2].kind == Kind::kPan && items[2].time &&
                 IsArgument(*items[2].time, Source::kRandom, -10 * kOne, 10 * kOne));
    XSFLIB_CHECK(items[3].time && IsArgument(*items[3].time, Source::kVariable, 5));
    XSFLIB_CHECK(items[4].kind == Kind::kNote && IsArgument(items[4].value, Source::kRandom, 16 * kOne, 32 * kOne));
    XSFLIB_CHECK(items[5].kind == Kind::kVolume && IsArgument(items[5].value, Source::kVariable, 7));
    XSFLIB_CHECK(items[6].kind == Kind::kWait && !items[6].time && items[6].encoding.unused_time == 0xA3);
    XSFLIB_CHECK(items[7].kind == Kind::kOther && items[7].encoding.opcode == 0xA1 && items[7].encoding.unused_random);
    XSFLIB_CHECK(items[8].kind == Kind::kSetVariable && items[8].index == 3 && items[8].value.value == -2 * kOne);
    XSFLIB_CHECK(items[9].kind == Kind::kAddVariable &&
                 IsArgument(items[9].value, Source::kRandom, 1 * kOne, 5 * kOne));
    XSFLIB_CHECK(items[10].kind == Kind::kLfoCurve && items[10].index == 1 && items[10].value.value == 5 * kOne);
    XSFLIB_CHECK(items[11].kind == Kind::kLfoDelay && items[11].index == 1 && items[11].value.value == 5000 * kOne);
    XSFLIB_CHECK(items[12].kind == Kind::kUserProcedure && items[12].value.value == 0x1234 * kOne);
    XSFLIB_CHECK(items[13].kind == Kind::kEnd && items[13].conditional);
    XSFLIB_CHECK(items[14].kind == Kind::kWait && items[14].value.value == 5 * kOne &&
                 items[14].encoding.value_bytes == 4);
    XSFLIB_CHECK(items[15].kind == Kind::kEnd);
}

// Bytes that no command reaches, a target past the end, and a label where the bytes run out inside a command.
void TestData()
{
    const std::vector<uint8_t> kBytes = {
        0x8A, 0x0A, 0x00, 0x00, // call 0x0A
        0x89, 0x20, 0x00, 0x00, // jump to 0x20, past the end
        0x12, 0x34,             // no command reaches these
        0xFD,                   // 0x0A: return
        0x56,                   //
        0x80,                   // 0x0C: a wait whose value runs past the end
    };
    const Sequence sequence = RoundTrip(kBytes, {{"Song", 0}, {"Cut", 0x0C}, {"End", 0x0D}}, kSwitch);
    const std::vector<Command>& items = sequence.items;
    XSFLIB_CHECK(items.size() == 6);
    XSFLIB_CHECK(items[0].kind == Kind::kCall && items[0].target == 3 && !items[0].target_outside);
    XSFLIB_CHECK(items[1].kind == Kind::kJump && items[1].target == 0x20 && items[1].target_outside);
    XSFLIB_CHECK(IsData(items[2], {0x12, 0x34}) && items[3].kind == Kind::kReturn);
    XSFLIB_CHECK(IsData(items[4], {0x56}) && IsData(items[5], {0x80}));
    XSFLIB_CHECK(sequence.labels[1].item == 5 && sequence.labels[2].item == 6);

    // A value that takes more than 32 bits: the encoder would write other bytes, and the bytes stay as data.
    const std::vector<uint8_t> kOverflow = {0x80, 0x81, 0x80, 0x80, 0x80, 0x80, 0x00, 0xFF};
    const std::vector<Command> kept = RoundTrip(kOverflow, {{"Song", 0}}, kSwitch).items;
    XSFLIB_CHECK(kept.size() == 2 && IsData(kept[0], {0x80, 0x81, 0x80, 0x80, 0x80, 0x80, 0x00}));
    XSFLIB_CHECK(kept[1].kind == Kind::kEnd);
}

// The commands in bytes that the player never reaches.
void TestGuesses()
{
    // The end after a track's last jump.
    const std::vector<uint8_t> kLoop = {0xC1, 0x64, 0x89, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00};
    const std::vector<Command> looped = RoundTrip(kLoop, {{"Song", 0}}, kSwitch).items;
    XSFLIB_CHECK(looped.size() == 4 && looped[1].kind == Kind::kJump && looped[1].target == 0);
    XSFLIB_CHECK(looped[2].kind == Kind::kEnd && IsData(looped[3], {0x00, 0x00}));

    // A track that no command starts, with a loop of its own.
    const std::vector<uint8_t> kTrack = {0xFF, 0xC1, 0x50, 0x3C, 0x7F, 0x30, 0x89, 0x01, 0x00, 0x00};
    const std::vector<Command> tracked = RoundTrip(kTrack, {{"Song", 0}}, kSwitch).items;
    XSFLIB_CHECK(tracked.size() == 4 && tracked[1].kind == Kind::kVolume && tracked[2].kind == Kind::kNote);
    XSFLIB_CHECK(tracked[3].kind == Kind::kJump && tracked[3].target == 1);

    // Commands that run into a track that the player reaches.
    const std::vector<uint8_t> kJoin = {0x88, 0x01, 0x0A, 0x00, 0x00, 0xFF, 0xC1,
                                        0x50, 0x80, 0x0C, 0x3C, 0x7F, 0x30, 0xFF};
    const std::vector<Command> joined = RoundTrip(kJoin, {{"Song", 0}}, kSwitch).items;
    XSFLIB_CHECK(joined.size() == 6 && joined[0].target == 4 && joined[2].kind == Kind::kVolume);
    XSFLIB_CHECK(joined[3].kind == Kind::kWait && joined[4].kind == Kind::kNote);

    // A jump to where no item starts: the bytes stay as data.
    const std::vector<uint8_t> kStray = {0xFF, 0x89, 0x07, 0x00, 0x00, 0x12, 0x34, 0x56};
    const std::vector<Command> strayed = RoundTrip(kStray, {{"Song", 0}}, kSwitch).items;
    XSFLIB_CHECK(strayed.size() == 2 && IsData(strayed[1], {0x89, 0x07, 0x00, 0x00, 0x12, 0x34, 0x56}));
}

// Sounds that start where no label lies.
void TestEntries()
{
    const std::vector<uint8_t> kBytes = {
        0xFF,                   // end
        0x8A, 0x07, 0x00, 0x00, // 0x01: call 0x07
        0xFF,                   // end
        0x00,                   //
        0xFD,                   // 0x07: return
    };
    const std::vector<LabelOffset> kLabels = {{"Song", 0}};
    const std::vector<uint32_t> kEntries = {1};
    const ntdlib::nwsnd::Decoded decoded = ntdlib::nwsnd::DecodeSequence(kBytes, kLabels, kSwitch, kEntries);
    const std::vector<Command>& items = decoded.sequence.items;
    XSFLIB_CHECK(!decoded.opaque && decoded.entries == kEntries && items.size() == 5);
    XSFLIB_CHECK(items[1].kind == Kind::kCall && items[1].target == 4 && IsData(items[3], {0x00}));
    XSFLIB_CHECK(items[4].kind == Kind::kReturn);
    XSFLIB_CHECK(ntdlib::nwsnd::EncodeSequence(decoded.sequence, kSwitch).bytes == kBytes);

    // Without the entry, the call leads where no item starts, and its bytes stay as data.
    const std::vector<Command> unentered = RoundTrip(kBytes, kLabels, kSwitch).items;
    XSFLIB_CHECK(unentered.size() == 2 && IsData(unentered[1], {0x8A, 0x07, 0x00, 0x00, 0xFF, 0x00, 0xFD}));

    // An entry inside a command.
    const std::vector<uint32_t> kInside = {1, 2};
    const ntdlib::nwsnd::Decoded split = ntdlib::nwsnd::DecodeSequence(kBytes, kLabels, kSwitch, kInside);
    XSFLIB_CHECK(split.opaque && split.sequence.items.size() == 3 && split.entries == kInside);

    const std::vector<uint32_t> kPast = {9};
    XSFLIB_CHECK(
        !ErrorOf<std::invalid_argument>([&] { ntdlib::nwsnd::DecodeSequence(kBytes, kLabels, kSwitch, kPast); })
             .empty());
}

// A jump or a label inside a command: the decoder keeps every byte as data, split at the labels.
void TestOpaque()
{
    const std::vector<uint8_t> kJump = {0xC1, 0x64, 0x89, 0x01, 0x00, 0x00};
    const Sequence jumped = RoundTrip(kJump, {{"Song", 0}}, kSwitch, true);
    XSFLIB_CHECK(jumped.items.size() == 1 && IsData(jumped.items[0], kJump) && jumped.labels[0].item == 0);

    const std::vector<uint8_t> kLabel = {0xC1, 0x64, 0xFF};
    const Sequence labelled = RoundTrip(kLabel, {{"Song", 0}, {"Inside", 1}}, kSwitch, true);
    XSFLIB_CHECK(labelled.items.size() == 2 && IsData(labelled.items[0], {0xC1}));
    XSFLIB_CHECK(IsData(labelled.items[1], {0x64, 0xFF}) && labelled.labels[1].item == 1);
}

// The 3DS's and the Wii's grammars.
void TestConsoles()
{
    // The 3DS: big-endian arguments, and no byte after 0xF0 0xA0.
    const std::vector<uint8_t> kCtr = {
        0xE1, 0x00, 0x78,             // tempo 120
        0xF0, 0xA0,                   // a command that does nothing
        0xF0, 0xE1, 0x01, 0x00,       // LFO 1's delay 256
        0x88, 0x02, 0x00, 0x00, 0x0E, // open track 2 at 0x0E
        0xFF,                         // end
    };
    const Grammar ctr_grammar = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::CtrPolicy>();
    const std::vector<Command> ctr_items = RoundTrip(kCtr, {{"Song", 0}}, ctr_grammar).items;
    XSFLIB_CHECK(ctr_items.size() == 5 && ctr_items[0].kind == Kind::kTempo && ctr_items[0].value.value == 120 * kOne);
    XSFLIB_CHECK(ctr_items[1].kind == Kind::kOther && ctr_items[1].encoding.opcode == 0xF0A0);
    XSFLIB_CHECK(ctr_items[2].kind == Kind::kLfoDelay && ctr_items[2].value.value == 1280 * kOne);
    XSFLIB_CHECK(ctr_items[3].kind == Kind::kOpenTrack && ctr_items[3].target == 4);

    // The Wii: a variable prefix after a random one applies, and an end ends the track whatever the condition. Before
    // 2008, 0xA3 is a command that does nothing.
    const std::vector<uint8_t> kWii = {
        0xA3, 0xC1, 0x64,       // volume 100
        0xA0, 0xA1, 0xC1, 0x02, // the volume in variable 2
        0xA2, 0xFF,             // end
        0x00, 0x00, 0x00,       // no command reaches these
    };
    ntdlib::SndBuild build;
    build.date = 20070101;
    const Grammar wii_2007 = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build);
    const std::vector<Command> items_2007 = RoundTrip(kWii, {{"Song", 0}}, wii_2007).items;
    XSFLIB_CHECK(items_2007.size() == 5 && items_2007[0].kind == Kind::kOther && items_2007[0].encoding.opcode == 0xA3);
    XSFLIB_CHECK(items_2007[1].kind == Kind::kVolume && items_2007[1].value.value == 100 * kOne);
    XSFLIB_CHECK(items_2007[2].kind == Kind::kVolume && IsArgument(items_2007[2].value, Source::kVariable, 2));
    XSFLIB_CHECK(items_2007[2].encoding.unused_random && items_2007[3].kind == Kind::kEnd);
    XSFLIB_CHECK(items_2007[3].conditional && IsData(items_2007[4], {0x00, 0x00, 0x00}));

    // From 2008, 0xA3 is a time prefix, and the volume reads a time.
    build.date = 20080308;
    const Grammar wii_2008 = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>(build);
    const std::vector<Command> items_2008 = RoundTrip(kWii, {{"Song", 0}}, wii_2008).items;
    XSFLIB_CHECK(items_2008.size() == 4 && items_2008[0].kind == Kind::kVolume && items_2008[0].time);
    XSFLIB_CHECK(IsArgument(*items_2008[0].time, Source::kValue, 0xA0A1 * kOne) &&
                 items_2008[1].value.value == 2 * kOne);
}

// One command at a time, and whether the track goes on after it.
void TestOneCommand()
{
    const std::vector<uint8_t> kBytes = {
        0xA2, 0x89, 0x05, 0x00, 0x00, // if: jump to 5
        0x3C, 0x7F, 0x30,             // note 60, velocity 127, 48 ticks
        0xA2, 0xFF,                   // if: end
        0x89, 0x00, 0x00, 0x00,       // jump to 0
        0xC1,                         // a volume whose value lies past the end
    };
    const auto jump = ntdlib::nwsnd::DecodeCommand(kBytes, 0, kSwitch);
    XSFLIB_CHECK(jump && jump->size == 5 && jump->command.kind == Kind::kJump && jump->command.conditional);
    XSFLIB_CHECK(jump && jump->command.target == 5 && ntdlib::nwsnd::FallsThrough(jump->command, kSwitch));

    const auto note = ntdlib::nwsnd::DecodeCommand(kBytes, 5, kSwitch);
    XSFLIB_CHECK(note && note->size == 3 && note->command.kind == Kind::kNote && note->command.key == 60);
    XSFLIB_CHECK(note && IsArgument(note->command.value, Source::kValue, 48 * kOne));

    // The Switch's end goes on when its condition fails. The Wii's ends the track whatever the condition.
    const auto end = ntdlib::nwsnd::DecodeCommand(kBytes, 8, kSwitch);
    XSFLIB_CHECK(end && end->size == 2 && end->command.kind == Kind::kEnd);
    XSFLIB_CHECK(end && ntdlib::nwsnd::FallsThrough(end->command, kSwitch));
    const Grammar wii = ntdlib::nwsnd::GrammarOf<ntdlib::nwsnd::RevolutionPolicy>();
    XSFLIB_CHECK(end && !ntdlib::nwsnd::FallsThrough(end->command, wii));

    const auto back = ntdlib::nwsnd::DecodeCommand(kBytes, 10, kSwitch);
    XSFLIB_CHECK(back && back->size == 4 && back->command.target == 0);
    XSFLIB_CHECK(back && !ntdlib::nwsnd::FallsThrough(back->command, kSwitch));

    XSFLIB_CHECK(!ntdlib::nwsnd::DecodeCommand(kBytes, 14, kSwitch));
    XSFLIB_CHECK(!ntdlib::nwsnd::DecodeCommand(kBytes, 15, kSwitch));
}

// A sequence made without bytes: the encoder writes the usual bytes, and puts each target's offset in.
void TestEncode()
{
    Sequence sequence;
    Command open;
    open.kind = Kind::kOpenTrack;
    open.index = 1;
    open.target = 3;
    Command volume;
    volume.kind = Kind::kVolume;
    volume.value.value = 100 * kOne;
    volume.time = Argument{Source::kValue, 96 * kOne, 0};
    Command end;
    end.kind = Kind::kEnd;
    Command note;
    note.kind = Kind::kNote;
    note.key = 64;
    note.velocity = 100 * kOne;
    note.value.value = 200 * kOne;
    sequence.items = {open, volume, end, note, end};
    sequence.labels = {{"Song", 0}};

    const ntdlib::nwsnd::Encoded encoded = ntdlib::nwsnd::EncodeSequence(sequence, kSwitch);
    const std::vector<uint8_t> kExpected = {0x88, 0x01, 0x0B, 0x00, 0x00, 0xA3, 0xC1, 0x64,
                                            0x60, 0x00, 0xFF, 0x40, 0x64, 0x81, 0x48, 0xFF};
    XSFLIB_CHECK(encoded.bytes == kExpected && encoded.offsets == std::vector<uint32_t>({0, 5, 10, 11, 15, 16}));

    const std::vector<LabelOffset> kLabels = {{"Song", 0}};
    const ntdlib::nwsnd::Decoded decoded = ntdlib::nwsnd::DecodeSequence(encoded.bytes, kLabels, kSwitch);
    XSFLIB_CHECK(!decoded.opaque && decoded.sequence == sequence);
}

// The bytes of `sequence` with its first item's value set to `ticks`.
std::vector<uint8_t> EncodedWithWait(Sequence sequence, int64_t ticks)
{
    sequence.items[0].value.value = ticks * kOne;

    return ntdlib::nwsnd::EncodeSequence(sequence, kSwitch).bytes;
}

// Edits of a wait of 5 in 2 bytes. A value that requires fewer bytes keeps the padding. A value that requires as many
// bytes as the padding, or more, takes the bytes that it requires.
void TestEditsOfPaddedWait()
{
    const std::vector<uint8_t> kBytes = {0x80, 0x80, 0x05, 0xFF}; // wait 5 in 2 bytes, then end
    const std::vector<LabelOffset> kLabels = {{"Song", 0}};
    const Sequence sequence = RoundTrip(kBytes, kLabels, kSwitch);

    const std::vector<uint8_t> kept = EncodedWithWait(sequence, 100);
    const std::vector<uint8_t> filled = EncodedWithWait(sequence, 200);
    const std::vector<uint8_t> longer = EncodedWithWait(sequence, 20000);
    const ntdlib::nwsnd::Decoded read = ntdlib::nwsnd::DecodeSequence(longer, kLabels, kSwitch);

    XSFLIB_CHECK(sequence.items[0].encoding.value_bytes == 2);
    XSFLIB_CHECK(kept == std::vector<uint8_t>({0x80, 0x80, 0x64, 0xFF}));
    XSFLIB_CHECK(filled == std::vector<uint8_t>({0x80, 0x81, 0x48, 0xFF}));
    XSFLIB_CHECK(longer == std::vector<uint8_t>({0x80, 0x81, 0x9C, 0x20, 0xFF}));
    XSFLIB_CHECK(read.sequence.items.size() == 2 && read.sequence.items[0].kind == Kind::kWait &&
                 read.sequence.items[0].value.value == 20000 * kOne);
}

// Whether the encoder refuses `command`.
bool Refuses(const Command& command)
{
    Sequence sequence;
    sequence.items.push_back(command);

    return !ErrorOf<std::invalid_argument>([&] { ntdlib::nwsnd::EncodeSequence(sequence, kSwitch); }).empty();
}

void TestRefusals()
{
    Command prefix; // 0xA0 is a prefix, not a command
    prefix.encoding.opcode = 0xA0;
    XSFLIB_CHECK(Refuses(prefix));

    Command wait; // 0x80 is a wait
    wait.encoding.opcode = 0x80;
    XSFLIB_CHECK(Refuses(wait));

    Command send; // a fourth bus
    send.kind = Kind::kSend;
    send.index = 3;
    XSFLIB_CHECK(Refuses(send));

    Command transpose; // the player reads 200 as -56
    transpose.kind = Kind::kTranspose;
    transpose.value.value = 200 * 100 * kOne;
    XSFLIB_CHECK(Refuses(transpose));

    Command jump; // past the items
    jump.kind = Kind::kJump;
    jump.target = 2;
    XSFLIB_CHECK(Refuses(jump));

    Command open; // a track start takes no random value
    open.kind = Kind::kOpenTrack;
    open.value.source = Source::kRandom;
    XSFLIB_CHECK(Refuses(open));

    Command timed; // a wait takes no time
    timed.kind = Kind::kWait;
    timed.time = Argument{};
    XSFLIB_CHECK(Refuses(timed));

    Command volume;
    volume.kind = Kind::kVolume;
    volume.value.value = 127 * kOne;
    XSFLIB_CHECK(!Refuses(volume));

    // A value between two of the console's takes the nearest.
    Sequence between;
    volume.value.value = 100 * kOne + kOne * 2 / 5;
    between.items.push_back(volume);
    XSFLIB_CHECK(ntdlib::nwsnd::EncodeSequence(between, kSwitch).bytes == std::vector<uint8_t>({0xC1, 0x64}));
}

} // namespace

int main()
{
    TestNames();
    TestTracks();
    TestPrefixes();
    TestData();
    TestGuesses();
    TestEntries();
    TestOpaque();
    TestConsoles();
    TestOneCommand();
    TestEncode();
    TestEditsOfPaddedWait();
    TestRefusals();

    return xsflib::testing::Summary("Sequence command");
}
