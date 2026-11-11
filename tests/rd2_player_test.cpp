// SPDX-License-Identifier: MIT

// Tests of rd2::ArchivePlayer on small synthetic cartridges. No game ROM is needed. Each expected value follows a rule
// of the driver's mixer or of the GBA's sound hardware. The comments give the rule. rd2_enhanced_test.cpp tests the
// enhanced mode.
//
// The driver's mix goes to the FIFOs at 1,596 cycles a byte: 176 bytes a frame. The hardware's output at 10 bits is the
// bias of 0x200 plus each FIFO's byte times 4 and the PSG's levels times 16 at the PSG's share, in steps of 4 at the
// driver's resolution. A step of the 10 bits gives 64 in a 16-bit sample.

#include "rd2_player_test_util.h"

namespace
{

// The console's level of a voice for a side's points, in the newer revisions: the pan's part times the volume, in 16
// bits, shifted right by 8.
int LeftLevel(const VoiceMix& mix)
{
    return int((uint32_t((127 - int(mix.pan)) * int(mix.volume & 0xFFFF)) & 0xFFFF) >> 8);
}

// A byte of the newer revisions: the sum over 128, towards zero, saturated.
int ByteOf(int sum)
{
    return std::clamp(sum / 128, -128, 127);
}

void TestOutput()
{
    // Every output sample holds the byte of the last overflow at or before it, times 4 in the 10 bits and 64 in the 16:
    // the first 193 overflows take zeros, and overflow 194 takes the first mix's first byte. FIFO A goes to the left
    // and FIFO B to the right. The sine moves the bytes at each point.
    Cart cart;
    FillSine(cart);
    cart.Sequences({{HeldTrack(0x10), HeldTrack(0x70)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0);
    XSFLIB_CHECK(player.Busy() && player.Frames() == 0);
    XSFLIB_CHECK(player.Rate() == 65536);

    std::vector<int8_t> left;
    std::vector<int8_t> right;
    std::vector<int16_t> out;
    constexpr int kRunFrames = 5;
    for (int f = 0; f < kRunFrames; f++)
    {
        player.RunFrame(out);
        const auto& a = player.Driver().FifoA();
        const auto& b = player.Driver().FifoB();
        left.insert(left.end(), a.begin(), a.end());
        right.insert(right.end(), b.begin(), b.end());
    }

    XSFLIB_CHECK(player.Frames() == uint64_t(kRunFrames));
    XSFLIB_CHECK(out.size() == 2 * size_t((kRunFrames * kFrameCycles + 255) / 256));
    XSFLIB_CHECK(std::any_of(left.begin(), left.end(), [](int8_t b) { return b != 0; }));
    XSFLIB_CHECK(std::any_of(right.begin(), right.end(), [](int8_t b) { return b != 0; }));
    XSFLIB_CHECK(left != right);

    bool same = true;
    for (size_t i = 0; i < out.size() / 2; i++)
    {
        const uint64_t overflows = uint64_t(i) * 256 / kFifoCycles;
        const bool played = overflows > uint64_t(kFifoLead);
        const int a = played ? left.at(size_t(overflows) - size_t(kFifoLead) - 1) : 0;
        const int b = played ? right.at(size_t(overflows) - size_t(kFifoLead) - 1) : 0;
        same = same && out[2 * i] == a * 4 * 64 && out[2 * i + 1] == b * 4 * 64;
    }
    XSFLIB_CHECK(same);

    // The first byte comes at 194 * 1,596 cycles, and reaches the first output sample at or after that cycle.
    XSFLIB_CHECK(kFirstByteCycle == 309624);
    XSFLIB_CHECK(out[2 * (kFirstByteCycle / 256)] == 0);
    XSFLIB_CHECK(out[2 * (kFirstByteCycle / 256 + 1)] == left[0] * 4 * 64);
    XSFLIB_CHECK(out[2 * (kFirstByteCycle / 256 + 1) + 1] == right[0] * 4 * 64);
}

void TestRegisters()
{
    Cart cart;
    cart.Sequences({{TrackData().End()}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.Driver(); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(); }));
    XSFLIB_CHECK(Throws<std::logic_error>([&] { player.SetEchoGoal(1); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(1); }));
    ArchivePlayerOptions late;
    late.store_cycle = kFrameCycles;
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(0, late); }));

    player.Start(0);
    const ntdlib::agb::Apu& apu = player.Hardware();
    XSFLIB_CHECK(apu.Register(0x04000080) == 0x77 && apu.Register(0x04000081) == 0xFF);
    XSFLIB_CHECK(apu.Register(0x04000082) == 0x0D);
    XSFLIB_CHECK((apu.Register(0x04000089) & 0xC0) == 0x40);
    XSFLIB_CHECK(!player.Enhanced());
    XSFLIB_CHECK(&player.Info() == &game.info);
}

