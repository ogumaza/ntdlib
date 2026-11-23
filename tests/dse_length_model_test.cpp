// SPDX-License-Identifier: MIT

// Tests of the length analysis of DSE's songs and sound effects (ntdlib/dse/length_model.h), on songs that play the
// bank and the main bank of dse_song_files.h. Each song plays at 120 quarter notes a minute, 48 ticks to the
// quarter note: 48 ticks last 500 ms.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/near.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/game_starts.h"
#include "ntdlib/dse/length_model.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace dse = ntdlib::dse;
using dse_song_files::Bytes;
using dse_song_files::Join;
using dse_song_files::Note;
using xsflib::testing::Near;

// A track's preamble and its selection of the song's bank, program 0 at full volume and octave 4.
Bytes Start(uint8_t track)
{
    return {track, track, 0, 0, 0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F, 0xA0, 0x04};
}

// Track 0 sets the tempo, then loops a wait of `ticks` ticks, or ends after it.
Bytes Conductor(uint8_t ticks, bool loops)
{
    return loops ? Bytes{0, 0, 0, 0, 0xA4, 0x78, 0x99, 0x92, ticks, 0x98}
                 : Bytes{0, 0, 0, 0, 0xA4, 0x78, 0x92, ticks, 0x98};
}

struct Files
{
    explicit Files(const std::vector<Bytes>& tracks)
        : song(dse::Song::Parse(dse_song_files::SongFile(tracks))),
          bank(dse::WaveBank::Parse(dse_song_files::BankFile())),
          main_bank(dse::WaveBank::Parse(dse_song_files::MainBankFile()))
    {
    }

    dse::SequenceFiles Get() const
    {
        return {.song = &song, .banks = {&bank}, .main_bank = &main_bank};
    }

    dse::Song song;
    dse::WaveBank bank;
    dse::WaveBank main_bank;
};

