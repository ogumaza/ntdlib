// SPDX-License-Identifier: MIT

// Tests of the conversion of JAudio's sound data, without game data. The tests build small JAudio2 archives and JAudio1
// sequences in memory.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/jaudio/archive_games.h"
#include "ntdlib/sequence_export/jaudio/convert.h"
#include "ntdlib/sequence_export/jaudio/envelope.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/jaudio/instruments.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/jaudio/wave.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/sequence_export_check.h"

namespace
{

using namespace ntdlib::sequence_export::jaudio;
using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// The name that the conversions' messages and files give the program.
constexpr char kProgram[] = "xyz2midi";

namespace files = ntdlib::testing::jaudio_files;
using files::Bytes;

// The test game's wave archive, test.aw, by its name.
std::optional<std::vector<uint8_t>> ReadTestAw(const std::string& name)
{
    return name == "test.aw" ? std::optional(files::AwFile()) : std::nullopt;
}

// A test game: its archive and the banks and waves it plays.
struct TestGame
{
    explicit TestGame(const std::vector<uint8_t>& bank, const std::vector<std::vector<uint8_t>>& sequences)
        : archive(SoundArchive::Parse(files::StartUpArchive(bank, sequences))), banks(archive, ReadTestAw)
    {
    }

    SequenceSound Sound(uint32_t index) const
    {
        return {"S" + std::to_string(index), archive.Collection(), *archive.CollectionSequence(0, index), 1.0f};
    }

    SoundArchive archive;
    BankSet banks;
};

// Sequence data addresses commands from the start of the collection that holds it. The first sequence of a collection
// of one starts at kFirstSequence.
constexpr uint32_t kFirstSequence = 20;

// Where the tests put the commands of a track that the first track opens.
constexpr uint32_t kChild = kFirstSequence + 0x20;

class Sequence
{
public:
    explicit Sequence(uint32_t base = kFirstSequence) : base_(base)
    {
    }

    Sequence& Byte(uint32_t v)
    {
        b_.U8(v);

        return *this;
    }

    Sequence& OpenTrack(uint32_t track, uint32_t offset)
    {
        b_.U8(0xc1).U8(track).U24(offset);

        return *this;
    }

    Sequence& Timebase(uint32_t tb)
    {
        b_.U8(0xd8).U8(0x62).U16(tb);

        return *this;
    }

    Sequence& Tempo(uint32_t bpm)
    {
        b_.U8(0xe0).U16(bpm);

        return *this;
    }

    Sequence& Bank(uint32_t bank)
    {
        b_.U8(0xe2).U8(bank);

        return *this;
    }

    Sequence& Program(uint32_t program)
    {
        b_.U8(0xe3).U8(program);

        return *this;
    }

    Sequence& NoteOn(uint32_t key, uint32_t id, uint32_t velocity)
    {
        b_.U8(key).U8(id).U8(velocity);

        return *this;
    }

    Sequence& NoteOff(uint32_t id)
    {
        b_.U8(0x80 | id);

        return *this;
    }

    // A gate note of fewer than 128 ticks. Bit 7 of `flags` sweeps from the latest key, and bit 6 ties the next note to
    // this one.
    Sequence& Gate(uint32_t key, uint32_t flags, uint32_t velocity, uint32_t length)
    {
        b_.U8(key).U8(flags).U8(velocity).U8(length);

        return *this;
    }

    Sequence& LatestKey(uint32_t key)
    {
        b_.U8(0xb4).U8(key);

        return *this;
    }

    Sequence& Wait(uint32_t ticks)
    {
        b_.U8(0xf1).U8(ticks);

        return *this;
    }

    Sequence& Volume(uint32_t byte)
    {
        b_.U8(0xb8).U8(0).U8(byte);

        return *this;
    }

    // The effect send. It feeds the game's echo.
    Sequence& FxMix(uint32_t byte)
    {
        b_.U8(0xb8).U8(2).U8(byte);

        return *this;
    }

    // Oscillator `osc`'s scale, in 16383ths.
    Sequence& OscillatorScale(uint32_t osc, uint32_t scale)
    {
        b_.U8(0xe7).U8(osc).U16(scale);

        return *this;
    }

    // Oscillator `osc`'s table, at `offset`.
    Sequence& OscillatorTable(uint32_t osc, uint32_t offset)
    {
        b_.U8(0xe8).U8(osc).U24(offset);

        return *this;
    }

    // A point of an oscillator's table.
    Sequence& Point(uint32_t mode, uint32_t time, uint32_t value)
    {
        b_.U16(mode).U16(time).U16(value);

        return *this;
    }

    // Oscillator 0's four-point table: an attack, a hold, a decay to `sustain`, and the track's direct release.
    Sequence& SimpleAdsr(uint32_t attack, uint32_t hold, uint32_t decay, uint32_t sustain, uint32_t release)
    {
        b_.U8(0xe9).U16(attack).U16(hold).U16(decay).U16(sustain).U16(release);

        return *this;
    }

    Sequence& Jump(uint32_t offset)
    {
        b_.U8(0xc7).U24(offset);

        return *this;
    }

    Sequence& Finish()
    {
        b_.U8(0xff);

        return *this;
    }

    // Fill up to `offset` with JAudio2's empty command.
    Sequence& PadTo(uint32_t offset)
    {
        while (Here() < offset)
        {
            b_.U8(0xfe);
        }

        return *this;
    }

    uint32_t Here() const
    {
        return base_ + static_cast<uint32_t>(b_.Size());
    }

