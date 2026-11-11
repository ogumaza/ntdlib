// SPDX-License-Identifier: MIT

// Unit tests for the conversion of Rare's songs: the MIDI and SoundFont files, envelopes, programs, drum banks and key
// pressures. They build small synthetic cartridges in memory, so no game ROM is needed.

#include "rare_test_util.h"

namespace ntdlib::sequence_export::rare
{
namespace
{

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

} // namespace

int RunTests()
{
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

    return seqlib::testing::Summary("rare_conversion");
}

} // namespace ntdlib::sequence_export::rare

int main()
{
    return ntdlib::sequence_export::rare::RunTests();
}
