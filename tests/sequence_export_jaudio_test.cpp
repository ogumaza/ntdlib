// SPDX-License-Identifier: MIT

// Tests of the conversion of JAudio's sound data, without game data. The tests build small JAudio2 archives and JAudio1
// sequences in memory.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/sf2/reader.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"

#include "ntdlib/disc/game.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/conversion.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/jaudio/archive_games.h"
#include "ntdlib/sequence_export/jaudio/convert.h"
#include "ntdlib/sequence_export/jaudio/envelope.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/jaudio/instruments.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/jaudio/track_filter.h"
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

    // The IIR filter's entry of the table.
    Sequence& TrackFilter(uint32_t entry)
    {
        b_.U8(0xeb).U8(entry);

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

    // A region whose wave's id names another wave system plays that system's wave (Bank::named_systems).
    Bank named = Bank::Parse(files::BankV0File(7, 100, true, 4, 1), 0);
    const std::optional<NoteSound> bank_system = FindSound(named, 0, 60, 100);
    named.named_systems = {{1, 2}};
    const std::optional<NoteSound> other_system = FindSound(named, 0, 60, 100);
    Check(bank_system && bank_system->wave_system == 0, "a region plays the bank's wave system");
    Check(other_system && other_system->wave_system == 2 && other_system->wave_id == 0,
          "a region plays the wave system that its wave's id names");
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

void TestHoldsSilent()
{
    // A decay to silence over 60 units that holds there. A release then stays silent unless its table rises. A release
    // table that is the attack table carries on from the hold. A hold at full level isn't silent.
    Oscillator falls;
    falls.attack = {{0, 0, 0x7fff}, {0, 60, 0}, {14, 0, 0}};
    falls.release = {{0, 30, 0}, {15, 0, 0}};
    Oscillator bare = falls;
    bare.release.clear();
    Oscillator rises = falls;
    rises.release = {{0, 30, 0x4000}, {15, 0, 0}};
    Oscillator goes_on = falls;
    goes_on.attack.push_back({0, 30, 0x7fff});
    goes_on.release = goes_on.attack;
    goes_on.release_is_attack = true;
    Oscillator full;
    full.attack = {{0, 0, 0x7fff}, {14, 0, 0}};
    std::array<OscillatorState, 5> s;
    const std::array<const Oscillator*, 5> oscs{&falls, &bare, &rises, &goes_on, &full};

    for (std::size_t i = 0; i < s.size(); i++)
    {
        s[i].Start(*oscs[i]);
        s[i].Advance(30.0f);
    }
    const bool early = s[0].HoldsSilent();
    for (OscillatorState& o : s)
    {
        o.Advance(30.0f);
    }

    Check(!early && s[0].Holding() && s[0].HoldsSilent(), "a decay that holds at 0 is silent from the hold on");
    Check(s[1].HoldsSilent(), "so is one without a release table");
    Check(s[2].Holding() && !s[2].HoldsSilent(), "a release table that rises can sound again");
    Check(s[3].Holding() && !s[3].HoldsSilent(), "so can an attack table that rises after the hold");
    Check(!s[4].HoldsSilent(), "a hold at full level isn't silent");
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

// Track 0 sets its volume table at kTable and holds key 60 for 96 ticks, a second. The table rises to full level at
// once. It then holds there, or with `decays` falls to silence over 60 units, a tenth of a second, and holds at 0.
// Return the tick of the MIDI note's note-off, or -1 without one.
int64_t NoteOffTick(bool decays)
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).OscillatorTable(0, kTable).NoteOn(60, 1, 127).Wait(96).NoteOff(1).Finish().PadTo(kTable);
    s.Point(0, 0, 0x7fff);
    if (decays)
    {
        s.Point(0, 60, 0);
    }
    s.Point(14, 0, 0);
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOff && e.key == 60)
            {
                return e.tick;
            }
        }
    }

    return -1;
}

void TestNoteEndsWhenItsVolumeHoldsAtZero()
{
    // The game keeps a channel whose volume holds at 0 until its note-off, but nothing sounds. The MIDI note ends where
    // the volume reaches 0: a tenth of a second at 96 ticks a second.
    const int64_t silenced = NoteOffTick(true);
    const int64_t held = NoteOffTick(false);

    Check(silenced >= 9 && silenced <= 11, "a note ends where its volume holds at 0");
    Check(held == 96, "a note that holds at full level ends at its note-off");
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

    // A game's rules give the sound a port's value (GameRules::ports). An option for the same port takes its place.
    SequenceSound with_rule = game.Sound(0);
    with_rule.ports[0] = 1;
    PerformOptions cleared;
    cleared.ports[0] = 0;
    Check(first_key(Perform(with_rule, game.banks, {}, kProgram)) == 72,
          "a game's rules set a port before the sequence starts");
    Check(first_key(Perform(with_rule, game.banks, cleared, kProgram)) == 60,
          "--port takes the place of the game's value for the same port");
}

// A read of a child's port after the child has finished writes nothing: the game deletes a finished child. The first
// track's child 0 writes 1 to its port 0 and finishes. The first track then clears register 3, reads that port into
// register 0, and opens child 1 at key 72's commands if register 3 isn't 0, or else at key 60's.
void TestPortOfFinishedChild()
{
    constexpr uint32_t kWriter = kFirstSequence + 0x40;
    constexpr uint32_t kLow = kFirstSequence + 0x50;
    constexpr uint32_t kHigh = kFirstSequence + 0x60;
    Sequence s;
    s.OpenTrack(0, kWriter).Timebase(48).Tempo(120).Wait(5);
    s.Byte(0xd8).Byte(0).Byte(0).Byte(0); // register 0, and so register 3, to 0
    s.Byte(0xd7).Byte(0x00).Byte(0);      // read child 0's port 0 into register 0
    const uint32_t jump = s.Here();
    s.Byte(0xc8).Byte(2).Byte(0).Byte(0).Byte(0); // jump if register 3 isn't 0 (target set below)
    s.OpenTrack(1, kLow).Wait(100).Finish();
    const uint32_t other = s.Here();
    s.OpenTrack(1, kHigh).Wait(100).Finish().PadTo(kWriter);
    s.Byte(0xd8).Byte(0x40).Byte(0).Byte(1).Finish().PadTo(kLow); // port 0 to 1
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(10).NoteOff(1).Finish().PadTo(kHigh);
    s.Bank(0).Program(0).NoteOn(72, 1, 100).Wait(10).NoteOff(1).Finish();
    std::vector<uint8_t> data = s.Data();
    const uint32_t at = jump - kFirstSequence;
    data[at + 2] = static_cast<uint8_t>(other >> 16);
    data[at + 3] = static_cast<uint8_t>(other >> 8);
    data[at + 4] = static_cast<uint8_t>(other);
    TestGame game(files::BankV0File(0, 60), {data});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    std::vector<int> keys;
    for (const std::vector<TrackEvent>& track : perf.tracks)
    {
        for (const TrackEvent& e : track)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                keys.push_back(static_cast<int>(e.key));
            }
        }
    }

    Check(keys == std::vector<int>{60}, "a read of a finished child's port leaves the register as it was");
}

// The root track's ports that a sequence reads while they hold their first values (Performance::ports_read). A variant
// of a sound's ports (GameRules::port_variants) converts only if its sequence reads one of them so
// (GameInput::Converts).
void TestPortsRead()
{
    // The sound gives port 0xc the value 20. A child of the root track reads the root's port `port` into register 0.
    // With `own`, the root reads it instead. With `write_first`, the root writes port 0xc before that.
    auto ports_read = [](uint8_t port, bool own, bool write_first)
    {
        Sequence s;
        if (write_first)
        {
            s.Byte(0xd1).Byte(0x0c).Byte(0); // write register 0's value to port 0xc
        }
        if (own)
        {
            s.Byte(0xd0).Byte(port).Byte(0); // read the port into register 0
        }
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0);
        if (!own)
        {
            s.Byte(0xd6).Byte(port).Byte(0); // read the parent's port into register 0
        }
        s.NoteOn(60, 1, 100).Wait(10).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.ports[0xc] = 20;

        return Perform(sound, game.banks, {}, kProgram).ports_read;
    };

    Check(ports_read(0xc, false, false) == 1u << 0xc, "a child's read of the root's port counts");
    Check(ports_read(0xc, true, false) == 1u << 0xc, "so does the root's read of its port");
    Check(ports_read(0xb, false, false) == 0, "a read of a port without a first value doesn't count");
    Check(ports_read(0xc, false, true) == 0, "nor does a read after the sequence wrote the port");
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
    const std::string kApproximation =
        "released notes take later settings of their track while it holds a note on their part";
    Check(first && second && *first == *second, "both notes play on one part");
    Check(volume && volume == second, "the held note takes the track's volume");
    Check(perf.approximations.contains(kApproximation), "the release takes the volume too, as an approximation");
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
    const std::string kApproximation = "notes cut when the game's 64 voices ran out";
    Check(cut_short(kept) == 0 && !kept.approximations.contains(kApproximation),
          "by default every note plays to its end");
    Check(cut_short(limited) == 6 && limited.approximations.contains(kApproximation),
          "with --voices 64, six notes are cut as in the game");
}

// The events of kind `kind` on the MIDI tracks of `perf` named `name`, in their order.
std::vector<TrackEvent> NamedTrackEvents(const Performance& perf, const std::string& name, EventKind kind)
{
    std::vector<TrackEvent> events;
    for (std::size_t slot = 0; slot < perf.tracks.size(); slot++)
    {
        if (perf.track_names[slot] != name)
        {
            continue;
        }
        for (const TrackEvent& e : perf.tracks[slot])
        {
            if (e.kind == kind)
            {
                events.push_back(e);
            }
        }
    }

    return events;
}

// The ticks of the note-ons on `perf`'s MIDI track named `name`.
std::vector<uint32_t> NoteTicks(const Performance& perf, const std::string& name)
{
    std::vector<uint32_t> ticks;
    for (const TrackEvent& e : NamedTrackEvents(perf, name, EventKind::kNoteOn))
    {
        ticks.push_back(e.tick);
    }

    return ticks;
}

// A game can mute some children of a sound's track 0 before the sound's first tick (SequenceSound::tracks). Track 0
// opens children 0, 1 and 2. Child 1 opens a child too. Each of the four plays a note at tick 0: child 1 a gate note,
// the others a note with an ID. With child 1's bit clear, children 0 and 2 play, and child 1 and its child stay silent.
// Track 0 opens child 1 again at tick 48. The open clears the mute, and both play then. Without a mask, every track
// plays at tick 0.
void TestMutedTracks()
{
    constexpr uint32_t kNote = kFirstSequence + 0x60;
    constexpr uint32_t kOpener = kFirstSequence + 0x80;
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.OpenTrack(0, kNote).OpenTrack(1, kOpener).OpenTrack(2, kNote).Wait(48).OpenTrack(1, kOpener).Wait(100).Finish();
    s.PadTo(kNote).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kOpener).OpenTrack(0, kNote).Bank(0).Program(0).Gate(64, 0x00, 100, 24).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound masked = game.Sound(0);
    masked.tracks[0] = 0xffff & ~(1u << 1);

    const Performance every = Perform(game.Sound(0), game.banks, {}, kProgram);
    const Performance muted = Perform(masked, game.banks, {}, kProgram);

    using Ticks = std::vector<uint32_t>;
    Check(NoteTicks(every, "Track 0.0") == Ticks{0} && NoteTicks(every, "Track 0.2") == Ticks{0} &&
              NoteTicks(every, "Track 0.1") == Ticks({0, 48}) && NoteTicks(every, "Track 0.1.0") == Ticks({0, 48}),
          "without a mask, every track plays at tick 0, and child 1 and its child again at tick 48");
    Check(NoteTicks(muted, "Track 0.0") == Ticks{0} && NoteTicks(muted, "Track 0.2") == Ticks{0},
          "the children whose bits are set play");
    Check(NoteTicks(muted, "Track 0.1") == Ticks{48} && NoteTicks(muted, "Track 0.1.0") == Ticks{48},
          "child 1 and its child play only once track 0 opens child 1 again");
}

// A game can answer JAudio2's command 0xf9 through a sequence callback (SequenceSound::callback). The root's child 0
// asks with 0xa at tick 24 and plays a note there if the flag register meets the condition. Condition 3 asks for 1,
// condition 2 for anything but 0. A callback that answers 1 to 0xa lets the note play under condition 3. One that
// doesn't list 0xa answers 0, and no note plays under either condition. Without a callback, the flag register gets
// 0xffff. The note then plays under condition 2 only.
void TestSequenceCallback()
{
    constexpr uint32_t kNote = kChild + 0x20;
    const auto sound = [&](uint32_t condition, std::optional<std::map<uint16_t, uint16_t>> callback)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Wait(24).Byte(0xf9).Byte(0x00).Byte(0x0a);
        s.Byte(0xc8).Byte(condition).Byte(kNote >> 16).Byte((kNote >> 8) & 0xff).Byte(kNote & 0xff).Finish();
        s.PadTo(kNote).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound played = game.Sound(0);
        played.callback = std::move(callback);

        return NoteTicks(Perform(played, game.banks, {}, kProgram), "Track 0");
    };

    using Ticks = std::vector<uint32_t>;
    const std::map<uint16_t, uint16_t> ready{{0xa, 1}};
    const std::map<uint16_t, uint16_t> other{{0xb, 1}};
    Check(sound(3, ready) == Ticks{24}, "the callback's answer of 1 meets condition 3");
    Check(sound(3, other).empty() && sound(2, other).empty(), "a callback answers 0 to an argument it doesn't list");
    Check(sound(3, std::nullopt).empty() && sound(2, std::nullopt) == Ticks{24},
          "without a callback, the flag register holds 0xffff");
}

// A companion (SequenceSound::companion) reads the notes with a voice of the root's child 0 and of its child 0, even
// while the game's code mutes them, and starts sounds beside the sequence once a game frame. Track 0 opens its child 0,
// which the mask mutes. The child writes 3 to its port 0xe, then plays key 0x27 at velocity 110 at ticks 0 and 48. The
// companion starts the effect at the root kEffect in its frame 30, after 0.5 s: tick 48 at a tempo of 120 and a
// timebase of 48. The effect's root sets a tempo of 60, which the MIDI file leaves out. Its child plays key 64 at once,
// on a MIDI track of its own. A second start restarts the effect, and a stop ends it.
struct CompanionLog
{
    std::vector<std::pair<uint8_t, uint8_t>> notes;
    std::optional<uint16_t> position;
};

class TestCompanion : public Companion
{
public:
    TestCompanion(std::shared_ptr<CompanionLog> log, const SequenceSound* effect)
        : log_(std::move(log)), effect_(effect)
    {
    }

    std::unique_ptr<Companion> Clone() const override
    {
        return std::make_unique<TestCompanion>(*this);
    }

    void Note(uint8_t key, uint8_t velocity) override
    {
        log_->notes.emplace_back(key, velocity);
    }

    std::vector<CompanionSound> Frame(const std::function<std::optional<uint16_t>(int, int)>& port) override
    {
        frames_++;
        if (const std::optional<uint16_t> value = port(0, 0xe))
        {
            log_->position = value;
        }

        if (frames_ == 30 || frames_ == 90)
        {
            return {{.sound = effect_, .ports = {{0xb, 64}}}};
        }

        if (frames_ == 150)
        {
            return {{.action = CompanionSound::Action::kStop}};
        }

        return {};
    }

private:
    std::shared_ptr<CompanionLog> log_;
    const SequenceSound* effect_ = nullptr;
    int frames_ = 0;
};

void TestCompanionSounds()
{
    constexpr uint32_t kBeat = kChild + 0x20;
    constexpr uint32_t kEffect = kChild + 0x60;
    constexpr uint32_t kEffectChild = kChild + 0x80;
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(250).Finish().PadTo(kChild);
    s.OpenTrack(0, kBeat).Wait(240).Finish().PadTo(kBeat);
    s.Byte(0xd8).Byte(0x01).Byte(0x00).Byte(0x03).Byte(0xd1).Byte(0x0e).Byte(0x01);
    s.Bank(0).Program(0).NoteOn(0x27, 1, 110).Wait(24).NoteOff(1).Wait(24).NoteOn(0x27, 1, 110).Wait(24);
    s.NoteOff(1).Finish().PadTo(kEffect);
    s.OpenTrack(0, kEffectChild).Timebase(48).Tempo(60).Wait(200).Finish().PadTo(kEffectChild);
    s.Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound effect = game.Sound(0);
    effect.start = effect.start - kFirstSequence + kEffect;

    SequenceSound music = game.Sound(0);
    music.tracks[0] = 0xfffe;
    const auto log = std::make_shared<CompanionLog>();
    music.companion = [&]
    {
        return std::make_unique<TestCompanion>(log, &effect);
    };
    const Performance perf = Perform(music, game.banks, {}, kProgram);

    Check(std::ranges::count(log->notes, std::pair<uint8_t, uint8_t>{0x27, 110}) >= 2,
          "the companion reads the muted beat's notes");
    Check(log->position == 3, "the companion reads port 0xe of child 0");
    Check(NoteTicks(perf, "Track 0.0").empty(), "the muted beat plays nothing");
    const std::vector<uint32_t> ticks = NoteTicks(perf, "Effect 0 track 0");
    Check(ticks.size() == 2 && ticks[0] >= 47 && ticks[0] <= 50 && ticks[1] >= 143 && ticks[1] <= 146,
          "the effect starts after frames 30 and 90, at ticks 48 and 144 of the sequence");
    Check(std::ranges::all_of(perf.tempo, [](const auto& t) { return std::fabs(t.bpm - 120.0) < 1e-6; }),
          "the effect's tempo stays out of the MIDI file");
}

// A game's code can play a sound at a factor of its tempo (SequenceSound::tempo_ratio). At half the ratio, the root's
// tempo of 120 plays at 60. The notes keep their ticks. Two beats then last 2 s.
void TestTempoRatio()
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(48).NoteOff(1).NoteOn(64, 1, 80).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound slow = game.Sound(0);
    slow.tempo_ratio = 0.5f;

    const Performance normal = Perform(game.Sound(0), game.banks, {}, kProgram);
    const Performance half = Perform(slow, game.banks, {}, kProgram);

    Check(!half.tempo.empty() && half.tempo[0].bpm == 60.0, "the tempo plays at half the ratio");
    Check(NoteTicks(half, "Track 0") == NoteTicks(normal, "Track 0"), "the notes keep their ticks");
    Check(std::abs(half.seconds - normal.seconds - 1.0) < 0.02, "two beats last 1 s longer");
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

