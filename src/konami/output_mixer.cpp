// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/output.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/sinc.h"
#include "output_impl.h"

namespace ntdlib::konami
{
namespace
{

constexpr int kRingVoices = 3;

constexpr uint8_t kFifoVoiceReverse = 0x08;

// The prescaler of a timer's control register, and its on bit.
constexpr std::array<uint32_t, 4> kTimerPrescale = {1, 64, 256, 1024};

} // namespace

int Output::Impl::Value(Scale scale, int sample, int level) const
{
    const int product = sample * level;
    switch (scale)
    {
    case Scale::kRoundUp7:
        return (product < 0 ? product + 128 : product) >> 7;
    case Scale::kDown7:
        return product >> 7;
    case Scale::kUp4:
        {
            const int value = product >> 4;
            return value < 0 ? value + 1 : value;
        }
    case Scale::kDown4:
        return product >> 4;
    case Scale::kTable:
        return rom.S8(uint32_t(rules.level_table) + uint32_t(level) * 256 + uint32_t(uint8_t(sample)));
    case Scale::kTableSigned:
        return rom.S8(uint32_t(rules.level_table) + uint32_t(level) * 256 + uint32_t(sample));
    }

    return 0;
}

int Output::Impl::NextAdpcm(OutputVoice& v, uint32_t& position, int advance)
{
    if (advance == 0)
    {
        return int8_t(v.held);
    }

    int value = v.adpcm_value;
    for (int i = 0; i < advance; i++)
    {
        const bool high = v.run & 1;
        v.run ^= 1;
        uint32_t nibble = rom.U8(position);
        if (high)
        {
            nibble >>= 4;
            position++;
        }
        nibble &= 15;

        const int step = wave::kImaStep[v.adpcm_index];
        const int change = (step * int((nibble & 7) * 2 + 1)) >> 3;
        if (nibble & 8)
        {
            value -= change;
            if (value < -32768)
            {
                value = -32768;
            }
        }
        else
        {
            value += change;
            if (value > 32767)
            {
                value = 32767;
            }
        }

        int index = v.adpcm_index + wave::kImaIndex[nibble];
        index = std::clamp(index, 0, 88);
        v.adpcm_index = uint8_t(index);
    }

    v.adpcm_value = int16_t(value);
    const int sample = value >> 8;
    v.held = uint8_t(sample);
    return sample;
}

int Output::Impl::NextPacked(OutputVoice& v, uint32_t& position, int advance)
{
    int left = advance;
    if (v.run != 0)
    {
        left = v.run - left;
        if (left > 0)
        {
            v.run = uint8_t(left);
            return int8_t(v.held);
        }

        v.run = 0;
        left = ~left;
    }

    for (;;)
    {
        const int code = rom.S8(position++);
        if (code >= (rules.coded == Coded::kPackedRuns ? -96 : -64))
        {
            // A single sample. Moving on takes the bytes that follow, and the next sample stays unread.
            left--;
            if (left >= 0)
            {
                continue;
            }

            position--;
            return rom.S8(position);
        }

        if (code >= -96)
        {
            // A pair: its high byte now, and its low byte next.
            const uint32_t pair = v.start - 64 + uint32_t(code + 96) * 2;
            const int16_t both = int16_t(rom.U16(pair));
            v.held = uint8_t(both);
            v.run = 2;
            return both >> 8;
        }

        int count = (code + 96 + 16) >= 0 ? code + 96 + 16 : code + 96 + 32;
        int sample = 0;
        const bool zeros = code + 96 + 16 >= 0;
        if (count == 0)
        {
            count = rom.U8(position++);
        }
        if (!zeros)
        {
            sample = rom.S8(position++);
        }

        v.held = uint8_t(sample);
        v.run = uint8_t(count);
        return sample;
    }
}

int Output::Impl::NextCoded(OutputVoice& v, uint32_t& position, int advance)
{
    return rules.coded == Coded::kAdpcm ? NextAdpcm(v, position, advance) : NextPacked(v, position, advance);
}

Path Output::Impl::PathOf(const OutputVoice& v) const
{
    if (rules.keeps_path && v.mix_path != uint8_t(Path::kNone))
    {
        return Path(v.mix_path);
    }

    if (v.flags & kVoiceReverse)
    {
        return Path::kReverse;
    }

    if (v.flags & kVoiceCoded)
    {
        return Path::kCoded;
    }

    if (rules.single_loop || v.right != v.left)
    {
        return Path::kUnequal;
    }

    return rules.double_loop && v.step == 0x800 ? Path::kDouble : Path::kEqual;
}

void Output::Impl::MixVoice(OutputVoice& v, int16_t* a, int16_t* b)
{
    uint32_t position = v.position;
    int32_t count = v.count;
    uint32_t fraction = v.fraction;
    const uint32_t step = v.step;
    const int level_a = v.right;
    const int level_b = v.left;
    int n = kBlock;
    int advance = 0;
    int out = 0;

    const Path path = PathOf(v);
    if (rules.keeps_path)
    {
        v.mix_path = uint8_t(path);
    }

    for (;;)
    {
        if (path == Path::kDouble)
        {
            n -= 2;
            if (n < 0)
            {
                break;
            }

            const int value = Value(rules.equal, rom.S8(position++), level_a);
            a[out] = int16_t(a[out] + value);
            b[out] = int16_t(b[out] + value);
            a[out + 1] = int16_t(a[out + 1] + value);
            b[out + 1] = int16_t(b[out + 1] + value);
            out += 2;
            count--;
        }
        else
        {
            int sample;
            int value_a;
            int value_b;
            bool crossed = false;
            switch (path)
            {
            case Path::kEqual:
                position += uint32_t(advance);
                sample = rom.S8(position);
                value_a = Value(rules.equal, sample, level_a);
                value_b = value_a;
                break;
            case Path::kReverse:
                position -= uint32_t(advance);
                sample = rom.S8(position);
                value_a = Value(rules.reverse, sample, level_a);
                value_b = rules.reverse_two_levels ? Value(rules.reverse, sample, level_b) : value_a;
                crossed = rules.reverse_crosses;
                break;
            case Path::kCoded:
                sample = NextCoded(v, position, advance);
                value_a = Value(rules.unequal, sample, level_a);
                value_b = Value(rules.unequal, sample, level_b);
                break;
            default:
                position += uint32_t(advance);
                sample = rom.S8(position);
                value_a = Value(rules.unequal, sample, level_a);
                value_b = Value(rules.unequal, sample, level_b);
                break;
            }

            bool block_done = false;
            for (;;)
            {
                n--;
                if (n < 0)
                {
                    block_done = true;
                    break;
                }

                if (crossed)
                {
                    const int16_t old_a = a[out];
                    a[out] = int16_t(b[out] + value_a);
                    b[out] = int16_t(old_a + value_b);
                }
                else
                {
                    a[out] = int16_t(a[out] + value_a);
                    b[out] = int16_t(b[out] + value_b);
                }

                out++;
                fraction += step;
                advance = int(fraction >> 12);
                if (advance != 0)
                {
                    break;
                }
            }

            if (block_done)
            {
                break;
            }

            fraction &= 0xFFF;
            count -= advance;
        }

        if (count > 0)
        {
            continue;
        }

        // The sample's end. Going backwards ends the note whatever the flags say.
        if (path == Path::kReverse || !(v.flags & kVoiceLooped))
        {
            v.flags = 0;
            v.volume = 0;
            break;
        }

        const uint32_t table = v.key & 0x8000 ? second_table : info.sample_table;
        const uint32_t header = rom.U32(table + (v.key & 0x7FFFu) * 4);
        const int32_t loop = rom.S32(header + 8);
        count = rom.S32(header + 4) - loop;
        position = (rules.loop_from_header ? header + 12 : v.start) + uint32_t(loop);
        advance = 0;
    }

    v.position = position;
    v.count = count;
    v.fraction = uint16_t(fraction);
}

void Output::Impl::MixBlock(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    // A dry sum for each FIFO, and one for each of the echo buses.
    std::array<std::array<int16_t, kBlock>, 8> sums{};
    const uint32_t routing = rules.mask_each_block ? echo[0].routing | (echo[1].routing << 1) : echo_mask;
    uint32_t mask = routing;
    for (int n = voice_count - 1; n >= 0; n--)
    {
        OutputVoice& v = voices[size_t(n)];
        const uint32_t choice = mask & 3;
        mask >>= 2;
        if (!(v.flags & kVoiceOn) || (rules.skips_silent && v.volume == 0))
        {
            continue;
        }

        // A muted voice runs on scratch sums. A meter takes a voice's points from what it adds to the bus's sums.
        std::array<int16_t, kBlock>& to_a = sums[choice * 2];
        std::array<int16_t, kBlock>& to_b = sums[choice * 2 + 1];
        if (Muted(n))
        {
            std::array<int16_t, kBlock> unheard_a{};
            std::array<int16_t, kBlock> unheard_b{};
            MixVoice(v, unheard_a.data(), unheard_b.data());
        }
        else if (metering)
        {
            const std::array<int16_t, kBlock> before_a = to_a;
            const std::array<int16_t, kBlock> before_b = to_b;
            MixVoice(v, to_a.data(), to_b.data());
            for (size_t i = 0; i < kBlock; i++)
            {
                MeterPart(n, 0, float(int16_t(to_a[i] - before_a[i])));
                MeterPart(n, 1, float(int16_t(to_b[i] - before_b[i])));
            }
        }
        else
        {
            MixVoice(v, to_a.data(), to_b.data());
        }
    }

    const uint32_t buses = rules.has_echo ? routing >> 30 : 0;
    for (int bus = 0; bus < 2; bus++)
    {
        if (!(buses & (1u << bus)))
        {
            continue;
        }

        OutputEcho& e = echo[size_t(bus)];
        const std::array<int16_t, kBlock>& in_a = sums[size_t(2 + bus * 2)];
        const std::array<int16_t, kBlock>& in_b = sums[size_t(3 + bus * 2)];
        const uint32_t read = e.line + uint32_t(e.position) * 32;
        const uint32_t write = e.line + uint32_t(uint8_t(e.position + e.delay_blocks)) * 32;
        e.position++;
        for (uint32_t i = 0; i < kBlock; i++)
        {
            const auto wet = [&](int stored)
            {
                const int product = stored * e.feedback;
                if (rules.wet_rounds_up)
                {
                    return (product < 0 ? product + 128 : product) >> 8;
                }

                const int value = product >> 8;
                return value < 0 ? value + 1 : value;
            };

            // Without the echo, the line stays silent, and the bus outputs only its input.
            const int a = wet(work[read + i * 2 + 1]) + in_a[i];
            const int b = wet(work[read + i * 2]) + in_b[i];
            if (echo_heard)
            {
                work[write + i * 2 + 1] = int8_t(a);
                work[write + i * 2] = int8_t(b);
            }
            sums[0][i] = int16_t(sums[0][i] + a);
            sums[1][i] = int16_t(sums[1][i] + b);
        }
    }

    for (size_t i = 0; i < kBlock; i++)
    {
        fifo_a.push_back(int8_t(sums[0][i]));
        fifo_b.push_back(int8_t(sums[1][i]));
    }
}

uint32_t Output::Impl::FifoCycles(int index) const
{
    // SOUNDCNT_H chooses a FIFO's timer.
    const uint32_t word = timers[(sound_control_high >> (2 + 4 * index)) & 1];
    const uint32_t control = word >> 16;
    return control & kTimerOn ? (0x10000 - (word & 0xFFFF)) * kTimerPrescale[control & 3] : 0;
}

void Output::Impl::FifoInterrupt(int index)
{
    Fifo& fifo = fifos[size_t(index)];
    fifo.dma_on = false;

    // The first voice that sounds goes into the buffer, and the second into `second` when a first has sounded.
    std::array<int8_t, kBlock> second{};
    int active = 0;
    bool into_second = false;
    for (int n = 0; n < 2; n++)
    {
        OutputVoice& v = voices[size_t(index * 2 + n)];
        if (!(v.flags & kVoiceOn))
        {
            continue;
        }

        v.count = int32_t(uint16_t(v.count - 1));
        if (int16_t(v.count) < 0)
        {
            v.flags &= 0x5F;
            continue;
        }

        // FIFO B's voices alone play backwards. The first voice counts only with a sound to mix.
        const bool reverse = index == 1 && (v.flags & kFifoVoiceReverse);
        const int level = v.volume + 1;
        uint32_t position = v.position;
        if (!reverse)
        {
            v.position += kBlock;
        }

        if (n == 1)
        {
            active++;
        }

        std::array<int8_t, kBlock>& out = into_second ? second : fifo.buffer;
        if (level <= 1)
        {
            out.fill(0);
            continue;
        }

        for (int i = 0; i < kBlock; i++)
        {
            out[size_t(i)] = int8_t((rom.S8(position) * level) >> 5);
            position += reverse ? uint32_t(-1) : 1;
            if (metering && !Muted(index * 2 + n))
            {
                MeterPart(index * 2 + n, index, float(out[size_t(i)]));
            }
        }

        if (Muted(index * 2 + n))
        {
            out.fill(0);
        }

        if (reverse)
        {
            v.position = position;
        }

        if (n == 0)
        {
            into_second = true;
            active = 1;
        }
    }

    if (active == 0)
    {
        timers[size_t(index)] = 0;
        fifo.running = false;
        return;
    }

    if (active == 2)
    {
        for (size_t i = 0; i < kBlock; i++)
        {
            fifo.buffer[i] = int8_t(fifo.buffer[i] + second[i]);
        }
    }

    fifo.dma_on = true;
    if (fifo.goal != fifo.period)
    {
        fifo.period = fifo.goal;
        timers[size_t(index)] = ((0x10000 - uint32_t(fifo.goal)) | kTimerOn << 16);
    }
}

void Output::Impl::TickFifo(int index, std::vector<int8_t>& out)
{
    Fifo& fifo = fifos[size_t(index)];
    if (fifo.queued > 0)
    {
        fifo.queued--;
    }

    if (fifo.queued <= kBlock && fifo.dma_on)
    {
        fifo.queued += kBlock;
        out.insert(out.end(), fifo.buffer.begin(), fifo.buffer.end());
        FifoInterrupt(index);
    }
}

void Output::Impl::MixFifos(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    timer_changes.clear();
    for (int f = 0; f < 2; f++)
    {
        Fifo& fifo = fifos[size_t(f)];
        if (fifo.restarted)
        {
            fifo.queued = 4;
        }

        std::vector<int8_t>& out = f == 0 ? fifo_a : fifo_b;
        uint32_t period = FifoCycles(f);
        if (period && !fifo.ticking)
        {
            fifo.ticking = true;
            fifo.next_tick = period;
        }

        // A FIFO on the other's timer only follows it. Each timer's changes come from the FIFO whose note sets it. An
        // overflow at the frame's first cycle has already reached the hardware.
        const bool own_timer = ((sound_control_high >> (2 + 4 * f)) & 1) == uint32_t(f);
        if (own_timer && applied_periods[size_t(f)] != period)
        {
            applied_periods[size_t(f)] = period;
            timer_changes.push_back({0, f, period,
                                     !period               ? 0
                                     : fifo.next_tick == 0 ? period
                                                           : uint32_t(fifo.next_tick)});
        }

        while (period && fifo.next_tick < int64_t(agb::kFrameCycles))
        {
            TickFifo(f, out);
            const uint32_t next_period = FifoCycles(f);
            if (own_timer && next_period != period)
            {
                applied_periods[size_t(f)] = next_period;
                timer_changes.push_back({uint32_t(fifo.next_tick), f, next_period, next_period});
            }

            period = next_period;
            if (period)
            {
                fifo.next_tick += period;
            }
        }

        if (period)
        {
            fifo.next_tick -= int64_t(agb::kFrameCycles);
        }
        else
        {
            fifo.ticking = false;
        }
    }

    std::stable_sort(timer_changes.begin(), timer_changes.end(),
                     [](const FifoTimerChange& a, const FifoTimerChange& b) { return a.cycle < b.cycle; });
}

void Output::Impl::Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    fifo_a.clear();
    fifo_b.clear();
    if (metering)
    {
        voice_peaks = {};
    }

