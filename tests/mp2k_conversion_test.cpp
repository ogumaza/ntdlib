// SPDX-License-Identifier: MIT

// Unit tests for MP2K's conversion: the MIDI and SoundFont files, the performance for an editor, the levels, the drum
// and voice channels and the PSG's exclusive classes. They build small synthetic cartridges in memory. No game ROM is
// needed.

#include "mp2k_test_util.h"

namespace ntdlib::sequence_export::mp2k
{
namespace
{

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
    const std::vector<seqlib::midi::Tempo> tempos = seqlib::midi::Tempos(m);
    SEQLIB_CHECK(std::any_of(tempos.begin(), tempos.end(), [](const seqlib::midi::Tempo& t)
                             { return t.track == 0 && t.micros_per_quarter == 401825; }));

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

int RunTests()
{
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

    return seqlib::testing::Summary("mp2k_conversion");
}

} // namespace
} // namespace ntdlib::sequence_export::mp2k

int main()
{
    return ntdlib::sequence_export::mp2k::RunTests();
}