// The open filter leaves a region as it is. Entry 64 makes an unfiltered zone and a low-passed one. Together, they play
// at the filter's gain at 125 Hz, a little under its gain at 0 Hz of (18463 + 1300) / (32768 - 8939 - 7314). At the
// Wii U's rate of 48 kHz, the same coefficients act higher up. Entry 126 moves no band by 0.3 dB.
void TestTrackFilterLayers()
{
    const std::vector<FilterLayer> open = TrackFilterLayers(kOpenTrackFilter, kOutputRate, FilterFormat::kSoundFont);
    Check(open.size() == 1 && open[0].gain == 1.0 && !open[0].cutoff_cents, "the open filter gives one plain zone");

    const std::vector<FilterLayer> layers = TrackFilterLayers(64, kOutputRate, FilterFormat::kSoundFont);
    Check(layers.size() == 2 && !layers[0].cutoff_cents && layers[1].cutoff_cents,
          "a filter gives an unfiltered zone and a low-passed one");
    const double together = layers.size() == 2 ? layers[0].gain + layers[1].gain : 0.0;
    Check(together < 19763.0 / 16515.0 && together > 19763.0 / 16515.0 - 0.01,
          "the zones add up to the filter's gain at 125 Hz");

    const std::vector<FilterLayer> wii_u = TrackFilterLayers(64, 48000.0, FilterFormat::kSoundFont);
    Check(wii_u.size() == 2 && layers.size() == 2 && *wii_u[1].cutoff_cents > *layers[1].cutoff_cents,
          "at a higher rate, the low-pass sits higher");

    // SFZ's low-pass at a resonance of 0 dB passes more near its cutoff. Its cutoff sits lower.
    const std::vector<FilterLayer> sfz = TrackFilterLayers(64, kOutputRate, FilterFormat::kSfz);
    Check(sfz.size() == 2 && layers.size() == 2 && *sfz[1].cutoff_cents < *layers[1].cutoff_cents,
          "SFZ's low-pass sits lower");

    Check(TrackFilterHeard(64, kOutputRate) && TrackFilterHeard(111, kOutputRate), "entries 64 and 111 are heard");
    Check(!TrackFilterHeard(126, kOutputRate) && !TrackFilterHeard(kOpenTrackFilter, kOutputRate),
          "entry 126 and the open filter aren't");
}

// A track's filter reaches the notes' presets where the sound's zones play filters (SequenceSound::zone_filters). A
// track whose notes play under two entries gets none, and the filter stays reported as omitted.
void TestZoneFilters()
{
    constexpr char kOmitted[] = "track filters omitted";
    constexpr char kPlayed[] = "track filters played by zones";
    const auto perform = [](uint32_t first, uint32_t second, bool zone_filters)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).TrackFilter(first).NoteOn(60, 1, 100).Wait(24).NoteOff(1);
        s.TrackFilter(second).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.zone_filters = zone_filters;
        return Perform(sound, game.banks, {}, kProgram);
    };

    const Performance fixed = perform(64, 64, true);
    Check(fixed.presets.size() == 1 && fixed.presets[0].track_filter == 64, "a fixed filter reaches the note's preset");
    Check(fixed.approximations.contains(kPlayed) && !fixed.approximations.contains(kOmitted),
          "and is reported as played by zones");
    Check(fixed.filter_layers.contains(64) && fixed.filter_layers.at(64).size() == 2,
          "the performance holds the entry's zones");

    const Performance swept = perform(64, 32, true);
    Check(swept.presets.size() == 1 && swept.presets[0].track_filter == kOpenTrackFilter, "a swept filter stays out");
    Check(swept.approximations.contains(kOmitted) && !swept.approximations.contains(kPlayed),
          "and is reported as omitted");

    const Performance quiet = perform(126, 126, true);
    Check(quiet.presets.size() == 1 && quiet.presets[0].track_filter == kOpenTrackFilter,
          "a filter that nobody would hear stays out");

    const Performance effect = perform(64, 64, false);
    Check(effect.presets.size() == 1 && effect.presets[0].track_filter == kOpenTrackFilter,
          "a sound whose zones play no filters keeps the plain preset");
    Check(effect.approximations.contains(kOmitted), "and reports the filter as omitted");

    // The filter's two zones reach the SoundFont and the SFZ file.
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0).TrackFilter(64).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.zone_filters = true;
    const Conversion c = ConvertSequence(sound, game.banks, {}, kProgram);
    const std::string sf2_problems = Joined(seqlib::sf2::Check(c.sf2));
    int plain = 0;
    int low_passed = 0;
    if (sf2_problems.empty())
    {
        const seqlib::sf2::Records sf = seqlib::sf2::Read(c.sf2);
        for (std::size_t z = 0; z + 1 < sf.Count("ibag", 4); z++)
        {
            const std::map<uint16_t, uint16_t> gens = sf.ZoneGens(z);
            if (gens.contains(53))
            {
                (gens.contains(8) ? low_passed : plain)++;
            }
        }
    }
    Check(sf2_problems.empty() && plain == 1 && low_passed == 1,
          "the SoundFont has an unfiltered zone and a low-passed one: " + sf2_problems);
    const std::string sfz = c.sfz.files.size() == 1 ? c.sfz.files[0].text : "";
    Check(sfz.find(" cutoff=") != std::string::npos && sfz.find("<region>") != sfz.rfind("<region>"),
          "the SFZ file has two regions, one with a cutoff");
    const std::string midi_problems = Joined(seqlib::midi::Problems(c.midi));
    const std::string sfz_problem =
        midi_problems.empty() ? ntdlib::testing::SfzProblem(c.sfz, seqlib::midi::TrackNames(seqlib::midi::Read(c.midi)))
                              : midi_problems;
    Check(sfz_problem.empty(), "the SFZ files match the MIDI file: " + sfz_problem);
}

// Program 0xf3 plays the DSP's pulse of 25% duty, and needs no bank. Its cycle lasts 32 samples at 16,736 Hz: key 60
// plays 523 Hz. The PCM holds the cycle in 256 samples at 33,472 Hz, two octaves below, and the zone plays it two
// octaves up. The pulse's last quarter is high, at 0x4000. Program 0xfb's noise is left out and reported.
void TestOscillatorNotes()
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(5).Program(0xf3).NoteOn(60, 1, 100).Wait(48).NoteOff(1).Program(0xfb).NoteOn(60, 1, 100).Wait(48);
    s.NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Conversion c = ConvertSequence(game.Sound(0), game.banks, {}, kProgram);
    const std::vector<PresetZone> zones =
        c.performance.presets.size() == 1 ? ZonesOf(0, c.performance, game.banks) : std::vector<PresetZone>{};
    const Pcm* pcm = game.banks.GetWave(kOscillatorWaveSystem, 3);

    Check(Count(c.performance, 0, EventKind::kNoteOn) == 1, "the pulse plays its note, and the noise none");
    Check(c.performance.approximations.contains("notes on the DSP's noise, ramp and other oscillators omitted"),
          "the noise's note is reported");
    Check(zones.size() == 1 && zones[0].low_key == 0 && zones[0].high_key == 127 && zones[0].root_key == 60 &&
              std::abs(zones[0].cents - 2400.0) < 1e-9 && zones[0].wave_system == kOscillatorWaveSystem &&
              zones[0].wave_id == 3,
          "one zone plays the pulse two octaves above the PCM's rate");
    Check(pcm && pcm->loop && pcm->loop_start == 0 && pcm->loop_end == 256 && pcm->Length() == 256 &&
              std::lround(pcm->sample_rate) == 33472,
          "the PCM loops a cycle of 256 samples at 33,472 Hz");
    Check(pcm && pcm->Length() == 256 &&
              std::count(pcm->channels[0].begin(), pcm->channels[0].begin() + 192, int16_t{-0x4000}) == 192 &&
              std::count(pcm->channels[0].begin() + 192, pcm->channels[0].end(), int16_t{0x4000}) == 64,
          "the pulse is high in the last quarter of its cycle");
    Check(c.samples == 1 && Joined(seqlib::sf2::Check(c.sf2)).empty(), "the SoundFont holds the pulse's sample");
    Check(!game.banks.GetWave(kOscillatorWaveSystem, 11), "the noise has no PCM");
}

