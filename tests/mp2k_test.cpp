// SPDX-License-Identifier: MIT

// Unit tests for MP2K's reader, its sequencer and its conversion. They build small synthetic cartridges in memory, so
// no game ROM is needed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/player/archive_source.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/mp2k/archive_player.h"
#include "ntdlib/mp2k/archive_source.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/agb/song_banks.h"
#include "ntdlib/sequence_export/mp2k/convert.h"
#include "ntdlib/sequence_export/mp2k/performance.h"
#include "ntdlib/sequence_export/mp2k/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/wave/blep.h"

namespace ntdlib::sequence_export::mp2k
{
using enum ntdlib::mp2k::Command;
using enum ntdlib::mp2k::Xcommand;
namespace cc = seqlib::midi::cc;
namespace sf2 = seqlib::sf2;
using ntdlib::agb::kFrameRate;
using ntdlib::agb::kRomBase;
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::TrackLayout;
using ntdlib::agb::Utf8;
using ntdlib::mp2k::Action;
using ntdlib::mp2k::ArchivePlayer;
using ntdlib::mp2k::ArchivePlayerOptions;
using ntdlib::mp2k::CamelotKeyToFrequency;
using ntdlib::mp2k::Channel;
using ntdlib::mp2k::ClockLength;
using ntdlib::mp2k::DecodeEvent;
using ntdlib::mp2k::DescribeEvent;
using ntdlib::mp2k::DetectDriver;
using ntdlib::mp2k::DriverOverrides;
using ntdlib::mp2k::DumpSong;
using ntdlib::mp2k::Event;
using ntdlib::mp2k::kChannelCount;
using ntdlib::mp2k::KeyToFrequency;
using ntdlib::mp2k::KeyToPsgFrequency;
using ntdlib::mp2k::kFifoLead;
using ntdlib::mp2k::kFirstPsgChannel;
using ntdlib::mp2k::kFullPlayerVolume;
using ntdlib::mp2k::kVoiceCompressed;
using ntdlib::mp2k::kVoiceDrumKit;
using ntdlib::mp2k::kVoiceFixed;
using ntdlib::mp2k::kVoiceKeySplit;
using ntdlib::mp2k::kVoiceReverse;
using ntdlib::mp2k::NoteArguments;
using ntdlib::mp2k::ReadSong;
using ntdlib::mp2k::RunningStatus;
using ntdlib::mp2k::ScanTrack;
using ntdlib::mp2k::Sequencer;
using ntdlib::mp2k::SongAddress;
using ntdlib::mp2k::SongChannels;
using ntdlib::mp2k::SongPlayer;
using ntdlib::mp2k::SongReverb;
using ntdlib::mp2k::SongVolume;
using ntdlib::mp2k::SynthStep;
using ntdlib::mp2k::Wave;
using ntdlib::sequence_export::agb::ChannelWriter;
using ntdlib::sequence_export::agb::kInstant;
using ntdlib::sequence_export::agb::MidiFile;
using ntdlib::sequence_export::agb::PresetInstrument;
using ntdlib::sequence_export::agb::Timecents;
using ntdlib::testing::agb_files::CartImage;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MakeTempFolder;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;

namespace
{

using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;
using ntdlib::testing::mp2k_cart::Cart;
using ntdlib::testing::mp2k_cart::kCamelotMode;
using ntdlib::testing::mp2k_cart::kData;
using ntdlib::testing::mp2k_cart::kInit;
using ntdlib::testing::mp2k_cart::kLaterCamelotMode;
using ntdlib::testing::mp2k_cart::kMode;
using ntdlib::testing::mp2k_cart::kMonoVolume;
using ntdlib::testing::mp2k_cart::kPlayerTable;
using ntdlib::testing::mp2k_cart::kSongStart;
using ntdlib::testing::mp2k_cart::kSongTable;
using ntdlib::testing::mp2k_cart::Saw;
using ntdlib::testing::mp2k_cart::Track;

// The MIDI file's ticks for each of the driver's, without frame timing: 960 to the quarter note.
constexpr uint32_t kMidiTicks = 40;

// A voice type: a DirectSound sample that plays at the mixer's rate.
constexpr uint8_t kFixed = kVoiceFixed;

// The points that the mixer of Golden Sun: The Lost Age mixes in a frame, at the rate of kCamelotMode.
constexpr int kCamelotPoints = 528;

// Returns the driver info of a cartridge, failing the test if detection fails.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

// Returns the bank and program that each program change on `channel` selects: the bank of the last bank select before
// it in its track.
std::vector<std::pair<int, int>> Selections(const MidiEvents& m, int channel)
{
    std::vector<std::pair<int, int>> out;
    for (const auto& t : m.tracks)
    {
        int bank = 0;
        for (const auto& e : t.events)
        {
            if (e.bytes[0] == 0xB0 + channel && e.bytes[1] == cc::kBankSelect)
            {
                bank = e.bytes[2];
            }
            else if (e.bytes[0] == 0xC0 + channel)
            {
                out.push_back({bank, e.bytes[1]});
            }
        }
    }

    return out;
}

// Returns the tempo events of a MIDI file's first track, as (tick, microseconds a quarter note).
std::vector<std::pair<uint32_t, uint32_t>> Tempos(const MidiEvents& m)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x51)
        {
            out.push_back({uint32_t(e.tick), uint32_t((e.bytes[3] << 16) | (e.bytes[4] << 8) | e.bytes[5])});
        }
    }

    return out;
}

// Returns true if `seconds` is the length of `frames` frames.
bool AtFrames(double seconds, double frames)
{
    return std::fabs(seconds - frames / kFrameRate) < 1e-9;
}

// Runs a sequencer for `frames` frames, and returns the actions of the kind given.
std::vector<Action> Actions(Sequencer& seq, int frames, Action::Kind kind)
{
    std::vector<Action> out;
    for (int f = 0; f < frames; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == kind)
            {
                out.push_back(a);
            }
        }
    }

    return out;
}

// Returns the CC10 and CC11 events of the MIDI track named after track `t`, in order. Each is a tick, a controller and
// a value.
std::vector<std::tuple<uint32_t, int, int>> LevelEvents(const MidiEvents& m, int t)
{
    const std::string name = "Track " + std::to_string(t);
    std::vector<std::tuple<uint32_t, int, int>> out;
    for (const auto& track : m.tracks)
    {
        if (track.events.empty())
        {
            continue;
        }

        const std::vector<uint8_t>& first = track.events.front().bytes;
        if (first.size() < 3 || std::string(first.begin() + 3, first.end()) != name)
        {
            continue;
        }

        for (const auto& e : track.events)
        {
            if ((e.bytes[0] & 0xF0) == 0xB0 && (e.bytes[1] == cc::kPan || e.bytes[1] == cc::kExpression))
            {
                out.emplace_back(uint32_t(e.tick), e.bytes[1], e.bytes[2]);
            }
        }
    }

    return out;
}

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

    // Rows give the modes that games set after the init with movs and lsls. The cart's init sets 5 channels and a
    // master volume of 12.
    struct ModeRow
    {
        const char* game_code;
        int channels;
        int master_volume;
    };
    constexpr ModeRow kModeRows[] = {{"B4LJ", 5, 6},  {"BFMJ", 5, 6},  {"BFPJ", 8, 8},
                                     {"BCQE", 8, 12}, {"BD7E", 6, 12}, {"B4TE", 6, 12}};
    for (const ModeRow& row : kModeRows)
    {
        Cart game;
        game.GameCode(row.game_code);
        game.Song({Track().Fine()}, voices);
        const DriverInfo game_info = Detect(game.ToRom());
        SEQLIB_CHECK_EQ(game_info.max_channels, row.channels);
        SEQLIB_CHECK_EQ(game_info.master_volume, row.master_volume);
    }

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
    SEQLIB_CHECK(swapped_info.mono && !info.mono);

    // A cartridge without the driver's code fails, unless the song table is given.
    Cart bare(kMode, {10}, false);
    bare.Song({Track().Fine()}, voices);
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.song_table = kSongTable;
    DriverInfo none;

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error) && error.empty());
    SEQLIB_CHECK(DetectDriver(bare_rom, table, none, error));
    SEQLIB_CHECK_EQ(none.song_count, 1);
    SEQLIB_CHECK_EQ(SongPlayer(bare_rom, none, 0).track_count, 16);
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

void TestConversion()
{
    // A sample voice with a bend, a key split of a sample and a square wave, and a drum kit. The kit has a fixed-pitch
    // sample panned left and three noise voices: one with no pan, one panned right and one panned a little left.
    Cart cart;
    const uint32_t lead = cart.Wave(Saw(), 11025, 32);
    const uint32_t low = cart.Wave(Saw(100), 8000);
    const uint32_t kick = cart.Wave(Saw(50), 13379);
    const uint32_t split_table = cart.VoiceGroup({{0, Cart::Voice(0, low)}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const uint32_t kit_table = cart.VoiceGroup({{36, Cart::Voice(kFixed, kick, 50, {255, 0, 255, 0}, 0xA0)},
                                                {38, Cart::Voice(4, 0, 40, {0, 2, 0, 0})},
                                                {40, Cart::Voice(4, 0, 40, {0, 2, 0, 0}, 0xD8)},
                                                {41, Cart::Voice(4, 0, 40, {0, 2, 0, 0}, 0xB0)}});
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(0, lead, 60, {255, 200, 128, 200})},
                                             {2, cart.KeySplit(split_table, 59)},
                                             {3, Cart::Voice(kVoiceDrumKit, kit_table)}});

    // The drum kit's track is track 9, after seven that only end.
    std::vector<Track> tracks = {
        Track()
            .Cmd(kCmdTempo, 75)
            .Cmd(kCmdVoice, 1)
            .Cmd(kCmdVol, 100)
            .Cmd(kCmdPan, 0x50)
            .Cmd(kCmdBendR, 12)
            .Cmd(kCmdBend, 0x50)
            .Note(24, 60, 100)
            .Wait(48)
            .Fine(),
        Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Note(24, 50, 90).Note(24, 70, 90).Wait(48).Fine()};
    tracks.resize(9, Track().Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 3).Cmd(kCmdVol, 127).Note(12, 36, 127).Wait(12).Note(12, 38, 127).Wait(36).Fine());
    cart.Song(tracks, voices, 0, 0, 0x80 | 40);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("convert");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK_EQ(r.tracks, 3);
    SEQLIB_CHECK(std::fabs(r.bpm - 60e6 / 401825) < 1e-9);
    SEQLIB_CHECK(PathFromUtf8(r.midi_path).filename() == "convert_00.mid");

    // 960 ticks a quarter note, the tempo at the GBA's speed, and a conductor track, then a track on the MIDI channel
    // of each of the song's tracks that plays notes. Track 9 moves off the drum channel to channel 11, the first free
    // one after it.
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK_EQ(m.division, 24 * kMidiTicks);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 4);
    const auto tempos = Tempos(m);
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), [](const auto& t) { return t.second == 401825; }));

    // A velocity of v is written as 127 × √(v / 127), rounded: the lead's 100 as 113.
    const auto lead_notes = m.Find(0x90);
    SEQLIB_CHECK(lead_notes.size() == 1 && lead_notes[0].tick == 0 && lead_notes[0].bytes[1] == 60 &&
                 lead_notes[0].bytes[2] == 113);
    const auto lead_offs = m.Find(0x80);
    SEQLIB_CHECK(lead_offs.size() == 1 && lead_offs[0].tick == 24 * kMidiTicks);
    SEQLIB_REQUIRE_EQ(m.Find(0x91).size(), 2);
    SEQLIB_REQUIRE_EQ(m.Find(0x9A).size(), 2);
    SEQLIB_CHECK(m.Find(0x99).empty());

    // The program change, full volume and the song's reverb. The track's volume of 100 and pan of 0x50 give it levels
    // of 74 on the left and 125 on the right, out of 128. CC10 84 and CC11 114 give those levels under a constant-power
    // pan law and the SF2 default curve of CC11. A bend range of 3 semitones fills the bend of 3 semitones.
    const auto programs = m.Find(0xC0);
    SEQLIB_CHECK(programs.size() == 1 && programs[0].bytes[1] == 1);
    std::map<int, int> controls;
    for (const auto& e : m.Find(0xB0))
    {
        controls[e.bytes[1]] = e.bytes[2];
    }
    SEQLIB_CHECK(controls[cc::kVolume] == 127 && controls[cc::kReverb] == 40);
    SEQLIB_CHECK(controls[cc::kPan] == 84 && controls[cc::kExpression] == 114);
    SEQLIB_CHECK_EQ(controls[cc::kDataEntry], 3);
    const auto bends = m.Find(0xE0);
    SEQLIB_CHECK(!bends.empty() && bends.back().bytes[1] == 0x7F && bends.back().bytes[2] == 0x7F);

    // The SoundFont: a preset in bank 0 for each voice. No note plays on the drum channel. Bank 128 has no presets.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<std::pair<int, int>, int> presets;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        presets[{Le16(h + 22), Le16(h + 20)}] =
            Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
    }
    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({0, 2}) && presets.count({0, 3}));
    SEQLIB_REQUIRE_EQ(presets.size(), 3);
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));

    auto zone_of = [&](int instrument, int index)
    {
        return sf.ZoneGens(Le16(sf.Record("inst", 22, size_t(instrument)) + 20) + size_t(index));
    };

    // The lead's zone: its envelope, and no tuning, since its sample's rate is a whole number. Its sample holds the
    // master volume of 12, at 13/16 of the saw's -128 widened to 16 bits, rather than the zone's attenuation.
    const auto lead_zone = zone_of(presets[{0, 1}], 1);
    SEQLIB_CHECK(lead_zone.count(sf2::kDecayVolEnv) && lead_zone.count(sf2::kSustainVolEnv) &&
                 lead_zone.count(sf2::kReleaseVolEnv) && !lead_zone.count(sf2::kAttackVolEnv));
    SEQLIB_CHECK(!lead_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK_EQ(sf.SamplePeak(lead_zone.at(sf2::kSampleId)), 128 * 256 * 13 / 16);
    SEQLIB_CHECK(!lead_zone.count(sf2::kCoarseTune) && !lead_zone.count(sf2::kFineTune));
    SEQLIB_CHECK_EQ(lead_zone.at(sf2::kSampleModes), 1);

    // The key split's zones split after key 59, and its square's sample is 6.3 dB quieter than a full-scale sample.
    const auto square_zone = zone_of(presets[{0, 2}], 2);
    SEQLIB_CHECK_EQ(zone_of(presets[{0, 2}], 1).at(sf2::kKeyRange), 0x3B00);
    SEQLIB_CHECK_EQ(square_zone.at(sf2::kKeyRange) & 0xFF, 60);
    SEQLIB_CHECK(!square_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK(std::fabs(20 * std::log10(sf.SamplePeak(square_zone.at(sf2::kSampleId)) / 32767.0) + 6.3) < 0.01);

    // The square's zones follow one another up to key 127. Each tunes its keys to the driver's frequency settings. On
    // key 96, the driver shifts C's entry of -2004 right 5 times and adds 2048. The setting of 1985 plays 2080.5 Hz, 10
    // cents below C7 in equal temperament.
    const size_t split_zones = Le16(sf.Record("inst", 22, size_t(presets[{0, 2}]) + 1) + 20) -
                               Le16(sf.Record("inst", 22, size_t(presets[{0, 2}])) + 20);
    int next_key = 60, key_96_cents = 0;
    for (size_t z = 2; z < split_zones; z++)
    {
        const auto zone = zone_of(presets[{0, 2}], int(z));
        const int lo = zone.at(sf2::kKeyRange) & 0xFF, hi = zone.at(sf2::kKeyRange) >> 8;
        SEQLIB_CHECK_EQ(lo, next_key);
        next_key = hi + 1;
        if (lo <= 96 && hi >= 96)
        {
            SEQLIB_CHECK(!zone.count(sf2::kCoarseTune));
            key_96_cents = zone.count(sf2::kFineTune) ? int16_t(zone.at(sf2::kFineTune)) : 0;
        }
    }
    SEQLIB_CHECK_EQ(next_key, 128);
    SEQLIB_CHECK_EQ(key_96_cents, -10);

    // The drum kit's fixed-pitch sample plays at the mixer's rate whatever the key or the bend. Its pan of 0xA0 gives
    // it 191/127 of the left level of a note with no pan and 64/128 of the right level. Pan -296 gives the sides that
    // ratio under a sin/cos pan law. The noise voice with no pan plays the noise setting of its key. Its zone holds no
    // pan.
    const auto kick_zone = zone_of(presets[{0, 3}], 1);
    SEQLIB_CHECK_EQ(kick_zone.at(sf2::kKeyRange), 0x2424);
    SEQLIB_CHECK_EQ(kick_zone.at(sf2::kScaleTuning), 0);
    SEQLIB_CHECK_EQ(int16_t(kick_zone.at(sf2::kPan)), -296);
    const auto noise_zone = zone_of(presets[{0, 3}], 2);
    SEQLIB_CHECK_EQ(noise_zone.at(sf2::kKeyRange), 0x2626);
    SEQLIB_CHECK_EQ(noise_zone.at(sf2::kScaleTuning), 0);
    SEQLIB_CHECK(!noise_zone.count(sf2::kPan));

    // The PSG plays a note on one side alone where that side's level is at least twice the other's. It plays in the
    // middle otherwise. The pan of 0xD8 gives 176 on the right and 79 on the left. The pan of 0xB0 gives 96 on the
    // right and 159 on the left. On one side alone, the zone's attenuation cancels the 3 dB that the RP-036 pan law
    // adds there. Most players apply 0.4 of a zone's attenuation. The zone therefore holds 75 cB.
    const auto right_zone = zone_of(presets[{0, 3}], 3), middle_zone = zone_of(presets[{0, 3}], 4);
    SEQLIB_CHECK(right_zone.at(sf2::kKeyRange) == 0x2828 && middle_zone.at(sf2::kKeyRange) == 0x2929);
    SEQLIB_CHECK(right_zone.count(sf2::kPan) && int16_t(right_zone.at(sf2::kPan)) == 500);
    SEQLIB_CHECK(right_zone.count(sf2::kInitialAttenuation) && right_zone.at(sf2::kInitialAttenuation) == 75);
    SEQLIB_CHECK(!middle_zone.count(sf2::kPan) && !middle_zone.count(sf2::kInitialAttenuation));
    SEQLIB_CHECK(!noise_zone.count(sf2::kInitialAttenuation));
    bool wheel_off = false;
    for (size_t i = 0; i < sf.Count("imod", 10); i++)
    {
        const uint8_t* mod = sf.Record("imod", 10, i);
        wheel_off = wheel_off || (Le16(mod) == 0x020E && Le16(mod + 2) == sf2::kFineTune && Le16(mod + 4) == 0);
    }
    SEQLIB_CHECK(wheel_off);
}

void TestSongFiles()
{
    // ConvertSongFiles gives the bytes of the files that ConvertSong writes, and names no paths. A song whose music
    // player plays no track gives no bytes.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Note(12, 64, 90).Wait(24);
    t.Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("song_files");

    const SongReport written = ConvertSong(rom, info, 0, opt, nullptr);
    const SongFiles files = ConvertSongFiles(rom, info, 0, opt);
    const SongFiles none = ConvertSongFiles(rom, info, 1, opt);

    SEQLIB_REQUIRE(written.ok && files.report.ok && !files.report.silent);
    SEQLIB_CHECK(files.report.midi_path.empty() && files.report.sf2_path.empty());
    SEQLIB_CHECK(!files.midi.empty() && files.midi == ReadAll(written.midi_path));
    SEQLIB_CHECK(!files.sf2.empty() && files.sf2 == ReadAll(written.sf2_path));
    SEQLIB_CHECK(files.report.warnings == written.warnings && files.report.seconds == written.seconds);
    SEQLIB_CHECK(!none.report.ok && none.midi.empty() && none.sf2.empty());
}

void TestVoiceGroupFiles()
{
    // A voice group's SoundFont holds the instruments of the SoundFont of a song that plays the same voices, byte for
    // byte with the song's title. Voice 2 has no sample: it gets a warning and no preset. Without a preset, the
    // SoundFont stays empty.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {0, 0, 15, 0})},
                                             {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))},
                                             {2, Cart::Voice(0, 0)}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(24, 60, 100).Wait(24).Cmd(kCmdVoice, 1);
    t.Note(24, 64, 90).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const ConvertSettings opt = TempSettings("voice_group");
    const SongFiles song = ConvertSongFiles(rom, info, 0, opt);
    const std::string title = rom.Title() + " #" + ntdlib::sequence_export::agb::SongNumber(0, info.song_count);

    const VoiceGroupFiles group = ConvertVoiceGroup(rom, info, voices, {0, 1, 2}, opt, title);
    const VoiceGroupFiles empty = ConvertVoiceGroup(rom, info, voices, {2, 200}, opt, title);

    SEQLIB_REQUIRE(song.report.ok && group.ok);
    SEQLIB_CHECK(!group.sf2.empty() && group.sf2 == song.sf2);
    SEQLIB_CHECK(group.warnings == std::vector<std::string>{"voice 2 has no samples to play"});
    SEQLIB_CHECK(!empty.ok && empty.sf2.empty() && empty.warnings.size() == 1);
}

void TestNoteChannels()
{
    // Voice 0 plays the square wave of PSG channel 2, and voice 1 a sample. Track 0's second square note takes the
    // channel while the driver still holds the first, and cuts that one short. Track 1's square note meets the channel
    // that an earlier track holds at the same priority, and never starts. Its sample note plays on a DirectSound
    // channel.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {0, 0, 15, 0})}, {1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(48, 60, 100).Wait(24).Note(24, 62, 100).Wait(24);
    first.Fine();
    Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(12);
    const size_t dropped = second.Here();
    second.Note(12, 64, 90).Wait(12).Cmd(kCmdVoice, 1).Note(12, 67, 90).Wait(12).Fine();
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ntdlib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, header) && header.tracks.size() == 2);

    const Performance perf = PerformSong(rom, info, 0, TempSettings("note_channels"));

    constexpr int8_t kSquare = kFirstPsgChannel + 1;
    SEQLIB_REQUIRE_EQ(perf.note_commands.size(), 3);
    SEQLIB_REQUIRE_EQ(perf.note_channels.size(), 3);
    SEQLIB_CHECK(perf.note_channels[0] == (NoteChannel{kSquare, 2, 24}));
    SEQLIB_CHECK(perf.note_channels[1] == (NoteChannel{kSquare, 2, std::nullopt}));
    SEQLIB_CHECK(perf.note_channels[2].channel >= 0 && perf.note_channels[2].channel < kSquare - 1);
    SEQLIB_CHECK_EQ(perf.note_channels[2].psg, 0);
    SEQLIB_CHECK(!perf.note_channels[2].cut_tick);
    const uint32_t offset = header.tracks[1] + uint32_t(dropped) - perf.base;
    SEQLIB_REQUIRE_EQ(perf.unstarted.size(), 1);
    SEQLIB_CHECK(perf.unstarted[0] == (UnstartedNote{12, 12, 1, 64, 90, 2, offset}));

    // The MIDI file's notes stay those of a performance without the records: the cut note ends where the next took its
    // channel.
    uint32_t end = 0;
    for (const TrackEvent& event : perf.tracks[0])
    {
        if (event.kind == EventKind::kNoteOff && event.note == 1)
        {
            end = event.tick;
        }
    }
    SEQLIB_CHECK_EQ(end, 24);
}

void TestPsgNoteTakenBeforeItSounds()
{
    // Voice 0 plays the square wave of PSG channel 2 with the slowest attack. The channel starts the first note at a
    // level of 0. The second note takes the channel a tick later, before the level rises, and the first note never
    // sounds. The performance lists it among the notes that never started, and the MIDI file leaves it out.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(2, 2, 60, {7, 0, 15, 0})}});
    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t taken = t.Here();
    t.Note(24, 60, 100).Wait(1).Note(24, 62, 100).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ntdlib::mp2k::SongHeader header;
    SEQLIB_REQUIRE(ReadSong(rom, info, 0, header) && header.tracks.size() == 1);

    const Performance perf = PerformSong(rom, info, 0, TempSettings("psg_taken"));

    constexpr int8_t kSquare = kFirstPsgChannel + 1;
    SEQLIB_REQUIRE_EQ(perf.note_channels.size(), 1);
    SEQLIB_CHECK(perf.note_channels[0] == (NoteChannel{kSquare, 2, std::nullopt}));
    const uint32_t offset = header.tracks[0] + uint32_t(taken) - perf.base;
    SEQLIB_REQUIRE_EQ(perf.unstarted.size(), 1);
    SEQLIB_CHECK(perf.unstarted[0] == (UnstartedNote{0, 24, 0, 60, 100, 2, offset}));
    std::vector<int> keys;
    for (const TrackEvent& event : perf.tracks[0])
    {
        if (event.kind == EventKind::kNoteOn)
        {
            keys.push_back(event.key);
        }
    }
    SEQLIB_CHECK(keys == std::vector<int>{62});
}

