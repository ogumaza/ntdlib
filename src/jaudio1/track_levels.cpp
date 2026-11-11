// SPDX-License-Identifier: MIT

// The levels of JAudio1's tracks (see track.cpp): the timed levels' steps, the updates of the levels, filters and tempo
// that the tracks give their channels.

#include "ntdlib/jaudio1/track.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "jaudio1/track_internal.h"
#include "ntdlib/jaudio1/calc.h"

namespace ntdlib::jaudio1
{

namespace
{

// JASTrack::panCalc: a level mixed with another by the weight, as the calculation type says.
float PanCalc(float a, float b, float weight, uint32_t type)
{
    switch (type)
    {
    case 0:
        return a;
    case 1:
        return b;
    case 2:
        return (1.0f - weight) * a + weight * b;
    default:
        return 0.0f;
    }
}

// The levels that UpdateTrack gives a track's channels.
struct TrackLevels
{
    float volume = 0.0f;
    float pitch = 1.0f;
    float pan = 0.0f;
    float fxmix = 0.0f;
    float dolby = 0.0f;
};

// The track's levels that `flags` names, with the outer levels that `outer` switches on. The other levels keep their
// defaults.
TrackLevels OwnLevels(const Track& track, uint32_t flags, const OuterParam* outer)
{
    const auto switched = [outer](uint16_t level)
    {
        return outer != nullptr && outer->Check(level);
    };

    TrackLevels levels;
    if ((flags & kUpdateVolume) != 0)
    {
        levels.volume = track.timed[kTimedVolume].value;
        if (track.volume_mode == 0)
        {
            levels.volume = levels.volume * levels.volume;
        }
        if (track.muted)
        {
            levels.volume = 0.0f;
        }
        if (switched(OuterParam::kVolume))
        {
            levels.volume = levels.volume * outer->volume;
        }
        if (track.paused && (track.pause_status & 1) != 0)
        {
            levels.volume = levels.volume * track.timed[kTimedPause].value;
        }
    }
    if ((flags & kUpdatePitch) != 0)
    {
        levels.pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
        levels.pitch = levels.pitch * track.vibrate.Value();
        if (switched(OuterParam::kPitch))
        {
            levels.pitch = levels.pitch * outer->pitch;
        }
    }

    const float weight = static_cast<float>(track.registers[kRegOuterWeight]) / kShortMax;
    if ((flags & kUpdatePan) != 0)
    {
        levels.pan = track.timed[kTimedPan].value;
        if (switched(OuterParam::kPan))
        {
            levels.pan = PanCalc(levels.pan, outer->pan, weight, track.pan_calc);
        }
    }
    if ((flags & kUpdateFxmix) != 0)
    {
        levels.fxmix = track.timed[kTimedFxmix].value;
        if (switched(OuterParam::kFxmix))
        {
            levels.fxmix = PanCalc(levels.fxmix, outer->fxmix, weight, track.fxmix_calc);
        }
    }
    if ((flags & kUpdateDolby) != 0)
    {
        levels.dolby = track.timed[kTimedDolby].value;
        if (switched(OuterParam::kDolby))
        {
            levels.dolby = PanCalc(levels.dolby, outer->dolby, weight, track.dolby_calc);
        }
    }

    return levels;
}

// The filters that `flags` names: the IIR's coefficients, the FIR when `outer` switches it on, and the distortion.
void UpdateFilters(Track& track, uint32_t flags, const OuterParam* outer)
{
    if ((flags & kUpdateIir) != 0)
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            track.updater.iir[i] = static_cast<int16_t>(ToInt32(track.timed[kTimedIir + i].value * kShortMax));
        }
        track.updater.filter_mode = static_cast<uint8_t>(track.updater.filter_mode | 0x20);
    }
    if ((flags & kUpdateFir) != 0 && outer != nullptr && outer->Check(OuterParam::kFir))
    {
        track.updater.fir = outer->fir;
        track.updater.filter_mode = static_cast<uint8_t>((track.updater.filter_mode & 0x20) | 8);
    }
    if ((flags & kUpdateDistortion) != 0)
    {
        track.updater.distortion = static_cast<int16_t>(ToInt32(track.timed[kTimedDistortion].value * kShortMax));
    }
}