// An oscillator plays the same waveform at every velocity. A tie that doubles the velocity of a note on the square
// wave therefore starts the MIDI note at the higher velocity. TestSweepAndTie checks the same for an instrument in a
// bank. Bank 5 doesn't exist. The oscillator needs none.
void TestOscillatorTie()
{
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.Bank(5).Program(0xf0).Gate(60, 0x40, 50, 24).Gate(60, 0x00, 100, 24).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});

    const Performance perf = Perform(game.Sound(0), game.banks, {}, kProgram);

    int velocity = 0;
    for (const TrackEvent& e : perf.tracks[0])
    {
        velocity = e.kind == EventKind::kNoteOn ? e.value : velocity;
    }
    Check(Count(perf, 0, EventKind::kNoteOn) == 1 && velocity == 100 && perf.level_cut_db == 0.0,
          "an oscillator's note starts at the velocity its tie reaches");
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
// without a release over 0x10. Their echo repeats a sound every 64 sub-frames. JA_BGM_SELECT (0x0100001a) plays only
// children 2 and 7 of its track 0. JA_BGM_SELECT_map plays every child of track 0 but 2. The Wind Waker's Japanese
// release and both releases of HD get the USA release's category volumes. Twilight Princess on the GameCube plays
// categories 0 and 2 to 9 at 120/127, and category 1 at 1. Its archive on the Wii and in HD gets the same volumes. Both
// archives give a volume of 0 to the children of track 0 that the game silences as it starts five pieces, and list
// Hyrule Field's day music a second time, as Link rides. They list the states that the game's code gives pieces later,
// some with volumes between 0 and 1, and start more pieces in the states that their scenes and starters give. Link's
// Crossbow Training plays category 9 at 100/127, and the others as Twilight Princess does. It silences the same
// children as it starts the five pieces. Its rules hold none of the later states. Another game's start-up archive gets
// the defaults.
void TestGameRules()
{
    const GameRules usa = RulesOf("8c55097f2cc9f5800fd8ab96da4e3b09727e0a39");
    const GameRules japan = RulesOf("aab07cea25ba957374524c4f29d36b72e6d36a67");
    const GameRules other = RulesOf(std::string(40, '0'));
    TrackMask select = kEveryTrack;
    select[0] = 0x0084;
    TrackMask map = kEveryTrack;
    map[0] = 0xfffb;
    const auto arranged = [&](const GameRules& rules)
    {
        return rules.arrangements.size() == 1 && rules.arrangements[0].sound == 0x0100001a &&
               rules.arrangements[0].suffix == "map" && rules.arrangements[0].state.tracks == map &&
               rules.arrangements[0].state.child_volumes.empty();
    };
    // The tracks that each sound starts with, by the sound's ID.
    const auto start_tracks = [](const GameRules& rules)
    {
        std::map<uint32_t, TrackMask> found;
        for (const auto& [id, state] : rules.start_states)
        {
            found[id] = state.tracks;
        }

        return found;
    };

    Check(usa.category_volumes == std::map<uint8_t, float>{{0, 0.7f}}, "the USA release plays category 0 at 0.7");
    Check(usa.default_release == 0x10, "the USA release releases a bare oscillator over 0x10");
    Check(usa.echo && usa.echo->delay == 64 && usa.echo->feedback == 0x3fff / 32768.0f && usa.echo->volume == 0.75f &&
              usa.echo->voice_level == 0.732421875f,
          "the USA release's echo has a delay of 64 sub-frames, a filter of 0x3fff and a volume of 0x6000");
    Check(start_tracks(usa) == std::map<uint32_t, TrackMask>{{0x0100001a, select}} &&
              usa.start_states.at(0x0100001a).child_volumes.empty() &&
              usa.start_states.at(0x0100001a).game_ports.empty(),
          "the USA release plays only children 2 and 7 of JA_BGM_SELECT's track 0");
    Check(arranged(usa), "the USA release plays JA_BGM_SELECT a second time without child 2 of track 0");
    Check(japan.category_volumes == usa.category_volumes && japan.default_release == 0x10 && japan.echo &&
              japan.echo->delay == 64 && start_tracks(japan) == start_tracks(usa) && arranged(japan),
          "the Japanese release has the same rules");
    Check(other.category_volumes.empty() && other.default_release == 0 && !other.echo && other.start_states.empty() &&
              other.arrangements.empty() && other.effects_sequence.empty() && other.sequence_names,
          "another game gets the defaults");

    // The games whose effects convert with GameOptions::known_effects: The Wind Waker and The Wind Waker HD (USA and
    // Japan), Luigi's Mansion, Pac-Man Vs., Pikmin 2 on the GameCube and on the Wii, Super Mario Sunshine, and Pikmin's
    // bank file on the GameCube and on the Wii.
    const std::vector<std::string> kKnownEffects = {
        "65ea70655099a196e0707bf4e4ce61e7818553d9", "0a0f93a5bb97313d895e0a5a16604ad77f567547",
        "c66c0103a5c7c1292b5fc6c3371ed1458035cde7", "aa3077a9dafc178e46b80b4e5d975550cb28c517",
        "4a60396b9c35333f3083848be1f49c82bf9ba4df", "7436b12c7470a2ed14f865e0a6aa96ccfa1af758",
        "a8b63d2f333c34aaf9cb859f37f23011149af917", "6747c3c7bce964bc62f817e1c8c32711c905c4c3",
        "00c63687e6963ad89e1d86f02070e1f552623632", "9ae6b40b8e9eacfda6ed0b5d59ff1e680373a018",
        "9f42f65be7f7159faa14b003524476d624e1227f"};
    Check(std::ranges::all_of(kKnownEffects, [](const std::string& sha1) { return RulesOf(sha1).known_effects; }),
          "the 11 archives of the games whose effects convert");
    Check(!usa.known_effects && !other.known_effects &&
              !RulesOf("a90e5f2b804e154a043f8b89e0bd9d33f4798a99").known_effects,
          "Four Swords Adventures, Double Dash!! and another game convert no effects");
    const GameRules wind_waker = RulesOf("65ea70655099a196e0707bf4e4ce61e7818553d9");
    Check(wind_waker.category_volumes.size() == 7 &&
              RulesOf("0a0f93a5bb97313d895e0a5a16604ad77f567547").category_volumes == wind_waker.category_volumes &&
              RulesOf("c66c0103a5c7c1292b5fc6c3371ed1458035cde7").category_volumes == wind_waker.category_volumes &&
              RulesOf("aa3077a9dafc178e46b80b4e5d975550cb28c517").category_volumes == wind_waker.category_volumes,
          "The Wind Waker's Japanese release and both releases of HD get the USA release's category volumes");
    const auto keys = [](const GameRules& rules, uint32_t id)
    {
        std::vector<std::pair<int, uint32_t>> listed;
        const auto found = rules.random_keys.find(id);
        if (found != rules.random_keys.end())
        {
            for (const GameRules::RandomKey& key : found->second)
            {
                listed.emplace_back(key.key, key.draws);
            }
        }

        return listed;
    };
    using Keys = std::vector<std::pair<int, uint32_t>>;
    Check(wind_waker.random_keys.size() == 4 && keys(wind_waker, 0x3831) == Keys{{48, 1}} &&
              keys(wind_waker, 0x48fe) == Keys{{48, 8}, {49, 5}, {50, 1}} &&
              keys(wind_waker, 0x48ff) == Keys{{36, 8}, {37, 5}, {38, 1}} &&
              keys(wind_waker, 0x4947) == Keys{{24, 3}, {25, 5}},
          "The Wind Waker lists the keys that sound of its four effects that pick a random key");
    Check(RulesOf("0a0f93a5bb97313d895e0a5a16604ad77f567547").random_keys == wind_waker.random_keys &&
              usa.random_keys.empty(),
          "The Wind Waker's Japanese release has the same keys, and other games have none");
    const GameRules sunshine = RulesOf("00c63687e6963ad89e1d86f02070e1f552623632");
    Check(sunshine.sequence_volume == 0.75f && usa.sequence_volume == 1.0f,
          "Super Mario Sunshine plays its music at 0.75");
    const std::map<uint8_t, float> sunshine_volumes{{0, 96 / 127.0f}, {3, 102 / 127.0f}, {4, 106 / 127.0f},
                                                    {5, 74 / 127.0f}, {6, 114 / 127.0f}, {8, 96 / 127.0f}};
    Check(sunshine.category_volumes == sunshine_volumes,
          "Super Mario Sunshine gives six of its categories of effects a volume below 1 at start-up");
    const GameRules jungle_beat = RulesOf("8cf5a451fc0533f8922e4cd0b922029a4d1f5129");
    Check(jungle_beat.start_states.size() == 62 && jungle_beat.start_states.at(0x01000005).tracks[0] == 0x0ffe &&
              jungle_beat.start_states.at(0x01000006).tracks[0] == 0xfffe &&
              jungle_beat.start_states.at(0x01000008).tracks[0] == 0xfffe &&
              jungle_beat.start_states.at(0x01000005).tracks[1] == 0xffff &&
              !jungle_beat.start_states.contains(0x01000000),
          "Donkey Kong Jungle Beat's pieces start with the children of track 0 that their first states and the "
          "rhythm leave on");
    const GameRules new_play_control = RulesOf("b8c1a0440d328b6d197827b7fd16b04fe170fd71");
    Check(start_tracks(new_play_control) == start_tracks(jungle_beat),
          "New Play Control! Donkey Kong Jungle Beat's pieces start with the GameCube release's children of track 0");
    const auto layer_tracks = [](const GameRules& rules, uint32_t id, const std::string& suffix)
    {
        const auto found = std::ranges::find_if(
            rules.arrangements, [&](const Arrangement& a) { return a.sound == id && a.suffix == suffix; });
        return found == rules.arrangements.end() ? TrackMask{} : found->state.tracks;
    };
    TrackMask fall = kEveryTrack;
    fall[0] = 0xc000;
    Check(jungle_beat.arrangements.size() == 101 && layer_tracks(jungle_beat, 0x01000005, "combo1")[0] == 0x8ffe &&
              layer_tracks(jungle_beat, 0x01000005, "combo2")[0] == 0xfefe &&
              layer_tracks(jungle_beat, 0x0100000f, "change")[0] == 0x0004 &&
              layer_tracks(jungle_beat, 0x0100004b, "below_30_hold")[0] == 0xfffe &&
              layer_tracks(jungle_beat, 0x0100004a, "fall") == fall &&
              layer_tracks(jungle_beat, 0x0100002c, "state1") == TrackMask{},
          "Donkey Kong Jungle Beat's 43 pieces convert once more in each state that the game sets for them later");
    TrackMask course_select = kEveryTrack;
    course_select[0] = 0xfff2;
    const auto same = [](const Arrangement& a, const Arrangement& b)
    {
        return a.sound == b.sound && a.suffix == b.suffix && a.state.tracks == b.state.tracks;
    };
    Check(new_play_control.arrangements.size() == 102 &&
              std::equal(jungle_beat.arrangements.begin(), jungle_beat.arrangements.end(),
                         new_play_control.arrangements.begin(), same) &&
              layer_tracks(new_play_control, 0x0100002c, "state1") == course_select,
          "New Play Control! Donkey Kong Jungle Beat's pieces use the later states of the GameCube release. "
          "JA_BGM_COURSE_SELECT also uses state 1 of the menus");
    using Ports = std::map<uint32_t, std::map<uint8_t, uint16_t>>;
    Check(jungle_beat.ports == Ports{{0x37, {{0xb, 60}}}, {0x8e, {{0xb, 6}}}, {0x4000f, {{0xb, 8}}}},
          "Donkey Kong Jungle Beat's effects take the first values that the game's code gives them at port 0xb");
    Check(new_play_control.ports == Ports{{0x37, {{0xb, 60}}},
                                          {0x8e, {{0xb, 1}}},
                                          {0xcc, {{0xb, 7}}},
                                          {0x169, {{0xb, 2}}},
                                          {0x18a, {{0xb, 8}}},
                                          {0x4000f, {{0xb, 8}}}},
          "New Play Control! Donkey Kong Jungle Beat's effects take the Wii release's first values at port 0xb");
    Check(usa.ports.empty(), "a game without such effects gives no sound a port's value");
    Check(RulesOf("a8b63d2f333c34aaf9cb859f37f23011149af917").effects_sequence == "se.bms" &&
              RulesOf("6747c3c7bce964bc62f817e1c8c32711c905c4c3").effects_sequence == "se.bms" &&
              usa.effects_sequence.empty(),
          "Pikmin 2 plays its effects in se.bms on the GameCube and the Wii");

    // The Wind Waker's music on the GameCube and in The Wind Waker HD. Each Japanese release has its USA release's
    // table of the tracks that the starters keep.
    const GameRules gamecube = RulesOf("65ea70655099a196e0707bf4e4ce61e7818553d9");
    const GameRules gamecube_japan = RulesOf("0a0f93a5bb97313d895e0a5a16604ad77f567547");
    const GameRules hd = RulesOf("c66c0103a5c7c1292b5fc6c3371ed1458035cde7");
    const GameRules hd_japan = RulesOf("aa3077a9dafc178e46b80b4e5d975550cb28c517");
    // The tracks that play in a state, bit n for the handle's child n.
    const auto mask = [](const SequenceState& state)
    {
        uint32_t playing = 0xffff;
        for (const auto& [child, volume] : state.child_volumes)
        {
            if (volume == 0.0f && child < 16)
            {
                playing &= ~(1u << child);
            }
        }

        return playing;
    };
    const auto children = [&mask](const GameRules& rules, uint32_t id)
    {
        const auto state = rules.start_states.find(id);
        return state == rules.start_states.end() ? 0xffffu : mask(state->second);
    };
    const auto find = [](const GameRules& rules, uint32_t id, const std::string& suffix)
    {
        return std::ranges::find_if(rules.arrangements,
                                    [&](const Arrangement& a) { return a.sound == id && a.suffix == suffix; });
    };
    Check(children(hd, 0x80000042) == 0x4801 && children(gamecube, 0x80000042) == 0x4801,
          "JA_BGM_D_GANON_1 starts with children 0, 11 and 14");
    Check(gamecube_japan.arrangements.size() == gamecube.arrangements.size() &&
              children(gamecube_japan, 0x80000015) == children(gamecube, 0x80000015) &&
              hd_japan.arrangements.size() == hd.arrangements.size() &&
              children(hd_japan, 0x80000015) == children(hd, 0x80000015) &&
              children(gamecube, 0x80000015) != children(hd, 0x80000015),
          "the Japanese releases get their USA releases' music states");
    Check(children(gamecube, 0x80000015) == 0xffef && children(hd, 0x80000015) == 0xffff,
          "JA_BGM_MJ_TOWER_BATTLE starts without child 4 on the GameCube only");
    const auto faster = find(hd, 0x80000028, "80000125");
    Check(faster != hd.arrangements.end() && mask(faster->state) == 0xffbf && faster->state.tempo_ratio == 1.3143f,
          "0x80000125 plays JA_BGM_KAMI_TOWER faster, without child 6");
    Check(find(hd, 0x80000015, "80000115") == hd.arrangements.end() &&
              find(gamecube, 0x80000015, "80000115") != gamecube.arrangements.end(),
          "an arrangement that plays what the start does is left out");
    const auto fade = [](const GameRules& rules, uint32_t id)
    {
        const auto state = rules.start_states.find(id);
        return state == rules.start_states.end() ? -1 : state->second.fade_in;
    };
    Check(fade(hd, 0x80000004) == 60 && fade(hd, 0x8000001a) == 60 && fade(hd, 0x8000001c) == 60 &&
              fade(gamecube, 0x80000047) == 60 && children(gamecube, 0x80000047) == 0xffff,
          "the battle music and three pieces that subBgmStart starts fade in over 60 frames");
    Check(fade(hd, 0x80000019) == 0 && fade(hd, 0x80000042) == 0, "JA_BGM_MBOSS and bgmStart's pieces don't");
    const auto near = find(hd, 0x80000004, "NEAR");
    Check(near != hd.arrangements.end() && near->state.fade_in == 60 && mask(near->state) == 0xf676,
          "an arrangement of the battle music keeps the start's fade");
    const auto port9 = [&](uint32_t id, const std::string& suffix)
    {
        const auto a = find(hd, id, suffix);
        return a != hd.arrangements.end() && a->state.game_ports.size() == 1 && a->state.game_ports.begin()->first == 9;
    };
    Check(port9(0x80000011, "PORT9_1") && port9(0x80000011, "PORT9_2") && port9(0x80000011, "PORT9_9") &&
              find(hd, 0x80000011, "PORT9_3") == hd.arrangements.end(),
          "JA_BGM_JI_TRAINING converts with the values that changeBgmStatus writes");
    Check(port9(0x80000041, "PORT9_2") && port9(0x80000041, "PORT9_10") &&
              find(hd, 0x80000041, "PORT9_1") == hd.arrangements.end(),
          "JA_BGM_P_GANON_1 converts with the values that changeSubBgmStatus writes");
    const auto damaged = find(hd, 0x8000001c, "PORT9_1");
    Check(damaged != hd.arrangements.end() && mask(damaged->state) == 0xf47d && damaged->state.fade_in == 60,
          "JA_BGM_SEA_ENEMY's port 9 keeps the start's tracks and fade");
    Check(find(hd, 0x80000019, "PORT9_1") == hd.arrangements.end() &&
              find(hd, 0x80000004, "PORT9_1") == hd.arrangements.end(),
          "a value that changes nothing in a piece gives it no arrangement");

    const GameRules twilight = RulesOf("05b96ae88eb84f1f29fb606aad19b1be138223e8");
    std::map<uint8_t, float> volumes;
    for (const uint8_t category : std::array<uint8_t, 9>{0, 2, 3, 4, 5, 6, 7, 8, 9})
    {
        volumes[category] = 120.0f / 127.0f;
    }
    Check(twilight.category_volumes == volumes && twilight.default_release == 0,
          "Twilight Princess on the GameCube plays all but category 1 at 120/127");
    const auto echo_of = [](const GameRules& rules, float level)
    {
        return rules.echo && rules.echo->delay == 64 && rules.echo->feedback == 0x3fff / 32768.0f &&
               rules.echo->volume == 0.75f && rules.echo->voice_level == 2.0f * level / 32768.0f;
    };
    Check(echo_of(twilight, 16225.0f),
          "Twilight Princess on the GameCube has an echo of 64 sub-frames, at a level of 16225");
    const GameRules twilight_wii = RulesOf("f3e93983847ccbb9d801ad047faea0182ab466cc");
    Check(twilight_wii.category_volumes == volumes && twilight_wii.default_release == 0,
          "Twilight Princess on the Wii and in HD plays all but category 1 at 120/127");
    Check(echo_of(twilight_wii, 12000.0f),
          "Twilight Princess on the Wii has an echo of 64 sub-frames, at a level of 12000");
    const GameRules twilight_hd = RulesOf("f3e93983847ccbb9d801ad047faea0182ab466cc", {.wii_u = true});
    Check(twilight_hd.echo && twilight_hd.echo->delay == 61 && twilight_hd.echo->feedback == 0x3fff / 32768.0f &&
              twilight_hd.echo->volume == 0.75f && twilight_hd.echo->voice_level == 2.0f * 12000.0f / 32768.0f &&
              twilight_hd.category_volumes == volumes,
          "Twilight Princess HD gets the categories and an echo of 61 sub-frames, at a level of 12000");
    const GameRules crossbow = RulesOf("b93115de942c91600250e2742be35005708aad91");
    std::map<uint8_t, float> crossbow_volumes = volumes;
    crossbow_volumes[9] = 100.0f / 127.0f;
    Check(
        crossbow.category_volumes == crossbow_volumes && crossbow.default_release == 0 && echo_of(crossbow, 12000.0f),
        "Link's Crossbow Training plays category 9 at 100/127, all others but category 1 at 120/127, and has Twilight "
        "Princess's echo");

    // Twilight Princess gives children of track 0 a volume of 0. Child m of track 0 is the handle's child m.
    const std::map<uint32_t, uint32_t> starts{
        {0x01000000, 0xf01f}, {0x01000004, 0xc7ff}, {0x01000005, 0xf9ff}, {0x0100004d, 0xe3ff}, {0x01000087, 0xc3ff}};
    const auto started = [&](const GameRules& rules)
    {
        return std::ranges::all_of(starts,
                                   [&](const auto& start)
                                   {
                                       const auto state = rules.start_states.find(start.first);
                                       return state != rules.start_states.end() &&
                                              state->second.tracks == kEveryTrack && state->second.game_ports.empty() &&
                                              mask(state->second) == start.second;
                                   });
    };
    const auto riding = [&](const GameRules& rules)
    {
        const auto a = find(rules, 0x01000000, "riding");
        return a != rules.arrangements.end() && mask(a->state) == 0x0fff && a->state.tracks == kEveryTrack;
    };
    Check(started(twilight) && riding(twilight),
          "Twilight Princess on the GameCube silences the children of track 0 that bgmStart silences, and plays Hyrule "
          "Field's day music a second time as Link rides");
    Check(started(twilight_wii) && riding(twilight_wii),
          "Twilight Princess on the Wii and in HD silences the same children");

    // The states that the game's code gives its music later (kTwilightPrincessStates).
    const auto ordon = find(twilight, 0x01000005, "status1");
    std::map<uint8_t, float> ordon_volumes{{8, 0.0f}};
    for (uint8_t child = 0; child < 8; child++)
    {
        ordon_volumes[child] = 0.65f;
    }
    Check(ordon != twilight.arrangements.end() && ordon->state.child_volumes == ordon_volumes &&
              ordon->state.game_ports == std::map<uint8_t, uint16_t>{{9, 1}},
          "Ordon Village's status 1 plays children 0 to 7 at 0.65 without child 8, with 1 on port 9");
    Check(children(twilight, 0x0100003b) == 0xe01f && children(twilight_wii, 0x0100003b) == 0xe01f,
          "Castle Town starts with room 0's children");
    const auto demo = find(twilight, 0x01000038, "status1");
    Check(demo != twilight.arrangements.end() && demo->state.child_volumes.at(0) == 0.3f &&
              demo->state.child_volumes.at(3) == 0.6f && mask(demo->state) == 0xffef,
          "Z2BGM_DEMO10's status 1 plays children 0 and 1 at 0.3 and 2 and 3 at 0.6");
    const auto variant = twilight.start_states.find(0x01000039);
    const auto variant_2 = find(twilight, 0x01000039, "status2");
    Check(variant != twilight.start_states.end() && mask(variant->second) == 0xf8ff &&
              variant->second.game_ports == std::map<uint8_t, uint16_t>{{9, 1}} &&
              variant_2 != twilight.arrangements.end() && mask(variant_2->state) == 0xfffe &&
              find(twilight, 0x01000039, "status1") == twilight.arrangements.end(),
          "Z2BGM_VARIANT starts without children 8 to 10, and its status 2 plays them in place of child 0");
    const auto sumo = find(twilight, 0x01000024, "status5");
    Check(sumo != twilight.arrangements.end() && sumo->state.child_volumes.empty() &&
              sumo->state.game_ports == std::map<uint8_t, uint16_t>{{9, 5}},
          "Z2BGM_SUMO's status 5 writes 5 to port 9 and sets no child");
    const auto zant = find(twilight, 0x01000062, "status11");
    Check(zant != twilight.arrangements.end() && zant->state.game_ports == std::map<uint8_t, uint16_t>{{9, 11}},
          "Z2BGM_BOSS_ZANT's last section is status 11");
    const auto ganon_1 = find(twilight, 0x01000088, "status1");
    Check(ganon_1 != twilight.arrangements.end() && mask(ganon_1->state) == 0x0fff &&
              ganon_1->state.game_ports == std::map<uint8_t, uint16_t>{{9, 1}},
          "Z2BGM_VS_GANON_04's status 1 plays without children 12 to 15, with 1 on port 9");
    const auto darknut = find(twilight, 0x0100006c, "status1");
    Check(darknut != twilight.arrangements.end() && darknut->state.game_ports == std::map<uint8_t, uint16_t>{{9, 1}} &&
              twilight.start_states.find(0x0100006c) == twilight.start_states.end(),
          "Z2BGM_TN_MBOSS's status 1 is a state, and its start writes nothing to port 9");
    const auto ganon = find(twilight, 0x01000087, "status1");
    Check(ganon != twilight.arrangements.end() && mask(ganon->state) == 0xff00 &&
              ganon->state.game_ports == std::map<uint8_t, uint16_t>{{9, 1}},
          "Z2BGM_VS_GANON_02's status 1 plays children 10 to 13 in place of 0 to 7");
    const auto horse_2 = find(twilight, 0x01000004, "status2");
    const auto battle_end = find(twilight_wii, 0x0100000f, "port9_2");
    Check(horse_2 != twilight.arrangements.end() && mask(horse_2->state) == 0xc7ff &&
              horse_2->state.game_ports == std::map<uint8_t, uint16_t>{{9, 2}} &&
              battle_end != twilight_wii.arrangements.end() && battle_end->state.child_volumes.empty() &&
              battle_end->state.game_ports == std::map<uint8_t, uint16_t>{{9, 2}},
          "a status that changes only port 9 is a state where the sequence reads it");
    const auto battle = find(twilight_wii, 0x0100000f, "near");
    const auto final_phase = find(twilight_wii, 0x0100000f, "port9_1");
    Check(children(twilight_wii, 0x0100000f) == 0xaff1 && battle != twilight_wii.arrangements.end() &&
              mask(battle->state) == 0xaff3 && final_phase != twilight_wii.arrangements.end() &&
              final_phase->state.child_volumes.empty() &&
              final_phase->state.game_ports == std::map<uint8_t, uint16_t>{{9, 1}},
          "the battle music starts without children 1 to 3, 12 and 14, and brings them in as the fight goes");
    // The Wii's code tests Z2BGM_DUNGEON_LV9_02's status for rooms 8 and 0x33, not 12. In each frame after the start,
    // the game passes Link's height as the status, and 3150 at the foot of room 12 gives room 12's volumes. The Wii's
    // archive has the GameCube's states, with room 12 as status 3150.
    const auto same_but_room_12 = [](const Arrangement& a, const Arrangement& b)
    {
        return a.sound == b.sound && (a.suffix == b.suffix || (a.suffix == "status12" && b.suffix == "status3150"));
    };
    const auto room_12 = find(twilight, 0x0100003f, "status12");
    const auto foot = find(twilight_wii, 0x0100003f, "status3150");
    Check(std::ranges::equal(twilight.arrangements, twilight_wii.arrangements, same_but_room_12) &&
              room_12 != twilight.arrangements.end() && foot != twilight_wii.arrangements.end() &&
              foot->state.child_volumes == room_12->state.child_volumes &&
              foot->state.game_ports == std::map<uint8_t, uint16_t>{{9, 3150}},
          "the Wii's archive has the GameCube's states, with Z2BGM_DUNGEON_LV9_02's room 12 as status 3150");
    // HD's code gives Z2BGM_DUNGEON_LV9_02 a start and other rooms, and the twilight battle music writes port 9.
    const auto castle_hd = twilight_hd.start_states.find(0x0100003f);
    const auto twilight_battle_hd = find(twilight_hd, 0x0100001b, "status3");
    const auto twilight_battle_wii = find(twilight_wii, 0x0100001b, "status3");
    Check(children(twilight, 0x0100003f) == 0xffff && castle_hd != twilight_hd.start_states.end() &&
              mask(castle_hd->second) == 0xfc78 && castle_hd->second.child_volumes.at(3) == 0.3f &&
              castle_hd->second.child_volumes.at(6) == 0.3f &&
              find(twilight_hd, 0x0100003f, "status12") == twilight_hd.arrangements.end() &&
              find(twilight_hd, 0x0100003f, "status3150") != twilight_hd.arrangements.end() &&
              find(twilight, 0x0100003f, "status12") != twilight.arrangements.end() &&
              find(twilight_wii, 0x0100003f, "status12") == twilight_wii.arrangements.end() &&
              twilight_battle_hd != twilight_hd.arrangements.end() &&
              twilight_battle_hd->state.game_ports == std::map<uint8_t, uint16_t>{{9, 3}} &&
              twilight_battle_wii != twilight_wii.arrangements.end() && twilight_battle_wii->state.game_ports.empty() &&
              twilight_hd.arrangements.size() == twilight_wii.arrangements.size(),
          "HD's states differ in Z2BGM_DUNGEON_LV9_02 and in the twilight battle music's port 9");
    // The Wii's first USA version and its European release hold other start-up archives. Their code gives the same
    // rules as the code of the third version.
    const auto same_state = [](const SequenceState& a, const SequenceState& b)
    {
        return a.child_volumes == b.child_volumes && a.tempo_ratio == b.tempo_ratio && a.game_ports == b.game_ports &&
               a.fade_in == b.fade_in && a.tracks == b.tracks && a.status_by_section == b.status_by_section;
    };
    for (const char* sha1 : {"b17393f6f9a15d891c8aa02682c6e5ee08b3f0c4", "d7d9d7b67d6ef6334a31d1d71be87a80e012cf0a"})
    {
        const GameRules release = RulesOf(sha1);
        Check(release.category_volumes == volumes && echo_of(release, 12000.0f) && started(release) &&
                  riding(release) && release.ports == twilight_wii.ports &&
                  std::ranges::equal(release.start_states, twilight_wii.start_states, [&](const auto& a, const auto& b)
                                     { return a.first == b.first && same_state(a.second, b.second); }) &&
                  std::ranges::equal(
                      release.arrangements, twilight_wii.arrangements, [&](const Arrangement& a, const Arrangement& b)
                      { return a.sound == b.sound && a.suffix == b.suffix && same_state(a.state, b.state); }),
              "the Wii's first USA version and its European release get the third version's rules");
    }
    // The scenes of Link's Crossbow Training and one boss give three pieces states (kCrossbowTrainingStates).
    const auto snow_ruins = find(crossbow, 0x01000037, "status1");
    const auto boss = find(crossbow, 0x0100006c, "status1");
    const std::map<uint8_t, uint16_t> status_1{{9, 1}};
    Check(started(crossbow) && riding(crossbow) && crossbow.start_states.size() == starts.size() + 2 &&
              children(crossbow, 0x01000037) == 0xc0ff && children(crossbow, 0x01000041) == 0xff8f &&
              crossbow.arrangements.size() == 3 && snow_ruins != crossbow.arrangements.end() &&
              mask(snow_ruins->state) == 0xffe0 && snow_ruins->state.game_ports == status_1 &&
              boss != crossbow.arrangements.end() && boss->state.child_volumes.empty() &&
              boss->state.game_ports == status_1,
          "Link's Crossbow Training silences the same children, starts the snow ruins' music without children 8 to 13 "
          "and the grove's without 4 to 6, and plays two pieces with status 1");
    // The Japanese release of Link's Crossbow Training holds another start-up archive. Its code gives the same rules.
    const GameRules crossbow_japan = RulesOf("53f7fd5c6de729cd161e877ca1bbd5689e163cf1");
    Check(crossbow_japan.category_volumes == crossbow.category_volumes &&
              crossbow_japan.default_release == crossbow.default_release && echo_of(crossbow_japan, 12000.0f) &&
              crossbow_japan.ports == crossbow.ports &&
              std::ranges::equal(crossbow_japan.start_states, crossbow.start_states, [&](const auto& a, const auto& b)
                                 { return a.first == b.first && same_state(a.second, b.second); }) &&
              std::ranges::equal(
                  crossbow_japan.arrangements, crossbow.arrangements, [&](const Arrangement& a, const Arrangement& b)
                  { return a.sound == b.sound && a.suffix == b.suffix && same_state(a.state, b.state); }),
          "the Japanese release of Link's Crossbow Training gets the USA release's rules");
    const std::map<uint32_t, uint32_t> pikmin2_starts = {{0x5829, 3}, {0x58fc, 1}, {0x58fe, 1}};
    Check(RulesOf("a8b63d2f333c34aaf9cb859f37f23011149af917").random_starts == pikmin2_starts &&
              RulesOf("6747c3c7bce964bc62f817e1c8c32711c905c4c3").random_starts == pikmin2_starts &&
              usa.random_starts.empty() && wind_waker.random_starts.empty(),
          "Pikmin 2 lists the draws before its three random effects play, and other games list none");
    const GameRules luigi = RulesOf("4a60396b9c35333f3083848be1f49c82bf9ba4df");
    const auto state = [&](std::size_t i, std::string_view suffix, uint32_t playing, float tempo_ratio)
    {
        return i < luigi.arrangements.size() && luigi.arrangements[i].sound == 0x80000007 &&
               luigi.arrangements[i].suffix == suffix && mask(luigi.arrangements[i].state) == playing &&
               luigi.arrangements[i].state.tracks == kEveryTrack &&
               std::abs(luigi.arrangements[i].state.tempo_ratio - tempo_ratio) < 1e-6f;
    };
    Check(luigi.arrangements.size() == 13 && luigi.only_arranged == std::vector<uint32_t>{0x80000007},
          "Luigi's Mansion plays LuigiSings only in its 13 states");
    Check(state(0, "hallway_100_HP", 0xf81f, 1.0f) && state(1, "hallway_70_HP", 0xf82f, 0.88f) &&
              state(2, "hallway_40_HP", 0xf84f, 0.76f),
          "in the hallways, children 0 to 3 and 11 play with the tune of Luigi's health");
    Check(state(3, "room_100_HP", 0xf510, 1.0f) && state(6, "outdoors_100_HP", 0xf210, 1.0f) &&
              state(11, "unaccompanied_40_HP", 0xf040, 0.76f),
          "the rooms play children 8 and 10, outdoors child 9, and the others no accompaniment");
    Check(state(12, "lit_room", 0xf080, 0.9f), "a lit room plays child 7 alone at 0.9 of the tempo");
    Check(!luigi.effect_parameters && usa.effect_parameters && wind_waker.effect_parameters,
          "Luigi's Mansion plays its effects at full volume, whatever the sound table gives them");
    Check(!luigi.squared_track_volume && wind_waker.squared_track_volume,
          "Luigi's Mansion plays a track's volume as it is, and The Wind Waker squares it");
    Check(luigi.timed_random && !wind_waker.timed_random && !usa.timed_random,
          "Luigi's Mansion's generator reads the time base, and The Wind Waker's doesn't");
    Check(!RulesOf("a8b63d2f333c34aaf9cb859f37f23011149af917").sequence_names &&
              !RulesOf("6747c3c7bce964bc62f817e1c8c32711c905c4c3").sequence_names,
          "Pikmin 2's sequences keep their files' names on the GameCube and the Wii");

    const GameRules galaxy = RulesOf("01611ea8ef87b9a6fcec990ec7c9accfb7c00720");
    const GameRules galaxy2 = RulesOf("037ac96ba485f943bdabef4e18b579df9a985f99");
    const std::map<uint8_t, float> categories{{0, 1.2f},  {1, 1.2f},  {2, 1.2f}, {3, 1.3f}, {4, 1.2f},
                                              {5, 1.2f},  {6, 1.2f},  {7, 1.2f}, {8, 1.2f}, {9, 1.2f},
                                              {10, 1.2f}, {11, 1.2f}, {13, 1.5f}};
    Check(galaxy.sequence_volume == 1.2f && galaxy2.sequence_volume == 1.2f && galaxy.category_volumes == categories &&
              galaxy2.category_volumes == categories,
          "both Galaxy games play their music at 1.2 and their effects at the first set's category volumes");
    Check(galaxy.unplayed_groups == std::set<uint16_t>{0x0101} && galaxy2.unplayed_groups == galaxy.unplayed_groups &&
              usa.unplayed_groups.empty(),
          "both Galaxy games leave out their multi BGMs' entries: the game never starts them as sequences");
    Check(galaxy.echo && galaxy.echo->delay == 64 && galaxy.echo->feedback == 0x4fff / 32768.0f &&
              galaxy.echo->volume == 0x7000 / 32768.0f && galaxy.echo->voice_level == 0.732421875f,
          "Galaxy's echo has a delay of 64 sub-frames, a filter's gain of 0x4fff and a volume of 0x7000");
    Check(galaxy2.echo && galaxy2.echo->delay == 69 && galaxy2.echo->feedback == 0.5135f &&
              galaxy2.echo->volume == 0x7fff / 32768.0f && galaxy2.echo->voice_level == 0.732421875f,
          "Galaxy 2's echo has a delay of 69 sub-frames, its filter's largest gain and a volume of 0x7fff");
    const auto luigi_voices = [](const GameRules& rules)
    {
        return rules.port_variants.size() == 1 && rules.port_variants[0].group == 0x0001 &&
               rules.port_variants[0].suffix == "_Luigi" &&
               rules.port_variants[0].ports == std::map<uint8_t, uint16_t>{{0xc, 20}};
    };
    Check(luigi_voices(galaxy) && luigi_voices(galaxy2) && usa.port_variants.empty(),
          "both Galaxy games list the player's voices again with port 0xc at 20, as Luigi's");
    Check(galaxy.rhythm_mutes_child_0 && galaxy2.rhythm_mutes_child_0 && !usa.rhythm_mutes_child_0,
          "both Galaxy games' rhythm mutes child 0 of a rhythm piece's track 0");
    Check(galaxy.start_states.size() == 18 && galaxy.start_states.at(0x01000008).tracks[0] == 0x3ffe &&
              galaxy.start_states.at(0x0100004b).tracks[0] == 0x07fe &&
              galaxy.start_states.at(0x01000005).tracks[0] == 0 &&
              galaxy.start_states.at(0x01000008).tracks[1] == 0xffff && !galaxy.start_states.contains(0x0100001d),
          "Galaxy's pieces with states start with the children of track 0 that their first states leave on");
    Check(galaxy2.start_states.size() == 41 && galaxy2.start_states.at(0x0100000a).tracks[0] == 0xc73e &&
              galaxy2.start_states.at(0x01000017).tracks[0] == 0x019e &&
              galaxy2.start_states.at(0x01000045).tracks[0] == 0,
          "so do Galaxy 2's");
    Check(!galaxy.volume_table && galaxy2.volume_table && galaxy2.volume_table->archive == "Info/BgmParam.arc" &&
              galaxy2.volume_table->file == "BgmParam.bcsv" && galaxy2.volume_table->name_field == "SoundIdName" &&
              galaxy2.volume_table->volume_field == "Volume" && !usa.volume_table,
          "Galaxy 2 takes its pieces' volumes from BgmParam.arc");
}