// Runs the song to its end, and checks that the output holds still once Busy() is false. Returns the frames until
// Busy() fell.
int RunToEnd(const Game& game, int most, const ArchivePlayerOptions& options = {})
{
    ArchivePlayer player(game.rom, game.info);
    player.Start(0, options);
    int frames = 0;
    std::vector<int16_t> out;
    while (player.Busy() && frames < most)
    {
        player.RunFrame(out);
        frames++;
    }

    XSFLIB_CHECK(!player.Busy());
    XSFLIB_CHECK(player.Driver().Song()->Ended());
    XSFLIB_CHECK(player.Driver().MixerSilent());
    XSFLIB_CHECK(player.Hardware().PsgSilent());
    XSFLIB_CHECK(!out.empty());
    if (out.empty())
    {
        return frames;
    }

    const std::array<int16_t, 2> last = {out[out.size() - 2], out[out.size() - 1]};
    const std::vector<int16_t> after = Frames(player, 8);
    XSFLIB_CHECK(!player.Busy());
    for (size_t i = 0; i + 1 < after.size(); i += 2)
    {
        XSFLIB_CHECK(after[i] == last[0] && after[i + 1] == last[1]);
    }

    return frames;
}

void TestBusy()
{
    // An empty song ends within the frames that its FIFOs need.
    Cart empty;
    empty.Sequences({{TrackData().End()}});
    const Game empty_game(empty);
    XSFLIB_CHECK(RunToEnd(empty_game, 16) <= 8);

    // Every kind of voice has to fall silent, however its note ends: the looped sample with a release, the plain
    // sample at once, and the PSG's three kinds through their envelopes.
    for (const uint8_t instrument : {kLooped, kPlain, kSquare, kNoise, kWave})
    {
        Cart cart;
        cart.Sequences({{PsgTrack(instrument, 60, 4)}});
        const Game game(cart);
        const int frames = RunToEnd(game, 2000);
        XSFLIB_CHECK(frames >= 4 && frames < 2000);
    }

    // The plain sample ends with the last point of the third frame, at 4 of its points for each of the mixer's. The
    // FIFOs play that frame's bytes over the next frame, and their last 17 in the frame after. The output has to hold
    // still from the song's end on.
    Cart stopped;
    stopped.Put32(kFirstPoints + kFirstLength, 4 * 3 * uint32_t(kFrameSamples));
    stopped.Sequences({{TrackData().Bytes({0xC2, kPlain}).Note(60, 3, 127).Wait(3).End()}});
    const Game stopped_game(stopped);
    ArchivePlayer last_points(stopped_game.rom, stopped_game.info);
    last_points.Start(0);
    Frames(last_points, 3);
    XSFLIB_CHECK(last_points.Driver().FifoA().back() != 0);
    Frames(last_points, 1);
    XSFLIB_CHECK(last_points.Driver().FifoA().back() == 0);
    XSFLIB_CHECK(RunToEnd(stopped_game, 16) < 16);

    // An echo that keeps half of its input rings on after the song's end, until the shifts leave nothing: 18 frames a
    // pass, for up to 15 passes of a sum of 15 bits. The song ends 6 frames into the first pass.
    Cart ringing;
    FillFirst(ringing, 64);
    ringing.Sequences({{TrackData().Wait(16).Bytes({0xE3, 0x7F, 0xC2, kHeld}).Note(60, 6, 127).Wait(6).End()}});
    const Game ringing_game(ringing);
    ArchivePlayerOptions echo;
    echo.echo_goal = 1;
    const int rung = RunToEnd(ringing_game, 2000, echo);
    XSFLIB_CHECK(rung > 16 + 6 + 2 * kEchoFrames && rung < 2000);

    // Without the echo, the same song ends with its voice.
    XSFLIB_CHECK(RunToEnd(ringing_game, 2000) < 16 + 6 + kEchoFrames);

    // At a level of 0, the echo keeps all of its input, and the song never ends.
    ArchivePlayer held(ringing_game.rom, ringing_game.info);
    ArchivePlayerOptions forever;
    forever.echo_goal = 0;
    held.Start(0, forever);
    for (int f = 0; f < 400; f++)
    {
        held.RunFrame();
    }
    XSFLIB_CHECK(held.Busy() && held.Driver().Song()->Ended());

    // The Super Mario Advance 2 revision's echo rounds to nearest. At a level of 1, a sum of 1 stays 1.
    Cart older(true, Revision::kSuperMarioAdvance2);
    FillFirst(older, 64);
    older.Sequences({{TrackData().Wait(32).Bytes({0xE3, 0x7F, 0xC2, kHeld}).Note(60, 6, 127).Wait(6).End()}});
    const Game older_game(older);
    ArchivePlayer rounded(older_game.rom, older_game.info);
    rounded.Start(0, echo);
    for (int f = 0; f < 1000; f++)
    {
        rounded.RunFrame();
    }
    XSFLIB_CHECK(rounded.Busy() && rounded.Driver().Song()->Ended());
    bool kept = false;
    for (int f = 0; f < kEchoFrames; f++)
    {
        rounded.RunFrame();
        kept = kept || rounded.Driver().FifoA()[0] == 1 || rounded.Driver().FifoB()[0] == 1;
    }
    XSFLIB_CHECK(kept);

    // Without the echo, whose level starts at 31 in that revision, the same song ends with its voice.
    XSFLIB_CHECK(RunToEnd(older_game, 400) < 32 + 6 + kEchoFrames);
}

