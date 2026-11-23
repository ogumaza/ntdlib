// SPDX-License-Identifier: MIT

// The length analysis's cases for a rip tool of the 3DS's, the Wii U's or the Switch's sound archives: sequences that
// each take one of the analysis's paths, an archive that holds them in either byte order, and the timing that the
// analysis gives each one. A rip tool's test measures every case on its model of the sound library, and CheckCases
// checks the timings. The timings hold for sound frames of 3 to 5 ms.
//
// Every sequence starts with timebase 48 and tempo 120: 48 ticks take 500 ms. Program 0 plays a wave of 1 s that ends,
// and program 1 a wave of 0.1 s that loops. Both have the fastest attack, decay and release, a sustain at full volume
// and no hold. A note then settles at once. The comment on each case in Cases() says what its sequence does.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"
#include "xsflib/testing/timing.h"

#include "ntdlib/testing/byte_writer.h"
#include "ntdlib/testing/sar_files.h"

namespace ntdlib::testing::length_cases
{

// A sequence's bytecode in an archive's byte order. A command's 16- and 24-bit arguments follow the byte order. The
// sequence player reads them that way. Lengths, waits and programs are variable-length numbers. Offsets count from the
// start of the bytecode. The timebase and tempo come first.
class Code
{
public:
    explicit Code(bool big_endian) : bytes_(big_endian)
    {
        Byte(0xB0).Byte(48); // timebase 48
        Byte(0xE1).U16(120); // tempo 120
    }

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_.Data();
    }

    Code& Note(uint8_t key, uint8_t velocity, uint32_t length)
    {
        return Byte(key).Byte(velocity).Var(length);
    }

    Code& Wait(uint32_t ticks)
    {
        return Byte(0x80).Var(ticks);
    }

    Code& Program(uint32_t program)
    {
        return Byte(0x81).Var(program);
    }

    Code& OpenTrack(uint8_t track, uint32_t offset)
    {
        return Byte(0x88).Byte(track).U24(offset);
    }

    Code& Jump(uint32_t offset)
    {
        return Byte(0x89).U24(offset);
    }

    Code& Call(uint32_t offset)
    {
        return Byte(0x8A).U24(offset);
    }

    // The next command runs only when the last comparison held.
    Code& If()
    {
        return Byte(0xA2);
    }

    Code& Volume(uint8_t volume)
    {
        return Byte(0xC1).Byte(volume);
    }

    // With note wait on, a note of length 0 waits until the track's notes have ended.
    Code& NoteWait(bool on)
    {
        return Byte(0xC7).Byte(on ? 1 : 0);
    }

    Code& Tie(bool on)
    {
        return Byte(0xC8).Byte(on ? 1 : 0);
    }

    // A loop that runs `count` times, or for ever when `count` is 0, up to LoopEnd.
    Code& LoopStart(uint8_t count)
    {
        return Byte(0xD4).Byte(count);
    }

    Code& Damper(uint8_t value)
    {
        return Byte(0xDF).Byte(value);
    }

    Code& SetVariable(uint8_t variable, uint16_t value)
    {
        return Byte(0xF0).Byte(0x80).Byte(variable).U16(value);
    }

    // A random set (the prefix 0xA0): the variable takes a number from `low` to `high`.
    Code& SetRandomVariable(uint8_t variable, uint16_t low, uint16_t high)
    {
        return Byte(0xA0).Byte(0xF0).Byte(0x80).Byte(variable).U16(low).U16(high);
    }

    Code& AddVariable(uint8_t variable, uint16_t value)
    {
        return Byte(0xF0).Byte(0x81).Byte(variable).U16(value);
    }

    Code& CompareEqual(uint8_t variable, uint16_t value)
    {
        return Byte(0xF0).Byte(0x90).Byte(variable).U16(value);
    }

    Code& CompareAtLeast(uint8_t variable, uint16_t value)
    {
        return Byte(0xF0).Byte(0x91).Byte(variable).U16(value);
    }

    Code& LoopEnd()
    {
        return Byte(0xFC);
    }

    Code& Return()
    {
        return Byte(0xFD);
    }

    Code& End()
    {
        return Byte(0xFF);
    }

private:
    Code& Byte(uint32_t value)
    {
        bytes_.U8(value);

        return *this;
    }

    // Seven bits a byte, the highest first. Each byte but the last has its top bit set.
    Code& Var(uint32_t value)
    {
        int shift = 28;
        while (shift > 0 && (value >> shift) == 0)
        {
            shift -= 7;
        }

        for (; shift > 0; shift -= 7)
        {
            Byte(0x80 | ((value >> shift) & 0x7F));
        }

        return Byte(value & 0x7F);
    }

    Code& U16(uint32_t value)
    {
        bytes_.U16(value);

        return *this;
    }

    Code& U24(uint32_t value)
    {
        bytes_.U24(value);

        return *this;
    }

