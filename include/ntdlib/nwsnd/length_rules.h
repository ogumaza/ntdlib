// SPDX-License-Identifier: MIT

// The rules of a rip tool's length analysis that differ between the console policies (see console.h).

#pragma once

namespace ntdlib::nwsnd
{

// The rules of a rip tool's length analysis that differ between the policies (see sequence.h). Each policy gives them
// as kLengthRules. A user's policy can give others. The Wii U, the Switch and the 3DS take the defaults.
struct LengthRules
{
    // A jump back inside a subroutine is a loop while the track holds a note that sounds until the game stops it. With
    // this rule, it's a loop too when the track has run no conditional command since the jump's target.
    bool jump_loops_without_condition = true;

    // A note while the damper is on isn't a note whose length runs out (SequenceTrack::HasEndingNote).
    bool damper_holds_notes = true;

    // A timebase of 0 stops the sequence like a tempo of 0 (SequenceSoundPlayer::IsSettled and IsStuck).
    bool timebase_zero_stops = true;

    // When no track is open, every open track passes a test (SequenceSoundPlayer::EveryOpenTrack). Without this rule,
    // none does. The sequence then isn't settled or stuck, and it moves.
    bool no_track_passes = true;

    // A pass that ends with a loop is marked quiet after the loop's report (SequenceTrack::CountLoop). Without this
    // rule, the report reads the new mark.
    bool quiet_after_report = true;
};

} // namespace ntdlib::nwsnd
