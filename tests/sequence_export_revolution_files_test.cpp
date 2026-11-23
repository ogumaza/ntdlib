// SPDX-License-Identifier: MIT

// Tests of sequence_export on the Wii's engine (Engine::kRevolution), with the Wii's settings (Wii() in
// sequence_export_test.h) and synthetic RSAR archives (sequence_export_revolution_test.h): the SFZ files, pieces of
// several sequences, and the banks.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/midi/performance.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/midi/reader.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/soundfont.h"
#include "seqlib/sf2/units.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/arrangements.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_build.h"
#include "ntdlib/sound_pieces.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "sequence_export/wii_pan.h"
#include "sequence_export_revolution_test.h"
#include "sequence_export_test.h"

using ntdlib::sequence_export::ArrangedSound;

using namespace ntdlib::sequence_export::wii_tests;

namespace
{

// Plays the first `n` samples of `data`, and loops over [ls, le).
std::vector<int16_t> Play(const std::vector<int16_t>& data, uint32_t ls, uint32_t le, std::size_t n)
{
    std::vector<int16_t> out;
    for (std::size_t i = 0; out.size() < n; i++)
    {
        out.push_back(data[i < le ? i : ls + (i - le) % (le - ls)]);
    }

    return out;
}

void TestSfzLoopUnrollingKeepsTheStream()
{
    // At 32000 Hz, 4 ms is 128 samples. A four-sample loop starting at 2 gets 32 copies of itself (128 samples) before
    // it, 16 within it (64 samples) and 64 samples after it. It plays the same. The data before the loop's two ends is
    // the same. sfizz's crossfade is then inaudible.
    Pcm pcm;
    pcm.sample_rate = 32000;
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 6;
    pcm.channels = {{10, 11, 20, 21, 22, 23}};

    const PreparedWave w = PrepareForSfz(pcm, seqlib::SfzLoopCrossfade(pcm.sample_rate));

    const auto& s = w.channels[0];
    XSFLIB_CHECK(w.loop && w.loop_start == 130 && w.loop_end == 194 && s.size() == 258);
    XSFLIB_CHECK(Play(s, w.loop_start, w.loop_end, 400) == Play(pcm.channels[0], 2, 6, 400));
    int different = 0;
    for (uint32_t i = 1; i <= 128; i++)
    {
        different += s[w.loop_start - i] != s[w.loop_end - i];
    }

    XSFLIB_CHECK(different == 0);
}

// The initialAttenuation of each sample zone of the SoundFont `data`, by the zone's lowest key and its pan generator. A
// zone without the generator has 0.
std::map<std::pair<int, int>, std::set<int>> ZoneAttenuations(const std::vector<uint8_t>& data)
{
    const sf2::Records v = sf2::Read(data);
    std::map<std::pair<int, int>, std::set<int>> attenuations;
    for (std::size_t z = 0; z + 1 < v.Count("ibag", 4); z++)
    {
        const std::map<uint16_t, uint16_t> gens = v.ZoneGens(z);
        if (!gens.contains(sf2::kSampleId))
        {
            continue;
        }

        const auto find = [&gens](uint16_t generator)
        {
            const auto found = gens.find(generator);
            return found == gens.end() ? 0 : static_cast<int>(static_cast<int16_t>(found->second));
        };
        const int low_key = gens.contains(sf2::kKeyRange) ? gens.at(sf2::kKeyRange) & 0xff : 0;
        attenuations[{low_key, find(sf2::kPan)}].insert(find(sf2::kInitialAttenuation));
    }

    return attenuations;
}

// The largest magnitude of each sample of the SoundFont `data`, in order.
std::vector<int> SamplePeaks(const std::vector<uint8_t>& data)
{
    const sf2::Records v = sf2::Read(data);
    std::vector<int> peaks;
    for (std::size_t s = 0; s + 1 < v.Count("shdr", sf2::kSampleHeaderSize); s++)
    {
        peaks.push_back(v.SamplePeak(s));
    }

    return peaks;
}

void TestBalanceModeAttenuatesTheZonesOfStereoWaves()
{
    // Keys 0 to 41 play a stereo wave at a pan of 0, keys 42 to 84 at full left, and keys 85 to 127 at full right. In
    // balance mode each channel stays on its side, at pan -500 or 500. The SoundFont's samples keep the wave's points.
    // Each zone takes the level that WaveChannelPan gives its channel as attenuation, 2.5 times over
    // (ToZoneAttenuation). At a pan of 0, each channel plays 3.01 dB down: 30.1 cB, held as 75. At full left, the left
    // channel plays at full level and the right is silent: 144 dB, held as the generator's limit of 1440. Full right
    // is the reverse. In dual mode, every zone plays at full level.
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq({0x3c, 0x64, 0x30, 0xff});
    files[1].data =
        Rbnk({TestInstrument{.regions = {TestRegion{.pan = 64}, TestRegion{.pan = 1}, TestRegion{.pan = 127}},
                             .upper_keys = {41, 84, 127}}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 2, true)});
    const SoundArchive archive =
        SoundArchive::Load(Rsar(files, {.sequences = {{.pan_mode = 1}, {.pan_mode = 0}}, .banks = {1}}));