    ByteWriter bytes_;
};

// A case: the sound's label, its sequence, where the sound starts in it, the sound's volume and the tracks it
// allocates, and the timing that the analysis gives it. The length is from min_ms to max_ms, or equals the length of
// case `same_length_as` when that's 0 or more. Where others_min_ms is set, an analysis that waits for the loops of the
// tracks other than the main track (a model with xsflib's OthersLooped) gives a length from others_min_ms to
// others_max_ms instead.
struct Case
{
    const char* name;
    void (*write)(Code& code);
    uint32_t start = 0;
    uint8_t volume = 127;
    uint32_t tracks = 1;
    long long min_ms = 0;
    long long max_ms = 0;
    long long fade_ms = 0;
    bool silent = false;
    int same_length_as = -1;
    long long others_min_ms = -1;
    long long others_max_ms = -1;
};

// Every case, by its sound's index in the archive. The comment on each case says what its sequence does, then why the
// analysis times it so.
inline const std::vector<Case>& Cases()
{
    static const std::vector<Case> kCases = {
        // A note of 24 ticks, then the end. The note lasts 250 ms with the fastest release, from the second frame. The
        // length ends with it. Under LengthRules::end_at_idle_start, the sequence can end at the engine's first idle
        // frame. Where the output runs a frame behind the engine, that's two frames before the last sound ends.
        {"SEQ_END", [](Code& c) { c.Note(60, 127, 24).End(); }, 0, 127, 1, 240, 280, 0, false},

        // A note of 24 ticks, played over and over by a jump back to it. Two loops of 24 ticks, the first starting in
        // the second frame, then the fade.
        {"SEQ_JUMP", [](Code& c) { c.Note(60, 127, 24).Jump(5).End(); }, 0, 127, 1, 495, 515, 7000},

        // The same note in a loop with a count of 0. Such a loop repeats for ever.
        {"SEQ_FOREVER", [](Code& c) { c.LoopStart(0).Note(60, 127, 24).LoopEnd().End(); }, 0, 127, 1, 495, 515, 7000},

        // A wait and the end, with no note. No sound: a length of 0.
        {"SEQ_SILENT", [](Code& c) { c.Wait(48).End(); }, 0, 127, 1, 0, 0, 0, true},

        // A held note of 9,600 ticks on program 1. Program 1's wave loops. The held note settles at once. The wave of
        // 0.1 s loops twice.
        {"SEQ_HELD_LOOP", [](Code& c) { c.Program(1).Note(60, 127, 9600).End(); }, 0, 127, 1, 200, 300, 7000},

        // A held note on program 0. The wave ends after 1 s. The sequence ends with it.
        {"SEQ_HELD_ONCE", [](Code& c) { c.Note(60, 127, 9600).End(); }, 0, 127, 1, 990, 1030, 0},

        // Starts after a note and its end, and jumps back to the note. The track hasn't played that note. The jump
        // isn't a loop, and the note plays once.
        {"SEQ_SHARED", [](Code& c) { c.Note(60, 127, 24).End().Jump(5); }, 9, 127, 1, 240, 280, 0},

        // A note at a track volume of 0. It makes no sound.
        {"SEQ_TRACK_MUTED", [](Code& c) { c.Volume(0).Note(60, 127, 24).End(); }, 0, 127, 1, 0, 0, 0, true},

        // A note in a sound whose volume is 0. It makes no sound either.
        {"SEQ_SOUND_MUTED", [](Code& c) { c.Note(60, 127, 24).End(); }, 0, 0, 1, 0, 0, 0, true},

        // Opens track 1 at 16 and waits a tick over and over. Meanwhile, track 1 waits 48 ticks and holds a note on
        // program 1. Track 0's loops of a tick change nothing, and don't count. The held note starts at 500 ms. Its
        // wave of 0.1 s loops twice, and then the fade starts.
        {"SEQ_LATE", [](Code& c) { c.OpenTrack(1, 16).Wait(1).Jump(10).Wait(48).Program(1).Note(60, 127, 9600).End(); },
         0, 127, 3, 690, 730, 7000},

        // Track 0 opens track 1 at 27, then polls variable 0 once a tick: it compares the variable with 1, would jump
        // to its end if they were equal, and waits a tick. Track 1 plays a note of 24 ticks over and over. Track 0 only
        // polls, and its passes don't count. Track 1's loops are the sequence's loops: two loops of 24 ticks.
        {"SEQ_POLL_MUSIC",
         [](Code& c)
         {
             c.OpenTrack(1, 27).CompareEqual(0, 1).If().Jump(26).Wait(1).Jump(10).End();
             c.Note(60, 127, 24).Jump(27).End();
         },
         0, 127, 3, 495, 515, 7000},

        // Track 0 polls as in SEQ_POLL_MUSIC. Track 1 ties a note of a tick on program 1 to itself over and over. The
        // note sounds on, and no track changes anything after the first ticks. The tied note's wave of 0.1 s loops
        // twice. On the 3DS, the first tie drops the region's volume from the note's channel. The sound changes once,
        // and the tie's pass isn't a loop.
        {"SEQ_POLL_HOLD",
         [](Code& c)
         {
             c.OpenTrack(1, 27).CompareEqual(0, 1).If().Jump(26).Wait(1).Jump(10).End();
             c.Program(1).Tie(true).Note(60, 127, 1).Jump(31).End();
         },
         0, 127, 3, 190, 260, 7000},

        // Track 0 polls as in SEQ_POLL_MUSIC. Track 1 plays a note of 12 ticks, rests for 48 ticks, plays the note
        // again and ends. The second note ends at 750 ms, and the length ends with it. Track 0's polls during the rest
        // aren't loops.
        {"SEQ_POLL_END",
         [](Code& c)
         {
             c.OpenTrack(1, 27).CompareEqual(0, 1).If().Jump(26).Wait(1).Jump(10).End();
             c.Note(60, 127, 12).Wait(48).Note(60, 127, 12).End();
         },
         0, 127, 3, 735, 785, 0},

        // Track 0 sets the track's volume to 100 at the start of each pass, then polls as in SEQ_POLL_MUSIC. Track 1,
        // at 29, plays a note of 24 ticks over and over. Track 0's first pass changes its volume, and counts as a loop.
        // Then track 1's loops take over, and the count starts again: two loops of 24 ticks.
        {"SEQ_POLL_MOVE",
         [](Code& c)
         {
             c.OpenTrack(1, 29).Volume(100).CompareEqual(0, 1).If().Jump(28).Wait(1).Jump(10).End();
             c.Note(60, 127, 24).Jump(29).End();
         },
         0, 127, 3, 495, 515, 7000},

        // Track 0 sets variable 1 to 1 and then to 2, 24 ticks apart, over and over. These are beats for the game. No
        // track reads them. Track 1, at 29, holds a note on program 1. The sequence is steady once track 0 has made a
        // pass, at 500 ms. By then the held note's wave has looped more than twice.
        {"SEQ_BEATS",
         [](Code& c)
         {
             c.OpenTrack(1, 29).SetVariable(1, 1).Wait(24).SetVariable(1, 2).Wait(24).Jump(10).End();
             c.Program(1).Note(60, 127, 9600).End();
         },
         0, 127, 3, 495, 530, 7000},

        // Track 0 counts in variable 2, once a tick, to 30. Then track 0 jumps to 31 and holds a note on program 1.
        // Track 1, at 38, holds another note on program 1 from the start. Track 0 isn't still while it counts. The
        // second held note starts at about 320 ms, two semitones up, and its wave loops twice in 180 ms.
        {"SEQ_POLL_COUNT",
         [](Code& c)
         {
             c.OpenTrack(1, 38).AddVariable(2, 1).CompareAtLeast(2, 30).If().Jump(31).Wait(1).Jump(10);
             c.Program(1).Note(62, 127, 9600).End();
             c.Program(1).Note(60, 127, 9600).End();
         },
         0, 127, 3, 495, 540, 7000},

        // Track 0 opens tracks 1 and 2, then waits 48 ticks and jumps back, over and over. Track 1, at 22, sets its
        // volume to 100 and to 90, 12 ticks apart, and jumps back. Track 2, at 35, plays a note of 24 ticks over and
        // over. Track 0's passes are idle. Track 1's passes change its volume but play no notes. Neither keeps track 2
        // from being the main track. Track 2's loops at 48 and 72 ticks count. At 24 ticks, track 0 had made no pass
        // yet.
        {"SEQ_QUIET",
         [](Code& c)
         {
             c.OpenTrack(1, 22).OpenTrack(2, 35).Wait(48).Jump(15).End();
             c.Volume(100).Wait(12).Volume(90).Wait(12).Jump(22).End();
             c.Note(60, 127, 24).Jump(35).End();
         },
         0, 127, 7, 745, 765, 7000},

        // Ties notes of 36 ticks on program 0 to one another over and over (from 10). The wave ends after 1 s, during
        // the third pass, and the next pass starts a new note at 1,125 ms. A track whose passes play notes isn't stuck.
        // That pass is the first loop. The pass that starts the note again at 2,250 ms is the second. On the 3DS, the
        // first tie after each new note drops the region's volume from the note's channel. The passes of those ties
        // aren't loops.
        {"SEQ_RESTART", [](Code& c) { c.Tie(true).Note(60, 127, 36).Note(60, 127, 36).Jump(10).End(); }, 0, 127, 1,
         2620, 2640, 7000},

        // Plays a note of 48 ticks on program 1 without waiting for it, then waits a tick and jumps back, over and over
        // (from 12). The note runs out at 500 ms, and the length ends with it. Its track polls, but isn't settled while
        // the note has length left. A model that ends a sequence at its output's last sound (LengthRules::
        // end_at_idle_start off) ends it later where the output runs behind the engine: 518 ms on the 3DS, whose DSP
        // output runs up to two frames behind.
        {"SEQ_ENDING", [](Code& c) { c.Program(1).NoteWait(false).Note(60, 127, 48).Wait(1).Jump(12).End(); }, 0, 127,
         1, 490, 525, 0},

        // Track 0 opens track 1 at 23, then sets its volume to 100 and to 90, 24 ticks apart, and jumps back (to 10),
        // over and over. Track 1 plays a note of 24 ticks over and over. Track 0's passes change its volume and play no
        // notes. Track 0 keeps track 1 from being the main track all the same. Otherwise the loops of both would count,
        // each starting the count again. Track 0's loops at 48 and 96 ticks are the sequence's loops.
        {"SEQ_CONDUCTOR",
         [](Code& c)
         {
             c.OpenTrack(1, 23).Volume(100).Wait(24).Volume(90).Wait(24).Jump(10).End();
             c.Note(60, 127, 24).Jump(23).End();
         },
         0, 127, 3, 995, 1015, 7000},

        // Plays a held note of 9,600 ticks on program 1 without waiting for it, then waits a tick and jumps back, over
        // and over (from 13). The game stops a held note before its length runs out. Its track polls and is settled,
        // and the note's wave of 0.1 s loops twice.
        {"SEQ_HELD_POLL", [](Code& c) { c.Program(1).NoteWait(false).Note(60, 127, 9600).Wait(1).Jump(13).End(); }, 0,
         127, 1, 200, 300, 7000},

        // Turns the damper on, plays a note of 48 ticks on program 1 without waiting for it, then waits a tick and
        // jumps back, over and over (from 14). The damper keeps the note on when its length runs out. The track is
        // settled. So is the track of SEQ_HELD_POLL.
        {"SEQ_DAMPER",
         [](Code& c) { c.Program(1).Damper(127).NoteWait(false).Note(60, 127, 48).Wait(1).Jump(14).End(); }, 0, 127, 1,
         200, 300, 7000},

        // Track 0 opens track 1 at 16, plays a note of length 0 on program 1 and ends. With note wait on, it waits for
        // the note to end, and the note never does. Track 1 plays a note of 24 ticks over and over. Track 0 waits for
        // ever, and doesn't keep track 1 from being the main track. Track 1's loops at 24 and 48 ticks count.
        {"SEQ_ENDLESS",
         [](Code& c) { c.OpenTrack(1, 16).Program(1).Note(60, 127, 0).End().Note(60, 127, 24).Jump(16).End(); }, 0, 127,
         3, 495, 515, 7000},

        // Track 0 opens track 1 at 17, holds a note of 9,600 ticks on program 1 and ends. Track 1 plays as in
        // SEQ_ENDLESS, and its loops count the same way.
        {"SEQ_HELD_UNDER",
         [](Code& c) { c.OpenTrack(1, 17).Program(1).Note(60, 127, 9600).End().Note(60, 127, 24).Jump(17).End(); }, 0,
         127, 3, 495, 515, 7000},

        // Track 0 opens track 1 at 25. With note wait off, it plays a note of 96 ticks on program 1. With note wait on,
        // it plays a note of length 0 on program 0 and waits for both notes to end. Program 0's wave ends after 1 s.
        // Then track 0 ends. Track 1 plays as in SEQ_ENDLESS. Until 1 s, track 0 keeps track 1 from being the main
        // track. Track 1's loops at 120 and 144 ticks count.
        {"SEQ_NOTES_END",
         [](Code& c)
         {
             c.OpenTrack(1, 25).NoteWait(false).Program(1).Note(60, 127, 96);
             c.NoteWait(true).Program(0).Note(60, 127, 0).End();
             c.Note(60, 127, 24).Jump(25).End();
         },
         0, 127, 3, 1495, 1515, 7000},

        // As SEQ_CONDUCTOR, but before its loop (from 17), track 0 plays a note of length 0 on program 1 with note wait
        // off. That note never ends, but track 0 doesn't wait for it. Track 0 therefore doesn't wait for ever. Track 1
        // is at 30. As in SEQ_CONDUCTOR, the sequence's loops are track 0's loops at 48 and 96 ticks.
        {"SEQ_ENDLESS_NO_WAIT",
         [](Code& c)
         {
             c.OpenTrack(1, 30).NoteWait(false).Program(1).Note(60, 127, 0);
             c.Volume(100).Wait(24).Volume(90).Wait(24).Jump(17).End();
             c.Note(60, 127, 24).Jump(30).End();
         },
         0, 127, 3, 995, 1015, 7000},

        // Track 0 calls a subroutine at 10. The subroutine plays a note of 24 ticks and jumps back to its start,
        // unconditionally. No conditional command runs in the subroutine, and nothing can leave its loop. The
        // subroutine's jumps back at 24 and 48 ticks count.
        {"SEQ_SUB_LOOP", [](Code& c) { c.Call(10).End().Note(60, 127, 24).Jump(10).End(); }, 0, 127, 1, 495, 515,
         7000},

        // Track 0 sets variable 0 to 0 and calls a subroutine at 15. The subroutine plays a note of 24 ticks and adds 1
        // to the variable. It returns once the variable is 3, and otherwise jumps back to its start. The subroutine's
        // passes run a conditional command, and don't count. The third note ends at 750 ms, and the length ends with
        // it.
        {"SEQ_SUB_COUNT",
         [](Code& c)
         {
             c.SetVariable(0, 0).Call(15).End();
             c.Note(60, 127, 24).AddVariable(0, 1).CompareEqual(0, 3).If().Return().Jump(15).End();
         },
         0, 127, 1, 735, 785, 0},

        // Track 0 opens track 1 at 17, then waits a tick and jumps back, over and over. Track 1 plays a note of length
        // 0 on program 1 without waiting for it. Then track 1 loops for ever. Each pass sets its volume to 100 and to
        // 90, 12 ticks apart, and plays no note. Track 0's passes are idle. Track 1's first pass follows its note and
        // ends at 24 ticks. The second pass plays no note and only changes the volume of the note of length 0. Two
        // loops of 24 ticks, then the fade.
        {"SEQ_ENDLESS_SWELL",
         [](Code& c)
         {
             c.OpenTrack(1, 17).Wait(1).Jump(10).End();
             c.NoteWait(false).Program(1).Note(60, 127, 0);
             c.LoopStart(0).Volume(100).Wait(12).Volume(90).Wait(12).LoopEnd().End();
         },
         0, 127, 3, 495, 515, 7000},

        // Track 0 opens track 1 at 17, then waits a tick and jumps back, over and over. Track 1 plays a note of 12
        // ticks. Then track 1 counts in variable 0 to 12, once a tick, in a loop from 25 that a conditional jump
        // leaves. After the count, the track jumps back to the note. Each pass of track 1 plays a note and then counts
        // ticks. The note counts for the pass. Track 1's loops at 24 and 48 ticks count.
        {"SEQ_INNER_COUNT",
         [](Code& c)
         {
             c.OpenTrack(1, 17).Wait(1).Jump(10).End();
             c.Note(60, 127, 12).SetVariable(0, 0).Wait(1).AddVariable(0, 1).CompareEqual(0, 12).If().Jump(46);
             c.Jump(25).Jump(17).End();
         },
         0, 127, 3, 495, 515, 7000},

        // A note of 24 ticks and a rest of 24 ticks. Then the track sets its volume to 100 and to 90, 12 ticks apart,
        // and jumps back, over and over (from 10). The note sounds as in SEQ_END. Then the track's loops make no sound.
        // A loop that went round without a sound ends the sequence at its last sound.
        {"SEQ_REST_LOOP",
         [](Code& c) { c.Note(60, 127, 24).Wait(24).Volume(100).Wait(12).Volume(90).Wait(12).Jump(10).End(); }, 0, 127,
         1, 240, 280, 0},

        // A note of 24 ticks, then a rest of 20,000 ticks (208 s), then the end. The note sounds as in SEQ_END. Then
        // the sequence stays busy without a sound for 180 s. The analysis ends the sequence where it fell silent.
        // SEQ_REST_LOOP and SEQ_LONG_REST end at the same frame. A different rule ends each one.
        {"SEQ_LONG_REST", [](Code& c) { c.Note(60, 127, 24).Wait(20000).End(); }, 0, 127, 1, 240, 280, 0, false, 30},

        // Track 0 opens track 1 at 11 and ends. Track 1 plays a note of 24 ticks, then counts 24 ticks in variable 0
        // with an inner loop that jumps back to 19. Then track 1 jumps back to the note, over and over. Track 1's
        // passes back to the note play the note, and the inner loop's passes play none. Track 1's loops at 48 and 96
        // ticks count.
        {"SEQ_INNER_LOOP",
         [](Code& c)
         {
             c.OpenTrack(1, 11).End();
             c.Note(60, 127, 24).SetVariable(0, 0).Wait(1).AddVariable(0, 1).CompareAtLeast(0, 24).If().Jump(40);
             c.Jump(19).Jump(11).End();
         },
         0, 127, 3, 995, 1015, 7000},

        // Track 0 opens track 1, then plays a note of 24 ticks and jumps back to it (from 10). Track 1 plays a note of
        // 60 ticks and jumps back to it (from 18). Neither loop's length divides the other's. Track 0's loops at 24 and
        // 48 ticks count, as in SEQ_JUMP. An analysis that waits for the other tracks' loops starts the fade at track
        // 1's second loop, at 120 ticks.
        {"SEQ_OTHER_LOOP",
         [](Code& c) { c.OpenTrack(1, 18).Note(60, 127, 24).Jump(10).End().Note(60, 127, 60).Jump(18).End(); }, 0, 127,
         3, 495, 515, 7000, false, -1, 1245, 1265},

        // Track 0 opens track 1 at 17, waits 48 ticks and jumps back to the open (to 5), over and over. Track 1 plays a
        // note of 24 ticks, waits 96 ticks and ends. Each pass of track 0 opens track 1 again before that wait runs
        // out. Track 1 never loops and never ends. Track 0's loops at 48 and 96 ticks are the sequence's loops. An
        // analysis that waits for the other tracks' loops gives the same length only if it counts each open of a
        // track after the first as a pass of that track. Otherwise it waits for track 1's loops until its limit. On
        // the Wii U and the Switch, an open clears the track's wait, and the track starts its part again at once.
        {"SEQ_REOPEN", [](Code& c) { c.OpenTrack(1, 17).Wait(48).Jump(5).End().Note(60, 127, 24).Wait(96).End(); }, 0,
         127, 3, 995, 1015, 7000},

        // Track 0 opens track 1 at 29 and calls a subroutine at 15. The subroutine polls variable 0 once a tick: it
        // compares the variable with 1, would end the track if they were equal, waits a tick and jumps back. Track 1
        // plays a note of 24 ticks over and over. The poll's passes could leave their loop, and they change nothing.
        // Track 1's loops are the sequence's loops: two loops of 24 ticks.
        {"SEQ_SUB_POLL_MUSIC",
         [](Code& c)
         {
             c.OpenTrack(1, 29).Call(15).End();
             c.CompareEqual(0, 1).If().End().Wait(1).Jump(15).Return();
             c.Note(60, 127, 24).Jump(29).End();
         },
         0, 127, 3, 495, 515, 7000},

        // Track 0 opens track 1 at 49. Then it plays a note of 24 ticks and calls a subroutine at 22 that counts 48
        // ticks in variable 0 and returns, and jumps back to the note (from 17), over and over. Track 1 plays a note of
        // 12 ticks over and over. With note wait on, each note waits for its length. A pass of track 0 therefore takes
        // 72 ticks. The count's passes change the variable, and they don't make track 0 idle. Track 0's loops at 72
        // and 144 ticks are the sequence's loops.
        {"SEQ_SUB_COUNT_MUSIC",
         [](Code& c)
         {
             c.OpenTrack(1, 49).Note(60, 127, 24).Call(22).Jump(10).End();
             c.SetVariable(0, 0).Wait(1).AddVariable(0, 1).CompareAtLeast(0, 48).If().Jump(48).Jump(27).Return();
             c.Note(62, 127, 12).Jump(49).End();
         },
         0, 127, 3, 1495, 1515, 7000},
    };

    return kCases;
}

