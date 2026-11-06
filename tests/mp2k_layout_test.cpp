// SPDX-License-Identifier: MIT

// Tests of the cut and the join of a GBA ROM's MP2K songs (ntdlib/mp2k/layout.h), on small synthetic cartridges. The
// expected places come from the rules in layout.h: a song that fits stays in its place, and one that outgrows it moves
// to the free space with a copy of its header, which the song table then names. In a ROM of 32 MiB, such a song goes
// into a filler run where the options allow it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "seqlib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/layout.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/layout.h"
#include "ntdlib/testing/mp2k_cart.h"

namespace
{

using ntdlib::agb::kRomBase;
using ntdlib::agb::Rom;
using ntdlib::commands::Command;
using ntdlib::commands::Kind;
using ntdlib::mp2k::Cut;
using ntdlib::mp2k::DecodeSong;
using ntdlib::mp2k::DetectDriver;
using ntdlib::mp2k::DriverInfo;
using ntdlib::mp2k::FillerRuns;
using ntdlib::mp2k::HeaderTracks;
using ntdlib::mp2k::Join;
using ntdlib::mp2k::JoinOptions;
using ntdlib::mp2k::kFreeSpaceMargin;
using ntdlib::mp2k::kLargestRom;
using ntdlib::mp2k::kLeastFillerRun;
using ntdlib::mp2k::Layout;
using ntdlib::mp2k::ReadSong;
using ntdlib::mp2k::ReadSongHeader;
using ntdlib::mp2k::SongHeader;
using ntdlib::mp2k::Span;
using ntdlib::testing::mp2k_cart::Cart;
using ntdlib::testing::mp2k_cart::kSongTable;
using ntdlib::testing::mp2k_cart::Saw;
using ntdlib::testing::mp2k_cart::Track;
using enum ntdlib::mp2k::Command;

// The size of the test cartridges, and where their runs of 0xFF bytes start: a long run, a run shorter than the free
// space's margin, and none.
constexpr uint32_t kCartSize = 0x40000;
constexpr uint32_t kPadding = 0x38000;
constexpr uint32_t kShortPadding = kCartSize - 16;
constexpr uint32_t kNoPadding = kCartSize;

// A cartridge with two songs. Song 0 has two tracks: track 0 calls a pattern, and track 1 repeats a note. Song 1 loops
// on a note. The cartridge's bytes from `padding` on are 0xFF.
Rom TwoSongs(uint32_t padding = kPadding)
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24);
    const size_t call = first.Here();
    first.Patt(call + 7).Wait(24).Fine();
    first.Note(12, 62).Wait(12).Raw({kCmdPend});
    Track second;
    second.Cmd(kCmdVoice, 0);
    const size_t note = second.Here();
    second.Note(12, 64, 90).Wait(12).Rept(2, note).Fine();
    cart.Song({first, second}, voices);

    Track looping;
    looping.Cmd(kCmdVoice, 0);
    const size_t loop = looping.Here();
    looping.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({looping}, voices);

    for (uint32_t a = padding; a < kCartSize; a++)
    {
        cart.Put8(kRomBase + a, 0xFF);
    }

    return cart.ToRom();
}

// A cartridge of `size` bytes with TwoSongs' songs. Voice 1 of their voice group plays a sample of 20 KiB of 0xFF
// bytes, though no song selects it. The bytes of each of `runs` are 0xFF, and the cartridge's other bytes after its
// data are 0.
Rom FillerCart(uint32_t size, const std::vector<Span>& runs)
{
    Cart cart;
    cart.Resize(size);
    const uint32_t blank = cart.Wave(std::vector<int8_t>(0x5000, -1));
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(0, blank)}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24);
    const size_t call = first.Here();
    first.Patt(call + 7).Wait(24).Fine();
    first.Note(12, 62).Wait(12).Raw({kCmdPend});
    Track second;
    second.Cmd(kCmdVoice, 0);
    const size_t note = second.Here();
    second.Note(12, 64, 90).Wait(12).Rept(2, note).Fine();
    cart.Song({first, second}, voices);

    Track looping;
    looping.Cmd(kCmdVoice, 0);
    const size_t loop = looping.Here();
    looping.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({looping}, voices);

    for (const Span& run : runs)
    {
        for (uint32_t a = run.start; a < run.start + run.size; a++)
        {
            cart.Put8(kRomBase + a, 0xFF);
        }
    }

    return cart.ToRom();
}

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, {}, info, error));

    return info;
}

