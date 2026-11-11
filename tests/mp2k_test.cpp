// SPDX-License-Identifier: MIT

// Unit tests for MP2K's reader and its sequencer: detection, timing, notes, channels, envelopes, pitch and the songs'
// settings. mp2k_loops_test.cpp tests the loops and the ends. They build small synthetic cartridges in memory. No game
// ROM is needed.

#include <algorithm>
#include <array>
#include <span>

#include "mp2k_test_util.h"

namespace ntdlib::sequence_export::mp2k
{
namespace
{

void TestDecoding()
{
    Cart cart;
    Track t;
    const size_t start = t.Here();
    t.Cmd(kCmdKeySh, 0)
        .Cmd(kCmdVoice, 5)
        .Raw({0x30})
        .Note(8, 60, 100, 2)
        .Raw({0x3E})
        .Wait(24)
        .Tie(64, 127)
        .Eot(64)
        .Raw({kCmdXcmd, kXcmdEchoVol, 16})
        .Raw({kCmdXcmd, kXcmdWave, 0x56, 0x34, 0x12, 0x08})
        .Raw({kCmdXcmd, kXcmdOffset, 0x40, 0x01, 0x00, 0x00})
        .Patt(start)
        .Rept(2, start)
        .Raw({kCmdMemAcc, 6, 1, 2, 0, 0, 0, 0})
        .Goto(start)
        .Fine();
    const uint32_t at = cart.Place(t.Bytes(kData), 1);
    const Rom rom = cart.ToRom();

    uint32_t a = at;
    uint8_t running = 0;
    Event e;
    auto next = [&]()
    {
        const bool ok = DecodeEvent(rom, a, running, e);
        running = RunningStatus(e, running);
        a += e.size;

        return ok;
    };

    SEQLIB_CHECK(next() && e.command == kCmdKeySh && e.size == 2 && DescribeEvent(e) == "KEYSH 0");
    SEQLIB_CHECK(next() && e.command == kCmdVoice && e.arg[0] == 5);
    SEQLIB_CHECK(next() && e.command == kCmdVoice && e.size == 1 && e.arg[0] == 0x30);
    SEQLIB_CHECK(next() && e.size == 4 && DescribeEvent(e) == "N08 key 60 vel 100 gate+2");
    SEQLIB_CHECK(next() && e.size == 1 && DescribeEvent(e) == "N08 key 62");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "W24");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "TIE key 64 vel 127");
    SEQLIB_CHECK(next() && DescribeEvent(e) == "EOT key 64");
    SEQLIB_CHECK(next() && e.size == 3 && DescribeEvent(e) == "XCMD xIECV 16");
    SEQLIB_CHECK(next() && e.size == 6 && e.target == 0x08123456 && DescribeEvent(e) == "XCMD xWAVE 0x08123456");
    SEQLIB_CHECK(next() && e.size == 6 && e.target == 320 && DescribeEvent(e) == "XCMD x0D 320");
    SEQLIB_CHECK(next() && e.command == kCmdPatt && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdRept && e.size == 6 && e.arg[0] == 2 && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdMemAcc && e.size == 8 && e.target == 0);
    SEQLIB_CHECK(next() && e.command == kCmdGoto && e.target == at);
    SEQLIB_CHECK(next() && e.command == kCmdFine);

    // A byte below 0x80 without a running status, and an extended command the driver doesn't have.
    const uint32_t bad = cart.Place({0x40, kCmdXcmd, 0x20}, 1);
    const Rom bad_rom = cart.ToRom();

    SEQLIB_CHECK(!DecodeEvent(bad_rom, bad, 0, e));
    SEQLIB_CHECK(!DecodeEvent(bad_rom, bad + 1, 0, e));
}

// The address that a new cart gives its first voice group, of one saw. Carts without a voice group of their own use it
// for their songs.
uint32_t SawVoices()
{
    Cart cart;
    return cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
}

void TestDetection()
{
    // The routine that starts a song gives the tables, and the init gives the mixer's settings. The song table ends
    // before the first entry that isn't a song.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
    cart.Song({Track().Fine()}, voices);
    cart.Song({Track().Fine(), Track().Fine()}, voices, 1);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK_EQ(info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.player_table, kPlayerTable);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_REQUIRE_EQ(info.players.size(), 2);
    SEQLIB_CHECK(info.players[1].track_count == 3);
    SEQLIB_CHECK_EQ(info.sound_mode, kMode);
    SEQLIB_CHECK_EQ(info.max_channels, 5);
    SEQLIB_CHECK_EQ(info.master_volume, 12);
    SEQLIB_CHECK_EQ(info.mix_rate, 13379);
    SEQLIB_CHECK_EQ(info.samples_per_frame, 224);
    SEQLIB_CHECK_EQ(info.step_scale, 627);
    SEQLIB_CHECK(info.special_samples);
    SEQLIB_CHECK(!info.mono);
    SEQLIB_CHECK(info.warnings.empty());

    // Overrides take the place of what detection finds.
    DriverOverrides overrides;
    overrides.song_count = 1;
    DriverInfo one;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, overrides, one, error));
    SEQLIB_CHECK_EQ(one.song_count, 1);

    // An all-zero entry, as a GSF rip can leave for a song it doesn't hold, is empty and doesn't end the song table,
    // but those after the last song don't count. A table without a song fails, with a hint.
    Cart gaps;
    for (int s = 0; s < 4; s++)
    {
        gaps.Song({Track().Fine()}, voices);
    }
    for (const uint32_t entry : {kSongTable + 8, kSongTable + 24})
    {
        gaps.Put32(entry, 0);
        gaps.Put32(entry + 4, 0);
    }
    const Rom gaps_rom = gaps.ToRom();
    const Rom empty_rom = Cart().ToRom();
    DriverInfo empty;

    const DriverInfo gaps_info = Detect(gaps_rom);
    const bool found_empty = DetectDriver(empty_rom, DriverOverrides(), empty, error);

    SEQLIB_CHECK_EQ(gaps_info.song_count, 3);
    SEQLIB_CHECK_EQ(SongAddress(gaps_rom, gaps_info, 1), 0);
    SEQLIB_CHECK(SongAddress(gaps_rom, gaps_info, 0) != 0 && SongAddress(gaps_rom, gaps_info, 2) != 0);
    SEQLIB_CHECK(!Sequencer(gaps_rom, gaps_info, 1).Valid() && Sequencer(gaps_rom, gaps_info, 2).Valid());
    SEQLIB_CHECK(!found_empty);
    SEQLIB_CHECK(error.find("--song-count") != std::string::npos);

    // Another mode, an older mixer, and no mode at all: then the driver's defaults stand.
    Cart slow(0x0093F800, {4}, true, false);
    slow.Song({Track().Fine()}, voices);
    Cart unset(0x00000500);
    unset.Song({Track().Fine()}, voices);

    const DriverInfo slow_info = Detect(slow.ToRom());
    const DriverInfo unset_info = Detect(unset.ToRom());

    SEQLIB_CHECK_EQ(slow_info.mix_rate, 10512);
    SEQLIB_CHECK_EQ(slow_info.max_channels, 8);
    SEQLIB_CHECK(!slow_info.special_samples);
    SEQLIB_CHECK_EQ(unset_info.sound_mode, 0);
    SEQLIB_CHECK_EQ(unset_info.max_channels, 8);
    SEQLIB_CHECK_EQ(unset_info.mix_rate, 13379);
    SEQLIB_CHECK(!unset_info.warnings.empty());

}