// Whether the sequence player of the console of `flavor` reads a command's arguments big-endian: the 3DS's and the Wii
// U's do, and the Switch's doesn't.
inline bool BigEndianArguments(sar::Flavor flavor)
{
    return flavor != sar::Flavor::kNx;
}

// The sequence of case `index` in the byte order.
inline std::vector<uint8_t> Bytecode(std::size_t index, bool big_endian)
{
    Code code(big_endian);
    Cases()[index].write(code);

    return code.Bytes();
}

// The archive that holds every case, for the console of `flavor`: big-endian for the Wii U, and little-endian for the
// 3DS and the Switch. Its sound table lists the cases in order, and nothing more.
inline std::vector<uint8_t> Archive(sar::Flavor flavor)
{
    // A wave of 1 s that ends, and one of 0.1 s that loops. Program 0 plays the wave that ends, and program 1 the one
    // that loops.
    const bool big_endian = sar_files::BigEndian(flavor);
    const sar_files::WaveSpec once = sar_files::ConstantWave(big_endian);
    sar_files::WaveSpec looping = sar_files::ConstantWave(big_endian, 3200);
    looping.loop = true;

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {
        {sar_files::Bank(flavor, sar_files::OneRegionBank(2)), "", false},
        {sar_files::WaveArchive(flavor, {sar_files::Wave(flavor, once), sar_files::Wave(flavor, looping)}), "", false}};

    for (std::size_t i = 0; i < Cases().size(); ++i)
    {
        const Case& item = Cases()[i];
        sar_files::SoundSpec sound;
        sound.name = item.name;
        sound.volume = item.volume;
        sound.allocate_track_flags = item.tracks;
        sound.banks = {0};
        sound.file = static_cast<uint32_t>(spec.files.size());
        sound.start_offset = item.start;
        spec.sounds.push_back(sound);
        spec.files.push_back(
            {sar_files::Sequence(flavor, Bytecode(i, BigEndianArguments(flavor)), {{"Start", 0}}), "", false});
    }

    return sar_files::Archive(flavor, spec);
}

