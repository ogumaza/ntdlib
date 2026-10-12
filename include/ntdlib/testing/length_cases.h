// SPDX-License-Identifier: MIT

// The length analysis's cases for a rip tool of the Wii U's or the Switch's sound archives: sequences that each take
// one of the analysis's paths, an archive that holds them in either byte order, and the timing that the analysis gives
// each one. A rip tool's test measures every case on its model of the sound library, and CheckCases checks the timings.
// The timings hold for sound frames of 3 ms and of 5 ms.
//
// Every sequence starts with timebase 48 and tempo 120: 48 ticks take 500 ms. Program 0 plays a wave of 1 s that ends,
// and program 1 a wave of 0.1 s that loops. Both have the fastest attack, decay and release, a sustain at full volume
// and no hold. A note then settles at once. The cases are: a sequence that ends; one that loops by jumping back, and
// one that loops for ever; one that never sounds; held notes on a looping wave and on one that ends; a jump back into
// code the track hasn't run (not a loop); notes at a track volume and at a sound volume of 0 (which never sound
// either); and a track that loops every tick before another track's note starts. In the rest, another track plays while
// a track polls or counts a variable once a tick or marks beats for the game; a track that changes its volume but plays
// no notes comes before the track that plays, or is track 0; a tied note's wave ends during a pass; or a note runs out
// while its track polls, or would but for a held length or the damper. In the next four, another track plays while
// track 0 waits for a note that never ends, waits out a held note, waits for notes that do end, or plays a note that
// never ends without waiting for it. In the next two, track 0 plays notes in a loop inside a subroutine: a loop that
// never ends, and one that a conditional command leaves after three passes. In the next, a later track's passes play no
// note and only change a note that the game stops. In the next, each pass of a later track plays a note and then counts
// ticks in a loop. In the next two, a note comes before loops that make no sound, or before a rest of more than 180 s.
// The two end at the same frame. In the last, a later track's inner loop counts ticks after its note.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

namespace ntdlib::testing::length_cases
{

// A sequence's bytecode in an archive's byte order. A command's 16- and 24-bit arguments follow the byte order. The
// sequence player reads them that way. Lengths, waits and programs are variable-length numbers. Offsets count from the
// start of the bytecode. The timebase and tempo come first.
class Code
{
public:
    explicit Code(bool big_endian) : big_endian_(big_endian)
    {
        Byte(0xB0).Byte(48); // timebase 48
        Byte(0xE1).U16(120); // tempo 120
    }

    const std::vector<uint8_t>& Bytes() const
    {
        return bytes_;
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
        bytes_.push_back(static_cast<uint8_t>(value));

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
        return big_endian_ ? Byte(value >> 8).Byte(value) : Byte(value).Byte(value >> 8);
    }

    Code& U24(uint32_t value)
    {
        return big_endian_ ? Byte(value >> 16).Byte(value >> 8).Byte(value)
                           : Byte(value).Byte(value >> 8).Byte(value >> 16);
    }

    bool big_endian_;
    std::vector<uint8_t> bytes_;
};

// A case: the sound's label, its sequence, where the sound starts in it, the sound's volume and the tracks it
// allocates, and the timing that the analysis gives it. The length lies from min_ms to max_ms, or equals the length of
// case `same_length_as` when that's 0 or more.
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
};