    std::vector<uint8_t> Data() const
    {
        return b_.Data();
    }

private:
    uint32_t base_ = 0;
    Bytes b_;
};

// Count `perf`'s events of `kind` on MIDI track `slot`.
int Count(const Performance& perf, std::size_t slot, EventKind kind)
{
    int n = 0;
    for (const TrackEvent& e : perf.tracks[slot])
    {
        n += e.kind == kind ? 1 : 0;
    }

    return n;
}

// Join the problems that one of seqlib's checkers reports, or return "" if there are none.
std::string Joined(const std::vector<std::string>& problems)
{
    std::string s;
    for (const std::string& p : problems)
    {
        s += (s.empty() ? "" : "; ") + p;
    }

    return s;
}

void TestDecodeWave()
{
    const SoundArchive archive = SoundArchive::Parse(files::StartUpArchive(files::BankV0File(0, 100), {}));
    const WaveSystem* ws = archive.FindWaveSystem(0);

    const std::optional<Pcm> pcm =
        ws != nullptr ? std::optional(DecodeWave(ws->waves.at(0), files::AwFile())) : std::nullopt;

    Check(pcm && pcm->Length() == 64 && pcm->loop && pcm->channels[0][8] == 0x4000, "PCM16 decodes big-endian");
}

void TestFindSound()
{
    const Bank v0 = Bank::Parse(files::BankV0File(7, 100), 0);
    const Bank v1 = Bank::Parse(files::BankV1File(129, 0, 600), 0);

    const std::optional<NoteSound> drum = FindSound(v0, 0xe4, 36, 100);
    const std::optional<NoteSound> low = FindSound(v1, 0, 60, 40);
    const std::optional<NoteSound> high = FindSound(v1, 0, 60, 100);

    Check(drum && drum->drum && drum->pitch == 2.0f && drum->volume == 0.5f, "a drum plays its region");
    Check(drum && std::abs(drum->pan - 1.0f) < 1e-6f && drum->direct_release == 200, "PER2 gives pan and release");
    Check(!FindSound(v0, 0xe4, 37, 100), "a drum set plays only its keys");
    Check(low && high && low->wave_id == 0 && high->wave_id == 1, "velocity regions choose the wave");
    Check(low && low->random, "a random effect is reported");
    Check(!FindSound(v1, 0, 65, 100), "keys above the last region play nothing");
}

void TestOscillator()
{
    // A linear attack to full over 60 units, then a hold, and a linear release over 30 units. Read the oscillator at
    // each step.
    Oscillator osc;
    osc.attack = {{0, 60, 0x7fff}, {14, 0, 0}};
    osc.release = {{0, 30, 0}, {15, 0, 0}};
    OscillatorState s;

    s.Start(osc);
    const float start = s.Value();
    s.Advance(30.0f);
    const float half = s.Value();
    s.Advance(30.0f);
    const bool holds = s.Holding();
    const float full = s.Value();

    s.Release(0, 0);
    s.Advance(15.0f);
    const bool releasing = s.Releasing();
    const float falling = s.Value();
    s.Advance(15.0f);

    Check(start == 0.0f, "an attack starts in silence");
    Check(std::abs(half - 0.5f) < 0.001f, "halfway through a linear attack is half the level");
    Check(holds && std::abs(full - 1.0f) < 0.001f, "the attack ends at its target and holds");
    Check(releasing && std::abs(falling - 0.5f) < 0.001f, "the release falls from the held level");
    Check(s.Stopped(), "the release stops at its end");
}

void TestOscillatorWithoutReleaseTable()
{
    // Without a release table, an oscillator stops at once. A direct release fades it. So does a game's default
    // release, 0x10 in Four Swords Adventures: linearly, over 16 units.
    Oscillator bare;
    bare.attack = {{0, 0, 0x7fff}, {14, 0, 0}};
    OscillatorState cut;
    OscillatorState fade;
    OscillatorState fallback;
    cut.Start(bare);
    fade.Start(bare);
    fallback.Start(bare);

    cut.Release(0, 0);
    fade.Release(100, 0);
    fallback.Release(0, 0x10);
    fallback.Advance(8.0f);
    const float halfway = fallback.Value();
    fallback.Advance(9.0f);

    Check(cut.Stopped(), "no release table stops at once");
    Check(!fade.Stopped(), "a direct release fades");
    Check(std::abs(halfway - 0.5f) < 0.001f, "the default release falls linearly");
    Check(fallback.Stopped(), "the default release stops at its end");
}

void TestEnvelopeFit()
{
    Oscillator osc;
    osc.attack = {{0, 0, 0x7fff}, {0, 600, 0x7fff}, {14, 0, 0}};
    osc.release = {{0, 600, 0}, {15, 0, 0}};

    const EnvelopeCurve curve = TraceEnvelope({&osc}, 0, 0);
    const Sf2Envelope e = ConvertEnvelope(curve);

    Check(curve.holds, "the curve holds");
    Check(e.attack == -12000 && e.sustain == 0, "an instant attack to full sustain");
    // A linear fall over a second reaches -20 dB at 0.9 s: 100 dB at 4.5 s.
    Check(std::abs(e.release - ToTimecents(4500.0, -12000, 8000)) <= 20, "a linear release fits at -20 dB");
}

void TestReleaseAfterDecayToSilence()
{
    // Two attacks that fall to silence over 3 s. One holds there and one stops. Their notes are released before then,
    // so the release table gives their release.
    Oscillator holds;
    holds.attack = {{0, 0, 0x7fff}, {0, 1800, 0}, {14, 0, 0}};
    holds.release = {{0, 600, 0}, {15, 0, 0}};
    Oscillator stops = holds;
    stops.attack.back().mode = 15;

    const Sf2Envelope held = ConvertEnvelope(TraceEnvelope({&holds}, 0, 0));
    const Sf2Envelope stopped = ConvertEnvelope(TraceEnvelope({&stops}, 0, 0));

    const int release = ToTimecents(4500.0, -12000, 8000);
    Check(std::abs(held.release - release) <= 20, "a curve that holds in silence keeps its release");
    Check(std::abs(stopped.release - release) <= 20, "a curve that stops keeps its release");
}

void TestPerformance()
{
    // The first track opens track 0. Track 0 plays two notes and finishes.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(64, 1, 80).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    bool second_at_48 = false;
    for (const TrackEvent& e : perf.tracks[0])
    {
        second_at_48 = second_at_48 || (e.kind == EventKind::kNoteOn && e.key == 64 && e.tick == 48);
    }
    Check(Count(perf, 0, EventKind::kNoteOn) == 2, "the track plays two notes");
    Check(Count(perf, 0, EventKind::kNoteOff) == 2, "and ends both");
    Check(!perf.loop, "a sequence that finishes doesn't loop");
    Check(perf.timebase == 48 && !perf.tempo.empty() && perf.tempo[0].bpm == 120.0, "the tempo is recorded");
    Check(std::abs(perf.seconds - 1.1) < 0.02, "two beats at 120 BPM and a release of 60 units last 1.1 s");
    Check(second_at_48, "the second note starts at tick 48");
}

void TestOscillatorScaleArguments()
{
    // Track 0 scales its pitch envelope by 1.1 with 0xe7, then plays one note. Twilight Princess's command table gives
    // 0xe7 a byte and a 16-bit value. A parser that reads no arguments for 0xe7 plays the bytes 01 46 66 as key 1.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).OscillatorScale(1, 0x4666).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    int notes = 0;
    for (std::size_t slot = 0; slot < perf.tracks.size(); slot++)
    {
        notes += Count(perf, slot, EventKind::kNoteOn);
    }
    Check(notes == 1, "0xe7 takes a byte and a 16-bit value");
}

// Track 0 runs `commands`, then plays key 60. Its volume table, at kTable, holds at full level. Return the performance
// and the zones of its first preset.
constexpr uint32_t kTable = kChild + 0x40;

std::pair<Performance, std::vector<PresetZone>> PerformWithTable(void (*commands)(Sequence&))
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0);
    commands(s);
    s.NoteOn(60, 1, 127).Wait(48).NoteOff(1).Finish().PadTo(kTable);
    s.Point(0, 0, 0x7fff).Point(14, 0, 0);
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);
    std::vector<PresetZone> zones = perf.presets.empty() ? std::vector<PresetZone>{} : ZonesOf(0, perf, game.banks);

    return {std::move(perf), std::move(zones)};
}