std::vector<uint8_t> Bytes(const Rom& rom)
{
    return std::vector<uint8_t>(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
}

Rom RomOf(std::vector<uint8_t> bytes)
{
    Rom rom;
    rom.Assign(std::move(bytes));

    return rom;
}

std::vector<uint8_t> JoinOf(const Layout& layout)
{
    return Join(layout.sequences, layout.pieces, layout.fields);
}

// The piece of the sequence piece that holds sequence `sequence`.
uint32_t PieceOf(const Layout& layout, uint32_t sequence)
{
    for (uint32_t p = 0; p < layout.pieces.size(); p++)
    {
        const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&layout.pieces[p]);
        if (piece && piece->sequence == sequence)
        {
            return p;
        }
    }

    throw std::logic_error("no piece holds the sequence");
}

// Puts `item` before item `at` of sequence `sequence`, and moves the targets and the fields' items after it.
void Insert(Layout& layout, uint32_t sequence, uint32_t at, const Command& item)
{
    std::vector<Command>& items = layout.sequences[sequence].items;
    items.insert(items.begin() + at, item);
    for (Command& command : items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target >= at)
        {
            command.target++;
        }
    }

    const uint32_t piece = PieceOf(layout, sequence);
    for (ntdlib::sar::Field& field : layout.fields)
    {
        if (field.to.item && field.to.piece == piece && field.to.offset >= at)
        {
            field.to.offset++;
        }
    }
}

// Takes item `at` out of sequence `sequence`, and moves the targets and the fields' items after it.
void Remove(Layout& layout, uint32_t sequence, uint32_t at)
{
    std::vector<Command>& items = layout.sequences[sequence].items;
    items.erase(items.begin() + at);
    for (Command& command : items)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside && command.target > at)
        {
            command.target--;
        }
    }

    const uint32_t piece = PieceOf(layout, sequence);
    for (ntdlib::sar::Field& field : layout.fields)
    {
        if (field.to.item && field.to.piece == piece && field.to.offset > at)
        {
            field.to.offset--;
        }
    }
}

// The first item of `kind` in `sequence`.
uint32_t FirstOf(const ntdlib::commands::Sequence& sequence, Kind kind)
{
    for (uint32_t i = 0; i < sequence.items.size(); i++)
    {
        if (sequence.items[i].kind == kind)
        {
            return i;
        }
    }

    throw std::logic_error("no such item");
}

// `sequence`'s items without the data of zeros at its end.
std::vector<Command> WithoutZeros(const ntdlib::commands::Sequence& sequence)
{
    std::vector<Command> items = sequence.items;
    while (!items.empty() && items.back().kind == Kind::kData &&
           std::all_of(items.back().bytes.begin(), items.back().bytes.end(), [](uint8_t b) { return b == 0; }))
    {
        items.pop_back();
    }

    return items;
}

// Whether the song that `header` heads decodes to `expected`'s items, with zeros after them.
bool DecodesTo(const Rom& rom, const SongHeader& header, const ntdlib::commands::Sequence& expected)
{
    return WithoutZeros(DecodeSong(rom, header).sequence) == WithoutZeros(expected);
}

