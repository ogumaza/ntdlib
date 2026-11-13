// SPDX-License-Identifier: MIT

// A player of a record of Donkey Kong Jungle Beat's chord database: the steps of an entry (see chord_files.h), each
// started by an event of one of the rhythm's timers, with the keys of chord_keys.h. The music drives the timers through
// its sequence callbacks. The addresses are in the main.dol of the GameCube release (USA).
//
// An entry's steps block starts with a count that the code doesn't read. Step c then takes two bytes: its flow at
// 1 + 2c and its level at 2 + 2c. The flow's low 3 bits give the wait after the step: 0 to 3 wait for an event of that
// timer, 4 plays the next step at once, and 5 to 7 end the record. Bit 3 holds the player until the game moves it. The
// high nibble, if not 0, is the step after next, less 1: a repeat.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/jaudio/chord_files.h"
#include "ntdlib/jaudio/chord_keys.h"

namespace ntdlib::jaudio
{

// What a step does to the record's effect (0x8024da30).
struct ChordStep
{
    enum class Action : uint8_t
    {
        kStart, // stops the effect and starts it again with the keys
        kStop,  // a level of 1: stops the effect
        kKeep,  // a level of 2: leaves the effect playing
    };

    Action action = Action::kStart;
    int step = 0;
    float level = 1.0f;
    // Each part's key for port 0xb + the part's index, or -1 for a part that writes none.
    std::vector<int> keys;
};

class ChordPlayer
{
public:
    // A player of `entry` of the chord database `data`, from step `start` (0x8024c28c, 0x8024d80c). The record's
    // fourth word gives the start step: the high nibble of its byte 1. `level` is the level that startChordSe passes
    // on.
    ChordPlayer(std::span<const uint8_t> data, const ChordDatabase::Entry& entry, int start, float level);

    // Starts the record. The wait comes from step 0's flow, whatever the start step. A wait of 4 plays the start step
    // + 1 at once, and the player then holds.
    std::vector<ChordStep> Start(ChordMoment& moment, ChordRandom& random);

    // The timer whose event plays the next step, or nothing while the player holds or after it has ended. A wait of 3
    // uses timer 3 only while +0x9a8 of the game's object is set, and timer 0 otherwise (0x80249db4). The caller
    // maps it: +0x9a8 is the first byte of the rhythm's timer 3 (the rhythm is at +0x810), which the timer sets once it
    // has fired (0x80249280). See ChordRhythm::TimerThreeRunning.
    std::optional<int> Waiting() const
    {
        return waiting_;
    }

    bool Playing() const
    {
        return playing_;
    }

    int NextStep() const
    {
        return step_;
    }

    // An event of the timer that the player waits for (0x80249fa8, 0x8024a108, 0x8024a1cc, 0x8024a2b0): plays the step
    // at the rhythm's accent, then takes the step's flow (0x8024dc2c). The rhythm's accent is 1.0 or 0.9 after an event
    // of its accent timer (0x8024a290), and 0.7 after an event of timer 0.
    std::vector<ChordStep> Event(float accent, ChordMoment& moment, ChordRandom& random);

    // Moves the player to step `step` (0x8024de64): the next event plays step + 1, and each part goes to block `step`.
    std::vector<ChordStep> Jump(int step, ChordMoment& moment, ChordRandom& random);

private:
    uint8_t Flow(int step) const;
    uint8_t Level(int step) const;
    ChordStep Play(float accent, ChordMoment& moment, ChordRandom& random);
    void Wait(uint8_t flow, ChordMoment& moment, ChordRandom& random, std::vector<ChordStep>& steps);

    std::span<const uint8_t> data_;
    uint32_t steps_ = 0;
    std::vector<ChordPart> parts_;
    std::vector<int> keys_;
    float level_ = 1.0f;
    int step_ = 0;
    std::optional<int> waiting_;
    bool playing_ = true;
};

// A beat of the rhythm: the wait whose players play their next step, at the accent that the beat gives.
struct ChordBeat
{
    int wait = 0;
    float accent = 1.0f;
};

// The rhythm that drives the players' waits (0x80247d90). The music's track 0 and its child 0 play through the
// rhythm's parser (0x80247e20). Each note there with a voice above 0 marks a timer by its key (0x80247194, 0x802485d4).
// Once a game frame, each marked timer fires and the marks clear (0x80248e94, 0x802490c4). A mark with a velocity of 0
// fires nothing. The timers fire in a fixed order (0x80248784): the accent, then waits 3, 0, 1 and 2.
class ChordRhythm
{
public:
    // The keys of the notes that mark the timers.
    static constexpr uint8_t kAccentKey = 0x26;
    static constexpr std::array<uint8_t, 4> kWaitKeys = {0x27, 0x2a, 0x24, 0x25};

    // A note of the music's track 0 or its child 0 with a voice above 0. Other keys mark nothing that the players
    // read.
    void Note(uint8_t key, uint8_t velocity);

    // The beats of a game frame, in the order that the game plays them. A beat of wait 3 reaches only the players
    // that wait for timer 3 (see ChordPlayer::Waiting). The accent comes from the accent timer: 1.0 for a velocity
    // below 100 and 0.9 from 100 up (0x802490c4, 0x8024a290). After the beat of wait 0, the accent is 0.7 until the
    // accent timer fires again (0x8024a1cc).
    std::vector<ChordBeat> Frame();

    float Accent() const
    {
        return accent_;
    }

    // Whether timer 3 has fired since the piece started: a player that waits for timer 3 then waits for it, and for
    // timer 0 otherwise.
    bool TimerThreeRunning() const
    {
        return timer_three_;
    }

    // Whether the last beat of wait 0 had a velocity of 100 or more. The timer then counts its strong beats in the
    // word at +8 (0x802490c4), which the game reads as +0x940 of the audio manager (0x8024e060).
    bool WaitZeroStrong() const
    {
        return wait_zero_strong_;
    }

private:
    std::optional<uint8_t> accent_mark_;
    std::array<std::optional<uint8_t>, 4> wait_marks_{};
    float accent_ = 1.0f;
    bool timer_three_ = false;
    bool wait_zero_strong_ = false;
};

} // namespace ntdlib::jaudio
