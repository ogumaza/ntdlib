// SPDX-License-Identifier: MIT

// Tests of the performance of a song of Konami's GBA driver (ntdlib/sequence_export/konami/performance.h), on small
// carts built here. Each song's bytes are worked out by hand from the driver's rules in ntdlib/konami/seqformat.h: a
// delay of n takes n frames, and a tick is a frame. The frames, offsets, stretches and mix follow from them. Every note
// then has to be a note of the MIDI file that the conversion writes.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "seqlib/midi/reader.h"
#include "xsflib/testing/check.h"

#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/konami/convert.h"
#include "ntdlib/sequence_export/konami/performance.h"
#include "ntdlib/testing/agb_cart.h"

using xsflib::testing::Check;

namespace
{

namespace konami = ntdlib::konami;
namespace export_ = ntdlib::sequence_export;
namespace perf_ = export_::konami;
using export_::Entry;
using export_::Mix;
using export_::MixChange;
using export_::Stretch;
using export_::TrackEvent;
using konami::Revision;
using konami::Rom;
using ntdlib::testing::agb_files::CartImage;
using ntdlib::testing::agb_files::kRomBase;
using perf_::NoteCause;
using perf_::Performance;

constexpr uint32_t kTable = kRomBase + 0x100;
constexpr uint32_t kBase = kRomBase + 0x200;
constexpr uint32_t kSamples = kRomBase + 0x600;
constexpr uint32_t kHeader = kRomBase + 0x700;

// The offset of the tracks that end at once, after the song's other bytes.
constexpr uint16_t kRest = 0x100;

// Returns a cart with a song table of one song at kTable. Each track starts at kRest but for those that `first` gives,
// and holds `00 FD` there: a delay of 0, and the song's end. Sample 5 of the table at kSamples plays 64 bytes.
CartImage Cart(Revision revision, const std::map<int, std::vector<uint8_t>>& tracks)
{
    CartImage cart(0x1000);
    cart.Put32(kTable, kBase);
    uint16_t at = 0;
    for (int t = 0; t < konami::TrackCount(revision); t++)
    {
        const auto track = tracks.find(t);
        cart.Put16(kTable + 4 + 2 * uint32_t(t), track == tracks.end() ? kRest : at);
        if (track != tracks.end())
        {
            cart.PutBytes(kBase + at, track->second);
            at = uint16_t(at + track->second.size());
        }
    }
    cart.PutBytes(kBase + kRest, {0x00, 0xFD});

    cart.Put32(kSamples + 4 * 5, kHeader);
    cart.Put32(kHeader, 0x1000);
    cart.Put32(kHeader + 4, 64);
    cart.Put32(kHeader + 8, 0xFFFFFFFF);
    for (uint32_t i = 0; i < 64; i++)
    {
        cart.Put8(kHeader + 12 + i, uint8_t(i * 4));
    }

    return cart;
}

// The DriverInfo of a song at the places above.
konami::DriverInfo InfoOf(Revision revision)
{
    konami::DriverInfo info;
    info.revision = revision;
    info.song_table = kTable;
    info.song_count = 1;
    info.sample_table = kSamples;
    info.mix_rate = 16000;
    return info;
}

// The track's notes, with each one's frames and ID.
struct Note
{
    uint32_t on = 0;
    uint32_t off = 0;
    int key = 0;
    uint32_t id = 0;
};

std::vector<Note> NotesOf(const std::vector<TrackEvent>& events)
{
    std::vector<Note> notes;
    for (const TrackEvent& e : events)
    {
        if (e.kind == export_::EventKind::kNoteOn)
        {
            notes.push_back({e.tick, 0, e.key, e.note});
        }
        else if (e.kind == export_::EventKind::kNoteOff)
        {
            notes.back().off = e.tick;
        }
    }

    return notes;
}

// Checks that the performance holds the notes of the MIDI file that the conversion writes with `settings`: their
// number, their keys and their ticks.
void CheckAgainstMidi(const Rom& rom, const konami::DriverInfo& info, const Performance& perf,
                      const export_::agb::ConvertSettings& settings)
{
    const perf_::SongFiles files = perf_::ConvertSongFiles(rom, info, 0, settings);
    if (!Check(files.report.ok, "the song converts"))
    {
        return;
    }

    const seqlib::midi::File file = seqlib::midi::Read(files.midi);
    std::vector<std::pair<uint64_t, int>> in_file;
    for (std::size_t i = 1; i < file.tracks.size(); i++)
    {
        for (const seqlib::midi::Event& e : file.tracks[i].events)
        {
            if ((e.bytes[0] & 0xF0) == 0x90 && e.bytes[2] > 0)
            {
                in_file.push_back({e.tick, e.bytes[1]});
            }
        }
    }

    std::vector<std::pair<uint64_t, int>> in_performance;
    for (const std::vector<TrackEvent>& track : perf.tracks)
    {
        for (const Note& note : NotesOf(track))
        {
            in_performance.push_back({perf.midi_places[note.id - 1].on, note.key});
        }
    }
    Check(in_performance == in_file, "the performance holds the MIDI file's notes");
}

void TestOneTrack()
{
    const std::map<int, std::vector<uint8_t>> tracks = {
        // Offset 0: a delay of 0, a PSG note (volume 3, note 24) at frame 0, a delay of 4, a rest at frame 4, a delay
        // of
        // 0, the track's end. Note 0 of a square channel is C2, MIDI key 36.
        {0, {0x00, 0xD3, 0x18, 0x04, 0xE0, 0x00, 0xFE}},
        // Offset 7: a delay of 0, a note (volume 16, sample 5) at frame 0, a delay of 10, a volume of 7 at frame 10, a
        // delay of 5, a pan of 0x20 at frame 15, a delay of 3, a rest at frame 18, a delay of 2, the track's end at
        // frame
        // 20.
        {4, {0x00, 0xA8, 0x10, 0x05, 0x0A, 0xC7, 0x05, 0xF0, 0x20, 0x03, 0xE0, 0x02, 0xFE}},
    };
    const CartImage cart = Cart(Revision::kUltimateMasters, tracks);
    const Rom rom = cart.ToRom();
    const konami::DriverInfo info = InfoOf(Revision::kUltimateMasters);
    const export_::agb::ConvertSettings settings;
    const Performance perf = perf_::PerformSong(rom, info, 0, settings);

    Check(perf.base == kBase, "offsets count from the song's base");
    Check(!perf.loop, "the song doesn't loop");
    Check(perf.track_names[0] == "Square 1" && perf.track_names[4] == "Voice 0", "the tracks' names");

    // The last track's end comes in frame 20, the song's 21st. The last notes then ring for 120 more frames.
    Check(perf.end_tick == 21 + 120, "the song's end");

    const std::vector<Note> square = NotesOf(perf.tracks[0]);
    if (Check(square.size() == 1, "the square's note"))
    {
        Check(square[0].on == 0 && square[0].off == 4 && square[0].key == 60, "the square's note's frames and key");
        Check(perf.note_commands[square[0].id - 1].offset == 1, "the square's note's command");
        Check(perf.note_starts[square[0].id - 1] == perf_::NoteStart{0, NoteCause::kNote}, "the square's note's cause");
    }

    const std::vector<Note> voice = NotesOf(perf.tracks[4]);
    if (Check(voice.size() == 1, "the voice's note: a volume plays on"))
    {
        Check(voice[0].on == 0 && voice[0].off == 18, "the voice's note's frames");
        Check(perf.note_commands[voice[0].id - 1].offset == 8, "the voice's note's command");
    }

    const std::vector<MixChange> voice_mix = {
        {0, Mix::kVolume, 16}, {10, Mix::kVolume, 7}, {15, Mix::kPan, 0x20 - 0x40}};
    Check(perf.mix[4] == voice_mix, "the voice's mix");
    const std::vector<MixChange> square_mix = {{0, Mix::kVolume, 3}};
    Check(perf.mix[0] == square_mix, "the square's mix");

    // Each track reads straight through its bytes.
    const std::vector<Stretch> voice_stretches = {{0, 7, 7 + 12, Entry::kOpen, 0, false}};
    Check(perf.stretches[4] == voice_stretches, "the voice's stretch");
    const std::vector<Stretch> ended = {{0, kRest, kRest + 1, Entry::kOpen, 0, false}};
    Check(perf.stretches[8] == ended, "a track that ends at once");

    CheckAgainstMidi(rom, info, perf, settings);
}

void TestCalls()
{
    // Offset 0: a delay of 0, a note at frame 0, a delay of 4, a call of 2 commands from offset 10 at frame 4. There, a
    // delay of 1, a rest at frame 5, a delay of 2, a note at frame 7. The track then goes back to offset 8: a delay of
    // 3 and a loop of the song at frame 10.
    const std::map<int, std::vector<uint8_t>> tracks = {
        {0, {0x00, 0xD3, 0x18, 0x04, 0x90, 0x0A, 0x00, 0x02, 0x03, 0xFE, 0x01, 0xE0, 0x02, 0xD3, 0x18}},
    };
    const CartImage cart = Cart(Revision::kWct2004, tracks);
    const Rom rom = cart.ToRom();
    const konami::DriverInfo info = InfoOf(Revision::kWct2004);

    export_::agb::ConvertSettings settings;
    settings.loops = 2;
    const Performance perf = perf_::PerformSong(rom, info, 0, settings);
    if (Check(perf.loop.has_value(), "the song loops"))
    {
        Check(perf.loop->start == 0 && perf.loop->end == 10, "the loop's frames");
    }

    const std::vector<Stretch> expected = {
        {0, 0, 4, Entry::kOpen, 0, false},  {4, 10, 13, Entry::kCall, 1, false},  {7, 8, 9, Entry::kReturn, 0, false},
        {10, 0, 4, Entry::kLoop, 0, false}, {14, 10, 13, Entry::kCall, 1, false}, {17, 8, 9, Entry::kReturn, 0, false},
    };
    Check(perf.stretches[0] == expected, "the stretches of a call and a loop");

    const std::vector<Note> notes = NotesOf(perf.tracks[0]);
    if (Check(notes.size() == 4, "the notes of two passes"))
    {
        const uint32_t frames[] = {0, 7, 10, 17};
        const uint32_t offsets[] = {1, 13, 1, 13};
        const uint32_t stretches[] = {0, 1, 3, 4};
        for (std::size_t n = 0; n < 4; n++)
        {
            const export_::NoteCommand& command = perf.note_commands[notes[n].id - 1];
            Check(notes[n].on == frames[n] && command.offset == offsets[n] && command.stretch == stretches[n],
                  "a note's frame, command and stretch");
        }
    }

    CheckAgainstMidi(rom, info, perf, settings);
}

void TestOtherCauses()
{
    // A volume restarts a PSG channel: a delay of 0, a note, a delay of 4, a volume of 5 at frame 4, a delay of 4, a
    // rest at frame 8, a delay of 0, the track's end.
    {
        const std::map<int, std::vector<uint8_t>> tracks = {
            {0, {0x00, 0xD3, 0x18, 0x04, 0xC5, 0x04, 0xE0, 0x00, 0xFE}},
        };
        const CartImage cart = Cart(Revision::kUltimateMasters, tracks);
        const Rom rom = cart.ToRom();
        const konami::DriverInfo info = InfoOf(Revision::kUltimateMasters);
        const Performance perf = perf_::PerformSong(rom, info, 0, {});
        const std::vector<Note> notes = NotesOf(perf.tracks[0]);
        if (Check(notes.size() == 2, "a volume starts a second note"))
        {
            Check(notes[1].on == 4 && perf.note_commands[notes[1].id - 1].offset == 4 &&
                      perf.note_starts[notes[1].id - 1] == perf_::NoteStart{0, NoteCause::kOther},
                  "the volume's note leads to the volume");
        }
        const std::vector<MixChange> mix = {{0, Mix::kVolume, 3}, {4, Mix::kVolume, 5}};
        Check(perf.mix[0] == mix, "the volume's mix");
        CheckAgainstMidi(rom, info, perf, {});
    }

    // The older revisions: a pan of left 8 and right F on square 1 plays the left side on the next channel. Its note
    // leads to square 1's command: a delay of 0, the pan, a delay of 0, a note at frame 0, a delay of 8, a rest, a
    // delay of 0, the track's end.
    {
        const std::map<int, std::vector<uint8_t>> tracks = {
            {0, {0x00, 0xF0, 0x8F, 0x00, 0xD3, 0x18, 0x08, 0xE0, 0x00, 0xFD}},
        };
        const CartImage cart = Cart(Revision::kWct2004, tracks);
        const Rom rom = cart.ToRom();
        const konami::DriverInfo info = InfoOf(Revision::kWct2004);
        const Performance perf = perf_::PerformSong(rom, info, 0, {});
        const std::vector<Note> notes = NotesOf(perf.tracks[1]);
        if (Check(!notes.empty(), "the pan plays a note on the next channel"))
        {
            Check(notes[0].on == 0 && perf.note_commands[notes[0].id - 1].offset == 4 &&
                      perf.note_starts[notes[0].id - 1] == perf_::NoteStart{0, NoteCause::kNote},
                  "the next channel's note leads to the track before it");
        }

        // The right side's level is 15, and the left side's 8: the pan lies 7/15 of the way to the right.
        const std::vector<MixChange> mix = {{0, Mix::kPan, 29}, {0, Mix::kVolume, 3}};
        Check(perf.mix[0] == mix, "the pan's levels");
        CheckAgainstMidi(rom, info, perf, {});
    }

    // An attack's steps restart a PSG channel in frames without a command. Those notes lead to the note command: a
    // delay of 0, an attack of 1, a delay of 0, a note at frame 0, a delay of 8, a rest, a delay of 0, the track's end.
    {
        const std::map<int, std::vector<uint8_t>> tracks = {
            {0, {0x00, 0xF4, 0x01, 0x00, 0xD8, 0x0F, 0x18, 0x08, 0xE0, 0x00, 0xFD}},
        };
        const CartImage cart = Cart(Revision::kWct2004, tracks);
        const Rom rom = cart.ToRom();
        const konami::DriverInfo info = InfoOf(Revision::kWct2004);
        const Performance perf = perf_::PerformSong(rom, info, 0, {});
        const std::vector<Note> notes = NotesOf(perf.tracks[0]);
        bool earlier = false;
        for (const Note& note : notes)
        {
            const perf_::NoteStart& start = perf.note_starts[note.id - 1];
            earlier = earlier || start.cause == NoteCause::kEarlier;
            Check(start.cause == NoteCause::kNote || start.cause == NoteCause::kEarlier,
                  "a step of an attack belongs to its note");
            Check(perf.note_commands[note.id - 1].offset == 4, "each step leads to the note command");
        }
        Check(earlier, "the attack's steps start notes");
        CheckAgainstMidi(rom, info, perf, {});
    }
}

void TestDungeonDice()
{
    // Offset 0: a delay of 0 and FA at frame 0: its last two bytes, a delay of 0 and a duty byte, run after a loop of
    // the song. A delay of 0, a note of volume 8 at frame 0, a delay of 4, a note of volume 7 on the next track at
    // frame 4, a delay of 4, a rest at frame 8, a delay of 2, and a loop of the song at frame 10.
    const std::map<int, std::vector<uint8_t>> tracks = {
        {4, {0x00, 0xFA, 0x00, 0x7E, 0x00, 0xB8, 0x05, 0x04, 0xA7, 0x05, 0x04, 0xFC, 0x02, 0xFE}},
    };
    CartImage cart = Cart(Revision::kDungeonDiceMonsters, tracks);

    // Entry 5 of the sample table: a pointer to the samples, and a word with 4 blocks of 16 at a period of 0x400.
    cart.Put32(kSamples + 8 * 5, kHeader + 12);
    cart.Put32(kSamples + 8 * 5 + 4, (4u << 20) | 0x400);
    const Rom rom = cart.ToRom();
    const konami::DriverInfo info = InfoOf(Revision::kDungeonDiceMonsters);

    export_::agb::ConvertSettings settings;
    settings.loops = 2;
    const Performance perf = perf_::PerformSong(rom, info, 0, settings);

    const std::vector<Note> here = NotesOf(perf.tracks[4]);
    if (Check(!here.empty(), "a note on the track"))
    {
        Check(here[0].on == 0 && perf.note_commands[here[0].id - 1].offset == 5,
              "the track's note leads to its command");
    }
    const std::vector<Note> next = NotesOf(perf.tracks[5]);
    if (Check(!next.empty(), "a note on the next track"))
    {
        Check(next[0].on == 4 && perf.note_commands[next[0].id - 1].offset == 8 &&
                  perf.note_starts[next[0].id - 1] == perf_::NoteStart{4, NoteCause::kNote},
              "the next track's note leads to the track before it");
    }
    if (Check(!perf.mix[5].empty(), "the next track's mix"))
    {
        Check(perf.mix[5][0] == MixChange{4, Mix::kVolume, 7}, "the next track's volume");
    }

    // After a loop, the track reads FA's last two bytes. The stretch begins with FA's item.
    if (Check(perf.stretches[4].size() >= 2, "a stretch after the loop"))
    {
        const Stretch first = {0, 0, 13, Entry::kOpen, 0, false};
        Check(perf.stretches[4][0] == first, "the track's first stretch");
        Check(perf.stretches[4][1].tick == 10 && perf.stretches[4][1].first == 1 &&
                  perf.stretches[4][1].entry == Entry::kLoop,
              "the stretch after the loop begins with FA");
    }

    CheckAgainstMidi(rom, info, perf, settings);
}

void TestUnreadable()
{
    CartImage cart(0x1000);
    cart.Put32(kTable, kRomBase + 0x2000);
    const konami::DriverInfo info = InfoOf(Revision::kUltimateMasters);
    const Performance perf = perf_::PerformSong(cart.ToRom(), info, 0, {});
    Check(perf.note_commands.empty() && !perf.warnings.empty(), "a song whose header names no bytes gives no notes");
}

} // namespace

int main()
{
    TestOneTrack();
    TestCalls();
    TestOtherCauses();
    TestDungeonDice();
    TestUnreadable();

    return xsflib::testing::Summary("Konami's performance");
}
