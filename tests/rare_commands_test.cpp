// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the commands of Rare's GBA driver (ntdlib/rare/commands.h). The sequences
// are written by hand, and the expected values come from the driver's format.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rare/commands.h"
#include "ntdlib/rare/song.h"

using xsflib::testing::Check;

namespace
{

namespace rare = ntdlib::rare;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using rare::Decoded;
using rare::DecodeSequence;
using rare::EncodeSequence;
using rare::Format;
using rare::Traits;

constexpr Traits kByte = {Format::kChannelByte, false};
constexpr Traits kNibble = {Format::kChannelNibble, false};
constexpr Traits kLater = {Format::kChannelNibble, true};

// The commands of a sequence: its items but data.
std::vector<Command> CommandsOf(const Decoded& decoded)
{
    std::vector<Command> commands;
    for (const Command& item : decoded.sequence.items)
    {
        if (item.kind != Kind::kData)
        {
            commands.push_back(item);
        }
    }
    return commands;
}

// Checks that `bytes`, with tracks at `starts`, decode without a problem and encode back.
Decoded Decodes(const std::vector<uint8_t>& bytes, const Traits& traits, const std::vector<uint32_t>& starts = {0})
{
    const Decoded decoded = DecodeSequence(bytes, starts, traits);
    Check(!decoded.opaque, decoded.reason.c_str());
    Check(EncodeSequence(decoded.sequence, decoded.entries, traits).bytes == bytes, "the items encode back");
    return decoded;
}

// A command of `kind` with `value` in the console's values, for the encoder.
Command NewCommand(Kind kind, int64_t value = 0, uint8_t index = 0)
{
    Command item;
    item.kind = kind;
    item.index = index;
    item.value.value = ntdlib::commands::ToUnit(value, rare::ScaleOf(kind));
    return item;
}

// The bytes of `item` alone, in a track that ends after it.
std::vector<uint8_t> BytesOf(const Command& item, const Traits& traits)
{
    ntdlib::commands::Sequence sequence;
    sequence.items = {item, NewCommand(Kind::kEnd)};
    const std::vector<uint32_t> entries = {0};
    const std::vector<uint8_t> bytes = EncodeSequence(sequence, entries, traits).bytes;
    return {bytes.begin(), bytes.end() - 1};
}

// Whether `body` throws std::invalid_argument.
template <typename Body>
bool Throws(Body body)
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

// Every command of the channel-byte format, with the channel in a byte after the command's.
void TestChannelByte()
{
    const Decoded decoded = Decodes(
        {
            0x00, 0x20, 0xA1, 0x07, // tempo 500,000
            0x01, 0x30,             // delay 48
            0x02, 0x10, 0x00,       // delay 16 in two bytes
            0x03, 0x00, 0x00, 0x01, // delay 65,536
            0x05, 0x02, 60,   100,  // note on, channel 2
            0x04, 0x02, 62,   90,   // note on of command 4
            0x06, 0x02, 60,   0x40, // note off, with a byte that the driver ignores
            0x07, 0x03, 1,    20,   // modulation
            0x07, 0x03, 7,    100,  // volume
            0x07, 0x03, 100,  5,    // controller 100
            0x07, 0x03, 20,   4,    // controller 20, which this revision ignores
            0x08, 0x03, 9,          // program
            0x09, 0x03, 0x11,       // channel pressure
            0x0A, 0x03, 0x00, 0x30, // bend 0x3000
            0x07, 0x03, 126,  0,    // mono
            0x07, 0x03, 127,  0,    // poly
            0x0C,                   // nothing
            0x0B,                   // end
        },
        kByte);
    const std::vector<Command> c = CommandsOf(decoded);
    Check(c.size() == 18, "every command is an item");
    Check(c[0].kind == Kind::kTempo && c[0].value.value == 500000 * kOne, "the tempo's microseconds");
    Check(c[1].kind == Kind::kWait && c[1].value.value == 48 * kOne && c[1].encoding.value_bytes == 0, "a delay");
    Check(c[2].value.value == 16 * kOne && c[2].encoding.value_bytes == 2, "a delay in more bytes than it needs");
    Check(c[3].value.value == 65536 * kOne && c[3].encoding.value_bytes == 0, "a delay of three bytes");
    Check(c[4].kind == Kind::kNote && c[4].index == 2 && c[4].key == 60 && c[4].velocity == 100 * kOne &&
              c[4].encoding.opcode == 0,
          "a note on");
    Check(c[5].kind == Kind::kNote && c[5].encoding.opcode == 4, "command 4 is a note on");
    Check(c[6].kind == Kind::kNoteOff && c[6].key == 60 && c[6].encoding.arguments == 0x40, "a note off");
    Check(c[7].kind == Kind::kLfoDepth && c[7].value.value == 20 * kOne, "controller 1 is the vibrato's depth");
    Check(c[8].kind == Kind::kVolume && c[8].value.value == 100 * kOne, "controller 7 is the volume");
    Check(c[9].kind == Kind::kOther && c[9].encoding.opcode == rare::kController + 100 && c[9].value.value == 5 * kOne,
          "a controller without a kind");
    Check(c[10].kind == Kind::kOther && c[10].encoding.opcode == rare::kController + 20,
          "controller 20 has no kind in the older revisions");
    Check(c[11].kind == Kind::kProgram && c[11].value.value == 9 * kOne, "a program");
    Check(c[12].kind == Kind::kOther && c[12].encoding.opcode == 9 && c[12].encoding.arguments == 0x11,
          "a channel pressure");
    Check(c[13].kind == Kind::kPitchBend && c[13].value.value == kOne / 2, "a bend of half the range");
    Check(c[14].kind == Kind::kMonophonic && c[14].value.value == kOne, "mono");
    Check(c[15].kind == Kind::kMonophonic && c[15].value.value == 0, "poly");
    Check(c[16].kind == Kind::kOther && c[16].encoding.opcode == 12, "command 12 does nothing");
    Check(c[17].kind == Kind::kEnd, "the end");
}

// The channel-nibble format, with the channel in the command byte's high nibble, and the later revision's controllers.
void TestChannelNibble()
{
    const Decoded decoded = Decodes(
        {
            0x35, 60,   100, // note on, channel 3
            0x41, 0x10,      // delay 16, with a high nibble that the driver ignores
            0x37, 20,   5,   // attack
            0x37, 21,   6,   // decay
            0x37, 22,   7,   // sustain
            0x37, 23,   8,   // release
            0x36, 60,   0,   // note off
            0x0B,            // end
        },
        kLater);
    const std::vector<Command> c = CommandsOf(decoded);
    Check(c[0].kind == Kind::kNote && c[0].index == 3 && c[0].key == 60, "the note's channel is the high nibble");
    Check(c[1].kind == Kind::kWait && c[1].index == 4 && c[1].value.value == 16 * kOne, "the delay keeps its nibble");
    Check(c[2].kind == Kind::kAttack && c[3].kind == Kind::kDecay && c[4].kind == Kind::kSustain &&
              c[5].kind == Kind::kRelease && c[5].value.value == 8 * kOne,
          "controllers 20 to 23 set the envelope in the later revision");
    Check(!c[6].encoding.arguments, "a note off's ignored byte of 0");

    // The older revision of the format reads them as controllers without a kind, and can't write the envelope.
    const Decoded older = Decodes({0x37, 20, 5, 0x0B}, kNibble);
    Check(CommandsOf(older)[0].kind == Kind::kOther, "controller 20 in the older revision");
    Check(Throws([] { BytesOf(NewCommand(Kind::kAttack, 5), kNibble); }), "the older revision has no attack");
    Check(Throws([] { BytesOf(NewCommand(Kind::kNote, 0, 16), kNibble); }), "a channel past the nibble");
}

// A track ends at its end, or at its loop's end after its loop's start. The bytes after it stay as data, and so do
// those between the tracks.
void TestTrackEnds()
{
    const Decoded decoded = Decodes(
        {
            0x07, 0x00, 102, 0,   // loop start
            0x01, 0x10,           // delay
            0x07, 0x00, 103, 0,   // loop end
            0x0B,                 // data: the end after the loop
            0xEE,                 // data: a byte between the tracks
            0x07, 0x01, 103, 9,   // a loop end without a start
            0x05, 0x01, 60,  100, // the track reads on past it
            0x0B,
        },
        kByte, {0, 12});
    const std::vector<Command>& items = decoded.sequence.items;
    Check(decoded.entries == std::vector<uint32_t>({0, 4}), "the tracks' entries");
    Check(items[0].kind == Kind::kLoopStart && items[0].value.value == 0, "the loop's start");
    Check(items[2].kind == Kind::kLoopEnd, "the loop's end");
    Check(items[3].kind == Kind::kData && items[3].bytes == std::vector<uint8_t>({0x0B, 0xEE}),
          "the bytes past the loop stay as data");
    Check(items[4].kind == Kind::kLoopEnd && items[4].encoding.arguments == 9, "a loop's end keeps its byte");
    Check(items[5].kind == Kind::kNote && items[6].kind == Kind::kEnd, "a track reads on past a loop's end alone");
}

// A sequence that the driver can't read safely stays as data, split at its tracks.
void TestOpaque()
{
    const std::vector<uint8_t> unknown = {0x01, 0x10, 0x0D, 0x0B, 0x0B};
    const Decoded decoded = DecodeSequence(unknown, std::vector<uint32_t>({0, 4}), kByte);
    Check(decoded.opaque && decoded.reason.find("no code") != std::string::npos, "a command without code");
    Check(decoded.sequence.items.size() == 2 && decoded.entries == std::vector<uint32_t>({0, 1}),
          "the data splits at the tracks");
    const std::vector<uint32_t> entries = decoded.entries;
    Check(EncodeSequence(decoded.sequence, entries, kByte).bytes == unknown, "the data encodes back");

    const std::vector<uint8_t> past = {0x01, 0x10, 0x05, 0x00, 60};
    Check(DecodeSequence(past, std::vector<uint32_t>({0}), kByte).opaque, "a track that runs past the bytes");

    // Track 1 starts inside track 0's note.
    const std::vector<uint8_t> overlap = {0x35, 0x0B, 0x00, 0x0B};
    Check(DecodeSequence(overlap, std::vector<uint32_t>({0, 1}), kNibble).opaque, "commands that overlap");
}

// The encoder writes new commands in the fewest bytes, and refuses what the driver can't hold or wouldn't read.
void TestNewCommands()
{
    Check(BytesOf(NewCommand(Kind::kWait, 300), kByte) == std::vector<uint8_t>({0x02, 0x2C, 0x01}),
          "a delay takes the bytes that it needs");
    Check(BytesOf(NewCommand(Kind::kWait, 0x10000, 2), kNibble) == std::vector<uint8_t>({0x23, 0, 0, 1}),
          "a delay of three bytes, with its nibble");
    Check(BytesOf(NewCommand(Kind::kPitchBend, 0x2000, 1), kByte) == std::vector<uint8_t>({0x0A, 0x01, 0x00, 0x20}),
          "a centred bend");
    Check(BytesOf(NewCommand(Kind::kLoopStart, 0, 0), kByte) == std::vector<uint8_t>({0x07, 0x00, 102, 0}),
          "a loop's start");
    Check(BytesOf(NewCommand(Kind::kMonophonic, 1, 5), kNibble) == std::vector<uint8_t>({0x57, 126, 0}), "mono");

    Command note = NewCommand(Kind::kNote, 0, 1);
    note.key = 64;
    note.velocity = 127 * kOne;
    Check(BytesOf(note, kByte) == std::vector<uint8_t>({0x05, 0x01, 64, 127}), "a note on");

    Check(Throws([] { BytesOf(NewCommand(Kind::kWait, 0x1000000), kByte); }), "a delay past three bytes");
    Check(Throws([] { BytesOf(NewCommand(Kind::kNote, 10), kByte); }), "a note with a length");
    Check(Throws([] { BytesOf(NewCommand(Kind::kLoopStart, 2), kByte); }), "a loop that ends");
    Check(Throws([] { BytesOf(NewCommand(Kind::kWait, 1, 3), kByte); }), "a channel on a delay");
    Check(Throws([] { BytesOf(NewCommand(Kind::kTranspose, 1), kByte); }), "a kind that the driver lacks");

    // A track that reaches data.
    ntdlib::commands::Sequence sequence;
    Command data;
    data.kind = Kind::kData;
    data.bytes = {0x0C};
    sequence.items = {NewCommand(Kind::kWait, 1), data, NewCommand(Kind::kEnd)};
    const std::vector<uint32_t> entries = {0};
    Check(Throws([&] { EncodeSequence(sequence, entries, kByte); }), "a track that reaches data");
    const std::vector<uint32_t> past = {3};
    Check(Throws([&] { EncodeSequence(sequence, past, kByte); }), "an entry past the items");
}

// The offsets of the items, without a sequence that reads back.
void TestItemOffsets()
{
    ntdlib::commands::Sequence sequence;
    sequence.items = {NewCommand(Kind::kTempo, 500000), NewCommand(Kind::kWait, 300), NewCommand(Kind::kProgram, 1)};
    Check(rare::ItemOffsets(sequence, kByte) == std::vector<uint32_t>({0, 4, 7, 10}), "the channel-byte offsets");
    Check(rare::ItemOffsets(sequence, kNibble) == std::vector<uint32_t>({0, 4, 7, 9}), "the channel-nibble offsets");
}

} // namespace

int main()
{
    TestChannelByte();
    TestChannelNibble();
    TestTrackEnds();
    TestOpaque();
    TestNewCommands();
    TestItemOffsets();

    return xsflib::testing::Summary("Rare's commands");
}