void TestPerformance()
{
    // Track 0 sets the tempo, the voice, the volume and the pan, plays a note, and calls a pattern with a second note.
    // Track 1 plays a note, and its repeat goes back to the note once.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x50).Note(24, 60, 100).Wait(24);
    const size_t call = first.Here();
    first.Patt(19).Wait(24).Fine();
    SEQLIB_REQUIRE_EQ(first.Here(), 19);
    first.Note(12, 62).Wait(12).Raw({kCmdPend});
    Track second;
    second.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127);
    const size_t note = second.Here();
    second.Note(12, 64, 90).Wait(12).Rept(2, note).Fine();
    cart.Song({first, second}, voices);

    // Song 1 loops on its note. Its GOTO runs in the end's tick, and so does the note of the next pass.
    Track looping;
    looping.Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127);
    const size_t loop = looping.Here();
    looping.Note(24, 60, 100).Wait(24).Goto(loop);
    cart.Song({looping}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    const Performance perf = PerformSong(rom, info, 0, TempSettings("performance"));

    // The Cart puts each track at a multiple of 4 bytes: track 1 starts 24 bytes after track 0. A tempo of 75 takes a
    // tick each frame, at the GBA's 16,777,216 / 280,896 frames a second.
    constexpr uint32_t kSecond = 24;
    SEQLIB_CHECK(perf.warnings.empty());
    SEQLIB_CHECK_EQ(perf.timebase, 24);
    SEQLIB_CHECK_EQ(perf.end_tick, 60);
    SEQLIB_CHECK(!perf.loop);
    SEQLIB_REQUIRE_EQ(perf.tempo.size(), 1);
    SEQLIB_CHECK_EQ(perf.tempo[0].tick, 0);
    SEQLIB_CHECK(std::fabs(perf.tempo[0].bpm - 150.0 * (16777216.0 / 280896) / 60) < 1e-9);

    // The stretches, with offsets from track 0's start.
    using Entry = ntdlib::sequence_export::Entry;
    const std::vector<Stretch> track0 = {{0, 0, uint32_t(call), Entry::kOpen, 0, false},
                                         {24, 19, 22, Entry::kCall, 1, false},
                                         {36, 17, 18, Entry::kReturn, 0, false}};
    const std::vector<Stretch> track1 = {{0, kSecond, kSecond + 8, Entry::kOpen, 0, false},
                                         {12, kSecond + 4, kSecond + 14, Entry::kLoop, 0, false}};
    SEQLIB_CHECK(perf.stretches[0] == track0);
    SEQLIB_CHECK(perf.stretches[1] == track1);
    for (size_t t = 2; t < perf.stretches.size(); t++)
    {
        SEQLIB_CHECK(perf.stretches[t].empty() && perf.tracks[t].empty() && perf.mix[t].empty());
    }

    // The notes, numbered in the order that they start, and the commands that started them.
    const std::vector<NoteCommand> commands = {{8, 0}, {kSecond + 4, 0}, {kSecond + 4, 1}, {19, 1}};
    SEQLIB_CHECK(perf.note_commands == commands);
    const auto event = [](uint32_t tick, EventKind kind, uint8_t key, uint8_t value, uint32_t id)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = kind;
        e.key = key;
        e.value = value;
        e.note = id;
        return e;
    };
    TrackEvent preset;
    preset.kind = EventKind::kPreset;
    preset.preset = 0;
    const std::vector<TrackEvent> events0 = {
        preset, event(0, EventKind::kNoteOn, 60, 100, 1), event(24, EventKind::kNoteOff, 60, 0, 1),
        event(24, EventKind::kNoteOn, 62, 100, 4), event(36, EventKind::kNoteOff, 62, 0, 4)};
    const std::vector<TrackEvent> events1 = {
        preset, event(0, EventKind::kNoteOn, 64, 90, 2), event(12, EventKind::kNoteOff, 64, 0, 2),
        event(12, EventKind::kNoteOn, 64, 90, 3), event(24, EventKind::kNoteOff, 64, 0, 3)};
    const auto same = [](const std::vector<TrackEvent>& a, const std::vector<TrackEvent>& b)
    {
        const auto fields = [](const TrackEvent& e)
        {
            return std::tuple(e.tick, e.kind, e.key, e.value, e.preset, e.note);
        };
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [&](const TrackEvent& x, const TrackEvent& y) { return fields(x) == fields(y); });
    };
    SEQLIB_CHECK(same(perf.tracks[0], events0));
    SEQLIB_CHECK(same(perf.tracks[1], events1));

    // The volume and the pan less 0x40.
    const std::vector<MixChange> mix0 = {{0, Mix::kVolume, 100}, {0, Mix::kPan, 16}};
    const std::vector<MixChange> mix1 = {{0, Mix::kVolume, 127}};
    SEQLIB_CHECK(perf.mix[0] == mix0);
    SEQLIB_CHECK(perf.mix[1] == mix1);

    // One pass of song 1's loop. The GOTO finishes the first stretch in the end's tick. The next pass's stretch begins
    // there, and stays out.
    ConvertSettings once = TempSettings("performance_loop");
    once.loops = 1;
    const Performance loops = PerformSong(rom, info, 1, once);
    SEQLIB_REQUIRE(loops.loop.has_value());
    SEQLIB_CHECK(loops.loop->start == 0 && loops.loop->end == 24);
    SEQLIB_CHECK_EQ(loops.end_tick, 24);
    const std::vector<Stretch> pass = {{0, 0, 8, Entry::kOpen, 0, false}};
    SEQLIB_CHECK(loops.stretches[0] == pass);
    const std::vector<NoteCommand> note_commands = {{4, 0}};
    SEQLIB_CHECK(loops.note_commands == note_commands);
}

void TestLevels()
{
    // A sample voice, and a voice for each square channel.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))},
                                             {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {2, Cart::Voice(2, 2, 60, {0, 0, 15, 0})}});

    // Tracks 0 to 2 play the sample voice: at volume 80 and pan 0x7F, at volume 127 and pan 0x7F, and at volume 100
    // and pan 0x50. Tracks 3 to 5 play square 1 at volume 100 in turn: at pan 0x50 from tick 0, 0x60 from tick 24 and
    // 0x20 from tick 48. Track 6 plays square 2, the sample voice and square 2 again from ticks 0, 24 and 48, at volume
    // 100 and pan 0x60.
    std::vector<Track> tracks;
    const std::vector<std::pair<int, int>> kSamples = {{80, 0x7F}, {127, 0x7F}, {100, 0x50}};
    for (const auto& [volume, pan] : kSamples)
    {
        tracks.push_back(
            Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, volume).Cmd(kCmdPan, pan).Note(12, 60, 127).Wait(12).Fine());
    }
    tracks.push_back(Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x50).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x60).Wait(24).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(
        Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x20).Wait(48).Note(12, 60, 127).Wait(12).Fine());
    tracks.push_back(Track()
                         .Cmd(kCmdVol, 100)
                         .Cmd(kCmdPan, 0x60)
                         .Cmd(kCmdVoice, 2)
                         .Note(12, 60, 127)
                         .Wait(24)
                         .Cmd(kCmdVoice, 0)
                         .Note(12, 60, 127)
                         .Wait(24)
                         .Cmd(kCmdVoice, 2)
                         .Note(12, 60, 127)
                         .Wait(12)
                         .Fine());
    cart.Song(tracks, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A sample plays at the levels that the volume and pan give each side, out of 128. Volume 80 at pan 0x7F gives 0
    // and 158. Volume 127 gives 0 and 252. CC11 can't reach that: it stops at the centre's loudest level. Volume 100 at
    // pan 0x50 gives 74 and 125.
    using Levels = std::vector<std::tuple<uint32_t, int, int>>;
    SEQLIB_REQUIRE(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK((LevelEvents(m, 0) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 119}}));
    SEQLIB_CHECK((LevelEvents(m, 1) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 127}}));
    SEQLIB_CHECK((LevelEvents(m, 2) == Levels{{0, cc::kPan, 84}, {0, cc::kExpression, 114}}));

    // A square plays on one side alone where that side's level is at least twice the other's. That side plays at the
    // level of the centre: the average of the two. Otherwise the square plays on both sides at that level. Pan 0x50
    // gives 74 and 125. The square then plays on both sides at 99.5. Pan 0x60 gives 49 and 150, on the right alone.
    // Pan 0x20 gives 149 and 50, on the left alone. A track's controllers follow the PSG's rule from the start where
    // its first note is a PSG note.
    SEQLIB_CHECK((LevelEvents(m, 3) == Levels{{0, cc::kPan, 64}, {0, cc::kExpression, 112}}));
    SEQLIB_CHECK((LevelEvents(m, 4) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 95}}));
    SEQLIB_CHECK((LevelEvents(m, 5) == Levels{{0, cc::kPan, 0}, {0, cc::kExpression, 95}}));

    // A track that plays both kinds of voice follows the rule of the notes that it plays.
    SEQLIB_CHECK((LevelEvents(m, 6) == Levels{{0, cc::kPan, 127},
                                              {0, cc::kExpression, 95},
                                              {24 * kMidiTicks, cc::kPan, 101},
                                              {24 * kMidiTicks, cc::kExpression, 119},
                                              {48 * kMidiTicks, cc::kPan, 127},
                                              {48 * kMidiTicks, cc::kExpression, 95}}));
}

void TestMonoLevels()
{
    // A cartridge with the mono mixer, a sample voice and a square voice.
    Cart cart;
    const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
    for (uint32_t i = 0; i < 12; i++)
    {
        cart.Put8(kMonoVolume + i, kMono[i]);
    }
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});

    // Track 0 plays the sample voice at volume 80 and pan 0x7F, and track 1 the square at volume 100 and pan 0x60.
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 80).Cmd(kCmdPan, 0x7F).Note(12, 60, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x60).Note(12, 60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("mono_levels");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The mixer plays the sample on both sides at the average of its levels, 0 and 158. The PSG still plays the square
    // on the right alone.
    using Levels = std::vector<std::tuple<uint32_t, int, int>>;
    SEQLIB_REQUIRE(info.mono && r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK((LevelEvents(m, 0) == Levels{{0, cc::kPan, 64}, {0, cc::kExpression, 100}}));
    SEQLIB_CHECK((LevelEvents(m, 1) == Levels{{0, cc::kPan, 127}, {0, cc::kExpression, 95}}));
}

void TestSlowAttackAtEnd()
{
    // A square note with the slowest attack starts on the last tick of each pass through a loop of 23 ticks from tick
    // 1, and the conversion ends after the second pass, on tick 47. The second pass's note goes on into the next pass,
    // and its level gets above 0 only after the end. So the model runs on past the end, and the MIDI file keeps it.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {7, 0, 15, 0})}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Wait(1);
    const size_t loop = track.Here();
    track.Wait(22).Note(24, 60, 100).Wait(1).Goto(loop);
    cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("slow_attack");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok);
    std::vector<uint32_t> ons;
    for (const auto& e : ReadMidi(r.midi_path).Find(0x90))
    {
        ons.push_back(uint32_t(e.tick));
    }
    SEQLIB_CHECK(ons == (std::vector<uint32_t>{23 * kMidiTicks, 46 * kMidiTicks}));
}

void TestTwoVoicesInATick()
{
    // A track sets voice 1 and plays a note, then sets voice 4 and plays another, in one tick. The driver plays each
    // note with the voice set before it. So the MIDI file gives the program changes and notes in the track's order.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup(
        {{1, Cart::Voice(0, cart.Wave(Saw(80), 13379, 0))}, {4, Cart::Voice(0, cart.Wave(Saw(90), 13379, 0))}});
    cart.Song({Track().Cmd(kCmdVoice, 1).Note(24, 60, 100).Cmd(kCmdVoice, 4).Note(24, 64, 100).Wait(24).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("two_voices");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok);
    std::vector<std::pair<int, int>> order;
    for (const auto& t : ReadMidi(r.midi_path).tracks)
    {
        for (const auto& [tick, bytes] : t.events)
        {
            if (tick == 0 && (bytes[0] == 0xC0 || bytes[0] == 0x90))
            {
                order.emplace_back(bytes[0], bytes[1]);
            }
        }
    }
    const std::vector<std::pair<int, int>> kOrder = {{0xC0, 1}, {0x90, 60}, {0xC0, 4}, {0x90, 64}};
    SEQLIB_CHECK(order == kOrder);
}

void TestSharedSoundfont()
{
    // Two voice groups with different voices as program 1 and the same voice as program 4.
    Cart cart;
    const std::vector<uint8_t> common = Cart::Voice(0, cart.Wave(Saw(), 13379, 0));
    const uint32_t first_group = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(80)))}, {4, common}});
    const uint32_t second_group = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(90)))}, {4, common}});

    // The first song plays program 1 on track 0 and program 4 on track 9, and the second plays both on track 0.
    std::vector<Track> tracks = {Track().Cmd(kCmdVoice, 1).Note(24, 60, 100).Wait(24).Fine()};
    tracks.resize(9, Track().Fine());
    tracks.push_back(Track().Cmd(kCmdVoice, 4).Note(24, 36, 100).Wait(24).Fine());
    cart.Song(tracks, first_group);
    cart.Song(
        {Track().Cmd(kCmdVoice, 1).Note(24, 60, 100).Wait(24).Cmd(kCmdVoice, 4).Note(24, 62, 100).Wait(24).Fine()},
        second_group);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("shared");
    SoundfontBuilder shared(rom, info);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport second = ConvertSong(rom, info, 1, opt, &shared);

    // Program 1 has the first song's voice in bank 0 and the second's in bank 1, the common voice has one preset, and
    // the drum bank has none.
    SEQLIB_CHECK(first.ok && second.ok && first.sf2_path.empty() && second.sf2_path.empty());
    std::map<std::pair<int, int>, int> presets;
    for (const sf2::Preset& p : shared.File().presets)
    {
        presets[{p.bank, p.program}] = PresetInstrument(p);
    }
    SEQLIB_REQUIRE_EQ(presets.size(), 3);
    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({1, 1}) && presets.count({0, 4}));
    SEQLIB_CHECK((presets[{0, 1}] != presets[{1, 1}]));

    // Each program change selects its preset's bank, and track 9 plays on channel 11, away from the drum channel.
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    using Slots = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK(a.Find(0x99).empty() && a.Find(0x9A).size() == 1);
    SEQLIB_CHECK(Selections(a, 0) == (Slots{{0, 1}}));
    SEQLIB_CHECK(Selections(a, 10) == (Slots{{0, 4}}));
    SEQLIB_CHECK(Selections(b, 0) == (Slots{{1, 1}, {0, 4}}));
}

void TestSharedPresets()
{
    // 129 instruments that songs play as program 5, which fill its 128 banks, and then another as program 0.
    // SharedPreset() doesn't look at the instruments, the ROM or the driver, so they needn't exist.
    const Rom rom = Cart().ToRom();
    const DriverInfo info;
    SoundfontBuilder sf(rom, info);
    std::vector<BankProgram> slots;

    for (int i = 0; i < 129; i++)
    {
        slots.push_back(sf.SharedPreset(5, i));
    }
    const BankProgram again = sf.SharedPreset(5, 128);
    const BankProgram next = sf.SharedPreset(0, 200);

    // The 129th gets the first free program of bank 0, which it keeps, and the next one as program 0 gets bank 1.
    SEQLIB_CHECK(slots[0].bank == 0 && slots[0].program == 5);
    SEQLIB_CHECK(slots[127].bank == 127 && slots[127].program == 5);
    SEQLIB_CHECK(slots[128].bank == 0 && slots[128].program == 0);
    SEQLIB_CHECK(again.bank == 0 && again.program == 0);
    SEQLIB_CHECK(next.bank == 1 && next.program == 0);
    SEQLIB_REQUIRE_EQ(sf.File().presets.size(), 130);
}

void TestSharedDrumChannel()
{
    // Two songs whose 16 tracks all play, one after another, so track 9 has no free channel to move to, with different
    // voices as program 2.
    Cart cart(kMode, {16});
    const uint32_t first_group = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(70)))}});
    const uint32_t second_group = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(90)))}});
    std::vector<Track> tracks;
    for (int t = 0; t < 16; t++)
    {
        tracks.push_back(Track().Wait(24 * t).Cmd(kCmdVoice, 2).Note(24, 60, 100).Wait(24).Fine());
    }
    cart.Song(tracks, first_group);
    cart.Song(tracks, second_group);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("drums");
    SoundfontBuilder shared(rom, info);

    const SongReport first = ConvertSong(rom, info, 0, opt, &shared);
    const SongReport second = ConvertSong(rom, info, 1, opt, &shared);

    auto warns = [](const SongReport& s)
    {
        return std::any_of(s.warnings.begin(), s.warnings.end(),
                           [](const std::string& w) { return w.find("drum bank") != std::string::npos; });
    };

    // Track 9 stays on the drum channel and selects bank 0 there. The first song's voice gets a copy in the drum bank,
    // and the second song's, which can't have one, gets a warning.
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    using Slots = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK(first.ok && second.ok);
    SEQLIB_CHECK(a.Find(0x99).size() == 1 && b.Find(0x99).size() == 1);
    SEQLIB_CHECK(Selections(a, 9) == (Slots{{0, 2}}) && Selections(b, 9) == (Slots{{0, 2}}));
    SEQLIB_CHECK(Selections(b, 0) == (Slots{{1, 2}}));
    int drum_presets = 0;
    for (const sf2::Preset& p : shared.File().presets)
    {
        drum_presets += p.bank == 128;
        SEQLIB_CHECK(p.bank != 128 ||
                     (p.program == 2 && PresetInstrument(p) == PresetInstrument(shared.File().presets[0])));
    }
    SEQLIB_CHECK_EQ(drum_presets, 1);
    SEQLIB_CHECK(!warns(first) && warns(second));
}

void TestDrumChannel()
{
    // Two songs whose tracks play one after another. In song 0, track 8 only ends. In song 1, all 16 tracks play.
    Cart cart(kMode, {16});
    const uint32_t voices = cart.VoiceGroup({{2, Cart::Voice(0, cart.Wave(Saw(70)))}});
    for (int song = 0; song < 2; song++)
    {
        std::vector<Track> tracks;
        for (int t = 0; t < 16; t++)
        {
            Track track;
            if (song == 1 || t != 8)
            {
                track.Wait(24 * t).Cmd(kCmdVoice, 2).Note(24, 60, 100).Wait(24);
            }
            tracks.push_back(track.Fine());
        }
        cart.Song(tracks, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("drum_channel");

    const SongReport first = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport second = ConvertSong(rom, info, 1, opt, nullptr);

    // In song 0, a track plays on every channel after the drum channel. Track 9 moves to channel 9, the last free one
    // before it. Its voice gets no copy in the drum bank. In song 1, track 9 stays on the drum channel. Its voice gets
    // a copy in the drum bank there.
    auto preset_list = [](const std::string& path)
    {
        const Sf2Records sf = ReadSf2(path);
        std::set<std::pair<int, int>> presets;
        for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
        {
            const uint8_t* h = sf.Record("phdr", 38, p);
            presets.insert({Le16(h + 22), Le16(h + 20)});
        }
        return presets;
    };
    using Presets = std::set<std::pair<int, int>>;
    SEQLIB_REQUIRE(first.ok && second.ok);
    const MidiEvents a = ReadMidi(first.midi_path);
    const MidiEvents b = ReadMidi(second.midi_path);
    SEQLIB_CHECK(a.Find(0x99).empty() && a.Find(0x98).size() == 1);
    SEQLIB_CHECK(b.Find(0x99).size() == 1);
    SEQLIB_CHECK(preset_list(first.sf2_path) == (Presets{{0, 2}}));
    SEQLIB_CHECK(preset_list(second.sf2_path) == (Presets{{0, 2}, {128, 2}}));
}

void TestVoiceChannels()
{
    // Track 0 plays a note with a slow release and turns its volume down while the note is held, and again while it
    // fades. Track 1 then plays a chord of 5 notes, with another voice, and the last of them takes the channel of track
    // 0's note, which is still fading.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})},
                                             {2, Cart::Voice(0, cart.Wave(Saw(80), 13379, 0))}});
    cart.Song({Track()
                   .Cmd(kCmdVoice, 1)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 60, 100)
                   .Wait(6)
                   .Cmd(kCmdVol, 80)
                   .Wait(12)
                   .Cmd(kCmdVol, 50)
                   .Wait(30)
                   .Fine(),
               Track()
                   .Wait(24)
                   .Cmd(kCmdVoice, 2)
                   .Cmd(kCmdVol, 100)
                   .Note(24, 62, 100)
                   .Note(24, 64, 100)
                   .Note(24, 65, 100)
                   .Note(24, 67, 100)
                   .Note(24, 69, 100)
                   .Wait(24)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voices");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A track for each of the driver's 5 channels, named after it, after the conductor track.
    SEQLIB_CHECK(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 6);
    std::vector<std::string> names;
    for (size_t t = 1; t < m.tracks.size(); t++)
    {
        const std::vector<uint8_t>& e = m.tracks[t].events.front().bytes;
        names.push_back(std::string(e.begin() + 3, e.end()));
    }
    SEQLIB_CHECK((names == std::vector<std::string>{"DirectSound 1", "DirectSound 2", "DirectSound 3", "DirectSound 4",
                                                    "DirectSound 5"}));

    // The first channel's events, without its track's meta events.
    std::vector<seqlib::midi::Event> first;
    for (const auto& e : m.tracks[1].events)
    {
        if (e.bytes[0] < 0xF0)
        {
            first.push_back(e);
        }
    }

    // Returns where an event comes among them, or -1 if it isn't there.
    auto find = [&](uint32_t tick, std::vector<uint8_t> bytes)
    {
        for (size_t i = 0; i < first.size(); i++)
        {
            if (first[i].tick == tick * kMidiTicks && first[i].bytes == bytes)
            {
                return int(i);
            }
        }

        return -1;
    };

    // On the first channel, track 0's note is released at tick 12. The note follows its track's volume before and
    // after that, at the centre: volumes of 127, 80 and 50 give CC11 127, 100 and 79. At tick 24 an All Sound Off cuts
    // off its release. The channel then takes track 1's voice and volume for the chord's last note. A velocity of 100
    // is written as 127 × √(100 / 127), rounded: 113.
    SEQLIB_CHECK(find(0, {0xC0, 1}) >= 0 && find(0, {0x90, 60, 113}) >= 0 && find(12, {0x80, 60, 0}) >= 0);
    SEQLIB_CHECK(find(0, {0xB0, cc::kVolume, 127}) >= 0 && find(0, {0xB0, cc::kPan, 64}) >= 0);
    SEQLIB_CHECK(find(0, {0xB0, cc::kExpression, 127}) >= 0 && find(6, {0xB0, cc::kExpression, 100}) >= 0 &&
                 find(18, {0xB0, cc::kExpression, 79}) >= 0);
    const int cut = find(24, {0xB0, cc::kAllSoundOff, 0});
    const int chord = find(24, {0x90, 69, 113});
    SEQLIB_CHECK(cut >= 0 && chord > cut);
    SEQLIB_CHECK(find(24, {0xC0, 2}) >= 0 && find(24, {0xB0, cc::kExpression, 112}) >= 0);

    // The chord's other notes take the 4 free channels.
    for (uint8_t ch = 1; ch < 5; ch++)
    {
        const auto ons = m.Find(uint8_t(0x90 | ch));
        SEQLIB_CHECK(ons.size() == 1 && ons[0].tick == 24 * kMidiTicks);
    }
}

void TestVoiceChannelsDrums()
{
    // A sample voice for each of the driver's 12 DirectSound channels, and a voice for each of its 4 PSG channels.
    Cart cart(0x0094CC00, {16});
    std::map<int, std::vector<uint8_t>> voice_map;
    for (int v = 0; v < 12; v++)
    {
        voice_map[v] = Cart::Voice(0, cart.Wave(Saw(size_t(40 + 4 * v)), 13379, 0));
    }
    voice_map[12] = Cart::Voice(1, 2, 60, {0, 0, 15, 0});
    voice_map[13] = Cart::Voice(2, 2, 60, {0, 0, 15, 0});
    voice_map[14] = Cart::Voice(3, cart.Place(std::vector<uint8_t>(16, 0x5A)), 60, {0, 0, 15, 0});
    voice_map[15] = Cart::Voice(4, 0, 60, {0, 0, 15, 0});
    const uint32_t voices = cart.VoiceGroup(voice_map);

    // A song whose 16 tracks play them all at once, so the 16th channel, the noise channel, gets the drum channel.
    std::vector<Track> tracks;
    for (int t = 0; t < 16; t++)
    {
        tracks.push_back(Track().Cmd(kCmdVoice, t).Cmd(kCmdVol, 127).Note(24, 60, 100).Wait(24).Fine());
    }
    cart.Song(tracks, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voice_drums");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The noise note plays on channel 10, and its voice has a copy in the drum bank.
    SEQLIB_CHECK(r.ok);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 17);
    const auto drum_notes = m.Find(0x99);
    SEQLIB_CHECK(drum_notes.size() == 1 && drum_notes[0].bytes[1] == 60);
    const Sf2Records sf = ReadSf2(r.sf2_path);
    bool drum_copy = false;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        drum_copy = drum_copy || (Le16(h + 22) == 128 && Le16(h + 20) == 15);
    }
    SEQLIB_CHECK(drum_copy);
}

