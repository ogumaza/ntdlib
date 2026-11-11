// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/wave/sinc.h"
#include "sequencer_shared.h"

namespace ntdlib::mp2k
{

namespace
{

// Adds `value` times `volume`, over 256 and rounded down, to a byte of the mixer's output. The byte wraps round.
void AddPoint(int8_t& out, int32_t value, int volume)
{
    out = int8_t(uint8_t(out) + uint8_t((value * volume) >> 8));
}

} // namespace

void Sequencer::StartMixing()
{
    if (dma_period_ < 2)
    {
        throw std::invalid_argument("the mixer's buffer doesn't hold two frames' parts");
    }
    if (info_.camelot_mixer &&
        (info_.camelot_tap <= 0 || info_.camelot_tap >= info_.samples_per_frame || info_.camelot_tap % 2 != 0))
    {
        throw std::invalid_argument("the echo of Camelot's mixer doesn't reach back within a frame");
    }

    // Camelot's output stage can write two points for each of its mixer's points, into buffers twice as long.
    mixing_ = true;
    pcm_.assign(size_t(2 * kPcmBufferSize * PointsPerMixed()), 0);
    sums_.assign(size_t(2 * kMaxTracks * info_.samples_per_frame), 0);
    if (info_.camelot_mixer)
    {
        camelot_sums_.assign(size_t(info_.samples_per_frame), 0);
        camelot_channels_ = 0;
        camelot_last_part_ = 0;
    }
}

int Sequencer::PointsPerMixed() const
{
    return info_.camelot_mixer ? info_.camelot_output_points : 1;
}

std::span<const int8_t> Sequencer::Right() const
{
    if (!mixing_)
    {
        return {};
    }

    return std::span<const int8_t>(pcm_).subspan(part_, size_t(info_.samples_per_frame * PointsPerMixed()));
}

std::span<const int8_t> Sequencer::Left() const
{
    if (!mixing_)
    {
        return {};
    }

    const size_t size = kPcmBufferSize * size_t(PointsPerMixed());
    return std::span<const int8_t>(pcm_).subspan((info_.mono ? 0 : size) + part_,
                                                 size_t(info_.samples_per_frame * PointsPerMixed()));
}

void Sequencer::RunDirect()
{
    const bool metering = mixing_ && metering_;
    if (mixing_)
    {
        StartPart();
    }
    if (enhanced_)
    {
        StartEnhancedPart();
    }
    if (metering)
    {
        std::fill(sums_.begin(), sums_.end(), int16_t(0));
    }

    // The console's DirectSound channels, then the grown ones.
    for (int i = 0; i < ChannelCount(); i++)
    {
        Channel& c = channels_[size_t(i)];
        if ((i >= direct_channels_ && (i < kChannelCount || c.part)) || !(c.status & kStatusOn))
        {
            continue;
        }

        // The mixer starts a note's sample, then moves its envelope on: up by the attack each frame, down by the decay
        // to the sustain level, and down by the release after the note's release, each a fraction out of 256. After the
        // release, the note can carry on at its echo's level for the echo's length.
        Wave wave;
        ReadWave(rom_, c.wave, wave);
        int envelope = c.envelope;
        bool echo = false;
        const bool starting = (c.status & kStatusStart) != 0;
        if (starting)
        {
            if (c.status & kStatusStop)
            {
                c.status = 0;
                EndNote(i, false);
                continue;
            }

            // The enhanced mode counts the position from the sample's first point. The note starts c.count points in.
            enhanced_channels_[size_t(i)].position = uint64_t(uint32_t(c.count)) << 23;
            c.status = kPhaseAttack;
            c.position = wave.Data() + uint32_t(c.count);
            c.count = int32_t(wave.size - uint32_t(c.count));
            c.fraction = 0;
            envelope = 0;
            if (wave.loops)
            {
                c.status |= kStatusLoop;
            }
        }
        else if (c.status & kStatusEcho)
        {
            if (c.echo_length-- <= 1)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }
        }
        else if (c.status & kStatusStop)
        {
            // Camelot's mixer takes 256 less the release off the level each frame, in a straight line.
            envelope = info_.camelot_mixer ? envelope + c.release - 256 : (envelope * c.release) >> 8;
            echo = info_.metroid_sequencer ? envelope <= 0 : envelope <= c.echo_volume;
        }
        else if ((c.status & kStatusEnvelope) == kPhaseDecay)
        {
            envelope = (envelope * c.decay) >> 8;
            if (envelope <= c.sustain)
            {
                envelope = c.sustain;
                if (envelope == 0)
                {
                    echo = true;
                }
                else
                {
                    c.status--;
                }
            }
        }

        if ((c.status & (kStatusStop | kStatusEcho | kStatusEnvelope)) == kPhaseAttack)
        {
            envelope += c.attack;
            if (envelope >= 255)
            {
                envelope = 255;
                c.status--;

                // The Metroid games' mixer moves the level down by the decay in the frame that the attack ends.
                if (info_.metroid_sequencer)
                {
                    envelope = (envelope * c.decay) >> 8;
                    if (envelope <= c.sustain)
                    {
                        envelope = c.sustain;
                        if (envelope == 0)
                        {
                            echo = true;
                        }
                        else
                        {
                            c.status--;
                        }
                    }
                }
            }
        }
        // The Metroid games' mixer ends a note whose level reaches 0, unless it has both an echo volume and an echo
        // length. Such a note is silent for that frame, then plays on at its last level for the length less 1
        // (0x080010EA in Metroid Fusion).
        if (echo && info_.metroid_sequencer)
        {
            if (!c.echo_volume || !c.echo_length)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }
            c.status |= kStatusEcho;
            continue;
        }
        if (echo)
        {
            envelope = c.echo_volume;
            if (envelope == 0)
            {
                c.status = 0;
                EndNote(i, true);
                continue;
            }

            c.status |= kStatusEcho;
        }

