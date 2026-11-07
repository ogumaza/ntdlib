// SPDX-License-Identifier: MIT

// Tests of the performance of a piece of Ubisoft Milan's GBA driver (ntdlib/sequence_export/ubimilan/performance.h), on
// small synthetic cartridges. The notes have to be those of the conversion's MIDI file, and the records have to lead to
// the decoder's items. The expected frames follow the driver's countdowns: a countdown of n runs the next command n + 1
// frames later, and the first countdown of a piece runs its first command n frames in.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "seqlib/midi/reader.h"
#include "xsflib/testing/check.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/ubimilan/convert.h"
#include "ntdlib/sequence_export/ubimilan/performance.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/ubimilan/commands.h"
#include "ntdlib/ubimilan/driver.h"
#include "ntdlib/ubimilan/sequencer.h"
#include "ubimilan_image.h"

namespace
{

namespace ubimilan = ntdlib::ubimilan;
namespace export_ = ntdlib::sequence_export;
using export_::Entry;
using export_::Stretch;
using export_::ubimilan::Performance;
using export_::ubimilan::PerformSong;
using ntdlib::agb::Rom;
using ntdlib::commands::Kind;
using ntdlib::testing::ubimilan_image::TestImage;
using seqlib::midi::EventKind;
using seqlib::midi::TrackEvent;
using ubimilan::DriverInfo;

struct Cart
{
    explicit Cart(const TestImage& image) : rom(image.ToRom())
    {
        std::string error;
        XSFLIB_CHECK(ubimilan::DetectDriver(rom, {}, info, error));
    }

    Rom rom;
    DriverInfo info;
};

export_::agb::ConvertSettings Settings()
{
    export_::agb::ConvertSettings settings;
    settings.out_dir = ntdlib::testing::agb_files::TempPath("").string();
    settings.base_name = "performance";
    return settings;
}

// A note of a performance's track.
struct TrackNote
{
    uint64_t on = 0;
    uint64_t off = 0;
    int key = 0;
    uint32_t id = 0;
};

std::vector<TrackNote> NotesOf(const std::vector<TrackEvent>& events)
{
    std::vector<TrackNote> notes;
    for (const TrackEvent& event : events)
    {
        if (event.kind == EventKind::kNoteOn)
        {
            notes.push_back({event.tick, 0, event.key, event.note});
        }
        else if (event.kind == EventKind::kNoteOff)
        {
            for (TrackNote& note : notes)
            {
                if (note.id == event.note)
                {
                    note.off = event.tick;
                }
            }
        }
    }

    return notes;
}

// The kit's key 40 and square 1's key 60, and their note-offs, with the commands' offsets in the comments.
TestImage OnePass(int flags)
{
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(40000, 64)));
    image.SetInstrument(0, 0, {{0, 0x80, 0, 0x80}});
    const int sequence = image.AddSequence({
        0x02, 0x00,                   // 4: the first command on frame 2
        0x90, 60,   0x70, 0x03, 0x00, // 6: on frame 2, the next on frame 6
        0x80, 60,   0,    0x00, 0x00, // 11: on frame 6, the next on frame 6
        0x99, 40,   64,   0x03, 0x00, // 16: on frame 6, the next on frame 10
        0x89, 40,   0,    0x01, 0x00, // 21: on frame 10, the end on frame 12
        0xFC,                         // 26
    });
    image.AddPiece(flags, {sequence});
    return image;
}