// The archive for the Wii U, or for the Switch.
inline std::vector<uint8_t> Archive(bool big_endian)
{
    return Archive(sar_files::FlavorOf(big_endian));
}

// Checks every case's timing. `analyze(index)` gives the timing of the archive's sound `index` (with length_ms, fade_ms
// and silent), or nothing. A sound past the last case has no timing. `waits_for_others` says whether the analysis waits
// for the other tracks' loops (see Case).
template <typename Analyze>
void CheckCases(const Analyze& analyze, bool waits_for_others = false)
{
    for (std::size_t i = 0; i < Cases().size(); ++i)
    {
        const Case& item = Cases()[i];
        const std::string name = item.name;
        const auto timing = analyze(static_cast<uint32_t>(i));
        long long min_ms = item.min_ms;
        long long max_ms = item.max_ms;
        if (waits_for_others && item.others_min_ms >= 0)
        {
            min_ms = item.others_min_ms;
            max_ms = item.others_max_ms;
        }

        if (item.same_length_as >= 0)
        {
            const auto other = analyze(static_cast<uint32_t>(item.same_length_as));
            min_ms = max_ms = other ? other->length_ms : -1;
        }

        xsflib::testing::CheckTiming(name, timing, min_ms, max_ms, item.fade_ms, item.silent);
    }

    xsflib::testing::Check(!analyze(static_cast<uint32_t>(Cases().size())).has_value(),
                           "a sound past the last has no length");
}

