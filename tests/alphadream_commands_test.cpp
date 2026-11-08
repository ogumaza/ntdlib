// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the songs of AlphaDream's GBA driver (ntdlib/alphadream/commands.h). Each
// song is a string of bytes as the driver reads them, worked out by hand from the driver's format: a mask of the
// tracks, an offset for each of them, then the tracks' commands. The decoder has to give the items that the bytes stand
// for, and the encoder the same bytes.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/alphadream/commands.h"
#include "ntdlib/alphadream/driver.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/testing/agb_cart.h"

namespace
{

namespace alphadream = ntdlib::alphadream;
using alphadream::Decoded;
using alphadream::DecodeSequence;
using alphadream::EncodeSequence;
using alphadream::kRestFlag;
using alphadream::Version;
using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::Scale;
using ntdlib::commands::ToUnit;
using ntdlib::testing::agb_files::CartImage;
using xsflib::testing::Check;

// Where the test songs start.
constexpr uint32_t kSong = kRomBase + 0x100;

// The value that a field holds for the driver's value `value`.
Fixed Value(int64_t value)
{
    return ToUnit(value, Scale{});
}

// A ROM that holds `bytes` at kSong.
Rom SongRom(const std::vector<uint8_t>& bytes, size_t size = 0x400)
{
    CartImage cart(size);
    cart.PutBytes(kSong, bytes);

    return cart.ToRom();
}

// The kinds of the items, in order.
std::vector<Kind> Kinds(const Decoded& decoded)
{
    std::vector<Kind> kinds;
    for (const Command& item : decoded.sequence.items)
    {
        kinds.push_back(item.kind);
    }

    return kinds;
}

// Whether the encoder gives `bytes` back for what the decoder made of them.
bool RoundTrips(const Decoded& decoded, const std::vector<uint8_t>& bytes, Version version)
{
    const alphadream::Encoded encoded = EncodeSequence(decoded.sequence, version);

    return encoded.bytes == bytes && encoded.offsets.back() == bytes.size();
}

template <typename F>
bool Throws(F f)
{
    try
    {
        f();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }

    return false;
}

// A88E: tracks 0 and 2. Track 0 sets its instrument, volume and pan, plays a note and a rest, and jumps back to the
// note. Track 2 plays a tied note whose key has bit 7. A wait follows it, then a note and the end. Two bytes between
// the tracks stay as data.
void TestSuperstarSaga()
{
    const std::vector<uint8_t> kBytes = {
        0x05, 0x00, 0x06, 0x00, 0x15, 0x00,       // the mask, then the offsets of tracks 0 and 2
        0xF0, 0x01, 0xF1, 0xC8, 0xF2, 0x7F,       // 0x06: instrument 1, volume 200, pan 0x7F
        0x18, 0x3C, 0xF6, 0x0C, 0xF8, 0xF9, 0xFF, // 0x0C: a note of 24 ticks at key 60, a rest, a jump to 0x0C
        0xAA, 0xBB,                               // 0x13: unread
        0x00, 0xBC, 0x06, 0x0C, 0x40, 0xFF,       // 0x15: a tie at key 60 with a wait of 6, a note, the end
    };
    const Rom rom = SongRom(kBytes);

    const Decoded decoded = DecodeSequence(rom, Version::kSuperstarSaga, kSong);

    Check(!decoded.opaque, "A88E's song reads");
    Check(decoded.size == kBytes.size(), "the song ends at its last byte read");
    Check(Kinds(decoded) == std::vector<Kind>{Kind::kAllocateTracks, Kind::kOpenTrack, Kind::kOpenTrack, Kind::kProgram,
                                              Kind::kVolume, Kind::kPan, Kind::kNote, Kind::kWait, Kind::kJump,
                                              Kind::kData, Kind::kNote, Kind::kWait, Kind::kNote, Kind::kEnd},
          "each command and the unread bytes become an item");

    const std::vector<Command>& items = decoded.sequence.items;
    Check(items[0].value.value == Value(5), "the header's mask");
    Check(items[1].index == 0 && items[1].target == 3 && items[2].index == 2 && items[2].target == 10,
          "each track's offset names its first item");
    Check(decoded.tracks == std::vector<uint32_t>{3, 10}, "the tracks' first items");
    Check(items[3].value.value == Value(1) && items[4].value.value == Value(200) && items[5].value.value == Value(0x7F),
          "the instrument, the volume and the pan");
    Check(items[6].value.value == Value(24) && items[6].key == 60 && items[6].encoding.opcode == 0,
          "a note's length and key");
    Check(items[7].encoding.opcode == 0xF6 && items[7].value.value == Value(12), "F6 is a rest");
    Check(items[8].target == 6, "F8 jumps back by its offset from after it");
    Check(items[9].bytes == std::vector<uint8_t>{0xAA, 0xBB}, "the bytes that no track reads stay as data");
    Check(items[10].value.value == Value(0) && items[10].key == 60 && items[10].encoding.opcode == kRestFlag,
          "a tie with bit 7 in its key");
    Check(items[11].encoding.opcode == 0 && items[11].value.value == Value(6), "the tie's wait has no opcode");
    Check(RoundTrips(decoded, kBytes, Version::kSuperstarSaga), "the encoder gives A88E's bytes back");
}

// Once a note that isn't tied has bit 7 in its key, every later note that isn't tied takes a rest after it.
void TestRests()
{
    const std::vector<uint8_t> kBytes = {
        0x01, 0x00, 0x04, 0x00,       // track 0
        0x0C, 0xBC, 0x04,             // a note with bit 7, and its rest
        0x0C, 0x3E, 0x02,             // a note without bit 7: its rest follows too
        0x00, 0x40, 0x0C, 0x41, 0x01, // a tie without a wait, then a note and its rest
        0xFF,
    };
    const Rom rom = SongRom(kBytes);

    const Decoded decoded = DecodeSequence(rom, Version::kSuperstarSaga, kSong);

    Check(!decoded.opaque, "a track with rests reads");
    Check(Kinds(decoded) == std::vector<Kind>{Kind::kAllocateTracks, Kind::kOpenTrack, Kind::kNote, Kind::kWait,
                                              Kind::kNote, Kind::kWait, Kind::kNote, Kind::kNote, Kind::kWait,
                                              Kind::kEnd},
          "a rest follows each note that isn't tied once one note has had bit 7");
    Check(decoded.sequence.items[5].value.value == Value(2) && decoded.sequence.items[5].encoding.opcode == 0,
          "the second note's rest");
    Check(RoundTrips(decoded, kBytes, Version::kSuperstarSaga), "the encoder gives the rests back");

    // A loop that reaches a note before and after a note with bit 7 reads other bytes the second time.
    const std::vector<uint8_t> kLoop = {
        0x01, 0x00, 0x04, 0x00, //
        0x0C, 0x3C,             // 0x04: a note without bit 7
        0x0C, 0xBE, 0x01,       // a note with bit 7 and its rest
        0xF8, 0xF8, 0xFF,       // a jump back to 0x04
    };

    const Decoded opaque = DecodeSequence(SongRom(kLoop), Version::kSuperstarSaga, kSong);

    Check(opaque.opaque && opaque.sequence.items.size() == 1 && opaque.sequence.items[0].kind == Kind::kData,
          "a note that takes a rest on one path and not on another keeps the song as data");
}

// The counted jumps take both ways, and the LFOs' commands keep their bytes.
void TestCountedJumps()
{
    const std::vector<uint8_t> kBytes = {
        0x01, 0x00, 0x04, 0x00, //
        0xE0, 0x10, 0x04, 0x02, // 0x04: the pitch LFO's settings
        0xE1,                   // 0x08: the pitch LFO on
        0x06, 0x30,             // 0x09: a note
        0xEC, 0x02, 0xFA, 0xFF, // 0x0B: while counter 0 is within 2, back to 0x09
        0xE9, 0x01, 0x01, 0x00, // 0x0F: counter 1: once it passes 1, on to 0x14
        0xE2,                   // 0x13
        0xFF,                   // 0x14
    };

    const Decoded decoded = DecodeSequence(SongRom(kBytes), Version::kSuperstarSaga, kSong);

    Check(!decoded.opaque, "counted jumps read");
    const std::vector<Command>& items = decoded.sequence.items;
    Check(items[2].kind == Kind::kOther && items[2].encoding.opcode == 0xE0 &&
              items[2].bytes == std::vector<uint8_t>{0x10, 0x04, 0x02},
          "E0 keeps the LFO's depth, period and delay");
    Check(items[5].kind == Kind::kJumpIfLessOrEqual && items[5].index == 0 && items[5].value.value == Value(2) &&
              items[5].target == 4,
          "EC goes back while the count holds the counter");
    Check(items[6].kind == Kind::kJumpIfGreater && items[6].index == 1 && items[6].target == 8,
          "E9 goes on once its counter passes the count");
    Check(items[7].kind == Kind::kOther && items[7].encoding.opcode == 0xE2, "the jump's other way reaches E2");
    Check(RoundTrips(decoded, kBytes, Version::kSuperstarSaga), "the encoder gives the counted jumps back");
}

// B85A: a note holds the track's volume after its key, F1 is a note's length, and FA sets the channel's priority. A84J:
// F1 and FA are notes' lengths.
void TestHamtaro()
{
    const std::vector<uint8_t> kSports = {
        0x01, 0x00, 0x04, 0x00, //
        0xFA, 0x05,             // the priority
        0x0C, 0x3C, 0x64,       // a note at volume 100
        0xF1, 0x3E, 0x50,       // a note of 0xF1 ticks
        0xFF,
    };

    const Decoded decoded = DecodeSequence(SongRom(kSports), Version::kHamSports, kSong);

    Check(!decoded.opaque && Kinds(decoded) == std::vector<Kind>{Kind::kAllocateTracks, Kind::kOpenTrack,
                                                                 Kind::kPriority, Kind::kNote, Kind::kNote, Kind::kEnd},
          "B85A's notes and priority");
    Check(decoded.sequence.items[3].velocity == Value(100) && decoded.sequence.items[4].value.value == Value(0xF1),
          "a note's volume, and F1 as a length");
    Check(RoundTrips(decoded, kSports, Version::kHamSports), "the encoder gives B85A's bytes back");

    const std::vector<uint8_t> kOlder = {
        0x01, 0x00, 0x04, 0x00, //
        0xFA, 0x3C, 0x20,       // a note of 0xFA ticks
        0xF1, 0x3E, 0x40,       // a note of 0xF1 ticks
        0xFF,
    };

    const Decoded hamtaro4 = DecodeSequence(SongRom(kOlder), Version::kHamtaro4, kSong);

    Check(!hamtaro4.opaque && Kinds(hamtaro4) == std::vector<Kind>{Kind::kAllocateTracks, Kind::kOpenTrack, Kind::kNote,
                                                                   Kind::kNote, Kind::kEnd},
          "A84J's F1 and FA are lengths");
    Check(RoundTrips(hamtaro4, kOlder, Version::kHamtaro4), "the encoder gives A84J's bytes back");

    // A88E reads FA as a note's length and F1 as the volume.
    const std::vector<uint8_t> kSagaBytes = {0x01, 0x00, 0x04, 0x00, 0xFA, 0x3C, 0xF1, 0x64, 0xFF};

    const Decoded saga = DecodeSequence(SongRom(kSagaBytes), Version::kSuperstarSaga, kSong);

    Check(!saga.opaque && Kinds(saga) == std::vector<Kind>{Kind::kAllocateTracks, Kind::kOpenTrack, Kind::kNote,
                                                           Kind::kVolume, Kind::kEnd},
          "A88E's FA is a length and its F1 the volume");
}

// What the decoder keeps as data, and what the encoder refuses.
void TestLimits()
{
    const Decoded inside = DecodeSequence(SongRom({0x01, 0x00, 0x01, 0x00, 0xFF}), Version::kSuperstarSaga, kSong);
    Check(inside.opaque && inside.reason.find("inside the header") != std::string::npos,
          "a track that starts inside the header keeps the song as data");

    std::vector<uint8_t> past = {0x01, 0x00, 0x04, 0x00};
    past.resize(0x400 - 0x100, 0x30);
    const Decoded runs = DecodeSequence(SongRom(past), Version::kSuperstarSaga, kSong);
    Check(runs.opaque && runs.reason.find("past the ROM") != std::string::npos,
          "a track that runs past the ROM keeps the song as data");

    const Decoded none = DecodeSequence(SongRom({0x00, 0xF0}), Version::kSuperstarSaga, kSong);
    Check(none.opaque, "a mask without tracks 0 to 11 plays nothing");

    // A song table's word that isn't a pointer into the ROM.
    const Rom rom = SongRom({0x01, 0x00, 0x04, 0x00, 0xFF});

    const Decoded below = DecodeSequence(rom, Version::kSuperstarSaga, 0x02000000);
    const Decoded beyond = DecodeSequence(rom, Version::kSuperstarSaga, uint32_t(rom.End() + 0x100));

    Check(below.opaque && below.size == 0 && below.sequence.items.empty() && beyond.opaque && beyond.size == 0 &&
              beyond.sequence.items.empty(),
          "a song outside the ROM has no bytes");

    // Track 0 runs on into track 1. The jump at 0x08 goes back 5 bytes from its end for track 0, and past the ROM for
    // track 1: a track's jumps wrap within the 64 KiB from its start.
    const Decoded shared = DecodeSequence(SongRom({0x03, 0x00, 0x06, 0x00, 0x08, 0x00, 0xF0, 0x01, 0xF8, 0xFB, 0xFF}),
                                          Version::kSuperstarSaga, kSong);
    Check(shared.opaque && shared.reason.find("two commands") != std::string::npos,
          "a jump that two tracks read with other targets keeps the song as data");

    const std::vector<uint8_t> kBytes = {0x01, 0x00, 0x04, 0x00, 0x0C, 0x3C, 0xFF};
    const Decoded decoded = DecodeSequence(SongRom(kBytes), Version::kSuperstarSaga, kSong);

    ntdlib::commands::Sequence edited = decoded.sequence;
    edited.items[2].value.value = Value(0xF0);
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a length that reads as a command");

    edited = decoded.sequence;
    edited.items[2].velocity = Value(1);
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a volume in A88E's note");

    edited = decoded.sequence;
    edited.items[2].kind = Kind::kPriority;
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a priority in A88E");
    edited.items[2].kind = Kind::kVolume;
    Check(Throws([&] { EncodeSequence(edited, Version::kHamSports); }), "a volume command in B85A");

    edited = decoded.sequence;
    edited.items[0].value.value = Value(3);
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a mask without an offset for each track");

    // The encoder follows the tracks as the driver does.
    const std::vector<uint8_t> kSaga = {
        0x05, 0x00, 0x06, 0x00, 0x15, 0x00, 0xF0, 0x01, 0xF1, 0xC8, 0xF2, 0x7F, 0x18, 0x3C,
        0xF6, 0x0C, 0xF8, 0xF9, 0xFF, 0xAA, 0xBB, 0x00, 0xBC, 0x06, 0x0C, 0x40, 0xFF,
    };
    const Decoded two = DecodeSequence(SongRom(kSaga), Version::kSuperstarSaga, kSong);
    Check(!two.opaque, "the song of two tracks reads");

    edited = two.sequence;
    edited.items[8] = Command{};
    edited.items[8].kind = Kind::kNoteOff;
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a track that runs into data");

    edited = two.sequence;
    edited.items[13].kind = Kind::kJump;
    edited.items[13].target = 6;
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a jump before its track's start");

    edited = two.sequence;
    edited.items.erase(edited.items.begin() + 11);
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a tie with bit 7 and no wait");

    edited = two.sequence;
    edited.items[11].kind = Kind::kNoteOff;
    edited.items[11].value = {};
    edited.items[10].encoding.opcode = 0;
    edited.items[12].encoding.opcode = kRestFlag;
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a note with bit 7 and no rest");
    edited.items[11].kind = Kind::kWait;
    Check(Throws([&] { EncodeSequence(edited, Version::kSuperstarSaga); }), "a byte of ticks without its note");

    // A new note in B85A writes its volume after its key.
    edited = decoded.sequence;
    edited.items[2].velocity = Value(0x7F);

    const alphadream::Encoded sports = EncodeSequence(edited, Version::kHamSports);

    Check(sports.bytes == std::vector<uint8_t>{0x01, 0x00, 0x04, 0x00, 0x0C, 0x3C, 0x7F, 0xFF},
          "B85A's note writes its volume");
}

} // namespace

int main()
{
    TestSuperstarSaga();
    TestRests();
    TestCountedJumps();
    TestHamtaro();
    TestLimits();

    return xsflib::testing::Summary("alphadream_commands");
}
