// SPDX-License-Identifier: MIT

// Tests of the model of Universomo's sequencer (ntdlib/universomo/player.h) on songs built by hand. The expected
// commands were worked out by hand from the rules in player.h: the rows and ticks, the volume's hardware value, the
// slides, the vibrato's table and the note delay.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/universomo/formats.h"
#include "ntdlib/universomo/player.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace uv = ntdlib::universomo;
using Kind = uv::Command::Kind;

// A song of one pattern of `rows` rows, with `tracks` tracks and two instruments: 1 plays sample 0 and 2 plays sample
// 1, both at volume 64 with no tuning.
uv::Song SongOf(uint32_t speed, uint32_t rows, std::vector<uv::Track> tracks)
{
    uv::Song song;
    song.tempo = 150;
    song.speed = speed;
    song.track_count = static_cast<uint32_t>(tracks.size());
    song.orders = {0};
    song.patterns.push_back({rows, std::move(tracks)});
    song.instruments = {{.sample = 0, .volume = 64, .pan = 128}, {.sample = 1, .volume = 64, .pan = 128}};
    return song;
}

// Sample 0: 16 bytes of 8-bit PCM that play once. Sample 1: 64 bytes of 16-bit PCM with a loop from byte 8, 48 long.
uv::SampleBank Bank()
{
    uv::SampleBank bank;
    bank.samples.push_back({.bits = 8, .data = std::vector<uint8_t>(16)});
    bank.samples.push_back(
        {.bits = 16, .loop_start = 8, .loop_length = 48, .looped = true, .data = std::vector<uint8_t>(64)});
    return bank;
}

uint16_t TimerAt(int pitch)
{
    return ntdlib::ntr::snd::Timer(uv::kBaseTimer, pitch);
}

// The timers that `player` sends on `channel` over `ticks` updates, one entry a tick (0 for a tick without one).
std::vector<uint16_t> Timers(uv::Player& player, int ticks, uint8_t channel = 0)
{
    std::vector<uint16_t> timers;
    for (int i = 0; i < ticks; i++)
    {
        uint16_t timer = 0;
        for (const uv::Command& c : player.Update())
        {
            if (c.kind == Kind::kTimer && c.channel == channel)
            {
                timer = c.timer;
            }
        }
        timers.push_back(timer);
    }
    return timers;
}