// A track that loops a note of 24 ticks and a wait of 48 ticks: the second loop ends at 1 s. The fade starts there,
// a tick or two later, and lasts 7 s. A pass lasts 500 ms, and the two passes end where the fade starts.
void TestLoop()
{
    const Files files({Conductor(0x30, true), Join({Start(1), {0x99}, Note(0x60, 0, 0, 24, 1), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && !timing->silent && timing->fade_ms == 7000);
    XSFLIB_CHECK(timing && Near(timing->length_ms, 1000.0, 2 * dse::kTickMs + 1));
    XSFLIB_CHECK(timing && timing->loop && Near(timing->loop->length_ms, 500.0, dse::kTickMs) &&
                 timing->loop->start_ms + 2 * timing->loop->length_ms == timing->length_ms);
}

// The track that plays loops every 96 ticks, and track 0 every 48. The fade waits for the second loop of the track
// that plays, at 2 s.
void TestOthersLoop()
{
    const Files files({Conductor(0x30, true), Join({Start(1), {0x99}, Note(0x60, 0, 0, 24, 1), {0x92, 0x60, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && timing->fade_ms == 7000);
    XSFLIB_CHECK(timing && Near(timing->length_ms, 2000.0, 2 * dse::kTickMs + 1));
}

// The song of TestEnd and TestLevel: a note of 24 ticks, then a wait, and every track ends.
Files EndingSong()
{
    return Files({Conductor(0x30, false), Join({Start(1), Note(0x60, 0, 0, 24, 1), {0x92, 0x30, 0x98}})});
}

// What a player that plays a song directly with a tick of `tick_microseconds` gives: the time of the first sample that
// isn't 0, and the last tick with sound. The output runs at ntr::kOutputRate.
struct DirectRun
{
    std::optional<double> first_ms;
    int last = -1;
};

DirectRun PlayDirectly(const Files& files, const dse::StartParameters& song_start,
                       int tick_microseconds = dse::Driver::kTickMicroseconds)
{
    DirectRun run;
    dse::Player player({.tick_microseconds = tick_microseconds});
    player.LoadBanks(files.Get());
    XSFLIB_CHECK(player.Start(files.Get(), song_start));
    std::vector<ntdlib::ntr::SoundOutput> samples;
    for (int tick = 0; tick < 2000; tick++)
    {
        samples.clear();
        player.RunTick(samples);
        std::vector<int16_t> out;
        ntdlib::ntr::AppendPcm16(samples, out);
        for (std::size_t i = 0; i < out.size() && !run.first_ms; i++)
        {
            if (out[i] != 0)
            {
                run.first_ms = tick * dse::TickMs(tick_microseconds) +
                               static_cast<double>(i / 2) * 1000.0 / ntdlib::ntr::kOutputRate;
            }
        }

        if (xsflib::rip::Audible(out))
        {
            run.last = tick;
        }
    }

    return run;
}

// A song whose tracks end: it ends at its last sound, with no fade. Its first sample that isn't 0 is the first one in
// the output of a player that plays the song directly, rounded to the millisecond.
void TestEnd()
{
    const Files files = EndingSong();

    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    const DirectRun direct = PlayDirectly(files, {});

    XSFLIB_CHECK(direct.last > 25 && direct.last < 1500);
    XSFLIB_CHECK(timing && timing->fade_ms == 0 && !timing->loop);
    XSFLIB_CHECK(timing && Near(timing->length_ms, (direct.last + 1) * dse::kTickMs, 1.0));
    XSFLIB_CHECK(direct.first_ms && timing &&
                 timing->first_sample_ms == static_cast<long long>(*direct.first_ms + 0.5));
}

// A song that a game starts at a level (game_starts.h) is measured at that level. At a lower level, the sound falls
// below the analysis's threshold of silence sooner. At 128 and at 40 of 256, the song ends before it does at full
// level, at the last sound of a player that starts it at the same level directly.
void TestLevel()
{
    const Files files = EndingSong();
    const std::optional<xsflib::rip::Timing> full = dse::MeasureSequence(files.Get());

    for (const int level : {128, 40})
    {
        const dse::StartParameters start = dse::StartOf({.suffix = "", .level = level});

        const std::optional<xsflib::rip::Timing> timing =
            dse::MeasureSequence(files.Get(), dse::Build::kLater, xsflib::rip::Limit::kDefault, start);
        const DirectRun direct = PlayDirectly(files, start);

        XSFLIB_CHECK(timing && full && !timing->silent && timing->length_ms < full->length_ms);
        XSFLIB_CHECK(timing && Near(timing->length_ms, (direct.last + 1) * dse::kTickMs, 1.0));
    }
}

// The slow tick of 16.715 ms, a video frame (Driver::kSlowTickMicroseconds), is a frame of the analysis. The looping
// song of TestLoop keeps its tempo: its loop lasts 500 ms and its second loop ends at 1 s, each to within a frame of
// the slow tick. The ending song of TestEnd ends at the last sound of a player with the slow tick that plays it
// directly. The rules count the limit of 600 s, the silence of 180 s that ends a busy sequence and the 100 ms without
// a busy tick that end a sequence in slow ticks: each is the most whole ticks that fit in its time. The 100 ms are 10
// ticks of 10 ms.
void TestSlowTick()
{
    constexpr int kSlow = dse::Driver::kSlowTickMicroseconds;
    const double slow_ms = dse::TickMs(kSlow);
    const xsflib::rip::LengthRules rules = dse::LengthRules(kSlow);
    const auto fits = [slow_ms](uint64_t frames, double ms)
    {
        return static_cast<double>(frames) * slow_ms <= ms && static_cast<double>(frames + 1) * slow_ms > ms;
    };
    const Files looping({Conductor(0x30, true), Join({Start(1), {0x99}, Note(0x60, 0, 0, 24, 1), {0x92, 0x30, 0x98}})});
    const Files ending = EndingSong();

    const std::optional<xsflib::rip::Timing> loop =
        dse::MeasureSequence(looping.Get(), dse::Build::kLater, xsflib::rip::Limit::kDefault, {}, kSlow);
    const std::optional<xsflib::rip::Timing> end =
        dse::MeasureSequence(ending.Get(), dse::Build::kLater, xsflib::rip::Limit::kDefault, {}, kSlow);
    const DirectRun direct = PlayDirectly(ending, {}, kSlow);

    XSFLIB_CHECK(std::abs(slow_ms - 16.715) < 0.01 && rules.frame_ms == slow_ms);
    XSFLIB_CHECK(fits(rules.max_frames, 600000.0) && fits(rules.silent_frames, 180000.0) &&
                 fits(rules.idle_frames, 100.0) && dse::LengthRules().idle_frames == 10);
    XSFLIB_CHECK(loop && loop->fade_ms == 7000 && Near(loop->length_ms, 1000.0, 2 * slow_ms + 1));
    XSFLIB_CHECK(loop && loop->loop && Near(loop->loop->length_ms, 500.0, slow_ms));
    XSFLIB_CHECK(direct.last > 15 && direct.last < 900);
    XSFLIB_CHECK(end && end->fade_ms == 0 && Near(end->length_ms, (direct.last + 1) * slow_ms, 1.0));
    XSFLIB_CHECK(
        xsflib::testing::ErrorOf<std::invalid_argument>(
            [&] { dse::MeasureSequence(ending.Get(), dse::Build::kLater, xsflib::rip::Limit::kDefault, {}, 12'345); })
            .starts_with("DSE's driver has no tick of 12345 us"));
}

// A note that outlasts its track, on a wave that loops. The track's end at 500 ms stops it. The sound ends a tick or
// two later, with no fade. On the earlier build, a note of length 0 lasts until its track ends the same way. The later
// build releases it at the next tick.
void TestNoteOutlastsSong()
{
    const Files files({Conductor(0x30, false), Join({Start(1), Note(0x60, 0, 0, 0xFFFFFF, 3), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && timing->fade_ms == 0 &&
                 Near(timing->length_ms, 500.0 + 1.5 * dse::kTickMs, 2 * dse::kTickMs));

    const Files zero({Conductor(0x30, false), Join({Start(1), Note(0x60, 0, 0, 0, 1), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> earlier = dse::MeasureSequence(zero.Get(), dse::Build::kEarlier);
    XSFLIB_CHECK(earlier && timing && earlier->fade_ms == 0 && earlier->length_ms == timing->length_ms);
    const std::optional<xsflib::rip::Timing> later = dse::MeasureSequence(zero.Get());
    XSFLIB_CHECK(later && later->fade_ms == 0 && later->length_ms > 0 && later->length_ms < 500);
}

// A song that makes no sound is silent.
void TestSilent()
{
    const Files files({Conductor(0x30, false)});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && timing->silent && timing->fade_ms == 0);
}

} // namespace

// A track that loops without waiting hangs the driver, and the measure stops with the driver's Hang. A track with a
// wait in the same loop plays on.
void TestHang()
{
    const Files hangs({Conductor(0x30, true), {1, 1, 0, 0, 0x99, 0x98}});
    XSFLIB_CHECK(xsflib::testing::ErrorOf<dse::Hang>([&] { dse::MeasureSequence(hangs.Get()); })
                     .starts_with("track 1 runs 100000 commands in a tick without waiting, up to offset "));
    const Files waits({Conductor(0x30, true), {1, 1, 0, 0, 0x99, 0x92, 0x30, 0x98}});
    XSFLIB_CHECK(dse::MeasureSequence(waits.Get()).has_value());
}

int main()
{
    TestLoop();
    TestOthersLoop();
    TestEnd();
    TestLevel();
    TestSlowTick();
    TestNoteOutlastsSong();
    TestSilent();
    TestHang();
    return xsflib::testing::Summary("DSE's lengths");
}