// Super Mario Galaxy 2's table of volumes: a JMap table in a RARC archive. Each field's name is a hash. The table
// gives two sounds their volumes. The game writes a value's low byte. The archive may be compressed. A table without
// the file or the field can't be read.
void TestVolumeTable()
{
    const GameRules::VolumeTable table{"Info/BgmParam.arc", "BgmParam.bcsv", "SoundIdName", "Volume"};
    std::vector<uint8_t> bcsv;
    const auto u32 = [&](uint32_t v)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bcsv.push_back(static_cast<uint8_t>(v >> shift));
        }
    };
    u32(2);
    u32(2);
    u32(40);
    u32(8);
    // The hashes of "Volume" and "SoundIdName" in Galaxy 2's table.
    u32(0x990fd33a);
    u32(0xffffffff);
    u32(0x00000000); // offset 0, shift 0, type 0
    u32(0xc42d0c15);
    u32(0xffffffff);
    u32(0x00040006); // offset 4, shift 0, type 6
    u32(105);
    u32(0);
    u32(0x17f);
    u32(6);
    for (const char c : std::string_view("BGM_A\0BGM_B\0", 12))
    {
        bcsv.push_back(static_cast<uint8_t>(c));
    }

    const std::vector<uint8_t> arc = files::RarcFile({{0, "BgmParam.bcsv", bcsv}});
    const std::map<std::string, uint8_t> expected{{"BGM_A", 105}, {"BGM_B", 0x7f}};
    Check(ReadVolumeTable(arc, table) == expected, "the table gives each sound its volume's low byte");
    Check(ReadVolumeTable(files::LiteralYaz0(arc), table) == expected, "and the compressed archive the same");

    const auto fails = [&](const std::vector<uint8_t>& archive, const GameRules::VolumeTable& rules)
    {
        try
        {
            ReadVolumeTable(archive, rules);
        }
        catch (const FormatError&)
        {
            return true;
        }

        return false;
    };
    GameRules::VolumeTable other_file = table;
    other_file.file = "Other.bcsv";
    GameRules::VolumeTable other_field = table;
    other_field.volume_field = "Level";
    Check(fails(arc, other_file) && fails(arc, other_field), "a table without its file or field can't be read");
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

void TestLateChangeAfterTheSoundIsLeftOut()
{
    // Track 1 plays key 60 for 12 ticks, waits, sets its volume and finishes a tick later. At timebase 48 and tempo
    // 120, a tick takes 10.4 ms. After a wait of 900 ticks the change comes 9.4 s after the note, and it ends the
    // performance. After a wait of 1,200 ticks it comes 12.5 s after the note, more than kHoldSeconds
    // (EndsAtLastSound). The performance then ends with the sound, and no event lies after its end. The root track
    // closes track 1 when it finishes. It therefore waits 1,500 ticks, longer than track 1 plays.
    const auto perform = [](uint32_t blocks)
    {
        Sequence s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120);
        for (uint32_t b = 0; b < 6; b++)
        {
            s.Wait(250);
        }
        s.Finish().PadTo(kChild);
        s.Bank(0).Program(0).NoteOn(60, 1, 127).Wait(12).NoteOff(1);
        for (uint32_t b = 0; b < blocks; b++)
        {
            s.Wait(150);
        }
        s.Volume(64).Wait(1).Finish();
        TestGame game(files::BankV0File(0, 0), {s.Data()});

        return Perform(game.Sound(0), game.banks, {}, kProgram);
    };
    const Performance near = perform(6);
    const Performance far = perform(8);

    const auto after_end = [&far](const TrackEvent& e)
    {
        return e.tick > far.end_tick;
    };
    bool late = false;
    for (const std::vector<TrackEvent>& events : far.tracks)
    {
        late = late || std::ranges::any_of(events, after_end);
    }
    Check(!near.loop && std::abs(near.seconds - 9.5) < 0.05,
          "a change 9.4 s after the sound ends the performance: " + std::to_string(near.seconds));
    Check(!far.loop && far.seconds < 2.0,
          "a change 12.5 s after the sound is left out: " + std::to_string(far.seconds));
    Check(!late, "no event lies after the end");
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

    // A gate note that sweeps from the latest key and ties the next note to this one (bits 6 and 5 of the flags).
    Sequence1& SweepGate(uint32_t key, uint32_t velocity, uint32_t share, uint32_t length)
    {
        b_.U8(key).U8(0x68).U8(velocity).U8(share).U8(length);

        return *this;
    }

    Sequence1& LatestKey(uint32_t key)
    {
        b_.U8(0xd4).U8(key);

        return *this;
    }

    // The track's oscillator 0 as an envelope: up to full over `attack`, held for `hold`, then to `sustain` over
    // `decay`. `release` is the notes' direct release.
    Sequence1& SimpleAdsr(uint32_t attack, uint32_t hold, uint32_t decay, uint32_t sustain, uint32_t release)
    {
        b_.U8(0xd8).U16(attack).U16(hold).U16(decay).U16(sustain).U16(release);

        return *this;
    }

    // The simple oscillator `which`: 1 sets the track's oscillator 0, and 0 and 2 set its oscillator 1.
    Sequence1& SimpleOsc(uint32_t which)
    {
        b_.U8(0xd6).U8(which);

        return *this;
    }

    // Route `route` for the track's oscillator `osc`: 0 to 3 put it in place of that oscillator of the notes.
    Sequence1& OscRoute(uint32_t osc, uint32_t route)
    {
        b_.U8(0xf0).U8(osc << 4 | route);

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

    // A call to the routine at entry n of the table of 24-bit offsets at `table`, where register `reg` holds n.
    Sequence1& CallTable(uint32_t reg, uint32_t table)
    {
        b_.U8(0xc4).U8(0xc0).U8(reg).U24(table);

        return *this;
    }

    Sequence1& Return()
    {
        b_.U8(0xc6).U8(0);

        return *this;
    }

    // An entry of a table of offsets.
    Sequence1& Offset(uint32_t offset)
    {
        b_.U24(offset);

        return *this;
    }

    // Port `port`'s value goes to register `reg`.
    Sequence1& ReadPort(uint32_t port, uint32_t reg)
    {
        b_.U8(0xcb).U8(port).U8(reg);

        return *this;
    }

    // A random number below `count` goes to register `reg` (register op 0x90 with a byte).
    Sequence1& Random(uint32_t reg, uint32_t count)
    {
        b_.U8(0xa9).U8(0x94).U8(reg).U8(count);

        return *this;
    }

    // Register `reg`'s value goes to port `port`.
    Sequence1& WritePort(uint32_t port, uint32_t reg)
    {
        b_.U8(0xcc).U8(port).U8(reg);

        return *this;
    }

    // Volume mode `mode`: 0 squares the track's volume, and 1 plays it as it is.
    Sequence1& VolumeMode(uint32_t mode)
    {
        b_.U8(0xf3).U8(mode);

        return *this;
    }

    // The track's volume, in 32767ths.
    Sequence1& Volume(uint32_t volume)
    {
        b_.U8(0x9c).U8(0).U16(volume);

        return *this;
    }

    // The track's pan and effect send, in 32767ths.
    Sequence1& Pan(uint32_t pan)
    {
        b_.U8(0x9c).U8(3).U16(pan);

        return *this;
    }

    Sequence1& Send(uint32_t send)
    {
        b_.U8(0x9c).U8(2).U16(send);

        return *this;
    }

    // The pan powers of a note's bank value, effects' value and track value, then the parent's and the track's in
    // hundredths (cmdPanPowSet).
    Sequence1& PanPowers(uint32_t bank, uint32_t effects, uint32_t track, uint32_t parent, uint32_t own)
    {
        b_.U8(0xeb).U8(bank).U8(effects).U8(track).U8(parent).U8(own);

        return *this;
    }

    // The rules for its notes' pan, effect send and surround (cmdPanSwSet), with the parent's values left out.
    Sequence1& PanRules(uint32_t pan, uint32_t send, uint32_t surround)
    {
        b_.U8(0xef).U8(pan).U8(send).U8(surround);

        return *this;
    }

    // Mix line `line` of the track's channels connects to `bus` (TTrack::connectBus).
    Sequence1& BusConnect(uint32_t line, uint32_t bus)
    {
        b_.U8(0xdd).U8(line).U16(bus);

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
Performance PerformJAudio1(const std::vector<uint8_t>& sequence, uint32_t draws = 0, bool timed_random = false,
                           bool routes = true)
{
    TestGame game(files::BankV0File(0, 60), {sequence});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    sound.draws = draws;
    sound.timed_random = timed_random;
    sound.track_oscillator_routes = routes;

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

// Track 0 sweeps from key 52 to key 57 and ties a note of key 57 to it, as The Wind Waker's se0-0065 does. JAudio1's
// tie sets the pitch at key 57. The note then holds 5 semitones above its start. A sweep that runs for 125% of its
// gate's length is still under way at the tie: from key 48 to key 60, tied to key 50 after 80% of it. The pitch then
// goes to key 50 and sweeps on to key 60.
void TestJAudio1SweepAndTie()
{
    const auto bends = [](uint32_t from, uint32_t to, uint32_t share, uint32_t tied)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).LatestKey(from).SweepGate(to, 100, share, 48).Gate(tied, 100, 100, 48).Finish();
        const Performance perf = PerformJAudio1(s.Data());

        std::vector<std::pair<uint32_t, float>> out;
        for (const TrackEvent& e : perf.tracks[0])
        {
            if (e.kind == EventKind::kPitch)
            {
                out.emplace_back(e.tick, e.semitones);
            }
        }

        return out;
    };

    const auto held = bends(52, 57, 100, 57);
    Check(!held.empty() && std::abs(held.back().second - 5.0f) < 0.01f, "the tied note holds at the sweep's key");
    Check(std::ranges::all_of(held, [](const auto& b) { return b.second < 5.01f; }),
          "the tie adds nothing to a sweep that has ended");

    const auto moving = bends(48, 60, 125, 50);
    const auto at_tie = std::ranges::find_if(moving, [](const auto& b) { return b.first >= 48; });
    Check(at_tie != moving.end() && std::abs(at_tie->second - 2.0f) < 0.5f, "the tie moves the pitch to key 50");
    Check(!moving.empty() && std::abs(moving.back().second - 12.0f) < 0.01f, "the sweep goes on to key 60");
}

// Track 0 sets a simple ADSR and routes its oscillator 0 to the notes' oscillator 0, as The Wind Waker's effects do
// through the routine at 0x15d of defaultse.bms. The note then plays the track's envelope: its attack table rises to
// full over 3 units, holds for 10 and falls to 0 over 50. Without the route, the note plays the instrument's
// oscillators. A new program after the route clears it (JASystem::TTrack, 0x8028369c in The Wind Waker's main.dol).
void TestJAudio1TrackOscillator()
{
    struct Case
    {
        bool route = true;
        uint32_t slot = 0;
        bool program_after = false;
        bool simple_osc_after = false;
        bool routes = true; // GameRules::track_oscillator_routes
    };
    const auto play = [](const Case& c)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0);
        if (c.route)
        {
            s.OscRoute(0, c.slot);
        }
        s.SimpleAdsr(3, 10, 50, 0, 50);
        if (c.program_after)
        {
            s.Program(0);
        }
        if (c.simple_osc_after)
        {
            s.SimpleOsc(1);
        }
        s.Gate(60, 100, 100, 24).Finish();

        return PerformJAudio1(s.Data(), 0, false, c.routes);
    };

    const Performance routed = play({});
    const int volume = routed.presets.empty() ? -1 : routed.presets[0].volume_oscillator;
    Check(routed.presets.size() == 1 && volume >= 0, "a routed note plays the track's volume oscillator");
    if (volume >= 0)
    {
        const Oscillator& osc = routed.oscillators[static_cast<std::size_t>(volume)];
        const std::vector<std::array<int, 3>> points{{0, 3, 0x7fff}, {0, 10, 0x7fff}, {0, 50, 0}, {14, 0, 0}};
        bool same = osc.attack.size() == points.size() && osc.release.empty() && osc.target == 0;
        for (std::size_t i = 0; same && i < points.size(); i++)
        {
            same = osc.attack[i].mode == points[i][0] && osc.attack[i].time == points[i][1] &&
                   osc.attack[i].value == points[i][2];
        }
        Check(same, "the track's oscillator holds the simple ADSR's table and no release table");
    }
    Check(routed.presets.size() == 1 && routed.presets[0].direct_release == 50, "the ADSR gives the direct release");

    const Performance unrouted = play({.route = false});
    Check(unrouted.presets.size() == 1 && unrouted.presets[0].volume_oscillator < 0,
          "without a route, the note plays the instrument's oscillators");
    const Performance cleared = play({.program_after = true});
    Check(cleared.presets.size() == 1 && cleared.presets[0].volume_oscillator < 0, "a new program clears the route");

    // Route 1 puts the track's oscillator in place of the note's second. The zones count it with the volume
    // oscillators, by its target.
    const Performance second = play({.slot = 1});
    Check(second.presets.size() == 1 && second.presets[0].volume_oscillator < 0 &&
              second.presets[0].pitch_oscillator >= 0,
          "route 1 gives the track's oscillator in place of the note's second");

    // simpleosc 1 gives the track's oscillator 0 a template that the performer leaves out. The note keeps the
    // instrument's oscillator.
    const Performance unknown = play({.simple_osc_after = true});
    Check(unknown.presets.size() == 1 && unknown.presets[0].volume_oscillator < 0,
          "a track's oscillator that simpleosc changed leaves the note's as it is");

    // Without the game's rule, the route is left out. The ADSR still gives the direct release.
    const Performance ruled_out = play({.routes = false});
    Check(ruled_out.presets.size() == 1 && ruled_out.presets[0].volume_oscillator < 0 &&
              ruled_out.presets[0].direct_release == 50,
          "a game without track_oscillator_routes leaves the route out");
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