    const Conversion balance = ConvertSequence(archive, 0);
    const Conversion dual = ConvertSequence(archive, 1);

    const std::map<std::pair<int, int>, std::set<int>> kBalance = {{{0, -500}, {75}},    {{0, 500}, {75}},
                                                                   {{42, -500}, {0}},    {{42, 500}, {1440}},
                                                                   {{85, -500}, {1440}}, {{85, 500}, {0}}};
    XSFLIB_CHECK(ZoneAttenuations(balance.sf2) == kBalance);
    const std::vector<int> kPoints = {1000, 1000};
    XSFLIB_CHECK(SamplePeaks(balance.sf2) == kPoints && SamplePeaks(dual.sf2) == kPoints);
    for (const auto& [place, attenuations] : ZoneAttenuations(dual.sf2))
    {
        XSFLIB_CHECK(attenuations == std::set<int>{0});
    }
}

void TestSfzFileHasTheTracksPresetsAndFilter()
{
    // Track 2 plays presets 0 and 1 with low-pass argument 4 (48 Hz) and the biquad high-pass (type 2). The other
    // tracks play nothing, and get no MIDI channel or SFZ file. With no banks in the archive, the presets have no
    // regions.
    const SoundArchive archive = SoundArchive::Load(Rsar({}));
    BankSet banks(archive, SoundInfo{});
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

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "TITLE", {"first", "second"}, true);

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

    // The low-pass is filter 1. Controller value 60 gives sfizz's cutoff for AX's at 48 Hz, 10440 cents below 20 kHz,
    // and 0 leaves the filter open.
    XSFLIB_CHECK(low_pass.value == 60);
    XSFLIB_CHECK(has("fil_type=lpf_1p cutoff=1000 cutoff_oncc103=9600 cutoff_curvecc103=8\n"));
    XSFLIB_CHECK(has("<curve> curve_index=8\nv000=0.540241 ") && has(" v060=-0.547259 "));

    // Each preset gets a master. Notes starting with the biquad controller above 0 use the high-pass in filter slot 2.
    XSFLIB_CHECK(has("// first\n<master> loprog=0 hiprog=0\n") && has("// second\n<master> loprog=1 hiprog=1\n"));
    XSFLIB_CHECK(
        has("<group> hicc104=0\n<group> locc104=1 fil2_type=hpf_2p cutoff2=1000 cutoff2_oncc104=9600 "
            "cutoff2_curvecc104=9 resonance2=0 resonance2_oncc104=40 resonance2_curvecc104=10 "
            "volume_oncc104=2 volume_curvecc104=11"));
}

void TestSfzCommentKeepsTheNameOnItsLine()
{
    // A line break in a sequence's name could end its comment and start SFZ opcodes. Without banks, any region would
    // have come from the name.
    const SoundArchive archive = SoundArchive::Load(Rsar({}));
    BankSet banks(archive, SoundInfo{});
    Performance p;
    p.presets.resize(1);
    TrackEvent preset;
    preset.kind = EventKind::kPreset;
    p.tracks[0] = {preset};
    MidiReport midi;
    midi.bend_ranges[0] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "A\r\n<region> sample=x.wav", {"b0 p0"}, true);

    XSFLIB_CHECK(sfz.files.size() == 1);
    for (const SfzFile& f : sfz.files)
    {
        XSFLIB_CHECK(f.text.starts_with("// Track 0 of A  <region> sample=x.wav, converted by xyz2midi"));
        XSFLIB_CHECK(f.text.find("\n<region>") == std::string::npos);
    }
}

// The ticks, keys and velocities of the note-ons of track `t` of piece `p`, with the fractions of a tick that they come
// after their ticks (TrackEvent::after).
std::vector<std::tuple<double, int, int>> PieceNoteOns(const Performance& p, int t)
{
    std::vector<std::tuple<double, int, int>> notes;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            notes.emplace_back(e.tick + e.after, e.key, e.value);
        }
    }

    return notes;
}

// The note id of the first note-on of track `t` of `p`, or 0 without one.
uint32_t FirstNoteId(const Performance& p, int t)
{
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            return e.note;
        }
    }

    return 0;
}