    if (enhanced)
    {
        MixEnhanced();
    }

    if (stage == Stage::kFifo)
    {
        MixFifos(fifo_a, fifo_b);
        return;
    }

    due_cycles += agb::kFrameCycles;
    const uint64_t blocks = due_cycles / block_cycles;
    due_cycles %= block_cycles;
    if (stage == Stage::kRing)
    {
        MixRing(fifo_a, fifo_b, blocks);
        return;
    }

    for (uint64_t b = 0; b < blocks; b++)
    {
        MixBlock(fifo_a, fifo_b);
    }
}

bool Output::Impl::RingEnd(OutputVoice& v)
{
    if (!(v.flags & kVoiceLooped))
    {
        v.flags = 0;
        return false;
    }

    const uint32_t table = v.key & 0x8000 ? second_table : info.sample_table;
    const uint32_t header = rom.U32(table + (v.key & 0x7FFFu) * 4);
    const uint32_t loop = rom.U32(header + 8);
    v.count = int32_t(rom.U32(header + 4) - loop);
    v.position = header + 12 + loop;
    return true;
}

void Output::Impl::MixRingPart(OutputVoice& v, int8_t* out, int samples, int level)
{
    const auto add = [&out](int value)
    {
        *out = int8_t(*out + value);
        out++;
    };

    if (v.step == 0x800)
    {
        // Two outputs for each sample byte, and the fraction stays.
        do
        {
            do
            {
                samples -= 2;
                if (samples < 0)
                {
                    return;
                }

                const int value = (rom.S8(v.position) * level) >> 4;
                v.position++;
                add(value);
                add(value);
                v.count--;
            } while (v.count > 0);
        } while (RingEnd(v));

        return;
    }

    uint32_t fraction = v.fraction;
    do
    {
        uint32_t advance = 0;
        do
        {
            v.position += advance;
            const int value = (rom.S8(v.position) * level) >> 4;
            do
            {
                samples--;
                if (samples < 0)
                {
                    v.fraction = uint16_t(fraction);
                    return;
                }

                add(value);
                fraction += v.step;
                advance = fraction >> 12;
            } while (advance == 0);

            fraction &= 0xFFF;
            v.count -= int32_t(advance);
        } while (v.count > 0);
    } while (RingEnd(v));

    v.fraction = uint16_t(fraction);
}

