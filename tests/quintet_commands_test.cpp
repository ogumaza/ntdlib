// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of Quintet's channel commands (ntdlib/quintet/commands.h), on songs built by
// hand.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/commands.h"

using xsflib::testing::Check;

namespace
{

namespace quintet = ntdlib::quintet;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;

// The note values of a test driver's length table: whole, half, quarter and so on, then a quarter again.
quintet::Traits TraitsA()
{
    quintet::Traits traits;
    traits.lengths = {1, 2, 4, 8, 16, 32, 64, 3, 6, 12, 24, 48, 4};
    return traits;
}

quintet::Traits TraitsS()
{
    quintet::Traits traits;
    traits.byte_notes = true;
    return traits;
}

std::vector<uint8_t> Encode(const quintet::Decoded& decoded, const quintet::Traits& traits)
{
    return quintet::EncodeSequence(decoded.sequence, decoded.entries, traits).bytes;
}

// Two channels: notes, a rest, a play again, a repeat, a tie, commands with arguments and their ends. A byte after the
// first channel's end that no channel reaches stays data.
void TestDecodeA()
{
    const quintet::Traits traits = TraitsA();
    const std::vector<uint8_t> data = {
        0x7F, 90,                  // tempo 90
        0x5E, 15,                  // volume 15
        0xD6,                      // repeat from here
        0x20,                      // C, a quarter note: 96 ticks
        0xD4, 0x30,                // tie to C, an eighth
        0x2D,                      // rest, a quarter
        0xD7, 2,                   // twice
        0x2C,                      // play again, a quarter
        0xC0,                      // C, a quarter note from index 12
        0xE6, 0x10, 0xFF,          // detune -240
        0xFF,                      // end
        0x99,                      // nothing reads it
        0xDC, 3,                   // sample 3
        0xF1, 0xFE,                // volume -2
        0xD2, 1,    2,    3, 4, 5, // volume envelope
        0x1A,                      // A#, a half note
        0xFF,
    };
    const std::vector<uint32_t> starts = {0, 18};
    const quintet::Decoded decoded = quintet::DecodeSequence(data, starts, traits);
    Check(!decoded.opaque, "the song reads");
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items.size() == 18, "an item for each command and the data");
    Check(decoded.entries == std::vector<uint32_t>{0, 13}, "the channels' entries");
    Check(items[0].kind == Kind::kTempo && items[0].value.value == 90 * kOne, "tempo");
    Check(items[3].kind == Kind::kNote && items[3].key == 0 && items[3].value.value == 96 * kOne &&
              !items[3].encoding.length_code,
          "a note");
    Check(items[4].kind == Kind::kTie, "a tie");
    Check(items[6].kind == Kind::kNoteOff && items[6].value.value == 96 * kOne, "a rest");
    Check(items[7].kind == Kind::kLoopEnd && items[7].value.value == 2 * kOne, "a repeat's end");
    Check(items[8].kind == Kind::kNote && items[8].index == 1, "play again");
    Check(items[9].kind == Kind::kNote && items[9].encoding.length_code == 12, "a note of the table's second quarter");
    Check(items[10].kind == Kind::kTune && items[10].index == 1 && items[10].value.value == -240 * kOne, "a detune");
    Check(items[12].kind == Kind::kData && items[12].bytes == std::vector<uint8_t>{0x99}, "data");
    Check(items[13].kind == Kind::kProgram && items[13].value.value == 3 * kOne, "a sample");
    Check(items[14].kind == Kind::kVolume && items[14].index == 1 && items[14].value.value == -2 * kOne, "a step");
    Check(items[15].kind == Kind::kOther && items[15].encoding.opcode == 0xD2 &&
              items[15].bytes == std::vector<uint8_t>{1, 2, 3, 4, 5},
          "an envelope keeps its arguments");
    Check(Encode(decoded, traits) == data, "the items encode to the bytes");

    // A repeat of 256 passes, written with 0.
    const std::vector<uint8_t> repeat = {0xD6, 0x20, 0xD7, 0, 0xFF};
    const quintet::Decoded passes = quintet::DecodeSequence(repeat, std::vector<uint32_t>{0}, traits);
    Check(passes.sequence.items[2].value.value == 256 * kOne, "a repeat of 0 makes 256 passes");
    Check(Encode(passes, traits) == repeat, "and encodes to 0");
}

