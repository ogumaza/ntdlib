// SPDX-License-Identifier: MIT

// Tests of sequence_export on synthetic archives: the MIDI, SoundFont and SFZ writers, and the conversion of banks.
// Each test runs on every console whose engine it describes, with the settings of that console (see
// sequence_export_test.h).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/file_name.h"
#include "seqlib/midi/encoder.h"
#include "seqlib/midi/performance.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/writer.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "sequence_export_test.h"

using namespace ntdlib::sequence_export;
using namespace ntdlib::sequence_export::tests;
using ntdlib::sar::SoundInfo;
using xsflib::testing::ErrorOf;

namespace sf2 = seqlib::sf2;

namespace
{

void TestSoundFontWriterOutputIsValid()
{
    sf2::SoundFont sf;
    sf.info = {{"INAM", "test"}, {"ISFT", console->program}};
    for (int c = 0; c < 3; c++)
    {
        sf2::Sample s;
        s.name = "s" + std::to_string(c);
        s.data.assign(100, static_cast<int16_t>(c * 1000));
        s.loop_start = 10;
        s.loop_end = 90;
        sf.samples.push_back(s);
    }

    sf.samples[1].type = sf2::kLeft;
    sf.samples[1].link = 2;
    sf.samples[2].type = sf2::kRight;
    sf.samples[2].link = 1;

    sf2::Instrument inst;
    inst.name = "i";
    sf2::Zone global;
    global.modulators.push_back({0x0081, sf2::kVibLfoToPitch, 100, 0});
    inst.zones.push_back(global);
    for (uint16_t id = 0; id < 3; id++)
    {
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, id)); // written last whatever the order
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, 1));
        z.generators.push_back(sf2::Generator::Range(sf2::kVelRange, 0, 127));
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
        inst.zones.push_back(z);
    }
    sf.instruments.push_back(inst);

    sf2::Preset p;
    p.name = "p";
    sf2::Zone pz;
    pz.generators.push_back(sf2::Generator::Value(sf2::kInstrument, 0));
    p.zones.push_back(pz);
    sf.presets.push_back(p);

    const auto bytes = sf2::Write(sf);

    const std::string problem = Sf2Problem(bytes);
    XSFLIB_CHECK(problem.empty());
    if (!problem.empty())
    {
        std::fprintf(stderr, "  SoundFont writer: %s\n", problem.c_str());
    }

    const seqlib::sf2::Records v = seqlib::sf2::Read(bytes);
    XSFLIB_CHECK(v.chunks.at("phdr").size() == 38 * 2 && v.chunks.at("inst").size() == 22 * 2);
    XSFLIB_CHECK(v.chunks.at("smpl").size() == 2 * 3 * (100 + 46));
    XSFLIB_CHECK(v.chunks.at("imod").size() == 10 * 2);
}

void TestSoundFontWithoutSamplesHasSampleData()
{
    // Even a silent sequence requires sample padding: FluidSynth rejects empty sample data.
    sf2::SoundFont sf;
    sf.info = {{"INAM", ""}, {"ISFT", console->program}};
    const auto bytes = sf2::Write(sf);

    const seqlib::sf2::Records v = seqlib::sf2::Read(bytes);

    XSFLIB_CHECK(Sf2Problem(bytes).empty() && v.chunks.at("smpl").size() == 2 * 46);
}

void TestMidiWriterLayersMarkersAndBendRange()
{
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.loop = Performance::Loop{48, 96};
    p.end_tick = 96;
    p.presets.push_back({});

    auto add = [&](int t, uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        p.tracks[t].push_back(e);
    };

    add(0, 0, EventKind::kPreset, 0, 0, 0);
    add(0, 0, EventKind::kVolume, 0, 100, 0);
    add(0, 0, EventKind::kNoteOn, 60, 100, 1);
    add(0, 10, EventKind::kNoteOn, 60, 90, 2); // the same key again: another channel
    add(0, 20, EventKind::kNoteOff, 60, 0, 1);
    add(0, 30, EventKind::kNoteOff, 60, 0, 2);
    add(3, 5, EventKind::kNoteOn, 64, 80, 3);
    add(3, 50, EventKind::kNoteOff, 64, 0, 3);
    TrackEvent bend;
    bend.tick = 5;
    bend.kind = EventKind::kPitch;
    bend.semitones = -7.5f;
    p.tracks[3].insert(p.tracks[3].begin(), bend);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep, console->midi);

    // Timebase 48 gives 160 MIDI ticks for each of the performance's ticks.
    const MidiView v = ParseMidi(bytes);
    XSFLIB_CHECK(v.ok);
    XSFLIB_CHECK(v.division == 7680);
    XSFLIB_CHECK(rep.channels == 3 && rep.cut_notes == 0);
    XSFLIB_CHECK(v.track_names.size() == 4 && v.track_names[0] == "test" && v.track_names[2] == "Track 0 (layer 2)");
    XSFLIB_CHECK(v.notes.size() == 3);

    // Track 0 gets channel 0, and channel 2 for its layer (track 3 has channel 1).
    int channels_for_key60 = 0;
    for (const MidiNote& n : v.notes)
    {
        if (n.key == 60)
        {
            channels_for_key60 |= 1 << n.channel;
        }
    }

    XSFLIB_CHECK(channels_for_key60 == 0b101);

    XSFLIB_CHECK(v.markers.size() == 2 && v.markers[0].first == 7680 && v.markers[1].second == "loopEnd");

    // Pitch bend range 8 (RPN 0) for a 7.5 semitone bend on track 3's channel (1).
    const std::vector<uint8_t> kRpn = {0xb1, 6, 8};
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kRpn.begin(), kRpn.end()) != bytes.end());
}

