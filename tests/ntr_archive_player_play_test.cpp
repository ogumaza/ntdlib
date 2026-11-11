// SPDX-License-Identifier: MIT

// Tests of the DS archive player's notes, mutes, meters, frames, play, snapshots and enhanced output.
// ntr_archive_player_test.cpp says how the tests are built.

#include "ntr_archive_player_test_util.h"

namespace ntr_archive_player_tests
{
namespace
{

// The note events of a sequence with two tracks. The offsets count from the start of the sequence's data. So do the
// decoder's offsets. Track 2 is the driver's second track: the events give the track's number in the player. At tick
// 48, track 0's note comes before track 2's: the driver steps a player's tracks in order.
void TestNotes()
{
    const Bytes sequence = {
        0xFE, 0x05, 0x00,             // 0: tracks 0 and 2
        0x93, 0x02, 0x15, 0x00, 0x00, // 3: track 2 starts at 0x15
        0xC3, 0x02,                   // 8: a transpose of 2
        0x3C, 0x7F, 0x30,             // 10: key 60 (62), 48 ticks
        0xC7, 0x00,                   // 13: no note wait
        0x3E, 0x64, 0x00,             // 15: key 62 (64) at velocity 100, until the track releases it
        0x80, 0x30,                   // 18: a wait of 48 ticks
        0xFF,                         // 20
        0xC8, 0x01,                   // 21: tie
        0x40, 0x50, 0x18,             // 23: key 64 at velocity 80, 24 ticks
        0x80, 0x18,                   // 26: a wait of 24 ticks
        0x43, 0x60, 0x18,             // 28: key 67 at velocity 96, on the channel of key 64
        0x80, 0x30,                   // 31: a wait of 48 ticks
        0xFF,                         // 33
    };
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    std::vector<snd::NoteEvent> notes;
    player.on_note = [&notes](const snd::NoteEvent& note)
    {
        notes.push_back(note);
    };
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < 200; frame++)
    {
        player.RunFrame(out);
    }