// The S revision: notes of two bytes, waits, and the commands numbered from 80.
void TestDecodeS()
{
    const quintet::Traits traits = TraitsS();
    const std::vector<uint8_t> data = {
        0x8F, 90,   // tempo 90 (7F)
        0x00, 8,    // C, an eighth
        0x9B,       // dot (D1)
        0x9E,       // tie (D4)
        0x00, 8,    // C, an eighth
        0x0E, 4,    // play again, a quarter
        0x0F, 4,    // rest, a quarter
        0x0C, 8,    // a wait
        0xB0, 0x8A, // skips a byte
        0xBC,       // nothing
        0x00, 200,  // C, 1 tick, by a value that isn't the lowest
        0xFF,
    };
    const quintet::Decoded decoded = quintet::DecodeSequence(data, std::vector<uint32_t>{0}, traits);
    Check(!decoded.opaque, "the song reads");
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items.size() == 12, "an item for each command");
    Check(items[0].kind == Kind::kTempo, "8F is the tempo");
    Check(items[1].kind == Kind::kNote && items[1].value.value == 48 * kOne, "an eighth");
    Check(items[2].kind == Kind::kOther && items[2].encoding.opcode == 0xD1, "a dot");
    Check(items[3].kind == Kind::kTie, "a tie");
    Check(items[5].kind == Kind::kNote && items[5].index == 1, "play again");
    Check(items[6].kind == Kind::kNoteOff, "a rest");
    Check(items[7].kind == Kind::kWait && items[7].key == 12, "a wait");
    Check(items[8].kind == Kind::kOther && items[8].encoding.opcode == quintet::kKeptByte + 0xB0 &&
              items[8].bytes == std::vector<uint8_t>{0x8A},
          "B0 keeps the byte it skips");
    Check(items[9].kind == Kind::kOther && items[9].encoding.opcode == quintet::kKeptByte + 0xBC,
          "a byte without a command");
    Check(items[10].encoding.length_code == 200, "a note value that isn't the lowest for its ticks");
    Check(Encode(decoded, traits) == data, "the items encode to the bytes");
}

// The encoder writes the usual bytes of edited items, and refuses what the driver can't hold.
void TestEncode()
{
    const quintet::Traits traits = TraitsA();
    ntdlib::commands::Sequence sequence;
    Command note;
    note.kind = Kind::kNote;
    note.key = 4;
    note.value.value = 48 * kOne;
    Command end;
    end.kind = Kind::kEnd;
    sequence.items = {note, end};
    const std::vector<uint32_t> entries = {0};
    Check(quintet::EncodeSequence(sequence, entries, traits).bytes == std::vector<uint8_t>{0x34, 0xFF},
          "an eighth note takes index 3");

    sequence.items[0].value.value = 5 * kOne;
    bool threw = false;
    try
    {
        quintet::EncodeSequence(sequence, entries, traits);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "no note value gives 5 ticks");

    // A channel that runs into data. The data's bytes read as a note and an end.
    ntdlib::commands::Sequence open;
    Command data;
    data.kind = Kind::kData;
    data.bytes = {0x20, 0xFF};
    open.items = {note, data};
    open.items[0].value.value = 96 * kOne;
    std::string message;
    try
    {
        quintet::EncodeSequence(open, entries, traits);
    }
    catch (const std::invalid_argument& e)
    {
        message = e.what();
    }
    Check(message == "a channel reaches bytes that no item starts at", "a channel that reaches data");

    // The S revision has no F1.
    ntdlib::commands::Sequence step;
    Command volume;
    volume.kind = Kind::kVolume;
    volume.index = 1;
    volume.value.value = -1 * kOne;
    step.items = {volume, end};
    threw = false;
    try
    {
        quintet::EncodeSequence(step, entries, TraitsS());
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    Check(threw, "the S revision has no step of the volume");
}

// A song whose channels can't be read safely stays data, split at the channels' starts.
void TestOpaque()
{
    const quintet::Traits traits = TraitsA();
    const std::vector<uint8_t> past = {0x20, 0x5E};
    const quintet::Decoded ran = quintet::DecodeSequence(past, std::vector<uint32_t>{0}, traits);
    Check(ran.opaque && ran.reason == "a channel runs past the song's bytes", "a channel without an end");
    Check(Encode(ran, traits) == past, "the data gives the bytes back");

    // The second channel starts inside the first channel's tempo.
    const std::vector<uint8_t> overlap = {0x7F, 0x20, 0xFF};
    const quintet::Decoded both = quintet::DecodeSequence(overlap, std::vector<uint32_t>{0, 1}, traits);
    Check(both.opaque && both.reason == "two commands overlap", "commands that overlap");
    Check(both.entries == std::vector<uint32_t>{0, 1}, "split at the starts");
    Check(Encode(both, traits) == overlap, "the data gives the bytes back");
}

} // namespace

int main()
{
    TestDecodeA();
    TestDecodeS();
    TestEncode();
    TestOpaque();

    return xsflib::testing::Summary("Quintet's commands");
}
