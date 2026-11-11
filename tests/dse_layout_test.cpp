// SPDX-License-Identifier: MIT

// Tests of DSE's tracks in the common form of the commands (ntdlib/dse/commands.h) and of the cut and the join of a set
// of files (ntdlib/dse/layout.h), on small synthetic files. The expected values and bytes follow from the driver's
// rules for an event's bytes (ntdlib/dse/formats.h) and from the writers' layout of a file, worked out by hand.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/rounding.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse = ntdlib::dse;
namespace files = ntdlib::testing::dse_song_files;
using Bytes = std::vector<uint8_t>;
using ntdlib::AlignUp;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using xsflib::testing::ErrorOf;

// Where a song's tracks start: after its header, its song chunk and its settings.
constexpr std::size_t kSongTracks = 0x80;
constexpr std::size_t kEocChunk = 16;
constexpr std::size_t kTrackStart = 20;

Sequence DecodeSong(const Bytes& song)
{
    return dse::DecodeTracks(dse::Song::Parse(song).tracks);
}

Bytes TracksOf(const Bytes& song)
{
    return {song.begin() + kSongTracks, song.end() - kEocChunk};
}

Command CommandOf(Kind kind, int64_t value = 0)
{
    Command command;
    command.kind = kind;
    command.value.value = value;
    return command;
}

// A song of one track: its preamble, then `events`.
Bytes OneTrack(const Bytes& events)
{
    Bytes track = {1, 2, 0, 0};
    track.insert(track.end(), events.begin(), events.end());
    return files::SongFile({track});
}

// The items of `events` in a song of one track, without the track's start.
std::vector<Command> Items(const Bytes& events)
{
    const Sequence sequence = DecodeSong(OneTrack(events));
    return {sequence.items.begin() + 1, sequence.items.end()};
}

// The track's bytes after its preamble, from `sequence`, a song of one track, with the padding.
Bytes EventsOf(const Sequence& sequence)
{
    const Bytes bytes = dse::EncodeTracks(sequence).bytes;
    return {bytes.begin() + kTrackStart, bytes.end()};
}

void TestTrackOpening()
{
    const Sequence sequence = DecodeSong(OneTrack({0x98}));
    XSFLIB_CHECK(sequence.items.size() == std::size_t{2});
    XSFLIB_CHECK(sequence.items[0].kind == Kind::kData);
    const Bytes start = {'t', 'r', 'k', ' ', 0, 0, 0, 1, 4, 0xFF, 0, 0, 0, 0, 0, 0, 1, 2, 0, 0};
    XSFLIB_CHECK(sequence.items[0].bytes == start);
    XSFLIB_CHECK(sequence.items[1].kind == Kind::kEnd);
    XSFLIB_CHECK(sequence.items[1].encoding.opcode == 0);

    // The encoder writes the chunk's size, and the padding of 0x98 up to a multiple of 4 bytes.
    const dse::Encoded encoded = dse::EncodeTracks(sequence);
    const Bytes expected = {'t', 'r', 'k', ' ', 0, 0, 0, 1, 4,    0xFF, 0,    0,
                            5,   0,   0,   0,   1, 2, 0, 0, 0x98, 0x98, 0x98, 0x98};
    XSFLIB_CHECK(encoded.bytes == expected);
    XSFLIB_CHECK(encoded.offsets == (std::vector<uint32_t>{0, 20, 24}));
}

void TestNewTrack()
{
    // A new track's start and an end give the file's bytes, with the chunk's size and the padding.
    Sequence sequence;
    sequence.items.push_back(dse::TrackOpening(3, 5));
    sequence.items.push_back(CommandOf(Kind::kEnd));
    const Bytes expected = {'t', 'r', 'k', ' ', 0, 0, 0, 1, 4,    0xFF, 0,    0,
                            5,   0,   0,   0,   3, 5, 0, 0, 0x98, 0x98, 0x98, 0x98};
    XSFLIB_CHECK(dse::EncodeTracks(sequence).bytes == expected);
}

void TestReadTrackOpening()
{
    // The start of OneTrack's track gives its number 1 and its channel 2. A start of 20 bytes needs the whole label of
    // a track chunk.
    const Sequence sequence = DecodeSong(OneTrack({0x98}));
    Command other = sequence.items[0];
    other.bytes[4] = 1;
    const Command end = sequence.items[1];

    const std::optional<dse::TrackStart> read = dse::ReadTrackOpening(sequence.items[0]);

    XSFLIB_CHECK(read == (dse::TrackStart{.number = 1, .channel = 2}));
    XSFLIB_CHECK(dse::ReadTrackOpening(dse::TrackOpening(3, 5)) == (dse::TrackStart{.number = 3, .channel = 5}));
    XSFLIB_CHECK(!dse::ReadTrackOpening(other));
    XSFLIB_CHECK(!dse::ReadTrackOpening(end));
}

void TestSetTrackOpening()
{
    // The number and the channel go after the chunk's header. The preamble's other two bytes stay.
    Command opening = dse::TrackOpening(0, 0);
    opening.bytes[18] = 0xAA;
    Command end = CommandOf(Kind::kEnd);

    dse::SetTrackOpening(opening, {.number = 4, .channel = 15});

    const Bytes expected = {'t', 'r', 'k', ' ', 0, 0, 0, 1, 4, 0xFF, 0, 0, 0, 0, 0, 0, 4, 15, 0xAA, 0};
    XSFLIB_CHECK(opening.bytes == expected);
    XSFLIB_CHECK(!ErrorOf<std::invalid_argument>([&] { dse::SetTrackOpening(end, {}); }).empty());
}

