// SPDX-License-Identifier: MIT

// Tests of sequence_export's performance types: the envelope overrides, the LFO units of each engine, the Performance
// that seqlib's MIDI writer takes, how FitParts fits a Performance's parts into MIDI's channels, and how
// RepeatsAsTheGame takes a part's pitch before its first pitch event, how it compares pitches within a tolerance, and
// how a run on fewer parts checks the loop of the run before it (MainLoopPasses, RunUntilSettled).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance_writer.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"

using namespace ntdlib::sequence_export;

namespace
{

// The 3DS's envelope time scale: the DSP's frame over the 5 ms envelope step.
constexpr double kCtrFrameMs = 160.0 * 8192.0 * 1000.0 / 268111856.0;
constexpr double kCtrEnvTimeScale = kCtrFrameMs / 5;

Settings SettingsFor(Engine engine)
{
    Settings settings;
    settings.engine = engine;
    if (engine == Engine::kCtr)
    {
        settings.env_time_scale = kCtrEnvTimeScale;
    }

    return settings;
}

void TestOverridesReplaceOnlyTheSetValues()
{
    PresetKey key;
    key.attack = 10;
    key.release = 127;
    const EnvelopeValues kRegion{1, 2, 3, 4, 5};

    const EnvelopeValues env = OverrideEnvelope(kRegion, key);

    XSFLIB_CHECK(env.attack == 10);
    XSFLIB_CHECK(env.decay == 2);
    XSFLIB_CHECK(env.sustain == 3);
    XSFLIB_CHECK(env.hold == 4);
    XSFLIB_CHECK(env.release == 127);
}

// The Wii U and the Switch keep Hz and milliseconds.
void TestLfoOnTheWiiUAndTheSwitchIsInHzAndMilliseconds()
{
    PresetKey key;
    key.lfo_speed = 7.5f;
    key.lfo_delay = 1234;

    for (const Engine engine : {Engine::kCafe, Engine::kSwitch})
    {
        const Settings settings = SettingsFor(engine);

        XSFLIB_CHECK(LfoHz(key, settings) == 7.5);
        XSFLIB_CHECK(LfoDelayMs(key, settings) == 1234.0);
    }
}

// The 3DS and the Wii keep the bytecode's units. The expected values convert them as each console does.
void TestLfoOnThe3dsAndTheWiiIsInTheBytecodesUnits()
{
    for (const uint8_t speed : {uint8_t{1}, uint8_t{16}, uint8_t{37}, uint8_t{255}})
    {
        for (const uint16_t delay : {uint16_t{0}, uint16_t{7}, uint16_t{65535}})
        {
            PresetKey key;
            key.lfo_speed = speed;
            key.lfo_delay = delay;

            const double ctr_hz = LfoHz(key, SettingsFor(Engine::kCtr));
            const double ctr_delay = LfoDelayMs(key, SettingsFor(Engine::kCtr));
            const double wii_hz = LfoHz(key, SettingsFor(Engine::kRevolution));
            const double wii_delay = LfoDelayMs(key, SettingsFor(Engine::kRevolution));

            const uint8_t ctr_speed = speed;
            const uint16_t ctr_delay_units = delay;
            constexpr int kEnvStepMs = 5;
            XSFLIB_CHECK(ctr_hz == ctr_speed * 100.0 / 256.0 / kCtrEnvTimeScale);
            XSFLIB_CHECK(ctr_delay == ctr_delay_units * kEnvStepMs * kCtrEnvTimeScale);
            XSFLIB_CHECK(wii_hz == speed * 100.0 / 256.0);
            XSFLIB_CHECK(wii_delay == delay * 5.0);
        }
    }
}

// seqlib's MIDI writer takes sequence_export's Performance as it is.
void TestPerformanceWritesAsMidi()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    perf.tracks[0].push_back({0, EventKind::kNoteOn, 60, 100, 0, 0.0f, 1});
    perf.tracks[0].push_back({48, kFilterEvent, 0, 64});
    perf.tracks[0].push_back({96, EventKind::kNoteOff, 60, 0, 0, 0.0f, 1});
    perf.end_tick = 96;
    perf.presets.push_back(PresetKey{});

    seqlib::midi::MidiReport report;
    const std::vector<uint8_t> midi = seqlib::midi::WriteMidi(perf, "Test", report);

    // The filter event becomes controller 102 on the track's channel.
    const std::vector<uint8_t> kController = {0xb0, kFilterController, 64};
    bool found = false;
    for (std::size_t i = 0; i + kController.size() <= midi.size() && !found; i++)
    {
        found = std::equal(kController.begin(), kController.end(), midi.begin() + static_cast<std::ptrdiff_t>(i));
    }

    XSFLIB_CHECK(found);
    XSFLIB_CHECK(report.channels == 1);
}

// A performance in which the first `tracks` tracks play a note each. Track 0 also plays two notes of key 60 at once on
// its second part.
Performance PartsWithALayer(int tracks)
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    uint32_t note = 1;
    for (int t = 0; t < tracks; t++)
    {
        perf.tracks[t].push_back({0, EventKind::kNoteOn, 48, 100, 0, 0.0f, note});
        perf.tracks[t].push_back({96, EventKind::kNoteOff, 48, 0, 0, 0.0f, note});
        note++;
    }
    perf.tracks[0].push_back({0, EventKind::kNoteOn, 60, 100, 0, 0.0f, note, 1});
    perf.tracks[0].push_back({48, EventKind::kNoteOn, 60, 100, 0, 0.0f, note + 1, 1});
    perf.tracks[0].push_back({96, EventKind::kNoteOff, 60, 0, 0, 0.0f, note, 1});
    perf.tracks[0].push_back({96, EventKind::kNoteOff, 60, 0, 0, 0.0f, note + 1, 1});
    perf.end_tick = 96;
    perf.presets.push_back(PresetKey{});

    return perf;
}

// The parts that PartsWithALayer's tracks use.
std::array<uint8_t, 16> PartsUsed(int tracks)
{
    std::array<uint8_t, 16> used{};
    std::fill_n(used.begin(), tracks, 1);
    used[0] = 2;

    return used;
}

