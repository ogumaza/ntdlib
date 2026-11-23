// SPDX-License-Identifier: MIT

// Tests of the DS conversion's loops and parts (ntdlib/sequence_export/ntr): the bent and swept notes that take parts
// of their own, the pass test that finds a sequence's loop, and the settings that the files keep across the loop.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/sdat_files.h"
#include "sequence_export/ntr/stands.h"
#include "sequence_export_ntr_cases.h"

namespace
{

using namespace sequence_export_ntr_cases;

// Whether the pitches `pitches` are `nominal`'s, each within 5 cents. A note alone on its part bends at the rate of its
// channel's timer, whose period has no fraction (TimerBend). Wave 0's timer of 511 then plays a semitone or two above
// its root key up to 4 cents sharp.
bool NearPitches(const std::vector<float>& pitches, const std::vector<float>& nominal)
{
    bool near = pitches.size() == nominal.size();
    for (std::size_t i = 0; near && i < pitches.size(); i++)
    {
        near = std::abs(pitches[i] - nominal[i]) < 0.05f && (nominal[i] != 0.0f || pitches[i] == 0.0f);
    }

    return near;
}

// A track's pitch events from `tick` on, in semitones.
std::vector<float> PitchesFrom(const Performance& p, int track, uint32_t tick)
{
    std::vector<float> pitches;
    for (const seqlib::midi::TrackEvent& e : EventsOf(p, track, EventKind::kPitch))
    {
        if (e.tick >= tick)
        {
            pitches.push_back(e.semitones);
        }
    }

    return pitches;
}

// A tie that starts the channel's sweep again. With a portamento time of 8, a sweep of p 64ths of a semitone runs for
// 64 * |p| >> 11 frames. Each frame's update plays p * (length - counter) / length and steps the counter. A tick runs
// every second frame, and the notes last 24 ticks.
void TestTiedSweeps()
{
    // 0: ties on; 2: a portamento time of 8; 4: a sweep of 32; 7: note 60; 10: a sweep of 128; 13: note 60; 16: end.
    // The first sweep has a length of 1. It plays half a semitone in the note's first frame, and its counter stays at
    // 1. The tie's sweep has a length of 4, and its counter starts at 0 again. Its first frame steps the counter to 1.
    // The sweep plays 2, 1.5, 1 and 0.5 semitones, then ends.
    const TestArchive restarted(
        {0xC8, 0x01, 0xCF, 0x08, 0xE3, 0x20, 0x00, 0x3C, 0x7F, 0x18, 0xE3, 0x80, 0x00, 0x3C, 0x7F, 0x18, 0xFF});
    const Performance p = Perform(restarted.archive, {0});
    Check(NearPitches(PitchesFrom(p, 0, 0), {0.5f, 0.0f, 2.0f, 1.5f, 1.0f, 0.5f, 0.0f}) &&
              NearPitches(PitchesFrom(p, 0, 24), {2.0f, 1.5f, 1.0f, 0.5f, 0.0f}),
          "a tie's sweep plays from its first frame, whatever the count of the sweep before");
    Check(EventsOf(p, 0, EventKind::kNoteOn).size() == 1 && p.approximations.empty(), "and the tie starts no note");

    // The same with sweeps of 64 and 16. The first has a length of 2: 1 and 0.5 semitones. The tie's sweep has a
    // length of 0, and the driver plays none of it. The performer divides by no length of 0.
    const TestArchive without_length(
        {0xC8, 0x01, 0xCF, 0x08, 0xE3, 0x40, 0x00, 0x3C, 0x7F, 0x18, 0xE3, 0x10, 0x00, 0x3C, 0x7F, 0x18, 0xFF});
    const Performance q = Perform(without_length.archive, {0});
    Check(NearPitches(PitchesFrom(q, 0, 0), {1.0f, 0.5f, 0.0f}) && PitchesFrom(q, 0, 24).empty(),
          "a tie's sweep of no length plays nothing");
}

// The parts of a track's note-ons, in their order.
std::vector<uint8_t> NoteParts(const Performance& p, int track)
{
    std::vector<uint8_t> parts;
    for (const seqlib::midi::TrackEvent& e : EventsOf(p, track, EventKind::kNoteOn))
    {
        parts.push_back(e.part);
    }

    return parts;
}

// The pitch events of a track's part, in semitones.
std::vector<float> PartPitches(const Performance& p, int track, uint8_t part)
{
    std::vector<float> pitches;
    for (const seqlib::midi::TrackEvent& e : EventsOf(p, track, EventKind::kPitch))
    {
        if (e.part == part)
        {
            pitches.push_back(e.semitones);
        }
    }

    return pitches;
}

// A note that starts while another note of its track sweeps. A sweep of 256 with a portamento time of 8 runs for 8
// frames: each frame's update plays 4, 3.5 ... 0.5 semitones and steps the counter, and the 9th frame plays 0. A tick
// runs every second frame. The second note starts in the ticks of a frame, before that frame's update applies the
// sweep.
void TestSweepUnderAnotherNote()
{
    // 0: no note waits; 2: a portamento time of 8; 4: a sweep of 256; 7: note 60 for 48; 10: a wait of `wait` ticks;
    // 12: no sweep; 15: note 67 for 24; 18: a wait of 48; 20: end.
    const auto bytecode = [](uint8_t wait)
    {
        return Bytes{0xC7, 0x00, 0xCF, 0x08, 0xE3, 0x00, 0x01, 0x3C, 0x7F, 0x30, 0x80,
                     wait, 0xE3, 0x00, 0x00, 0x43, 0x7F, 0x18, 0x80, 0x30, 0xFF};
    };

    // After 2 ticks, the frame's update plays 2 semitones of the sweep. Note 67 doesn't sweep. It takes another part,
    // and the sweep goes on to its end on the first note's part.
    const TestArchive during(bytecode(2));
    const Performance p = Perform(during.archive, {0});
    Check(NoteParts(p, 0) == std::vector<uint8_t>{0, 1},
          "a note that starts during another note's sweep takes another part");
    Check(NearPitches(PartPitches(p, 0, 0), {4.0f, 3.5f, 3.0f, 2.5f, 2.0f, 1.5f, 1.0f, 0.5f, 0.0f}),
          "the sweep plays on to its end on the first note's part");

    // After 4 ticks, the frame's update plays none of the sweep: its counter has reached its length. Note 67 shares
    // the first note's part.
    const TestArchive after(bytecode(4));
    const Performance q = Perform(after.archive, {0});
    Check(NoteParts(q, 0) == std::vector<uint8_t>{0, 0}, "a note that starts once the sweep has ended shares the part");
    Check(NearPitches(PartPitches(q, 0, 0), {4.0f, 3.5f, 3.0f, 2.5f, 2.0f, 1.5f, 1.0f, 0.5f, 0.0f}),
          "and the sweep has played to its end");
}

// A part that holds one bent note plays the timer's pitch for the bend. Once the note is released, the part still
// plays the track's bend: the next note shares the part, and the release tail follows no change.
void TestTimerPitchAfterRelease()
{
    // 0: no note waits; 2: a bend of 0.5 semitones; 4: a release of 100, 2.3 s; then note 60 for 24 ticks and, after
    // 48 ticks, note 64 for 48. Note 64 starts while note 60's release sounds.
    const TestArchive released(
        {0xC7, 0x00, 0xC4, 0x20, 0xD3, 0x64, 0x3C, 0x7F, 0x18, 0x80, 0x30, 0x40, 0x7F, 0x30, 0x80, 0x30, 0xFF});
    const Performance p = Perform(released.archive, {0});
    const std::vector<float> pitches = PartPitches(p, 0, 0);
    Check(!pitches.empty() && pitches.front() != 0.5f && std::abs(pitches.front() - 0.5f) < 0.05f,
          "the held note bends to the timer's pitch, a little off the track's bend");
    Check(NoteParts(p, 0) == std::vector<uint8_t>{0, 0} && p.approximations.empty(),
          "the next note shares the part, and the release tail follows no change");
}

// A loop whose first pass bends a note on a part that holds another, and whose later passes bend it alone. 0: no note
// waits; 2: a bend of 0.5 semitones; 4: a release of 100; note 64 for 96 ticks and a wait of 24; then the loop: a
// release of 127, note 60 for 24 ticks, a wait of 300 and a jump back. The first pass plays note 60 at the track's bend
// on note 64's part. The second plays it alone at the timer's pitch, 1.6 cents higher. The loop starts at the first
// pass: the pass test takes two pitches as the same where both stand for the same bend and are within the timer's
// rounding (TimerRounding).
//
// A real bend of one step, 1.6 cents, in the first pass only: a wait of 24 ticks and a bend of 1/64 semitone; then the
// loop: note 36 for 24 ticks, a wait of 300, a bend of 0 and a jump back. The first pass plays the bend at the timer's
// 1.7 cents, and the later passes play no bend. The two pitches are within the timer's rounding but stand for other
// bends. The loop starts at the second pass. The same bend on note 84 plays period 127 at the timer's pitch: 0.2
// cents, well within the 14 cents by which the period can round, and still another bend.
//
// The first loop nine semitones higher, with note 73 and note 69. Wave 0's period is 303 at key 69 and 295 at its
// bend. The bent note plays 3.9 cents below the track's bend: over 3 cents, and within the 6.4 cents by which the
// period of 295 can round. The loop still starts at the first pass.
void TestLoopOverABentNote()
{
    const TestArchive test({0xC7, 0x00, 0xC4, 0x20, 0xD3, 0x64, 0x40, 0x7F, 0x60, 0x80, 0x18, 0xD3,
                            0x7F, 0x3C, 0x7F, 0x18, 0x80, 0x82, 0x2C, 0x94, 0x0B, 0x00, 0x00});
    const Performance p = Perform(test.archive, {0});
    const std::vector<float> pitches = PartPitches(p, 0, 0);
    Check(pitches.size() >= 4 && pitches[1] == 0.5f && pitches[3] > 0.5f && pitches[3] < 0.56f,
          "the first pass's note at the track's bend, and the second's at the timer's pitch");
    Check(p.loop && p.loop->start == 24 && p.loop->end == 324, "the loop starts at the first pass");

    const TestArchive high({0xC7, 0x00, 0xC4, 0x20, 0xD3, 0x64, 0x49, 0x7F, 0x60, 0x80, 0x18, 0xD3,
                            0x7F, 0x45, 0x7F, 0x18, 0x80, 0x82, 0x2C, 0x94, 0x0B, 0x00, 0x00});
    const Performance h = Perform(high.archive, {0});
    const std::vector<float> high_pitches = PartPitches(h, 0, 0);
    Check(high_pitches.size() >= 4 && high_pitches[1] == 0.5f && high_pitches[3] < 0.47f && high_pitches[3] > 0.45f,
          "a high note alone plays 3.9 cents below the track's bend");
    Check(h.loop && h.loop->start == 24 && h.loop->end == 324,
          "the timer's rounding over 3 cents: the first pass loops");

    const TestArchive real_bend(
        {0x80, 0x18, 0xC4, 0x01, 0x24, 0x7F, 0x18, 0x80, 0x82, 0x2C, 0xC4, 0x00, 0x94, 0x04, 0x00, 0x00});
    const Performance q = Perform(real_bend.archive, {0});
    Check(q.loop && q.loop->start == 348 && q.loop->end == 672,
          "a bend in the first pass only: the loop starts after it");

    const TestArchive high_real_bend(
        {0x80, 0x18, 0xC4, 0x01, 0x54, 0x7F, 0x18, 0x80, 0x82, 0x2C, 0xC4, 0x00, 0x94, 0x04, 0x00, 0x00});
    const Performance r = Perform(high_real_bend.archive, {0});
    Check(r.loop && r.loop->start == 348 && r.loop->end == 672,
          "a bend within the timer's rounding in the first pass only: the loop starts after it");
}

// Whether `p` loops from `start` to `end`.
bool LoopsOn(const Performance& p, uint32_t start, uint32_t end)
{
    return p.loop && p.loop->start == start && p.loop->end == end;
}

// A loop whose first pass plays its note shorter than the later passes do. 0: no note waits; 2: variable 0 takes
// `first`; then the loop: note 60 for the ticks in variable 0 (the prefix 0xA1), variable 0 takes 24, a wait of 300 and
// a jump back. Every pass strikes the same note. A note 12 or 7 ticks shorter in the first pass moves the loop to the
// second pass. The passes after the second repeat it with their lengths. A note 6 ticks shorter keeps the loop on the
// first pass.
void TestLoopOnASettledPass()
{
    const auto loop = [](uint8_t first)
    {
        const TestArchive test({0xC7, 0x00, 0xB0, 0x00, first, 0x00, 0xA1, 0x3C, 0x7F, 0x00, 0xB0,
                                0x00, 0x18, 0x00, 0x80, 0x82,  0x2C, 0x94, 0x06, 0x00, 0x00});
        return Perform(test.archive, {0});
    };

    const Performance shorter = loop(12);
    const Performance a_tick_over = loop(17);
    const Performance within = loop(18);

    Check(LoopsOn(shorter, 300, 600), "a note 12 ticks shorter in the first pass: the loop starts at the second");
    Check(LoopsOn(a_tick_over, 300, 600), "a note 7 ticks shorter in the first pass: the loop starts at the second");
    Check(LoopsOn(within, 0, 300), "a note 6 ticks shorter in the first pass: the loop starts at the first");
}

// The value of `kind` on part `part` of track `track` at a note-on in tick `tick`, in the order of time that the MIDI
// file plays: the last event before the tick, or in the tick at its start. -1 without such an event.
int ValueAtTick(const Performance& p, int track, uint8_t part, EventKind kind, uint32_t tick)
{
    std::vector<seqlib::midi::TrackEvent> events = p.tracks[static_cast<std::size_t>(track)];
    std::ranges::stable_sort(events, {}, [](const seqlib::midi::TrackEvent& e) { return std::pair(e.tick, e.after); });

    int value = -1;
    for (const seqlib::midi::TrackEvent& e : events)
    {
        if (e.tick > tick || (e.tick == tick && e.after > 0.0))
        {
            break;
        }
        if (e.kind == kind && e.part == part)
        {
            value = e.value;
        }
    }

    return value;
}

// The value of `kind` on part `part` of track `track` that a MIDI player starts a repeat of the loop with: the value
// that the loop's first tick sends at its start, or else the last value before the loop's end, by tick and `after`. -1
// without either.
int RepeatValue(const Performance& p, int track, uint8_t part, EventKind kind)
{
    if (!p.loop)
    {
        return -1;
    }

    std::vector<seqlib::midi::TrackEvent> events = p.tracks[static_cast<std::size_t>(track)];
    std::ranges::stable_sort(events, {}, [](const seqlib::midi::TrackEvent& e) { return std::pair(e.tick, e.after); });
    int sent = -1;
    int kept = -1;
    for (const seqlib::midi::TrackEvent& e : events)
    {
        if (e.kind != kind || e.part != part || e.tick >= p.loop->end)
        {
            continue;
        }

        kept = e.value;
        if (e.tick == p.loop->start && e.after == 0.0)
        {
            sent = e.value;
        }
    }

    return sent >= 0 ? sent : kept;
}

// The pass test reads what CC10, CC11 and CC1 stand for on a part: the track's pan, level and LFO depth, whether the
// part follows a pan LFO or not. A pass that plays its notes in one form and the next pass in the other repeats.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 64; 6: a release of 100, 2.3 s; 8: a wait of 24; then the
// loop: note 60 for 24 ticks, a wait of 48 and a jump back. The first pass's note strikes alone and follows the LFO,
// with CC1 0. Each later note strikes over the tail of the one before, with the track's values and CC1 at the depth.
// The game plays every pass alike, and the loop starts at the first pass. A repeat starts its note with the CC10 and
// CC11 of the first pass's note.
void TestLoopOverAReleaseTail()
{
    const TestArchive test({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x40, 0xD3, 0x64, 0x80, 0x18, 0x3C, 0x7F, 0x18, 0x80, 0x30,
                            0x94, 0x0A, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 24, 72), "a note over a release tail: the loop starts at the first pass");
    Check(
        ValueAtTick(p, 0, 0, EventKind::kModulation, 24) == 0 && ValueAtTick(p, 0, 0, EventKind::kModulation, 72) == 64,
        "the first note follows its LFO with CC1 0, and the second plays CC1 at the depth");
    Check(RepeatValue(p, 0, 0, EventKind::kPan) == ValueAtTick(p, 0, 0, EventKind::kPan, 24) &&
              RepeatValue(p, 0, 0, EventKind::kExpression) == ValueAtTick(p, 0, 0, EventKind::kExpression, 24),
          "a repeat starts its note with the first pass's CC10 and CC11");
}

// The depth of a pan LFO at each strike counts, although CC1 is 0 on a part that follows the LFO.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 0; 6: a wait of 24; then the loop: note 60 for 48 ticks, a
// wait of 4, a depth of 30, a wait of 44 and a jump back. The first pass strikes its note at depth 0, and the later
// passes at depth 30. Both play CC1 0 at the strike. The loop starts at the second pass. With the depth of 30 before
// the loop, every pass strikes at depth 30, and the loop starts at the first pass.
void TestPanLfoDepthSetAfterTheFirstStrike()
{
    const auto bytecode = [](uint8_t depth)
    {
        return Bytes{0xC7, 0x00, 0xCC, 0x02, 0xCA, depth, 0x80, 0x18, 0x3C, 0x7F, 0x30,
                     0x80, 0x04, 0xCA, 0x1E, 0x80, 0x2C,  0x94, 0x08, 0x00, 0x00};
    };
    const TestArchive late(bytecode(0x00));
    const TestArchive early(bytecode(0x1E));

    const Performance p = Perform(late.archive, {0});
    const Performance q = Perform(early.archive, {0});

    Check(LoopsOn(p, 72, 120), "a pan LFO's depth set after the first strike: the loop starts at the second pass");
    Check(LoopsOn(q, 24, 72), "the depth set before the loop: the loop starts at the first pass");
}

// A pan LFO's depth at a strike counts where a repeat would keep the right CC1. The SoundFont plays the LFO only as
// the pass wrote it.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 0; 6: a wait of 24; then the loop: note 60 for 48 ticks, a
// wait of 4, a depth of 30, a wait of 44, notes 64 and 67 for 6 ticks each, a wait of 4 and a jump back. The two last
// notes overlap. The part then plays the track's values, and the loop's end leaves CC1 30, the depth of the game's
// note 60 in the later passes. A repeat of the first pass would play note 60 with that CC1 and no pan LFO in the
// SoundFont: the first pass wrote the note at depth 0. The loop starts at the second pass.
void TestPanLfoDepthKeptAtTheLoopEnd()
{
    const TestArchive test({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x00, 0x80, 0x18, 0x3C, 0x7F, 0x30, 0x80, 0x04, 0xCA, 0x1E,
                            0x80, 0x2C, 0x40, 0x7F, 0x06, 0x43, 0x7F, 0x06, 0x80, 0x04, 0x94, 0x08, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 76, 128),
          "a pan LFO's depth that the loop's end leaves in CC1: the loop starts at the second pass");
}

// A depth that the intro sets and the loop's end takes away. The pass test reads the depth that each part stands for,
// and the depth counts at each strike of a track whose LFO moves the pan.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 30; 6: a wait of 24; then the loop: note 60 for 12 ticks,
// a wait of 24, note 60 for 12, a wait of 24, a depth of 0, a wait of 12 and a jump back. The first pass's notes
// follow the LFO at depth 30, and the later passes' notes play at depth 0, with CC1 0 in both. The loop starts at the
// second pass.
void TestPanLfoDepthOfTheIntro()
{
    const TestArchive test({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x1E, 0x80, 0x18, 0x3C, 0x7F, 0x0C, 0x80, 0x18,
                            0x3C, 0x7F, 0x0C, 0x80, 0x18, 0xCA, 0x00, 0x80, 0x0C, 0x94, 0x08, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 84, 144), "a depth that the intro sets: the loop starts at the second pass");
}

// A part sends the track's values again after it has followed a pan LFO, and a repeat plays a note at the values that
// the pass sent.
//
// 0: no note waits; 2: a pan of 102; 4: the LFO moves the pan; 6: a depth of 30; 8: a release of 122; 10: note 60 for
// 6 ticks; 13: a wait of 12; then the loop: a depth of 30, note A, 60 for 6 ticks, a wait of 22, note B, 64 for 6, a
// wait of 36, a pan of 97, a wait of 2 and a jump back. The intro's depth already is 30. The loop's depth command
// changes nothing, and A shares the intro note's part. In the first pass, A strikes over the intro note's tail and B
// over A's tail: the part follows A's tail in between, and B sends pan 102 again. In the later passes, A strikes alone
// and B plays at 97. A repeat of the first pass would keep 97 for A and play B at 102. The loop starts at the second
// pass.
void TestPanSentAgainAfterAPanLfo()
{
    const TestArchive test({0xC7, 0x00, 0xC0, 0x66, 0xCC, 0x02, 0xCA, 0x1E, 0xD3, 0x7A, 0x3C, 0x7F,
                            0x06, 0x80, 0x0C, 0xCA, 0x1E, 0x3C, 0x7F, 0x06, 0x80, 0x16, 0x40, 0x7F,
                            0x06, 0x80, 0x24, 0xC0, 0x61, 0x80, 0x02, 0x94, 0x0F, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 72, 132), "a pan sent again after a pan LFO: the loop starts at the second pass");
}

// Two notes play alike only if a bend that moves while both sound has the same bend range.
//
// 0: no note waits; 2: a bend range of 5; 4: a wait of 24; then the loop: note 60 for 48 ticks, a wait of 4, a bend
// of 32, a wait of 4, a bend of 0, a wait of 40, a bend range of 6 and a jump back. The first pass slides its note at
// range 5, and the later passes at range 6. The loop starts at the second pass. Without the slide, the loop starts at
// the first pass.
//
// A bend in the frame of the strike isn't a slide: the channel first sounds with the bend that the tick's commands
// leave. 0: no note waits; 2: a bend range of 5; 4: a bend of 32; 6: a wait of 24; then the loop: note 60 for 24
// ticks, a bend of 0 in the same tick, a wait of 48, a bend of 32, a bend range of 6 and a jump back. The loop starts
// at the first pass.
void TestSlideAtAnotherBendRange()
{
    const TestArchive slide({0xC7, 0x00, 0xC5, 0x05, 0x80, 0x18, 0x3C, 0x7F, 0x30, 0x80, 0x04, 0xC4, 0x20,
                             0x80, 0x04, 0xC4, 0x00, 0x80, 0x28, 0xC5, 0x06, 0x94, 0x06, 0x00, 0x00});
    const TestArchive still(
        {0xC7, 0x00, 0xC5, 0x05, 0x80, 0x18, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xC5, 0x06, 0x94, 0x06, 0x00, 0x00});
    const TestArchive at_strike({0xC7, 0x00, 0xC5, 0x05, 0xC4, 0x20, 0x80, 0x18, 0x3C, 0x7F, 0x18, 0xC4,
                                 0x00, 0x80, 0x30, 0xC4, 0x20, 0xC5, 0x06, 0x94, 0x08, 0x00, 0x00});

    const Performance p = Perform(slide.archive, {0});
    const Performance q = Perform(still.archive, {0});
    const Performance r = Perform(at_strike.archive, {0});

    Check(LoopsOn(p, 72, 120), "a slide at another bend range: the loop starts at the second pass");
    Check(LoopsOn(q, 24, 72), "no slide: the bend range doesn't count");
    Check(LoopsOn(r, 24, 72), "a bend in the frame of the strike isn't a slide");
}

// A bend in the tick after a note's length runs out doesn't slide the note, even where the two ticks run in one frame.
//
// 0: no note waits; 2: a tempo of 480; 5: a bend range of 5; 7: a wait of 24; then the loop: note 60 for 48 ticks, a
// wait of 4, a bend of 32, a wait of 4, a bend of 0, a wait of 40, note 60 for 8 ticks, a wait of 9, a bend of 32, a
// wait of 2, a bend of 0, a wait of 6, a bend range of 6 and a jump back. At a tempo of 480 every frame runs two ticks.
// A pass of 65 ticks moves the pairs by one tick each pass. The bend 9 ticks after the second note's strike shares a
// frame with the note's last tick in every other pass. The first pass slides its first note at range 5, and the later
// passes at range 6. The loop starts at the second pass.
void TestBendAfterANotesLength()
{
    const TestArchive test({0xC7, 0x00, 0xE1, 0xE0, 0x01, 0xC5, 0x05, 0x80, 0x18, 0x3C, 0x7F, 0x30, 0x80, 0x04,
                            0xC4, 0x20, 0x80, 0x04, 0xC4, 0x00, 0x80, 0x28, 0x3C, 0x7F, 0x08, 0x80, 0x09, 0xC4,
                            0x20, 0x80, 0x02, 0xC4, 0x00, 0x80, 0x06, 0xC5, 0x06, 0x94, 0x09, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 89, 154), "a bend after a note's length in the frame of its last tick: the second pass");
}

// The loop's start sends a part's pan again where the part stands for the same pan at the loop's end, on a part that
// doesn't take the track's new notes.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 30; 6: a sweep of 64; 9: a wait of 24; then the loop: note
// 45 for 72 ticks, note 50 for 72, a wait of 72 and a jump back. Both notes sweep. Note 50 takes another part, and
// the track's new notes go there. Note 45 strikes first, on a part that doesn't take them. Both notes follow the LFO.
// The loop's end leaves note 45's part at the LFO's last pan, where the game's next note starts at the track's pan.
// The loop starts at the first pass, and its first tick sends that part the track's pan.
void TestRestoreOnAnotherPart()
{
    const TestArchive test({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x1E, 0xE3, 0x40, 0x00, 0x80, 0x18, 0x2D,
                            0x7F, 0x48, 0x32, 0x7F, 0x48, 0x80, 0x48, 0x94, 0x0B, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});
    const std::vector<uint8_t> parts = NoteParts(p, 0);

    Check(LoopsOn(p, 24, 96), "a pan LFO on two parts: the loop starts at the first pass");
    Check(parts.size() >= 2 && parts[0] != parts[1], "the two notes take two parts");
    Check(!parts.empty() &&
              RepeatValue(p, 0, parts[0], EventKind::kPan) == ValueAtTick(p, 0, parts[0], EventKind::kPan, 24),
          "the loop's first tick sends the first note's part its pan");
}

// A note that strikes before the pass sends a value on its part plays, on a repeat, with the value of the loop's end.
// That value must be the one the game's note plays, and the note must not follow a pan LFO.
//
// 0: no note waits; 2: a pan of 40; 4: the LFO moves the pan; 6: a depth of 0; 8: a release of 122; 10: note 60 for
// 16 ticks; 13: a wait of 12; then the loop: a depth of 30, note 64 for 6 ticks, a wait of 32, a pan of 90, a wait of
// 2, note 67 for 6, a wait of 14 and a jump back. Note 64 strikes beside the held note 60, and the first pass sends no
// pan before it. Note 67 strikes alone, and its tail still follows the LFO at the jump. The game's later passes start
// note 64 at pan 90, and a repeat of the first pass would start it at the LFO's last pan. The loop starts at the
// second pass.
void TestKeptPanOfAPanLfo()
{
    const TestArchive test({0xC7, 0x00, 0xC0, 0x28, 0xCC, 0x02, 0xCA, 0x00, 0xD3, 0x7A, 0x3C, 0x7F,
                            0x10, 0x80, 0x0C, 0xCA, 0x1E, 0x40, 0x7F, 0x06, 0x80, 0x20, 0xC0, 0x5A,
                            0x80, 0x02, 0x43, 0x7F, 0x06, 0x80, 0x0E, 0x94, 0x0F, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 60, 108), "a kept pan of a pan LFO: the loop starts at the second pass");
}

// A depth that the first pass doesn't send before a note, where the loop's end leaves another depth.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 0; 6: a wait of 12; then the loop: a wait of 6, a depth of
// 0, note 60 for 6 ticks, a wait of 36, a depth of 30, a wait of 6 and a jump back. The first pass's depth command
// changes nothing. The pass sends no CC1, and a repeat would start the note with CC1 30. The game's later passes
// play it at depth 0. The loop starts at the second pass.
//
// With the depth command in the loop's first tick, the game sets the depth back at each pass's start. The loop starts
// at the first pass, and its first tick sends CC1 0.
void TestKeptPanLfoDepth()
{
    const TestArchive late({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x00, 0x80, 0x0C, 0x80, 0x06, 0xCA, 0x00, 0x3C,
                            0x7F, 0x06, 0x80, 0x24, 0xCA, 0x1E, 0x80, 0x06, 0x94, 0x08, 0x00, 0x00});
    const TestArchive first_tick({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x00, 0x80, 0x0C, 0xCA, 0x00, 0x80, 0x06, 0x3C,
                                  0x7F, 0x06, 0x80, 0x24, 0xCA, 0x1E, 0x80, 0x06, 0x94, 0x08, 0x00, 0x00});

    const Performance p = Perform(late.archive, {0});
    const Performance q = Perform(first_tick.archive, {0});

    Check(LoopsOn(p, 60, 108),
          "a kept depth that the game sets back later in the pass: the loop starts at the second pass");
    Check(LoopsOn(q, 12, 60) && RepeatValue(q, 0, 0, EventKind::kModulation) == 0,
          "a depth that the game sets back at the pass's start: the loop starts at the first pass, and sends CC1 0");
}

// A kept value that equals the raw value at the pass's start must still be the one the game's note plays.
//
// 0: no note waits; 2: the LFO moves the pan; 4: a depth of 0; 6: a pan of 61; 8: a wait of 12; then the loop: a
// release of 127, note 60 for 6 ticks, a wait of 12, a pan of 70, a depth of 30, a release of 122, note 60 for 6, a
// wait of 10, a depth of 0 and a jump back. The second note strikes alone, and its tail still follows the LFO at the
// jump. The LFO's last CC10 there equals the CC10 of the intro's pan of 61. The game's later passes start the first
// note at pan 70, and a repeat of the first pass would start it at the LFO's last pan. The loop starts at the second
// pass.
void TestKeptValueEqualToTheStart()
{
    const TestArchive test({0xC7, 0x00, 0xCC, 0x02, 0xCA, 0x00, 0xC0, 0x3D, 0x80, 0x0C, 0xD3, 0x7F,
                            0x3C, 0x7F, 0x06, 0x80, 0x0C, 0xC0, 0x46, 0xCA, 0x1E, 0xD3, 0x7A, 0x3C,
                            0x7F, 0x06, 0x80, 0x0A, 0xCA, 0x00, 0x94, 0x0A, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 34, 56), "a kept pan equal to the start's: the loop starts at the second pass");
}

// The pass test reads the settings of one time ahead of its other events. Each pass strikes a note at a pitch of -1
// semitone, and the pitch goes back to 0 later in the pass. The recorder puts the second pass's pitch after its NoteOn,
// behind a CC120 of another part. A player applies the settings of one time together, before it plays the notes. The
// file therefore plays the three passes alike.
void TestSettingAfterTheNoteOnOfItsTime()
{
    seqlib::midi::Performance perf;
    perf.tempo = {{0, 120.0}};
    for (uint32_t pass = 0; pass < 3; pass++)
    {
        std::vector<seqlib::midi::TrackEvent>& events = perf.tracks[0];
        const uint32_t tick = 100 * pass + 10;
        const seqlib::midi::TrackEvent pitch = {.tick = tick, .kind = EventKind::kPitch, .semitones = -1.0f};
        const seqlib::midi::TrackEvent note = {
            .tick = tick, .kind = EventKind::kNoteOn, .key = 60, .value = 100, .note = pass + 1};
        if (pass == 1)
        {
            events.push_back(note);
            events.push_back({.tick = tick, .kind = EventKind::kSoundOff, .part = 1});
            events.push_back(pitch);
        }
        else
        {
            events.push_back(pitch);
            events.push_back(note);
        }
        events.push_back({.tick = tick + 50, .kind = EventKind::kPitch});
        events.push_back({.tick = tick + 60, .kind = EventKind::kNoteOff, .key = 60, .note = pass + 1});
    }
    perf.end_tick = 300;

    const seqlib::midi::Performance test = TestList(perf, {});

    Check(!ntdlib::sequence_export::RepeatsAsTheGame(perf, {}, 0, 100, {}),
          "a pitch recorded after the NoteOn of its time: the recorded order fails the first pass");
    Check(ntdlib::sequence_export::RepeatsAsTheGame(test, {}, 0, 100, {}) &&
              ntdlib::sequence_export::RepeatsAsTheGame(test, {}, 100, 200, {}),
          "a pitch recorded after the NoteOn of its time: in the order that the file plays, the passes repeat");
}

// A pitch between two ticks stays after a NoteOn of its tick. In the first pass the note strikes at pitch 0 and the
// pitch moves to -1 half a tick later. In the second the pitch moves a tick later. Both notes strike at pitch 0, and
// the passes repeat.
void TestSettingBetweenTicksAfterANoteOn()
{
    seqlib::midi::Performance perf;
    perf.tempo = {{0, 120.0}};
    std::vector<seqlib::midi::TrackEvent>& events = perf.tracks[0];
    for (uint32_t pass = 0; pass < 2; pass++)
    {
        const uint32_t tick = 100 * pass + 10;
        events.push_back({.tick = tick, .kind = EventKind::kNoteOn, .key = 60, .value = 100, .note = pass + 1});
        if (pass == 0)
        {
            events.push_back({.tick = tick, .kind = EventKind::kPitch, .semitones = -1.0f, .after = 0.5});
        }
        else
        {
            events.push_back({.tick = tick + 1, .kind = EventKind::kPitch, .semitones = -1.0f});
        }
        events.push_back({.tick = tick + 50, .kind = EventKind::kPitch});
        events.push_back({.tick = tick + 60, .kind = EventKind::kNoteOff, .key = 60, .note = pass + 1});
    }
    perf.end_tick = 200;

    Check(ntdlib::sequence_export::RepeatsAsTheGame(TestList(perf, {}), {}, 0, 100, {}),
          "a pitch half a tick after a NoteOn: the note strikes at the pitch before it");
}

// A program change counts for the NoteOns after it in its tick. 0: no note waits; 2: program 0; 4: note 60 for 12
// ticks; 7: a wait of 24; then the loop: program 0, note 62 for 12, program 1, note 64 for 12, a wait of 48 and a jump
// back. The game plays note 62 with program 0 in every pass. At tick 24 the first pass sends no program 0 (the track
// has it), and the list holds note 62, program 1, note 64. A repeat of that pass plays note 62 with the loop end's
// program 1. The second pass sends program 0 first, and the loop starts there.
void TestProgramBeforeTheNotesAfterIt()
{
    const TestArchive test({0xC7, 0x00, 0x81, 0x00, 0x3C, 0x7F, 0x0C, 0x80, 0x18, 0x81, 0x00, 0x3E, 0x7F,
                            0x0C, 0x81, 0x01, 0x40, 0x7F, 0x0C, 0x80, 0x30, 0x94, 0x09, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 72, 120), "a program change between two NoteOns of a tick: the loop starts at the second pass");
}

// A run on fewer parts tests the first run's loop again on its parts. Tracks 1 to 15 each play one note, and track 0
// requires a second part: 16 tracks and 17 parts, one more than MIDI's channels. The run on 16 parts gives track 0 one
// part.
//
// Track 0: 0: no note waits; 2: program 0; 4: note 60 for 48 ticks; 7: a wait of 12; then the loop: program 0, a sweep
// of 64, a wait of 6, note 62 for 6, a sweep of 0, a wait of 6, program 1, note 64 for 6, a wait of 48 and a jump back.
// In the first pass, note 62 sweeps while note 60 still sounds. On two parts it goes to the second part. That part
// takes program 0 there. On one part it keeps program 0 without an event. The loop's end leaves program 1, and the
// second pass sends program 0 with note 62. On one part, a repeat of the first pass plays note 62 with program 1. The
// loop starts at the second pass.
void TestLoopChosenAgainOnFewerParts()
{
    Bytes bytecode = {0xFE, 0xFF, 0xFF};
    for (uint8_t track = 1; track < 16; track++)
    {
        bytecode.insert(bytecode.end(), {0x93, track, 0x71, 0x00, 0x00});
    }
    bytecode.insert(bytecode.end(), {0xC7, 0x00, 0x81, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x0C, 0x81, 0x00, 0xE3, 0x40,
                                     0x00, 0x80, 0x06, 0x3E, 0x7F, 0x06, 0xE3, 0x00, 0x00, 0x80, 0x06, 0x81, 0x01,
                                     0x40, 0x7F, 0x06, 0x80, 0x30, 0x94, 0x57, 0x00, 0x00, 0x3C, 0x7F, 0x01, 0xFF});
    const TestArchive test(bytecode);

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 72, 132), "a pass that repeats only on the first run's parts: the loop starts at the second pass");
}

