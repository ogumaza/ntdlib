// SPDX-License-Identifier: MIT

// Tests of the DSE performance's records for an editor (ntdlib/sequence_export/dse/performance.h): each track's
// stretches of events, each note's event and track, and each track's changes of its channel's volume and pan. The
// files come from dse_song_files.h. The expected values are worked out by hand from the driver's events: a pause waits
// for its length, and a repeat's end goes back to the event after the repeat's start until its count runs out. A
// channel starts at volume 100 and the centre.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/sequence_export/dse/performance.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace dse = ntdlib::dse;
using namespace ntdlib::sequence_export::dse;
using dse_song_files::BankFile;
using dse_song_files::Bytes;
using dse_song_files::Join;
using dse_song_files::kBankId;
using dse_song_files::MainBankFile;
using dse_song_files::Note;
using dse_song_files::SongFile;
using ntdlib::sequence_export::Entry;
using ntdlib::sequence_export::Mix;
using ntdlib::sequence_export::MixChange;
using ntdlib::sequence_export::NoteCommand;
using ntdlib::sequence_export::Stretch;
using xsflib::testing::Check;

// Where the first track's chunk starts in SongFile: after the header (0x40) and the song chunk (0x10 and the 0x30 bytes
// of its settings). A track's events start after its chunk's header (0x10) and its preamble (4).
constexpr uint32_t kFirstChunk = 0x80;
constexpr uint32_t kEvents = 0x14;

Stretch StretchOf(uint32_t tick, uint32_t first, uint32_t last, Entry entry, uint8_t depth)
{
    Stretch stretch;
    stretch.tick = tick;
    stretch.first = first;
    stretch.last = last;
    stretch.entry = entry;
    stretch.depth = depth;

    return stretch;
}

// Track 0 sets the tempo and ends. Track 1 plays on channel 1:
//
//    0: A9 7F AA 00  bank 0x7F00
//    4: AC 00        program 0
//    6: E0 50        volume 80
//    8: E8 20        pan 0x20: 32 to the left of the centre
//   10: 9C 02        repeat twice
//   12: 60 60 18     note 48 at velocity 0x60 for 24 ticks
//   15: 92 18        pause 24 ticks
//   17: 9D           repeat's end
//   18: 98           end
//
// The first pass runs events 0 to 15 at tick 0. At tick 24, the repeat's end goes back to the note. At tick 48, it
// leaves the repeat, and the end follows it.
void TestRepeatStretches()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x50, 0xE8, 0x20, 0x9C, 0x02},
                              Note(0x60, 0, 0, 0x18, 1),
                              {0x92, 0x18, 0x9D, 0x98}});
    const dse::Song song = dse::Song::Parse(SongFile({conductor, track}));
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    const Performance performance = Perform({.song = &song, .banks = {&bank}, .main_bank = &main_bank});

    // The conductor's chunk holds 7 bytes, padded to 8. Track 1's chunk follows it.
    const uint32_t conductor_events = kFirstChunk + kEvents;
    const uint32_t events = kFirstChunk + 0x10 + 8 + kEvents;
    Check(performance.stretches.size() == 2, "a list of stretches for each track");
    Check(performance.stretches[0] ==
              std::vector<Stretch>{StretchOf(0, conductor_events, conductor_events + 2, Entry::kOpen, 0)},
          "the conductor's stretch");
    const std::vector<Stretch> expected = {
        StretchOf(0, events, events + 17, Entry::kOpen, 0),
        StretchOf(24, events + 12, events + 18, Entry::kLoop, 1),
    };
    Check(performance.stretches[1] == expected, "the repeat's stretches");

    Check(performance.note_commands == std::vector<NoteCommand>{{events + 12, 0}, {events + 12, 1}},
          "each pass's note");
    Check(performance.note_tracks == std::vector<uint8_t>{1, 1}, "the notes' track");
    uint32_t notes = 0;
    for (const auto& event : performance.tracks[1])
    {
        if (event.kind == seqlib::midi::EventKind::kNoteOn)
        {
            Check(event.tick == 24 * notes && event.note == notes + 1, "each note at its tick");
            notes++;
        }
    }
    Check(notes == 2, "two notes on channel 1");

    Check(performance.mix.size() == 2, "a list of changes for each track");
    Check(performance.mix[0] == std::vector<MixChange>{{0, Mix::kVolume, 100}}, "channel 0's start");
    Check(performance.mix[1] == std::vector<MixChange>{{0, Mix::kVolume, 80}, {0, Mix::kPan, -32}},
          "track 1's volume and pan");
}