// The sound modes that a game's code sets: Metroid Fusion's mode kept in r8, a mode set again after the init, the
// rows of games that work their modes out while they run, and modes shifted into place before each call.
void TestSoundModes()
{
    const uint32_t voices = SawVoices();

    // Metroid Fusion's init keeps its mode in r8 for a later call, and the mode's top byte of 1 asks for stereo.
    // Without the move, nothing marks the load as the mode's.
    Cart fusion(0x0193F600, {4});
    fusion.Put16(kInit + 2, 0x4680);
    fusion.Put16(kInit + 4, 0);
    fusion.Song({Track().Fine()}, voices);
    Cart loose(0x0193F600, {4});
    loose.Put16(kInit + 2, 0);
    loose.Put16(kInit + 4, 0);
    loose.Song({Track().Fine()}, voices);

    const DriverInfo fusion_info = Detect(fusion.ToRom());
    const DriverInfo loose_info = Detect(loose.ToRom());

    SEQLIB_CHECK_EQ(fusion_info.sound_mode, 0x0193F600);
    SEQLIB_CHECK_EQ(fusion_info.mix_rate, 10512);
    SEQLIB_CHECK_EQ(fusion_info.max_channels, 6);
    SEQLIB_CHECK_EQ(fusion_info.master_volume, 15);
    SEQLIB_CHECK_EQ(loose_info.sound_mode, 0);

    // A game's code can set the mode again straight after the init: bl m4aSoundInit; ldr r0, =mode; bl m4aSoundMode.
    // Each field of that mode that isn't 0 replaces the init's. A mode that the code sets elsewhere doesn't count.
    Cart again;
    again.GameSoundMode(0x00960680, true);
    again.Song({Track().Fine()}, voices);
    const DriverInfo again_info = Detect(again.ToRom());
    SEQLIB_CHECK_EQ(again_info.sound_mode, kMode);
    SEQLIB_CHECK_EQ(again_info.max_channels, 6);
    SEQLIB_CHECK_EQ(again_info.master_volume, 12);
    SEQLIB_CHECK_EQ(again_info.mix_rate, 18157);
    Cart elsewhere;
    elsewhere.GameSoundMode(0x00960680, false);
    elsewhere.Song({Track().Fine()}, voices);
    SEQLIB_CHECK_EQ(Detect(elsewhere.ToRom()).max_channels, 5);

    // A row gives the mode of a game that works it out while it runs.
    Cart spyro;
    spyro.GameCode("ASYE");
    spyro.Song({Track().Fine()}, voices);
    const DriverInfo spyro_info = Detect(spyro.ToRom());
    SEQLIB_CHECK_EQ(spyro_info.max_channels, 8);
    SEQLIB_CHECK_EQ(spyro_info.master_volume, 10);
    SEQLIB_CHECK_EQ(spyro_info.mix_rate, 13379);

    // A game's code can also set the mode straight after the init with movs r0, #imm; lsls r0, r0, #n before each call
    // of m4aSoundMode, after calls of up to two other routines. The calls apply in order. The cart's init sets 5
    // channels and a master volume of 12.
    struct ShiftedCase
    {
        std::vector<Cart::ShiftedMode> modes;
        int other_calls;
        int channels;
        int master_volume;
    };
    const ShiftedCase kShiftedCases[] = {
        {{{0xC0, 3}}, 0, 6, 12},            // The Proud Family: 6 channels
        {{{0xC0, 4}, {0xC0, 7}}, 1, 12, 6}, // Sugar Sugar Rune: 12 channels, then a master volume of 6
        {{{0x80, 4}, {0x80, 8}}, 2, 8, 8},
        {{{0xC0, 3}}, 3, 5, 12}, // past the calls that count
    };
    for (const ShiftedCase& c : kShiftedCases)
    {
        Cart game;
        game.GameShiftedSoundModes(c.modes, c.other_calls);
        game.Song({Track().Fine()}, voices);

        const DriverInfo game_info = Detect(game.ToRom());

        SEQLIB_CHECK_EQ(game_info.max_channels, c.channels);
        SEQLIB_CHECK_EQ(game_info.master_volume, c.master_volume);
    }

    // A row gives the mode that Strawberry Shortcake: Sweet Dreams sets each time it starts a song.
    Cart sweet_dreams;
    sweet_dreams.GameCode("B4TE");
    sweet_dreams.Song({Track().Fine()}, voices);

    const DriverInfo sweet_dreams_info = Detect(sweet_dreams.ToRom());

    SEQLIB_CHECK_EQ(sweet_dreams_info.max_channels, 6);
    SEQLIB_CHECK_EQ(sweet_dreams_info.master_volume, 12);
}

