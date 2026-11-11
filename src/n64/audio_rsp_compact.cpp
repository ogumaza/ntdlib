// SPDX-License-Identifier: MIT

// The commands of the compact audio microcode. The compact microcode uses the decoder and the resampler of the
// standard microcode, in audio_rsp.cpp.

#include <array>
#include <cstddef>
#include <cstdint>

#include "audio_rsp_common.h"
#include "ntdlib/n64/audio_rsp.h"

namespace ntdlib::n64
{

namespace
{

// The compact microcode's fixed buffers, counted from its base, and its length of a buffer in bytes.
constexpr uint32_t kCompactDryLeft = 2512;
constexpr uint32_t kCompactDryRight = 2880;
constexpr uint32_t kCompactWetLeft = 3248;
constexpr uint32_t kCompactWetRight = 3616;
constexpr uint32_t kCompactSecond = 1632;
constexpr uint32_t kCompactMain = 1264;
constexpr int32_t kCompactBytes = 368;
constexpr uint32_t kCompactLoopAddress = 14;

using audio_rsp_common::kDmemMask;
using audio_rsp_common::LinearFirstRamp;
using audio_rsp_common::LinearLimitLanes;
using audio_rsp_common::LinearStepLanes;
using audio_rsp_common::MixSample;
using audio_rsp_common::MultiplyFraction;
using audio_rsp_common::RecordDma;
using audio_rsp_common::RoundedUp;

} // namespace

void AudioRsp::RunCompact(const AudioCommand& command, AudioBus& bus)
{
    switch (command.Opcode() & 0x7F)
    {
    case 0x01:
        CompactAdpcm(command, bus);
        break;
    case 0x02:
        CompactClear(command);
        break;
    case 0x03:
        CompactEnvMixer(command, bus);
        break;
    case 0x04:
        CompactLoad(command, bus);
        break;
    case 0x05:
        CompactResample(command, bus);
        break;
    case 0x06:
        CompactSave(command, bus);
        break;
    case 0x09:
        CompactSetVolume(command);
        break;
    case 0x0A:
        CompactMove(command);
        break;
    case 0x0B:
        DmaRead(layout_.book, command.w1 & 0xFFFFFF, (command.w0 & 0xFFFF) - 1, bus);
        break;
    case 0x0C:
        CompactMixer(command);
        break;
    case 0x0D:
        CompactInterleave();
        break;
    case 0x0E:
        // The microcode's table sends this command into the middle of its volume command. The pole filter's code never
        // runs. The command stores one field of the right ramp.
        SetField(74, static_cast<uint16_t>(command.w1));
        break;
    case 0x0F:
        Put32(kCompactLoopAddress, command.w1 & 0xFFFFFF);
        break;
    default:
        break;
    }
}

void AudioRsp::CompactClear(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>(command.w1 & 0xFFFF);
    uint32_t place = (command.w0 & 0xFFFF) + kCompactMain;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kClear, 0, place, RoundedUp(count, 16));
    }

    do
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            dmem_[(place + i) & kDmemMask] = 0;
        }

        count -= 16;
        place += 16;
    } while (count > 0);
}

void AudioRsp::CompactLoad(const AudioCommand& command, const AudioBus& bus)
{
    const uint32_t length = (command.w0 >> 12) & 0xFFF;
    if (length != 0)
    {
        DmaRead((command.w0 & 0xFFF) + kCompactMain, command.w1 & 0xFFFFFF, length - 1, bus);
        RecordDma(ops_, AudioOpKind::kLoad, (command.w0 & 0xFFF) + kCompactMain, command.w1 & 0xFFFFFF, length);
    }
}

void AudioRsp::CompactSave(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t length = (command.w0 >> 12) & 0xFFF;
    if (length != 0)
    {
        DmaWrite((command.w0 & 0xFFF) + kCompactMain, command.w1 & 0xFFFFFF, length - 1, bus);
        RecordDma(ops_, AudioOpKind::kSave, (command.w0 & 0xFFF) + kCompactMain, command.w1 & 0xFFFFFF, length);
    }
}

void AudioRsp::CompactSetVolume(const AudioCommand& command)
{
    const uint32_t flags = command.w0 >> 16;
    const auto low = static_cast<uint16_t>(command.w0);
    const auto high_word = static_cast<uint16_t>(command.w1 >> 16);
    const auto low_word = static_cast<uint16_t>(command.w1);
    uint32_t first = 64;
    uint32_t second = 66;
    if ((flags & 4) != 0)
    {
        first = (flags & 2) != 0 ? 80 : 70;
        second = (flags & 2) != 0 ? 76 : 72;
    }

    SetField(first, low);
    SetField(second, high_word);
    SetField(second + 2, low_word);
}

