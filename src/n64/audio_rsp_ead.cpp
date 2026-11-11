// SPDX-License-Identifier: MIT

// The commands of EAD's audio microcodes. The commands that they share with the standard microcode are in
// audio_rsp.cpp.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "audio_rsp_common.h"
#include "ntdlib/n64/audio_rsp.h"

namespace ntdlib::n64
{

namespace
{

using audio_rsp_common::Clamp16;
using audio_rsp_common::kCount;
using audio_rsp_common::kDmemMask;
using audio_rsp_common::kFlagInit;
using audio_rsp_common::kFlagLoop;
using audio_rsp_common::kIn;
using audio_rsp_common::kOut;
using audio_rsp_common::MixSample;
using audio_rsp_common::MultiplyHigh;
using audio_rsp_common::RecordDma;
using audio_rsp_common::RoundedUp;
using audio_rsp_common::Vector;
using audio_rsp_common::Wrap48;

// The output of the filter for a group of 8 samples: for each sample, the sum over the taps of the samples of this
// group and the group before it.
Vector FilterGroup(const Vector& previous, const Vector& current, const Vector& taps)
{
    Vector results = {};
    for (std::size_t lane = 0; lane < 8; lane++)
    {
        int64_t accumulator = 0x8000;
        for (std::size_t k = 1; k < 8; k++)
        {
            if (lane + 8 - k < 8)
            {
                accumulator = Wrap48(accumulator + int64_t{previous[k]} * taps[lane + 8 - k] * 2);
            }
        }

        for (std::size_t k = 0; k <= lane; k++)
        {
            accumulator = Wrap48(accumulator + int64_t{current[k]} * taps[lane - k] * 2);
        }

        results[lane] = Clamp16(accumulator >> 16);
    }

    return results;
}

} // namespace

void AudioRsp::RunEadFirst(const AudioCommand& command, AudioBus& bus)
{
    switch (command.Opcode() & 0x7F)
    {
    case 0x01:
        Adpcm(command, bus);
        break;
    case 0x02:
        ClearBuffer(command);
        break;
    case 0x05:
        Resample(command, bus);
        break;
    case 0x07:
        Segment(command);
        break;
    case 0x08:
        SetBuffer(command);
        break;
    case 0x0A:
        DmemMove(command);
        break;
    case 0x0B:
        LoadAdpcm(command, bus);
        break;
    case 0x0C:
        EadMixer(command);
        break;
    case 0x0D:
        Interleave(command);
        break;
    case 0x0E:
        PoleFilter(command, bus);
        break;
    case 0x0F:
        SetLoop(command);
        break;
    case 0x10:
        EadCopyRepeated(command);
        break;
    case 0x11:
        EadHalve(command);
        break;
    case 0x12:
        // The ramps' steps stay in two of the microcode's registers.
        gains_ = {};
        gains_[4] = static_cast<int16_t>((command.w0 >> 8) & 0xFF00);
        ramp_left_ = command.w1 >> 16;
        ramp_right_ = command.w1 & 0xFFFF;
        break;
    case 0x13:
        EadFirstEnvMixer(command);
        break;
    case 0x14:
        EadLoad(command, bus);
        break;
    case 0x15:
        EadSave(command, bus);
        break;
    case 0x16:
        gains_[0] = static_cast<int16_t>(command.w1 >> 16);
        gains_[2] = static_cast<int16_t>(command.w1);
        break;
    default:
        break;
    }
}

void AudioRsp::RunEadLate(const AudioCommand& command, AudioBus& bus)
{
    const bool third = microcode_ == AudioMicrocode::kEadThird;
    switch (command.Opcode() & 0x7F)
    {
    case 0x00:
        if (third)
        {
            EadMultiplyTable(command);
        }

        break;
    case 0x01:
        Adpcm(command, bus);
        break;
    case 0x02:
        ClearBuffer(command);
        break;
    case 0x04:
        EadAddBuffer(command);
        break;
    case 0x05:
        EadLateResample(command, bus);
        break;
    case 0x06:
        EadZeroOrderResample(command);
        break;
    case 0x07:
        EadFilter(command, bus);
        break;
    case 0x08:
        SetField(kIn, static_cast<uint16_t>(command.w0));
        SetField(kOut, static_cast<uint16_t>(command.w1 >> 16));
        SetField(kCount, static_cast<uint16_t>(command.w1));
        break;
    case 0x09:
        EadDuplicate(command);
        break;
    case 0x0A:
        if (third)
        {
            EadThirdDmemMove(command);
        }
        else
        {
            DmemMove(command);
        }

        break;
    case 0x0B:
        LoadAdpcm(command, bus);
        break;
    case 0x0C:
        EadMixer(command);
        break;
    case 0x0D:
        EadInterleave(command);
        break;
    case 0x0E:
        EadGain(command);
        break;
    case 0x0F:
        SetLoop(command);
        break;
    case 0x10:
        EadCopyRepeated(command);
        break;
    case 0x11:
        EadHalve(command);
        break;
    case 0x12:
        {
            gains_ = {};
            ramp_wet_ = command.w0 & 0xFFFF;
            const uint32_t wet = (command.w0 >> 8) & 0xFF00;
            gains_[4] = static_cast<int16_t>(wet);
            gains_[5] = static_cast<int16_t>(wet + ramp_wet_);
            ramp_left_ = command.w1 >> 16;
            ramp_right_ = command.w1 & 0xFFFF;
            break;
        }
    case 0x13:
        EadLateEnvMixer(command);
        break;
    case 0x14:
        EadLoad(command, bus);
        break;
    case 0x15:
        EadSave(command, bus);
        break;
    case 0x16:
        {
            const uint32_t left = command.w1 >> 16;
            const uint32_t right = command.w1 & 0xFFFF;
            gains_[0] = static_cast<int16_t>(left);
            gains_[1] = static_cast<int16_t>(left + ramp_left_);
            gains_[2] = static_cast<int16_t>(right);
            gains_[3] = static_cast<int16_t>(right + ramp_right_);
            break;
        }
    case 0x17:
        EadDecodeBytes(command, bus);
        break;
    default:
        break;
    }
}

void AudioRsp::EadLoad(const AudioCommand& command, const AudioBus& bus)
{
    const uint32_t length = (command.w0 >> 12) & 0xFF0;
    if (length != 0)
    {
        DmaRead((command.w0 & 0xFFFF) + layout_.buffers, Address(command.w1), length - 1, bus);
        RecordDma(ops_, AudioOpKind::kLoad, (command.w0 & 0xFFFF) + layout_.buffers, Address(command.w1), length);
    }
}

void AudioRsp::EadSave(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t length = (command.w0 >> 12) & 0xFF0;
    if (length != 0)
    {
        DmaWrite((command.w0 & 0xFFFF) + layout_.buffers, Address(command.w1), length - 1, bus);
        RecordDma(ops_, AudioOpKind::kSave, (command.w0 & 0xFFFF) + layout_.buffers, Address(command.w1), length);
    }
}

void AudioRsp::EadMixer(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>((command.w0 >> 12) & 0xFF0);
    const auto gain = static_cast<int16_t>(command.w0);
    uint32_t out = (command.w1 & 0xFFFF) + layout_.buffers;
    uint32_t in = (command.w1 >> 16) + layout_.buffers;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kMix, in, out, RoundedUp(count, 32));
        ops_->ops.back().gain = gain;
    }

    Vector out0 = {};
    Vector out1 = {};
    Vector in0 = {};
    Vector in1 = {};
    LoadQuad(out0, out);
    LoadQuad(in0, in);
    LoadQuad(out1, out + 16);
    LoadQuad(in1, in + 16);
    do
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            out0[i] = MixSample(out0[i], in0[i], gain);
            out1[i] = MixSample(out1[i], in1[i], gain);
        }

        count -= 32;
        in += 32;
        LoadQuad(in0, in);
        StoreQuad(out0, out);
        LoadQuad(out0, out + 32);
        StoreQuad(out1, out + 16);
        LoadQuad(in1, in + 16);
        out += 32;
        LoadQuad(out1, out + 16);
    } while (count > 0);
}

