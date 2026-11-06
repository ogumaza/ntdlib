// SPDX-License-Identifier: MIT

// Unit tests of the decoder and the encoder of the DS's sequence bytecode (ntdlib/ntr/snd/commands.h). The bytes are
// made up. The expected commands are those that the driver's track step reads from them, worked out by hand.

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ntr/snd/commands.h"

namespace
{

namespace snd = ntdlib::ntr::snd;
using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Scale;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;
using xsflib::testing::ErrorOf;

using Bytes = std::vector<uint8_t>;

// The command at the start of `bytes`, by the driver's rules. The bytes past the end read as 0.
snd::PlayedCommand Played(const Bytes& bytes)
{
    const auto byte_at = [&bytes](uint32_t address)
    {
        return address < bytes.size() ? bytes[address] : uint8_t{0};
    };
    return snd::ReadPlayedCommand(byte_at, 0);
}

bool IsArgument(const Argument& argument, Source source, int64_t value, int64_t high = 0)
{
    return argument == Argument{source, value, high};
}

// Decodes `bytes` from `entries`, and checks that the encoder writes them back.
Sequence RoundTrip(const Bytes& bytes, const std::vector<uint32_t>& entries, bool opaque = false)
{
    const snd::Decoded decoded = snd::DecodeSequence(bytes, {}, entries);
    XSFLIB_CHECK(decoded.opaque == opaque);
    XSFLIB_CHECK(snd::EncodeSequence(decoded.sequence).bytes == bytes);

    return decoded.sequence;
}

void TestPlayedCommands()
{
    snd::PlayedCommand played = Played({0x3C, 0x7F, 0x30});
    XSFLIB_CHECK(played.command.kind == Kind::kNote && played.command.key == 0x3C && played.command.velocity == 0x7F);
    XSFLIB_CHECK(IsArgument(played.command.value, Source::kValue, 0x30) && played.code == 0x3C && played.next == 3);

    // An if prefix, then a random prefix: a note whose length lies from 1 to 5.
    played = Played({0xA2, 0xA0, 0x3C, 0x7F, 0x01, 0x00, 0x05, 0x00});
    XSFLIB_CHECK(played.command.conditional && played.command.kind == Kind::kNote);
    XSFLIB_CHECK(IsArgument(played.command.value, Source::kRandom, 1, 5) && played.next == 8);

    // A variable prefix after a random one takes its place.
    played = Played({0xA0, 0xA1, 0xC1, 0x03});
    XSFLIB_CHECK(played.command.kind == Kind::kVolume && IsArgument(played.command.value, Source::kVariable, 3));
    XSFLIB_CHECK(played.command.encoding.unused_random && played.next == 4);

    // A jump reads no argument for a prefix.
    played = Played({0xA1, 0x94, 0x10, 0x00, 0x00});
    XSFLIB_CHECK(played.command.kind == Kind::kJump && played.command.target == 0x10);
    XSFLIB_CHECK(played.command.encoding.unused_variable && played.next == 5);

    played = Played({0x93, 0x02, 0x20, 0x01, 0x00});
    XSFLIB_CHECK(played.command.kind == Kind::kOpenTrack && played.command.index == 2);
    XSFLIB_CHECK(played.command.target == 0x120 && played.next == 5);

    played = Played({0xB6, 0x05, 0xFE, 0xFF});
    XSFLIB_CHECK(played.command.kind == Kind::kRandomVariable && played.command.index == 5);
    XSFLIB_CHECK(IsArgument(played.command.value, Source::kValue, -2) && played.next == 4);

    played = Played({0xC3, 0xFE});
    XSFLIB_CHECK(played.command.kind == Kind::kTranspose && played.command.value.value == -2);
    played = Played({0xE1, 0x78, 0x00});
    XSFLIB_CHECK(played.command.kind == Kind::kTempo && played.command.value.value == 120 && played.next == 3);

    // A wait of 128 takes two bytes. A wait of 5 in two bytes keeps them.
    played = Played({0x80, 0x81, 0x00});
    XSFLIB_CHECK(played.command.value.value == 128 && played.command.encoding.value_bytes == 0);
    played = Played({0x80, 0x80, 0x05});
    XSFLIB_CHECK(played.command.value.value == 5 && played.command.encoding.value_bytes == 2 && played.next == 3);

    played = Played({0xA1, 0x80, 0x02});
    XSFLIB_CHECK(played.command.kind == Kind::kWait && IsArgument(played.command.value, Source::kVariable, 2));
    XSFLIB_CHECK(played.next == 3);

    // 0xFE in a track reads nothing. 0xB7 reads a variable's number and a value, and 0x82 a variable length.
    played = Played({0xFE, 0x03, 0x00});
    XSFLIB_CHECK(played.command.kind == Kind::kOther && played.command.encoding.opcode == 0xFE && played.next == 1);
    played = Played({0xB7, 0x01, 0x02, 0x00});
    XSFLIB_CHECK(played.command.kind == Kind::kOther && played.command.encoding.opcode == 0xB7);
    XSFLIB_CHECK(played.command.index == 1 && played.command.value.value == 2 && played.next == 4);
    played = Played({0x82, 0x05});
    XSFLIB_CHECK(played.command.kind == Kind::kOther && played.command.value.value == 5 && played.next == 2);

    // A random prefix after a variable one is a command that does nothing.
    played = Played({0xA1, 0xA0, 0x90});
    XSFLIB_CHECK(played.command.kind == Kind::kOther && played.command.encoding.opcode == 0xA0);
    XSFLIB_CHECK(played.command.encoding.unused_variable && played.next == 2);

    // The addresses wrap at 2^32.
    std::vector<uint32_t> addresses;
    const auto byte_at = [&addresses](uint32_t address)
    {
        addresses.push_back(address);
        return uint8_t{0x10};
    };
    played = snd::ReadPlayedCommand(byte_at, 0xFFFFFFFF);
    XSFLIB_CHECK(addresses == (std::vector<uint32_t>{0xFFFFFFFF, 0, 1}) && played.next == 2);
}

// Two sequences, an orphan and padding. The first sequence allocates tracks 0 and 1. Track 0 opens track 1, sets its
// volume and loops a note. Track 1 pans, calls a transpose and ends. The second sequence sets the LFO, and a sequence
// that no entry names sets a sweep.
const Bytes kSequences = {
    0xFE, 0x03, 0x00,             // 0x00: allocate tracks 0 and 1
    0x93, 0x01, 0x11, 0x00, 0x00, // 0x03: open track 1 at 0x11
    0xC1, 0x64,                   // 0x08: volume 100
    0x3C, 0x7F, 0x30,             // 0x0A: note
    0x94, 0x0A, 0x00, 0x00,       // 0x0D: jump to 0x0A
    0xC0, 0x40,                   // 0x11: pan 64
    0x95, 0x1A, 0x00, 0x00,       // 0x13: call 0x1A
    0x80, 0x18,                   // 0x17: wait 24
    0xFF,                         // 0x19: end
    0xC3, 0xFE,                   // 0x1A: transpose -2
    0xFD,                         // 0x1C: return
    0xCA, 0x01,                   // 0x1D: LFO depth 1
    0xCB, 0x01,                   // 0x1F: LFO speed 1
    0xE0, 0x02, 0x00,             // 0x21: LFO delay 2
    0xFF,                         // 0x24: end
    0xFE, 0x01, 0x00,             // 0x25: allocate track 0
    0xE3, 0x40, 0x00,             // 0x28: sweep pitch 64
    0xFF,                         // 0x2B: end
    0x00, 0x00, 0x00, 0x00,       // 0x2C: padding
};

void TestDecode()
{
    const std::vector<snd::LabelOffset> labels = {{"SEQ_B", 0x1D}};
    const std::vector<uint32_t> entries = {0};
    const snd::Decoded decoded = snd::DecodeSequence(kSequences, labels, entries);
    XSFLIB_CHECK(!decoded.opaque);
    XSFLIB_CHECK(decoded.entries == std::vector<uint32_t>{0});
    XSFLIB_CHECK(decoded.sequence.labels.size() == 1 && decoded.sequence.labels[0].item == 11);

    const std::vector<Command>& items = decoded.sequence.items;
    XSFLIB_CHECK(items.size() == 19);
    if (items.size() != 19)
    {
        return;
    }
    const Kind kinds[] = {Kind::kAllocateTracks,
                          Kind::kOpenTrack,
                          Kind::kVolume,
                          Kind::kNote,
                          Kind::kJump,
                          Kind::kPan,
                          Kind::kCall,
                          Kind::kWait,
                          Kind::kEnd,
                          Kind::kTranspose,
                          Kind::kReturn,
                          Kind::kLfoDepth,
                          Kind::kLfoSpeed,
                          Kind::kLfoDelay,
                          Kind::kEnd,
                          Kind::kAllocateTracks,
                          Kind::kSweepPitch,
                          Kind::kEnd,
                          Kind::kData};
    for (std::size_t i = 0; i < items.size(); i++)
    {
        XSFLIB_CHECK(items[i].kind == kinds[i]);
    }

    // The values in their units, and the targets by item.
    XSFLIB_CHECK(items[0].value.value == 3 * kOne);
    XSFLIB_CHECK(items[1].index == 1 && items[1].target == 5);
    XSFLIB_CHECK(items[2].value.value == 100 * kOne);
    XSFLIB_CHECK(items[3].key == 60 && items[3].velocity == 127 * kOne && items[3].value.value == 48 * kOne);
    XSFLIB_CHECK(items[4].target == 3 && items[6].target == 9);
    XSFLIB_CHECK(items[5].value.value == 0);
    XSFLIB_CHECK(items[7].value.value == 24 * kOne);
    XSFLIB_CHECK(items[9].value.value == -200 * kOne);
    XSFLIB_CHECK(items[11].value.value == 508);
    XSFLIB_CHECK(items[12].value.value == 24570);
    XSFLIB_CHECK(items[13].value.value == 2 * 341412);
    XSFLIB_CHECK(items[15].value.value == kOne);
    XSFLIB_CHECK(items[16].value.value == 100 * kOne);
    XSFLIB_CHECK(items[18].bytes == (Bytes{0, 0, 0, 0}));

    XSFLIB_CHECK(snd::EncodeSequence(decoded.sequence).bytes == kSequences);
}

// Bytes that the driver never reaches: a call after a jump, the end after it, and the subroutine that it calls. The
// three commands up to the end lead to the subroutine. The subroutine follows them.
void TestGuesses()
{
    const Bytes bytes = {
        0x94, 0x0C, 0x00, 0x00, // 0x00: jump to 0x0C
        0x95, 0x09, 0x00, 0x00, // 0x04: call 0x09
        0xFF,                   // 0x08: end
        0xC1, 0x10,             // 0x09: volume 16
        0xFD,                   // 0x0B: return
        0xFF,                   // 0x0C: end
    };
    const Sequence sequence = RoundTrip(bytes, {0});
    XSFLIB_CHECK(sequence.items.size() == 6);
    if (sequence.items.size() == 6)
    {
        XSFLIB_CHECK(sequence.items[0].target == 5 && sequence.items[1].kind == Kind::kCall);
        XSFLIB_CHECK(sequence.items[1].target == 3 && sequence.items[4].kind == Kind::kReturn);
    }

    // A call into bytes that no command covers leaves the run as data.
    const Bytes stray = {0x94, 0x0A, 0x00, 0x00, 0x95, 0x09, 0x00, 0x00, 0xFF, 0x00, 0xFF};
    const Sequence kept = RoundTrip(stray, {0});
    XSFLIB_CHECK(kept.items.size() == 3 && kept.items[1].kind == Kind::kData && kept.items[1].bytes.size() == 6);
}

// Only a sequence's start reads 0xFE as an allocation of tracks. In a track, 0xFE reads nothing.
void TestAllocations()
{
    const Sequence sequence = RoundTrip({0xFE, 0x01, 0x00, 0xFE, 0xC1, 0x10, 0xFF}, {0});
    XSFLIB_CHECK(sequence.items.size() == 4);
    if (sequence.items.size() == 4)
    {
        XSFLIB_CHECK(sequence.items[0].kind == Kind::kAllocateTracks && sequence.items[0].value.value == kOne);
        XSFLIB_CHECK(sequence.items[1].kind == Kind::kOther && sequence.items[1].encoding.opcode == 0xFE);
        XSFLIB_CHECK(sequence.items[2].kind == Kind::kVolume && sequence.items[3].kind == Kind::kEnd);
    }

    const Sequence wide = RoundTrip({0xFE, 0x01, 0x02, 0xFF}, {0});
    XSFLIB_CHECK(wide.items.size() == 2 && wide.items[0].value.value == 0x201 * kOne);

    // Two bytes can't hold an allocation: the track reads 0xFE, then a note that runs past the end.
    const Sequence short_one = RoundTrip({0xFE, 0x01}, {0});
    XSFLIB_CHECK(short_one.items.size() == 2 && short_one.items[0].kind == Kind::kOther);

    // An orphan starts after a command, not amid bytes that no command covers.
    const Sequence amid = RoundTrip({0xFF, 0x00, 0xFE, 0x01, 0x00, 0xFF}, {0});
    XSFLIB_CHECK(amid.items.size() == 2 && amid.items[1].kind == Kind::kData);

    // The jump's target stays data, and the orphan after the jump still counts.
    const Sequence orphan = RoundTrip({0x94, 0x08, 0x00, 0x00, 0xFE, 0x01, 0x00, 0xFF, 0x3C}, {0});
    XSFLIB_CHECK(orphan.items.size() == 4);
    if (orphan.items.size() == 4)
    {
        XSFLIB_CHECK(orphan.items[1].kind == Kind::kAllocateTracks && orphan.items[3].kind == Kind::kData);
    }
}

// The prefixes that the commands take and those that they don't, and the bytes of a padded wait, come back.
void TestPrefixesComeBack()
{
    const Sequence sequence = RoundTrip(
        {0xA0, 0xA1, 0xC1, 0x03, 0xA1, 0xA0, 0xA2, 0xA0, 0x80, 0x01, 0x00, 0x05, 0x00, 0x80, 0x80, 0x05, 0xA1, 0xFF},
        {0});
    XSFLIB_CHECK(sequence.items.size() == 5);
    if (sequence.items.size() != 5)
    {
        return;
    }
    XSFLIB_CHECK(sequence.items[0].kind == Kind::kVolume && sequence.items[0].encoding.unused_random);
    XSFLIB_CHECK(sequence.items[1].kind == Kind::kOther && sequence.items[1].encoding.unused_variable);
    XSFLIB_CHECK(sequence.items[2].kind == Kind::kWait && sequence.items[2].conditional);
    XSFLIB_CHECK(sequence.items[3].kind == Kind::kWait && sequence.items[3].encoding.value_bytes == 2);
    XSFLIB_CHECK(sequence.items[4].kind == Kind::kEnd && sequence.items[4].encoding.unused_variable);

    // A wait of 5 in two bytes, edited to 300, takes the two bytes that 300 requires.
    Sequence edited = sequence;
    edited.items[3].value.value = 300 * kOne;
    XSFLIB_CHECK(snd::EncodeSequence(edited).bytes == (Bytes{0xA0, 0xA1, 0xC1, 0x03, 0xA1, 0xA0, 0xA2, 0xA0, 0x80, 0x01,
                                                             0x00, 0x05, 0x00, 0x80, 0x82, 0x2C, 0xA1, 0xFF}));

    // A variable length longer than 32 bits loses its high bits. The encoder can't write them back. The bytes stay
    // data.
    const Sequence long_wait = RoundTrip({0x80, 0x81, 0x80, 0x80, 0x80, 0x80, 0x05, 0xFF}, {0});
    XSFLIB_CHECK(long_wait.items.size() == 2 && long_wait.items[0].kind == Kind::kData);
}

void TestOpaque()
{
    // A jump into the middle of the jump.
    Sequence sequence = RoundTrip({0x94, 0x01, 0x00, 0x00}, {0}, true);
    XSFLIB_CHECK(sequence.items.size() == 1 && sequence.items[0].kind == Kind::kData);

    // A jump to an entry's allocation of tracks. A track reads those bytes as other commands.
    sequence = RoundTrip({0x94, 0x04, 0x00, 0x00, 0xFE, 0x01, 0x00, 0xFF}, {0, 4}, true);
    XSFLIB_CHECK(sequence.items.size() == 2 && sequence.items[1].bytes.size() == 4);
}

// A longer wait moves the commands after it, and the targets follow them.
void TestEdits()
{
    const snd::Decoded decoded = snd::DecodeSequence(kSequences, {}, std::vector<uint32_t>{0, 0x1D});
    Sequence edited = decoded.sequence;
    edited.items[7].value.value = 300 * kOne;
    const snd::Encoded encoded = snd::EncodeSequence(edited);
    XSFLIB_CHECK(encoded.bytes.size() == kSequences.size() + 1);
    XSFLIB_CHECK(encoded.offsets[9] == 0x1B && encoded.offsets[7] == 0x17);
    XSFLIB_CHECK(Bytes(encoded.bytes.begin() + 0x13, encoded.bytes.begin() + 0x1A) ==
                 (Bytes{0x95, 0x1B, 0x00, 0x00, 0x80, 0x82, 0x2C}));
    XSFLIB_CHECK(Bytes(encoded.bytes.begin() + 0x0D, encoded.bytes.begin() + 0x11) == (Bytes{0x94, 0x0A, 0x00, 0x00}));
}

void TestOneCommand()
{
    const std::optional<snd::DecodedCommand> volume = snd::DecodeCommand(Bytes{0xC1, 0x64}, 0);
    XSFLIB_CHECK(volume && volume->command.kind == Kind::kVolume && volume->command.value.value == 100 * kOne);
    XSFLIB_CHECK(volume && volume->size == 2);
    XSFLIB_CHECK(!snd::DecodeCommand(Bytes{0x94, 0x01}, 0));
}

void TestFallsThrough()
{
    Command command;
    for (const Kind kind : {Kind::kJump, Kind::kReturn, Kind::kEnd})
    {
        command.kind = kind;
        command.conditional = false;
        XSFLIB_CHECK(!snd::FallsThrough(command));
        command.conditional = true;
        XSFLIB_CHECK(snd::FallsThrough(command));
    }
    command = {};
    command.kind = Kind::kLoopEnd;
    XSFLIB_CHECK(snd::FallsThrough(command));
}

void TestScales()
{
    XSFLIB_CHECK(snd::ScaleOf(Kind::kPan) == (Scale{kOne, -64 * kOne}));
    XSFLIB_CHECK(snd::ScaleOf(Kind::kTranspose) == (Scale{100 * kOne, 0}));
    XSFLIB_CHECK(snd::ScaleOf(Kind::kBendRange) == (Scale{100 * kOne, 0}));
    XSFLIB_CHECK(snd::ScaleOf(Kind::kSweepPitch) == (Scale{102400, 0}));
    XSFLIB_CHECK(snd::ScaleOf(Kind::kPitchBend) == (Scale{512, 0}));
    XSFLIB_CHECK(snd::ScaleOf(Kind::kVolume) == Scale{});
    XSFLIB_CHECK(snd::ScaleOf(Kind::kTempo) == Scale{});
}

void TestRefusals()
{
    const auto encode = [](const Command& command)
    {
        snd::EncodeSequence(Sequence{{command}, {}});
    };

    Command other;
    other.kind = Kind::kOther;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(other); }) == "no opcode 0");

    // 0xA0 alone is a random prefix.
    other.encoding.opcode = 0xA0;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(other); }) ==
                 "the driver would read other commands from the bytes of other");

    Command lfo;
    lfo.kind = Kind::kLfoDepth;
    lfo.index = 1;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(lfo); }) == "no opcode for lfo_depth 1");

    Command jump;
    jump.kind = Kind::kJump;
    jump.value.source = Source::kRandom;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(jump); }) == "jump takes no random or variable value");
    jump.value.source = Source::kValue;
    jump.target = 0x1000000;
    jump.target_outside = true;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(jump); }) == "a target past 24 bits");
    jump.target = 2;
    jump.target_outside = false;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(jump); }) == "a target past the items");

    Command note;
    note.kind = Kind::kNote;
    note.key = 0x80;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(note); }) == "a note's key above 127");

    Command allocation;
    allocation.kind = Kind::kAllocateTracks;
    allocation.conditional = true;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { encode(allocation); }) ==
                 "an allocation of tracks takes no prefix");

    XSFLIB_CHECK(
        ErrorOf<std::invalid_argument>([] { snd::DecodeSequence(Bytes{0xFF}, {}, std::vector<uint32_t>{2}); }) ==
        "an entry past the end of the sequence: 2");
}

} // namespace

int main()
{
    TestPlayedCommands();
    TestDecode();
    TestGuesses();
    TestAllocations();
    TestPrefixesComeBack();
    TestOpaque();
    TestEdits();
    TestOneCommand();
    TestFallsThrough();
    TestScales();
    TestRefusals();

    return xsflib::testing::Summary("DS sequence commands");
}