    XSFLIB_CHECK(notes.size() == 4);
    if (notes.size() != 4)
    {
        return;
    }
    // Player 1 of the archive gives the tracks channels 4 to 11.
    const int first = notes[0].channel;
    const int second = notes[1].channel;
    XSFLIB_CHECK(first >= 4 && first <= 11 && second >= 4 && second <= 11 && first != second);
    XSFLIB_CHECK(notes[0] == (snd::NoteEvent{.player = 0,
                                             .track = 0,
                                             .offset = 10,
                                             .key = 62,
                                             .velocity = 127,
                                             .length = 48,
                                             .channel = first,
                                             .tie = false}));
    XSFLIB_CHECK(notes[1] == (snd::NoteEvent{.player = 0,
                                             .track = 2,
                                             .offset = 23,
                                             .key = 64,
                                             .velocity = 80,
                                             .length = 24,
                                             .channel = second,
                                             .tie = false}));
    XSFLIB_CHECK(notes[3] == (snd::NoteEvent{.player = 0,
                                             .track = 2,
                                             .offset = 28,
                                             .key = 67,
                                             .velocity = 96,
                                             .length = 24,
                                             .channel = second,
                                             .tie = true}));
    XSFLIB_CHECK(notes[2].channel >= 4 && notes[2].channel <= 11);
    XSFLIB_CHECK(notes[2] == (snd::NoteEvent{.player = 0,
                                             .track = 0,
                                             .offset = 15,
                                             .key = 64,
                                             .velocity = 100,
                                             .length = -1,
                                             .channel = notes[2].channel,
                                             .tie = false}));
    for (const snd::NoteEvent& note : notes)
    {
        const std::optional<snd::DecodedCommand> command = snd::DecodeCommand(sequence, note.offset);
        XSFLIB_CHECK(command && command->command.kind == ntdlib::commands::Kind::kNote);
    }
}

// Plays `file`'s SEQ_A to its end, at most 1,000 frames, after `set_up` has set the player up.
template <typename SetUp>
std::vector<ntr::SoundOutput> PlayToEnd(const Bytes& file, const SetUp& set_up)
{
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    set_up(player);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    int frames = 0;
    do
    {
        player.RunFrame(out);
        frames++;
    } while (player.Busy() && frames < 1000);
    return out;
}

bool Silent(std::span<const ntr::SoundOutput> samples)
{
    return std::ranges::all_of(samples,
                               [](const ntr::SoundOutput& sample) { return sample == ntr::SoundOutput{0x200, 0x200}; });
}

void TestMutes()
{
    // A muted track plays as before, release included, and nothing of it reaches the output. The tie keeps the
    // channel. The release comes after the track has ended and let the channel go.
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const std::vector<ntr::SoundOutput> played = PlayToEnd(file, [](ntr::ArchivePlayer&) {});
    const std::vector<ntr::SoundOutput> muted =
        PlayToEnd(file, [](ntr::ArchivePlayer& player) { player.MuteTrack(0, 0, true); });
    XSFLIB_CHECK(!Silent(played));
    XSFLIB_CHECK(muted.size() == played.size() && Silent(muted));

    // The mutes of other tracks and players change nothing.
    XSFLIB_CHECK(PlayToEnd(file,
                           [](ntr::ArchivePlayer& player)
                           {
                               player.MuteTrack(0, 1, true);
                               player.MuteTrack(1, 0, true);
                           }) == played);

    // A mute names the driver's player too.
    const sdat::Archive two_archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer two(two_archive);
    two.Start({0});
    two.Start({0}, {}, 1);
    const auto frame_of = [&two]
    {
        std::vector<ntr::SoundOutput> samples;
        two.RunFrame(samples);
        return samples;
    };
    for (int frame = 0; frame < 4; frame++)
    {
        two.RunFrame();
    }
    two.MuteTrack(0, 0, true);
    XSFLIB_CHECK(!Silent(frame_of()));
    two.MuteTrack(1, 0, true);
    XSFLIB_CHECK(Silent(frame_of()));
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { two.Start({0}, {}, 16); }) == "no player 16");

    // A change takes effect at the next frame.
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    int channel = -1;
    player.on_note = [&channel](const snd::NoteEvent& note)
    {
        channel = note.channel;
    };
    player.MuteTrack(0, 0, true);
    XSFLIB_CHECK(player.IsTrackMuted(0, 0) && !player.IsTrackMuted(0, 1) && !player.IsTrackMuted(1, 0));
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(out);
    }
    const std::size_t unmuted = out.size();
    player.MuteTrack(0, 0, false);
    XSFLIB_CHECK(!player.IsTrackMuted(0, 0));
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(out);
    }
    XSFLIB_CHECK(Silent(std::span(out).first(unmuted)));
    XSFLIB_CHECK(std::equal(out.begin() + static_cast<std::ptrdiff_t>(unmuted), out.end(),
                            played.begin() + static_cast<std::ptrdiff_t>(unmuted)));

    // A channel that the ARM9 locks and sets up plays no track's note. The mute of the track whose note it played
    // no longer reaches it. The mutes stay through a restore, and the channels go back to their tracks then.
    XSFLIB_CHECK(channel >= 4 && channel <= 11);
    if (channel < 0)
    {
        return;
    }
    player.MuteTrack(0, 0, true);
    const ntr::ArchivePlayer::Snapshot snapshot = player.Save(nullptr);
    const auto bit = 1u << channel;
    player.Send({{snd::CommandId::kLockChannels, {bit, 0, 0, 0}},
                 {snd::CommandId::kSetUpPcmChannel,
                  {static_cast<uint32_t>(channel) | 0x400u << 16, ntr::ArchivePlayer::kFiles, 127u << 24 | 0x100,
                   1u << 26 | 64u << 16}},
                 {snd::CommandId::kStartTimer, {bit, 0, 0, 0}}});
    player.RunFrame(out);
    std::vector<ntr::SoundOutput> set_up;
    player.RunFrame(set_up);
    XSFLIB_CHECK(!Silent(set_up));
    player.MuteTrack(0, 0, false);
    player.Restore(snapshot);
    XSFLIB_CHECK(!player.IsTrackMuted(0, 0));
    player.MuteTrack(0, 0, true);
    std::vector<ntr::SoundOutput> restored;
    player.RunFrame(restored);
    XSFLIB_CHECK(Silent(restored));
    player.MuteTrack(0, 0, false);
    restored.clear();
    player.RunFrame(restored);
    XSFLIB_CHECK(!Silent(restored));

    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { player.MuteTrack(0, 16, true); }) == "no track 16");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { player.IsTrackMuted(0, -1); }) == "no track -1");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { player.MuteTrack(16, 0, true); }) == "no player 16");
}