// Sequence A plays key 60 on track 0. B's track 0 opens track 1 and waits. Track 1 plays key 64 a tick later. C plays
// nothing.
const std::vector<std::vector<uint8_t>> kPieceOfTwoTracks = {
    {0x3c, 0x64, 0x30, 0xff},
    {0x88, 0x01, 0x00, 0x00, 0x08, 0x80, 0x31, 0xff, 0x80, 0x01, 0x40, 0x64, 0x30, 0xff},
    {0x80, 0x30, 0xff},
};

void TestPieceKeepsEachSequencesTracksWithNotes()
{
    // The piece holds A's track 0 and B's track 1, in that order, named after their sequences and tracks. B's note
    // takes another note id than A's. Both notes play program 0 with the same settings. They share one preset. The
    // piece ends with B, a tick after A.
    const SoundArchive archive = SequenceArchive(kPieceOfTwoTracks, {.track_flags = 3, .names = {"A", "B", "C"}});

    const Performance first = PerformSound(archive, 0);
    const Performance second = PerformSound(archive, 1);
    const Performance p = PerformTogether(archive, {0, 1, 2});

    const std::vector<std::tuple<uint32_t, int, int>> kFirst = {{0, 60, 100}};
    const std::vector<std::tuple<uint32_t, int, int>> kSecond = {{1, 64, 100}};
    const auto empty = [](const std::vector<TrackEvent>& events)
    {
        return events.empty();
    };
    XSFLIB_CHECK(NoteOns(p, 0) == kFirst && NoteOns(p, 1) == kSecond &&
                 std::all_of(p.tracks.begin() + 2, p.tracks.end(), empty));
    XSFLIB_CHECK(p.track_names[0] == "A Track 0" && p.track_names[1] == "B Track 1" && p.track_names[2].empty());
    XSFLIB_CHECK(FirstNoteId(p, 0) != FirstNoteId(p, 1) && p.presets.size() == 1);
    XSFLIB_CHECK(p.end_tick == second.end_tick && second.end_tick == first.end_tick + 1);
    XSFLIB_CHECK(!p.loop && p.loops == 0 && p.warnings.empty());
}

void TestPieceLoopsOnThePassThatEverySequenceRepeats()
{
    // A plays key 60 for 960 ticks, 10 s, and loops. B plays key 62 for 960 ticks and then loops key 64 for 960 ticks.
    // A marks its loop on its first pass, B on its second. The piece marks B's. A then plays its second pass beside
    // B's, and plays on to the piece's end.
    const SoundArchive archive =
        SequenceArchive({{0x3c, 0x64, 0x87, 0x40, 0x89, 0x00, 0x00, 0x00},
                         {0x3e, 0x64, 0x87, 0x40, 0x40, 0x64, 0x87, 0x40, 0x89, 0x00, 0x00, 0x04}},
                        {.names = {"A", "B"}});

    const Performance alone = PerformSound(archive, 0);
    const Performance p = PerformTogether(archive, {0, 1});

    const std::vector<std::tuple<uint32_t, int, int>> kFirst = {{0, 60, 100}, {960, 60, 100}};
    const std::vector<std::tuple<uint32_t, int, int>> kSecond = {{0, 62, 100}, {960, 64, 100}};
    XSFLIB_CHECK(alone.loop && alone.loop->start == 0 && alone.loops == 1 && alone.end_tick == 960);
    XSFLIB_CHECK(p.loop && p.loop->start == 960 && p.loop->end == 1920 && p.loops == 1 && p.end_tick == 1920);
    XSFLIB_CHECK(NoteOns(p, 0) == kFirst && NoteOns(p, 1) == kSecond);
}

void TestPieceLoopsAfterItsSequencesThatEnd()
{
    // A loops key 60 every 960 ticks. B holds key 62 for 1000 ticks and ends. The pass of A that B's note reaches into
    // doesn't repeat. The piece marks the pass after it, and A plays three passes.
    const SoundArchive archive =
        SequenceArchive({{0x3c, 0x64, 0x87, 0x40, 0x89, 0x00, 0x00, 0x00}, {0x3e, 0x64, 0x87, 0x68, 0xff}});

    const Performance p = PerformTogether(archive, {0, 1});

    const std::vector<std::tuple<uint32_t, int, int>> kFirst = {{0, 60, 100}, {960, 60, 100}, {1920, 60, 100}};
    XSFLIB_CHECK(p.loop && p.loop->start == 1920 && p.loop->end == 2880 && p.loops == 1 && p.end_tick == 2880);
    XSFLIB_CHECK(NoteOns(p, 0) == kFirst && NoteEvents(p, 1).back().first == 1000);
}

