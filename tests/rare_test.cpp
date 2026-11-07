// SPDX-License-Identifier: MIT

// Unit tests for Rare's driver. They build small synthetic cartridges in memory, so no game ROM is needed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/rare/archive_player.h"
#include "ntdlib/rare/driver.h"
#include "ntdlib/rare/sequencer.h"
#include "ntdlib/rare/song.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/rare/convert.h"
#include "ntdlib/sequence_export/rare/performance.h"
#include "ntdlib/sequence_export/rare/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/rare_cart.h"

namespace ntdlib::sequence_export::rare
{
namespace
{

using ntdlib::agb::kFrameRate;
using ntdlib::agb::kRomBase;
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::Rom;
using ntdlib::agb::Utf8;
using ntdlib::rare::Action;
using ntdlib::rare::ArchivePlayer;
using ntdlib::rare::DecodeEvent;
using ntdlib::rare::DescribeEvent;
using ntdlib::rare::DetectDriver;
using ntdlib::rare::DriverInfo;
using ntdlib::rare::DriverOverrides;
using ntdlib::rare::EnvelopeSettings;
using ntdlib::rare::Event;
using ntdlib::rare::FadeFrames;
using ntdlib::rare::Format;
using ntdlib::rare::Instrument;
using ntdlib::rare::kCmdBend;
using ntdlib::rare::kCmdControl;
using ntdlib::rare::kCmdDelay1;
using ntdlib::rare::kCmdDelay2;
using ntdlib::rare::kCmdDelay3;
using ntdlib::rare::kCmdEnd;
using ntdlib::rare::kCmdNoteOff;
using ntdlib::rare::kCmdNoteOn;
using ntdlib::rare::kCmdNoteOnB;
using ntdlib::rare::kCmdProgram;
using ntdlib::rare::kCmdTempo;
using ntdlib::rare::kCtrlAttack;
using ntdlib::rare::kCtrlDecay;
using ntdlib::rare::kCtrlLoopEnd;
using ntdlib::rare::kCtrlLoopStart;
using ntdlib::rare::kCtrlModulation;
using ntdlib::rare::kCtrlRelease;
using ntdlib::rare::kCtrlSustain;
using ntdlib::rare::kCtrlVolume;
using ntdlib::rare::kFullLevel;
using ntdlib::rare::kInstDrumKit;
using ntdlib::rare::kInstKeySplit;
using ntdlib::rare::kInstSample;
using ntdlib::rare::kLoopForward;
using ntdlib::rare::kLoopForwardB;
using ntdlib::rare::kLoopOnce;
using ntdlib::rare::kSlotFree;
using ntdlib::rare::kSlotOn;
using ntdlib::rare::kSlotReleased;
using ntdlib::rare::kTempoScale;
using ntdlib::rare::LookUpInstrument;
using ntdlib::rare::MixResult;
using ntdlib::rare::MusicVolume;
using ntdlib::rare::ReadInstrument;
using ntdlib::rare::ReadTuneHeader;
using ntdlib::rare::Sequencer;
using ntdlib::rare::SilencedChannels;
using ntdlib::rare::Slot;
using ntdlib::rare::SongCount;
using ntdlib::rare::TuneAddress;
using ntdlib::rare::TuneHeader;
using ntdlib::sequence_export::agb::kDrumBank;
using ntdlib::sequence_export::agb::PresetInstrument;
using ntdlib::sequence_export::agb::VolumeEnvelope;

using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;
using ntdlib::testing::rare_cart::Cart;
using ntdlib::testing::rare_cart::kInit;
using ntdlib::testing::rare_cart::kPitchTable;
using ntdlib::testing::rare_cart::kSineTable;
using ntdlib::testing::rare_cart::kTuneTable;
using ntdlib::testing::rare_cart::Saw;
using ntdlib::testing::rare_cart::Track;

// Returns the driver info of a cartridge, failing the test if detection fails.
DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

// Runs a sequencer for `frames` frames, and returns the frame each note on was played on, as (frame, key).
std::vector<std::pair<uint32_t, int>> NoteFrames(Sequencer& seq, int frames)
{
    std::vector<std::pair<uint32_t, int>> out;
    for (int f = 0; f < frames; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kNoteOn)
            {
                out.push_back({a.frame, a.a});
            }
        }
    }

    return out;
}

void TestDecoding()
{
    // The same commands in both formats.
    for (Format format : {Format::kChannelByte, Format::kChannelNibble})
    {
        Cart cart(format);
        const std::vector<uint8_t> track = Track(format)
                                               .Tempo(500000)
                                               .Wait(0x123456)
                                               .On(3, 60, 100)
                                               .Off(3, 60)
                                               .Control(3, 7, 90)
                                               .Program(3, 12)
                                               .Bend(3, 0x3000)
                                               .Wait(200)
                                               .Wait(0x1234)
                                               .End();
        const uint32_t at = cart.Place(track, 1);
        const Rom rom = cart.ToRom();

        uint32_t a = at;
        Event e;
        auto next = [&]()
        {
            const bool ok = DecodeEvent(rom, a, format, e);
            a += e.size;

            return ok;
        };

        SEQLIB_CHECK(next() && e.command == kCmdTempo && e.value == 500000);
        SEQLIB_CHECK(next() && e.command == kCmdDelay3 && e.value == 0x123456);
        SEQLIB_CHECK(next() && e.command == kCmdNoteOn && e.channel == 3 && e.a == 60 && e.b == 100);
        SEQLIB_CHECK(next() && e.command == kCmdNoteOff && e.channel == 3 && e.a == 60);
        SEQLIB_CHECK(next() && e.command == kCmdControl && e.a == 7 && e.b == 90);
        SEQLIB_CHECK(next() && e.command == kCmdProgram && e.a == 12);
        SEQLIB_CHECK(next() && e.command == kCmdBend && e.value == 0x3000);
        SEQLIB_CHECK(DescribeEvent(e) == "pitch bend ch 3 4096");
        SEQLIB_CHECK(next() && e.command == kCmdDelay1 && e.value == 200);
        SEQLIB_CHECK(next() && e.command == kCmdDelay2 && e.value == 0x1234);
        SEQLIB_CHECK(next() && e.command == kCmdEnd);
    }

    // A command number the driver doesn't have.
    Cart cart(Format::kChannelByte);
    const uint32_t at = cart.Place({0x0D, 0x00}, 1);
    Event e;

    SEQLIB_CHECK(!DecodeEvent(cart.ToRom(), at, Format::kChannelByte, e));
}

void TestDetection()
{
    // The driver's code gives the tune table, the format and the slots for each channel. The table ends before the
    // first entry that isn't a tune.
    Cart cart(Format::kChannelNibble, 5);
    const auto bank = cart.Bank({{0, cart.Sample(Saw())}});
    cart.Tune({Track(Format::kChannelNibble).End()}, bank);
    cart.Tune({Track(Format::kChannelNibble).End(), Track(Format::kChannelNibble).End()}, bank);
    const Rom rom = cart.ToRom();

    const DriverInfo info = Detect(rom);

    SEQLIB_CHECK_EQ(info.init, kInit);
    SEQLIB_CHECK_EQ(info.tune_table, kTuneTable);
    SEQLIB_CHECK_EQ(info.tune_count, 2);
    SEQLIB_CHECK(info.format == Format::kChannelNibble);
    SEQLIB_CHECK_EQ(info.slots_per_channel, 5);
    SEQLIB_CHECK_EQ(info.voice_limit, 8);
    SEQLIB_CHECK_EQ(info.mix_rate, 13379);
    SEQLIB_CHECK_EQ(info.pitch_table, kPitchTable);
    SEQLIB_CHECK_EQ(info.sine_table, kSineTable);
    SEQLIB_CHECK_EQ(FadeFrames(info, 95), 5);
    SEQLIB_CHECK_EQ(FadeFrames(info, 99), 0);
    SEQLIB_CHECK(!info.envelope_controllers && !info.frees_loop_once);
    SEQLIB_CHECK(info.warnings.empty());

    // The code of Donkey Kong Country 3's revision.
    cart.AddEnvelopeControllers();

    const DriverInfo newer = Detect(cart.ToRom());

    SEQLIB_CHECK(newer.envelope_controllers && newer.frees_loop_once);
    SEQLIB_CHECK(std::find(newer.log.begin(), newer.log.end(),
                           "controllers 20-23 set a channel's attack, decay, sustain and release") != newer.log.end());

    // Overrides take the place of what detection finds.
    DriverOverrides overrides;
    overrides.tune_count = 1;
    DriverInfo one;
    std::string error;

    SEQLIB_CHECK(DetectDriver(rom, overrides, one, error));

    SEQLIB_CHECK_EQ(one.tune_count, 1);

    // A cartridge without the driver's code fails, unless the tune table is given. Its format is read from its tracks
    // then.
    Cart bare(Format::kChannelByte, 6, false);
    const auto bare_bank = bare.Bank({{0, bare.Sample(Saw())}});
    bare.Tune({Track(Format::kChannelByte).Program(1, 0).On(1, 60, 100).Wait(10).Off(1, 60).End()}, bare_bank);
    const Rom bare_rom = bare.ToRom();
    DriverOverrides table;
    table.tune_table = kTuneTable;
    DriverInfo none;

    SEQLIB_CHECK(!DetectDriver(bare_rom, DriverOverrides(), none, error));
    SEQLIB_CHECK(DetectDriver(bare_rom, table, none, error));
    SEQLIB_CHECK(none.format == Format::kChannelByte);
    SEQLIB_CHECK_EQ(none.tune_count, 1);
    SEQLIB_CHECK(!none.warnings.empty());

    // Without the driver's fade table, the fades fall from 255 frames at index 0 to none at index 99.
    SEQLIB_CHECK_EQ(FadeFrames(none, 0), 255);
    SEQLIB_CHECK_EQ(FadeFrames(none, 10), 89);
    SEQLIB_CHECK_EQ(FadeFrames(none, 90), 8);
    SEQLIB_CHECK_EQ(FadeFrames(none, 98), 2);
    SEQLIB_CHECK_EQ(FadeFrames(none, 99), 0);
    for (uint32_t i = 0; i < 99; i++)
    {
        SEQLIB_CHECK(FadeFrames(none, i) >= FadeFrames(none, i + 1));
    }
}