void AudioRsp::CompactInterleave()
{
    uint32_t out = kCompactMain;
    uint32_t left = kCompactDryLeft;
    uint32_t right = kCompactDryRight;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kInterleave, left, out, kCompactBytes);
        ops_->ops.back().extra = right;
    }

    Vector left_samples = {};
    Vector right_samples = {};
    for (int32_t count = kCompactBytes; count > 0; count -= 16)
    {
        LoadQuad(left_samples, left);
        LoadQuad(right_samples, right);
        for (uint32_t i = 0; i < 8; i++)
        {
            Put16(out + i * 4, static_cast<uint16_t>(left_samples[i]));
            Put16(out + i * 4 + 2, static_cast<uint16_t>(right_samples[i]));
        }

        left += 16;
        right += 16;
        out += 32;
    }
}

void AudioRsp::CompactMove(const AudioCommand& command)
{
    int32_t count = static_cast<int32_t>(command.w1 & 0xFFFF);
    uint32_t from = (command.w0 & 0xFFFF) + kCompactMain;
    uint32_t to = (command.w1 >> 16) + kCompactMain;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kMove, from, to, RoundedUp(count, 16));
        ops_->ops.back().group = 16;
    }

    std::array<uint8_t, 16> bytes;
    do
    {
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
    } while (count > 0);
}

void AudioRsp::CompactAdpcm(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t in = ((command.w1 >> 12) & 0xF) + kCompactMain;
    const uint32_t out = (command.w1 & 0xFFF) + kCompactMain;
    const auto count = static_cast<int32_t>((command.w1 >> 16) & 0xFFF);
    DecodeAdpcm(in, out, count, command.w1 >> 28, command.w0 & 0xFFFFFF, Get32(kCompactLoopAddress), false, bus);
}

void AudioRsp::CompactResample(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t state = command.w0 & 0xFFFFFF;
    const auto start = static_cast<int32_t>(((command.w1 >> 2) & 0xFFF) + kCompactMain - 8);
    const auto pitch = static_cast<uint16_t>(command.w1 >> 14);
    const int32_t out = static_cast<int32_t>((command.w1 & 3) == 0 ? kCompactMain : kCompactSecond);
    std::array<uint8_t, 8> saved = {};
    uint16_t fraction = 0;
    if ((command.w1 >> 30) == 0)
    {
        DmaRead(layout_.scratch, state, 15, bus);
        for (uint32_t i = 0; i < 8; i++)
        {
            saved[i] = dmem_[(layout_.scratch + i) & kDmemMask];
        }

        fraction = Get16(layout_.scratch + 8);
    }

    for (uint32_t i = 0; i < 8; i++)
    {
        dmem_[(static_cast<uint32_t>(start) + i) & kDmemMask] = saved[i];
    }

    if (ops_ != nullptr)
    {
        AudioResampleOp op;
        op.start = static_cast<uint32_t>(start) & kDmemMask;
        op.out = static_cast<uint32_t>(out) & kDmemMask;
        op.bytes = kCompactBytes;
        op.state = state;
        op.first_fraction = fraction;
        op.pitch = pitch;
        op.init = (command.w1 >> 30) != 0;
        RecordResample(op);
    }

    const ResampleEnd end = ResampleLoop(start, out, kCompactBytes, fraction, pitch, 176);
    Put16(layout_.scratch + 8, end.fraction);
    for (uint32_t i = 0; i < 8; i++)
    {
        dmem_[(layout_.scratch + i) & kDmemMask] = dmem_[(static_cast<uint32_t>(end.place) + i) & kDmemMask];
    }

    DmaWrite(layout_.scratch, state, 15, bus);
}

void AudioRsp::CompactEnvMixer(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t state = command.w1 & 0xFFFFFF;
    const bool init = ((command.w0 >> 16) & 1) != 0;
    EnvRamps ramps;
    if (!init)
    {
        DmaRead(layout_.fields, state, 79, bus);
        LoadEnvRamps(ramps, layout_.fields);
    }
    else
    {
        LoadQuad(ramps.settings, layout_.fields + 64);
    }

    AudioEnvOp env;
    if (ops_ != nullptr)
    {
        constexpr float kFull = 1.0F / 32768.0F;
        RecordPlaces(env, {.in = kCompactMain,
                           .dry_left = kCompactDryLeft,
                           .dry_right = kCompactDryRight,
                           .wet_left = kCompactWetLeft,
                           .wet_right = kCompactWetRight});
        env.bytes = kCompactBytes;
        env.state = state;
        env.init = init;
        env.from_left = static_cast<float>(init ? static_cast<int16_t>(Field(80)) : ramps.left[7]) * kFull;
        env.from_right = static_cast<float>(init ? static_cast<int16_t>(command.w0) : ramps.right[7]) * kFull;
        env.dry_factor_left = static_cast<float>(ramps.settings[6]) * kFull;
        env.dry_factor_right = env.dry_factor_left;
        env.wet_factor_left = static_cast<float>(ramps.settings[7]) * kFull;
        env.wet_factor_right = env.wet_factor_left;
    }

    MixCompactEnvGroups(ramps, init, static_cast<int16_t>(command.w0));
    if (ops_ != nullptr)
    {
        env.to_left = static_cast<float>(ramps.left[7]) / 32768.0F;
        env.to_right = static_cast<float>(ramps.right[7]) / 32768.0F;
        RecordEnv(env);
    }

    StoreEnvRamps(ramps, layout_.fields);
    DmaWrite(layout_.fields, state, 79, bus);
}

