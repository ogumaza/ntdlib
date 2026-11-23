// SPDX-License-Identifier: MIT

#include "ntdlib/universomo/walk.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "xsflib/binary.h"

namespace ntdlib::universomo
{

Walk::Walk(const Song& song, bool loop) : song_(song), loop_(loop), speed_(song.speed)
{
    EnterOrder();
}

void Walk::Update()
{
    if (!playing_)
    {
        return;
    }

    if (tick_ >= speed_ || first_row_)
    {
        tick_ = 0;
        first_row_ = false;
        const Pattern& pattern = song_.patterns[song_.orders[order_]];
        ReadRow(pattern);

        ++row_;
        if (row_ >= pattern.rows)
        {
            row_ = 0;
            ++order_;
            if (order_ >= song_.orders.size())
            {
                if (!loop_)
                {
                    playing_ = false;
                    ++updates_;
                    return;
                }

                order_ = song_.restart;
                ++wraps_;
            }

            EnterOrder();
        }
    }

    ++tick_;
    ++updates_;
}

bool Walk::Playing() const
{
    return playing_;
}

uint32_t Walk::Order() const
{
    return order_;
}

uint32_t Walk::Row() const
{
    return row_;
}

uint32_t Walk::Tick() const
{
    return tick_;
}

uint32_t Walk::Speed() const
{
    return speed_;
}

bool Walk::ReadsRowNext() const
{
    return first_row_ || tick_ >= speed_;
}

uint64_t Walk::Updates() const
{
    return updates_;
}

uint32_t Walk::Wraps() const
{
    return wraps_;
}

void Walk::EnterOrder()
{
    const Pattern& pattern = song_.patterns[song_.orders[order_]];
    waits_.resize(pattern.tracks.size());
    next_.assign(pattern.tracks.size(), 0);
    for (std::size_t t = 0; t < pattern.tracks.size(); ++t)
    {
        waits_[t] = pattern.tracks[t].first_rows;
    }
}

// Each track whose wait has run out reads its events up to one that waits. A track that runs out of events waits 255
// rows. Then every track with events counts the row, as an 8-bit counter.
void Walk::ReadRow(const Pattern& pattern)
{
    for (std::size_t t = 0; t < pattern.tracks.size(); ++t)
    {
        const std::vector<Event>& events = pattern.tracks[t].events;
        if (events.empty() || waits_[t] != 0 || next_[t] >= events.size())
        {
            continue;
        }

        uint8_t rows = 0;
        std::size_t i = next_[t];
        for (; i < events.size(); ++i)
        {
            const Event& event = events[i];
            if (event.note == (kEffectFlag | kSpeed))
            {
                speed_ = event.value;
            }

            ++next_[t];
            rows = event.rows;
            if (rows != 0)
            {
                break;
            }
        }

        waits_[t] = i >= events.size() ? 255 : rows;
    }

    for (std::size_t t = 0; t < pattern.tracks.size(); ++t)
    {
        if (!pattern.tracks[t].events.empty())
        {
            --waits_[t];
        }
    }
}

SongLength MeasureSong(const Song& song, bool loop, uint64_t max_ticks)
{
    Walk walk(song, loop);
    const auto run_while = [&](auto condition)
    {
        while (condition())
        {
            if (walk.Updates() >= max_ticks)
            {
                throw xsflib::FormatError("the song plays past " + std::to_string(max_ticks) + " ticks");
            }

            walk.Update();
        }
    };

    if (!loop)
    {
        run_while([&] { return walk.Playing(); });

        return {walk.Updates(), 0};
    }

    // A pass starts where the update reads the restart order's first row. The tracks' waits start again there. The
    // speed is therefore the only state of the walk that goes on from one pass to the next. The song repeats from the
    // first pass that starts at the speed of the next one. Three passes after the first have to agree. The state of the
    // walk then repeats.
    const auto next_pass = [&]
    {
        run_while([&] { return walk.Order() != song.restart || walk.Row() != 0 || !walk.ReadsRowNext(); });
        const uint64_t start = walk.Updates();
        const uint32_t speed = walk.Speed();
        walk.Update();
        return std::pair{start, speed};
    };

    const auto [start0, speed0] = next_pass();
    const auto [start1, speed1] = next_pass();
    const auto [start2, speed2] = next_pass();
    const auto [start3, speed3] = next_pass();
    const uint64_t loop_ticks = start2 - start1;
    if (speed2 != speed1 || speed3 != speed1 || start3 - start2 != loop_ticks)
    {
        throw xsflib::FormatError("the song's loop takes " + std::to_string(loop_ticks) + " ticks, then " +
                                  std::to_string(start3 - start2));
    }

    return {speed0 == speed1 ? start0 : start1, loop_ticks};
}

} // namespace ntdlib::universomo