// Every case, by its sound's index in the archive. The comment on each case says what its sequence does, then why the
// analysis times it so.
inline const std::vector<Case>& Cases()
{
    static const std::vector<Case> kCases = {
        // A note of 24 ticks, then the end. The note lasts 250 ms with the fastest release, from the second frame, and
        // 500 ms of tail follow. Under LengthRules::end_at_idle_start, the sequence can end at the engine's first idle
        // frame. Where the output runs a frame behind the engine, that's two frames before the last sound ends.
        {"SEQ_END", [](Code& c) { c.Note(60, 127, 24).End(); }, 0, 127, 1, 740, 780, 0, false},

        // A note of 24 ticks, played over and over by a jump back to it. Two loops of 24 ticks, the first starting in
        // the second frame, then the fade.
        {"SEQ_JUMP", [](Code& c) { c.Note(60, 127, 24).Jump(5).End(); }, 0, 127, 1, 495, 515, 10000},

        // The same note in a loop with a count of 0. Such a loop repeats for ever.
        {"SEQ_FOREVER", [](Code& c) { c.LoopStart(0).Note(60, 127, 24).LoopEnd().End(); }, 0, 127, 1, 495, 515, 10000},

        // A wait and the end, with no note. No sound: the tail alone.
        {"SEQ_SILENT", [](Code& c) { c.Wait(48).End(); }, 0, 127, 1, 500, 500, 0, true},

        // A held note of 9,600 ticks on program 1. Program 1's wave loops. The held note settles at once. The wave of
        // 0.1 s loops twice.
        {"SEQ_HELD_LOOP", [](Code& c) { c.Program(1).Note(60, 127, 9600).End(); }, 0, 127, 1, 200, 300, 10000},

        // A held note on program 0. The wave ends after 1 s. The sequence ends with it.
        {"SEQ_HELD_ONCE", [](Code& c) { c.Note(60, 127, 9600).End(); }, 0, 127, 1, 1490, 1530, 0},

        // Starts after a note and its end, and jumps back to the note. The track hasn't played that note. The jump
        // isn't a loop, and the note plays once.
        {"SEQ_SHARED", [](Code& c) { c.Note(60, 127, 24).End().Jump(5); }, 9, 127, 1, 740, 780, 0},

        // A note at a track volume of 0. It makes no sound.
        {"SEQ_TRACK_MUTED", [](Code& c) { c.Volume(0).Note(60, 127, 24).End(); }, 0, 127, 1, 500, 500, 0, true},

        // A note in a sound whose volume is 0. It makes no sound either.
        {"SEQ_SOUND_MUTED", [](Code& c) { c.Note(60, 127, 24).End(); }, 0, 0, 1, 500, 500, 0, true},

        // Opens track 1 at 16 and waits a tick over and over. Meanwhile, track 1 waits 48 ticks and holds a note on
        // program 1. Track 0's loops of a tick change nothing, and don't count. The held note starts at 500 ms. Its
        // wave of 0.1 s loops twice, and then the fade starts.
        {"SEQ_LATE", [](Code& c) { c.OpenTrack(1, 16).Wait(1).Jump(10).Wait(48).Program(1).Note(60, 127, 9600).End(); },
         0, 127, 3, 690, 730, 10000},

        // Track 0 opens track 1 at 27, then polls variable 0 once a tick: it compares the variable with 1, would jump
        // to its end if they were equal, and waits a tick. Track 1 plays a note of 24 ticks over and over. Track 0 only
        // polls, and its passes don't count. Track 1's loops are the sequence's loops: two loops of 24 ticks.
        {"SEQ_POLL_MUSIC",
         [](Code& c)
         {
             c.OpenTrack(1, 27).CompareEqual(0, 1).If().Jump(26).Wait(1).Jump(10).End();
             c.Note(60, 127, 24).Jump(27).End();
         },
         0, 127, 3, 495, 515, 10000},

        // Track 0 polls as in SEQ_POLL_MUSIC. Track 1 ties a note of a tick on program 1 to itself over and over. The
        // note sounds on, and no track changes anything after the first ticks. The tied note's wave of 0.1 s loops
        // twice.
        {"SEQ_POLL_HOLD",
         [](Code& c)
         {
             c.OpenTrack(1, 27).CompareEqual(0, 1).If().Jump(26).Wait(1).Jump(10).End();
             c.Program(1).Tie(true).Note(60, 127, 1).Jump(31).End();
         },
         0, 127, 3, 190, 260, 10000},

        // Track 0 polls as in SEQ_POLL_MUSIC. Track 1 plays a note of 12 ticks, rests for 48 ticks, plays the note
        // again and ends. The second note ends at 750 ms, and 500 ms of tail follow. Track 0's polls during the rest
        // aren't loops.
        {"SEQ_POLL_END",
         [](Code& c)
         {
             c.OpenTrack(1, 27).CompareEqual(0, 1).If().Jump(26).Wait(1).Jump(10).End();
             c.Note(60, 127, 12).Wait(48).Note(60, 127, 12).End();
         },
         0, 127, 3, 1235, 1285, 0},

        // Track 0 sets the track's volume to 100 at the start of each pass, then polls as in SEQ_POLL_MUSIC. Track 1,
        // at 29, plays a note of 24 ticks over and over. Track 0's first pass changes its volume, and counts as a loop.
        // Then track 1's loops take over, and the count starts again: two loops of 24 ticks.
        {"SEQ_POLL_MOVE",
         [](Code& c)
         {
             c.OpenTrack(1, 29).Volume(100).CompareEqual(0, 1).If().Jump(28).Wait(1).Jump(10).End();
             c.Note(60, 127, 24).Jump(29).End();
         },
         0, 127, 3, 495, 515, 10000},

        // Track 0 sets variable 1 to 1 and then to 2, 24 ticks apart, over and over. These are beats for the game. No
        // track reads them. Track 1, at 29, holds a note on program 1. The sequence is steady once track 0 has made a
        // pass, at 500 ms. By then the held note's wave has looped more than twice.
        {"SEQ_BEATS",
         [](Code& c)
         {
             c.OpenTrack(1, 29).SetVariable(1, 1).Wait(24).SetVariable(1, 2).Wait(24).Jump(10).End();
             c.Program(1).Note(60, 127, 9600).End();
         },
         0, 127, 3, 495, 530, 10000},

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
         0, 127, 3, 495, 540, 10000},

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
         0, 127, 7, 745, 765, 10000},

        // Ties notes of 36 ticks on program 0 to one another over and over (from 10). The wave ends after 1 s, during
        // the third pass, and the next pass starts a new note at 1,125 ms. A track whose passes play notes isn't stuck.
        // That pass is the first loop. The pass that starts the note again at 2,250 ms is the second.
        {"SEQ_RESTART", [](Code& c) { c.Tie(true).Note(60, 127, 36).Note(60, 127, 36).Jump(10).End(); }, 0, 127, 1,
         2620, 2640, 10000},

        // Plays a note of 48 ticks on program 1 without waiting for it, then waits a tick and jumps back, over and over
        // (from 12). The note runs out at 500 ms, and 500 ms of tail follow. Its track polls, but isn't settled while
        // the note has length left.
        {"SEQ_ENDING", [](Code& c) { c.Program(1).NoteWait(false).Note(60, 127, 48).Wait(1).Jump(12).End(); }, 0, 127,
         1, 990, 1015, 0},

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
         0, 127, 3, 995, 1015, 10000},

        // Plays a held note of 9,600 ticks on program 1 without waiting for it, then waits a tick and jumps back, over
        // and over (from 13). The game stops a held note before its length runs out. Its track polls and is settled,
        // and the note's wave of 0.1 s loops twice.
        {"SEQ_HELD_POLL", [](Code& c) { c.Program(1).NoteWait(false).Note(60, 127, 9600).Wait(1).Jump(13).End(); }, 0,
         127, 1, 200, 300, 10000},

        // Turns the damper on, plays a note of 48 ticks on program 1 without waiting for it, then waits a tick and
        // jumps back, over and over (from 14). The damper keeps the note on when its length runs out. The track is
        // settled. So is the track of SEQ_HELD_POLL.
        {"SEQ_DAMPER",
         [](Code& c) { c.Program(1).Damper(127).NoteWait(false).Note(60, 127, 48).Wait(1).Jump(14).End(); }, 0, 127, 1,
         200, 300, 10000},

        // Track 0 opens track 1 at 16, plays a note of length 0 on program 1 and ends. With note wait on, it waits for
        // the note to end, and the note never does. Track 1 plays a note of 24 ticks over and over. Track 0 waits for
        // ever, and doesn't keep track 1 from being the main track. Track 1's loops at 24 and 48 ticks count.
        {"SEQ_ENDLESS",
         [](Code& c) { c.OpenTrack(1, 16).Program(1).Note(60, 127, 0).End().Note(60, 127, 24).Jump(16).End(); }, 0, 127,
         3, 495, 515, 10000},

        // Track 0 opens track 1 at 17, holds a note of 9,600 ticks on program 1 and ends. Track 1 plays as in
        // SEQ_ENDLESS, and its loops count the same way.
        {"SEQ_HELD_UNDER",
         [](Code& c) { c.OpenTrack(1, 17).Program(1).Note(60, 127, 9600).End().Note(60, 127, 24).Jump(17).End(); }, 0,
         127, 3, 495, 515, 10000},

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
         0, 127, 3, 1495, 1515, 10000},

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
         0, 127, 3, 995, 1015, 10000},

        // Track 0 calls a subroutine at 10. The subroutine plays a note of 24 ticks and jumps back to its start,
        // unconditionally. No conditional command runs in the subroutine, and nothing can leave its loop. The
        // subroutine's jumps back at 24 and 48 ticks count.
        {"SEQ_SUB_LOOP", [](Code& c) { c.Call(10).End().Note(60, 127, 24).Jump(10).End(); }, 0, 127, 1, 495, 515,
         10000},

        // Track 0 sets variable 0 to 0 and calls a subroutine at 15. The subroutine plays a note of 24 ticks and adds 1
        // to the variable. It returns once the variable is 3, and otherwise jumps back to its start. The subroutine's
        // passes run a conditional command, and don't count. The third note ends at 750 ms, and 500 ms of tail follow.
        {"SEQ_SUB_COUNT",
         [](Code& c)
         {
             c.SetVariable(0, 0).Call(15).End();
             c.Note(60, 127, 24).AddVariable(0, 1).CompareEqual(0, 3).If().Return().Jump(15).End();
         },
         0, 127, 1, 1235, 1285, 0},

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
         0, 127, 3, 495, 515, 10000},

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
         0, 127, 3, 495, 515, 10000},

        // A note of 24 ticks and a rest of 24 ticks. Then the track sets its volume to 100 and to 90, 12 ticks apart,
        // and jumps back, over and over (from 10). The note sounds as in SEQ_END. Then the track's loops make no sound.
        // A loop that went round without a sound ends the sequence at its last sound.
        {"SEQ_REST_LOOP",
         [](Code& c) { c.Note(60, 127, 24).Wait(24).Volume(100).Wait(12).Volume(90).Wait(12).Jump(10).End(); }, 0, 127,
         1, 740, 780, 0},

        // A note of 24 ticks, then a rest of 20,000 ticks (208 s), then the end. The note sounds as in SEQ_END. Then
        // the sequence stays busy without a sound for 180 s. The analysis ends the sequence where it fell silent.
        // SEQ_REST_LOOP and SEQ_LONG_REST end at the same frame. A different rule ends each one.
        {"SEQ_LONG_REST", [](Code& c) { c.Note(60, 127, 24).Wait(20000).End(); }, 0, 127, 1, 740, 780, 0, false, 30},

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
         0, 127, 3, 995, 1015, 10000},
    };

    return kCases;
}