void TestRoundTrip()
{
    for (const uint32_t padding : {kNoPadding, kPadding})
    {
        const Rom rom = TwoSongs(padding);
        const DriverInfo info = Detect(rom);
        const Layout layout = Cut(rom, info);

        // A run, song 0, the run from its header to song 1, song 1, and the run from its header to the end.
        SEQLIB_REQUIRE_EQ(layout.sequences.size(), 2);
        SEQLIB_REQUIRE_EQ(layout.pieces.size(), 5);
        SEQLIB_CHECK(std::holds_alternative<std::vector<uint8_t>>(layout.pieces[0]));
        SEQLIB_CHECK(std::holds_alternative<ntdlib::sar::SequencePiece>(layout.pieces[1]));
        SEQLIB_CHECK(std::holds_alternative<ntdlib::sar::SequencePiece>(layout.pieces[3]));

        // The fields of the headers' three tracks, and of the song table's two entries.
        SongHeader first;
        SEQLIB_REQUIRE(ReadSong(rom, info, 0, first));
        const auto& piece = std::get<ntdlib::sar::SequencePiece>(layout.pieces[1]);
        SEQLIB_CHECK_EQ(piece.room, first.address - first.tracks[0]);
        SEQLIB_REQUIRE_EQ(layout.fields.size(), 5);
        const ntdlib::sar::Field track0 = {{2, 8}, {0, 0}, {1, 0, true}};
        SEQLIB_CHECK(layout.fields[0] == track0);
        SEQLIB_CHECK_EQ(layout.fields[1].to.piece, 1);
        SEQLIB_CHECK(layout.fields[1].to.offset > 0);
        const ntdlib::sar::Field entry0 = {{0, kSongTable - kRomBase}, {0, 0}, {2, 0}};
        SEQLIB_CHECK(layout.fields[3] == entry0);

        SEQLIB_CHECK(JoinOf(layout) == Bytes(rom));
    }
}

void TestInPlace()
{
    const Rom rom = TwoSongs();
    const DriverInfo info = Detect(rom);
    Layout layout = Cut(rom, info);
    SongHeader first;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, first));

    // Without track 0's first wait, song 0 takes a byte less. Its pattern call, its pattern and track 1 move a byte
    // earlier, and a zero follows the song up to its header.
    Remove(layout, 0, FirstOf(layout.sequences[0], Kind::kWait));
    const Rom joined = RomOf(JoinOf(layout));
    SEQLIB_CHECK_EQ(joined.Size(), rom.Size());
    SongHeader header;
    SEQLIB_REQUIRE(ReadSong(joined, info, 0, header));
    SEQLIB_CHECK_EQ(header.address, first.address);
    SEQLIB_REQUIRE_EQ(header.tracks.size(), 2);
    SEQLIB_CHECK_EQ(header.tracks[0], first.tracks[0]);
    SEQLIB_CHECK_EQ(header.tracks[1], first.tracks[1] - 1);
    SEQLIB_CHECK_EQ(joined.U8(first.address - 1), 0);
    SEQLIB_CHECK(DecodesTo(joined, header, layout.sequences[0]));

    // Song 1 stays as it was.
    SongHeader second;
    SEQLIB_REQUIRE(ReadSong(rom, info, 1, second));
    const uint32_t end = second.address + 12;
    SEQLIB_CHECK(std::equal(rom.Ptr(second.tracks[0]), rom.Ptr(second.tracks[0]) + (end - second.tracks[0]),
                            joined.Ptr(second.tracks[0])));
}

