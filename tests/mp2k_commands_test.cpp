// SPDX-License-Identifier: MIT

// Tests of MP2K's decoder and encoder of track commands (ntdlib/mp2k/commands.h). Each song is a string of bytes as the
// driver reads them: the codes and their arguments, worked out by hand from the driver's command table. The decoder has
// to give the items that the bytes stand for, and the encoder the same bytes.

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/mp2k/commands.h"

using xsflib::testing::Check;

namespace
{

using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Source;
using ntdlib::mp2k::Decoded;
using ntdlib::mp2k::DecodeSequence;
using ntdlib::mp2k::EncodeSequence;

// The address of the songs' first byte.
constexpr uint32_t kBase = 0x08100000;

uint32_t At(uint32_t offset)
{
    return kBase + offset;
}

void Put32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

Decoded Decode(const std::vector<uint8_t>& bytes, std::vector<uint32_t> tracks = {kBase})
{
    return DecodeSequence(bytes, kBase, tracks);
}

// Whether the items encode back to `bytes`.
bool EncodesBack(const Decoded& decoded, const std::vector<uint8_t>& bytes)
{
    return EncodeSequence(decoded.sequence, kBase).bytes == bytes;
}

bool Throws(const ntdlib::commands::Sequence& sequence, int note_arguments = 3)
{
    try
    {
        static_cast<void>(EncodeSequence(sequence, kBase, note_arguments));
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// A track's settings, notes and waits, with running statuses and notes that leave out their arguments.
void TestTrack()
{
    const std::vector<uint8_t> bytes = {
        0xBD, 0x05,             // VOICE 5
        0xBE, 0x64,             // VOL 100
        0x50,                   // VOL 80, repeated
        0xBB, 0x4B,             // TEMPO 75: 150 BPM
        0xBC, 0xFE,             // KEYSH -2
        0xBF, 0x30,             // PAN 0x30
        0xC0, 0x60,             // BEND 0x60
        0xC8, 0x50,             // TUNE 0x50
        0xE7, 0x3C, 0x64,       // N24 Cn3 v100
        0x98,                   // W24
        0x3E,                   // N24 Dn3, repeated, with the last velocity
        0x98,                   // W24
        0xE7, 0x40, 0x50, 0x01, // N24 En3 v080 gtp1: 25 ticks
        0xE7, 0x41, 0x50, 0x04, // N24 Fn3 v080 4: 28 ticks, by another code than N28's
        0xE7,                   // N24, with the last key and velocity
        0x98,                   // W24
        0xB1,                   // FINE
    };
    const Decoded decoded = Decode(bytes);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque && items.size() == 17, "the track's items: " + std::to_string(items.size()));
    if (items.size() != 17)
    {
        return;
    }

    Check(items[0].kind == Kind::kProgram && items[0].value.value == 5 * kOne, "the voice");
    Check(items[1].kind == Kind::kVolume && items[1].value.value == 100 * kOne && !items[1].encoding.repeated,
          "the volume");
    Check(items[2].kind == Kind::kVolume && items[2].value.value == 80 * kOne && items[2].encoding.repeated,
          "the repeated volume");
    Check(items[3].kind == Kind::kTempo && items[3].value.value == 150 * kOne, "the tempo in BPM");
    Check(items[4].kind == Kind::kTranspose && items[4].value.value == -200 * kOne, "the key shift in cents");
    Check(items[5].kind == Kind::kPan && items[5].value.value == -16 * kOne, "the pan from the centre");
    Check(items[6].kind == Kind::kPitchBend && items[6].value.value == kOne / 2, "the bend: half its range");
    Check(items[7].kind == Kind::kTune && items[7].value.value == 25 * kOne, "the tune: 16 64ths of a semitone");

    const Command& note = items[8];
    Check(note.kind == Kind::kNote && note.key == 60 && note.velocity == 100 * kOne && note.value.value == 24 * kOne &&
              !note.encoding.arguments && !note.encoding.length_code,
          "N24 Cn3 v100");
    Check(items[9].kind == Kind::kWait && items[9].value.value == 24 * kOne, "W24");
    const Command& repeated = items[10];
    Check(repeated.kind == Kind::kNote && repeated.key == 62 && repeated.velocity == 100 * kOne &&
              repeated.encoding.repeated && repeated.encoding.arguments == std::optional<uint8_t>(1),
          "the repeated note takes the last velocity");
    const Command& extra = items[12];
    Check(extra.value.value == 25 * kOne && !extra.encoding.arguments && !extra.encoding.length_code,
          "N24 with 1 tick more");
    const Command& other_code = items[13];
    Check(other_code.value.value == 28 * kOne && other_code.encoding.length_code == std::optional<uint8_t>(24),
          "28 ticks from N24's code");
    const Command& bare = items[14];
    Check(bare.key == 65 && bare.velocity == 80 * kOne && bare.encoding.arguments == std::optional<uint8_t>(0),
          "a note without arguments takes the last key and velocity");
    Check(items[16].kind == Kind::kEnd && items[16].encoding.opcode == 0, "FINE");
    Check(decoded.entries == std::vector<uint32_t>{0}, "the track's entry");
    Check(EncodesBack(decoded, bytes), "the track encodes back");
}

// A tie, and its end with and without the key.
void TestTie()
{
    const std::vector<uint8_t> bytes = {
        0xCF, 0x3C, 0x64, // TIE Cn3 v100
        0x98,             // W24
        0xCE, 0x3C,       // EOT Cn3
        0xCF, 0x3E,       // TIE Dn3
        0x98,             // W24
        0xCE,             // EOT, with the last key
        0xB1,             // FINE
    };
    const Decoded decoded = Decode(bytes);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items.size() == 7, "the tie's items");
    if (items.size() != 7)
    {
        return;
    }

    Check(items[0].kind == Kind::kNote && items[0].value.value == 0 && !items[0].encoding.length_code,
          "a tie is a note of length 0");
    Check(items[2].kind == Kind::kNoteOff && items[2].key == 60 && !items[2].encoding.arguments, "EOT Cn3");
    Check(items[3].velocity == 100 * kOne && items[3].encoding.arguments == std::optional<uint8_t>(1),
          "the second tie takes the velocity");
    Check(items[5].kind == Kind::kNoteOff && items[5].key == 62 &&
              items[5].encoding.arguments == std::optional<uint8_t>(0),
          "EOT with the last key");
    Check(EncodesBack(decoded, bytes), "the tie encodes back");
}

// Jumps, patterns, repeats and MEMACC's operations, with their targets as items, and the targets outside the song.
void TestFlow()
{
    std::vector<uint8_t> bytes;
    bytes.push_back(0xB3); // 0: PATT 23
    Put32(bytes, At(23));
    bytes.push_back(0xB5); // 5: REPT 2, back to 0
    bytes.push_back(0x02);
    Put32(bytes, At(0));
    bytes.insert(bytes.end(), {0xB9, 0x00, 0x05, 0x03}); // 11: MEMACC set byte 5 to 3
    bytes.insert(bytes.end(), {0xB9, 0x0C, 0x05, 0x07}); // 15: MEMACC jump if byte 5 equals byte 7
    Put32(bytes, At(0));
    bytes.push_back(0xB4); // 23: PEND, the pattern
    bytes.push_back(0xB2); // 24: GOTO outside the song
    Put32(bytes, 0x08200000);

    // The flow from the start reaches every command but GOTO through the pattern's PEND, outside a call too.
    const Decoded decoded = Decode(bytes);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque && items.size() == 6, "the flow's items: " + std::to_string(items.size()));
    if (items.size() != 6)
    {
        return;
    }