void AudioRsp::MixCompactEnvGroups(EnvRamps& ramps, bool init, int16_t right_volume)
{
    const int16_t left_target = ramps.settings[0];
    const int16_t left_rate = ramps.settings[1];
    const auto left_rate_fraction = static_cast<uint16_t>(ramps.settings[2]);
    const int16_t right_target = ramps.settings[3];
    const int16_t right_rate = ramps.settings[4];
    const auto right_rate_fraction = static_cast<uint16_t>(ramps.settings[5]);
    const int16_t dry = ramps.settings[6];
    const int16_t wet = ramps.settings[7];

    uint32_t in = kCompactMain;
    uint32_t dry_left = kCompactDryLeft;
    uint32_t dry_right = kCompactDryRight;
    uint32_t wet_left = kCompactWetLeft;
    uint32_t wet_right = kCompactWetRight;
    Vector samples = {};
    int32_t count = kCompactBytes;
    if (init)
    {
        LoadQuad(samples, in);
        LinearFirstRamp(static_cast<int16_t>(Field(80)), left_rate, left_rate_fraction, ramps.left,
                        ramps.left_fraction);
        LinearLimitLanes(ramps.left, left_target, left_rate);
        LinearFirstRamp(right_volume, right_rate, right_rate_fraction, ramps.right, ramps.right_fraction);
        LinearLimitLanes(ramps.right, right_target, right_rate);
        MixEnvBuffer(dry_left, samples, ramps.left, dry);
        MixEnvBuffer(wet_left, samples, ramps.left, wet);
        MixEnvBuffer(dry_right, samples, ramps.right, dry);
        MixEnvBuffer(wet_right, samples, ramps.right, wet);
        count -= 16;
        in += 16;
        dry_left += 16;
        wet_left += 16;
        dry_right += 16;
        wet_right += 16;
    }

    LinearStepLanes(ramps.left, ramps.left_fraction, left_rate, left_rate_fraction);
    Vector saved_left = {};
    Vector saved_left_fraction = {};
    while (true)
    {
        LinearStepLanes(ramps.right, ramps.right_fraction, right_rate, right_rate_fraction);
        LoadQuad(samples, in);
        LinearLimitLanes(ramps.left, left_target, left_rate);
        LinearLimitLanes(ramps.right, right_target, right_rate);
        saved_left = ramps.left;
        saved_left_fraction = ramps.left_fraction;
        MixEnvBuffer(dry_left, samples, ramps.left, dry);
        MixEnvBuffer(wet_left, samples, ramps.left, wet);
        LinearStepLanes(ramps.left, ramps.left_fraction, left_rate, left_rate_fraction);
        MixEnvBuffer(dry_right, samples, ramps.right, dry);
        MixEnvBuffer(wet_right, samples, ramps.right, wet);
        count -= 16;
        in += 16;
        dry_left += 16;
        wet_left += 16;
        dry_right += 16;
        wet_right += 16;
        if (count <= 0)
        {
            break;
        }
    }

    // The microcode keeps the left ramp as it stood before the last group's step.
    ramps.left = saved_left;
    ramps.left_fraction = saved_left_fraction;
}

void AudioRsp::MixEnvBuffer(uint32_t place, const Vector& samples, const Vector& volume, int16_t amount)
{
    Vector buffer = {};
    LoadQuad(buffer, place);
    for (std::size_t i = 0; i < 8; i++)
    {
        buffer[i] = MixSample(buffer[i], samples[i], MultiplyFraction(volume[i], amount));
    }

    StoreQuad(buffer, place);
}

void AudioRsp::CompactMixer(const AudioCommand& command)
{
    uint32_t out = (command.w1 & 0xFFFF) + kCompactMain;
    uint32_t in = (command.w1 >> 16) + kCompactMain;
    const auto gain = static_cast<int16_t>(command.w0);
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kMix, in, out, kCompactBytes);
        ops_->ops.back().gain = gain;
    }

    Vector out_samples = {};
    Vector in_samples = {};
    LoadQuad(out_samples, out);
    LoadQuad(in_samples, in);
    for (int32_t count = kCompactBytes; count > 0; count -= 16)
    {
        Vector results = {};
        for (std::size_t i = 0; i < 8; i++)
        {
            results[i] = MixSample(out_samples[i], in_samples[i], gain);
        }

        in += 16;
        out += 16;
        LoadQuad(out_samples, out);
        LoadQuad(in_samples, in);
        StoreQuad(results, out - 16);
    }
}

} // namespace ntdlib::n64