constexpr std::array<int32_t, 2> kNoPeaks = {};

// The largest magnitude of a side of `samples` on the scale of 16-bit samples.
std::array<int32_t, 2> OutputPeaks(std::span<const ntr::SoundOutput> samples)
{
    std::array<int32_t, 2> peaks{};
    for (const ntr::SoundOutput& sample : samples)
    {
        peaks[0] = std::max(peaks[0], std::abs((sample.left - 0x200) * 64));
        peaks[1] = std::max(peaks[1], std::abs((sample.right - 0x200) * 64));
    }
    return peaks;
}

void TestMeters()
{
    // A track's meter follows the output of its channels, before the cut to 10 bits. With one track, each frame's
    // peaks lie within 64 of the output's. The meters change nothing in the output.
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.MeterTracks(0);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    bool sounded = false;
    bool near = true;
    bool others = true;
    for (int frame = 0; frame < 400; frame++)
    {
        const std::size_t start = out.size();
        player.RunFrame(out);
        const std::array<int32_t, 2> peaks = player.TrackPeaks(0);
        const std::array<int32_t, 2> expected = OutputPeaks(std::span(out).subspan(start));
        sounded = sounded || peaks[0] > 0;
        near = near && std::abs(peaks[0] - expected[0]) < 64 && std::abs(peaks[1] - expected[1]) < 64;
        others = others && player.TrackPeaks(1) == kNoPeaks && player.TrackPeaks(15) == kNoPeaks;
    }
    XSFLIB_CHECK(sounded && near && others);
    const std::vector<ntr::SoundOutput> played = PlayToEnd(file, [](ntr::ArchivePlayer&) {});
    XSFLIB_CHECK(std::equal(out.begin(), out.end(), played.begin()));

    // The peaks stay through a restore. A muted track's meter, another player's meters and no meters give 0, and so
    // does a frame without mixing.
    const std::array<int32_t, 2> peaks = player.TrackPeaks(0);
    const ntr::ArchivePlayer::Snapshot snapshot = player.Save(nullptr);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.TrackPeaks(0) == peaks);

    ntr::ArchivePlayer other(archive);
    other.Start({0});
    const auto peaks_after = [&other](int frames)
    {
        std::vector<ntr::SoundOutput> samples;
        for (int frame = 0; frame < frames; frame++)
        {
            other.RunFrame(samples);
        }
        return other.TrackPeaks(0);
    };
    other.MeterTracks(0);
    XSFLIB_CHECK(peaks_after(20) != kNoPeaks);
    other.MuteTrack(0, 0, true);
    XSFLIB_CHECK(peaks_after(1) == kNoPeaks);
    other.MuteTrack(0, 0, false);
    XSFLIB_CHECK(peaks_after(1) != kNoPeaks);
    other.RunFrame();
    XSFLIB_CHECK(other.TrackPeaks(0) == kNoPeaks);
    XSFLIB_CHECK(peaks_after(1) != kNoPeaks);
    other.MeterTracks(1);
    XSFLIB_CHECK(peaks_after(1) == kNoPeaks);
    other.MeterTracks(-1);
    XSFLIB_CHECK(peaks_after(1) == kNoPeaks);

    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { other.MeterTracks(16); }) == "no player 16");
    XSFLIB_CHECK(ErrorOf<std::out_of_range>([&] { other.TrackPeaks(16); }) == "no track 16");
}