void Output::Impl::MixRing(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b, uint64_t blocks)
{
    if (blocks == 0)
    {
        return;
    }

    const int from = ring_position;
    for (uint64_t b = 0; b < blocks; b++)
    {
        ring_position += kBlock;
        if (ring_position >= kRingBytes)
        {
            ring_position = 0;
        }
    }

    // The part up to the ring's end, then the part from its start. A voice that ends in the first part still runs in
    // the second: the mixer checks the voice's flags once.
    const int first = ring_position > from ? ring_position - from : kRingBytes - from;
    const int second = ring_position > from ? 0 : ring_position;
    for (int f = 0; f < 2; f++)
    {
        std::vector<int8_t>& out = f == 0 ? fifo_a : fifo_b;
        out.assign(size_t(first + second), 0);
        for (int n = f * kRingVoices; n < (f + 1) * kRingVoices; n++)
        {
            OutputVoice& v = voices[size_t(n)];
            if (!(v.flags & kVoiceOn) || v.volume == 0)
            {
                continue;
            }

            const int level = v.volume + 1;
            if (!Muted(n) && !metering)
            {
                MixRingPart(v, out.data(), first, level);
                MixRingPart(v, out.data() + first, second, level);
                continue;
            }

            // A muted or metered voice mixes into a part of its own. A metered voice's part then joins the sum.
            std::vector<int8_t> part(out.size(), 0);
            MixRingPart(v, part.data(), first, level);
            MixRingPart(v, part.data() + first, second, level);
            if (Muted(n))
            {
                continue;
            }

            for (size_t i = 0; i < part.size(); i++)
            {
                MeterPart(n, f, float(part[i]));
                out[i] = int8_t(out[i] + part[i]);
            }
        }
    }
}