// With one channel spare, FitParts gives it to track 0's second part by default. With SpareChannels::kForLayers, the
// second note of key 60 takes it instead, and track 0 keeps one part.
void TestFitPartsKeepsAChannelForALayer()
{
    const Performance perf = PartsWithALayer(15);

    const std::array<uint8_t, 16> for_parts = FitParts(perf, PartsUsed(15));
    const std::array<uint8_t, 16> for_layers = FitParts(perf, PartsUsed(15), SpareChannels::kForLayers);

    XSFLIB_CHECK(for_parts[0] == 2);
    XSFLIB_CHECK(for_layers[0] == 1);
    XSFLIB_CHECK(std::count(for_layers.begin(), for_layers.end(), 1) == 16);
}

// With channels to spare for both, track 0 keeps both of its parts.
void TestFitPartsGivesPartsTheChannelsThatLayersLeave()
{
    const Performance perf = PartsWithALayer(14);

    const std::array<uint8_t, 16> limits = FitParts(perf, PartsUsed(14), SpareChannels::kForLayers);

    XSFLIB_CHECK(limits[0] == 2);
}

} // namespace

// A track whose notes go on to its second part after the first pass, with no pitch event there. The recorder sends a
// part no pitch of 0 before it has sent the part another pitch. Every MIDI channel starts with its pitch bend at the
// centre. A MIDI player that loops on the first pass then plays each repeat at the pitch of the game's next pass. With
// a pitch of 1 semitone on the second part, it doesn't.
void TestRepeatOnAPartWithoutAPitch()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    for (uint32_t pass = 0; pass < 3; pass++)
    {
        const auto part = static_cast<uint8_t>(pass == 0 ? 0 : 1);
        perf.tracks[0].push_back({100 * pass, EventKind::kNoteOn, 60, 100, 0, 0.0f, pass + 1, part});
        perf.tracks[0].push_back({100 * pass + 50, EventKind::kNoteOff, 60, 0, 0, 0.0f, pass + 1, part});
    }
    perf.end_tick = 300;
    const std::vector<PartSwitch> switches = {{100, 0, 1}};

    XSFLIB_CHECK(RepeatsAsTheGame(perf, switches, 0, 100, LoopRules{}));

    perf.tracks[0].insert(perf.tracks[0].begin() + 2, {100, EventKind::kPitch, 0, 0, 0, 1.0f, 0, 1});
    XSFLIB_CHECK(!RepeatsAsTheGame(perf, switches, 0, 100, LoopRules{}));
}

// The same track with a pitch of 0.05 semitones on the second part. The repeat plays 5 cents off the game's next pass:
// the default asks for the same pitch, and LoopRules::same_pitch decides where the two differ. The callback gets the
// repeat's pitch, 0, and the game's, 0.05.
void TestRepeatWithADifferentPitch()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    for (uint32_t pass = 0; pass < 3; pass++)
    {
        const auto part = static_cast<uint8_t>(pass == 0 ? 0 : 1);
        perf.tracks[0].push_back({100 * pass, EventKind::kNoteOn, 60, 100, 0, 0.0f, pass + 1, part});
        perf.tracks[0].push_back({100 * pass + 50, EventKind::kNoteOff, 60, 0, 0, 0.0f, pass + 1, part});
    }
    perf.tracks[0].insert(perf.tracks[0].begin() + 2, {100, EventKind::kPitch, 0, 0, 0, 0.05f, 0, 1});
    perf.end_tick = 300;
    const std::vector<PartSwitch> switches = {{100, 0, 1}};
    float seen_a = -1.0f;
    float seen_b = -1.0f;
    const auto same = [&](int, const TrackEvent& a, const TrackEvent& b)
    {
        seen_a = a.semitones;
        seen_b = b.semitones;
        return true;
    };
    const auto other = [](int, const TrackEvent&, const TrackEvent&)
    {
        return false;
    };

    XSFLIB_CHECK(!RepeatsAsTheGame(perf, switches, 0, 100, LoopRules{}));
    XSFLIB_CHECK(RepeatsAsTheGame(perf, switches, 0, 100, LoopRules{.same_pitch = same}));
    XSFLIB_CHECK(seen_a == 0.0f && seen_b == 0.05f);
    XSFLIB_CHECK(!RepeatsAsTheGame(perf, switches, 0, 100, LoopRules{.same_pitch = other}));
}

// A track whose part 0 bends at tick 0 and whose notes play on part 1. Part 1 has no pitch event in the pass, and the
// game's next pass sends it 0.05 semitones. The repeat starts at part 1's pitch without an event. LoopRules::same_pitch
// gets that pitch with part 1 as its part, not part 0, whose bend it doesn't play.
void TestRepeatPitchWithoutAnEventNamesItsPart()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    perf.tracks[0].push_back({0, EventKind::kPitch, 0, 0, 0, 1.0f, 0, 0});
    for (uint32_t pass = 0; pass < 3; pass++)
    {
        if (pass == 1)
        {
            perf.tracks[0].push_back({100, EventKind::kPitch, 0, 0, 0, 0.05f, 0, 1});
        }
        perf.tracks[0].push_back({100 * pass, EventKind::kNoteOn, 60, 100, 0, 0.0f, pass + 1, 1});
        perf.tracks[0].push_back({100 * pass + 50, EventKind::kNoteOff, 60, 0, 0, 0.0f, pass + 1, 1});
    }
    perf.end_tick = 300;
    int seen_part_a = -1;
    int seen_part_b = -1;
    const auto same = [&](int, const TrackEvent& a, const TrackEvent& b)
    {
        seen_part_a = a.part;
        seen_part_b = b.part;
        return true;
    };

    const bool repeats = RepeatsAsTheGame(perf, {}, 0, 100, LoopRules{.same_pitch = same});

    XSFLIB_CHECK(repeats);
    XSFLIB_CHECK(seen_part_a == 1 && seen_part_b == 1);
}