void TestRecords()
{
    const Cart cart(OnePass(0));
    const Performance perf = PerformSong(cart.rom, cart.info, 0, Settings());
    SEQLIB_REQUIRE(perf.tracks.size() == 16);
    XSFLIB_CHECK(perf.warnings.empty());

    // The end on frame 12 leaves the piece 13 frames long.
    XSFLIB_CHECK(perf.end_tick == 13);
    XSFLIB_CHECK(!perf.loop);

    // The piece's one sequence.
    SEQLIB_REQUIRE(perf.sequences.size() == 1);
    XSFLIB_CHECK(perf.sequences[0].base == ubimilan::ResourceAddress(cart.rom, cart.info, perf.sequences[0].resource));

    // One pass, from the bytes after the kind up to the end.
    SEQLIB_REQUIRE(perf.stretches.size() == 1 && perf.stretch_sequences.size() == 1);
    const Stretch& stretch = perf.stretches[0];
    XSFLIB_CHECK(stretch.tick == 0 && stretch.first == 4 && stretch.last == 26);
    XSFLIB_CHECK(stretch.entry == Entry::kOpen);
    XSFLIB_CHECK(perf.stretch_sequences[0] == 0);

    // Each channel's notes on the track of its number, in frames.
    const std::vector<TrackNote> square = NotesOf(perf.tracks[0]);
    SEQLIB_REQUIRE(square.size() == 1);
    XSFLIB_CHECK(square[0].on == 2 && square[0].off == 6 && square[0].key == 60);
    const std::vector<TrackNote> kit = NotesOf(perf.tracks[9]);
    SEQLIB_REQUIRE(kit.size() == 1);
    XSFLIB_CHECK(kit[0].on == 6 && kit[0].off == 10 && kit[0].key == 40);
    for (size_t t = 0; t < perf.tracks.size(); t++)
    {
        XSFLIB_CHECK(t == 0 || t == 9 || perf.tracks[t].empty());
    }

    // A channel's instruments lie in the bank of the channel's number.
    const auto first_preset = [](const std::vector<TrackEvent>& events)
    {
        for (const TrackEvent& event : events)
        {
            if (event.kind == EventKind::kPreset)
            {
                return event.preset;
            }
        }

        return uint32_t(0xFFFFFFFF);
    };
    XSFLIB_CHECK(first_preset(perf.tracks[0]) == 0);
    XSFLIB_CHECK(first_preset(perf.tracks[9]) == 9 * 128);

    // Each note leads to its command.
    SEQLIB_REQUIRE(perf.note_commands.size() == 2);
    XSFLIB_CHECK(perf.note_commands[square[0].id - 1].offset == 6);
    XSFLIB_CHECK(perf.note_commands[kit[0].id - 1].offset == 16);
    XSFLIB_CHECK(perf.note_commands[0].stretch == 0 && perf.note_commands[1].stretch == 0);

    // The tracks' names and the tempo: the driver's frames, 59.73 of them to a second.
    XSFLIB_CHECK(perf.track_names[0] == "Square 1" && perf.track_names[9] == "Kit");
    XSFLIB_CHECK(perf.track_names[5] == "Channel 5");
    SEQLIB_REQUIRE(perf.tempo.size() == 1);
    XSFLIB_CHECK(perf.timebase >= 1 && perf.tempo[0].tick == 0);
    XSFLIB_CHECK(std::abs(perf.tempo[0].bpm * perf.timebase / 60 - ntdlib::agb::kFrameRate) < 1e-9);
}

void TestDecoder()
{
    // The offsets of the notes' commands and of the stretch's ends are those of the decoder's items.
    const Cart cart(OnePass(0));
    const Performance perf = PerformSong(cart.rom, cart.info, 0, Settings());
    SEQLIB_REQUIRE(perf.sequences.size() == 1);
    const ubimilan::Decoded decoded = ubimilan::DecodeResource(cart.rom, cart.info, perf.sequences[0].resource);
    const ubimilan::Encoded encoded = ubimilan::EncodeSequence(decoded.sequence);
    std::map<uint32_t, size_t> items;
    for (size_t i = 0; i < decoded.sequence.items.size(); i++)
    {
        items[encoded.offsets[i]] = i;
    }

    SEQLIB_REQUIRE(perf.stretches.size() == 1);
    XSFLIB_CHECK(items.contains(perf.stretches[0].first) && items.contains(perf.stretches[0].last));
    XSFLIB_CHECK(decoded.sequence.items[items[perf.stretches[0].first]].kind == Kind::kWait);
    XSFLIB_CHECK(decoded.sequence.items[items[perf.stretches[0].last]].kind == Kind::kEnd);
    for (const export_::NoteCommand& command : perf.note_commands)
    {
        SEQLIB_REQUIRE(items.contains(command.offset));
        XSFLIB_CHECK(decoded.sequence.items[items[command.offset]].kind == Kind::kNote);
    }
}

