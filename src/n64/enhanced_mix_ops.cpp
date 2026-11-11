// SPDX-License-Identifier: MIT

// The commands of an audio list as the enhanced mixing mode runs them: in single-precision floating point, in the
// buffers of one rate.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/n64/enhanced_mix.h"

namespace ntdlib::n64
{

namespace
{

constexpr int kPageBits = 12;
constexpr std::size_t kPageSamples = std::size_t{1} << kPageBits;
constexpr float kScale15 = 1.0F / 32768.0F;
constexpr float kScale14 = 1.0F / 16384.0F;
constexpr float kScale16 = 1.0F / 65536.0F;

// Filters a group of 8 samples with the 8 taps of `table`, after the previous group of the same run. Returns the
// outputs on the scale of the inputs.
std::array<float, 8> FilterGroup(const std::array<float, 8>& previous, const std::array<float, 8>& current,
                                 const int16_t* table)
{
    std::array<float, 8> results;
    for (std::size_t lane = 0; lane < 8; lane++)
    {
        float sum = 0.0F;
        for (std::size_t k = 1; k < 8; k++)
        {
            if (lane + 8 - k < 8)
            {
                sum += previous[k] * static_cast<float>(table[lane + 8 - k]);
            }
        }

        for (std::size_t k = 0; k <= lane; k++)
        {
            sum += current[k] * static_cast<float>(table[lane - k]);
        }

        results[lane] = sum * kScale15;
    }

    return results;
}

} // namespace

std::size_t EnhancedMix::World::Place(uint32_t byte) const
{
    return static_cast<std::size_t>((byte & (AudioRsp::kDmemSize - 1)) >> 1) * static_cast<std::size_t>(scale);
}

float& EnhancedMix::World::Dmem(std::size_t index)
{
    return dmem[index >= dmem.size() ? index % dmem.size() : index];
}

float EnhancedMix::World::ReadMemory(uint64_t index) const
{
    const auto page = pages.find(index >> kPageBits);

    return page == pages.end() ? 0.0F : page->second[static_cast<std::size_t>(index & (kPageSamples - 1))];
}

void EnhancedMix::World::WriteMemory(uint64_t index, float value)
{
    auto& page = pages[index >> kPageBits];
    if (page.empty())
    {
        page.resize(kPageSamples);
    }

    page[static_cast<std::size_t>(index & (kPageSamples - 1))] = value;
}

void EnhancedMix::RunResample(World& world, const AudioResampleOp& op)
{
    ResamplerState& state = world.resamplers[op.state];
    if (op.init)
    {
        state = {};
    }

    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t in = ((static_cast<std::size_t>(op.start) >> 1) + op.in_tap) * scale;
    const std::size_t outputs = (op.bytes / 2) * scale;
    const std::size_t out = world.Place(op.out);

    // The state goes before the input. A later form of the microcode takes 2 of its samples, or each of them twice.
    std::size_t first = in - 4;
    if (op.mode == 2)
    {
        first = in - 2;
        world.Dmem(first) = state.history[0];
        world.Dmem(first + 1) = state.history[2];
    }
    else if (op.mode == 4)
    {
        first = in - 8;
        for (std::size_t i = 0; i < 4; i++)
        {
            world.Dmem(first + i * 2) = state.history[i];
            world.Dmem(first + i * 2 + 1) = state.history[i];
        }
    }
    else
    {
        for (std::size_t i = 0; i < 4; i++)
        {
            world.Dmem(first + i) = state.history[i];
        }
    }

    const std::array<int16_t, 256>& table = AudioResampleTable();
    std::vector<float> results(outputs);
    const uint64_t step = uint64_t{op.pitch} * 2;
    uint64_t place = state.fraction;
    for (std::size_t j = 0; j < outputs; j++)
    {
        const std::size_t tap = first + static_cast<std::size_t>(place >> 16);
        const int16_t* row = &table[(((place & 0xFFFF) >> 10) & 0x3F) * 4];
        float sum = world.Dmem(tap) * static_cast<float>(row[0]);
        sum += world.Dmem(tap + 1) * static_cast<float>(row[1]);
        sum += world.Dmem(tap + 2) * static_cast<float>(row[2]);
        sum += world.Dmem(tap + 3) * static_cast<float>(row[3]);
        results[j] = sum * kScale15;
        place += step;
    }

    const std::size_t end = first + static_cast<std::size_t>(place >> 16);
    state.fraction = static_cast<uint16_t>(place);
    for (std::size_t i = 0; i < 4; i++)
    {
        state.history[i] = world.Dmem(end + i);
    }

    for (std::size_t j = 0; j < outputs; j++)
    {
        world.Dmem(out + j) = results[j];
    }
}

void EnhancedMix::RunEnv(World& world, const AudioEnvOp& env)
{
    const std::size_t in = world.Place(env.in);
    const std::size_t dry_left = world.Place(env.dry_left);
    const std::size_t dry_right = world.Place(env.dry_right);
    const std::size_t wet_left = world.Place(env.wet_left);
    const std::size_t wet_right = world.Place(env.wet_right);
    const std::size_t count = (env.bytes / 2) * static_cast<std::size_t>(world.scale);
    const float each = 1.0F / static_cast<float>(count);
    const float left_step = (env.to_left - env.from_left) * each;
    const float right_step = (env.to_right - env.from_right) * each;
    const float wet_step = (env.to_wet - env.from_wet) * each;
    for (std::size_t i = 0; i < count; i++)
    {
        const auto done = static_cast<float>(i + 1);
        const float left = env.from_left + (left_step * done);
        const float right = env.from_right + (right_step * done);
        const float sample = world.Dmem(in + i);
        const float left_sample = sample * left;
        const float right_sample = sample * right;
        world.Dmem(dry_left + i) += left_sample * env.dry_factor_left;
        world.Dmem(dry_right + i) += right_sample * env.dry_factor_right;
        if (env.has_wet)
        {
            const float wet = env.from_wet + (wet_step * done);
            const float wet_of_left = left_sample * env.wet_factor_left * wet;
            const float wet_of_right = right_sample * env.wet_factor_right * wet;
            world.Dmem(wet_left + i) += env.swap ? wet_of_right : wet_of_left;
            world.Dmem(wet_right + i) += env.swap ? wet_of_left : wet_of_right;
        }
    }
}

void EnhancedMix::RunPole(World& world, const AudioOp& op, const int16_t* table)
{
    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t in = world.Place(op.in);
    const std::size_t out = world.Place(op.out);
    const std::size_t groups = op.bytes / 16;
    const float gain = static_cast<float>(op.gain);
    const auto scale_factor = static_cast<float>(static_cast<uint16_t>(table[0]));
    const int16_t* row1 = table + 1;
    const int16_t* row2 = table + 9;
    const uint64_t state = uint64_t{op.address >> 1} * scale;

    // Each of the `scale` runs through the samples takes the filter as the console's samples do.
    for (std::size_t run = 0; run < scale; run++)
    {
        std::array<float, 2> last = {};
        if (!op.init)
        {
            last[0] = world.ReadMemory(state + (2 * scale) + run);
            last[1] = world.ReadMemory(state + (3 * scale) + run);
        }

        std::array<float, 4> kept = {};
        for (std::size_t group = 0; group < groups; group++)
        {
            std::array<float, 8> samples;
            for (std::size_t i = 0; i < 8; i++)
            {
                samples[i] = world.Dmem(in + (((group * 8) + i) * scale) + run);
            }

            std::array<float, 8> results;
            for (std::size_t i = 0; i < 8; i++)
            {
                float sum = (static_cast<float>(row1[i]) * last[0]) + (static_cast<float>(row2[i]) * last[1]);
                for (std::size_t j = 0; j < i; j++)
                {
                    sum += samples[j] * (static_cast<float>(row2[i - j - 1]) * scale_factor * kScale16);
                }

                sum += samples[i] * gain;
                results[i] = sum * kScale14;
                world.Dmem(out + (((group * 8) + i) * scale) + run) = results[i];
            }

            last = {results[6], results[7]};
            kept = {results[4], results[5], results[6], results[7]};
        }

        for (std::size_t i = 0; i < 4; i++)
        {
            world.WriteMemory(state + (i * scale) + run, kept[i]);
        }
    }
}

void EnhancedMix::RunFilter(World& world, const AudioOp& op, const int16_t* table)
{
    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t place = world.Place(op.in);
    const std::size_t count = op.bytes / 2;
    const uint64_t state = uint64_t{op.address >> 1} * scale;
    for (std::size_t run = 0; run < scale; run++)
    {
        std::array<float, 8> previous = {};
        if (!op.init)
        {
            for (std::size_t i = 0; i < 8; i++)
            {
                previous[i] = world.ReadMemory(state + (i * scale) + run);
            }
        }

        for (std::size_t group = 0; group < count / 8; group++)
        {
            std::array<float, 8> current;
            for (std::size_t i = 0; i < 8; i++)
            {
                current[i] = world.Dmem(place + (((group * 8) + i) * scale) + run);
            }

            const std::array<float, 8> results = FilterGroup(previous, current, table);
            for (std::size_t lane = 0; lane < 8; lane++)
            {
                world.Dmem(place + (((group * 8) + lane) * scale) + run) = results[lane];
            }

            previous = current;
        }

        for (std::size_t i = 0; i < 8; i++)
        {
            world.WriteMemory(state + (i * scale) + run, previous[i]);
        }
    }
}

// Moves `count` samples in DMEM, a group of the op's size at a time.
void EnhancedMix::RunMove(World& world, const AudioOp& op, std::size_t count)
{
    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t from = world.Place(op.in);
    const std::size_t to = world.Place(op.out);
    const std::size_t group = (op.group / 2) * scale;
    std::vector<float> held(group);
    for (std::size_t done = 0; done < count; done += group)
    {
        const std::size_t now = std::min(group, count - done);
        for (std::size_t i = 0; i < now; i++)
        {
            held[i] = world.Dmem(from + done + i);
        }

        for (std::size_t i = 0; i < now; i++)
        {
            world.Dmem(to + done + i) = held[i];
        }
    }
}

// Loads `count` samples into DMEM: 16-bit samples from the image, or the samples that a list saved in main memory.
void EnhancedMix::RunLoad(World& world, const AudioOp& op, const AudioBus& bus, std::size_t count)
{
    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t place = world.Place(op.in);
    const uint64_t start = uint64_t{op.address >> 1} * scale;
    if (op.address >= AudioMemory::kImageBase)
    {
        std::vector<uint8_t> bytes(op.bytes);
        bus.Read(op.address, bytes.data(), bytes.size());
        for (std::size_t i = 0; i < count; i++)
        {
            world.Dmem(place + i) =
                static_cast<float>(static_cast<int16_t>(xsflib::LoadBe16(bytes.data() + ((i / scale) * 2))));
        }
    }
    else
    {
        for (std::size_t i = 0; i < count; i++)
        {
            world.Dmem(place + i) = world.ReadMemory(start + i);
        }
    }
}

// Copies `count` samples `op.group` times, one copy after another.
void EnhancedMix::RunDuplicate(World& world, const AudioOp& op, std::size_t count)
{
    const std::size_t in = world.Place(op.in);
    const std::size_t out = world.Place(op.out);
    std::vector<float> held(count);
    for (std::size_t i = 0; i < count; i++)
    {
        held[i] = world.Dmem(in + i);
    }

    for (std::size_t time = 0; time < op.group; time++)
    {
        for (std::size_t i = 0; i < count; i++)
        {
            world.Dmem(out + (time * count) + i) = held[i];
        }
    }
}

// Interleaves `count` samples of the left side with `count` samples of the right side.
void EnhancedMix::RunInterleave(World& world, const AudioOp& op, std::size_t count)
{
    const std::size_t left = world.Place(op.in);
    const std::size_t right = world.Place(op.extra);
    const std::size_t out = world.Place(op.out);
    std::vector<float> held(count * 2);
    for (std::size_t i = 0; i < count; i++)
    {
        held[i * 2] = world.Dmem(left + i);
        held[(i * 2) + 1] = world.Dmem(right + i);
    }

    for (std::size_t i = 0; i < held.size(); i++)
    {
        world.Dmem(out + i) = held[i];
    }
}

// Resamples `count` outputs without interpolation. Each output copies the DMEM sample at its place, rounded down.
void EnhancedMix::RunNearest(World& world, const AudioOp& op, std::size_t count)
{
    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t out = world.Place(op.out);
    std::vector<float> held(count);
    for (std::size_t j = 0; j < count; j++)
    {
        const uint64_t place = (uint64_t{op.address} * scale) + (uint64_t{static_cast<uint32_t>(op.gain)} * j);
        held[j] = world.Dmem(static_cast<std::size_t>(place >> 17));
    }

    for (std::size_t j = 0; j < count; j++)
    {
        world.Dmem(out + j) = held[j];
    }
}

void EnhancedMix::RunOp(World& world, const Frame& frame, const AudioOp& op, const AudioBus& bus) const
{
    const auto scale = static_cast<std::size_t>(world.scale);
    const std::size_t count = (op.bytes / 2) * scale;
    switch (op.kind)
    {
    case AudioOpKind::kClear:
        {
            const std::size_t out = world.Place(op.out);
            for (std::size_t i = 0; i < count; i++)
            {
                world.Dmem(out + i) = 0.0F;
            }

            break;
        }
    case AudioOpKind::kMove:
        RunMove(world, op, count);
        break;
    case AudioOpKind::kLoad:
        RunLoad(world, op, bus, count);
        break;
    case AudioOpKind::kSave:
        {
            const std::size_t place = world.Place(op.in);
            const uint64_t start = uint64_t{op.address >> 1} * scale;
            for (std::size_t i = 0; i < count; i++)
            {
                world.WriteMemory(start + i, world.Dmem(place + i));
            }

            break;
        }
    case AudioOpKind::kMix:
        {
            const std::size_t in = world.Place(op.in);
            const std::size_t out = world.Place(op.out);
            const float gain = static_cast<float>(static_cast<int16_t>(op.gain)) * kScale15;
            for (std::size_t i = 0; i < count; i++)
            {
                world.Dmem(out + i) += world.Dmem(in + i) * gain;
            }

            break;
        }
    case AudioOpKind::kAdd:
        {
            const std::size_t in = world.Place(op.in);
            const std::size_t out = world.Place(op.out);
            for (std::size_t i = 0; i < count; i++)
            {
                world.Dmem(out + i) += world.Dmem(in + i);
            }

            break;
        }
    case AudioOpKind::kGain:
        {
            const std::size_t place = world.Place(op.in);
            const float gain = static_cast<float>(op.gain) * kScale16;
            for (std::size_t i = 0; i < count; i++)
            {
                world.Dmem(place + i) *= gain;
            }

            break;
        }
    case AudioOpKind::kMultiply:
        {
            const std::size_t place = world.Place(op.in);
            const int16_t* factors = frame.ops.tables.data() + op.extra;
            for (std::size_t i = 0; i < count; i++)
            {
                world.Dmem(place + i) *= static_cast<float>(factors[(i / scale) % 32]);
            }

            break;
        }
    case AudioOpKind::kHalve:
        {
            const std::size_t in = world.Place(op.in);
            const std::size_t out = world.Place(op.out);
            for (std::size_t i = 0; i < count; i++)
            {
                world.Dmem(out + i) = world.Dmem(in + (i * 2));
            }

            break;
        }
    case AudioOpKind::kDuplicate:
        RunDuplicate(world, op, count);
        break;
    case AudioOpKind::kInterleave:
        RunInterleave(world, op, count);
        break;
    case AudioOpKind::kPole:
        RunPole(world, op, frame.ops.tables.data() + op.extra);
        break;
    case AudioOpKind::kFilter:
        RunFilter(world, op, frame.ops.tables.data() + op.extra);
        break;
    case AudioOpKind::kResample:
        RunResample(world, frame.ops.resamples[op.extra]);
        break;
    case AudioOpKind::kNearest:
        RunNearest(world, op, count);
        break;
    case AudioOpKind::kEnvMixer:
        RunEnv(world, frame.ops.envs[op.extra]);
        break;
    case AudioOpKind::kVoice:
    case AudioOpKind::kVoicesEnd:
    case AudioOpKind::kChunkEnd:
    case AudioOpKind::kMainResample:
        break;
    }
}

} // namespace ntdlib::n64