// The track of TestRepeatOnAPartWithoutAPitch on one part: three passes of 100 ticks, each with note 60 at its start
// and a second note 64 at tick 20 of the pass.
Performance ThreePasses()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    for (uint32_t pass = 0; pass < 3; pass++)
    {
        perf.tracks[0].push_back({100 * pass, EventKind::kNoteOn, 60, 100, 0, 0.0f, 2 * pass + 1, 0});
        perf.tracks[0].push_back({100 * pass + 20, EventKind::kNoteOn, 64, 100, 0, 0.0f, 2 * pass + 2, 0});
        perf.tracks[0].push_back({100 * pass + 50, EventKind::kNoteOff, 60, 0, 0, 0.0f, 2 * pass + 1, 0});
        perf.tracks[0].push_back({100 * pass + 60, EventKind::kNoteOff, 64, 0, 0, 0.0f, 2 * pass + 2, 0});
    }
    perf.end_tick = 300;

    return perf;
}

// LoopRules::plays_alike that takes one pair of notes as other notes: the pass doesn't repeat. It gets the tick of the
// repeat's note and of the game's note.
void TestRepeatThatDoesntPlayAlike()
{
    const Performance perf = ThreePasses();
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    const auto alike = [&pairs](int, uint32_t repeat_tick, uint32_t game_tick)
    {
        pairs.emplace_back(repeat_tick, game_tick);
        return true;
    };
    const auto second_differs = [](int, uint32_t repeat_tick, uint32_t)
    {
        return repeat_tick != 20;
    };

    const bool repeats = RepeatsAsTheGame(perf, {}, 0, 100, LoopRules{.plays_alike = alike});
    const bool differs = RepeatsAsTheGame(perf, {}, 0, 100, LoopRules{.plays_alike = second_differs});

    XSFLIB_CHECK(repeats);
    XSFLIB_CHECK((pairs == std::vector<std::pair<uint32_t, uint32_t>>{{0, 100}, {20, 120}}));
    XSFLIB_CHECK(!differs);
}

// The passes of ThreePasses without note 64 of the pass that starts at `tick`.
Performance WithoutNote64(uint32_t tick)
{
    Performance perf = ThreePasses();
    std::erase_if(perf.tracks[0], [tick](const TrackEvent& e) { return e.key == 64 && e.tick / 100 == tick / 100; });
    return perf;
}

// LoopRules::unstarted names the notes that got no channel. A strike whose twin in the other pass got none counts as no
// strike, in the loop or in the game's next pass. The twin must be on the same track, at the same offset in its pass
// and with the same key.
void TestRepeatOverANoteWithoutAChannel()
{
    const Performance dropped_next = WithoutNote64(100);
    const Performance dropped_first = WithoutNote64(0);
    std::vector<std::tuple<int, uint32_t, uint8_t>> asked;
    const auto at = [&asked](int track, uint32_t tick, uint8_t key)
    {
        asked.emplace_back(track, tick, key);
        return track == 0 && key == 64 && (tick == 120 || tick == 20);
    };
    const auto other_key = [](int track, uint32_t tick, uint8_t key)
    {
        return track == 0 && key == 65 && tick == 120;
    };
    const auto other_track = [](int track, uint32_t tick, uint8_t key)
    {
        return track == 1 && key == 64 && tick == 120;
    };

    XSFLIB_CHECK(!RepeatsAsTheGame(dropped_next, {}, 0, 100, LoopRules{}));
    XSFLIB_CHECK(RepeatsAsTheGame(dropped_next, {}, 0, 100, LoopRules{.unstarted = at}));
    XSFLIB_CHECK((asked == std::vector<std::tuple<int, uint32_t, uint8_t>>{{0, 100, 60}, {0, 120, 64}, {0, 0, 60}}));
    XSFLIB_CHECK(RepeatsAsTheGame(dropped_first, {}, 0, 100, LoopRules{.unstarted = at}));
    XSFLIB_CHECK(!RepeatsAsTheGame(dropped_next, {}, 0, 100, LoopRules{.unstarted = other_key}));
    XSFLIB_CHECK(!RepeatsAsTheGame(dropped_next, {}, 0, 100, LoopRules{.unstarted = other_track}));
}

// A loop from 100 to 200 over a track with two parts. Part 0 takes the new notes, part 1 doesn't. Both send CC10 64 at
// 0 and 30 at 150, and part 0 gets 64 again at 200, the game's next pass. Without an answer of LoopRules::restores,
// only part 0 gets 64 again at the loop's start. An answer of true sends it on part 1 too. An answer of false sends
// nothing on part 0, although the game sets the value back.
void TestRestoreAsTheRulesAnswer()
{
    Performance perf;
    perf.tempo = {{0, 120.0}};
    for (uint8_t part = 0; part < 2; part++)
    {
        perf.tracks[0].push_back({.tick = 0, .kind = EventKind::kPan, .value = 64, .part = part});
    }
    for (uint32_t pass = 0; pass < 3; pass++)
    {
        perf.tracks[0].push_back({100 * pass + 10, EventKind::kNoteOn, 60, 100, 0, 0.0f, 2 * pass + 1, 0});
        perf.tracks[0].push_back({100 * pass + 10, EventKind::kNoteOn, 67, 100, 0, 0.0f, 2 * pass + 2, 1});
        perf.tracks[0].push_back({100 * pass + 50, EventKind::kNoteOff, 60, 0, 0, 0.0f, 2 * pass + 1, 0});
        perf.tracks[0].push_back({100 * pass + 50, EventKind::kNoteOff, 67, 0, 0, 0.0f, 2 * pass + 2, 1});
        if (pass == 1)
        {
            for (uint8_t part = 0; part < 2; part++)
            {
                perf.tracks[0].push_back({.tick = 150, .kind = EventKind::kPan, .value = 30, .part = part});
            }
        }
    }
    perf.tracks[0].push_back({.tick = 200, .kind = EventKind::kPan, .value = 64, .part = 0});
    std::ranges::stable_sort(perf.tracks[0], {}, &TrackEvent::tick);
    perf.loop = Performance::Loop{100, 200};
    perf.end_tick = 300;
    const PassStart next = PassStartAt(perf, {}, 200);
    const auto restored_at_start = [](const Performance& p, uint8_t part)
    {
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.tick == 100 && e.kind == EventKind::kPan && e.part == part && e.value == 64)
            {
                return true;
            }
        }
        return false;
    };

    Performance unanswered = perf;
    RestoreAtLoopStart(unanswered, {}, next, LoopRules{});
    Performance answered = perf;
    RestoreAtLoopStart(answered, {}, next, LoopRules{.restores = [](int, uint8_t, EventKind) { return true; }});
    Performance refused = perf;
    RestoreAtLoopStart(refused, {}, next, LoopRules{.restores = [](int, uint8_t, EventKind) { return false; }});

    XSFLIB_CHECK(restored_at_start(unanswered, 0) && !restored_at_start(unanswered, 1));
    XSFLIB_CHECK(restored_at_start(answered, 0) && restored_at_start(answered, 1));
    XSFLIB_CHECK(!restored_at_start(refused, 0) && !restored_at_start(refused, 1));
}