void TestNotes()
{
    // A track starts at octave 4, with no length before.
    const std::vector<Command> items = Items({
        0x40, 0x65, 0x30,             // key 5 of octave 4, with a length of 48
        0x50, 0x27,                   // key 7 of octave 4, that takes the length before
        0x60, 0xB0, 0x01, 0x00,       // key 0 of octave 5 with a length of 256
        0x70, 0x03,                   // key 3 of octave 3
        0x7F, 0xC0, 0x01, 0x00, 0x00, // key 0 of octave 1, with a length of 65,536 in 3 bytes
        0x01, 0xE0, 0x00, 0x00, 0x05, // the same octave with the length 5 in 3 bytes
        0x98,
    });
    XSFLIB_CHECK(items.size() == std::size_t{7});
    XSFLIB_CHECK(items[0].kind == Kind::kNote);
    XSFLIB_CHECK(items[0].key == 53);
    XSFLIB_CHECK(items[0].velocity == 0x40 * kOne);
    XSFLIB_CHECK(items[0].value.value == 48 * kOne);
    XSFLIB_CHECK(!items[0].encoding.repeated);
    XSFLIB_CHECK(items[0].encoding.value_bytes == 0);
    XSFLIB_CHECK(!items[0].encoding.arguments);

    XSFLIB_CHECK(items[1].key == 55);
    XSFLIB_CHECK(items[1].value.value == 48 * kOne);
    XSFLIB_CHECK(items[1].encoding.repeated);

    XSFLIB_CHECK(items[2].key == 60);
    XSFLIB_CHECK(items[2].value.value == 256 * kOne);
    XSFLIB_CHECK(items[2].encoding.value_bytes == 0);

    XSFLIB_CHECK(items[3].key == 39);
    XSFLIB_CHECK(!items[3].encoding.arguments);
    XSFLIB_CHECK(items[3].value.value == 256 * kOne);

    // An octave out of reach of the note before can only come from 2 octaves down each: 3, then 1.
    XSFLIB_CHECK(items[4].key == 12);
    XSFLIB_CHECK(items[4].value.value == 65536 * kOne);
    XSFLIB_CHECK(items[4].encoding.value_bytes == 0);

    XSFLIB_CHECK(items[5].key == 12);
    XSFLIB_CHECK(items[5].value.value == 5 * kOne);
    XSFLIB_CHECK(items[5].encoding.value_bytes == 3);
}

void TestKeysThatANewNoteDoesNotTake()
{
    // Key 12 of octave 4 is key 0 of octave 5, in other bits. Octaves below 0 wrap as the driver's sums do.
    const Bytes events = {
        0x40, 0x6C, 0x30, // key 12 of octave 4: the key 60
        0xA0, 0xFF,       // octave -1
        0x40, 0x23,       // key 3 of octave -1: the key 247
        0x98,
    };
    const std::vector<Command> items = Items(events);
    XSFLIB_CHECK(items[0].key == 60);
    XSFLIB_CHECK(*items[0].encoding.arguments == 0x2C);
    XSFLIB_CHECK(items[2].key == 247);
    XSFLIB_CHECK(*items[2].encoding.arguments == 0x23);
    XSFLIB_CHECK(TracksOf(OneTrack(events)) == dse::EncodeTracks(DecodeSong(OneTrack(events))).bytes);
}

void TestWaits()
{
    const std::vector<Command> items = Items({
        0x85,                   // a pause of 32
        0x90,                   // 32 again
        0x91, 0xFC,             // 28
        0x91, 0x02,             // 30
        0x92, 0x40,             // 64, which a pause holds
        0x92, 0x41,             // 65
        0x93, 0x00, 0x01,       // 256
        0x94, 0x00, 0x00, 0x01, // 65,536
        0x93, 0x05, 0x00,       // 5 in 2 bytes
        0x98,
    });
    const std::vector<int64_t> ticks = {32, 32, 28, 30, 64, 65, 256, 65536, 5};
    const std::vector<uint16_t> opcodes = {0, 0x90, 0x91, 0x91, 0x92, 0, 0, 0, 0x93};
    for (std::size_t i = 0; i < ticks.size(); i++)
    {
        XSFLIB_CHECK(items[i].kind == Kind::kWait);
        XSFLIB_CHECK(items[i].value.value == ticks[i] * kOne);
        XSFLIB_CHECK(items[i].encoding.opcode == opcodes[i]);
    }
}

void TestCommands()
{
    const std::vector<Command> items = Items({
        0xA4, 0x78,             // tempo
        0xA5, 0x64,             // tempo again
        0xAC, 0x07,             // program
        0xE0, 0x64,             // volume
        0xE3, 0x7F,             // expression
        0xE8, 0x50,             // pan
        0xDB, 0x0C,             // bend range
        0x9C, 0x03,             // a repeat of 3
        0x9D,                   // its end
        0x99,                   // the main loop
        0xD4, 0x30, 0x00, 0x02, // a fade of the bend, which the common form has no kind for
        0x98,
    });
    XSFLIB_CHECK(items[0].kind == Kind::kTempo);
    XSFLIB_CHECK(items[0].value.value == 0x78 * kOne);
    XSFLIB_CHECK(items[0].encoding.opcode == 0);
    XSFLIB_CHECK(items[1].kind == Kind::kTempo);
    XSFLIB_CHECK(items[1].encoding.opcode == 0xA5);
    XSFLIB_CHECK(items[2].kind == Kind::kProgram);
    XSFLIB_CHECK(items[2].value.value == 7 * kOne);
    XSFLIB_CHECK(items[3].kind == Kind::kVolume);
    XSFLIB_CHECK(items[4].kind == Kind::kVolume2);
    XSFLIB_CHECK(items[5].kind == Kind::kPan);
    XSFLIB_CHECK(items[5].value.value == 16 * kOne);
    XSFLIB_CHECK(items[6].kind == Kind::kBendRange);
    XSFLIB_CHECK(items[6].value.value == 1200 * kOne);
    XSFLIB_CHECK(items[7].kind == Kind::kLoopStart);
    XSFLIB_CHECK(items[7].value.value == 3 * kOne);
    XSFLIB_CHECK(items[8].kind == Kind::kLoopEnd);
    XSFLIB_CHECK(items[9].kind == Kind::kOther);
    XSFLIB_CHECK(items[9].encoding.opcode == 0x99);
    XSFLIB_CHECK(items[9].bytes.empty());
    XSFLIB_CHECK(items[10].kind == Kind::kOther);
    XSFLIB_CHECK(items[10].encoding.opcode == 0xD4);
    XSFLIB_CHECK(items[10].bytes == (Bytes{0x30, 0x00, 0x02}));
    XSFLIB_CHECK(items[11].kind == Kind::kEnd);
}

