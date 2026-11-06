// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a GBA ROM of Konami's driver (ntdlib/konami/layout.h), on small carts built here.
// The expected ROMs come from the driver's rules: a song table entry holds a song's address and its tracks' offsets
// from that address, and nothing else names a song's bytes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/konami/commands.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/layout.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/agb_cart.h"

using xsflib::testing::Check;

namespace
{

namespace konami = ntdlib::konami;
namespace sar = ntdlib::sar;
using konami::Layout;
using konami::Revision;
using konami::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::testing::agb_files::kRomBase;

constexpr Revision kRevision = Revision::kUltimateMasters;
constexpr uint32_t kEntrySize = 36;
constexpr uint32_t kTable = 0x100;

// The offsets of the three songs from the ROM's start.
constexpr uint32_t kSongA = 0x200;
constexpr uint32_t kSongB = 0x20B;
constexpr uint32_t kSongC = 0x214;
constexpr uint32_t kSongsEnd = 0x216;

// Song A's bytes. Track 0: its first delay, a duty byte, a delay, a PSG note, a delay of 4, a rest, a delay, the end.
// The song's other tracks start after it: a delay and the song's end.
const std::vector<uint8_t> kBytesA = {0x00, 0x81, 0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE, 0x00, 0xFD};

// Song B's bytes. Track 1: its first delay, a PSG note, a delay of 8, a rest, a delay, the end. The others as above.
const std::vector<uint8_t> kBytesB = {0x00, 0xD5, 0x20, 0x08, 0xE0, 0x00, 0xFE, 0x00, 0xFD};

// Song C's bytes: each track has a delay and the song's end.
const std::vector<uint8_t> kBytesC = {0x00, 0xFD};

// Writes a song table entry with the base `base` from the ROM's start, and the offset `rest` on each track but those
// that `first` gives.
void PutEntry(std::vector<uint8_t>& image, int song, uint32_t base, uint16_t rest, std::vector<uint16_t> first = {})
{
    const uint32_t entry = kTable + uint32_t(song) * kEntrySize;
    xsflib::StoreLe32(&image[entry], kRomBase + base);
    first.resize(16, rest);
    for (uint32_t t = 0; t < 16; t++)
    {
        xsflib::StoreLe16(&image[entry + 4 + 2 * t], first[t]);
    }
}

void PutBytes(std::vector<uint8_t>& image, uint32_t at, const std::vector<uint8_t>& bytes)
{
    std::copy(bytes.begin(), bytes.end(), image.begin() + at);
}

// A ROM of 0x1000 bytes with the song table at kTable and the three songs one after another. Bytes of 0xAB fill the
// rest, up to a run of 0xFF from `tail` to its end. With `tail` at the ROM's size, no such run exists.
std::vector<uint8_t> CartBytes(uint32_t tail = 0xC00)
{
    std::vector<uint8_t> image(0x1000, 0xAB);
    std::fill(image.begin() + tail, image.end(), uint8_t(0xFF));
    PutEntry(image, 0, kSongA, 9, {0});
    PutEntry(image, 1, kSongB, 7, {7, 0});
    PutEntry(image, 2, kSongC, 0);
    PutBytes(image, kSongA, kBytesA);
    PutBytes(image, kSongB, kBytesB);
    PutBytes(image, kSongC, kBytesC);
    return image;
}

konami::DriverInfo InfoOf(int songs = 3)
{
    konami::DriverInfo info;
    info.revision = kRevision;
    info.song_table = kRomBase + kTable;
    info.song_count = songs;
    return info;
}

Rom RomOf(std::vector<uint8_t> image)
{
    Rom rom;
    rom.Assign(std::move(image));
    return rom;
}

// Adds `size` bytes after the last item of `sequence`. No command reaches them.
void Grow(ntdlib::commands::Sequence& sequence, std::size_t size)
{
    Command data;
    data.kind = Kind::kData;
    data.bytes.assign(size, 0xFD);
    sequence.items.push_back(std::move(data));
}

// `bytes`, with `size` more bytes of what Grow adds.
std::vector<uint8_t> Grown(std::vector<uint8_t> bytes, std::size_t size)
{
    bytes.insert(bytes.end(), size, uint8_t(0xFD));
    return bytes;
}

// Moves a song's entry to `base` in `image`, and fills its old bytes with zeros.
void MoveSong(std::vector<uint8_t>& image, int song, uint32_t old_base, uint32_t old_size, uint32_t base)
{
    std::fill_n(image.begin() + old_base, old_size, uint8_t(0));
    xsflib::StoreLe32(&image[kTable + uint32_t(song) * kEntrySize], kRomBase + base);
}

void TestCut()
{
    const std::vector<uint8_t> image = CartBytes();
    const Rom rom = RomOf(image);
    const Layout layout = konami::Cut(rom, InfoOf());

    Check(layout.revision == kRevision, "the layout gives the driver's revision");
    if (!Check(layout.sequences.size() == 3 && layout.pieces.size() == 5, "three songs lie between two runs"))
    {
        return;
    }

    const auto* before = std::get_if<std::vector<uint8_t>>(&layout.pieces[0]);
    Check(before && before->size() == kSongA, "the first run ends where the first song starts");
    const uint32_t rooms[] = {uint32_t(kBytesA.size()), uint32_t(kBytesB.size()), uint32_t(kBytesC.size())};
    for (uint32_t i = 0; i < 3; i++)
    {
        const auto* piece = std::get_if<sar::SequencePiece>(&layout.pieces[i + 1]);
        Check(piece && piece->sequence == i && piece->room == rooms[i], "a song's room is the size of its bytes");
    }
    const auto* after = std::get_if<std::vector<uint8_t>>(&layout.pieces[4]);
    Check(after && after->size() == image.size() - kSongsEnd, "the last run ends with the ROM");

    // Song A: 8 items on track 0, and the other tracks' 2 after them.
    if (Check(layout.sequences[0].items.size() == 10, "song A's items"))
    {
        Check(layout.sequences[0].items[3].kind == Kind::kNote && layout.sequences[0].items[9].kind == Kind::kEnd,
              "song A's note and its last end");
    }

    // Each entry gives its base's field, then one for each of its 16 tracks.
    if (Check(layout.fields.size() == 3 * 17, "the fields of three entries"))
    {
        const sar::Field& base = layout.fields[0];
        Check(base.at == sar::Place{0, kTable} && base.from == sar::Place{0, 0} && base.to == sar::Place{1, 0, true},
              "a base's field holds the distance from the ROM's start to the song's first item");
        const sar::Field& first = layout.fields[1];
        Check(first.at == sar::Place{0, kTable + 4} && first.from == sar::Place{1, 0, true} &&
                  first.to == sar::Place{1, 0, true},
              "song A's track 0 starts at its first item");
        const sar::Field& second = layout.fields[2];
        Check(second.at == sar::Place{0, kTable + 6} && second.from == sar::Place{1, 0, true} &&
                  second.to == sar::Place{1, 8, true},
              "song A's track 1 starts at item 8");
    }

    const std::vector<std::vector<uint32_t>> entries =
        konami::TrackEntries(layout.pieces, layout.fields, layout.revision);
    if (Check(entries.size() == 5 && entries[1].size() == 16, "16 tracks of song A"))
    {
        Check(entries[0].empty() && entries[1][0] == 0 && entries[1][1] == 8, "the items where the tracks start");
    }

    // The tail of 0xFF starts at 0xC00. The free space lies 256 bytes into it.
    Check(layout.free.start == 0xD00 && layout.free.padding == 0xFF, "the free space");
    Check(konami::Join(layout) == image, "the pieces join to give the ROM back");
}

void TestSongMoves()
{
    const std::vector<uint8_t> image = CartBytes();
    const Rom rom = RomOf(image);

    // Song B grows, and goes into the free space. Its old place holds zeros, and only its base changes in the table.
    {
        Layout layout = konami::Cut(rom, InfoOf());
        Grow(layout.sequences[1], 3);
        std::vector<uint8_t> expected = image;
        MoveSong(expected, 1, kSongB, 9, 0xD00);
        PutBytes(expected, 0xD00, Grown(kBytesB, 3));
        Check(konami::Join(layout) == expected, "a song that grows goes to the free space");

        // Its cut then takes the new place as the song's.
        const Layout again = konami::Cut(RomOf(expected), InfoOf());
        Check(again.sequences.size() == 3, "the moved song cuts as a song");
        Check(konami::Join(again) == expected, "the new ROM joins to give itself back");
    }

    // A song that shrinks stays, with zeros after it.
    {
        Layout layout = konami::Cut(rom, InfoOf());
        std::vector<Command>& items = layout.sequences[0].items;
        items.erase(items.begin() + 1, items.begin() + 3); // the duty byte and the delay after it
        for (sar::Field& field : layout.fields)
        {
            if (field.to == sar::Place{1, 8, true})
            {
                field.to.offset = 6;
            }
        }

        std::vector<uint8_t> expected = image;
        PutBytes(expected, kSongA, {0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE, 0x00, 0xFD, 0x00, 0x00});
        for (uint32_t t = 1; t < 16; t++)
        {
            xsflib::StoreLe16(&expected[kTable + 4 + 2 * t], 7);
        }
        Check(konami::Join(layout) == expected, "a song that shrinks stays, with zeros after it");
    }

    // The places of two neighbouring songs count as one. The largest song goes first: song A takes the start of the
    // 20 bytes, and song B doesn't fit in the other 8.
    {
        Layout layout = konami::Cut(rom, InfoOf());
        Grow(layout.sequences[0], 1);
        Grow(layout.sequences[1], 1);
        std::vector<uint8_t> expected = image;
        MoveSong(expected, 1, kSongB, 9, 0xD00);
        PutBytes(expected, kSongA, Grown(kBytesA, 1));
        PutBytes(expected, 0xD00, Grown(kBytesB, 1));
        Check(konami::Join(layout) == expected, "neighbouring places count as one");
    }

    // The larger song A doesn't fit in either place, and goes to the free space. Song C then fits in song A's place.
    {
        Layout layout = konami::Cut(rom, InfoOf());
        Grow(layout.sequences[0], 1);
        Grow(layout.sequences[2], 7);
        std::vector<uint8_t> expected = image;
        MoveSong(expected, 0, kSongA, 11, 0xD00);
        MoveSong(expected, 2, kSongC, 2, kSongA);
        PutBytes(expected, 0xD00, Grown(kBytesA, 1));
        PutBytes(expected, kSongA, Grown(kBytesC, 7));
        Check(konami::Join(layout) == expected, "a smaller song takes the place of a larger one that moved");
    }
}

void TestFreeSpace()
{
    // A tail of fewer than 256 bytes leaves the free space past the ROM's end. The ROM grows, with the run's byte up to
    // the song.
    {
        const std::vector<uint8_t> image = CartBytes(0xF80);
        Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.free.start == 0x1100 && layout.free.padding == 0xFF, "the free space past the ROM's end");
        Grow(layout.sequences[0], 1);
        std::vector<uint8_t> expected = image;
        expected.resize(0x1100, 0xFF);
        MoveSong(expected, 0, kSongA, 11, 0x1100);
        const std::vector<uint8_t> grown = Grown(kBytesA, 1);
        expected.insert(expected.end(), grown.begin(), grown.end());
        Check(konami::Join(layout) == expected, "the ROM grows up to the song");
    }