void AudioRsp::EadAddBuffer(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>((command.w0 >> 12) & 0xFF0);
    uint32_t out = command.w1 & 0xFFFF;
    uint32_t in = command.w1 >> 16;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kAdd, in, out, RoundedUp(count, 64));
    }

    do
    {
        for (uint32_t i = 0; i < 32; i++)
        {
            const uint32_t at = i * 2;
            const int32_t sum = int32_t{static_cast<int16_t>(Get16(out + at))} + static_cast<int16_t>(Get16(in + at));
            Put16(out + at, static_cast<uint16_t>(Clamp16(sum)));
        }

        in += 64;
        out += 64;
        count -= 64;
    } while (count > 0);
}

void AudioRsp::EadCopyRepeated(const AudioCommand& command)
{
    int32_t times = static_cast<int32_t>((command.w0 >> 16) & 0xFF);
    uint32_t from = (command.w0 & 0xFFFF) + layout_.buffers;
    uint32_t to = (command.w1 >> 16) + layout_.buffers;
    if (ops_ != nullptr)
    {
        const auto each = RoundedUp(static_cast<int32_t>(command.w1 & 0xFFFF), 32);
        RecordOp(AudioOpKind::kMove, from, to, each * static_cast<uint32_t>(std::max(times, 1)));
        ops_->ops.back().group = 32;
    }

    Vector first = {};
    Vector second = {};
    do
    {
        times--;
        int32_t count = static_cast<int32_t>(command.w1 & 0xFFFF);
        do
        {
            LoadQuad(first, from);
            LoadQuad(second, from + 16);
            count -= 32;
            from += 32;
            StoreQuad(first, to);
            StoreQuad(second, to + 16);
            to += 32;
        } while (count > 0);
    } while (times > 0);
}