// Track 0 draws a random number below 8 into register 0 and compares it with a value. It jumps on a flag of 0 to key
// 72, and otherwise plays key 60. JAudio1's generator starts in the game's state. Its first draw, 0xf0da6c9b, gives 3,
// and the second gives 2 (JASystem::Player::getRandomS32 in The Wind Waker). The Wind Waker's se4-0254 adds the same
// draw to key 48. A sound's draws come before its
// first. A game whose generator reads the time base (Luigi's Mansion's) draws from JAudio2's generator. From seed 0,
// that one's first number is 0x3c6ef35f, and the draw gives 7.
void TestJAudio1Random()
{
    const auto jumps = [](uint32_t draws, uint32_t compared, bool timed_random = false)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).Random(0, 8).Register(3, 0, compared);
        const uint32_t jump = s.Here();
        s.Jump(1, 0).NoteOn(60, 1, 100).Wait(10).NoteOff(1).Finish();
        const uint32_t other = s.Here();
        s.NoteOn(72, 1, 100).Wait(10).NoteOff(1).Finish();
        std::vector<uint8_t> data = s.Data();
        const uint32_t at = jump - kFirstSequence;
        data[at + 2] = static_cast<uint8_t>(other >> 16);
        data[at + 3] = static_cast<uint8_t>(other >> 8);
        data[at + 4] = static_cast<uint8_t>(other);
        const Performance perf = PerformJAudio1(data, draws, timed_random);

        return NoteTicks(perf, 0, 72).size() == 2 && NoteTicks(perf, 0, 60).empty();
    };

    Check(jumps(0, 3), "the first draw gives 3");
    Check(!jumps(0, 2), "the first draw doesn't give 2");
    Check(jumps(1, 2), "after one draw, the next gives 2");
    Check(!jumps(1, 3), "after one draw, the next doesn't give 3");
    Check(jumps(0, 7, true) && !jumps(0, 3, true), "with a generator that reads the time base, the first draw gives 7");
}

// A game's code can mute child 15 of a track that calls it with ID 0 (SequenceSound::mute_child_15). The first track
// opens children 0 and 15. Each calls the game with `id` and plays a note. Child 15 opens its child 0 first, and that
// child plays a note too. With the rule and ID 0, child 15 and its child stay silent. Child 0 plays. Without the rule,
// or with ID 1, every track plays.
void TestMuteChild15()
{
    constexpr uint32_t kFifteen = kChild + 0x20;
    constexpr uint32_t kGrandchild = kFifteen + 0x20;
    const auto play = [](uint32_t id, bool rule)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).OpenTrack(15, kFifteen).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Byte(0xe7).Byte(0).Byte(id).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
        s.PadTo(kFifteen).Byte(0xe7).Byte(0).Byte(id).OpenTrack(0, kGrandchild);
        s.Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
        s.PadTo(kGrandchild).Bank(0).Program(0).NoteOn(67, 1, 100).Wait(24).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.mute_child_15 = rule;
        const Performance perf = Perform(sound, game.banks, {}, kProgram);

        return std::array{NoteTicks(perf, "Track 0").size(), NoteTicks(perf, "Track 15").size(),
                          NoteTicks(perf, "Track 15.0").size()};
    };

    using Counts = std::array<std::size_t, 3>;
    Check(play(0, true) == Counts{1, 0, 0}, "with the rule, child 15 and its child start no notes");
    Check(play(0, false) == Counts{1, 1, 1}, "without the rule, every track plays");
    Check(play(1, true) == Counts{1, 1, 1}, "a call with ID 1 mutes nothing");
}

// Pikmin's code can give children of the first track volumes through their external buffers
// (SequenceSound::child_volumes and SequenceSound::jac). The first track opens children 0 and 1, and child 0 opens a
// child. Each plays a note. Child 0 plays at 0.5 and child 1 at 0. Child 0's CC11 holds the square root of 0.5, and
// so does its child's. Child 1 starts no notes.
void TestJacChildVolumes()
{
    constexpr uint32_t kOne = kChild + 0x20;
    constexpr uint32_t kGrandchild = kOne + 0x20;
    Sequence1 s;
    s.OpenTrack(0, kChild).OpenTrack(1, kOne).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.OpenTrack(0, kGrandchild).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kOne).Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kGrandchild).Bank(0).Program(0).NoteOn(67, 1, 100).Wait(24).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    sound.jac = true;
    const Performance every = Perform(sound, game.banks, {}, kProgram);
    sound.child_volumes = {{0, 0.5f}, {1, 0.0f}};
    const Performance set = Perform(sound, game.banks, {}, kProgram);

    // The first CC11 of the MIDI track named `name`.
    const auto expression = [](const Performance& perf, const std::string& name)
    {
        const std::vector<TrackEvent> events = NamedTrackEvents(perf, name, EventKind::kExpression);
        return events.empty() ? -1 : static_cast<int>(events.front().value);
    };

    const auto half = static_cast<int>(std::lround(expression(every, "Track 0") * std::sqrt(0.5)));
    Check(NoteTicks(every, "Track 1").size() == 1 && NoteTicks(set, "Track 1").empty(), "child 1 at 0 starts no notes");
    Check(NoteTicks(set, "Track 0").size() == 1 && NoteTicks(set, "Track 0.0").size() == 1,
          "child 0 and its child play");
    Check(
        expression(every, "Track 0") > 0 && expression(set, "Track 0") == half && expression(set, "Track 0.0") == half,
        "child 0 and its child play at half the level");
}

// A game's code can answer the calls of a sequence's tracks (SequenceSound::sync_answers) and mute children of the
// first track as they start (SequenceSound::muted_children). Like a root track of Pikmin 2's music, the first track
// opens children 0 and 1, then waits for as many ticks as call 0x2e00 gives, without end. Each child plays a note,
// waits 96 ticks and plays another. With the answer of 60 ticks, child 0 plays both notes, and child 1 is muted.
// Without an answer, the call gives 0. The first track then loops without waiting, and the performer stops it at the
// first tick.
void TestSyncAnswers()
{
    constexpr uint32_t kOne = kChild + 0x20;
    constexpr uint32_t kLoop = kFirstSequence + 0x10;
    const auto play = [](bool rules)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).OpenTrack(1, kOne).Timebase(48).Tempo(120).PadTo(kLoop);
        s.Byte(0xe7).Byte(0x2e).Byte(0x00).Byte(0xcf).Byte(3).Jump(0, kLoop).PadTo(kChild);
        s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Wait(72).NoteOn(62, 1, 100).Wait(24).NoteOff(1);
        s.Finish().PadTo(kOne);
        s.Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Wait(72).NoteOn(65, 1, 100).Wait(24).NoteOff(1);
        s.Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        if (rules)
        {
            sound.sync_answers = {{0x2e00, 60}};
            sound.muted_children = 1u << 1;
        }

        return Perform(sound, game.banks, {}, kProgram);
    };

    const Performance with = play(true);
    const Performance without = play(false);
    Check(NoteTicks(with, "Track 0").size() == 2, "with the answer, child 0 plays both notes");
    Check(NoteTicks(with, "Track 1").empty(), "child 1 is muted");
    Check(with.warnings.empty(), "with the answer, the first track waits");
    Check(NoteTicks(without, "Track 0").size() < 2 && !without.warnings.empty(),
          "without it, the first track runs without waiting and stops");
}

// Pikmin 2's cave music: a conductor (SequenceSound::conductor) picks the phrases that the first track's children play.
// The first track writes the offsets of the sequence's two tables of lists to its ports 2 to 5, and call 0x300 gives
// it the conductor's tempo. It then calls 0x2e00 and waits for the conductor's 60 ticks, without end. Child 0 calls
// 0x600 and 0x800 and reads its bank and program from port 3, as a cave's track does. Then it loops: call 0x900 takes
// a step, and calls 0xa00 and 0xc00 pick a phrase and a length. The child opens the phrase at the offset that ports 6
// and 7 give registers 16 and 17, and waits for the length from port 8. A call 0xc00 that answers 1 starts a rest. The
// conductor's track has one module of four phrases and no rests, with a transpose of 12. The module's list holds
// three phrases of one note each: keys 60, 62 and 64. The game's random numbers start at 0. Its draws pick phrases 0,
// 2, 1 and 1. The round ends, and the round of rests has no rests. The next draw would give phrase 1 a third time. The
// game draws again from the other two, and the next phrases are 2, 1, 0, 2 and 1.
void TestConductor()
{
    constexpr uint32_t kLoop = kFirstSequence + 0x30;
    constexpr uint32_t kTrack = kFirstSequence + 0x40;
    constexpr uint32_t kTrackLoop = kTrack + 0x10;
    constexpr uint32_t kRest = kTrack + 0x30;
    constexpr uint32_t kTables = kFirstSequence + 0x90;
    constexpr uint32_t kLists = kTables + 0x10;
    constexpr uint32_t kPhrases = kLists + 0x20;
    Sequence1 s;
    const auto load = [&s](uint32_t reg, uint32_t value)
    {
        s.Byte(0xac).Byte(reg).Byte(value >> 8).Byte(value & 0xff);
    };
    load(0x10, 0);
    load(0x11, kTables);
    s.WritePort(2, 0x10).WritePort(3, 0x11);
    load(0x10, 0);
    load(0x11, kTables + 3);
    s.WritePort(4, 0x10).WritePort(5, 0x11);
    s.Byte(0xe7).Byte(0x03).Byte(0x00).OpenTrack(0, kTrack).Timebase(120).Byte(0xe7).Byte(0x0e).Byte(0x00);
    s.PadTo(kLoop).Byte(0xe7).Byte(0x2e).Byte(0x00).Byte(0xcf).Byte(3).Jump(0, kLoop).PadTo(kTrack);
    s.Byte(0xe7).Byte(0x06).Byte(0x00).Byte(0xcf).Byte(3).Byte(0xe7).Byte(0x08).Byte(0x00).Byte(0xcf).Byte(3);
    s.ReadPort(3, 6).PadTo(kTrackLoop);
    s.Byte(0xe7).Byte(0x09).Byte(0x00).Byte(0xe7).Byte(0x0a).Byte(0x00).Byte(0xe7).Byte(0x0c).Byte(0x00).Jump(2, kRest);
    s.ReadPort(6, 0x10).ReadPort(7, 0x11).Byte(0xb1).Byte(0xc1).Byte(0x40).Byte(0).Byte(0x28).PadTo(kRest);
    s.ReadPort(8, 4).Byte(0xcf).Byte(4).Jump(0, kTrackLoop).PadTo(kTables);
    s.Offset(kLists).Offset(kLists + 0x10).PadTo(kLists);
    s.Offset(kPhrases).Offset(kPhrases + 0x10).Offset(kPhrases + 0x20).Offset(0xffffff).PadTo(kLists + 0x10);
    s.Byte(0).Byte(120).Byte(0xff).Byte(0xff).PadTo(kPhrases);
    for (const uint32_t key : {60, 62, 64})
    {
        s.NoteOn(key, 1, 100).Wait(60).NoteOff(1).Finish().PadTo(s.Here() + 0x10 - (s.Here() - kPhrases) % 0x10);
    }

    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    sound.later_jaudio1 = true;
    Conductor conductor;
    conductor.poll = 60;
    conductor.tempo = 120;
    ConductorTrack track;
    track.modules.push_back({.beats = 64, .phrases = 4, .transpose = 12, .volume = 127, .pan = 64});
    conductor.tracks.push_back(track);
    sound.conductor = conductor;
    const Performance perf = Perform(sound, game.banks, {}, kProgram);

    std::vector<uint32_t> keys;
    for (const TrackEvent& e : NamedTrackEvents(perf, "Track 0.0", EventKind::kNoteOn))
    {
        keys.push_back(e.key);
    }
    keys.resize(std::min<std::size_t>(keys.size(), 9));
    Check(keys == std::vector<uint32_t>{72, 76, 74, 74, 76, 74, 72, 76, 74},
          "the conductor picks the game's phrases, transposed by 12");
    Check(NoteTicks(perf, "Track 0.0").size() >= 5 && NoteTicks(perf, "Track 0.0")[4] == 600,
          "the round of rests waits for a length before the fifth phrase");
}

// Pikmin 2's later JAudio1 counts down a track's wait while an interrupt runs (SequenceSound::later_jaudio1). The first
// track sets a routine for the tick's interrupt that only returns. It plays a note, waits 5 ticks twice and plays
// another. Under the later library, the second note comes at tick 10. The Wind Waker's library puts the wait aside in
// each tick, so the wait never ends.
void TestLaterJAudio1Interrupt()
{
    constexpr uint32_t kRoutine = kFirstSequence + 0x20;
    const auto play = [](bool later)
    {
        Sequence1 s;
        s.Byte(0xdf).Byte(7).Offset(kRoutine).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(5).NoteOff(1).Wait(5);
        s.NoteOn(62, 1, 100).Wait(5).NoteOff(1).Finish().PadTo(kRoutine).Byte(0xe3);
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.later_jaudio1 = later;
        return Perform(sound, game.banks, {}, kProgram);
    };

    Check(NoteTicks(play(true), "Root") == std::vector<uint32_t>{0, 10},
          "the later library counts down the wait while the interrupt runs");
    Check(NoteTicks(play(false), "Root").size() == 1, "the earlier library puts the wait aside in each tick");
}

// The later JAudio1 gives a track the current tick rate (SequenceSound::later_jaudio1). The first track opens track 0
// and halves the timebase, in either order. Track 0's gate note of 24 ticks starts in that tick. Under the later
// library, it lasts 24 ticks at the new rate. The Wind Waker's library keeps the default rate for it, so it lasts as
// long as 12.
void TestLaterJAudio1TickRate()
{
    const auto note_end = [](bool later, bool open_first)
    {
        Sequence1 s;
        if (open_first)
        {
            s.OpenTrack(0, kChild).Timebase(24).Tempo(120);
        }
        else
        {
            s.Timebase(24).Tempo(120).OpenTrack(0, kChild);
        }
        s.Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).Gate(60, 100, 100, 24).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.later_jaudio1 = later;
        const std::vector<uint32_t> ticks = NoteTicks(Perform(sound, game.banks, {}, kProgram), 0, 60);

        return ticks.size() == 2 ? ticks[1] : 0u;
    };

    for (const bool open_first : {true, false})
    {
        const uint32_t later = note_end(true, open_first);
        Check(later >= 23 && later <= 24, "the later library gives the note the first track's rate at once");
        const uint32_t earlier = note_end(false, open_first);
        Check(earlier >= 11 && earlier <= 12,
              "the earlier library keeps the default rate in the tick that opens a track");
    }
}

// A track at half the volume. In volume mode 0, the game squares the volume, as CC7 does. In mode 1, the game plays the
// volume as it is. CC7 then holds the volume's square root. A game whose JASystem never squares the volume (Luigi's
// Mansion's) plays it as it is in mode 0 too. Pikmin's library has no volume mode (its 0xf3 takes no argument) and
// plays the volume as it is.
void TestJAudio1VolumeMode()
{
    const auto cc7 = [](std::optional<uint32_t> mode, bool squared = true)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        if (mode)
        {
            s.VolumeMode(*mode);
        }
        s.Volume(0x4000).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.jac = !mode;
        sound.squared_track_volume = squared;
        const Performance perf = Perform(sound, game.banks, {}, kProgram);

        int volume = -1;
        for (const TrackEvent& e : perf.tracks[0])
        {
            volume = e.kind == EventKind::kVolume ? e.value : volume;
        }

        return volume;
    };

    // 0x4000 is half of 32767. 127 times a half is 64, and 127 times the square root of a half is 90.
    Check(cc7(0) == 64, "in volume mode 0, CC7 holds the track's volume");
    Check(cc7(1) == 90, "in volume mode 1, CC7 holds the volume's square root");
    Check(cc7(0, false) == 90 && cc7(1, false) == 90,
          "where the game never squares the volume, CC7 holds its square root in both modes");
    Check(cc7(std::nullopt) == 90, "in Pikmin's library, CC7 holds the volume's square root");
}

