// SPDX-License-Identifier: MIT

// What CC10, CC11 and CC1 stand for, for the pass test of the DS performer (performance.cpp). On a part that sounds one
// note under a pan LFO, the three controllers follow the LFO frame by frame (SendPanLfos). Elsewhere their values are
// the track's pan, level and LFO depth. That second meaning is the stand. The performer records the stands as events in
// a separate list. The pass test reads the stands in place of the controllers' values (TestList). It then checks the
// values that a MIDI player keeps from the loop's end (KeptValuesRepeat).

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sequence_export/passes.h"

namespace ntdlib::sequence_export::ntr
{

// What CC10, CC11 and CC1 stand for on the parts of each track, for the pass test: events of kPan, kExpression and
// kModulation, by track, in the order recorded.
using StandLists = std::array<std::vector<seqlib::midi::TrackEvent>, ntdlib::ntr::snd::kTracksPerPlayer>;

// What the performer records at each strike, for the pass test.
struct Strikes
{
    std::map<std::pair<int, uint32_t>, std::array<uint8_t, 3>> stands; // by track and tick: CC10, CC11, CC1
    std::set<std::tuple<int, uint32_t, uint8_t>> follows;              // track, tick and part of a follow strike
    std::map<std::pair<int, uint32_t>, int> slides;                    // by track and tick: the slide range
    std::set<std::tuple<int, uint32_t, uint8_t>> unstarted;            // track, tick and key of a note with no channel
};

// kPan, kExpression or kModulation.
bool IsLfoSetting(seqlib::midi::EventKind kind);

// `perf` as the pass test reads it: without its events of CC10, CC11 and CC1, and with the stands of `stands` in their
// place. Within a tick, the stands sent at the tick's start come first, then the performance's events, then the other
// stands. The performance's events go by time, and at one time the settings come first (seqlib::midi::IsSetting).
// The other events keep their recorded order.
seqlib::midi::Performance TestList(const seqlib::midi::Performance& perf, const StandLists& stands);

// Whether a repeat of the loop that ends at `end` should start `kind` on `part` of `track` with the loop start's value
// again, by the stands of `test`: the part's stand at the loop's end is its stand at the start (`at_start`), or the
// part takes the track's new notes and the game's next pass (`next`, null without one) sets the stand back.
bool StandReturns(const seqlib::midi::Performance& test, const ntdlib::sequence_export::PassStart& at_start,
                  const ntdlib::sequence_export::PassStart* next, uint32_t end, int track, uint8_t part,
                  seqlib::midi::EventKind kind);

// Whether the values of CC10, CC11 and CC1 that a MIDI player keeps from the end of the loop from `start` to `end`
// play the game's notes of the next pass. `played` is the performance, `test` its test list (TestList). A note that
// strikes before the pass sends the controller on its part plays with the loop end's value, unless the loop's start
// sends the start value again (StandReturns). That value must be the stand of the game's note, and the note must not
// follow a pan LFO. A note that follows one passes where the value is the start's raw value: it then starts as in the
// first play. With `excuse`, a note whose twin in the next pass got no channel (Strikes::unstarted) counts as no note.
bool KeptValuesRepeat(const seqlib::midi::Performance& played, const seqlib::midi::Performance& test,
                      const std::vector<ntdlib::sequence_export::PartSwitch>& switches, const Strikes& strikes,
                      uint32_t start, uint32_t end, bool excuse);

} // namespace ntdlib::sequence_export::ntr