void TestMidiWriterGivesPartsChannelsOfTheirOwn()
{
    // Track 0 plays key 60 on its first part and key 62 on its second part. Only the second part sets volume 50.
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});

    auto add = [&](uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note, uint8_t part)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        e.part = part;
        p.tracks[0].push_back(e);
    };
    add(0, EventKind::kPreset, 0, 0, 0, 0);
    add(0, EventKind::kNoteOn, 60, 100, 1, 0);
    add(10, EventKind::kPreset, 0, 0, 0, 1);
    add(10, EventKind::kVolume, 0, 50, 0, 1);
    add(10, EventKind::kNoteOn, 62, 100, 2, 1);
    add(20, EventKind::kNoteOff, 60, 0, 1, 0);
    add(30, EventKind::kNoteOff, 62, 0, 2, 1);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep, console->midi);

    const MidiView v = ParseMidi(bytes);
    const std::vector<uint8_t> kSecondVolume = {0xb1, 7, 50}, kFirstVolume = {0xb0, 7, 50};
    XSFLIB_CHECK(v.ok && rep.channels == 2 && v.track_names.size() == 3 && v.track_names[2] == "Track 0 (layer 2)");
    XSFLIB_CHECK(v.notes.size() == 2 && v.notes[0].channel != v.notes[1].channel);
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kSecondVolume.begin(), kSecondVolume.end()) != bytes.end() &&
                 std::search(bytes.begin(), bytes.end(), kFirstVolume.begin(), kFirstVolume.end()) == bytes.end());
}

void TestMidiWriterHasAtLeast7680TicksPerQuarterNote()
{
    // Use at least 7680 MIDI ticks per quarter note, with an integer ratio to performance ticks. Without frame
    // timestamps, preserve note positions.
    const std::pair<uint32_t, uint16_t> kCases[] = {{48, 7680}, {96, 7680}, {100, 7700}, {255, 7905}, {1, 7680}};
    for (const auto& [timebase, division] : kCases)
    {
        Performance p;
        p.timebase = timebase;
        p.tempo = {{0, 120.0}};
        p.end_tick = 8;
        p.presets.push_back({});
        TrackEvent on;
        on.tick = 3;
        on.key = 60;
        on.value = 100;
        on.note = 1;
        TrackEvent off = on;
        off.tick = 7;
        off.kind = EventKind::kNoteOff;
        p.tracks[0] = {on, off};

        MidiReport rep;
        const MidiView v = ParseMidi(WriteMidi(p, "test", rep, console->midi));
        const uint32_t scale = division / timebase;
        XSFLIB_CHECK(v.ok && v.division == division);
        XSFLIB_CHECK(v.notes.size() == 1 && v.notes[0].on == 3 * scale && v.notes[0].off == 7 * scale);
    }
}

void TestMidiEventsGoOnSoundFrames()
{
    // At timebase 48, use 160 MIDI ticks per performance tick. Align events to fractional frame starts without crossing
    // the loop start at tick 2. Keys 60, 62 and 64 span ticks 1-2, 2-3 and 3-4, and a sweep bends key 64 up a semitone
    // in a frame between ticks 3 and 4, half a tick after tick 3.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 8;
    p.presets.push_back({});
    p.frame_ticks = {0.0, 0.5, 1.75, 2.75, 4.0};
    p.loop = Performance::Loop{2, 6};
    const std::pair<uint8_t, uint32_t> kNotes[] = {{60, 1}, {62, 2}, {64, 3}}; // key, tick
    uint32_t id = 1;
    for (const auto& [key, tick] : kNotes)
    {
        TrackEvent on;
        on.tick = tick;
        on.key = key;
        on.value = 100;
        on.note = id++;
        TrackEvent off = on;
        off.tick = tick + 1;
        off.kind = EventKind::kNoteOff;
        p.tracks[0].push_back(on);
        p.tracks[0].push_back(off);
    }

    TrackEvent bend;
    bend.tick = 3;
    bend.kind = EventKind::kPitch;
    bend.semitones = 1.0f;
    bend.after = 0.5;
    p.tracks[0].insert(p.tracks[0].end() - 1, bend);

    MidiReport rep;
    const MidiView m = ParseMidi(WriteMidi(p, "test", rep, console->midi));

    XSFLIB_CHECK(m.ok && m.notes.size() == 3);
    XSFLIB_CHECK(m.bends == (std::vector<std::pair<uint32_t, int>>{{560, 12288}}));
    if (m.notes.size() == 3)
    {
        XSFLIB_CHECK(m.notes[0].key == 60 && m.notes[0].on == 80 && m.notes[0].off == 320);
        XSFLIB_CHECK(m.notes[1].key == 62 && m.notes[1].on == 320 && m.notes[1].off == 440);
        XSFLIB_CHECK(m.notes[2].key == 64 && m.notes[2].on == 440 && m.notes[2].off == 640);
    }
    XSFLIB_CHECK(m.markers == (std::vector<std::pair<uint32_t, std::string>>{{320, "loopStart"}, {960, "loopEnd"}}));
}

void TestMidiWriterSplitsLongDeltaTimes()
{
    // At timebase 1, each performance tick is 7680 MIDI ticks. A 600000-tick note exceeds both 32-bit MIDI positions
    // and the 28-bit delta limit.
    Performance p;
    p.timebase = 1;
    p.tempo = {{0, 120.0}};
    p.end_tick = 600000;
    p.presets.push_back({});
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent off = on;
    off.tick = 600000;
    off.kind = EventKind::kNoteOff;
    p.tracks[0] = {on, off};

    MidiReport rep;
    const MidiView v = ParseMidi(WriteMidi(p, "test", rep, console->midi));

    XSFLIB_CHECK(v.ok && v.track_ends == std::vector<uint64_t>(2, uint64_t{600000} * 7680));
}

