// SPDX-License-Identifier: MIT

// The walk of a song of Universomo's sound driver: where the driver's update is in the order list, a call at a time,
// and how long a song plays. It follows the update at 0x0202CE98 in Beat City (USA, NTR-BEYE) for the timing only:
// the rows, the orders, the tracks' waits and the effect that sets the speed. It plays no notes.
//
// One call of the update is one tick. On the first tick of a row, the update reads each track's events for the row,
// then counts the row. After the last row of a pattern it goes to the next order. After the last order, a looped song
// goes to the restart order, and a song that doesn't loop stops on the tick that reads its last row.

#pragma once

#include <cstdint>
#include <vector>

#include "ntdlib/ntr/sound.h"
#include "ntdlib/universomo/formats.h"

namespace ntdlib::universomo
{

// The game calls the update once a frame of the DS: 560,190 cycles of the system clock.
constexpr uint32_t kFrameCycles = ntr::kFrameCycles;
constexpr double kTicksPerSecond = static_cast<double>(ntr::kSystemClockHz) / kFrameCycles;

class Walk
{
public:
    // A walk from the song's first tick. The song must outlive the walk.
    Walk(const Song& song, bool loop);

    // One call of the update.
    void Update();

    bool Playing() const;

    // The order, the row and the tick that the next call of the update starts from.
    uint32_t Order() const;
    uint32_t Row() const;
    uint32_t Tick() const;

    // The calls of the update a row, as the effect kSpeed last set it.
    uint32_t Speed() const;

    // Whether the next call of the update reads a row.
    bool ReadsRowNext() const;

    // The calls of the update so far, and the times that a looped song has gone back to its restart order.
    uint64_t Updates() const;
    uint32_t Wraps() const;

private:
    void EnterOrder();
    void ReadRow(const Pattern& pattern);

    const Song& song_;
    bool loop_ = true;
    bool playing_ = true;
    bool first_row_ = true;
    uint32_t speed_ = 0;
    uint32_t tick_ = 0;
    uint32_t row_ = 0;
    uint32_t order_ = 0;
    uint64_t updates_ = 0;
    uint32_t wraps_ = 0;
    std::vector<uint8_t> waits_; // each track's rows until its next event
    std::vector<uint32_t> next_; // each track's next event
};

// How long a song plays, in calls of the update. A looped song plays `intro` ticks up to a read of the first row of its
// restart order, then repeats `loop` ticks. The intro ends at the first such read when the song's first pass has the
// speed of the later ones, and at the second read otherwise. A song that doesn't loop plays `intro` ticks, the last of
// which stops it, and `loop` is 0.
struct SongLength
{
    friend bool operator==(const SongLength&, const SongLength&) = default;

    uint64_t intro = 0;
    uint64_t loop = 0;
};

// Walks a song to its end, or through four passes of its loop. Throws xsflib::FormatError when the song passes
// `max_ticks` first, or when its passes after the first differ in length or speed.
SongLength MeasureSong(const Song& song, bool loop, uint64_t max_ticks = 100000000);

} // namespace ntdlib::universomo