// A pass that only a probe of over kMaxSeconds can test. The tempo of 1 gives a tick every 240 frames, about 1.25 s. 0:
// no note waits; 2: a tempo of 1; 5: variable 0 to 1; then the loop: whether variable 0 is 1, if so a pan of 32,
// variable 0 to 0, note 60 for 6 ticks, a wait of 6, a pan of 96, a wait of 258 ticks and a jump back: 264 ticks, about
// 330 s, a pass. The first pass sends pan 32 before its note, and a repeat of it would too. The later passes strike at
// the pan of 96 that the pass before leaves. Pass 2 is tested against pass 3. Pass 3 ends about 990 s in. The loop
// starts at the second pass.
//
// With a pass of 400 ticks (about 500 s), pass 2 would end past kMaxSeconds. The performance doesn't play on to it,
// and keeps the first pass.
void TestPassTestedPastTheFileLimit()
{
    const TestArchive slow({0xC7, 0x00, 0xE1, 0x01, 0x00, 0xB0, 0x00, 0x01, 0x00, 0xB8, 0x00, 0x01,
                            0x00, 0xA2, 0xC0, 0x20, 0xB0, 0x00, 0x00, 0x00, 0x3C, 0x7F, 0x06, 0x80,
                            0x06, 0xC0, 0x60, 0x80, 0x82, 0x02, 0x94, 0x09, 0x00, 0x00});
    const Performance p = Perform(slow.archive, {0});
    Check(LoopsOn(p, 264, 528), "a pass of 330 s: the probe tests pass 2, and the loop starts there");

    const TestArchive slower({0xC7, 0x00, 0xE1, 0x01, 0x00, 0xB0, 0x00, 0x01, 0x00, 0xB8, 0x00, 0x01,
                              0x00, 0xA2, 0xC0, 0x20, 0xB0, 0x00, 0x00, 0x00, 0x3C, 0x7F, 0x06, 0x80,
                              0x06, 0xC0, 0x60, 0x80, 0x83, 0x0A, 0x94, 0x09, 0x00, 0x00});
    const Performance q = Perform(slower.archive, {0});
    Check(LoopsOn(q, 0, 400) && !q.truncated, "a pass of 500 s: the performance keeps the first pass within the limit");
}

