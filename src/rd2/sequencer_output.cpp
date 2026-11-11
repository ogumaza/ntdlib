// SPDX-License-Identifier: MIT

// The output of the R&D2 sequencer in each frame: the writes to the PSG channels, and the mixing of the sample voices.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "sequencer_internal.h"

namespace ntdlib::rd2
{

// Runs the PSG voices' frame: each voice releases when its frames run out, starts its channel on its first frame, and
// then writes its pitch, envelope and duty each frame.
void Sequencer::UpdatePsgVoices()
{
    // The console's PSG voices, then the grown ones.
    for (size_t at = kSampleVoices; at < voices_.size(); at++)
    {
        const int index = int(at);
        Voice& v = voices_[at];
        if (v.type == kSampleType)
        {
            continue;
        }

        if (v.state == kVoicePlaying && v.frames == 0)
        {
            Release(index);
        }
        if (v.state == kVoiceFree)
        {
            continue;
        }

        unit_ = v.unit;
        uint32_t pitch;
        uint8_t pan;
        if (v.state == kVoicePlaying)
        {
            pitch = FramePitch(v);
            v.pitch = pitch;
            pan = v.own_pan ? v.pan : TrackOf(v).pan;
        }
        else
        {
            pitch = v.pitch;
            pan = v.pan;
        }

        const uint8_t envelope = PsgEnvelope(v, pan != kCentre);
        WritePan(v.type - 1, pan);
        if (v.state == kVoicePlaying)
        {
            if (v.position == 0)
            {
                StartPsgVoice(v, envelope);
                v.position = 1;
                v.frames--;
                continue;
            }

            v.position++;
            v.frames--;
        }
        else if (v.type == kWaveType)
        {
            FadeReleasedWave(index, pan);
            continue;
        }

        WriteChannel(v, pitch, envelope, FrameDuty(v));
    }
}

// Writes a PSG channel's pan to NR51: both sides at the centre (0x40), and the left or the right side only otherwise.
void Sequencer::WritePan(int channel, uint8_t pan)
{
    const uint8_t both = uint8_t(0x11 << channel);
    const uint8_t kept = uint8_t(ReadBack(kNr51, 1) & ~both);
    if (pan == kCentre)
    {
        Write(kNr51, 1, kept | both);
    }
    else if (pan < kCentre)
    {
        Write(kNr51, 1, kept | uint8_t(0x10 << channel));
    }
    else
    {
        Write(kNr51, 1, kept | uint8_t(1 << channel));
    }
}

// Fades a released wave voice by itself, through the wave channel's four volumes. The voice stops at volume 0.
void Sequencer::FadeReleasedWave(int index, uint8_t pan)
{
    Voice& v = voices_[size_t(index)];
    v.volume = ReleaseFade(v);
    uint32_t volume = pan != kCentre ? v.volume << 1 : v.volume;
    volume = (volume * 5) >> 7;
    if (volume == 0)
    {
        StopVoice(index);
        return;
    }

    Write(kNr32, 1, rom_.U8(info_.wave_volumes + std::min<uint32_t>(volume, 4)));
}

// Returns the duty or noise width for a PSG voice's frame, from the region's table of them, or 0xFF without a table.
uint8_t Sequencer::FrameDuty(const Voice& v) const
{
    return HasPsgTable(rom_, v.region) ? PsgTableEntry(rom_, v.psg, v.position) : 0xFF;
}

// Writes a PSG voice's frame to its channel: the envelope at a segment's start, the pitch, and the duty or noise width
// from `duty` unless it's 0xFF.
void Sequencer::WriteChannel(const Voice& v, uint32_t pitch, uint8_t envelope, uint8_t duty)
{
    switch (v.type)
    {
    case kSquare1Type:
        // As in the driver, square 1 writes its pitch between restarts only if its NR10 is 8 (no sweep).
        if (envelope != kNoEnvelope)
        {
            Write(kNr12, 1, envelope);
            Write(kNr13, 2, pitch | 0x8000);
        }
        else if (RegionSweep(rom_, v.region) == 8)
        {
            Write(kNr13, 2, pitch);
        }
        Write(kNr11, 1, ReadBack(kNr11, 1) & 0xC0);
        if (duty != 0xFF)
        {
            Write(kNr11, 1, uint32_t(duty) << 6);
        }
        break;

    case kSquare2Type:
        if (envelope != kNoEnvelope)
        {
            Write(kNr22, 1, envelope);
            Write(kNr23, 2, pitch | 0x8000);
        }
        else
        {
            Write(kNr23, 2, pitch);
        }
        Write(kNr21, 1, ReadBack(kNr21, 1) & 0xC0);
        if (duty != 0xFF)
        {
            Write(kNr21, 1, uint32_t(duty) << 6);
        }
        break;

    case kWaveType:
        Write(kNr33, 2, (ReadBack(kNr33, 2) & 0x4000) | (pitch & 0xFFFF));
        if (envelope != kNoEnvelope)
        {
            Write(kNr32, 1, rom_.U8(info_.wave_volumes + envelope));
        }
        break;

    case kNoiseType:
        {
            if (envelope != kNoEnvelope)
            {
                Write(kNr42, 1, envelope);
                Write(kNr44, 1, 0x80);
            }

            const uint8_t shape = NoiseShape(pitch);
            if (duty != 0xFF)
            {
                Write(kNr43, 1, shape | (duty ? 8 : 0));
            }
            else
            {
                Write(kNr43, 1, (ReadBack(kNr43, 1) & 8) | shape);
            }
            break;
        }

    default:
        break;
    }
}

// Starts a PSG voice's channel in the voice's first frame, at the note's pitch, before the frame's slide, bend and LFO.
void Sequencer::StartPsgVoice(Voice& v, uint8_t envelope)
{
    switch (v.type)
    {
    case kSquare1Type:
        Write(kNr10, 1, RegionSweep(rom_, v.region));
        Write(kNr13, 2, v.base_pitch | 0x8000);
        Write(kNr12, 1, envelope);
        Write(kNr11, 1, uint32_t(FirstDuty(v)) << 6);
        Write(kNr13, 2, v.base_pitch | 0x8000);
        break;

    case kSquare2Type:
        // As in the driver, square 2 takes its first duty from v.psg's low byte even when v.psg is a table's address.
        Write(kNr22, 1, envelope);
        Write(kNr23, 2, v.base_pitch | 0x8000);
        Write(kNr21, 1, uint32_t(uint8_t(v.psg)) << 6);
        break;

    case kWaveType:
        if (v.psg != CurrentWave())
        {
            Write(kNr30, 1, 0);
            for (uint32_t i = 0; i < 16; i += 2)
            {
                Write(kWaveRam + i, 2, rom_.U16(v.psg + i));
            }
            CurrentWave() = v.psg;
        }
        Write(kNr30, 1, 0xC0);
        Write(kNr33, 2, v.base_pitch | 0x8000);
        Write(kNr32, 1, rom_.U8(info_.wave_volumes + envelope));
        Write(kNr31, 1, 0);
        break;

    case kNoiseType:
        {
            Write(kNr42, 1, envelope);
            uint8_t shape = NoiseShape(v.base_pitch);
            if (FirstDuty(v) != 0)
            {
                shape |= 8;
            }
            Write(kNr43, 1, shape);
            Write(kNr44, 1, 0x80);
            Write(kNr41, 1, 0);
            break;
        }

    default:
        break;
    }
}

// Returns a square 1 or noise voice's duty or width for its first frame: the first entry of the region's table of them,
// or the low byte of `v.psg` without a table.
uint8_t Sequencer::FirstDuty(const Voice& v) const
{
    return HasPsgTable(rom_, v.region) ? rom_.U8(v.psg + 2) : uint8_t(v.psg);
}

// Returns the NR43 that the noise table gives a noise voice's pitch. A noise voice's pitch is an index into the table.
uint8_t Sequencer::NoiseShape(uint32_t pitch) const
{
    return NoiseTableEntry(rom_, info_, pitch & 0xFFFF);
}

// Runs the mixer's frame: each sample voice that plays, in the list's order, works out its level, pitch and pan and
// mixes its frame. A released voice stops when its level reaches 0, and any voice when its sample ends. Then the voices
// whose frames have run out are released.
void Sequencer::UpdateSampleVoices()
{
    mixes_.clear();
    mix_runs_.clear();
    for (int index = active_; index >= 0;)
    {
        Voice& v = voices_[size_t(index)];
        const int next = v.next;
        const bool starts = v.position == 0 && v.state == kVoicePlaying;
        const uint32_t level = SampleLevel(v);
        uint32_t pitch;
        uint8_t pan;
        if (v.state == kVoicePlaying)
        {
            v.frames--;
            pan = v.own_pan ? v.pan : TrackOf(v).pan;
            pitch = FramePitch(v);
            v.pitch = pitch;
            v.echo = TrackOf(v).echo;
        }
        else if (level == 0)
        {
            StopVoice(index);
            index = next;
            continue;
        }
        else
        {
            pan = v.pan;
            pitch = v.pitch;
        }

        // The Super Mario Advance 2 revision multiplies the step by the frame's points and divides it by them again,
        // which changes it only where the product wraps.
        uint32_t step = UnsignedDiv((pitch >> 2) * rom_.U32(v.sample + 4), kMixRate);
        if (traits_.wrapped_step)
        {
            step = UnsignedDiv(step * kFrameSamples, kFrameSamples);
        }
        step >>= 5;
        if (keep_mixes_)
        {
            mixes_.push_back({index, v.sample, level, step, pan, v.echo, starts, uint32_t(mix_runs_.size()), 0});
        }
        const bool ended = MixVoice(v, step);
        if (keep_mixes_)
        {
            mixes_.back().runs = uint32_t(mix_runs_.size()) - mixes_.back().run;
        }
        if (ended)
        {
            StopVoice(index);
        }
        index = next;
    }

    // The console's sample voices, then the grown ones.
    for (size_t at = 0; at < voices_.size(); at++)
    {
        if (voices_[at].type == kSampleType && voices_[at].state == kVoicePlaying && voices_[at].frames == 0)
        {
            Release(int(at));
        }
    }
}

void Sequencer::KeepRun(int32_t first, int32_t points, uint32_t position)
{
    if (keep_mixes_)
    {
        mix_runs_.push_back({first, points, position});
    }
}

// Moves a sample voice's position on by the frame's 176 points, going back by the loop's length each time it reaches
// the loop's end. Returns true if a sample without a loop has reached its end.
bool Sequencer::MixVoice(Voice& v, uint32_t step)
{
    // The mixer's inner loop mixes 4 points at a time when it's asked for a multiple of 4, and at least one pass.
    auto mixed = [](int32_t count)
    {
        return count > 0 ? count : (count % 4 == 0 ? 4 : 1);
    };

    const uint32_t loop_end = rom_.U32(v.sample + 12);
    const uint32_t end = loop_end ? loop_end : rom_.U32(v.sample);

    // Returns how many of `wanted` points the mixer mixes from `from`, and whether it reaches the end. If it does, it
    // mixes only the points up to the end.
    auto until_end = [&](uint32_t from, int32_t wanted) -> std::pair<int32_t, bool>
    {
        if (((from + step * uint32_t(wanted)) >> 8) < end)
        {
            return {wanted, false};
        }

        return {SignedDiv(int32_t((end << 8) - from - 1 + step), int32_t(step)), true};
    };

    const auto [count, ends] = until_end(v.position, int32_t(kFrameSamples));
    KeepRun(0, mixed(count), v.position);
    uint32_t position = v.position + step * uint32_t(mixed(count));
    if (!loop_end || !ends)
    {
        if (ends)
        {
            return true;
        }

        v.position = position;
        return false;
    }

    // Each time the mix reaches the loop's end, it goes back by the loop's length.
    const uint32_t loop_length = (loop_end - rom_.U32(v.sample + 8)) << 8;
    position -= loop_length;
    int32_t left = int32_t(kFrameSamples) - count;
    int32_t first = count;
    for (int pass = 0; left != 0; pass++)
    {
        if (pass == 1000 || left < 0)
        {
            Warn("a sample's loop is too short for the driver's mixer, which would hang");
            break;
        }

        const auto [part, at_end] = until_end(position, left);
        KeepRun(first, mixed(part), position);
        position += step * uint32_t(mixed(part));
        if (at_end)
        {
            position -= loop_length;
        }
        left -= part;
        first += part;
    }

    v.position = position;

    return false;
}

} // namespace ntdlib::rd2
