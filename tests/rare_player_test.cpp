// SPDX-License-Identifier: MIT

// Unit tests for Rare's performance figures, archive player, audition and dump. They build small synthetic cartridges
// in memory, so no game ROM is needed.

#include "rare_test_util.h"

namespace ntdlib::sequence_export::rare
{
namespace
{

// The performance of a tune: its notes with their commands, the stretches of a track that loops, and its volume. The
// note on lies at offset 15 of the track, after the tempo, the program, the volume and the loop's start.
void TestPerformance()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(0, 0)
                   .Control(0, kCtrlVolume, 100)
                   .Control(0, kCtrlLoopStart, 0)
                   .On(0, 60, 90)
                   .Wait(240)
                   .Off(0, 60)
                   .Wait(240)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings settings = TempSettings("performance");
    settings.loops = 2;
    const Performance perf = PerformSong(rom, info, 0, settings);

    SEQLIB_CHECK_EQ(perf.timebase, 480u);
    SEQLIB_REQUIRE(perf.tempo.size() == 1);
    SEQLIB_CHECK(perf.tempo[0].bpm > 119.0 && perf.tempo[0].bpm < 120.0);
    SEQLIB_CHECK(perf.loop.has_value() && perf.loop->start == 0 && perf.loop->end == 480);
    SEQLIB_CHECK(rom.U8(perf.base) == kCmdTempo && rom.U8(perf.base + 15) == kCmdNoteOn);

    // A note for each pass, each from the note on at offset 15.
    SEQLIB_REQUIRE(perf.note_commands.size() >= 2);
    for (const NoteCommand& c : perf.note_commands)
    {
        SEQLIB_CHECK_EQ(c.offset, 15u);
    }

    // The track opens at its start, and loops back to the note on.
    const std::vector<Stretch>& stretches = perf.stretches[0];
    SEQLIB_REQUIRE(stretches.size() >= 2);
    SEQLIB_CHECK(stretches[0].entry == Entry::kOpen && stretches[0].first == 0 && stretches[0].last == 27);
    SEQLIB_CHECK(stretches[1].entry == Entry::kLoop && stretches[1].first == 15 && stretches[1].tick == 480);
    SEQLIB_CHECK_EQ(perf.note_commands[0].stretch, 0u);
    SEQLIB_CHECK_EQ(perf.note_commands[1].stretch, 1u);

    // The notes on the track, at their ticks, with the driver's velocity.
    std::vector<std::pair<uint32_t, uint32_t>> notes;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            SEQLIB_CHECK(e.key == 60 && e.value == 90);
            notes.push_back({e.tick, e.note});
        }
    }
    SEQLIB_REQUIRE(notes.size() >= 2);
    SEQLIB_CHECK(notes[0] == std::make_pair(0u, 1u) && notes[1] == std::make_pair(480u, 2u));
    SEQLIB_CHECK(perf.tracks[0].front().kind == EventKind::kPreset && perf.tracks[0].front().preset == 0);

    // The volume.
    SEQLIB_REQUIRE(!perf.mix[0].empty());
    SEQLIB_CHECK(perf.mix[0][0] == (MixChange{0, Mix::kVolume, 100}));
}

// Runs `frames` frames of `player` into `out`.
void RunFrames(ArchivePlayer& player, int frames, std::vector<int16_t>& out)
{
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }
}