// A loop that settles in its third pass. 0: no note waits; 2: variable 0 to 0; then the loop: variable 0 up by 1; if
// it is 1, a pan of 32; if it is 2, a pan of 48; note 60 for 6 ticks, a wait of 6, a pan of 96, a wait of 994 ticks
// and a jump back. Passes 1 and 2 send their pans before the note, and from pass 3 on the note strikes at the pan of
// 96 that the pass before leaves. The loop starts at the third pass.
void TestLoopSettlesInPassThree()
{
    const TestArchive test({0xC7, 0x00, 0xB0, 0x00, 0x00, 0x00, 0xB1, 0x00, 0x01, 0x00, 0xB8, 0x00, 0x01,
                            0x00, 0xA2, 0xC0, 0x20, 0xB8, 0x00, 0x02, 0x00, 0xA2, 0xC0, 0x30, 0x3C, 0x7F,
                            0x06, 0x80, 0x06, 0xC0, 0x60, 0x80, 0x87, 0x62, 0x94, 0x06, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 2000, 3000), "a loop that settles in its third pass: the loop starts there");
}

// A loop whose passes repeat in a period of three. 0: no note waits; 2: variable 0 to 0; then the loop: variable 0 up
// by 1; if it is 1, a pan of 32; if 2, a pan of 48; if 3, a pan of 64 and variable 0 back to 0; note 60 for 6 ticks, a
// wait of 1000 ticks and a jump back. No pass plays as the next. Passes 1 to 3 play as passes 4 to 6: the loop holds
// the three.
void TestLoopOfThreePasses()
{
    const TestArchive test({0xC7, 0x00, 0xB0, 0x00, 0x00, 0x00, 0xB1, 0x00, 0x01, 0x00, 0xB8, 0x00,
                            0x01, 0x00, 0xA2, 0xC0, 0x20, 0xB8, 0x00, 0x02, 0x00, 0xA2, 0xC0, 0x30,
                            0xB8, 0x00, 0x03, 0x00, 0xA2, 0xC0, 0x40, 0xA2, 0xB0, 0x00, 0x00, 0x00,
                            0x3C, 0x7F, 0x06, 0x80, 0x87, 0x68, 0x94, 0x06, 0x00, 0x00});

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 0, 3000), "passes that repeat in a period of three: the loop holds three passes");
}