void Output::Impl::MeterPart(int voice, int fifo, float part)
{
    float& peak = voice_peaks[size_t(voice + 4)][size_t(fifo)];
    peak = std::max(peak, std::fabs(part));
}

void Output::Impl::StartEnhanced(int voice, uint32_t data, uint32_t size, int32_t loop, bool coded, bool reverse)
{
    EnhancedVoice& e = enhanced->voices[size_t(voice)];
    e = EnhancedVoice();
    e.data = data;
    e.size = size;
    e.loops = loop >= 0 && !reverse && uint32_t(loop) < size;
    e.loop_start = e.loops ? uint32_t(loop) : 0;
    e.reverse = reverse;
    e.starting = true;
    e.position = uint64_t(voices[size_t(voice)].fraction) << 20;
    if (coded)
    {
        e.decoded = uint16_t(DecodedSampleOf(data, size, loop) + 1);
    }
}

int Output::Impl::DecodedSampleOf(uint32_t data, uint32_t size, int32_t loop)
{
    std::vector<std::shared_ptr<const DecodedSample>>& decoded = enhanced->decoded;
    for (size_t i = 0; i < decoded.size(); i++)
    {
        if (decoded[i]->start == data)
        {
            return int(i);
        }
    }

    // The console's decoder takes each point in turn. A new pass starts at the loop's byte, with the decoder as the
    // first pass left it.
    const auto decode = [this](uint32_t from, uint32_t points, OutputVoice& v, std::vector<int8_t>& out)
    {
        uint32_t position = from;
        out.resize(points);
        for (uint32_t k = 0; k < points; k++)
        {
            out[k] = int8_t(NextCoded(v, position, k == 0 ? 0 : 1));
        }
    };

    auto sample = std::make_shared<DecodedSample>();
    sample->start = data;
    OutputVoice state;
    state.start = data;
    decode(data, size, state, sample->first);
    if (loop >= 0 && uint32_t(loop) < size)
    {
        decode(data + uint32_t(loop), size - uint32_t(loop), state, sample->loop);
    }

    decoded.push_back(std::move(sample));
    return int(decoded.size()) - 1;
}