void TestOscillatorScale()
{
    // 0xe7 scales one of a track's oscillators, here the volume by 2. The scale applies once the track has set the
    // oscillator's table, whichever comes first. A simple ADSR sets oscillator 0 back to a scale of 1, and a table set
    // after it keeps that. Without a table, the note plays the instrument's oscillator, and the scale has no effect.
    const auto [after, after_zones] =
        PerformWithTable([](Sequence& s) { s.OscillatorTable(0, kTable).OscillatorScale(0, 32766); });
    const auto [before, before_zones] =
        PerformWithTable([](Sequence& s) { s.OscillatorScale(0, 32766).OscillatorTable(0, kTable); });
    const auto [adsr, adsr_zones] = PerformWithTable(
        [](Sequence& s) { s.OscillatorScale(0, 32766).SimpleAdsr(0, 0, 0, 0x7fff, 0).OscillatorTable(0, kTable); });
    const auto [bare, bare_zones] = PerformWithTable([](Sequence& s) { s.OscillatorScale(0, 32766); });

    // The level that the zone's envelope peaks at, in dB.
    const auto peak = [](const std::vector<PresetZone>& zones)
    {
        return zones.size() == 1 ? ConvertEnvelope(zones[0].envelope).peak_db : -1000.0;
    };
    const auto scale = [](const Performance& perf)
    {
        const int osc = perf.presets.size() == 1 ? perf.presets[0].volume_oscillator : -1;
        return osc >= 0 ? perf.oscillators[static_cast<std::size_t>(osc)].scale : -1.0f;
    };
    Check(scale(after) == 2.0f && std::abs(peak(after_zones) - 6.02) < 0.01,
          "a scale of 2 doubles the level of the track's oscillator");
    Check(scale(before) == 2.0f && std::abs(peak(before_zones) - 6.02) < 0.01, "a table keeps the scale set before it");
    Check(scale(adsr) == 1.0f && std::abs(peak(adsr_zones)) < 0.01, "a simple ADSR sets the scale back to 1");
    Check(bare.presets.size() == 1 && bare.presets[0].volume_oscillator == -1 && std::abs(peak(bare_zones)) < 0.01,
          "without a table, the scale leaves the instrument's oscillator alone");
}

void TestSweepAndTie()
{
    // Track 0 sweeps up an octave from the latest key over a beat, and ties the next note to it. That note sweeps on
    // from where the first ended, at half the velocity.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).LatestKey(60).Gate(72, 0xc0, 100, 48).Gate(72, 0x80, 50, 48).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    int key = -1;
    float highest = 0.0f;
    float last = 0.0f;
    bool rises = true;
    bool between = false;
    std::vector<int> expression;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            key = e.key;
        }
        else if (e.kind == EventKind::kPitch)
        {
            highest = std::max(highest, e.semitones);
            rises = rises && e.semitones >= last;
            last = e.semitones;
            between = between || (e.after > 0.0 && e.after < 1.0);
        }
        else if (e.kind == EventKind::kExpression)
        {
            expression.push_back(e.value);
        }
    }

    Check(Count(perf, 0, EventKind::kNoteOn) == 1 && key == 60, "the tie continues the note that starts at key 60");
    Check(std::abs(highest - 12.0f) < 0.01f && rises, "pitch bends sweep the note up an octave");
    Check(between, "the sweep moves between ticks");
    Check(expression.size() >= 2 && std::abs(expression.back() * 2 - expression.front()) <= 2,
          "CC11 halves with the tied note's velocity");

    // A tie that doubles the velocity starts the MIDI note at the higher velocity, with CC11 at half until the tie.
    Sequence rising;
    rising.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    rising.Bank(0).Program(0).Gate(60, 0x40, 50, 24).Gate(60, 0x00, 100, 24).Finish();
    TestGame rising_game(files::BankV0File(0, 60), {rising.Data()});

    const Performance up = Perform(rising_game.Sound(0), rising_game.banks, {}, kProgram);

    int velocity = 0;
    std::vector<int> levels;
    for (const TrackEvent& e : up.tracks[0])
    {
        velocity = e.kind == EventKind::kNoteOn ? e.value : velocity;
        if (e.kind == EventKind::kExpression)
        {
            levels.push_back(e.value);
        }
    }
    Check(velocity == 100 && up.level_cut_db == 0.0, "the note starts at the velocity its tie reaches");
    Check(levels.size() >= 2 && std::abs(levels.front() * 2 - levels.back()) <= 2, "and CC11 doubles at the tie");
}

void TestTieBelowHearing()
{
    // A note at velocity 50 falls to silence over a second. A tie then raises it to velocity 127. That velocity plays
    // another wave. The MIDI note can't start there. A quarter of a second in, the note can still be heard. CC11 rises
    // past 127 there. The whole sequence plays quieter. At 1.25 seconds, the note is below hearing. Its tie raises
    // nothing. A tie 50 ms into an attack of ten seconds comes before the note can be heard. That note still rises.
    auto perform = [](int16_t attack, uint32_t ticks)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).Gate(60, 0x40, 50, ticks).Gate(60, 0x00, 127, 24).Finish();
        TestGame game(files::BankV1File(0, attack, 600), {s.Data()});

        return Perform(game.Sound(0), game.banks, {}, kProgram);
    };

    const Performance heard = perform(0, 24);
    const Performance below = perform(0, 120);
    const Performance rising = perform(6000, 5);

    int velocity = 0;
    for (const TrackEvent& e : below.tracks[0])
    {
        velocity = e.kind == EventKind::kNoteOn ? e.value : velocity;
    }
    Check(heard.level_cut_db > 10.0, "a tie into another wave that can be heard lowers the sequence");
    Check(below.level_cut_db == 0.0, "a tie on a note below hearing lowers nothing");
    Check(Count(below, 0, EventKind::kNoteOn) == 1 && velocity == 50, "and its note starts at its first velocity");
    Check(rising.level_cut_db > 10.0, "a tie in an attack from silence still raises the note");
}