void TestMidiBendUpByAWholeRangeIsExact()
{
    // A +7-semitone bend requires range 8 because the pitch wheel stops at +8191, one step short of full range. With
    // range 8, +7 is 7168 steps: wheel value 15360 (0x3c00, bytes 0 and 0x78).
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent bend;
    bend.kind = EventKind::kPitch;
    bend.semitones = 7.0f;
    p.tracks[0].push_back(bend);
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    p.tracks[0].push_back(on);
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    p.tracks[0].push_back(off);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep, console->midi);

    const std::vector<uint8_t> kRange = {0xb0, 6, 8};
    const std::vector<uint8_t> kBend = {0xe0, 0x00, 0x78};
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kRange.begin(), kRange.end()) != bytes.end());
    XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), kBend.begin(), kBend.end()) != bytes.end());
}

void TestMidiWriterSendsTheFilterToEveryLayer()
{
    // Overlapping strikes of one key require a second channel. Send filter controllers to both.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent filter;
    filter.kind = kFilterEvent;
    filter.value = 40;
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent again = on;
    again.tick = 12;
    again.note = 2;
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    TrackEvent off_again = again;
    off_again.tick = 36;
    off_again.kind = EventKind::kNoteOff;
    p.tracks[0] = {filter, on, again, off, off_again};

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep, console->midi);

    XSFLIB_CHECK(rep.channels == 2);
    for (const int channel : {0, 1})
    {
        const std::vector<uint8_t> filter_change = {static_cast<uint8_t>(0xb0 | channel), kFilterController, 40};
        XSFLIB_CHECK(std::search(bytes.begin(), bytes.end(), filter_change.begin(), filter_change.end()) !=
                     bytes.end());
    }
}

void TestMidiWriterListsChannel10Presets()
{
    // Track t uses preset t. With 16 tracks, the last gets channel 10 (index 9). Copy its preset to bank 128. With 15
    // tracks, leave channel 10 unused.
    for (int tracks : {15, 16})
    {
        Performance p;
        p.tempo = {{0, 120.0}};
        p.end_tick = 48;
        for (int t = 0; t < tracks; t++)
        {
            TrackEvent preset;
            preset.kind = EventKind::kPreset;
            preset.preset = static_cast<uint32_t>(t);
            TrackEvent on;
            on.key = 60;
            on.value = 100;
            on.note = static_cast<uint32_t>(t + 1);
            TrackEvent off = on;
            off.tick = 24;
            off.kind = EventKind::kNoteOff;
            p.presets.push_back({});
            p.tracks[t] = {preset, on, off};
        }

        MidiReport rep;
        WriteMidi(p, "test", rep, console->midi);

        XSFLIB_CHECK(rep.channels == tracks);
        XSFLIB_CHECK(rep.channel_10_presets == (tracks == 16 ? std::set<uint32_t>{15} : std::set<uint32_t>{}));
    }
}

void TestSfzFileHasTheTracksPresetsAndFilters()
{
    // Track 2 uses presets 0 and 1 with nominal 80 Hz low-pass (0xd8 argument 4) and biquad high-pass (type 2). Other
    // tracks are silent and get no MIDI channel or SFZ file. With no archive banks, the presets have no regions.
    const SoundArchive archive = SoundArchive::Load(Fsar({}), console->settings.archive);
    BankSet banks(archive, SoundInfo{}, console->settings);
    Performance p;
    p.presets.resize(2);
    TrackEvent low_pass;
    low_pass.kind = kSfzLowPassEvent;
    low_pass.value = SfzLowPassValue(4);
    TrackEvent biquad;
    biquad.kind = kSfzBiquadEvent;
    biquad.value = SfzBiquadValue(2, 64);
    TrackEvent first;
    first.kind = EventKind::kPreset;
    TrackEvent second = first;
    second.preset = 1;
    p.tracks[2] = {low_pass, biquad, first, second};
    p.biquad_types[2] = 2;
    p.level_cut_db = 2.5;
    MidiReport midi;
    midi.bend_ranges[2] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "TITLE", {"first", "second"});

    XSFLIB_CHECK(sfz.files.size() == 1 && sfz.samples.empty());
    if (sfz.files.size() != 1)
    {
        return;
    }

    const SfzFile& f = sfz.files[0];
    auto has = [&](const std::string& s)
    {
        return f.text.find(s) != std::string::npos;
    };
    XSFLIB_CHECK(f.name == "Track 2.sfz");
    XSFLIB_CHECK(has("MIDI gain was reduced by 2.5 dB. Region volumes restore that gain.\n"));
    XSFLIB_CHECK(has("<control>\ndefault_path=../samples/\n"));
    XSFLIB_CHECK(has("bend_up=200 bend_down=-200\n"));

    // CC7, CC10 and CC11 act at once, with CC7 and CC11 squared.
    XSFLIB_CHECK(has("pan_oncc10=100 pan_curvecc10=7 pan_smoothcc10=0\n"));
    XSFLIB_CHECK(
        has("amplitude_oncc7=100 amplitude_curvecc7=4 amplitude_smoothcc7=0 amplitude_oncc11=100 "
            "amplitude_curvecc11=4 amplitude_smoothcc11=0\n"));

    // Low-pass uses filter 1. Controller value 60 selects nominal 80 Hz: 9558 cents below 20 kHz, 8936 on the Switch,
    // the 115 Hz of nn::atk's table, or 9131 on the 3DS. 0 leaves it open.
    XSFLIB_CHECK(low_pass.value == 60);
    XSFLIB_CHECK(has("fil_type=lpf_1p cutoff=1000 cutoff_oncc103=9600 cutoff_curvecc103=8\n"));
    const std::string v060 = ByConsole<std::string>(" v060=-0.455384 ", " v060=-0.390592 ", " v060=-0.410905 ");
    XSFLIB_CHECK(has("<curve> curve_index=8\nv000=0.540241 ") && has(v060));

    // Each preset gets a master. Notes starting with the biquad controller above 0 use high-pass in filter slot 2.
    XSFLIB_CHECK(has("// first\n<master> loprog=0 hiprog=0\n") && has("// second\n<master> loprog=1 hiprog=1\n"));
    XSFLIB_CHECK(
        has("<group> hicc104=0\n<group> locc104=1 fil2_type=hpf_2p cutoff2=1000 cutoff2_oncc104=9600 "
            "cutoff2_curvecc104=9 resonance2=0 resonance2_oncc104=40 resonance2_curvecc104=10 "
            "volume_oncc104=2 volume_curvecc104=11"));
}

