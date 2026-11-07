// SPDX-License-Identifier: MIT

// Tests of the performance of a sequence of Nintendo R&D2's GBA driver (ntdlib/sequence_export/rd2/performance.h), on
// small carts built here. Each sequence's bytes are worked out by hand from the driver's rules in
// ntdlib/rd2/commands.h: a wait of n takes n ticks, and at the driver's first tempo a tick is a frame. The ticks,
// offsets, stretches and mix follow from them. Every note then has to be a note of the MIDI file that the conversion
// writes.

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/rd2/driver.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/rd2/convert.h"
#include "ntdlib/sequence_export/rd2/performance.h"
#include "ntdlib/testing/agb_files.h"
#include "rd2_cart.h"

using xsflib::testing::Check;

namespace
{

using namespace ntdlib::testing::rd2_cart;

namespace export_ = ntdlib::sequence_export;
namespace perf_ = export_::rd2;
namespace rd2 = ntdlib::rd2;
using export_::Entry;
using export_::Mix;
using export_::MixChange;
using export_::Stretch;
using export_::TrackEvent;
using ntdlib::agb::kFrameRate;
using ntdlib::agb::Rom;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::TempSettings;
using perf_::Performance;
using rd2::DriverInfo;

DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    Check(rd2::DetectDriver(rom, rd2::DriverOverrides(), info, error), "the driver is found");

    return info;
}

// A note of a track, from its events.
struct Note
{
    bool operator==(const Note&) const = default;

    uint32_t on = 0;
    int key = 0;
    int velocity = 0;
    uint32_t preset = 0;
};

// Each note of a track, with the preset that it plays.
std::vector<Note> NotesOf(const std::vector<TrackEvent>& track)
{
    std::vector<Note> notes;
    uint32_t preset = 0;
    for (const TrackEvent& e : track)
    {
        if (e.kind == export_::EventKind::kPreset)
        {
            preset = e.preset;
        }
        else if (e.kind == export_::EventKind::kNoteOn)
        {
            notes.push_back({e.tick, e.key, e.value, preset});
        }
    }

    return notes;
}

// The offsets of a performance's note commands.
std::vector<uint32_t> OffsetsOf(const Performance& perf)
{
    std::vector<uint32_t> offsets;
    for (const export_::NoteCommand& command : perf.note_commands)
    {
        offsets.push_back(command.offset);
    }

    return offsets;
}