void TestGateTickRate()
{
    // The first track opens track 0, then halves the timebase. Track 0's first gate note starts in that tick, so its
    // length goes by the default tick rate: 24 ticks last as long as 12 at the new rate. The second note starts in a
    // later tick, at the new rate.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(24).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).Gate(60, 0x00, 100, 24).Gate(62, 0x00, 100, 24).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    std::vector<uint32_t> ends;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOff)
        {
            ends.push_back(e.tick);
        }
    }
    Check(ends.size() == 2 && ends[0] >= 11 && ends[0] <= 12,
          "a note in the tick that opens its track keeps the old rate");
    Check(ends.size() == 2 && ends[1] >= 47, "a later note takes the first track's rate");
}

void TestLoop()
{
    // Track 0 loops two notes for ever.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Wait(200);
    s.Jump(s.Here() - 4).PadTo(kChild);
    s.Bank(0).Program(0);
    const uint32_t loop = s.Here();
    s.NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(62, 1, 100).Wait(48).NoteOff(1).Jump(loop);
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    Check(perf.loop.has_value(), "an endless loop of notes is the main loop");
    Check(perf.loop && perf.loop->end - perf.loop->start == 96, "the loop is one pass of two beats");
    Check(perf.seconds >= 10.0, "a short loop plays for at least ten seconds");
}

void TestJumpToTheNextCommand()
{
    // Track 0 loops two notes of 25 ticks for ever. Between them, it jumps to the next command. That jump goes nowhere.
    // It ends no pass of a loop. The loop's passes start where the loop does.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Wait(200);
    s.Jump(s.Here() - 4).PadTo(kChild);
    s.Bank(0).Program(0);
    const uint32_t loop = s.Here();
    s.NoteOn(60, 1, 100).Wait(25).NoteOff(1);
    s.Jump(s.Here() + 4);
    s.NoteOn(62, 1, 100).Wait(25).NoteOff(1).Jump(loop);
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    Check(perf.loop && perf.loop->end - perf.loop->start == 50 && perf.loop->start % 50 == 0,
          "a jump to the next command ends no pass: the loop starts at its first note");
}

void TestPortsAndRegisters()
{
    // Track 0 reads its parent's port 0, which the game writes, into register 0; if it's 0 it plays key 60, otherwise
    // key 72.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).Byte(0xd6).Byte(0).Byte(0); // read the first track's port 0 into register 0
    const uint32_t jump = s.Here();
    s.Byte(0xc8).Byte(2).Byte(0).Byte(0).Byte(0); // jump if register 3 isn't 0 (target set below)
    s.NoteOn(60, 1, 100).Wait(10).NoteOff(1).Finish();
    const uint32_t other = s.Here();
    s.NoteOn(72, 1, 100).Wait(10).NoteOff(1).Finish();
    std::vector<uint8_t> data = s.Data();
    const uint32_t at = jump - kFirstSequence;
    data[at + 2] = static_cast<uint8_t>(other >> 16);
    data[at + 3] = static_cast<uint8_t>(other >> 8);
    data[at + 4] = static_cast<uint8_t>(other);
    TestGame game(files::BankV0File(0, 60), {data});
    PerformOptions options;
    options.ports[0] = 1;

    const Performance plain = Perform(game.Sound(0), game.banks, {}, kProgram);
    const Performance set = Perform(game.Sound(0), game.banks, options, kProgram);

    auto first_key = [](const Performance& p)
    {
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                return static_cast<int>(e.key);
            }
        }

        return -1;
    };

    Check(first_key(plain) == 60, "an unset port reads 0");
    Check(first_key(set) == 72, "--port sets a port before the sequence starts");
}

void TestDrumAndVolume()
{
    // Track 0 plays the drum set at volume 0x40.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0xe4).Volume(0x40).NoteOn(36, 1, 127).Wait(10).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);
    const std::vector<PresetZone> zones =
        perf.presets.empty() ? std::vector<PresetZone>{} : ZonesOf(0, perf, game.banks);

    int volume = -1;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kVolume)
        {
            volume = e.value;
        }
    }
    // 0x40 spreads to 0x4080: 0.5039 of 127.
    Check(volume == 64, "CC7 holds the track's volume");
    Check(perf.presets.size() == 1 && perf.presets[0].program == 0xe4, "the drum set is a preset");
    Check(zones.size() == 1 && zones[0].root_key == 36 && zones[0].low_key == 36, "a drum plays on its key");
    Check(zones.size() == 1 && std::abs(zones[0].cents - 1200.0) < 0.01, "a drum's pitch tunes its zone");
}

void TestTailsKeepTheirSettings()
{
    // Track 0 releases a note, lowers its volume 12 ticks later while the release still sounds, then plays a note.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Wait(12).Volume(0x20).NoteOn(64, 1, 100);
    s.Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 600), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    std::optional<uint8_t> first, second;
    bool tail_changed = false;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            (e.key == 60 ? first : second) = e.part;
        }
        tail_changed = tail_changed || (e.kind == EventKind::kVolume && e.tick > 48 && e.part == first);
    }
    Check(first && second && *first != *second, "the next note plays on another part while the release sounds");
    Check(!tail_changed, "the release keeps its volume");
}

void TestHeldNoteKeepsItsPart()
{
    // Track 0 plays two notes, releases one, and lowers its volume 12 ticks later while it still holds the other.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).NoteOn(64, 2, 100).Wait(48).NoteOff(1).Wait(12).Volume(0x20);
    s.Wait(48).NoteOff(2).Finish();
    TestGame game(files::BankV0File(0, 600), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    std::optional<uint8_t> first, second, volume;
    for (const TrackEvent& e : perf.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            (e.key == 60 ? first : second) = e.part;
        }
        if (e.kind == EventKind::kVolume && e.tick > 48)
        {
            volume = e.part;
        }
    }
    const std::string approximation =
        "released notes take later settings of their track while it holds a note on their part";
    Check(first && second && *first == *second, "both notes play on one part");
    Check(volume && volume == second, "the held note takes the track's volume");
    Check(perf.approximations.contains(approximation), "the release takes the volume too, as an approximation");
}

