// SPDX-License-Identifier: MIT

// Tests of the length analysis of DSE's songs and sound effects (ntdlib/dse/length_model.h), on songs that play the
// bank and the main bank of dse_song_files.h. Each song plays at 120 quarter notes a minute, 48 ticks to the
// quarter note: 48 ticks last 500 ms.

#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
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

bool Near(long long ms, double expected, double tolerance)
{
    return std::abs(static_cast<double>(ms) - expected) <= tolerance;
}

// A track that loops a note of 24 ticks and a wait of 48 ticks: the second loop ends at 1 s. The fade starts there,
// a tick or two later, and lasts 10 s.
void TestLoop()
{
    const Files files({Conductor(0x30, true), Join({Start(1), {0x99}, Note(0x60, 0, 0, 24, 1), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && !timing->silent && timing->fade_ms == 10000);
    XSFLIB_CHECK(timing && Near(timing->length_ms, 1000.0, 2 * dse::kTickMs + 1));
}

// The track that plays loops every 96 ticks, and track 0 every 48. The fade waits for the second loop of the track
// that plays, at 2 s.
void TestOthersLoop()
{
    const Files files({Conductor(0x30, true), Join({Start(1), {0x99}, Note(0x60, 0, 0, 24, 1), {0x92, 0x60, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && timing->fade_ms == 10000);
    XSFLIB_CHECK(timing && Near(timing->length_ms, 2000.0, 2 * dse::kTickMs + 1));
}

// A song whose tracks end: it ends at its last sound, with a tail of 0.5 s and no fade.
void TestEnd()
{
    const Files files({Conductor(0x30, false), Join({Start(1), Note(0x60, 0, 0, 24, 1), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());

    // The last tick with sound, on a player that plays the song directly.
    dse::Player player;
    player.LoadBanks(files.Get());
    XSFLIB_CHECK(player.Start(files.Get()));
    int last = -1;
    std::vector<ntdlib::ntr::SoundOutput> samples;
    for (int tick = 0; tick < 2000; tick++)
    {
        samples.clear();
        player.RunTick(samples);
        std::vector<int16_t> out;
        ntdlib::ntr::AppendPcm16(samples, out);
        if (xsflib::rip::Audible(out))
        {
            last = tick;
        }
    }
    XSFLIB_CHECK(last > 25 && last < 1500);
    XSFLIB_CHECK(timing && timing->fade_ms == 0);
    XSFLIB_CHECK(timing && Near(timing->length_ms, (last + 1) * dse::kTickMs + 500.0, 1.0));
}

// A note that outlasts its track, on a wave that loops. The track's end at 500 ms stops it. The sound ends a tick or
// two later, with no fade. On the earlier build, a note of length 0 lasts until its track ends the same way. The later
// build releases it at the next tick.
void TestNoteOutlastsSong()
{
    const Files files({Conductor(0x30, false), Join({Start(1), Note(0x60, 0, 0, 0xFFFFFF, 3), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> timing = dse::MeasureSequence(files.Get());
    XSFLIB_CHECK(timing && timing->fade_ms == 0 &&
                 Near(timing->length_ms, 1000.0 + 1.5 * dse::kTickMs, 2 * dse::kTickMs));

    const Files zero({Conductor(0x30, false), Join({Start(1), Note(0x60, 0, 0, 0, 1), {0x92, 0x30, 0x98}})});
    const std::optional<xsflib::rip::Timing> earlier = dse::MeasureSequence(zero.Get(), dse::Build::kEarlier);
    XSFLIB_CHECK(earlier && timing && earlier->fade_ms == 0 && earlier->length_ms == timing->length_ms);
    const std::optional<xsflib::rip::Timing> later = dse::MeasureSequence(zero.Get());
    XSFLIB_CHECK(later && later->fade_ms == 0 && later->length_ms > 500 && later->length_ms < 1000);
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
    TestNoteOutlastsSong();
    TestSilent();
    TestHang();
    return xsflib::testing::Summary("DSE's lengths");
}