void TestPieceWhoseLoopsDontMeetHasNoLoop()
{
    // A loops every 960 ticks, B every 480. The piece has no loop markers, and says why.
    const SoundArchive archive = SequenceArchive(
        {{0x3c, 0x64, 0x87, 0x40, 0x89, 0x00, 0x00, 0x00}, {0x40, 0x64, 0x83, 0x60, 0x89, 0x00, 0x00, 0x00}});

    const Performance p = PerformTogether(archive, {0, 1});

    const std::vector<std::string> kWarnings = {
        "the sequences loop at different places: the piece has no loop markers"};
    XSFLIB_CHECK(!p.loop && p.loops == 0 && p.warnings == kWarnings && p.end_tick == 960);
}

void TestPieceTakesTheTempoOfItsFirstSequence()
{
    // A plays at tempo 150, B at tempo 180. B's note at tick 48 is due at 333.3 ms. It sounds at the start of the frame
    // that runs the tick, at 333 ms. In A's ticks, that's 39.96. The note goes there, between two ticks. NW4R SND times
    // the ticks in single precision, a little off the frame.
    const SoundArchive archive = SequenceArchive(
        {{0xe1, 0x00, 0x96, 0x3c, 0x64, 0x60, 0xff}, {0xe1, 0x00, 0xb4, 0x80, 0x30, 0x40, 0x64, 0x30, 0xff}});

    const Performance p = PerformTogether(archive, {0, 1});

    const std::vector<std::tuple<double, int, int>> notes = PieceNoteOns(p, 1);
    const double ms = notes.empty() ? 0.0 : std::get<0>(notes[0]) * 60000.0 / (150.0 * 48.0);
    XSFLIB_CHECK(p.tempo.size() == 1 && p.tempo[0].tick == 0 && p.tempo[0].bpm == 150.0);
    XSFLIB_CHECK(notes.size() == 1 && std::fabs(ms - 111 * kFrameMs) < 1e-3);
    XSFLIB_CHECK(NoteOns(p, 0) == (std::vector<std::tuple<uint32_t, int, int>>{{0, 60, 100}}));
}

void TestPieceSharesOneLevelScale()
{
    // A's note requires CC11 252 (TestLevelTooLoudForMidiIsCut). B's note at full level requires CC11 127. B sends
    // that alone. In the piece, every CC11 scales by the same amount to fit A's. B's then becomes 64. The tracks keep
    // their balance.
    const SoundArchive archive =
        SequenceArchive({{0xc1, 0x40, 0xd5, 0xff, 0x3c, 0x7f, 0x30, 0xff}, {0x3c, 0x7f, 0x30, 0xff}});

    const Performance alone = PerformSound(archive, 1);
    const Performance p = PerformTogether(archive, {0, 1});

    const std::vector<std::pair<uint32_t, int>> kFull = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kScaled = {{0, 64}};
    XSFLIB_CHECK(Controls(alone, EventKind::kExpression) == kFull && alone.level_cut_db == 0.0);
    XSFLIB_CHECK(Controls(p, EventKind::kExpression, 0) == kFull && Controls(p, EventKind::kExpression, 1) == kScaled);
    XSFLIB_CHECK(std::fabs(p.level_cut_db - 40.0 * std::log10(127.0 / 64.0)) < 1e-9);
}

void TestPieceSharesMidisChannels()
{
    // Each of A's 8 tracks releases a key with a slow release and changes the volume during the release
    // (TestWhatMidiCantSeparateIsReported). Then it plays another key at the new volume. Alone, that note goes to
    // another part, with a channel of its own. B plays a note on each of its 8 tracks. In the piece, the 16 tracks take
    // every channel. MIDI then can't keep A's new volumes away from its releases.
    std::vector<uint8_t> releases;
    std::vector<uint8_t> notes;
    for (uint8_t t = 1; t < 8; t++)
    {
        releases.insert(releases.end(), {0x88, t, 0x00, 0x00, 7 * 5});
        notes.insert(notes.end(), {0x88, t, 0x00, 0x00, 7 * 5});
    }
    releases.insert(releases.end(), {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01,
                                     0x80, 0x01, 0xc1, 0x40, 0x3e, 0x64, 0x30, 0x80, 0x30, 0xff});
    notes.insert(notes.end(), {0x3c, 0x64, 0x30, 0xff});
    const SoundArchive archive = SequenceArchive({releases, notes}, {.track_flags = 0xff});

    const Performance alone = PerformSound(archive, 0);
    const Performance p = PerformTogether(archive, {0, 1});
    const Conversion c = ConvertTogether(archive, {0, 1}, "Piece");

    const std::string kDetached = "detached notes (still receive track controller changes in MIDI)";
    const std::vector<int> kAloneParts = {0, 1};
    const std::vector<int> kPieceParts = {0, 0};
    XSFLIB_CHECK(!alone.approximations.contains(kDetached) && NoteParts(alone) == kAloneParts);
    XSFLIB_CHECK(p.approximations.contains(kDetached) && NoteParts(p) == kPieceParts);
    XSFLIB_CHECK(c.channels == 16 && ntdlib::testing::CheckConversion(c, "Piece") == 0);
}