void TestDefaultKeepsEveryNote()
{
    // Nine tracks each hold seven notes from tick 0 to tick 48. The first track plays seven more from tick 12: 70 notes
    // at once, six more than the game's 64 voices.
    constexpr uint32_t kNotes = kFirstSequence + 0x80;
    Sequence s;
    for (uint32_t t = 0; t < 9; t++)
    {
        s.OpenTrack(t, kNotes);
    }
    s.Timebase(48).Tempo(120).Bank(0).Program(0).Wait(12);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOn(59 + id, id, 100);
    }
    s.Wait(48);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOff(id);
    }
    s.Finish().PadTo(kNotes);
    s.Bank(0).Program(0);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOn(59 + id, id, 100);
    }
    s.Wait(48);
    for (uint32_t id = 1; id <= 7; id++)
    {
        s.NoteOff(id);
    }
    s.Finish();
    TestGame game(files::BankV0File(0, 600), {s.Data()});
    PerformOptions game_voices;
    game_voices.voice_limit = 64;

    const Performance kept = Perform(game.Sound(0), game.banks, {}, kProgram);
    const Performance limited = Perform(game.Sound(0), game.banks, game_voices, kProgram);

    // The notes that end before tick 48, the earliest note-off.
    const auto cut_short = [](const Performance& perf)
    {
        int n = 0;
        for (const std::vector<TrackEvent>& track : perf.tracks)
        {
            for (const TrackEvent& e : track)
            {
                n += e.kind == EventKind::kNoteOff && e.tick < 48 ? 1 : 0;
            }
        }
        return n;
    };
    const std::string approximation = "notes cut when the game's 64 voices ran out";
    Check(cut_short(kept) == 0 && !kept.approximations.contains(approximation),
          "by default every note plays to its end");
    Check(cut_short(limited) == 6 && limited.approximations.contains(approximation),
          "with --voices 64, six notes are cut as in the game");
}

void TestConversion()
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Conversion c = ConvertSequence(game.Sound(0), game.banks, {}, kProgram);

    const std::string midi_problems = Joined(seqlib::midi::Problems(c.midi));
    const std::string sf2_problems = Joined(seqlib::sf2::Check(c.sf2));
    const seqlib::midi::File m = midi_problems.empty() ? seqlib::midi::Read(c.midi) : seqlib::midi::File{};
    Check(c.midi.size() > 14 && std::memcmp(c.midi.data(), "MThd", 4) == 0, "the MIDI file has a header");
    Check(
        c.sf2.size() > 12 && std::memcmp(c.sf2.data(), "RIFF", 4) == 0 && std::memcmp(c.sf2.data() + 8, "sfbk", 4) == 0,
        "the SoundFont is a RIFF sfbk");
    Check(c.samples == 1, "the SoundFont holds the one wave it plays");
    Check(c.sfz.files.size() == 1 && c.sfz.samples.size() == 1, "one SFZ file and its sample");
    Check(midi_problems.empty() && seqlib::midi::Notes(m).size() == 1,
          "an independent reader reads the MIDI file's note: " + midi_problems);
    Check(sf2_problems.empty(), "an independent reader accepts the SoundFont: " + sf2_problems);
    const std::string sfz_problem = ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(m));
    Check(sfz_problem.empty(), "the SFZ files match the MIDI file: " + sfz_problem);
}

// The instrument plays its wave, at 32000 Hz, at the wave's rate on key 60. On key 96, three octaves up, the DSP plays
// it just under its limit, a rate of 32767/4096 of the output's. On key 97, the DSP plays it at that limit. The zones
// of a preset that plays such a key stop below it. A zone for the keys from there up plays every key at the limit.
void TestPitchLimit()
{
    auto convert = [](uint32_t key)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).NoteOn(key, 1, 100).Wait(48).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});

        Conversion c = ConvertSequence(game.Sound(0), game.banks, {}, kProgram);
        std::vector<PresetZone> zones =
            c.performance.presets.empty() ? std::vector<PresetZone>{} : ZonesOf(0, c.performance, game.banks);
        return std::make_pair(std::move(c), std::move(zones));
    };
    const auto [below, below_zones] = convert(96);
    const auto [above, above_zones] = convert(97);

    // The SoundFont's zone for keys 97 to 127 plays every key at its tuning: a scaleTuning (generator 56) of 0.
    bool fixed_sf2 = false;
    const std::string sf2_problems = Joined(seqlib::sf2::Check(above.sf2));
    if (sf2_problems.empty())
    {
        const seqlib::sf2::Records sf = seqlib::sf2::Read(above.sf2);
        for (std::size_t z = 0; z + 1 < sf.Count("ibag", 4); z++)
        {
            const std::map<uint16_t, uint16_t> gens = sf.ZoneGens(z);
            const auto keys = gens.find(43);
            const auto scale = gens.find(56);
            fixed_sf2 = fixed_sf2 || (keys != gens.end() && keys->second == (97 | 127 << 8) && scale != gens.end() &&
                                      scale->second == 0);
        }
    }
    const std::string sfz = above.sfz.files.size() == 1 ? above.sfz.files[0].text : "";

    const double limit = 1200.0 * std::log2(32767.0 / 4096.0 * static_cast<double>(kDacRate) / 32000.0);
    Check(below.performance.pitch_limited_presets.empty() && below_zones.size() == 1 && !below_zones[0].fixed_pitch,
          "below the limit, one zone plays every key");
    Check(above.performance.pitch_limited_presets == std::set<uint32_t>{0} && above_zones.size() == 2 &&
              above_zones[0].low_key == 0 && above_zones[0].high_key == 96 && !above_zones[0].fixed_pitch,
          "a preset that plays a key at the limit stops its zone below that key");
    Check(above_zones.size() == 2 && above_zones[1].low_key == 97 && above_zones[1].high_key == 127 &&
              above_zones[1].fixed_pitch && std::abs(above_zones[1].cents - limit) < 0.01,
          "the keys above play the wave at the limit");
    Check(fixed_sf2, "the SoundFont's zone plays every key at the limit: " + sf2_problems);
    Check(sfz.find("lokey=97 hikey=127") != std::string::npos && sfz.find(" pitch_keytrack=0") != std::string::npos,
          "the SFZ region plays every key at the limit");
}

// The read of a wave archive fails, as on a truncated disc image. The sequence converts without the archive's waves,
// and the failure is reported. The archive isn't read again. A later note that requires it plays nothing either.
void TestWaveArchiveThatCannotBeRead()
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    const SoundArchive archive = SoundArchive::Parse(files::StartUpArchive(files::BankV0File(0, 60), {s.Data()}));
    int reads = 0;
    const auto truncated = [&reads](const std::string&) -> std::optional<std::vector<uint8_t>>
    {
        reads++;
        throw FormatError("read out of bounds at offset 4096");
    };
    BankSet banks(archive, truncated);
    const SequenceSound sound{"S0", archive.Collection(), *archive.CollectionSequence(0, 0), 1.0f};

    const std::string error = ErrorOf([&] { ConvertSequence(sound, banks, {}, kProgram); });
    const Pcm* later = banks.GetWave(0, 1);

    Check(error.empty(), "a wave archive that can't be read doesn't stop the sequence: " + error);
    Check(banks.Errors() == std::vector<std::string>{"wave archive test.aw: read out of bounds at offset 4096"},
          "the failed read is reported once: " + Joined(banks.Errors()));
    Check(later == nullptr && reads == 1, "the archive isn't read again");
}