void TestMove()
{
    for (const uint32_t padding : {kPadding, kShortPadding, kNoPadding})
    {
        const Rom rom = TwoSongs(padding);
        const DriverInfo info = Detect(rom);
        Layout layout = Cut(rom, info);
        SongHeader first;
        SEQLIB_REQUIRE(ReadSong(rom, info, 0, first));

        // A second voice command after track 0's first command makes song 0 two bytes longer than its place.
        Insert(layout, 0, 1, layout.sequences[0].items[1]);
        const Rom joined = RomOf(JoinOf(layout));

        // The song goes to the first multiple of 256 bytes that lies 256 bytes into the run of 0xFF bytes, or else to
        // the ROM's end, and its header's copy follows it. The song table names the copy, and both headers give the
        // tracks' new addresses. After a short run, 0xFF bytes fill the ROM's growth up to the song.
        const uint32_t song = kRomBase + (padding == kPadding        ? kPadding + kFreeSpaceMargin
                                          : padding == kShortPadding ? kCartSize + kFreeSpaceMargin
                                                                     : kCartSize);
        SongHeader header;
        SEQLIB_REQUIRE(ReadSong(joined, info, 0, header));
        SEQLIB_REQUIRE_EQ(header.tracks.size(), 2);
        SEQLIB_CHECK_EQ(header.tracks[0], song);
        SEQLIB_CHECK_EQ(header.tracks[1], song + (first.tracks[1] - first.tracks[0]) + 2);
        SEQLIB_CHECK_EQ(header.address % 4, 0);
        SEQLIB_CHECK(header.address >= song + (first.address - first.tracks[0]) + 2);
        SEQLIB_CHECK(header.address < song + (first.address - first.tracks[0]) + 6);
        SEQLIB_CHECK_EQ(header.voices, first.voices);
        SEQLIB_CHECK(DecodesTo(joined, header, layout.sequences[0]));
        SEQLIB_CHECK_EQ(joined.Size(), padding == kPadding ? rom.Size() : header.address + 16 - kRomBase);
        SEQLIB_CHECK(
            std::all_of(joined.Ptr(kRomBase + padding), joined.Ptr(song), [](uint8_t b) { return b == 0xFF; }));

        SongHeader old;
        SEQLIB_REQUIRE(ntdlib::mp2k::ReadSongHeader(joined, first.address, old));
        SEQLIB_CHECK(old.tracks == header.tracks);

        // Zeros fill the song's old place.
        SEQLIB_CHECK(std::all_of(joined.Ptr(first.tracks[0]), joined.Ptr(first.address), [](uint8_t b) { return !b; }));

        // The cut of the new ROM takes the song from its new place.
        const Layout again = Cut(joined, info);
        SEQLIB_CHECK_EQ(again.sequences.size(), 2);
        SEQLIB_CHECK(JoinOf(again) == Bytes(joined));
    }
}

void TestHeaderSharedBySongs()
{
    // Songs 0 and 2 name the same header, and both entries of the song table take a field. The move of the song
    // changes both.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine();
    const uint32_t header = cart.Song({track}, voices);
    cart.Song({track}, voices);
    cart.Put32(kSongTable + 16, header);
    cart.Put32(kSongTable + 20, 0);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Layout layout = Cut(rom, info);
    SEQLIB_REQUIRE_EQ(layout.sequences.size(), 2);
    SEQLIB_CHECK_EQ(layout.fields.size(), 5);
    SEQLIB_CHECK(JoinOf(layout) == Bytes(rom));

    Insert(layout, 0, 1, layout.sequences[0].items[0]);
    const Rom joined = RomOf(JoinOf(layout));
    SEQLIB_CHECK(joined.U32(kSongTable) != header);
    SEQLIB_CHECK_EQ(joined.U32(kSongTable + 16), joined.U32(kSongTable));
    SEQLIB_CHECK_EQ(joined.U32(kSongTable + 8), rom.U32(kSongTable + 8));
}

void TestOverlap()
{
    // Song 1's header names the note of song 0's track, 2 bytes into it. The songs' bytes overlap, and only song 0
    // becomes a piece. Song 1's header stays in the bytes, and so does its entry in the song table.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine();
    const uint32_t header = cart.Song({track}, voices);
    const uint32_t note = cart.Get16(header + 8) + 2 + (uint32_t(cart.Get16(header + 10)) << 16);
    std::vector<uint8_t> other = {1, 0, 0, 0};
    for (const uint32_t word : {voices, note})
    {
        for (int b = 0; b < 4; b++)
        {
            other.push_back(uint8_t(word >> (8 * b)));
        }
    }
    cart.Put32(kSongTable + 8, cart.Place(other));
    cart.Put32(kSongTable + 12, 0);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    const Layout layout = Cut(rom, info);
    SEQLIB_CHECK_EQ(layout.sequences.size(), 1);
    SEQLIB_CHECK_EQ(layout.fields.size(), 2);
    SEQLIB_CHECK(JoinOf(layout) == Bytes(rom));
}