float Output::Impl::Gain(Scale scale, int level) const
{
    switch (scale)
    {
    case Scale::kRoundUp7:
    case Scale::kDown7:
        return float(level) / 128.0f;
    case Scale::kUp4:
    case Scale::kDown4:
        return float(level) / 16.0f;
    case Scale::kTable:
    case Scale::kTableSigned:
        {
            const uint32_t row = uint32_t(rules.level_table) + uint32_t(level) * 256;
            return float(rom.S8(row + 0x7F) - rom.S8(row + 0x80)) / 255.0f;
        }
    }

    return 0.0f;
}

float Output::Impl::PointOf(const EnhancedVoice& e, int64_t point) const
{
    if (point < 0)
    {
        return 0.0f;
    }

    uint64_t at = uint64_t(point);
    const uint64_t loop_length = e.size - e.loop_start;
    if (e.decoded)
    {
        const DecodedSample& sample = *enhanced->decoded[size_t(e.decoded - 1)];
        if (at < e.size)
        {
            return float(sample.first[size_t(at)]);
        }

        return e.loops ? float(sample.loop[size_t((at - e.size) % loop_length)]) : 0.0f;
    }

    if (at >= e.size)
    {
        if (!e.loops)
        {
            return 0.0f;
        }

        at = e.loop_start + (at - e.loop_start) % loop_length;
    }

    return float(rom.S8(e.reverse ? e.data + e.size - 1 - uint32_t(at) : e.data + uint32_t(at)));
}

