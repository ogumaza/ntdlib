// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/jaudio/conductor.h"

#include <cstdint>
#include <span>
#include <utility>

#include "jaudio/conductor_file.h"

namespace ntdlib::sequence_export::jaudio
{

namespace
{

using ntdlib::jaudio::detail::ConductorModuleValues;
using ntdlib::jaudio::detail::ConductorTrackValues;
using ntdlib::jaudio::detail::ConductorValues;
using ntdlib::jaudio::detail::ReadConductorValues;

} // namespace

Conductor ReadConductor(std::span<const uint8_t> cnd)
{
    const ConductorValues values = ReadConductorValues(cnd, /*big_endian=*/true, /*check_whole_name=*/true);
    Conductor conductor;
    conductor.poll = values.beat_interval;
    conductor.tempo = values.tempo;

    for (const ConductorTrackValues& track_values : values.tracks)
    {
        ConductorTrack track;
        track.name = track_values.name;
        track.start_beats = static_cast<int16_t>(track_values.start);
        track.half_beat = track_values.half_start;
        track.follows = track_values.follows;
        track.send = track_values.send;

        for (const ConductorModuleValues& module_values : track_values.modules)
        {
            ConductorModule module;
            module.start_beats = static_cast<int16_t>(module_values.start);
            module.beats = module_values.beats;
            module.in_order = module_values.in_order;
            module.bank = module_values.port_high;
            module.program = module_values.port_low;
            module.phrase_list = static_cast<int16_t>(module_values.lists[0]);
            module.length_list = static_cast<int16_t>(module_values.lists[1]);
            module.phrases = module_values.repeats[0];
            module.rests = module_values.repeats[1];
            module.transpose = module_values.transpose;
            module.volume = module_values.volume;
            module.pan = module_values.pan;
            module.half_beat = module_values.half_start;
            track.modules.push_back(module);
        }
        conductor.tracks.push_back(std::move(track));
    }

    return conductor;
}

} // namespace ntdlib::sequence_export::jaudio