// The player plays each frame's mix in the next frame, through FIFO A on both sides. The output of two tracks is the
// sum of each track's alone. A restore and a seek give the run's samples again. The tune stops sounding once its
// tracks have ended and its notes have fallen silent. Track 0 waits for its note's release of 50 frames to end: the
// driver stops the envelopes once every track has ended.
void TestPlayer()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank =
        cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}, {1, cart.Sample(Saw(2048))}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(0, 0).On(0, 60, 40).Wait(240).Off(0, 60).Wait(1920).End(),
               Track(kFormat).Program(1, 1).On(1, 67, 40).Wait(240).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayer full(rom, info);
    full.Start(0);
    full.MeterTracks(true);
    std::vector<int16_t> first;
    full.RunFrame(first);
    SEQLIB_CHECK_EQ(full.Rate(), 65536);
    SEQLIB_CHECK(first.size() >= 2 * 1097 && first.size() <= 2 * 1098);
    SEQLIB_CHECK(std::all_of(first.begin(), first.end(), [](int16_t s) { return s == 0; }));

    std::vector<int16_t> out;
    RunFrames(full, 4, out);
    bool sides = true;
    for (size_t i = 0; i + 1 < out.size(); i += 2)
    {
        sides = sides && out[i] == out[i + 1];
    }
    SEQLIB_CHECK(sides);
    SEQLIB_CHECK(std::any_of(out.begin(), out.end(), [](int16_t s) { return s != 0; }));
    SEQLIB_CHECK(full.TrackPeaks(0)[0] > 0 && full.TrackPeaks(0)[0] == full.TrackPeaks(0)[1]);
    SEQLIB_CHECK(full.TrackPeaks(1)[0] > 0);
    SEQLIB_CHECK(full.OutputPeaks()[0] > 0);

    // Each track alone.
    std::vector<int16_t> alone[2];
    for (int t = 0; t < 2; t++)
    {
        ArchivePlayer player(rom, info);
        player.Start(0);
        player.MuteTrack(1 - t, true);
        player.MeterTracks(true);
        RunFrames(player, 5, alone[t]);
        SEQLIB_CHECK_EQ(player.TrackPeaks(1 - t)[0], 0);
    }

    std::vector<int16_t> all = first;
    all.insert(all.end(), out.begin(), out.end());
    bool sums = all.size() == alone[0].size() && all.size() == alone[1].size();
    for (size_t i = 0; sums && i < all.size(); i++)
    {
        sums = all[i] == alone[0][i] + alone[1][i];
    }
    SEQLIB_CHECK(sums);

    // A restore and a seek.
    ArchivePlayer run(rom, info);
    run.Start(0);
    for (int f = 0; f < 5; f++)
    {
        run.RunFrame();
    }
    const ArchivePlayer::Snapshot snapshot = run.Save();
    std::vector<int16_t> after;
    RunFrames(run, 20, after);
    run.Restore(snapshot);
    SEQLIB_CHECK_EQ(run.Frames(), 5);
    std::vector<int16_t> again;
    RunFrames(run, 20, again);
    SEQLIB_CHECK(again == after);

    ArchivePlayer played(rom, info);
    played.Start(0);
    std::vector<int16_t> start;
    RunFrames(played, 5, start);
    std::vector<int16_t> played_on;
    RunFrames(played, 20, played_on);
    SEQLIB_CHECK(played_on == after);

    // The end.
    int frames = 0;
    while (run.Busy() && frames < 3600)
    {
        run.RunFrame();
        frames++;
    }
    SEQLIB_CHECK(!run.Busy());
    SEQLIB_CHECK(run.Driver().Ended());
    std::vector<int16_t> still;
    RunFrames(run, 3, still);
    SEQLIB_CHECK(std::all_of(still.begin(), still.end(), [](int16_t s) { return s == 0; }));
    SEQLIB_CHECK(run.Tick() >= 240);

    // The enhanced mode.
    ArchivePlayer enhanced(rom, info);
    enhanced.Start(0, {.enhanced = true});
    std::vector<float> floats;
    for (int f = 0; f < 5; f++)
    {
        enhanced.RunFrame(floats);
    }
    SEQLIB_CHECK_EQ(enhanced.Rate(), ntdlib::agb::kEnhancedRate);
    SEQLIB_CHECK(floats.size() == size_t(5 * 2 * ntdlib::agb::kEnhancedFrameSamples));
    SEQLIB_CHECK(std::all_of(floats.begin(), floats.end(), [](float s) { return std::isfinite(s); }));
    SEQLIB_CHECK(std::any_of(floats.begin(), floats.end(), [](float s) { return s != 0.0f; }));
    bool threw = false;
    try
    {
        std::vector<int16_t> console;
        enhanced.RunFrame(console);
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }
    SEQLIB_CHECK(threw);
}

// An audition plays one note of a tune's program, with the tracks idle, until its release.
void TestAudition()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(3, 0).On(3, 72, 100).Wait(4800).End()}, bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayer player(rom, info);
    player.StartNote(0, 0, 60, 100);
    std::vector<int16_t> out;
    RunFrames(player, 5, out);
    SEQLIB_CHECK(std::any_of(out.begin(), out.end(), [](int16_t s) { return s != 0; }));
    SEQLIB_CHECK(player.Driver().GetSlot(0).state == kSlotOn && player.Driver().GetSlot(0).key == 60);
    SEQLIB_CHECK(player.Driver().GetSlot(3 * info.slots_per_channel).state == kSlotFree);
    SEQLIB_CHECK(player.Busy());

    player.ReleaseNote();
    int frames = 0;
    while (player.Driver().GetSlot(0).state != kSlotFree && frames < 3600)
    {
        player.RunFrame();
        frames++;
    }
    SEQLIB_CHECK(player.Driver().GetSlot(0).state == kSlotFree);

    // The audition ends once the note and the mix have fallen silent.
    while (player.Busy() && frames < 3600)
    {
        player.RunFrame();
        frames++;
    }
    SEQLIB_CHECK(!player.Busy());
}

// The driver ignores a loop's end before the loop's start. The listing goes on past such an end, and stops at the
// loop's end.
void TestDump()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(0, 0).On(0, 60, 100).Wait(100).Off(0, 60).End(),
               Track(kFormat)
                   .Control(1, kCtrlLoopEnd, 0)
                   .On(1, 60, 100)
                   .Wait(100)
                   .Off(1, 60)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 62, 100)
                   .Wait(50)
                   .Off(1, 62)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const std::string path = Utf8(TempPath("dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    const std::string second = s.substr(s.find("---- track 1"));
    SEQLIB_CHECK(s.find("tempo 500000 us per quarter (120.00 BPM)") != std::string::npos);
    SEQLIB_CHECK(s.find("note on ch 0 key 60 vel 100") != std::string::npos);
    SEQLIB_CHECK(s.find("     100  0B              end of track") != std::string::npos);
    SEQLIB_CHECK(second.find("#103 = 0 (no loop to end; ignored)\n") != std::string::npos);
    SEQLIB_CHECK(second.find("note on ch 1 key 62 vel 100") != std::string::npos);
    SEQLIB_CHECK(second.find("#103 = 0\n") != std::string::npos);
    SEQLIB_CHECK(second.find("end of track") == std::string::npos);
}

} // namespace

int RunTests()
{
    TestPerformance();
    TestPlayer();
    TestAudition();
    TestDump();

    return seqlib::testing::Summary("rare_player");
}

} // namespace ntdlib::sequence_export::rare

int main()
{
    return ntdlib::sequence_export::rare::RunTests();
}