void TestVoiceChannelsLoop()
{
    // Two voices that play on square channel 1.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}, {2, Cart::Voice(1, 1, 60, {0, 0, 15, 0})}});

    // Track 0 plays program 1 at volume 100 on tick 24, and every 48 ticks after that, looping from the start. Track 1
    // plays program 2 at volume 60 on ticks 0 and 12, and every 48 ticks after that, looping from tick 6. So the loop
    // runs from tick 6 to tick 54, and ends with track 0's settings on the channel.
    Track first;
    first.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100);
    const size_t first_loop = first.Here();
    first.Wait(24).Note(12, 48, 100).Wait(24).Goto(first_loop);
    Track second;
    second.Cmd(kCmdVoice, 2).Cmd(kCmdVol, 60).Note(12, 52, 100).Wait(6);
    const size_t second_loop = second.Here();
    second.Wait(6).Note(12, 52, 100).Wait(42).Goto(second_loop);
    cart.Song({first, second}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("voice_loop");
    opt.voice_channels = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop's first note, at tick 12, sets the channel's program, CC10, CC11 and pitch bend again. The note before
    // it left them the same. A player that jumps back to the loop's start comes from track 0's note. Both tracks play
    // at the centre. CC11 is 87 for volume 60 and 112 for volume 100.
    using Events = std::vector<seqlib::midi::Event>;
    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(AtFrames(r.loop_start, 6));
    SEQLIB_CHECK(AtFrames(r.loop_end, 54));
    const MidiEvents m = ReadMidi(r.midi_path);
    Events levels;
    for (const auto& e : m.Find(0xB0))
    {
        if (e.bytes[1] == cc::kPan || e.bytes[1] == cc::kExpression)
        {
            levels.push_back(e);
        }
    }
    SEQLIB_CHECK((m.Find(0xC0) == Events{{0, {0xC0, 2}},
                                         {12 * kMidiTicks, {0xC0, 2}},
                                         {24 * kMidiTicks, {0xC0, 1}},
                                         {60 * kMidiTicks, {0xC0, 2}},
                                         {72 * kMidiTicks, {0xC0, 1}}}));
    SEQLIB_CHECK((levels == Events{{0, {0xB0, cc::kPan, 64}},
                                   {0, {0xB0, cc::kExpression, 87}},
                                   {12 * kMidiTicks, {0xB0, cc::kPan, 64}},
                                   {12 * kMidiTicks, {0xB0, cc::kExpression, 87}},
                                   {24 * kMidiTicks, {0xB0, cc::kExpression, 112}},
                                   {60 * kMidiTicks, {0xB0, cc::kExpression, 87}},
                                   {72 * kMidiTicks, {0xB0, cc::kExpression, 112}}}));
    SEQLIB_CHECK((m.Find(0xE0) == Events{{12 * kMidiTicks, {0xE0, 0x00, 0x40}}}));
}

// Returns the zones of the instrument that each preset of bank 0 plays, by the preset's program: for each zone after
// the global zone, its key range and its exclusive class, or 0 for none.
std::map<int, std::vector<std::pair<int, int>>> ExclusiveClasses(const Sf2Records& sf)
{
    std::map<int, std::vector<std::pair<int, int>>> out;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        if (Le16(h + 22) != 0)
        {
            continue;
        }

        const size_t instrument = Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
        const size_t first = Le16(sf.Record("inst", 22, instrument) + 20);
        const size_t end = Le16(sf.Record("inst", 22, instrument + 1) + 20);
        for (size_t z = first + 1; z < end; z++)
        {
            const std::map<uint16_t, uint16_t> gens = sf.ZoneGens(z);
            const auto found = gens.find(sf2::kExclusiveClass);
            out[Le16(h + 20)].push_back({gens.at(sf2::kKeyRange), found != gens.end() ? found->second : 0});
        }
    }

    return out;
}

// Returns true if each zone in `zones` has the exclusive class `expected`.
bool AllOfClass(const std::vector<std::pair<int, int>>& zones, int expected)
{
    return !zones.empty() &&
           std::all_of(zones.begin(), zones.end(), [&](const auto& zone) { return zone.second == expected; });
}

// A test cartridge, and the driver that detection finds in it.
struct ExclusiveSong
{
    Rom rom;
    DriverInfo info;
};

// Returns a cartridge whose song plays programs 0 to 9: a voice of each PSG channel with a slow release, a square 1
// voice with the fixed-frequency bit, two square 1 voices with quick releases, a sample with a slow release, a drum kit
// and a key split.
ExclusiveSong MakeExclusiveSong()
{
    Cart cart;
    const uint32_t wave = cart.Place(std::vector<uint8_t>(16, 0x5A));
    const uint32_t sample = cart.Wave(Saw(), 13379, 0);
    const uint32_t kit = cart.VoiceGroup({{36, Cart::Voice(4, 0, 50, {0, 0, 15, 3})},
                                          {38, Cart::Voice(4, 0, 50, {0, 0, 15, 0})},
                                          {40, Cart::Voice(0, sample, 60, {255, 0, 255, 250})}});
    const uint32_t split = cart.VoiceGroup({{0, Cart::Voice(2, 1, 60, {0, 0, 15, 5})}, {1, Cart::Voice(0, sample)}});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 7})},
                                             {1, Cart::Voice(2, 2, 60, {0, 0, 15, 1})},
                                             {2, Cart::Voice(3, wave, 60, {0, 0, 15, 4})},
                                             {3, Cart::Voice(4, 0, 60, {0, 0, 15, 2})},
                                             {4, Cart::Voice(1 | kVoiceFixed, 2, 60, {0, 0, 15, 7})},
                                             {5, Cart::Voice(1, 2, 60, {0, 0, 15, 0})},
                                             {6, Cart::Voice(1, 2, 60, {0, 0, 2, 1})},
                                             {7, Cart::Voice(0, sample, 60, {255, 0, 255, 250})},
                                             {8, Cart::Voice(kVoiceDrumKit, kit)},
                                             {9, cart.KeySplit(split, 59)}});

    Track t;
    t.Cmd(kCmdTempo, 75).Cmd(kCmdVol, 127);
    for (int v = 0; v < 8; v++)
    {
        t.Cmd(kCmdVoice, v).Note(12, 60, 100).Wait(24);
    }
    t.Cmd(kCmdVoice, 8).Note(12, 36, 100).Wait(24).Note(12, 38, 100).Wait(24).Note(12, 40, 100).Wait(24);
    t.Cmd(kCmdVoice, 9).Note(12, 50, 100).Wait(24).Note(12, 70, 100).Wait(24).Fine();
    cart.Song({t}, voices);
    const Rom rom = cart.ToRom();

    return {rom, Detect(rom)};
}

// With --psg-exclusive, each zone of a PSG voice whose release is slower than FluidSynth's fade for an exclusive class
// gets the class of its channel: 1 and 2 for the squares, 3 for the wave and 4 for the noise. The fixed-frequency bit
// doesn't change the channel. A PSG voice with a quicker release and a sample voice get no class. The MIDI file stays
// the same, and the SoundFont without the option has no class.
void TestPsgExclusive()
{
    const ExclusiveSong song = MakeExclusiveSong();
    ConvertSettings plain = TempSettings("exclusive_plain");
    ConvertSettings exclusive = TempSettings("exclusive");
    exclusive.psg_exclusive = true;

    const SongReport a = ConvertSong(song.rom, song.info, 0, plain, nullptr);
    const SongReport b = ConvertSong(song.rom, song.info, 0, exclusive, nullptr);

    SEQLIB_CHECK(a.ok && b.ok);
    SEQLIB_CHECK(ReadAll(a.midi_path) == ReadAll(b.midi_path));
    const auto without = ExclusiveClasses(ReadSf2(a.sf2_path));
    const auto with = ExclusiveClasses(ReadSf2(b.sf2_path));
    SEQLIB_REQUIRE_EQ(without.size(), 10);
    SEQLIB_REQUIRE_EQ(with.size(), 10);
    for (const auto& [program, zones] : without)
    {
        SEQLIB_CHECK(AllOfClass(zones, 0));
    }

    SEQLIB_CHECK(AllOfClass(with.at(0), 1));
    SEQLIB_CHECK(AllOfClass(with.at(1), 2));
    SEQLIB_CHECK(AllOfClass(with.at(2), 3));
    SEQLIB_CHECK(AllOfClass(with.at(3), 4));
    SEQLIB_CHECK(AllOfClass(with.at(4), 1));
    SEQLIB_CHECK(AllOfClass(with.at(5), 0));
    SEQLIB_CHECK(AllOfClass(with.at(6), 0));
    SEQLIB_CHECK(AllOfClass(with.at(7), 0));

    // The drum kit's slow noise on key 36 has the class, and its quick noise and its sample don't. The key split's
    // square 2 has class 2 on the keys up to 59, and its sample has none. The square's zones from key 36 tune runs of
    // keys to the driver's frequency settings.
    using Zones = std::vector<std::pair<int, int>>;
    SEQLIB_CHECK((with.at(8) == Zones{{0x2424, 4}, {0x2626, 0}, {0x2828, 0}}));
    const Zones& split = with.at(9);
    SEQLIB_REQUIRE(split.size() >= 3);
    SEQLIB_CHECK((split.front() == std::pair{0x2300, 2}));
    SEQLIB_CHECK((split.back() == std::pair{0x7F3C, 0}));
    int next_key = 36;
    for (size_t z = 1; z + 1 < split.size(); z++)
    {
        SEQLIB_CHECK_EQ(split[z].first & 0xFF, next_key);
        SEQLIB_CHECK_EQ(split[z].second, 2);
        next_key = (split[z].first >> 8) + 1;
    }
    SEQLIB_CHECK_EQ(next_key, 60);
}

// The exclusive classes go in a shared SoundFont too. They also go in the SoundFont of a conversion with a MIDI channel
// for each of the driver's sound channels. The option doesn't change that conversion's MIDI file either.
void TestPsgExclusiveShared()
{
    const ExclusiveSong song = MakeExclusiveSong();
    ConvertSettings exclusive = TempSettings("exclusive_shared");
    exclusive.psg_exclusive = true;
    ConvertSettings voices = TempSettings("exclusive_voices_plain");
    voices.voice_channels = true;
    ConvertSettings exclusive_voices = TempSettings("exclusive_voices");
    exclusive_voices.voice_channels = true;
    exclusive_voices.psg_exclusive = true;
    SoundfontBuilder shared(song.rom, song.info, true);
    const std::string shared_path = Utf8(TempPath("exclusive_shared.sf2"));
    std::string error;

    const SongReport s = ConvertSong(song.rom, song.info, 0, exclusive, &shared);
    const bool written = shared.File().Write(shared_path, error);
    const SongReport v = ConvertSong(song.rom, song.info, 0, voices, nullptr);
    const SongReport w = ConvertSong(song.rom, song.info, 0, exclusive_voices, nullptr);

    SEQLIB_CHECK(s.ok && written && v.ok && w.ok);
    SEQLIB_CHECK(ReadAll(v.midi_path) == ReadAll(w.midi_path));
    for (const std::string& path : {shared_path, w.sf2_path})
    {
        const auto classes = ExclusiveClasses(ReadSf2(path));
        SEQLIB_REQUIRE_EQ(classes.size(), 10);
        SEQLIB_CHECK(AllOfClass(classes.at(0), 1) && AllOfClass(classes.at(3), 4) && AllOfClass(classes.at(5), 0));
    }
}

// For each preset of bank 0, by program, returns one flag for each zone of the instrument that the preset plays, after
// the global zone. A flag is true if its zone holds a modulator from CC91 to the reverb send with an amount of 0.
std::map<int, std::vector<bool>> DryZones(const Sf2Records& sf)
{
    std::map<int, std::vector<bool>> out;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        if (Le16(h + 22) != 0)
        {
            continue;
        }

        const size_t instrument = Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
        const size_t first = Le16(sf.Record("inst", 22, instrument) + 20);
        const size_t end = Le16(sf.Record("inst", 22, instrument + 1) + 20);
        for (size_t z = first + 1; z < end; z++)
        {
            bool dry = false;
            for (size_t m = Le16(sf.Record("ibag", 4, z) + 2); m < Le16(sf.Record("ibag", 4, z + 1) + 2); m++)
            {
                const uint8_t* mod = sf.Record("imod", 10, m);
                dry =
                    dry || (Le16(mod) == (sf2::kController | 91) && Le16(mod + 2) == sf2::kReverbEffectsSend &&
                            Le16(mod + 4) == 0 && Le16(mod + 6) == sf2::kNoController && Le16(mod + 8) == sf2::kLinear);
            }
            out[Le16(h + 20)].push_back(dry);
        }
    }

    return out;
}

// Every zone of a PSG voice holds kNoReverb: the default modulator from CC91 to the reverb send, at an amount of 0. A
// sample's zone keeps the default.
void TestPsgNoReverb()
{
    const ExclusiveSong song = MakeExclusiveSong();
    const SongReport r = ConvertSong(song.rom, song.info, 0, TempSettings("no_reverb"), nullptr);

    SEQLIB_CHECK(r.ok);
    const auto dry = DryZones(ReadSf2(r.sf2_path));
    SEQLIB_REQUIRE_EQ(dry.size(), 10);
    for (int program = 0; program < 7; program++)
    {
        const std::vector<bool>& zones = dry.at(program);
        SEQLIB_CHECK(!zones.empty() && std::all_of(zones.begin(), zones.end(), [](bool d) { return d; }));
    }
    SEQLIB_CHECK(!dry.at(7).empty() && std::none_of(dry.at(7).begin(), dry.at(7).end(), [](bool d) { return d; }));

    // In the drum kit, the zones of both noises are dry. The sample's zone isn't. In the key split, the zones of
    // square 2 cover the keys up to 59 and are dry. The sample's zone isn't.
    SEQLIB_CHECK((dry.at(8) == std::vector<bool>{true, true, false}));
    const std::vector<bool>& split = dry.at(9);
    SEQLIB_CHECK(split.size() >= 3 && std::all_of(split.begin(), split.end() - 1, [](bool d) { return d; }) &&
                 !split.back());
}

// A compressed sample decodes to its first point and then a step from the table for each of the others: from the low
// half of the second byte, and then from the high and low halves of each byte after it.
void TestCompressedSample()
{
    // One block: the first point is 10, the second byte's low half (2) adds 4, the third byte's halves (1 and 8) add 1
    // and -64, and the rest add 0. The second byte's high half isn't used.
    Cart cart;
    std::vector<uint8_t> block(33, 0);
    block[0] = 10;
    block[1] = 0xF2;
    block[2] = 0x18;
    const uint32_t wave = cart.CompressedWave(block, 64);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, wave)}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("compressed");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The sample holds the master volume of 12 too, at 13/16, so each of its points is 208 times the decoded one.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    const uint32_t start = ntdlib::testing::agb_files::Le32(sf.Record("shdr", 46, 0) + 20);
    const auto point = [&](uint32_t i)
    {
        return int16_t(Le16(&pcm[2 * (start + i)])) / (256 * 13 / 16);
    };

    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(point(0) == 10 && point(1) == 14 && point(2) == 15 && point(3) == -49 && point(63) == -49);
}

// Each frame's output of the model's mixer, on each side.
struct MixedFrames
{
    std::vector<std::vector<int8_t>> right;
    std::vector<std::vector<int8_t>> left;
};

// Runs a sequencer for `frames` frames with the model mixing, and keeps each frame's output.
MixedFrames Mix(Sequencer& seq, int frames)
{
    MixedFrames out;
    seq.StartMixing();
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        out.right.emplace_back(seq.Right().begin(), seq.Right().end());
        out.left.emplace_back(seq.Left().begin(), seq.Left().end());
    }

    return out;
}

// Returns the number of the first frame whose output isn't silent, or -1.
int FirstSound(const MixedFrames& m)
{
    const auto loud = [](int8_t point)
    {
        return point != 0;
    };
    for (size_t f = 0; f < m.right.size(); f++)
    {
        if (std::any_of(m.right[f].begin(), m.right[f].end(), loud) ||
            std::any_of(m.left[f].begin(), m.left[f].end(), loud))
        {
            return int(f);
        }
    }

    return -1;
}

// Returns the volume at which the mixer plays a channel on a side: the channel's volume there times its envelope's
// level, over 256. The level is the envelope times the master volume plus 1, over 16. Each is rounded down.
int MixVolume(const DriverInfo& info, int volume, int envelope)
{
    return (volume * ((envelope * (info.master_volume + 1)) >> 4)) >> 8;
}

// Returns what a point of `value` at `volume` adds to a byte of the output: the product over 256, rounded down.
uint8_t Added(int32_t value, int volume)
{
    return uint8_t((value * volume) >> 8);
}

// Returns the reverb's point for `sum`, the sum of the points that the reverb adds up: the sum times `reverb`, over 2
// to the `shift` and rounded down. A result whose bit 7 is set goes up by 1.
int8_t ReverbPoint(int32_t sum, int reverb, int shift)
{
    int32_t value = (sum * reverb) >> shift;
    if (value & 0x80)
    {
        value++;
    }

    return int8_t(value);
}

// Returns a sample of `length` points that jump about over the whole range.
std::vector<int8_t> Jumpy(size_t length)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 37 % 256) - 128));
    }

    return pcm;
}

// Decodes a compressed sample's blocks of 33 bytes. A block's first byte is its first point. The low half of its
// second byte gives the second point's step from the first, and the two halves of each later byte the next two steps,
// the high half first.
std::vector<int8_t> Decompress(const std::vector<uint8_t>& blocks)
{
    static const int kSteps[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};
    std::vector<int8_t> out;
    for (size_t b = 0; b + 33 <= blocks.size(); b += 33)
    {
        uint8_t point = blocks[b];
        out.push_back(int8_t(point));
        point = uint8_t(point + kSteps[blocks[b + 1] & 0xF]);
        out.push_back(int8_t(point));
        for (size_t k = 2; k < 33; k++)
        {
            for (int half : {blocks[b + k] >> 4, blocks[b + k] & 0xF})
            {
                point = uint8_t(point + kSteps[half]);
                out.push_back(int8_t(point));
            }
        }
    }

    return out;
}

void TestMixFixedPitch()
{
    // Four tracks play a sample of 224 points at a fixed pitch: one frame of the mixer's output at 13379 Hz. Three
    // play at the centre, and one on the left. Each of the frame's points is the sum of each channel's point times its
    // volume over 256, rounded down. The sum wraps round at 8 bits. The channels stop at the sample's end.
    Cart cart;
    const std::vector<int8_t> pcm = Jumpy(224);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    const auto track = [](int pan)
    {
        return Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, pan).Note(24, 60, 127).Wait(24).Fine();
    };
    cart.Song({track(0x40), track(0x40), track(0x40), track(0x10)}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 4);
    const int f = FirstSound(m);
    SEQLIB_REQUIRE(f >= 0 && f + 1 < int(m.right.size()));

    bool wrapped = false;
    bool sides_differ = false;
    for (size_t i = 0; i < pcm.size(); i++)
    {
        uint8_t right = 0;
        uint8_t left = 0;
        int sum = 0;
        for (int c = 0; c < 4; c++)
        {
            const Channel& channel = seq.GetChannel(c);
            SEQLIB_CHECK(channel.status == 0 && channel.envelope == 255);
            const int volume = MixVolume(info, channel.right, channel.envelope);
            right = uint8_t(right + Added(pcm[i], volume));
            left = uint8_t(left + Added(pcm[i], MixVolume(info, channel.left, channel.envelope)));
            sum += int8_t(Added(pcm[i], volume));
        }

        SEQLIB_CHECK_EQ(int(m.right[size_t(f)][i]), int(int8_t(right)));
        SEQLIB_CHECK_EQ(int(m.left[size_t(f)][i]), int(int8_t(left)));
        wrapped = wrapped || sum < -128 || sum > 127;
        sides_differ = sides_differ || right != left;
    }

    SEQLIB_CHECK(wrapped && sides_differ);
    SEQLIB_CHECK(
        FirstSound(MixedFrames{{m.right.begin() + f + 1, m.right.end()}, {m.left.begin() + f + 1, m.left.end()}}) < 0);
}

// Checks that a channel's output for `frames` frames from frame `first` interpolates between the points that `point`
// gives. The channel's position starts at point `start`. It moves by the channel's step for each point of the output:
// forwards for a `direction` of 1, and backwards for -1. Each point of the output is the point at the position, plus
// the difference to the next point times the fraction between them, over 2^23 and rounded down. The product wraps round
// at 32 bits.
template <typename Point>
void CheckInterpolation(const MixedFrames& m, const DriverInfo& info, const Channel& c, int first, int frames,
                        int32_t start, int direction, Point point)
{
    const uint32_t step = c.frequency * info.step_scale;
    SEQLIB_CHECK((step & 0x7FFFFF) != 0);
    SEQLIB_REQUIRE(first >= 0 && first + frames <= int(m.right.size()));

    const int right = MixVolume(info, c.right, c.envelope);
    const int left = MixVolume(info, c.left, c.envelope);
    int32_t position = start;
    uint32_t fraction = 0;
    int differences = 0;
    for (int f = first; f < first + frames; f++)
    {
        for (int i = 0; i < info.samples_per_frame; i++)
        {
            const int32_t s0 = point(position);
            const int32_t s1 = point(position + direction);
            const int32_t value = s0 + (int32_t(fraction * uint32_t(s1 - s0)) >> 23);
            differences += m.right[size_t(f)][size_t(i)] != int8_t(Added(value, right));
            differences += m.left[size_t(f)][size_t(i)] != int8_t(Added(value, left));

            fraction += step;
            position += direction * int32_t(fraction >> 23);
            fraction &= 0x7FFFFF;
        }
    }

    SEQLIB_CHECK_EQ(differences, 0);
}

void TestMixInterpolation()
{
    // A saw wave plays a fifth above its rate. The saw's points fall by 252 where it wraps round.
    Cart cart;
    const std::vector<int8_t> pcm = Saw(2000);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 4);
    const Channel& c = seq.GetChannel(0);
    SEQLIB_CHECK(c.status != 0 && c.envelope == 255);
    CheckInterpolation(m, info, c, FirstSound(m), 3, 0, 1, [&](int32_t at) { return int32_t(pcm.at(size_t(at))); });
}

void TestMixSpecialSamples()
{
    // A compressed sample of 8 blocks, and a saw wave played backwards. Both play a fifth above their rates.
    Cart cart;
    std::vector<uint8_t> blocks(8 * 33);
    uint32_t seed = 1;
    for (uint8_t& byte : blocks)
    {
        seed = seed * 1103515245u + 12345u;
        byte = uint8_t(seed >> 16);
    }
    const std::vector<int8_t> points = Decompress(blocks);
    const std::vector<int8_t> pcm = Saw(1200);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, cart.CompressedWave(blocks, 512))},
                                             {1, Cart::Voice(kVoiceReverse, cart.Wave(pcm))}});
    for (int voice = 0; voice < 2; voice++)
    {
        cart.Song({Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(24).Fine()}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.special_samples && points.size() == 512);

    // The compressed sample's position counts points from its start. The reversed sample plays from its last point
    // backwards.
    Sequencer compressed(rom, info, 0);
    const MixedFrames cm = Mix(compressed, 2);
    CheckInterpolation(cm, info, compressed.GetChannel(0), FirstSound(cm), 1, 0, 1,
                       [&](int32_t at) { return int32_t(points.at(size_t(at))); });

    Sequencer reversed(rom, info, 1);
    const MixedFrames rm = Mix(reversed, 3);
    CheckInterpolation(rm, info, reversed.GetChannel(0), FirstSound(rm), 2, int32_t(pcm.size()) - 1, -1,
                       [&](int32_t at) { return int32_t(pcm.at(size_t(at))); });
}