void TestFramesWithoutSamples()
{
    // A frame without samples ends in the state that a frame with samples leaves.
    Cart cart;
    FillSine(cart);
    cart.Sequences({{HeldTrack(0x40), PsgTrack(kSquare, 60, 1000)}});
    const Game game(cart);
    ArchivePlayer whole(game.rom, game.info);
    ArchivePlayer seeking(game.rom, game.info);
    whole.Start(0);
    seeking.Start(0);
    Frames(whole, 5);
    for (int f = 0; f < 5; f++)
    {
        seeking.RunFrame();
    }

    XSFLIB_CHECK(seeking.Frames() == 5);
    const std::vector<int16_t> run = Frames(whole, 3);
    XSFLIB_CHECK(std::any_of(run.begin(), run.end(), [](int16_t s) { return s != 0; }));
    XSFLIB_CHECK(Frames(seeking, 3) == run);
}

void TestSnapshots()
{
    Cart cart;
    FillSine(cart);
    cart.Sequences({{TrackData().Bytes({0xE3, 0x7F, 0xC2, kHeld}).Note(60, 1000, 127).Wait(1000).End(),
                     PsgTrack(kSquare, 60, 1000)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    XSFLIB_CHECK(Throws<std::logic_error>([&] { static_cast<void>(player.Save()); }));
    ArchivePlayerOptions options;
    options.echo_goal = 1;
    player.Start(0, options);
    Frames(player, 30);

    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<int16_t> run = Frames(player, 30);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.Frames() == 30);
    XSFLIB_CHECK(Frames(player, 30) == run);

    // A snapshot can take another's place, and stands alone: a later save takes nothing from it.
    ArchivePlayer::Snapshot later = player.Save(&snapshot);
    later = snapshot;
    player.Restore(later);
    XSFLIB_CHECK(player.Frames() == 30);
    XSFLIB_CHECK(Frames(player, 30) == run);

    ArchivePlayer other(game.rom, game.info);
    other.Start(0);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { other.Restore(snapshot); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { other.Restore(ArchivePlayer::Snapshot()); }));

    // A restore takes the frames back that a song had left before its end, and a later one the end.
    Cart short_song;
    FillFirst(short_song, 64);
    short_song.Sequences({{HeldTrack(0x40, 4)}});
    const Game short_game(short_song);
    ArchivePlayer ending(short_game.rom, short_game.info);
    ending.Start(0);
    Frames(ending, 2);
    const ArchivePlayer::Snapshot early = ending.Save();
    while (ending.Busy() && ending.Frames() < 16)
    {
        Frames(ending, 1);
    }

    XSFLIB_CHECK(!ending.Busy());
    const ArchivePlayer::Snapshot ended = ending.Save();
    ending.Restore(early);
    XSFLIB_CHECK(ending.Busy());
    ending.Restore(ended);
    XSFLIB_CHECK(!ending.Busy());

    // The last frame's meters go with a restore.
    player.MeterTracks(true);
    Frames(player, 3);
    XSFLIB_CHECK(player.OutputPeaks()[0] != 0);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.OutputPeaks()[0] == 0 && player.TrackPeaks(0)[0] == 0);

    // A snapshot leaves the mutes out: a mute stays through a restore.
    player.MuteTrack(0, true);
    player.Restore(snapshot);
    XSFLIB_CHECK(player.IsTrackMuted(0));
}