void TestTrackEnds()
{
    // A code that the driver takes as invalid ends the track, and the bytes after it stay unread.
    const Bytes unread = files::SongFile({{1, 2, 0, 0, 0x96, 0x11, 0x22, 0x33}});
    const Sequence with_unread = DecodeSong(unread);
    XSFLIB_CHECK(with_unread.items.size() == std::size_t{3});
    XSFLIB_CHECK(with_unread.items[1].kind == Kind::kEnd);
    XSFLIB_CHECK(with_unread.items[1].encoding.opcode == 0x96);
    XSFLIB_CHECK(with_unread.items[2].kind == Kind::kData);
    XSFLIB_CHECK(with_unread.items[2].bytes == (Bytes{0x11, 0x22, 0x33}));
    XSFLIB_CHECK(dse::EncodeTracks(with_unread).bytes == TracksOf(unread));

    // A track that fills its chunk without an end.
    const Bytes no_end = OneTrack({0x92, 0x41, 0x99, 0x99});
    const Sequence full = DecodeSong(no_end);
    XSFLIB_CHECK(full.items.size() == std::size_t{4});
    XSFLIB_CHECK(dse::EncodeTracks(full).bytes == TracksOf(no_end));
}

void TestEndInThePadding()
{
    Bytes song = OneTrack({0x92, 0x41, 0x99, 0x98});
    xsflib::StoreLe32(&song[kSongTracks + 12], 7);
    const dse::Song read = dse::Song::Parse(song);
    XSFLIB_CHECK(read.tracks[0].end_in_padding);
    Sequence sequence = DecodeSong(song);
    XSFLIB_CHECK(sequence.items.back().kind == Kind::kEnd);
    XSFLIB_CHECK(sequence.items.back().encoding.value_bytes == 1);
    XSFLIB_CHECK(dse::EncodeTracks(sequence).bytes == TracksOf(song));

    // An event more leaves no room for the end in the padding: the size then counts the end.
    Command loop = CommandOf(Kind::kOther);
    loop.encoding.opcode = 0x99;
    sequence.items.insert(sequence.items.begin() + 3, loop);
    const Bytes bytes = dse::EncodeTracks(sequence).bytes;
    XSFLIB_CHECK(xsflib::LoadLe32(&bytes[12]) == uint32_t{9});
    XSFLIB_CHECK(bytes == (Bytes{'t', 'r', 'k', ' ', 0, 0, 0,    1,    4,    0xFF, 0,    0,    9,    0,
                                 0,   0,   1,   2,   0, 0, 0x92, 0x41, 0x99, 0x99, 0x98, 0x98, 0x98, 0x98}));
}

void TestDataAfterAnEndInThePadding()
{
    // The bytes after the end count in the chunk's size. The end then lies inside the chunk.
    Bytes song = OneTrack({0x92, 0x41, 0x99, 0x98});
    xsflib::StoreLe32(&song[kSongTracks + 12], 7);
    Sequence sequence = DecodeSong(song);
    Command data = CommandOf(Kind::kData);
    data.bytes = {0x11, 0x22, 0x33, 0x44};
    sequence.items.push_back(data);
    XSFLIB_CHECK(xsflib::LoadLe32(&dse::EncodeTracks(sequence).bytes[12]) == 12);

    // An empty run counts nothing.
    sequence.items.back().bytes = {};
    XSFLIB_CHECK(xsflib::LoadLe32(&dse::EncodeTracks(sequence).bytes[12]) == 7);
}

void TestEveryEventComesBack()
{
    for (const Bytes& track : files::Tracks())
    {
        const Bytes song = files::SongFile({track});
        XSFLIB_CHECK(dse::EncodeTracks(DecodeSong(song)).bytes == TracksOf(song));
    }

    const Bytes song = files::SongFile();
    const Sequence sequence = DecodeSong(song);
    const dse::Encoded encoded = dse::EncodeTracks(sequence);
    XSFLIB_CHECK(encoded.bytes == TracksOf(song));
    XSFLIB_CHECK(encoded.offsets.size() == sequence.items.size() + 1);
    XSFLIB_CHECK(encoded.offsets.back() == encoded.bytes.size());
}

void TestEditOfAKey()
{
    // Keys 53, 55 and 67, each after the octave of the note before.
    const Bytes events = {0x40, 0x65, 0x30, 0x40, 0x27, 0x40, 0x37, 0x98};
    Sequence sequence = DecodeSong(OneTrack(events));
    XSFLIB_CHECK(EventsOf(sequence) == events);

    // The first note goes up an octave. The second keeps its key, an octave down from the first, and the third its
    // octave.
    sequence.items[1].key = 65;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0x75, 0x30, 0x40, 0x17, 0x40, 0x37, 0x98}));

    // An octave out of reach of the note before starts the note's bytes with the octave's command. The note after it
    // keeps its key the same way.
    sequence.items[1].key = 53;
    sequence.items[2].key = 115;
    const dse::Encoded encoded = dse::EncodeTracks(sequence);
    const Bytes expected = {0x40, 0x65, 0x30, 0xA0, 0x09, 0x40, 0x27, 0xA0, 0x05, 0x40, 0x27, 0x98};
    XSFLIB_CHECK(Bytes(encoded.bytes.begin() + kTrackStart, encoded.bytes.end()) == expected);

    // The offsets name each note's code, where the driver's event starts, past the octave's command.
    XSFLIB_CHECK(encoded.offsets[2] == kTrackStart + 5);
    XSFLIB_CHECK(encoded.offsets[3] == kTrackStart + 9);
    XSFLIB_CHECK(encoded.offsets[4] == kTrackStart + 11);
    const Sequence read = DecodeSong(OneTrack(expected));
    XSFLIB_CHECK(read.items[2].kind == Kind::kOther);
    XSFLIB_CHECK(read.items[3].key == 115);
    XSFLIB_CHECK(read.items[4].kind == Kind::kOther);
    XSFLIB_CHECK(read.items[5].key == 67);
}