// The game's code sets the music volume as it starts a tune, and the mixer scales every note of the tune by it. The
// MIDI file gives each channel the music volume as expression, written as 127 × √(volume / 128). A game that the table
// of games doesn't list plays at the init's 128. Donkey Kong Country 3's revision leaves out the notes whose level is 0
// before their velocity counts: a lower music volume leaves out those of more channel volumes.
void TestMusicVolume()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw())}});
    for (int t = 0; t < 3; t++)
    {
        cart.Tune({Track(kFormat).Program(0, 0).On(0, 60, 100).Wait(48).Off(0, 60).End()}, bank);
    }
    const Rom rom = cart.ToRom();
    DriverInfo info = Detect(rom);

    SEQLIB_CHECK_EQ(MusicVolume(info, 0), 128);
    SEQLIB_CHECK(info.warnings.empty());

    // The expression that each tune's channel gets in its MIDI file.
    auto expression = [](const Rom& r, const DriverInfo& i, int tune)
    {
        const SongReport report = ConvertSong(r, i, tune, TempSettings("music_volume"), nullptr);
        std::vector<int> values;
        for (const auto& e : ReadMidi(report.midi_path).Find(0xB0))
        {
            if (e.bytes[1] == 11)
            {
                values.push_back(e.bytes[2]);
            }
        }

        return values;
    };

    SEQLIB_CHECK(expression(rom, info, 0) == std::vector<int>({127}));

    // 70 is written as 94 and 40 as 71. A music volume above 128 plays at 128.
    info.music_volumes = {70, 40, 150};

    SEQLIB_CHECK_EQ(MusicVolume(info, 1), 40);
    SEQLIB_CHECK_EQ(MusicVolume(info, 3), 128);
    SEQLIB_CHECK(expression(rom, info, 0) == std::vector<int>({94}));
    SEQLIB_CHECK(expression(rom, info, 1) == std::vector<int>({71}));
    SEQLIB_CHECK(expression(rom, info, 2) == std::vector<int>({127}));

    // A game that the table of games lists, in another build, plays at 128 and gets a warning.
    Cart other = cart;
    other.PutBytes(kRomBase + 0xAC, {'B', '2', 'D', 'E'});
    const DriverInfo other_info = Detect(other.ToRom());

    SEQLIB_CHECK_EQ(MusicVolume(other_info, 0), 128);
    SEQLIB_CHECK_EQ(other_info.warnings.size(), 1u);

    // In Donkey Kong Country 3's revision, a channel at volume 1 has a level of 0 at a music volume of 39, and of 1 at
    // 69. Its note is then left out at 39 and mixed at 69. A build that the table of games doesn't list
    // plays at 69.
    Cart levels(kFormat);
    const auto levels_bank = levels.Bank({{0, levels.Sample(Saw())}});
    levels.Tune({Track(kFormat).Program(0, 0).Control(0, kCtrlVolume, 1).On(0, 60, 100).Wait(48).Off(0, 60).End()},
                levels_bank);
    levels.AddEnvelopeControllers();
    levels.AddLevelTest();
    const Rom levels_rom = levels.ToRom();
    DriverInfo levels_info = Detect(levels_rom);

    SEQLIB_CHECK_EQ(MusicVolume(levels_info, 0), 69);
    SEQLIB_CHECK_EQ(levels_info.warnings.size(), 1u);

    auto first_mix = [&](const DriverInfo& i)
    {
        Sequencer seq(levels_rom, i, 0);
        seq.Step();
        return seq.GetSlot(0).mix;
    };

    SEQLIB_CHECK(first_mix(levels_info) == MixResult::kMixed);
    levels_info.music_volumes = {39};
    SEQLIB_CHECK(first_mix(levels_info) == MixResult::kSilent);
}

// Donkey Kong Country 3's code picks the arrangement of a tune by the water. In the driver's revision with channel
// levels, detection reads the tune and the channels of each arrangement from the routine that does it.
void TestWaterTune()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw())}});
    cart.Tune({Track(kFormat).End()}, bank);
    cart.Tune({Track(kFormat).End()}, bank);
    const uint32_t routine = cart.AddWaterRoutine(1, 0, 3, 4, 8);
    const Rom older = cart.ToRom();
    cart.AddEnvelopeControllers();
    cart.AddLevelTest();

    const DriverInfo info = Detect(cart.ToRom());

    SEQLIB_CHECK_EQ(info.water.tune, 1);
    SEQLIB_CHECK_EQ(info.water.above, 0x000F);
    SEQLIB_CHECK_EQ(info.water.under, 0x01F0);
    SEQLIB_CHECK(std::find(info.log.begin(), info.log.end(),
                           "the game's code plays tune 1 on channels 0-3 above water and 4-8 under water") !=
                 info.log.end());
    SEQLIB_CHECK_EQ(SilencedChannels(info, 1, false), 0x01F0);
    SEQLIB_CHECK_EQ(SilencedChannels(info, 1, true), 0x000F);
    SEQLIB_CHECK_EQ(SilencedChannels(info, 0, true), 0);
    const std::vector<SongVersion> versions = TuneVersions(info, 1);
    SEQLIB_REQUIRE_EQ(versions.size(), 2);
    SEQLIB_CHECK(versions[0].name.empty() && versions[0].suffix.empty());
    SEQLIB_CHECK(versions[1].name == "under water" && versions[1].suffix == "_underwater");
    SEQLIB_CHECK_EQ(TuneVersions(info, 0).size(), 1);

    // The older revisions have no channel levels.
    SEQLIB_CHECK_EQ(Detect(older).water.tune, -1);

    // A routine whose states don't swap the same channels isn't the game's: here the state under water fades in
    // channels 4-7, and the state above water fades out 4-8.
    cart.Put16(routine + 36, 0x2307);

    SEQLIB_CHECK_EQ(Detect(cart.ToRom()).water.tune, -1);

    // Nor is one that checks a tune past the table.
    cart.Put16(routine + 36, 0x2308);
    cart.Put16(routine + 2, 0x2802);

    SEQLIB_CHECK_EQ(Detect(cart.ToRom()).water.tune, -1);
}

void TestTiming()
{
    // At 500,000 us a quarter and 480 ticks a quarter, 480 ticks are 30 frames of 1/60 s. The driver takes only 1/256
    // us off the first track's time in the first frame, before the tempo is known, so that track runs a frame late.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(0, 0).Wait(480).On(0, 60, 100).Wait(10).Off(0, 60).End(),
               Track(kFormat).Program(1, 0).Wait(480).On(1, 62, 100).Wait(10).Off(1, 62).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    const auto notes = NoteFrames(seq, 40);

    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    SEQLIB_CHECK_EQ(notes[0].first, 30);
    SEQLIB_CHECK_EQ(notes[0].second, 62);
    SEQLIB_CHECK_EQ(notes[1].first, 31);
    SEQLIB_CHECK_EQ(notes[1].second, 60);
}

void TestSlots()
{
    // A channel has 6 slots. A seventh note takes a slot whose note is releasing, and an eighth, with every slot's note
    // on, is left out. Two notes of one key take the slots of two released notes, and a note off for the key releases
    // only the first.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});
    Track t(kFormat);
    t.Tempo(500000).Program(2, 0);
    for (int k = 60; k < 66; k++)
    {
        t.On(2, k, 100);
    }
    t.Wait(48).Off(2, 61).Wait(48).On(2, 70, 100).On(2, 71, 100).Wait(48);
    t.Off(2, 62).Off(2, 63).On(2, 72, 100).On(2, 72, 100).Wait(48).Off(2, 72);
    cart.Tune({t.End()}, bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<Action> actions;
    for (int frame = 0; frame < 20; frame++)
    {
        for (const Action& a : seq.Step())
        {
            actions.push_back(a);
        }
    }

    std::vector<int> slots, dropped, released;
    for (const Action& a : actions)
    {
        if (a.kind == Action::kNoteOn)
        {
            slots.push_back(a.slot);
        }
        if (a.kind == Action::kNoteDropped)
        {
            dropped.push_back(a.a);
        }
        if (a.kind == Action::kNoteOff && a.a == 72)
        {
            released.push_back(a.slot);
        }
    }

    // Keys 60-65 take slots 12-17, key 70 takes 61's slot once it's releasing, and 71 is dropped. The two 72s take 62's
    // and 63's slots, and the note off releases the first.
    SEQLIB_CHECK(slots == std::vector<int>({12, 13, 14, 15, 16, 17, 13, 14, 15}));
    SEQLIB_CHECK(dropped == std::vector<int>({71}));
    SEQLIB_CHECK(released == std::vector<int>({14}));
    SEQLIB_CHECK(seq.GetSlot(15).state == kSlotOn);
}

// Track 0 plays 7 notes at once on a channel of 6 slots, and the driver drops one. Track 1 plays a note on another
// channel. The warning about dropped notes counts the tracks that the conversion keeps. Track 1 alone has none.
void TestDroppedWarning()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});
    Track crowded(kFormat);
    crowded.Tempo(500000).Program(2, 0);
    for (int k = 60; k < 67; k++)
    {
        crowded.On(2, k, 100);
    }
    crowded.Wait(48);
    cart.Tune({crowded.End(), Track(kFormat).Program(3, 0).On(3, 60, 100).Wait(48).Off(3, 60).End()}, bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("dropped");

    const SongReport all = ConvertSong(rom, info, 0, opt, nullptr);
    opt.track_mask = 1 << 1;
    const SongReport second = ConvertSong(rom, info, 0, opt, nullptr);

    auto warns = [](const SongReport& s)
    {
        return std::any_of(s.warnings.begin(), s.warnings.end(),
                           [](const std::string& w) { return w.find("found no available slot") != std::string::npos; });
    };

    SEQLIB_CHECK(all.ok && second.ok && warns(all) && !warns(second));
}