void TestTableAfterSongs()
{
    // A copy of the song table after the songs serves as the table. Its entries' fields lie in the last run, and a
    // song that moves changes its entry there.
    std::vector<uint8_t> bytes = Bytes(TwoSongs());
    constexpr uint32_t kTable = kPadding - 0x100;
    std::copy_n(bytes.begin() + (kSongTable - kRomBase), 16, bytes.begin() + kTable);
    const Rom rom = RomOf(bytes);
    DriverInfo info;
    std::string error;
    SEQLIB_REQUIRE(DetectDriver(rom, {kRomBase + kTable, 2}, info, error));
    Layout layout = Cut(rom, info);
    SEQLIB_REQUIRE_EQ(layout.fields.size(), 5);
    const auto last = uint32_t(layout.pieces.size() - 1);
    const uint32_t start = kCartSize - uint32_t(std::get<std::vector<uint8_t>>(layout.pieces[last]).size());
    const ntdlib::sar::Field entry1 = {{last, kTable + 8 - start}, {0, 0}, {last, 0}};
    SEQLIB_CHECK(layout.fields[4] == entry1);
    SEQLIB_CHECK(JoinOf(layout) == bytes);

    Insert(layout, 0, 1, layout.sequences[0].items[1]);
    const Rom joined = RomOf(JoinOf(layout));
    SEQLIB_CHECK(joined.U32(kRomBase + kTable) != rom.U32(kRomBase + kTable));
    SEQLIB_CHECK_EQ(joined.U32(kSongTable), rom.U32(kSongTable));
}

void TestOpaque()
{
    // Track 1 starts on the argument of track 0's voice command. The decoder keeps the song as data, and the ROM stays
    // one run.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine();
    const uint32_t at = cart.Place(track.Bytes(0));
    std::vector<uint8_t> header = {2, 0, 0, 0};
    for (const uint32_t word : {voices, at, at + 1})
    {
        for (int b = 0; b < 4; b++)
        {
            header.push_back(uint8_t(word >> (8 * b)));
        }
    }
    cart.Put32(kSongTable, cart.Place(header));
    const Rom rom = cart.ToRom();
    const Layout layout = Cut(rom, Detect(rom));
    SEQLIB_CHECK(layout.sequences.empty());
    SEQLIB_CHECK_EQ(layout.pieces.size(), 1);
    SEQLIB_CHECK(layout.fields.empty());
    SEQLIB_CHECK(JoinOf(layout) == Bytes(rom));
}

void TestTableInSong()
{
    // The song table lies between a song's track and its header. The song stays in the bytes.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Fine();
    const uint32_t at = cart.Place(track.Bytes(0));
    const uint32_t table = cart.Place(std::vector<uint8_t>(8, 0));
    std::vector<uint8_t> header = {1, 0, 0, 0};
    for (const uint32_t word : {voices, at})
    {
        for (int b = 0; b < 4; b++)
        {
            header.push_back(uint8_t(word >> (8 * b)));
        }
    }
    cart.Put32(table, cart.Place(header));
    const Rom rom = cart.ToRom();
    DriverInfo info;
    std::string error;
    SEQLIB_REQUIRE(DetectDriver(rom, {table, 1}, info, error));
    const Layout layout = Cut(rom, info);
    SEQLIB_CHECK(layout.sequences.empty());
    SEQLIB_CHECK(layout.fields.empty());
    SEQLIB_CHECK(JoinOf(layout) == Bytes(rom));
}

void TestFillerRuns()
{
    // A run of 16 KiB is a filler run without 256 bytes at each end, and a run a byte shorter isn't. Nor is the
    // sample's run, whose voice no song selects, nor the run at the ROM's end.
    const Span least = {0x30000, kLeastFillerRun};
    const Span shorter = {0x30000, kLeastFillerRun - 1};
    const Span longer = {0x38000, 0x8000};
    const Span end = {0x80000 - 0x8000, 0x8000};
    const std::vector<Span> taken = {{0x30100, kLeastFillerRun - 2 * kFreeSpaceMargin}, {0x38100, 0x7E00}};
    for (const bool at_end : {false, true})
    {
        std::vector<Span> runs = {least, longer};
        if (at_end)
        {
            runs.push_back(end);
        }
        const Rom rom = FillerCart(0x80000, runs);
        SEQLIB_CHECK(FillerRuns(rom, Detect(rom)) == taken);

        runs[0] = shorter;
        const Rom short_run = FillerCart(0x80000, runs);
        SEQLIB_CHECK(FillerRuns(short_run, Detect(short_run)) == std::vector<Span>{taken[1]});
    }

    // A run that a pattern call reaches. The driver reads its 0xFF bytes as notes.
    const Rom rom = FillerCart(0x80000, {longer});
    const DriverInfo info = Detect(rom);
    SongHeader first;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, first));
    std::vector<uint8_t> bytes = Bytes(rom);
    std::vector<uint8_t> track = {kCmdPatt, 0, 0, 0, 0};
    const uint32_t pattern = kRomBase + 0x3C000;
    for (int i = 0; i < 4; i++)
    {
        track[1 + i] = uint8_t(pattern >> (8 * i));
    }
    std::copy(track.begin(), track.end(), bytes.begin() + (first.tracks[0] - kRomBase));
    const Rom reached = RomOf(bytes);
    SEQLIB_CHECK(FillerRuns(reached, info).empty());
}

