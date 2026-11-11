// SPDX-License-Identifier: MIT

// Unit tests of the performance of Quintet's songs (ntdlib/sequence_export/quintet/performance.h): the notes with
// their commands, the stretches of commands and the changes of the mix. They build small synthetic cartridges in
// memory. No game ROM is needed.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/quintet/commands.h"
#include "ntdlib/quintet/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/quintet/convert.h"
#include "ntdlib/sequence_export/quintet/performance.h"
#include "ntdlib/testing/quintet_cart.h"

namespace ntdlib::sequence_export::quintet
{
namespace
{

using ntdlib::agb::Rom;
using ntdlib::quintet::DetectDriver;
using ntdlib::quintet::DriverInfo;
using ntdlib::quintet::DriverOverrides;
using ntdlib::quintet::Revision;
using ntdlib::quintet::SongAddress;
using ntdlib::sequence_export::agb::ConvertSettings;
using ntdlib::testing::quintet_cart::Cart;
using ntdlib::testing::quintet_cart::Channel;
using ntdlib::testing::quintet_cart::Song;

// Returns the driver info of a cartridge, failing the test if detection fails.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));
    SEQLIB_CHECK(error.empty());

    return info;
}

// The performance holds the notes of the MIDI file, each with its command. A stretch begins at the channel's start,
// where D7 goes back to its repeat, one repeat deep, where FF goes back to the loop point, and after each wait. The mix
// holds the changes of the volume. The volume starts at 0. Here BF is at offset 2. FF at 12 therefore goes back to 3.
// The repeat starts at 5 and plays the note at 6 twice. D7 at 7 runs at the tick of the note after it, in a stretch of
// its own that ends where D7 goes back. FF at 12 runs where the loop's first note starts its frame. The volume goes up
// to 15 at 3, and down to 8 at 9 for the note at 11. Square 2 reads only its FF, at 13.
void TestPerformance()
{
    Cart cart(Revision::kA);
    cart.Songs({Song(
        {Channel().Bytes({0x7F, 90, 0xBF, 0x5E, 15, 0xD6}).Note(0, 3).Bytes({0xD7, 2, 0x5E, 8}).Note(4, 3).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;
    opt.loops = 2;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);

    std::vector<uint32_t> commands;
    std::vector<uint32_t> ons;
    std::vector<std::tuple<int, uint64_t, int, uint64_t>> notes;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            commands.push_back(p.note_commands[e.note - 1].offset);
            ons.push_back(e.tick);
        }
        else if (e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(0, ons[e.note - 1], e.key, e.tick);
        }
    }
    std::vector<std::tuple<int, uint64_t, int, uint64_t>> midi;
    for (const seqlib::midi::Note& n : seqlib::midi::Notes(seqlib::midi::Read(files.midi)))
    {
        midi.emplace_back(n.channel, n.on, n.key, n.off);
    }
    std::sort(notes.begin(), notes.end());
    std::sort(midi.begin(), midi.end());

    const std::vector<Stretch>& stretches = p.stretches[0];
    const std::vector<MixChange>& mix = p.mix[0];
    SEQLIB_REQUIRE(ons.size() >= 6 && stretches.size() >= 9 && mix.size() >= 3);
    SEQLIB_CHECK(p.base == SongAddress(info, 0) + 14 && p.timebase == 3552 && p.loop && p.loops == 2);
    SEQLIB_CHECK(notes == midi);
    SEQLIB_CHECK(std::vector<uint32_t>(commands.begin(), commands.begin() + 6) ==
                 std::vector<uint32_t>({6, 6, 11, 6, 6, 11}));
    SEQLIB_CHECK(stretches[0] == (Stretch{0, 0, 6, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(stretches[1] == (Stretch{ons[1], 7, 7, Entry::kOpen, 1, true}));
    SEQLIB_CHECK(stretches[2] == (Stretch{ons[1], 6, 6, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(stretches[3] == (Stretch{ons[2], 7, 11, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(stretches[4] == (Stretch{ons[3], 12, 12, Entry::kLoop, 0, true}));
    SEQLIB_CHECK(stretches[5] == (Stretch{ons[3], 3, 6, Entry::kLoop, 0, true}));
    SEQLIB_CHECK(stretches[6] == (Stretch{ons[4], 7, 7, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(stretches[7] == (Stretch{ons[4], 6, 6, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(stretches[8] == (Stretch{ons[5], 7, 11, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(p.note_commands[0].stretch == 0 && p.note_commands[1].stretch == 2 && p.note_commands[2].stretch == 3);
    SEQLIB_CHECK(p.note_commands[3].stretch == 5 && p.note_commands[4].stretch == 7 && p.note_commands[5].stretch == 8);
    SEQLIB_CHECK(mix[0] == (MixChange{0, Mix::kVolume, 15}));
    SEQLIB_CHECK(mix[1] == (MixChange{ons[2], Mix::kVolume, 8}));
    SEQLIB_CHECK(mix[2] == (MixChange{ons[3], Mix::kVolume, 15}));
    SEQLIB_CHECK(p.tracks[1].empty() && p.track_names[4] == "PCM A");
    SEQLIB_CHECK(p.stretches[1].size() == 1 && p.stretches[1][0].first == 13 && p.stretches[1][0].last == 13);
}

// Returns the tick of the command at `offset` that the waits of `stretch` give, at 37 of the MIDI file's ticks for each
// of the driver's.
uint64_t WalkTick(const ntdlib::quintet::Decoded& decoded, const std::vector<uint32_t>& offsets, const Stretch& stretch,
                  uint32_t offset)
{
    uint64_t tick = stretch.tick;
    for (size_t i = 0; i + 1 < offsets.size() && offsets[i] < offset; i++)
    {
        const ntdlib::commands::Command& item = decoded.sequence.items[i];
        const bool waits = item.kind == ntdlib::commands::Kind::kNote ||
                           item.kind == ntdlib::commands::Kind::kNoteOff || item.kind == ntdlib::commands::Kind::kWait;
        if (offsets[i] >= stretch.first && waits)
        {
            tick += uint64_t(item.value.value / ntdlib::commands::kOne) * 37;
        }
    }

    return tick;
}

// On a channel at another tempo than the first channel's, the stretches keep the contract of a stretch: its commands
// run at the ticks that its waits give, at 37 of the MIDI file's ticks for each of the driver's, from its tick on.
// Square 1 plays at tempo 120, and so does the MIDI file. Square 2 plays at tempo 90. Each of its ticks lasts
// 37 × 120 / 90 of the file's ticks. On both channels, each note and rest ends a stretch. The repeat from 13 to 19
// plays twice. The stretches that split the first pass keep its entry. Their depth is the channel's: the stretch from
// 15 after D6 has the depth 1. Square 1's FF at 8 runs a quarter note after its last note, before square 2 ends.
void TestPerformanceAtTwoTempos()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel().Bytes({0x7F, 120, 0x5E, 15}).Note(0, 3).Note(2, 3).Note(4, 3).Note(5, 3).End(),
                      Channel()
                          .Bytes({0x7F, 90, 0x5E, 15, 0xD6})
                          .Note(0, 3)
                          .Bytes({0x5E, 12})
                          .Note(4, 3)
                          .Rest(3)
                          .Bytes({0xD7, 2})
                          .Note(7, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const ntdlib::quintet::Decoded decoded = ntdlib::quintet::DecodeRomSequence(rom, info, 0);
    const std::vector<uint32_t> offsets =
        ntdlib::quintet::ItemOffsets(decoded.sequence, ntdlib::quintet::TraitsOf(rom, info));

    const auto walk = [&](const Stretch& stretch, uint32_t offset)
    {
        return WalkTick(decoded, offsets, stretch, offset);
    };

    std::vector<std::tuple<int, uint64_t, int, uint64_t>> notes;
    std::array<std::vector<uint32_t>, 2> ons;
    std::map<uint32_t, uint64_t> starts;
    int walked = 0;
    for (size_t t = 0; t < 2; t++)
    {
        for (const TrackEvent& e : p.tracks[t])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                const NoteCommand& command = p.note_commands[e.note - 1];
                SEQLIB_REQUIRE(command.stretch < p.stretches[t].size());
                SEQLIB_CHECK_EQ(walk(p.stretches[t][command.stretch], command.offset), uint64_t(e.tick));
                walked++;
                ons[t].push_back(e.tick);
                starts[e.note] = e.tick;
            }
            else if (e.kind == EventKind::kNoteOff)
            {
                notes.emplace_back(int(t), starts[e.note], e.key, e.tick);
            }
        }
    }
    std::vector<std::tuple<int, uint64_t, int, uint64_t>> midi;
    for (const seqlib::midi::Note& n : seqlib::midi::Notes(seqlib::midi::Read(files.midi)))
    {
        midi.emplace_back(n.channel, n.on, n.key, n.off);
    }
    std::sort(notes.begin(), notes.end());
    std::sort(midi.begin(), midi.end());

    const std::vector<Stretch>& square2 = p.stretches[1];
    SEQLIB_REQUIRE(ons[0].size() == 4 && ons[1].size() == 5 && square2.size() >= 8);
    SEQLIB_CHECK(walked == 9 && notes == midi);
    const std::vector<Stretch>& square1 = p.stretches[0];
    SEQLIB_REQUIRE(square1.size() == 5);
    SEQLIB_CHECK(square1[0] == (Stretch{0, 0, 4, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(square1[1] == (Stretch{ons[0][1], 5, 5, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(square1[2] == (Stretch{ons[0][2], 6, 6, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(square1[3] == (Stretch{ons[0][3], 7, 7, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(square1[4] == (Stretch{ons[0][3] + 96 * 37, 8, 8, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(ons[1][1] - ons[1][0] > 4700 && ons[1][1] - ons[1][0] < 4820);
    SEQLIB_CHECK(square2[0] == (Stretch{0, 9, 14, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(square2[1] == (Stretch{ons[1][1], 15, 17, Entry::kOpen, 1, true}));
    SEQLIB_CHECK(square2[2] == (Stretch{square2[2].tick, 18, 18, Entry::kOpen, 1, true}));
    SEQLIB_CHECK(square2[2].tick > ons[1][1] && square2[2].tick < ons[1][2]);
    SEQLIB_CHECK(square2[3] == (Stretch{ons[1][2], 19, 19, Entry::kOpen, 1, true}));
    SEQLIB_CHECK(square2[4] == (Stretch{ons[1][2], 14, 14, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(square2[5] == (Stretch{ons[1][3], 15, 17, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(square2[6] == (Stretch{square2[6].tick, 18, 18, Entry::kLoop, 1, true}));
    SEQLIB_CHECK(square2[6].tick > ons[1][3] && square2[6].tick < ons[1][4]);
    SEQLIB_CHECK(square2[7] == (Stretch{ons[1][4], 19, 21, Entry::kLoop, 1, true}));
}

// A tempo starts the driver's count of ticks again: square 1's 7F on its channel and on every other. The first note
// after it starts its frame. The stretches keep the contract of a stretch. The 7F at 6 runs in the stretch of the note
// after it, at that note's tick, where the MIDI file's tempo changes.
void TestPerformanceAfterTempo()
{
    Cart cart(Revision::kA);
    cart.Songs({Song(
        {Channel().Bytes({0x7F, 120, 0x5E, 15}).Note(0, 3).Note(2, 4).Bytes({0x7F, 100}).Note(4, 3).Note(5, 3).End(),
         Channel().Bytes({0x5E, 15}).Note(0, 2).Note(2, 2).Note(4, 2).Note(5, 2).Note(7, 2).End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;

    const Performance p = PerformSong(rom, info, 0, opt);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const ntdlib::quintet::Decoded decoded = ntdlib::quintet::DecodeRomSequence(rom, info, 0);
    const std::vector<uint32_t> offsets =
        ntdlib::quintet::ItemOffsets(decoded.sequence, ntdlib::quintet::TraitsOf(rom, info));

    std::vector<std::tuple<int, uint64_t, int, uint64_t>> notes;
    std::array<std::vector<uint32_t>, 2> ons;
    std::map<uint32_t, uint64_t> starts;
    for (size_t t = 0; t < 2; t++)
    {
        for (const TrackEvent& e : p.tracks[t])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                const NoteCommand& command = p.note_commands[e.note - 1];
                SEQLIB_REQUIRE(command.stretch < p.stretches[t].size());
                SEQLIB_CHECK_EQ(WalkTick(decoded, offsets, p.stretches[t][command.stretch], command.offset),
                                uint64_t(e.tick));
                ons[t].push_back(e.tick);
                starts[e.note] = e.tick;
            }
            else if (e.kind == EventKind::kNoteOff)
            {
                notes.emplace_back(int(t), starts[e.note], e.key, e.tick);
            }
        }
    }
    std::vector<std::tuple<int, uint64_t, int, uint64_t>> midi;
    for (const seqlib::midi::Note& n : seqlib::midi::Notes(seqlib::midi::Read(files.midi)))
    {
        midi.emplace_back(n.channel, n.on, n.key, n.off);
    }
    std::sort(notes.begin(), notes.end());
    std::sort(midi.begin(), midi.end());

    SEQLIB_REQUIRE(ons[0].size() == 4 && ons[1].size() == 5);
    SEQLIB_CHECK(notes == midi);
    SEQLIB_REQUIRE(p.stretches[0].size() == 5 && p.stretches[1].size() == 5);
    SEQLIB_CHECK(p.stretches[0][2] == (Stretch{ons[0][2], 6, 8, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(p.tempo.size() == 2 && p.tempo[1].tick == ons[0][2]);
    const uint32_t kFirsts[] = {11, 14, 15, 16, 17};
    for (size_t i = 0; i < 5; i++)
    {
        SEQLIB_CHECK(p.stretches[1][i].tick == ons[1][i] && p.stretches[1][i].first == kFirsts[i]);
    }
}

// The driver reads the commands between the ends of two waits in one frame. They run at the tick of the note that ends
// them. Square 1 rests a quarter note at tempo 120: 29.6 frames of 120 ticks of the MIDI file each. The driver reads
// 5E, 7F and the note at 7 at the start of frame 29, tick 3480. The 7F starts the count again. The note therefore
// starts its frame. 5E sets the volume at that tick too. D4 ties a second quarter note to the note at 7. The stretch
// ends with the tied note at 9. The next note comes two quarter notes later at tempo 100: 7104 ticks after 3480.
void TestPerformanceRuns()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 120})
                          .Rest(3)
                          .Bytes({0x5E, 12, 0x7F, 100})
                          .Note(0, 3)
                          .Bytes({0xD4})
                          .Note(0, 3)
                          .Bytes({0x5E, 8})
                          .Note(2, 3)
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;

    const Performance p = PerformSong(rom, info, 0, opt);
    std::vector<uint32_t> ons;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            ons.push_back(e.tick);
        }
    }

    const std::vector<Stretch>& stretches = p.stretches[0];
    SEQLIB_REQUIRE(ons.size() == 2 && stretches.size() == 3);
    SEQLIB_CHECK(ons[0] == 3480 && ons[1] == 3480 + 7104);
    SEQLIB_CHECK(stretches[0] == (Stretch{0, 0, 2, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(stretches[1] == (Stretch{3480, 3, 9, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(stretches[2] == (Stretch{ons[1], 10, 12, Entry::kOpen, 0, true}));
    SEQLIB_CHECK(p.tempo.size() == 2 && p.tempo[1].tick == 3480);
    SEQLIB_CHECK(p.mix[0].size() == 2 && p.mix[0][0] == (MixChange{3480, Mix::kVolume, 12}));
    SEQLIB_CHECK(p.mix[0][1] == (MixChange{ons[1], Mix::kVolume, 8}));
}

// D1 makes the note before it dotted. The dot ends the stretch of its note. The note at 2 has a dot at 3. D4 ties the
// note at 6 to the note at 4. The dot at 7 follows the tied note. FF at 8 runs at the song's end. A stretch that
// begins at the end is left out.
void TestPerformanceDots()
{
    Cart cart(Revision::kA);
    cart.Songs({Song({Channel()
                          .Bytes({0x7F, 120})
                          .Note(0, 3)
                          .Bytes({0xD1})
                          .Note(2, 3)
                          .Bytes({0xD4})
                          .Note(2, 3)
                          .Bytes({0xD1})
                          .End()})});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt;

    const Performance p = PerformSong(rom, info, 0, opt);

    const std::vector<Stretch>& stretches = p.stretches[0];
    SEQLIB_REQUIRE(stretches.size() == 2);
    SEQLIB_CHECK(stretches[0].first == 0 && stretches[0].last == 3);
    SEQLIB_CHECK(stretches[1].first == 4 && stretches[1].last == 7);
}

} // namespace

int RunTests()
{
    TestPerformance();
    TestPerformanceAtTwoTempos();
    TestPerformanceAfterTempo();
    TestPerformanceRuns();
    TestPerformanceDots();

    return seqlib::testing::Summary("quintet_performance");
}

} // namespace ntdlib::sequence_export::quintet

int main()
{
    return ntdlib::sequence_export::quintet::RunTests();
}