void TestEnvelope()
{
    // An instant attack and decay, full sustain, and a release of 5 frames (index 95), which falls in a straight line
    // in level: 0x80000 less 3 << 4 of the division's remainder, then 104848 a frame.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const uint32_t sample = cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 95});
    const auto bank = cart.Bank({{0, sample}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(),
               Track(kFormat).Program(1, 0).On(1, 60, 100).Wait(80).Off(1, 60).Wait(480).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    std::vector<std::pair<int, int32_t>> levels;
    for (int frame = 0; frame < 12; frame++)
    {
        seq.Step();
        const Slot& s = seq.GetSlot(6);
        levels.push_back({s.state, s.level});
    }

    // 80 ticks are 5 frames: the note off comes on frame 5.
    SEQLIB_CHECK_EQ(levels[0].second, kFullLevel);
    SEQLIB_CHECK_EQ(levels[4].first, kSlotOn);
    SEQLIB_CHECK_EQ(levels[5].first, kSlotReleased);
    SEQLIB_CHECK_EQ(levels[5].second, 419392);
    SEQLIB_CHECK_EQ(levels[6].second, 314544);
    SEQLIB_CHECK_EQ(levels[8].second, 104848);
    SEQLIB_CHECK_EQ(levels[9].first, kSlotFree);

    // The SoundFont envelope: the release stretched to 3 times its 5 frames, and the rest at their defaults.
    Instrument inst;
    SEQLIB_CHECK(ReadInstrument(rom, sample, inst));

    const VolumeEnvelope env = EnvelopeFor(info, inst);

    SEQLIB_CHECK_EQ(env.attack, -12000);
    SEQLIB_CHECK_EQ(env.decay, -12000);
    SEQLIB_CHECK_EQ(env.sustain, 0);
    SEQLIB_CHECK_EQ(env.release, int(std::lround(1200 * std::log2(15 / kFrameRate))));

    // A decay to silence of 70 frames (index 30) is stretched to 6 times as long. One to a sustain of 40, a level of
    // 51 + 183/256 out of 128, reaches it in the decay's time.
    inst.decay = 30;
    inst.sustain = 0;

    SEQLIB_CHECK_EQ(EnvelopeFor(info, inst).decay, int(std::lround(1200 * std::log2(6 * 70 / kFrameRate))));
    SEQLIB_CHECK_EQ(EnvelopeFor(info, inst).sustain, 1440);

    inst.sustain = 40;

    const VolumeEnvelope sustained = EnvelopeFor(info, inst);

    const double kLevel = (51 * 256 + 183) / 32768.0;
    SEQLIB_CHECK_EQ(sustained.sustain, int(std::lround(-200 * std::log10(kLevel))));
    SEQLIB_CHECK_EQ(sustained.decay, int(std::lround(1200 * std::log2(1000.0 / sustained.sustain * 70 / kFrameRate))));
}

// The levels of channel 1's first slot on each of the first 40 frames of tune 0, and the settings that each phase of
// its envelopes took.
struct EnvelopeRun
{
    std::vector<int32_t> levels; // 0 where the slot is free
    std::vector<EnvelopeSettings> took;
};

EnvelopeRun RunEnvelope(const Rom& rom, const DriverInfo& info)
{
    EnvelopeRun run;
    Sequencer seq(rom, info, 0);
    for (int frame = 0; frame < 40; frame++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == Action::kEnvelope && a.slot == 6)
            {
                run.took.push_back(a.envelope);
            }
        }

        const Slot& s = seq.GetSlot(6);
        run.levels.push_back(s.state == kSlotFree ? 0 : s.level);
    }

    return run;
}

// In the revision of Donkey Kong Country 3, controllers 20 to 23 give a channel's notes an attack of 3 steps, a decay
// and a release whose fade table entries are 4 and 2, and a sustain level of 64/128, in place of their instrument's
// instant attack, decay and release at the full level. A program change takes the settings away again. Older revisions
// ignore the controllers.
void TestEnvelopeSettings()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(), Track(kFormat)
                                                                 .Program(1, 0)
                                                                 .Control(1, kCtrlAttack, 3)
                                                                 .Control(1, kCtrlDecay, 4)
                                                                 .Control(1, kCtrlSustain, 64)
                                                                 .Control(1, kCtrlRelease, 2)
                                                                 .On(1, 60, 100)
                                                                 .Wait(160)
                                                                 .Off(1, 60)
                                                                 .Wait(160)
                                                                 .Program(1, 0)
                                                                 .On(1, 62, 100)
                                                                 .Wait(160)
                                                                 .Off(1, 62)
                                                                 .Wait(160)
                                                                 .End()},
              bank);
    const Rom older_rom = cart.ToRom();
    cart.AddEnvelopeControllers();
    const Rom rom = cart.ToRom();

    const EnvelopeRun older = RunEnvelope(older_rom, Detect(older_rom));
    const EnvelopeRun newer = RunEnvelope(rom, Detect(rom));

    // The older revision plays both notes at the full level from their first frame, and stops each at its note off on
    // frames 10 and 30.
    SEQLIB_CHECK_EQ(older.levels[0], kFullLevel);
    SEQLIB_CHECK_EQ(older.levels[9], kFullLevel);
    SEQLIB_CHECK_EQ(older.levels[10], 0);
    SEQLIB_CHECK_EQ(older.levels[20], kFullLevel);
    SEQLIB_CHECK(older.took.empty());

    // The first note rises over 4 frames, falls to the sustain level over 5, and releases over 3 from frame 10.
    const std::vector<int32_t> kAttack = {0x20000, 0x40000, 0x60000, kFullLevel, 471808, 419392};
    SEQLIB_CHECK(std::vector<int32_t>(newer.levels.begin(), newer.levels.begin() + 6) == kAttack);
    SEQLIB_CHECK_EQ(newer.levels[8], 0x40000);
    SEQLIB_CHECK_EQ(newer.levels[10], 174752);
    SEQLIB_CHECK_EQ(newer.levels[11], 87376);
    SEQLIB_CHECK_EQ(newer.levels[12], 0);

    // The second note plays as in the older revision.
    SEQLIB_CHECK_EQ(newer.levels[20], kFullLevel);
    SEQLIB_CHECK_EQ(newer.levels[30], 0);

    // The phases of the first note took the settings, and those of the second none.
    SEQLIB_REQUIRE_EQ(newer.took.size(), 3);
    SEQLIB_CHECK(newer.took[0] == EnvelopeSettings{.attack = 3});
    SEQLIB_CHECK(newer.took[1] == (EnvelopeSettings{.decay = 4, .sustain = 64}));
    SEQLIB_CHECK(newer.took[2] == EnvelopeSettings{.release = 2});
}

// Eight looping notes on channels 0 and 1 fill the mixer, so it doesn't mix the short sample without a loop that
// channel 2 plays, and that sample ends where the notes are moved on. The revision of Donkey Kong Country 3 frees its
// slot there, and the older revisions leave it in use until the mixer takes the voice.
void TestUnmixedEnd()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}, {1, cart.Sample(Saw(), 0)}});
    Track loops(kFormat);
    loops.Tempo(500000).Program(0, 0).Program(1, 0);
    for (int k = 60; k < 66; k++)
    {
        loops.On(0, k, 100);
    }
    loops.On(1, 60, 100).On(1, 61, 100).Wait(480);
    cart.Tune({loops.End(), Track(kFormat).Program(2, 1).On(2, 60, 100).Wait(480).End()}, bank);
    const Rom older_rom = cart.ToRom();
    cart.AddEnvelopeControllers();
    const Rom rom = cart.ToRom();

    auto state_after = [](const Rom& r)
    {
        const DriverInfo info = Detect(r);
        Sequencer seq(r, info, 0);
        for (int frame = 0; frame < 3; frame++)
        {
            seq.Step();
        }

        return seq.GetSlot(12).state;
    };

    SEQLIB_CHECK_EQ(state_after(older_rom), kSlotOn);
    SEQLIB_CHECK_EQ(state_after(rom), kSlotFree);
}

// Nine notes play at once on channels 0 and 1. The mixer takes the eight in the first slots and leaves out the ninth.
// Once the first note is released, the mixer takes the eight that are on and leaves out the released one. Channel 0
// then turns its volume to 0. In Donkey Kong Country 3's revision, its notes are then at a level of 0 at the game's
// music volume. The mixer leaves them out, and they don't count towards the limit.
void TestVoiceLimit()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});
    Track notes(kFormat);
    notes.Program(0, 0).Program(1, 0);
    for (int k = 60; k < 66; k++)
    {
        notes.On(0, k, 100);
    }
    notes.On(1, 70, 100).On(1, 71, 100).On(1, 72, 100).Wait(48).Off(0, 60).Control(0, kCtrlVolume, 0).Wait(48);
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(), notes.End()}, bank);
    const Rom older_rom = cart.ToRom();
    cart.AddEnvelopeControllers();
    cart.AddLevelTest();
    const Rom rom = cart.ToRom();

    // What the mixer does with slots 0, 1 and 6-8 in the first frame and in the frame of the note off.
    auto results = [](const Rom& r)
    {
        const DriverInfo info = Detect(r);
        Sequencer seq(r, info, 0);
        std::vector<MixResult> out;
        for (int frame = 0; frame < 10; frame++)
        {
            const std::vector<Action>& actions = seq.Step();
            const bool released =
                std::any_of(actions.begin(), actions.end(), [](const Action& a) { return a.kind == Action::kNoteOff; });
            if (frame == 0 || released)
            {
                for (int slot : {0, 1, 6, 7, 8})
                {
                    out.push_back(seq.GetSlot(slot).mix);
                }
            }
        }

        return out;
    };

    using M = MixResult;
    const std::vector<MixResult> kOlder = {M::kMixed,   M::kMixed, M::kMixed, M::kMixed, M::kSkipped,
                                           M::kSkipped, M::kMixed, M::kMixed, M::kMixed, M::kMixed};
    const std::vector<MixResult> kLevels = {M::kMixed,  M::kMixed,  M::kMixed, M::kMixed, M::kSkipped,
                                            M::kSilent, M::kSilent, M::kMixed, M::kMixed, M::kMixed};
    SEQLIB_CHECK(results(older_rom) == kOlder);
    SEQLIB_CHECK(results(rom) == kLevels);
}

void TestPitch()
{
    // A bend of 0x1000 with a range of 2 semitones is a semitone. Vibrato adds (modulation + 1) times the sine times
    // the instrument's depth.
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const uint32_t inst = cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 99}, 0, 2, 0x111111, 400);
    const auto bank = cart.Bank({{0, inst}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(), Track(kFormat)
                                                                 .Program(1, 0)
                                                                 .Bend(1, 0x3000)
                                                                 .On(1, 60, 100)
                                                                 .Wait(64)
                                                                 .Bend(1, 0x2000)
                                                                 .Control(1, kCtrlModulation, 10)
                                                                 .Wait(480)
                                                                 .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    Sequencer seq(rom, info, 0);
    seq.Step();

    SEQLIB_CHECK_EQ(seq.PitchOffset(1), int64_t(1) << 32);

    // 64 ticks are 4 frames. On that frame the vibrato runs for the first time, to phase 0x111111: index 0x11 of the
    // sine table, and 0x1111 of the way to the next entry.
    for (int frame = 1; frame <= 4; frame++)
    {
        seq.Step();
    }

    const int32_t a = rom.S32(kSineTable + 0x11 * 4), b = rom.S32(kSineTable + 0x12 * 4);
    const int32_t sine = a + int32_t((int64_t(b - a) * 0x1111) >> 16);
    SEQLIB_CHECK_EQ(seq.Channel(1).vibrato_phase, 0x111111);
    SEQLIB_CHECK_EQ(seq.PitchOffset(1), int64_t(11 * sine) * 400);
}

void TestLoops()
{
    // A track loops between controllers 102 and 103. The conversion plays the loop twice, and marks it.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Wait(480)
                   .Control(0, kCtrlLoopStart, 0)
                   .Wait(960)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 60, 100)
                   .Wait(480)
                   .Off(1, 60)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loops");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    const auto notes = m.Find(0x91);
    SEQLIB_REQUIRE_EQ(notes.size(), 2);
    SEQLIB_CHECK_EQ(notes[0].tick, 480);
    SEQLIB_CHECK_EQ(notes[1].tick, 1440);

    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    SEQLIB_CHECK(markers.size() == 2 && markers[0] == std::make_pair(480u, std::string("loopStart")) &&
                 markers[1] == std::make_pair(1440u, std::string("loopEnd")));

    // The loop starts at 0.5 s and ends at 1.5 s of song time, which the GBA plays a little slower.
    SEQLIB_CHECK(std::fabs(r.loop_start - 0.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 1.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 2.5 * kTempoScale) < 1e-9);
}