// Stores the levels that `flags` names in the track's updater. A child track that doesn't inherit them alone mixes
// them with its parent's, `from`.
void StoreLevels(Track& track, const ChannelUpdater* from, uint32_t flags, const TrackLevels& levels)
{
    ChannelUpdater& updater = track.updater;
    if (from == nullptr)
    {
        if ((flags & kUpdateVolume) != 0)
        {
            updater.volume = levels.volume;
        }
        if ((flags & kUpdatePitch) != 0)
        {
            updater.pitch = levels.pitch;
        }
        if ((flags & kUpdatePan) != 0)
        {
            updater.pan = levels.pan;
        }
        if ((flags & kUpdateFxmix) != 0)
        {
            updater.fxmix = levels.fxmix;
        }
        if ((flags & kUpdateDolby) != 0)
        {
            updater.dolby = levels.dolby;
        }
        return;
    }

    const float parent_weight = static_cast<float>(track.registers[kRegParentWeight]) / kShortMax;
    if ((flags & kUpdateVolume) != 0)
    {
        updater.volume = levels.volume * from->volume;
    }
    if ((flags & kUpdatePitch) != 0)
    {
        updater.pitch = levels.pitch * from->pitch;
    }
    if ((flags & kUpdatePan) != 0)
    {
        updater.pan = PanCalc(levels.pan, from->pan, parent_weight, track.parent_pan_calc);
    }
    if ((flags & kUpdateFxmix) != 0)
    {
        updater.fxmix = PanCalc(levels.fxmix, from->fxmix, parent_weight, track.parent_fxmix_calc);
    }
    if ((flags & kUpdateDolby) != 0)
    {
        updater.dolby = PanCalc(levels.dolby, from->dolby, parent_weight, track.parent_dolby_calc);
    }
}

} // namespace

void Tracks::UpdateTimedParam(Track& track)
{
    // The timed part of mainProc: each level that moves takes a step. A level's flag asks for an update, except for
    // the oscillators' 5 settings from index 6, which go straight into the track's settings. Index 11, the second
    // oscillator's vertex, sets its flag and changes nothing.
    for (std::size_t i = 0; i < track.timed.size(); i++)
    {
        MoveParam& param = track.timed[i];
        if (!(param.time > 0.0f))
        {
            continue;
        }

        param.time = param.time + -1.0f;
        param.value = param.amount + param.value;
        switch (i)
        {
        case 6:
            track.oscillators[0].width = param.value;
            break;
        case 7:
            track.oscillators[0].rate = param.value;
            break;
        case 8:
            track.oscillators[0].vertex = param.value;
            break;
        case 9:
            track.oscillators[1].width = param.value;
            break;
        case 10:
            track.oscillators[1].rate = param.value;
            break;
        default:
            track.update_flags |= 1u << i;
            break;
        }
    }
    track.update_flags |= kUpdatePitch;
}

void Tracks::UpdateSeq(uint32_t index, uint32_t flags, bool recursive)
{
    // JASTrack::updateSeq (0x3dcbc4).
    Track& track = tracks_[index];
    uint32_t update = flags | track.update_flags;
    if (track.outer != nullptr)
    {
        update |= track.outer->outer_update;
        track.outer->outer_update = 0;
    }
    track.vibrate.IncCounter();
    track.update_flags = 0;
    if (update != 0)
    {
        UpdateTrack(index, update);
    }

    for (const uint32_t child : track.children)
    {
        if (child == Track::kNone || tracks_[child].status == Track::kClosed)
        {
            continue;
        }
        if (recursive)
        {
            UpdateSeq(child, update, true);
        }
        else
        {
            tracks_[child].update_flags |= update;
        }
    }
}

void Tracks::UpdateTrack(uint32_t index, uint32_t flags)
{
    // JASTrack::updateTrack (0x3ddcbc).
    Track& track = tracks_[index];
    const OuterParam* const outer = track.outer;
    if ((flags & kUpdateDelay) != 0)
    {
        const std::array<uint8_t, 2> delays = DelaySamples(track.timed[kTimedDelay].value);
        track.updater.delays[0] = delays[0];
        track.updater.delays[1] = delays[1];
    }
    if ((flags & kUpdateTempo) != 0 && track.parent == Track::kNone)
    {
        UpdateTempo(index);
    }

    const TrackLevels levels = OwnLevels(track, flags, outer);
    UpdateFilters(track, flags, outer);
    const bool alone = track.parent == Track::kNone || (track.inherit & 1) != 0;
    StoreLevels(track, alone ? nullptr : &tracks_[track.parent].updater, flags, levels);
}