void Output::Impl::MixEnhancedVoice(int voice, float* a, float* b, std::array<float, 2> to, uint32_t step,
                                    uint32_t period)
{
    EnhancedVoice& e = enhanced->voices[size_t(voice)];
    const std::array<float, 2> from = e.starting ? to : e.levels;
    e.levels = to;
    e.starting = false;

    // A sample's position moves by the step for each of the console's points that the frame's time holds.
    const uint64_t move = (uint64_t(step) * agb::kFrameCycles << 20) / period;
    const uint64_t start = e.position;
    e.position += move;
    const bool heard = from[0] != 0.0f || from[1] != 0.0f || to[0] != 0.0f || to[1] != 0.0f;
    if (!enhanced->frames || !heard || Muted(voice))
    {
        return;
    }

    constexpr double kUnit = 4294967296.0;
    const size_t samples = agb::kEnhancedFrameSamples;
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(double(move) / (double(samples) * kUnit));
    const int64_t width = kernel.HalfWidth();
    const int64_t first = int64_t(start >> 32) - (width - 1);
    const int64_t last = int64_t((start + move) >> 32) + width;
    std::vector<float> input(size_t(last - first + 1));
    for (int64_t n = first; n <= last; n++)
    {
        input[size_t(n - first)] = PointOf(e, n);
    }

    const std::array<float, 2> slopes = {(to[0] - from[0]) / float(samples), (to[1] - from[1]) / float(samples)};
    for (size_t i = 0; i < samples; i++)
    {
        const uint64_t position = start + uint64_t(i) * move / samples;
        const int64_t point = int64_t(position >> 32);
        const float value = kernel.At(&input[size_t(point - (width - 1) - first)], uint32_t(position >> 16) & 0xFFFF);
        const float part_a = value * (from[0] + float(i) * slopes[0]);
        const float part_b = value * (from[1] + float(i) * slopes[1]);
        a[i] += part_a;
        b[i] += part_b;
        if (metering)
        {
            MeterPart(voice, 0, part_a);
            MeterPart(voice, 1, part_b);
        }
    }
}