        c.envelope = uint8_t(envelope);
        CheckAudible(i);

        // A muted channel plays at volume 0.
        const int owner = owners_[size_t(i)];
        const bool muted = owner >= 0 && (muted_tracks_ >> owner & 1);
        const int meter = metering && !muted ? owner : -1;

        // Camelot's mixer plays each side at 9/8 of the envelope. It leaves out a note that comes out silent on both
        // sides. Such a note doesn't move on through its sample (0x03000846 to 0x03000888 in Golden Sun: The Lost Age).
        if (info_.camelot_mixer)
        {
            const int level = envelope + (envelope >> 3);
            const uint32_t right = uint32_t((c.right * level) >> 9);
            const uint32_t left = uint32_t((c.left * level) >> 9);
            if (right == 0 && left == 0)
            {
                enhanced_channels_[size_t(i)].volumes = {};
                continue;
            }
            if (mixing_)
            {
                // The enhanced mode's volumes: each side's volume at 9/8 of the envelope, over 512, and the output
                // stage's 128 to a point.
                if (enhanced_)
                {
                    const float scale = muted ? 0.0f : float(envelope) * (9.0f / 8.0f) / (512.0f * 128.0f);
                    MixEnhanced(i, c, wave, {float(c.right) * scale, float(c.left) * scale}, starting);
                }
                MixCamelot(c, wave, muted ? 0 : right | left << 16, meter);
                if (!c.status)
                {
                    EndNote(i, true);
                }
                continue;
            }
        }

        // The master volume scales the envelope's level, and the level scales each side's volume. The mono mixer plays
        // the channel at the average of its volumes.
        const int level = muted ? 0 : (envelope * (info_.master_volume + 1)) >> 4;
        if (enhanced_)
        {
            // The enhanced mode's volumes: the master volume's share of the envelope, times each side's volume, over
            // 2^20. The mono mixer's one side takes the average of the two.
            const float scale = muted ? 0.0f : float(envelope * (info_.master_volume + 1)) / 1048576.0f;
            const std::array<float, 2> volumes =
                info_.mono ? std::array<float, 2>{float(c.right + c.left) * scale * 0.5f,
                                                  float(c.right + c.left) * scale * 0.5f}
                           : std::array<float, 2>{float(c.right) * scale, float(c.left) * scale};
            MixEnhanced(i, c, wave, volumes, starting);
        }
        if (info_.mono)
        {
            const int volume = ((c.right + c.left) * level) >> 9;
            MixChannel(c, wave, volume, volume, meter);
        }
        else
        {
            MixChannel(c, wave, (c.right * level) >> 8, (c.left * level) >> 8, meter);
        }
        if (!c.status)
        {
            EndNote(i, true);
        }
    }

    if (mixing_ && info_.camelot_mixer)
    {
        OutputCamelot();
    }
    if (enhanced_)
    {
        EndEnhancedPart();
    }

    // The mono mixer's left side is its right side.
    peaks_ = {};
    if (metering)
    {
        const size_t points = size_t(info_.samples_per_frame);
        for (size_t t = 0; t < tracks_.size(); t++)
        {
            for (size_t side = 0; side < 2; side++)
            {
                const int16_t* sums = &sums_[(2 * t + (info_.mono ? 0 : side)) * points];
                for (size_t i = 0; i < points; i++)
                {
                    peaks_[t][1 - side] = std::max<int32_t>(peaks_[t][1 - side], std::abs(sums[i]));
                }
            }
        }
    }
}