void TestPieceConvertsToOneSetOfFiles()
{
    // The piece's MIDI tracks and SFZ files take the names of its tracks. The SoundFont's comment lists the sequences.
    const SoundArchive archive = SequenceArchive(kPieceOfTwoTracks, {.track_flags = 3, .names = {"A", "B", "C"}});

    const Conversion c = ConvertTogether(archive, {0, 1}, "Piece");

    const std::vector<std::string> tracks = seqlib::midi::TrackNames(seqlib::midi::Read(c.midi));
    std::vector<std::string> files;
    for (const SfzFile& f : c.sfz.files)
    {
        files.push_back(f.name);
    }
    const sf2::Records records = sf2::Read(c.sf2);
    const auto comment = records.chunks.find("ICMT");
    const std::vector<std::string> kTracks = {"Piece", "A Track 0", "B Track 1"};
    const std::vector<std::string> kFiles = {"A Track 0.sfz", "B Track 1.sfz"};
    XSFLIB_CHECK(ntdlib::testing::CheckConversion(c, "Piece") == 0 && c.channels == 2);
    XSFLIB_CHECK(tracks == kTracks && files == kFiles);
    XSFLIB_CHECK(comment != records.chunks.end() &&
                 Text(comment->second).starts_with("Converted by xyz2midi from sequences A and B together."));
}

void TestSoundThatStartsAPieceConvertsWithThePiece()
{
    // A and B form a piece. A converts together with B. B starts no piece and converts alone. An arrangement of A
    // converts alone too.
    const SoundArchive archive = SequenceArchive(kPieceOfTwoTracks, {.track_flags = 3, .names = {"A", "B", "C"}});
    const std::vector<ntdlib::SoundPiece> pieces = {{"Piece", {0, 1}}};
    const auto convert = [&archive, &pieces](const ArrangedSound& at)
    {
        const PerformOptions kOptions;

        return ntdlib::sequence_export::ConvertSoundOrPiece(archive, at, pieces, kOptions,
                                                            SettingsFor(archive, kOptions), Wii().program);
    };

    const Conversion first = convert({0, std::nullopt});
    const Conversion second = convert({1, std::nullopt});
    const Conversion arranged = convert({0, 0});

    const Conversion together = ConvertTogether(archive, {0, 1}, "Piece");
    const Conversion a = ConvertSequence(archive, 0);
    const Conversion b = ConvertSequence(archive, 1);
    XSFLIB_CHECK(together.midi != a.midi && together.sf2 != a.sf2);
    XSFLIB_CHECK(first.midi == together.midi && first.sf2 == together.sf2);
    XSFLIB_CHECK(second.midi == b.midi && second.sf2 == b.sf2);
    XSFLIB_CHECK(arranged.midi == a.midi && arranged.sf2 == a.sf2);
}

void TestPieceOfMoreThan16TracksIsReported()
{
    // A plays a note on each of its 16 tracks, and B on one more.
    std::vector<uint8_t> all_tracks;
    for (uint8_t t = 1; t < 16; t++)
    {
        all_tracks.insert(all_tracks.end(), {0x88, t, 0x00, 0x00, 15 * 5});
    }
    all_tracks.insert(all_tracks.end(), {0x3c, 0x64, 0x30, 0xff});
    const SoundArchive archive = SequenceArchive({all_tracks, {0x3c, 0x64, 0x30, 0xff}}, {.track_flags = 0xffff});

    const std::string message = ErrorOf<xsflib::FormatError>([&] { PerformTogether(archive, {0, 1}); });

    XSFLIB_CHECK(message == "the sequences play notes on more tracks than MIDI's 16 channels");
}