// The loop that MainLoopPasses marks over passes of 100 ticks from tick 0, as a performer counts them: a choice at the
// end of each pass, until a choice stops the performance or six passes have played. The passes that start at a tick of
// `repeats` repeat as the next pass. A run makes the choices of `replay` first. `played` is the count of passes played,
// `loops` the performance's count of the loop's plays, and `approximations` the performance's list of them.
struct Marked
{
    std::vector<LoopChoice> choices;
    std::optional<seqlib::midi::Performance::Loop> loop;
    std::size_t played = 0;
    int loops = 0;
    std::map<std::string, uint32_t> approximations;
};

Marked MarkPasses(const LoopReplay& replay, const std::set<uint32_t>& repeats)
{
    PerformanceBase out;
    MainLoopPasses passes(replay);
    const std::pair<int, uint32_t> kOwner{0, 0};
    passes.Restart(out, kOwner);
    const auto play_on = [&repeats](uint32_t)
    {
        return [&repeats](uint32_t start, uint32_t)
        {
            return repeats.contains(start);
        };
    };
    for (uint32_t k = 0; k < 6; k++)
    {
        passes.CountPass(out, kOwner, 100 * k, 100 * k + 100);
        if (passes.MarkSteadyPass(out, std::nullopt, 1, play_on))
        {
            break;
        }
    }

    return {passes.Choices(), out.loop, passes.Passes().size(), out.loops, out.approximations};
}

bool LoopsFrom(const Marked& marked, uint32_t start)
{
    return marked.loop && marked.loop->start == start && marked.loop->end == start + 100;
}

// A run on fewer parts checks the first run's loop. The first run marks pass 2: pass 1 doesn't repeat. On the fitted
// parts both repeat, and the loop stays on pass 2: a check never marks an earlier pass. Where pass 1 alone repeats in
// the first run and pass 2 alone on the fitted parts, the loop moves to pass 2. Where the marked pass and the next
// don't repeat on the fitted parts, the first run's pass stays. Without the check, the fitted run makes the first
// run's choices again.
void TestCheckedLoopOnFewerParts()
{
    const Marked second = MarkPasses({}, {100});
    const Marked first = MarkPasses({}, {0});
    XSFLIB_CHECK(LoopsFrom(second, 100) && LoopsFrom(first, 0));

    XSFLIB_CHECK(LoopsFrom(MarkPasses({second.choices, true}, {0, 100}), 100));
    XSFLIB_CHECK(LoopsFrom(MarkPasses({first.choices, true}, {100}), 100));
    XSFLIB_CHECK(LoopsFrom(MarkPasses({second.choices, true}, {300}), 100));
    XSFLIB_CHECK(LoopsFrom(MarkPasses({second.choices, true}, {200}), 200));
    XSFLIB_CHECK(LoopsFrom(MarkPasses({first.choices, false}, {100}), 0));

    // A later run replays the checked run's choices as they were made.
    const Marked checked = MarkPasses({first.choices, true}, {100});
    XSFLIB_CHECK(LoopsFrom(MarkPasses({checked.choices, false}, {}), 100));
}

// The count of strikes that the test of the marked pass left out stays with the choice that marked it
// (LoopChoice::left_out). A later run makes the choice again without a test and keeps the count. A choice after the
// mark that marks nothing doesn't change it.
void TestLeftOutStaysWithTheMarkingChoice()
{
    const auto run = [](const LoopReplay& replay, std::optional<std::size_t> left_out)
    {
        PerformanceBase out;
        MainLoopPasses passes(replay);
        const std::pair<int, uint32_t> kOwner{0, 0};
        passes.Restart(out, kOwner);
        const auto play_on = [](uint32_t)
        {
            return [](uint32_t start, uint32_t)
            {
                return start == 100;
            };
        };
        for (uint32_t k = 0; k < 6; k++)
        {
            passes.CountPass(out, kOwner, 100 * k, 100 * k + 100);
            const bool stop = passes.MarkSteadyPass(out, std::nullopt, 2, play_on);
            if (left_out && passes.Choices().back().marked)
            {
                passes.SetLeftOut(*left_out);
            }
            if (stop)
            {
                break;
            }
        }
        return std::pair(passes.LeftOut(), passes.Choices());
    };

    const auto [counted, choices] = run({}, 3);
    const std::size_t kept = run({choices, false}, std::nullopt).first;

    XSFLIB_CHECK(counted == 3);
    XSFLIB_CHECK(kept == 3);
    XSFLIB_CHECK(run({}, std::nullopt).first == 0);

    const std::vector<LoopChoice> kMarkThenPlayOn = {{.marked = 1, .left_out = 3}, {.extend = true}, {.stop = true}};
    XSFLIB_CHECK(run({kMarkThenPlayOn, false}, std::nullopt).first == 3);
}