// Band-pass type 5's widest steps go below -40 dB. A track of that type takes a resonance amount of 50 dB. A track of
// another type takes 40 dB.
void TestSfzResonanceAmountFitsTheType()
{
    const SoundArchive archive = SoundArchive::Load(Fsar({}), console->settings.archive);
    BankSet banks(archive, SoundInfo{}, console->settings);
    auto track_file = [&](int type)
    {
        Performance p;
        p.presets.resize(1);
        TrackEvent biquad;
        biquad.kind = kSfzBiquadEvent;
        biquad.value = SfzBiquadValue(type, 64);
        TrackEvent preset;
        preset.kind = EventKind::kPreset;
        p.tracks[0] = {biquad, preset};
        p.biquad_types[0] = static_cast<uint8_t>(type);
        MidiReport midi;
        midi.bend_ranges[0] = 2;
        const SfzInstruments sfz = BuildSfz(p, midi, banks, "TITLE", {"first"});
        return sfz.files.size() == 1 ? sfz.files[0].text : std::string();
    };

    const std::string wide = track_file(5);
    const std::string narrow = track_file(4);

    XSFLIB_CHECK(wide.find("// Biquad resonance (Q in dB): units of 50 dB\n") != std::string::npos);
    XSFLIB_CHECK(wide.find(" resonance=0 resonance_oncc104=50 ") != std::string::npos);
    XSFLIB_CHECK(narrow.find("// Biquad resonance (Q in dB): units of 40 dB\n") != std::string::npos);
    XSFLIB_CHECK(narrow.find(" resonance=0 resonance_oncc104=40 ") != std::string::npos);
}

// The archive of the tests of a bank's conversion. Its sequence plays program 0 at key 60, then program 2 at key 64.
// Program 1 of the bank holds no instrument. Program 0 plays a mono wave. Program 2 plays a stereo wave, at its
// original key, volume, pan and envelope. `volume` is program 2's volume. On the Wii, BankTestRsar builds the archive.
// Program 1 then holds an instrument whose region has no wave.
TestArchive BankTestArchive(uint8_t volume = 100)
{
    if (OnWii())
    {
        return {.rsar_archive = WiiBankTestArchive(volume)};
    }

    const auto table = [](const files::Region& region)
    {
        const files::VelocityTable velocities{files::VelocityTable::kDirect, 0, {{127, region}}};
        return files::KeyTable{files::KeyTable::kDirect, 0, {{127, velocities}}};
    };
    files::Region second;
    second.wave = 1;
    second.original_key = 64;
    second.volume = volume;
    second.pan = 40;
    second.adshr = std::array<uint8_t, 5>{120, 100, 90, 0, 110};

    std::vector<ArchiveFile> archive_files(3);
    archive_files[0].data = Fseq({0x81, 0x00, 0x3c, 0x64, 0x30, 0x81, 0x02, 0x40, 0x64, 0x30, 0xff});
    archive_files[1].data = files::Bank(console->settings.archive.flavor,
                                        {{{0x05000000, 0}, {0x05000000, 1}}, {table({}), std::nullopt, table(second)}});
    archive_files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true),
                                  Pcm16Fwav(32, std::vector<int16_t>(32, -2000), 2, true)});

    return LoadTestArchive(
        Fsar(archive_files, {.sequences = {{.banks = {0x03000000}}}, .banks = {1}, .wave_archives = {2}}));
}

// Converts bank `bank` of `archive`. On the Wii, the bank's stereo waves play as in BankTestRsar's first sound, in pan
// mode 1.
BankConversion ConvertTestBank(const TestArchive& archive, std::size_t bank)
{
    if (archive.rsar_archive)
    {
        return WiiConvertBank(*archive.rsar_archive, bank, 1);
    }

    return ConvertBank(*archive.sar_archive, bank, console->settings, console->program);
}

// The name of the first bank of `archive`.
std::string FirstBankName(const TestArchive& archive)
{
    return archive.rsar_archive ? archive.rsar_archive->Banks().at(0).name : archive.sar_archive->Banks().at(0).name;
}

// A bank's SoundFont holds each instrument at its program. Each instrument's zones and samples are those of a sequence
// that plays the instrument with a track's starting settings.
void TestBankSoundFontHoldsEachInstrumentAtItsProgram()
{
    const TestArchive archive = BankTestArchive();

    const BankConversion bank = ConvertTestBank(archive, 0);
    const Conversion sequence = ConvertSound(archive, 0);

    CheckBankSoundFontHoldsEachInstrument(bank, sequence);
}