// A player of one channel, and a note that gets no channel in the first pass only. 0: alloc tracks 0 and 1; 3: open
// track 1 at 25; 8: wait 24; then the loop: 10: note 60 for 6 ticks; 13: wait 6; 15: note 62 for 6 ticks; 18: wait
// 982; 21: jump back to 10. 25: track 1 at priority 127; 27: note 72 for 30 ticks; 30: end. Note 72 holds the channel
// at tick 24, and track 0's note 60 gets none there. From pass 2 on, every note gets the channel. Pass 1 repeats only
// with note 60 left out, and pass 2 repeats exactly: the loop starts at pass 2, with no approximation.
void TestLoopOverANoteWithoutAChannel()
{
    const TestArchive test(
        {0xFE, 0x03, 0x00, 0x93, 0x01, 0x19, 0x00, 0x00, 0x80, 0x18, 0x3C, 0x7F, 0x06, 0x80, 0x06, 0x3E,
         0x7F, 0x06, 0x80, 0x87, 0x56, 0x94, 0x0A, 0x00, 0x00, 0xC6, 0x7F, 0x48, 0x7F, 0x1E, 0xFF},
        0x0001);

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 1024, 2024) && p.approximations.empty(),
          "a pass that repeats exactly wins over pass 1, which repeats only with note 60 left out");
}

