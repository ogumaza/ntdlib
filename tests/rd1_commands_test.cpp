// SPDX-License-Identifier: MIT

// Tests of the decoder and the encoder of the MIDI files of Nintendo R&D1's GBA driver (ntdlib/rd1/commands.h). The
// files are written by hand, and the expected values come from the Standard MIDI File's format and the driver's reading
// of it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/rd1/commands.h"
#include "ntdlib/rd1/driver.h"

using xsflib::testing::Check;

namespace
{

namespace rd1 = ntdlib::rd1;
using ntdlib::agb::kRomBase;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::ToUnit;
using rd1::Decoded;
using rd1::DecodeSequence;
using rd1::EncodeSequence;

// A MIDI file of format 1 with a track chunk for each of `tracks`.
std::vector<uint8_t> File(const std::vector<std::vector<uint8_t>>& tracks, uint16_t division = 48,
                          uint16_t count = 0xFFFF)
{
    if (count == 0xFFFF)
    {
        count = uint16_t(tracks.size());
    }

    std::vector<uint8_t> bytes = {'M',
                                  'T',
                                  'h',
                                  'd',
                                  0,
                                  0,
                                  0,
                                  6,
                                  0,
                                  1,
                                  uint8_t(count >> 8),
                                  uint8_t(count),
                                  uint8_t(division >> 8),
                                  uint8_t(division)};
    for (const std::vector<uint8_t>& track : tracks)
    {
        const auto length = uint32_t(track.size());
        bytes.insert(bytes.end(), {'M', 'T', 'r', 'k', uint8_t(length >> 24), uint8_t(length >> 16),
                                   uint8_t(length >> 8), uint8_t(length)});
        bytes.insert(bytes.end(), track.begin(), track.end());
    }

    return bytes;
}

// The end of a track.
const std::vector<uint8_t> kEnd = {0, 0xFF, 0x2F, 0};

// A track with every kind of event.
const std::vector<uint8_t> kEveryEvent = {
    0,    0xFF, 0x51, 3,    0x07, 0xA1, 0x20, // a tempo of 500,000 microseconds: 120 beats a minute
    0,    0xFF, 0x06, 1,    '[',              // the loop's start
    0,    0xC3, 5,                            // program 5 on channel 3
    0,    0xB3, 7,    100,                    // volume
    0,    0xB3, 10,   0x40,                   // pan
    0,    0xB3, 11,   0x7F,                   // expression
    0,    0xB3, 1,    0x10,                   // modulation
    0,    0xB3, 20,   12,                     // bend range
    0,    0xB3, 21,   0x20,                   // the LFO's speed
    0,    0xB3, 22,   1,                      // the LFO's target
    0,    0xB3, 26,   5,                      // the LFO's delay
    0,    0xB3, 33,   2,                      // priority
    0,    0xB3, 72,   1,                      // a controller without a kind
    0,    0xE3, 0,    0x50,                   // a bend of a quarter of the range up
    0,    0x93, 60,   100,                    // a note on
    0x30, 0x83, 60,   0x40,                   // a note off after 48 ticks
    0,    0x93, 62,   0x50,                   // a note on
    0x81, 0,    62,   0,                      // a note on of velocity 0 after 128 ticks, with the running status
    0,    0xA3, 60,   0x10,                   // polyphonic pressure
    0,    0xD3, 0x20,                         // channel pressure
    0,    0xF0, 3,    1,    2,    0xF7,       // a system exclusive event
    0,    0xFF, 0x01, 2,    'h',  'i',        // a text event
    0x80, 0,    0xFF, 0x06, 1,    ']',        // the loop's end, after a delta time of 2 bytes
    0,    0xFF, 0x2F, 0,                      // the end
    0xAA, 0xBB,                               // bytes after the end
};

// Checks that `bytes` decode without a problem and encode back.
Decoded Decodes(const std::vector<uint8_t>& bytes)
{
    const Decoded decoded = DecodeSequence(bytes);
    Check(!decoded.opaque, decoded.reason.c_str());
    Check(EncodeSequence(decoded.sequence).bytes == bytes, "the items encode back to the bytes");

    return decoded;
}

// The events of a track: its items after its chunk's header, without the waits.
std::vector<Command> EventsOf(const Decoded& decoded, size_t track)
{
    const std::vector<Command>& items = decoded.sequence.items;
    const size_t end = track + 1 < decoded.tracks.size() ? decoded.tracks[track + 1] : items.size();
    std::vector<Command> events;
    for (size_t i = decoded.tracks[track] + 1; i < end; i++)
    {
        if (items[i].kind != Kind::kWait)
        {
            events.push_back(items[i]);
        }
    }

    return events;
}

// A command of `kind` with `value` in the driver's values.
Command NewCommand(Kind kind, int64_t value = 0)
{
    Command item;
    item.kind = kind;
    item.value.value = ToUnit(value, rd1::ScaleOf(kind));

    return item;
}

// A sequence of a header and one track of `items`.
Sequence OneTrack(const std::vector<Command>& items)
{
    Sequence sequence;
    sequence.items.push_back(NewCommand(Kind::kTimebase, 48));
    Command chunk = NewCommand(Kind::kOpenTrack);
    chunk.encoding.opcode = rd1::kTrackChunk;
    chunk.target = 2;
    sequence.items.push_back(chunk);
    sequence.items.insert(sequence.items.end(), items.begin(), items.end());

    return sequence;
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

void TestChunks()
{
    const Decoded decoded = Decodes(File({kEnd, kEnd}, 96));

    const std::vector<Command>& items = decoded.sequence.items;
    Check(items.size() == 7, "a header, then a chunk's header, a wait and an end for each track");
    Check(items[0].kind == Kind::kTimebase && items[0].value.value == 96 * kOne &&
              items[0].bytes == std::vector<uint8_t>{'M', 'T', 'h', 'd', 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
          "the header holds the division, and the chunk's bytes without the fields that the encoder writes");
    Check(decoded.tracks == std::vector<uint32_t>{1, 4}, "the tracks' chunks");
    Check(items[4].kind == Kind::kOpenTrack && items[4].encoding.opcode == rd1::kTrackChunk && items[4].index == 1 &&
              items[4].target == 5 && items[4].bytes == std::vector<uint8_t>{'M', 'T', 'r', 'k', 0, 0, 0, 0},
          "a track chunk's header names the track and its first item, without its length");
    Check(decoded.size == 14 + 2 * 12, "the file's bytes");
}

void TestEveryEvent()
{
    const Decoded decoded = Decodes(File({kEveryEvent, kEnd}));

    const std::vector<Command> events = EventsOf(decoded, 0);
    const std::vector<Kind> kKinds = {
        Kind::kTempo,    Kind::kLoopStart, Kind::kProgram,   Kind::kVolume,    Kind::kPan,
        Kind::kVolume2,  Kind::kLfoDepth,  Kind::kBendRange, Kind::kLfoSpeed,  Kind::kLfoType,
        Kind::kLfoDelay, Kind::kPriority,  Kind::kOther,     Kind::kPitchBend, Kind::kNote,
        Kind::kNoteOff,  Kind::kNote,      Kind::kNoteOff,   Kind::kOther,     Kind::kOther,
        Kind::kOther,    Kind::kOther,     Kind::kLoopEnd,   Kind::kEnd,       Kind::kData,
    };
    Check(events.size() == kKinds.size(), "an item for each event, and the bytes after the end");
    for (size_t i = 0; i < events.size() && i < kKinds.size(); i++)
    {
        Check(events[i].kind == kKinds[i], ("event " + std::to_string(i) + "'s kind").c_str());
    }
    if (events.size() != kKinds.size())
    {
        return;
    }

    Check(events[0].value.value == 120 * kOne && events[0].bytes == std::vector<uint8_t>{0x07, 0xA1, 0x20},
          "the tempo's beats a minute and its bytes");
    Check(events[2].value.value == 5 * kOne && events[2].encoding.opcode == 0xC3, "the program and its channel");
    Check(events[3].value.value == 100 * kOne, "the volume");
    Check(events[4].value.value == 0, "a pan of 64 is the centre");
    Check(events[7].value.value == 1200 * kOne, "the bend range in cents");
    Check(events[12].encoding.opcode == 0xB3 && events[12].bytes == std::vector<uint8_t>{72, 1},
          "a controller without a kind keeps its bytes");
    Check(events[13].value.value == kOne / 4, "the bend, a fraction of the range");
    Check(events[14].key == 60 && events[14].velocity == 100 * kOne && events[14].value.value == 0,
          "a note on has no length");
    Check(events[15].key == 60 && events[15].velocity == 0x40 * kOne && events[15].encoding.opcode == 0x83,
          "a note off keeps its release velocity");
    Check(events[17].encoding.opcode == 0x93 && events[17].encoding.repeated && events[17].velocity == 0,
          "a note on of velocity 0 is a note off, with the running status");
    Check(events[20].encoding.opcode == 0xF0 && events[20].bytes == std::vector<uint8_t>{1, 2, 0xF7},
          "a system exclusive event keeps its bytes");
    Check(events[21].encoding.opcode == (rd1::kMetaEvent | 1) && events[21].bytes == std::vector<uint8_t>{'h', 'i'},
          "another meta event keeps its type and its bytes");
    Check(events[24].bytes == std::vector<uint8_t>{0xAA, 0xBB}, "the bytes after the end");

    // The delta times.
    const std::vector<Command>& items = decoded.sequence.items;
    int long_waits = 0;
    int waits = 0;
    for (const Command& item : items)
    {
        if (item.kind == Kind::kWait)
        {
            waits++;
            long_waits += item.encoding.value_bytes == 2 && item.value.value == 0;
        }
    }

    Check(waits == 24 + 1, "a wait before each event");
    Check(long_waits == 1, "a delta time of 0 in 2 bytes");
}

void TestRunningStatus()
{
    // A meta event under the running status: the driver keeps 0xFF as the status.
    const Decoded decoded = Decodes(File({{0, 0xFF, 0x01, 1, 'a', 0, 0x02, 1, 'b', 0, 0xFF, 0x2F, 0}}));

    const std::vector<Command> events = EventsOf(decoded, 0);
    Check(events.size() == 3 && events[1].kind == Kind::kOther && events[1].encoding.opcode == (rd1::kMetaEvent | 2) &&
              events[1].encoding.repeated,
          "a meta event without its status byte");
}

void TestOpaque()
{
    const auto opaque = [](const std::vector<uint8_t>& bytes)
    {
        const Decoded decoded = DecodeSequence(bytes);
        return decoded.opaque && decoded.sequence.items.size() == 1 && decoded.sequence.items[0].kind == Kind::kData &&
               decoded.sequence.items[0].bytes == bytes && !decoded.reason.empty();
    };

    Check(opaque(File({{0, 60, 64, 0, 0xFF, 0x2F, 0}})), "a data byte before any status byte");
    Check(opaque(File({{0, 0x90, 60, 64}})), "a track without an end runs past its chunk");
    Check(opaque(File({{0, 0xFF, 0x01, 9, 'a', 0, 0xFF, 0x2F, 0}})), "a meta event that runs past its chunk");
    Check(opaque(File({kEnd}, 48, 33)), "a header that names more tracks than the driver reads");
    Check(opaque(File({kEnd}, 0)), "a division of 0");
    Check(opaque(File({kEnd}, 0x8019)), "a division in frames");

    std::vector<uint8_t> short_header = File({kEnd});
    short_header[7] = 4;
    Check(opaque(short_header), "a header chunk shorter than its fields");

    // A file cut short keeps its whole chunks.
    std::vector<uint8_t> cut = File({kEnd, kEnd});
    cut.pop_back();

    const Decoded decoded = DecodeSequence(cut);

    Check(decoded.opaque && decoded.size == 26, "a chunk that runs past the bytes");

    Check(Throws<std::invalid_argument>([] { DecodeSequence({}); }), "no bytes");
}

void TestEdits()
{
    Decoded decoded = Decodes(File({kEveryEvent, kEnd}));
    std::vector<Command>& items = decoded.sequence.items;

    // A note off under the running status turns into a note on: the status stays 0x93.
    size_t note_off = 0;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (items[i].kind == Kind::kNoteOff && items[i].encoding.repeated)
        {
            note_off = i;
        }
    }
    items[note_off].kind = Kind::kNote;
    items[note_off].velocity = 0x30 * kOne;

    // A new note after the program, without a wait: it takes a delta time of 0 and the channel of the program.
    Command note;
    note.kind = Kind::kNote;
    note.key = 64;
    note.velocity = 90 * kOne;

    size_t program = 0;
    for (size_t i = 0; i < items.size(); i++)
    {
        if (items[i].kind == Kind::kProgram)
        {
            program = i;
        }
    }
    items.insert(items.begin() + std::ptrdiff_t(program + 1), note);

    // A tempo of 150 beats a minute.
    items[3].value.value = 150 * kOne;

    const std::vector<uint8_t> bytes = EncodeSequence(decoded.sequence).bytes;
    const Decoded again = DecodeSequence(bytes);

    Check(!again.opaque, again.reason.c_str());
    const std::vector<Command> events = EventsOf(again, 0);
    Check(events[0].value.value == 150 * kOne && events[0].bytes == std::vector<uint8_t>{0x06, 0x1A, 0x80},
          "a new tempo takes the longest quarter note that gives it");
    Check(events[3].kind == Kind::kNote && events[3].key == 64 && events[3].encoding.opcode == 0x93,
          "a new note on the channel of the track's last channel event");
    Check(events[18].kind == Kind::kNote && events[18].encoding.repeated && events[18].velocity == 0x30 * kOne,
          "an edited note keeps the running status");
    Check(again.size == decoded.size + 4, "the new note's 4 bytes");

    // Two waits write their sum.
    Sequence waits = OneTrack({NewCommand(Kind::kWait, 100), NewCommand(Kind::kWait, 50), NewCommand(Kind::kEnd)});

    const rd1::Encoded encoded = EncodeSequence(waits);

    const std::vector<uint8_t> track(encoded.bytes.begin() + 22, encoded.bytes.end());
    Check(track == std::vector<uint8_t>{0x81, 0x16, 0xFF, 0x2F, 0}, "two waits write one delta time of 150");
    Check(encoded.offsets[3] == encoded.offsets[4], "the second wait writes no bytes");

    // A new file's header and track chunk.
    const std::vector<uint8_t> file(encoded.bytes.begin(), encoded.bytes.begin() + 22);
    Check(
        file == std::vector<uint8_t>{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 1, 0, 48, 'M', 'T', 'r', 'k', 0, 0, 0, 5},
        "a new header and a new track chunk");
}

void TestEncoderErrors()
{
    const auto fails = [](const Sequence& sequence)
    {
        return Throws<std::invalid_argument>([&sequence] { EncodeSequence(sequence); });
    };

    Command note = NewCommand(Kind::kNote);
    note.key = 60;
    note.velocity = 64 * kOne;
    Command data;
    data.kind = Kind::kData;
    data.bytes = {1};

    Check(!fails(OneTrack({note, NewCommand(Kind::kEnd)})), "a note and an end");
    Check(fails(OneTrack({note})), "a track without an end");
    Check(fails(OneTrack({data, NewCommand(Kind::kEnd)})), "a track that reaches data");
    Check(fails(OneTrack({NewCommand(Kind::kEnd), note})), "an event after the end");

    Sequence no_header = OneTrack({NewCommand(Kind::kEnd)});
    no_header.items.erase(no_header.items.begin());
    Check(fails(no_header), "a file without a header");

    Command silent = note;
    silent.velocity = 0;
    Check(fails(OneTrack({silent, NewCommand(Kind::kEnd)})), "a note on of velocity 0");

    Command high = note;
    high.key = 128;
    Check(fails(OneTrack({high, NewCommand(Kind::kEnd)})), "a key above 127");

    Check(fails(OneTrack({NewCommand(Kind::kTempo, 2), NewCommand(Kind::kEnd)})), "a tempo too slow for 3 bytes");
    Check(fails(OneTrack({NewCommand(Kind::kOther), NewCommand(Kind::kEnd)})), "a kOther without a status byte");
    Check(fails(OneTrack({NewCommand(Kind::kPitchBend, 0x4000), NewCommand(Kind::kEnd)})), "a bend past 14 bits");
    Check(fails(OneTrack({NewCommand(Kind::kTranspose, 1), NewCommand(Kind::kEnd)})), "a kind that a file can't hold");

    // kOther items that the driver reads as structure.
    const auto other = [](uint16_t opcode, std::vector<uint8_t> bytes)
    {
        Command item = NewCommand(Kind::kOther);
        item.encoding.opcode = opcode;
        item.bytes = std::move(bytes);
        return OneTrack({item, NewCommand(Kind::kEnd)});
    };

    Check(fails(other(rd1::kMetaEvent | 0x2F, {})), "a kOther end of track");
    Check(fails(other(rd1::kMetaEvent | 0x51, {0x07, 0xA1, 0x20})), "a kOther tempo of 3 bytes");
    Check(fails(other(rd1::kMetaEvent | 0x06, {'['})) && fails(other(rd1::kMetaEvent | 0x06, {']'})),
          "a kOther loop marker");
    Check(fails(other(0x93, {60, 100})) && fails(other(0x93, {0x85, 0x40})), "a kOther note on");
    Check(!fails(other(rd1::kMetaEvent | 0x51, {0x07, 0xA1})) && !fails(other(rd1::kMetaEvent | 0x06, {'[', '['})) &&
              !fails(other(0x93, {0x85, 0x80})) && !fails(other(0xA3, {60, 100})),
          "kOther items that the driver reads as they say");
    Check(rd1::ItemOffsets(other(rd1::kMetaEvent | 0x2F, {})).size() == 5, "a kOther end of track still has offsets");

    Sequence many = OneTrack({NewCommand(Kind::kEnd)});
    for (int t = 1; t < 32; t++)
    {
        many.items.push_back(many.items[1]);
        many.items.push_back(NewCommand(Kind::kEnd));
    }

    Check(fails(many), "32 tracks");
}

void TestItemOffsets()
{
    const std::vector<uint8_t> bytes = File({{0, 0x90, 60, 64, 0x10, 60, 0, 0, 0xFF, 0x2F, 0}});
    const Decoded decoded = Decodes(bytes);

    const std::vector<uint32_t> offsets = rd1::ItemOffsets(decoded.sequence);

    Check(offsets == std::vector<uint32_t>{0, 14, 22, 23, 26, 27, 29, 30, 33},
          "each item's offset, then the end, with an event's offset at its status byte or its first byte");

    // Items that don't read back as a file still have offsets.
    Sequence open = OneTrack({NewCommand(Kind::kWait, 1)});
    Check(rd1::ItemOffsets(open).size() == 4, "a track without an end");
}

void TestRomSequences()
{
    // Three records: two of one file, and one of another, with an empty entry between them.
    constexpr uint32_t kTable = kRomBase + 0x100;
    constexpr uint32_t kRecords = kRomBase + 0x200;
    constexpr uint32_t kFiles = kRomBase + 0x400;

    std::vector<uint8_t> image(0x800, 0);
    const auto store32 = [&image](uint32_t address, uint32_t value)
    {
        for (int b = 0; b < 4; b++)
        {
            image[address - kRomBase + uint32_t(b)] = uint8_t(value >> (8 * b));
        }
    };

    const std::vector<uint8_t> first = File({kEnd});
    const std::vector<uint8_t> second = File({kEnd, kEnd});
    std::copy(first.begin(), first.end(), image.begin() + (kFiles - kRomBase));
    std::copy(second.begin(), second.end(), image.begin() + (kFiles + 0x40 - kRomBase));

    const uint32_t kSongFiles[] = {kFiles, 0, kFiles + 0x40, kFiles};
    for (uint32_t s = 0; s < 4; s++)
    {
        if (kSongFiles[s])
        {
            store32(kTable + 8 * s, kRecords + 20 * s);
            store32(kRecords + 20 * s, kSongFiles[s]);
        }
    }

    ntdlib::agb::Rom rom;
    rom.Assign(image);
    rd1::DriverInfo info;
    info.song_table = kTable;
    info.song_count = 4;
    info.players.resize(1);
    info.bank_count = 1;

    Check(rd1::SequenceCount(rom, info) == 2, "each file once");
    Check(rd1::SequenceAddress(rom, info, 1) == kFiles + 0x40, "the kSongFiles in the order of the songs");
    Check(rd1::SequenceOfSong(rom, info, 3) == 0 && rd1::SequenceOfSong(rom, info, 2) == 1, "a song's file");
    Check(Throws<std::out_of_range>([&] { rd1::SequenceOfSong(rom, info, 1); }), "an empty entry");
    Check(Throws<std::out_of_range>([&] { rd1::SequenceAddress(rom, info, 2); }), "no third file");

    const Decoded decoded = rd1::DecodeRomSequence(rom, info, 1);

    Check(!decoded.opaque && decoded.size == second.size() && EncodeSequence(decoded.sequence).bytes == second,
          "a file's bytes end with its last chunk");
}

} // namespace

int main()
{
    TestChunks();
    TestEveryEvent();
    TestRunningStatus();
    TestOpaque();
    TestEdits();
    TestEncoderErrors();
    TestItemOffsets();
    TestRomSequences();

    return xsflib::testing::Summary("rd1_commands");
}