void TestNoteAtALoopsStart()
{
    // A new note after a loop's start or a repeat's start sets its octave. Each pass then plays it alike. The note
    // after it keeps its key through the new octave.
    for (const Bytes& start : {Bytes{0x99}, Bytes{0x9C, 0x02}})
    {
        Bytes events = start;
        const Bytes rest = {0xA0, 0x05, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98};
        events.insert(events.end(), rest.begin(), rest.end());
        Sequence sequence = DecodeSong(OneTrack(events));
        Command note = sequence.items[3];
        note.key = 49;
        sequence.items.insert(sequence.items.begin() + 2, note);

        Bytes expected = start;
        const Bytes after = {0xA0, 0x04, 0x40, 0x61, 0x30, 0xA0, 0x05, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98};
        expected.insert(expected.end(), after.begin(), after.end());
        const Bytes bytes = dse::EncodeTracks(sequence).bytes;
        XSFLIB_CHECK(Bytes(bytes.begin() + kTrackStart, bytes.begin() + kTrackStart + expected.size()) == expected);
    }

    // A first note without the command keeps its bytes, and an edit of its key sets the octave.
    const Bytes events = {0x99, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98, 0x98};
    Sequence sequence = DecodeSong(OneTrack(events));
    XSFLIB_CHECK(EventsOf(sequence) == events);
    sequence.items[2].key = 50;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x99, 0xA0, 0x04, 0x40, 0x62, 0x30, 0x92, 0x30, 0x98, 0x98, 0x98, 0x98}));

    // A note after the first takes its octave from the note before.
    sequence = DecodeSong(OneTrack({0x99, 0xA0, 0x04, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98}));
    Command later = sequence.items[3];
    later.key = 62;
    sequence.items.insert(sequence.items.begin() + 4, later);
    const Bytes later_bytes = dse::EncodeTracks(sequence).bytes;
    const Bytes later_expected = {0x99, 0xA0, 0x04, 0x40, 0x65, 0x30, 0x40, 0x72, 0x30};
    XSFLIB_CHECK(Bytes(later_bytes.begin() + kTrackStart, later_bytes.begin() + kTrackStart + later_expected.size()) ==
                 later_expected);
}

void TestReachOfAnOctave()
{
    // A note reaches 2 octaves down and 1 up from the note before. A wider step sets the octave first.
    Sequence sequence = DecodeSong(OneTrack({0x40, 0x60, 0x30, 0x40, 0x20, 0x98}));
    sequence.items[2].key = 24;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0x60, 0x30, 0x40, 0x00, 0x98, 0x98, 0x98}));
    sequence.items[2].key = 12;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0x60, 0x30, 0xA0, 0x01, 0x40, 0x20, 0x98}));
    sequence.items[2].key = 60;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0x60, 0x30, 0x40, 0x30, 0x98, 0x98, 0x98}));
    sequence.items[2].key = 72;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0x60, 0x30, 0xA0, 0x06, 0x40, 0x20, 0x98}));
}

void TestEachTrackStartsAnew()
{
    // The first track ends on octave 5, with a length of 48 and a wait of 65. The second track's items keep their
    // values by the driver's state at a track's start: octave 4, and nothing before.
    const Bytes song =
        files::SongFile({{0, 0, 0, 0, 0x40, 0x70, 0x30, 0x92, 0x41, 0x98}, {1, 1, 0, 0, 0x40, 0x25, 0x90, 0x98}});
    const Sequence sequence = DecodeSong(song);
    XSFLIB_CHECK(sequence.items[5].key == 53);
    XSFLIB_CHECK(sequence.items[5].value.value == 0);
    XSFLIB_CHECK(sequence.items[6].value.value == 0);
    XSFLIB_CHECK(dse::EncodeTracks(sequence).bytes == TracksOf(song));
}

void TestEditOfAnOctaveCommand()
{
    // An edit of the octave's command changes what the notes after it take: the notes keep their keys.
    Sequence sequence = DecodeSong(OneTrack({0xA0, 0x05, 0x40, 0x64, 0x30, 0x98}));
    XSFLIB_CHECK(sequence.items[2].key == 64);
    sequence.items[1].bytes = {0x04};
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0xA0, 0x04, 0x40, 0x74, 0x30, 0x98, 0x98, 0x98}));
}

void TestEditOfALength()
{
    // A length of 48, then a note that leaves its length out.
    const Bytes events = {0x40, 0x65, 0x30, 0x40, 0x27, 0x98};
    Sequence sequence = DecodeSong(OneTrack(events));

    // The second note writes its length once the first note's changes.
    sequence.items[1].value.value = 300 * kOne;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0xA5, 0x01, 0x2C, 0x40, 0x67, 0x30, 0x98}));

    // A note that takes the length before again leaves it out.
    sequence.items[2].value.value = 300 * kOne;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x40, 0xA5, 0x01, 0x2C, 0x40, 0x27, 0x98, 0x98}));

    // A length in more bytes than it requires keeps them, until it requires more.
    Sequence padded = DecodeSong(OneTrack({0x40, 0xA5, 0x00, 0x05, 0x98}));
    XSFLIB_CHECK(padded.items[1].encoding.value_bytes == 2);
    padded.items[1].value.value = 6 * kOne;
    XSFLIB_CHECK(EventsOf(padded) == (Bytes{0x40, 0xA5, 0x00, 0x06, 0x98, 0x98, 0x98, 0x98}));
    padded.items[1].value.value = 70000 * kOne;
    XSFLIB_CHECK(EventsOf(padded) == (Bytes{0x40, 0xE5, 0x01, 0x11, 0x70, 0x98, 0x98, 0x98}));
}