// macOS writes an AppleDouble file, named "._" and the file's name, beside each file that it copies to a FAT or exFAT
// drive. The search for the sequence archive leaves those files out.
void TestFolderWithAppleDoubleFiles()
{
    namespace fs = std::filesystem;
    const fs::path folder =
        fs::temp_directory_path() / ("sequence_export_jaudio_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(folder / "Seqs");
    const files::SoundTableBlocks sounds = files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<uint8_t> music = Sequence(0).Tempo(120).Finish().Data();
    xsflib::WriteFile(folder / "test.baa", files::StartUpArchive(files::BankV0File(0, 60), {}, sounds));
    xsflib::WriteFile(folder / "Seqs" / "test.arc", files::RarcFile({{7, "test.bms", music}}));

    // An AppleDouble file starts with its magic number. Its name sorts before the name of the file it describes.
    xsflib::WriteFile(folder / "Seqs" / "._test.arc", Bytes().U32(0x00051607).U32(0x00020000).Data());

    const Game game = Game::FromFolder(folder / "test.baa", kProgram);

    Check(game.Warnings().empty(), "an AppleDouble file is no sequence archive: " + Joined(game.Warnings()));
    Check(game.Sequences().size() == 1 && game.Sequences()[0].name == "BGM_FIELD", "the music is in the real archive");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// Both releases of Four Swords Adventures play their system sounds, category 0, at 0.7, and release an oscillator
// without a release over 0x10. Their echo repeats a sound every 64 sub-frames. Another game's start-up archive gets the
// defaults.
void TestGameRules()
{
    const GameRules usa = RulesOf("8c55097f2cc9f5800fd8ab96da4e3b09727e0a39");
    const GameRules japan = RulesOf("aab07cea25ba957374524c4f29d36b72e6d36a67");
    const GameRules other = RulesOf(std::string(40, '0'));

    Check(usa.category_volumes == std::map<uint8_t, float>{{0, 0.7f}}, "the USA release plays category 0 at 0.7");
    Check(usa.default_release == 0x10, "the USA release releases a bare oscillator over 0x10");
    Check(usa.echo && usa.echo->delay == 64 && usa.echo->feedback == 0x3fff / 32768.0f && usa.echo->volume == 0.75f &&
              usa.echo->voice_level == 0.732421875f,
          "the USA release's echo has a delay of 64 sub-frames, a filter of 0x3fff and a volume of 0x6000");
    Check(japan.category_volumes == usa.category_volumes && japan.default_release == 0x10 && japan.echo &&
              japan.echo->delay == 64,
          "the Japanese release has the same rules");
    Check(other.category_volumes.empty() && other.default_release == 0 && !other.echo,
          "another game gets the defaults");
}

// A note that sends to Four Swords Adventures' echo lengthens its sequence by the repeats that the game plays at 12 or
// above. The test wave peaks at 16384. At a send of 22/127 and the centre's pan gain, the line takes in 1,470 a side.
// The first repeat plays at 0.375 of that, 551. The 6th plays at 17, and the 7th at 8.6. A note of 12 ticks lasts less
// than a delay. No repeat overlaps it. A note of 48 ticks lasts three delays. The line then holds the send and its
// earlier repeats. They add by power to 1.15 times the send. Its 6th repeat after the note is the last at 12 or above.
// Added as levels, they would give 1.875 times the send and a 7th repeat. A note without a send gets no echo.
void TestEchoLengthensTheEnd()
{
    const auto note = [](uint32_t send, uint32_t ticks)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).FxMix(send).NoteOn(60, 1, 127).Wait(ticks).NoteOff(1).Finish();

        return s.Data();
    };
    const auto repeats = [](const std::vector<uint8_t>& sequence)
    {
        TestGame game(files::BankV0File(0, 0), {sequence});
        SequenceSound with_echo = game.Sound(0);
        with_echo.echo = RulesOf("8c55097f2cc9f5800fd8ab96da4e3b09727e0a39").echo;
        const Performance dry = Perform(game.Sound(0), game.banks, {}, kProgram);
        const Performance wet = Perform(with_echo, game.banks, {}, kProgram);

        return (wet.seconds - dry.seconds) / (64 * kSubFrameSeconds);
    };

    const double short_note = repeats(note(22, 12));
    const double long_note = repeats(note(22, 48));
    const double unsent = repeats(note(0, 12));

    Check(std::abs(short_note - 6.0) < 0.05, "a short note's echo adds six repeats: " + std::to_string(short_note));
    Check(std::abs(long_note - 6.0) < 0.05, "a long note's echo adds six repeats: " + std::to_string(long_note));
    Check(unsent == 0.0, "a note without a send gets no echo");
}

// JAudio1's commands (JASystem::TSeqParser in The Wind Waker's main.dol), for the tests' sequences.
class Sequence1
{
public:
    explicit Sequence1(uint32_t base = kFirstSequence) : base_(base)
    {
    }

    Sequence1& Byte(uint32_t v)
    {
        b_.U8(v);

        return *this;
    }

    Sequence1& OpenTrack(uint32_t track, uint32_t offset)
    {
        b_.U8(0xc1).U8(track).U24(offset);

        return *this;
    }

    Sequence1& Timebase(uint32_t tb)
    {
        b_.U8(0xfe).U16(tb);

        return *this;
    }

    Sequence1& Tempo(uint32_t bpm)
    {
        b_.U8(0xfd).U16(bpm);

        return *this;
    }

    // Operation `operation` on register `reg` with a byte: 0 loads the byte, 1 adds it, and 3 compares the register
    // with it. A comparison sets the flag (register 3) to the register less the byte.
    Sequence1& Register(uint32_t operation, uint32_t reg, uint32_t byte)
    {
        b_.U8(0xa4 | operation).U8(reg).U8(byte);

        return *this;
    }

    // The bank and the program are registers 0x20 and 0x21.
    Sequence1& Bank(uint32_t bank)
    {
        return Register(0, 0x20, bank);
    }

    Sequence1& Program(uint32_t program)
    {
        return Register(0, 0x21, program);
    }