// The sequence of case `index` in the byte order.
inline std::vector<uint8_t> Bytecode(std::size_t index, bool big_endian)
{
    Code code(big_endian);
    Cases()[index].write(code);

    return code.Bytes();
}

// The archive that holds every case, big-endian for the Wii U or little-endian for the Switch. Its sound table lists
// the cases in order, and nothing more.
inline std::vector<uint8_t> Archive(bool big_endian)
{
    // A wave of 1 s that ends, and one of 0.1 s that loops: PCM16 at 0x1000.
    std::vector<uint8_t> samples(2 * 32000);
    for (std::size_t i = 0; i < samples.size(); i += 2)
    {
        samples[big_endian ? i : i + 1] = 0x10;
    }

    sar_files::WaveSpec once;
    once.encoding = 1;
    once.loop_end = 32000;
    once.channels = {samples};
    sar_files::WaveSpec looping = once;
    looping.loop = true;
    looping.loop_end = 3200;
    looping.channels = {std::vector<uint8_t>(samples.begin(), samples.begin() + 2 * 3200)};

    // Program 0 plays the wave that ends, and program 1 the one that loops.
    sar_files::BankSpec bank;
    bank.waves = {{0x05000000, 0}, {0x05000000, 1}};
    for (uint32_t wave = 0; wave < 2; ++wave)
    {
        sar_files::Region region;
        region.wave = wave;
        region.original_key = 60;
        region.adshr = {127, 127, 127, 0, 127};
        sar_files::VelocityTable velocities;
        velocities.entries = {{0, region}};
        sar_files::KeyTable keys;
        keys.entries = {{0, velocities}};
        bank.instruments.push_back(keys);
    }

    sar_files::ArchiveSpec spec;
    spec.banks = {{"BANK", 0}};
    spec.wave_archives = {{"WARC", 1}};
    spec.players = {{"PLAYER", 1}};
    spec.files = {
        {sar_files::Bank(big_endian, bank), "", false},
        {sar_files::WaveArchive(big_endian, {sar_files::Wave(big_endian, once), sar_files::Wave(big_endian, looping)}),
         "", false}};

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
        spec.files.push_back({sar_files::Sequence(big_endian, Bytecode(i, big_endian), {{"Start", 0}}), "", false});
    }

    return sar_files::Archive(big_endian, spec);
}