void AudioRsp::EadDuplicate(const AudioCommand& command)
{
    int32_t times = static_cast<int32_t>((command.w0 >> 16) & 0xFF);
    const uint32_t from = command.w0 & 0xFFFF;
    uint32_t to = command.w1 >> 16;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kDuplicate, from, to, 128);
        ops_->ops.back().group = static_cast<uint32_t>(std::max(times, 1));
    }

    std::array<Vector, 8> vectors = {};
    for (uint32_t i = 0; i < 8; i++)
    {
        LoadQuad(vectors[i], from + i * 16);
    }

    do
    {
        times--;
        for (uint32_t i = 0; i < 8; i++)
        {
            StoreQuad(vectors[i], to + i * 16);
        }

        to += 128;
    } while (times > 0);
}

void AudioRsp::EadHalve(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>(command.w0 & 0xFFFF);
    uint32_t in = (command.w1 >> 16) + layout_.buffers;
    uint32_t out = (command.w1 & 0xFFFF) + layout_.buffers;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kHalve, in, out, RoundedUp(count, 8) * 2);
    }

    do
    {
        // Keeps the first and the third of every 4 samples.
        for (uint32_t i = 0; i < 4; i++)
        {
            const uint16_t first = Get16(in + i * 8);
            const uint16_t second = Get16(in + i * 8 + 4);
            Put16(out + i * 4, first);
            Put16(out + i * 4 + 2, second);
        }

        in += 32;
        out += 16;
        count -= 8;
    } while (count > 0);
}