    Check(items[0].kind == Kind::kCall && items[0].target == 4 && !items[0].target_outside, "the pattern's call");
    Check(items[1].kind == Kind::kRepeat && items[1].value.value == 2 * kOne && items[1].target == 0, "the repeat");
    Check(items[2].kind == Kind::kSetVariable && items[2].index == 5 && items[2].value.value == 3 * kOne &&
              items[2].value.source == Source::kValue,
          "MEMACC's set");
    Check(items[3].kind == Kind::kJumpIfEqual && items[3].index == 5 && items[3].value.source == Source::kVariable &&
              items[3].value.value == 7 && items[3].target == 0,
          "MEMACC's jump against another byte");
    Check(items[4].kind == Kind::kReturn, "PEND");
    Check(items[5].kind == Kind::kJump && items[5].target_outside && items[5].target == 0x08200000,
          "a jump outside the song keeps its address");
    Check(EncodesBack(decoded, bytes), "the flow encodes back");

    // At another address, the targets inside the song move with it.
    const std::vector<uint8_t> moved = EncodeSequence(decoded.sequence, kBase + 0x100).bytes;
    Check(moved[1] == 0x17 && moved[2] == 0x01 && moved[3] == 0x10 && moved[25] == 0x00 && moved[27] == 0x20,
          "the encoder writes the targets at the new address");
}

