// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the commands of Ubisoft Milan's GBA driver (ntdlib/ubimilan/commands.h). Each
// sequence is a string of bytes as the driver reads them, worked out by hand from the driver's format: the kind and
// three bytes, then <countdown> { <command> <countdown> }* <end>. The decoder has to give the items that the bytes
// stand for, and the encoder the same bytes.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/song.h"
#include "xsflib/testing/check.h"

#include "ubimilan_image.h"

namespace
{

namespace ubimilan = ntdlib::ubimilan;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::Scale;
using ntdlib::commands::ToUnit;
using ntdlib::testing::ubimilan_image::TestImage;
using ubimilan::Decoded;
using ubimilan::DecodeSequence;
using ubimilan::EncodeSequence;

// The value that a field holds for the driver's value `value`.
Fixed Value(int64_t value)
{
    return ToUnit(value, Scale{});
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

// A sequence of the driver's four kinds of command on several channels: a countdown of 3, square 2's program 5, a note
// of the kit, a command without an effect, a note-off with a second byte, and the end.
const std::vector<uint8_t> kSequence = {
    uint8_t(ubimilan::kSequence),
    0xFF,
    0xFF,
    0xFF, // the kind and three bytes
    0x03,
    0x00, // a countdown of 3
    0xC1,
    0x05,
    0x34,
    0x12, // program 5 on channel 1, a countdown of 0x1234
    0x99,
    0x28,
    0x40,
    0x00,
    0x00, // key 40 at velocity 64 on channel 9
    0xE7,
    0xFF,
    0xFF, // nothing, on channel 7, the longest countdown
    0x82,
    0x3C,
    0x7F,
    0x01,
    0x00, // a note-off of key 60 on channel 2
    0xFC, // the end
};

void TestItems()
{
    const Decoded decoded = DecodeSequence(kSequence);
    XSFLIB_CHECK(!decoded.opaque);
    XSFLIB_CHECK(decoded.size == kSequence.size());
    const std::vector<Command>& items = decoded.sequence.items;
    SEQLIB_REQUIRE(items.size() == 11);
    XSFLIB_CHECK(items[0].kind == Kind::kData);
    XSFLIB_CHECK(items[0].bytes == std::vector<uint8_t>(kSequence.begin(), kSequence.begin() + 4));
    XSFLIB_CHECK(items[1].kind == Kind::kWait && items[1].value.value == Value(3));
    XSFLIB_CHECK(items[2].kind == Kind::kProgram && items[2].index == 1 && items[2].value.value == Value(5));
    XSFLIB_CHECK(items[3].kind == Kind::kWait && items[3].value.value == Value(0x1234));
    XSFLIB_CHECK(items[4].kind == Kind::kNote && items[4].index == 9 && items[4].key == 40);
    XSFLIB_CHECK(items[4].velocity == Value(64));
    XSFLIB_CHECK(items[5].kind == Kind::kWait && items[5].value.value == Value(0));
    XSFLIB_CHECK(items[6].kind == Kind::kOther && items[6].encoding.opcode == 0xE7);
    XSFLIB_CHECK(items[7].kind == Kind::kWait && items[7].value.value == Value(0xFFFF));
    XSFLIB_CHECK(items[8].kind == Kind::kNoteOff && items[8].index == 2 && items[8].key == 60);
    XSFLIB_CHECK(items[8].velocity == Value(0x7F));
    XSFLIB_CHECK(items[9].kind == Kind::kWait && items[9].value.value == Value(1));
    XSFLIB_CHECK(items[10].kind == Kind::kEnd);
}

void TestBytesComeBack()
{
    const Decoded decoded = DecodeSequence(kSequence);
    const ubimilan::Encoded encoded = EncodeSequence(decoded.sequence);
    XSFLIB_CHECK(encoded.bytes == kSequence);

    // Each item's offset, and the end.
    const std::vector<uint32_t> offsets = {0, 4, 6, 8, 10, 13, 15, 16, 18, 21, 23, 24};
    XSFLIB_CHECK(encoded.offsets == offsets);
}

void TestBytesAfterTheEnd()
{
    // The bytes after the end stay out of the sequence. A later item of data goes after the end, and the items before
    // it keep their bytes.
    std::vector<uint8_t> bytes = kSequence;
    bytes.insert(bytes.end(), {1, 2, 3});
    Decoded decoded = DecodeSequence(bytes);
    XSFLIB_CHECK(decoded.size == kSequence.size());

    Command data;
    data.kind = Kind::kData;
    data.bytes = {1, 2, 3};
    decoded.sequence.items.push_back(data);
    XSFLIB_CHECK(EncodeSequence(decoded.sequence).bytes == bytes);
}

void TestKeptAsData()
{
    const std::vector<uint8_t> kind = {uint8_t(ubimilan::kSample), 0xFF, 0xFF, 0xFF, 0, 0, 0xFC};
    const std::vector<uint8_t> short_bytes = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0};
    const std::vector<uint8_t> no_end = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0, 0xE0, 0, 0};
    const std::vector<uint8_t> no_countdown = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0, 0xE0, 0};
    const std::vector<uint8_t> no_argument = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0, 0x90, 60};

    // Status bytes outside the driver's commands reset the game: its table of commands has no other entry.
    const std::vector<uint8_t> a0 = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0, 0xA0, 0, 0, 0xFC};
    const std::vector<uint8_t> fb = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0, 0xFB, 0, 0, 0xFC};
    for (const std::vector<uint8_t>& bytes : {kind, short_bytes, no_end, no_countdown, no_argument, a0, fb})
    {
        const Decoded decoded = DecodeSequence(bytes);
        XSFLIB_CHECK(decoded.opaque);
        XSFLIB_CHECK(!decoded.reason.empty());
        SEQLIB_REQUIRE(decoded.sequence.items.size() == 1);
        XSFLIB_CHECK(decoded.sequence.items[0].kind == Kind::kData && decoded.sequence.items[0].bytes == bytes);
        XSFLIB_CHECK(decoded.size == bytes.size());
    }

    // A command's arguments and a countdown can end on the bytes' last byte. What's missing then lies after them.
    const std::vector<uint8_t> last_argument = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0, 0x90, 60, 0x40};
    XSFLIB_CHECK(DecodeSequence(last_argument).reason == "a countdown runs past the bytes");
    const std::vector<uint8_t> last_countdown = {uint8_t(ubimilan::kSequence), 0xFF, 0xFF, 0xFF, 0, 0};
    XSFLIB_CHECK(DecodeSequence(last_countdown).reason == "the bytes hold no end");
}