// Checks the reverb in each frame after `first`. Nothing plays in those frames. A frame's part of the buffer holds the
// reverb of two frames' output: the frame a buffer's length of frames before it, and the frame after that. The first
// wrote the same part. The stereo mixer adds both sides of both frames, and gives each side the same reverb. The mono
// mixer adds its one side.
void CheckReverb(const MixedFrames& m, const DriverInfo& info, int first, int reverb)
{
    const int period = int(ntdlib::mp2k::kPcmBufferSize) / info.samples_per_frame;
    const auto at = [&](const std::vector<std::vector<int8_t>>& side, int f, int i)
    {
        return f < 0 ? 0 : int32_t(side[size_t(f)][size_t(i)]);
    };

    int differences = 0;
    for (int f = first + 1; f < int(m.right.size()); f++)
    {
        for (int i = 0; i < info.samples_per_frame; i++)
        {
            const int8_t expected =
                info.mono ? ReverbPoint(at(m.right, f - period, i) + at(m.right, f - period + 1, i), reverb, 8)
                          : ReverbPoint(at(m.right, f - period, i) + at(m.left, f - period, i) +
                                            at(m.right, f - period + 1, i) + at(m.left, f - period + 1, i),
                                        reverb, 9);
            differences += m.right[size_t(f)][size_t(i)] != expected;
            differences += m.left[size_t(f)][size_t(i)] != expected;
        }
    }

    SEQLIB_CHECK_EQ(differences, 0);
    SEQLIB_CHECK(m.right.size() > size_t(first + 3 * period));
    SEQLIB_CHECK(FirstSound(MixedFrames{{m.right.begin() + first + 2 * period, m.right.end()},
                                        {m.left.begin() + first + 2 * period, m.left.end()}}) >= 0);
}

void TestMixReverb()
{
    // A sample of 224 points plays for one frame at a fixed pitch, on the left. The song sets a reverb of 100.
    Cart cart;
    const std::vector<int8_t> pcm = Jumpy(224);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x10).Note(24, 60, 127).Wait(48).Fine()},
              voices, 0, 0, 0x80 | 100);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 40);
    const int f = FirstSound(m);
    SEQLIB_REQUIRE(f >= 0);

    const Channel& c = seq.GetChannel(0);
    const int right = MixVolume(info, c.right, c.envelope);
    const int left = MixVolume(info, c.left, c.envelope);
    SEQLIB_CHECK(c.status == 0 && right != left);
    for (size_t i = 0; i < pcm.size(); i++)
    {
        SEQLIB_CHECK_EQ(int(m.right[size_t(f)][i]), int(int8_t(Added(pcm[i], right))));
        SEQLIB_CHECK_EQ(int(m.left[size_t(f)][i]), int(int8_t(Added(pcm[i], left))));
    }
    CheckReverb(m, info, f, 100);
}

void TestMixMono()
{
    // The mono mixer plays a channel at the average of its volumes. Its reverb is the sum of its one side's points
    // times the reverb, over 256.
    Cart cart;
    const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
    for (uint32_t i = 0; i < 12; i++)
    {
        cart.Put8(kMonoVolume + i, kMono[i]);
    }
    const std::vector<int8_t> pcm = Jumpy(224);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x10).Note(24, 60, 127).Wait(48).Fine()},
              voices, 0, 0, 0x80 | 100);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.mono);

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, 40);
    const int f = FirstSound(m);
    SEQLIB_REQUIRE(f >= 0);

    const Channel& c = seq.GetChannel(0);
    const int volume = ((c.right + c.left) * ((c.envelope * (info.master_volume + 1)) >> 4)) >> 9;
    SEQLIB_CHECK(c.status == 0 && c.right != c.left);
    for (size_t i = 0; i < pcm.size(); i++)
    {
        SEQLIB_CHECK_EQ(int(m.right[size_t(f)][i]), int(int8_t(Added(pcm[i], volume))));
    }
    SEQLIB_CHECK(m.left == m.right);
    CheckReverb(m, info, f, 100);
}

// A store to a sound register: the address's low byte, the size in bytes and the value.
using Store = std::tuple<uint32_t, int, uint32_t>;

// A frame's stores to the sound registers, and PSG channel `n`'s state after it.
struct StoreFrame
{
    std::vector<Store> stores;
    Channel channel;
};

// Runs `frames` frames, and returns those that store to the sound registers.
std::vector<StoreFrame> StoreFrames(Sequencer& seq, int frames, int n)
{
    std::vector<StoreFrame> out;
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        StoreFrame frame;
        for (const ntdlib::agb::RegisterWrite& w : seq.SoundWrites())
        {
            frame.stores.emplace_back(w.address & 0xFF, w.size, w.value);
        }
        frame.channel = seq.GetChannel(kFirstPsgChannel + n - 1);
        if (!frame.stores.empty())
        {
            out.push_back(frame);
        }
    }

    return out;
}

void TestPsgSquareStores()
{
    // A note on square channel 1 starts with the sweep in NR10 (0x60), the duty and the length in NR11 (0x62), the
    // frequency in NR13 and NR14 (0x64, 0x65), both outputs in NR51 (0x81, bits 0 and 4), then the envelope in NR12
    // (0x63): a level of 0 that the hardware raises a step every 1/64 s, with bit 3 for upwards. NR14's bit 7 restarts
    // the channel, and the driver restarts it a second time while NR10's bit 3 is clear. At the attack's end, NR12
    // takes the sustain's level with a step time of 0. A bend writes the new frequency alone. The release of 0 and the
    // echo of 0 turn the channel off: NR12 = 8, then NR14 = 0x80.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {1, 0, 15, 0}, 0x15)}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(24, 60, 127)
                   .Wait(20)
                   .Cmd(kCmdBend, 0x50)
                   .Wait(4)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 60, 1);
    const uint32_t f = KeyToPsgFrequency(1, 60, 0);
    const uint32_t bent = KeyToPsgFrequency(1, 60, 128);
    SEQLIB_REQUIRE_EQ(frames.size(), 4);

    const std::vector<Store> start = {
        {0x60, 1, 0x15}, {0x62, 1, 0x80}, {0x64, 1, f & 0xFF},        {0x65, 1, f >> 8},
        {0x81, 1, 0x11}, {0x63, 1, 0x09}, {0x65, 1, 0x80 | (f >> 8)}, {0x65, 1, 0x80 | (f >> 8)}};
    SEQLIB_CHECK(frames[0].stores == start);
    SEQLIB_CHECK_EQ(frames[0].channel.envelope, 0);

    const int level = frames[1].channel.envelope;
    const std::vector<Store> sustain = {{0x81, 1, 0x11},
                                        {0x63, 1, uint32_t(0x08 + (level << 4))},
                                        {0x65, 1, 0x80 | (f >> 8)},
                                        {0x65, 1, 0x80 | (f >> 8)}};
    SEQLIB_CHECK_EQ(frames[1].channel.status, ntdlib::mp2k::kPhaseSustain);
    SEQLIB_CHECK(level > 0 && level == frames[1].channel.goal);
    SEQLIB_CHECK(frames[1].stores == sustain);

    SEQLIB_CHECK(frames[2].stores == std::vector<Store>({{0x64, 1, bent & 0xFF}, {0x65, 1, bent >> 8}}));
    SEQLIB_CHECK(frames[3].stores == std::vector<Store>({{0x63, 1, 0x08}, {0x65, 1, 0x80}}));
    SEQLIB_CHECK_EQ(frames[3].channel.status, 0);
}

void TestPsgWaveStores()
{
    // A note on the wave channel loads its pattern into the wave RAM (0x90 to 0x9F) in four words, while NR30 (0x70)
    // has the channel play the other bank. NR30 then turns the channel off, NR31 (0x72) takes the length, and NR33 and
    // NR34 (0x74, 0x75) the frequency with the length's enable in bit 6 and the restart in bit 7. NR32 (0x73) takes the
    // volume code of the envelope's level: mute, 25%, 50%, 75% or full. NR30's bit 7 turns the channel on, and NR34
    // restarts it. A second note with the same pattern leaves the wave RAM alone. A note with another pattern loads it.
    // A note's end turns the channel off in NR30.
    static const uint8_t kVolumes[16] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40,
                                         0x40, 0x40, 0x80, 0x80, 0x80, 0x80, 0x20, 0x20};
    Cart cart;
    std::vector<uint8_t> first(16);
    std::vector<uint8_t> second(16);
    for (size_t i = 0; i < 16; i++)
    {
        first[i] = uint8_t(0x10 * i + 1);
        second[i] = uint8_t(0xF0 - 0x10 * i);
    }
    const uint32_t a = cart.Place(first);
    const uint32_t b = cart.Place(second);
    const uint32_t voices = cart.VoiceGroup(
        {{0, Cart::Voice(3, a, 60, {0, 0, 15, 0}, 0, 5)}, {1, Cart::Voice(3, b, 60, {0, 0, 15, 0}, 0, 5)}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(4, 60, 127)
                   .Wait(8)
                   .Note(4, 60, 127)
                   .Wait(8)
                   .Cmd(kCmdVoice, 1)
                   .Note(4, 60, 127)
                   .Wait(8)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 40, 3);
    const uint32_t f = KeyToPsgFrequency(3, 60, 0);
    SEQLIB_REQUIRE_EQ(frames.size(), 6);

    const auto start = [&](const std::vector<uint8_t>* pattern, int level)
    {
        std::vector<Store> stores;
        if (pattern)
        {
            stores.emplace_back(0x70, 1, 0x40);
            for (uint32_t i = 0; i < 4; i++)
            {
                stores.emplace_back(0x90 + 4 * i, 4, Le32(pattern->data() + 4 * i));
            }
        }
        const std::vector<Store> rest = {{0x70, 1, 0x00},     {0x72, 1, 5},
                                         {0x74, 1, f & 0xFF}, {0x75, 1, 0xC0 | (f >> 8)},
                                         {0x81, 1, 0x44},     {0x73, 1, kVolumes[level]},
                                         {0x70, 1, 0x80},     {0x75, 1, 0xC0 | (f >> 8)}};
        stores.insert(stores.end(), rest.begin(), rest.end());
        return stores;
    };
    const std::vector<Store> off = {{0x70, 1, 0x00}};

    const int level = frames[0].channel.envelope;
    SEQLIB_CHECK(level > 1 && level < 16);
    SEQLIB_CHECK(frames[0].stores == start(&first, level));
    SEQLIB_CHECK(frames[1].stores == off);
    SEQLIB_CHECK(frames[2].stores == start(nullptr, level));
    SEQLIB_CHECK(frames[3].stores == off);
    SEQLIB_CHECK(frames[4].stores == start(&second, level));
    SEQLIB_CHECK(frames[5].stores == off);
}

void TestPsgNoiseStores()
{
    // A note on the noise channel puts the length in NR41 (0x78) and its 7-bit type in NR43's bit 3 (0x7C). The pitch
    // then goes in NR43's other bits, and NR44 (0x7D) takes the length's enable. NR51's bits 3 and 7 give both outputs.
    // NR42 (0x79) takes the sustain's level, with bit 3 set: the attack and the decay of 0 skip to the sustain. NR44's
    // bit 7 restarts the channel once.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(4, 1, 60, {0, 0, 15, 0}, 0, 0x20)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(8).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 20, 4);
    const uint32_t f = KeyToPsgFrequency(4, 60, 0);
    SEQLIB_REQUIRE_EQ(frames.size(), 2);

    const int level = frames[0].channel.envelope;
    const std::vector<Store> start = {{0x78, 1, 0x20}, {0x7C, 1, 0x08}, {0x7C, 1, (0x08 | f) & 0xFF},
                                      {0x7D, 1, 0x40}, {0x81, 1, 0x88}, {0x79, 1, uint32_t(0x08 + (level << 4))},
                                      {0x7D, 1, 0xC0}};
    SEQLIB_CHECK(level > 0);
    SEQLIB_CHECK(frames[0].stores == start);
    SEQLIB_CHECK(frames[1].stores == std::vector<Store>({{0x79, 1, 0x08}, {0x7D, 1, 0x80}}));
}

void TestPsgWaveVolumes()
{
    // The wave channel's NR32 (0x73) takes the volume code of the envelope's level: levels 0 and 1 mute it, 2 to 5 play
    // at 25%, 6 to 9 at 50%, 10 to 13 at 75% and 14 and 15 at the full volume. Notes at falling velocities start at
    // falling levels.
    static const uint8_t kVolumes[16] = {0x00, 0x00, 0x60, 0x60, 0x60, 0x60, 0x40, 0x40,
                                         0x40, 0x40, 0x80, 0x80, 0x80, 0x80, 0x20, 0x20};
    Cart cart;
    const uint32_t pattern = cart.Place(std::vector<uint8_t>(16, 0x5A));
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(3, pattern, 60, {0, 0, 15, 0})}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127);
    for (int velocity = 127; velocity > 0; velocity -= 8)
    {
        track.Note(2, 60, uint8_t(velocity)).Wait(4);
    }
    cart.Song({track.Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::set<int> levels;
    int starts = 0;
    for (const StoreFrame& frame : StoreFrames(seq, 80, 3))
    {
        for (const Store& s : frame.stores)
        {
            if (std::get<0>(s) == 0x73)
            {
                const int level = frame.channel.envelope;
                SEQLIB_REQUIRE(level < 16);
                SEQLIB_CHECK_EQ(std::get<2>(s), kVolumes[level]);
                levels.insert(level);
                starts++;
            }
        }
    }
    SEQLIB_CHECK_EQ(starts, 16);
    SEQLIB_CHECK(levels.size() >= 14);
}

void TestEnvelopeCounter()
{
    // A square's decay of 1 count a step falls a level a frame after the note's first frame, until the channel stops at
    // level 0. The frame in which the envelopes' frame counter reaches 0 takes another count. With the counter set to
    // c, that's each frame n, counting from 1, with n mod 15 = c.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 1, 0, 0})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(96, 60, 127).Wait(96).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    for (int counter : {0, 1, 5, 14})
    {
        Sequencer seq(rom, info, 0);
        seq.SetEnvelopeCounter(counter);
        int level = 15;
        bool matches = true;
        for (int n = 1; n <= 24; n++)
        {
            seq.Step();
            level -= (n > 1 ? 1 : 0) + (n % 15 == counter ? 1 : 0);
            const Channel& c = seq.GetChannel(kFirstPsgChannel);
            matches &= (c.status ? int(c.envelope) : 0) == std::max(level, 0);
        }
        SEQLIB_CHECK(matches);
    }

    Sequencer seq(rom, info, 0);
    for (int counter : {-1, 15})
    {
        bool threw = false;
        try
        {
            seq.SetEnvelopeCounter(counter);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        SEQLIB_CHECK(threw);
    }
}

void TestPsgEchoStores()
{
    // After a release of 0, a track's echo holds the note at the echo's share of its level: 128 of 256, rounded up.
    // NR12 (0x63) takes that level with bit 3 set and a step time of 0, and NR14 (0x65) restarts the channel. A sweep
    // of 0 leaves NR10's bit 3 set, so the channel restarts once. The echo lasts 3 frames, then NR12 = 8 and NR14 =
    // 0x80 turn the channel off.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Raw({kCmdXcmd, kXcmdEchoVol, 128, kCmdXcmd, kXcmdEchoLen, 3})
                   .Note(4, 60, 127)
                   .Wait(12)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<int> frame_numbers;
    std::vector<StoreFrame> frames;
    for (int f = 0; f < 30; f++)
    {
        for (const StoreFrame& frame : StoreFrames(seq, 1, 1))
        {
            frames.push_back(frame);
            frame_numbers.push_back(f);
        }
    }
    SEQLIB_REQUIRE_EQ(frames.size(), 3);

    const uint32_t f = KeyToPsgFrequency(1, 60, 0);
    const int goal = frames[0].channel.goal;
    const int echo = (goal * 128 + 255) >> 8;
    SEQLIB_CHECK(echo > 0);
    SEQLIB_CHECK_EQ(frames[1].channel.envelope, echo);
    SEQLIB_CHECK(
        frames[1].stores ==
        std::vector<Store>({{0x81, 1, 0x11}, {0x63, 1, uint32_t(0x08 + (echo << 4))}, {0x65, 1, 0x80 | (f >> 8)}}));
    SEQLIB_CHECK(frames[2].stores == std::vector<Store>({{0x63, 1, 0x08}, {0x65, 1, 0x80}}));
    SEQLIB_CHECK_EQ(frame_numbers[2] - frame_numbers[1], 3);
}

void TestPsgStartingRegisters()
{
    // The driver sets a channel's bits of NR51 and keeps the others as it reads them. A game that left channel 2's
    // outputs on keeps them through channel 1's note. The registers come as 64 bytes from 0x04000060.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(8).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<uint8_t> registers(ntdlib::agb::kSoundRegisterBytes, 0);
    registers[0x21] = 0x22;
    seq.SetSoundRegisters(registers);
    const std::vector<StoreFrame> frames = StoreFrames(seq, 20, 1);
    SEQLIB_REQUIRE(!frames.empty());
    bool outputs = false;
    for (const Store& s : frames[0].stores)
    {
        outputs = outputs || (std::get<0>(s) == 0x81 && std::get<2>(s) == 0x33);
    }
    SEQLIB_CHECK(outputs);

    bool threw = false;
    try
    {
        seq.SetSoundRegisters(std::vector<uint8_t>(16, 0));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    SEQLIB_CHECK(threw);
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
    const std::vector<std::pair<uint32_t, uint32_t>> kTempos = {
        {0, Micros(0.75)}, {2 * kMidiTicks, Micros(1.5)}, {3 * kMidiTicks, Micros(3)}};
    SEQLIB_CHECK(Tempos(ReadMidi(r.midi_path)) == kTempos);
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

// Camelot's version of the driver has an unused copy of the routine that starts a song, whose literal pool is zeroed,
// before the one the game uses. Its note start and mixer have changes of their own, which detection finds by their
// code.
void TestCamelotDetection()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}});
    cart.Song({Track().Fine()}, voices);
    Cart plain;
    plain.Song({Track().Fine()}, voices);

    const DriverInfo info = Detect(cart.ToRom());
    const DriverInfo plain_info = Detect(plain.ToRom());

    SEQLIB_CHECK_EQ(info.song_start, kSongStart);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.sound_mode, kMode);
    SEQLIB_CHECK(info.camelot_sequencer && info.camelot_mixer);
    SEQLIB_CHECK(!plain_info.camelot_sequencer && !plain_info.camelot_mixer);

    // Without an echo's literals, the echo is the earlier version's, and the left side's tap lies a third of a frame
    // back.
    SEQLIB_CHECK_EQ(info.camelot_output_points, 2);
    SEQLIB_CHECK_EQ(info.camelot_echo, 53);
    SEQLIB_CHECK_EQ(info.camelot_cross_echo, -8);
    SEQLIB_CHECK_EQ(info.camelot_tap, 74);
    SEQLIB_CHECK(!info.camelot_adpcm && !info.camelot_top_priority);
    SEQLIB_CHECK_EQ(info.max_channels, 5);
}

// Camelot's note start takes the third free channel, or the last free one when there are only one or two, unless a
// released note of the same track has a channel. It works out the left side's level from 128 rather than 127, and
// rounds a fine-tuned key's rate a little differently. Its LFO counts its delay down at a speed of 0 too.
void TestCamelotSequencer()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 250})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 127)
                   .Tie(62, 127)
                   .Tie(64, 127)
                   .Tie(65, 127)
                   .Wait(1)
                   .Eot(60)
                   .Wait(1)
                   .Tie(67, 127)
                   .Wait(1)
                   .Tie(69, 127)
                   .Wait(10)
                   .Fine()},
              voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Cmd(kCmdMod, 30)
                   .Cmd(kCmdLfoDl, 4)
                   .Cmd(kCmdLfoS, 0)
                   .Tie(60, 127)
                   .Wait(2)
                   .Cmd(kCmdLfoS, 32)
                   .Wait(10)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<std::pair<int, int>> ons;
    int left = -1;
    for (int f = 0; f < 6; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                ons.push_back({a.key, a.channel});
            }
        }
        if (f == 0)
        {
            left = seq.GetChannel(2).left;
        }
    }

    Sequencer lfo(rom, info, 1);
    std::vector<Action> pitches = Actions(lfo, 12, Action::kPitch);
    pitches.erase(std::remove_if(pitches.begin(), pitches.end(), [](const Action& a) { return a.value == 0; }),
                  pitches.end());

    // Of 5 channels, keys 60, 62 and 64 take the third free one each time, and 65 the last of the two left. Once 60 is
    // released, 67 takes its channel rather than the free channel 0, which 69 takes.
    const std::vector<std::pair<int, int>> kOns = {{60, 2}, {62, 3}, {64, 4}, {65, 1}, {67, 2}, {69, 0}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK_EQ(left, 125);
    SEQLIB_CHECK_EQ(KeyToFrequency(13379 * 1024, 11, 255), 836);
    SEQLIB_CHECK_EQ(CamelotKeyToFrequency(13379 * 1024, 11, 255), 835);
    SEQLIB_CHECK_EQ(CamelotKeyToFrequency(13379 * 1024, 60, 0), 13379);

    // The LFO's delay of 4 ticks has run out by tick 4, two ticks after its speed is set: a depth of 30 on the first
    // step of 32 bends the pitch by 15/16 of a semitone.
    SEQLIB_CHECK(!pitches.empty() && pitches[0].tick == 4 && pitches[0].value == 240);
}

// Camelot's mixer takes 256 less the release off the level each frame. It leaves out a note that comes out silent on
// both sides, which doesn't move on through its sample. It plays a sample of no length as a synth voice, which goes on
// until it's released: a pulse wave moves its duty on each frame, and a saw wave keeps its filter in the count.
void TestCamelotMixer()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x00, 0x00});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 200})},
                                             {1, Cart::Voice(0, pulse)},
                                             {2, Cart::Voice(0, saw)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(4, 60, 127).Wait(12).Fine()},
              voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 1).Tie(60, 1).Wait(12).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Tie(60, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Tie(60, 127).Wait(12).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The note's level after each frame from its release, on frame 4, until it stops.
    Sequencer release(rom, info, 0);
    std::vector<int> levels;
    for (int f = 0; f < 9; f++)
    {
        release.Step();
        if (f >= 4)
        {
            levels.push_back(release.GetChannel(2).status ? release.GetChannel(2).envelope : -1);
        }
    }

    Sequencer silent(rom, info, 1);
    for (int f = 0; f < 3; f++)
    {
        silent.Step();
    }

    // The model refuses an echo that doesn't reach back within a frame.
    DriverInfo far_echo = info;
    far_echo.camelot_tap = info.samples_per_frame;
    bool refused = false;
    try
    {
        Sequencer(rom, far_echo, 0).StartMixing();
    }
    catch (const std::invalid_argument&)
    {
        refused = true;
    }

    Sequencer synths(rom, info, 2);
    synths.Step();
    const int32_t saw_filter = synths.GetChannel(3).count;
    synths.Step();
    synths.Step();
    const std::vector<int16_t> saw_points = RenderSynth(rom, info, Synth::kSaw, saw + 16, 13379, 224);

    const std::vector<int> kLevels = {199, 143, 87, 31, -1};
    SEQLIB_CHECK(levels == kLevels);
    SEQLIB_CHECK(refused);
    SEQLIB_CHECK(silent.GetChannel(2).status && silent.GetChannel(2).count == 64);

    // The pulse wave's phase moves on by 8 times a sample's step for each of the mixer's 224 points a frame, and its
    // duty's phase by its fourth byte. The saw wave's filter ends the frame where the SoundFont's sample does.
    const Channel& p = synths.GetChannel(2);
    SEQLIB_CHECK(p.status && p.synth && uint32_t(p.count) == 0x30000000u);
    SEQLIB_CHECK_EQ(p.fraction, uint32_t(3 * uint64_t((13379u * 627u) << 3) * 224 % 0x100000000u));
    SEQLIB_CHECK(synths.GetChannel(3).status && saw_points.size() == 224 && saw_points[223] == saw_filter * 128);
}

// Returns the volumes at which Camelot's mixer plays channel `c`, right then left: each side's volume times 9/8 of
// the envelope, over 512 and rounded down (0x03000846 in Golden Sun: The Lost Age's mixer).
std::array<int32_t, 2> CamelotVolumes(const Channel& c)
{
    const int32_t level = c.envelope + (c.envelope >> 3);
    return {(c.right * level) >> 9, (c.left * level) >> 9};
}

// Returns the two points that Camelot's output stage writes for a side's sum, after the 64 that rounds it when `round`
// (0x03000d40). A negative sum takes 64 more. The first point is the sum over 128, rounded down, and the second that of
// the sum less 64.
std::array<int8_t, 2> CamelotPoints(int32_t sum, bool round)
{
    sum += round ? 64 : 0;
    sum += sum < 0 ? 64 : 0;
    return {int8_t(sum >> 7), int8_t((sum - 64) >> 7)};
}