void AudioRsp::EadFirstEnvMixer(const AudioCommand& command)
{
    uint32_t in = ((command.w0 >> 12) & 0xFF0) + layout_.buffers;
    uint32_t dry_left = ((command.w1 >> 20) & 0xFF0) + layout_.buffers;
    uint32_t dry_right = ((command.w1 >> 12) & 0xFF0) + layout_.buffers;
    uint32_t wet_left = ((command.w1 >> 4) & 0xFF0) + layout_.buffers;
    uint32_t wet_right = ((command.w1 << 4) & 0xFF0) + layout_.buffers;
    const uint16_t mask_left = (command.w0 & 2) != 0 ? 0xFFFF : 0;
    const uint16_t mask_right = (command.w0 & 1) != 0 ? 0xFFFF : 0;
    int32_t count = static_cast<int32_t>((command.w0 >> 8) & 0xFF);
    const auto wet_gain = static_cast<uint16_t>(gains_[4]);
    AudioEnvOp env;
    if (ops_ != nullptr)
    {
        constexpr float kFull = 1.0F / 65536.0F;
        env.in = in & kDmemMask;
        env.dry_left = dry_left & kDmemMask;
        env.dry_right = dry_right & kDmemMask;
        env.wet_left = wet_left & kDmemMask;
        env.wet_right = wet_right & kDmemMask;
        env.bytes = static_cast<uint32_t>(std::max(count, 8)) * 2;

        // The microcode's ramps run past the end where the count isn't a multiple of their steps. The gains at the end
        // of the samples don't.
        const float groups = static_cast<float>(std::max(count, 8)) / 8.0F;
        const auto left_from = static_cast<float>(static_cast<uint16_t>(gains_[0]));
        const auto right_from = static_cast<float>(static_cast<uint16_t>(gains_[2]));
        env.from_left = left_from * kFull;
        env.from_right = right_from * kFull;
        env.to_left = (left_from + (static_cast<float>(static_cast<int16_t>(ramp_left_)) * groups)) * kFull;
        env.to_right = (right_from + (static_cast<float>(static_cast<int16_t>(ramp_right_)) * groups)) * kFull;
        env.from_wet = static_cast<float>(wet_gain) * kFull;
        env.to_wet = env.from_wet;
        env.dry_factor_left = mask_left != 0 ? -1.0F : 1.0F;
        env.dry_factor_right = mask_right != 0 ? -1.0F : 1.0F;
        env.wet_factor_left = env.dry_factor_left;
        env.wet_factor_right = env.dry_factor_right;
        RecordEnv(env);
    }

    do
    {
        const auto left_gain = static_cast<uint16_t>(gains_[0]);
        const auto right_gain = static_cast<uint16_t>(gains_[2]);
        Vector samples = {};
        Vector dry_l = {};
        Vector dry_r = {};
        Vector wet_l = {};
        Vector wet_r = {};
        LoadQuad(samples, in);
        LoadQuad(dry_l, dry_left);
        LoadQuad(dry_r, dry_right);
        LoadQuad(wet_l, wet_left);
        LoadQuad(wet_r, wet_right);
        for (std::size_t i = 0; i < 8; i++)
        {
            const auto left = static_cast<int16_t>(MultiplyHigh(samples[i], left_gain) ^ mask_left);
            const auto right = static_cast<int16_t>(MultiplyHigh(samples[i], right_gain) ^ mask_right);
            dry_l[i] = Clamp16(int32_t{dry_l[i]} + left);
            dry_r[i] = Clamp16(int32_t{dry_r[i]} + right);
            wet_l[i] = Clamp16(int32_t{wet_l[i]} + MultiplyHigh(left, wet_gain));
            wet_r[i] = Clamp16(int32_t{wet_r[i]} + MultiplyHigh(right, wet_gain));
        }

        StoreQuad(dry_l, dry_left);
        StoreQuad(dry_r, dry_right);
        StoreQuad(wet_l, wet_left);
        StoreQuad(wet_r, wet_right);
        in += 16;
        dry_left += 16;
        dry_right += 16;
        wet_left += 16;
        wet_right += 16;
        count -= 8;
        gains_[0] = static_cast<int16_t>(left_gain + ramp_left_);
        gains_[2] = static_cast<int16_t>(right_gain + ramp_right_);
    } while (count > 0);
}