// The channels that games set for each song, a compiler's swapped registers, a cartridge without the driver, and a
// player table with an entry of zeros.
void TestDetectionVariants()
{
    const uint32_t voices = SawVoices();

    // Fire Emblem and Kuru Kuru Kururin set the DirectSound channels for the song they start.
    Cart emblem;
    emblem.GameCode("AE7E");
    emblem.PutHalfwords(0x08003FC0,
                        {0xB580, 0xB081, 0x466F, 0x6038, 0x6838, 0x285A, 0xD010, 0x285A, 0xDC04, 0x282A, 0xDC18,
                         0x2829, 0xDB16, 0xE009, 0x285F, 0xD007, 0x285F, 0xDC02, 0x285C, 0xD003, 0xE00E, 0x2874,
                         0xD000, 0xE00B, 0x4804, 0x2108, 0x5641, 0x2908, 0xD002, 0x2008, 0xF7FF, 0xFFC6});
    emblem.Song({Track().Fine()}, voices);
    const Rom emblem_rom = emblem.ToRom();
    const DriverInfo emblem_info = Detect(emblem_rom);
    SEQLIB_CHECK_EQ(SongChannels(emblem_rom, emblem_info, 41), 8);
    SEQLIB_CHECK_EQ(SongChannels(emblem_rom, emblem_info, 40), emblem_info.max_channels);
    Cart kururin;
    kururin.GameCode("AKRP");
    kururin.Put32(0x0801EF6C + 4, 2);
    kururin.Song({Track().Fine()}, voices);
    const Rom kururin_rom = kururin.ToRom();
    const DriverInfo kururin_info = Detect(kururin_rom);
    SEQLIB_CHECK_EQ(SongChannels(kururin_rom, kururin_info, 1), 2);
    SEQLIB_CHECK_EQ(SongChannels(kururin_rom, kururin_info, 24), kururin_info.max_channels);
    SEQLIB_CHECK_EQ(Sequencer(kururin_rom, kururin_info, 1).DirectChannels(), 2);

    // Some games' compiler swaps r2 and r3 in the routine that starts a song. The mono mixer's volume code marks a
    // mixer that plays every channel on both sides.
    Cart swapped;
    swapped.Song({Track().Fine()}, voices);
    const uint16_t kSwapped[] = {0x4B08, 0x4909, 0x0B40, 0x1840, 0x8882, 0x0051, 0x1889, 0x0089, 0x18C9};
    for (uint32_t i = 0; i < 9; i++)
    {
        swapped.Put16(kSongStart + 4 + 2 * i, kSwapped[i]);
    }
    const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
    for (uint32_t i = 0; i < 12; i++)
    {
        swapped.Put8(kMonoVolume + i, kMono[i]);
    }

    const DriverInfo swapped_info = Detect(swapped.ToRom());

    SEQLIB_CHECK_EQ(swapped_info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(swapped_info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(swapped_info.player_table, kPlayerTable);
    SEQLIB_CHECK(swapped_info.mono);

    // A cartridge without the driver's code fails, unless the song table is given.
    Cart bare(kMode, {10}, false);
    bare.Song({Track().Fine()}, voices);
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = kSongTable;
    DriverInfo none;
    // The failure clears an earlier message.
    std::string error = "an earlier message";

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error) && error.empty());
    SEQLIB_CHECK(DetectDriver(bare_rom, table, none, error));
    SEQLIB_CHECK_EQ(none.song_count, 1);
    SEQLIB_CHECK_EQ(SongPlayer(bare_rom, none, 0).track_count, 16);
    // An entry of zeros in the player table is a player with no tracks. The players after it still count. A song plays
    // no more tracks than its player has. Song 82 of Advance Wars is such a song.
    Cart gap(kMode, {10, 1});
    for (uint32_t i = 0; i < 12; i += 4)
    {
        gap.Put32(kPlayerTable + i, 0);
    }
    gap.Song({Track().Fine(), Track().Fine()}, voices, 1);
    const Rom gap_rom = gap.ToRom();

    const DriverInfo gap_info = Detect(gap_rom);

    SEQLIB_REQUIRE_EQ(gap_info.players.size(), 2);
    SEQLIB_CHECK_EQ(gap_info.players[0].track_count, 0);
    SEQLIB_CHECK_EQ(SongPlayer(gap_rom, gap_info, 0).track_count, 1);
}

void TestUnusedPlayers()
{
    // The music player table's entry 1 is zeros: an unused player. Detection reads on past it. A song on player 2 plays
    // as many of its tracks as player 2 has room for. A song on the unused player plays nothing.
    Cart cart(kMode, {10, 3, 2});
    for (uint32_t i = 0; i < 12; i += 4)
    {
        cart.Put32(kPlayerTable + 12 + i, 0);
    }
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    const Track track = Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine();
    cart.Song({track, track, track}, voices, 2);
    cart.Song({track}, voices, 1);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("unused_player");

    SEQLIB_REQUIRE_EQ(info.players.size(), 3);
    SEQLIB_CHECK(info.players[1].track_count == 0 && info.players[2].track_count == 2);
    SEQLIB_CHECK_EQ(info.song_count, 2);
    SEQLIB_CHECK(Sequencer(rom, info, 0).Valid() && Sequencer(rom, info, 0).TrackCount() == 2);
    SEQLIB_CHECK(!Sequencer(rom, info, 1).Valid());
    SEQLIB_CHECK(!ConvertSong(rom, info, 1, opt, nullptr).ok);
}

void TestTiming()
{
    // The driver adds the tempo to a counter each frame, and plays a tick for each 150 in it. At the start, before a
    // tempo command, it adds 150. A tempo command of 75 sets a tempo of 150 (150 quarter notes a minute at 60 frames a
    // second), so a tick is a frame. One of 60 sets 120, so a tick takes 1.25 frames, and tick 24 is on frame 30.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(24).Note(24, 60, 100).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 60).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(24).Note(24, 60, 100).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer fast(rom, info, 0);
    Sequencer slow(rom, info, 1);
    const std::vector<Action> fast_notes = Actions(fast, 40, Action::kNoteOn);
    const std::vector<Action> slow_notes = Actions(slow, 40, Action::kNoteOn);

    SEQLIB_CHECK(fast_notes.size() == 1 && fast_notes[0].frame == 24 && fast_notes[0].tick == 24);
    SEQLIB_CHECK(slow_notes.size() == 1 && slow_notes[0].frame == 30 && slow_notes[0].tick == 24);
}

