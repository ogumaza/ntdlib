// SPDX-License-Identifier: MIT

// What the models of the driver's two builds share: the vibrato's wave and the layout of a channel's record.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ntdlib/logikstate/player.h"

namespace ntdlib::logikstate::parts
{

// The first half of the vibrato's wave, 64 steps a cycle: ProTracker's sine table. The second half is the first,
// negated. Both builds keep the whole cycle as a table of 64 halfwords.
constexpr std::array<int16_t, 32> kSine = {0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212,
                                           224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224,
                                           212, 197, 180, 161, 141, 120, 97,  74,  49,  24};

// Returns the vibrato's wave at `position`, from 0 to 63.
inline int32_t VibratoWave(uint16_t position)
{
    return position < 32 ? kSine[position & 31] : -kSine[position & 31];
}

// Appends `value` to `out` in `bytes` bytes, the lowest first.
inline void Put(std::vector<uint8_t>& out, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; i++)
    {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

// Returns the first kChannelRecordSize bytes of a channel's record, in the layout of the memory of the build with short
// records and the GBAMOD1 build.
inline std::vector<uint8_t> RecordBytes(const Channel& ch)
{
    std::vector<uint8_t> out;
    Put(out, ch.points, 4);
    Put(out, ch.position, 4);
    Put(out, ch.step, 4);
    Put(out, ch.length, 2);
    Put(out, ch.loop_start, 2);
    Put(out, ch.loop_length, 2);
    Put(out, 0, 2);
    Put(out, uint32_t(ch.volume), 4);
    Put(out, ch.sample, 2);
    Put(out, 0, 2);
    Put(out, ch.fine_tune, 2);
    Put(out, ch.pitch, 2);
    Put(out, uint32_t(ch.effect), 4);
    Put(out, ch.volume_up, 2);
    Put(out, ch.volume_down, 2);
    Put(out, ch.arpeggio_1, 2);
    Put(out, ch.arpeggio_2, 2);
    Put(out, uint16_t(ch.vibrato), 2);
    Put(out, ch.vibrato_depth, 2);
    Put(out, ch.vibrato_speed, 2);
    Put(out, ch.vibrato_position, 2);
    Put(out, uint16_t(ch.slide), 2);
    Put(out, ch.slide_speed, 2);
    Put(out, uint16_t(ch.slide_step), 2);
    Put(out, uint16_t(ch.slide_target), 2);

    return out;
}

} // namespace ntdlib::logikstate::parts