// The game's level of the mix reaches a track's CC11 once, for a track below another too. CC11 acts squared.
void TestMixLevel()
{
    const auto cc11 = [](float mix_level)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.mix_level = mix_level;
        const Performance perf = Perform(sound, game.banks, {}, kProgram);

        int level = -1;
        for (const TrackEvent& e : perf.tracks[0])
        {
            level = e.kind == EventKind::kExpression ? e.value : level;
        }

        return level;
    };

    // 127 times the square root of a half is 90.
    Check(cc11(1.0f) == 127, "a mix at the level of Super Mario Sunshine's leaves CC11 at full level");
    Check(cc11(0.5f) == 90, "a mix at half the level gives CC11 the square root of a half");

    const auto about = [](float value, float expected)
    {
        return std::abs(value - expected) < 0.0001f;
    };
    Check(about(RulesOf("9ae6b40b8e9eacfda6ed0b5d59ff1e680373a018").mix_level, 0.54866f),
          "Pikmin mixes at 18022 * 8192 / (13139 * 20480) of Super Mario Sunshine's level");
    Check(about(RulesOf("9f42f65be7f7159faa14b003524476d624e1227f").mix_level, 0.59851f),
          "New Play Control! Pikmin mixes at 26213 * 6144 / (13139 * 20480) of Super Mario Sunshine's level");
    Check(RulesOf(std::string(40, '0')).mix_level == 1.0f, "an unknown game mixes at Super Mario Sunshine's level");
}

// Track 0 plays drum 36 of the test bank, which pans hard right, at a pan of 0.75 and a send of 0.5. JAudio1's rule
// 13 adds the drum's pan and the track's. Rule 26 weights each by its pan power. The powers 0, 0 and 1 then leave the
// drum's pan out. Rule 2 takes the track's value alone. Rule 26 with the default powers, 0, 1 and 1, weights the
// track's pan and send by a half (JASystem::TChannel::calcPan and calcEffect).
void TestJAudio1PanRules()
{
    struct Result
    {
        double zone_pan = -1.0;
        int pan = -1;
        int send = -1;
    };
    const auto play = [](const std::function<void(Sequence1&)>& rules)
    {
        Sequence1 s;
        s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
        rules(s);
        s.Pan(0x6000).Send(0x4000).Bank(0).Program(0xe4).NoteOn(36, 1, 100).Wait(24).NoteOff(1).Finish();
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        const Performance perf = Perform(sound, game.banks, {}, kProgram);

        Result r;
        const std::vector<PresetZone> zones =
            perf.presets.size() == 1 ? ZonesOf(0, perf, game.banks) : std::vector<PresetZone>{};
        r.zone_pan = zones.size() == 1 ? zones[0].pan : -1.0;
        for (const TrackEvent& e : perf.tracks[0])
        {
            r.pan = e.kind == EventKind::kPan ? e.value : r.pan;
            r.send = e.kind == EventKind::kReverb ? e.value : r.send;
        }

        return r;
    };

    // 0x6000 is 0.75 of 32767 and 0x4000 is 0.5. 127 times 0.75 is 95, and 127 times 0.5 is 64.
    const Result sum = play([](Sequence1&) {});
    const Result track = play([](Sequence1& s) { s.PanPowers(0, 0, 1, 100, 50).PanRules(26, 1, 0); });
    const Result alone = play([](Sequence1& s) { s.PanRules(2, 2, 0); });
    const Result halves = play([](Sequence1& s) { s.PanRules(26, 26, 0); });
    Check(std::abs(sum.zone_pan - 1.0) < 1e-6 && sum.pan == 95 && sum.send == 64,
          "rule 13 adds the drum's pan to the track's");
    Check(std::abs(track.zone_pan - 0.5) < 1e-6 && track.pan == 95 && track.send == 64,
          "rule 26 with the powers 0, 0 and 1 leaves the drum's pan out");
    Check(std::abs(alone.zone_pan - 0.5) < 1e-6 && alone.pan == 95 && alone.send == 64,
          "rule 2 takes the track's pan and send alone");
    // 0.5 + 0.5 x 0.25 is 0.625: 79 of 127. Half of 0.5 is 32 of 127.
    Check(std::abs(halves.zone_pan - 0.5) < 1e-6 && halves.pan == 79 && halves.send == 32,
          "rule 26 with the default powers weights the track's pan and send by a half");
}

// The CC11 values of the MIDI track that plays `key`, in order.
std::vector<int> ExpressionOf(const Performance& perf, uint32_t key)
{
    for (const std::vector<TrackEvent>& track : perf.tracks)
    {
        std::vector<int> values;
        bool plays = false;
        for (const TrackEvent& e : track)
        {
            plays = plays || (e.kind == EventKind::kNoteOn && e.key == key);
            if (e.kind == EventKind::kExpression)
            {
                values.push_back(e.value);
            }
        }
        if (plays)
        {
            return values;
        }
    }

    return {};
}

// A game can give children of a JAudio2 sound's track 0 volumes through the sound's handle
// (SequenceSound::child_volumes). Track 0 opens children 0, 1 and 2, and child 1 again at tick 48. Each plays a note.
// At a volume of 0, child 1 plays neither time: the volume holds when track 0 opens the child again. At 0.5, child 2
// plays at half the level. CC11 acts squared, and 127 times the square root of a half is 90. Child 0 plays as it does
// without the volumes. Index 16 + m reaches the root's child 1, which the sequence doesn't open.
void TestChildVolumes()
{
    constexpr uint32_t kFirst = kFirstSequence + 0x60;
    constexpr uint32_t kSecond = kFirstSequence + 0x80;
    constexpr uint32_t kThird = kFirstSequence + 0xa0;
    Sequence s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(200).Finish().PadTo(kChild);
    s.OpenTrack(0, kFirst).OpenTrack(1, kSecond).OpenTrack(2, kThird).Wait(48).OpenTrack(1, kSecond).Wait(100);
    s.Finish().PadTo(kFirst).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kSecond).Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kThird).Bank(0).Program(0).NoteOn(67, 1, 100).Wait(24).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound set = game.Sound(0);
    set.child_volumes = {{1, 0.0f}, {2, 0.5f}, {17, 0.0f}};

    const Performance every = Perform(game.Sound(0), game.banks, {}, kProgram);
    const Performance some = Perform(set, game.banks, {}, kProgram);

    using Ticks = std::vector<uint32_t>;
    Check(NoteTicks(every, "Track 0.1") == Ticks({0, 48}) && NoteTicks(some, "Track 0.1").empty(),
          "child 1 stays silent, the second time too");
    Check(ExpressionOf(every, 67) == std::vector<int>{127} && ExpressionOf(some, 67) == std::vector<int>{90},
          "child 2 plays at half the level");
    Check(NoteTicks(some, "Track 0.0") == Ticks{0} && ExpressionOf(some, 60) == ExpressionOf(every, 60),
          "child 0 plays as it does without the volumes");
}

// Track 0 connects its mix line 0 to 0xffff: the DSP's automatic mixer, which plays its notes at half the level. Its
// child 0 opens with the default lines. Its child 1, opened in mode 4, plays on track 0's channels. Track 1 connects
// only mix line 1. Track 2 connects line 0 to 0xffff and back to the default, 0x150, before its note.
void TestJAudio1AutoMixer()
{
    constexpr uint32_t kTrack0 = kChild;
    constexpr uint32_t kTrack1 = kTrack0 + 0x40;
    constexpr uint32_t kTrack2 = kTrack1 + 0x40;
    constexpr uint32_t kChild0 = kTrack2 + 0x40;
    constexpr uint32_t kChild1 = kChild0 + 0x40;

    Sequence1 s;
    s.OpenTrack(0, kTrack0).OpenTrack(1, kTrack1).OpenTrack(2, kTrack2).Timebase(48).Tempo(120).Wait(200).Finish();
    s.PadTo(kTrack0);
    s.BusConnect(0, 0xffff).Bank(0).Program(0).OpenTrack(0, kChild0).OpenTrack(0x20 | 1, kChild1);
    s.NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish().PadTo(kTrack1);
    s.BusConnect(1, 0xffff).Bank(0).Program(0).NoteOn(62, 1, 100).Wait(24).NoteOff(1).Finish().PadTo(kTrack2);
    s.BusConnect(0, 0xffff).BusConnect(0, 0x150).Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kChild0);
    s.Bank(0).Program(0).NoteOn(65, 1, 100).Wait(24).NoteOff(1).Finish().PadTo(kChild1);
    s.Bank(0).Program(0).NoteOn(67, 1, 100).Wait(24).NoteOff(1).Finish();

    const Performance perf = PerformJAudio1(s.Data());

    // CC11 acts squared. Half the level is 127 times the square root of a half: 90.
    Check(ExpressionOf(perf, 60) == std::vector<int>{90}, "the automatic mixer plays track 0 at half the level");
    Check(ExpressionOf(perf, 62) == std::vector<int>{127}, "mix line 1 leaves the level alone");
    Check(ExpressionOf(perf, 64) == std::vector<int>{127}, "back on the default line, the level is whole");
    Check(ExpressionOf(perf, 65) == std::vector<int>{127}, "a child doesn't take its parent's lines");
    Check(ExpressionOf(perf, 67) == std::vector<int>{90}, "a child in mode 4 plays on its parent's channels");
}

// An effects sequence with slots as The Wind Waker's has them. The first track opens track 0, which holds category 0's
// slots, and waits. That track opens slots 0, 1 and 4 and waits. A slot writes 0 to its ports 0 and 2 and reads port 0
// each tick. Once port 0 holds 1, the slot writes 2 to ports 0 and 2 and calls the routine of the effect that port 4
// names. On the routine's return, it writes 0 to port 2 and reads port 0 again. Effect 0 plays key 60 for 24 ticks.
// Effect 1 plays key 64 for 48 ticks, or key 67 if port 6 holds 7. Effect 2 plays key 72 for 24 ticks of every 48, for
// ever.
std::vector<uint8_t> EffectsSequence()
{
    constexpr uint32_t kSlots = kChild;
    constexpr uint32_t kSlot = kSlots + 0x20;
    constexpr uint32_t kStart = kSlot + 0x20;
    constexpr uint32_t kRoutines = kStart + 0x20;
    constexpr uint32_t kEffect0 = kRoutines + 0x10;
    constexpr uint32_t kEffect1 = kEffect0 + 0x20;
    constexpr uint32_t kEffect1Port = kEffect1 + 0x20;
    constexpr uint32_t kEffect2 = kEffect1Port + 0x20;

    Sequence1 s;
    s.Timebase(48).OpenTrack(0, kSlots).Tempo(120);
    const uint32_t first = s.Here();
    s.Wait(255).Jump(0, first).PadTo(kSlots);
    s.OpenTrack(0, kSlot).OpenTrack(1, kSlot).OpenTrack(4, kSlot);
    const uint32_t slots = s.Here();
    s.Wait(255).Jump(0, slots).PadTo(kSlot);
    s.Register(0, 0, 0).WritePort(0, 0).WritePort(2, 0);
    const uint32_t poll = s.Here();
    s.ReadPort(0, 0).Jump(3, kStart).Wait(1).Jump(0, poll).PadTo(kStart);
    s.Register(0, 0, 2).WritePort(0, 0).WritePort(2, 0).ReadPort(4, 4).CallTable(4, kRoutines);
    s.Register(0, 0, 0).WritePort(2, 0).Jump(0, poll).PadTo(kRoutines);
    s.Offset(kEffect0).Offset(kEffect1).Offset(kEffect2).PadTo(kEffect0);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Return().PadTo(kEffect1);
    s.Bank(0).Program(0).ReadPort(6, 5).Register(3, 5, 7).Jump(1, kEffect1Port);
    s.NoteOn(64, 1, 100).Wait(48).NoteOff(1).Return().PadTo(kEffect1Port);
    s.NoteOn(67, 1, 100).Wait(48).NoteOff(1).Return().PadTo(kEffect2);
    s.Bank(0).Program(0);
    const uint32_t again = s.Here();
    s.NoteOn(72, 1, 100).Wait(24).NoteOff(1).Wait(24).Jump(0, again);

    return s.Data();
}

// Play effect `number` of EffectsSequence in slot `slot`, with `ports` and at `pitch`, as Game lists an effect: only
// the slot's track plays.
Performance PerformEffect(uint8_t slot, uint16_t number, const std::map<uint8_t, uint16_t>& ports = {},
                          float pitch = 1.0f)
{
    TestGame game(files::BankV0File(0, 60), {EffectsSequence()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    sound.pitch = pitch;
    sound.tracks.fill(0);
    sound.tracks[slot >> 4] = static_cast<uint16_t>(1u << (slot & 15));
    sound.effect = EffectStart{slot, number};
    PerformOptions options;
    options.ports = ports;

    return Perform(sound, game.banks, options, kProgram);
}

// The keys of a performance's note-ons, and the ticks of the note-ons and note-offs, over its MIDI tracks.
std::pair<std::set<uint32_t>, std::vector<uint32_t>> EffectNotes(const Performance& perf)
{
    std::set<uint32_t> keys;
    std::vector<uint32_t> ticks;
    for (const std::vector<TrackEvent>& track : perf.tracks)
    {
        for (const TrackEvent& e : track)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                keys.insert(e.key);
            }
            if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
            {
                ticks.push_back(e.tick);
            }
        }
    }
    std::ranges::sort(ticks);

    return {keys, ticks};
}

// The game's sound library starts an effect in its slot (JAISeMgr::checkPlayingSe): it writes the slot's ports 3, 6, 4
// and 0 once the slot has opened. The slot then plays the effect from the next tick. The performance ends when the slot
// is free again. The other slots play nothing.
void TestJAudio1Effects()
{
    const Performance first = PerformEffect(0, 0);
    Check(EffectNotes(first) == std::pair(std::set<uint32_t>{60}, std::vector<uint32_t>{1, 25}),
          "effect 0 plays key 60 from the tick after the start");
    Check(!first.loop && first.seconds < 2.0 && first.warnings.empty(), "the effect ends when its slot is free");

    Check(EffectNotes(PerformEffect(1, 1)) == std::pair(std::set<uint32_t>{64}, std::vector<uint32_t>{1, 49}),
          "effect 1 plays key 64 in slot 1");

    // A value for port 6 stands for the map's information. The library's values for ports 0 and 4 win.
    Check(EffectNotes(PerformEffect(0, 1, {{6, 7}})).first == std::set<uint32_t>{67}, "port 6 picks effect 1's key 67");
    Check(EffectNotes(PerformEffect(0, 1, {{0, 5}, {4, 2}})).first == std::set<uint32_t>{64},
          "the library writes ports 0 and 4");

    // The sound table's pitch moves every note as a bend does: twice the pitch is 12 semitones up.
    const Performance higher = PerformEffect(0, 0, {}, 2.0f);
    const auto bent = [&higher]
    {
        for (const std::vector<TrackEvent>& track : higher.tracks)
        {
            for (const TrackEvent& e : track)
            {
                if (e.kind == EventKind::kPitch && std::abs(e.semitones - 12.0) < 1e-4)
                {
                    return true;
                }
            }
        }

        return false;
    };
    Check(EffectNotes(higher) == EffectNotes(first) && bent(), "a pitch of 2 bends the effect up an octave");

    // An effect that loops loops. The slots that poll their ports don't count.
    const Performance looped = PerformEffect(4, 2);
    Check(EffectNotes(looped).first == std::set<uint32_t>{72} && looped.loop.has_value(), "effect 2 loops in slot 4");

    // Slot 8 would be child 8 of the first track's child 0. The sequence never opens it.
    const Performance none = PerformEffect(8, 0);
    Check(EffectNotes(none).first.empty() && none.warnings.size() == 1 &&
              none.warnings[0] == "the effects sequence opens no track for slot 8: the effect plays nothing",
          "an effect whose slot never opens plays nothing");
}

// The first track opens children 0 and 1, and child 1 again at tick 48. Each child plays a note. With a volume of 0
// for child 1 in SequenceSound::child_volumes, child 1 plays neither time: the game gave it a track volume of 0. A
// tempo ratio of 2 doubles the MIDI tempo and leaves the ticks alone.
void TestJAudio1RootChildren()
{
    constexpr uint32_t kOther = kChild + 0x20;
    Sequence1 s;
    s.OpenTrack(0, kChild).OpenTrack(1, kOther).Timebase(48).Tempo(120).Wait(48).OpenTrack(1, kOther).Wait(100);
    s.Finish().PadTo(kChild).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kOther).Bank(0).Program(0).NoteOn(64, 1, 100).Wait(24).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound every = game.Sound(0);
    every.jaudio1 = true;
    SequenceSound silent = every;
    silent.child_volumes = {{1, 0.0f}};
    SequenceSound faster = every;
    faster.tempo_ratio = 2.0f;

    const Performance all = Perform(every, game.banks, {}, kProgram);
    const Performance some = Perform(silent, game.banks, {}, kProgram);
    const Performance quick = Perform(faster, game.banks, {}, kProgram);

    using Ticks = std::vector<uint32_t>;
    Check(NoteTicks(all, "Track 0") == Ticks{0} && NoteTicks(all, "Track 1") == Ticks({0, 48}),
          "every child plays, and child 1 again at tick 48");
    Check(NoteTicks(some, "Track 0") == Ticks{0} && NoteTicks(some, "Track 1").empty(),
          "child 1 stays silent, the second time too");
    Check(!quick.tempo.empty() && quick.tempo[0].bpm == 240.0 && NoteTicks(quick, "Track 1") == Ticks({0, 48}),
          "a tempo ratio of 2 doubles the tempo");
}