void TestMutes()
{
    Cart cart;
    FillFirst(cart, 32);
    cart.Sequences({{HeldTrack(0), HeldTrack(127)}});
    const Game game(cart);
    ArchivePlayer whole(game.rom, game.info);
    ArchivePlayer player(game.rom, game.info);
    whole.Start(0);
    player.Start(0);
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(kPlayerTracks, true); }));
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(-1, true); }));
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.SoloTrack(kPlayerTracks, true); }));
    XSFLIB_CHECK(Throws<std::out_of_range>([&] { player.SoloTrack(-1, true); }));

    const auto silent = [](const std::array<int8_t, kFrameSamples>& bytes)
    {
        return std::all_of(bytes.begin(), bytes.end(), [](int8_t b) { return b == 0; });
    };
    const auto same_voices = [&]
    {
        const auto& a = whole.Driver().Song()->Voices();
        const auto& b = player.Driver().Song()->Voices();
        bool same = a.size() == b.size();
        for (size_t v = 0; same && v < a.size(); v++)
        {
            same = a[v].position == b[v].position && a[v].state == b[v].state && a[v].level == b[v].level;
        }

        return same;
    };

    // Track 0 plays on the left only, and track 1 on the right only. A mute takes the voice out of the sums, and the
    // voice runs on as before.
    player.MuteTrack(1, true);
    XSFLIB_CHECK(player.IsTrackMuted(1) && !player.IsTrackMuted(0));
    whole.RunFrame();
    player.RunFrame();
    whole.RunFrame();
    player.RunFrame();
    XSFLIB_CHECK(!silent(whole.Driver().FifoA()) && !silent(whole.Driver().FifoB()));
    XSFLIB_CHECK(player.Driver().FifoA() == whole.Driver().FifoA());
    XSFLIB_CHECK(silent(player.Driver().FifoB()));
    XSFLIB_CHECK(same_voices());

    // A solo leaves every other track out.
    player.MuteTrack(1, false);
    player.SoloTrack(1, true);
    XSFLIB_CHECK(player.IsTrackSoloed(1) && !player.IsTrackSoloed(0));
    whole.RunFrame();
    player.RunFrame();
    XSFLIB_CHECK(silent(player.Driver().FifoA()));
    XSFLIB_CHECK(player.Driver().FifoB() == whole.Driver().FifoB());
    XSFLIB_CHECK(same_voices());

    // A mute still holds on a soloed track.
    player.MuteTrack(1, true);
    whole.RunFrame();
    player.RunFrame();
    XSFLIB_CHECK(silent(player.Driver().FifoA()) && silent(player.Driver().FifoB()));

    // Back to the whole sound.
    player.MuteTrack(1, false);
    player.SoloTrack(1, false);
    whole.RunFrame();
    player.RunFrame();
    XSFLIB_CHECK(player.Driver().FifoA() == whole.Driver().FifoA());
    XSFLIB_CHECK(player.Driver().FifoB() == whole.Driver().FifoB());
    XSFLIB_CHECK(same_voices());
}