// A bank's SFZ files play each instrument by itself, under the bank's name and the program. Each file's regions and
// samples are those of a sequence's track file that plays the instrument with a track's starting settings.
void TestBankSfzFilesPlayEachInstrumentByItself()
{
    const TestArchive archive = BankTestArchive();
    const std::string name = FirstBankName(archive);

    // A preset's text in a track's file runs from its <master> line to the next preset's comment. In an instrument's
    // file, it follows <global>. A track's file numbers the presets in the order that the sequence plays them. Program
    // 2 of the bank is preset 1 there.
    const auto preset = [](const std::string& text, const std::string& after)
    {
        const std::size_t found = text.find(after);
        if (found == std::string::npos)
        {
            return std::string("no preset");
        }

        const std::size_t start = found + after.size();
        const std::size_t end = text.find("\n// ", start);
        return text.substr(start, end == std::string::npos ? end : end - start);
    };

    const BankConversion bank = ConvertTestBank(archive, 0);
    const Conversion sequence = ConvertSound(archive, 0);

    XSFLIB_CHECK(bank.sfz.files.size() == 2 && sequence.sfz.files.size() == 1);
    if (bank.sfz.files.size() != 2 || sequence.sfz.files.size() != 1)
    {
        return;
    }

    const std::string& track = sequence.sfz.files[0].text;
    const std::string& first = bank.sfz.files[0].text;
    const std::string& second = bank.sfz.files[1].text;
    const std::string kGlobal = "amplitude_smoothcc11=0\n\n";
    XSFLIB_CHECK(bank.sfz.files[0].name == seqlib::SafeFileName(name) + "_0.sfz");
    XSFLIB_CHECK(bank.sfz.files[1].name == seqlib::SafeFileName(name) + "_2.sfz");
    XSFLIB_CHECK(first.starts_with("// Program 0 of " + name + ", converted by " + console->program + ".\n\n"));
    XSFLIB_CHECK(first.find("<control>\ndefault_path=../samples/\n") != std::string::npos);
    XSFLIB_CHECK(first.find("bend_up=200 bend_down=-200\n") != std::string::npos);
    XSFLIB_CHECK(first.find("<master>") == std::string::npos && first.find("<region> ") != std::string::npos);
    XSFLIB_CHECK(preset(first, kGlobal) == preset(track, "<master> loprog=0 hiprog=0\n"));
    XSFLIB_CHECK(preset(second, kGlobal) == preset(track, "<master> loprog=1 hiprog=1\n"));
    XSFLIB_CHECK(bank.sfz.samples.size() == 3 && sequence.sfz.samples.size() == 3);
    for (std::size_t i = 0; i < bank.sfz.samples.size() && i < sequence.sfz.samples.size(); i++)
    {
        XSFLIB_CHECK(bank.sfz.samples[i].name == sequence.sfz.samples[i].name);
        XSFLIB_CHECK(bank.sfz.samples[i].wav == sequence.sfz.samples[i].wav);
    }
}

// A bank's SFZ file names pad a program with zeros to the digits of the bank's last program.
void TestProgramSuffixPadsToTheLastProgram()
{
    XSFLIB_CHECK(ProgramSuffix(0, 0) == "_0");
    XSFLIB_CHECK(ProgramSuffix(7, 130) == "_007");
    XSFLIB_CHECK(ProgramSuffix(42, 130) == "_042");
    XSFLIB_CHECK(ProgramSuffix(130, 130) == "_130");
    XSFLIB_CHECK(ProgramSuffix(5, 9) == "_5");
    XSFLIB_CHECK(ProgramSuffix(1000, 130) == "_1000");
}

// Programs above 127 take the SoundFont's next banks: program 130 is program 2 of bank 1. The SFZ files' names pad the
// programs to the digits of the last one.
void TestBankProgramsAbove127TakeTheNextBank()
{
    const files::VelocityTable kVelocities{files::VelocityTable::kDirect, 0, {{127, files::Region{}}}};
    const files::KeyTable kKeys{files::KeyTable::kDirect, 0, {{127, kVelocities}}};
    std::vector<std::optional<files::KeyTable>> instruments(131);
    instruments[0] = kKeys;
    instruments[130] = kKeys;
    std::vector<ArchiveFile> archive_files(2);
    archive_files[0].data = files::Bank(console->settings.archive.flavor, {{{0x05000000, 0}}, instruments});
    archive_files[1].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive =
        SoundArchive::Load(Fsar(archive_files, {.banks = {0}, .wave_archives = {1}}), console->settings.archive);

    const BankConversion bank = ConvertBank(archive, 0, console->settings, console->program);

    const std::vector<uint8_t>& sf2 = bank.sound_fonts.at(0).sf2;
    const seqlib::sf2::Records v = seqlib::sf2::Read(sf2);
    XSFLIB_CHECK(bank.sound_fonts.size() == 1 && Sf2Problem(sf2).empty() && v.Count("phdr", 38) == 3);
    XSFLIB_CHECK(xsflib::LoadLe16(v.Record("phdr", 38, 1) + 20) == 2 &&
                 xsflib::LoadLe16(v.Record("phdr", 38, 1) + 22) == 1);
    XSFLIB_CHECK(bank.sfz.files.size() == 2 && bank.sfz.files.front().name == "BANK_0_000.sfz" &&
                 bank.sfz.files.back().name == "BANK_0_130.sfz");
}

// A region above volume 127 attenuates every zone of the SoundFont, as in a sequence's SoundFont, and the conversion
// says so. A bank missing from the archive throws. The SFZ file plays the region at 6 dB. SFZ's documentation gives
// volume a range up to 6 dB.
void TestLoudBankIsAttenuatedAndAMissingBankThrows()
{
    const TestArchive archive = BankTestArchive(254);

    const BankConversion bank = ConvertTestBank(archive, 0);
    const Conversion sequence = ConvertSound(archive, 0);
    const bool threw = !ErrorOf<std::out_of_range>([&] { ConvertTestBank(archive, 1); }).empty();

    XSFLIB_CHECK(bank.warnings ==
                 std::vector<std::string>{
                     "region volume exceeds 127 (SoundFont attenuated by 6.0 dB; SFZ keeps the original volume)"});
    // On the Wii the bank plays in balance pan mode. It attenuates each channel zone of the stereo wave by the pan, and
    // the SFZ volume isn't 6 there.
    if (!OnWii())
    {
        XSFLIB_CHECK(bank.sfz.files.size() == 2 && bank.sfz.files[1].text.find(" volume=6 ") != std::string::npos);
    }
    XSFLIB_CHECK(seqlib::sf2::Read(bank.sound_fonts.at(0).sf2).chunks.at("igen") ==
                 seqlib::sf2::Read(sequence.sf2).chunks.at("igen"));
    XSFLIB_CHECK(threw);
}