void TestPieceRequiresOneBankAndPanMode()
{
    // A and B play one bank, but B's sound places stereo waves in another pan mode. A piece also requires a sequence.
    const std::vector<ArchiveFile> files = {
        {Rseq({0x3c, 0x64, 0x30, 0xff})},
        {Rseq({0x3c, 0x64, 0x30, 0xff})},
        {Rbnk({TestInstrument{{TestRegion{}}}}), Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)})},
    };
    const SoundArchive archive = SoundArchive::Load(
        Rsar(files, {.sequences = {{.file_id = 0}, {.file_id = 1, .pan_mode = 1}}, .banks = {2}, .names = {"A", "B"}}));
    BankSet banks(archive, archive.Sounds()[0]);
    const auto message = [&archive, &banks](const std::vector<uint32_t>& indices)
    {
        return ErrorOf<std::invalid_argument>(
            [&] { ntdlib::sequence_export::PerformTogether(archive, indices, banks, {}, kSettings); });
    };

    XSFLIB_CHECK(message({0, 1}) == "A and B differ in bank or pan mode");
    XSFLIB_CHECK(message({}) == "no sequences to perform together");
}

// Converts bank `bank` of `archive`, with its stereo waves as in a sound of pan mode `pan_mode`. If `build` is given,
// plays the bank as that build of NW4R SND does. Otherwise the archive's version gives the build.
BankConversion ConvertBank(const SoundArchive& archive, std::size_t bank, uint8_t pan_mode,
                           std::optional<SndBuild> build = std::nullopt)
{
    PerformOptions options;
    options.build = build;

    return ntdlib::sequence_export::ConvertBank(archive, bank, pan_mode, SettingsFor(archive, options), Wii().program);
}

// The archive of the tests of a bank's conversion. Its sequence plays program 0 at key 60, then program 2 at key 64.
// Program 1 of the bank plays nothing. Program 0 plays a mono wave. Program 2 plays a stereo wave, at its original
// key, volume, pan and envelope, with a hold. `volume` is program 2's volume. The first sound plays the sequence in
// balance mode, and the second in dual mode.
SoundArchive BankTestArchive(uint8_t volume = 100)
{
    TestRegion second;
    second.wave_index = 1;
    second.original_key = 64;
    second.volume = volume;
    second.pan = 40;
    second.attack = 120;
    second.decay = 100;
    second.sustain = 90;
    second.hold = 20;
    second.release = 110;
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq({0x81, 0x00, 0x3c, 0x64, 0x30, 0x81, 0x02, 0x40, 0x64, 0x30, 0xff});
    files[1].data = Rbnk({TestInstrument{}, TestInstrument{{{.wave_index = -1}}}, TestInstrument{{second}}});
    files[1].wave_data =
        Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true), Rwav(32, std::vector<int16_t>(32, -2000), 2, true)});

    return SoundArchive::Load(Rsar(files, {.sequences = {{.pan_mode = 1}, {.pan_mode = 0}}, .banks = {1}}));
}

// A bank's SoundFont holds each instrument that plays a region at its program. Each instrument's zones and samples
// are those of a sequence that plays the instrument with a track's starting settings, in the sequence's pan mode.
void TestBankSoundFontHoldsEachInstrumentAtItsProgram()
{
    const SoundArchive archive = BankTestArchive();

    for (const uint8_t pan_mode : {uint8_t{1}, uint8_t{0}})
    {
        const BankConversion bank = ConvertBank(archive, 0, pan_mode);
        const Conversion sequence = ConvertSequence(archive, pan_mode == 1 ? 0 : 1);

        XSFLIB_CHECK(bank.sound_fonts.size() == 1);
        if (bank.sound_fonts.size() != 1)
        {
            return;
        }

        const BankSoundFont& sound_font = bank.sound_fonts[0];
        const sf2::Records v = sf2::Read(sound_font.sf2);
        const sf2::Records w = sf2::Read(sequence.sf2);
        XSFLIB_CHECK(Sf2Problem(sound_font.sf2).empty() && bank.warnings.empty() && sound_font.samples == 3);
        XSFLIB_CHECK(sound_font.first == 0 && sound_font.last == 2 && v.Count("phdr", 38) == 3);
        XSFLIB_CHECK(std::string(reinterpret_cast<const char*>(v.Record("phdr", 38, 0))) == "Program 0");
        XSFLIB_CHECK(xsflib::LoadLe16(v.Record("phdr", 38, 0) + 20) == 0 &&
                     xsflib::LoadLe16(v.Record("phdr", 38, 0) + 22) == 0);
        XSFLIB_CHECK(std::string(reinterpret_cast<const char*>(v.Record("phdr", 38, 1))) == "Program 2");
        XSFLIB_CHECK(xsflib::LoadLe16(v.Record("phdr", 38, 1) + 20) == 2 &&
                     xsflib::LoadLe16(v.Record("phdr", 38, 1) + 22) == 0);
        for (const char* chunk : {"ibag", "igen", "imod", "shdr", "smpl"})
        {
            XSFLIB_CHECK(v.chunks.at(chunk) == w.chunks.at(chunk));
        }
    }
}