bool Sequencer::MixerSilent() const
{
    const auto silent = [](std::span<const int8_t> points)
    {
        return std::all_of(points.begin(), points.end(), [](int8_t point) { return point == 0; });
    };
    if (!mixing_)
    {
        return true;
    }

    // Camelot's echo comes from the buffer's earlier output, and from the next frame's sums that the output stage
    // starts. With one point for each of the mixer's, the stage starts each half of a silent sum at 64 less, and its
    // rounding gives the 64 back.
    if (info_.camelot_mixer)
    {
        const uint32_t start = info_.camelot_output_points == 1 ? 0xFFBFFFC0 : 0;
        return silent(pcm_) && std::all_of(camelot_sums_.begin(), camelot_sums_.end(),
                                           [start](uint32_t s) { return s == 0 || s == start; });
    }
    return reverb_ ? silent(pcm_) : silent(Right()) && silent(Left());
}

void Sequencer::StartPart()
{
    // The frame's part follows the parts that the DMA has played since it last started the buffer: the buffer's start
    // when the DMA starts it again in the next frame. Camelot's SoundMain works out the place in the same way, from the
    // copy of the DMA counter that the game's VBlank interrupt makes (0x08000682 in Golden Sun: The Lost Age).
    const uint32_t points = uint32_t(info_.samples_per_frame);
    part_ = dma_counter_ > 1 ? (uint32_t(dma_period_) - (dma_counter_ - 1u)) * points * uint32_t(PointsPerMixed()) : 0;
    if (info_.camelot_mixer)
    {
        return;
    }

    int8_t* right = &pcm_[part_];
    int8_t* left = right + kPcmBufferSize;
    if (!reverb_)
    {
        std::fill(right, right + points, int8_t(0));
        if (!info_.mono)
        {
            std::fill(left, left + points, int8_t(0));
        }
        return;
    }

    // The reverb adds both sides' points of the part, from the DMA's last pass over the buffer, to those of the next
    // part, or of the buffer's first part after its last. The part's points on both sides take the sum times the
    // reverb, over 512 and rounded down. A result whose bit 7 is set goes up by 1. The mono mixer adds its one side's
    // points, and takes the sum times the reverb over 256.
    const uint32_t next = dma_counter_ == 2 ? 0 : part_ + points;
    if (info_.mono)
    {
        for (uint32_t i = 0; i < points; i++)
        {
            int32_t value = ((right[i] + pcm_[next + i]) * reverb_) >> 8;
            if (value & 0x80)
            {
                value++;
            }
            right[i] = int8_t(value);
        }
        return;
    }

    for (uint32_t i = 0; i < points; i++)
    {
        const int32_t sum = right[i] + left[i] + pcm_[next + i] + pcm_[kPcmBufferSize + next + i];
        int32_t value = (sum * reverb_) >> 9;
        if (value & 0x80)
        {
            value++;
        }
        right[i] = int8_t(value);
        left[i] = int8_t(value);
    }
}