// The archive of a bank of 100 instruments, programs 0 to 99, or of those from `first` to `last` alone. Each instrument
// has a region for each key, on one mono wave. The bank's zones then need more than the SoundFont format's 65,535
// generators.
SoundArchive LargeBankArchive(uint32_t first = 0, uint32_t last = 99)
{
    const files::VelocityTable kVelocities{files::VelocityTable::kDirect, 0, {{127, files::Region{}}}};
    files::KeyTable keys{files::KeyTable::kIndex, 0, {}};
    for (int key = 0; key < 128; key++)
    {
        keys.entries.emplace_back(key, kVelocities);
    }
    std::vector<std::optional<files::KeyTable>> instruments(100);
    for (uint32_t p = first; p <= last; p++)
    {
        instruments[p] = keys;
    }
    std::vector<ArchiveFile> archive_files(2);
    archive_files[0].data = files::Bank(console->settings.archive.flavor, {{{0x05000000, 0}}, instruments});
    archive_files[1].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});

    return SoundArchive::Load(Fsar(archive_files, {.banks = {0}, .wave_archives = {1}}), console->settings.archive);
}

// Instruments past the SoundFont format's limits go into several SoundFonts. Each holds a run of programs in order, at
// their places, under a name that gives them. The first SoundFont's instruments have the zones and samples of a bank
// that holds them alone. The SFZ files hold every instrument.
void TestLargeBankSplitsItsSoundFont()
{
    const SoundArchive archive = LargeBankArchive();

    const BankConversion bank = ConvertBank(archive, 0, console->settings, console->program);

    XSFLIB_CHECK(bank.sound_fonts.size() >= 2 && bank.sfz.files.size() == 100 && bank.warnings.empty());
    if (bank.sound_fonts.size() < 2)
    {
        return;
    }

    uint32_t next = 0;
    for (const BankSoundFont& sound_font : bank.sound_fonts)
    {
        const seqlib::sf2::Records v = seqlib::sf2::Read(sound_font.sf2);
        XSFLIB_CHECK(Sf2Problem(sound_font.sf2).empty() && sound_font.first == next);
        XSFLIB_CHECK(v.Count("phdr", 38) == sound_font.last - sound_font.first + 2);
        for (uint32_t p = sound_font.first; p <= sound_font.last && p - sound_font.first + 1 < v.Count("phdr", 38); p++)
        {
            XSFLIB_CHECK(xsflib::LoadLe16(v.Record("phdr", 38, p - sound_font.first) + 20) == static_cast<int>(p));
        }
        const std::string name =
            "BANK_0, programs " + std::to_string(sound_font.first) + " to " + std::to_string(sound_font.last);
        XSFLIB_CHECK(std::string(v.chunks.at("INAM").begin(), v.chunks.at("INAM").end()).starts_with(name + '\0'));
        next = sound_font.last + 1;
    }
    XSFLIB_CHECK(next == 100);
    const BankSoundFont& run = bank.sound_fonts[0];
    const BankConversion alone =
        ConvertBank(LargeBankArchive(run.first, run.last), 0, console->settings, console->program);
    const seqlib::sf2::Records v = seqlib::sf2::Read(run.sf2);
    const seqlib::sf2::Records w = seqlib::sf2::Read(alone.sound_fonts.at(0).sf2);
    XSFLIB_CHECK(alone.sound_fonts.size() == 1);
    for (const char* chunk : {"ibag", "igen", "imod", "phdr", "shdr", "smpl"})
    {
        XSFLIB_CHECK(v.chunks.at(chunk) == w.chunks.at(chunk));
    }
}

