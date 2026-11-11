// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "xsflib/bytes.h"

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/warnings.h"
#include "ntdlib/krawall/build.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"
#include "player_shared.h"

namespace ntdlib::krawall
{

void Player::Advance(MixChannel& m, uint32_t samples)
{
    // The ramp at a stop counts down its samples and frees the channel when they're done. Its routine leaves the
    // position alone, even if a change of frequency after the stop has given the channel a step again.
    if (m.mix == 7)
    {
        const uint32_t done = std::min<uint32_t>(m.sfx, samples);
        const uint16_t level = uint16_t(m.loop_from + (m.loop_from >> 16) * done);
        m.loop_from = (m.loop_from & 0xFFFF0000) | level;
        m.sfx = uint8_t(m.sfx - done);
        if (m.sfx == 0)
        {
            m.status = 0;
        }
        return;
    }

    // A mixing routine steps through the points a sample of the mix at a time, keeping the position's fraction.
    const int32_t sum = int32_t(uint32_t(m.frac) + uint32_t(m.inc) * samples);
    m.pos += uint32_t(sum >> 16);
    m.frac = uint16_t(sum);
}

void Player::Mix(uint32_t amount)
{
    // The mixer clears its sums, then adds each channel that plays. A channel at volume 0 adds nothing.
    amount_ = amount;
    if (mix_output_)
    {
        sums_.assign(amount, 0);
        volume_sums_ = {};
    }
    if (mix_enhanced_)
    {
        // The enhanced samples whose times fall in the amount.
        const auto rate = uint64_t(agb::kEnhancedRate);
        const auto mix_rate = uint64_t(info_.mix_rate);
        enhanced_first_ = (elapsed_ * rate + mix_rate - 1) / mix_rate;
        const uint64_t end = ((elapsed_ + amount) * rate + mix_rate - 1) / mix_rate;
        enhanced_at_ = enhanced_[0].size();
        enhanced_[0].resize(enhanced_at_ + size_t(end - enhanced_first_), 0.0f);
        enhanced_[1].resize(enhanced_at_ + size_t(end - enhanced_first_), 0.0f);
    }

    for (MixChannel& m : mixer_)
    {
        if (m.status != 1)
        {
            continue;
        }

        if (m.volume == 0)
        {
            SkipSilent(m, amount);
        }
        else
        {
            if (!Muted(m))
            {
                volume_sums_[0] += m.left;
                volume_sums_[1] += m.right;
            }
            MixPlaying(m, amount);
        }
    }

    if (mix_output_)
    {
        Convert(amount);
    }
    elapsed_ += amount;
}

uint32_t Player::GuessPoints(const MixChannel& m, uint32_t samples, bool margin) const
{
    // From the build of 2003/09/01 on, the guess takes the whole points of the step. The builds before it take the step
    // in 256ths.
    if (info_.build >= Build::k2003_09_01)
    {
        return uint32_t(m.inc >> 16) * samples + (margin ? samples : 0);
    }

    const int32_t points = int32_t(uint32_t(m.inc >> 8) * samples) >> 8;
    return uint32_t(points) + (margin ? 1 : 0);
}

void Player::SkipSilent(MixChannel& m, uint32_t amount) const
{
    // A silent channel moves on by a guess at the points that its samples take, without mixing them.
    const uint32_t guess = GuessPoints(m, amount, true);
    if (m.loop == 0)
    {
        if (m.pos + guess >= m.end)
        {
            m.status = 0;
        }
        else
        {
            m.pos += guess;
        }
    }
    else if (m.loop == 2)
    {
        // Back and forth: each time the guess passes an end of the loop, it comes back by the loop's length, and an odd
        // count of times turns it round.
        uint32_t turns = 0;
        m.pos += guess;
        if (m.inc >= 0)
        {
            for (; m.pos >= m.end && m.loop_length; turns++)
            {
                m.pos -= m.loop_length;
            }
            if (turns & 1)
            {
                m.pos = m.end - (m.pos - m.loop_from);
                m.inc = -m.inc;
            }
        }
        else
        {
            for (; m.pos < m.loop_from && m.loop_length; turns++)
            {
                m.pos += m.loop_length;
            }
            if (turns & 1)
            {
                m.pos = m.loop_from + (m.end - m.pos);
                m.inc = -m.inc;
            }
        }
    }
    else
    {
        m.pos += guess;
        while (m.pos >= m.end && m.loop_length)
        {
            m.pos -= m.loop_length;
        }
    }
}

void Player::MixPlaying(MixChannel& m, uint32_t todo)
{
    // A playing channel mixes until its sample or loop ends, then loops or stops, and goes on with what's left. A
    // channel that stops leaves the rest of the amount silent.
    uint32_t at = 0;
    for (;;)
    {
        bool fits = false;
        int32_t num = 0, den = 0;
        if (m.inc >= 0)
        {
            fits = m.pos + GuessPoints(m, todo, true) < m.end;
            num = int32_t(((m.end - m.pos) << 14) - (uint32_t(m.frac) >> 2));
            den = m.inc >> 2;
        }
        else
        {
            fits = m.pos + GuessPoints(m, todo, false) > m.loop_from;
            num = int32_t(((m.pos - m.loop_from) << 14) + (uint32_t(m.frac) >> 2));
            den = int32_t(0u - uint32_t(m.inc)) >> 2;
        }
        if (fits)
        {
            MixPoints(m, at, todo);
            Advance(m, todo);
            break;
        }

        const auto [quotient, remainder] = Div(num, den);
        int32_t fit = quotient + (remainder != 0 ? 1 : 0);
        if (fit > int32_t(todo))
        {
            MixPoints(m, at, todo);
            Advance(m, todo);
            break;
        }
        if (fit & 3)
        {
            fit = (fit & ~3) + 4;
        }
        if (fit >= int32_t(todo))
        {
            MixPoints(m, at, uint32_t(fit));
            Advance(m, uint32_t(fit));
            if (m.loop == 0)
            {
                m.status = 0;
            }
            else if (m.loop == 2)
            {
                m.inc = -m.inc;
            }
            else
            {
                m.pos -= m.loop_length;
            }
            break;
        }

        if (fit != 0)
        {
            MixPoints(m, at, uint32_t(fit));
            Advance(m, uint32_t(fit));
            todo -= uint32_t(fit);
            at += uint32_t(fit);
        }
        if (m.loop == 0)
        {
            m.status = 0;
            FillSilence(m, at, todo);
            break;
        }
        if (m.loop == 2)
        {
            m.inc = -m.inc;
        }
        else
        {
            m.pos -= m.loop_length;
        }
        if (todo == 0)
        {
            break;
        }
    }
}

void Player::MixOutput(bool on)
{
    // kramSetMasterVol() builds the output table: entry i of 0 to 0x3FF is i times the master volume's level, shifted
    // down 7 bits, up to 127. Entry -i of -1 to -0x400 is the same down to -128. The mixer shifts its sums down by the
    // master volume's shift before the table, or by 5 for a shift of 0. The builds before 1.4 take the whole master
    // volume as the level, and always shift by 5. They store entry -0x400 through an address with the top 4 bits set,
    // which the GBA ignores.
    mix_output_ = on;
    const bool old = info_.build < Build::kV1_4;
    const uint32_t level = old ? info_.master_volume : info_.master_volume & 0xFF;
    shift_ = (info_.master_volume >> 16) && !old ? int(info_.master_volume >> 16) : 5;
    for (uint32_t i = 0; i <= 0x400; i++)
    {
        const int32_t entry = int32_t(i * level) >> 7;
        if (i < 0x400)
        {
            table_[0x400 + i] = int8_t(std::min(entry, 127));
        }
        if (i > 0)
        {
            table_[0x400 - i] = int8_t(-std::min(entry, 128));
        }
    }
    output_[0].clear();
    output_[1].clear();
}

bool Player::Muted(const MixChannel& m) const
{
    const int owner = owners_[size_t(&m - mixer_.data())];
    return owner >= 0 && (muted_ >> owner & 1);
}

void Player::MeterChannels(bool on)
{
    metered_ = on;
    peaks_ = {};
}

void Player::Meter(const MixChannel& m, int side, uint32_t volume, int32_t low, int32_t high)
{
    // A point adds its product with the volume, shifted down 3 bits, and the mixer takes off the middle's. The output
    // shifts the rest down and scales it by the level over 128. A FIFO's point counts 256 times on the 16-bit scale.
    const int owner = owners_[size_t(&m - mixer_.data())];
    if (owner < 0)
    {
        return;
    }

    const auto part = [volume](int32_t point)
    {
        return std::abs(int64_t((uint32_t(std::clamp(point, 0, 255)) * volume) >> 3) - int64_t((volume << 7) >> 3));
    };
    const bool old = info_.build < Build::kV1_4;
    const int64_t level = old ? int64_t(info_.master_volume) : int64_t(info_.master_volume & 0xFF);
    const int64_t peak = (std::max(part(low), part(high)) * level * 2) >> shift_;
    int32_t& kept = peaks_[size_t(owner)][size_t(side)];
    kept = std::max(kept, int32_t(std::min<int64_t>(peak, INT32_MAX)));
}

bool Player::MixerPlays() const
{
    return std::any_of(mixer_.begin(), mixer_.end(), [](const MixChannel& m) { return m.status == 1; });
}

void Player::MixEnhanced(bool on)
{
    mix_enhanced_ = on;
    enhanced_[0].clear();
    enhanced_[1].clear();
}

double Player::EnhancedPoint(const MixChannel& m, int64_t at) const
{
    // A forward loop goes on from its start after its last point, and a loop back and forth turns at each end. The
    // loop turns at its start only while it plays backwards. Before that, the points before the loop are the attack.
    // Past a sample's end without a loop lies silence.
    const int64_t end = m.end;
    const int64_t length = m.loop_length;
    if (m.loop == 1 && length > 0 && at >= end)
    {
        at = end - length + (at - end) % length;
    }
    else if (m.loop == 2 && length > 0 && at >= end)
    {
        at = std::max(end - 1 - (at - end), end - length);
    }
    else if (m.loop == 2 && length > 0 && m.inc < 0 && at < end - length)
    {
        at = std::min(2 * (end - length) - 1 - at, end - 1);
    }
    else if (at >= end || at < 0)
    {
        return 128.0;
    }

    return double(rom_.U8(uint32_t(at)));
}

void Player::MixEnhancedPoints(const MixChannel& m, uint32_t at, uint32_t samples)
{
    // Each enhanced sample takes the channel's place at its time: between two points of its sample, or on the ramp.
    const auto rate = uint64_t(agb::kEnhancedRate);
    const auto mix_rate = uint64_t(info_.mix_rate);
    const uint64_t start = elapsed_ + at;
    const uint64_t first = (start * rate + mix_rate - 1) / mix_rate;
    const uint64_t end = ((start + samples) * rate + mix_rate - 1) / mix_rate;
    const bool old = info_.build < Build::kV1_4;
    const double level = old ? double(info_.master_volume) : double(info_.master_volume & 0xFF);
    const double scale = level / (1024.0 * double(1u << shift_));
    const bool left = m.mix != 3;
    const bool right = m.mix != 1;
    const double left_volume = double(m.left) * scale;
    const double right_volume = double(m.mix == 4 ? m.left : m.right) * scale;
    const uint32_t ramp = m.mix == 7 ? std::min<uint32_t>(m.sfx, samples) : 0;

    for (uint64_t e = first; e < end; e++)
    {
        const double offset = double(e * mix_rate - start * rate) / double(rate);
        double value = 128.0;
        if (m.mix == 7)
        {
            if (offset >= double(ramp))
            {
                continue;
            }
            value = (double(int16_t(m.loop_from)) + double(int16_t(m.loop_from >> 16)) * offset) / 16.0;
        }
        else
        {
            const double x = (double(m.frac) + offset * double(m.inc)) / 65536.0;
            const double whole = std::floor(x);
            const int64_t at_point = int64_t(m.pos) + int64_t(whole);
            const double a = EnhancedPoint(m, at_point);
            value = a + (EnhancedPoint(m, at_point + 1) - a) * (x - whole);
        }

        const size_t i = enhanced_at_ + size_t(e - enhanced_first_);
        if (left)
        {
            enhanced_[0][i] += float((value - 128.0) * left_volume);
        }
        if (right)
        {
            enhanced_[1][i] += float((value - 128.0) * right_volume);
        }
    }
}

void Player::MixPoints(const MixChannel& m, uint32_t at, uint32_t samples)
{
    samples = InAmount(at, samples);
    if (samples == 0 || Muted(m))
    {
        return;
    }
    if (mix_enhanced_)
    {
        MixEnhancedPoints(m, at, samples);
    }
    if (!mix_output_)
    {
        return;
    }
    if (m.mix == 7)
    {
        MixRamp(m, at, samples);
        return;
    }

    // The routine reads two points at a time, packs them in the halves of a word and multiplies the word by a side's
    // volume. It clears the 3 bits that the second point's product would shift into the first's, and adds the word
    // shifted down 3 bits to the side's sum of the two samples. An interpolating routine takes the point before the
    // position and the one after, and adds their difference times the fraction, shifted down 16 bits. The shift is
    // logical, so a falling difference adds 0x10000 to the point.
    //
    // Routine 1 adds the left side alone, and routine 3 the right side alone. Routine 4, for the centre, adds the left
    // side's volume to both sides. Routines 0 and 2 add each side at its volume.
    const bool interpolate = m.hq != 0;
    const bool left = m.mix != 3;
    const bool right = m.mix != 1;
    const uint32_t left_volume = m.left;
    const uint32_t right_volume = m.mix == 4 ? m.left : m.right;
    int32_t position = int32_t(uint32_t(m.frac));
    const auto point = [&]
    {
        const uint32_t at_point = m.pos + uint32_t(position >> 16);
        uint32_t p = rom_.U8(at_point);
        if (interpolate)
        {
            const int32_t difference = int32_t(rom_.U8(at_point + 1)) - int32_t(p);
            p += uint32_t(int32_t(uint32_t(position) & 0xFFFF) * difference) >> 16;
        }
        position = int32_t(uint32_t(position) + uint32_t(m.inc));
        return p;
    };

    int32_t low = 255;
    int32_t high = 0;
    const auto kept = [&](uint32_t p)
    {
        const int32_t value = int32_t(std::min<uint32_t>(p & 0xFFFF, 255));
        low = std::min(low, value);
        high = std::max(high, value);
        return p;
    };
    for (uint32_t i = 0; i + 4 <= samples; i += 4)
    {
        uint32_t* const sums = &sums_[at + i];
        for (int pair = 0; pair < 2; pair++)
        {
            const uint32_t first = kept(point());
            const uint32_t word = first + (kept(point()) << 16);
            if (left)
            {
                sums[pair] += ((word * left_volume) & ~0x70000u) >> 3;
            }
            if (right)
            {
                sums[2 + pair] += ((word * right_volume) & ~0x70000u) >> 3;
            }
        }
    }

    if (metered_ && low <= high)
    {
        if (left)
        {
            Meter(m, 0, left_volume, low, high);
        }
        if (right)
        {
            Meter(m, 1, right_volume, low, high);
        }
    }
}

void Player::MixRamp(const MixChannel& m, uint32_t at, uint32_t samples)
{
    if (Muted(m))
    {
        return;
    }

    // The ramp adds its level, from the point where the note stopped to the middle, times each side's volume to the
    // halfword of each sample's sum, without a carry into the next. A side's volume times the level of 0 to 255 is
    // shifted down 3 bits, as the mixing routines shift a point's.
    const uint32_t ramp = std::min<uint32_t>(m.sfx, samples);
    uint16_t level = uint16_t(m.loop_from);
    const uint16_t step = uint16_t(m.loop_from >> 16);
    for (uint32_t i = 0; i < ramp; i++)
    {
        const int32_t value = int32_t(int16_t(level)) >> 4;
        uint32_t& left = sums_[at + (i & ~3u) + ((i >> 1) & 1)];
        uint32_t& right = sums_[at + (i & ~3u) + 2 + ((i >> 1) & 1)];
        const int shift = (i & 1) ? 16 : 0;
        const auto add = [shift](uint32_t& word, int32_t part)
        {
            const uint16_t half = uint16_t((word >> shift) + uint32_t(part));
            word = (word & ~(0xFFFFu << shift)) | uint32_t(half) << shift;
        };
        add(left, (value * int32_t(m.left)) >> 3);
        add(right, (value * int32_t(m.right)) >> 3);
        if (metered_)
        {
            Meter(m, 0, m.left, value, value);
            Meter(m, 1, m.right, value, value);
        }
        level = uint16_t(level + step);
    }

    FillSilence(m, at + ramp, samples - ramp);
}

void Player::FillSilence(const MixChannel& m, uint32_t at, uint32_t samples)
{
    // The mixer adds each side's volume times the middle of 128, shifted down 3 bits, to each sample's sum, as the
    // channel's points would at silence.
    samples = InAmount(at, samples);
    if (!mix_output_ || Muted(m))
    {
        return;
    }

    const uint32_t left = (uint32_t(m.left) << 7) >> 3;
    const uint32_t right = (uint32_t(m.right) << 7) >> 3;
    for (uint32_t i = 0; i + 4 <= samples; i += 4)
    {
        uint32_t* const sums = &sums_[at + i];
        sums[0] += left + (left << 16);
        sums[1] += left + (left << 16);
        sums[2] += right + (right << 16);
        sums[3] += right + (right << 16);
    }
}

void Player::Convert(uint32_t amount)
{
    // Each half of a sum, less the side's volumes times the middle of 128 shifted down 3 bits, is shifted down and
    // goes through the output table. The builds before 1.4 take the halves as signed. Past the table's ends, the
    // game's mixer reads the bytes around it. The model takes the entry at the end instead.
    const bool old = info_.build < Build::kV1_4;
    for (int side = 0; side < 2; side++)
    {
        const int32_t middle = int32_t((volume_sums_[size_t(side)] << 7) >> 3);
        for (uint32_t i = 0; i < amount; i++)
        {
            const uint32_t word = sums_[(i & ~3u) + 2 * uint32_t(side) + ((i >> 1) & 1)];
            const uint32_t bits = (i & 1) ? word >> 16 : word & 0xFFFF;
            const int32_t half = old ? int32_t(int16_t(bits)) : int32_t(bits);
            const int32_t index = std::clamp((half - middle) >> shift_, -0x400, 0x3FF);
            output_[size_t(side)].push_back(table_[size_t(index + 0x400)]);
        }
    }
}

} // namespace ntdlib::krawall