// Frames without mixing leave the player where mixed frames do.
void TestFramesWithoutMixing()
{
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer mixed(archive);
    ntr::ArchivePlayer skipped(archive);
    mixed.Start({0});
    skipped.Start({0});
    std::vector<ntr::SoundOutput> lead;
    for (int frame = 0; frame < 150; frame++)
    {
        mixed.RunFrame(lead);
        skipped.RunFrame();
    }
    XSFLIB_CHECK(mixed.Clock() == skipped.Clock());
    XSFLIB_CHECK(std::ranges::equal(mixed.MainMemory(), skipped.MainMemory()));
    std::vector<ntr::SoundOutput> after_mixed;
    std::vector<ntr::SoundOutput> after_skipped;
    for (int frame = 0; frame < 100; frame++)
    {
        mixed.RunFrame(after_mixed);
        skipped.RunFrame(after_skipped);
    }
    XSFLIB_CHECK(!Silent(after_mixed) && after_mixed == after_skipped);
}

// Track 1 waits 0 ticks and jumps back, without end. In its first tick, after kCommandBudget commands, the frame throws
// at the wait, at offset 0x0B.
void TestHang()
{
    const Bytes sequence = {
        0xFE, 0x03, 0x00,             // 0: tracks 0 and 1
        0x93, 0x01, 0x0B, 0x00, 0x00, // 3: track 1 starts at 0x0B
        0x80, 0x30,                   // 8: a wait of 48 ticks
        0xFF,                         // 10
        0x80, 0x00,                   // 11: a wait of 0 ticks
        0x94, 0x0B, 0x00, 0x00,       // 13: back to 11
    };
    const Bytes file = ArchiveFile(sequence);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0}, {}, 2);

    std::optional<snd::Hang> hang;
    std::vector<ntr::SoundOutput> out;
    try
    {
        for (int frame = 0; frame < 4; frame++)
        {
            player.RunFrame(out);
        }
    }
    catch (const snd::Hang& e)
    {
        hang = e;
    }

    XSFLIB_CHECK(hang && hang->PlayerIndex() == 2 && hang->TrackIndex() == 1 && hang->Offset() == 0x0B);
    XSFLIB_CHECK(hang &&
                 std::string(hang->what()) ==
                     "track 1 of player 2 runs 100000 commands in a tick without waiting, up to offset 0xB: the "
                     "sequence hangs");
}

void TestPlay()
{
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);

    // The shared work that the ARM9's SND_Init sets up.
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x20) == 0xFFFF);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x260) == 0xFFFF);
    XSFLIB_CHECK(!player.Busy());

    // At 120 beats a minute and 48 ticks a beat, the driver steps half a tick a frame. The note and the wait take 192
    // frames, about 1 s.
    ntr::Variables variables;
    variables.global[2] = 7;
    player.Start({0}, {.variables = variables});
    XSFLIB_CHECK(player.Busy());
    uint32_t writes = 0;
    player.on_sound_access = [&writes](const ntr::RegisterAccess& access)
    {
        writes += access.write ? 1 : 0;
    };
    std::vector<ntr::SoundOutput> out;
    int frames = 0;
    do
    {
        player.RunFrame(out);
        frames++;
    } while (player.Busy() && frames < 1000);
    XSFLIB_CHECK(frames >= 192 && frames < 200);
    XSFLIB_CHECK(writes > 0);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x264) == 7);
    XSFLIB_CHECK(player.Clock() == static_cast<uint64_t>(frames) * snd::kFrameClocks);

    // A frame holds 170 or 171 samples: 174,592 clocks over 1,024 a sample.
    XSFLIB_CHECK(out.size() == static_cast<uint64_t>(frames) * snd::kFrameClocks / 1024);
    bool sounded = false;
    for (const ntr::SoundOutput& sample : out)
    {
        sounded = sounded || sample.left != 0x200 || sample.right != 0x200;
    }
    XSFLIB_CHECK(sounded);
}

