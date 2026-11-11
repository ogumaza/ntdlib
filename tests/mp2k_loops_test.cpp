// SPDX-License-Identifier: MIT

// Unit tests of the loops and the ends of MP2K's songs in a conversion: the loops' marks, starts, levels and lengths,
// the settings that a song makes again, the limit of an hour and the end after the last sound. They build small
// synthetic cartridges in memory. No game ROM is needed.

#include "mp2k_test_util.h"

namespace ntdlib::sequence_export::mp2k
{
namespace
{

void TestLoops()
{
    // A track that goes back to a command after 12 ticks plays its loop of 24 ticks twice, and the MIDI file marks it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
    const size_t loop = t.Here();
    t.Note(12, 60, 100).Wait(24).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loops");

    const TrackLayout layout = ScanTrack(rom, rom.U32(rom.U32(kSongTable) + 8));
    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(layout.loops && layout.loop_start == 12 && layout.loop_end == 36);
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(AtFrames(r.loop_start, 12));
    SEQLIB_CHECK(AtFrames(r.loop_end, 36));
    SEQLIB_CHECK(AtFrames(r.seconds, 60));

    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{12 * kMidiTicks, "loopStart"},
                                                                    {36 * kMidiTicks, "loopEnd"}};
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_REQUIRE_EQ(m.Find(0x90).size(), 2);
}

void TestHang()
{
    // A track that goes back to a command without a wait would hang the game. The sequencer stops and says where. The
    // other track of the song runs until then, and a track that waits takes no part in the count.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(1);
    const size_t first_loop = first.Here();
    first.Wait(1).Goto(first_loop);
    Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(6);
    const size_t second_loop = second.Here();
    second.Cmd(kCmdVol, 90).Goto(second_loop);
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer sequencer(rom, info, 0);
    SEQLIB_REQUIRE(sequencer.Valid());
    std::optional<ntdlib::mp2k::Hang> hang;
    int steps = 0;
    try
    {
        for (; steps < 60; steps++)
        {
            sequencer.Step();
        }
    }
    catch (const ntdlib::mp2k::Hang& e)
    {
        hang = e;
    }
    SEQLIB_REQUIRE(hang.has_value());
    SEQLIB_CHECK_EQ(hang->TrackIndex(), 1);
    SEQLIB_CHECK(steps >= 1 && steps < 60);

    // The second track's address: the song's header holds the address of each track from its eighth byte. Its loop
    // starts past its voice, its volume and its wait, and the track goes round the two commands of the loop.
    const uint32_t header = rom.U32(kSongTable);
    const uint32_t track = rom.U32(header + 12);
    SEQLIB_CHECK(hang->Address() >= track && hang->Address() < track + uint32_t(second.Here()) + 16);
    SEQLIB_CHECK(std::string(hang->what()).find("the song hangs") != std::string::npos);
}

void TestLoopStarts()
{
    // The first track loops 24 ticks from tick 4, the second 48 ticks from tick 16, and the third 12 ticks from tick 8.
    // Every track is looping from tick 16. The loop goes from there for the 48 ticks that each track's loop divides, to
    // tick 64, and the song plays it twice. The middle track's loop starts last and is the longest. Neither the first
    // track's loop nor the last track's loop gives the same times.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(4);
    const size_t first_loop = first.Here();
    first.Note(12, 60, 100).Wait(24).Goto(first_loop);
    Track middle;
    middle.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(16);
    const size_t middle_loop = middle.Here();
    middle.Note(12, 64, 100).Wait(48).Goto(middle_loop);
    Track last;
    last.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(8);
    const size_t last_loop = last.Here();
    last.Note(6, 67, 100).Wait(12).Goto(last_loop);
    cart.Song({first, middle, last}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_starts");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(AtFrames(r.loop_start, 16));
    SEQLIB_CHECK(AtFrames(r.loop_end, 64));
    SEQLIB_CHECK(AtFrames(r.seconds, 112));
}

void TestLoopStartsAtSecondPass()
{
    // A track sets tempo 75 on tick 12, just before its loop, and tempo 60 halfway through the loop's 24 ticks, which
    // the next pass keeps. The first pass starts at tempo 75 and the later ones at 60, so the marked loop starts at the
    // second pass, at tick 36, and the song plays the first pass before it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12).Cmd(kCmdTempo, 75);
    const size_t loop = t.Here();
    t.Note(12, 60, 100).Wait(12).Cmd(kCmdTempo, 60).Wait(12).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("second_pass");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    const std::vector<std::pair<uint32_t, std::string>> kMarkers = {{36 * kMidiTicks, "loopStart"},
                                                                    {60 * kMidiTicks, "loopEnd"}};
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(markers == kMarkers);
    SEQLIB_REQUIRE_EQ(m.Find(0x90).size(), 3);
}

void TestLoopLevelsAtSecondPass()
{
    // A track plays at volume 100 and the centre before its loop. The loop sets the volume as it starts. The loop's
    // second note plays at another pan. The next pass keeps that pan. In song 0 it's 0x7F. In song 1 the loop also sets
    // the pan to the centre as it starts. In song 2 the second note's pan is 0x42.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    for (int song = 0; song < 3; song++)
    {
        Track t;
        t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x40).Wait(12);
        const size_t loop = t.Here();
        t.Cmd(kCmdVol, 100);
        if (song == 1)
        {
            t.Cmd(kCmdPan, 0x40);
        }
        t.Note(12, 60, 100).Wait(12).Cmd(kCmdPan, song == 2 ? 0x42 : 0x7F).Note(12, 62, 100).Wait(12).Goto(loop);
        cart.Song({t}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_levels");

    std::vector<SongReport> reports;
    for (int song = 0; song < 3; song++)
    {
        reports.push_back(ConvertSong(rom, info, song, opt, nullptr));
    }

    // In song 0, a player that loops on the markers would play the loop's first note at the centre. The game plays it
    // on the right in the later passes. The marked loop therefore starts at the second pass, from tick 36. In song 1
    // the passes play alike. In song 2 each side of the later passes' first note plays less than 1 dB away from the
    // centre's level. Both keep the loop at the first pass, from tick 12.
    SEQLIB_CHECK(reports[0].ok && AtFrames(reports[0].loop_start, 36) && AtFrames(reports[0].loop_end, 60));
    SEQLIB_CHECK(reports[1].ok && AtFrames(reports[1].loop_start, 12) && AtFrames(reports[1].loop_end, 36));
    SEQLIB_CHECK(reports[2].ok && AtFrames(reports[2].loop_start, 12) && AtFrames(reports[2].loop_end, 36));
}

void TestLoopLevelsWithDroppedNote()
{
    // Two tracks loop from tick 12 for 24 ticks, at one volume and pan throughout. Track 0 plays a note on square 1 at
    // tick 30 that lasts past the loop's end. Track 1 plays square 1 as its loop starts. The channel is free in the
    // first pass. In the later ones, track 0's note holds it. The driver then drops track 1's note.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(12);
    const size_t first_loop = first.Here();
    first.Wait(18).Note(12, 70, 127).Wait(6).Goto(first_loop);
    Track second;
    second.Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(12);
    const size_t second_loop = second.Here();
    second.Note(6, 60, 127).Wait(24).Goto(second_loop);
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_dropped");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The passes start other notes. The notes that both start play at the same levels. The loop therefore stays at the
    // first pass.
    SEQLIB_CHECK(r.ok && AtFrames(r.loop_start, 12) && AtFrames(r.loop_end, 36));
    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<uint32_t> ons;
    for (const auto& e : m.Find(0x91))
    {
        ons.push_back(uint32_t(e.tick));
    }
    SEQLIB_CHECK((ons == std::vector<uint32_t>{12 * kMidiTicks}));
}

void TestLoopLengths()
{
    // Two tracks loop 36 and 48 ticks from the start, and a third, without a loop, plays a note of 30 ticks at tick 10.
    // The loop starts at tick 40, where that note ends, and lasts until both loops are back where they started, 144
    // ticks, to tick 184, and the song plays it twice.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track three;
    three.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t three_loop = three.Here();
    three.Note(12, 60, 100).Wait(36).Goto(three_loop);
    Track four;
    four.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t four_loop = four.Here();
    four.Note(12, 64, 100).Wait(48).Goto(four_loop);
    Track once;
    once.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(10).Note(30, 67, 100).Wait(40).Fine();
    cart.Song({three, four, once}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(AtFrames(r.loop_start, 40));
    SEQLIB_CHECK(AtFrames(r.loop_end, 184));
    SEQLIB_CHECK(AtFrames(r.seconds, 328));
}

void TestLoopAfterTie()
{
    // In both songs, one track loops 24 ticks from the start, and another, without a loop, holds a tie and ends at tick
    // 96. In the first song only the track's end releases the tie, so the loop starts there. In the second, an end of
    // tie releases it at tick 30, where the loop starts.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track looping;
    looping.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t loop = looping.Here();
    looping.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({looping, Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Tie(64, 100).Wait(96).Fine()}, voices);
    cart.Song({looping, Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Tie(64, 100).Wait(30).Eot(64).Wait(66).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loop_tie");

    const SongReport held = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport ended = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(held.ok && ended.ok);
    SEQLIB_CHECK(AtFrames(held.loop_start, 96));
    SEQLIB_CHECK(AtFrames(held.loop_end, 120));
    SEQLIB_CHECK(AtFrames(ended.loop_start, 30));
    SEQLIB_CHECK(AtFrames(ended.loop_end, 54));
}

// A track's volume is 100 when its loop starts the first time, and the loop sets 60 for its second note. In song 0 the
// loop sets 100 again as it starts, and in song 1 just before it jumps back, in the tick of the loop's end, which the
// next pass starts on. Either way the marked pass gives the levels of volume 100 again at its start: CC10 64 and CC11
// 112. A player that jumps back there from volume 60 then plays at volume 100. In song 2 the loop doesn't set it again.
// The game then plays the later passes' first note at 60, and so does such a player. Nothing is written again.
void TestLoopSettingsAgain()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    for (int song = 0; song < 3; song++)
    {
        Track t;
        t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
        const size_t loop = t.Here();
        if (song == 0)
        {
            t.Cmd(kCmdVol, 100);
        }
        t.Note(12, 60, 100).Wait(12).Cmd(kCmdVol, 60).Note(12, 62, 100).Wait(12);
        if (song == 1)
        {
            t.Cmd(kCmdVol, 100);
        }
        cart.Song({t.Goto(loop)}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loop_again");

    for (int song = 0; song < 3; song++)
    {
        const SongReport r = ConvertSong(rom, info, song, opt, nullptr);

        std::map<int, int> at_start;
        for (const auto& [tick, bytes] : ReadMidi(r.midi_path).Find(0xB0))
        {
            if (tick == 12 * kMidiTicks && (bytes[1] == cc::kPan || bytes[1] == cc::kExpression))
            {
                at_start[bytes[1]] = bytes[2];
            }
        }

        const std::map<int, int> kAgain = {{cc::kPan, 64}, {cc::kExpression, 112}};
        SEQLIB_CHECK(r.ok && AtFrames(r.loop_start, 12));
        SEQLIB_CHECK(at_start == (song < 2 ? kAgain : std::map<int, int>()));
    }
}

void TestSettingsAgain()
{
    // A writer on a shared MIDI channel writes a pan, expression or bend where it changes. After SetSettingsAgain(), it
    // writes them again, the same values included.
    MidiFile file(48);
    ChannelWriter writer(file.AddTrack(), 0, 0, {0, 0}, 2, 0, true);
    for (const uint32_t at : {0u, 10u, 20u})
    {
        if (at == 20)
        {
            writer.SetSettingsAgain();
        }
        writer.SetLevels(at, 0.5, 0.25);
        writer.SetBend(at, 1);
    }
    const MidiEvents m = seqlib::midi::Read(file.Encode());

    std::vector<std::pair<uint64_t, int>> settings;
    for (const auto& [tick, bytes] : m.Find(0xB0))
    {
        if (bytes[1] == cc::kPan || bytes[1] == cc::kExpression)
        {
            settings.emplace_back(tick, bytes[1]);
        }
    }
    std::vector<uint64_t> bends;
    for (const auto& e : m.Find(0xE0))
    {
        bends.push_back(e.tick);
    }

    const std::vector<std::pair<uint64_t, int>> kSettings = {
        {0, cc::kPan}, {0, cc::kExpression}, {20, cc::kPan}, {20, cc::kExpression}};
    SEQLIB_CHECK(settings == kSettings);
    SEQLIB_CHECK(bends == std::vector<uint64_t>({0, 20}));
}

void TestHourLimit()
{
    // A song asked to play its loop of 3000 ticks, 50 s, a hundred times stops where the model gives up, after 216000
    // frames. The loop starts 12 ticks in, so the limit falls in the middle of a note: the song's length, its MIDI file
    // and that note all end there.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
    const size_t loop = t.Here();
    t.Tie(60, 100).Wait(3000).Eot(60).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("hour");
    opt.loops = 100;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto cut_off = [](const std::string& w)
    {
        return w.find("cut off after an hour") != std::string::npos;
    };
    SEQLIB_CHECK(r.ok && std::any_of(r.warnings.begin(), r.warnings.end(), cut_off));
    SEQLIB_CHECK(std::fabs(r.seconds - 216000 / kFrameRate) < 0.1);

    const MidiEvents m = ReadMidi(r.midi_path);
    uint64_t last = 0;
    for (const auto& track : m.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }
    const auto offs = m.Find(0x80);
    SEQLIB_CHECK(last <= 216001 * kMidiTicks);
    SEQLIB_CHECK(!offs.empty() && offs.back().tick >= 215999 * kMidiTicks);
}

void TestSilentEndAfterLastSound()
{
    // A note of 24 ticks at a tick a frame, a wait, a volume change and the end. The voice's release of 0 stops the
    // note's sound a frame after its release. After a wait of 600 ticks, the change comes 9.6 s after the sound has
    // died away, and the song ends at the change. After a wait of 720 ticks, it comes 11.6 s after, more than
    // kMaxSilentEndSeconds. The song then ends a tick after the sound, and its MIDI file holds nothing later.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    for (int wait : {600, 720})
    {
        cart.Song({Track()
                       .Cmd(kCmdTempo, 75)
                       .Cmd(kCmdVoice, 0)
                       .Cmd(kCmdVol, 100)
                       .Note(24, 60, 100)
                       .Wait(wait)
                       .Cmd(kCmdVol, 50)
                       .Fine()},
                  voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const SongReport near = ConvertSong(rom, info, 0, TempSettings("silent_end_near"), nullptr);
    const SongReport far = ConvertSong(rom, info, 1, TempSettings("silent_end_far"), nullptr);

    SEQLIB_CHECK(near.ok && AtFrames(near.seconds, 600));
    SEQLIB_CHECK(far.ok && far.seconds > 24 / kFrameRate && far.seconds < 30 / kFrameRate);
    const MidiEvents m = ReadMidi(far.midi_path);
    uint64_t last = 0;
    for (const auto& track : m.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }
    SEQLIB_CHECK(m.Find(0x90).size() == 1 && last < 30 * kMidiTicks);
}

int RunTests()
{
    TestLoops();
    TestHang();
    TestLoopStarts();
    TestLoopStartsAtSecondPass();
    TestLoopLevelsAtSecondPass();
    TestLoopLevelsWithDroppedNote();
    TestLoopLengths();
    TestLoopAfterTie();
    TestLoopSettingsAgain();
    TestSettingsAgain();
    TestHourLimit();
    TestSilentEndAfterLastSound();

    return seqlib::testing::Summary("mp2k_loops");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