void TestMidiFile()
{
    // The notes of the conversion's MIDI file, at the places that the performance gives them.
    const Cart cart(OnePass(0));
    const export_::agb::ConvertSettings settings = Settings();
    const Performance perf = PerformSong(cart.rom, cart.info, 0, settings);
    const export_::agb::SongReport report = export_::ubimilan::ConvertSong(cart.rom, cart.info, 0, settings, nullptr);
    XSFLIB_CHECK(report.ok && !report.silent);
    const seqlib::midi::File file = ntdlib::testing::agb_files::ReadMidi(report.midi_path);
    std::vector<uint64_t> ons;
    for (const seqlib::midi::Track& track : file.tracks)
    {
        for (const seqlib::midi::Event& event : track.events)
        {
            if (!event.bytes.empty() && (event.bytes[0] & 0xF0) == 0x90 && event.bytes[2] > 0)
            {
                ons.push_back(event.tick);
            }
        }
    }

    SEQLIB_REQUIRE(perf.midi_places.size() == 2 && ons.size() == 2);
    XSFLIB_CHECK(perf.midi_places[0].on == ons[0]);
    XSFLIB_CHECK(perf.midi_places[1].on == ons[1]);
    XSFLIB_CHECK(perf.midi_places[0].off > perf.midi_places[0].on);
    XSFLIB_CHECK(perf.midi_end_tick >= perf.midi_places[1].off);
}

void TestPasses()
{
    // A looping piece of two sequences. The first runs on frames 0 and 2, and its end on frame 4 takes the second in
    // the same frame, whose countdown of 0 runs its first command then. The second's end on frame 8 starts the list
    // again.
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(40000, 64)));
    image.SetKey(41, image.AddSample(std::vector<int8_t>(40000, 32)));
    const int first = image.AddSequence({0x00, 0x00, 0x99, 40, 64, 0x01, 0x00, 0x89, 40, 0, 0x01, 0x00, 0xFC});
    const int second = image.AddSequence({0x00, 0x00, 0x99, 41, 64, 0x01, 0x00, 0x89, 41, 0, 0x01, 0x00, 0xFC});
    image.AddPiece(1, {first, second});
    const Cart cart(image);
    const Performance perf = PerformSong(cart.rom, cart.info, 0, Settings());
    SEQLIB_REQUIRE(perf.sequences.size() == 2);
    XSFLIB_CHECK(perf.sequences[0].resource == first && perf.sequences[1].resource == second);
    XSFLIB_CHECK(bool(perf.loop));

    SEQLIB_REQUIRE(perf.stretches.size() >= 3);
    XSFLIB_CHECK(perf.stretches[0].tick == 0 && perf.stretches[0].entry == Entry::kOpen);
    XSFLIB_CHECK(perf.stretch_sequences[0] == 0);
    XSFLIB_CHECK(perf.stretches[1].tick == 4 && perf.stretches[1].entry == Entry::kJump);
    XSFLIB_CHECK(perf.stretch_sequences[1] == 1);
    XSFLIB_CHECK(perf.stretches[2].tick == 8 && perf.stretches[2].entry == Entry::kLoop);
    XSFLIB_CHECK(perf.stretch_sequences[2] == 0);

    // A note of the second sequence leads to its pass, at the same offset in the sequence.
    const std::vector<TrackNote> kit = NotesOf(perf.tracks[9]);
    SEQLIB_REQUIRE(kit.size() >= 2);
    XSFLIB_CHECK(kit[0].on == 0 && kit[0].key == 40);
    XSFLIB_CHECK(kit[1].on == 4 && kit[1].key == 41);
    XSFLIB_CHECK(perf.note_commands[kit[0].id - 1].stretch == 0);
    XSFLIB_CHECK(perf.note_commands[kit[1].id - 1].stretch == 1);
    XSFLIB_CHECK(perf.note_commands[kit[0].id - 1].offset == 6);
    XSFLIB_CHECK(perf.note_commands[kit[1].id - 1].offset == 6);

    // With one pass of the loop, the performance ends at the second time through the list. The pass that starts there
    // belongs to the next pass of the loop.
    export_::agb::ConvertSettings once = Settings();
    once.loops = 1;
    const Performance one_pass = PerformSong(cart.rom, cart.info, 0, once);
    XSFLIB_CHECK(one_pass.end_tick == 8);
    XSFLIB_CHECK(one_pass.stretches.size() == 2);
    SEQLIB_REQUIRE(one_pass.loop.has_value());
    XSFLIB_CHECK(one_pass.loop->start == 0 && one_pass.loop->end == 8);
}