// The index of the case `name` in Cases(), or the count of the cases when no case has that name.
inline uint32_t CaseIndex(const std::string& name)
{
    uint32_t index = 0;
    while (index < Cases().size() && name != Cases()[index].name)
    {
        ++index;
    }

    return index;
}

// The length that `analyze(index)` gives the case `name` (see CheckCases), or -1 when it gives nothing or no case has
// that name.
template <typename Analyze>
long long LengthOf(const Analyze& analyze, const std::string& name)
{
    const uint32_t index = CaseIndex(name);
    if (index == Cases().size())
    {
        return -1;
    }

    const auto timing = analyze(index);

    return timing ? timing->length_ms : -1;
}

// An archive for the console of `flavor` with one sequence sound, `name`, of `code`'s commands, on the tracks in
// `tracks`. As in the cases, program 0 plays a wave of 1 s that ends.
inline std::vector<uint8_t> SequenceArchive(sar::Flavor flavor, const std::string& name, const Code& code,
                                            uint32_t tracks = 1)
{
    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    const std::vector<uint8_t> wave = sar_files::Wave(flavor, sar_files::ConstantWave(sar_files::BigEndian(flavor)));
    spec.files = {{sar_files::Bank(flavor, sar_files::OneRegionBank()), "", false},
                  {sar_files::WaveArchive(flavor, {wave}), "", false},
                  {sar_files::Sequence(flavor, code.Bytes(), {{"Start", 0}}), "", false}};
    sar_files::SoundSpec sound;
    sound.name = name;
    sound.volume = 127;
    sound.allocate_track_flags = tracks;
    sound.banks = {0};
    sound.file = 2;
    spec.sounds = {sound};

    return sar_files::Archive(flavor, spec);
}