// A track bends back to the centre where its loop starts, where it already is the first time through, and bends up
// later in the loop. A player that jumps back to the loop's start comes from the bend up, as the game does, so the
// centre is written again there.
void TestLoopBend()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Wait(480)
                   .Control(0, kCtrlLoopStart, 0)
                   .Wait(960)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .Bend(1, 0x2000)
                   .On(1, 60, 100)
                   .Wait(480)
                   .Off(1, 60)
                   .Bend(1, 0x3000)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("loop_bend");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    using Events = std::vector<seqlib::midi::Event>;
    const Events kBends = {{0, {0xE1, 0x00, 0x40}},
                           {480, {0xE1, 0x00, 0x40}},
                           {960, {0xE1, 0x00, 0x60}},
                           {1440, {0xE1, 0x00, 0x40}},
                           {1920, {0xE1, 0x00, 0x60}}};
    SEQLIB_CHECK(r.ok);
    SEQLIB_CHECK(ReadMidi(r.midi_path).Find(0xE1) == kBends);
}

void TestLoopStarts()
{
    // The first track loops 480 ticks from the start, the second 960 ticks from tick 480, and the third 240 ticks from
    // tick 240. Every track is looping from tick 480. The loop goes from there for the 960 ticks that each track's loop
    // divides, and the tune plays it twice. The middle track's loop starts last and is the longest. Neither the first
    // track's loop nor the last track's loop gives the same times.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(0, 5)
                   .Control(0, kCtrlLoopStart, 0)
                   .On(0, 60, 100)
                   .Wait(240)
                   .Off(0, 60)
                   .Wait(240)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 64, 100)
                   .Wait(480)
                   .Off(1, 64)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(2, 5)
                   .Wait(240)
                   .Control(2, kCtrlLoopStart, 0)
                   .On(2, 67, 100)
                   .Wait(120)
                   .Off(2, 67)
                   .Wait(120)
                   .Control(2, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_starts");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop goes from 0.5 s to 1.5 s of song time, which the GBA plays a little slower, and the tune ends at 2.5 s.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(std::fabs(r.loop_start - 0.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 1.5 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 2.5 * kTempoScale) < 1e-9);
}

void TestLoopLengths()
{
    // One track loops 720 ticks and the other 960, both from the start. The loop lasts until both are back where they
    // started, 2880 ticks or 3 s of song time, and the tune plays it twice.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(0, 5)
                   .Control(0, kCtrlLoopStart, 0)
                   .On(0, 60, 100)
                   .Wait(360)
                   .Off(0, 60)
                   .Wait(360)
                   .Control(0, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 64, 100)
                   .Wait(480)
                   .Off(1, 64)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_lengths");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The GBA plays song time a little slower.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(std::fabs(r.loop_start) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 3 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 6 * kTempoScale) < 1e-9);
}

void TestTrackWithoutLoop()
{
    // In a looping tune, the loop starts no earlier than the last command of a track without a loop, and a track that
    // ends in a long delay doesn't lengthen the tune. Track 0 sets the tempo and waits 9600 ticks, track 1 loops 960
    // ticks from the start, and track 2 plays a note from tick 480 to tick 960 and ends. The loop goes from tick 960 to
    // tick 1920, and the tune ends at tick 2880.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(9600).End(),
               Track(kFormat)
                   .Program(1, 5)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 60, 100)
                   .Wait(480)
                   .Off(1, 60)
                   .Wait(480)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End(),
               Track(kFormat).Program(2, 5).Wait(480).On(2, 64, 100).Wait(480).Off(2, 64).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("without_loop");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // At 960 ticks a second of song time, which the GBA plays a little slower.
    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK(std::fabs(r.loop_start - 1.0 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.loop_end - 2.0 * kTempoScale) < 1e-9);
    SEQLIB_CHECK(std::fabs(r.seconds - 3.0 * kTempoScale) < 1e-9);
}

void TestHourLimit()
{
    // A tune asked to play its loop of 50 s a hundred times stops where the model gives up, after 216000 frames
    // (60:16.42). The loop starts half a second in, so the limit falls in the middle of a note: the tune's length, its
    // MIDI file and that note all end there.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat)
                   .Tempo(500000)
                   .Program(1, 5)
                   .Wait(480)
                   .Control(1, kCtrlLoopStart, 0)
                   .On(1, 60, 100)
                   .Wait(48000)
                   .Off(1, 60)
                   .Control(1, kCtrlLoopEnd, 0)
                   .End()},
              bank);
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
    SEQLIB_CHECK(r.seconds > 3610 && r.seconds < 3620);

    const MidiEvents m = ReadMidi(r.midi_path);
    uint64_t last = 0;
    for (const auto& track : m.tracks)
    {
        for (const auto& e : track.events)
        {
            last = std::max(last, e.tick);
        }
    }
    const auto offs = m.Find(0x81);
    SEQLIB_CHECK(last / 960.0 * kTempoScale < 3620);
    SEQLIB_CHECK(!offs.empty() && offs.back().tick / 960.0 * kTempoScale > 3610);
}

void TestFrameTiming()
{
    // With frame timing, each event goes at the start of the frame the driver plays it in. Track 1 loops 1000 ticks,
    // 62.5 frames, from tick 0: a note at the loop's start, and a grace note of the same key a tick before its end,
    // which the driver releases on the frame it starts on. The grace note and the second pass's first note come on
    // frame 62, which starts at tick 991, and the third pass's first note on frame 125, which starts at tick 1999.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(9600).End(), Track(kFormat)
                                                                  .Program(1, 5)
                                                                  .Control(1, kCtrlLoopStart, 0)
                                                                  .On(1, 60, 100)
                                                                  .Wait(500)
                                                                  .Off(1, 60)
                                                                  .Wait(499)
                                                                  .On(1, 60, 50)
                                                                  .Wait(1)
                                                                  .Off(1, 60)
                                                                  .Control(1, kCtrlLoopEnd, 0)
                                                                  .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("frame_timing");
    opt.frame_timing = true;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The loop ends at the start of frame 62, so that it holds the first pass's first note but not the second's, nor
    // the grace note on the same frame. The grace note lasts a tick, and the note after it starts as it ends. The
    // conversion ends after the second pass, so the third pass's first note is left out.
    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<std::pair<uint32_t, std::string>> markers;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x06)
        {
            markers.push_back({uint32_t(e.tick), std::string(e.bytes.begin() + 3, e.bytes.end())});
        }
    }
    SEQLIB_CHECK(markers.size() == 2 && markers[0] == std::make_pair(0u, std::string("loopStart")) &&
                 markers[1] == std::make_pair(991u, std::string("loopEnd")));

    std::vector<std::pair<uint32_t, int>> ons, offs;
    for (const auto& e : m.Find(0x91))
    {
        ons.push_back({uint32_t(e.tick), e.bytes[2]});
    }
    for (const auto& e : m.Find(0x81))
    {
        offs.push_back({uint32_t(e.tick), e.bytes[1]});
    }
    // A velocity of v is written as 127 × √(v / 127), rounded: 100 as 113 and 50 as 80.
    const std::vector<std::pair<uint32_t, int>> kOns = {{0, 113}, {991, 80}, {992, 113}, {1983, 80}};
    const std::vector<std::pair<uint32_t, int>> kOffs = {{495, 60}, {992, 60}, {1487, 60}, {1999, 60}};
    SEQLIB_CHECK(ons == kOns);
    SEQLIB_CHECK(offs == kOffs);
}