// RunUntilSettled runs again on fewer parts where the tracks' parts don't fit MIDI's channels, and again for the level
// scale where CC11 would pass 127. The run on fewer parts checks the first run's last loop choice. The run for the
// level scale makes the fitted run's choices as they were made.
void TestRunsCheckTheLoopOnFewerPartsOnly()
{
    std::vector<LoopReplay> replays;
    const auto run = [&replays](double, const std::array<uint8_t, 16>&, const LoopReplay& replay)
    {
        replays.push_back(replay);
        const bool first = replays.size() == 1;
        return RunOutcome<Performance>{.performance = first ? PartsWithALayer(15) : Performance{},
                                       .level_peak = first ? 0.0 : 300.0,
                                       .parts_used = first ? PartsUsed(15) : std::array<uint8_t, 16>{},
                                       .choices = {{.stop = true, .marked = replays.size() - 1}}};
    };

    RunUntilSettled<Performance>(run);

    XSFLIB_CHECK(replays.size() == 3);
    XSFLIB_CHECK(replays[0].choices.empty() && !replays[0].check_last);
    XSFLIB_CHECK(replays[1].check_last && replays[1].choices.size() == 1 && replays[1].choices[0].marked == 0u);
    XSFLIB_CHECK(!replays[2].check_last && replays[2].choices.size() == 1 && replays[2].choices[0].marked == 1u);
}

// A probe for SearchSteadyPass: the passes that it counted, the passes and blocks that repeat, and those of them that
// repeat with their lengths too (PassTest::kSettled).
struct ScriptedProbe
{
    std::vector<CountedPass> passes;
    const std::set<std::pair<uint32_t, uint32_t>>* repeats = nullptr;
    const std::set<std::pair<uint32_t, uint32_t>>* excused = nullptr;
    const std::set<std::pair<uint32_t, uint32_t>>* settled = nullptr;
    std::size_t whole_span = 0;

    bool operator()(uint32_t start, uint32_t end, PassTest test) const
    {
        if (!repeats->contains({start, end}))
        {
            return test == PassTest::kExcused && excused->contains({start, end});
        }

        return test != PassTest::kSettled || (settled && settled->contains({start, end}));
    }
};

// The loop that MainLoopPasses marks with SearchSteadyPass (PassSearch's defaults and a limit of 10,000 ms) over passes
// from tick 0 whose lengths repeat `lengths`, a tick to the ms. A probe counts the passes that end by its tick, at most
// `counted` of them, and no more than the search asks for. The ranges in `repeats` repeat, those of them in `settled`
// repeat with their lengths too, and those in `excused` repeat once the strikes whose twins got no channel are left
// out. A run makes the choices of `replay` first. As the DS performer does, the run makes a choice at the end of a pass
// only once the performance has played `loops` passes, or the marked pass or block `loops` times, and plays 10 passes
// for each loop at most. Each probe gives the whole period of `whole_span` passes.
Marked SearchPasses(const LoopReplay& replay, const std::vector<uint32_t>& lengths,
                    const std::set<std::pair<uint32_t, uint32_t>>& repeats, std::size_t counted = 100,
                    const std::set<std::pair<uint32_t, uint32_t>>& excused = {}, int loops = 1,
                    const std::set<std::pair<uint32_t, uint32_t>>& settled = {}, std::size_t whole_span = 0)
{
    std::vector<CountedPass> all;
    uint32_t tick = 0;
    for (std::size_t k = 0; k < 40; k++)
    {
        const uint32_t length = lengths[k % lengths.size()];
        all.push_back({tick, tick + length, static_cast<double>(tick + length)});
        tick += length;
    }

    const PassSearch kSearch{.max_ms = 10000.0};
    PerformanceBase out;
    MainLoopPasses passes(replay);
    const std::pair<int, uint32_t> kOwner{0, 0};
    passes.Restart(out, kOwner);
    const auto play_on = [&](uint32_t until, std::size_t enough)
    {
        const std::size_t most = enough > 0 ? std::min(counted, enough) : counted;
        ScriptedProbe probe{.repeats = &repeats, .excused = &excused, .settled = &settled, .whole_span = whole_span};
        for (const CountedPass& pass : all)
        {
            if (pass.end <= until && probe.passes.size() < most)
            {
                probe.passes.push_back(pass);
            }
        }
        return probe;
    };
    for (std::size_t k = 0; k < 10 * static_cast<std::size_t>(loops); k++)
    {
        passes.CountPass(out, kOwner, all[k].start, all[k].end);
        if (out.loops >= loops && passes.MarkSteadyPass(out, std::nullopt, loops, play_on, &kSearch))
        {
            break;
        }
    }

    return {passes.Choices(), out.loop, passes.Passes().size(), out.loops, out.approximations};
}

bool LoopsOn(const Marked& marked, uint32_t start, uint32_t end)
{
    return marked.loop && marked.loop->start == start && marked.loop->end == end;
}

// SearchSteadyPass runs the whole search with the strict test before it leaves out the strikes whose twins got no
// channel. A pass or block that repeats exactly wins over an earlier pass that repeats only with the excuse. The search
// with the excuse reaches as far as the strict one.
void TestStrictRepeatBeforeAnExcusedOne()
{
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {{100, 200}}, 100, {{0, 100}}), 100, 200));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {{300, 400}}, 100, {{0, 100}}), 300, 400));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100, 100, 150}, {{100, 450}}, 100, {{0, 100}}), 100, 450));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {}, 100, {{100, 200}}), 100, 200));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {}, 100, {{300, 400}}), 300, 400));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {{0, 100}}, 100, {{0, 100}}), 0, 100));
}

// SearchSteadyPass. It plays on to the first pass that repeats, up to pass 4, and to no pass that ends past the file's
// limit. Where no pass repeats, it marks a block of 2 or 3 passes whose next block has its length and plays as it does.
// It tests a pass only where the probe counted the pass after it. A check on fewer parts plays on one pass at most.
void TestSearchForTheSteadyPass()
{
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {{0, 100}}), 0, 100));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {{300, 400}}), 300, 400));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100}, {{400, 500}}), 0, 100));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {6000}, {{6000, 12000}}), 0, 6000));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100, 100, 150}, {{100, 450}}), 100, 450));
    XSFLIB_CHECK(LoopsOn(SearchPasses({}, {100, 100, 150}, {{0, 200}}), 0, 100));

    const Marked uncounted = SearchPasses({}, {100}, {{0, 100}}, 1);
    XSFLIB_CHECK(!uncounted.choices.empty() && !uncounted.choices.back().marked);

    const Marked first = SearchPasses({}, {100}, {{0, 100}});
    XSFLIB_CHECK(LoopsOn(SearchPasses({first.choices, true}, {100}, {{200, 300}}), 0, 100));
    XSFLIB_CHECK(LoopsOn(SearchPasses({first.choices, true}, {100}, {{100, 200}}), 100, 200));
}