// The archive for the Wii U, or for the Switch.
inline std::vector<uint8_t> SequenceArchive(bool big_endian, const std::string& name, const Code& code,
                                            uint32_t tracks = 1)
{
    return SequenceArchive(sar_files::FlavorOf(big_endian), name, code, tracks);
}

// SEQ_COUNTED_LOOP: a loop of 64 passes, each C4 for 250 ms and a rest, 10 s in all. The passes take 640 s, past the
// limit of 600 s.
inline std::vector<uint8_t> CountedLoopArchive(sar::Flavor flavor)
{
    Code code(BigEndianArguments(flavor));
    code.LoopStart(64).Note(60, 127, 24).Wait(936).LoopEnd().End();

    return SequenceArchive(flavor, "SEQ_COUNTED_LOOP", code);
}

// SEQ_NESTED_LOOPS: a loop of 127 passes of 10.5 s: C4 for 0.5 s, then D4 for 5 s twice in an inner loop. The passes
// take 1,333.5 s. The limit comes 1.5 s into the 58th pass, inside the inner loop.
inline std::vector<uint8_t> NestedLoopsArchive(sar::Flavor flavor)
{
    Code code(BigEndianArguments(flavor));
    code.LoopStart(127).Note(60, 127, 48);
    code.LoopStart(2).Note(62, 127, 480).LoopEnd();
    code.LoopEnd().End();

    return SequenceArchive(flavor, "SEQ_NESTED_LOOPS", code);
}