void TestConversion()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const uint32_t lead = cart.Sample(Saw(), 4, 11025, 57, {50, 60, 40, 95}, 150, 2);
    const uint32_t low = cart.Sample(Saw(100), 0, 8000, 48);
    const uint32_t high = cart.Sample(Saw(80), 40, 16000, 72);
    cart.Put32(high + 4, kLoopForwardB);
    const uint32_t split = cart.Split(kInstKeySplit, {{{0, 59}, low}, {{60, 127}, high}});
    const uint32_t kick = cart.Sample(Saw(50), 0, 11025, 36);
    const uint32_t kit = cart.Split(kInstDrumKit, {{{35, 36}, kick}});
    const auto bank = cart.Bank({{1, lead}, {2, split}, {127, kit}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(),
               Track(kFormat)
                   .Program(0, 1)
                   .Control(0, kCtrlVolume, 100)
                   .On(0, 60, 64)
                   .Bend(0, 0x3000)
                   .Wait(240)
                   .Off(0, 60)
                   .On(0, 64, 30)
                   .On(0, 64, 40)
                   .Wait(240)
                   .Off(0, 64)
                   .Off(0, 64)
                   .End(),
               Track(kFormat).Program(4, 2).On(4, 50, 100).On(4, 70, 100).Wait(240).Off(4, 50).Off(4, 70).End(),
               Track(kFormat).On(9, 36, 127).Program(9, 127).Wait(240).Off(9, 36).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("convert");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    SEQLIB_CHECK_EQ(r.tracks, 3);
    SEQLIB_CHECK(std::fabs(r.bpm - 120 / kTempoScale) < 1e-6);
    SEQLIB_CHECK(PathFromUtf8(r.midi_path).filename() == "convert_00.mid");

    // The tempo is scaled to the GBA's speed, and the notes keep their ticks.
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK_EQ(m.division, 480);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 4);
    bool tempo = false;
    for (const auto& e : m.tracks[0].events)
    {
        if (e.bytes[0] == 0xFF && e.bytes[1] == 0x51)
        {
            tempo = ((e.bytes[3] << 16) | (e.bytes[4] << 8) | e.bytes[5]) == std::lround(500000 * kTempoScale);
        }
    }
    SEQLIB_CHECK(tempo);

    // Key 64 played twice at once is one note. The note is as loud as both: (30 + 1) + (40 + 1) - 1 = 71. A velocity
    // of v is written as 127 × √(v / 127), rounded: 64 as 90 and 71 as 95.
    const auto ch0 = m.Find(0x90);
    SEQLIB_REQUIRE_EQ(ch0.size(), 2);
    SEQLIB_CHECK(ch0[0].tick == 0 && ch0[0].bytes[1] == 60 && ch0[0].bytes[2] == 90);
    SEQLIB_CHECK(ch0[1].tick == 240 && ch0[1].bytes[1] == 64 && ch0[1].bytes[2] == 95);

    const auto offs = m.Find(0x80);
    SEQLIB_CHECK(offs.size() == 2 && offs[1].tick == 480);

    // The bend range is set to the instrument's 2 semitones, so the bend keeps its value.
    const auto bends = m.Find(0xE0);
    SEQLIB_CHECK(!bends.empty() && bends.back().bytes[1] == 0x00 && bends.back().bytes[2] == 0x60);
    bool range = false;
    for (const auto& e : m.Find(0xB0))
    {
        range = range || (e.bytes[1] == 6 && e.bytes[2] == 2);
    }
    SEQLIB_CHECK(range);

    // The channel's volume of 100 is written in controller 7 as 127 × √(100 / 127), rounded: 113.
    std::vector<int> volumes;
    for (const auto& e : m.Find(0xB0))
    {
        if (e.bytes[1] == 7)
        {
            volumes.push_back(e.bytes[2]);
        }
    }
    SEQLIB_CHECK(volumes == std::vector<int>({113}));

    // The drum channel's note comes before its program change, at the same tick, and gets that program, so the MIDI
    // file puts the change first.
    const auto drums = m.Find(0x99);
    SEQLIB_CHECK(drums.size() == 1 && drums[0].bytes[1] == 36);
    int change = -1, note = -1;
    for (const auto& t : m.tracks)
    {
        for (size_t i = 0; i < t.events.size(); i++)
        {
            if (change < 0 && t.events[i].bytes[0] == 0xC9 && t.events[i].bytes[1] == 127)
            {
                change = int(i);
            }
            if (note < 0 && t.events[i].bytes[0] == 0x99)
            {
                note = int(i);
            }
        }
    }
    SEQLIB_CHECK(change >= 0 && change < note);

    // The SoundFont: a preset for each program, and the drum channel's in bank 128 too.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<std::pair<int, int>, int> presets;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        presets[{Le16(h + 22), Le16(h + 20)}] =
            Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
    }
    SEQLIB_CHECK(presets.count({0, 1}) && presets.count({0, 2}) && presets.count({0, 127}) &&
                 presets.count({128, 127}));
    SEQLIB_REQUIRE_EQ(presets.size(), 4);

    // Each instrument's global zone has seqlib's four modulators.
    SEQLIB_CHECK(HasGlobalZoneModulators(sf));
    SEQLIB_CHECK_EQ(sf.Count("imod", 10), 3 * 4 + 1);

    // The lead's zone has its envelope and fine tune. The key split's zones split at key 60. Its high sample loops in
    // loop mode 4. The drum kit's zone plays at its sample's pitch whatever the key.
    auto zone_of = [&](int instrument, int index)
    {
        return sf.ZoneGens(Le16(sf.Record("inst", 22, size_t(instrument)) + 20) + size_t(index));
    };

    const auto lead_zone = zone_of(presets[{0, 1}], 1);
    SEQLIB_CHECK(lead_zone.count(sf2::kAttackVolEnv) && lead_zone.count(sf2::kDecayVolEnv) &&
                 lead_zone.count(sf2::kSustainVolEnv) && lead_zone.count(sf2::kReleaseVolEnv));
    SEQLIB_CHECK_EQ(int16_t(lead_zone.at(sf2::kCoarseTune)), 1);
    SEQLIB_CHECK_EQ(int16_t(lead_zone.at(sf2::kFineTune)), 50);
    SEQLIB_CHECK_EQ(lead_zone.at(sf2::kSampleModes), 1);
    SEQLIB_CHECK_EQ(zone_of(presets[{0, 2}], 1).at(sf2::kKeyRange), 0x3B00);
    SEQLIB_CHECK_EQ(zone_of(presets[{0, 2}], 2).at(sf2::kKeyRange), 0x7F3C);
    SEQLIB_CHECK_EQ(zone_of(presets[{0, 2}], 2).at(sf2::kSampleModes), 1);
    const auto drum_zone = zone_of(presets[{0, 127}], 1);
    SEQLIB_CHECK_EQ(drum_zone.at(sf2::kKeyRange), 0x2423);
    SEQLIB_CHECK_EQ(drum_zone.at(sf2::kScaleTuning), 0);

    // The lead's loop of 4 bytes is repeated to 32, and followed by 8 points that repeat its start.
    int leads = 0;
    for (size_t s = 0; s + 1 < sf.Count("shdr", 46); s++)
    {
        const uint8_t* h = sf.Record("shdr", 46, s);
        if (Le32(h + 36) == 11025 && h[40] == 57)
        {
            leads++;
            SEQLIB_CHECK_EQ(Le32(h + 32) - Le32(h + 28), 32);
            SEQLIB_CHECK_EQ(Le32(h + 24) - Le32(h + 32), 8);
        }
    }
    SEQLIB_CHECK_EQ(leads, 1);
}

// Track 1 changes the tempo at tick 98, and then track 0 at tick 97, a frame later: track 0 gave the first tempo, and
// plays each tick a frame after the other tracks. The driver keeps the tempo it set last. So track 0's tempo plays from
// tick 98 on, in the length and in the MIDI file.
void TestTwoTempoTracks()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{2, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(97).Tempo(250000).Wait(383).End(),
               Track(kFormat).Program(0, 2).On(0, 60, 100).Wait(98).Tempo(1000000).Wait(382).Off(0, 60).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    for (bool frame_timing : {false, true})
    {
        ConvertSettings opt = TempSettings(frame_timing ? "two_tempos_frames" : "two_tempos");
        opt.frame_timing = frame_timing;

        const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

        SEQLIB_CHECK(r.ok && !r.silent);
        SEQLIB_CHECK(std::fabs(r.seconds - (98 * 0.5 + 382 * 0.25) / 480 * kTempoScale) < 1e-9);
        std::vector<std::pair<uint32_t, long>> tempos;
        for (const auto& e : ReadMidi(r.midi_path).Find(0xFF))
        {
            if (e.bytes[1] == 0x51)
            {
                tempos.push_back({uint32_t(e.tick), (e.bytes[3] << 16) | (e.bytes[4] << 8) | e.bytes[5]});
            }
        }
        const std::vector<std::pair<uint32_t, long>> kTempos = {{0, std::lround(500000 * kTempoScale)},
                                                                {98, std::lround(1000000 * kTempoScale)},
                                                                {98, std::lround(250000 * kTempoScale)}};
        SEQLIB_CHECK(tempos == kTempos);
    }
}

// In the revision of Donkey Kong Country 3, channel 1's first note plays with settings from controllers 20 to 23, and
// its second, after a program change, without them. The first note's preset is in a bank of its own, which a bank
// select gives before its program change, with an instrument whose envelope has the settings, and the second note's
// bank select goes back to bank 0.
void TestEnvelopeConversion()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    cart.AddEnvelopeControllers();
    const uint32_t sample = cart.Sample(Saw(), 32);
    const auto bank = cart.Bank({{5, sample}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(), Track(kFormat)
                                                                 .Program(1, 5)
                                                                 .Control(1, kCtrlAttack, 3)
                                                                 .Control(1, kCtrlDecay, 4)
                                                                 .Control(1, kCtrlSustain, 64)
                                                                 .Control(1, kCtrlRelease, 2)
                                                                 .On(1, 60, 100)
                                                                 .Wait(160)
                                                                 .Off(1, 60)
                                                                 .Wait(160)
                                                                 .Program(1, 5)
                                                                 .On(1, 62, 100)
                                                                 .Wait(160)
                                                                 .Off(1, 62)
                                                                 .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("envelope");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // Bank 1 from the first note's tick, and bank 0 from the second's.
    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    std::vector<std::pair<uint32_t, int>> banks;
    for (const auto& e : m.Find(0xB1))
    {
        if (e.bytes[1] == 0)
        {
            banks.push_back({uint32_t(e.tick), e.bytes[2]});
        }
    }
    const std::vector<std::pair<uint32_t, int>> kBanks = {{0, 1}, {320, 0}};
    SEQLIB_CHECK(banks == kBanks);

    // The SoundFont has program 5 in banks 0 and 1. Bank 1's instrument is named after the settings, and its envelope
    // has them.
    const Sf2Records sf = ReadSf2(r.sf2_path);
    std::map<std::pair<int, int>, int> presets;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        presets[{Le16(h + 22), Le16(h + 20)}] =
            Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2);
    }
    SEQLIB_CHECK(presets.size() == 2 && presets.count({0, 5}) && presets.count({1, 5}));

    const uint8_t* variant = sf.Record("inst", 22, size_t(presets[{1, 5}]));
    char name[32];
    std::snprintf(name, sizeof name, "%08X a3d4s64r2", unsigned(sample));
    SEQLIB_CHECK(std::string(reinterpret_cast<const char*>(variant)) == name);

    Instrument inst;
    ReadInstrument(rom, sample, inst);
    const VolumeEnvelope want = EnvelopeFor(info, inst, {.attack = 3, .decay = 4, .sustain = 64, .release = 2});
    const auto zone = sf.ZoneGens(Le16(variant + 20) + 1);
    SEQLIB_CHECK_EQ(int16_t(zone.at(sf2::kAttackVolEnv)), want.attack);
    SEQLIB_CHECK_EQ(int16_t(zone.at(sf2::kDecayVolEnv)), want.decay);
    SEQLIB_CHECK_EQ(int16_t(zone.at(sf2::kSustainVolEnv)), want.sustain);
    SEQLIB_CHECK_EQ(int16_t(zone.at(sf2::kReleaseVolEnv)), want.release);

    // That envelope: an attack of 4 frames, a sustain at half the level, which the decay reaches in its 5 frames, and a
    // release of 3 frames stretched to 3 times as long.
    SEQLIB_CHECK_EQ(want.attack, int(std::lround(1200 * std::log2(4 / kFrameRate))));
    SEQLIB_CHECK_EQ(want.sustain, 60);
    SEQLIB_CHECK_EQ(want.decay, int(std::lround(1200 * std::log2(1000.0 / 60 * 5 / kFrameRate))));
    SEQLIB_CHECK_EQ(want.release, int(std::lround(1200 * std::log2(9 / kFrameRate))));

    // Bank 0's instrument has the instrument's instant envelope.
    const auto plain = sf.ZoneGens(Le16(sf.Record("inst", 22, size_t(presets[{0, 5}])) + 20) + 1);
    SEQLIB_CHECK(!plain.count(sf2::kAttackVolEnv) && !plain.count(sf2::kReleaseVolEnv));
}