// Checks every case's timing. `analyze(index)` gives the timing of the archive's sound `index` (with length_ms, fade_ms
// and silent), or nothing. A sound past the last case has no timing.
template <typename Analyze>
void CheckCases(const Analyze& analyze)
{
    for (std::size_t i = 0; i < Cases().size(); ++i)
    {
        const Case& item = Cases()[i];
        const std::string name = item.name;
        const auto timing = analyze(static_cast<uint32_t>(i));
        if (!xsflib::testing::Check(timing.has_value(), name + " has a length"))
        {
            continue;
        }

        long long min_ms = item.min_ms;
        long long max_ms = item.max_ms;
        if (item.same_length_as >= 0)
        {
            const auto other = analyze(static_cast<uint32_t>(item.same_length_as));
            min_ms = max_ms = other ? other->length_ms : -1;
        }

        xsflib::testing::Check(
            timing->length_ms >= min_ms && timing->length_ms <= max_ms && timing->fade_ms == item.fade_ms,
            name + "'s length is " + std::to_string(timing->length_ms) + " ms with a fade of " +
                std::to_string(timing->fade_ms) + " ms");
        xsflib::testing::Check(timing->silent == item.silent, name + (item.silent ? " is silent" : " isn't silent"));
    }

    xsflib::testing::Check(!analyze(static_cast<uint32_t>(Cases().size())).has_value(),
                           "a sound past the last has no length");
}

} // namespace ntdlib::testing::length_cases