void Sequencer::MixChannel(Channel& c, const Wave& wave, int right, int left, int meter)
{
    const int samples = info_.samples_per_frame;

    // Each point that the channel plays adds to the frame's part on each side, or on the mono mixer's one side. A
    // track's sums for its meter add the points without wrapping round.
    int8_t* out_right = mixing_ ? &pcm_[part_] : nullptr;
    int8_t* out_left = mixing_ && !info_.mono ? &pcm_[kPcmBufferSize + part_] : nullptr;
    int16_t* sums_right = meter >= 0 ? &sums_[size_t(2 * meter * samples)] : nullptr;
    int16_t* sums_left = sums_right ? sums_right + samples : nullptr;
    const auto mix = [&](int i, int32_t value)
    {
        if (out_right)
        {
            AddPoint(out_right[i], value, right);
        }
        if (out_left)
        {
            AddPoint(out_left[i], value, left);
        }
        if (sums_right)
        {
            sums_right[i] = int16_t(sums_right[i] + ((value * right) >> 8));
            sums_left[i] = int16_t(sums_left[i] + ((value * left) >> 8));
        }
    };

    // Camelot's mixer takes a sample that has no points left when it comes to mix it, as one of no length is from the
    // start, as a synth voice from then on, unless it plays at a fixed pitch.
    if (info_.camelot_mixer && !(c.type & kVoiceFixed) && (c.synth || c.count == 0))
    {
        c.synth = true;
        MixSynth(c);
        return;
    }

    // A newer driver's mixer plays a reversed sample backwards from its end, and counts its way through a compressed
    // sample by the point rather than the address. A compressed voice with an uncompressed sample isn't mixed.
    const bool special = info_.special_samples && (c.type & (kVoiceReverse | kVoiceCompressed));
    const bool reverse = special && (c.type & kVoiceReverse);
    if (special && !(c.status & kStatusSpecial))
    {
        c.status |= kStatusSpecial;
        if (reverse)
        {
            c.position = 2 * c.wave + 32 + wave.size - c.position;
        }
        if (wave.type != 0)
        {
            c.position -= c.wave + 16;
        }
    }
    if (special && !reverse && wave.type == 0)
    {
        return;
    }

    // Otherwise a fixed-pitch sample plays a point for each point of the output. The driver takes a loop of no length
    // as no loop.
    const uint32_t loop_start = special ? wave.loop_start : wave.Data() + wave.loop_start;
    const int32_t loop_length = int32_t(wave.size - wave.loop_start);
    const bool loops = !reverse && (c.status & kStatusLoop) && loop_length > 0;
    if (!special && (c.type & kVoiceFixed))
    {
        for (int i = 0; i < samples; i++)
        {
            mix(i, rom_.S8(c.position));
            c.position++;
            if (--c.count == 0)
            {
                if (!loops)
                {
                    c.status = 0;
                    return;
                }

                c.position = loop_start;
                c.count = loop_length;
            }
        }

        return;
    }

    // Other samples move on by their rate, in 2^-23 points, for each point of the output. A sample that ends stops the
    // channel there, without saving its progress, and a loop goes back by its length as many times as it takes. A
    // reversed sample doesn't loop.
    //
    // Each point of the output lies between two of the sample's points, at the fraction. A reversed sample's position
    // is one past its point, and its next point lies before that. The fraction keeps the bits above 29 that the steps
    // carry into it, and the product with the difference wraps round at 32 bits.
    const uint32_t step = special && (c.type & kVoiceFixed) ? 0x800000u : c.frequency * info_.step_scale;
    const bool compressed = special && wave.type != 0;
    DecodedBlock block;
    const auto point = [&](uint32_t at)
    {
        return compressed ? CompressedPoint(rom_, c.wave, at, block) : int32_t(rom_.S8(at));
    };
    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t position = c.position;
    for (int i = 0; i < samples; i++)
    {
        if (out_right)
        {
            const int32_t s0 = reverse ? point(position - 1) : point(position);
            const int32_t s1 = reverse ? point(position - 2) : point(position + 1);
            mix(i, s0 + (int32_t(fraction * uint32_t(s1 - s0)) >> 23));
        }

        fraction += step;
        const int32_t advance = int32_t(fraction >> 23);
        if (advance == 0)
        {
            continue;
        }

        fraction &= ~0x3F800000u;
        count -= advance;
        if (count > 0)
        {
            position = reverse ? position - uint32_t(advance) : position + uint32_t(advance);
            continue;
        }
        if (!loops)
        {
            c.status = 0;
            return;
        }

        int32_t overshoot = -count;
        count += loop_length;
        while (count <= 0)
        {
            overshoot -= loop_length;
            count += loop_length;
        }

        position = loop_start + uint32_t(overshoot);
    }

    c.fraction = fraction;
    c.count = count;
    c.position = position;
}