void TestNotes()
{
    // A note of 24 ticks is released 24 ticks after it starts, a tied note at its end command, and a note with an extra
    // length that much later.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 100)
                   .Wait(48)
                   .Tie(62, 100)
                   .Wait(12)
                   .Eot(62)
                   .Wait(12)
                   .Note(12, 64, 100, 3)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> ons, releases;
    for (int f = 0; f < 120; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                ons.push_back(a);
            }
            if (a.kind == Action::kRelease)
            {
                releases.push_back(a);
            }
        }
    }

    SEQLIB_REQUIRE_EQ(ons.size(), 3);
    SEQLIB_REQUIRE_EQ(releases.size(), 3);
    SEQLIB_CHECK(ons[0].tick == 0 && ons[1].tick == 48 && ons[2].tick == 72);
    SEQLIB_CHECK(releases[0].tick == 24 && releases[1].tick == 60 && releases[2].tick == 87);
    SEQLIB_CHECK(ons[0].key == 60 && ons[0].velocity == 100 && ons[0].program == 0 && ons[0].channel == 0);
}

void TestChannels()
{
    // Two DirectSound channels. Track 0, of priority 10, holds both. A note of track 1, of priority 0, finds none it
    // can take. Once track 0 releases a note, track 1 takes its channel, released notes first.
    Cart cart(0x00940200);
    const uint32_t kSquare = 0;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {1, Cart::Voice(1, kSquare, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdPrio, 10)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Tie(62, 100)
                   .Wait(2)
                   .Eot(60)
                   .Wait(10)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Wait(1).Tie(64, 100).Wait(2).Tie(65, 100).Wait(10).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(70, 100).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1).Tie(72, 100).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> actions;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            actions.push_back(a);
        }
    }

    std::vector<std::pair<int, int>> ons, dropped;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            ons.push_back({a.key, a.channel});
        }
        if (a.kind == Action::kNoteDropped)
        {
            dropped.push_back({a.key, int(a.tick)});
        }
    }

    // Keys 60 and 62 take channels 0 and 1, and 70 takes the first PSG channel. Key 64 and track 3's 72, of the same
    // priority as 70 but from a later track, are dropped. Key 65 takes the released key 60's channel.
    const std::vector<std::pair<int, int>> kOns = {{60, 0}, {62, 1}, {70, kFirstPsgChannel}, {65, 0}};
    const std::vector<std::pair<int, int>> kDropped = {{64, 1}, {72, 1}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK(dropped == kDropped);
}

// The song of TestChannels with the channels grown: the notes that the driver left out take grown channels after the
// console's 16. Track 1's key 64 takes DirectSound channel 16, and track 3's 72 a square 1 in PSG unit 1. Key 65 takes
// channel 0, as on the console: key 60 there is in its release. The grown square writes the unit's registers as the
// driver writes the console's: its duty and length, its pitch and its envelope, and its outputs in the unit's NR51.
void TestGrownChannels()
{
    Cart cart(0x00940200);
    const uint32_t kSquare = 0;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {1, Cart::Voice(1, kSquare, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdPrio, 10)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Tie(62, 100)
                   .Wait(2)
                   .Eot(60)
                   .Wait(10)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Wait(1).Tie(64, 100).Wait(2).Tie(65, 100).Wait(10).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(70, 100).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1).Tie(72, 100).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.GrowChannels(true);
    std::vector<Action> actions;
    std::vector<ntdlib::agb::RegisterWrite> unit_writes;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            actions.push_back(a);
        }
        for (const ntdlib::agb::RegisterWrite& write : seq.SoundWrites())
        {
            if (write.address >= ntdlib::agb::kGrownPsgRegisters)
            {
                unit_writes.push_back(write);
            }
        }
    }

    std::vector<std::pair<int, int>> ons;
    int dropped = 0;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            ons.push_back({a.key, a.channel});
        }
        dropped += a.kind == Action::kNoteDropped;
    }
    const std::vector<std::pair<int, int>> kOns = {
        {60, 0}, {62, 1}, {70, kFirstPsgChannel}, {64, kChannelCount}, {72, kChannelCount + 1}, {65, 0}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK(dropped == 0);
    SEQLIB_REQUIRE(seq.ChannelCount() == kChannelCount + 2);
    const Channel& square = seq.GetChannel(kChannelCount + 1);
    SEQLIB_CHECK(square.part == 1 && square.unit == 1);
    SEQLIB_CHECK(seq.GetChannel(kChannelCount).part == 0);

    // The unit's NR11 takes the duty, its NR13 the low byte of key 72's pitch, and its NR51 bits 0 and 4. Unit 2 takes
    // no store.
    const auto wrote = [&](uint32_t offset, uint32_t value)
    {
        return std::any_of(unit_writes.begin(), unit_writes.end(), [&](const ntdlib::agb::RegisterWrite& w)
                           { return w.address == ntdlib::agb::kGrownPsgRegisters + offset && w.value == value; });
    };
    const uint32_t pitch = KeyToPsgFrequency(1, 72, 0);
    SEQLIB_CHECK(wrote(0x02, 0x00));
    SEQLIB_CHECK(wrote(0x04, pitch & 0xFF));
    SEQLIB_CHECK(wrote(0x21, 0x11));
    SEQLIB_CHECK(std::none_of(unit_writes.begin(), unit_writes.end(), [](const ntdlib::agb::RegisterWrite& w)
                              { return w.address >= ntdlib::agb::kGrownPsgRegisters + ntdlib::agb::kGrownPsgBytes; }));

    // Without growth, the song plays as TestChannels has it.
    Sequencer console(rom, info, 0);
    int console_dropped = 0;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : console.Step())
        {
            console_dropped += a.kind == Action::kNoteDropped;
        }
    }
    SEQLIB_CHECK(console_dropped == 2 && console.ChannelCount() == kChannelCount);
}

void TestEnvelope()
{
    // An attack of 100 a frame, a decay of 128/256 a frame to a sustain level of 64, and a release of 128/256 a frame
    // after the note's 96 ticks. With an echo volume of 16 and an echo length of 3, the release holds at 16 for 3
    // frames and then stops.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {100, 128, 64, 128})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(96, 60, 127).Wait(96).Wait(24).Fine()},
        voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Raw({kCmdXcmd, kXcmdEchoVol, 16, kCmdXcmd, kXcmdEchoLen, 3})
                   .Note(96, 60, 127)
                   .Wait(96)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The envelope's level after each frame: frames 0-4, then 96-103.
    auto levels = [&](int song)
    {
        Sequencer seq(rom, info, song);
        std::vector<int> out;
        for (int f = 0; f < 104; f++)
        {
            seq.Step();
            if (f < 5 || f >= 96)
            {
                out.push_back(seq.GetChannel(0).status ? seq.GetChannel(0).envelope : -1);
            }
        }

        return out;
    };

    const std::vector<int> kPlain = {100, 200, 255, 127, 64, 32, 16, 8, 4, 2, 1, -1, -1};
    const std::vector<int> kEcho = {100, 200, 255, 127, 64, 32, 16, 16, 16, -1, -1, -1, -1};
    SEQLIB_CHECK(levels(0) == kPlain);
    SEQLIB_CHECK(levels(1) == kEcho);
}

