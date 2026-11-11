// SPDX-License-Identifier: MIT

// The pitch and the level of each voice in a frame: the pitch steps, the envelopes, the bends, the LFO, and the levels
// of the sample and PSG voices.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sequencer_internal.h"

namespace ntdlib::rd2
{

// Returns a voice's pitch for a note: a sample's step from the pitch table, a square or wave's frequency setting, or a
// noise voice's index into the noise table, at the note's pitch index.
uint32_t Sequencer::PitchStep(uint8_t type, uint8_t note, uint8_t tuning) const
{
    const int index = PitchIndex(note, tuning);
    if (type == kSampleType)
    {
        return rom_.U32(info_.pitch_table + 4 * uint32_t(index));
    }
    if (type == kNoiseType)
    {
        return uint32_t(index);
    }

    return rom_.U16(info_.frequency_table + 2 * uint32_t(index));
}

// Moves a voice's envelope on by a frame, starting its next segment when one ends. A point with a negative length holds
// the level of the point before it. Returns the new level.
int32_t Sequencer::EnvelopeStep(Voice& v)
{
    if (v.segment == 0)
    {
        v.level = v.target;
        const uint8_t last = v.point;
        v.point = uint8_t(last + 1);
        if (rom_.U16(v.envelope + 4 * uint32_t(int32_t(int8_t(v.point)))) & 0x8000)
        {
            v.point = last;
        }

        const uint32_t at = v.envelope + 4 * uint32_t(int32_t(int8_t(v.point)));
        v.target = int16_t(rom_.U16(at + 2));
        v.segment = rom_.U16(at);
        v.level_step = int16_t(SignedDiv(int16_t(uint16_t(v.target - v.level)), v.segment));
    }

    v.level += v.level_step;
    v.segment--;

    return v.level;
}

// Returns a voice's pitch for the frame: its note with the slide, the track's bend and the LFO. A sample's pitch is a
// step that rises with the pitch. A square or wave's is a frequency setting. The bend and LFO work on 2048 less the
// setting: the period.
uint32_t Sequencer::FramePitch(Voice& v)
{
    const Track& t = TrackOf(v);
    AdvanceSlide(v);
    const uint32_t pitch = Bend(v, t, v.base_pitch + uint32_t(v.slide));
    return ApplyLfo(v, t, pitch);
}

// Returns `pitch` with the track's bend.
uint32_t Sequencer::Bend(const Voice& v, const Track& t, uint32_t pitch) const
{
    const int bend = t.bend;
    if (bend == 0)
    {
        return pitch;
    }

    const uint32_t range = rom_.U32(info_.pitch_table + 4 * uint32_t(t.bend_range + kUnityIndex));
    if (traits_.scaled_bend)
    {
        // The Super Mario Advance 2 revision scales the pitch by a factor in 256ths. The factor goes in a straight line
        // from 1 at no bend to the range's step at a bend of 128, both ways.
        const uint32_t factor = uint32_t(int32_t((range - 0x8000) * uint32_t(bend) + 0x400000) >> 14);
        return v.type == kSampleType ? (pitch * factor) >> 8 : 0x800 - UnsignedDiv((0x800 - pitch) << 8, factor);
    }

    const uint32_t scale = uint32_t(bend > 0 ? bend : -bend) * range + 0x400000;
    if (bend > 0)
    {
        return v.type == kSampleType ? (pitch * (scale >> 7)) >> 15 : 0x800 - UnsignedDiv((0x800 - pitch) << 22, scale);
    }
    if (v.type == kSampleType)
    {
        return UnsignedDiv(pitch << 15, scale >> 7);
    }

    pitch = ((0x800 - pitch) * (scale >> 7)) >> 15;
    return pitch > 0x7FF ? 0 : 0x800 - pitch;
}

// Returns `pitch` with the LFO, and moves the LFO on by a frame. The LFO waits out its delay, then follows its sine at
// its rate.
uint32_t Sequencer::ApplyLfo(Voice& v, const Track& t, uint32_t pitch)
{
    const uint32_t depth = t.lfo_depth;
    if (depth == 0)
    {
        return pitch;
    }
    if (v.lfo_delay != 0)
    {
        v.lfo_delay--;
        return pitch;
    }

    const int sine = rom_.S8(info_.lfo_table + (v.lfo_phase >> 1));
    if (v.type == kSampleType)
    {
        if (sine >= 0)
        {
            pitch += (uint32_t(sine) * pitch * depth) >> 19;
        }
        else
        {
            pitch = UnsignedDiv(pitch << 12, ((uint32_t(-sine) * depth) >> 3) + 0x10000) << 4;
        }
    }
    else
    {
        uint32_t period;
        if (sine >= 0)
        {
            period = UnsignedDiv((0x800 - pitch) << 19, uint32_t(sine) * depth + 0x80000);
        }
        else
        {
            period = ((0x800 - pitch) * (uint32_t(-sine) * depth + 0x80000)) >> 19;
        }
        pitch = 0x800 - period;
    }

    v.lfo_phase += t.lfo_rate;
    if ((v.lfo_phase >> 1) > 0xFF)
    {
        v.lfo_phase -= 0x200;
    }

    return pitch;
}

// Returns a sample voice's level for the frame, and keeps its loudness for its release: the velocity, the player's and
// the track's volumes and the envelope while it plays, and a fade at the release's rate after it.
uint32_t Sequencer::SampleLevel(Voice& v)
{
    uint32_t level;
    if (v.state == kVoicePlaying && traits_.older_levels)
    {
        // The Super Mario Advance 2 revision has no second volume for the player. A voice's volume reaches 126 rather
        // than 190.
        const Track& t = TrackOf(v);
        level = uint32_t(v.velocity) << 8;
        level = (level * player_.fade) >> 7;
        level = (level * player_.volume) >> 7;
        level = (level * t.volume) >> 15;
        level = (level * t.volume2) >> 7;
        level = (level * uint32_t(EnvelopeStep(v))) >> 15;
        v.volume = level;
    }
    else if (v.state == kVoicePlaying)
    {
        const Track& t = TrackOf(v);
        level = uint32_t(v.velocity) << 7;
        level = (level * player_.fade) >> 8;
        level = (level * player_.volume) >> 7;
        level = (level * player_.volume2) >> 8;
        level = (level * t.volume) >> 8;
        level = (level * t.volume2) >> 15;
        level = (level * uint32_t(EnvelopeStep(v))) >> 11;
        v.volume = level;
    }
    else
    {
        level = ReleaseFade(v);
        v.volume = level;
    }

    // A Link to the Past's revision multiplies the level by 3 and shifts it right by 9. The other two shift it right
    // by 8.
    return (level * traits_.volume_scale) >> traits_.volume_shift;
}

// Returns a PSG voice's NRx2 for the frame, or for the wave voice its volume (0-4), or 8 to leave the channel as it is.
// The PSG's envelope runs each segment of the voice's: from the level at its start to the level at its end, stepping at
// the rate that covers the difference in the segment's frames.
uint8_t Sequencer::PsgEnvelope(Voice& v, bool panned)
{
    const bool starts = v.segment == 0;
    EnvelopeStep(v);
    if (!starts)
    {
        return kNoEnvelope;
    }

    uint32_t level = PsgLevel(v, panned);
    if (v.type == kWaveType)
    {
        level >>= 22;
        v.volume = level;

        return uint8_t(std::min<uint32_t>((level * 5) >> 7, 4));
    }

    level >>= 15;
    v.volume = level;
    uint32_t start = (level * uint32_t(v.level)) >> 25;
    if (start & ~0xFu)
    {
        start = 0xF;
    }
    v.volume = (v.volume * uint32_t(v.target)) >> 25;
    if (v.volume & ~0xFu)
    {
        v.volume = 0xF;
    }

    const uint32_t end = v.volume;
    if (end == start)
    {
        return uint8_t((start << 4) | 8);
    }

    const uint32_t difference = end > start ? end - start : start - end;
    uint32_t period = uint16_t(SignedDiv(int32_t(uint16_t(v.segment + 15)), int32_t(difference)));
    if (period == 0)
    {
        return uint8_t((start << 4) | 8);
    }
    if (period & 0xFFF8)
    {
        period = 7;
    }

    uint8_t value = uint8_t((start << 4) | period);
    if (start < end)
    {
        value |= 8;
    }

    return value;
}

// Returns a PSG voice's velocity times the track's volumes, the player's volumes and the player's fade. PsgEnvelope
// shifts the product for the voice's channel. The product doubles for a voice panned to one side. Such a voice plays on
// that side only.
uint32_t Sequencer::PsgLevel(const Voice& v, bool panned) const
{
    // A released wave voice has no track, and the driver reads its settings from address 0, where the emulator reads 0.
    Track none;
    none.volume = 0;
    none.volume2 = 0;
    const bool has_track = v.track >= 0;
    const Track& t = has_track ? tracks_[size_t(v.track)] : none;

    uint32_t level = v.velocity;
    if (panned)
    {
        level <<= 1;
    }
    level <<= 15;
    level = (level * t.volume) >> 14;
    level = (level * t.volume2) >> 7;
    if (traits_.player_volume)
    {
        level = (level * (has_track ? player_.volume : 0)) >> 7;
        level = (level * (has_track ? player_.volume2 : 0)) >> 8;
    }
    else
    {
        // The Super Mario Advance 2 revision has no second volume for the player.
        level = (level * (has_track ? player_.volume : 0)) >> 8;
    }

    return level * (has_track ? player_.fade : 0);
}

} // namespace ntdlib::rd2