void Sequencer::MixSynth(Channel& c) const
{
    // A synth voice's data follows its sample's 16-byte header. It starts 0x80 and its type: 0 for a pulse wave, 1 for
    // a saw wave, and anything else for a triangle wave.
    const uint32_t data = c.wave + 16;
    const int type = int8_t(rom_.U8(data + 1));
    const uint32_t step = SynthStep(c.frequency, info_.step_scale);
    const int samples = info_.samples_per_frame;

    // A pulse wave's duty follows a triangle wave whose phase is the top byte of the count, which moves on by the
    // fourth byte of the sample's data each frame.
    if (type == 0)
    {
        c.count = int32_t(uint32_t(c.count) + DutyStep(rom_, data));
        c.fraction += step * uint32_t(samples);
        return;
    }

    // A saw wave goes through a filter that adds half its last output to each point, which the count keeps.
    if (type == 1)
    {
        int32_t filter = c.count;
        for (int i = 0; i < samples; i++)
        {
            c.fraction += step;
            filter = FilterSaw(SawPoint(c.fraction), filter);
        }

        c.count = filter;
        return;
    }

    c.fraction += step * uint32_t(samples);
}

bool Sequencer::CamelotSynthPoints(Channel& c, uint32_t step, int32_t* points, int count) const
{
    const int type = rom_.S8(c.position + 1);
    if (type == 0)
    {
        // A pulse wave's duty follows a triangle wave, at the top of the count. The wave is high below the duty
        // (0x03000eec).
        c.count = int32_t(uint32_t(c.count) + (uint32_t(rom_.U8(c.position + 3)) << 24));
        uint32_t phase = uint32_t(c.count) + (uint32_t(rom_.U8(c.position + 5)) << 24);
        if (int32_t(phase) < 0)
        {
            phase = ~phase;
        }
        const uint32_t duty = (phase >> 8) * rom_.U8(c.position + 4) + (uint32_t(rom_.U8(c.position + 2)) << 24);
        for (int i = 0; i < count; i++)
        {
            points[i] = c.fraction < duty ? 64 : -64;
            c.fraction += step << 3;
        }
        return false;
    }
    if (type == 1)
    {
        // A saw wave goes through its filter.
        int32_t filter = c.count;
        for (int i = 0; i < count; i++)
        {
            c.fraction += step << 3;
            filter = FilterSaw(SawPoint(c.fraction), filter);
            points[i] = filter;
        }
        c.count = filter;
        return true;
    }

    // A triangle wave (0x03001044).
    for (int i = 0; i < count; i++)
    {
        c.fraction += step << 3;
        const int32_t phase = int32_t(c.fraction);
        points[i] = phase >= 0 ? (phase >> 23) - 128 : 384 - int32_t(c.fraction >> 23);
    }
    return false;
}

bool Sequencer::PlaysCamelotSteps(const Channel& c) const
{
    return info_.camelot_adpcm && (c.type & kVoiceFixed) && (c.count < 0 || (c.type & kVoiceCompressed));
}

const std::vector<float>& Sequencer::CamelotStepPoints(const Wave& wave)
{
    std::shared_ptr<const std::vector<float>>& known = camelot_step_points_[wave.address];
    if (known)
    {
        return *known;
    }

    const std::vector<int32_t> points = DecodeCamelotSteps(rom_, wave, uint32_t(info_.samples_per_frame));
    known = std::make_shared<const std::vector<float>>(points.begin(), points.end());
    return *known;
}