// Works out the points that Camelot's output stage writes over whole frames, side by side, from what the channels add
// to each sum: `added[f][side][i]`, the right side then the left. `many[f]` says whether frame f mixed 4 channels or
// more: only the first two sums then take the 64 that rounds them. Each frame's sums start with an echo of the points
// before (0x03000e00 to 0x03000e60). Each side takes 53/128 of its first points from 4 frames before, less 8/128 of
// the other side's. The right side also takes a quarter of its second points from the frame before, plus 1/128 of that.
// The left side takes a quarter of the right side's first points from 528 + 176 points before. The right side's sums
// have to stay from 64 to 16319, and the left side's from 0: the output stage then neither saturates nor carries from
// one side to the other.
MixedFrames CamelotFrames(const std::vector<std::array<std::vector<int32_t>, 2>>& added, const std::vector<bool>& many)
{
    constexpr size_t kPoints = size_t(kCamelotPoints);
    constexpr size_t kLeftEcho = 176;
    const size_t frames = added.size();
    std::array<std::vector<int32_t>, 2> first = {std::vector<int32_t>(frames * kPoints),
                                                 std::vector<int32_t>(frames * kPoints)};
    std::vector<int32_t> second(frames * kPoints);
    MixedFrames out;
    for (size_t f = 0; f < frames; f++)
    {
        out.right.emplace_back(2 * kPoints);
        out.left.emplace_back(2 * kPoints);
        for (size_t i = 0; i < kPoints; i++)
        {
            const size_t n = f * kPoints + i;
            std::array<int32_t, 2> sums = {added[f][0][i], added[f][1][i]};
            if (f >= 4)
            {
                const size_t back = n - 4 * kPoints;
                sums[0] += 53 * first[0][back] - 8 * first[1][back];
                sums[1] += 53 * first[1][back] - 8 * first[0][back];
            }
            if (f >= 1)
            {
                sums[0] += 32 * second[n - kPoints] + second[n - kPoints] / 4;
            }
            if (n >= kPoints + kLeftEcho)
            {
                sums[1] += 32 * first[0][n - kPoints - kLeftEcho];
            }
            SEQLIB_CHECK(sums[0] >= 64 && sums[0] < 16320 && sums[1] >= 0 && sums[1] < 16320);

            const bool round = i < 2 || !many[f];
            const std::array<int8_t, 2> right = CamelotPoints(sums[0], round);
            const std::array<int8_t, 2> left = CamelotPoints(sums[1], round);
            first[0][n] = right[0];
            first[1][n] = left[0];
            second[n] = right[1];
            out.right[f][2 * i] = right[0];
            out.right[f][2 * i + 1] = right[1];
            out.left[f][2 * i] = left[0];
            out.left[f][2 * i + 1] = left[1];
        }
    }

    return out;
}

// Returns the values that Camelot's mixer takes from a sample whose points all hold `point`, in a frame from the
// sample's start, at `step` 2^-23 points for each of the frame's points (0x03000940). The sample has to hold more
// points than the frame takes. Below a step of 1, the values are twice the points. The step goes on to the next point
// when the fraction, kept less 1, reaches 0, and at each point from a step of 1 up. The first value of each 8 holds the
// point. A later one that comes just after the step goes on to a point holds the last value shifted right by 22 or 23
// bits instead, added to the point: 0, or -1 when that value was negative.
std::vector<int32_t> CamelotConstantValues(int32_t point, uint32_t step)
{
    const bool below_one = step < 0x800000;
    std::vector<int32_t> values;
    uint32_t fraction = 0;
    bool moved = false;
    for (int i = 0; i < kCamelotPoints; i++)
    {
        const int32_t value = below_one ? 2 * point : point;
        const bool shifted = i % 8 != 0 && moved && !values.empty() && values.back() < 0;
        values.push_back(shifted ? value - 1 : value);

        moved = true;
        if (below_one)
        {
            fraction += step - 0x800000;
            moved = int32_t(fraction) >= 0;
            fraction &= 0x7FFFFF;
        }
    }

    return values;
}

// Returns the value that Camelot's mixer takes from point `k` of `pcm` and the next point, at `fraction` of the way
// from one to the other in 2^-23 points. The value is twice the point interpolated to 9 bits when `nine_bits`, and
// otherwise the point interpolated to 8 bits (0x03000940 and 0x03000b44).
int32_t CamelotValue(const std::vector<int8_t>& pcm, size_t k, uint32_t fraction, bool nine_bits)
{
    const int32_t point = pcm[k];
    const int32_t product = int32_t(fraction * uint32_t(pcm[k + 1] - point));
    return nine_bits ? 2 * point + (product >> 22) : point + (product >> 23);
}

// The channels of a sequencer that sound, in order.
std::vector<Channel> Sounding(const Sequencer& seq, const DriverInfo& info)
{
    std::vector<Channel> channels;
    for (int i = 0; i < info.max_channels; i++)
    {
        if (seq.GetChannel(i).status)
        {
            channels.push_back(seq.GetChannel(i));
        }
    }

    return channels;
}

// Camelot's mixer adds each channel's points to sums of 16 bits a side, and its output stage writes two points for
// each sum. Each frame's sums start with the echo of the points before. A fixed-pitch sample plays at the channel's
// volumes, point for point, through its loop. A sum past 15 bits saturates. Other samples play at half the volumes,
// rounded up, interpolated between their points. A sample that ends in a frame stops its channel there. A synth voice
// plays a pulse or triangle wave at the full volumes, and a saw wave at half of them, rounded down.
void TestCamelotMixing()
{
    Cart cart(kCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    std::vector<int8_t> ramp(300);
    for (size_t k = 0; k < ramp.size(); k++)
    {
        ramp[k] = int8_t(4 + k % 37);
    }
    const uint32_t looped = cart.Wave(ramp, 31536, 100);
    const uint32_t three = cart.Wave(std::vector<int8_t>(64, 3), 31536, 0);
    const uint32_t high = cart.Wave(std::vector<int8_t>(64, 127), 31536, 0);
    const uint32_t low = cart.Wave(std::vector<int8_t>(64, -128), 31536, 0);
    const std::vector<int8_t> negative(4000, -10);
    const std::array<uint32_t, 3> rates = {31536, 47304, 70000};
    const std::array<uint32_t, 3> interpolated = {cart.Wave(negative, rates[0], 0), cart.Wave(negative, rates[1], 0),
                                                  cart.Wave(negative, rates[2], 0)};
    // A sample of 200 points of 20 without a loop. The byte after its end holds 20 too.
    const uint32_t ending = cart.Wave(std::vector<int8_t>(201, 20), 31536);
    cart.Put32(ending + 12, 200);
    const std::vector<int8_t> jumpy = Jumpy(2001);
    const std::array<uint32_t, 3> jumping = {cart.Wave(jumpy, rates[0], 0), cart.Wave(jumpy, rates[1], 0),
                                             cart.Wave(jumpy, rates[2], 0)};
    // A sample of 50 points that loops from point 10. The tools write the loop's first point after a looped sample's
    // end.
    std::vector<int8_t> short_loop = Jumpy(50);
    short_loop.push_back(short_loop[10]);
    const uint32_t short_looped = cart.Wave(short_loop, rates[2], 10);
    cart.Put32(short_looped + 12, 50);
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x40, 0x90});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, looped)},
                                             {1, Cart::Voice(kFixed, three)},
                                             {2, Cart::Voice(kFixed, high)},
                                             {3, Cart::Voice(kFixed, low)},
                                             {4, Cart::Voice(0, interpolated[0])},
                                             {5, Cart::Voice(0, interpolated[1])},
                                             {6, Cart::Voice(0, interpolated[2])},
                                             {7, Cart::Voice(0, ending)},
                                             {8, Cart::Voice(0, pulse)},
                                             {9, Cart::Voice(0, saw)},
                                             {10, Cart::Voice(0, triangle)},
                                             {11, Cart::Voice(0, jumping[0])},
                                             {12, Cart::Voice(0, jumping[1])},
                                             {13, Cart::Voice(0, jumping[2])},
                                             {14, Cart::Voice(0, short_looped)}});
    const auto held = [](int voice, int pan = 0x40, int velocity = 127)
    {
        return Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, 127).Cmd(kCmdPan, pan).Tie(60, velocity).Wait(48).Fine();
    };
    cart.Song({held(0, 0x50)}, voices);
    cart.Song({held(1), held(1), held(1), held(1)}, voices);
    cart.Song({held(2), held(2)}, voices);
    cart.Song({held(3), held(3)}, voices);
    // At velocity 122, both sides' volumes are odd, and their halves round up.
    for (int voice = 4; voice <= 14; voice++)
    {
        cart.Song({held(voice, 0x40, 122)}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_CHECK(info.camelot_mixer && info.samples_per_frame == kCamelotPoints);

    // Mixes each of the first frames of a song, and works out what its channels add to the sums in each, from a
    // channel's volumes and its sample's point for each of the frame's points.
    using Added = std::array<std::vector<int32_t>, 2>;
    const auto mix_fixed = [&](int song, int frames, const auto& point)
    {
        Sequencer seq(rom, info, song);
        seq.StartMixing();
        MixedFrames mixed;
        std::vector<Added> added;
        std::vector<bool> many;
        for (int f = 0; f < frames; f++)
        {
            seq.Step();
            mixed.right.emplace_back(seq.Right().begin(), seq.Right().end());
            mixed.left.emplace_back(seq.Left().begin(), seq.Left().end());
            const std::vector<Channel> sounding = Sounding(seq, info);
            Added frame = {std::vector<int32_t>(kCamelotPoints), std::vector<int32_t>(kCamelotPoints)};
            for (const Channel& c : sounding)
            {
                const std::array<int32_t, 2> volumes = CamelotVolumes(c);
                for (size_t i = 0; i < size_t(kCamelotPoints); i++)
                {
                    const int32_t value = point(size_t(f) * size_t(kCamelotPoints) + i);
                    frame[0][i] += volumes[0] * value;
                    frame[1][i] += volumes[1] * value;
                }
            }
            added.push_back(frame);
            many.push_back(sounding.size() >= 4);
        }

        return std::pair{mixed, CamelotFrames(added, many)};
    };

    // The looped sample plays its 300 points, then the 200 from its loop's start again and again. The echo of the
    // first frames comes back in later ones. Four channels sum the second song's points, and only the first two sums
    // of each frame take the 64 that rounds them.
    const auto [looping, looping_frames] =
        mix_fixed(0, 12, [&](size_t n) { return ramp[n < 300 ? n : 100 + (n - 300) % 200]; });
    const auto [four, four_frames] = mix_fixed(1, 6, [](size_t) { return 3; });
    SEQLIB_CHECK(looping.right == looping_frames.right && looping.left == looping_frames.left);
    SEQLIB_CHECK(four.right == four_frames.right && four.left == four_frames.left);
    SEQLIB_CHECK(looping.right[0] != looping.left[0]);

    // Two channels at the top or the bottom of the range go past 15 bits on each side.
    for (int song = 2; song <= 3; song++)
    {
        Sequencer seq(rom, info, song);
        const MixedFrames m = Mix(seq, 1);
        const std::vector<Channel> sounding = Sounding(seq, info);
        const int8_t end = song == 2 ? 127 : -128;
        const int32_t sum = 2 * CamelotVolumes(sounding[0])[0] * 127;
        SEQLIB_CHECK(sounding.size() == 2 && sum > 16383 && sum < 32768);
        SEQLIB_CHECK(std::all_of(m.right[0].begin(), m.right[0].end(), [&](int8_t p) { return p == end; }) &&
                     m.left[0] == m.right[0]);
    }

    // The other samples play at half the volumes, rounded up, at the three steps' codes. Each point of a long sample
    // comes from CamelotConstantValues(). A step from 1 up interpolates to 8 bits at twice the halved volume. The left
    // side borrows from a negative right side.
    std::array<int32_t, 2> volumes = {};
    for (size_t r = 0; r < rates.size(); r++)
    {
        Sequencer seq(rom, info, 4 + int(r));
        const MixedFrames m = Mix(seq, 1);
        const Channel& c = seq.GetChannel(2);
        volumes = CamelotVolumes(c);
        const uint32_t step = c.frequency * info.step_scale;
        const uint32_t advance = uint32_t((uint64_t(step) * uint64_t(kCamelotPoints)) >> 23);
        const int32_t times = r == 0 ? 1 : 2;
        const std::array<int32_t, 2> scales = {times * ((volumes[0] + 1) / 2), times * ((volumes[1] + 1) / 2)};
        const std::vector<int32_t> values = CamelotConstantValues(-10, step);
        bool same = true;
        for (size_t i = 0; i < values.size(); i++)
        {
            const std::array<int8_t, 2> right = CamelotPoints(scales[0] * values[i], true);
            const std::array<int8_t, 2> left = CamelotPoints(scales[1] * values[i] - 1, true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }
        SEQLIB_CHECK(same);
        SEQLIB_CHECK(c.frequency == rates[r] && (step < 0x800000) == (r == 0) && (step < 0x1000000) == (r < 2));
        SEQLIB_CHECK(c.count == int32_t(4000 - advance) && c.position == interpolated[r] + 16 + advance);
        SEQLIB_CHECK(volumes[0] % 2 == 1 && volumes[1] % 2 == 1 &&
                     std::count(values.begin(), values.end(), r == 0 ? -21 : -11) > 0);
    }

    // A sample whose neighbouring points all differ plays the value between the point at its position and the next
    // point, by the position's fraction. Below a step of 1, the value is interpolated to 9 bits, and from 1 up to 8
    // bits at twice the volume. A looped sample with fewer points left than the frame takes plays point by point to 9
    // bits. At its end, it goes back to its loop's start, past it by the points that the step overshot.
    for (size_t r = 0; r <= rates.size(); r++)
    {
        Sequencer seq(rom, info, 11 + int(r));
        const MixedFrames m = Mix(seq, 1);
        const Channel& c = seq.GetChannel(2);
        const std::array<int32_t, 2> v = CamelotVolumes(c);
        const uint32_t step = c.frequency * info.step_scale;
        const bool nine_bits = r == 0 || r == rates.size();
        const int32_t times = nine_bits ? 1 : 2;
        const std::vector<int8_t>& pcm = r < rates.size() ? jumpy : short_loop;
        bool same = true;
        for (size_t i = 0; i < size_t(kCamelotPoints); i++)
        {
            const uint64_t position = uint64_t(i) * step;
            size_t k = size_t(position >> 23);
            if (r == rates.size() && k >= 50)
            {
                k = 10 + (k - 50) % 40;
            }
            const int32_t value = CamelotValue(pcm, k, uint32_t(position & 0x7FFFFF), nine_bits);
            const std::array<int32_t, 2> sums = {times * ((v[0] + 1) / 2) * value, times * ((v[1] + 1) / 2) * value};
            const std::array<int8_t, 2> right = CamelotPoints(sums[0], true);
            const std::array<int8_t, 2> left = CamelotPoints(sums[1] - (sums[0] < 0 ? 1 : 0), true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }
        SEQLIB_CHECK(same && c.status);
    }

    // A sample with fewer points left than the frame takes plays point by point, and stops its channel at its end: its
    // 200 points, and the point where the fraction first reaches 1.
    {
        Sequencer seq(rom, info, 7);
        const MixedFrames m = Mix(seq, 1);
        bool same = true;
        for (size_t i = 0; i < size_t(kCamelotPoints); i++)
        {
            const std::array<int8_t, 2> right = CamelotPoints(i <= 200 ? (volumes[0] + 1) / 2 * 40 : 0, true);
            const std::array<int8_t, 2> left = CamelotPoints(i <= 200 ? (volumes[1] + 1) / 2 * 40 : 0, true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }
        SEQLIB_CHECK(same && !seq.GetChannel(2).status);
    }

    // The synth voices: the pulse wave as the SoundFont's sample plays it, the triangle wave rising by a 256th of its
    // range for each 2^23 of the phase and falling after half the cycle, and the saw wave through its filter.
    for (int synth = 0; synth < 3; synth++)
    {
        Sequencer seq(rom, info, 8 + synth);
        const MixedFrames m = Mix(seq, 1);
        const Channel& c = seq.GetChannel(2);
        const std::array<int32_t, 2> v = CamelotVolumes(c);
        const uint32_t step = SynthStep(c.frequency, info.step_scale);
        const std::vector<int16_t> rendered =
            synth == 2 ? std::vector<int16_t>{}
                       : RenderSynth(rom, info, synth == 0 ? Synth::kPulse : Synth::kSaw,
                                     (synth == 0 ? pulse : saw) + 16, c.frequency, uint32_t(kCamelotPoints));
        bool same = true;
        int32_t lowest = 0;
        for (size_t i = 0; i < size_t(kCamelotPoints); i++)
        {
            std::array<int32_t, 2> sums = {};
            if (synth == 0)
            {
                sums = {v[0] * (rendered[i] / 256), v[1] * (rendered[i] / 256)};
            }
            else if (synth == 1)
            {
                sums = {(v[0] >> 1) * (rendered[i] / 128), (v[1] >> 1) * (rendered[i] / 128)};
            }
            else
            {
                const uint32_t phase = uint32_t(i + 1) * step;
                const int32_t value = int32_t(phase) >= 0 ? int32_t(phase >> 23) - 128 : 384 - int32_t(phase >> 23);
                sums = {v[0] * value, v[1] * value};
            }
            lowest = std::min(lowest, sums[0]);
            const std::array<int8_t, 2> right = CamelotPoints(sums[0], true);
            const std::array<int8_t, 2> left = CamelotPoints(sums[1] - (sums[0] < 0 ? 1 : 0), true);
            same = same && m.right[0][2 * i] == right[0] && m.right[0][2 * i + 1] == right[1] &&
                   m.left[0][2 * i] == left[0] && m.left[0][2 * i + 1] == left[1];
        }
        SEQLIB_CHECK(same && c.synth && lowest < 0);
    }

    // The player's timer takes two points from the FIFOs for each of the mixer's: every 280,896 / 1,056 = 266 cycles.
    // As in TestPlayerFifo(), the first frame plays the silent part of the buffer and the FIFOs' lead, and each output
    // sample, 256 cycles after the last, plays the point of the last overflow at or before it.
    const uint32_t period = uint32_t(ntdlib::agb::kFrameCycles) / uint32_t(2 * kCamelotPoints);
    std::vector<int8_t> right(size_t(2 * kCamelotPoints + kFifoLead), 0);
    std::vector<int8_t> left = right;
    ArchivePlayer player(rom, info);
    player.Start(0);
    std::vector<int16_t> out;
    for (size_t f = 0; f < looping.right.size(); f++)
    {
        right.insert(right.end(), looping.right[f].begin(), looping.right[f].end());
        left.insert(left.end(), looping.left[f].begin(), looping.left[f].end());
        player.RunFrame(out);
    }
    bool matches = true;
    for (size_t k = 0; k < out.size() / 2; k++)
    {
        const size_t pop = k * 256 / period;
        matches = matches && out[2 * k] == 256 * left[pop] && out[2 * k + 1] == 256 * right[pop];
    }
    SEQLIB_CHECK(period == 266 && matches && out.size() == 2 * looping.right.size() * ntdlib::agb::kFrameCycles / 256);
}

// A later version of Camelot's driver mixes the mode's points a frame, 12 channels whatever the mode says, and writes
// one point for each of the mixer's. The echo's levels and the left side's tap come from the output stage's literals
// (0x03000dd8 and 0x03000de0 in Mario Tennis: Power Tour).
void TestLaterCamelotMixing()
{
    constexpr int kPoints = 264;
    constexpr int kTap = 100;
    Cart cart(kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(kTap, 32, -6);
    const uint32_t constant = cart.Wave(std::vector<int8_t>(40 * kPoints, 50), 15768);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, constant)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Tie(60, 127).Wait(96).Eot(60).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_CHECK_EQ(info.samples_per_frame, kPoints);
    SEQLIB_CHECK_EQ(info.max_channels, 12);
    SEQLIB_CHECK_EQ(info.camelot_output_points, 1);
    SEQLIB_CHECK_EQ(info.camelot_echo, 32);
    SEQLIB_CHECK_EQ(info.camelot_cross_echo, -6);
    SEQLIB_CHECK_EQ(info.camelot_tap, kTap);
    SEQLIB_CHECK(info.camelot_adpcm && info.camelot_top_priority);

    // Each frame's sums start with 32/128 of each side's points from 7 frames before, less 6/128 of the other side's.
    // The right side takes a quarter of the last frame's right side, and the left side a quarter of the right side from
    // kTap points before. Each start takes 64 less, and the output stage's rounding gives it back: only the first
    // frame's sums end up 64 higher. The sums stay positive, so that no side carries into the other, and they fit in 15
    // bits. A point holds bits 7 to 14 of the sum.
    Sequencer seq(rom, info, 0);
    seq.StartMixing();
    std::array<std::vector<std::vector<int32_t>>, 2> output;
    const auto point = [&](size_t side, int frame, int i)
    {
        return frame >= 0 ? output[side][size_t(frame)][size_t(i)] : 0;
    };
    bool matches = true;
    bool in_range = true;
    for (int f = 0; f < 30; f++)
    {
        seq.Step();
        const std::vector<Channel> sounding = Sounding(seq, info);
        SEQLIB_REQUIRE(sounding.size() == 1);
        const std::array<int32_t, 2> volumes = CamelotVolumes(sounding[0]);
        SEQLIB_REQUIRE(int(seq.Right().size()) == kPoints && int(seq.Left().size()) == kPoints);
        output[0].emplace_back(kPoints);
        output[1].emplace_back(kPoints);
        for (int i = 0; i < kPoints; i++)
        {
            const int32_t tap = i >= kTap ? point(0, f - 1, i - kTap) : point(0, f - 2, kPoints - kTap + i);
            const int32_t right = 32 * point(0, f - 7, i) - 6 * point(1, f - 7, i) + 32 * point(0, f - 1, i);
            const int32_t left = 32 * point(1, f - 7, i) - 6 * point(0, f - 7, i) + 32 * tap;
            const int32_t round = f == 0 ? 64 : 0;
            const std::array<int32_t, 2> sums = {right + 50 * volumes[0] + round, left + 50 * volumes[1] + round};
            in_range = in_range && sums[0] >= 64 && sums[0] < 16320 && sums[1] >= 64 && sums[1] < 16320;
            output[0][size_t(f)][size_t(i)] = sums[0] >> 7;
            output[1][size_t(f)][size_t(i)] = sums[1] >> 7;
            matches = matches && seq.Right()[size_t(i)] == sums[0] >> 7 && seq.Left()[size_t(i)] == sums[1] >> 7;
        }
    }
    SEQLIB_CHECK(in_range);
    SEQLIB_CHECK(matches);

    // A MIDI file's reverb takes the echo's level.
    ConvertSettings opt = TempSettings("later_camelot");
    const SongReport report = ConvertSong(rom, info, 0, opt, nullptr);
    SEQLIB_CHECK(report.ok);
    const MidiEvents midi = ReadMidi(report.midi_path);
    int reverbs = 0;
    for (const auto& e : midi.Find(uint8_t(0xB0)))
    {
        if (e.bytes[1] == cc::kReverb)
        {
            reverbs++;
            SEQLIB_CHECK_EQ(e.bytes[2], 32);
        }
    }
    SEQLIB_CHECK_EQ(reverbs, 1);
}

// A later version of Camelot's mixer plays a fixed-pitch sample of a negative size in 4-bit steps, a byte for each two
// points, the high nibble first (0x03000c1c in Mario Tennis: Power Tour). The index starts at 56, and a step's value
// shifts right by half the index. The index then moves by 4 less the step's size. The channel's fraction keeps the
// index and the point between frames, and its type marks it. Such a sample has no loop.
void TestLaterCamelotSteps()
{
    constexpr int kPoints = 264;
    Cart cart(kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(88, 32, -6);

    // +4 and -4 keep the index at 56 and a value of 1 at a shift of 28: the points go 4, 8, 4, 8.
    std::vector<uint8_t> even = {0x44};
    even.resize(kPoints, 0xC4);

    // By hand: +4 to 4, +4 to 8, +4 to 12, -8 to 4 with an index of 52. At that index, a shift of 26 makes the next
    // step of 1 worth 4, to 8, with an index of 55. A step of 0 then changes nothing.
    const std::vector<uint8_t> by_hand = {0x44, 0x48, 0x10};

    // After a point of 8, steps of 0 hold it there.
    std::vector<uint8_t> level = {0x44};
    level.resize(10 * kPoints, 0x00);

    for (const auto& [steps, points] :
         {std::pair{even, 2 * kPoints}, std::pair{by_hand, 6}, std::pair{level, 20 * kPoints}})
    {
        const uint32_t wave = cart.StepWave(steps, uint32_t(points));
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, wave)}});
        cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Tie(60, 127).Wait(96).Eot(60).Fine()},
                  voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer even_song(rom, info, 0);
    even_song.StartMixing();
    even_song.Step();
    const std::vector<Channel> sounding = Sounding(even_song, info);
    SEQLIB_REQUIRE(sounding.size() == 1);
    const std::array<int32_t, 2> volumes = CamelotVolumes(sounding[0]);
    bool even_points = true;
    for (int i = 0; i < kPoints; i++)
    {
        const int32_t value = i % 2 == 0 ? 4 : 8;
        even_points = even_points && even_song.Right()[size_t(i)] == (volumes[0] * value + 64) >> 7 &&
                      even_song.Left()[size_t(i)] == (volumes[1] * value + 64) >> 7;
    }
    SEQLIB_CHECK(even_points);
    SEQLIB_CHECK_EQ(sounding[0].count, kPoints);
    SEQLIB_CHECK_EQ(sounding[0].fraction, 56u | 8u << 16);
    SEQLIB_CHECK(sounding[0].type & kVoiceCompressed);
    even_song.Step();
    SEQLIB_CHECK(Sounding(even_song, info).empty());

    Sequencer hand_song(rom, info, 1);
    hand_song.StartMixing();
    hand_song.Step();
    const std::array<int32_t, 6> kPointsByHand = {4, 8, 12, 4, 8, 8};
    bool hand_points = true;
    for (size_t i = 0; i < kPointsByHand.size(); i++)
    {
        hand_points = hand_points && hand_song.Right()[i] == (volumes[0] * kPointsByHand[i] + 64) >> 7;
    }
    SEQLIB_CHECK(hand_points);
    SEQLIB_CHECK_EQ(hand_song.Right()[6], 0);
    SEQLIB_CHECK(Sounding(hand_song, info).empty());

    // The echo dies away, and the mixer falls silent.
    SEQLIB_CHECK(!hand_song.MixerSilent());
    for (int f = 0; f < 100; f++)
    {
        hand_song.Step();
    }
    SEQLIB_CHECK(hand_song.MixerSilent());

    // A SoundFont holds the decoded points, at the mixer's rate.
    ConvertSettings opt = TempSettings("later_camelot_steps");
    const SongReport report = ConvertSong(rom, info, 1, opt, nullptr);
    SEQLIB_CHECK(report.ok && !report.silent);
    const Sf2Records sf = ReadSf2(report.sf2_path);
    const std::vector<uint8_t>& pcm = sf.chunks.at("smpl");
    const uint8_t* header = sf.Record("shdr", 46, 0);
    const uint32_t start = ntdlib::testing::agb_files::Le32(header + 20);
    SEQLIB_CHECK_EQ(ntdlib::testing::agb_files::Le32(header + 24) - start, 6);
    SEQLIB_CHECK_EQ(ntdlib::testing::agb_files::Le32(header + 36), 15768);
    bool sample_points = true;
    for (uint32_t i = 0; i < 6; i++)
    {
        sample_points = sample_points && int16_t(Le16(&pcm[2 * (start + i)])) == 256 * kPointsByHand[i];
    }
    SEQLIB_CHECK(sample_points);

    // The enhanced mode plays the decoded points: a point of 8 at the note's volume, and not the bytes of the steps.
    Sequencer enhanced(rom, info, 2);
    enhanced.StartMixing();
    enhanced.StartEnhancedMixing();
    const auto volume = [&](int side_volume)
    {
        return 8.0f * float(side_volume) * float(sounding[0].envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
    };
    const float point_right = volume(sounding[0].right);
    const float point_left = volume(sounding[0].left);

    // The echo keeps the console's levels: 32/128 of each side from 7 frames before, less 6/128 of the other side's.
    // The right side takes a quarter of itself from a frame before, and the left side a quarter of the right side from
    // a frame and a third before. The sample holds its point, and the level stays flat across each third of a frame.
    using Thirds = std::array<float, 3>;
    const auto at = [](const std::vector<Thirds>& frames, int f, size_t third)
    {
        return f >= 0 ? frames[size_t(f)][third] : 0.0f;
    };
    std::vector<Thirds> rights;
    std::vector<Thirds> lefts;
    bool echoes = true;
    for (int f = 0; f < 15; f++)
    {
        enhanced.Step();
        const size_t samples = enhanced.EnhancedRight().size();
        Thirds right = {};
        Thirds left = {};
        for (size_t third = 0; third < 3; third++)
        {
            const float old_right = at(rights, f - 7, third);
            const float old_left = at(lefts, f - 7, third);
            const float last = at(rights, f - 1, third);
            const float tap = third == 0 ? at(rights, f - 2, 2) : at(rights, f - 1, third - 1);
            right[third] = point_right + (32.0f * old_right - 6.0f * old_left) / 128.0f + last / 4.0f;
            left[third] = point_left + (32.0f * old_left - 6.0f * old_right) / 128.0f + tap / 4.0f;
            const size_t middle = (2 * third + 1) * samples / 6;
            echoes = echoes && std::fabs(enhanced.EnhancedRight()[middle] - right[third]) < right[third] / 2000.0f &&
                     std::fabs(enhanced.EnhancedLeft()[middle] - left[third]) < left[third] / 2000.0f;
        }
        rights.push_back(right);
        lefts.push_back(left);
    }
    SEQLIB_CHECK(echoes);
}

// A later version of Camelot's sequencer gives the notes of a player whose priority is 120 a priority of 255, and
// others 254 at most. The search for a channel leaves out a released note of priority 255 (0x0824ecd0 and 0x0824ed3e in
// Mario Tennis: Power Tour).
void TestLaterCamelotPriority()
{
    Cart cart(kLaterCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    cart.WriteLaterCamelot(88, 32, -6);
    const uint32_t wave = cart.Wave(std::vector<int8_t>(4000, 20), 15768, 0);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, wave, 60, {255, 0, 255, 250})}});
    const auto notes = [&](uint8_t priority, uint8_t track_priority)
    {
        // Twelve notes take channels 2 to 11, then 1 and 0: the fourth lies on channel 5. It releases, and one more
        // note starts.
        Track track;
        track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPrio, track_priority);
        for (int key = 60; key < 72; key++)
        {
            track.Tie(uint8_t(key), 127);
        }
        track.Eot(63).Tie(72, 127).Wait(96).Fine();
        cart.Song({track}, voices, 0, priority);
    };
    notes(120, 0);
    notes(100, 200);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // Every note has a priority of 255. The released note keeps its channel, and the last note takes the first in the
    // list of channels.
    Sequencer top(rom, info, 0);
    top.Step();
    SEQLIB_CHECK_EQ(top.GetChannel(2).priority, 255);
    SEQLIB_CHECK_EQ(top.GetChannel(5).midi_key, 63);
    SEQLIB_CHECK_EQ(top.GetChannel(0).midi_key, 72);

    // Every note has a priority of 254. The last note takes the released note's channel.
    Sequencer other(rom, info, 1);
    other.Step();
    SEQLIB_CHECK_EQ(other.GetChannel(2).priority, 254);
    SEQLIB_CHECK_EQ(other.GetChannel(5).midi_key, 72);
    SEQLIB_CHECK_EQ(other.GetChannel(0).midi_key, 71);
}