// A sequence that plays programs 0 to `programs` - 1, a note each: key 60 or 61 at velocity 100. Each instrument has a
// region for each key, and each key's velocities split at 64. Each region plays a mono wave. Key 100 plays the second
// wave, and the other keys play the first. A hundred programs pass the SoundFont format's limit of 65,535 generators.
SoundArchive LargeSequenceArchive(int programs)
{
    files::VelocityTable velocities{files::VelocityTable::kRange, 0, {{64, files::Region{}}, {127, files::Region{}}}};
    files::KeyTable keys{files::KeyTable::kIndex, 0, {}};
    for (int key = 0; key < 128; key++)
    {
        if (key == 100)
        {
            files::VelocityTable other = velocities;
            other.entries[0].second->wave = 1;
            other.entries[1].second->wave = 1;
            keys.entries.emplace_back(key, other);
            continue;
        }

        keys.entries.emplace_back(key, velocities);
    }

    std::vector<uint8_t> code;
    for (int p = 0; p < programs; p++)
    {
        const std::array<uint8_t, 5> note = {0x81, static_cast<uint8_t>(p), static_cast<uint8_t>(60 + p % 2), 0x64,
                                             0x30};
        code.insert(code.end(), note.begin(), note.end());
    }
    code.push_back(0xff);

    std::vector<ArchiveFile> archive_files(3);
    archive_files[0].data = Fseq(code);
    archive_files[1].data =
        files::Bank(console->settings.archive.flavor,
                    {{{0x05000000, 0}, {0x05000000, 1}}, std::vector<std::optional<files::KeyTable>>(programs, keys)});
    archive_files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true),
                                  Pcm16Fwav(32, std::vector<int16_t>(32, -2000), 1, true)});

    return SoundArchive::Load(
        Fsar(archive_files, {.sequences = {{.banks = {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
        console->settings.archive);
}

// A sequence's SoundFont past the format's limits holds only the zones that its notes pick, and the waves of those
// zones, and says so. A SoundFont within the limits holds every zone of its presets, and says nothing. The MIDI file
// plays the same notes on the same presets either way.
void TestLargeSequenceKeepsThePlayedZones()
{
    const auto starts_with = [](const std::string& text)
    {
        return text.starts_with("the SoundFont holds only the zones");
    };

    const Conversion large = ConvertSequence(LargeSequenceArchive(100), 0);
    const seqlib::sf2::Records v = seqlib::sf2::Read(large.sf2);
    XSFLIB_CHECK(Sf2Problem(large.sf2).empty());
    XSFLIB_CHECK(v.Count("inst", 22) == 101 && v.Count("phdr", 38) == 101);
    XSFLIB_CHECK(v.Count("ibag", 4) == 201); // each instrument: its global zone and one zone
    XSFLIB_CHECK(v.Count("shdr", 46) == 2);  // one wave, and the terminal record
    XSFLIB_CHECK(std::ranges::count_if(large.warnings, starts_with) == 1);
    XSFLIB_CHECK(!large.sfz.files.empty());

    const Conversion small = ConvertSequence(LargeSequenceArchive(10), 0);
    const seqlib::sf2::Records w = seqlib::sf2::Read(small.sf2);
    XSFLIB_CHECK(Sf2Problem(small.sf2).empty());
    XSFLIB_CHECK(w.Count("ibag", 4) == 10 * (1 + 128 * 2) + 1);
    XSFLIB_CHECK(w.Count("shdr", 46) == 3); // both waves, and the terminal record
    XSFLIB_CHECK(std::ranges::none_of(small.warnings, starts_with));

    auto note_ons = [](const Performance& p)
    {
        std::vector<std::pair<int, int>> notes;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                notes.emplace_back(e.key, e.value);
            }
        }

        return notes;
    };
    const std::vector<std::pair<int, int>> large_notes = note_ons(large.performance);
    const std::vector<std::pair<int, int>> small_notes = note_ons(small.performance);
    XSFLIB_CHECK(large_notes.size() == 100 && small_notes.size() == 10);
    XSFLIB_CHECK(std::equal(small_notes.begin(), small_notes.end(), large_notes.begin()));
}

void TestDirectVelocityTableTakesVelocitiesAbove127()
{
    // With velocity range 255 (0xb3), key 60 at velocity 127 plays at velocity 255, and at velocity 255 at 512. Like
    // the game's GetRegion (0x022BBA30), the bank's direct velocity table gives its region for any velocity. Both notes
    // play.
    auto note_ons = [](uint8_t velocity)
    {
        const Performance p = PerformOnBank({0xb3, 0xff, 0x3c, velocity, 0x18, 0xff}, OneRegionFbnk({{0x05000000, 0}}));
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };

    XSFLIB_CHECK(note_ons(0x7f) == 1 && note_ons(0xff) == 1);
}

void TestSfzRegionFollowsLinearInterpolation()
{
    // A region that selects linear interpolation (1) on AX gets sfizz's linear one. The SoundFont can't follow it. The
    // performance therefore reports it. A region that selects the 4-tap filters (0) keeps sfizz's default.
    auto convert = [](uint8_t interpolation)
    {
        std::vector<ArchiveFile> files(3);
        files[0].data = Fseq({0x3c, 0x64, 0x18, 0xff});
        files[1].data = OneRegionFbnk({{0x05000000, 0}}, 127, 64, false, interpolation);
        files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
        const SoundArchive archive =
            SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                               console->settings.archive);
        BankSet banks(archive, archive.Sounds()[0], console->settings);
        MidiReport midi;
        midi.bend_ranges[0] = 2;

        const Performance p = Perform(archive, 0, banks, {});
        const SfzInstruments sfz =
            BuildSfz(p, midi, banks, "TITLE", std::vector<std::string>(p.presets.size(), "preset"));

        const bool reported =
            p.approximations.contains("linear sample interpolation (SoundFont uses the synth's interpolation)");
        return std::pair{sfz.files.size() == 1 && sfz.files[0].text.find(" sample_quality=1") != std::string::npos,
                         reported};
    };

    XSFLIB_CHECK(convert(1) == std::pair(true, true) && convert(0) == std::pair(false, false));
}

void TestSfzKeyGroupFadesAtTheGamesRate()
{
    // A note fades out the other notes of its key group at release rate 126. The level then falls 2.4 dB a millisecond
    // (CurveAdshr::CalcRelease). sfzformat.com's release falls 8 * 20 / ln(10) = 69.49 dB over its time. The game's
    // fade takes 28.953 ms for that. The 3DS's envelopes step 5 ms in a frame of 4.889 ms, and the time there scales to
    // 28.309 ms. A region without a key group writes none of the four opcodes.
    auto region_line = [](uint8_t key_group)
    {
        files::Region region;
        region.volume = 127;
        region.pan = 64;
        if (key_group != 0)
        {
            region.flags = uint32_t{key_group} << 8;
        }

        const files::VelocityTable velocities{files::VelocityTable::kDirect, 0, {{127, region}}};
        const files::KeyTable keys{files::KeyTable::kDirect, 0, {{127, velocities}}};
        std::vector<ArchiveFile> files(3);
        files[0].data = Fseq({0x3c, 0x64, 0x18, 0xff});
        files[1].data = files::Bank(console->settings.archive.flavor, {{{0x05000000, 0}}, {keys}});
        files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
        const SoundArchive archive =
            SoundArchive::Load(Fsar(files, {.sequences = {{0, 0, {0x03000000}}}, .banks = {1}, .wave_archives = {2}}),
                               console->settings.archive);
        BankSet banks(archive, archive.Sounds()[0], console->settings);
        MidiReport midi;
        midi.bend_ranges[0] = 2;

        const Performance p = Perform(archive, 0, banks, {});
        const SfzInstruments sfz =
            BuildSfz(p, midi, banks, "TITLE", std::vector<std::string>(p.presets.size(), "preset"));
        if (sfz.files.size() != 1)
        {
            return std::string();
        }

        const std::string& text = sfz.files[0].text;
        const std::size_t start = text.find("<region>");
        return start == std::string::npos ? std::string() : text.substr(start, text.find('\n', start) - start);
    };

    const std::string off_time = ByConsole<std::string>("0.028953", "0.028953", "0.028309");

    const std::string grouped = region_line(3);
    const std::string alone = region_line(0);

    XSFLIB_CHECK(grouped.find(" group=3 off_by=3 off_mode=time off_time=" + off_time) != std::string::npos);
    XSFLIB_CHECK(!alone.empty() && alone.find("group=") == std::string::npos &&
                 alone.find("off_") == std::string::npos);
}

// Builds the SFZ files of `perf` without banks, as BuildSfz does. On the Wii, the stereo waves play in pan mode 1.
SfzInstruments BuildSfzWithoutBanks(const Performance& perf, const MidiReport& midi, const std::string& title,
                                    const std::vector<std::string>& preset_names)
{
    if (OnWii())
    {
        const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(Fsar({}));
        RsarBankSet banks(archive, ntdlib::rsar::SoundInfo{}, console->settings);
        return ntdlib::sequence_export::BuildSfz(perf, midi, banks, title, preset_names, true, console->settings,
                                                 console->program);
    }

    const SoundArchive archive = SoundArchive::Load(Fsar({}), console->settings.archive);
    BankSet banks(archive, SoundInfo{}, console->settings);
    return BuildSfz(perf, midi, banks, title, preset_names);
}

void TestSfzCommentKeepsTheNameOnItsLine()
{
    // A newline in a sequence name could end its comment and inject SFZ opcodes. Test without banks. Any region would
    // then have come from the name.
    Performance p;
    p.presets.resize(1);
    TrackEvent preset;
    preset.kind = EventKind::kPreset;
    p.tracks[0] = {preset};
    MidiReport midi;
    midi.bend_ranges[0] = 2;

    const SfzInstruments sfz = BuildSfzWithoutBanks(p, midi, "A\r\n<region> sample=x.wav", {"b0 p0"});

    XSFLIB_CHECK(sfz.files.size() == 1);
    for (const SfzFile& f : sfz.files)
    {
        XSFLIB_CHECK(f.text.starts_with("// Track 0 of A  <region> sample=x.wav, converted by " + console->program));
        XSFLIB_CHECK(f.text.find("\n<region>") == std::string::npos);
    }
}

// length_cases.h's Code writes a sequence's variable-length numbers in MIDI's format. seqlib's AppendVlq writes the
// same format. length_cases.h also serves tests that don't link seqlib. The two writers therefore stay apart. A fix to
// one of them fails this check until the other matches.
void TestLengthCasesWriteVariableLengthNumbersAsSeqlib()
{
    using ntdlib::testing::length_cases::Code;

    // A wait is its command byte, then the number of ticks.
    const auto prologue = static_cast<std::ptrdiff_t>(Code(true).Bytes().size()) + 1;
    for (const uint32_t value :
         {0x0u, 0x7Fu, 0x80u, 0x3FFFu, 0x4000u, 0x1FFFFFu, 0x200000u, 0xFFFFFFFu, 0x10000000u, 0xFFFFFFFFu})
    {
        Code code(true);
        code.Wait(value);
        const std::vector<uint8_t> ours(code.Bytes().begin() + prologue, code.Bytes().end());
        std::vector<uint8_t> midi;
        seqlib::midi::AppendVlq(midi, value);
        XSFLIB_CHECK(ours == midi);
    }
}

// The tests that hold on every console, the Wii included.
const Test kEveryConsoleTests[] = {
    TestSoundFontWriterOutputIsValid,
    TestSoundFontWithoutSamplesHasSampleData,
    TestMidiWriterLayersMarkersAndBendRange,
    TestMidiWriterGivesPartsChannelsOfTheirOwn,
    TestMidiWriterHasAtLeast7680TicksPerQuarterNote,
    TestMidiWriterSplitsLongDeltaTimes,
    TestMidiBendUpByAWholeRangeIsExact,
    TestMidiEventsGoOnSoundFrames,
    TestMidiWriterSendsTheFilterToEveryLayer,
    TestMidiWriterListsChannel10Presets,
    TestSfzCommentKeepsTheNameOnItsLine,
    TestBankSoundFontHoldsEachInstrumentAtItsProgram,
    TestBankSfzFilesPlayEachInstrumentByItself,
    TestLoudBankIsAttenuatedAndAMissingBankThrows,
};

// The tests that hold on the Wii U, the Switch and the 3DS.
const Test kWiiUSwitchAndCtrTests[] = {
    TestSfzFileHasTheTracksPresetsAndFilters, TestSfzResonanceAmountFitsTheType,
    TestSfzKeyGroupFadesAtTheGamesRate,       TestDirectVelocityTableTakesVelocitiesAbove127,
    TestProgramSuffixPadsToTheLastProgram,    TestBankProgramsAbove127TakeTheNextBank,
    TestLargeBankSplitsItsSoundFont,          TestLargeSequenceKeepsThePlayedZones,
};

// The tests of the Wii U alone.
const Test kWiiUOnlyTests[] = {
    TestSfzRegionFollowsLinearInterpolation,
};

} // namespace

int main()
{
    RunOn(WiiU(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests, kWiiUOnlyTests}));
    RunOn(Switch(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests}));
    RunOn(Ctr(), Join({kEveryConsoleTests, kWiiUSwitchAndCtrTests}));
    RunOn(Wii(), kEveryConsoleTests);
    TestLengthCasesWriteVariableLengthNumbersAsSeqlib();

    return xsflib::testing::Summary("sequence_export_files");
}