    // A tail of zeros takes zeros as padding.
    {
        std::vector<uint8_t> image = CartBytes();
        std::fill(image.begin() + 0xFF0, image.end(), uint8_t(0));
        const Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.free.start == 0x1100 && layout.free.padding == 0, "a tail of zeros");
    }

    // Without a tail, the free space lies at the ROM's end, on a multiple of 4 bytes.
    {
        std::vector<uint8_t> image = CartBytes(0x1000);
        image.push_back(0xAB);
        const Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.free.start == 0x1004 && layout.free.padding == 0xFF, "no tail");
        Check(konami::Join(layout) == image, "a ROM without a tail comes back");
    }

    // A sample's data in the tail keeps the free space past it. The run of 0xFF then counts from the sample's end, at
    // 0xF00.
    {
        std::vector<uint8_t> image = CartBytes();
        constexpr uint32_t kSampleTable = 0xA00;
        constexpr uint32_t kHeader = 0xC00 - 12;
        xsflib::StoreLe32(&image[kSampleTable], kRomBase + kHeader);
        xsflib::StoreLe32(&image[kHeader], 0x1000);
        xsflib::StoreLe32(&image[kHeader + 4], 0x300);
        xsflib::StoreLe32(&image[kHeader + 8], 0xFFFFFFFF);
        konami::DriverInfo info = InfoOf();
        info.sample_table = kRomBase + kSampleTable;
        const Layout layout = konami::Cut(RomOf(image), info);
        Check(layout.free.start == 0x1000, "the free space lies past the bytes that the driver reads");
    }
}