void TestEnvelopeHold()
{
    // The frame in which a sample note's attack reaches 255 plays at 255, and the decay starts on the next frame. The
    // SoundFont holds the full level for that frame where the decay changes the level: below a sustain of 255. A PSG
    // voice's envelope steps on the hardware's counter instead.
    const DriverInfo info;
    Voice sample;
    sample.attack = 255;
    sample.sustain = 128;
    Voice full = sample;
    full.sustain = 255;
    Voice square;
    square.type = 1;
    square.attack = 0;
    square.decay = 2;
    square.sustain = 7;

    const VolumeEnvelope held = EnvelopeFor(info, sample);
    const VolumeEnvelope sustained = EnvelopeFor(info, full);
    const VolumeEnvelope psg = EnvelopeFor(info, square);

    SEQLIB_CHECK_EQ(held.hold, Timecents(1.0 / kFrameRate));
    SEQLIB_CHECK_EQ(held.decay, kInstant);
    SEQLIB_CHECK_EQ(sustained.hold, kInstant);
    SEQLIB_CHECK_EQ(psg.hold, kInstant);
}

void TestPsgEnvelopeBytes()
{
    // The driver counts a PSG voice's whole attack, decay and release bytes. An attack of 10 takes 10 counts a step, 16
    // counts every 15 frames. A sustain above 15 never falls below the full level. The zone holds it.
    const DriverInfo info;
    Voice slow;
    slow.type = 2;
    slow.attack = 10;
    slow.decay = 10;
    slow.sustain = 18;
    slow.release = 10;
    Voice fast = slow;
    fast.release = 2;

    const VolumeEnvelope env = EnvelopeFor(info, slow);
    const VolumeEnvelope fast_env = EnvelopeFor(info, fast);

    SEQLIB_CHECK_EQ(env.attack, Timecents(15.0 * 10 / (kFrameRate * 16 / 15)));
    SEQLIB_CHECK_EQ(env.decay, kInstant);
    SEQLIB_CHECK_EQ(env.sustain, 0);
    SEQLIB_CHECK(env.release > fast_env.release);
}

void TestPitch()
{
    // A sample's rate for a key: its rate at key 60, in 1/1024 Hz, times 2^(n/12), with a fine tune that runs in a
    // straight line in rate to the next key.
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 60, 0), 13379);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 72, 0), 26758);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 48, 0), 6689);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 60, 128), 13776);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 66, 0), 18920);

    // The PSG's square settings: C4 is 1547, and keys up to 36 all play C2. The noise settings run from 0xD7 at key 21
    // to 0 at key 80.
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 60, 0), 1547);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 36, 0), 44);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(1, 20, 0), 44);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 21, 0), 0xD7);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 26, 0), 0xC6);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 77, 0), 0x03);
    SEQLIB_CHECK_EQ(KeyToPsgFrequency(4, 90, 0), 0x00);

    // A bend of +16 with a range of 12 raises the pitch by 3 semitones, and the channel's rate follows at the end of
    // the frame.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 100)
                   .Wait(2)
                   .Cmd(kCmdBendR, 12)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.Step();
    const uint32_t before = seq.GetChannel(0).frequency;
    seq.Step();
    seq.Step();
    const uint32_t after = seq.GetChannel(0).frequency;

    SEQLIB_CHECK_EQ(before, 13379);
    SEQLIB_CHECK_EQ(after, KeyToFrequency(13379 * 1024, 63, 0));
}

void TestLevelReports()
{
    // A track sets volume 100 and pan 0x50, and a tick later volume 1 and pan 0x20.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 100)
                   .Cmd(kCmdPan, 0x50)
                   .Wait(1)
                   .Cmd(kCmdVol, 1)
                   .Cmd(kCmdPan, 0x20)
                   .Wait(1)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);

    const std::vector<Action> reports = Actions(seq, 4, Action::kLevels);

    // Volume 100 gives a level of 200. Pan 0x50 gives the left 95/256 of it and the right 160/256: 74 and 125, rounded
    // down. Volume 1 gives a level of 2, and pan 0x20 gives the left 191/256 of it and the right 64/256. Rounded down,
    // the left plays at 1 and the right not at all.
    SEQLIB_REQUIRE_EQ(reports.size(), 2);
    SEQLIB_CHECK(reports[0].tick == 0 && reports[0].left == 74 && reports[0].right == 125);
    SEQLIB_CHECK(reports[1].tick == 1 && reports[1].left == 1 && reports[1].right == 0);
}

void TestDroppedNotes()
{
    // With one DirectSound channel, the second of two notes at once is left out of the MIDI file, as in the game. After
    // a tempo command of 150, a frame plays two ticks, so a note of one tick that starts on a frame's first tick is
    // released before the mixer has started it. It never sounds, and it's left out too.
    Cart cart(0x00940100);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 150)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 100)
                   .Wait(25)
                   .Note(1, 64, 100)
                   .Wait(24)
                   .Fine(),
               Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 62, 100).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("dropped");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK(r.ok && r.warnings.size() == 1 && r.warnings[0].find("1 note had no available sound channel") == 0);
    SEQLIB_CHECK(m.Find(0x90).size() == 1 && m.Find(0x91).empty());
}