void TestMutesOfPsg()
{
    // A PSG channel belongs to the track of its last note. A mute silences the channel, and the stores stay.
    Cart cart;
    cart.Sequences({{TrackData().Wait(1000).End(), PsgTrack(kSquare, 60, 1000)}});
    const Game game(cart);
    ArchivePlayer whole(game.rom, game.info);
    ArchivePlayer muted(game.rom, game.info);
    ArchivePlayer soloed(game.rom, game.info);
    ArchivePlayer other(game.rom, game.info);
    whole.Start(0);
    muted.Start(0);
    soloed.Start(0);
    other.Start(0);
    muted.MuteTrack(1, true);
    soloed.SoloTrack(1, true);
    other.SoloTrack(0, true);
    const std::vector<int16_t> sound = Frames(whole, 8);
    XSFLIB_CHECK(whole.Driver().PsgChannelTrack(0, 1) == 1);
    XSFLIB_CHECK(std::any_of(sound.begin(), sound.end(), [](int16_t s) { return s != 0; }));
    const std::vector<int16_t> quiet = Frames(muted, 8);
    XSFLIB_CHECK(std::all_of(quiet.begin(), quiet.end(), [](int16_t s) { return s == 0; }));
    XSFLIB_CHECK(muted.Hardware().Register(0x04000069) == whole.Hardware().Register(0x04000069));
    XSFLIB_CHECK(Frames(soloed, 8) == sound);
    const std::vector<int16_t> left_out = Frames(other, 8);
    XSFLIB_CHECK(std::all_of(left_out.begin(), left_out.end(), [](int16_t s) { return s == 0; }));

    // A note takes the channel from the note that it replaces, whichever track played that note. The channel then
    // follows the new note's track, for a mute of either.
    Cart taken;
    taken.Sequences(
        {{PsgTrack(kSquare, 60, 1000), TrackData().Wait(3).Bytes({0xC2, kSquare}).Note(72, 100, 127).Wait(100).End()}});
    const Game taken_game(taken);
    ArchivePlayer first(taken_game.rom, taken_game.info);
    first.Start(0);
    Frames(first, 2);
    XSFLIB_CHECK(first.Driver().PsgChannelTrack(0, 1) == 0);
    Frames(first, 3);
    XSFLIB_CHECK(first.Driver().PsgChannelTrack(0, 1) == 1);
    first.MuteTrack(0, true);
    const std::vector<int16_t> follows = Frames(first, 2);
    XSFLIB_CHECK(std::any_of(follows.begin(), follows.end(), [](int16_t s) { return s != 0; }));
    first.MuteTrack(0, false);
    first.MuteTrack(1, true);
    const std::vector<int16_t> gone = Frames(first, 2);
    XSFLIB_CHECK(std::all_of(gone.begin(), gone.end(), [](int16_t s) { return s == 0; }));
}

void TestMeters()
{
    // Track 0 plays on the left only. A voice's points reach its meter in the frame that plays them: a frame after the
    // frame that mixes them. A byte counts 4 times in the output's 10 bits, and each of those 64 times in a sample. The
    // part stays exact: the level times the point over 128.
    Cart cart;
    FillFirst(cart, 64);
    cart.Sequences({{HeldTrack(0)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0);

    // Without meters, every peak is 0.
    Frames(player, 1);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == 0 && player.OutputPeaks()[0] == 0);

    player.MeterTracks(true);
    Frames(player, 1);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == 0);
    Frames(player, 1);
    const VoiceMix mix = player.Driver().Song()->Mixes().at(0);
    const int level = LeftLevel(mix);
    XSFLIB_CHECK(level != 0);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == int32_t(std::lround(double(level * 64) / 128.0 * 4.0 * 64.0)));
    XSFLIB_CHECK(player.TrackPeaks(0)[1] == 0);
    XSFLIB_CHECK(player.TrackPeaks(1)[0] == 0 && player.TrackPeaks(1)[1] == 0);
    XSFLIB_CHECK(player.OutputPeaks()[0] == ByteOf(level * 64) * 4 * 64);
    XSFLIB_CHECK(player.OutputPeaks()[1] == 0);

    // A frame without samples gives no peaks.
    player.RunFrame();
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == 0 && player.OutputPeaks()[0] == 0);

    // A muted track's meter stays at 0.
    player.MuteTrack(0, true);
    Frames(player, 3);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == 0 && player.TrackPeaks(0)[1] == 0);

    // Meters that are off give nothing.
    player.MeterTracks(false);
    player.MuteTrack(0, false);
    Frames(player, 3);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == 0 && player.OutputPeaks()[0] == 0);
}