void TestEditOfAWait()
{
    const Bytes events = {0x92, 0x41, 0x90, 0x91, 0x02, 0x98};
    Sequence sequence = DecodeSong(OneTrack(events));
    XSFLIB_CHECK(EventsOf(sequence) == Bytes({0x92, 0x41, 0x90, 0x91, 0x02, 0x98, 0x98, 0x98}));

    // The first wait takes a pause's code. The waits after it keep their ticks, 65 and 67.
    sequence.items[1].value.value = 96 * kOne;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x80, 0x92, 0x41, 0x91, 0x02, 0x98, 0x98, 0x98}));

    // A wait out of reach of its code takes another: the code of a new wait.
    sequence.items[3].value.value = 300 * kOne;
    XSFLIB_CHECK(EventsOf(sequence) == (Bytes{0x80, 0x92, 0x41, 0x93, 0x2C, 0x01, 0x98, 0x98}));

    // A change of the wait before reaches 127 up and 128 down.
    Sequence change = DecodeSong(OneTrack({0x92, 0xFF, 0x91, 0x7F, 0x98}));
    change.items[2].value.value = (255 + 127) * kOne;
    XSFLIB_CHECK(EventsOf(change) == (Bytes{0x92, 0xFF, 0x91, 0x7F, 0x98, 0x98, 0x98, 0x98}));
    change.items[2].value.value = (255 + 128) * kOne;
    XSFLIB_CHECK(EventsOf(change) == (Bytes{0x92, 0xFF, 0x93, 0x7F, 0x01, 0x98, 0x98, 0x98}));
    change.items[2].encoding.opcode = 0x91;
    change.items[2].value.value = (255 - 128) * kOne;
    XSFLIB_CHECK(EventsOf(change) == (Bytes{0x92, 0xFF, 0x91, 0x80, 0x98, 0x98, 0x98, 0x98}));
    change.items[2].value.value = (255 - 129) * kOne;
    XSFLIB_CHECK(EventsOf(change) == (Bytes{0x92, 0xFF, 0x92, 0x7E, 0x98, 0x98, 0x98, 0x98}));

    // New waits take a pause's code, or the fewest bytes.
    Sequence limits = DecodeSong(OneTrack({0x98}));
    limits.items.insert(limits.items.begin() + 1, CommandOf(Kind::kWait, 255 * kOne));
    limits.items.insert(limits.items.begin() + 2, CommandOf(Kind::kWait, 65535 * kOne));
    limits.items.insert(limits.items.begin() + 3, CommandOf(Kind::kWait, 0xFFFFFF * kOne));
    XSFLIB_CHECK(EventsOf(limits) == (Bytes{0x92, 0xFF, 0x93, 0xFF, 0xFF, 0x94, 0xFF, 0xFF, 0xFF, 0x98, 0x98, 0x98}));

    Sequence fresh = DecodeSong(OneTrack({0x98}));
    fresh.items.insert(fresh.items.begin() + 1, CommandOf(Kind::kWait, 96 * kOne));
    fresh.items.insert(fresh.items.begin() + 2, CommandOf(Kind::kWait, 97 * kOne));
    fresh.items.insert(fresh.items.begin() + 3, CommandOf(Kind::kWait, 256 * kOne));
    fresh.items.insert(fresh.items.begin() + 4, CommandOf(Kind::kWait, 65536 * kOne));
    XSFLIB_CHECK(EventsOf(fresh) == (Bytes{0x80, 0x92, 0x61, 0x93, 0x00, 0x01, 0x94, 0x00, 0x00, 0x01, 0x98, 0x98}));
}

void TestNewCommands()
{
    Sequence sequence = DecodeSong(OneTrack({0x98}));
    const auto add = [&sequence](Kind kind, int64_t value)
    {
        sequence.items.insert(sequence.items.end() - 1, CommandOf(kind, value));
    };
    add(Kind::kTempo, 120 * kOne);
    add(Kind::kProgram, 3 * kOne);
    add(Kind::kVolume, 100 * kOne);
    add(Kind::kVolume2, 90 * kOne);
    add(Kind::kPan, -14 * kOne);
    add(Kind::kBendRange, 200 * kOne);
    add(Kind::kLoopStart, 2 * kOne);
    add(Kind::kLoopEnd, 0);
    XSFLIB_CHECK(EventsOf(sequence) ==
                 (Bytes{0xA4, 120, 0xAC, 3, 0xE0, 100, 0xE3, 90, 0xE8, 50, 0xDB, 2, 0x9C, 2, 0x9D, 0x98}));
}

void TestRefusals()
{
    const auto refused = [](const Sequence& sequence)
    {
        return ErrorOf<std::invalid_argument>([&sequence] { dse::EncodeTracks(sequence); }).size() > 0;
    };

    Sequence no_start;
    no_start.items.push_back(CommandOf(Kind::kEnd));
    XSFLIB_CHECK(refused(no_start));

    const Sequence base = DecodeSong(OneTrack({0x98}));
    const auto with = [&base](const Command& command)
    {
        Sequence sequence = base;
        sequence.items.insert(sequence.items.begin() + 1, command);
        return sequence;
    };
    XSFLIB_CHECK(!refused(with(CommandOf(Kind::kProgram, 255 * kOne))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kJump))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kProgram, 256 * kOne))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kProgram, -kOne))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kPan, 200 * kOne))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kWait, 0x1000000 * kOne))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kWait, -kOne))));
    XSFLIB_CHECK(!refused(with(CommandOf(Kind::kWait, 0xFFFFFF * kOne))));
    XSFLIB_CHECK(refused(with(CommandOf(Kind::kNote, 0x1000000 * kOne))));
    XSFLIB_CHECK(!refused(with(CommandOf(Kind::kNote, 0xFFFFFF * kOne))));

    Command note = CommandOf(Kind::kNote);
    note.velocity = 128 * kOne;
    XSFLIB_CHECK(refused(with(note)));
    note.velocity = 127 * kOne;
    XSFLIB_CHECK(!refused(with(note)));

    Command conditional = CommandOf(Kind::kProgram);
    conditional.conditional = true;
    XSFLIB_CHECK(refused(with(conditional)));

    Command timed = CommandOf(Kind::kProgram);
    timed.time = ntdlib::commands::Argument{};
    XSFLIB_CHECK(refused(with(timed)));

    Command variable = CommandOf(Kind::kProgram);
    variable.value.source = ntdlib::commands::Source::kVariable;
    XSFLIB_CHECK(refused(with(variable)));

    // A kOther command holds a code from 0x90 on, with that code's arguments.
    Command other = CommandOf(Kind::kOther);
    XSFLIB_CHECK(refused(with(other)));
    other.encoding.opcode = 0xD4;
    XSFLIB_CHECK(refused(with(other)));
    other.bytes = {0x30, 0x00, 0x02};
    XSFLIB_CHECK(!refused(with(other)));
    other.encoding.opcode = 0x96;
    other.bytes = {};
    XSFLIB_CHECK(refused(with(other)));
    other.encoding.opcode = 0x180;
    XSFLIB_CHECK(refused(with(other)));
    other.encoding.opcode = 0x85;
    XSFLIB_CHECK(refused(with(other)));
    other.encoding.opcode = 0x40;
    other.bytes = {0x60};
    XSFLIB_CHECK(refused(with(other)));
}