// Writes a part of 10.5 s for ever: C4 for 0.5 s, then D4 for 5 s twice in a counted loop.
inline void EndlessPart(Code& code)
{
    code.LoopStart(0).Note(60, 127, 48);
    code.LoopStart(2).Note(62, 127, 480).LoopEnd();
    code.LoopEnd().End();
}

// SEQ_COUNTED_IN_ENDLESS: track 0 plays EndlessPart, and track 1 the loop of 64 passes of 10 s. The limit comes 1.5 s
// into track 0's 58th pass, inside its counted loop.
inline std::vector<uint8_t> CountedInEndlessArchive(sar::Flavor flavor)
{
    const bool big_endian = BigEndianArguments(flavor);
    Code track_0(big_endian);
    track_0.OpenTrack(1, 0);
    EndlessPart(track_0);
    const auto track_1 = static_cast<uint32_t>(track_0.Bytes().size());

    Code code(big_endian);
    code.OpenTrack(1, track_1);
    EndlessPart(code);
    code.LoopStart(64).Note(64, 127, 24).Wait(936).LoopEnd().End();

    return SequenceArchive(flavor, "SEQ_COUNTED_IN_ENDLESS", code, 3);
}

// The archives of the counted loops that run past the limit, for the console of `flavor`, in the order that
// CheckLongLoops checks them.
inline std::vector<std::vector<uint8_t>> LongLoopArchives(sar::Flavor flavor)
{
    return {CountedLoopArchive(flavor), NestedLoopsArchive(flavor), CountedInEndlessArchive(flavor)};
}