// A bank's SFZ files play each instrument by itself, under the bank's name and the program. Each file's regions and
// samples are those of a sequence's track file that plays the instrument with a track's starting settings.
void TestBankSfzFilesPlayEachInstrumentByItself()
{
    const SoundArchive archive = BankTestArchive();
    const std::string& name = archive.Banks()[0].name;

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

    const BankConversion bank = ConvertBank(archive, 0, 1);
    const Conversion sequence = ConvertSequence(archive, 0);

    XSFLIB_CHECK(bank.sfz.files.size() == 2 && sequence.sfz.files.size() == 1);
    if (bank.sfz.files.size() != 2 || sequence.sfz.files.size() != 1)
    {
        return;
    }

    const std::string& track = sequence.sfz.files[0].text;
    const std::string& first = bank.sfz.files[0].text;
    const std::string& second = bank.sfz.files[1].text;
    const std::string kGlobal = "amplitude_smoothcc11=0\n\n";
    XSFLIB_CHECK(bank.sfz.files[0].name == name + "_0.sfz" && bank.sfz.files[1].name == name + "_2.sfz");
    XSFLIB_CHECK(first.starts_with("// Program 0 of " + name + ", converted by " + Wii().program + ".\n\n"));
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

// Without a main.dol, an archive's RSAR version gives a dated build: one from June 2008 for version 1.4 or later, and
// otherwise one of 2007. A date of 0 would mean a build without a banner. SndBuildWithAx plays such a build as the
// build of October 2006. With a later AX, the build of 2007 plays by the rules of 2007.
void TestRsarVersionGivesADatedBuild()
{
    const SndBuild old_version = ntdlib::sequence_export::BuildOfRsarVersion(0x0102);
    const SndBuild new_version = ntdlib::sequence_export::BuildOfRsarVersion(0x0104);
    XSFLIB_CHECK(old_version.date == 20070101 && !old_version.October2006() && !old_version.SinceMarch2008());
    XSFLIB_CHECK(new_version.date == 20080620 && !new_version.October2006() && new_version.SinceJune2008());
    XSFLIB_CHECK(!ntdlib::SndBuildWithAx(old_version, ntdlib::AxBuild::kDecember2006).October2006());
    XSFLIB_CHECK(!ntdlib::SndBuildWithAx(new_version, ntdlib::AxBuild::kDecember2006).October2006());
}

// A bank plays its regions by the rules of the build that the conversion takes. The builds from March 2008 give a
// region its hold. Under each build, the bank's zones and SFZ regions are those of the sequence that the same build
// plays.
void TestBankFollowsTheBuild()
{
    const SoundArchive archive = BankTestArchive();
    PerformOptions earlier;
    earlier.build = SndBuild{.date = 20070608};

    const BankConversion later_bank = ConvertBank(archive, 0, 1);
    const BankConversion earlier_bank = ConvertBank(archive, 0, 1, earlier.build);
    const Conversion later_sequence = ConvertSequence(archive, 0);
    const Conversion earlier_sequence = ConvertSequence(archive, 0, earlier);

    // The generators of a SoundFont's instruments, and the region lines of SFZ files.
    const auto igen = [](const std::vector<uint8_t>& sf2)
    {
        return sf2::Read(sf2).chunks.at("igen");
    };
    const auto regions = [](const SfzInstruments& sfz)
    {
        std::string lines;
        for (const SfzFile& file : sfz.files)
        {
            for (std::size_t at = file.text.find("<region>"); at != std::string::npos;
                 at = file.text.find("<region>", at + 1))
            {
                lines += file.text.substr(at, file.text.find('\n', at) - at) + "\n";
            }
        }

        return lines;
    };
    XSFLIB_CHECK(later_bank.sound_fonts.size() == 1 && earlier_bank.sound_fonts.size() == 1);
    if (later_bank.sound_fonts.size() != 1 || earlier_bank.sound_fonts.size() != 1)
    {
        return;
    }
    XSFLIB_CHECK(igen(later_bank.sound_fonts[0].sf2) == igen(later_sequence.sf2));
    XSFLIB_CHECK(igen(earlier_bank.sound_fonts[0].sf2) == igen(earlier_sequence.sf2));
    XSFLIB_CHECK(igen(later_bank.sound_fonts[0].sf2) != igen(earlier_bank.sound_fonts[0].sf2));
    XSFLIB_CHECK(regions(later_bank.sfz) == regions(later_sequence.sfz));
    XSFLIB_CHECK(regions(earlier_bank.sfz) == regions(earlier_sequence.sfz));
    XSFLIB_CHECK(regions(later_bank.sfz) != regions(earlier_bank.sfz));
}

// A bank's stereo waves play as in a sound of the pan mode that the conversion takes. In balance mode, each channel
// plays 3 dB down: its zones take 30.1 cB, held as 75 (ToZoneAttenuation). In dual mode each channel plays at full
// level. The build of October 2006 has only dual mode. The SoundFont's samples keep the wave's points in every mode.
void TestBankStereoWavesFollowThePanMode()
{
    std::vector<ArchiveFile> files(1);
    files[0].data = Rbnk({TestInstrument{}});
    files[0].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 2, true)});
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.banks = {0}}));

    const BankConversion balance = ConvertBank(archive, 0, 1);
    const BankConversion dual = ConvertBank(archive, 0, 0);
    const BankConversion october = ConvertBank(archive, 0, 1, SndBuild{.october_2006 = true});

    const auto attenuations = [](const BankConversion& c)
    {
        std::set<int> found;
        for (const auto& [place, values] : ZoneAttenuations(c.sound_fonts.at(0).sf2))
        {
            found.insert(values.begin(), values.end());
        }

        return found;
    };
    XSFLIB_CHECK(attenuations(balance) == std::set<int>{75} && attenuations(dual) == std::set<int>{0} &&
                 attenuations(october) == std::set<int>{0});
    const std::vector<int> kPoints = {1000, 1000};
    XSFLIB_CHECK(SamplePeaks(balance.sound_fonts.at(0).sf2) == kPoints &&
                 SamplePeaks(dual.sound_fonts.at(0).sf2) == kPoints &&
                 SamplePeaks(october.sound_fonts.at(0).sf2) == kPoints);
    XSFLIB_CHECK(balance.sfz.files.size() == 1 && dual.sfz.files.size() == 1 && october.sfz.files.size() == 1);
    if (balance.sfz.files.size() == 1 && dual.sfz.files.size() == 1 && october.sfz.files.size() == 1)
    {
        XSFLIB_CHECK(balance.sfz.files[0].text != dual.sfz.files[0].text);
        XSFLIB_CHECK(october.sfz.files[0].text == dual.sfz.files[0].text);
    }
}