// The files of a set: a song, a bank, a bank of sound effects and a file that isn't DSE's.
struct SetFiles
{
    Bytes song = files::SongFile();
    Bytes bank = files::BankFile();
    Bytes effects =
        files::EffectsFile({files::Effect{.tracks = {{0, 0, 0, 0, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98}}}, std::nullopt,
                            files::Effect{.tracks = {{0, 0, 0, 0, 0x40, 0x67, 0x18, 0x92, 0x18, 0x98},
                                                     {1, 1, 0, 0, 0x99, 0x92, 0x10, 0x98}}}},
                           0x6430);
    Bytes other = {1, 2, 3, 4, 5};

    std::vector<dse::NamedFile> List() const
    {
        return {{"SOUND/BGM/bgm.smd", song},
                {"SOUND/BGM/bgm.swd", bank},
                {"SOUND/SE/se.sed", effects},
                {"other.bin", other}};
    }
};

void CheckFiles(std::span<const dse::NamedFile> read, std::span<const dse::NamedFile> expected)
{
    XSFLIB_CHECK(read.size() == expected.size());
    for (std::size_t i = 0; i < read.size() && i < expected.size(); i++)
    {
        XSFLIB_CHECK(read[i].path == expected[i].path);
        XSFLIB_CHECK(std::ranges::equal(read[i].bytes, expected[i].bytes));
    }
}

void TestJoinGivesTheFilesBack()
{
    const SetFiles files;
    const dse::Layout layout = dse::Cut(files.List());
    XSFLIB_CHECK(layout.sequences.size() == std::size_t{3});
    XSFLIB_CHECK(layout.sequences[0].labels == (std::vector<ntdlib::commands::Label>{{"SOUND_BGM_bgm_smd", 0}}));
    XSFLIB_CHECK(layout.sequences[1].labels == (std::vector<ntdlib::commands::Label>{{"SOUND_SE_se_sed_0", 0}}));
    XSFLIB_CHECK(layout.sequences[2].labels == (std::vector<ntdlib::commands::Label>{{"SOUND_SE_se_sed_2", 0}}));

    const Bytes set = dse::Join(layout.sequences, layout.pieces, layout.fields);
    XSFLIB_CHECK(dse::IsSet(set));
    CheckFiles(dse::ReadSet(set), files.List());

    // Each file starts on a multiple of 16 bytes, and the directory gives the set's size.
    for (const dse::NamedFile& file : dse::ReadSet(set))
    {
        XSFLIB_CHECK((file.bytes.data() - set.data()) % 16 == 0);
    }
    XSFLIB_CHECK(xsflib::LoadLe32(&set[4]) == set.size());
    XSFLIB_CHECK(xsflib::LoadLe32(&set[8]) == uint32_t{4});

    const std::vector<uint64_t> starts = dse::Starts(layout.sequences, layout.pieces);
    XSFLIB_CHECK(starts.size() == layout.pieces.size() + 1);
    XSFLIB_CHECK(starts.back() == set.size());

    // A set without files.
    const dse::Layout empty = dse::Cut({});
    const Bytes no_files = dse::Join(empty.sequences, empty.pieces, empty.fields);
    XSFLIB_CHECK(no_files.size() == std::size_t{16});
    XSFLIB_CHECK(dse::ReadSet(no_files).empty());
}

void TestGrowthOfASong()
{
    const SetFiles files;
    dse::Layout layout = dse::Cut(files.List());

    // The song's first track grows by 3 bytes, up to the next multiple of 4.
    Sequence& song = layout.sequences[0];
    song.items.insert(song.items.begin() + 1, CommandOf(Kind::kWait, 300 * kOne));
    const Bytes set = dse::Join(layout.sequences, layout.pieces, layout.fields);
    const std::vector<dse::NamedFile> read = dse::ReadSet(set);
    const std::size_t track = files::Tracks()[0].size();
    XSFLIB_CHECK(read[0].bytes.size() == files.song.size() + AlignUp(track + 3, 4) - AlignUp(track, 4));
    XSFLIB_CHECK(xsflib::LoadLe32(&read[0].bytes[8]) == read[0].bytes.size());
    const dse::Song parsed = dse::Song::Parse(read[0].bytes);
    XSFLIB_CHECK(parsed.tracks.size() == files::Tracks().size());
    XSFLIB_CHECK(parsed.tracks[0].events[0].code == 0x93);
    XSFLIB_CHECK(dse::DecodeTracks(parsed.tracks).items == song.items);

    // The other files keep their bytes.
    XSFLIB_CHECK(std::ranges::equal(read[1].bytes, files.bank));
    XSFLIB_CHECK(std::ranges::equal(read[2].bytes, files.effects));
    XSFLIB_CHECK(std::ranges::equal(read[3].bytes, files.other));
}

void TestGrowthOfASoundEffect()
{
    const SetFiles files;
    dse::Layout layout = dse::Cut(files.List());
    const dse::SoundEffects before = dse::SoundEffects::Parse(files.effects);
    const uint16_t last = xsflib::LoadLe16(&files.effects[0x50 + 4]);
    XSFLIB_CHECK(xsflib::LoadLe16(&files.effects[0x50 + 2]) == 0);

    // 6 bytes more take the first effect's track from 10 bytes, in a chunk of 12, to 16.
    Sequence& effect = layout.sequences[1];
    effect.items.insert(effect.items.begin() + 1, 2, CommandOf(Kind::kWait, 300 * kOne));
    const Bytes set = dse::Join(layout.sequences, layout.pieces, layout.fields);
    const std::vector<dse::NamedFile> read = dse::ReadSet(set);

    const std::span<const uint8_t> bytes = read[2].bytes;
    XSFLIB_CHECK(xsflib::LoadLe16(&bytes[0x50]) == xsflib::LoadLe16(&files.effects[0x50]));
    XSFLIB_CHECK(xsflib::LoadLe16(&bytes[0x50 + 2]) == 0);
    XSFLIB_CHECK(xsflib::LoadLe16(&bytes[0x50 + 4]) == last + 4);

    // The seq chunk grows by 4 bytes, and what follows it starts on a multiple of 16.
    const std::size_t old_end = 0x50 + xsflib::LoadLe32(&files.effects[0x4C]);
    XSFLIB_CHECK(xsflib::LoadLe32(&bytes[0x4C]) == xsflib::LoadLe32(&files.effects[0x4C]) + 4);
    XSFLIB_CHECK(bytes.size() == files.effects.size() - AlignUp(old_end, 16) + AlignUp(old_end + 4, 16));
    XSFLIB_CHECK(xsflib::LoadLe32(&bytes[8]) == bytes.size());

    const dse::SoundEffects after = dse::SoundEffects::Parse(bytes);
    XSFLIB_CHECK(after.sequences[1] == before.sequences[1]);
    XSFLIB_CHECK(after.sequences[2] == before.sequences[2]);
    XSFLIB_CHECK(after.macros == before.macros);
    XSFLIB_CHECK(after.bank_lists == before.bank_lists);
    XSFLIB_CHECK(after.sequences[0]->tracks[0].events[0].code == 0x93);
}