// A note at full volume plays at 127 with the divider of 1 (divider 0): 4095 * 255 / 256 is 4079, over 2047. Its timer
// is 0x7D3 at pitch 0. An 8-bit sample that plays once gives its length in words. Channel 0 pans to 74.
void TestNoteStart()
{
    const uv::Song song = SongOf(4, 2, {{0, {{.value = 0x50, .note = 48, .instrument = 1, .rows = 0}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    const std::vector<uv::Command> commands = player.Update();

    const uv::Command setup{.kind = Kind::kSetup,
                            .channel = 0,
                            .format = 0,
                            .loop_mode = 2,
                            .loop_start = 0,
                            .length = 4,
                            .volume = 127,
                            .divider = 0,
                            .timer = 0x7D3,
                            .pan = 74,
                            .sample = 0};
    XSFLIB_CHECK(commands.size() == 2);
    XSFLIB_CHECK(commands.size() == 2 && commands[0] == setup);
    XSFLIB_CHECK(commands.size() == 2 && commands[1] == (uv::Command{.kind = Kind::kStart, .channel = 0}));
    XSFLIB_CHECK(TimerAt(0) == 0x7D3 && TimerAt(-768) == 0x7D3 * 2 && TimerAt(768) == (0x7D3 >> 1));
}

// A looped 16-bit sample gives its loop in words. Track 1 plays on channel 1 at pan 54. A volume column of 0x30 is
// volume 2048. 2048 * 255 / 256 is 2040, under 2048: the channel's volume is 2040 / 16 = 127 with the divider of 2
// (divider 1). Note 49 with relative note 0 is pitch 64.
void TestLoopedSampleAndVolumeColumn()
{
    const uv::Song song = SongOf(4, 2, {{0, {}}, {0, {{.value = 0x30, .note = 49, .instrument = 2, .rows = 0}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    const std::vector<uv::Command> commands = player.Update();

    XSFLIB_CHECK(commands.size() == 2);
    if (commands.size() == 2)
    {
        const uv::Command& c = commands[0];
        XSFLIB_CHECK(c.kind == Kind::kSetup && c.channel == 1 && c.format == 1 && c.loop_mode == 1);
        XSFLIB_CHECK(c.loop_start == 2 && c.length == 12 && c.volume == 127 && c.divider == 1);
        XSFLIB_CHECK(c.timer == TimerAt(64) && c.pan == 54 && c.sample == 1);
    }
}

// The hardware volume's four ranges and the level that they give.
void TestHardwareVolume()
{
    XSFLIB_CHECK(uv::HardwareVolumeOf(254, 256).volume == 127 && uv::HardwareVolumeOf(254, 256).divider == 3);
    XSFLIB_CHECK(uv::HardwareVolumeOf(256, 256).volume == 32 && uv::HardwareVolumeOf(256, 256).divider == 2);
    XSFLIB_CHECK(uv::HardwareVolumeOf(1024, 256).volume == 64 && uv::HardwareVolumeOf(1024, 256).divider == 1);
    XSFLIB_CHECK(uv::HardwareVolumeOf(4095, 256).volume == 127 && uv::HardwareVolumeOf(4095, 256).divider == 0);
    XSFLIB_CHECK(uv::LevelOf(64, 1) == 0.25 && uv::LevelOf(127, 3) == 127.0 / 2048.0);
}

// A song that doesn't loop stops on the tick that reads its last row, and every channel goes to volume 0. With speed
// 2 and 2 rows, that's the third update. A looped song goes back to its restart order there instead.
void TestEndAndLoop()
{
    const uv::Song song = SongOf(2, 2, {{0, {}}, {0, {}}});
    const uv::SampleBank bank = Bank();
    uv::Player once(song, bank, false);
    uv::Player looped(song, bank, true);

    once.Update();
    once.Update();
    const std::vector<uv::Command> end = once.Update();
    for (int i = 0; i < 3; i++)
    {
        looped.Update();
    }

    XSFLIB_CHECK(!once.Playing() && end.size() == 2);
    XSFLIB_CHECK(end.size() == 2 && end[0] == (uv::Command{.kind = Kind::kVolume, .channel = 0}) &&
                 end[1] == (uv::Command{.kind = Kind::kVolume, .channel = 1}));
    XSFLIB_CHECK(once.Update().empty());
    XSFLIB_CHECK(looped.Playing() && looped.Wraps() == 1 && looped.Order() == 0 && looped.Row() == 0);
}

// A slide up of 5 at speed 4 moves 4 * 16 * 5 / 5 = 64 over the row: f = 64, 128 and 192 on ticks 0 to 2 give 16,
// 32 and 48 (from 0, as (64 * f) >> 8), and the last tick gives 64.
void TestPitchSlide()
{
    const uv::Song song = SongOf(
        4, 2, {{0, {{.value = 0x50, .note = 48, .instrument = 1, .rows = 1}, {.value = 5, .note = 0x82, .rows = 1}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    const std::vector<uint16_t> timers = Timers(player, 8);

    const std::vector<uint16_t> expected = {0, 0, 0, 0, TimerAt(16), TimerAt(32), TimerAt(48), TimerAt(64)};
    XSFLIB_CHECK(timers == expected);
}

// A vibrato of speed 8 and depth 4 from position 0: table entries 16, 24, 0 and 8, then the position wraps from 32 to
// -32 and entry 16 comes back negative. 255 * 4 >> 6 = 15 and 181 * 4 >> 6 = 11. At an offset of 0 the tick sends the
// note's pitch. That is a change from the tick before.
void TestVibrato()
{
    const uv::Song song = SongOf(
        6, 2,
        {{0, {{.value = 0x50, .note = 48, .instrument = 1, .rows = 0}, {.value = 0x84, .note = 0x86, .rows = 1}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    const std::vector<uint16_t> timers = Timers(player, 5);

    const std::vector<uint16_t> expected = {TimerAt(15), TimerAt(11), TimerAt(0), TimerAt(11), TimerAt(-15)};
    XSFLIB_CHECK(timers == expected);
}

// A volume slide of 0x20 adds 2 * 64 a tick, tick 0 included: 2048 + 128 = 2176, then 2304. A slide of 3 takes 3 * 64.
void TestVolumeSlide()
{
    const uv::Song song = SongOf(
        2, 2,
        {{0, {{.value = 0x30, .note = 48, .instrument = 1, .rows = 1}, {.value = 0x20, .note = 0x85, .rows = 1}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    player.Update();
    player.Update();
    const std::vector<uv::Command> first = player.Update();
    const std::vector<uv::Command> second = player.Update();

    const uv::HardwareVolume a = uv::HardwareVolumeOf(2176, uv::kFullSongVolume);
    const uv::HardwareVolume b = uv::HardwareVolumeOf(2304, uv::kFullSongVolume);
    XSFLIB_CHECK(first.size() == 1 &&
                 first[0] ==
                     (uv::Command{.kind = Kind::kVolume, .channel = 0, .volume = a.volume, .divider = a.divider}));
    XSFLIB_CHECK(second.size() == 1 &&
                 second[0] ==
                     (uv::Command{.kind = Kind::kVolume, .channel = 0, .volume = b.volume, .divider = b.divider}));
}

// A note delay of 0xD2 holds the note for 2 ticks: it starts on the third update.
void TestNoteDelay()
{
    const uv::Song song = SongOf(
        4, 2,
        {{0, {{.value = 0x50, .note = 48, .instrument = 1, .rows = 0}, {.value = 0xD2, .note = 0x88, .rows = 1}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    const bool first = player.Update().empty();
    const bool second = player.Update().empty();
    const std::vector<uv::Command> third = player.Update();

    XSFLIB_CHECK(first && second && third.size() == 2 && third[0].kind == Kind::kSetup);
}

// A note with tone portamento on its row doesn't start the sample again: the update sends only its volume. The pitch
// then moves toward the new note by 4 * 16 * 2 / 5 = 25 over the row, and stops at the note: f = 64 on tick 0 gives
// (25 * 64) >> 8 = 6.
void TestTonePortamento()
{
    const uv::Song song = SongOf(4, 2,
                                 {{0,
                                   {{.value = 0x50, .note = 48, .instrument = 1, .rows = 1},
                                    {.value = 0x50, .note = 49, .rows = 0},
                                    {.value = 2, .note = 0x84, .rows = 1}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    for (int i = 0; i < 4; i++)
    {
        player.Update();
    }
    const std::vector<uv::Command> commands = player.Update();

    XSFLIB_CHECK(commands.size() == 2 && commands[0].kind == Kind::kVolume && commands[1].kind == Kind::kTimer);
    XSFLIB_CHECK(commands.size() == 2 && commands[1].timer == TimerAt(6));
}

// Effect 9 sets the speed: from 4 to 2, the next row comes 2 ticks after the one that set it.
void TestSpeed()
{
    const uv::Song song = SongOf(4, 4, {{0, {{.value = 2, .note = 0x89, .rows = 1}}}});
    const uv::SampleBank bank = Bank();
    uv::Player player(song, bank);

    player.Update();
    player.Update();
    player.Update();

    XSFLIB_CHECK(player.Speed() == 2 && player.Row() == 2 && player.Tick() == 1);
}

// The player refuses an instrument without a sample, more tracks than channels, and an event's missing instrument.
void TestRefusals()
{
    uv::Song song = SongOf(4, 2, {{0, {{.value = 0x50, .note = 48, .instrument = 3, .rows = 0}}}});
    const uv::SampleBank bank = Bank();
    uv::Song bad_sample = song;
    bad_sample.instruments[1].sample = 2;
    uv::Song too_many = SongOf(4, 2, std::vector<uv::Track>(17));

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { uv::Player player(bad_sample, bank); }) ==
                 "an instrument names a missing sample (2)");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { uv::Player player(too_many, bank); }) ==
                 "the song has more tracks than the sound unit has channels");
    uv::Player player(song, bank);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { player.Update(); }) == "an event names a missing instrument (3)");
}

} // namespace

int main()
{
    TestNoteStart();
    TestLoopedSampleAndVolumeColumn();
    TestHardwareVolume();
    TestEndAndLoop();
    TestPitchSlide();
    TestVibrato();
    TestVolumeSlide();
    TestNoteDelay();
    TestTonePortamento();
    TestSpeed();
    TestRefusals();

    return xsflib::testing::Summary("Universomo's player");
}