void TestFiller()
{
    // A ROM of 32 MiB without free space at its end. Song 0 grows by two bytes, and then song 1 by two.
    const Span run = {0x38000, 0x8000};
    const Rom rom = FillerCart(kLargestRom, {run});
    const DriverInfo info = Detect(rom);
    Layout layout = Cut(rom, info);
    SongHeader first;
    SongHeader second;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, first));
    SEQLIB_REQUIRE(ReadSong(rom, info, 1, second));
    Insert(layout, 0, 1, layout.sequences[0].items[1]);

    bool refused = false;
    try
    {
        JoinOf(layout);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused);

    // With filler, the song goes 256 bytes into the run, and its header's copy follows it. The ROM keeps its size, and
    // its other bytes stay.
    const Rom joined = RomOf(Join(layout.sequences, layout.pieces, layout.fields, JoinOptions{.filler = true}));
    SEQLIB_REQUIRE_EQ(joined.Size(), kLargestRom);
    const uint32_t song = kRomBase + run.start + kFreeSpaceMargin;
    SongHeader header;
    SEQLIB_REQUIRE(ReadSong(joined, info, 0, header));
    SEQLIB_REQUIRE_EQ(header.tracks.size(), 2);
    SEQLIB_CHECK_EQ(header.tracks[0], song);
    SEQLIB_CHECK_EQ(header.address % 4, 0);
    SEQLIB_CHECK(DecodesTo(joined, header, layout.sequences[0]));
    SongHeader old;
    SEQLIB_REQUIRE(ntdlib::mp2k::ReadSongHeader(joined, first.address, old));
    SEQLIB_CHECK(old.tracks == header.tracks);
    SEQLIB_CHECK(std::all_of(joined.Ptr(header.address + 16), joined.Ptr(kRomBase + run.start + run.size),
                             [](uint8_t b) { return b == 0xFF; }));
    SEQLIB_CHECK(
        std::equal(joined.Ptr(first.address + 16), joined.Ptr(kRomBase + run.start), rom.Ptr(first.address + 16)));
    SEQLIB_CHECK(std::equal(joined.Ptr(kRomBase + run.start + run.size), joined.Ptr(kRomBase) + kLargestRom,
                            rom.Ptr(kRomBase + run.start + run.size)));

    // The cut of the new ROM takes the song from the run, and joins it again.
    const Layout again = Cut(joined, info);
    SEQLIB_CHECK_EQ(again.sequences.size(), 2);
    SEQLIB_CHECK(JoinOf(again) == Bytes(joined));

    // Song 1 follows song 0's header in the run.
    Insert(layout, 1, 1, layout.sequences[1].items[1]);
    const Rom both = RomOf(Join(layout.sequences, layout.pieces, layout.fields, JoinOptions{.filler = true}));
    SongHeader moved;
    SEQLIB_REQUIRE(ReadSong(both, info, 1, moved));
    SEQLIB_CHECK_EQ(moved.tracks[0], header.address + 16);
    SEQLIB_CHECK(DecodesTo(both, moved, layout.sequences[1]));

    // Without a run long enough, the join refuses the ROM.
    const Rom short_run = FillerCart(kLargestRom, {{0x38000, kLeastFillerRun - 1}});
    Layout cramped = Cut(short_run, Detect(short_run));
    Insert(cramped, 0, 1, cramped.sequences[0].items[1]);
    refused = false;
    try
    {
        Join(cramped.sequences, cramped.pieces, cramped.fields, JoinOptions{.filler = true});
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused);

    // Where the free space at the ROM's end holds the song, the option changes nothing.
    const Rom small = TwoSongs();
    Layout grown = Cut(small, Detect(small));
    Insert(grown, 0, 1, grown.sequences[0].items[1]);
    SEQLIB_CHECK(Join(grown.sequences, grown.pieces, grown.fields, JoinOptions{.filler = true}) == JoinOf(grown));
}