void AudioRsp::EadLateEnvMixer(const AudioCommand& command)
{
    // The ramps' steps double here, in their registers, for each group of 16 samples.
    ramp_left_ *= 2;
    ramp_right_ *= 2;
    ramp_wet_ *= 2;
    const std::array<uint16_t, 6> steps = {static_cast<uint16_t>(ramp_left_),  static_cast<uint16_t>(ramp_left_),
                                           static_cast<uint16_t>(ramp_right_), static_cast<uint16_t>(ramp_right_),
                                           static_cast<uint16_t>(ramp_wet_),   static_cast<uint16_t>(ramp_wet_)};
    EnvPlaces places = {.in = (command.w0 >> 12) & 0xFF0,
                        .dry_left = (command.w1 >> 20) & 0xFF0,
                        .dry_right = (command.w1 >> 12) & 0xFF0,
                        .wet_left = (command.w1 >> 4) & 0xFF0,
                        .wet_right = (command.w1 << 4) & 0xFF0};

    // The microcode masks with these values. The wet ones aren't a whole inversion.
    EnvMasks masks;
    masks.left = (command.w0 & 2) != 0 ? 0xFFFF : 0;
    masks.right = (command.w0 & 1) != 0 ? 0xFFFF : 0;
    masks.wet_left = (command.w0 & 8) != 0 ? 0xFFFC : 0;
    masks.wet_right = (command.w0 & 4) != 0 ? 0xFFFE : 0;
    masks.swap = (command.w0 & 0x10) != 0;
    int32_t count = static_cast<int32_t>((command.w0 >> 8) & 0xFF);
    if (ops_ != nullptr)
    {
        RecordEadLateEnv(places, masks, count);
    }

    do
    {
        for (uint32_t half = 0; half < 2; half++)
        {
            const auto left_gain = static_cast<uint16_t>(gains_[half]);
            const auto right_gain = static_cast<uint16_t>(gains_[2 + half]);
            const auto wet_gain = static_cast<uint16_t>(gains_[4 + half]);
            const uint32_t step = half * 16;
            Vector samples = {};
            Vector dry_l = {};
            Vector dry_r = {};
            Vector wet_l = {};
            Vector wet_r = {};
            LoadQuad(samples, places.in + step);
            LoadQuad(dry_l, places.dry_left + step);
            LoadQuad(dry_r, places.dry_right + step);
            LoadQuad(wet_l, places.wet_left + step);
            LoadQuad(wet_r, places.wet_right + step);
            for (std::size_t i = 0; i < 8; i++)
            {
                const auto left = static_cast<int16_t>(MultiplyHigh(samples[i], left_gain) ^ masks.left);
                const auto right = static_cast<int16_t>(MultiplyHigh(samples[i], right_gain) ^ masks.right);
                dry_l[i] = Clamp16(int32_t{dry_l[i]} + left);
                dry_r[i] = Clamp16(int32_t{dry_r[i]} + right);
                const auto wet_of_left = static_cast<int16_t>(MultiplyHigh(left, wet_gain) ^ masks.wet_left);
                const auto wet_of_right = static_cast<int16_t>(MultiplyHigh(right, wet_gain) ^ masks.wet_right);
                wet_l[i] = Clamp16(int32_t{wet_l[i]} + (masks.swap ? wet_of_right : wet_of_left));
                wet_r[i] = Clamp16(int32_t{wet_r[i]} + (masks.swap ? wet_of_left : wet_of_right));
            }

            StoreQuad(dry_l, places.dry_left + step);
            StoreQuad(dry_r, places.dry_right + step);
            StoreQuad(wet_l, places.wet_left + step);
            StoreQuad(wet_r, places.wet_right + step);
        }

        places.in += 32;
        places.dry_left += 32;
        places.dry_right += 32;
        places.wet_left += 32;
        places.wet_right += 32;
        count -= 16;
        for (std::size_t i = 0; i < steps.size(); i++)
        {
            gains_[i] = static_cast<int16_t>(static_cast<uint16_t>(gains_[i]) + steps[i]);
        }
    } while (count > 0);
}

void AudioRsp::RecordEadLateEnv(const EnvPlaces& places, const EnvMasks& masks, int32_t count)
{
    constexpr float kFull = 1.0F / 65536.0F;
    AudioEnvOp env;
    RecordPlaces(env, places);
    env.bytes = static_cast<uint32_t>(std::max(count, 16)) * 2;
    env.swap = masks.swap;

    // Each pair of a microcode's gains holds a value and the value 8 samples on. The ramps run past the end where
    // the count isn't a multiple of their steps. The gains at the end of the samples don't.
    const float groups = static_cast<float>(std::max(count, 16)) / 8.0F;
    const auto gain_at_end = [this, groups](std::size_t first)
    {
        const auto from = static_cast<float>(static_cast<uint16_t>(gains_[first]));
        const auto step =
            static_cast<int16_t>(static_cast<uint16_t>(gains_[first + 1]) - static_cast<uint16_t>(gains_[first]));

        return from + (static_cast<float>(step) * groups);
    };

    env.from_left = static_cast<float>(static_cast<uint16_t>(gains_[0])) * kFull;
    env.from_right = static_cast<float>(static_cast<uint16_t>(gains_[2])) * kFull;
    env.from_wet = static_cast<float>(static_cast<uint16_t>(gains_[4])) * kFull;
    env.to_left = gain_at_end(0) * kFull;
    env.to_right = gain_at_end(2) * kFull;
    env.to_wet = gain_at_end(4) * kFull;
    env.dry_factor_left = masks.left != 0 ? -1.0F : 1.0F;
    env.dry_factor_right = masks.right != 0 ? -1.0F : 1.0F;
    env.wet_factor_left = env.dry_factor_left * (masks.wet_left != 0 ? -1.0F : 1.0F);
    env.wet_factor_right = env.dry_factor_right * (masks.wet_right != 0 ? -1.0F : 1.0F);
    RecordEnv(env);
}

