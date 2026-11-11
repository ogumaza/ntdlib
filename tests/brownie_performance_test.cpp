// SPDX-License-Identifier: MIT

// Tests of the performance of a sound of Brownie Brown's driver (ntdlib/sequence_export/brownie/performance.h), on
// small carts built here. Each expected value follows from the driver's format: a note, a rest, a hold or a release
// waits for the channel's length. A channel's first command runs in the frame after the sound's request.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/brownie/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/brownie/convert.h"
#include "ntdlib/sequence_export/brownie/performance.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/testing/brownie_cart.h"

namespace
{

using namespace ntdlib::testing::brownie_cart;

namespace sequence_export = ntdlib::sequence_export;
namespace driver_export = sequence_export::brownie;
using sequence_export::Entry;
using sequence_export::EventKind;
using sequence_export::Mix;
using sequence_export::MixChange;
using sequence_export::Stretch;
using sequence_export::TrackEvent;

using Settings = sequence_export::agb::ConvertSettings;

// The address of a sound's list and of its first channel's data: sound 0's list takes a word.
constexpr uint32_t kList = kSongLists + 4;

// Returns the offset of byte `at` of the first channel's data from the performance's base.
uint32_t DataOffset(const driver_export::Performance& perf, uint32_t at)
{
    return kSequences + at - perf.base;
}

// Returns the notes of `events`, in order.
std::vector<TrackEvent> NotesOf(const std::vector<TrackEvent>& events)
{
    std::vector<TrackEvent> notes;
    for (const TrackEvent& e : events)
    {
        if (e.kind == EventKind::kNoteOn)
        {
            notes.push_back(e);
        }
    }

    return notes;
}

// Returns the changes of `setting` in `changes`.
std::vector<MixChange> ChangesOf(const std::vector<MixChange>& changes, Mix setting)
{
    std::vector<MixChange> kept;
    for (const MixChange& c : changes)
    {
        if (c.setting == setting)
        {
            kept.push_back(c);
        }
    }

    return kept;
}

// Each note leads to its command, a call and its return begin stretches, and the ticks count frames.
void TestNotesAndStretches()
{
    const std::vector<uint8_t> data = {
        0xF0, kPsgHold, // 0: the envelope
        0xF1, 0x80,     // 2: NRx1
        0x89,           // 4: a length of 24 frames
        24,             // 5: a note in frame 1
        0xF6, 0x00,     // 6: a call in frame 25
        0x8A,           // 8: a length of 32 frames, in frame 49
        26,             // 9: a note in frame 49
        0xED,           // 10: a rest in frame 81
        0xFF,           // 11: the end in frame 113
        28,             // 12: the call's note in frame 25
        0xF7,           // 13: the return in frame 49
    };
    Cart cart;
    cart.Sound(1, {{0, data, {12}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const driver_export::Performance perf = driver_export::PerformSong(rom, info, 1, Settings());

    SEQLIB_CHECK_EQ(perf.base, kList);
    const std::vector<TrackEvent> notes = NotesOf(perf.tracks[0]);
    SEQLIB_REQUIRE(notes.size() == 3);
    SEQLIB_CHECK_EQ(notes[0].tick, 1);
    SEQLIB_CHECK_EQ(notes[1].tick, 25);
    SEQLIB_CHECK_EQ(notes[2].tick, 49);
    SEQLIB_REQUIRE(perf.note_commands.size() == 3);
    SEQLIB_CHECK_EQ(perf.note_commands[0].offset, DataOffset(perf, 5));
    SEQLIB_CHECK_EQ(perf.note_commands[1].offset, DataOffset(perf, 12));
    SEQLIB_CHECK_EQ(perf.note_commands[2].offset, DataOffset(perf, 9));
    SEQLIB_REQUIRE(perf.note_causes.size() == 3);
    SEQLIB_CHECK(perf.note_causes[0] == driver_export::NoteCause::kNote);
    SEQLIB_CHECK(perf.note_causes[2] == driver_export::NoteCause::kNote);

    // The stretches: from the start to the call, the call's note and return, and on from the return to the end.
    const std::vector<Stretch>& stretches = perf.stretches[0];
    SEQLIB_REQUIRE(stretches.size() == 3);
    SEQLIB_CHECK(stretches[0].entry == Entry::kOpen);
    SEQLIB_CHECK_EQ(stretches[0].tick, 1);
    SEQLIB_CHECK_EQ(stretches[0].first, DataOffset(perf, 0));
    SEQLIB_CHECK_EQ(stretches[0].last, DataOffset(perf, 6));
    SEQLIB_CHECK_EQ(stretches[0].depth, 0);
    SEQLIB_CHECK(stretches[1].entry == Entry::kCall);
    SEQLIB_CHECK_EQ(stretches[1].tick, 25);
    SEQLIB_CHECK_EQ(stretches[1].first, DataOffset(perf, 12));
    SEQLIB_CHECK_EQ(stretches[1].last, DataOffset(perf, 13));
    SEQLIB_CHECK_EQ(stretches[1].depth, 1);
    SEQLIB_CHECK(stretches[2].entry == Entry::kReturn);
    SEQLIB_CHECK_EQ(stretches[2].tick, 49);
    SEQLIB_CHECK_EQ(stretches[2].first, DataOffset(perf, 8));
    SEQLIB_CHECK_EQ(stretches[2].last, DataOffset(perf, 11));
    SEQLIB_CHECK_EQ(stretches[2].depth, 0);
    SEQLIB_CHECK_EQ(perf.note_commands[0].stretch, 0);
    SEQLIB_CHECK_EQ(perf.note_commands[1].stretch, 1);
    SEQLIB_CHECK_EQ(perf.note_commands[2].stretch, 2);

    // The track's name and the sound's end.
    SEQLIB_CHECK(perf.track_names[0] == "Square 1");
    SEQLIB_CHECK(perf.track_names[8] == "Samples 1");
    SEQLIB_CHECK_EQ(perf.end_tick, 113);
    SEQLIB_CHECK(!perf.loop);
}

// A repeat's end that goes back begins a stretch, and the repeat's last pass runs on.
void TestRepeat()
{
    const std::vector<uint8_t> data = {
        0xF0, kPsgHold, // 0
        0xF4, 2,        // 2: two passes
        0x89,           // 4
        24,             // 5: frames 1 and 25
        0xF5,           // 6: frames 25 and 49
        0xFF,           // 7: frame 49
    };
    Cart cart;
    cart.Sound(1, {{0, data, {}}});
    const Rom rom = cart.ToRom();
    const driver_export::Performance perf = driver_export::PerformSong(rom, Detect(rom), 1, Settings());

    const std::vector<Stretch>& stretches = perf.stretches[0];
    SEQLIB_REQUIRE(stretches.size() == 2);
    SEQLIB_CHECK_EQ(stretches[0].first, DataOffset(perf, 0));
    SEQLIB_CHECK_EQ(stretches[0].last, DataOffset(perf, 6));
    SEQLIB_CHECK(stretches[1].entry == Entry::kLoop);
    SEQLIB_CHECK_EQ(stretches[1].tick, 25);
    SEQLIB_CHECK_EQ(stretches[1].first, DataOffset(perf, 4));
    SEQLIB_CHECK_EQ(stretches[1].last, DataOffset(perf, 7));
    SEQLIB_CHECK_EQ(stretches[1].depth, 0);
}

// F8 back to a place that the channel read is the sound's loop. The loop runs from the frame that first read the place
// up to the jump's frame, and the performance plays it as the conversion does.
void TestLoop()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0xF1, 0x80, 0x89, 24, 26, 0xF8, 0}, {0}}});
    const Rom rom = cart.ToRom();
    const Settings settings;
    const driver_export::Performance perf = driver_export::PerformSong(rom, Detect(rom), 1, settings);

    SEQLIB_REQUIRE(perf.loop.has_value());
    SEQLIB_CHECK_EQ(perf.loop->start, 1);
    SEQLIB_CHECK_EQ(perf.loop->end, 49);
    SEQLIB_CHECK_EQ(perf.loops, settings.loops);
    SEQLIB_CHECK_EQ(perf.end_tick, 97);

    const std::vector<Stretch>& stretches = perf.stretches[0];
    SEQLIB_REQUIRE(stretches.size() >= 2);
    SEQLIB_CHECK(stretches[1].entry == Entry::kJump);
    SEQLIB_CHECK_EQ(stretches[1].tick, 49);
    SEQLIB_CHECK_EQ(stretches[1].first, DataOffset(perf, 0));
    SEQLIB_CHECK_EQ(NotesOf(perf.tracks[0]).size(), 4);
}