void TestSoundEffectsPast16Bits()
{
    const SetFiles files;
    dse::Layout layout = dse::Cut(files.List());

    // The last effect's slot passes 64 KiB, and the table can't reach it.
    Sequence& effect = layout.sequences[1];
    effect.items.insert(effect.items.begin() + 1, 21845, CommandOf(Kind::kWait, 300 * kOne));
    const auto error =
        ErrorOf<std::invalid_argument>([&layout] { dse::Join(layout.sequences, layout.pieces, layout.fields); });
    XSFLIB_CHECK(!error.empty());
    XSFLIB_CHECK(error.find("doesn't fit") != std::string::npos);

    // A little less fits.
    effect.items.erase(effect.items.begin() + 1, effect.items.begin() + 1000);
    const Bytes set = dse::Join(layout.sequences, layout.pieces, layout.fields);
    XSFLIB_CHECK(dse::SoundEffects::Parse(dse::ReadSet(set)[2].bytes).sequences[0]->tracks[0].events.size() > 20000);
}

void TestFilesThatStayRuns()
{
    // A padding other than 0x98 keeps a song in one run.
    Bytes song = OneTrack({0x98});
    song[kSongTracks + kTrackStart + 1] = 0x00;

    // A byte other than zero after the seq chunk keeps a bank of sound effects in one run. So does another byte after
    // the last effect's eoc chunk that the chunk's size counts.
    Bytes effects = files::EffectsFile({files::Effect{.tracks = {{0, 0, 0, 0, 0x98, 0x98, 0x98, 0x98}}}}, 0x6430);
    const uint32_t chunk_end = 0x50 + xsflib::LoadLe32(&effects[0x4C]);
    XSFLIB_CHECK(chunk_end % 16 != 0);
    Bytes other_padding = effects;
    other_padding[chunk_end] = 1;
    Bytes longer_chunk = effects;
    xsflib::StoreLe32(&longer_chunk[0x4C], xsflib::LoadLe32(&effects[0x4C]) + 4);

    const Bytes damaged = {'s', 'm', 'd', 'l', 0};
    const Bytes damaged_effects = {'s', 'e', 'd', 'l', 0};
    const std::vector<dse::NamedFile> list = {{"song.smd", song},
                                              {"other_padding.sed", other_padding},
                                              {"longer_chunk.sed", longer_chunk},
                                              {"damaged.smd", damaged},
                                              {"damaged.sed", damaged_effects}};
    const dse::Layout layout = dse::Cut(list);
    XSFLIB_CHECK(layout.sequences.empty());
    CheckFiles(dse::ReadSet(dse::Join(layout.sequences, layout.pieces, layout.fields)), list);

    // The same bank without the change becomes pieces.
    const std::vector<dse::NamedFile> whole = {{"effects.sed", effects}};
    const dse::Layout cut = dse::Cut(whole);
    XSFLIB_CHECK(cut.sequences.size() == std::size_t{1});
    CheckFiles(dse::ReadSet(dse::Join(cut.sequences, cut.pieces, cut.fields)), whole);
}

void TestJoinRefusals()
{
    using ntdlib::sar::Field;
    using ntdlib::sar::Padding;
    using ntdlib::sar::Piece;
    using ntdlib::sar::Place;
    using ntdlib::sar::SequencePiece;

    const auto refused = [](const std::vector<Piece>& pieces, const std::vector<Field>& fields)
    {
        return ErrorOf<std::invalid_argument>([&] { dse::Join({}, pieces, fields); }).size() > 0;
    };

    const std::vector<Piece> pieces = {Bytes(8, 0), Padding{16}, Bytes(8, 0)};
    XSFLIB_CHECK(!refused(pieces, {}));
    XSFLIB_CHECK(refused({SequencePiece{.sequence = 1}}, {}));

    // At, from and to must lie in the set.
    const auto field = [](const Place& at, const Place& from, const Place& to, uint8_t bytes = 4)
    {
        return std::vector<Field>{Field{at, from, to, bytes}};
    };
    const Place start{.piece = 0};
    const Place end{.piece = 3};
    XSFLIB_CHECK(!refused(pieces, field(start, start, end)));
    XSFLIB_CHECK(refused(pieces, field({.piece = 1}, start, end)));
    XSFLIB_CHECK(refused(pieces, field(end, start, end)));
    XSFLIB_CHECK(refused(pieces, field({.piece = 0, .offset = 5}, start, end)));
    XSFLIB_CHECK(!refused(pieces, field({.piece = 0, .offset = 6}, start, end, 2)));
    XSFLIB_CHECK(refused(pieces, field({.piece = 0, .offset = 7}, start, end, 2)));
    XSFLIB_CHECK(refused(pieces, field(start, start, {.piece = 4})));
    XSFLIB_CHECK(refused(pieces, field(start, start, {.piece = 0, .offset = 9})));
    XSFLIB_CHECK(!refused(pieces, field(start, start, {.piece = 0, .offset = 8})));
    XSFLIB_CHECK(refused(pieces, field(start, start, {.piece = 1, .offset = 1})));
    XSFLIB_CHECK(refused(pieces, field(start, {.piece = 2}, start)));
    XSFLIB_CHECK(refused(pieces, field(start, start, end, 3)));
    XSFLIB_CHECK(refused(pieces, field({.piece = 0, .item = true}, start, end)));
    XSFLIB_CHECK(refused(pieces, field(start, {.piece = 0, .item = true}, end)));
    XSFLIB_CHECK(refused(pieces, field(start, start, {.piece = 3, .sequence = true})));

    // A 16-bit field writes 2 bytes, and a 32-bit field 4.
    const std::vector<Field> fields = {Field{start, start, end, 2},
                                       Field{{.piece = 0, .offset = 4}, {.piece = 2}, end}};
    const Bytes joined = dse::Join({}, pieces, fields);
    XSFLIB_CHECK(joined.size() == std::size_t{24});
    XSFLIB_CHECK(Bytes(joined.begin(), joined.begin() + 8) == (Bytes{24, 0, 0, 0, 8, 0, 0, 0}));
}