void AudioRsp::EadLateResample(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t flags = command.w0 >> 16;
    const auto pitch = static_cast<uint16_t>(command.w0);
    int32_t start = static_cast<int16_t>(Field(kIn));
    const int32_t out = static_cast<int16_t>(Field(kOut));
    const int32_t count = static_cast<int16_t>(Field(kCount));
    const uint32_t state = Address(command.w1);
    if ((flags & kFlagInit) != 0)
    {
        Put16(layout_.scratch + 8, 0);
        Put32(layout_.scratch, 0);
        Put32(layout_.scratch + 4, 0);
    }
    else
    {
        DmaRead(layout_.scratch, state, 31, bus);
    }

    const std::array<uint16_t, 4> saved = {Get16(layout_.scratch), Get16(layout_.scratch + 2),
                                           Get16(layout_.scratch + 4), Get16(layout_.scratch + 6)};
    if ((flags & 2) != 0)
    {
        // A resampler that takes 2 samples of its state before the input.
        start -= 4;
        Put16(static_cast<uint32_t>(start), saved[0]);
        Put16(static_cast<uint32_t>(start) + 2, saved[2]);
    }
    else if ((flags & 4) != 0)
    {
        // The state's 4 samples, each twice.
        start -= 16;
        for (uint32_t i = 0; i < 4; i++)
        {
            Put16(static_cast<uint32_t>(start) + i * 4, saved[i]);
            Put16(static_cast<uint32_t>(start) + i * 4 + 2, saved[i]);
        }
    }
    else
    {
        start -= 8;
        for (uint32_t i = 0; i < 4; i++)
        {
            Put16(static_cast<uint32_t>(start) + i * 2, saved[i]);
        }
    }

    if (ops_ != nullptr)
    {
        AudioResampleOp op;
        op.start = static_cast<uint32_t>(start) & kDmemMask;
        op.out = static_cast<uint32_t>(out) & kDmemMask;
        op.bytes = static_cast<uint32_t>(std::max(count, 0));
        op.state = state;
        op.first_fraction = Get16(layout_.scratch + 8);
        op.pitch = pitch;
        op.init = (flags & kFlagInit) != 0;
        op.mode = (flags & 2) != 0 ? 2 : ((flags & 4) != 0 ? 4 : 0);
        op.in_tap = op.mode == 2 ? 2 : (op.mode == 4 ? 8 : 4);
        RecordResample(op);
    }

    const ResampleEnd end = ResampleLoop(start, out, count, Get16(layout_.scratch + 8), pitch, 224);
    Put16(layout_.scratch + 8, end.fraction);
    for (uint32_t i = 0; i < 8; i++)
    {
        dmem_[(layout_.scratch + i) & kDmemMask] = dmem_[(static_cast<uint32_t>(end.place) + i) & kDmemMask];
    }

    DmaWrite(layout_.scratch, state, 31, bus);
}

void AudioRsp::EadZeroOrderResample(const AudioCommand& command)
{
    auto place = (static_cast<uint32_t>(static_cast<int16_t>(Field(kIn))) << 16) | (command.w1 & 0xFFFF);
    uint32_t out = Field(kOut);
    int32_t count = static_cast<int16_t>(Field(kCount));
    const uint32_t step = (command.w0 & 0xFFFF) << 2;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kNearest, 0, out, RoundedUp(count, 8));
        AudioOp& op = ops_->ops.back();
        op.address = place;
        op.gain = static_cast<int32_t>(step);
    }

    do
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            Put16(out + i * 2, Get16((place >> 16) & 0xFFFE));
            place += step;
        }

        count -= 8;
        out += 8;
    } while (count > 0);
}