void Sequencer::MixCamelot(Channel& c, const Wave& wave, uint32_t volumes, int meter)
{
    // The addresses below are those of the mixer's code in Golden Sun: The Lost Age, in IWRAM. The mixer counts the
    // channels that it mixes in a frame (0x0300088c). Each point adds the volumes times the point to the point's sums,
    // in a word that wraps round at 32 bits. A negative right side borrows from the left.
    camelot_channels_++;
    const int points = info_.samples_per_frame;
    uint32_t* sums = camelot_sums_.data();
    int16_t* meter_right = meter >= 0 ? &sums_[size_t(2 * meter * points)] : nullptr;
    int16_t* meter_left = meter_right ? meter_right + points : nullptr;
    const auto add = [&](int i, uint32_t volume, int32_t value)
    {
        sums[i] += volume * uint32_t(value);
        if (meter_right)
        {
            meter_right[i] = int16_t(meter_right[i] + int32_t((int64_t(volume & 0xFFFF) * value) >> 7));
            meter_left[i] = int16_t(meter_left[i] + int32_t((int64_t(volume >> 16) * value) >> 7));
        }
    };

    // A looped sample goes back to its loop's start at its end. A sample without a loop stops the channel there, and
    // so does a loop of no length (0x0300085a).
    const uint32_t loop_start = wave.Data() + wave.loop_start;
    const int32_t loop_length = (c.status & kStatusLoop) ? int32_t(wave.size - wave.loop_start) : 0;

    // A fixed-pitch sample plays a point for each point of the output, at the channel's volumes (0x03000c1c).
    if (c.type & kVoiceFixed)
    {
        // A mixer with 4-bit steps plays a sample of a negative size in steps, a byte for each two points. Such a
        // sample has no loop. The channel's fraction keeps the steps' state between frames: the index in its low bits
        // and the point above them. The voice's type marks the channel (0x03000c1c to 0x03000da0 in Mario Tennis: Power
        // Tour). A negative count takes the mark and the first state.
        if (PlaysCamelotSteps(c))
        {
            uint32_t state = c.fraction;
            if (c.count < 0)
            {
                c.type |= kVoiceCompressed;
                c.count = -c.count;
                state = kCamelotStepsStart;
            }

            int32_t point = int32_t(state) >> 16;
            uint32_t index = state & 0xFF;
            const int mixed = std::min(c.count, points);
            c.count -= mixed;
            int i = 0;
            do
            {
                const uint32_t byte = uint32_t(rom_.S8(c.position++));
                CamelotStep(byte << 24 & 0xF0000000u, point, index);
                add(i, volumes, point);
                CamelotStep(byte << 28, point, index);
                add(i + 1, volumes, point);
                i += 2;
            } while (i < mixed);

            c.fraction = index | uint32_t(point) << 16;
            if (c.count == 0)
            {
                c.status = 0;
            }
            return;
        }

        for (int i = 0; i < points; i++)
        {
            add(i, volumes, rom_.S8(c.position));
            c.position++;
            if (--c.count == 0)
            {
                if (loop_length == 0)
                {
                    c.status = 0;
                    return;
                }
                c.count = loop_length;
                c.position = loop_start;
            }
        }
        return;
    }

    // A sample with no points left becomes a synth voice (0x030008a4). Its data follows at its position: 0x80, its
    // type, and a pulse wave's duty, speed, depth and offset. The phase moves on by 8 times a sample's step.
    const uint32_t step = c.frequency * info_.step_scale;
    if (c.synth || c.count == 0)
    {
        // A saw wave plays at half the volumes, rounded down on each side (0x03000fc0).
        c.synth = true;
        std::array<int32_t, kMostFramePoints> synth_points;
        const bool saw = CamelotSynthPoints(c, step, synth_points.data(), points);
        const uint32_t volume = saw ? (volumes >> 1) & ~0xFF00u : volumes;
        for (int i = 0; i < points; i++)
        {
            add(i, volume, synth_points[size_t(i)]);
        }
        return;
    }

    // Other samples play at each side's volume halved, rounded up (0x030008d0). Each point lies between two of the
    // sample's points, at the fraction in 2^-23 points.
    uint32_t volume = ((volumes >> 1) + 0x8000 + (volumes & 1)) & ~0xFF00u;
    uint32_t fraction = c.fraction;
    int32_t count = c.count;
    uint32_t next = c.position + 1; // the address of the next point
    int32_t point = rom_.S8(c.position);
    int32_t difference = rom_.S8(next) - point;

    // When the sample has more points left than the frame takes, the mixer runs a loop of 8 points with the code of
    // its step: below 1, from 1 to 2, or from 2 up (0x03000940). The code below 1 keeps the fraction less 1 and
    // interpolates to 9 bits. The others interpolate to 8 bits at twice the volumes. The first point of each 8
    // multiplies the fraction by the difference. Each later one does so unless the step has just gone on to a point
    // whose difference is 0. The point then takes the last point's result, shifted right by 22 or 23 bits: 0, or -1
    // when it was negative.
    const uint64_t frame_steps = fraction + uint64_t(step) * uint64_t(points);
    const uint32_t advance = uint32_t(frame_steps >> 23);
    if (count > int32_t(advance))
    {
        enum class Code
        {
            kBelowOne,
            kOneToTwo,
            kTwoUp,
        };
        Code code = Code::kBelowOne;
        uint32_t increment = step - 0x800000;
        if (int32_t(increment) >= 0)
        {
            volume <<= 1;
            code = int32_t(step - 0x1000000) < 0 ? Code::kOneToTwo : Code::kTwoUp;
            increment = code == Code::kOneToTwo ? step - 0x1000000 : step - 0x800000;
        }

        uint32_t product = 0;
        bool multiply = true;
        for (int i = 0; i < points; i++)
        {
            if (i % 8 == 0 || multiply)
            {
                product = fraction * uint32_t(difference);
            }
            const int32_t value =
                code == Code::kBelowOne ? (int32_t(product) >> 22) + 2 * point : point + (int32_t(product) >> 23);
            product = uint32_t(value);
            add(i, volume, value);

            fraction += increment;
            if (code == Code::kBelowOne)
            {
                multiply = true;
                if (int32_t(fraction) >= 0)
                {
                    point += difference;
                    next++;
                    difference = rom_.S8(next) - point;
                    multiply = difference != 0;
                }
            }
            else
            {
                if (code == Code::kOneToTwo)
                {
                    point += difference;
                    if (int32_t(fraction) >= 0)
                    {
                        next++;
                        point = rom_.S8(next);
                    }
                }
                else
                {
                    next += fraction >> 23;
                    point = rom_.S8(next);
                }
                next++;
                difference = rom_.S8(next) - point;
                multiply = difference != 0;
            }
            fraction &= 0x7FFFFF;
        }

        c.fraction = fraction;
        c.count = count - int32_t(advance);
        c.position += advance;
        return;
    }

    // Otherwise each point goes on by the step, and the sample's end loops or stops the channel (0x03000b44). A loop
    // goes back to its start, past it by the points that the step overshot.
    for (int i = 0; i < points; i++)
    {
        add(i, volume, (int32_t(fraction * uint32_t(difference)) >> 22) + 2 * point);
        fraction += step;
        const uint32_t steps = fraction >> 23;
        if (steps == 0)
        {
            continue;
        }

        count -= int32_t(steps);
        if (count <= 0)
        {
            if (loop_length == 0)
            {
                c.status = 0;
                return;
            }
            const int32_t overshoot = -count;
            count += loop_length;
            next = loop_start + uint32_t(overshoot);
            point = rom_.S8(next);
        }
        else if (steps == 1)
        {
            point += difference;
        }
        else
        {
            next += steps - 1;
            point = rom_.S8(next);
        }
        next++;
        difference = rom_.S8(next) - point;
        fraction &= ~0x3F800000u;
    }

    c.fraction = fraction;
    c.count = count;
    c.position = next - 1;
}