// A player of one channel, and a note that gets no channel from the second pass on. 0: alloc tracks 0 and 1; 3: open
// track 1 at 27; 8: wait 24; then track 0's loop: 10: note 60 for 6 ticks; 13: wait 6; 15: a pan of 32; 17: note 62
// for 6 ticks; 20: wait 982; 23: jump back to 10. 27: track 1 at priority 127; 29: no note waits; 31: wait 24; then its
// loop: 33: wait 994; 36: note 72 for 12 ticks; 39: wait 6; 41: jump back to 33. Note 72 holds the channel across the
// start of each later pass, and note 60 gets none there. Pass 1's note 60 strikes at the pan of 64, before the pass
// sends its pan, and a repeat would play it at the loop end's pan of 32. Pass 1 repeats only with that note left out,
// and pass 2 repeats exactly: the loop starts at pass 2.
void TestLoopOverANoteDroppedInTheNextPass()
{
    const TestArchive test({0xFE, 0x03, 0x00, 0x93, 0x01, 0x1B, 0x00, 0x00, 0x80, 0x18, 0x3C, 0x7F, 0x06, 0x80, 0x06,
                            0xC0, 0x20, 0x3E, 0x7F, 0x06, 0x80, 0x87, 0x56, 0x94, 0x0A, 0x00, 0x00, 0xC6, 0x7F, 0xC7,
                            0x00, 0x80, 0x18, 0x80, 0x87, 0x62, 0x48, 0x7F, 0x0C, 0x80, 0x06, 0x94, 0x21, 0x00, 0x00},
                           0x0001);

    const Performance p = Perform(test.archive, {0});

    Check(LoopsOn(p, 1024, 2024),
          "a pass that repeats exactly wins over pass 1, which repeats only with note 60 left out");
}