// The player stays busy while a channel releases after the sequence has ended. The sequence ends as its note does,
// after 96 frames. Release 100 lowers the envelope by 295 a frame, in units of 1/128 of 0.1 dB. The driver stops the
// channel at -72.3 dB, with the sequence's level of -2.1 dB in the sum: (723 - 21) * 128 / 295 gives 305 frames.
void TestBusyWhileReleasing()
{
    files::Archive spec = ArchiveSpec();
    spec.files[0] = files::Sequence({0x3C, 0x7F, 0x30, 0xFF});
    spec.files[2] = files::Bank({{.type = 1, .region = {.wave = 0, .release = 100}}});
    const Bytes file = files::SoundArchive(spec);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    int frames = 0;
    int ended = 0;
    do
    {
        player.RunFrame(out);
        frames++;
        if (ended == 0 && !player.Driver().GetPlayer(0).active)
        {
            ended = frames;
        }
    } while (player.Busy() && frames < 1000);
    XSFLIB_CHECK(ended >= 96 && ended < 100);
    XSFLIB_CHECK(frames >= ended + 304 && frames <= ended + 306);
}

void TestSnapshots()
{
    const Bytes file = ArchiveFile();
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer player(archive);
    const ntr::ArchivePlayer::Snapshot unstarted = player.Save(nullptr);
    player.Start({0});
    std::vector<ntr::SoundOutput> out;
    for (int frame = 0; frame < 10; frame++)
    {
        player.RunFrame(out);
    }

    // The frames after a restore play as they did after the save, with a snapshot that shares pages with an earlier
    // one too.
    const ntr::ArchivePlayer::Snapshot first = player.Save(nullptr);
    player.RunFrame(out);
    const ntr::ArchivePlayer::Snapshot second = player.Save(&first);
    const std::vector<uint8_t> saved(player.MainMemory().begin(), player.MainMemory().end());
    std::vector<ntr::SoundOutput> played;
    for (int frame = 0; frame < 100; frame++)
    {
        player.RunFrame(played);
    }
    const uint64_t clock = player.Clock();

    player.Restore(second);
    XSFLIB_CHECK(std::equal(saved.begin(), saved.end(), player.MainMemory().begin()));
    std::vector<ntr::SoundOutput> again;
    for (int frame = 0; frame < 100; frame++)
    {
        player.RunFrame(again);
    }
    XSFLIB_CHECK(again == played && player.Clock() == clock);

    // A start sent before the save is taken after the restore.
    player.Restore(first);
    player.Send({{snd::CommandId::kGlobalVariable, {0, 9, 0, 0}}});
    const ntr::ArchivePlayer::Snapshot pending = player.Save(nullptr);
    player.RunFrame(out);
    player.Restore(pending);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x260) == 0xFFFF);
    player.RunFrame(out);
    XSFLIB_CHECK(Word16(player.MainMemory(), ntr::ArchivePlayer::kSharedWork + 0x260) == 9);

    // A restore takes the files back out of the heap. A start after it loads them where a new player does.
    player.Restore(unstarted);
    player.Start({0, 0});
    player.RunFrame(out);
    ntr::ArchivePlayer other(archive);
    other.Start({0, 0});
    std::vector<ntr::SoundOutput> other_out;
    other.RunFrame(other_out);
    XSFLIB_CHECK(std::ranges::equal(player.MainMemory(), other.MainMemory()));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { other.Restore(first); }) == "the snapshot is another player's");
}