// Checks that the performance's notes are the MIDI file's, at the same ticks and with the same keys.
void CheckAgainstMidi(const Rom& rom, const DriverInfo& info, int song, const Performance& perf)
{
    export_::agb::ConvertSettings settings = TempSettings("rd2_performance");
    settings.program = "test";
    const export_::agb::SongReport report = perf_::ConvertSong(rom, info, song, settings, nullptr);
    Check(report.ok && !report.silent, "the conversion writes the song");
    const seqlib::midi::File midi = ReadMidi(report.midi_path);

    std::vector<std::pair<uint64_t, int>> in_file;
    for (size_t t = 1; t < midi.tracks.size(); t++)
    {
        for (const seqlib::midi::Event& e : midi.tracks[t].events)
        {
            if (e.bytes.size() == 3 && (e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0)
            {
                in_file.emplace_back(e.tick, e.bytes[1]);
            }
        }
    }

    std::vector<std::pair<uint64_t, int>> in_performance;
    for (const auto& track : perf.tracks)
    {
        for (const TrackEvent& e : track)
        {
            if (e.kind == export_::EventKind::kNoteOn)
            {
                in_performance.emplace_back(perf.midi_places[e.note - 1].on, e.key);
            }
        }
    }
    Check(in_performance == in_file, "the performance holds the MIDI file's notes");
}

void TestOneTrack()
{
    // The header is a count of tracks, a zero and an offset: 4 bytes.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC3, 0x20})    //  4: a pan, 32 to the left
                         .Bytes({0xE0, 0x70})    //  6: a volume
                         .Bytes({0xE3, 0x10})    //  8: an echo
                         .Bytes({0xC2, kLooped}) // 10: an instrument
                         .Note(60, 24, 100)      // 12: a note at tick 0, of 24 ticks
                         .Wait(24)               // 15: a wait
                         .Bytes({60})            // 17: a note that repeats the length and the velocity, at tick 24
                         .Bytes({0xC0})          // 18: a wait that repeats its length
                         .Bytes({0xE0, 0x60})    // 19: a volume at tick 48
                         .Bytes({0xC3, 0x10})    // 21: a pan in the same tick
                         .Bytes({0xC3, 0x50})    // 23: a pan that replaces it
                         .Bytes({0xC2, kPlain})  // 25: another instrument
                         .Bytes({62})            // 27: a note at tick 48
                         .Bytes({0xC0})          // 28: a wait
                         .End()}});              // 29: the end, at tick 72
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 0, export_::agb::ConvertSettings());

    Check(perf.base == kSequenceData, "offsets count from the sequence's start");
    Check(perf.timebase == 24, "24 of the driver's ticks make a quarter note");
    Check(perf.tempo.size() == 1 && perf.tempo[0].tick == 0, "one tempo");
    Check(std::abs(perf.tempo[0].bpm - 60.0 * kFrameRate / 24) < 1e-9, "a tick lasts a frame at the first tempo");
    Check(!perf.loop, "the sequence doesn't loop");
    Check(perf.end_tick == 72, "the sequence's end");
    Check(perf.midi_end_tick == 72 * 150, "the MIDI file's end: 150 of its ticks to a tick");
    Check(perf.track_names[0] == "Track 0" && perf.track_names[9] == "Track 9", "the tracks' names");

    const std::vector<Note> notes = {{0, 60, 100, kLooped}, {24, 60, 100, kLooped}, {48, 62, 100, kPlain}};
    Check(NotesOf(perf.tracks[0]) == notes, "the track's notes");
    Check(OffsetsOf(perf) == std::vector<uint32_t>({12, 17, 27}), "the notes' commands");
    Check(perf.midi_places[0].on == 0 && perf.midi_places[0].off == 24 * 150,
          "the first note's ticks in the MIDI file");
    Check(perf.midi_places[1].on == 24 * 150, "the second note's start in the MIDI file");

    const std::vector<Stretch> stretches = {{0, 4, 29, Entry::kOpen, 0, false}};
    Check(perf.stretches[0] == stretches, "the track reads straight through its bytes");
    const std::vector<MixChange> mix = {{0, Mix::kPan, 0x20 - 0x40},
                                        {0, Mix::kVolume, 0x70},
                                        {0, Mix::kSendA, 0x10},
                                        {48, Mix::kVolume, 0x60},
                                        {48, Mix::kPan, 0x50 - 0x40}};
    Check(perf.mix[0] == mix, "the track's mix: a later pan in a tick replaces an earlier one");

    CheckAgainstMidi(rom, info, 0, perf);
}

