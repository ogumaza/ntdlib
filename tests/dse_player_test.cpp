// SPDX-License-Identifier: MIT

// A test of the player of DSE's songs (ntdlib/dse/player.h), on the files of dse_song_files.h. The channels' status
// that the player gives DSE's driver at each tick matches the status that the driver's code read in the game
// (dse_driver_test_lists.h): the sound unit ends the waves without a loop as the console does. The commands therefore
// match the game's too.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "dse_driver_test_lists.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/testing/dse_song_files.h"

namespace
{

namespace dse_song_files = ntdlib::testing::dse_song_files;
namespace dse = ntdlib::dse;
namespace snd = ntdlib::ntr::snd;
using dse_song_files::BankFile;
using dse_song_files::kBankId;
using dse_song_files::MainBankFile;
using dse_song_files::SongFile;
using xsflib::testing::ErrorOf;

// The bytes of main memory at `address`.
std::span<const uint8_t> At(const dse::Player& player, uint32_t address, std::size_t size)
{
    return player.MainMemory().subspan(address - dse::Player::kMainMemory, size);
}

// A bank that holds its samples keeps them at the first free place. The waves of a bank that lie in the main bank go
// after them, in the order of the bank's slots, each with (its loop's start + its loop's length) * 4 bytes from its
// place in the main bank's samples.
void TestSamples()
{
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    const std::vector<uint8_t>& pcmd = *main_bank.samples;
    dse::Player player;
    XSFLIB_CHECK(player.AddBank(main_bank) == 0);
    XSFLIB_CHECK(player.AddBank(bank) == kBankId);
    XSFLIB_CHECK(!player.AddBank(bank));
    player.PlaceWaves(kBankId, main_bank);

    const std::span<const uint8_t> samples = std::span(pcmd);
    XSFLIB_CHECK(std::ranges::equal(At(player, dse::Player::kSamples, pcmd.size()), samples));
    const uint32_t waves = dse::Player::kSamples + 0x400;
    XSFLIB_CHECK(std::ranges::equal(At(player, waves, 0x100), samples.subspan(0x000, 0x100)));
    XSFLIB_CHECK(std::ranges::equal(At(player, waves + 0x100, 0x80), samples.subspan(0x100, 0x80)));
    XSFLIB_CHECK(std::ranges::equal(At(player, waves + 0x180, 0x80), samples.subspan(0x180, 0x80)));
    XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { player.PlaceWaves(0x1234, main_bank); }) ==
                 "no bank of that id is loaded");
}

// The player's status at each tick matches the game's, and so does each block's hash of the commands.
void TestStatus()
{
    namespace data = dse_driver_test;
    const dse::Song song = dse::Song::Parse(SongFile());
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Player player;
    player.AddBank(bank);
    player.PlaceWaves(kBankId, main_bank);
    player.Driver().PlaySong(song);

    std::size_t change = 0;
    uint16_t expected = 0;
    int differences = 0;
    std::optional<int> first;
    const int ticks = static_cast<int>(std::size(data::kBlockHashes)) * data::kBlockTicks;
    for (int tick = 0; tick < ticks; tick++)
    {
        while (change < std::size(data::kStatus) && data::kStatus[change].tick <= tick)
        {
            expected = data::kStatus[change++].status;
        }
        player.RunTick();
        if (player.LastStatus() != expected)
        {
            differences++;
            first = first ? first : tick;
        }
    }
    xsflib::testing::Check(differences == 0, "the status of " + std::to_string(differences) + " ticks, from tick " +
                                                 std::to_string(first.value_or(-1)));
}

// A snapshot plays on as the player did, and the player then plays on from where it was.
void TestSnapshot()
{
    const dse::Song song = dse::Song::Parse(SongFile());
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Player player;
    player.AddBank(bank);
    player.PlaceWaves(kBankId, main_bank);
    player.Driver().PlaySong(song);
    for (int i = 0; i < 150; i++)
    {
        player.RunTick();
    }

    const dse::Player::Snapshot snapshot = player.Save();
    std::vector<ntdlib::ntr::SoundOutput> first;
    for (int i = 0; i < 200; i++)
    {
        player.RunTick(first);
    }
    const uint64_t clock = player.Clock();
    player.Restore(snapshot);
    std::vector<ntdlib::ntr::SoundOutput> second;
    for (int i = 0; i < 200; i++)
    {
        player.RunTick(second);
    }
    XSFLIB_CHECK(first == second);
    XSFLIB_CHECK(player.Clock() == clock);

    // The ticks take 335,104 system clocks, and an output sample 1,024.
    XSFLIB_CHECK(first.size() == 200 * 335104 / 1024 || first.size() == 200 * 335104 / 1024 + 1);
    bool sounds = false;
    for (const ntdlib::ntr::SoundOutput& sample : first)
    {
        sounds = sounds || sample.left != 0x200 || sample.right != 0x200;
    }
    XSFLIB_CHECK(sounds);

    dse::Player other;
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { other.Restore(snapshot); }) ==
                 "the snapshot is another player's");
}