// F2's byte, EA's nibbles and FD's bits give a channel's changes of its mix, at the ticks that run them. A later change
// in a tick replaces an earlier one.
void TestMix()
{
    const std::vector<uint8_t> square = {
        0xF0, kPsgHold, 0xF2, 0x40, 0xF2, 0x41, 0xFD, 0x11, 0x89, 24, // frame 1: a volume twice, the centre
        0xFD, 0x10,     0xF2, 0x42, 26,                               // frame 25: the left
        0xFD, 0x01,     28,                                           // frame 49: the right
        0xFF,
    };
    const std::vector<uint8_t> samples = {
        0xE1, 0,    0xF0, kSampleHold, 0xEA, 0xFF, 0x89, 38, // frame 1: the centre
        0xEA, 0xF0, 40,                                      // frame 25: the left
        0xEA, 0x0F, 42,                                      // frame 49: the right
        0xFF,
    };
    Cart cart;
    cart.Sound(1, {{0, square, {}}, {8, samples, {}}});
    const Rom rom = cart.ToRom();
    const driver_export::Performance perf = driver_export::PerformSong(rom, Detect(rom), 1, Settings());

    const std::vector<MixChange> volumes = ChangesOf(perf.mix[0], Mix::kVolume);
    SEQLIB_REQUIRE(volumes.size() == 2);
    SEQLIB_CHECK_EQ(volumes[0].tick, 1);
    SEQLIB_CHECK_EQ(volumes[0].value, 0x41);
    SEQLIB_CHECK_EQ(volumes[1].tick, 25);
    SEQLIB_CHECK_EQ(volumes[1].value, 0x42);

    for (const size_t track : {size_t(0), size_t(8)})
    {
        const std::vector<MixChange> pans = ChangesOf(perf.mix[track], Mix::kPan);
        SEQLIB_REQUIRE(pans.size() == 3);
        SEQLIB_CHECK_EQ(pans[0].tick, 1);
        SEQLIB_CHECK_EQ(pans[0].value, 0);
        SEQLIB_CHECK_EQ(pans[1].tick, 25);
        SEQLIB_CHECK_EQ(pans[1].value, -64);
        SEQLIB_CHECK_EQ(pans[2].tick, 49);
        SEQLIB_CHECK_EQ(pans[2].value, 63);
    }
}