// A ROM of 32 MiB, without a tail. Runs of filler lie at known places, with a sample's zeros in one of them.
std::vector<uint8_t> FullCart()
{
    std::vector<uint8_t> image(konami::kLargestRom, 0xAB);
    PutEntry(image, 0, kSongA, 9, {0});
    PutEntry(image, 1, kSongB, 7, {7, 0});
    PutEntry(image, 2, kSongC, 0);
    PutBytes(image, kSongA, kBytesA);
    PutBytes(image, kSongB, kBytesB);
    PutBytes(image, kSongC, kBytesC);

    // A run of 8 KiB of 0xFF, a run of 4 KiB of zeros, and a run of 0xFF a byte too short.
    std::fill_n(image.begin() + 0x100000, 0x2000, uint8_t(0xFF));
    std::fill_n(image.begin() + 0x200000, 0x1000, uint8_t(0));
    std::fill_n(image.begin() + 0x300000, 0xFFF, uint8_t(0xFF));

    // Sample 0: its header, then a run of 4 KiB of zeros that holds its 4 bytes of data.
    constexpr uint32_t kSampleTable = 0x1000;
    constexpr uint32_t kHeader = 0x400000 - 12;
    xsflib::StoreLe32(&image[kSampleTable], kRomBase + kHeader);
    xsflib::StoreLe32(&image[kHeader], 0x1000);
    xsflib::StoreLe32(&image[kHeader + 4], 4);
    xsflib::StoreLe32(&image[kHeader + 8], 0xFFFFFFFF);
    std::fill_n(image.begin() + 0x400000, 0x1000, uint8_t(0));

    return image;
}