// SongFile's tracks, each on channel 4 more than its slot: track 1 on channel 5, and so on.
std::vector<dse_song_files::Bytes> ShiftedTracks()
{
    std::vector<dse_song_files::Bytes> tracks = dse_song_files::Tracks();
    for (dse_song_files::Bytes& track : tracks)
    {
        track[1] = static_cast<uint8_t>(track[1] + 4);
    }
    return tracks;
}

// A player whose banks are loaded, playing `song` from its start.
struct SongPlayer
{
    explicit SongPlayer(const dse::PlayerOptions& options = {}, const dse_song_files::Bytes& file = SongFile())
        : song(dse::Song::Parse(file)), player(options)
    {
        player.AddBank(bank);
        player.PlaceWaves(kBankId, main_bank);
        player.Start({.song = &song});
    }

    const dse::Song song;
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    dse::Player player;
};

// Whether a sample plays something: the sound unit's output is 0x200 at rest.
bool Sounds(const ntdlib::ntr::SoundOutput& sample)
{
    return sample.left != 0x200 || sample.right != 0x200;
}

// Runs of any length play the samples that the ticks play, and run the ARM7's frames and DSE's ticks at their clocks.
// A run of two of the ARM7's frames holds 349,184 system clocks: 341 samples, a sample every 1,024 clocks. The
// enhanced mode gives kEnhancedFrameSamples samples a frame.
void TestRuns()
{
    SongPlayer ticks;
    SongPlayer runs;
    XSFLIB_CHECK(!dse::Player().Busy());
    XSFLIB_CHECK(ticks.player.Busy());

    std::vector<ntdlib::ntr::SoundOutput> by_ticks;
    std::vector<ntdlib::ntr::SoundOutput> by_runs;
    for (int i = 0; i < 300; i++)
    {
        ticks.player.RunTick(by_ticks);
    }
    constexpr uint64_t kRunClocks = 2 * snd::kFrameClocks;
    while (runs.player.Clock() + kRunClocks <= ticks.player.Clock())
    {
        const std::size_t before = by_runs.size();
        runs.player.Run(kRunClocks, by_runs);
        XSFLIB_CHECK(by_runs.size() - before == 341);
    }
    runs.player.Run(ticks.player.Clock() - runs.player.Clock(), by_runs);
    XSFLIB_CHECK(by_runs == by_ticks);
    XSFLIB_CHECK(runs.player.NextTick() == ticks.player.NextTick());
    XSFLIB_CHECK(runs.player.LastStatus() == ticks.player.LastStatus());
    XSFLIB_CHECK(std::ranges::any_of(by_runs, Sounds));

    SongPlayer enhanced({.enhanced = true});
    std::vector<float> floats;
    for (int i = 0; i < 100; i++)
    {
        enhanced.player.Run(kRunClocks, floats);
    }
    XSFLIB_CHECK(floats.size() == 100 * 2 * 2 * ntdlib::ntr::kEnhancedFrameSamples);
    XSFLIB_CHECK(std::ranges::any_of(floats, [](float sample) { return sample != 0.0f; }));
    XSFLIB_CHECK(ErrorOf<std::logic_error>([&] { enhanced.player.RunTick(by_ticks); }) ==
                 "the player mixes in the enhanced mode");
    XSFLIB_CHECK(ErrorOf<std::logic_error>([&] { runs.player.Run(kRunClocks, floats); }) ==
                 "the player doesn't mix in the enhanced mode");
}