// The Magical Vacation revision's sample channels play at their FIFO's volume, on both sides, and hold no pan. E2 is
// its volume there.
void TestVacationMix()
{
    Cart cart(true, Revision::kMagicalVacation);
    cart.Sound(1, {{8, {0xE1, 0, 0xF0, kSampleHold, 0xE2, 0x7F, 0xEA, 0xF0, 0x89, 48, 0xEF}, {}}});
    const Rom rom = cart.ToRom();
    const driver_export::Performance perf = driver_export::PerformSong(rom, Detect(rom), 1, Settings());

    const std::vector<MixChange> volumes = ChangesOf(perf.mix[8], Mix::kVolume);
    SEQLIB_REQUIRE(volumes.size() == 1);
    SEQLIB_CHECK_EQ(volumes[0].value, 0x7F);
    SEQLIB_CHECK(ChangesOf(perf.mix[8], Mix::kPan).empty());
    SEQLIB_CHECK(perf.track_names[12].empty());
    SEQLIB_CHECK_EQ(NotesOf(perf.tracks[8]).size(), 1);
}

// A PSG note whose envelope falls to nothing and rises again sounds twice from one command. The second note counts as
// an earlier note's.
void TestEarlierNote()
{
    Cart cart;
    const uint32_t envelope = kEnvelopes + 0x20;
    const uint32_t entry = 6;
    cart.Put32(kEnvelopeTable + 4 * entry, envelope);
    cart.PutBytes(envelope, {2, 0xF0, 2, 0x00, 0, 0xF0});
    cart.Sound(1, {{0, {0xF0, uint8_t(entry), 0xF1, 0x80, 0x89, 24, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const driver_export::Performance perf = driver_export::PerformSong(rom, Detect(rom), 1, Settings());

    const std::vector<TrackEvent> notes = NotesOf(perf.tracks[0]);
    SEQLIB_REQUIRE(notes.size() == 2);
    SEQLIB_CHECK_EQ(notes[0].tick, 1);
    SEQLIB_CHECK_EQ(notes[1].tick, 5);
    SEQLIB_CHECK(perf.note_causes[0] == driver_export::NoteCause::kNote);
    SEQLIB_CHECK(perf.note_causes[1] == driver_export::NoteCause::kEarlier);
    SEQLIB_CHECK_EQ(perf.note_commands[0].offset, perf.note_commands[1].offset);
    SEQLIB_CHECK_EQ(perf.note_commands[1].offset, DataOffset(perf, 5));
}

// Each note of the performance is a note of the MIDI file that the conversion writes, with the same ticks in the file.
void TestMidiFile()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF0, kPsgHold, 0xF1, 0x80, 0x89, 24, 26, 0xFF}, {}}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Settings settings;
    const driver_export::Performance perf = driver_export::PerformSong(rom, info, 1, settings);
    const driver_export::SongFiles files = driver_export::ConvertSongFiles(rom, info, 1, settings);
    SEQLIB_REQUIRE(files.report.ok);
    SEQLIB_CHECK(!files.sf2.empty());

    const seqlib::midi::File file = seqlib::midi::Read(files.midi);
    std::vector<uint64_t> starts;
    for (const seqlib::midi::Event& e : file.tracks.at(1).events)
    {
        if (!e.bytes.empty() && (e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0)
        {
            SEQLIB_CHECK_EQ(e.bytes[2], 127);
            starts.push_back(e.tick);
        }
    }

    SEQLIB_REQUIRE(starts.size() == perf.midi_places.size());
    for (size_t n = 0; n < starts.size(); n++)
    {
        SEQLIB_CHECK_EQ(starts[n], perf.midi_places[n].on);
        SEQLIB_CHECK(perf.midi_places[n].on < perf.midi_places[n].off);
    }
    SEQLIB_CHECK_EQ(file.tracks.at(1).end_tick, perf.midi_end_tick);
}

// A sound without notes keeps its stretches, and a list that can't be read gives a warning and no tracks.
void TestEmpty()
{
    Cart cart;
    cart.Sound(1, {{0, {0xF2, 0x40, 0x89, 0xFE, 0xFF}, {}}});
    const uint32_t bad_list = kRomBase + 0x4000;
    cart.Put32(kSongTable + 4 * 2, bad_list);
    cart.Put32(bad_list, 99);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const driver_export::Performance silent = driver_export::PerformSong(rom, info, 1, Settings());
    SEQLIB_CHECK(silent.note_commands.empty());
    SEQLIB_CHECK_EQ(silent.stretches[0].size(), 1);
    SEQLIB_CHECK_EQ(ChangesOf(silent.mix[0], Mix::kVolume).size(), 1);

    const driver_export::Performance unread = driver_export::PerformSong(rom, info, 2, Settings());
    SEQLIB_CHECK(!unread.warnings.empty());
    SEQLIB_CHECK(unread.note_commands.empty());
    SEQLIB_CHECK_EQ(unread.base, 0);
}

} // namespace

int main()
{
    TestNotesAndStretches();
    TestRepeat();
    TestLoop();
    TestMix();
    TestVacationMix();
    TestEarlierNote();
    TestMidiFile();
    TestEmpty();
    return xsflib::testing::Summary("brownie_performance");
}