// A region above volume 127 attenuates every zone of the SoundFont, as in a sequence's SoundFont, and the conversion
// says so. A bank missing from the archive throws.
void TestLoudBankIsAttenuatedAndAMissingBankThrows()
{
    const SoundArchive archive = BankTestArchive(254);

    const BankConversion bank = ConvertBank(archive, 0, 1);
    const Conversion sequence = ConvertSequence(archive, 0);
    const bool threw = !ErrorOf<std::out_of_range>([&] { ConvertBank(archive, 1, 1); }).empty();

    XSFLIB_CHECK(bank.warnings ==
                 std::vector<std::string>{
                     "region volume exceeds 127 (SoundFont attenuated by 6.0 dB; SFZ keeps the original volume)"});
    XSFLIB_CHECK(sf2::Read(bank.sound_fonts.at(0).sf2).chunks.at("igen") == sf2::Read(sequence.sf2).chunks.at("igen"));
    XSFLIB_CHECK(threw);
}

// The tests on the Wii.
const Test kTests[] = {
    TestSfzLoopUnrollingKeepsTheStream,
    TestBalanceModeAttenuatesTheZonesOfStereoWaves,
    TestSfzFileHasTheTracksPresetsAndFilter,
    TestSfzCommentKeepsTheNameOnItsLine,
    TestPieceKeepsEachSequencesTracksWithNotes,
    TestPieceLoopsOnThePassThatEverySequenceRepeats,
    TestPieceLoopsAfterItsSequencesThatEnd,
    TestPieceWhoseLoopsDontMeetHasNoLoop,
    TestPieceTakesTheTempoOfItsFirstSequence,
    TestPieceSharesOneLevelScale,
    TestPieceSharesMidisChannels,
    TestPieceConvertsToOneSetOfFiles,
    TestSoundThatStartsAPieceConvertsWithThePiece,
    TestPieceOfMoreThan16TracksIsReported,
    TestPieceRequiresOneBankAndPanMode,
    TestBankSoundFontHoldsEachInstrumentAtItsProgram,
    TestBankSfzFilesPlayEachInstrumentByItself,
    TestBankFollowsTheBuild,
    TestRsarVersionGivesADatedBuild,
    TestBankStereoWavesFollowThePanMode,
    TestLoudBankIsAttenuatedAndAMissingBankThrows,
};

} // namespace

int main()
{
    RunOn(Wii(), kTests);

    return xsflib::testing::Summary("sequence_export_revolution_files");
}