// Ten passes from tick 0 that last `length` ticks each.
std::set<std::pair<uint32_t, uint32_t>> TenPasses(uint32_t length = 100)
{
    std::set<std::pair<uint32_t, uint32_t>> passes;
    for (uint32_t k = 0; k < 10; k++)
    {
        passes.insert({length * k, length * k + length});
    }

    return passes;
}

// SearchSteadyPass looks for a settled pass first: one that repeats with its lengths, and whose next pass does too. It
// plays on to that pass, up to pass 4 and within the file's limit. Without one, it marks the first pass that repeats.
// At two loops of 4,000 ms, pass 3 ends past the limit of 10,000 ms. At five loops, the near probe finds the settled
// pass without the far probe.
void TestSettledPassFirst()
{
    const std::set<std::pair<uint32_t, uint32_t>> kEvery = TenPasses();
    const std::set<std::pair<uint32_t, uint32_t>> kLong = {{0, 4000}, {4000, 8000}, {8000, 12000}, {12000, 16000}};

    const Marked second = SearchPasses({}, {100}, kEvery, 100, {}, 1, {{100, 200}, {200, 300}});
    const Marked fourth = SearchPasses({}, {100}, kEvery, 100, {}, 1, {{300, 400}, {400, 500}});
    const Marked fifth = SearchPasses({}, {100}, kEvery, 100, {}, 1, {{400, 500}, {500, 600}});
    const Marked next_unsettled = SearchPasses({}, {100}, kEvery, 100, {}, 1, {{100, 200}, {300, 400}});
    const Marked past_limit = SearchPasses({}, {4000}, kLong, 100, {}, 2, {{8000, 12000}, {12000, 16000}});
    const Marked unrepeated = SearchPasses({}, {100}, {{0, 100}}, 100, {}, 1, {{100, 200}, {200, 300}});
    const Marked near_only = SearchPasses({}, {100}, kEvery, 100, {}, 5, {{100, 200}, {200, 300}});

    XSFLIB_CHECK(LoopsOn(second, 100, 200));
    XSFLIB_CHECK(LoopsOn(fourth, 300, 400));
    XSFLIB_CHECK(LoopsOn(fifth, 0, 100));
    XSFLIB_CHECK(LoopsOn(next_unsettled, 0, 100));
    XSFLIB_CHECK(LoopsOn(past_limit, 0, 4000));
    XSFLIB_CHECK(LoopsOn(unrepeated, 0, 100));
    XSFLIB_CHECK(LoopsOn(near_only, 100, 200));
}

// The performance plays on toward a settled pass only where that pass ends within the file's limit. At two loops of
// 3,000 ms, pass 2 ends at 9,000 ms and the settled pass 3 at 12,000 ms. The loop stays on pass 0 after two passes.
void TestNoSettledPassPastTheLimit()
{
    const Marked marked = SearchPasses({}, {3000}, TenPasses(3000), 100, {}, 2, {{9000, 12000}, {12000, 15000}});

    XSFLIB_CHECK(LoopsOn(marked, 0, 3000));
    XSFLIB_CHECK(marked.played == 2);
}

// A later call reaches further than the call that marked a strict pass. At two loops, where passes 3 and later repeat,
// the strict tier marks pass 3 after four passes, and the performance plays on to play it twice. The call after pass 6
// reaches the settled passes 4 and 5, and the loop moves to pass 4 after six passes.
void TestSettledPassOnALaterCall()
{
    std::set<std::pair<uint32_t, uint32_t>> repeats = TenPasses();
    repeats.erase({0, 100});
    repeats.erase({100, 200});
    repeats.erase({200, 300});

    const Marked strict = SearchPasses({}, {100}, repeats, 100, {}, 2);
    const Marked settled = SearchPasses({}, {100}, repeats, 100, {}, 2, {{400, 500}, {500, 600}});

    XSFLIB_CHECK(LoopsOn(strict, 300, 400) && strict.played == 5);
    XSFLIB_CHECK(LoopsOn(settled, 400, 500) && settled.played == 6);
}

// Where nothing repeats, the search tests the whole period of the tracks' loops: a block of `whole_span` passes from
// pass 0 or 1. It marks the block where the block repeats and a play ends within the limit of 10,000 ms, and the
// performance plays on to the block's end. Blocks of 2,500 ms end past the limit. Of blocks of 1,500 ms, one play fits
// where two don't: the file plays the block once. Of blocks of 4,500 ms, two plays fit where three don't: the file
// plays the block twice, and stops at the end of the second play. A block from pass 0 that settles, whose next block
// doesn't, gives way to the settled block from pass 1.
void TestWholePeriod()
{
    const Marked fits = SearchPasses({}, {100}, {{0, 500}}, 100, {}, 1, {}, 5);
    const Marked later = SearchPasses({}, {100}, {{100, 600}}, 100, {}, 1, {}, 5);
    const Marked twice = SearchPasses({}, {900}, {{0, 4500}}, 100, {}, 2, {}, 5);
    const Marked long_passes = SearchPasses({}, {2500}, {{0, 12500}}, 100, {}, 1, {}, 5);
    const Marked long_twice = SearchPasses({}, {1500}, {{0, 7500}}, 100, {}, 2, {}, 5);
    const Marked thrice = SearchPasses({}, {900}, {{0, 4500}}, 100, {}, 3, {}, 5);
    const Marked settled = SearchPasses({}, {100}, {{0, 500}, {100, 600}, {500, 1000}, {600, 1100}}, 100, {}, 1,
                                        {{0, 500}, {100, 600}, {600, 1100}}, 5);

    XSFLIB_CHECK(LoopsOn(fits, 0, 500) && fits.played == 5);
    XSFLIB_CHECK(LoopsOn(later, 100, 600) && later.played == 6);
    XSFLIB_CHECK(LoopsOn(twice, 0, 4500) && twice.played == 10 && twice.loops == 2);
    XSFLIB_CHECK(LoopsOn(long_passes, 0, 2500));
    XSFLIB_CHECK(LoopsOn(long_twice, 0, 7500) && long_twice.played == 5 && long_twice.loops == 1);
    XSFLIB_CHECK(LoopsOn(thrice, 0, 4500) && thrice.played == 10 && thrice.loops == 2);
    XSFLIB_CHECK(LoopsOn(settled, 100, 600) && settled.played == 6);
}