// Whether `joined` holds `rom`'s bytes but for the byte at `address`.
bool DiffersOnlyAt(const Rom& rom, const Rom& joined, uint32_t address)
{
    if (joined.Size() != rom.Size())
    {
        return false;
    }

    for (uint32_t a = kRomBase; a < kRomBase + rom.Size(); a++)
    {
        if (a != address && joined.U8(a) != rom.U8(a))
        {
            return false;
        }
    }

    return joined.U8(address) != rom.U8(address);
}

void TestHeaderTracks()
{
    const Rom rom = TwoSongs();
    const DriverInfo info = Detect(rom);
    const Layout layout = Cut(rom, info);
    SongHeader first;
    SongHeader second;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, first));
    SEQLIB_REQUIRE(ReadSong(rom, info, 1, second));

    // Each header's fields give its tracks.
    SEQLIB_CHECK(HeaderTracks(layout.pieces, layout.fields) == std::vector<uint32_t>({0, 2, 0, 1, 0}));

    // Song 1 gains a second track: a copy of its first track's commands, after them. The song goes into the free space,
    // and the copy of its header holds both tracks. The header in its place keeps one track, at the song's new
    // address.
    Layout added = layout;
    const uint32_t piece = PieceOf(added, 1);
    std::vector<Command>& items = added.sequences[1].items;
    uint32_t end = 0;
    while (ntdlib::mp2k::FallsThrough(items[end]))
    {
        end++;
    }
    const uint32_t copied = end + 1;
    std::vector<Command> copy(items.begin(), items.begin() + copied);
    for (Command& command : copy)
    {
        if (ntdlib::commands::HasTarget(command.kind) && !command.target_outside)
        {
            command.target += copied;
        }
    }
    items.insert(items.begin() + copied, copy.begin(), copy.end());
    added.fields.push_back({{piece + 1, 12}, {0, 0}, {piece, copied, true}});
    SEQLIB_CHECK_EQ(HeaderTracks(added.pieces, added.fields)[piece], 2);

    const Rom grown = RomOf(JoinOf(added));
    SEQLIB_CHECK_EQ(grown.Size(), rom.Size());
    SongHeader header;
    SEQLIB_REQUIRE(ReadSong(grown, info, 1, header));
    SEQLIB_REQUIRE_EQ(header.tracks.size(), 2);
    SEQLIB_CHECK_EQ(header.tracks[0], kRomBase + kPadding + kFreeSpaceMargin);
    SEQLIB_CHECK_EQ(header.tracks[1] - header.tracks[0],
                    ntdlib::mp2k::EncodeSequence(layout.sequences[1], header.tracks[0]).offsets[copied]);
    SEQLIB_CHECK_EQ(header.voices, second.voices);
    SEQLIB_CHECK(DecodesTo(grown, header, added.sequences[1]));
    SongHeader old;
    SEQLIB_REQUIRE(ReadSongHeader(grown, second.address, old));
    SEQLIB_CHECK(old.tracks == std::vector<uint32_t>({header.tracks[0]}));
    SEQLIB_CHECK(std::equal(rom.Ptr(first.tracks[0]), rom.Ptr(first.address + 16), grown.Ptr(first.tracks[0])));

    // The cut of the new ROM takes the song with both tracks from its new place.
    const Layout again = Cut(grown, info);
    SEQLIB_CHECK_EQ(again.sequences.size(), 2);
    SEQLIB_CHECK_EQ(HeaderTracks(again.pieces, again.fields)[PieceOf(again, 1)], 2);
    SEQLIB_CHECK(JoinOf(again) == Bytes(grown));

    // Song 0 loses track 1. Its header in its place takes one track, and the song keeps its bytes.
    Layout removed = layout;
    removed.fields.erase(removed.fields.begin() + 1);
    const Rom shrunk = RomOf(JoinOf(removed));
    SEQLIB_CHECK(DiffersOnlyAt(rom, shrunk, first.address));
    SEQLIB_REQUIRE(ReadSong(shrunk, info, 0, header));
    SEQLIB_CHECK(header.tracks == std::vector<uint32_t>({first.tracks[0]}));
    SEQLIB_CHECK(JoinOf(Cut(shrunk, info)) == Bytes(shrunk));

    // Song 0 loses track 0. Track 1's field takes the first place of the header.
    Layout front = layout;
    front.fields.erase(front.fields.begin());
    front.fields[0].at.offset = 8;
    const Rom without = RomOf(JoinOf(front));
    SEQLIB_REQUIRE(ReadSong(without, info, 0, header));
    SEQLIB_CHECK(header.tracks == std::vector<uint32_t>({first.tracks[1]}));
    SEQLIB_CHECK(JoinOf(Cut(without, info)) == Bytes(without));

    // A song that outgrows its place and loses a track takes the smaller header in the free space too.
    Layout both = removed;
    Insert(both, 0, 1, both.sequences[0].items[1]);
    const Rom moved = RomOf(JoinOf(both));
    SEQLIB_REQUIRE(ReadSong(moved, info, 0, header));
    SEQLIB_CHECK_EQ(header.tracks.size(), 1);
    SEQLIB_CHECK(header.address != first.address);
    SEQLIB_REQUIRE(ReadSongHeader(moved, first.address, old));
    SEQLIB_CHECK(old.tracks == header.tracks);

    // A header holds 16 tracks at most.
    Layout many = layout;
    for (uint32_t track = 1; track < 17; track++)
    {
        many.fields.push_back({{piece + 1, 8 + 4 * track}, {0, 0}, {piece, 0, true}});
    }
    bool refused = false;
    try
    {
        JoinOf(many);
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }
    SEQLIB_CHECK(refused);
    many.fields.pop_back();
    SEQLIB_REQUIRE(ReadSong(RomOf(JoinOf(many)), info, 1, header));
    SEQLIB_CHECK_EQ(header.tracks.size(), 16);
}