void AudioRsp::EadFilter(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t state_place = layout_.scratch;
    const uint32_t coefficients = layout_.scratch + 32;
    const uint32_t address = Address(command.w1);
    const Vector zero = {};
    StoreQuad(zero, state_place);
    StoreQuad(zero, state_place + 16);
    const uint32_t flags = (command.w0 >> 16) & 0xFF;
    if (flags >= 2)
    {
        // Sets up the command that follows: its length, and the filter's taps.
        filter_count_ = static_cast<int32_t>(command.w0 & 0xFFFF);
        StoreQuad(zero, coefficients);
        StoreQuad(zero, coefficients + 32);
        DmaRead(coefficients + 16, address, 15, bus);
        return;
    }

    if (flags == 0)
    {
        DmaRead(state_place, address, 31, bus);
    }

    // The taps: the mean of the set's taps and the taps that the state holds.
    Vector first_taps = {};
    Vector second_taps = {};
    LoadQuad(first_taps, coefficients + 16);
    LoadQuad(second_taps, state_place + 16);
    Vector taps = {};
    for (std::size_t i = 0; i < 8; i++)
    {
        const int64_t accumulator = 0x8000 + int64_t{first_taps[i]} * 0x4000 * 2 + int64_t{second_taps[i]} * 0x4000 * 2;
        taps[i] = Clamp16(accumulator >> 16);
    }

    StoreQuad(taps, coefficients + 16);
    StoreQuad(taps, state_place + 16);

    Vector previous = {};
    LoadQuad(previous, state_place);
    Vector current = {};
    uint32_t buffer = command.w0 & 0xFFFF;
    int32_t count = filter_count_;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kFilter, buffer, buffer, RoundedUp(count, 16));
        AudioOp& op = ops_->ops.back();
        op.init = flags != 0;
        op.address = address;
        op.extra = static_cast<uint32_t>(ops_->tables.size());
        ops_->tables.insert(ops_->tables.end(), taps.begin(), taps.end());
    }

    do
    {
        LoadQuad(current, buffer);
        const Vector results = FilterGroup(previous, current, taps);
        count -= 16;
        StoreQuad(results, buffer);
        buffer += 16;
        previous = current;
    } while (count > 0);

    filter_count_ = count;
    StoreQuad(current, state_place);
    DmaWrite(state_place, address, 31, bus);
}

void AudioRsp::EadGain(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>(command.w0 & 0xFFFF);
    uint32_t place = command.w1 >> 16;
    const uint16_t fraction = static_cast<uint16_t>((command.w0 >> 4) & 0xF000);
    const int32_t whole = static_cast<int32_t>((command.w0 >> 20) & 0xF);
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kGain, place, place, RoundedUp(count, 32));
        ops_->ops.back().gain = (whole * 65536) + fraction;
    }

    do
    {
        for (uint32_t half = 0; half < 2; half++)
        {
            Vector samples = {};
            LoadQuad(samples, place + half * 16);
            for (std::size_t i = 0; i < 8; i++)
            {
                const int64_t accumulator = int64_t{samples[i]} * fraction + (int64_t{samples[i]} * whole) * 65536;
                samples[i] = Clamp16(Wrap48(accumulator) >> 16);
            }

            StoreQuad(samples, place + half * 16);
        }

        count -= 32;
        place += 32;
    } while (count > 0);
}