// Where the whole period doesn't repeat, the loop stays on the first pass, and the performance lists the period. A run
// that makes the first run's choices again, without the probes, lists it too. A period that the search marks lists
// nothing. A search that stops without a probe, after a draw of a random value in the first pass, lists the period
// that the performance gives.
void TestWholePeriodListed()
{
    const char kLine[] = "tracks loop at other lengths (their loops line up every 5 passes)";
    const std::set<std::pair<uint32_t, uint32_t>> kNone;
    const PassSearch kSearch{.max_ms = 10000.0};
    const std::pair<int, uint32_t> kOwner{0, 0};
    PerformanceBase drawn;
    MainLoopPasses passes(LoopReplay{});
    passes.Restart(drawn, kOwner);
    passes.CountPass(drawn, kOwner, 0, 100);
    passes.CountPass(drawn, kOwner, 100, 200);
    const auto play_on = [&kNone](uint32_t, std::size_t)
    {
        return ScriptedProbe{.repeats = &kNone, .excused = &kNone};
    };

    const Marked unrepeated = SearchPasses({}, {100}, {{0, 400}}, 100, {}, 1, {}, 5);
    const Marked replayed = SearchPasses({unrepeated.choices, false}, {100}, {{0, 400}});
    const Marked fits = SearchPasses({}, {100}, {{0, 500}}, 100, {}, 1, {}, 5);
    const bool drawn_stops = passes.MarkSteadyPass(drawn, uint32_t{50}, 1, play_on, &kSearch, 5);

    XSFLIB_CHECK(LoopsOn(unrepeated, 0, 100) && unrepeated.approximations.contains(kLine));
    XSFLIB_CHECK(LoopsOn(replayed, 0, 100) && replayed.approximations.contains(kLine));
    XSFLIB_CHECK(fits.approximations.empty());
    XSFLIB_CHECK(drawn_stops && drawn.approximations.contains(kLine));
}

// The whole period of loops of 100, 300 and 200 ticks over passes of 100 is 6 passes. A loop that hasn't played gives
// none. Loops of 1,009 and 1,013 ticks over passes of 1 line up only after over kMostWholeSpan passes.
void TestWholeSpan()
{
    XSFLIB_CHECK(WholeSpan(100, {100, 300, 200}) == 6);
    XSFLIB_CHECK(WholeSpan(100, {100, 0}) == 0);
    XSFLIB_CHECK(WholeSpan(0, {100}) == 0);
    XSFLIB_CHECK(WholeSpan(1, {1009, 1013}) == kMostWholeSpan + 1);
}

// Where the far probe doesn't start, the near probe tests the blocks, strict and with the excuse. At 5 loops and more,
// the performance has played the passes that the far probe would, and the block of passes 2 and 3 repeats. Where the
// far probe starts, its later single passes come before the blocks: at 2 loops, pass 4 wins over the block of passes 1
// and 2.
void TestBlockOnTheNearProbe()
{
    const Marked later = SearchPasses({}, {100}, {{0, 200}, {300, 400}}, 100, {}, 2);

    XSFLIB_CHECK(LoopsOn(later, 300, 400));

    for (const int loops : {4, 5, 6})
    {
        const Marked block = SearchPasses({}, {100}, {{100, 300}}, 100, {}, loops);
        const Marked excused = SearchPasses({}, {100}, {}, 100, {{100, 300}}, loops);

        XSFLIB_CHECK(LoopsOn(block, 100, 300) && block.loops == loops);
        XSFLIB_CHECK(LoopsOn(excused, 100, 300) && excused.loops == loops);
    }
}

// LengthsRepeat compares each note's length with its twin's in the next pass, within the tolerance, from its strike's
// frame to its end's. A note without an end counts as alike. Another key, velocity or offset doesn't repeat, and nor
// does a note more.
void TestLengthsRepeat()
{
    const auto two_passes = [](uint8_t next_key, uint32_t next_offset, double next_end, double next_after = 0.0)
    {
        Performance p;
        std::vector<TrackEvent>& events = p.tracks[0];
        events.push_back({.tick = 10, .kind = EventKind::kNoteOn, .key = 60, .value = 100, .note = 1});
        events.push_back({.tick = 30, .kind = EventKind::kNoteOff, .key = 60, .note = 1});
        events.push_back({.tick = 50, .kind = EventKind::kNoteOn, .key = 62, .value = 90, .note = 2});
        events.push_back({.tick = 100 + next_offset,
                          .kind = EventKind::kNoteOn,
                          .key = next_key,
                          .value = 100,
                          .note = 3,
                          .after = next_after});
        const auto end_tick = static_cast<uint32_t>(next_end);
        events.push_back(
            {.tick = end_tick, .kind = EventKind::kNoteOff, .key = next_key, .note = 3, .after = next_end - end_tick});
        events.push_back({.tick = 150, .kind = EventKind::kNoteOn, .key = 62, .value = 90, .note = 4});
        events.push_back({.tick = 190, .kind = EventKind::kNoteOff, .key = 62, .note = 4});
        return p;
    };

    XSFLIB_CHECK(LengthsRepeat(two_passes(60, 10, 136.0), 0, 100, 6.0));
    XSFLIB_CHECK(LengthsRepeat(two_passes(60, 10, 124.0), 0, 100, 6.0));
    XSFLIB_CHECK(!LengthsRepeat(two_passes(60, 10, 136.5), 0, 100, 6.0));
    XSFLIB_CHECK(!LengthsRepeat(two_passes(60, 10, 123.5), 0, 100, 6.0));
    XSFLIB_CHECK(!LengthsRepeat(two_passes(61, 10, 130.0), 0, 100, 6.0));
    XSFLIB_CHECK(!LengthsRepeat(two_passes(60, 11, 131.0), 0, 100, 6.0));
    XSFLIB_CHECK(LengthsRepeat(two_passes(60, 10, 136.5, 0.5), 0, 100, 6.0));

    Performance extra = two_passes(60, 10, 130.0);
    extra.tracks[0].push_back({.tick = 160, .kind = EventKind::kNoteOn, .key = 64, .value = 90, .note = 5});

    XSFLIB_CHECK(!LengthsRepeat(extra, 0, 100, 6.0));
}