void Sequencer::OutputCamelot()
{
    // The output stage (0x03000d40) writes two points for each sum on each side, or one in Mario Tennis: Power Tour
    // (0x03000dec). The two sides' sums share a word, 16 bits each, and the output stage works on both halves at once.
    // A sum that doesn't fit in 15 bits saturates (0x03000e90).
    constexpr uint32_t kSigns = 0x80008000;
    constexpr uint32_t kRound = kSigns >> 9;
    constexpr uint32_t kBytes = 0xFF00FF00;
    const auto rotate = [](uint32_t value, int bits)
    {
        return value >> bits | value << (32 - bits);
    };
    const auto saturate = [](uint32_t sum)
    {
        const uint32_t swapped = sum >> 16 | sum << 16;
        if ((swapped ^ swapped << 1) & 0x80000000)
        {
            const bool high = sum & 0x4000;
            sum = (sum | 0x4000) & ~0x3FC0u;
            if (high)
            {
                sum -= 0x40;
            }
        }
        if ((sum ^ sum << 1) & 0x80000000)
        {
            const bool high = sum & 0x40000000;
            sum = (sum | 0x40000000) & ~0x3FC00000u;
            if (high)
            {
                sum -= 0x400000;
            }
        }
        return sum;
    };
    // The points of both halves in a word's bytes: the left side's first and second points, then the right side's. A
    // negative half takes 64 more first. The first point holds bits 7 to 14 of the half, and the second those of the
    // half less 64.
    const auto points = [&](uint32_t sum)
    {
        const uint32_t up = sum + ((sum & kSigns) >> 9);
        return (rotate(up - kRound, 15) & kBytes) | (rotate(up, 15) & kBytes) >> 8;
    };

    // The sums take 64 more before they turn into points. With 4 channels or more in the frame, only the first two do.
    // The stage writes the jump back of its loop to match (0x03000d4c). It also shifts the count up by 4 bits for the
    // next frame. Only the next frame's count reaches the low 4 bits that the stage reads.
    const bool many = (camelot_channels_ & 12) != 0;
    camelot_channels_ = 0;

    // The echo of the left side takes the right side's points from camelot_tap points before: the last frame's last
    // part, then the frame's first points.
    const uint32_t frame_points = uint32_t(info_.samples_per_frame);
    const uint32_t tap = uint32_t(info_.camelot_tap);
    const uint32_t scale = uint32_t(PointsPerMixed());
    const uint32_t size = scale * kPcmBufferSize;
    int8_t* right = pcm_.data();
    int8_t* left = right + size;
    uint32_t place = part_;
    uint32_t echo = camelot_last_part_ + scale * (frame_points - tap);
    camelot_last_part_ = part_;

    // Each side of the next frame's sums starts with a share of its old points, and a share of the other side's. The
    // constants spread over both halves.
    const uint32_t echo_right = uint32_t(info_.camelot_cross_echo) << 16 | uint32_t(info_.camelot_echo);
    const uint32_t echo_left = (uint32_t(info_.camelot_echo) << 16) + uint32_t(info_.camelot_cross_echo);
    for (uint32_t i = 0; i < frame_points; i += 2)
    {
        if (i == tap)
        {
            echo = part_;
        }

        uint32_t first = camelot_sums_[i];
        uint32_t second = camelot_sums_[i + 1];
        if (i == 0 || !many)
        {
            first += kRound;
            second += kRound;
        }
        if (((first ^ first << 1) | (second ^ second << 1)) & kSigns)
        {
            first = saturate(first);
            second = saturate(second);
        }

        // With one point for each sum, a word holds the left side's point in its low byte and the right side's above
        // it. The sums start with shares of the buffer's old points, a quarter of the frame's right side and a quarter
        // of the right side's tap. A half takes 64 less, or 64 more when it's negative.
        if (scale == 1)
        {
            const int32_t first_word = int32_t(rotate(first, 16)) >> 7;
            const int32_t second_word = int32_t(rotate(second, 16)) >> 7;
            uint32_t first_echo = uint32_t(right[place]) * echo_right + uint32_t(left[place]) * echo_left;
            uint32_t second_echo = uint32_t(right[place + 1]) * echo_right + uint32_t(left[place + 1]) * echo_left;
            left[place] = int8_t(first_word);
            left[place + 1] = int8_t(second_word);
            const int32_t first_right = first_word >> 16;
            const int32_t second_right = second_word >> 16;
            right[place] = int8_t(first_right);
            right[place + 1] = int8_t(second_right);
            place += 2;

            first_echo += uint32_t(first_right) << 5;
            second_echo += uint32_t(second_right) << 5;
            first_echo += uint32_t(right[echo]) << 21;
            second_echo += uint32_t(right[echo + 1]) << 21;
            echo += 2;
            camelot_sums_[i] = first_echo - kRound + ((first_echo & kSigns) >> 8);
            camelot_sums_[i + 1] = second_echo - kRound + ((second_echo & kSigns) >> 8);
            continue;
        }

        const uint32_t first_points = points(first);
        const uint32_t second_points = points(second);

        // The shares come from the first points of the buffer's part: 3 frames before this one in Golden Sun: The Lost
        // Age.
        uint32_t first_echo = uint32_t(right[place]) * echo_right + uint32_t(left[place]) * echo_left;
        uint32_t second_echo = uint32_t(right[place + 2]) * echo_right + uint32_t(left[place + 2]) * echo_left;
        left[place] = int8_t(first_points);
        left[place + 1] = int8_t(first_points >> 8);
        left[place + 2] = int8_t(second_points);
        left[place + 3] = int8_t(second_points >> 8);
        right[place] = int8_t(first_points >> 16);
        right[place + 1] = int8_t(first_points >> 24);
        right[place + 2] = int8_t(second_points >> 16);
        right[place + 3] = int8_t(second_points >> 24);
        place += 4;

        // The right side takes a quarter of its second points from this frame, plus 1/128 of that. The left side takes
        // a quarter of the right side's first points from camelot_tap points before (0x03000e34).
        uint32_t quarter = ((second_points & 0xFFFF0000) | first_points >> 16) & kBytes;
        quarter += quarter >> 7;
        second_echo += uint32_t(int32_t(quarter) >> 19);
        first_echo += uint32_t(int32_t(rotate(quarter, 16)) >> 19);
        first_echo += uint32_t(int32_t(right[echo])) << 21;
        second_echo += uint32_t(int32_t(right[echo + 2])) << 21;
        echo += 4;
        camelot_sums_[i] = first_echo;
        camelot_sums_[i + 1] = second_echo;
    }
}

} // namespace ntdlib::mp2k