    // A gate note of `length` ticks, fewer than 256, that sounds for `share` percent of them. The track waits for the
    // length.
    Sequence1& Gate(uint32_t key, uint32_t velocity, uint32_t share, uint32_t length)
    {
        b_.U8(key).U8(0x08).U8(velocity).U8(share).U8(length);

        return *this;
    }

    // A note of id `id`, 1 to 7, that sounds until a note-off of the id.
    Sequence1& NoteOn(uint32_t key, uint32_t id, uint32_t velocity)
    {
        b_.U8(key).U8(id).U8(velocity);

        return *this;
    }

    Sequence1& NoteOff(uint32_t id)
    {
        b_.U8(0x80 | id);

        return *this;
    }

    Sequence1& Wait(uint32_t ticks)
    {
        b_.U8(0x80).U8(ticks);

        return *this;
    }

    // A jump to `offset` on condition `condition`: 0 always, 1 if the flag is 0, and 2 if it isn't.
    Sequence1& Jump(uint32_t condition, uint32_t offset)
    {
        b_.U8(0xc8).U8(condition).U24(offset);

        return *this;
    }

    Sequence1& Finish()
    {
        b_.U8(0xff);

        return *this;
    }

    // Fill up to `offset` with JAudio1's empty command.
    Sequence1& PadTo(uint32_t offset)
    {
        while (Here() < offset)
        {
            b_.U8(0xfc);
        }

        return *this;
    }

    uint32_t Here() const
    {
        return base_ + static_cast<uint32_t>(b_.Size());
    }

    std::vector<uint8_t> Data() const
    {
        return b_.Data();
    }

private:
    uint32_t base_ = 0;
    Bytes b_;
};

// Play `sequence` as JAudio1's player does, with the test bank.
Performance PerformJAudio1(const std::vector<uint8_t>& sequence)
{
    TestGame game(files::BankV0File(0, 60), {sequence});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;

    return Perform(sound, game.banks, {}, kProgram);
}

// The ticks of the note-ons and note-offs of `key` on MIDI track `slot`.
std::vector<uint32_t> NoteTicks(const Performance& perf, std::size_t slot, uint32_t key)
{
    std::vector<uint32_t> ticks;
    for (const TrackEvent& e : perf.tracks[slot])
    {
        if ((e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff) && e.key == key)
        {
            ticks.push_back(e.tick);
        }
    }

    return ticks;
}

// The first track opens track 0, sets the timebase and the tempo, and waits. Track 0 plays a gate note of 48 ticks that
// sounds for half of them, then a note of id 1 for 24 ticks. JASystem::TSeqParser::cmdNoteOn makes the track wait for
// a gate note's length. The note sounds for the share of it. Its time runs out in DSP sub-frames. Its end can then fall
// in the last sub-frame of the tick before.
void TestJAudio1Notes()
{
    Sequence1 s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).Gate(60, 100, 50, 48).NoteOn(64, 1, 80).Wait(24).NoteOff(1).Finish();

    const Performance perf = PerformJAudio1(s.Data());

    Check(perf.timebase == 48 && !perf.tempo.empty() && perf.tempo[0].bpm == 120.0, "the timebase and the tempo");
    const std::vector<uint32_t> gate = NoteTicks(perf, 0, 60);
    Check(gate.size() == 2 && gate[0] == 0 && gate[1] >= 23 && gate[1] <= 24,
          "the gate note sounds for half its 48 ticks");
    Check(NoteTicks(perf, 0, 64) == std::vector<uint32_t>{48, 72}, "the next note starts when the gate's wait ends");
    Check(!perf.loop, "a sequence that finishes doesn't loop");
}

// Track 0 loads register 0 with 5 and adds 3. It compares the register with 8 and jumps on a flag of 0 to key 72. The
// comparison sets the flag to the difference, 0. The track then plays key 72, not key 60. The second run compares with
// 9.
void TestJAudio1Registers()
{
    const auto play = [](uint32_t compared)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).Register(0, 0, 5).Register(1, 0, 3).Register(3, 0, compared);
        const uint32_t jump = s.Here();
        s.Jump(1, 0).NoteOn(60, 1, 100).Wait(10).NoteOff(1).Finish();
        const uint32_t other = s.Here();
        s.NoteOn(72, 1, 100).Wait(10).NoteOff(1).Finish();
        std::vector<uint8_t> data = s.Data();
        const uint32_t at = jump - kFirstSequence;
        data[at + 2] = static_cast<uint8_t>(other >> 16);
        data[at + 3] = static_cast<uint8_t>(other >> 8);
        data[at + 4] = static_cast<uint8_t>(other);
        const Performance perf = PerformJAudio1(data);

        return std::pair(NoteTicks(perf, 0, 60).size(), NoteTicks(perf, 0, 72).size());
    };

    Check(play(8) == std::pair<std::size_t, std::size_t>(0, 2), "5 + 3 equals 8: the jump is taken");
    Check(play(9) == std::pair<std::size_t, std::size_t>(2, 0), "5 + 3 isn't 9: the track plays on");
}