// A check on fewer parts never makes a block shorter. The first run marks the block of passes 1 and 2: no single pass
// repeats. On the fitted parts pass 1 alone repeats, and the loop stays on the block. Where the block repeats there
// too, or nothing repeats, the block stays as well.
void TestCheckKeepsTheBlock()
{
    const Marked block = SearchPasses({}, {100}, {{0, 200}});

    const Marked single = SearchPasses({block.choices, true}, {100}, {{0, 100}});
    const Marked both = SearchPasses({block.choices, true}, {100}, {{0, 100}, {0, 200}});
    const Marked none = SearchPasses({block.choices, true}, {100}, {});

    XSFLIB_CHECK(LoopsOn(block, 0, 200));
    XSFLIB_CHECK(LoopsOn(single, 0, 200));
    XSFLIB_CHECK(LoopsOn(both, 0, 200));
    XSFLIB_CHECK(LoopsOn(none, 0, 200));
}

// At more than one loop, a marked block plays that many full times, and a check on fewer parts keeps it. The first run
// marks the block of passes 2 and 3 (or 2 to 4) and plays on to the end of its last play. On the fitted parts pass 2
// alone repeats, and the loop stays on the block. A replay whose last choice marks nothing checks the block that an
// earlier choice marked.
void TestBlockPlaysEveryLoop()
{
    for (const int loops : {2, 3})
    {
        const auto plays = [loops](const Marked& marked, uint32_t end, std::size_t span)
        {
            return LoopsOn(marked, 100, end) && marked.played == 1 + span * static_cast<std::size_t>(loops) &&
                   marked.loops == loops;
        };

        const Marked two = SearchPasses({}, {100}, {{100, 300}}, 100, {}, loops);
        const Marked three = SearchPasses({}, {100}, {{100, 400}}, 100, {}, loops);
        const Marked checked = SearchPasses({two.choices, true}, {100}, {{100, 200}}, 100, {}, loops);

        XSFLIB_CHECK(plays(two, 300, 2));
        XSFLIB_CHECK(plays(three, 400, 3));
        XSFLIB_CHECK(plays(checked, 300, 2));
    }

    const std::vector<LoopChoice> kMarkThenStop = {{.extend = true, .toward = 2},
                                                   {.extend = true, .toward = 2},
                                                   {.marked = 1, .span = 2},
                                                   {.marked = 1, .span = 2},
                                                   {.stop = true}};
    const Marked replayed = SearchPasses({kMarkThenStop, true}, {100}, {{100, 200}}, 100, {}, 2);

    XSFLIB_CHECK(LoopsOn(replayed, 100, 300));
}

// A new owner of the loop ends the block that the last owner's passes played. A replay marks the block of passes 2
// and 3 at two loops, and the performance plays on. Another loop then takes the markers: its first pass doesn't repeat,
// and the choice after it marks nothing.
void TestNewOwnerEndsTheBlock()
{
    const std::set<std::pair<uint32_t, uint32_t>> kNone;
    const PassSearch kSearch{.max_ms = 10000.0};
    const std::pair<int, uint32_t> kFirst{0, 0};
    const std::pair<int, uint32_t> kSecond{1, 0};
    const auto play_on = [&kNone](uint32_t, std::size_t)
    {
        return ScriptedProbe{.repeats = &kNone, .excused = &kNone};
    };
    PerformanceBase out;
    MainLoopPasses passes(std::vector<LoopChoice>{{.marked = 1, .span = 2}});

    // The first owner's passes: the replay marks the block of passes 2 and 3, and the performance plays on.
    passes.Restart(out, kFirst);
    for (uint32_t k = 0; k < 3; k++)
    {
        passes.CountPass(out, kFirst, 100 * k, 100 * k + 100);
    }
    const bool stopped = passes.MarkSteadyPass(out, std::nullopt, 2, play_on, &kSearch);

    // The second owner's passes: the first doesn't repeat.
    passes.Restart(out, kSecond);
    for (uint32_t k = 3; k < 6; k++)
    {
        passes.CountPass(out, kSecond, 100 * k, 100 * k + 100);
    }
    passes.MarkSteadyPass(out, std::nullopt, 2, play_on, &kSearch);

    XSFLIB_CHECK(!stopped && passes.Choices().size() == 2 && !passes.Choices().back().marked);
}

int main()
{
    TestOverridesReplaceOnlyTheSetValues();
    TestLfoOnTheWiiUAndTheSwitchIsInHzAndMilliseconds();
    TestLfoOnThe3dsAndTheWiiIsInTheBytecodesUnits();
    TestPerformanceWritesAsMidi();
    TestFitPartsKeepsAChannelForALayer();
    TestFitPartsGivesPartsTheChannelsThatLayersLeave();
    TestRepeatOnAPartWithoutAPitch();
    TestRepeatWithADifferentPitch();
    TestRepeatPitchWithoutAnEventNamesItsPart();
    TestRepeatThatDoesntPlayAlike();
    TestRepeatOverANoteWithoutAChannel();
    TestRestoreAsTheRulesAnswer();
    TestCheckedLoopOnFewerParts();
    TestRunsCheckTheLoopOnFewerPartsOnly();
    TestLeftOutStaysWithTheMarkingChoice();
    TestSearchForTheSteadyPass();
    TestStrictRepeatBeforeAnExcusedOne();
    TestSettledPassFirst();
    TestNoSettledPassPastTheLimit();
    TestSettledPassOnALaterCall();
    TestWholePeriod();
    TestWholePeriodListed();
    TestWholeSpan();
    TestBlockOnTheNearProbe();
    TestLengthsRepeat();
    TestCheckKeepsTheBlock();
    TestBlockPlaysEveryLoop();
    TestNewOwnerEndsTheBlock();

    return xsflib::testing::Summary("sequence_export_performance");
}