void TestRefusals()
{
    const Rom rom = TwoSongs();
    const Layout layout = Cut(rom, Detect(rom));
    const auto refuses = [](const Layout& changed)
    {
        try
        {
            JoinOf(changed);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }

        return false;
    };

    Layout padding = layout;
    padding.pieces.insert(padding.pieces.begin() + 1, ntdlib::sar::Padding{4});
    SEQLIB_CHECK(refuses(padding));

    Layout sequence = layout;
    std::get<ntdlib::sar::SequencePiece>(sequence.pieces[1]).sequence = 2;
    SEQLIB_CHECK(refuses(sequence));

    Layout field = layout;
    field.fields[0].at = {1, 0};
    SEQLIB_CHECK(refuses(field));
    field.fields[0].at = {2, uint32_t(std::get<std::vector<uint8_t>>(layout.pieces[2]).size() - 3)};
    SEQLIB_CHECK(refuses(field));

    Layout item = layout;
    item.fields[0].to.offset = uint32_t(layout.sequences[0].items.size() + 1);
    SEQLIB_CHECK(refuses(item));

    Layout backwards = layout;
    backwards.fields[0].from = {4, 0};
    SEQLIB_CHECK(refuses(backwards));
}

} // namespace

int main()
{
    TestRoundTrip();
    TestInPlace();
    TestMove();
    TestHeaderSharedBySongs();
    TestOverlap();
    TestTableAfterSongs();
    TestOpaque();
    TestTableInSong();
    TestFillerRuns();
    TestFiller();
    TestHeaderTracks();
    TestRefusals();

    return seqlib::testing::Summary("mp2k layout");
}