void TestCallsAndLoop()
{
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kLooped}) //  4
                         .Bytes({0xF4, 17, 0})   //  6: a call of offset 17
                         .Note(60, 12, 100)      //  9: the loop's start, at tick 12
                         .Wait(12)               // 12
                         .Bytes({0xF0, 9, 0})    // 14: a jump to offset 9, at tick 24
                         .Note(60, 12, 100)      // 17: the subroutine, at tick 0
                         .Wait(12)               // 20
                         .End()}});              // 22: the return, at tick 12
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 0, export_::agb::ConvertSettings());

    Check(perf.loop && perf.loop->start == 12 && perf.loop->end == 24, "the loop");
    Check(perf.loops == 2 && perf.end_tick == 36, "two passes of the loop");

    // The third pass starts at the end, and no stretch begins there.
    const std::vector<Stretch> stretches = {{0, 4, 6, Entry::kOpen, 0, false},
                                            {0, 17, 22, Entry::kCall, 1, false},
                                            {12, 9, 14, Entry::kReturn, 0, false},
                                            {24, 9, 14, Entry::kJump, 0, false}};
    Check(perf.stretches[0] == stretches, "the track's stretches");

    const std::vector<Note> notes = {{0, 60, 100, kLooped}, {12, 60, 100, kLooped}, {24, 60, 100, kLooped}};
    Check(NotesOf(perf.tracks[0]) == notes, "the notes up to the end");
    Check(OffsetsOf(perf) == std::vector<uint32_t>({17, 9, 9}), "the notes' commands");
    Check(
        perf.note_commands[0].stretch == 1 && perf.note_commands[1].stretch == 2 && perf.note_commands[2].stretch == 3,
        "each note's stretch");

    CheckAgainstMidi(rom, info, 0, perf);
}

void TestNoteWait()
{
    // After C8, a note waits for its length, and a jump's stretch starts with the wait on.
    Cart cart;
    cart.Sequences({{TrackData()
                         .Bytes({0xC2, kLooped}) // 4
                         .Bytes({0xC8})          // 6: the note wait
                         .Note(60, 6, 100)       // 7: a note at tick 0
                         .Bytes({0xF0, 13, 0})   // 10: a jump at tick 6
                         .Note(62, 6, 100)       // 13: a note at tick 6
                         .End()}});              // 16: the end at tick 12
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 0, export_::agb::ConvertSettings());

    const std::vector<Stretch> stretches = {{0, 4, 10, Entry::kOpen, 0, false}, {6, 13, 16, Entry::kJump, 0, true}};
    Check(perf.stretches[0] == stretches, "the stretch after C8 starts with the note wait");
    Check(perf.end_tick == 12, "the notes' lengths give the time");
    Check(OffsetsOf(perf) == std::vector<uint32_t>({7, 13}), "the notes' commands");

    CheckAgainstMidi(rom, info, 0, perf);
}