void Tracks::UpdateTrackAll(uint32_t index)
{
    // JASTrack::updateTrackAll (0x3dd770).
    Track& track = tracks_[index];
    const OuterParam* const outer = track.outer;
    const std::array<uint8_t, 2> delays = DelaySamples(track.timed[kTimedDelay].value);
    track.updater.delay_max = 16;
    track.updater.delays[0] = delays[0];
    track.updater.delays[1] = delays[1];

    float volume = track.timed[kTimedVolume].value;
    if (track.volume_mode == 0)
    {
        volume = volume * volume;
    }
    if (track.muted)
    {
        volume = 0.0f;
    }
    float pitch = PitchToCent(track.timed[kTimedPitch].value, static_cast<float>(track.registers[kRegBendRange]));
    pitch = pitch * track.vibrate.Value();
    float pan = track.timed[kTimedPan].value;
    float fxmix = track.timed[kTimedFxmix].value;
    float dolby = track.timed[kTimedDolby].value;

    if (outer != nullptr)
    {
        const float weight = static_cast<float>(track.registers[kRegOuterWeight]) / kShortMax;
        if (outer->Check(OuterParam::kVolume))
        {
            volume = volume * outer->volume;
        }
        if (outer->Check(OuterParam::kPitch))
        {
            pitch = pitch * outer->pitch;
        }
        if (outer->Check(OuterParam::kFxmix))
        {
            fxmix = PanCalc(fxmix, outer->fxmix, weight, track.fxmix_calc);
        }
        if (outer->Check(OuterParam::kDolby))
        {
            dolby = PanCalc(dolby, outer->dolby, weight, track.dolby_calc);
        }
        if (outer->Check(OuterParam::kPan))
        {
            pan = PanCalc(pan, outer->pan, weight, track.pan_calc);
        }
    }

    if (track.parent == Track::kNone || (track.inherit & 1) != 0)
    {
        track.updater.volume = volume;
        track.updater.pitch = pitch;
        track.updater.pan = pan;
        track.updater.fxmix = fxmix;
        track.updater.dolby = dolby;
        return;
    }

    const ChannelUpdater& from = tracks_[track.parent].updater;
    const float parent_weight = static_cast<float>(track.registers[kRegParentWeight]) / kShortMax;
    track.updater.volume = volume * from.volume;
    track.updater.pitch = pitch * from.pitch;
    track.updater.pan = PanCalc(pan, from.pan, parent_weight, track.parent_pan_calc);
    track.updater.fxmix = PanCalc(fxmix, from.fxmix, parent_weight, track.parent_fxmix_calc);
    track.updater.dolby = PanCalc(dolby, from.dolby, parent_weight, track.parent_dolby_calc);
    if (outer != nullptr && outer->Check(OuterParam::kFir))
    {
        track.updater.fir = outer->fir;
        track.updater.filter_mode = 8;
    }
    for (std::size_t i = 0; i < 4; i++)
    {
        track.updater.iir[i] = static_cast<int16_t>(ToInt32(track.timed[kTimedIir + i].value * kShortMax));
    }
    track.updater.filter_mode = static_cast<uint8_t>(track.updater.filter_mode | 0x20);
    track.updater.distortion = static_cast<int16_t>(ToInt32(track.timed[kTimedDistortion].value * kShortMax));
}

void Tracks::UpdateTempo(uint32_t index)
{
    // JASTrack::updateTempo (0x3dc42c).
    Track& track = tracks_[index];
    if (track.parent == Track::kNone)
    {
        float rate = static_cast<float>(track.timebase) * static_cast<float>(track.tempo);
        rate = rate / voices_.DacRate();
        rate = rate * 1.33333337f;
        if (track.outer != nullptr && track.outer->Check(OuterParam::kTempo))
        {
            rate = track.outer->tempo * rate;
        }
        track.tempo_rate = rate;
    }
    else
    {
        track.tempo_rate = tracks_[track.parent].tempo_rate;
        track.timebase = tracks_[track.parent].timebase;
    }

    for (const uint32_t child : track.children)
    {
        if (child != Track::kNone && tracks_[child].status != Track::kClosed)
        {
            UpdateTempo(child);
        }
    }
}

} // namespace ntdlib::jaudio1