void TestReadSetRefusals()
{
    const auto refused = [](const Bytes& bytes)
    {
        return ErrorOf<xsflib::FormatError>([&bytes] { dse::ReadSet(bytes); }).size() > 0;
    };

    XSFLIB_CHECK(refused({}));
    XSFLIB_CHECK(refused(Bytes(16, 0)));

    // A directory that names more files than the bytes hold, and a file past the end.
    Bytes set = {'D', 'S', 'E', 'T', 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    XSFLIB_CHECK(refused(set));
    set.resize(28);
    xsflib::StoreLe32(&set[16], 16);
    xsflib::StoreLe32(&set[20], 100);
    xsflib::StoreLe32(&set[24], 27);
    XSFLIB_CHECK(refused(set));
}

void TestLabels()
{
    const dse::SequenceRef song = {.path = "SOUND/BGM/bgm.smd"};
    const dse::SequenceRef effect = {.path = "SOUND/SE/se.sed", .effect = 12};
    XSFLIB_CHECK(dse::SequenceLabel(song) == "SOUND_BGM_bgm_smd");
    XSFLIB_CHECK(dse::SequenceLabel(effect) == "SOUND_SE_se_sed_12");
    XSFLIB_CHECK(dse::SequenceLabel({.path = "1.smd"}) == "_1_smd");

    const SetFiles files;
    const std::vector<dse::NamedFile> list = files.List();
    XSFLIB_CHECK(dse::SequenceNamed("SOUND_BGM_bgm_smd", list) == song);
    XSFLIB_CHECK(dse::SequenceNamed("SOUND_SE_se_sed_12", list) == effect);
    XSFLIB_CHECK(!dse::SequenceNamed("SOUND_BGM_none_smd", list));
    XSFLIB_CHECK(!dse::SequenceNamed("SOUND_SE_se_sed_x", list));
    XSFLIB_CHECK(!dse::SequenceNamed("SOUND_SE_se_sed_70000", list));
    XSFLIB_CHECK(!dse::SequenceNamed("SOUND_SE_se_sed_", list));
    XSFLIB_CHECK(!dse::SequenceNamed("none", list));
    XSFLIB_CHECK(!dse::SequenceNamed("SOUND_SE_se_sed_12x", list));
    XSFLIB_CHECK(!dse::SequenceNamed("SOUND_SE_none_sed_12", list));
}

void TestTracksOffset()
{
    const SetFiles files;
    XSFLIB_CHECK(*dse::TracksOffset(files.song, std::nullopt) == kSongTracks);

    // The table's 3 slots take the first 16 bytes after 0x50, then the first effect's settings.
    XSFLIB_CHECK(*dse::TracksOffset(files.effects, 0) == uint32_t{0x50 + 16 + 0x30});
    XSFLIB_CHECK(!dse::TracksOffset(files.effects, 1));
    XSFLIB_CHECK(*dse::TracksOffset(files.effects, 2) ==
                 uint32_t(0x50 + xsflib::LoadLe16(&files.effects[0x54]) + 0x30));
    XSFLIB_CHECK(!dse::TracksOffset(files.effects, 3));
    XSFLIB_CHECK(!dse::TracksOffset(Bytes(16, 0), 0));

    // A table that fills its 16 bytes: the slot after it would read the first effect's settings.
    const Bytes full = files::EffectsFile({files::Effect{.tracks = {{0, 0, 0, 0, 0x98}}}, std::nullopt, std::nullopt,
                                           std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
                                          0x6430);
    XSFLIB_CHECK(dse::TracksOffset(full, 0).has_value());
    XSFLIB_CHECK(!dse::TracksOffset(full, 8));
}

void TestScales()
{
    XSFLIB_CHECK(ntdlib::commands::ToUnit(64, dse::ScaleOf(Kind::kPan)) == 0);
    XSFLIB_CHECK(ntdlib::commands::ToUnit(127, dse::ScaleOf(Kind::kPan)) == 63 * kOne);
    XSFLIB_CHECK(ntdlib::commands::ToUnit(12, dse::ScaleOf(Kind::kBendRange)) == 1200 * kOne);
    XSFLIB_CHECK(ntdlib::commands::ToUnit(100, dse::ScaleOf(Kind::kVolume)) == 100 * kOne);
    XSFLIB_CHECK(ntdlib::commands::ToUnit(24, dse::ScaleOf(Kind::kNote)) == 24 * kOne);
}

} // namespace

int main()
{
    TestTrackOpening();
    TestNewTrack();
    TestReadTrackOpening();
    TestSetTrackOpening();
    TestNotes();
    TestKeysThatANewNoteDoesNotTake();
    TestWaits();
    TestCommands();
    TestTrackEnds();
    TestEndInThePadding();
    TestDataAfterAnEndInThePadding();
    TestEveryEventComesBack();
    TestEditOfAKey();
    TestNoteAtALoopsStart();
    TestReachOfAnOctave();
    TestEachTrackStartsAnew();
    TestEditOfAnOctaveCommand();
    TestEditOfALength();
    TestEditOfAWait();
    TestNewCommands();
    TestRefusals();
    TestJoinGivesTheFilesBack();
    TestGrowthOfASong();
    TestGrowthOfASoundEffect();
    TestSoundEffectsPast16Bits();
    TestFilesThatStayRuns();
    TestJoinRefusals();
    TestReadSetRefusals();
    TestLabels();
    TestTracksOffset();
    TestScales();

    return xsflib::testing::Summary("dse_layout_test");
}
