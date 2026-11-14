// SPDX-License-Identifier: MIT

// Tests the readers of Universomo's songs, sample bank and table of sounds, and the walk of a song
// (ntdlib/universomo), on files built here: every field of a song, signed instrument settings, the errors of damaged
// files, 8-bit and 16-bit samples, the kinds of sounds, the rows and orders of the walk, the speed effect, tracks with
// waits and without events, and the lengths of looped songs and of songs that play once.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/universomo/formats.h"
#include "ntdlib/universomo/walk.h"

namespace
{

namespace universomo = ntdlib::universomo;

using universomo::Event;
using universomo::Instrument;
using universomo::Pattern;
using universomo::Song;
using universomo::Track;
using xsflib::testing::Check;

void AppendText(std::vector<uint8_t>& out, const std::string& text)
{
    out.insert(out.end(), text.begin(), text.end());
}

// A song's file, laid out as the driver's loader reads it.
std::vector<uint8_t> SongFile(const Song& song)
{
    std::vector<uint8_t> out;
    AppendText(out, "UNIMUSICSQ:");
    for (const uint32_t word :
         {song.tempo, song.speed, static_cast<uint32_t>(song.orders.size()), song.track_count,
          static_cast<uint32_t>(song.patterns.size()), static_cast<uint32_t>(song.instruments.size()), song.restart})
    {
        xsflib::AppendLe32(out, word);
    }

    for (const Instrument& instrument : song.instruments)
    {
        out.push_back(instrument.sample);
    }

    out.insert(out.end(), song.orders.begin(), song.orders.end());
    for (const Pattern& pattern : song.patterns)
    {
        xsflib::AppendLe32(out, pattern.rows);
        for (const Track& track : pattern.tracks)
        {
            out.push_back(track.first_rows);
            xsflib::AppendLe32(out, static_cast<uint32_t>(track.events.size()));
            for (const Event& event : track.events)
            {
                out.insert(out.end(), {event.value, event.note, event.instrument, event.rows});
            }
        }
    }

    for (const Instrument& instrument : song.instruments)
    {
        out.insert(out.end(), {instrument.volume, instrument.pan, static_cast<uint8_t>(instrument.finetune),
                               static_cast<uint8_t>(instrument.relative_note)});
    }

    return out;
}

// A song of one pattern with `rows` rows and one track that has no events.
Song EmptySong(uint32_t speed, uint32_t rows)
{
    Song song;
    song.tempo = 150;
    song.speed = speed;
    song.track_count = 1;
    song.orders = {0};
    song.patterns = {{rows, {{0, {}}}}};

    return song;
}

bool Throws(const std::function<void()>& call)
{
    try
    {
        call();
    }
    catch (const xsflib::FormatError&)
    {
        return true;
    }

    return false;
}

void TestSong()
{
    Song song;
    song.tempo = 150;
    song.speed = 6;
    song.restart = 1;
    song.track_count = 2;
    song.orders = {1, 0, 1};
    song.patterns = {
        {16, {{0, {{0x40, 49, 1, 4}, {0x05, 0x85, 0, 0}, {0, 50, 2, 12}}}, {3, {}}}},
        {8, {{2, {{0x30, 0x89, 0, 6}}}, {0, {{0x10, 0, 0, 8}}}}},
    };
    song.instruments = {{3, 64, 128, -12, 5}, {86, 40, 0, 127, -128}};
    const std::vector<uint8_t> file = SongFile(song);

    const Song read = universomo::ReadSong(file);

    Check(read == song, "a song reads back field for field");
    Check(read.instruments[0].finetune == -12 && read.instruments[1].relative_note == -128,
          "finetune and the relative note are signed");
    std::vector<uint8_t> longer = file;
    longer.push_back(0xAA);
    Check(universomo::ReadSong(longer) == song, "bytes after the instruments don't count");
}

void TestSongErrors()
{
    const Song good = EmptySong(4, 4);
    const auto reads = [](const Song& song)
    {
        const std::vector<uint8_t> file = SongFile(song);
        return !Throws([&] { universomo::ReadSong(file); });
    };

    Check(reads(good), "the song for the errors reads");
    std::vector<uint8_t> file = SongFile(good);
    file[0] = 'X';
    Check(Throws([&] { universomo::ReadSong(file); }), "a file without the magic is refused");
    file = SongFile(good);
    file.pop_back();
    Check(Throws([&] { universomo::ReadSong(file); }), "a file cut short is refused");
    Check(Throws([&] { universomo::ReadSong(std::vector<uint8_t>(10, 0)); }),
          "a file shorter than the header is refused");

    Song song = good;
    song.orders = {1};
    Check(!reads(song), "an order that names a missing pattern is refused");
    song = good;
    song.restart = 1;
    Check(!reads(song), "a restart past the order list is refused");
    song = good;
    song.orders.clear();
    Check(!reads(song), "a song without orders is refused");
    song = good;
    song.track_count = 17;
    song.patterns[0].tracks.resize(17);
    Check(!reads(song), "a song with 17 tracks is refused");
    song = good;
    song.track_count = 16;
    song.patterns[0].tracks.resize(16);
    Check(reads(song), "a song with 16 tracks reads");

    file = SongFile(good);
    xsflib::StoreLe32(file.data() + 0x27 + 1 + 4 + 1, 0xFFFFFFFF);
    Check(Throws([&] { universomo::ReadSong(file); }), "an event count past the end is refused");
    file = SongFile(good);
    xsflib::StoreLe32(file.data() + 0x1B, 0x40000000);
    Check(Throws([&] { universomo::ReadSong(file); }), "a pattern count past the end is refused");
}

std::vector<uint8_t> SampleRecord(uint8_t bits, uint32_t loop_start, uint32_t loop_length, uint8_t looped,
                                  const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out;
    xsflib::AppendLe32(out, static_cast<uint32_t>(data.size()));
    out.push_back(bits);
    xsflib::AppendLe32(out, loop_start);
    xsflib::AppendLe32(out, loop_length);
    out.push_back(looped);
    out.insert(out.end(), data.begin(), data.end());

    return out;
}

void TestBank()
{
    const std::vector<uint8_t> first = SampleRecord(8, 0, 0, 0, {1, 2, 0xFF, 0x80});
    const std::vector<uint8_t> second = SampleRecord(16, 4, 8, 1, {0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0});
    const std::vector<uint8_t> empty = SampleRecord(8, 0, 0, 0, {});
    std::vector<uint8_t> file;
    AppendText(file, "UNISAMBA");
    file.push_back(0x3A);
    xsflib::AppendLe32(file, 3);
    xsflib::AppendLe32(file, static_cast<uint32_t>(second.size())); // the records needn't be in order
    xsflib::AppendLe32(file, 0);
    xsflib::AppendLe32(file, static_cast<uint32_t>(second.size() + first.size()));
    file.insert(file.end(), second.begin(), second.end());
    file.insert(file.end(), first.begin(), first.end());
    file.insert(file.end(), empty.begin(), empty.end());

    const universomo::SampleBank bank = universomo::ReadSampleBank(file);

    Check(bank.version == 0x3A && bank.samples.size() == 3, "the bank gives its byte 8 and its samples");
    Check(bank.samples[0].bits == 8 && !bank.samples[0].looped &&
              bank.samples[0].data == std::vector<uint8_t>{1, 2, 0xFF, 0x80},
          "an 8-bit sample that doesn't loop");
    Check(bank.samples[1].bits == 16 && bank.samples[1].looped && bank.samples[1].loop_start == 4 &&
              bank.samples[1].loop_length == 8 && bank.samples[1].data.size() == 12,
          "a looped 16-bit sample, with its loop in bytes");
    Check(bank.samples[2].data.empty(), "an empty sample");

    std::vector<uint8_t> bad = file;
    bad[13 + 12 + 4] = 12;
    Check(Throws([&] { universomo::ReadSampleBank(bad); }), "a sample of 12 bits is refused");
    bad = file;
    bad.resize(bad.size() - 1);
    Check(Throws([&] { universomo::ReadSampleBank(bad); }), "a bank cut short is refused");
    bad = file;
    bad[0] = 'X';
    Check(Throws([&] { universomo::ReadSampleBank(bad); }), "a file without the magic is refused");
}

void TestSoundTable()
{
    std::vector<uint8_t> file;
    AppendText(file, "AU0");
    file.push_back(3);
    file.insert(file.end(), {0, 0, 0, 0, 3, 0, 0});
    file.insert(file.end(), {0, 1, 1, 0, 1, 0, 0});
    file.insert(file.end(), {0, 1, 2, 0, 5, 0, 0});

    const std::vector<universomo::Sound> sounds = universomo::ReadSoundTable(file);

    Check(sounds.size() == 3, "the table gives a sound for each number");
    Check(!sounds[0].loops && sounds[0].kind == universomo::SoundKind::kEffect && sounds[0].bytes[4] == 3,
          "a sound effect that plays once");
    Check(sounds[1].loops && sounds[1].kind == universomo::SoundKind::kMusic, "music loops");
    Check(sounds[2].loops && sounds[2].kind == universomo::SoundKind::kLoopedEffect, "a looped sound effect");
    file.pop_back();
    Check(Throws([&] { universomo::ReadSoundTable(file); }), "a table cut short is refused");
    Check(universomo::SongPath(105) == "/au105.ums", "the path of song 105");
}

void TestWalk()
{
    const Song song = EmptySong(4, 3);
    universomo::Walk walk(song, true);
    Check(walk.ReadsRowNext() && walk.Order() == 0 && walk.Row() == 0, "the first call reads the first row");

    walk.Update();

    Check(walk.Row() == 1 && walk.Tick() == 1 && !walk.ReadsRowNext(), "the first call counts the row");
    for (int i = 0; i < 3; ++i)
    {
        walk.Update();
    }

    Check(walk.Row() == 1 && walk.ReadsRowNext(), "a row lasts the speed's calls");
    for (int i = 0; i < 5; ++i)
    {
        walk.Update();
    }

    Check(walk.Wraps() == 1 && walk.Updates() == 9 && walk.Order() == 0 && walk.Row() == 0,
          "the read of the last row goes back to the restart order");
    Check(universomo::MeasureSong(song, true) == universomo::SongLength{0, 12}, "the loop is rows times speed");
    Check(universomo::MeasureSong(song, false) == universomo::SongLength{9, 0},
          "a song that plays once stops on the call that reads its last row");

    universomo::Walk once(song, false);
    for (int i = 0; i < 20; ++i)
    {
        once.Update();
    }

    Check(!once.Playing() && once.Updates() == 9, "a stopped song counts no more calls");
}

void TestWalkRestartAndWaits()
{
    // Order 0 has 2 rows, orders 1 and 2 have 3 rows each. The loop goes back to order 1.
    Song song;
    song.speed = 2;
    song.restart = 1;
    song.track_count = 2;
    song.orders = {0, 1, 1};
    song.patterns = {{2, {{0, {}}, {0, {}}}},
                     {3, {{1, {{0x10, 49, 1, 1}}}, {0, {{0x10, 49, 1, 0}, {0x20, 50, 1, 2}}}}}};

    const universomo::SongLength length = universomo::MeasureSong(song, true);

    Check(length == universomo::SongLength{4, 12}, "the intro runs to the restart order's first row");
    Check(universomo::MeasureSong(song, false) == universomo::SongLength{15, 0}, "8 rows of 2 calls, less one");
}

void TestWalkSpeed()
{
    // The effect kSpeed on row 0 sets 3 calls a row, row 0 included, and row 2's sets 6. The header gives 5.
    Song song = EmptySong(5, 4);
    song.patterns[0].tracks[0].events = {{3, universomo::kEffectFlag | universomo::kSpeed, 0, 2},
                                         {6, universomo::kEffectFlag | universomo::kSpeed, 0, 0}};

    universomo::Walk walk(song, true);
    walk.Update();
    const uint32_t after_row_0 = walk.Speed();
    for (int i = 0; i < 6; ++i)
    {
        walk.Update();
    }

    Check(after_row_0 == 3, "the effect sets the speed on its row");
    Check(walk.Speed() == 6 && walk.Row() == 3, "row 2's effect sets 6 calls a row");
    Check(universomo::MeasureSong(song, false) == universomo::SongLength{1 + 3 + 3 + 6, 0},
          "the rows take the speeds that the effects set");
    // The first pass starts at the header's speed and each later pass at the last row's, so the loop starts with the
    // second pass. Each pass takes 18 calls.
    Check(universomo::MeasureSong(song, true) == universomo::SongLength{18, 6 + 3 + 3 + 6},
          "the loop starts with the first pass that starts at the speed that the last row leaves");
}

void TestWalkTrackWithoutWaits()
{
    // A track whose events all wait 0 reads them on its first row, then waits 255 rows.
    Song song = EmptySong(1, 300);
    song.patterns[0].tracks[0].events = {{3, universomo::kEffectFlag | universomo::kSpeed, 0, 0},
                                         {2, universomo::kEffectFlag | universomo::kSpeed, 0, 0}};

    const universomo::SongLength length = universomo::MeasureSong(song, false);

    Check(length.intro == 1 + 299 * 2, "the last event's speed holds for the pattern");
    Check(Throws([&] { universomo::MeasureSong(song, false, 100); }), "a song past the limit is refused");
}

} // namespace

int main()
{
    TestSong();
    TestSongErrors();
    TestBank();
    TestSoundTable();
    TestWalk();
    TestWalkRestartAndWaits();
    TestWalkSpeed();
    TestWalkTrackWithoutWaits();

    return xsflib::testing::Summary("universomo");
}