// A player of one channel, where track 1's note holds the channel for another length in each pass, in a period of five
// passes. 0: alloc tracks 0 and 1; 3: open track 1 at 52; 8: variable 1 to 1; then track 0's loop: 12: if variable 1
// is 1, a pan of 32; 19: variable 1 to 0; 23: wait 6; 25 to 42: note 60 for 6 ticks, six times; 43: a pan of 96; 45:
// wait 958; 48: jump back to 12. 52: track 1 at priority 127; 54: no note waits; 56: variable 0 to 6; then its loop:
// 60: variable 0 up by 6; 64 and 68: back to 12 once it reaches 42; 73: note 72 for variable 0 ticks; 77: wait 1000;
// 80: jump back to 60. Note 72 lasts 12, 18, 24, 30 and 36 ticks in turn, and each pass drops another count of track
// 0's notes. No pass or block repeats exactly. Pass 1 plays its notes at the pan of 32. Pass 2 repeats as pass 3 with
// the notes that get no channel in one of them left out: the loop starts at pass 2, and the file counts them.
void TestLoopWhereEveryPassDropsOtherNotes()
{
    const TestArchive test(
        {0xFE, 0x03, 0x00, 0x93, 0x01, 0x34, 0x00, 0x00, 0xB0, 0x01, 0x01, 0x00, 0xB8, 0x01, 0x01, 0x00, 0xA2,
         0xC0, 0x20, 0xB0, 0x01, 0x00, 0x00, 0x80, 0x06, 0x3C, 0x7F, 0x06, 0x3C, 0x7F, 0x06, 0x3C, 0x7F, 0x06,
         0x3C, 0x7F, 0x06, 0x3C, 0x7F, 0x06, 0x3C, 0x7F, 0x06, 0xC0, 0x60, 0x80, 0x87, 0x3E, 0x94, 0x0C, 0x00,
         0x00, 0xC6, 0x7F, 0xC7, 0x00, 0xB0, 0x00, 0x06, 0x00, 0xB1, 0x00, 0x06, 0x00, 0xB8, 0x00, 0x2A, 0x00,
         0xA2, 0xB0, 0x00, 0x0C, 0x00, 0xA1, 0x48, 0x7F, 0x00, 0x80, 0x87, 0x68, 0x94, 0x3C, 0x00, 0x00},
        0x0001);

    const Performance p = Perform(test.archive, {0});
    const auto approximation =
        p.approximations.find("note that gets no channel in one pass and plays in the next (repeats play the loop's)");

    Check(LoopsOn(p, 1000, 2000), "passes that each drop other notes: the loop starts at pass 2");
    Check(approximation != p.approximations.end() && approximation->second > 0,
          "the file counts the notes left out as an approximation");
}