// A note that plays without settings ends before the tune's end, where its slot's next note starts with an attack from
// controller 20. That note isn't converted, and with frame timing the settings it takes don't go to the note before it
// either, so both conversions have only bank 0.
void TestEnvelopePastEnd()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    cart.AddEnvelopeControllers();
    const auto bank = cart.Bank({{5, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(96).End(), Track(kFormat)
                                                                .Program(0, 5)
                                                                .On(0, 60, 100)
                                                                .Wait(64)
                                                                .Off(0, 60)
                                                                .Wait(32)
                                                                .Control(0, kCtrlAttack, 3)
                                                                .On(0, 62, 100)
                                                                .End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    for (bool frame_timing : {false, true})
    {
        ConvertSettings opt = TempSettings(frame_timing ? "past_end_frames" : "past_end");
        opt.frame_timing = frame_timing;

        const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

        SEQLIB_CHECK(r.ok && !r.silent);
        const Sf2Records sf = ReadSf2(r.sf2_path);
        SEQLIB_CHECK_EQ(sf.Count("phdr", 38), 2);
        SEQLIB_CHECK_EQ(Le16(sf.Record("phdr", 38, 0) + 22), 0);
        SEQLIB_REQUIRE_EQ(ReadMidi(r.midi_path).Find(0x90).size(), 1);
    }
}

// A note still playing when the tune ends is released where the MIDI file ends. Controller 23 sets its channel's
// release after the note starts, and the note takes that release at the end. So its preset is in a bank of its own with
// that release.
void TestEnvelopeAtEnd()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    cart.AddEnvelopeControllers();
    const uint32_t sample = cart.Sample(Saw(), 32);
    const auto bank = cart.Bank({{5, sample}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(320).End(),
               Track(kFormat).Program(1, 5).On(1, 60, 100).Wait(160).Control(1, kCtrlRelease, 2).Wait(160).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("envelope_at_end");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const Sf2Records sf = ReadSf2(r.sf2_path);
    const uint8_t* variant = nullptr;
    for (size_t p = 0; p + 1 < sf.Count("phdr", 38); p++)
    {
        const uint8_t* h = sf.Record("phdr", 38, p);
        if (Le16(h + 22) == 1 && Le16(h + 20) == 5)
        {
            variant = sf.Record("inst", 22, Le16(sf.Record("pgen", 4, Le16(sf.Record("pbag", 4, Le16(h + 24)))) + 2));
        }
    }
    SEQLIB_CHECK(variant != nullptr);
    if (variant)
    {
        char name[32];
        std::snprintf(name, sizeof name, "%08X r2", unsigned(sample));
        SEQLIB_CHECK(std::string(reinterpret_cast<const char*>(variant)) == name);
        Instrument inst;
        ReadInstrument(rom, sample, inst);
        SEQLIB_CHECK_EQ(int16_t(sf.ZoneGens(Le16(variant + 20) + 1).at(sf2::kReleaseVolEnv)),
                        EnvelopeFor(info, inst, {.release = 2}).release);
    }
}

// The banks for envelope settings come after those of the tunes' presets, one for each bank and set of settings, and
// leave out the drum bank.
void TestEnvelopeBanks()
{
    Cart cart(Format::kChannelNibble);
    const Rom rom = cart.ToRom();
    const DriverInfo info;
    SoundfontBuilder sf(rom, info, 3);
    SoundfontBuilder high(rom, info, kDrumBank);

    SEQLIB_CHECK_EQ(sf.BankFor(1, EnvelopeSettings()), 1);
    SEQLIB_CHECK_EQ(sf.BankFor(0, {.attack = 1}), 3);
    SEQLIB_CHECK_EQ(sf.BankFor(1, {.attack = 1}), 4);
    SEQLIB_CHECK_EQ(sf.BankFor(0, {.decay = 2}), 5);
    SEQLIB_CHECK_EQ(sf.BankFor(0, {.attack = 1}), 3);
    SEQLIB_CHECK_EQ(sf.BankFor(0, {.attack = 0x80}), 0);
    SEQLIB_CHECK_EQ(high.BankFor(0, {.release = 0}), kDrumBank + 1);
}

// ConvertSongFiles gives the bytes of the files that ConvertSong writes, and names no paths. A tune without notes gives
// no bytes.
void TestSongFiles()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{1, cart.Sample(Saw(), 4, 11025, 57, {50, 60, 40, 95})}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(0, 1).On(0, 60, 100).Wait(240).Off(0, 60).Wait(240).End()}, bank);
    cart.Tune({Track(kFormat).Tempo(500000).Wait(240).End()}, bank);
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
    SEQLIB_CHECK(none.report.silent && none.midi.empty() && none.sf2.empty());
}

// A program's instrument plays every key. A key split gives each key the instrument of its range, and a drum kit plays
// its instrument for the key at that instrument's root key. A program that the map doesn't have reads entry 0xFF of the
// instrument table. The tune table holds one tune, which is the one song.
void TestLookUpInstrument()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const uint32_t lead = cart.Sample(Saw(), 4, 11025, 57);
    const uint32_t low = cart.Sample(Saw(100), 0, 8000, 48);
    const uint32_t high = cart.Sample(Saw(80), 40, 16000, 72);
    const uint32_t split = cart.Split(kInstKeySplit, {{{0, 59}, low}, {{60, 127}, high}});
    const uint32_t kick = cart.Sample(Saw(50), 0, 11025, 36);
    const uint32_t kit = cart.Split(kInstDrumKit, {{{35, 36}, kick}});
    const auto bank = cart.Bank({{1, lead}, {2, split}, {127, kit}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(0, 1).On(0, 60, 100).Wait(240).Off(0, 60).End()}, bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    TuneHeader header;
    SEQLIB_REQUIRE(ReadTuneHeader(rom, TuneAddress(rom, info, 0), header));
    SEQLIB_CHECK_EQ(SongCount(info), 1);

    const auto lead_note = LookUpInstrument(rom, header, 1, 90);
    SEQLIB_CHECK(lead_note.mapped && lead_note.program == lead && lead_note.voice == lead && !lead_note.drum_kit);
    const auto low_note = LookUpInstrument(rom, header, 2, 59);
    const auto high_note = LookUpInstrument(rom, header, 2, 60);
    SEQLIB_CHECK(low_note.mapped && low_note.program == split && low_note.voice == low && !low_note.drum_kit);
    SEQLIB_CHECK(high_note.voice == high && !high_note.drum_kit);
    const auto drum = LookUpInstrument(rom, header, 127, 36);
    SEQLIB_CHECK(drum.mapped && drum.program == kit && drum.voice == kick && drum.drum_kit);
    const auto unmapped = LookUpInstrument(rom, header, 5, 60);
    SEQLIB_CHECK(!unmapped.mapped && unmapped.program == rom.U32(header.instruments + 4u * 0xFF));
}

// An instrument of a type the driver doesn't name plays as a drum kit, so its SoundFont instrument has a zone for its
// drum.
void TestOtherDrumKitType()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const uint32_t kick = cart.Sample(Saw(50), 0, 11025, 36);
    const uint32_t kit = cart.Split(0x24, {{{35, 36}, kick}});
    cart.Tune({Track(kFormat).End()}, cart.Bank({{1, kit}}));
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SoundfontBuilder sf(rom, info);

    const int instrument = sf.InstrumentFor(kit);

    SEQLIB_CHECK(instrument >= 0);
    SEQLIB_CHECK(instrument >= 0 && sf.File().instruments[size_t(instrument)].zones.size() == 2);
}

void TestPrograms()
{
    // Track 1 plays a note on channel 0 and then changes the channel's program on the same tick, so the driver plays
    // the note with the old program. Track 2 gives channel 1 a new program on the tick that track 3 plays a note on it,
    // and the driver runs track 2's commands first, so that note plays with the new program.
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{1, cart.Sample(Saw(), 32)}, {2, cart.Sample(Saw(), 32, 8000)}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(),
               Track(kFormat).Program(0, 1).Wait(240).On(0, 60, 100).Program(0, 2).Wait(240).Off(0, 60).End(),
               Track(kFormat).Program(1, 1).Wait(240).Program(1, 2).Wait(240).End(),
               Track(kFormat).Wait(240).On(1, 62, 100).Wait(240).Off(1, 62).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("programs");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // The file holds track 1's note on before its program change, and track 3's note after track 2's change.
    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 3);

    const auto statuses_at_240 = [&m](size_t track, uint8_t note_on, uint8_t program)
    {
        std::vector<uint8_t> statuses;
        for (const auto& e : m.tracks[track].events)
        {
            if (e.tick == 240 && (e.bytes[0] == note_on || e.bytes[0] == program))
            {
                statuses.push_back(e.bytes[0]);
            }
        }

        return statuses;
    };

    SEQLIB_CHECK(statuses_at_240(1, 0x90, 0xC0) == std::vector<uint8_t>({0x90, 0xC0}));
    SEQLIB_CHECK(statuses_at_240(2, 0x91, 0xC1) == std::vector<uint8_t>({0xC1, 0x91}));

    std::vector<std::pair<uint32_t, int>> channel1;
    for (const auto& e : m.Find(0xC1))
    {
        channel1.push_back({uint32_t(e.tick), e.bytes[1]});
    }
    const std::vector<std::pair<uint32_t, int>> kChannel1 = {{0, 1}, {240, 2}};
    SEQLIB_CHECK(channel1 == kChannel1);
}

