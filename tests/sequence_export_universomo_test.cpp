// SPDX-License-Identifier: MIT

// Tests of the conversion of Universomo's songs (ntdlib/sequence_export/universomo/convert.h) on songs built by hand.
// The expected events were worked out by hand from the rules in convert.h and player.h.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/rom.h"
#include "ntdlib/sequence_export/universomo/convert.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "ntdlib/testing/universomo_files.h"
#include "ntdlib/universomo/formats.h"

namespace
{

namespace uv = ntdlib::universomo;
namespace conv = ntdlib::sequence_export::universomo;
using ntdlib::testing::universomo_files::BankFile;
using ntdlib::testing::universomo_files::Bytes;
using ntdlib::testing::universomo_files::SongFile;
using ntdlib::testing::universomo_files::TableFile;
using seqlib::midi::EventKind;
using seqlib::midi::TrackEvent;

// A song of one pattern with one track and two instruments: 1 plays sample 0 (8-bit, 16 bytes, once) and 2 plays
// sample 1 (16-bit, 64 bytes, looped from byte 8 for 48 bytes).
uv::Song SongOf(uint32_t speed, uint32_t rows, std::vector<uv::Event> events)
{
    uv::Song song;
    song.speed = speed;
    song.track_count = 1;
    song.orders = {0};
    song.patterns.push_back({rows, {{0, std::move(events)}}});
    song.instruments = {{.sample = 0, .volume = 64, .pan = 128}, {.sample = 1, .volume = 64, .pan = 128}};
    return song;
}

uv::SampleBank Bank()
{
    uv::SampleBank bank;
    bank.samples.push_back({.bits = 8, .data = std::vector<uint8_t>(16, 0x40)});
    bank.samples.push_back(
        {.bits = 16, .loop_start = 8, .loop_length = 48, .looped = true, .data = std::vector<uint8_t>(64, 0x10)});
    return bank;
}

std::vector<TrackEvent> EventsOf(const conv::Performance& perf, EventKind kind)
{
    std::vector<TrackEvent> out;
    std::ranges::copy_if(perf.tracks[0], std::back_inserter(out),
                         [kind](const TrackEvent& e) { return e.kind == kind; });
    return out;
}

// A note of a sample that plays once, at full volume on channel 0. The level is 127/128 at pan 74, whose gain is
// hypot(74, 54) / 128 = 0.7157: CC11 is 127 * sqrt(0.9922 * 0.7157) = 107. CC10 holds the pan 74 as the sin/cos law's
// position 0.1975: 64 + 12.6 = 77. Note 48 is pitch 0, key 60 with no bend. Its 16 samples play in tick 0 (280,095 /
// 2003 = 139.8 samples a tick). The note therefore ends at tick 1. The song stops on the tick that reads its fourth
// row: tick 3 * 4 = 12.
void TestNoteOnce()
{
    const uv::Song song = SongOf(4, 4, {{.value = 0x50, .note = 48, .instrument = 1, .rows = 0}});
    const uv::SampleBank bank = Bank();

    const conv::Performance perf = conv::Perform(song, bank, {.loop = false});

    const std::vector<TrackEvent> on = EventsOf(perf, EventKind::kNoteOn);
    const std::vector<TrackEvent> off = EventsOf(perf, EventKind::kNoteOff);
    const std::vector<TrackEvent> expression = EventsOf(perf, EventKind::kExpression);
    const std::vector<TrackEvent> pan = EventsOf(perf, EventKind::kPan);
    XSFLIB_CHECK(on.size() == 1 && on[0].tick == 0 && on[0].key == 60 && on[0].value == 127);
    XSFLIB_CHECK(off.size() == 1 && off[0].tick == 1);
    XSFLIB_CHECK(!expression.empty() && expression[0].tick == 0 && expression[0].value == 107);
    XSFLIB_CHECK(pan.size() == 1 && pan[0].value == 77);
    XSFLIB_CHECK(EventsOf(perf, EventKind::kPitch).empty());

    // The driver has no reverb send: CC91 0 before the note.
    const std::vector<TrackEvent> reverb = EventsOf(perf, EventKind::kReverb);
    XSFLIB_CHECK(reverb.size() == 1 && reverb[0].tick == 0 && reverb[0].value == 0);
    XSFLIB_CHECK(ntdlib::testing::ReverbBeforeNotes(perf.tracks[0]));
    XSFLIB_CHECK(perf.presets == std::vector<uint8_t>{0});
    XSFLIB_CHECK(perf.end_tick == 12 && !perf.loop);
}

// A pitch slide up of 5 at speed 4 moves 16 a tick in 64ths of a semitone: the bends follow the timers, 0.25 to 1
// semitone over the row, within the timer's rounding. The note keeps key 60.
void TestSlideBends()
{
    const uv::Song song =
        SongOf(4, 4, {{.value = 0x50, .note = 48, .instrument = 2, .rows = 1}, {.value = 5, .note = 0x82, .rows = 3}});
    const uv::SampleBank bank = Bank();

    const conv::Performance perf = conv::Perform(song, bank, {.loop = false});

    const std::vector<TrackEvent> bends = EventsOf(perf, EventKind::kPitch);
    XSFLIB_CHECK(bends.size() == 4);
    for (std::size_t i = 0; i < bends.size() && i < 4; i++)
    {
        XSFLIB_CHECK(bends[i].tick == 4 + i &&
                     std::abs(bends[i].semitones - 0.25f * static_cast<float>(i + 1)) < 0.01f);
    }
}

// A looped song whose pass plays the same each time: the first pass is the loop. Two rows of speed 2 make a pass of 4
// ticks, and the looped sample's note starts again at its start.
void TestLoop()
{
    const uv::Song song = SongOf(2, 2, {{.value = 0x50, .note = 48, .instrument = 2, .rows = 2}});
    const uv::SampleBank bank = Bank();

    const conv::Performance perf = conv::Perform(song, bank, {.loops = 2});

    XSFLIB_CHECK(perf.loop && perf.loop->start == 0 && perf.loop->end == 4);
    XSFLIB_CHECK(perf.loops == 2 && perf.end_tick == 8);
    XSFLIB_CHECK(EventsOf(perf, EventKind::kNoteOn).size() == 2);
}

// A vibrato's position continues from pass to pass. A vibrato of speed 3 and depth 4 on the note's row moves it 6 a
// pass. Each note then starts with another bend: 15/64 of a semitone in the first pass (position 0), 13/64 in the
// second (6) and 6/64 in the third (12). No pass repeats exactly. The loop marks the first pass. It matches the next
// but for its bends, and the report counts the approximation.
void TestVibratoPhaseLoop()
{
    const uv::Song song = SongOf(
        2, 2, {{.value = 0x50, .note = 48, .instrument = 2, .rows = 0}, {.value = 0x34, .note = 0x86, .rows = 2}});
    const uv::SampleBank bank = Bank();

    const conv::Performance perf = conv::Perform(song, bank, {});

    const std::vector<TrackEvent> bends = EventsOf(perf, EventKind::kPitch);
    XSFLIB_CHECK(perf.loop && perf.loop->start == 0 && perf.loop->end == 4 && perf.end_tick == 4);
    XSFLIB_CHECK(perf.approximations.size() == 1 && perf.warnings.empty());
    XSFLIB_CHECK(!bends.empty() && bends.front().tick == 0 &&
                 std::abs(bends.front().semitones - 15.0f / 64.0f) < 0.01f);
}

// A note that sounds across the loop's end. The intro's order starts a looped sample, and the restart order's pattern
// has no note. The first pass after the wrap (ticks 8 to 12) plays as the next one does. The markers therefore loop it.
// The note sounds from tick 0 into every later pass, and the report counts it once.
void TestNoteAcrossLoop()
{
    uv::Song song = SongOf(2, 2, {{.value = 0x50, .note = 48, .instrument = 2, .rows = 2}});
    song.orders = {0, 1};
    song.restart = 1;
    song.patterns.push_back({2, {{0, {}}}});
    const uv::SampleBank bank = Bank();

    const conv::Performance perf = conv::Perform(song, bank, {});

    XSFLIB_CHECK(perf.loop && perf.loop->start == 8 && perf.loop->end == 12 && perf.end_tick == 12);
    XSFLIB_CHECK(perf.approximations.size() == 1 && perf.approximations.begin()->second == 1);
    XSFLIB_CHECK(EventsOf(perf, EventKind::kNoteOn).size() == 1);
}

// The files: a looped 16-bit sample gives an SFZ region over every key, without interpolation, with its loop.
void TestFiles()
{
    const uv::Song song = SongOf(2, 2, {{.value = 0x50, .note = 48, .instrument = 2, .rows = 2}});
    const uv::SampleBank bank = Bank();

    const conv::Conversion c = conv::ConvertSequence(song, bank, "au1", {}, "xyz2midi");

    XSFLIB_CHECK(!c.midi.empty() && !c.sf2.empty() && c.samples >= 1 && c.channels == 1);
    XSFLIB_CHECK(c.sfz.files.size() == 1 && c.sfz.samples.size() == 1 && c.sfz.samples[0].name == "usb_1.wav");
    XSFLIB_CHECK(c.sfz.files.size() == 1 &&
                 c.sfz.files[0].text.find("sample=usb_1.wav lokey=0 hikey=127 "
                                          "pitch_keycenter=60 sample_quality=0") != std::string::npos);
    XSFLIB_CHECK(c.sfz.files.size() == 1 && c.sfz.files[0].text.find("loop_mode=loop_continuous") != std::string::npos);
}

// FindSongs finds the songs beside the bank, whatever the case of their names, and in their numbers' order. The table
// gives the loop flags. A song that can't be read gives a warning. A song's name is "au" and its number. Files in other
// folders and other names don't count.
void TestFindSongs()
{
    namespace nds_files = ntdlib::testing::nds_files;
    nds_files::Image spec;
    spec.game_code = "BEYE";
    spec.files = {{"AU105.UMS", SongFile()},      {"au0.ums", SongFile()},  {"au7.ums", Bytes(8, 0)},
                  {"samplebank.usb", BankFile()}, {"AUDIO", TableFile()},   {"data/au3.ums", SongFile()},
                  {"au.ums", SongFile()},         {"au84.ums", SongFile()}, {"au108.ums", SongFile()}};
    const Bytes image = nds_files::BuildImage(spec).first;
    std::vector<std::string> warnings;

    const std::vector<conv::SongSet> sets = conv::FindSongs(ntdlib::ntr::Rom::Parse(image), warnings);

    const bool found = sets.size() == 1 && sets[0].songs.size() == 4;
    XSFLIB_CHECK(found && warnings.size() == 1);
    if (!found)
    {
        return;
    }

    XSFLIB_CHECK(sets[0].folder.empty() && sets[0].bank.samples.size() == 1);
    XSFLIB_CHECK(sets[0].songs[0].number == 0 && sets[0].songs[0].name == "au0" && !sets[0].songs[0].loop);
    XSFLIB_CHECK(sets[0].songs[1].number == 84 && sets[0].songs[1].name == "au84");
    XSFLIB_CHECK(sets[0].songs[2].number == 105 && sets[0].songs[2].name == "au105" && sets[0].songs[2].loop);
    XSFLIB_CHECK(sets[0].songs[3].number == 108 && sets[0].songs[3].name == "au108");
}

} // namespace

int main()
{
    TestNoteOnce();
    TestSlideBends();
    TestLoop();
    TestVibratoPhaseLoop();
    TestNoteAcrossLoop();
    TestFiles();
    TestFindSongs();

    return xsflib::testing::Summary("Universomo's conversion");
}