// KeptValuesRepeat with a note that strikes at tick 100, before the pass sends its pan of 32 at 110. A repeat plays
// it at the loop end's pan of 32, and the game's twin at tick 200 got no channel. The note counts only with `excuse`,
// and only where the twin has its key.
void TestKeptValueOverANoteWithoutAChannel()
{
    seqlib::midi::Performance perf;
    perf.tempo = {{0, 120.0}};
    std::vector<seqlib::midi::TrackEvent>& events = perf.tracks[0];
    events.push_back({.tick = 0, .kind = EventKind::kPan, .value = 64});
    events.push_back({.tick = 100, .kind = EventKind::kNoteOn, .key = 60, .value = 100, .note = 1});
    events.push_back({.tick = 106, .kind = EventKind::kNoteOff, .key = 60, .note = 1});
    events.push_back({.tick = 110, .kind = EventKind::kPan, .value = 32});
    events.push_back({.tick = 210, .kind = EventKind::kPan, .value = 32});
    perf.end_tick = 300;
    const seqlib::midi::Performance test = TestList(perf, {});
    Strikes dropped;
    dropped.unstarted.insert({0, 200, 60});
    Strikes other_key;
    other_key.unstarted.insert({0, 200, 61});

    Check(!KeptValuesRepeat(perf, test, {}, dropped, 100, 200, false), "without the excuse, the note counts");
    Check(KeptValuesRepeat(perf, test, {}, dropped, 100, 200, true),
          "with it, a note whose twin got no channel doesn't");
    Check(!KeptValuesRepeat(perf, test, {}, other_key, 100, 200, true), "a twin of another key leaves the note in");
}

} // namespace