// A pulse or saw synth voice's SoundFont instrument has a zone for each key that the song plays it at, with a sample
// made for that key at the mixer's rate. A triangle wave's has one cycle for every key. The MIDI file's reverb is the
// echo of Camelot's mixer, whatever the song sets, and a PSG voice is quieter against the samples.
void TestCamelotConversion()
{
    Cart cart(kMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t pulse = cart.Synth(0, {0x80, 0x00, 0x00, 0x00});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup(
        {{0, Cart::Voice(0, pulse)}, {1, Cart::Voice(0, triangle)}, {2, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Note(12, 60, 127)
                   .Wait(12)
                   .Note(12, 64, 127)
                   .Wait(12)
                   .Note(12, 60, 127)
                   .Wait(12)
                   .Fine(),
               Track().Cmd(kCmdVoice, 1).Cmd(kCmdVol, 127).Note(12, 48, 127).Wait(12).Fine(),
               Track().Cmd(kCmdVoice, 2).Cmd(kCmdVol, 127).Note(12, 72, 127).Wait(12).Fine()},
              voices, 0, 0, 0x80 | 10);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("camelot");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);
    const std::vector<int16_t> points = RenderSynth(rom, info, Synth::kPulse, pulse + 16, 13379, 64);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    int reverbs = 0;
    for (uint8_t ch = 0; ch < 3; ch++)
    {
        for (const auto& e : m.Find(uint8_t(0xB0 + ch)))
        {
            if (e.bytes[1] == cc::kReverb)
            {
                reverbs++;
                SEQLIB_CHECK_EQ(e.bytes[2], 53);
            }
        }
    }
    SEQLIB_CHECK_EQ(reverbs, 3);

    // The SoundFont's instrument for each program.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<int, int> instrument;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        instrument[Le16(h + 20)] = Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
    }

    // Each zone's key range, its sample's name, rate and root key, and its attenuation.
    auto zones = [&](int program)
    {
        const size_t first = Le16(sf.Record("inst", 22, size_t(instrument[program])) + 20);
        const size_t last = Le16(sf.Record("inst", 22, size_t(instrument[program]) + 1) + 20);
        std::vector<std::map<uint16_t, uint16_t>> out;
        for (size_t z = first + 1; z < last; z++)
        {
            out.push_back(sf.ZoneGens(z));
        }

        return out;
    };

    auto sample_name = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        const uint8_t* h = sf.Record("shdr", 46, zone.at(sf2::kSampleId));
        return std::string(reinterpret_cast<const char*>(h), std::find(h, h + 20, 0) - h);
    };

    auto sample_rate = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        return ntdlib::testing::agb_files::Le32(sf.Record("shdr", 46, zone.at(sf2::kSampleId)) + 36);
    };

    auto root_key = [&](const std::map<uint16_t, uint16_t>& zone)
    {
        return int(sf.Record("shdr", 46, zone.at(sf2::kSampleId))[40]);
    };

    // The names of the pulse wave's samples for keys 60 and 64, and of the triangle wave's.
    char pulse_60[32], pulse_64[32], triangle_name[32];
    std::snprintf(pulse_60, sizeof pulse_60, "Pulse %08X 60", unsigned(pulse));
    std::snprintf(pulse_64, sizeof pulse_64, "Pulse %08X 64", unsigned(pulse));
    std::snprintf(triangle_name, sizeof triangle_name, "Triangle %08X", unsigned(triangle));

    const auto pulse_zones = zones(0);
    SEQLIB_REQUIRE_EQ(pulse_zones.size(), 2);
    SEQLIB_CHECK(pulse_zones[0].at(sf2::kKeyRange) == 0x3C3C && sample_name(pulse_zones[0]) == pulse_60);
    SEQLIB_CHECK(pulse_zones[1].at(sf2::kKeyRange) == 0x4040 && sample_name(pulse_zones[1]) == pulse_64);
    SEQLIB_CHECK(sample_rate(pulse_zones[1]) == 13379 && root_key(pulse_zones[1]) == 64);
    SEQLIB_CHECK(!pulse_zones[0].count(sf2::kInitialAttenuation) && pulse_zones[0].count(sf2::kSampleModes));
    const auto triangle_zones = zones(1);
    SEQLIB_CHECK(triangle_zones.size() == 1 && triangle_zones[0].at(sf2::kKeyRange) == 0x7F00 &&
                 sample_name(triangle_zones[0]) == triangle_name && sample_rate(triangle_zones[0]) == 13379);
    // Camelot's mixer plays a sample at 9/8 of the SDK mixer's level, so the square's sample is that much quieter than
    // the SDK's, 6.3 dB and 1 dB more below a full-scale sample.
    const auto square_zones = zones(2);
    SEQLIB_CHECK(!square_zones.empty() && !square_zones[0].count(sf2::kInitialAttenuation));
    if (!square_zones.empty())
    {
        const double decibels = 20 * std::log10(sf.SamplePeak(square_zones[0].at(sf2::kSampleId)) / 32767.0);
        SEQLIB_CHECK(std::fabs(decibels + 6.3 + 20 * std::log10(9.0 / 8.0)) < 0.01);
    }

    // A pulse wave with a duty of 128/256 at the mixer's rate: 32 points high and 32 low, at half the full level.
    SEQLIB_CHECK(points.size() == 64 && points[0] == 16384 && points[31] == 16384 && points[32] == -16384 &&
                 points[63] == -16384);
}

// Runs every test. Returns the exit code.
// The player's tests. The test cartridges' driver mixes 224 points a frame at 13,379 Hz, and gives the sound bias
// register a resolution of 8 bits: an output of 65,536 samples a second, 256 cycles apart.
constexpr uint32_t kFrameCycles = uint32_t(ntdlib::agb::kFrameCycles);
constexpr uint32_t kOutputCycles = 256;

// Runs `frames` frames, and returns each frame's samples.
std::vector<std::vector<int16_t>> PlayFrames(ArchivePlayer& player, int frames)
{
    std::vector<std::vector<int16_t>> out(static_cast<size_t>(frames));
    for (std::vector<int16_t>& frame : out)
    {
        player.RunFrame(frame);
    }

    return out;
}

// Runs `frames` frames, and returns their samples.
std::vector<int16_t> Play(ArchivePlayer& player, int frames)
{
    std::vector<int16_t> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

// Returns the largest magnitude of the samples of side `side` (0 for the left).
int32_t Peak(const std::vector<int16_t>& samples, size_t side)
{
    int32_t peak = 0;
    for (size_t i = side; i < samples.size(); i += 2)
    {
        peak = std::max(peak, std::abs(int32_t(samples[i])));
    }

    return peak;
}

// Returns whether `f` throws an exception of type E.
template <typename E, typename F>
bool Throws(F f)
{
    try
    {
        f();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

// The Metroid games' sequencer is found by its TEMPO handler. Metroid: Zero Mission has no routine that starts a song:
// its tables come from two loads of literals in a row. The table of PSG frequency settings comes from the routine that
// reads it.
void TestMetroidDetection()
{
    Cart cart(kMode, {10, 3}, false, false);
    const uint32_t psg_table = cart.WriteMetroid(16);
    cart.Song({Track().Fine()}, cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw()))}}));
    Cart plain(kMode, {10, 3}, false, false);
    plain.Song({Track().Fine()}, plain.VoiceGroup({{0, Cart::Voice(0, plain.Wave(Saw()))}}));

    const DriverInfo info = Detect(cart.ToRom());
    DriverInfo plain_info;
    std::string error;
    const bool plain_found = DetectDriver(plain.ToRom(), DriverOverrides(), plain_info, error);

    SEQLIB_CHECK(info.metroid_sequencer);
    SEQLIB_CHECK_EQ(info.song_start, 0u);
    SEQLIB_CHECK_EQ(info.player_table, kPlayerTable);
    SEQLIB_CHECK_EQ(info.song_table, kSongTable);
    SEQLIB_CHECK_EQ(info.song_count, 1);
    SEQLIB_CHECK_EQ(info.metroid_psg_table, psg_table);
    SEQLIB_CHECK(!plain_found && !plain_info.metroid_sequencer && !plain_info.metroid_psg_table);
}

// The Metroid games' sequencer keeps each note of a drum kit at the pan that it started with when the track's volume
// changes. Its mixer ends a note whose level reaches 0, unless the track gave both an echo volume and an echo length.
// The echo then plays at the note's last level for the length less 1. A square takes its setting from the game's table.
// In Metroid Fusion, a note reads a key and a velocity, and a third byte below 0x80 is the next note's key. In Metroid:
// Zero Mission, the third byte adds to the note's length.
void TestMetroidSequencer()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t wave = cart.Wave(Saw(), 13379, 0);
    const uint32_t kit = cart.VoiceGroup({{60, Cart::Voice(0, wave, 60, {255, 0, 255, 0}, 0xA0)},
                                          {61, Cart::Voice(0, wave, 60, {255, 0, 255, 0}, 0xE0)}});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceDrumKit, kit)},
                                             {1, Cart::Voice(0, wave, 60, {255, 0, 255, 0})},
                                             {2, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 127)
                   .Tie(60, 127)
                   .Tie(61, 127)
                   .Wait(1)
                   .Cmd(kCmdVol, 64)
                   .Wait(2)
                   .Fine()},
              voices);
    for (const int length : {0, 5})
    {
        cart.Song({Track()
                       .Cmd(kCmdTempo, 75)
                       .Cmd(kCmdVoice, 1)
                       .Cmd(kCmdVol, 127)
                       .Raw({kCmdXcmd, kXcmdEchoVol, 70, kCmdXcmd, kXcmdEchoLen, uint8_t(length)})
                       .Note(2, 60, 127)
                       .Wait(20)
                       .Fine()},
                  voices);
    }
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 2).Cmd(kCmdTune, 0x48).Tie(60, 127).Wait(2).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Note(24, 60, 100, 50).Wait(24).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // After the volume falls, the note of key 60 stays on the left and that of key 61 on the right.
    Sequencer drums(rom, info, 0);
    drums.Step();
    drums.Step();
    std::vector<std::pair<int, int>> sides;
    for (int c = 0; c < 2; c++)
    {
        const Channel& ch = drums.GetChannel(c);
        sides.push_back({ch.midi_key, ch.left - ch.right});
    }
    std::sort(sides.begin(), sides.end());
    SEQLIB_CHECK(sides.size() == 2 && sides[0].first == 60 && sides[0].second > 0 && sides[1].first == 61 &&
                 sides[1].second < 0);

    // The note's release takes its level to 0 in a frame. Without an echo length it ends there. With 5, it stays on for
    // 4 frames more at the level it had before.
    std::vector<int> frames;
    std::vector<int> levels;
    for (int song = 1; song <= 2; song++)
    {
        Sequencer seq(rom, info, song);
        int on = 0;
        for (int f = 0; f < 12; f++)
        {
            seq.Step();
            const Channel& ch = seq.GetChannel(0);
            if (ch.status & ntdlib::mp2k::kStatusOn)
            {
                on++;
                if (ch.status & ntdlib::mp2k::kStatusEcho)
                {
                    levels.push_back(ch.envelope);
                }
            }
        }
        frames.push_back(on);
    }
    SEQLIB_CHECK(frames.size() == 2 && frames[1] == frames[0] + 5);
    SEQLIB_CHECK(levels.size() == 5 && std::all_of(levels.begin(), levels.end(), [](int l) { return l == 255; }));

    // A tune of 8/64 of a semitone takes 129/256 of the step from key 60's setting to key 61's.
    Sequencer square(rom, info, 3);
    square.Step();
    SEQLIB_CHECK_EQ(square.GetChannel(kFirstPsgChannel).frequency, 60u * 16 + 8);

    Sequencer notes(rom, info, 4);
    std::vector<int> keys;
    for (const Action& a : Actions(notes, 1, Action::kNoteOn))
    {
        keys.push_back(a.key);
    }
    SEQLIB_CHECK(!info.metroid_short_notes && keys == std::vector<int>({60}));

    Cart fusion;
    fusion.WriteMetroid(16, true);
    fusion.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100, 50).Wait(24).Fine()},
                fusion.VoiceGroup({{0, Cart::Voice(0, fusion.Wave(Saw(), 13379, 0))}}));
    const Rom fusion_rom = fusion.ToRom();
    const DriverInfo fusion_info = Detect(fusion_rom);
    Sequencer short_notes(fusion_rom, fusion_info, 0);
    keys.clear();
    for (const Action& a : Actions(short_notes, 1, Action::kNoteOn))
    {
        keys.push_back(a.key);
    }
    SEQLIB_CHECK(fusion_info.metroid_short_notes && keys == std::vector<int>({60, 50}));

    // The scan of a track reads the same two notes, each of 24 ticks, where a wait of 96 ticks follows them. The SDK's
    // library would read one note of 74 ticks.
    Cart longer;
    longer.WriteMetroid(16, true);
    longer.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100, 50).Wait(96).Fine()},
                longer.VoiceGroup({{0, Cart::Voice(0, longer.Wave(Saw(), 13379, 0))}}));
    const Rom longer_rom = longer.ToRom();
    const DriverInfo longer_info = Detect(longer_rom);
    ntdlib::mp2k::SongHeader longer_header;
    SEQLIB_REQUIRE(ReadSong(longer_rom, longer_info, 0, longer_header));
    const uint32_t track = longer_header.tracks[0];
    SEQLIB_CHECK_EQ(ScanTrack(longer_rom, track, NoteArguments(longer_info)).last_command, 24u);
    SEQLIB_CHECK_EQ(ScanTrack(longer_rom, track).last_command, 74u);
}

// The Metroid games' PSG routine counts a square's envelope a level every `attack`, `decay` or `release` frames, with
// no extra step every 15th frame, and writes NR12 (0x63) only when a phase starts, each time with a restart in NR14
// (0x65). The attack restarts the channel at 0, rising every 6/64 s, while the count starts at 1. When the count
// reaches the full level, the decay restarts the channel a level below it, falling. The sustain restarts it at the
// sustain level with a step time of 0. The release restarts it a level below the count, falling every 2/64 s, and the
// note ends when the count reaches 0: NR12 = 8, then NR14 = 0x80. A change of volume restarts the channel at the count
// with a step time of 0. Metroid Fusion and Metroid: Zero Mission play these stores as mGBA 0.11.0 records them.
void TestMetroidPsgEnvelope()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {6, 6, 4, 2})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 64).Note(96, 60, 127).Wait(96).Fine()},
              voices);
    cart.Song({Track()
                   .Cmd(kCmdTempo, 75)
                   .Cmd(kCmdVoice, 0)
                   .Cmd(kCmdVol, 64)
                   .Note(96, 60, 127)
                   .Wait(10)
                   .Cmd(kCmdVol, 32)
                   .Wait(86)
                   .Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // The frames that store to NR12, with the value and whether NR14 restarts the channel in the same frame.
    struct Envelope
    {
        int frame;
        uint32_t nr12;
        bool restart;
        Channel channel;
    };
    const auto envelopes = [&](int song)
    {
        Sequencer seq(rom, info, song);
        std::vector<Envelope> out;
        for (int f = 0; f < 140; f++)
        {
            seq.Step();
            std::optional<uint32_t> nr12;
            bool restart = false;
            for (const ntdlib::agb::RegisterWrite& w : seq.SoundWrites())
            {
                nr12 = (w.address & 0xFF) == 0x63 ? std::optional<uint32_t>(w.value) : nr12;
                restart = restart || ((w.address & 0xFF) == 0x65 && (w.value & 0x80));
            }
            if (nr12)
            {
                out.push_back({f, *nr12, restart, seq.GetChannel(kFirstPsgChannel)});
            }
        }
        return out;
    };

    const std::vector<Envelope> plain = envelopes(0);
    SEQLIB_REQUIRE(plain.size() >= 4);
    const int goal = plain[0].channel.goal;
    const int sustain = plain[0].channel.sustain_goal;
    SEQLIB_REQUIRE(goal > sustain + 2 && sustain > 1);
    SEQLIB_CHECK(plain[0].nr12 == 0x0E && plain[0].restart && plain[0].channel.envelope == 1);
    SEQLIB_CHECK(plain[1].frame == plain[0].frame + 6 * (goal - 1) && plain[1].restart);
    SEQLIB_CHECK_EQ(plain[1].nr12, uint32_t(((goal - 1) << 4) | 6));
    SEQLIB_CHECK(plain[2].frame == plain[1].frame + 6 * (goal - 1 - sustain) && plain[2].restart);
    SEQLIB_CHECK_EQ(plain[2].nr12, uint32_t(sustain << 4));
    SEQLIB_CHECK_EQ(plain[3].nr12, uint32_t(((sustain - 1) << 4) | 2));
    SEQLIB_CHECK(plain[3].restart && plain[3].frame == plain[0].frame + 96);
    SEQLIB_REQUIRE_EQ(plain.size(), 5);
    SEQLIB_CHECK(plain[4].nr12 == 0x08 && plain[4].frame == plain[3].frame + 2 * (sustain - 1));
    SEQLIB_CHECK_EQ(plain[4].channel.status, 0);

    // The fall of volume in the attack holds the channel at the count, 2 after 10 frames. The decay then starts from
    // the new full level.
    const std::vector<Envelope> quieter = envelopes(1);
    SEQLIB_REQUIRE(quieter.size() >= 2);
    SEQLIB_CHECK(quieter[1].frame == quieter[0].frame + 10 && quieter[1].nr12 == 0x20 && quieter[1].restart);
    const int new_goal = quieter[1].channel.goal;
    SEQLIB_CHECK(new_goal < goal);
    SEQLIB_REQUIRE(quieter.size() >= 3);
    SEQLIB_CHECK_EQ(quieter[2].nr12, uint32_t(((new_goal - 1) << 4) | 6));
}

// The Metroid games' LFO leaves its swing as it is when the triangle wave's value, before the depth scales it, equals
// the old swing: at a speed of 32 and a depth of 63, the swing goes 16, 32, 32 and 0 rather than 16, 32, 16 and 0.
void TestMetroidLfo()
{
    Cart cart;
    cart.WriteMetroid(16);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0), 60, {255, 0, 255, 0})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdLfoS, 63).Cmd(kCmdMod, 63).Tie(60, 127).Wait(6).Fine()},
        voices);
    const Rom rom = cart.ToRom();
    Sequencer seq(rom, Detect(rom), 0);

    // The swing moves the pitch by 48/256 of a semitone a step. The note at tick 0 sets the swing to 0.
    std::vector<std::pair<uint64_t, int>> pitches;
    for (const Action& a : Actions(seq, 8, Action::kPitch))
    {
        if (a.tick >= 1 && a.tick <= 4)
        {
            pitches.push_back({a.tick, a.value});
        }
    }
    const std::vector<std::pair<uint64_t, int>> kPitches = {{1, 768}, {2, 1536}, {4, 0}};
    SEQLIB_CHECK(pitches == kPitches);
}

// A song of two tracks: a saw that loops on a DirectSound channel, and a square. A rest takes the place of either.
struct TwoTracks
{
    Rom rom;
    DriverInfo info;
};