// The first track clears port 9, waits two ticks and reads it. On a value other than 0 it opens child 0 at kSecond
// (key 72). Otherwise it opens child 0 at kFirst (key 60). A value that PerformOptions::ports sets before the first
// tick is cleared. One that the game's code writes once the sequence runs (SequenceSound::game_ports) is read.
void TestJAudio1GamePorts()
{
    constexpr uint32_t kTaken = kFirstSequence + 0x30;
    constexpr uint32_t kFirst = kFirstSequence + 0x40;
    constexpr uint32_t kSecond = kFirstSequence + 0x60;
    Sequence1 s;
    s.Timebase(48).Tempo(120).Register(0, 0, 0).WritePort(9, 0).Wait(2).ReadPort(9, 0).Jump(2, kTaken);
    s.OpenTrack(0, kFirst).Wait(100).Finish().PadTo(kTaken).OpenTrack(0, kSecond).Wait(100).Finish();
    s.PadTo(kFirst).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
    s.PadTo(kSecond).Bank(0).Program(0).NoteOn(72, 1, 100).Wait(24).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    SequenceSound written = sound;
    written.game_ports = {{9, 1}};
    PerformOptions before;
    before.ports = {{9, 1}};

    const auto keys = [](const Performance& perf)
    {
        std::vector<uint32_t> found;
        for (const std::vector<TrackEvent>& events : perf.tracks)
        {
            for (const TrackEvent& e : events)
            {
                if (e.kind == EventKind::kNoteOn)
                {
                    found.push_back(e.key);
                }
            }
        }

        return found;
    };

    using Keys = std::vector<uint32_t>;
    Check(keys(Perform(sound, game.banks, {}, kProgram)) == Keys{60}, "with port 9 at 0, child 0 plays key 60");
    Check(keys(Perform(sound, game.banks, before, kProgram)) == Keys{60},
          "the sequence clears a value set before its first tick");
    Check(keys(Perform(written, game.banks, {}, kProgram)) == Keys{72},
          "a value that the game writes once the sequence runs takes the jump");
}

// The game's writes by section (SequenceSound::status_by_section). The first track gives a value on port 10, waits two
// ticks and reads port 9. On a value other than 0 it opens child 0 at kSecond (key 72). Otherwise it opens child 0 at
// kFirst (key 60). A status for the first section, 0, reaches a read in the first tick.
void TestJAudio1StatusBySection()
{
    constexpr uint32_t kTaken = kFirstSequence + 0x30;
    constexpr uint32_t kFirst = kFirstSequence + 0x40;
    constexpr uint32_t kSecond = kFirstSequence + 0x60;
    const auto game_of = [&](bool read_at_once)
    {
        Sequence1 s;
        s.Timebase(48).Tempo(120);
        if (!read_at_once)
        {
            s.Register(0, 0, 5).WritePort(10, 0).Wait(2);
        }
        s.ReadPort(9, 0).Jump(2, kTaken);
        s.OpenTrack(0, kFirst).Wait(100).Finish().PadTo(kTaken).OpenTrack(0, kSecond).Wait(100).Finish();
        s.PadTo(kFirst).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish();
        s.PadTo(kSecond).Bank(0).Program(0).NoteOn(72, 1, 100).Wait(24).NoteOff(1).Finish();
        return s.Data();
    };
    const auto keys = [](const Performance& perf)
    {
        std::vector<uint32_t> found;
        for (const std::vector<TrackEvent>& events : perf.tracks)
        {
            for (const TrackEvent& e : events)
            {
                if (e.kind == EventKind::kNoteOn)
                {
                    found.push_back(e.key);
                }
            }
        }

        return found;
    };
    const auto keys_with = [&](bool read_at_once, std::map<uint16_t, uint16_t> statuses)
    {
        TestGame game(files::BankV0File(0, 60), {game_of(read_at_once)});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.status_by_section = std::move(statuses);
        return keys(Perform(sound, game.banks, {}, kProgram));
    };

    using Keys = std::vector<uint32_t>;
    Check(keys_with(false, {}) == Keys{60}, "without a status, child 0 plays key 60");
    Check(keys_with(false, {{5, 1}}) == Keys{72}, "the status for the section on port 10 takes the jump");
    Check(keys_with(false, {{6, 1}}) == Keys{60}, "the status for another section doesn't");
    Check(keys_with(true, {{0, 1}}) == Keys{72}, "the first tick reads the status for section 0");
}

// A note plays for three seconds. With a fade-in of 60 game frames, the game's 30 a second, CC11 starts at 0 and
// rises step by step to the level it holds without the fade, two seconds in: tick 192 at 96 ticks a second. A wait
// takes at most 255 ticks.
void TestJAudio1FadeIn()
{
    Sequence1 s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120).Wait(150).Wait(150).Finish();
    s.PadTo(kChild).Bank(0).Program(0).NoteOn(60, 1, 100).Wait(144).Wait(144).NoteOff(1).Finish();
    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    SequenceSound faded = sound;
    faded.fade_in = 60;

    const std::vector<int> plain = ExpressionOf(Perform(sound, game.banks, {}, kProgram), 60);
    const Performance perf = Perform(faded, game.banks, {}, kProgram);
    const std::vector<int> rising = ExpressionOf(perf, 60);
    uint32_t full_at = 0;
    for (const std::vector<TrackEvent>& track : perf.tracks)
    {
        for (const TrackEvent& e : track)
        {
            if (e.kind == EventKind::kExpression && !plain.empty() && e.value == plain.back() && full_at == 0)
            {
                full_at = e.tick;
            }
        }
    }

    Check(!plain.empty() && rising.size() > 20 && rising.front() == 0 && rising.back() == plain.back() &&
              std::ranges::is_sorted(rising),
          "CC11 rises from 0 to the level without the fade");
    Check(full_at >= 186 && full_at <= 198, "the fade ends two seconds in, at tick " + std::to_string(full_at));
}