void TestSongChannels()
{
    // Before The Sacred Stones starts a song up to 127, its routine at 0x080028FC gives songs 1, 2, 64, 66, 67, 86 and
    // 116 8 DirectSound channels, and the other songs the init's number. The cart's init gives one. Song 0 then plays
    // one of its two notes at once, and song 1 plays both. A routine with other code gives no song another number.
    const std::vector<uint16_t> kRoutine = {0xB500, 0x2843, 0xDC08, 0x2842, 0xDA0A, 0x2801, 0xDB14, 0x2802, 0xDD06,
                                            0x2840, 0xD004, 0xE00F, 0x2856, 0xD001, 0x2874, 0xD10B, 0x4804, 0x7A00,
                                            0x0600, 0x1600, 0x2808, 0xD00E, 0x2008, 0xF7FF, 0xFFDD};

    auto make = [&](uint16_t first)
    {
        Cart cart(0x00940100);
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
        for (int s = 0; s < 2; s++)
        {
            cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 100).Wait(24).Fine(),
                       Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 62, 100).Wait(24).Fine()},
                      voices);
        }
        cart.GameCode("BE8E");
        cart.PutHalfwords(0x080028FC, kRoutine);
        cart.Put16(0x080028FC, first);

        return cart.ToRom();
    };

    const Rom rom = make(kRoutine[0]);
    const Rom other_rom = make(0xB510);
    const DriverInfo info = Detect(rom);
    const DriverInfo other_info = Detect(other_rom);
    ConvertSettings opt = TempSettings("song_channels");

    const SongReport r0 = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport r1 = ConvertSong(rom, info, 1, opt, nullptr);
    const SongReport other = ConvertSong(other_rom, other_info, 1, opt, nullptr);

    SEQLIB_CHECK_EQ(SongChannels(rom, info, 0), 1);
    SEQLIB_CHECK_EQ(SongChannels(rom, info, 1), 8);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "songs 1, 2, 64, 66, 67, 86 and 116: 8 DirectSound channels (the game's routine at "
                           "0x080028FC)") != info.log.end());
    SEQLIB_CHECK(r0.ok && r0.warnings.size() == 1 && r0.warnings[0].find("1 note had no available sound channel") == 0);
    SEQLIB_CHECK(r1.ok && r1.warnings.empty());
    SEQLIB_CHECK(other_info.song_channels.songs.empty() && SongChannels(other_rom, other_info, 1) == 1);
    SEQLIB_CHECK(other.ok && other.warnings.size() == 1);
}

void TestSongSettings()
{
    // Steel Empire gives the player of song 1 a volume of 0x122, a volX of 72. Volume 100 then gives a level of 225.
    // Song 0's 0x100 gives 200. A pan of 0 gives the left 127/256 of the level and the right 128/256, rounded down.
    auto make = [](std::string_view code, uint8_t reverb)
    {
        Cart cart;
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
        for (int s = 0; s < 5; s++)
        {
            cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Fine()}, voices, 0, 0,
                      s == 2 ? reverb : 0);
        }
        cart.GameCode(code);

        return cart.ToRom();
    };

    const Rom steel_rom = make("BKTP", 0);
    const DriverInfo steel_info = Detect(steel_rom);
    Sequencer quiet(steel_rom, steel_info, 0);
    Sequencer loud(steel_rom, steel_info, 1);
    const std::vector<Action> quiet_levels = Actions(quiet, 1, Action::kLevels);
    const std::vector<Action> loud_levels = Actions(loud, 1, Action::kLevels);
    SEQLIB_CHECK_EQ(SongVolume(steel_info, 0), kFullPlayerVolume);
    SEQLIB_CHECK_EQ(SongVolume(steel_info, 1), 0x122);
    SEQLIB_REQUIRE(quiet_levels.size() == 1 && loud_levels.size() == 1);
    SEQLIB_CHECK(quiet_levels[0].left == 99 && quiet_levels[0].right == 100);
    SEQLIB_CHECK(loud_levels[0].left == 111 && loud_levels[0].right == 112);
    const std::string louder =
        "songs 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13 and 14: player volume 0x122 (the game's code)";
    SEQLIB_CHECK(std::find(steel_info.log.begin(), steel_info.log.end(), louder) != steel_info.log.end());

    // Mr. Driller 2 plays song 1 with a reverb of 50. Song 2's header sets a reverb of 5. The song keeps it. Song 0
    // plays with the driver's reverb.
    const Rom driller_rom = make("BR2E", 0x85);
    const DriverInfo driller_info = Detect(driller_rom);
    ntdlib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ReadSong(driller_rom, driller_info, 1, header));
    SEQLIB_CHECK_EQ(SongReverb(driller_info, 1, header), 50);
    SEQLIB_REQUIRE(ReadSong(driller_rom, driller_info, 2, header));
    SEQLIB_CHECK_EQ(SongReverb(driller_info, 2, header), 5);
    SEQLIB_REQUIRE(ReadSong(driller_rom, driller_info, 0, header));
    SEQLIB_CHECK_EQ(SongReverb(driller_info, 0, header), driller_info.reverb);

    // The Barbie Diaries: High School Mystery plays song 3 with 6 DirectSound channels and song 4 with 8. Song 0 keeps
    // the init's number.
    const Rom barbie_rom = make("BBIE", 0);
    const DriverInfo barbie_info = Detect(barbie_rom);
    SEQLIB_CHECK_EQ(SongChannels(barbie_rom, barbie_info, 3), 6);
    SEQLIB_CHECK_EQ(SongChannels(barbie_rom, barbie_info, 4), 8);
    SEQLIB_CHECK_EQ(SongChannels(barbie_rom, barbie_info, 0), barbie_info.max_channels);

    // Another game's songs get none of these settings.
    const Rom other_rom = make("XXXX", 0);
    const DriverInfo other_info = Detect(other_rom);
    SEQLIB_CHECK(other_info.song_settings.empty());
    SEQLIB_CHECK_EQ(SongVolume(other_info, 1), kFullPlayerVolume);
}