// A JAudio1 game's folder: the start-up archive (test.aaf) and its sequence archive in Seqs. The sound table's first
// sequence, the effects sequence, plays file 7. The music plays files 3 and 9. Two more sounds play file 3 again at the
// same volume and file 9 at another volume. File 11 is played by no sound.
void TestJAudio1Folder()
{
    namespace fs = std::filesystem;
    const fs::path folder =
        fs::temp_directory_path() / ("sequence_export_jaudio1_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(folder / "Seqs");
    const std::vector<uint8_t> music = Sequence1(0).Tempo(120).Finish().Data();
    xsflib::WriteFile(
        folder / "test.aaf",
        files::AafFile({{16, 127, 7}, {16, 100, 3}, {16, 90, 9}, {16, 100, 3}, {16, 60, 9}, {0, 80}}, {"test.aw"}));
    xsflib::WriteFile(
        folder / "Seqs" / "test.arc",
        files::RarcFile(
            {{7, "effects.bms", music}, {3, "field.bms", music}, {9, "boss.bms", music}, {11, "unused.bms", music}}));

    const auto names = [](const Game& game)
    {
        std::vector<std::string> list;
        for (const SequenceSound& sound : game.Sequences())
        {
            list.push_back(sound.name + (sound.jaudio1 ? "" : " (not JAudio1)") +
                           (sound.default_release == 0x10 ? "" : " (another release)"));
        }

        return list;
    };

    // The music takes the names of its files. A sound that repeats an earlier one is left out. One at another volume
    // takes the name of its ID. The effects sequence and the effects aren't converted.
    const Game plain = Game::FromFolder(folder / "test.aaf", kProgram);
    Check(names(plain) == std::vector<std::string>{"field", "boss", "SOUND_128_0_4", "unused"},
          "the music, by the names of its files");
    Check(plain.Sequences().size() == 4 && std::abs(plain.Sequences()[1].volume - 90.0f / 127.0f) < 1e-6f,
          "a sound plays at the volume of its entry");

    // A name file beside the archive names the sounds by their IDs.
    std::vector<uint8_t> asn(16 + 20 * 32);
    xsflib::StoreBe32(asn.data() + 12, 2);
    const std::string field = "JA_BGM_FIELD";
    const std::string boss = "JA_BGM_BOSS_2";
    std::copy(field.begin(), field.end(), asn.begin() + 16 + 18 * 32);
    xsflib::StoreBe32(asn.data() + 16 + 18 * 32 + 28, 0x80000001);
    std::copy(boss.begin(), boss.end(), asn.begin() + 16 + 19 * 32);
    xsflib::StoreBe32(asn.data() + 16 + 19 * 32 + 28, 0x80000004);
    xsflib::WriteFile(folder / "test.asn", asn);

    const Game named = Game::FromFolder(folder / "test.aaf", kProgram);
    Check(names(named) == std::vector<std::string>{"JA_BGM_FIELD", "boss", "JA_BGM_BOSS_2", "unused"},
          "the name file's names come first");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// A SoundFont whose loudest zone plays above full level takes one attenuation in every zone, and the conversion records
// it. Zones at full level take none.
void TestCutAboveFullLevel()
{
    Conversion loud;
    Conversion full;

    const double quieter = ntdlib::sequence_export::CutAboveFullLevel(loud, 2.0);
    const double none = ntdlib::sequence_export::CutAboveFullLevel(full, 1.0);

    Check(std::fabs(quieter - 200.0 * std::log10(2.0)) < 1e-9, "twice full level takes 6 dB");
    Check(loud.performance.approximations.size() == 1 &&
              loud.performance.approximations.begin()->first ==
                  "instrument volume above full level (SoundFont attenuated by 6.0 dB; SFZ keeps the level)",
          "and the conversion records it");
    Check(none == 0.0 && full.performance.approximations.empty(), "full level takes none");
}

// The frame's converter of JAudio games opens a start-up archive with its game's files, lists the sequences and
// converts them. IsStartUpArchive identifies a BAA and an AAF by their first bytes. A converter of another library's
// archives converts the game where it takes JAudio games, with its options.
void TestGameConverter()
{
    namespace fs = std::filesystem;
    const fs::path folder =
        fs::temp_directory_path() / ("sequence_export_jaudio_converter_test_" + std::to_string(std::random_device{}()));
    files::WriteGameFiles(folder);
    Sequence s(0);
    s.OpenTrack(0, 0x20).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(0x20);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    const std::optional<std::vector<uint8_t>> baa = ntdlib::sequence_export::ReadFile(folder / "test.baa");

    const GameConverter converter = MakeGameConverter("xyz2midi", "xyz2midi 1.0", {}, [] {});
    std::optional<std::unique_ptr<GameInput>> input = converter.open(folder / "test.baa", "test.baa");
    const std::vector<ntdlib::sequence_export::Sequence> list =
        input ? converter.sequences(*input) : std::vector<ntdlib::sequence_export::Sequence>{};
    const Conversion c = input && list.size() == 1 ? converter.convert(*input, 0, {}) : Conversion{};

    Check(s.Data() == files::OneNoteSequence(), "the game's sequence");
    Check(list.size() == 1 && list[0].index == 0 && list[0].name == "BGM_FIELD", "the converter lists the music");
    Check(c.midi.size() > 14 && c.samples == 1 && c.warnings.empty(), "and converts it with its wave");
    Check(baa && IsStartUpArchive(*baa) && IsStartUpArchive(files::AafFile({{16, 127, 7}}, {})) &&
              !IsStartUpArchive(std::vector<uint8_t>{'R', 'S', 'A', 'R'}) &&
              IsStartUpArchiveFile(folder / "test.baa") && !IsStartUpArchiveFile(folder / "missing.baa"),
          "a start-up archive by its first bytes");

    namespace frame = ntdlib::sequence_export;
    frame::ArchiveConverter<ntdlib::sar::SoundArchive> archives;
    archives.program = "xyz2midi";
    archives.settings.voice_limit = 96;
    const frame::OptionHandler<PerformOptions> options = ArchiveProgramOptions(frame::ArchiveOptions(archives));
    const frame::Options<PerformOptions> parsed =
        frame::ParseOptions<PerformOptions>({"xyz2midi", "--voices", "8", "--var", "1=2", "--seed", "5"}, options);
    const std::string unknown =
        ErrorOf([&] { frame::ParseOptions<PerformOptions>({"xyz2midi", "--port", "1=2"}, options); });

    Check(parsed.perform.voice_limit == 8 && parsed.perform.seed == 5, "a game takes --voices and --seed");
    Check(unknown == "unknown option --port", "and only the options of the program's archives: " + unknown);

    std::string program = "xyz2midi";
    std::string quiet = "--quiet";
    std::string path = (folder / "test.baa").string();
    char* argv[] = {program.data(), quiet.data(), path.data(), nullptr};
    const int refused = frame::RunArchiveConverter(3, argv, archives);
    const int converted = RunArchiveConverterWithGames(3, argv, archives);

    Check(refused == 1 && converted == 0 && fs::exists(folder / "test_xyz2midi" / "BGM_FIELD.mid"),
          "a converter of archives converts a JAudio game where it takes them");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

} // namespace

int main()
{
    TestDecodeWave();
    TestFindSound();
    TestOscillator();
    TestOscillatorWithoutReleaseTable();
    TestEnvelopeFit();
    TestReleaseAfterDecayToSilence();
    TestPerformance();
    TestOscillatorScaleArguments();
    TestOscillatorScale();
    TestSweepAndTie();
    TestTieBelowHearing();
    TestGateTickRate();
    TestLoop();
    TestJumpToTheNextCommand();
    TestPortsAndRegisters();
    TestDrumAndVolume();
    TestTailsKeepTheirSettings();
    TestHeldNoteKeepsItsPart();
    TestDefaultKeepsEveryNote();
    TestConversion();
    TestCutAboveFullLevel();
    TestPitchLimit();
    TestWaveArchiveThatCannotBeRead();
    TestFolderWithAppleDoubleFiles();
    TestGameRules();
    TestEchoLengthensTheEnd();
    TestJAudio1Notes();
    TestJAudio1Registers();
    TestJAudio1Folder();
    TestGameConverter();

    return xsflib::testing::Summary("sequence_export_jaudio");
}