// The extended commands, a long wait among them, and an extended command that ends the track.
void TestExtended()
{
    std::vector<uint8_t> bytes = {
        0xCD, 0x08, 0x05,       // xIECV 5
        0x09, 0x02,             // xIECL 2, repeated
        0xCD, 0x0C, 0x10, 0x01, // xWAIT 272
        0xCD, 0x01,             // xWAVE
    };
    Put32(bytes, 0x08300000);
    bytes.insert(bytes.end(), {0xCD, 0x00}); // an end
    const Decoded decoded = Decode(bytes);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items.size() == 5, "the extended commands' items");
    if (items.size() != 5)
    {
        return;
    }

    Check(items[0].kind == Kind::kEchoVolume && items[0].value.value == 5 * kOne, "the echo's volume");
    Check(items[1].kind == Kind::kEchoLength && items[1].encoding.repeated, "the echo's length, repeated");
    Check(items[2].kind == Kind::kWait && items[2].value.value == 272 * kOne && items[2].encoding.opcode == 0xCD0C,
          "a long wait");
    Check(items[3].kind == Kind::kSampleWave && items[3].value.value == int64_t{0x08300000} * kOne, "the sample");
    Check(items[4].kind == Kind::kEnd && items[4].encoding.opcode == 0xCD00, "the extended end");
    Check(EncodesBack(decoded, bytes), "the extended commands encode back");

    // A wait of a length that no code has can't go in, but goes in as a long wait.
    ntdlib::commands::Sequence sequence;
    Command wait;
    wait.kind = Kind::kWait;
    wait.value.value = 25 * kOne;
    sequence.items = {wait};
    Check(Throws(sequence), "a wait of 25 ticks has no code");
    sequence.items[0].encoding.opcode = 0xCD0C;
    Check(EncodeSequence(sequence, kBase).bytes == std::vector<uint8_t>{0xCD, 0x0C, 0x19, 0x00}, "a long wait of 25");
}

// The bytes that no track reaches: a FINE after a jump reads as an end, and the rest stays data.
void TestUnreached()
{
    std::vector<uint8_t> bytes = {0xBE, 0x64, 0xB2};
    Put32(bytes, At(0));
    bytes.insert(bytes.end(), {0xB1, 0x12, 0x34});
    const Decoded decoded = Decode(bytes);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items.size() == 4 && items[2].kind == Kind::kEnd && items[3].kind == Kind::kData &&
              items[3].bytes == std::vector<uint8_t>{0x12, 0x34},
          "the end after the jump, and data");
    Check(EncodesBack(decoded, bytes), "the unreached bytes encode back");

    // A byte below 0x80 at a track's start has no running status to repeat.
    const Decoded bare = Decode({0x40, 0xB1});
    Check(bare.sequence.items.size() == 1 && bare.sequence.items[0].kind == Kind::kData &&
              bare.sequence.items[0].bytes == std::vector<uint8_t>{0x40, 0xB1},
          "a repeat without a running status is data");
}

// Two tracks that reach a note with other keys: the note leaves its key out. The song stays data.
void TestOpaque()
{
    // Track 1 at 0 plays Cn3 and track 2 at 8 plays Dn3. Both go to the note at 16, which leaves its key out.
    std::vector<uint8_t> song = {0xE7, 0x3C, 0x64, 0xB2};
    Put32(song, At(16));
    song.insert(song.end(), {0xE7, 0x3E, 0x64, 0xB2});
    Put32(song, At(16));
    song.insert(song.end(), {0xE7, 0x98, 0xB1});
    const Decoded decoded = Decode(song, {At(0), At(8)});
    Check(decoded.opaque, "the note at 16 plays two keys");
    Check(decoded.entries.size() == 2 && decoded.sequence.items.size() == 2, "the song is data, split at the tracks");
    Check(EncodesBack(decoded, song), "the data encodes back");

    // With the same key, the note is the same for both tracks.
    song[9] = 0x3C;
    const Decoded same = Decode(song, {At(0), At(8)});
    Check(!same.opaque && same.sequence.items[4].key == 60, "the note at 16 plays one key");
}