void TestRefusals()
{
    const auto changed = [](size_t item, void (*change)(Command&))
    {
        Decoded decoded = DecodeSequence(kSequence);
        change(decoded.sequence.items[item]);
        return decoded.sequence;
    };

    // A value that the driver's bytes can't hold.
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(1, [](Command& c) { c.value.value = Value(0x10000); })); }));
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(2, [](Command& c) { c.value.value = Value(256); })); }));
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(4, [](Command& c) { c.velocity = Value(256); })); }));
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(4, [](Command& c) { c.velocity = Value(-1); })); }));
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(4, [](Command& c) { c.index = 16; })); }));
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(6, [](Command& c) { c.encoding.opcode = 0xD0; })); }));
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(changed(6, [](Command& c) { c.kind = Kind::kCall; })); }));

    // A sequence that the driver wouldn't read as its items say.
    Decoded decoded = DecodeSequence(kSequence);
    ntdlib::commands::Sequence sequence = decoded.sequence;
    sequence.items.erase(sequence.items.begin());
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(sequence); }));
    sequence = decoded.sequence;
    sequence.items.erase(sequence.items.begin() + 3);
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(sequence); }));
    sequence = decoded.sequence;
    sequence.items.pop_back();
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(sequence); }));
    sequence = decoded.sequence;
    sequence.items.push_back(decoded.sequence.items[2]);
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(sequence); }));
    sequence = decoded.sequence;
    sequence.items[0].bytes[0] = uint8_t(ubimilan::kSample);
    XSFLIB_CHECK(Refuses([&] { EncodeSequence(sequence); }));
}

void TestResources()
{
    TestImage image;
    const int sample = image.AddSample({1, 2, 3, 4});
    const int sequence = image.AddSequence({0, 0, 0xFC});
    const int piece = image.AddPiece(0, {sequence});
    const ntdlib::agb::Rom rom = image.ToRom();
    ubimilan::DriverInfo info;
    std::string error;
    SEQLIB_REQUIRE(ubimilan::DetectDriver(rom, {}, info, error));

    // The decoder takes the bytes up to the ROM's end, and stops at the end command.
    const Decoded decoded = ubimilan::DecodeResource(rom, info, sequence);
    XSFLIB_CHECK(!decoded.opaque && decoded.size == 7);
    XSFLIB_CHECK(Refuses([&] { ubimilan::DecodeResource(rom, info, sample); }));
    XSFLIB_CHECK(Refuses([&] { ubimilan::DecodeResource(rom, info, piece); }));
    XSFLIB_CHECK(Refuses([&] { ubimilan::DecodeResource(rom, info, 1000); }));
    XSFLIB_CHECK(Refuses([&] { ubimilan::DecodeResource(rom, info, -1); }));
}

} // namespace

int main()
{
    TestItems();
    TestBytesComeBack();
    TestBytesAfterTheEnd();
    TestKeptAsData();
    TestRefusals();
    TestResources();
    return xsflib::testing::Summary("Ubisoft Milan's commands");
}