bool JoinThrows(const Layout& layout, const konami::JoinOptions& options, const std::string& text)
{
    try
    {
        static_cast<void>(konami::Join(layout, options));
    }
    catch (const std::invalid_argument& e)
    {
        return std::string(e.what()).find(text) != std::string::npos;
    }

    return false;
}

void TestFiller()
{
    const std::vector<uint8_t> image = FullCart();
    const Rom rom = RomOf(image);
    konami::DriverInfo info = InfoOf();
    info.sample_table = kRomBase + 0x1000;

    const std::vector<konami::Span> runs = konami::FillerRuns(rom, info);
    const std::vector<konami::Span> expected_runs = {{0x100100, 0x1E00}, {0x200100, 0xE00}};
    Check(runs == expected_runs, "the runs of 0xFF come first, each 256 bytes clear of its ends");

    Layout layout = konami::Cut(rom, info);
    Check(layout.free.start == konami::kLargestRom, "the free space lies at the end of a full ROM");
    Check(layout.free.filler == expected_runs, "the layout keeps the filler runs");
    Check(konami::Join(layout) == image, "a full ROM comes back");

    Grow(layout.sequences[0], 0x1000);
    Grow(layout.sequences[1], 0xDF0);
    Check(JoinThrows(layout, {}, "32 MiB"), "without filler runs, a full ROM refuses a song that outgrows its place");

    // Song A takes the run of 0xFF, and song B doesn't fit in what remains of it. It takes the run of zeros.
    konami::JoinOptions options;
    options.filler = true;
    std::vector<uint8_t> expected = image;
    MoveSong(expected, 0, kSongA, 11, 0x100100);
    MoveSong(expected, 1, kSongB, 9, 0x200100);
    PutBytes(expected, 0x100100, Grown(kBytesA, 0x1000));
    PutBytes(expected, 0x200100, Grown(kBytesB, 0xDF0));
    Check(konami::Join(layout, options) == expected, "songs go into the filler runs");

    Grow(layout.sequences[2], 0x2000);
    Check(JoinThrows(layout, options, "filler"), "a song that fits nowhere is refused");
}