// Two tracks on one channel take the channel's changes. Track 1 sets the volume to 80 at tick 0, and track 2 sets it
// to 64 at tick 24 and pans it to 0x60 at tick 48.
void TestSharedChannel()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes first = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x50},
                              Note(0x60, 0, 0, 0x18, 1),
                              {0x92, 0x48, 0x98}});
    const Bytes second = {2, 1, 0, 0, 0x92, 0x18, 0xE0, 0x40, 0x92, 0x18, 0xE8, 0x60, 0x92, 0x18, 0x98};
    const dse::Song song = dse::Song::Parse(SongFile({conductor, first, second}));
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    const Performance performance = Perform({.song = &song, .banks = {&bank}, .main_bank = &main_bank});

    const std::vector<MixChange> expected = {{0, Mix::kVolume, 80}, {24, Mix::kVolume, 64}, {48, Mix::kPan, 32}};
    Check(performance.mix.size() == 3 && performance.mix[1] == expected && performance.mix[2] == expected,
          "both tracks take the channel's changes");
    Check(performance.note_tracks == std::vector<uint8_t>{1}, "track 1's note");
    Check(performance.stretches[2].size() == 1 &&
              performance.stretches[2][0].last == performance.stretches[2][0].first + 10,
          "track 2's events in one stretch");
}

// Program 2's splits leave out keys from 64 up at velocities below 0x20. Track 1's note of key 72 at velocity 0x10
// therefore starts no voice: it lists among the notes that the driver never started, with its tick, its length, its
// key, its velocity and its event. The next note plays.
void TestUnstartedNote()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x02, 0x92, 0x18, 0xA0, 0x06},
                              Note(0x10, 0, 0, 0x18, 1),
                              {0x92, 0x18},
                              Note(0x7F, 0, 0, 0x18, 1),
                              {0x92, 0x18, 0x98}});
    const dse::Song song = dse::Song::Parse(SongFile({conductor, track}));
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    const Performance performance = Perform({.song = &song, .banks = {&bank}, .main_bank = &main_bank});

    // The second track's chunk follows the conductor's, whose 7 bytes take 8 with their padding. The note follows the
    // bank, the program, the pause and the octave.
    const uint32_t note = kFirstChunk + 0x10 + 8 + kEvents + 10;
    const std::vector<UnstartedNote> expected = {
        {.tick = 24, .length = 24, .track = 1, .key = 72, .velocity = 0x10, .offset = note}};
    Check(performance.unstarted == expected, "the note that starts no voice");
    Check(performance.note_tracks == std::vector<uint8_t>{1}, "the next note plays");
}

// Key group 1 of the test's bank allows two voices of a channel. With one, a note of the group takes the voice of the
// group's last note on the channel. Key 64 of program 1 starts at 0 for 48 ticks, and key 65 takes its voice at 12. Key
// 65 ends by its length.
void TestCutNote()
{
    const Bytes conductor = {0, 0, 0, 0, 0xA4, 0x78, 0x98};
    const Bytes track = Join({{1, 1, 0, 0},
                              {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x01},
                              Note(0x60, 4, 1, 0x30, 1),
                              {0x92, 0x0C},
                              Note(0x60, 5, 0, 0x0C, 1),
                              {0x92, 0x60, 0x98}});
    const dse::Song song = dse::Song::Parse(SongFile({conductor, track}));
    dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    const auto perform = [&]
    {
        return Perform({.song = &song, .banks = {&bank}, .main_bank = &main_bank});
    };

    const Performance two = perform();
    const std::vector<NoteVoices> none(2);
    Check(two.note_channels == none, "a key group of two voices cuts no note");

    bank.instruments->key_groups.at(1)[2] = 1;
    const Performance one = perform();
    const std::vector<NoteVoices> cut = {{.cut_tick = 12}, {}};
    Check(one.note_channels == cut, "the later note cuts the note that its channel holds");
}

} // namespace

int main()
{
    TestRepeatStretches();
    TestSharedChannel();
    TestUnstartedNote();
    TestCutNote();

    return xsflib::testing::Summary("DSE performance for an editor");
}
