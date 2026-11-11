// SPDX-License-Identifier: MIT

// Unit tests for Rare's driver: decoding, detection, the sequencer's timing, slots, envelopes and loops.
// rare_conversion_test.cpp tests the conversion, and rare_player_test.cpp the player. They build small synthetic
// cartridges in memory, so no game ROM is needed.

#include "rare_test_util.h"

namespace ntdlib::sequence_export::rare
{
namespace
{

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

    // A track's end can be the image's last byte. A command whose arguments or channel byte pass the end can't be read.
    for (Format format : {Format::kChannelByte, Format::kChannelNibble})
    {
        Rom end;
        end.Assign({0x00, 0x0B});
        SEQLIB_CHECK(DecodeEvent(end, kRomBase + 1, format, e) && e.command == kCmdEnd && e.size == 1);

        Rom wait;
        wait.Assign({0x00, 0x01});
        SEQLIB_CHECK(!DecodeEvent(wait, kRomBase + 1, format, e));
    }

    Rom program;
    program.Assign({0x00, 0x08});
    SEQLIB_CHECK(!DecodeEvent(program, kRomBase + 1, Format::kChannelByte, e));
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

} // namespace

int RunTests()
{
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

    return seqlib::testing::Summary("rare");
}

} // namespace ntdlib::sequence_export::rare

int main()
{
    return ntdlib::sequence_export::rare::RunTests();
}