TwoTracks TwoTrackSongs(uint8_t reverb = 0)
{
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const Track saw = Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(48, 60, 127).Wait(48).Fine();
    const Track square =
        Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(6).Note(48, 64, 127).Wait(48).Fine();
    const Track rest = Track().Cmd(kCmdTempo, 75).Wait(96).Fine();
    cart.Song({saw, square}, voices, 0, 0, reverb);
    cart.Song({rest, square}, voices, 0, 0, reverb);
    cart.Song({saw, rest}, voices, 0, 0, reverb);
    TwoTracks songs{cart.ToRom(), {}};
    songs.info = Detect(songs.rom);

    return songs;
}

void TestPlayerFifo()
{
    // The mixer's points of each frame play in the next frame, after the kFifoLead points that the FIFOs hold ahead.
    // Timer 0 overflows every 280,896 / 224 = 1,254 cycles, from the start. Each output sample plays the point of the
    // last overflow at or before it. FIFO A plays the right side, and FIFO B the left. A point p adds 4p to the
    // output's 10 bits, at the FIFOs' volume of 100%: 256p in a 16-bit sample. The PSG stays silent.
    Cart cart;
    const std::vector<int8_t> pcm = Jumpy(300);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(pcm))}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Cmd(kCmdPan, 0x20).Note(24, 60, 127).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    constexpr int kFrames = 12;

    Sequencer seq(rom, info, 0);
    const MixedFrames m = Mix(seq, kFrames);
    SEQLIB_REQUIRE(FirstSound(m) >= 0);
    std::vector<int8_t> right(size_t(224 + kFifoLead), 0);
    std::vector<int8_t> left(size_t(224 + kFifoLead), 0);
    for (int f = 0; f < kFrames; f++)
    {
        right.insert(right.end(), m.right[size_t(f)].begin(), m.right[size_t(f)].end());
        left.insert(left.end(), m.left[size_t(f)].begin(), m.left[size_t(f)].end());
    }

    ArchivePlayer player(rom, info);
    player.Start(0);
    SEQLIB_REQUIRE_EQ(player.Rate(), 65536);
    const std::vector<int16_t> out = Play(player, kFrames);
    SEQLIB_REQUIRE_EQ(out.size(), size_t(2 * kFrames * kFrameCycles / kOutputCycles));
    bool matches = true;
    bool sounds = false;
    for (size_t k = 0; k < out.size() / 2; k++)
    {
        const size_t pop = k * kOutputCycles / (kFrameCycles / 224);
        matches &= out[2 * k] == 256 * left[pop] && out[2 * k + 1] == 256 * right[pop];
        sounds |= out[2 * k] != 0;
    }
    SEQLIB_CHECK(matches);
    SEQLIB_CHECK(sounds);
    SEQLIB_CHECK_EQ(player.Frames(), uint64_t(kFrames));
}

void TestPlayerStoreCycle()
{
    // The driver's writes take effect at the options' cycle of each frame. The output stays silent before the cycle
    // of the frame whose writes first give square 1 a level in NR12 (0x04000063). A square at key 60, about 262 Hz,
    // sounds within a cycle of its wave after it: 64,000 of the CPU's cycles.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(48, 60, 127).Wait(48).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    Sequencer seq(rom, info, 0);
    uint32_t level_frame = 0;
    bool level = false;
    while (!level && level_frame < 10)
    {
        seq.Step();
        for (const ntdlib::agb::RegisterWrite& w : seq.SoundWrites())
        {
            level |= w.address == 0x04000063 && (w.value >> 4) > 0;
        }
        level_frame += level ? 0 : 1;
    }
    SEQLIB_REQUIRE(level);
    for (uint32_t cycle : {0u, 100000u, 280000u})
    {
        ArchivePlayer player(rom, info);
        ArchivePlayerOptions options;
        options.store_cycle = cycle;
        player.Start(0, options);
        const std::vector<int16_t> out = Play(player, 3);
        size_t first = 0;
        while (first < out.size() && out[first] == 0)
        {
            first++;
        }
        SEQLIB_REQUIRE(first < out.size());
        const uint64_t at = first / 2 * kOutputCycles;
        const uint64_t stores = uint64_t(level_frame) * kFrameCycles + cycle;
        SEQLIB_CHECK(at >= stores && at < stores + 64000);
    }
}

void TestPlayerFramesWithoutSamples()
{
    // Frames without samples leave the player as frames with them do. The song's reverb keeps the mixer's buffer in
    // the state.
    const TwoTracks songs = TwoTrackSongs(0x80 | 60);
    ArchivePlayer reference(songs.rom, songs.info);
    reference.Start(0);
    const std::vector<std::vector<int16_t>> expected = PlayFrames(reference, 120);

    ArchivePlayer player(songs.rom, songs.info);
    player.MeterTracks(true);
    player.Start(0);
    PlayFrames(player, 30);
    bool silent_meters = true;
    for (int f = 0; f < 40; f++)
    {
        player.RunFrame();
        silent_meters &=
            player.TrackPeaks(0) == std::array<int32_t, 2>{} && player.TrackPeaks(1) == std::array<int32_t, 2>{};
    }
    SEQLIB_CHECK(silent_meters);
    const std::vector<std::vector<int16_t>> after = PlayFrames(player, 50);
    SEQLIB_CHECK(std::equal(after.begin(), after.end(), expected.begin() + 70));
    SEQLIB_CHECK_EQ(player.Frames(), 120u);
    SEQLIB_CHECK_EQ(player.Busy(), reference.Busy());
}

void TestPlayerSnapshots()
{
    // A restore gives the frames after the snapshot again. The mutes and the meters stay through it.
    const TwoTracks songs = TwoTrackSongs(0x80 | 60);
    ArchivePlayer player(songs.rom, songs.info);
    SEQLIB_CHECK(Throws<std::logic_error>([&] { player.Save(); }));
    player.Start(0);
    PlayFrames(player, 30);
    const ArchivePlayer::Snapshot snapshot = player.Save();
    const std::vector<int16_t> first = Play(player, 60);
    player.MuteTrack(1, true);
    player.MeterTracks(true);
    player.Restore(snapshot);
    SEQLIB_CHECK_EQ(player.Frames(), 30u);
    SEQLIB_CHECK(player.IsTrackMuted(1));
    player.MuteTrack(1, false);
    std::vector<int16_t> second;
    bool metered = false;
    for (int f = 0; f < 60; f++)
    {
        player.RunFrame(second);
        metered |= player.TrackPeaks(0)[0] > 0;
    }
    SEQLIB_CHECK(first == second);
    SEQLIB_CHECK(metered);

    ArchivePlayer other(songs.rom, songs.info);
    other.Start(0);
    SEQLIB_CHECK(Throws<std::invalid_argument>([&] { other.Restore(snapshot); }));
}

void TestPlayerMutes()
{
    // A muted track plays as a rest in its place would, whether its channel is a DirectSound channel or a square.
    const TwoTracks songs = TwoTrackSongs();
    const auto play = [&](int song, int muted)
    {
        ArchivePlayer player(songs.rom, songs.info);
        if (muted >= 0)
        {
            player.MuteTrack(muted, true);
        }
        player.Start(song);
        return Play(player, 120);
    };
    const std::vector<int16_t> both = play(0, -1);
    const std::vector<int16_t> square = play(1, -1);
    const std::vector<int16_t> saw = play(2, -1);
    SEQLIB_CHECK(both != square && both != saw && Peak(square, 0) > 0 && Peak(saw, 0) > 0);
    SEQLIB_CHECK(play(0, 0) == square);
    SEQLIB_CHECK(play(0, 1) == saw);

    ArchivePlayer player(songs.rom, songs.info);
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(16, true); }));
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.MuteTrack(-1, true); }));
}

void TestPlayerMeters()
{
    // A track's meter gives the largest part of the output that its channels give. With the other track muted, the
    // track's largest meter over the song is the output's largest sample. Each side's output then takes the track's
    // channels alone: a FIFO's point times 256, or a square's level times 2 times NR50's master volume plus 1, times
    // 64. Neither loses bits to the 8 bits of the output's resolution. A muted track's meter gives 0, and so does any
    // track's without meters.
    const TwoTracks songs = TwoTrackSongs();
    for (int track = 0; track < 2; track++)
    {
        ArchivePlayer player(songs.rom, songs.info);
        player.MuteTrack(1 - track, true);
        player.MeterTracks(true);
        player.Start(0);
        std::vector<int16_t> out;
        std::array<int32_t, 2> peaks = {};
        bool muted_silent = true;
        for (int f = 0; f < 120; f++)
        {
            player.RunFrame(out);
            for (size_t side = 0; side < 2; side++)
            {
                peaks[side] = std::max(peaks[side], player.TrackPeaks(track)[side]);
            }
            muted_silent &= player.TrackPeaks(1 - track) == std::array<int32_t, 2>{};
        }
        SEQLIB_CHECK(peaks[0] > 0 && peaks[0] == Peak(out, 0));
        SEQLIB_CHECK(peaks[1] > 0 && peaks[1] == Peak(out, 1));
        SEQLIB_CHECK(muted_silent);
    }

    ArchivePlayer player(songs.rom, songs.info);
    player.Start(0);
    bool silent = true;
    for (int f = 0; f < 60; f++)
    {
        std::vector<int16_t> out;
        player.RunFrame(out);
        silent &= player.TrackPeaks(0) == std::array<int32_t, 2>{} && player.TrackPeaks(1) == std::array<int32_t, 2>{};
    }
    SEQLIB_CHECK(silent);
}

void TestPlayerNotes()
{
    // on_note gives each note that starts, with its first byte: the note's command, or its key where it repeats the
    // command.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t first = track.Here();
    track.Note(24, 60, 100).Wait(24);
    const size_t second = track.Here();
    track.Raw({62}).Wait(24).Fine();
    const uint32_t header = cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const uint32_t start = rom.U32(header + 8);

    ArchivePlayer player(rom, info);
    std::vector<std::pair<uint32_t, int>> notes;
    player.on_note = [&](const Action& a)
    {
        notes.emplace_back(a.command, a.key);
    };
    player.Start(0);
    for (int f = 0; f < 80; f++)
    {
        player.RunFrame();
    }
    const std::vector<std::pair<uint32_t, int>> expected = {{start + uint32_t(first), 60},
                                                            {start + uint32_t(second), 62}};
    SEQLIB_CHECK(notes == expected);
}

void TestPlayerMemory()
{
    // A memacc command that jumps when byte 3 of the memory holds 1 reaches a note. The memory starts at 0. A write
    // before the first frame reaches the command.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}});
    Track track;
    track.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100);
    const size_t jump = track.Here();
    track.MemAccJump(6, 3, 1, jump + 9).Fine().Note(24, 60, 100).Wait(24).Fine();
    cart.Song({track}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    for (int value : {-1, 1, 2})
    {
        ArchivePlayer player(rom, info);
        int notes = 0;
        player.on_note = [&](const Action&)
        {
            notes++;
        };
        player.Start(0);
        if (value >= 0)
        {
            player.WriteMemory(3, uint8_t(value));
        }
        for (int f = 0; f < 30; f++)
        {
            player.RunFrame();
        }
        SEQLIB_CHECK_EQ(notes, value == 1 ? 1 : 0);
        SEQLIB_CHECK_EQ(int(player.Driver().Memory(3)), std::max(value, 0));
    }

    ArchivePlayer player(rom, info);
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.WriteMemory(256, 0); }));
    SEQLIB_CHECK(Throws<std::out_of_range>([&] { player.WriteMemory(-1, 0); }));
}

void TestPlayerEnd()
{
    // A looped sample plays until its note's release of 0 stops it. The track ends in the same tick. Busy() stays true
    // until the FIFOs have played the mixer's last points: the frame after the mixer's last sound, and the next. The
    // output after it is silent. A reverb keeps the song busy while it echoes, and until the whole buffer holds 0: up
    // to a pass of the buffer's 7 parts later.
    std::array<int, 2> ends = {};
    for (int r = 0; r < 2; r++)
    {
        Cart cart;
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(Jumpy(224), 13379, 0))}});
        cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 60, 127).Wait(24).Fine()}, voices, 0, 0,
                  r ? 0x80 | 100 : 0);
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        ArchivePlayer player(rom, info);
        SEQLIB_CHECK(!player.Busy());
        player.Start(0);
        std::vector<std::vector<int16_t>> frames;
        while (player.Busy() && frames.size() < 600)
        {
            frames.emplace_back();
            player.RunFrame(frames.back());
        }
        SEQLIB_REQUIRE(!player.Busy());
        const auto loud = [](const std::vector<int16_t>& frame)
        {
            return std::any_of(frame.begin(), frame.end(), [](int16_t s) { return s != 0; });
        };
        size_t last = frames.size();
        while (last > 0 && !loud(frames[last - 1]))
        {
            last--;
        }
        SEQLIB_CHECK(last > 0 && last + (r ? 8 : 1) >= frames.size());
        SEQLIB_CHECK(!loud(Play(player, 10)));
        ends[size_t(r)] = int(frames.size());
    }
    SEQLIB_CHECK(ends[1] > ends[0] + 7);
}

void TestPlayerErrors()
{
    // The player refuses a song that can't be played, options out of range, a frame before a start, and Camelot's
    // mixer whose echo doesn't reach back within a frame.
    const TwoTracks songs = TwoTrackSongs();
    ArchivePlayer player(songs.rom, songs.info);
    SEQLIB_CHECK(Throws<std::logic_error>([&] { player.RunFrame(); }));
    SEQLIB_CHECK(Throws<std::logic_error>([&] { player.Driver(); }));
    SEQLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(5); }));
    for (int option = 0; option < 3; option++)
    {
        ArchivePlayerOptions options;
        options.store_cycle = option == 0 ? kFrameCycles : 0;
        options.envelope_counter = option == 1 ? 15 : 0;
        options.sequencer_step = option == 2 ? 8 : 0;
        SEQLIB_CHECK(Throws<std::invalid_argument>([&] { player.Start(0, options); }));
    }

    DriverInfo camelot = songs.info;
    camelot.camelot_mixer = true;
    camelot.camelot_tap = camelot.samples_per_frame;
    ArchivePlayer camelot_player(songs.rom, camelot);
    SEQLIB_CHECK(Throws<std::invalid_argument>([&] { camelot_player.Start(0); }));
}

// A test program's set for MP2K's archive mode: its files by path, and the song that it plays.
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

    static std::optional<std::string> Select(RomSet& set, ntdlib::mp2k::ArchiveSelection& selection)
    {
        selection.rom_path = "game.gba";
        selection.song = set.song;
        return std::nullopt;
    }
};

using RomSource = xsflib::player::ArchiveSource<ntdlib::mp2k::ArchiveTraits<RomProgram>>;

// Returns a set that holds `rom` as game.gba, with song `song` selected.
RomSet SetOf(const Rom& rom, int song)
{
    RomSet set;
    set.files["game.gba"].assign(rom.Ptr(kRomBase), rom.Ptr(kRomBase) + rom.Size());
    set.song = song;
    return set;
}

void TestPlayerArchiveSource()
{
    // Archive mode plays the song as the player does, takes snapshots, and gives the player's refusals as its errors. A
    // ROM without the driver fails to load.
    const TwoTracks songs = TwoTrackSongs(0x80 | 60);
    ArchivePlayer player(songs.rom, songs.info);
    player.Start(0);
    const std::vector<int16_t> expected = Play(player, 120);

    RomSource source;
    RomSet set = SetOf(songs.rom, 0);
    std::vector<int16_t> out;
    SEQLIB_REQUIRE(!source.Start(set, out));
    SEQLIB_CHECK(out.empty() && source.Busy());
    for (int f = 0; f < 60; f++)
    {
        SEQLIB_CHECK(source.RunFrame(out));
    }
    const std::shared_ptr<ArchivePlayer::Snapshot> snapshot = source.Save(nullptr);
    for (int f = 0; f < 60; f++)
    {
        source.RunFrame(out);
    }
    SEQLIB_CHECK(out == expected);
    source.Restore(*snapshot);
    std::vector<int16_t> again;
    for (int f = 0; f < 60; f++)
    {
        source.RunFrame(again);
    }
    SEQLIB_CHECK(std::equal(again.begin(), again.end(), expected.end() - long(again.size())));

    RomSet missing = SetOf(songs.rom, 5);
    const std::optional<std::string> refused = source.Start(missing, out);
    SEQLIB_CHECK(refused && refused->find("song 5") != std::string::npos);
    RomSet empty;
    const std::optional<std::string> absent = source.Start(empty, out);
    SEQLIB_CHECK(absent && absent->find("game.gba") != std::string::npos);
    RomSet blank = SetOf(Cart().ToRom(), 0);
    SEQLIB_CHECK(Throws<xsflib::FormatError>([&] { source.Start(blank, out); }));
}

// The enhanced mode's samples in each frame.
constexpr size_t kEnhanced = ntdlib::agb::kEnhancedFrameSamples;

// The enhanced mode's output of each frame, with the console's, and a channel's state after each frame.
struct EnhancedFrames
{
    std::vector<std::vector<float>> right;
    std::vector<std::vector<float>> left;
    MixedFrames console;
    std::vector<Channel> channels;
};

// Runs a sequencer for `frames` frames in the enhanced mode, and keeps each frame's output and channel `channel`. The
// frames in `without` run without the enhanced mode's mixing.
EnhancedFrames MixEnhancedFrames(Sequencer& seq, int frames, int channel = 0, const std::set<int>& without = {})
{
    EnhancedFrames out;
    seq.StartMixing();
    seq.StartEnhancedMixing();
    for (int f = 0; f < frames; f++)
    {
        seq.SetEnhancedFrames(!without.contains(f));
        seq.Step();
        out.right.emplace_back(seq.EnhancedRight().begin(), seq.EnhancedRight().end());
        out.left.emplace_back(seq.EnhancedLeft().begin(), seq.EnhancedLeft().end());
        out.console.right.emplace_back(seq.Right().begin(), seq.Right().end());
        out.console.left.emplace_back(seq.Left().begin(), seq.Left().end());
        out.channels.push_back(seq.GetChannel(channel));
    }

    return out;
}

// Returns whether every sample of a frame is 0.
bool Silent(const std::vector<float>& frame)
{
    return std::all_of(frame.begin(), frame.end(), [](float sample) { return sample == 0.0f; });
}

// A sine at a quarter of a sample's rate: 0, 100, 0 and -100, over and over.
std::vector<int8_t> QuarterSine(size_t length)
{
    std::vector<int8_t> pcm(length);
    const int8_t kCycle[4] = {0, 100, 0, -100};
    for (size_t i = 0; i < length; i++)
    {
        pcm[i] = kCycle[i % 4];
    }

    return pcm;
}

// A triangle wave of 128 points between -64 and 63, from `start` points into its cycle.
std::vector<int8_t> Triangle(size_t length, size_t start = 0)
{
    std::vector<int8_t> pcm(length);
    for (size_t i = 0; i < length; i++)
    {
        const int at = int((i + start) % 128);
        pcm[i] = int8_t(at < 64 ? at - 32 : 95 - at);
    }

    return pcm;
}

void TestEnhancedSamples()
{
    // A looped sine at a quarter of its rate plays at a fixed pitch, and then a fifth up. Each enhanced sample lies
    // between the console's points, at its share of the frame's move: the sine at its position, within the
    // interpolator's ripple of 0.01 dB. Each side's volume ramps across the frame from the last frame's. A volume
    // change and the release's frames ramp. Once the channel stops, the output is silent. The mono mixer plays both
    // sides at the average of the volumes.
    for (bool mono : {false, true})
    {
        Cart cart;
        if (mono)
        {
            const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
            for (uint32_t i = 0; i < 12; i++)
            {
                cart.Put8(kMonoVolume + i, kMono[i]);
            }
        }
        const uint32_t sine = cart.Wave(QuarterSine(64), 13379, 0);
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, sine, 60, {255, 0, 255, 200})},
                                                 {1, Cart::Voice(0, sine, 60, {255, 0, 255, 200})}});
        for (int key : {60, 67})
        {
            cart.Song({Track()
                           .Cmd(kCmdTempo, 75)
                           .Cmd(kCmdVoice, key == 60 ? 0 : 1)
                           .Cmd(kCmdVol, 127)
                           .Cmd(kCmdPan, 0x30)
                           .Wait(2)
                           .Note(24, key, 127)
                           .Wait(12)
                           .Cmd(kCmdVol, 50)
                           .Wait(60)
                           .Fine()},
                      voices);
        }
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        SEQLIB_REQUIRE(info.mono == mono);

        for (int song = 0; song < 2; song++)
        {
            Sequencer seq(rom, info, song);
            const EnhancedFrames e = MixEnhancedFrames(seq, 60);
            int first = -1;
            for (int f = 0; f < int(e.channels.size()) && first < 0; f++)
            {
                first = e.channels[size_t(f)].status ? f : -1;
            }
            SEQLIB_REQUIRE(first >= 0);
            SEQLIB_CHECK(Silent(e.right[size_t(first) - 1]) && Silent(e.left[size_t(first) - 1]));

            const auto gains = [&](const Channel& c)
            {
                const float scale = float(c.envelope * (info.master_volume + 1)) / 1048576.0f;
                if (mono)
                {
                    return std::array<float, 2>{float(c.right + c.left) * scale / 2.0f,
                                                float(c.right + c.left) * scale / 2.0f};
                }
                return std::array<float, 2>{float(c.right) * scale, float(c.left) * scale};
            };
            const uint32_t step = song == 0 ? 0x800000u : e.channels[size_t(first)].frequency * info.step_scale;
            double worst = 0.0;
            int compared = 0;
            int ramps = 0;
            int stopped = -1;
            for (int f = first + 1; f < int(e.channels.size()); f++)
            {
                const Channel& c = e.channels[size_t(f)];
                if (!c.status)
                {
                    stopped = f;
                    break;
                }
                const std::array<float, 2> to = gains(c);
                const std::array<float, 2> from = gains(e.channels[size_t(f) - 1]);
                ramps += from != to;
                for (size_t j = 0; j < kEnhanced; j++)
                {
                    const double position =
                        (double(f - first) * 224.0 * step + double(j) * 224.0 * step / double(kEnhanced)) / 8388608.0;
                    const double point = 100.0 * std::sin(2.0 * 3.141592653589793 * position / 4.0);
                    for (size_t side = 0; side < 2; side++)
                    {
                        const double gain = from[side] + double(j) * (to[side] - from[side]) / double(kEnhanced);
                        const float out = side == 0 ? e.right[size_t(f)][j] : e.left[size_t(f)][j];
                        worst =
                            std::max(worst, std::fabs(out - point * gain) / (100.0 * std::max(from[side], to[side])));
                    }
                    compared++;
                }
            }
            SEQLIB_CHECK(worst < 0.002);
            SEQLIB_CHECK(compared > 10 * int(kEnhanced) && ramps > 5);
            SEQLIB_CHECK(mono ? e.right == e.left : e.right[size_t(first) + 1] != e.left[size_t(first) + 1]);
            SEQLIB_REQUIRE(stopped > 0);
            for (int f = stopped; f < int(e.right.size()); f++)
            {
                SEQLIB_CHECK(Silent(e.right[size_t(f)]) && Silent(e.left[size_t(f)]));
            }
        }
    }
}

void TestEnhancedSpecialSamples()
{
    // A compressed triangle wave and a reversed one play a fifth up. Each enhanced sample lies within 2 points of the
    // console's output at its time, interpolated: the console rounds its points down, and interpolates in straight
    // lines.
    Cart cart;
    std::vector<uint8_t> blocks;
    for (int b = 0; b < 16; b++)
    {
        // Each block rises by 1 or falls by 1 for its 64 points.
        const bool rising = b % 2 == 0;
        blocks.push_back(uint8_t(rising ? -32 : 31));
        blocks.push_back(rising ? 0x01 : 0x0F);
        blocks.insert(blocks.end(), 31, uint8_t(rising ? 0x11 : 0xFF));
    }
    const std::vector<int8_t> points = Decompress(blocks);
    SEQLIB_REQUIRE(points.size() == 1024 && points[63] == 31 && points[64] == 31 && points[127] == -32);
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kVoiceCompressed, cart.CompressedWave(blocks, 1024))},
                                             {1, Cart::Voice(kVoiceReverse, cart.Wave(Triangle(1200, 20)))},
                                             {2, Cart::Voice(0, cart.Wave(Triangle(1200, 20)))}});
    for (int voice = 0; voice < 3; voice++)
    {
        cart.Song({Track().Cmd(kCmdVoice, voice).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(24).Fine()}, voices);
    }
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SEQLIB_REQUIRE(info.special_samples);

    std::array<std::vector<float>, 3> outputs;
    for (int song = 0; song < 3; song++)
    {
        Sequencer seq(rom, info, song);
        const EnhancedFrames e = MixEnhancedFrames(seq, 4);
        const int f = FirstSound(e.console);
        SEQLIB_REQUIRE(f >= 0);
        double worst = 0.0;
        for (int frame = f + 1; frame < f + 3; frame++)
        {
            const std::vector<int8_t>& console = e.console.right[size_t(frame)];
            for (size_t j = 0; j < kEnhanced; j++)
            {
                const double at = double(j) * 224.0 / double(kEnhanced);
                const size_t k = size_t(at);
                if (k + 1 >= console.size())
                {
                    continue;
                }
                const double expected = console[k] + (at - double(k)) * (console[k + 1] - console[k]);
                worst = std::max(worst, std::fabs(e.right[size_t(frame)][j] - expected));
            }
        }
        SEQLIB_CHECK(worst < 2.0);
        outputs[size_t(song)] = e.right[size_t(f) + 1];
    }

    // The reversed triangle plays otherwise than the forward one.
    double apart = 0.0;
    for (size_t j = 0; j < kEnhanced; j++)
    {
        apart = std::max(apart, double(std::fabs(outputs[1][j] - outputs[2][j])));
    }
    SEQLIB_CHECK(apart > 5.0);
}