void Output::Impl::MixEnhancedBus(int bus, const OutputEcho& e)
{
    // A delay of no blocks reads the line's oldest block: the line's whole length.
    const size_t samples = agb::kEnhancedFrameSamples;
    const uint32_t blocks = e.delay_blocks ? e.delay_blocks : 256;
    const uint64_t delay = (uint64_t(blocks) * block_cycles * samples + agb::kFrameCycles / 2) / agb::kFrameCycles;
    std::vector<float>& line = enhanced->lines[size_t(bus)];
    const size_t length = enhanced->line_samples;
    size_t write = enhanced->line_position[size_t(bus)];
    const float feedback = float(e.feedback) / 256.0f;
    float* out_a = enhanced->out.data();
    float* out_b = out_a + samples;
    const float* in_a = enhanced->inputs[size_t(bus)].data();
    const float* in_b = in_a + samples;
    const auto keep = [](float value)
    {
        return std::fabs(value) < 1.0f / 65536.0f ? 0.0f : value;
    };

    for (size_t i = 0; i < samples; i++)
    {
        const size_t read = (write + length - size_t(delay)) % length;
        const float a = line[read] * feedback + in_a[i];
        const float b = line[length + read] * feedback + in_b[i];
        if (echo_heard)
        {
            line[write] = keep(a);
            line[length + write] = keep(b);
        }
        out_a[i] += a;
        out_b[i] += b;
        write = (write + 1) % length;
    }

    enhanced->line_position[size_t(bus)] = write;
}