void TestStartedTrack()
{
    // At a tempo of 130, a wait of a tick runs out in the third frame: 150 less 130 is 20, and 20 less 130 is below 0.
    // F8 runs there, at 260 units. The new track reads its first command at the frame's time, the nearest tick to which
    // is tick 2.
    std::vector<uint8_t> track = TrackData()
                                     .Bytes({0xE4, 0x80, 130})                        //  4: the tempo, in two bytes
                                     .Bytes({0xC2, kLooped})                          //  7
                                     .Wait(1)                                         //  9
                                     .Bytes({0xF8, 3, 18, 0})                         // 11: starts track 3 at offset 18
                                     .Wait(40)                                        // 15
                                     .End();                                          // 17
    const std::vector<uint8_t> started = TrackData().Note(64, 12, 90).Wait(12).End(); // 18, 21 and 23
    track.insert(track.end(), started.begin(), started.end());
    Cart cart;
    cart.Sequences({{track}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 0, export_::agb::ConvertSettings());

    const std::vector<Stretch> stretches = {{2, 18, 23, Entry::kOpen, 0, false}};
    Check(perf.stretches[3] == stretches, "the started track's stretch, at the nearest tick");
    const std::vector<Note> notes = {{2, 64, 90, kLooped}};
    Check(NotesOf(perf.tracks[3]) == notes, "the started track's note");
    Check(perf.midi_places[0].on == 260, "the note's start in the MIDI file: the frame's time");
    Check(perf.note_commands[0].offset == 18, "the note's command");

    CheckAgainstMidi(rom, info, 0, perf);
}

void TestStartedTrackMix()
{
    // F8 gives the started track the volume, pan and echo of the track that runs it. The started track takes them at
    // its first command, at tick 2, and its E0 there replaces the volume.
    std::vector<uint8_t> track = TrackData()
                                     .Bytes({0xE4, 0x80, 130}) //  4: the tempo, in two bytes
                                     .Bytes({0xC2, kLooped})   //  7
                                     .Bytes({0xE0, 0x50})      //  9
                                     .Bytes({0xC3, 0x30})      // 11
                                     .Wait(1)                  // 13
                                     .Bytes({0xF8, 3, 22, 0})  // 15: starts track 3 at offset 22
                                     .Wait(40)                 // 19
                                     .End();                   // 21
    const std::vector<uint8_t> started = TrackData().Bytes({0xE0, 0x60}).Note(64, 12, 90).Wait(12).End(); // 22
    track.insert(track.end(), started.begin(), started.end());
    Cart cart;
    cart.Sequences({{track}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 0, export_::agb::ConvertSettings());

    const std::vector<MixChange> first = {{0, Mix::kVolume, 0x50}, {0, Mix::kPan, -16}};
    Check(perf.mix[0] == first, "the first track's mix");
    const std::vector<MixChange> copied = {{2, Mix::kVolume, 0x60}, {2, Mix::kPan, -16}};
    Check(perf.mix[3] == copied, "the started track's mix: the pan that F8 copies, and its volume");
    Check(perf.note_commands[0].offset == 24, "the note's command");

    CheckAgainstMidi(rom, info, 0, perf);
}

void TestEffect()
{
    // A sound effect's table has a halfword for each track: its first command lies at offset 2. A sound effect's notes
    // wait for their lengths from the start.
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    cart.Effects({{TrackData()
                       .Bytes({0xC2, kLooped}) //  2
                       .Bytes({0xE3, 0x20})    //  4
                       .Note(60, 12, 100)      //  6: a note at tick 0
                       .Bytes({60})            //  9: a note at tick 12
                       .End()}});              // 10: the end at tick 24
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 1, export_::agb::ConvertSettings());

    Check(perf.base == kEffectData, "offsets count from the effects' sequence");
    Check(perf.end_tick == 24, "the effect's end");
    const std::vector<Stretch> stretches = {{0, 2, 10, Entry::kOpen, 0, true}};
    Check(perf.stretches[0] == stretches, "the effect's stretch");
    Check(OffsetsOf(perf) == std::vector<uint32_t>({6, 9}), "the notes' commands");
    const std::vector<MixChange> mix = {{0, Mix::kSendA, 0x20}};
    Check(perf.mix[0] == mix, "the effect's mix");

    CheckAgainstMidi(rom, info, 1, perf);
}

void TestWithoutNotes()
{
    // A sequence that plays no note keeps its stretches and its mix.
    Cart cart;
    cart.Sequences({{TrackData().Bytes({0xE0, 0x40}).Wait(12).End()}});
    cart.Effects({{{}, TrackData().End()}});
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const Performance perf = perf_::PerformSong(rom, info, 0, export_::agb::ConvertSettings());

    Check(perf.note_commands.empty() && perf.tracks[0].empty(), "no notes");
    const std::vector<Stretch> stretches = {{0, 4, 8, Entry::kOpen, 0, false}};
    Check(perf.stretches[0] == stretches, "the stretch stays");
    const std::vector<MixChange> mix = {{0, Mix::kVolume, 0x40}};
    Check(perf.mix[0] == mix, "the mix stays");

    // An effect's entry that gives no track, and a song past the last, give a warning and no tracks.
    for (const int song : {1, 99})
    {
        const Performance none = perf_::PerformSong(rom, info, song, export_::agb::ConvertSettings());
        Check(!none.warnings.empty() && none.base == 0 && none.tempo.empty(), "a song without a track");
    }
}

} // namespace

int main()
{
    TestOneTrack();
    TestCallsAndLoop();
    TestNoteWait();
    TestStartedTrack();
    TestStartedTrackMix();
    TestEffect();
    TestWithoutNotes();

    return xsflib::testing::Summary("rd2_performance");
}