void TestSongsThatStay()
{
    // Song B starts inside song A: when either moved, the other would lose its bytes. Both stay, with song C alone in a
    // piece, and only song C's entry has fields.
    {
        std::vector<uint8_t> image = CartBytes();
        PutEntry(image, 1, kSongA + 9, 0);
        const Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.sequences.size() == 1 && layout.pieces.size() == 3 && layout.fields.size() == 17,
              "songs that overlap stay among the ROM's bytes");
        Check(konami::Join(layout) == image, "songs that overlap come back");
    }

    // Two entries with one base and other tracks read the bytes with other starts. Both stay, and only song C is cut.
    {
        std::vector<uint8_t> image = CartBytes();
        PutEntry(image, 1, kSongA, 9);
        const Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.sequences.size() == 1 && layout.fields.size() == 17,
              "entries with one base and other tracks stay among the ROM's bytes");
        Check(konami::Join(layout) == image, "the song with two readings comes back");
    }

    // Track 1 starts at the end command of track 0, and reads it as a delay. The decoder keeps the song as data, and
    // the song stays.
    {
        std::vector<uint8_t> image = CartBytes();
        PutEntry(image, 2, kSongC, 0, {0, 1});
        const Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.sequences.size() == 2 && layout.fields.size() == 2 * 17, "a song kept as data stays");
        Check(konami::Join(layout) == image, "a song kept as data comes back");
    }

    // Two entries that give one base and the same tracks are one song, and both follow it. No entry names song B's
    // place now. Its 9 bytes count as data of song A: bytes that no track reaches before the next song.
    {
        std::vector<uint8_t> image = CartBytes();
        PutEntry(image, 1, kSongA, 9, {0});
        Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.sequences.size() == 2 && layout.fields.size() == 3 * 17, "two entries share a song");
        Check(konami::Join(layout) == image, "the shared song comes back");

        Grow(layout.sequences[0], 1);
        std::vector<uint8_t> expected = image;
        MoveSong(expected, 0, kSongA, 20, 0xD00);
        PutEntry(expected, 1, 0xD00, 9, {0});
        PutBytes(expected, 0xD00, kBytesA);
        PutBytes(expected, 0xD00 + 11, Grown(kBytesB, 1));
        Check(konami::Join(layout) == expected, "both entries follow the shared song");
    }

    // An entry of zeros holds no song.
    {
        std::vector<uint8_t> image = CartBytes();
        std::fill_n(image.begin() + kTable + kEntrySize, kEntrySize, uint8_t(0));
        const Layout layout = konami::Cut(RomOf(image), InfoOf());
        Check(layout.sequences.size() == 2 && layout.fields.size() == 2 * 17, "an empty entry has no fields");
        Check(konami::Join(layout) == image, "a ROM with an empty entry comes back");
    }
}

void TestRefusals()
{
    const Layout cut = konami::Cut(RomOf(CartBytes()), InfoOf());

    // A track more than 0xFFFF bytes from its song's base doesn't fit in its entry's 16 bits.
    {
        Layout layout = cut;
        std::vector<Command>& items = layout.sequences[0].items;
        Command data;
        data.kind = Kind::kData;
        data.bytes.assign(0x10000, 0xFD);
        items.insert(items.begin() + 8, std::move(data));
        for (sar::Field& field : layout.fields)
        {
            if (field.to == sar::Place{1, 8, true})
            {
                field.to.offset = 9;
            }
        }
        Check(JoinThrows(layout, {}, "0xFFFF"), "a track past 0xFFFF bytes is refused");
    }

    {
        Layout layout = cut;
        layout.pieces.insert(layout.pieces.begin() + 1, sar::Padding{4});
        Check(JoinThrows(layout, {}, "padding"), "a piece of padding is refused");
    }

    {
        Layout layout = cut;
        std::get<sar::SequencePiece>(layout.pieces[1]).sequence = 3;
        Check(JoinThrows(layout, {}, "sequence"), "a piece without a sequence is refused");
    }

    {
        Layout layout = cut;
        layout.fields[0].at = sar::Place{1, 0};
        Check(JoinThrows(layout, {}, "run"), "a field outside a run is refused");
    }

    {
        Layout layout = cut;
        layout.fields[0].to.offset = 11;
        Check(JoinThrows(layout, {}, "item"), "a field that names an item past the song's end is refused");
    }

    // A ROM past 32 MiB.
    {
        Layout layout = cut;
        layout.pieces.emplace_back(std::vector<uint8_t>(konami::kLargestRom, 0));
        Check(JoinThrows(layout, {}, "32 MiB"), "a ROM past 32 MiB is refused");
    }
}