// Two tunes with sets of instruments of their own share a SoundFont, and each plays program 127 on channel 10. The
// second also plays program 126 there. Players can take channel 10's programs from the drum bank. The first set's
// program 127 gets a copy there, and so does the second set's program 126. The drum bank has the first set's program
// 127 already. The second tune warns about it.
void TestSharedDrumBank()
{
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const uint32_t first_kick = cart.Sample(Saw(50), 0, 11025, 36);
    const uint32_t second_kick = cart.Sample(Saw(70), 0, 11025, 36);
    const auto first = cart.Bank({{127, first_kick}});
    const auto second = cart.Bank({{126, second_kick}, {127, second_kick}});
    cart.Tune({Track(kFormat).Tempo(500000).Wait(240).End(),
               Track(kFormat).Program(9, 127).On(9, 36, 100).Wait(240).Off(9, 36).End()},
              first);
    cart.Tune({Track(kFormat).Tempo(500000).Wait(240).End(), Track(kFormat)
                                                                 .Program(9, 126)
                                                                 .On(9, 36, 100)
                                                                 .Wait(120)
                                                                 .Off(9, 36)
                                                                 .Program(9, 127)
                                                                 .On(9, 36, 100)
                                                                 .Wait(120)
                                                                 .Off(9, 36)
                                                                 .End()},
              second);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SoundfontBuilder shared(rom, info, 2);
    ConvertSettings opt = TempSettings("drum_bank");

    const SongReport a = ConvertSong(rom, info, 0, opt, &shared, 0);
    const SongReport b = ConvertSong(rom, info, 1, opt, &shared, 1);

    std::map<std::pair<int, int>, int> presets;
    for (const sf2::Preset& p : shared.File().presets)
    {
        presets[{p.bank, p.program}] = PresetInstrument(p);
    }

    auto warns = [](const SongReport& s)
    {
        return std::any_of(s.warnings.begin(), s.warnings.end(),
                           [](const std::string& w) { return w.find("channel 10's program 127") == 0; });
    };

    SEQLIB_CHECK(a.ok && b.ok && !warns(a) && warns(b));
    SEQLIB_CHECK(presets.count({0, 127}) && presets.count({1, 126}) && presets.count({1, 127}));
    SEQLIB_CHECK((presets.count({kDrumBank, 127}) && presets[{kDrumBank, 127}] == presets[{0, 127}]));
    SEQLIB_CHECK((presets.count({kDrumBank, 126}) && presets[{kDrumBank, 126}] == presets[{1, 126}]));
}

void TestSharedKey()
{
    // Track 2 plays key 60 on channel 0, and track 1 plays it again 3 frames later, while track 2's note still plays.
    // In the driver, track 1's note off releases the older note, and track 2's the newer one.
    const Format kFormat = Format::kChannelNibble;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{1, cart.Sample(Saw(), 32)}});
    cart.Tune(
        {Track(kFormat).Tempo(500000).Wait(960).End(), Track(kFormat).Wait(48).On(0, 60, 100).Wait(48).Off(0, 60).End(),
         Track(kFormat).Program(0, 1).On(0, 60, 90).Wait(144).Off(0, 60).End()},
        bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("shared_key");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    // A player takes track 1's events of a tick before track 2's, so track 2's note ends a tick before track 1's
    // starts. Track 1's note lasts until track 2's note off.
    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_REQUIRE_EQ(m.tracks.size(), 3);

    auto notes = [](const seqlib::midi::Track& track)
    {
        std::vector<std::pair<uint32_t, int>> out;
        for (const auto& e : track.events)
        {
            if ((e.bytes[0] & 0xE0) == 0x80)
            {
                out.push_back({uint32_t(e.tick), e.bytes[0] & 0xF0});
            }
        }

        return out;
    };

    const std::vector<std::pair<uint32_t, int>> kTrack1 = {{48, 0x90}, {144, 0x80}};
    const std::vector<std::pair<uint32_t, int>> kTrack2 = {{0, 0x90}, {47, 0x80}};
    SEQLIB_CHECK(notes(m.tracks[1]) == kTrack1);
    SEQLIB_CHECK(notes(m.tracks[2]) == kTrack2);
}

// Key pressures of a MIDI file, as (tick, key, pressure).
using Pressures = std::vector<std::tuple<uint32_t, int, int>>;

// Returns the key pressures of a MIDI file on channel `ch` in the order that a player meets them.
Pressures KeyPressuresInOrder(const MidiEvents& m, int ch)
{
    Pressures out;
    for (const auto& e : m.Find(uint8_t(0xA0 | ch)))
    {
        out.push_back({uint32_t(e.tick), e.bytes[1], e.bytes[2]});
    }

    return out;
}

// Returns the key pressures of a MIDI file on channel `ch`, sorted.
Pressures KeyPressures(const MidiEvents& m, int ch)
{
    Pressures out = KeyPressuresInOrder(m, ch);
    std::sort(out.begin(), out.end());

    return out;
}

// Returns the General MIDI 2 Controller Destination Setting message that makes key pressure mute channel `ch`'s notes,
// as seqlib's reader gives it: F0, its length, then the rest.
std::vector<uint8_t> MuteMessage(int ch)
{
    return {0xF0, 0x08, 0x7F, 0x7F, 0x09, 0x02, uint8_t(ch), 0x02, 0x00, 0xF7};
}

// Returns true if no track of a MIDI file has a system exclusive event after a key pressure.
bool SysExFirst(const MidiEvents& m)
{
    for (const auto& track : m.tracks)
    {
        bool pressure = false;
        for (const auto& e : track.events)
        {
            pressure = pressure || (e.bytes[0] & 0xF0) == 0xA0;
            if (e.bytes[0] == 0xF0 && pressure)
            {
                return false;
            }
        }
    }

    return true;
}

// Returns true if every instrument's global zone has the modulator that mutes a note while its key's pressure is high.
bool MutesNotes(const Sf2Records& sf)
{
    const size_t instruments = sf.Count("inst", 22) - 1;
    for (size_t i = 0; i < instruments; i++)
    {
        const uint16_t bag = Le16(sf.Record("inst", 22, i) + 20);
        const uint16_t first = Le16(sf.Record("ibag", 4, bag) + 2), end = Le16(sf.Record("ibag", 4, bag + 1) + 2);
        bool found = false;
        for (uint16_t m = first; m < end; m++)
        {
            const uint8_t* mod = sf.Record("imod", 10, m);
            found = found || (Le16(mod) == 0x0C0A && Le16(mod + 2) == sf2::kInitialAttenuation && Le16(mod + 4) == 960);
        }
        if (!found)
        {
            return false;
        }
    }

    return instruments > 0;
}

// The notes of TestVoiceLimit's older revision. The ninth note, key 72 on channel 1, starts muted, and plays from the
// frame in which the first note is released, at tick 47. That note is muted from that frame. The SoundFont's instrument
// mutes a note while its key's pressure is high. The file starts with pressure 0 for the keys that play before it gives
// them a pressure: all but key 72. Those come before key 72's pressure at tick 0. Channels 0 and 1 mute keys. Each
// starts with General MIDI 2's message that makes key pressure mute its notes, before its pressures.
void TestKeyPressures()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});
    Track notes(kFormat);
    notes.Program(0, 0).Program(1, 0);
    for (int k = 60; k < 66; k++)
    {
        notes.On(0, k, 100);
    }
    notes.On(1, 70, 100).On(1, 71, 100).On(1, 72, 100).Wait(48).Off(0, 60).Wait(48);
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(), notes.End()}, bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("key_pressures");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const MidiEvents m = ReadMidi(r.midi_path);
    const Pressures kChannel0 = {{0, 60, 0}, {0, 61, 0}, {0, 62, 0}, {0, 63, 0}, {0, 64, 0}, {0, 65, 0}, {47, 60, 127}};
    SEQLIB_CHECK(KeyPressures(m, 0) == kChannel0);
    SEQLIB_CHECK(KeyPressuresInOrder(m, 1) == Pressures({{0, 70, 0}, {0, 71, 0}, {0, 72, 127}, {47, 72, 0}}));
    SEQLIB_CHECK(MutesNotes(ReadSf2(r.sf2_path)));
    const std::vector<seqlib::midi::Event> kMessages = {{0, MuteMessage(0)}, {0, MuteMessage(1)}};
    SEQLIB_CHECK(m.Find(0xF0) == kMessages);
    SEQLIB_CHECK(SysExFirst(m));
}

// Track 0 plays a ninth note at tick 32. The driver plays it a frame late, in the frame that starts at tick 47. The
// note starts muted: its pressure goes at its note on. Channel 3 alone gets General MIDI 2's mute message. Another tune
// plays key 70 twice on channel 1 with seven other notes, and the mixer leaves out one of the two. The key isn't muted.
// The SoundFont has no modulator for it, and the file has no message.
void TestPressureTiming()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});

    Track eight(kFormat);
    eight.Program(0, 0).Program(1, 0).Program(3, 0);
    for (int k = 60; k < 66; k++)
    {
        eight.On(0, k, 100);
    }
    const std::vector<uint8_t> held = eight.On(1, 70, 100).On(1, 71, 100).Wait(960).End();
    cart.Tune({Track(kFormat).Tempo(500000).Wait(32).On(3, 90, 100).Wait(928).End(), held}, bank);

    Track twice(kFormat);
    twice.Program(0, 0).Program(1, 0);
    for (int k = 60; k < 66; k++)
    {
        twice.On(0, k, 100);
    }
    cart.Tune({Track(kFormat).Tempo(500000).Wait(960).End(),
               twice.On(1, 70, 100).On(1, 71, 100).On(1, 70, 100).Wait(96).End()},
              bank);

    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("pressure_timing");

    const SongReport late = ConvertSong(rom, info, 0, opt, nullptr);
    const SongReport shared = ConvertSong(rom, info, 1, opt, nullptr);

    SEQLIB_CHECK(late.ok && shared.ok);
    const MidiEvents m = ReadMidi(late.midi_path);
    SEQLIB_CHECK(KeyPressures(m, 3) == Pressures({{32, 90, 127}}));
    SEQLIB_REQUIRE_EQ(m.Find(0x93).size(), 1);
    SEQLIB_CHECK_EQ(m.Find(0x93)[0].tick, 32);
    SEQLIB_CHECK(m.Find(0xF0) == std::vector<seqlib::midi::Event>({{0, MuteMessage(3)}}));

    const MidiEvents s = ReadMidi(shared.midi_path);
    SEQLIB_CHECK(KeyPressures(s, 0).empty() && KeyPressures(s, 1).empty());
    SEQLIB_CHECK(!MutesNotes(ReadSf2(shared.sf2_path)));
    SEQLIB_CHECK(s.Find(0xF0).empty());
}