void AudioRsp::EadInterleave(const AudioCommand& command)
{
    uint32_t out = command.w0 & 0xFFFF;
    int32_t count = static_cast<int32_t>((command.w0 >> 12) & 0xFF0);
    uint32_t left = command.w1 >> 16;
    uint32_t right = command.w1 & 0xFFFF;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kInterleave, left, out, RoundedUp(count, 8));
        ops_->ops.back().extra = right & kDmemMask;
    }

    do
    {
        std::array<uint16_t, 4> left_samples;
        std::array<uint16_t, 4> right_samples;
        for (uint32_t i = 0; i < 4; i++)
        {
            left_samples[i] = Get16(left + i * 2);
            right_samples[i] = Get16(right + i * 2);
        }

        for (uint32_t i = 0; i < 4; i++)
        {
            Put16(out + i * 4, left_samples[i]);
        }

        for (uint32_t i = 0; i < 4; i++)
        {
            Put16(out + i * 4 + 2, right_samples[i]);
        }

        count -= 8;
        out += 16;
        left += 8;
        right += 8;
    } while (count > 0);
}

void AudioRsp::EadDecodeBytes(const AudioCommand& command, AudioBus& bus)
{
    uint32_t in = Field(kIn);
    uint32_t out = Field(kOut);
    int32_t count = static_cast<int32_t>(Field(kCount));
    const uint32_t state = Address(command.w1);
    const uint32_t flags = command.w0 >> 16;
    const Vector zero = {};
    StoreQuad(zero, out);
    StoreQuad(zero, out + 16);
    if ((flags & kFlagInit) == 0)
    {
        DmaRead(out, (flags & kFlagLoop) != 0 ? Get32(layout_.fields + layout_.loop) : state, 31, bus);
    }

    out += 32;
    while (count > 0)
    {
        // A packed load reads each byte as the upper half of a sample. It stays in the 16 bytes around its 8-byte
        // place.
        const uint32_t base = in & ~uint32_t{7};
        for (uint32_t half = 0; half < 2; half++)
        {
            for (uint32_t lane = 0; lane < 8; lane++)
            {
                const uint32_t place = base + half * 8 + (((in & 7) + lane) & 15);
                Put16(out + half * 16 + lane * 2, static_cast<uint16_t>(dmem_[place & kDmemMask] << 8));
            }
        }

        in += 16;
        out += 32;
        count -= 32;
    }

    DmaWrite(out - 32, state, 31, bus);
}

void AudioRsp::EadThirdDmemMove(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>(command.w1 & 0xFFFF);
    uint32_t from = command.w0 & 0xFFFF;
    uint32_t to = command.w1 >> 16;
    if (ops_ != nullptr && count > 0)
    {
        RecordOp(AudioOpKind::kMove, from, to, static_cast<uint32_t>(count));
        ops_->ops.back().group = 16;
    }

    while (count >= 16)
    {
        std::array<uint8_t, 16> bytes;
        for (uint32_t i = 0; i < 16; i++)
        {
            bytes[i] = dmem_[(from + i) & kDmemMask];
        }

        for (uint32_t i = 0; i < 16; i++)
        {
            dmem_[(to + i) & kDmemMask] = bytes[i];
        }

        count -= 16;
        from += 16;
        to += 16;
    }

    while (count > 0)
    {
        Put16(to, Get16(from));
        count -= 2;
        from += 2;
        to += 2;
    }
}

void AudioRsp::EadMultiplyTable(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>(command.w0 & 0xFFFF);
    uint32_t place = command.w1 >> 16;
    const uint32_t table = (command.w1 & 0xFFFF) + ((command.w0 >> 16) & 0xFF);
    std::array<Vector, 4> factors = {};
    for (uint32_t vector = 0; vector < 4; vector++)
    {
        LoadDouble(factors[vector], 0, table + vector * 16);
        LoadDouble(factors[vector], 4, table + vector * 16 + 8);
    }

    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kMultiply, place, place, RoundedUp(count, 64));
        ops_->ops.back().extra = static_cast<uint32_t>(ops_->tables.size());
        for (const Vector& factor : factors)
        {
            ops_->tables.insert(ops_->tables.end(), factor.begin(), factor.end());
        }
    }

    do
    {
        for (uint32_t vector = 0; vector < 4; vector++)
        {
            Vector samples = {};
            LoadQuad(samples, place + vector * 16);
            for (std::size_t i = 0; i < 8; i++)
            {
                samples[i] = Clamp16(int32_t{samples[i]} * factors[vector][i]);
            }

            StoreQuad(samples, place + vector * 16);
        }

        count -= 64;
        place += 64;
    } while (count > 0);
}

} // namespace ntdlib::n64