// A JAudio1 game's folder: the start-up archive (test.aaf) and its sequence archive in Seqs. The sound table's first
// sequence, the effects sequence, plays file 7. The music plays files 3 and 9. Two more sounds play file 3 again at the
// same volume and file 9 at another volume. File 11 is played by no sound. The table has an effect in category 0 and
// two in category 2.
void TestJAudio1Folder()
{
    namespace fs = std::filesystem;
    const fs::path folder =
        fs::temp_directory_path() / ("sequence_export_jaudio1_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(folder / "Seqs");
    const std::vector<uint8_t> music = Sequence1(0).Tempo(120).Finish().Data();
    const std::vector<uint8_t> effects = Sequence1(0).Tempo(150).Finish().Data();
    xsflib::WriteFile(
        folder / "test.aaf",
        files::AafFile(
            {{16, 127, 7}, {16, 100, 3}, {16, 90, 9}, {16, 100, 3}, {16, 60, 9}, {0, 80}, {2, 100}, {2, 90, 0, 1.25f}},
            {"test.aw"}));
    xsflib::WriteFile(
        folder / "Seqs" / "test.arc",
        files::RarcFile(
            {{7, "effects.bms", effects}, {3, "field.bms", music}, {9, "boss.bms", music}, {11, "unused.bms", music}}));

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

    // With GameOptions::known_effects, the effects of a game that RulesOf doesn't list stay out.
    const Game unknown = Game::FromFolder(folder / "test.aaf", kProgram, {.known_effects = true});
    Check(names(unknown) == names(plain), "known_effects lists no effects of a game that the rules don't list");

    // With GameOptions::jaudio1_effects, the effects follow in the table's order. Each plays the effects sequence in
    // the first slot of its category, with four slots to a category. By the effects' format, an effect's number is its
    // index. The effects take their categories and indexes for names.
    const Game with_effects = Game::FromFolder(folder / "test.aaf", kProgram, {.jaudio1_effects = true});
    const std::vector<SequenceSound>& listed = with_effects.Sequences();
    Check(names(with_effects) ==
              std::vector<std::string>{"field", "boss", "SOUND_128_0_4", "unused", "se0-0000", "se2-0000", "se2-0001"},
          "the effects come after the music");
    if (listed.size() == 7)
    {
        TrackMask slot8{};
        slot8[0] = static_cast<uint16_t>(1u << 8);
        Check(listed[4].effect && listed[4].effect->slot == 0 && listed[4].effect->number == 0 &&
                  listed[4].tracks[0] == 1 && std::abs(listed[4].volume - 80.0f / 127.0f) < 1e-6f,
              "category 0's effect plays in slot 0 at the volume of its entry");
        Check(listed[6].effect && listed[6].effect->slot == 8 && listed[6].effect->number == 1 &&
                  listed[6].tracks == slot8 && listed[6].pitch == 1.25f && listed[4].pitch == 1.0f,
              "category 2's second effect plays number 1 in slot 8 at its pitch, and only that slot's track plays");
        Check(std::ranges::equal(listed[5].data, effects) && listed[5].start == 0 && !listed[0].effect,
              "the effects play the effects sequence");
    }

    // A name file beside the archive names the sounds by their IDs. It gives category 2's second effect the ID without
    // bit 11 (kAafToEnd). It gives the sequence 0x80000004 the bits of category 16 too, as Super Mario Sunshine's name
    // file gives its music.
    const std::vector<std::pair<std::string, uint32_t>> kNames = {
        {"JA_BGM_FIELD", 0x80000001}, {"JA_BGM_BOSS_2", 0x80010004}, {"JA_SE_STEP", 0x0800}, {"JA_SE_DOOR", 0x2001}};
    std::vector<uint8_t> asn(16 + (18 + kNames.size()) * 32);
    xsflib::StoreBe32(asn.data() + 12, static_cast<uint32_t>(kNames.size()));
    for (std::size_t i = 0; i < kNames.size(); i++)
    {
        const std::size_t record = 16 + (18 + i) * 32;
        std::ranges::copy(kNames[i].first, asn.begin() + static_cast<std::ptrdiff_t>(record));
        xsflib::StoreBe32(asn.data() + record + 28, kNames[i].second);
    }
    xsflib::WriteFile(folder / "test.asn", asn);

    const Game named = Game::FromFolder(folder / "test.aaf", kProgram);
    Check(names(named) == std::vector<std::string>{"JA_BGM_FIELD", "boss", "JA_BGM_BOSS_2", "unused"},
          "the name file's names come first");
    const Game named_effects = Game::FromFolder(folder / "test.aaf", kProgram, {.jaudio1_effects = true});
    Check(names(named_effects) == std::vector<std::string>{"JA_BGM_FIELD", "boss", "JA_BGM_BOSS_2", "unused",
                                                           "JA_SE_STEP", "se2-0000", "JA_SE_DOOR"},
          "the name file names effects by their IDs, with or without bit 11");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// The files of a disc in memory, in the order given.
std::shared_ptr<ntdlib::disc::GameFiles> DiscOf(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files)
{
    auto disc = std::make_shared<ntdlib::disc::GameFiles>();
    for (std::size_t i = 0; i < files.size(); i++)
    {
        disc->files.push_back({files[i].first, files[i].second.size(), static_cast<uint32_t>(i)});
    }
    disc->read_file = [files](const std::string& path, std::vector<uint8_t>& data)
    {
        const auto f = std::ranges::find(files, path, &std::pair<std::string, std::vector<uint8_t>>::first);
        if (f == files.end())
        {
            return false;
        }

        data = f->second;
        return true;
    };

    return disc;
}

// Donkey Kong Jungle Beat compresses its start-up archive with Yaz0 and keeps it beside its Seqs folder as a .szs
// file. Its wave archives are in Banks, not Waves. Super Mario Sunshine keeps its AAF in the RARC data/nintendo.szs,
// and its other sound files in AudioRes. Both open from a folder and from a disc's files. A .szs file away from a Seqs
// folder is no start-up archive.
void TestCompressedStartUpArchives()
{
    namespace fs = std::filesystem;
    const files::SoundTableBlocks sounds = files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}}}}}});
    const std::vector<uint8_t> baa = files::StartUpArchive(files::BankV0File(0, 60), {}, sounds);
    const std::vector<uint8_t> szs = files::LiteralYaz0(baa);
    const std::vector<uint8_t> arc = files::RarcFile({{7, "field.bms", files::OneNoteSequence()}});
    const std::vector<uint8_t> aw = files::AwFile();
    const auto names = [](const Game& game)
    {
        std::vector<std::string> list;
        for (const SequenceSound& sound : game.Sequences())
        {
            list.push_back(sound.name);
        }

        return list;
    };

    const fs::path folder =
        fs::temp_directory_path() / ("sequence_export_jaudio_szs_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(folder / "Seqs");
    fs::create_directories(folder / "Banks");
    xsflib::WriteFile(folder / "test.szs", szs);
    xsflib::WriteFile(folder / "Seqs" / "test.arc", arc);
    xsflib::WriteFile(folder / "Banks" / "test.aw", aw);

    Check(IsStartUpArchiveFile(folder / "test.szs") && !IsStartUpArchiveFile(folder / "Seqs" / "test.arc"),
          "a start-up archive compressed with Yaz0 is one");
    const Game from_folder = Game::FromFolder(folder / "test.szs", kProgram);
    Check(names(from_folder) == std::vector<std::string>{"BGM_FIELD"} && from_folder.Warnings().empty(),
          "the compressed archive opens from a folder: " + Joined(from_folder.Warnings()));
    Check(from_folder.ReadWaveArchive("test.aw") == aw, "without a Waves folder, the wave archives are in Banks");
    Check(from_folder.ArchiveSha1() == xsflib::Sha1::Hex(xsflib::Sha1::Of(baa)),
          "the archive's SHA-1 is that of the decoded archive");

    const std::optional<Game> jungle_beat = Game::FromDiscFiles(
        DiscOf({{"AudioRes/test.szs", szs}, {"AudioRes/Banks/test.aw", aw}, {"AudioRes/Seqs/test.arc", arc}}),
        kProgram);
    Check(jungle_beat && jungle_beat->DiscPath() == "AudioRes/test.szs" &&
              names(*jungle_beat) == std::vector<std::string>{"BGM_FIELD"} &&
              jungle_beat->ReadWaveArchive("test.aw") == aw,
          "the compressed archive beside the Seqs folder opens from a disc's files");

    // The wave archives of the generation's folder come first.
    const std::vector<uint8_t> other_aw(16, 0x11);
    const std::optional<Game> both = Game::FromDiscFiles(DiscOf({{"AudioRes/test.szs", szs},
                                                                 {"AudioRes/Banks/test.aw", other_aw},
                                                                 {"AudioRes/Waves/test.aw", aw},
                                                                 {"AudioRes/Seqs/test.arc", arc}}),
                                                         kProgram);
    Check(both && both->ReadWaveArchive("test.aw") == aw, "a JAudio2 game's Waves folder comes before its Banks");

    const std::optional<Game> away =
        Game::FromDiscFiles(DiscOf({{"Other/test.szs", szs}, {"AudioRes/Seqs/test.arc", arc}}), kProgram);
    Check(!away, "a .szs file away from a Seqs folder isn't read");

    // Sunshine's container. The AAF's first sequence is the effects sequence.
    const std::vector<uint8_t> aaf = files::AafFile({{16, 127, 7}, {16, 100, 3}}, {"test.aw"});
    const std::vector<uint8_t> aaf_arc =
        files::RarcFile({{7, "effects.bms", files::OneNoteSequence()}, {3, "field.bms", files::OneNoteSequence()}});
    const std::vector<uint8_t> nintendo =
        files::LiteralYaz0(files::RarcFile({{0, "nintendo.bti", {1, 2, 3}}, {1, "msound.aaf", aaf}}));
    const std::optional<Game> sunshine = Game::FromDiscFiles(
        DiscOf(
            {{"AudioRes/Banks/test.aw", aw}, {"AudioRes/Seqs/sequence.arc", aaf_arc}, {"data/nintendo.szs", nintendo}}),
        kProgram);
    Check(sunshine && sunshine->DiscPath() == "data/nintendo.szs" &&
              names(*sunshine) == std::vector<std::string>{"field"} && sunshine->ReadWaveArchive("test.aw") == aw &&
              sunshine->ArchiveSha1() == xsflib::Sha1::Hex(xsflib::Sha1::Of(aaf)),
          "the AAF in data/nintendo.szs opens with the sound files in AudioRes");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// Super Mario Galaxy and Super Mario Galaxy 2 compress each file of their sequence archive with Yaz0. Each sound plays
// its file decoded. A file that isn't compressed stays as it is.
void TestCompressedSequences()
{
    const files::SoundTableBlocks sounds =
        files::SoundTable({{"BGM", {{"FIELD", {{0x11, 127, 7, "BGM_FIELD"}, {0x11, 127, 3, "BGM_TOWN"}}}}}});
    const std::vector<uint8_t> szs = files::LiteralYaz0(files::StartUpArchive(files::BankV0File(0, 60), {}, sounds));
    const std::vector<uint8_t> field = files::OneNoteSequence();
    std::vector<uint8_t> town = files::OneNoteSequence();
    town.push_back(0);
    const std::vector<uint8_t> arc =
        files::RarcFile({{7, "field.szs", files::LiteralYaz0(field)}, {3, "town.bms", town}});
    const std::optional<Game> galaxy = Game::FromDiscFiles(DiscOf({{"AudioRes/SMR.szs", szs},
                                                                   {"AudioRes/Waves/test.aw", files::AwFile()},
                                                                   {"AudioRes/Seqs/JaiSeq.arc", arc}}),
                                                           kProgram);
    Check(galaxy && galaxy->Sequences().size() == 2 && galaxy->Warnings().empty(),
          "the game with compressed sequences opens");
    if (galaxy && galaxy->Sequences().size() == 2)
    {
        Check(std::ranges::equal(galaxy->Sequences()[0].data, field), "a compressed sequence plays decoded");
        Check(std::ranges::equal(galaxy->Sequences()[1].data, town), "a sequence that isn't compressed stays");
    }
}

// Pikmin's commands (SequenceSound::jac). The first track names itself with 0xd0 and 0xe5, which take four bytes and
// none. 0xf3 and 0xf4 take none. Track 0 loads registers 16 and 17 with the offset of two bytes, 50 and 72. It loads
// the second byte into register 4 through address register 0x28, which holds registers 16 and 17. It plays the byte
// as a key. Without Pikmin's library, register 0x28 holds 0, and the track plays the collection's second byte.
void TestJacCommands()
{
    constexpr uint32_t kKeys = kChild + 0x40;
    Sequence1 s;
    s.OpenTrack(0, kChild).Timebase(48).Tempo(120);
    s.Byte(0xd0).Byte(0).Byte(1).Byte(0).Byte(9).Byte(0xe5).Byte(0xf3).Byte(0xf4).Wait(200).Finish().PadTo(kChild);
    s.Bank(0).Program(0);
    s.Byte(0xac).Byte(16).Byte(0).Byte(0).Byte(0xac).Byte(17).Byte(kKeys >> 8).Byte(kKeys & 0xff);
    s.Byte(0xaa).Byte(0x04).Byte(4).Byte(0x28).Byte(1);
    s.Byte(4).Byte(0x81).Byte(100).Wait(24).NoteOff(1).Finish().PadTo(kKeys);
    s.Byte(50).Byte(72);

    TestGame game(files::BankV0File(0, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    sound.jac = true;
    const Performance jac = Perform(sound, game.banks, {}, kProgram);
    Check(EffectNotes(jac) == std::pair(std::set<uint32_t>{72}, std::vector<uint32_t>{0, 24}),
          "Pikmin's track plays the byte that register 0x28 points at");

    sound.jac = false;
    Check(EffectNotes(Perform(sound, game.banks, {}, kProgram)).first != std::set<uint32_t>{72},
          "The Wind Waker's library reads the commands otherwise");
}

// A piece's first track starts with SequenceSound::bank as its bank and with SequenceSound::ports. The track plays the
// key in its port 0 on program 0. The test bank is bank 5. PerformOptions::ports take the place of the sound's.
void TestFirstTrackBankAndPorts()
{
    Sequence1 s;
    s.Timebase(48).Tempo(120).Program(0).ReadPort(0, 4).Byte(4).Byte(0x81).Byte(100).Wait(24).NoteOff(1).Finish();
    TestGame game(files::BankV0File(5, 60), {s.Data()});
    SequenceSound sound = game.Sound(0);
    sound.jaudio1 = true;
    sound.ports = {{0, 67}};

    const Performance bank0 = Perform(sound, game.banks, {}, kProgram);
    Check(EffectNotes(bank0).first.empty(), "bank 0 plays nothing");

    sound.bank = 5;
    Check(EffectNotes(Perform(sound, game.banks, {}, kProgram)).first == std::set<uint32_t>{67},
          "the first track plays bank 5 and the key in its port 0");

    PerformOptions options;
    options.ports = {{0, 72}};
    Check(EffectNotes(Perform(sound, game.banks, options, kProgram)).first == std::set<uint32_t>{72},
          "the options' port takes the place of the sound's");
}

// Pikmin's library writes an effect's number to port 0 of the effects sequence's track for the effect's kind
// (EffectRequest::kPort). The first track opens child 2 and waits. Child 2 sets interrupt 3 for a write to port 0 and
// waits. The interrupt's routine reads the number and opens track 0 below it: at effect 0's code for 0, and at effect
// 1's for any other number. The effect plays from the tick after the request. The performance ends when the track
// below has closed.
void TestJacEffects()
{
    constexpr uint32_t kSlot = kChild;
    constexpr uint32_t kRoutine = kSlot + 0x20;
    constexpr uint32_t kOther = kRoutine + 0x20;
    constexpr uint32_t kEffect0 = kOther + 0x10;
    constexpr uint32_t kEffect1 = kEffect0 + 0x20;
    Sequence1 s;
    s.Timebase(48).OpenTrack(2, kSlot).Tempo(120);
    const uint32_t first = s.Here();
    s.Wait(255).Jump(0, first).PadTo(kSlot);
    s.Byte(0xdf).Byte(3).Byte(kRoutine >> 16).Byte((kRoutine >> 8) & 0xff).Byte(kRoutine & 0xff);
    const uint32_t wait = s.Here();
    s.Wait(255).Jump(0, wait).PadTo(kRoutine);
    s.ReadPort(0, 0).Register(3, 0, 0).Jump(2, kOther).OpenTrack(0, kEffect0).Byte(0xe3).PadTo(kOther);
    s.OpenTrack(0, kEffect1).Byte(0xe3).PadTo(kEffect0);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish().PadTo(kEffect1);
    s.Bank(0).Program(0).NoteOn(64, 1, 100).Wait(48).NoteOff(1).Finish();

    const auto play = [&s](uint8_t slot, uint16_t number)
    {
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.jac = true;
        sound.effect = EffectStart{.slot = slot, .number = number, .request = EffectRequest::kPort, .port = 0};

        return Perform(sound, game.banks, {}, kProgram);
    };

    const Performance zero = play(2, 0);
    Check(EffectNotes(zero) == std::pair(std::set<uint32_t>{60}, std::vector<uint32_t>{1, 25}),
          "effect 0 plays key 60 from the tick after the request");
    Check(!zero.loop && zero.seconds < 2.0 && zero.warnings.empty(), "the effect ends when its track closes");
    Check(EffectNotes(play(2, 5)) == std::pair(std::set<uint32_t>{64}, std::vector<uint32_t>{1, 49}),
          "effect 5 plays key 64");

    const Performance none = play(3, 0);
    Check(EffectNotes(none).first.empty() && none.warnings.size() == 1, "a track that never opens plays nothing");
}

// An effect of EffectRequest::kPort with SequenceSound::ports. The library writes them to the effect's track before the
// number, and PerformOptions::ports take their place. The routine of TestJacEffects reads port 1 here. It opens effect
// 0's code for 0 and effect 1's for any other value.
void TestJacEffectPorts()
{
    constexpr uint32_t kSlot = kChild;
    constexpr uint32_t kRoutine = kSlot + 0x20;
    constexpr uint32_t kOther = kRoutine + 0x20;
    constexpr uint32_t kEffect0 = kOther + 0x10;
    constexpr uint32_t kEffect1 = kEffect0 + 0x20;
    Sequence1 s;
    s.Timebase(48).OpenTrack(2, kSlot).Tempo(120);
    const uint32_t first = s.Here();
    s.Wait(255).Jump(0, first).PadTo(kSlot);
    s.Byte(0xdf).Byte(3).Byte(kRoutine >> 16).Byte((kRoutine >> 8) & 0xff).Byte(kRoutine & 0xff);
    const uint32_t wait = s.Here();
    s.Wait(255).Jump(0, wait).PadTo(kRoutine);
    s.ReadPort(1, 0).Register(3, 0, 0).Jump(2, kOther).OpenTrack(0, kEffect0).Byte(0xe3).PadTo(kOther);
    s.OpenTrack(0, kEffect1).Byte(0xe3).PadTo(kEffect0);
    s.Bank(0).Program(0).NoteOn(60, 1, 100).Wait(24).NoteOff(1).Finish().PadTo(kEffect1);
    s.Bank(0).Program(0).NoteOn(64, 1, 100).Wait(48).NoteOff(1).Finish();

    const auto play = [&s](std::map<uint8_t, uint16_t> ports, const PerformOptions& options)
    {
        TestGame game(files::BankV0File(0, 60), {s.Data()});
        SequenceSound sound = game.Sound(0);
        sound.jaudio1 = true;
        sound.jac = true;
        sound.effect = EffectStart{.slot = 2, .number = 1, .request = EffectRequest::kPort, .port = 0};
        sound.ports = std::move(ports);

        return EffectNotes(Perform(sound, game.banks, options, kProgram)).first;
    };

    Check(play({}, {}) == std::set<uint32_t>{60}, "without a value for port 1, effect 0 plays");
    Check(play({{1, 5}}, {}) == std::set<uint32_t>{64}, "the sound's value for port 1 reaches the track first");
    PerformOptions options;
    options.ports = {{1, 0}};
    Check(play({{1, 5}}, options) == std::set<uint32_t>{60}, "the options' value takes the place of the sound's");
}

// Pikmin keeps its banks and wave systems in a bank file (.bx) in a Banks folder, its sequences in a file in Seqs, and
// the sequence index in main.dol. The index names the file of sequences. Its empty entries are left out. Each sequence
// plays Pikmin's library (SequenceSound::jac). An index that names another file opens nothing.
void TestPikminDiscFiles()
{
    const std::vector<uint8_t> effects = files::OneNoteSequence();
    const std::vector<uint8_t> tune = files::OneNoteSequence();
    std::vector<uint8_t> sequences = effects;
    sequences.insert(sequences.end(), tune.begin(), tune.end());
    const auto size = static_cast<uint32_t>(effects.size());
    const std::vector<uint8_t> bx = files::BxFile({files::WaveSystemFile(0, 2, "test.aw")}, {files::BankV0File(0, 60)});
    const std::vector<uint8_t> aw = files::AwFile();
    const auto disc_with = [&](const std::string& named)
    {
        std::vector<uint8_t> dol(0x100, 0);
        const std::vector<uint8_t> index =
            files::BarcFile({{"pikise.jam", 0, size}, {"dummy", 0, 0}, {"tune.jam", size, size}}, named);
        dol.insert(dol.end(), index.begin(), index.end());
        std::shared_ptr<ntdlib::disc::GameFiles> disc = DiscOf({{"dataDir/SndData/Banks/test.bx", bx},
                                                                {"dataDir/SndData/Banks/test.aw", aw},
                                                                {"dataDir/SndData/Seqs/test.arc", sequences}});
        disc->dol = std::move(dol);

        return disc;
    };

    const std::optional<Game> game = Game::FromDiscFiles(disc_with("test.arc"), kProgram);
    std::vector<std::string> names;
    bool jac = true;
    for (const SequenceSound& sound : game ? game->Sequences() : std::vector<SequenceSound>{})
    {
        names.push_back(sound.name);
        jac = jac && sound.jac && sound.jaudio1;
    }
    Check(game && game->DiscPath() == "dataDir/SndData/Banks/test.bx" &&
              names == std::vector<std::string>{"pikise", "tune"} && jac && game->ReadWaveArchive("test.aw") == aw &&
              game->Archive().FindBank(0) != nullptr && game->Warnings().empty(),
          "Pikmin's bank file, sequence index and file of sequences open from a disc's files");
    Check(!Game::FromDiscFiles(disc_with("other.arc"), kProgram), "an index that names another file opens nothing");

    // The USA release's rules: a piece plays the bank of its number, and demobgm.jam plays eight parts.
    const GameRules rules = RulesOf("9ae6b40b8e9eacfda6ed0b5d59ff1e680373a018");
    const auto parts = rules.parts.find("demobgm.jam");
    Check(rules.bank_of_number && rules.effects_sequence == "pikise.jam" && parts != rules.parts.end() &&
              parts->second.size() == 8 && parts->second[0].suffix == "-1" &&
              parts->second[0].ports == std::map<uint8_t, uint16_t>{{0, 1}} && rules.jac_effects &&
              rules.jac_effects->events_sequence == "sysevent.jam" && rules.jac_effects->port_effects.size() == 1 &&
              rules.jac_effects->port_effects[0].track == 7 &&
              rules.jac_effects->port_effects[0].ports == std::map<uint8_t, uint16_t>{{2, 0}, {3, 127}},
          "Pikmin's rules");

    // New Play Control! Pikmin on the Wii: the same rules, with the effects' tables of the Wii's main.dol.
    const GameRules wii = RulesOf("9f42f65be7f7159faa14b003524476d624e1227f");
    const auto wii_parts = wii.parts.find("demobgm.jam");
    Check(wii.bank_of_number && wii.effects_sequence == rules.effects_sequence && wii_parts != wii.parts.end() &&
              wii_parts->second.size() == 8 && wii_parts->second[7].ports == parts->second[7].ports &&
              wii.child_volumes == rules.child_volumes && !wii.child_volumes.empty() && wii.jac_effects &&
              wii.jac_effects->dol_sha1 == "551306334183a633d0fde434a930b98809433b70" &&
              wii.jac_effects->catalogue == 0x80341868 && wii.jac_effects->count == 332 &&
              wii.jac_effects->event_offsets == 0x80315688 &&
              wii.jac_effects->left_out == rules.jac_effects->left_out &&
              wii.jac_effects->other_system == rules.jac_effects->other_system &&
              wii.jac_effects->port_effects.size() == 1 && rules.jac_effects->count == 331,
          "New Play Control! Pikmin's rules: the GameCube's, with the Wii's tables of effects");
}

// A SoundFont whose loudest zone plays above full level takes one attenuation in every zone, and the conversion records
// it from 0.5 cB, the least that the report shows as 0.1 dB. Zones at full level take none.
void TestCutAboveFullLevel()
{
    Conversion loud;
    Conversion full;
    Conversion least;
    Conversion hair;

    const double quieter = ntdlib::sequence_export::CutAboveFullLevel(loud, 2.0);
    const double none = ntdlib::sequence_export::CutAboveFullLevel(full, 1.0);
    const double least_cut = ntdlib::sequence_export::CutAboveFullLevel(least, std::pow(10.0, 0.6 / 200.0));
    const double hair_cut = ntdlib::sequence_export::CutAboveFullLevel(hair, std::pow(10.0, 0.4 / 200.0));

    Check(std::fabs(quieter - 200.0 * std::log10(2.0)) < 1e-9, "twice full level takes 6 dB");
    Check(loud.performance.approximations.size() == 1 &&
              loud.performance.approximations.begin()->first ==
                  "instrument volume above full level (SoundFont attenuated by 6.0 dB; SFZ keeps the level)",
          "and the conversion records it");
    Check(none == 0.0 && full.performance.approximations.empty(), "full level takes none");
    Check(std::fabs(least_cut - 0.6) < 1e-9 && least.performance.approximations.size() == 1 &&
              least.performance.approximations.begin()->first ==
                  "instrument volume above full level (SoundFont attenuated by 0.1 dB; SFZ keeps the level)",
          "a cut of 0.6 cB is recorded as 0.1 dB");
    Check(std::fabs(hair_cut - 0.4) < 1e-9 && hair.performance.approximations.empty(),
          "a cut of 0.4 cB, which the report would show as 0.0 dB, applies but isn't recorded");
}

// Runs the converters that RunArchiveConverterWithGames builds from `archive_converter` and `game_options` on `args`.
// The tests make up their arguments and pass them here. On Windows, the entry point reads the process's command line in
// place of argv (xsflib's Arguments).
int RunWithGames(const std::vector<std::string>& args,
                 const ntdlib::sequence_export::ArchiveConverter<ntdlib::sar::SoundArchive>& archive_converter,
                 const GameOptions& game_options = {})
{
    namespace frame = ntdlib::sequence_export;
    const auto archives = frame::MakeArchiveConverter(archive_converter);
    const GameConverter games = MakeGameConverter(archives.program, archives.version_line,
                                                  ArchiveProgramOptions(archives.option), archives.usage, game_options);

    return frame::Run(args, archives, ArchiveProgramGames(args, games));
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

    // The converters of the two entry points run on made-up arguments. RunArchiveConverter runs the converter of
    // archives. RunArchiveConverterWithGames adds the converter of games. An entry point runs its converters on
    // Arguments(argc, argv).
    const std::vector<std::string> args = {"xyz2midi", "--quiet", frame::Utf8(folder / "test.baa")};
    std::string program = "xyz2midi";
    char* argv[] = {program.data(), nullptr};

    const int refused = frame::Run(args, frame::MakeArchiveConverter(archives));
    const int converted = RunWithGames(args, archives);
    const int entry_point = RunArchiveConverterWithGames(1, argv, archives);
    const int on_arguments = RunWithGames(frame::Arguments(1, argv), archives);

    Check(refused == 1 && converted == 0 && fs::exists(folder / "test_xyz2midi" / "BGM_FIELD.mid"),
          "a converter of archives converts a JAudio game where it takes them");
    Check(entry_point == on_arguments, "the entry point runs the same converters on the command line");

    std::error_code ignored;
    fs::remove_all(folder, ignored);
}

// The converters of RunArchiveConverterWithGames (RunWithGames) open each JAudio game with the game options that the
// caller gives. Without them, a JAudio1 game's music converts alone. With GameOptions::jaudio1_effects, the game's
// effects convert too. The game here has its effects sequence (file 7), one piece of music (file 3) and one effect.
void TestArchiveConverterGameOptions()
{
    namespace fs = std::filesystem;
    namespace frame = ntdlib::sequence_export;
    const fs::path folder =
        fs::temp_directory_path() / ("sequence_export_jaudio_options_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(folder / "Seqs");
    const std::vector<uint8_t> sequence = Sequence1(0).Tempo(120).Finish().Data();
    xsflib::WriteFile(folder / "test.aaf", files::AafFile({{16, 127, 7}, {16, 100, 3}, {0, 80}}, {"test.aw"}));
    xsflib::WriteFile(folder / "Seqs" / "test.arc",
                      files::RarcFile({{7, "effects.bms", sequence}, {3, "field.bms", sequence}}));

    // Two command lines of a converter of archives, with separate output folders.
    frame::ArchiveConverter<ntdlib::sar::SoundArchive> archives;
    archives.program = "xyz2midi";
    const std::string path = frame::Utf8(folder / "test.aaf");
    const std::vector<std::string> plain_args = {"xyz2midi", "--quiet", "--output", frame::Utf8(folder / "plain"),
                                                 path};
    const std::vector<std::string> effects_args = {"xyz2midi", "--quiet", "--output", frame::Utf8(folder / "effects"),
                                                   path};

    const int plain = RunWithGames(plain_args, archives);
    const int effects = RunWithGames(effects_args, archives, {.jaudio1_effects = true});

    Check(plain == 0 && fs::exists(folder / "plain" / "field.mid") && !fs::exists(folder / "plain" / "se0-0000.mid"),
          "without game options, the music converts alone");
    Check(
        effects == 0 && fs::exists(folder / "effects" / "field.mid") && fs::exists(folder / "effects" / "se0-0000.mid"),
        "with jaudio1_effects, the effect converts too");

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
    TestHoldsSilent();
    TestEnvelopeFit();
    TestReleaseAfterDecayToSilence();
    TestPerformance();
    TestOscillatorScaleArguments();
    TestOscillatorScale();
    TestNoteEndsWhenItsVolumeHoldsAtZero();
    TestSweepAndTie();
    TestTieBelowHearing();
    TestGateTickRate();
    TestLoop();
    TestJumpToTheNextCommand();
    TestPortsAndRegisters();
    TestPortOfFinishedChild();
    TestPortsRead();
    TestDrumAndVolume();
    TestTailsKeepTheirSettings();
    TestHeldNoteKeepsItsPart();
    TestDefaultKeepsEveryNote();
    TestMutedTracks();
    TestCompanionSounds();
    TestSequenceCallback();
    TestTempoRatio();
    TestChildVolumes();
    TestConversion();
    TestTrackFilterLayers();
    TestZoneFilters();
    TestCutAboveFullLevel();
    TestPitchLimit();
    TestOscillatorNotes();
    TestOscillatorTie();
    TestWaveArchiveThatCannotBeRead();
    TestFolderWithAppleDoubleFiles();
    TestGameRules();
    TestEchoLengthensTheEnd();
    TestLateChangeAfterTheSoundIsLeftOut();
    TestJAudio1Notes();
    TestJAudio1SweepAndTie();
    TestJAudio1TrackOscillator();
    TestJAudio1Registers();
    TestJAudio1Random();
    TestMuteChild15();
    TestJacChildVolumes();
    TestSyncAnswers();
    TestConductor();
    TestLaterJAudio1Interrupt();
    TestLaterJAudio1TickRate();
    TestJAudio1Effects();
    TestJAudio1AutoMixer();
    TestJAudio1VolumeMode();
    TestJAudio1RootChildren();
    TestJAudio1GamePorts();
    TestJAudio1StatusBySection();
    TestJAudio1FadeIn();
    TestMixLevel();
    TestJAudio1PanRules();
    TestJAudio1Folder();
    TestCompressedStartUpArchives();
    TestCompressedSequences();
    TestVolumeTable();
    TestJacCommands();
    TestFirstTrackBankAndPorts();
    TestJacEffects();
    TestJacEffectPorts();
    TestPikminDiscFiles();
    TestGameConverter();
    TestArchiveConverterGameOptions();

    return xsflib::testing::Summary("sequence_export_jaudio");
}