// A loop starts with key 90 on channel 3 alone, and later plays it with nine other notes. The mixer then leaves out key
// 91 and the released key 90. Key 90 plays again from the frame that starts the next pass, at tick 287, before the
// loop's end. Key 91 stays muted. A player that jumps back would keep it muted. The file therefore gives its pressure
// from the loop's start, 0, again there. Key 90 plays before the file gives it a pressure. The file starts with
// pressure 0 for it too. In the next pass, the mixer also leaves out the release of key 71 on channel 1. Channels 1 and
// 3 therefore get General MIDI 2's mute message.
void TestLoopPressures()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});

    Track loop(kFormat);
    loop.Program(0, 0).Program(1, 0).Program(3, 0).Control(0, kCtrlLoopStart, 0);
    loop.On(3, 90, 100).Wait(96).Off(3, 90);

    for (int k = 60; k < 66; k++)
    {
        loop.On(0, k, 100);
    }
    loop.On(1, 70, 100).On(1, 71, 100).On(3, 91, 100).Wait(96);

    for (int k = 60; k < 66; k++)
    {
        loop.Off(0, k);
    }
    loop.Off(1, 70).Off(1, 71).Off(3, 91).Wait(96).Control(0, kCtrlLoopEnd, 0);
    cart.Tune({Track(kFormat).Tempo(500000).Wait(96).End(), loop.End()}, bank);

    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    ConvertSettings opt = TempSettings("loop_pressures");

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent);
    const Pressures kPressures = {{0, 90, 0},   {0, 91, 0},     {95, 90, 127}, {95, 91, 127},
                                  {287, 90, 0}, {383, 90, 127}, {575, 90, 0}};
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK(KeyPressures(m, 3) == kPressures);
    const std::vector<seqlib::midi::Event> kMessages = {{0, MuteMessage(1)}, {0, MuteMessage(3)}};
    SEQLIB_CHECK(m.Find(0xF0) == kMessages);
}

// A tune that the mixer leaves no note out of gets no key pressures with a SoundFont of its own. With the shared
// SoundFont of --single-sf2, which another tune's mutes can give the modulator, the file starts with pressure 0 for
// each key that it plays. Neither file mutes a key, and neither has General MIDI 2's mute message.
void TestStartPressures()
{
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32, 11025, 60, {99, 99, 99, 50})}});
    Track notes(kFormat);
    notes.Program(0, 0).Program(2, 0).On(0, 60, 100).On(2, 64, 100).Wait(48).Off(0, 60).On(0, 62, 100).Wait(48);
    notes.Off(0, 62).Off(2, 64);
    cart.Tune({Track(kFormat).Tempo(500000).Wait(96).End(), notes.End()}, bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);
    SoundfontBuilder shared(rom, info, 1);
    ConvertSettings own = TempSettings("start_pressures_own");
    ConvertSettings in_set = own;
    in_set.base_name = "start_pressures_set";

    const SongReport alone = ConvertSong(rom, info, 0, own, nullptr);
    const SongReport set = ConvertSong(rom, info, 0, in_set, &shared);

    SEQLIB_CHECK(alone.ok && set.ok);
    const MidiEvents a = ReadMidi(alone.midi_path);
    const MidiEvents s = ReadMidi(set.midi_path);
    SEQLIB_CHECK(KeyPressures(a, 0).empty() && KeyPressures(a, 2).empty());
    SEQLIB_CHECK(KeyPressuresInOrder(s, 0) == Pressures({{0, 60, 0}, {0, 62, 0}}));
    SEQLIB_CHECK(KeyPressuresInOrder(s, 2) == Pressures({{0, 64, 0}}));
    SEQLIB_CHECK(a.Find(0xF0).empty() && s.Find(0xF0).empty());
}

void TestTrackChoice()
{
    // Only the chosen tracks' notes are written, and a tune with none of them is skipped.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}});
    cart.Tune({Track(kFormat).Tempo(500000).Program(0, 0).On(0, 60, 100).Wait(100).Off(0, 60).End(),
               Track(kFormat).Program(1, 0).On(1, 62, 100).Wait(100).Off(1, 62).End()},
              bank);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    ConvertSettings opt = TempSettings("tracks");
    opt.track_mask = 1 << 1;

    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && !r.silent && r.tracks == 1);
    const MidiEvents m = ReadMidi(r.midi_path);
    SEQLIB_CHECK(m.Find(0x90).empty() && m.Find(0x91).size() == 1);

    opt.track_mask = 1 << 5;

    SEQLIB_CHECK(ConvertSong(rom, info, 0, opt, nullptr).silent);
}

// The tune that the game plays by the water has a version above water and one under water. From the tune's second
// frame, each version leaves the channels of the other out of the mix and out of the voice limit. Neither MIDI file has
// the channels of the other version.
void TestWaterVersions()
{
    // Above water, channels 0 and 1 play five notes. Under water, channels 2 and 3 play five: ten voices, two more than
    // the limit.
    const Format kFormat = Format::kChannelByte;
    Cart cart(kFormat);
    const auto bank = cart.Bank({{0, cart.Sample(Saw(), 32)}, {1, cart.Sample(Saw(), 32)}});
    Track above(kFormat);
    Track under(kFormat);
    above.Tempo(500000).Program(0, 0).Program(1, 0).On(0, 60, 100).On(0, 61, 100).On(0, 62, 100);
    above.On(1, 70, 100).On(1, 71, 100);
    under.Program(2, 1).Program(3, 1).Control(2, kCtrlVolume, 100).On(2, 80, 100).On(2, 81, 100).On(2, 82, 100);
    under.On(3, 90, 100).On(3, 91, 100);
    cart.Tune({above.Wait(240).End(), under.Wait(240).End()}, bank);
    cart.AddEnvelopeControllers();
    cart.AddLevelTest();
    cart.AddWaterRoutine(0, 0, 1, 2, 3);
    const Rom rom = cart.ToRom();
    const DriverInfo info = Detect(rom);

    // What the mixer does with the first voice of each channel in the tune's first or second frame. The driver gives
    // every channel a level of 128 as it starts the tune. The first frame therefore plays every channel, and channel
    // 3's voices are past the limit. The game turns the other version's channels down to 0 before the second frame.
    // From then on, those voices are left out and don't count.
    auto results = [&](uint16_t silenced, int frames)
    {
        Sequencer seq(rom, info, 0, silenced);
        for (int f = 0; f < frames; f++)
        {
            seq.Step();
        }
        std::vector<MixResult> out;
        for (int channel = 0; channel < 4; channel++)
        {
            out.push_back(seq.GetSlot(channel * info.slots_per_channel).mix);
        }

        return out;
    };

    using M = MixResult;
    const std::vector<MixResult> every = {M::kMixed, M::kMixed, M::kMixed, M::kSkipped};
    SEQLIB_CHECK(results(0, 1) == every);
    SEQLIB_CHECK(results(SilencedChannels(info, 0, false), 1) == every);
    SEQLIB_CHECK(results(SilencedChannels(info, 0, true), 1) == every);
    SEQLIB_CHECK(results(SilencedChannels(info, 0, false), 2) ==
                 std::vector<MixResult>({M::kMixed, M::kMixed, M::kSilent, M::kSilent}));
    SEQLIB_CHECK(results(SilencedChannels(info, 0, true), 2) ==
                 std::vector<MixResult>({M::kSilent, M::kSilent, M::kMixed, M::kMixed}));

    // Each version's files: above water as usual, and under water with the version's suffix.
    ConvertSettings opt = TempSettings("water");
    const SongReport r = ConvertSong(rom, info, 0, opt, nullptr);
    opt.version = kUnderWater;
    const SongReport u = ConvertSong(rom, info, 0, opt, nullptr);

    SEQLIB_CHECK(r.ok && u.ok && r.tracks == 1 && u.tracks == 1);
    SEQLIB_CHECK(PathFromUtf8(r.midi_path).filename() == "water_00.mid");
    SEQLIB_CHECK(PathFromUtf8(u.midi_path).filename() == "water_00_underwater.mid");
    SEQLIB_CHECK(PathFromUtf8(u.sf2_path).filename() == "water_00_underwater.sf2");

    // Each file plays the notes of its version and has no events of the other version's channels. Above water, no
    // note is left out. Under water, the mixer leaves out channel 3's notes in the first frame. Their keys are muted
    // until the second frame, which starts at tick 15. The keys of channel 2 start at pressure 0.
    const MidiEvents a = ReadMidi(r.midi_path);
    const MidiEvents b = ReadMidi(u.midi_path);
    SEQLIB_CHECK(a.Find(0x90).size() == 3 && a.Find(0x91).size() == 2 && a.Find(0xA0).empty() && a.Find(0xA1).empty());
    SEQLIB_CHECK(a.Find(0x92).empty() && a.Find(0x93).empty() && a.Find(0xB2).empty() && a.Find(0xC3).empty());
    SEQLIB_CHECK(b.Find(0x92).size() == 3 && b.Find(0x93).size() == 2);
    SEQLIB_CHECK(KeyPressures(b, 2) == Pressures({{0, 80, 0}, {0, 81, 0}, {0, 82, 0}}));
    SEQLIB_CHECK(KeyPressures(b, 3) == Pressures({{0, 90, 127}, {0, 91, 127}, {15, 90, 0}, {15, 91, 0}}));
    SEQLIB_CHECK(b.Find(0x90).empty() && b.Find(0x91).empty() && b.Find(0xB0).empty() && b.Find(0xC1).empty());

    // The version under water has its name in the title.
    std::string title;
    for (const auto& e : b.tracks[0].events)
    {
        if (e.bytes.size() > 3 && e.bytes[0] == 0xFF && e.bytes[1] == 0x03)
        {
            title.assign(e.bytes.begin() + 3, e.bytes.end());
        }
    }
    SEQLIB_CHECK(title.ends_with("#00 under water"));

    // The tune has both versions.
    SEQLIB_CHECK(TuneVersions(info, 0).size() == 2 && TuneVersions(info, 0)[1].name == "under water");
}

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
    if (!ntdlib::testing::agb_files::MakeTempFolder())
    {
        std::fprintf(stderr, "couldn't make a temp folder\n");
        return 1;
    }

    TestDecoding();
    TestDetection();
    TestMusicVolume();
    TestWaterTune();
    TestTiming();
    TestSlots();
    TestDroppedWarning();
    TestEnvelope();
    TestEnvelopeSettings();
    TestUnmixedEnd();
    TestVoiceLimit();
    TestPitch();
    TestLoops();
    TestLoopBend();
    TestLoopStarts();
    TestLoopLengths();
    TestTrackWithoutLoop();
    TestHourLimit();
    TestFrameTiming();
    TestConversion();
    TestTwoTempoTracks();
    TestEnvelopeConversion();
    TestEnvelopePastEnd();
    TestEnvelopeAtEnd();
    TestEnvelopeBanks();
    TestLookUpInstrument();
    TestSongFiles();
    TestOtherDrumKitType();
    TestPrograms();
    TestSharedKey();
    TestKeyPressures();
    TestPressureTiming();
    TestLoopPressures();
    TestStartPressures();
    TestSharedDrumBank();
    TestTrackChoice();
    TestWaterVersions();
    TestPerformance();
    TestPlayer();
    TestAudition();
    TestDump();

    std::error_code ec;
    std::filesystem::remove_all(ntdlib::testing::agb_files::g_temp, ec);
    return seqlib::testing::Summary("rare");
}

} // namespace ntdlib::sequence_export::rare

int main()
{
    return ntdlib::sequence_export::rare::RunTests();
}