void TestSecondPassLoops()
{
    // A piece's first countdown of n runs its first command n frames in. Each later countdown takes n + 1. The first
    // pass's note therefore lies a frame nearer its start than the note of each later pass. The first pass runs its
    // note on frame 2 and its end on frame 6. The second pass starts there, with its note on frame 9 and its end on
    // frame 13. The loop runs from the second pass's start.
    TestImage image;
    image.SetKey(40, image.AddSample(std::vector<int8_t>(40000, 64)));
    image.AddPiece(1, {image.AddSequence({0x02, 0x00, 0x99, 40, 64, 0x03, 0x00, 0xFC})});
    const Cart cart(image);
    export_::agb::ConvertSettings once = Settings();
    once.loops = 1;
    const Performance perf = PerformSong(cart.rom, cart.info, 0, once);
    SEQLIB_REQUIRE(perf.loop.has_value());
    XSFLIB_CHECK(perf.loop->start == 6 && perf.loop->end == 13);
    XSFLIB_CHECK(perf.end_tick == 13);
    XSFLIB_CHECK(!perf.truncated);
}

void TestCutOff()
{
    // A piece that doesn't end within kMaxFrames is cut off there. Each countdown of 0xFFFF takes 65,536 frames.
    TestImage image;
    image.AddPiece(0, {image.AddSequence({
                          0xFF,
                          0xFF,
                          0xE0,
                          0xFF,
                          0xFF,
                          0xE0,
                          0xFF,
                          0xFF,
                          0xE0,
                          0xFF,
                          0xFF,
                          0xE0,
                          0xFF,
                          0xFF,
                          0xFC,
                      })});
    const Cart cart(image);
    const Performance perf = PerformSong(cart.rom, cart.info, 0, Settings());
    XSFLIB_CHECK(perf.truncated);
    XSFLIB_CHECK(perf.end_tick == ubimilan::kMaxFrames);
    XSFLIB_CHECK(perf.warnings.size() == 1);
}

void TestWithoutNotes()
{
    // A piece that plays no note gives no notes, and its pass stays.
    TestImage image;
    image.AddPiece(0, {image.AddSequence({0x04, 0x00, 0xE0, 0x00, 0x00, 0xFC})});
    const Cart cart(image);
    const Performance perf = PerformSong(cart.rom, cart.info, 0, Settings());
    XSFLIB_CHECK(perf.note_commands.empty());
    SEQLIB_REQUIRE(perf.stretches.size() == 1);
    XSFLIB_CHECK(perf.stretches[0].last == 9);

    // A song that the bank doesn't hold gives no tracks, with a warning.
    const Performance missing = PerformSong(cart.rom, cart.info, 5, Settings());
    for (const std::vector<TrackEvent>& track : missing.tracks)
    {
        XSFLIB_CHECK(track.empty());
    }

    XSFLIB_CHECK(missing.stretches.empty() && missing.sequences.empty());
    XSFLIB_CHECK(!missing.warnings.empty());
}

} // namespace

int main()
{
    if (!ntdlib::testing::agb_files::MakeTempFolder())
    {
        return 1;
    }

    TestRecords();
    TestDecoder();
    TestMidiFile();
    TestPasses();
    TestSecondPassLoops();
    TestCutOff();
    TestWithoutNotes();
    std::error_code ec;
    std::filesystem::remove_all(ntdlib::testing::agb_files::g_temp, ec);
    return xsflib::testing::Summary("Ubisoft Milan's performance");
}