void TestEnhanced()
{
    // The enhanced mode gives kEnhancedFrameSamples samples a frame in floating point. Its level follows the console's
    // within 0.5 dB on this wave at 32,728 Hz. A muted track gives 0, the meters follow the track, and a restore plays
    // the same samples again. A frame of the other mode's output is refused.
    const Bytes file = ArchiveFile(kTiedNotes, 100);
    const sdat::Archive archive = sdat::Archive::Parse(file);
    ntr::ArchivePlayer console(archive);
    ntr::ArchivePlayerOptions options;
    options.enhanced = true;
    ntr::ArchivePlayer player(archive, options);
    player.MeterTracks(0);
    console.Start({0});
    player.Start({0});
    std::vector<ntr::SoundOutput> console_out;
    std::vector<float> out;
    double console_sum = 0.0;
    double sum = 0.0;
    bool metered = true;
    for (int frame = 0; frame < 100; frame++)
    {
        const std::size_t start = out.size();
        console.RunFrame(console_out);
        player.RunFrame(out);
        XSFLIB_CHECK(out.size() - start == 2 * ntr::kEnhancedFrameSamples);
        float peak = 0.0f;
        for (std::size_t i = start; i < out.size(); i += 2)
        {
            peak = std::max(peak, std::abs(out[i]));
        }
        const int32_t meter = player.TrackPeaks(0)[0];
        metered = metered && (frame < 2 || std::abs(meter - peak * 32768.0f) < 0.05f * peak * 32768.0f + 2.0f);
    }
    for (const ntr::SoundOutput& sample : console_out)
    {
        const double value = (sample.left - 0x200) * 64 / 32768.0;
        console_sum += value * value;
    }
    for (std::size_t i = 0; i < out.size(); i += 2)
    {
        sum += static_cast<double>(out[i]) * out[i];
    }
    const double console_rms = std::sqrt(console_sum / static_cast<double>(console_out.size()));
    const double rms = std::sqrt(sum / static_cast<double>(out.size() / 2));
    XSFLIB_CHECK(rms > 0.01);
    XSFLIB_CHECK(std::abs(20.0 * std::log10(rms / console_rms)) < 0.5);
    XSFLIB_CHECK(metered);
    XSFLIB_CHECK(player.TrackPeaks(1) == kNoPeaks);

    const ntr::ArchivePlayer::Snapshot snapshot = player.Save(nullptr);
    std::vector<float> played;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(played);
    }
    player.Restore(snapshot);
    std::vector<float> again;
    for (int frame = 0; frame < 20; frame++)
    {
        player.RunFrame(again);
    }
    XSFLIB_CHECK(again == played);

    ntr::ArchivePlayer muted(archive, options);
    muted.MuteTrack(0, 0, true);
    muted.Start({0});
    std::vector<float> silence;
    for (int frame = 0; frame < 100; frame++)
    {
        muted.RunFrame(silence);
    }
    XSFLIB_CHECK(std::ranges::all_of(silence, [](float sample) { return sample == 0.0f; }));

    XSFLIB_CHECK(ErrorOf<std::logic_error>([&] { player.RunFrame(console_out); }) ==
                 "the player mixes in the enhanced mode");
    XSFLIB_CHECK(ErrorOf<std::logic_error>([&] { console.RunFrame(out); }) ==
                 "the player doesn't mix in the enhanced mode");
}

} // namespace

int RunTests()
{
    TestPlay();
    TestBusyWhileReleasing();
    TestSnapshots();
    TestEnhanced();
    TestNotes();
    TestMutes();
    TestMeters();
    TestFramesWithoutMixing();
    TestHang();

    return xsflib::testing::Summary("DS archive player play");
}

} // namespace ntr_archive_player_tests

int main()
{
    return ntr_archive_player_tests::RunTests();
}