void TestDoubledNote()
{
    // Two notes of one key that a track starts on one tick make one MIDI note, which goes on as long as either of them
    // sounds. After a tempo command of 150, the note of one tick is released before the mixer has started it, but the
    // note of 24 ticks still plays.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 11025, 32), 60, {255, 0, 255, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 150)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 100)
                   .Wait(1)
                   .Note(1, 60, 100)
                   .Note(24, 60, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("doubled");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    const auto ons = m.Find(0x90);
    const auto offs = m.Find(0x80);
    SEQLIB_CHECK(ons.size() == 1 && offs.size() == 1);
    if (ons.size() == 1 && offs.size() == 1)
    {
        SEQLIB_CHECK(ons[0].tick == kMidiTicks && ons[0].bytes[1] == 60);
        SEQLIB_CHECK_EQ(offs[0].tick, 25 * kMidiTicks);
    }
}

// Returns the microseconds a quarter note lasts when a tick takes `frames` frames.
uint32_t Micros(double frames)
{
    return uint32_t(std::lround(1e6 * 24 * frames / kFrameRate));
}

void TestTempoChange()
{
    // At a tempo of 200, a frame adds 200 to the tick counter. Tick 2 runs in frame 2 with 100 of the counter left
    // over, so a change to a tempo of 50 there makes tick 3 come a frame later, in frame 3, rather than 3 frames later.
    // The MIDI file's tick 2 takes the 100 at the old tempo and the other 50 at the new one: 1.5 frames.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 100)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Wait(2)
                   .Cmd(kCmdTempo, 25)
                   .Wait(1)
                   .Note(1, 60, 100)
                   .Wait(2)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("tempo");

    Sequencer seq(rom, info, 0);
    const std::vector<Action> notes = Actions(seq, 10, Action::kNoteOn);
    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(notes.size() == 1 && notes[0].tick == 3 && notes[0].frame == 3);
    const std::vector<seqlib::midi::Tempo> kTempos = {
        {.track = 0, .tick = 0, .micros_per_quarter = Micros(0.75)},
        {.track = 0, .tick = 2 * kMidiTicks, .micros_per_quarter = Micros(1.5)},
        {.track = 0, .tick = 3 * kMidiTicks, .micros_per_quarter = Micros(3)}};
    SEQLIB_CHECK(seqlib::midi::Tempos(ReadMidi(r.midi_path)) == kTempos);
    SEQLIB_CHECK(AtFrames(r.seconds, 2 * 0.75 + 1.5 + 2 * 3));
}

void TestPsgPitch()
{
    // A noise note bent up 3 semitones plays the noise setting 3 keys up, 1.6 times as fast, so the MIDI file bends it
    // by 8.14 semitones, with a range of 9. A square note at key 38 bent down 4 semitones plays at key 36, the lowest,
    // so the MIDI file bends it by 2.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(4, 0, 60, {0, 0, 15, 0})}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 12)
                   .Tie(60, 127)
                   .Wait(1)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Eot(60)
                   .Fine(),
               Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 8)
                   .Tie(38, 127)
                   .Wait(1)
                   .Cmd(kCmdBend, 0x20)
                   .Wait(4)
                   .Eot(38)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_pitch");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const MidiEvents m = ReadMidi(r.midi_path);
    std::map<int, int> ranges;
    for (int ch = 0; ch < 2; ch++)
    {
        for (const auto& e : m.Find(uint8_t(0xB0 | ch)))
        {
            ranges[ch] = e.bytes[1] == cc::kDataEntry ? e.bytes[2] : ranges[ch];
        }
    }

    const auto noise = m.Find(0xE0);
    const auto square = m.Find(0xE1);
    const int noise_bend = noise.empty() ? 0 : noise.back().bytes[1] | (noise.back().bytes[2] << 7);
    const int square_bend = square.empty() ? -1 : square.back().bytes[1] | (square.back().bytes[2] << 7);
    const int kExpected = 8192 + int(std::lround(256.0 * 12.0 * std::log2(1.6) * 8192 / (9 * 256)));

    SEQLIB_CHECK(ranges[0] == 9 && ranges[1] == 2);
    SEQLIB_CHECK(std::abs(noise_bend - kExpected) <= 1);
    SEQLIB_CHECK_EQ(square_bend, 0);
}