// Returns a value of the enhanced mode's history: 0 below 2^-16.
float Kept(float value)
{
    return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
}

void TestEnhancedReverb()
{
    // A jumpy sample of 224 points plays for a frame at a fixed pitch, on the left, with a reverb of 100. Each later
    // frame holds the reverb alone: both sides of the frames a buffer's length of frames before and one frame later,
    // times the reverb over 512, on both sides. The mono mixer adds its one side, times the reverb over 256.
    for (bool mono : {false, true})
    {
        Cart cart;
        if (mono)
        {
            const uint8_t kMono[] = {0xA0, 0x78, 0xE1, 0x78, 0x40, 0x18, 0x68, 0x43, 0x40, 0x0A, 0xA0, 0x72};
            for (uint32_t i = 0; i < 12; i++)
            {
                cart.Put8(kMonoVolume + i, kMono[i]);
            }
        }
        const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(Jumpy(224)))}});
        cart.Song({Track().Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Cmd(kCmdPan, 0x10).Note(24, 60, 127).Wait(48).Fine()},
                  voices, 0, 0, 0x80 | 100);
        const Rom rom = cart.ToRom();
        const DriverInfo info = Detect(rom);
        SEQLIB_REQUIRE(info.mono == mono);

        Sequencer seq(rom, info, 0);
        const EnhancedFrames e = MixEnhancedFrames(seq, 50);
        const int first = FirstSound(e.console);
        SEQLIB_REQUIRE(first >= 0 && !e.channels[size_t(first)].status);
        const int period = int(ntdlib::mp2k::kPcmBufferSize) / info.samples_per_frame;
        const auto at = [&](const std::vector<std::vector<float>>& side, int f, size_t j)
        {
            return f < 0 ? 0.0f : Kept(side[size_t(f)][j]);
        };

        int differences = 0;
        bool echoes = false;
        for (int f = first + 1; f < int(e.right.size()); f++)
        {
            for (size_t j = 0; j < kEnhanced; j++)
            {
                const float expected =
                    mono ? (at(e.right, f - period, j) + at(e.right, f - period + 1, j)) * (100.0f / 256.0f)
                         : ((at(e.right, f - period, j) + at(e.left, f - period, j)) +
                            (at(e.right, f - period + 1, j) + at(e.left, f - period + 1, j))) *
                               (100.0f / 512.0f);
                differences += e.right[size_t(f)][j] != expected;
                differences += e.left[size_t(f)][j] != expected;
                echoes |= f > first + 2 * period && expected != 0.0f;
            }
        }
        SEQLIB_CHECK_EQ(differences, 0);
        SEQLIB_CHECK(echoes);
        SEQLIB_CHECK(mono ? e.right == e.left : e.right[size_t(first)] != e.left[size_t(first)]);
    }
}

// Returns the root mean square of the enhanced mode's right side, and of the console's, over frames `first` to `last`.
std::pair<double, double> Levels(const EnhancedFrames& e, int first, int last)
{
    double enhanced = 0.0;
    double console = 0.0;
    size_t enhanced_count = 0;
    size_t console_count = 0;
    for (int f = first; f <= last; f++)
    {
        for (float sample : e.right[size_t(f)])
        {
            enhanced += double(sample) * sample;
            enhanced_count++;
        }
        for (int8_t point : e.console.right[size_t(f)])
        {
            console += double(point) * point;
            console_count++;
        }
    }

    return {std::sqrt(enhanced / double(enhanced_count)), std::sqrt(console / double(console_count))};
}

// Returns the first DirectSound channel that sounds in song `song`'s first 4 frames, or -1. Camelot's driver starts a
// note on the last channel that its search finds.
int SoundingChannel(const Rom& rom, const DriverInfo& info, int song)
{
    Sequencer seq(rom, info, song);
    for (int f = 0; f < 4; f++)
    {
        seq.Step();
        for (int n = 0; n < info.max_channels; n++)
        {
            if (seq.GetChannel(n).status)
            {
                return n;
            }
        }
    }

    return -1;
}

// Counts the samples of the frames from `first` on that differ from Camelot's echo of the frames before them, and
// sets `echoes` when a frame 5 or more after `first` holds an echo. The frames in `without` are left out. The echo
// comes from the frames before: 4 frames back on each side, less a share of the other side, a quarter of the right
// side's last frame, and for the left side a quarter of the right side's from a frame and a third back: 268 samples.
int EchoDifferences(const EnhancedFrames& e, int first, const std::set<int>& without, bool& echoes)
{
    int differences = 0;
    for (int f = first; f < int(e.right.size()); f++)
    {
        if (without.contains(f))
        {
            continue;
        }
        const std::vector<float>& before = e.right[size_t(f) - 1];
        for (size_t j = 0; j < kEnhanced; j++)
        {
            const float old_right = Kept(e.right[size_t(f) - 4][j]);
            const float old_left = Kept(e.left[size_t(f) - 4][j]);
            const float delayed = j >= 268 ? before[j - 268] : Kept(e.right[size_t(f) - 2][j + 536]);
            const float expected_right =
                Kept((53.0f * old_right - 8.0f * old_left) / 128.0f + before[j] * (1.0f / 4.0f + 1.0f / 512.0f));
            const float expected_left = Kept((53.0f * old_left - 8.0f * old_right) / 128.0f + delayed * (1.0f / 4.0f));
            differences += e.right[size_t(f)][j] != expected_right;
            differences += e.left[size_t(f)][j] != expected_left;
            echoes |= f >= first + 5 && expected_left != 0.0f;
        }
    }

    return differences;
}

// Songs of Camelot's mixer. Song 0 plays a sample of 40 at a fixed pitch for 6 frames. Songs 1 to 3 hold a pulse, a
// saw and a triangle wave.
struct CamelotSongs
{
    Rom rom;
    DriverInfo info;
};

CamelotSongs MakeCamelotSongs()
{
    Cart cart(kCamelotMode, {10, 3}, true, false);
    cart.WriteCamelot();
    const uint32_t forty = cart.Wave(std::vector<int8_t>(64, 40), 31536, 0);
    const uint32_t pulse = cart.Synth(0, {0x80, 0x10, 0x40, 0x90});
    const uint32_t saw = cart.Synth(1, {});
    const uint32_t triangle = cart.Synth(2, {});
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(kFixed, forty)},
                                             {1, Cart::Voice(0, pulse)},
                                             {2, Cart::Voice(0, saw)},
                                             {3, Cart::Voice(0, triangle)}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(6, 60, 127).Wait(48).Fine()},
              voices);
    for (int voice = 1; voice < 4; voice++)
    {
        cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, voice).Cmd(kCmdVol, 50).Note(24, 48, 127).Wait(48).Fine()},
                  voices);
    }
    CamelotSongs songs{cart.ToRom(), {}};
    songs.info = Detect(songs.rom);

    return songs;
}

void TestEnhancedCamelot()
{
    // Camelot's mixer plays a sample of 40 at a fixed pitch, at the centre, for 6 frames. In its first frame, the
    // output is the sample at each side's volume: 9/8 of the envelope over 512, and the output stage's 128 to a point.
    // Once the channel stops, each frame holds the echo of the frames before it. The pulse, saw and triangle waves play
    // at the console's level, echo included, and the triangle wave at its points, across the frames' joins.
    const CamelotSongs songs = MakeCamelotSongs();
    const Rom& rom = songs.rom;
    const DriverInfo& info = songs.info;
    SEQLIB_REQUIRE(info.camelot_mixer);

    const int channel = SoundingChannel(rom, info, 0);
    SEQLIB_REQUIRE(channel >= 0);
    Sequencer seq(rom, info, 0);
    const EnhancedFrames e = MixEnhancedFrames(seq, 40, channel);
    int first = -1;
    int last = -1;
    for (int f = 0; f < int(e.channels.size()); f++)
    {
        if (e.channels[size_t(f)].status)
        {
            first = first < 0 ? f : first;
            last = f;
        }
    }
    SEQLIB_REQUIRE(first >= 0 && last > first && last + 10 < int(e.channels.size()));

    const Channel& c = e.channels[size_t(first)];
    const float scale = float(c.envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
    const float right = 40.0f * float(c.right) * scale;
    const float left = 40.0f * float(c.left) * scale;
    double worst = 0.0;
    for (size_t j = 200; j < kEnhanced; j++)
    {
        worst = std::max(worst, double(std::fabs(e.right[size_t(first)][j] - right)));
        worst = std::max(worst, double(std::fabs(e.left[size_t(first)][j] - left)));
    }
    SEQLIB_CHECK(worst < 1e-4 && right > 1.0f && left > 1.0f);

    bool echoes = false;
    SEQLIB_CHECK_EQ(EchoDifferences(e, last + 1, {}, echoes), 0);
    SEQLIB_CHECK(echoes);

    for (int song = 1; song < 4; song++)
    {
        const int n = SoundingChannel(rom, info, song);
        SEQLIB_REQUIRE(n >= 0);
        Sequencer synth(rom, info, song);
        const EnhancedFrames e = MixEnhancedFrames(synth, 20, n);
        const int f = FirstSound(e.console);
        SEQLIB_REQUIRE(f >= 0);

        // The console rounds each side's volume down, and halves it again for the saw. The enhanced output, scaled to
        // the console's volume, plays at the console's level.
        const Channel& c = e.channels[size_t(f) + 2];
        const int32_t rounded = ((c.right * (c.envelope + (c.envelope >> 3))) >> 9) >> (song == 2 ? 1 : 0);
        const double exact = c.right * c.envelope * 9.0 / 8.0 / 512.0 / (song == 2 ? 2.0 : 1.0);
        const double scale = rounded / exact;
        const auto [enhanced, console] = Levels(e, f + 2, f + 12);
        SEQLIB_CHECK(console > 5.0 && std::fabs(20.0 * std::log10(enhanced * scale / console)) < 0.15);

        // The triangle wave lies within 2 points of the console's points, at each frame's start and end too. The
        // console's sums and echo round towards 0, and the echo adds up their errors over later frames.
        if (song == 3)
        {
            double worst = 0.0;
            for (int frame = f + 1; frame < f + 5; frame++)
            {
                const std::vector<int8_t>& points = e.console.right[size_t(frame)];
                for (size_t j = 0; j < kEnhanced; j++)
                {
                    const double at = double(j) * double(points.size()) / double(kEnhanced);
                    const size_t k = size_t(at);
                    if (k + 1 >= points.size())
                    {
                        continue;
                    }
                    const double expected = points[k] + (at - double(k)) * (points[k + 1] - points[k]);
                    worst = std::max(worst, std::fabs(e.right[size_t(frame)][j] * scale - expected));
                }
            }
            SEQLIB_CHECK(worst < 2.0);
        }
    }
}

void TestEnhancedFramesWithout()
{
    // Frames without the enhanced mode's mixing are silent, and move the channel on. The frames after them give the
    // samples of a run that mixed every frame.
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Triangle(4000)), 60, {255, 0, 255, 200})}});
    cart.Song({Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 127).Note(24, 67, 127).Wait(48).Fine()},
              voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer all(rom, info, 0);
    const EnhancedFrames a = MixEnhancedFrames(all, 30);
    Sequencer some(rom, info, 0);
    const EnhancedFrames b = MixEnhancedFrames(some, 30, 0, {3, 4, 5, 6});
    for (int f = 3; f < 7; f++)
    {
        SEQLIB_CHECK(Silent(b.right[size_t(f)]) && Silent(b.left[size_t(f)]));
        SEQLIB_CHECK(!Silent(a.right[size_t(f)]));
    }
    for (int f = 7; f < 30; f++)
    {
        SEQLIB_CHECK(a.right[size_t(f)] == b.right[size_t(f)] && a.left[size_t(f)] == b.left[size_t(f)]);
    }
    SEQLIB_CHECK(a.console.right == b.console.right && a.console.left == b.console.left);

    // After Camelot's sample, two frames without the mixing. The echo takes them as silence.
    const CamelotSongs camelot = MakeCamelotSongs();
    const int channel = SoundingChannel(camelot.rom, camelot.info, 0);
    SEQLIB_REQUIRE(channel >= 0);
    Sequencer gap(camelot.rom, camelot.info, 0);
    const EnhancedFrames g = MixEnhancedFrames(gap, 30, channel, {12, 13});
    SEQLIB_REQUIRE(!g.channels[10].status);
    bool echoes = false;
    SEQLIB_CHECK_EQ(EchoDifferences(g, 11, {12, 13}, echoes), 0);
    SEQLIB_CHECK(echoes && Silent(g.right[12]) && Silent(g.left[13]) && !Silent(g.right[14]));
}

// Returns the time in seconds of the first sample of `out`'s side `side` (0 for the left) whose magnitude reaches
// `level`, or -1.
template <typename Sample>
double FirstAt(const std::vector<Sample>& out, size_t side, double level, double rate)
{
    for (size_t i = side; i < out.size(); i += 2)
    {
        if (std::fabs(double(out[i])) >= level)
        {
            return double(i / 2) / rate;
        }
    }

    return -1.0;
}

// In the enhanced mode, two square 1 notes of two tracks play together: the second on a grown channel in PSG unit 1.
// Each plays as it plays alone, and the output is the sum. A mute of the second track gives the first alone. The
// second track's meter takes the grown channel's peaks.
void TestEnhancedPlayerGrows()
{
    Cart cart;
    const uint32_t voices = cart.VoiceGroup({{1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const auto track = [](int key, int pan)
    {
        return Track()
            .Cmd(kCmdTempo, 75)
            .Cmd(kCmdVoice, 1)
            .Cmd(kCmdVol, 100)
            .Cmd(kCmdPan, pan)
            .Wait(3)
            .Note(48, key, 127);
    };
    cart.Song({track(60, 0x40).Wait(60).Fine(), track(67, 0x40).Wait(60).Fine()}, voices);
    cart.Song({track(60, 0x40).Wait(60).Fine()}, voices);
    cart.Song({Track().Cmd(kCmdTempo, 75).Wait(60).Fine(), track(67, 0x40).Wait(60).Fine()}, voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ArchivePlayerOptions options;
    options.enhanced = true;
    const auto play = [&](int song, bool mute, std::array<int32_t, 2>* meter)
    {
        ArchivePlayer player(rom, info);
        player.Start(song, options);
        player.MuteTrack(1, mute);
        player.MeterTracks(meter != nullptr);
        std::vector<float> out;
        for (int f = 0; f < 30; f++)
        {
            player.RunFrame(out);
            if (meter)
            {
                for (size_t side = 0; side < 2; side++)
                {
                    (*meter)[side] = std::max((*meter)[side], player.TrackPeaks(1)[side]);
                }
            }
        }
        return out;
    };
    std::array<int32_t, 2> meter = {};
    const std::vector<float> both = play(0, false, &meter);
    const std::vector<float> first = play(1, false, nullptr);
    const std::vector<float> second = play(2, false, nullptr);
    const std::vector<float> muted = play(0, true, nullptr);
    SEQLIB_REQUIRE(both.size() == first.size() && both.size() == second.size() && both.size() == muted.size());
    double worst = 0.0;
    double worst_muted = 0.0;
    float loudest = 0.0f;
    for (size_t i = 0; i < both.size(); i++)
    {
        worst = std::max(worst, std::fabs(double(both[i]) - double(first[i]) - double(second[i])));
        worst_muted = std::max(worst_muted, std::fabs(double(muted[i]) - double(first[i])));
        loudest = std::max(loudest, std::fabs(second[i]));
    }
    SEQLIB_CHECK(loudest > 0.05f);
    SEQLIB_CHECK(worst < 1e-6);
    SEQLIB_CHECK(worst_muted < 1e-6);
    SEQLIB_CHECK(meter[0] > 0 && meter[1] > 0);
}

void TestEnhancedPlayer()
{
    // The player in the enhanced mode: about 48,022 Hz, 804 samples a frame on each side, and the console's timing
    // 32 samples later. A sample's onset reaches half its level half a point before the console's output steps. A
    // square's first edge reaches half its level at the console's step.
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(kFixed, cart.Wave(std::vector<int8_t>(64, 100), 13379, 0))},
                         {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Wait(3).Note(24, 60, 127).Wait(48).Fine()},
        voices);
    cart.Song(
        {Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(3).Note(24, 60, 127).Wait(48).Fine()},
        voices);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    const double enhanced_rate = 16777216.0 * double(kEnhanced) / double(kFrameCycles);
    const double delay = double(ntdlib::wave::BandLimitedSteps::kHalfWidth) / enhanced_rate;

    for (int song = 0; song < 2; song++)
    {
        ArchivePlayer console(rom, info);
        console.Start(song);
        const std::vector<int16_t> c = Play(console, 12);
        ArchivePlayer player(rom, info);
        ArchivePlayerOptions options;
        options.enhanced = true;
        player.Start(song, options);
        SEQLIB_CHECK(player.Enhanced() && player.Rate() == ntdlib::agb::kEnhancedRate);
        std::vector<float> e;
        bool sizes = true;
        for (int f = 0; f < 12; f++)
        {
            const size_t before = e.size();
            player.RunFrame(e);
            sizes &= e.size() - before == 2 * kEnhanced;
        }
        SEQLIB_CHECK(sizes);

        // The console's output steps to its level. The enhanced output crosses half of it.
        const int32_t peak = Peak(c, 1);
        SEQLIB_REQUIRE(peak > 0);
        const double console_onset = FirstAt(c, 1, 1.0, double(console.Rate()));
        const double enhanced_onset = FirstAt(e, 1, 0.5 * peak / 32768.0, enhanced_rate);
        SEQLIB_REQUIRE(console_onset > 0.0 && enhanced_onset > 0.0);
        const double offset = enhanced_onset - delay - console_onset - (song == 0 ? -0.5 / 13379.0 : 0.0);
        SEQLIB_CHECK(std::fabs(offset) < 1.5 / enhanced_rate);
        SEQLIB_CHECK(Throws<std::logic_error>(
            [&]
            {
                std::vector<int16_t> out;
                player.RunFrame(out);
            }));
        SEQLIB_CHECK(Throws<std::logic_error>(
            [&]
            {
                std::vector<float> out;
                console.RunFrame(out);
            }));
    }

    // Busy() follows the console's mixer, and a reverb keeps it busy as long.
    const TwoTracks wet = TwoTrackSongs(0x80 | 60);
    ArchivePlayerOptions options;
    options.enhanced = true;
    ArchivePlayer enhanced(wet.rom, wet.info);
    enhanced.Start(0, options);
    ArchivePlayer console(wet.rom, wet.info);
    console.Start(0);
    bool busy = true;
    for (int f = 0; f < 200; f++)
    {
        std::vector<float> frame;
        enhanced.RunFrame(frame);
        console.RunFrame();
        busy &= enhanced.Busy() == console.Busy();
    }
    SEQLIB_CHECK(busy && !enhanced.Busy());

    // A snapshot gives the frames after it again. Two frames after frames without samples, the output is the
    // reference's: the FIFOs' silence of those frames has played, and the squares' steps have settled.
    const TwoTracks dry = TwoTrackSongs();
    ArchivePlayer reference(dry.rom, dry.info);
    reference.Start(0, options);
    std::vector<float> all;
    for (int f = 0; f < 110; f++)
    {
        reference.RunFrame(all);
    }
    ArchivePlayer player(dry.rom, dry.info);
    player.Start(0, options);
    std::vector<float> first;
    for (int f = 0; f < 30; f++)
    {
        player.RunFrame(first);
    }
    for (int f = 0; f < 20; f++)
    {
        player.RunFrame();
    }
    const ArchivePlayer::Snapshot snapshot = player.Save();
    std::vector<float> after;
    for (int f = 0; f < 60; f++)
    {
        player.RunFrame(after);
    }
    player.Restore(snapshot);
    SEQLIB_CHECK(player.Enhanced());
    std::vector<float> again;
    for (int f = 0; f < 60; f++)
    {
        player.RunFrame(again);
    }
    SEQLIB_CHECK(after == again && after.size() == 120 * kEnhanced);
    SEQLIB_CHECK(std::equal(first.begin(), first.end(), all.begin()));
    SEQLIB_CHECK(std::equal(after.begin() + 2 * 2 * kEnhanced, after.end(), all.begin() + 52 * 2 * kEnhanced));
    SEQLIB_CHECK(!std::equal(after.begin(), after.begin() + 2 * kEnhanced, all.begin() + 50 * 2 * kEnhanced));

    // A frame without samples leaves its DirectSound samples silent. At a frame's start, the FIFOs hold 118 samples
    // of the frame before: the band-limited steps' delay of 32, and the 24 points ahead of the timer, 86 samples. The
    // first frame after the frame without samples sounds for those 118, and the second is silent for them.
    ArchivePlayer saw(dry.rom, dry.info);
    saw.Start(2, options);
    std::vector<float> sounding;
    for (int f = 0; f < 10; f++)
    {
        saw.RunFrame(sounding);
    }
    saw.RunFrame();
    std::vector<float> next;
    saw.RunFrame(next);
    saw.RunFrame(next);
    SEQLIB_CHECK(!Silent(std::vector<float>(sounding.end() - 2 * kEnhanced, sounding.end())));
    const auto part = [&](size_t from, size_t to)
    {
        return std::vector<float>(next.begin() + 2 * from, next.begin() + 2 * to);
    };
    SEQLIB_CHECK(!Silent(part(0, 118)) && Silent(part(118, kEnhanced)));
    SEQLIB_CHECK(Silent(part(kEnhanced, kEnhanced + 118)) && !Silent(part(kEnhanced + 118, 2 * kEnhanced)));
}

int RunTests()
{
    if (!ntdlib::testing::agb_files::MakeTempFolder())
    {
        std::fprintf(stderr, "can't create a temporary folder for the test output\n");
        return 1;
    }

    TestDecoding();
    TestDetection();
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
    TestConversion();
    TestSongFiles();
    TestVoiceGroupFiles();
    TestNoteChannels();
    TestPsgNoteTakenBeforeItSounds();
    TestPerformance();
    TestLevels();
    TestMonoLevels();
    TestSlowAttackAtEnd();
    TestTwoVoicesInATick();
    TestSharedSoundfont();
    TestSharedPresets();
    TestSharedDrumChannel();
    TestDrumChannel();
    TestVoiceChannels();
    TestVoiceChannelsDrums();
    TestVoiceChannelsLoop();
    TestPsgExclusive();
    TestPsgExclusiveShared();
    TestPsgNoReverb();
    TestCompressedSample();
    TestMixFixedPitch();
    TestMixInterpolation();
    TestMixSpecialSamples();
    TestMixReverb();
    TestMixMono();
    TestPsgSquareStores();
    TestPsgWaveStores();
    TestPsgNoiseStores();
    TestPsgWaveVolumes();
    TestEnvelopeCounter();
    TestPsgEchoStores();
    TestPsgStartingRegisters();
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
    TestCamelotDetection();
    TestCamelotSequencer();
    TestCamelotMixer();
    TestCamelotMixing();
    TestLaterCamelotMixing();
    TestLaterCamelotSteps();
    TestLaterCamelotPriority();
    TestCamelotConversion();
    TestMetroidDetection();
    TestMetroidSequencer();
    TestMetroidLfo();
    TestMetroidPsgEnvelope();
    TestPlayerFifo();
    TestPlayerStoreCycle();
    TestPlayerFramesWithoutSamples();
    TestPlayerSnapshots();
    TestPlayerMutes();
    TestPlayerMeters();
    TestPlayerNotes();
    TestPlayerMemory();
    TestPlayerEnd();
    TestPlayerErrors();
    TestPlayerArchiveSource();
    TestEnhancedSamples();
    TestEnhancedSpecialSamples();
    TestEnhancedReverb();
    TestEnhancedCamelot();
    TestEnhancedFramesWithout();
    TestEnhancedPlayer();
    TestEnhancedPlayerGrows();

    std::error_code ec;
    std::filesystem::remove_all(ntdlib::testing::agb_files::g_temp, ec);
    return seqlib::testing::Summary("mp2k");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
