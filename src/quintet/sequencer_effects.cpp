// SPDX-License-Identifier: MIT

// The effects that the quintet sequencer runs in each frame: the envelopes, the LFO, the slides, the noise macros, the
// switches and the random numbers that the LFO uses.

#include <cstddef>
#include <cstdint>

#include "sequencer_internal.h"

namespace ntdlib::quintet
{

void Sequencer::ChannelEffects(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (!ch.position)
    {
        return;
    }

    if (ch.envelope_on)
    {
        VolumeEnvelope(c);
    }

    ch.pitch_out = ch.pitch;
    if (ch.bend_on)
    {
        PitchEnvelope(c);
    }
    if (ch.lfo_on)
    {
        Lfo(c);
    }
    if (ch.sliding)
    {
        Slide(c);
    }

    if (ch.macro)
    {
        NoiseMacro(c);
    }
    if (ch.switch_count)
    {
        Switch(c);
    }
}

void Sequencer::PcmEffects(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (!ch.position)
    {
        return;
    }

    ch.pitch_out = ch.pitch;
    if (ch.bend_on)
    {
        PitchEnvelope(c);
    }
    if (ch.lfo_on)
    {
        Lfo(c);
    }
    if (ch.sliding)
    {
        Slide(c);
    }

    // Only a channel whose pitch moves sets its FIFO's rate again.
    if (ch.bend_on || ch.lfo_on || ch.sliding)
    {
        BendFifo(ch.pitch_out, c - kPcmA);
    }
}

void Sequencer::VolumeEnvelope(int c)
{
    Channel& ch = channels_[size_t(c)];
    const int32_t t = ch.envelope_time;
    if (traits_.idle_after_rest && t == kAfterRest)
    {
        return;
    }

    const int32_t start = ch.envelope[0];
    const int32_t peak = ch.envelope[1];
    const int32_t end = ch.envelope[2];
    const int32_t attack = ch.envelope[3];
    const int32_t decay = ch.envelope[4];

    // The level goes in straight lines from the start to the peak over the attack, and then to the end over the decay.
    // The A revision then leaves the volume alone, and the J revision goes on applying the end level.
    int32_t level = end;
    if (t <= attack)
    {
        level = start + Divide((peak - start) * t, attack);
    }
    else if (t <= attack + decay)
    {
        level = peak + Divide((end - peak) * (t - attack), decay);
    }
    else if (!traits_.envelope_holds)
    {
        return;
    }

    // It scales the channel's volume. A square or noise channel restarts at the new volume when it changes, and the
    // wave channel takes the nearest of its volume codes: in the J revision, 75% for 3 and 100% for 4, and silence
    // above.
    const int32_t volume = Divide(int8_t(ch.volume) * int32_t(uint16_t(level)), 100);
    const uint16_t bits = uint16_t((volume & 15) << 12);
    if (c == kWave)
    {
        const int wave_level = bits >> 12;
        const uint8_t wave_code =
            wave_level < traits_.wave_levels ? traits_.wave_codes[size_t(wave_level)] : traits_.envelope_wave_above;
        const uint16_t code = uint16_t(wave_code << 13);
        const uint16_t mask = traits_.envelope_wave_mask;
        if ((image_[kNr31] & mask) != code)
        {
            image_[kNr31] = uint16_t((image_[kNr31] & ~mask) | code);
        }
    }
    else
    {
        uint16_t& envelope = image_[size_t(kPsgEnvelope[c])];
        if ((envelope & 0xF000) != bits)
        {
            envelope = uint16_t((envelope & 0x0FFF) | bits);
            image_[size_t(kPsgControl[c])] |= kRestart;
        }
    }

    ch.envelope_time++;
}

void Sequencer::PitchEnvelope(int c)
{
    Channel& ch = channels_[size_t(c)];
    const int32_t t = ch.bend_time;
    if (traits_.idle_after_rest && t == kAfterRest)
    {
        return;
    }

    const int32_t start = ch.bend[0];
    const int32_t middle = ch.bend[1];
    const int32_t end = ch.bend[2];
    const int32_t first = ch.bend[3];
    const int32_t second = ch.bend[4];

    // The offset goes in straight lines from the start to the middle, and then to the end, where it stays.
    int32_t offset = end;
    if (t <= first)
    {
        offset = start + Divide((middle - start) * t, first);
    }
    else if (t <= first + second)
    {
        offset = middle + Divide((end - middle) * (t - first), second);
    }

    AddPitch(c, int16_t(offset));
    ch.bend_time++;
}

void Sequencer::Lfo(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (ch.lfo_time == kAfterRest)
    {
        return;
    }

    if (ch.lfo_time >= ch.lfo_delay)
    {
        // The LFO's phase, 0-255, over its period.
        const int32_t phase = Remainder(ch.lfo_time - ch.lfo_delay, ch.lfo_period);
        uint16_t v = uint16_t(Divide(int32_t(int16_t(phase)) * 256, ch.lfo_period));
        const int16_t s = int16_t(v);
        switch (ch.lfo_shape & 0x7F)
        {
        case 1:
            // Triangle.
            v = s > 0x7F ? uint16_t(0xFF - v) : v;
            break;

        case 2:
            // Rising saw.
            v = uint16_t(Half(s));
            break;

        case 3:
            // Falling saw.
            v = uint16_t(Half(0xFF - s));
            break;

        case 4:
            v = s > 0x7F ? 0x7F : 0;
            break;

        case 5:
            v = s > 0x7F ? 0 : 0x7F;
            break;

        case 6:
            // A new random value each period, in its second half.
            if (s > 0x7F)
            {
                if (!random_high_)
                {
                    random_high_ = 1;
                    NextRandom();
                }
                v = random_value_;
            }
            else
            {
                v = 0;
                random_high_ = 0;
            }
            break;

        case 7:
            // A new random value each half period.
            if ((s > 0x7F) != (random_high_ != 0))
            {
                random_high_ = s > 0x7F;
                NextRandom();
            }
            v = random_value_;
            break;

        default:
            // Sine, also for shapes above 7.
            v = uint16_t(rom_.S8(info_.lfo_table + uint32_t(int32_t(s))));
            break;
        }

        if (ch.lfo_shape & 0x80)
        {
            v = uint16_t(-v);
        }

        int32_t m = int32_t(int16_t(v)) * ch.lfo_depth;
        if (m < 0)
        {
            m += 0x7F;
        }

        AddPitch(c, int16_t(uint16_t(m >> 7)));
    }

    ch.lfo_time++;
}

void Sequencer::AddPitch(int c, int16_t offset)
{
    // On a square or wave channel the pitch is a frequency setting. The offset moves the setting directly and keeps it
    // within the register's range. On a PCM channel the pitch is a PCM pitch. In the A revision, the offset to a PCM
    // pitch counts double. The noise channel's pitch takes the offset in the same way as a PCM pitch. Nothing reads the
    // noise channel's pitch.
    Channel& ch = channels_[size_t(c)];
    if (c <= kWave)
    {
        int32_t f = int16_t(uint16_t(ch.pitch_out + offset));
        f = f < 0 ? 0 : f > 0x7FF ? 0x7FF : f;
        ch.pitch_out = uint16_t(f);
        uint16_t& reg = image_[size_t(kPsgControl[c])];
        reg = uint16_t((reg & 0xF800) | f);
    }
    else
    {
        ch.pitch_out = uint16_t(ch.pitch_out + offset * traits_.pcm_offset_factor);
    }
}

void Sequencer::Slide(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (ch.slide_time <= ch.slide_frames)
    {
        const int32_t from = int16_t(ch.pitch_from);
        const int32_t to = int16_t(ch.pitch);
        const int32_t p = from + Divide((to - from) * int32_t(ch.slide_time), ch.slide_frames);
        if (c <= kWave)
        {
            uint16_t& reg = image_[size_t(kPsgControl[c])];
            reg = uint16_t((reg & 0xF800) | uint32_t(p));
        }
        else if (c >= kPcmA)
        {
            ch.pitch_out = uint16_t(p);
        }
    }

    ch.slide_time++;
}

void Sequencer::NoiseMacro(int c)
{
    Channel& ch = channels_[size_t(c)];
    uint16_t& nr41 = image_[kNr41];
    uint16_t& nr43 = image_[kNr43];
    const uint16_t length = uint16_t(ch.length_on << 14);

    // Hold the setting for a few more frames.
    if (ch.macro_count)
    {
        nr43 = uint16_t(length | rom_.U8(ch.macro) | (nr43 & kRestart));
        if (--ch.macro_count == 0)
        {
            ch.macro++;
        }
        return;
    }

    // FF ends the macro, silencing the channel; FD n holds each setting for n frames; FE v sets the envelope and
    // restarts the channel; any other byte is the frame's NR43 setting.
    for (int bytes = 0; bytes < kMaxMacroBytes; bytes++)
    {
        const uint8_t b = rom_.U8(ch.macro);
        if (b == 0xFF)
        {
            ch.macro = 0;
            nr41 &= 0x0FFF;
            nr43 = uint16_t(length | kRestart);
            return;
        }
        if (b == 0xFD)
        {
            ch.macro_hold = uint8_t(rom_.U8(ch.macro + 1) - 1);
            ch.macro += 2;
            continue;
        }
        if (b == 0xFE)
        {
            nr41 = uint16_t(rom_.U8(ch.macro + 1) << 8 | (nr41 & 0xFF));
            nr43 |= kRestart;
            ch.macro += 2;
            continue;
        }

        nr43 = uint16_t(b | length | (nr43 & kRestart));
        if (ch.macro_hold)
        {
            ch.macro_count = ch.macro_hold;
        }
        else
        {
            ch.macro++;
        }
        return;
    }

    ch.macro = 0;
    Warn("a noise macro never reaches a setting, so it was stopped");
}

void Sequencer::Switch(int c)
{
    Channel& ch = channels_[size_t(c)];
    if (--ch.switch_count != 0)
    {
        return;
    }

    switch (c)
    {
    case kSquare1:
    case kSquare2:
        {
            uint16_t& envelope = image_[size_t(kPsgEnvelope[c])];
            envelope = uint16_t((envelope & 0xFF3F) | ch.switch_to << 6);
            break;
        }

    case kWave:
        image_[kNr30] = uint16_t(ch.wave_size << 5 | ch.switch_to << 6 | ch.wave_on << 7);
        break;

    default:
        break;
    }
}

void Sequencer::NextRandom()
{
    random_seed_ = random_seed_ * 0x41C64E6D + 0x3039;
    random_value_ = uint8_t((random_seed_ >> 16) & 0x7F);
}

void Sequencer::Reseed()
{
    random_seed_ = 1;
    random_value_ = 0;
    random_high_ = 0;
}

} // namespace ntdlib::quintet