// Metroid Fusion's notes, which read a key and a velocity alone. A byte below 0x80 after a note's velocity starts a
// note of the running status. The SDK's library reads the same byte as the first note's extra length. With 2 note
// arguments, a note takes the length of its code alone: N28's code is 0xE8, and 25, 27, 29 and 97 ticks have no code.
void TestShortNotes()
{
    const std::vector<uint8_t> bytes = {
        0xBD, 0x05,       // VOICE 5
        0xE7, 0x3C, 0x64, // N24 Cn3 v100
        0x3E, 0x50,       // N24 Dn3 v080, repeated
        0x98,             // W24
        0xE7, 0x40,       // N24 En3, with the last velocity
        0x98,             // W24
        0xB1,             // FINE
    };
    const Decoded decoded = DecodeSequence(bytes, kBase, std::vector<uint32_t>{kBase}, 2);
    const std::vector<Command>& items = decoded.sequence.items;
    Check(!decoded.opaque && items.size() == 7, "the short notes' items: " + std::to_string(items.size()));
    if (items.size() != 7)
    {
        return;
    }

    Check(items[1].kind == Kind::kNote && items[1].key == 60 && items[1].velocity == 100 * kOne &&
              items[1].value.value == 24 * kOne && !items[1].encoding.arguments,
          "N24 Cn3 v100 reads no extra length");
    Check(items[2].kind == Kind::kNote && items[2].key == 62 && items[2].velocity == 80 * kOne &&
              items[2].value.value == 24 * kOne && items[2].encoding.repeated && !items[2].encoding.arguments,
          "the byte after the velocity starts a repeated note");
    Check(items[4].kind == Kind::kNote && items[4].key == 64 && items[4].velocity == 80 * kOne &&
              items[4].encoding.arguments == std::optional<uint8_t>(1),
          "a note with a key alone takes the last velocity");
    Check(EncodeSequence(decoded.sequence, kBase, 2).bytes == bytes, "the short notes encode back");

    const Decoded sdk = Decode(bytes);
    Check(sdk.sequence.items.size() > 1 && sdk.sequence.items[1].value.value == (24 + 62) * kOne,
          "the SDK's library reads the byte as an extra length of 62 ticks");
    Check(Throws(decoded.sequence), "with 3 arguments, a note can't leave out its extra length before a key");

    const auto with_length = [&decoded](int64_t ticks)
    {
        ntdlib::commands::Sequence sequence = decoded.sequence;
        sequence.items[4].value.value = ticks * kOne;
        return sequence;
    };
    const std::vector<uint8_t> n28 = EncodeSequence(with_length(28), kBase, 2).bytes;
    Check(n28.size() == bytes.size() && n28[8] == 0xE8, "a note of 28 ticks takes N28's code");
    for (const int64_t ticks : {25, 27, 29, 97})
    {
        Check(Throws(with_length(ticks), 2), "no code gives a short note of " + std::to_string(ticks) + " ticks");
    }
}

// What the encoder refuses: values its bytes can't hold, a running status that the kind has none of, and a note that
// leaves out an argument before a byte below 0x80.
void TestRefusals()
{
    Command note;
    note.kind = Kind::kNote;
    note.key = 60;
    note.velocity = 100 * kOne;
    note.value.value = 24 * kOne;
    Command volume;
    volume.kind = Kind::kVolume;
    volume.value.value = 90 * kOne;
    volume.encoding.repeated = true;

    ntdlib::commands::Sequence sequence;
    sequence.items = {note, volume};
    Check(Throws(sequence), "a repeated volume after a note");

    note.encoding.arguments = 1;
    volume.encoding.repeated = false;
    sequence.items = {note, volume};
    Check(EncodeSequence(sequence, kBase).bytes == std::vector<uint8_t>{0xE7, 0x3C, 0xBE, 0x5A},
          "a note without its velocity before VOL");

    note.value.value = 25 * kOne;
    sequence.items = {note, volume};
    Check(Throws(sequence), "an extra length without the velocity's byte");

    Command tempo;
    tempo.kind = Kind::kTempo;
    tempo.value.value = 600 * kOne;
    sequence.items = {tempo};
    Check(Throws(sequence), "a tempo past 510 BPM");

    Command wait;
    wait.kind = Kind::kWait;
    wait.value.value = 24 * kOne;
    wait.encoding.repeated = true;
    sequence.items = {wait};
    Check(Throws(sequence), "a wait can't repeat the running status");

    Command send;
    send.kind = Kind::kSend;
    sequence.items = {send};
    Check(Throws(sequence), "MP2K has no sends");
}

// The usual length code of a length is the code of the longest length up to it in the driver's clock table: every
// length up to 24 ticks, then 28, 30, 32 and on up to 92 and 96.
void TestUsualLengthCode()
{
    using ntdlib::mp2k::UsualLengthCode;

    Check(ntdlib::mp2k::kLengthCodes == 49, "TIE's code and N01 to N96");
    Check(UsualLengthCode(-1) == 0 && UsualLengthCode(0) == 0, "no length");
    Check(UsualLengthCode(1) == 1 && UsualLengthCode(24) == 24, "the lengths up to 24 ticks");
    Check(UsualLengthCode(27) == 24 && UsualLengthCode(28) == 25, "a length between two codes");
    Check(UsualLengthCode(95) == 47 && UsualLengthCode(96) == 48, "N92 and N96");
    Check(UsualLengthCode(1000) == 48, "a length past N96");
}

} // namespace

int main()
{
    TestTrack();
    TestTie();
    TestShortNotes();
    TestFlow();
    TestExtended();
    TestUnreached();
    TestOpaque();
    TestRefusals();
    TestUsualLengthCode();

    return xsflib::testing::Summary("MP2K's commands");
}