// Checks the timings of the counted loops that run past the limit. The loops loop for ever, and the fade starts after
// two passes. Of the nested loops, only the outer one does: its passes take 10.5 s. Were the inner loop endless too,
// the track would repeat D4 for ever, and the fade would start after 10.5 s. A track whose outermost loop is endless
// keeps its counted loop: the fade starts once track 0 has looped twice, at 21 s. Were track 0's counted loop endless
// too, track 0 would repeat D4 for ever, and the fade would start once track 1 had looped twice, at 20 s.
// `measure(bytes)` gives the timing of sound 0 of the archive `bytes` (with length_ms and fade_ms), or nothing.
// `console` names the console in the checks' messages.
template <typename Measure>
void CheckLongLoops(sar::Flavor flavor, const Measure& measure, const std::string& console)
{
    const auto counted = measure(CountedLoopArchive(flavor));
    const auto nested = measure(NestedLoopsArchive(flavor));
    const auto in_endless = measure(CountedInEndlessArchive(flavor));

    xsflib::testing::CheckTiming("on " + console + ", a counted loop that runs past the limit loops after two passes",
                                 counted, 19995, 20015, 7000);
    xsflib::testing::CheckTiming("on " + console + ", only the outer of two counted loops loops for ever", nested,
                                 20995, 21015, 7000);
    xsflib::testing::CheckTiming("on " + console + ", a counted loop inside an endless loop still ends", in_endless,
                                 20995, 21015, 7000);
}

// SEQ_TWO_TRACKS: track 0 plays C4 for 250 ms at the start. Track 1 waits 5 s, then plays C4 for 250 ms too.
inline std::vector<uint8_t> TwoTracksArchive(sar::Flavor flavor)
{
    // Track 1 starts after the timebase and the tempo (5 bytes), and track 0's open (5), note (3) and end (1).
    constexpr uint32_t kTrack1 = 14;
    Code code(BigEndianArguments(flavor));
    code.OpenTrack(1, kTrack1).Note(60, 127, 24).End();
    XSFLIB_CHECK(code.Bytes().size() == kTrack1);
    code.Wait(480).Note(60, 127, 24).End();

    return SequenceArchive(flavor, "SEQ_TWO_TRACKS", code, 3);
}

// Checks that an analysis plays the tracks of TwoTracksArchive at the volumes that the game gives them as it starts
// the sound. Track 1's late note sets the length: 5 s more than SEQ_END's. With track 1 at 0, the sequence ends after
// track 0's note, as SEQ_END does. With track 0 at 0, the length stays. With both at 0, the sequence never sounds.
// `analyze(muted)` gives the timing of the sound with the tracks of the mask `muted` at 0 (with length_ms and silent),
// or nothing.
template <typename Analyze>
void CheckTrackVolumes(const Analyze& analyze)
{
    const auto length = [&analyze](uint16_t muted)
    {
        const auto timing = analyze(muted);
        return timing && !timing->silent ? timing->length_ms : -1;
    };

    const long long full = length(0);
    const auto muted = analyze(uint16_t{3});

    xsflib::testing::Check(full >= 5240 && full <= 5280,
                           "two tracks with a note 5 s apart play for " + std::to_string(full) + " ms");
    xsflib::testing::Check(length(2) >= 240 && length(2) <= 280,
                           "with track 1 at 0, the sequence ends after track 0's note");
    xsflib::testing::Check(length(1) == full, "with track 0 at 0, the length stays");
    xsflib::testing::Check(muted && muted->silent, "with both tracks at 0, the sequence never sounds");
}

} // namespace ntdlib::testing::length_cases