void TestMetersOlder()
{
    // The Super Mario Advance 2 revision's levels shift by 7, and its parts by 8 on their way to the sums: a part's
    // byte is the level times the point over 256.
    Cart cart(true, Revision::kSuperMarioAdvance2);
    FillFirst(cart, 64);
    cart.Sequences({{HeldTrack(0)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0);
    player.MeterTracks(true);
    Frames(player, 3);
    const VoiceMix mix = player.Driver().Song()->Mixes().at(0);
    const int level = int((uint32_t((127 - int(mix.pan)) * int(mix.volume & 0xFFFF)) & 0xFFFF) >> 7);
    XSFLIB_CHECK(level != 0);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == int32_t(std::lround(double(level * 64) / 256.0 * 4.0 * 64.0)));
    XSFLIB_CHECK(player.OutputPeaks()[0] == std::clamp((level * 64) >> 8, -128, 127) * 4 * 64);
}

void TestMetersOfPsg()
{
    // The driver's init gives the PSG half of its share: a channel's level counts 16 / 2 times in the 10 bits at the
    // master volume's 7 plus 1, on the sides that NR51 gives it. Square 2's level lies in NR22's high bits. The
    // channel's meter comes without a frame's delay: the hardware plays it at once.
    Cart cart;
    cart.Sequences({{PsgTrack(kSquare, 60, 1000)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    player.Start(0);
    player.MeterTracks(true);
    Frames(player, 4);
    const int level = player.Hardware().Register(0x04000069) >> 4;
    const int sides = player.Hardware().Register(0x04000081);
    XSFLIB_CHECK(level != 0);
    XSFLIB_CHECK(player.TrackPeaks(0)[0] == ((sides >> 5 & 1) ? level * 8 * 64 : 0));
    XSFLIB_CHECK(player.TrackPeaks(0)[1] == ((sides >> 1 & 1) ? level * 8 * 64 : 0));
    XSFLIB_CHECK(player.TrackPeaks(0)[0] != 0 || player.TrackPeaks(0)[1] != 0);

    // A frame's stores can give a channel to a muted track. The channel has sounded up to then, for the other track's
    // note. The muted track's meter stays at 0.
    Cart taken;
    taken.Sequences(
        {{PsgTrack(kSquare, 60, 1000), TrackData().Wait(6).Bytes({0xC2, kSquare}).Note(72, 100, 127).Wait(100).End()}});
    const Game taken_game(taken);
    ArchivePlayer muted(taken_game.rom, taken_game.info);
    muted.Start(0);
    muted.MeterTracks(true);
    muted.MuteTrack(1, true);
    bool sounded = false;
    for (int f = 0; f < 12; f++)
    {
        Frames(muted, 1);
        XSFLIB_CHECK(muted.TrackPeaks(1)[0] == 0 && muted.TrackPeaks(1)[1] == 0);
        sounded = sounded || muted.TrackPeaks(0)[0] != 0 || muted.TrackPeaks(0)[1] != 0;
    }
    XSFLIB_CHECK(sounded);
    XSFLIB_CHECK(muted.Driver().PsgChannelTrack(0, 1) == 1);
}

void TestEchoGoal()
{
    // The echo's level moves by 1 a frame towards the goal, from 16. A goal past 31 counts as 31.
    Cart cart;
    cart.Sequences({{HeldTrack(0x40)}});
    const Game game(cart);
    ArchivePlayer player(game.rom, game.info);
    ArchivePlayerOptions options;
    options.echo_goal = 10;
    player.Start(0, options);
    XSFLIB_CHECK(player.Driver().EchoLevel() == 16);
    Frames(player, 2);
    XSFLIB_CHECK(player.Driver().EchoLevel() == 14);
    Frames(player, 10);
    XSFLIB_CHECK(player.Driver().EchoLevel() == 10);
    player.SetEchoGoal(40);
    Frames(player, 25);
    XSFLIB_CHECK(player.Driver().EchoLevel() == 31);
}

void TestVersions()
{
    // The game's play-music routine mutes track 1 of sequence 1 in the first version, and track 2 in the second. A
    // muted track plays no notes.
    Cart cart;
    cart.Sequences({{HeldTrack(0x40)},
                    {TrackData().Bytes({0xC2, kPlain}).Note(60, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(62, 6, 127).Wait(6).End(),
                     TrackData().Bytes({0xC2, kPlain}).Note(64, 6, 127).Wait(6).End()}});
    cart.MutingPlayMusic({0x0000, 0x0001, 0x0001, 0x0001, 0x0009}, 1, 2, {1, 1}, {1, 2});
    const Game game(cart);

    const auto keys = [&](int version)
    {
        ArchivePlayer player(game.rom, game.info);
        ArchivePlayerOptions options;
        options.version = version;
        player.Start(1, options);
        player.RunFrame();
        std::vector<int> played;
        for (const ntdlib::rd2::Event& e : player.Driver().Song()->Events())
        {
            if (e.kind == ntdlib::rd2::Event::kNote)
            {
                played.push_back(e.key);
            }
        }

        return played;
    };

    XSFLIB_CHECK(keys(0) == std::vector<int>({60, 64}));
    XSFLIB_CHECK(keys(1) == std::vector<int>({60, 62}));
}

void TestSoundEffects()
{
    // A sound effect plays as a song after the sequences. Its track holds the priority and the echo of an effect.
    Cart cart;
    FillFirst(cart, 64);
    cart.Sequences({{TrackData().End()}});
    cart.Effects({{TrackData().Bytes({0xC2, kHeld}).Note(60, 20, 127).Wait(20).End()}});
    const Game game(cart);
    XSFLIB_CHECK(ntdlib::rd2::SongCount(game.info) == 2);
    ArchivePlayer player(game.rom, game.info);
    player.Start(1);
    std::vector<int16_t> out;
    int frames = 0;
    while (player.Busy() && frames < 200)
    {
        player.RunFrame(out);
        frames++;
    }

    XSFLIB_CHECK(!player.Busy());
    XSFLIB_CHECK(std::any_of(out.begin(), out.end(), [](int16_t s) { return s != 0; }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(2); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(-1); }));
}

struct RomSet
{
    std::vector<uint8_t>* File(const std::string& path)
    {
        const auto file = files.find(path);
        return file != files.end() ? &file->second : nullptr;
    }

    std::map<std::string, std::vector<uint8_t>> files;
    int song = 0;
};

struct RomProgram
{
    using Set = RomSet;

    static std::optional<std::string> Select(RomSet& set, ntdlib::rd2::ArchiveSelection& selection)
    {
        selection.rom_path = "game.gba";
        selection.song = set.song;
        return std::nullopt;
    }
};

using RomSource = xsflib::player::ArchiveSource<ntdlib::rd2::ArchiveTraits<RomProgram>>;

RomSet SetOf(const Rom& rom, int song)
{
    RomSet set;
    set.files["game.gba"].assign(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    set.song = song;
    return set;
}

void TestArchiveSource()
{
    // Archive mode plays the song as the player does, takes snapshots, and gives the player's refusals as its errors. A
    // ROM without the driver fails to load.
    Cart cart;
    FillFirst(cart, 64);
    cart.Sequences({{HeldTrack(0x40)}});
    const Game game(cart);

    RomSource source;
    RomSet set = SetOf(game.rom, 0);
    std::vector<int16_t> out;
    const std::optional<std::string> started = source.Start(set, out);
    XSFLIB_CHECK(!started);
    XSFLIB_CHECK(out.empty() && source.Busy());
    for (int f = 0; f < 6; f++)
    {
        XSFLIB_CHECK(source.RunFrame(out));
    }

    ArchivePlayer player(game.rom, game.info);
    player.Start(0);
    XSFLIB_CHECK(Frames(player, 6) == out);

    const std::shared_ptr<ArchivePlayer::Snapshot> snapshot = source.Save(nullptr);
    const size_t saved = out.size();
    for (int f = 0; f < 6; f++)
    {
        source.RunFrame(out);
    }
    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int f = 0; f < 6; f++)
    {
        source.RunFrame(again);
    }
    XSFLIB_CHECK(std::equal(again.begin(), again.end(), out.begin() + std::ptrdiff_t(saved), out.end()));

    RomSet missing = SetOf(game.rom, 5);
    const std::optional<std::string> refused = source.Start(missing, out);
    XSFLIB_CHECK(refused && refused->find("song 5") != std::string::npos);
    RomSet empty;
    const std::optional<std::string> absent = source.Start(empty, out);
    XSFLIB_CHECK(absent && absent->find("game.gba") != std::string::npos);
    const Rom blank_rom = Cart(false).ToRom();
    RomSet blank = SetOf(blank_rom, 0);
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { source.Start(blank, out); }));
}

} // namespace

int main()
{
    TestOutput();
    TestRegisters();
    TestBusy();
    TestFramesWithoutSamples();
    TestSnapshots();
    TestMutes();
    TestMutesOfPsg();
    TestMeters();
    TestMetersOlder();
    TestMetersOfPsg();
    TestEchoGoal();
    TestVersions();
    TestSoundEffects();
    TestArchiveSource();
    return xsflib::testing::Summary("rd2_player");
}