void TestPsgRelease()
{
    // A noise note bent up 3 semitones plays the noise setting 3 keys up, which the MIDI file bends up 8.14 semitones,
    // with a range of 9. Its release goes on at that pitch after the note ends on tick 6, so the bend stays there until
    // the track's next note, a sample on tick 24, which plays with the track's bend of 3 semitones.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(4, 0, 60, {0, 0, 15, 4})}, {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdBendR, 12)
                   .Cmd(kCmdBend, 0x50)
                   .Note(6, 60, 127)
                   .Wait(24)
                   .Cmd(kCmdVoice, 1)
                   .Note(12, 62, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_release");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto bends = ReadMidi(r.midi_path).Find(0xE0);
    const int kNoise = 8192 + int(std::lround(256.0 * 12.0 * std::log2(1.6) * 8192 / (9 * 256)));
    const int kSample = 8192 + int(std::lround(768.0 * 8192 / (9 * 256)));

    SEQLIB_CHECK(r.ok);
    SEQLIB_REQUIRE_EQ(bends.size(), 2);
    SEQLIB_CHECK(bends[0].tick == 0 && bends[1].tick == 24 * kMidiTicks);
    SEQLIB_CHECK(std::abs((bends[0].bytes[1] | (bends[0].bytes[2] << 7)) - kNoise) <= 1);
    SEQLIB_CHECK((bends[1].bytes[1] | (bends[1].bytes[2] << 7)) == kSample);
}

void TestQuietPsg()
{
    // A PSG note's level is its two volumes added up and divided by 16. At a velocity of 32 and a track volume of 31,
    // that rounds to 0, so the note never sounds and the MIDI file leaves it out. At a velocity of 96 it sounds.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 31)
                   .Note(12, 60, 32)
                   .Wait(12)
                   .Note(12, 62, 96)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("quiet");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    const auto ons = ReadMidi(r.midi_path).Find(0x90);
    SEQLIB_CHECK(ons.size() == 1 && ons[0].tick == 12 * kMidiTicks && ons[0].bytes[1] == 62);
}

void TestPsgLevels()
{
    // At a track volume of 127, the MIDI file plays a PSG note of velocity v at level 15v/127, and the hardware at the
    // driver's whole number: its two volumes added up and divided by 16, rounded down. The MIDI velocity's square
    // follows v times the hardware's level against the MIDI file's.
    //  - A square at velocity 64 plays at level 7, not 7.56. The velocity of 59.27 gives a MIDI velocity of 87, not 90.
    //  - The wave channel at velocity 36 plays at level 4 in the driver, which NR32 plays at 25%. A pattern of 3 and 14
    //    plays as 0 and 3 at 25%, at 0.273 of the pattern's level rather than 0.25. That's level 4.09, not 4.25: a
    //    MIDI velocity of 66, not 68.
    //  - At velocity 95, the driver's level of 11 plays at 75%. A pattern of 0 and 15 plays as 0 and 11 there: level
    //    11, not 11.22. That's a MIDI velocity of 109, not 110.
    //  - At velocity 127, the wave channel plays its full volume, and the MIDI velocity stays 127.
    Cart cart;
    const uint32_t three_fourteen = cart.Place(std::vector<uint8_t>(16, 0x3E));
    const uint32_t zero_fifteen = cart.Place(std::vector<uint8_t>(16, 0x0F));
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {1, Cart::Voice(3, three_fourteen, 60, {0, 0, 15, 0})},
                                             {2, Cart::Voice(3, zero_fifteen, 60, {0, 0, 15, 0})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(12, 60, 64).Wait(12).Fine(),
               Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 62, 36)
                   .Wait(12)
                   .Cmd(kCmdVoice, 2)
                   .Note(12, 64, 95)
                   .Wait(12)
                   .Note(12, 65, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("psg_levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // Each track plays on a MIDI channel of its own.
    std::map<int, int> velocities;
    const MidiEvents midi = ReadMidi(r.midi_path);
    for (const uint8_t status : {uint8_t(0x90), uint8_t(0x91)})
    {
        for (const auto& on : midi.Find(status))
        {
            velocities[on.bytes[1]] = on.bytes[2];
        }
    }
    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(velocities == (std::map<int, int>{{60, 87}, {62, 66}, {64, 109}, {65, 127}}));
}

void TestDump()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0);
    const size_t loop = t.Here();
    t.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const std::string path = Utf8(TempPath("mp2k_dump.txt"));
    std::string error;

    SEQLIB_CHECK(DumpSong(rom, info, 0, path, error));

    const std::vector<uint8_t> text = ReadAll(path);
    const std::string s(text.begin(), text.end());
    SEQLIB_CHECK(s.find("TEMPO 75 (150 BPM)") != std::string::npos);
    SEQLIB_CHECK(s.find("N24 key 60 vel 100") != std::string::npos);
    SEQLIB_CHECK(s.find("      24  B2 ") != std::string::npos);
}

// A voice's bytes as the test cartridge writes them (Cart::Voice and Cart::KeySplit). VoiceOfBytes reads them as
// ReadVoice does, and VoiceBytes gives them back. A key split's last 4 bytes give its key table, and those of another
// voice its envelope.
void TestVoiceBytes()
{
    Cart cart;
    const std::vector<uint8_t> sample = Cart::Voice(kVoiceFixed, 0x08123456, 64, {200, 10, 128, 30}, 0xC4, 5);
    const std::vector<uint8_t> split = cart.KeySplit(0x08200000, 50);
    const uint32_t group = cart.VoiceGroup({{0, sample}, {1, split}});
    const Rom rom = cart.ToRom();

    const auto check = [&rom](const std::vector<uint8_t>& bytes, uint32_t address)
    {
        ntdlib::mp2k::Voice read;
        SEQLIB_CHECK(ReadVoice(rom, address, read));
        const ntdlib::mp2k::Voice voice =
            ntdlib::mp2k::VoiceOfBytes(std::span<const uint8_t, 12>(bytes.data(), 12), address);
        SEQLIB_CHECK(voice.address == address && voice.address == read.address);
        SEQLIB_CHECK(voice.type == read.type && voice.key == read.key && voice.length == read.length);
        SEQLIB_CHECK(voice.pan_sweep == read.pan_sweep && voice.wave == read.wave && voice.key_table == read.key_table);
        SEQLIB_CHECK(voice.attack == read.attack && voice.decay == read.decay && voice.sustain == read.sustain &&
                     voice.release == read.release);
        const std::array<uint8_t, 12> written = ntdlib::mp2k::VoiceBytes(voice);
        SEQLIB_CHECK(std::equal(written.begin(), written.end(), bytes.begin(), bytes.end()));
        return voice;
    };

    const ntdlib::mp2k::Voice sampled = check(sample, group);
    SEQLIB_CHECK(sampled.type == kVoiceFixed && sampled.key == 64 && sampled.length == 5 && sampled.pan_sweep == 0xC4);
    SEQLIB_CHECK(sampled.wave == 0x08123456 && sampled.attack == 200 && sampled.decay == 10);
    SEQLIB_CHECK(sampled.sustain == 128 && sampled.release == 30 && sampled.key_table == 0x1E800AC8);
    const ntdlib::mp2k::Voice keys = check(split, group + 12);
    SEQLIB_CHECK(keys.type == kVoiceKeySplit && keys.wave == 0x08200000);
    SEQLIB_CHECK(keys.key_table == xsflib::LoadLe32(&split[8]));

    // A changed envelope goes into the last 4 bytes of a voice that isn't a key split. A key split writes its table.
    ntdlib::mp2k::Voice edited = sampled;
    edited.attack = 255;
    ntdlib::mp2k::Voice edited_split = keys;
    edited_split.attack = 255;

    const std::array<uint8_t, 12> edited_bytes = ntdlib::mp2k::VoiceBytes(edited);
    const std::array<uint8_t, 12> split_bytes = ntdlib::mp2k::VoiceBytes(edited_split);

    SEQLIB_CHECK(edited_bytes[8] == 255 && edited_bytes[9] == 10);
    SEQLIB_CHECK(std::equal(split_bytes.begin(), split_bytes.end(), split.begin(), split.end()));
}

// Camelot's version of the driver has an unused copy of the routine that starts a song, whose literal pool is zeroed,
// before the one the game uses. Its note start and mixer have changes of their own, which detection finds by their
// code.

int RunTests()
{
    TestDecoding();
    TestDetection();
    TestSoundModes();
    TestDetectionVariants();
    TestUnusedPlayers();
    TestTiming();
    TestNotes();
    TestChannels();
    TestGrownChannels();
    TestEnvelope();
    TestEnvelopeHold();
    TestPsgEnvelopeBytes();
    TestPitch();
    TestLevelReports();
    TestDroppedNotes();
    TestSongChannels();
    TestSongSettings();
    TestDoubledNote();
    TestTempoChange();
    TestPsgPitch();
    TestPsgRelease();
    TestQuietPsg();
    TestPsgLevels();
    TestDump();
    TestVoiceBytes();

    return seqlib::testing::Summary("mp2k");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