// A host's release of a held note gives the samples of a note whose length runs out in the same tick, in both builds.
// The note starts in the song's tick 0, in the driver's tick 2. A note of 24 ticks ends in the song's tick 24, which
// runs in the driver's first tick d with 480,000 d > 500,000 * 25: tick 27. The host therefore releases the held note
// after the driver's tick 26. In both songs, a pause of 256 ticks keeps the track going past the release's end.
void TestRelease()
{
    const auto song = [](const dse_song_files::Bytes& note)
    {
        return dse_song_files::SongFile(
            {{0, 0, 0, 0, 0xA4, 0x78, 0x98},
             dse_song_files::Join(
                 {{1, 1, 0, 0}, {0xA9, 0x7F, 0xAA, 0x00, 0xAC, 0x00, 0xE0, 0x7F}, note, {0x93, 0x00, 0x01, 0x98}})});
    };
    const dse::WaveBank bank = dse::WaveBank::Parse(BankFile());
    const dse::WaveBank main_bank = dse::WaveBank::Parse(MainBankFile());
    const dse::Song ending = dse::Song::Parse(song(dse_song_files::Note(0x7F, 0, 0, 0x18, 1)));
    const dse::Song held = dse::Song::Parse(song(dse_song_files::Note(0x7F, 0, 0, 0xFFFFFF, 3)));
    for (const dse::Build build : {dse::Build::kLater, dse::Build::kEarlier})
    {
        const auto run = [&](const dse::Song& played, bool release)
        {
            dse::Player player({.build = build});
            const dse::SequenceFiles files{.song = &played, .banks = {&bank}, .main_bank = &main_bank};
            player.LoadBanks(files);
            player.Start(files);
            std::vector<ntdlib::ntr::SoundOutput> out;
            for (int tick = 1; tick <= 300; tick++)
            {
                player.RunTick(out);
                if (release && tick == 26)
                {
                    player.Driver().ReleaseFollowedNotes();
                }
            }
            return out;
        };
        const std::vector<ntdlib::ntr::SoundOutput> expected = run(ending, false);
        XSFLIB_CHECK(std::ranges::any_of(expected, Sounds));
        XSFLIB_CHECK(run(held, true) == expected);
        XSFLIB_CHECK(run(held, false) != expected);
    }
}

// A muted track's voices reach neither the output nor the meters. SongFile's tracks 1 to 4 play notes from tick 0, here
// on channels 5 to 8. With tracks 1 to 3 muted, track 4's notes still sound. With every track muted, the output rests.
void TestMutes()
{
    SongPlayer song({}, SongFile(ShiftedTracks()));
    song.player.MeterTracks(true);
    std::vector<ntdlib::ntr::SoundOutput> out;
    for (int i = 0; i < 20; i++)
    {
        song.player.RunTick(out);
    }
    for (int track = 1; track <= 4; track++)
    {
        const std::array<int32_t, 2> peaks = song.player.TrackPeaks(track);
        XSFLIB_CHECK(peaks[0] > 0 || peaks[1] > 0);
    }
    XSFLIB_CHECK(song.player.TrackPeaks(0) == (std::array<int32_t, 2>{}));

    for (int track = 1; track <= 3; track++)
    {
        song.player.MuteTrack(track, true);
    }
    XSFLIB_CHECK(song.player.IsTrackMuted(2) && !song.player.IsTrackMuted(4));
    out.clear();
    song.player.RunTick(out);
    XSFLIB_CHECK(std::ranges::any_of(out, Sounds));
    for (int track = 1; track <= 3; track++)
    {
        XSFLIB_CHECK(song.player.TrackPeaks(track) == (std::array<int32_t, 2>{}));
    }
    const std::array<int32_t, 2> fourth = song.player.TrackPeaks(4);
    XSFLIB_CHECK(fourth[0] > 0 || fourth[1] > 0);

    song.player.MuteTrack(4, true);
    out.clear();
    song.player.RunTick(out);
    XSFLIB_CHECK(std::ranges::none_of(out, Sounds));
    XSFLIB_CHECK(song.player.TrackPeaks(4) == (std::array<int32_t, 2>{}));

    // The mutes stay through a restore, and a mute's end lets the voices through again.
    const dse::Player::Snapshot snapshot = song.player.Save();
    song.player.Restore(snapshot);
    out.clear();
    song.player.RunTick(out);
    XSFLIB_CHECK(std::ranges::none_of(out, Sounds));
    song.player.MuteTrack(4, false);
    out.clear();
    song.player.RunTick(out);
    XSFLIB_CHECK(std::ranges::any_of(out, Sounds));

    song.player.MeterTracks(false);
    song.player.RunTick(out);
    XSFLIB_CHECK(song.player.TrackPeaks(4) == (std::array<int32_t, 2>{}));
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { song.player.MuteTrack(dse::Player::kHostTracks, true); }) ==
                 "a host mutes tracks 0 to 31");
}

} // namespace

int main()
{
    TestSamples();
    TestStatus();
    TestSnapshot();
    TestRuns();
    TestRelease();
    TestMutes();
    return xsflib::testing::Summary("dse_player_test");
}