int main()
{
    TestTiedSweeps();
    TestSweepUnderAnotherNote();
    TestTimerPitchAfterRelease();
    TestLoopOverABentNote();
    TestLoopOnASettledPass();
    TestLoopOverAReleaseTail();
    TestPanLfoDepthSetAfterTheFirstStrike();
    TestPanLfoDepthKeptAtTheLoopEnd();
    TestPanLfoDepthOfTheIntro();
    TestPanSentAgainAfterAPanLfo();
    TestSlideAtAnotherBendRange();
    TestBendAfterANotesLength();
    TestRestoreOnAnotherPart();
    TestKeptPanOfAPanLfo();
    TestKeptPanLfoDepth();
    TestKeptValueEqualToTheStart();
    TestSettingAfterTheNoteOnOfItsTime();
    TestSettingBetweenTicksAfterANoteOn();
    TestProgramBeforeTheNotesAfterIt();
    TestLoopChosenAgainOnFewerParts();
    TestPassTestedPastTheFileLimit();
    TestLoopSettlesInPassThree();
    TestLoopOfThreePasses();
    TestLoopOverANoteWithoutAChannel();
    TestLoopOverANoteDroppedInTheNextPass();
    TestLoopWhereEveryPassDropsOtherNotes();
    TestKeptValueOverANoteWithoutAChannel();

    return xsflib::testing::Summary("DS conversion: loops and parts");
}