void Output::Impl::MixEnhanced()
{
    const size_t samples = agb::kEnhancedFrameSamples;
    Enhanced& e = *enhanced;
    std::fill(e.out.begin(), e.out.end(), 0.0f);
    float* out_a = e.out.data();
    float* out_b = out_a + samples;
    if (stage == Stage::kFifo)
    {
        // Each FIFO's two voices move on at the FIFO's timer, a point for each overflow.
        for (int n = 0; n < voice_count; n++)
        {
            const OutputVoice& v = voices[size_t(n)];
            const int f = n / 2;
            const uint32_t period = FifoCycles(f);
            if (!(v.flags & kVoiceOn) || period == 0)
            {
                continue;
            }

            const int level = v.volume + 1;
            const float gain = level <= 1 ? 0.0f : float(level) / 32.0f;
            MixEnhancedVoice(n, out_a, out_b, {f == 0 ? gain : 0.0f, f == 1 ? gain : 0.0f}, 0x1000, period);
        }

        return;
    }

    const uint32_t period = block_cycles / kBlock;
    if (stage == Stage::kRing)
    {
        for (int n = 0; n < voice_count; n++)
        {
            const OutputVoice& v = voices[size_t(n)];
            if (!(v.flags & kVoiceOn) || v.volume == 0)
            {
                continue;
            }

            const float gain = Gain(Scale::kDown4, v.volume + 1);
            const bool on_a = n < kRingVoices;
            MixEnhancedVoice(n, out_a, out_b, {on_a ? gain : 0.0f, on_a ? 0.0f : gain}, v.step, period);
        }

        return;
    }

    // The console's mask sends each voice to the dry sums or to its bus's input.
    for (std::vector<float>& input : e.inputs)
    {
        std::fill(input.begin(), input.end(), 0.0f);
    }

    const uint32_t routing = rules.mask_each_block ? echo[0].routing | (echo[1].routing << 1) : echo_mask;
    uint32_t mask = routing;
    std::array<float, 2 * agb::kEnhancedFrameSamples> unused{};
    for (int n = voice_count - 1; n >= 0; n--)
    {
        const OutputVoice& v = voices[size_t(n)];
        const uint32_t choice = mask & 3;
        mask >>= 2;
        if (!(v.flags & kVoiceOn) || (rules.skips_silent && v.volume == 0))
        {
            continue;
        }

        // A path with one value for both sides takes the right side's level. A keeping build's path stays the note's.
        const Path path = PathOf(v);
        const Scale scale = path == Path::kReverse                          ? rules.reverse
                            : path == Path::kEqual || path == Path::kDouble ? rules.equal
                                                                            : rules.unequal;
        const float to_a = Gain(scale, v.right);
        const bool one_value =
            path == Path::kEqual || path == Path::kDouble || (path == Path::kReverse && !rules.reverse_two_levels);
        const float to_b = one_value ? to_a : Gain(scale, v.left);
        float* a = choice == 0 ? out_a : choice == 3 ? unused.data() : e.inputs[choice - 1].data();
        float* b = choice == 0 ? out_b : a + samples;
        MixEnhancedVoice(n, a, b, {to_a, to_b}, v.step, period);
    }

    if (!rules.has_echo)
    {
        return;
    }

    const uint32_t buses = routing >> 30;
    for (int bus = 0; bus < 2; bus++)
    {
        if (buses & (1u << bus))
        {
            MixEnhancedBus(bus, echo[size_t(bus)]);
        }
    }

    // A line rings on through a frame without the mode's mixing, without any of it reaching the output.
    if (!e.frames)
    {
        std::fill(e.out.begin(), e.out.end(), 0.0f);
    }
}

} // namespace ntdlib::konami