void TestPlaysTheSame()
{
    const Rom rom = RomOf(CartBytes());
    const konami::DriverInfo info = InfoOf();
    Layout layout = konami::Cut(rom, info);
    for (ntdlib::commands::Sequence& sequence : layout.sequences)
    {
        Grow(sequence, 9);
    }
    const Rom grown = RomOf(konami::Join(layout));

    bool any = false;
    for (int song = 0; song < 3; song++)
    {
        const std::unique_ptr<konami::Sequencer> a = konami::Sequencer::Create(rom, info, song);
        const std::unique_ptr<konami::Sequencer> b = konami::Sequencer::Create(grown, info, song);
        if (!Check(a->Valid() && b->Valid(), "both ROMs play the song"))
        {
            continue;
        }

        bool same = true;
        for (int frame = 0; frame < 40; frame++)
        {
            const std::array<konami::TrackOutput, konami::kTracks> first = a->Step();
            const std::array<konami::TrackOutput, konami::kTracks>& second = b->Step();
            for (int t = 0; t < konami::kTracks; t++)
            {
                const konami::TrackOutput& x = first[std::size_t(t)];
                const konami::TrackOutput& y = second[std::size_t(t)];
                same = same && x.pitch == y.pitch && x.vol == y.vol && x.trig == y.trig && x.flags == y.flags &&
                       x.b2 == y.b2 && x.active == y.active;
                any = any || x.flags != 0;
            }
        }
        Check(same, "a moved song gives the same records");
    }
    Check(any, "the songs play something");
}

void TestDetection()
{
    // Code that works out song * 36 + table with other registers than the play-song routine, and the start of a command
    // reader of the Ultimate Masters revision: it compares the command with FD third.
    constexpr uint32_t kCode = 0x800;
    const auto cart = [&]
    {
        std::vector<uint8_t> image = CartBytes();
        const uint16_t sum[] = {0x00D0, 0x1880, 0x0080, 0x4902};
        for (uint32_t i = 0; i < 4; i++)
        {
            xsflib::StoreLe16(&image[kCode + 2 * i], sum[i]);
        }
        xsflib::StoreLe32(&image[kCode + 0x10], kRomBase + kTable);
        const uint16_t reader[] = {0x781D, 0x2DFC, 0xDD1E, 0x2DFF, 0xD107, 0x2DFD,
                                   0xD100, 0x2DEF, 0xDC00, 0x2DE0, 0xD110};
        for (uint32_t i = 0; i < 11; i++)
        {
            xsflib::StoreLe16(&image[0x900 + 2 * i], reader[i]);
        }

        return image;
    };

    konami::DriverOverrides overrides;
    overrides.sample_table = kRomBase + 0xA00;
    overrides.mix_rate = 16000;
    konami::DriverInfo info;
    std::string error;
    const Rom rom = RomOf(cart());
    const bool found = konami::DetectDriver(rom, overrides, info, error);
    Check(found && info.song_table == kRomBase + kTable && info.song_count == 3, "the cart's song table");
    if (!found)
    {
        return;
    }

    // Every song moves, with bytes that no command reaches after it. The songs no longer lie one after another.
    Layout layout = konami::Cut(rom, info);
    for (ntdlib::commands::Sequence& sequence : layout.sequences)
    {
        Grow(sequence, 9);
    }
    konami::DriverInfo grown_info;
    const bool found_grown = konami::DetectDriver(RomOf(konami::Join(layout)), overrides, grown_info, error);
    Check(found_grown && grown_info.song_table == info.song_table && grown_info.song_count == info.song_count &&
              grown_info.revision == info.revision,
          "detection finds the song table of an edited ROM");
}

} // namespace

int main()
{
    TestCut();
    TestSongMoves();
    TestFreeSpace();
    TestFiller();
    TestSongsThatStay();
    TestRefusals();
    TestPlaysTheSame();
    TestDetection();

    return xsflib::testing::Summary("Konami's layout");
}
