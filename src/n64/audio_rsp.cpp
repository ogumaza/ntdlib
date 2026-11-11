// SPDX-License-Identifier: MIT

// The model of the audio microcodes: DMEM, the DMA and the standard microcode's commands. The other microcodes reuse
// many of those commands. audio_rsp_ead.cpp contains the commands of EAD's microcodes, and audio_rsp_compact.cpp the
// compact microcode's commands.

#include "ntdlib/n64/audio_rsp.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "audio_rsp_common.h"

namespace ntdlib::n64
{

namespace
{

using audio_rsp_common::Clamp16;
using audio_rsp_common::kAuxMix;
using audio_rsp_common::kCount;
using audio_rsp_common::kDmemMask;
using audio_rsp_common::kDryRight;
using audio_rsp_common::kFlagAux;
using audio_rsp_common::kFlagInit;
using audio_rsp_common::kFlagLeft;
using audio_rsp_common::kFlagLoop;
using audio_rsp_common::kFlagSmall;
using audio_rsp_common::kFlagVolume;
using audio_rsp_common::kIn;
using audio_rsp_common::kOut;
using audio_rsp_common::kRamps;
using audio_rsp_common::kRampSteps;
using audio_rsp_common::kVolumeLeft;
using audio_rsp_common::kVolumeRight;
using audio_rsp_common::kWetLeft;
using audio_rsp_common::kWetRight;
using audio_rsp_common::LinearFirstRamp;
using audio_rsp_common::LinearLimitLanes;
using audio_rsp_common::LinearStepLanes;
using audio_rsp_common::MixSample;
using audio_rsp_common::MultiplyFraction;
using audio_rsp_common::RecordDma;
using audio_rsp_common::RoundedUp;
using audio_rsp_common::SaturateLow;
using audio_rsp_common::Vector;
using audio_rsp_common::Wrap48;

// The microcode's segment table, where it has one. Every microcode that has one holds it here.
constexpr uint32_t kSegments = 0x320;

// The resampler's filters: 64 phases of 4 taps, from the table in the microcode's data.
constexpr std::array<int16_t, 256> kResampleTable = {
    0x0C39,  0x66AD,  0x0D46,  -0x0021, 0x0B39,  0x6696,  0x0E5F,  -0x0028, 0x0A44,  0x6669,  0x0F83,  -0x0030, 0x095A,
    0x6626,  0x10B4,  -0x0038, 0x087D,  0x65CD,  0x11F0,  -0x0041, 0x07AB,  0x655E,  0x1338,  -0x004A, 0x06E4,  0x64D9,
    0x148C,  -0x0054, 0x0628,  0x643F,  0x15EB,  -0x005F, 0x0577,  0x638F,  0x1756,  -0x006A, 0x04D1,  0x62CB,  0x18CB,
    -0x0076, 0x0435,  0x61F3,  0x1A4C,  -0x0082, 0x03A4,  0x6106,  0x1BD7,  -0x008F, 0x031C,  0x6007,  0x1D6C,  -0x009C,
    0x029F,  0x5EF5,  0x1F0B,  -0x00AA, 0x022A,  0x5DD0,  0x20B3,  -0x00B8, 0x01BE,  0x5C9A,  0x2264,  -0x00C6, 0x015B,
    0x5B53,  0x241E,  -0x00D4, 0x0101,  0x59FC,  0x25E0,  -0x00E2, 0x00AE,  0x5896,  0x27A9,  -0x00F0, 0x0063,  0x5720,
    0x297A,  -0x00FE, 0x001F,  0x559D,  0x2B50,  -0x010C, -0x001E, 0x540D,  0x2D2C,  -0x0118, -0x0054, 0x5270,  0x2F0D,
    -0x0125, -0x0084, 0x50C7,  0x30F3,  -0x0130, -0x00AD, 0x4F14,  0x32DC,  -0x013A, -0x00D2, 0x4D57,  0x34C8,  -0x0143,
    -0x00F1, 0x4B91,  0x36B6,  -0x014A, -0x010B, 0x49C2,  0x38A5,  -0x0150, -0x0121, 0x47ED,  0x3A95,  -0x0154, -0x0132,
    0x4611,  0x3C85,  -0x0155, -0x0140, 0x4430,  0x3E74,  -0x0154, -0x014A, 0x424A,  0x4060,  -0x0151, -0x0151, 0x4060,
    0x424A,  -0x014A, -0x0154, 0x3E74,  0x4430,  -0x0140, -0x0155, 0x3C85,  0x4611,  -0x0132, -0x0154, 0x3A95,  0x47ED,
    -0x0121, -0x0150, 0x38A5,  0x49C2,  -0x010B, -0x014A, 0x36B6,  0x4B91,  -0x00F1, -0x0143, 0x34C8,  0x4D57,  -0x00D2,
    -0x013A, 0x32DC,  0x4F14,  -0x00AD, -0x0130, 0x30F3,  0x50C7,  -0x0084, -0x0125, 0x2F0D,  0x5270,  -0x0054, -0x0118,
    0x2D2C,  0x540D,  -0x001E, -0x010C, 0x2B50,  0x559D,  0x001F,  -0x00FE, 0x297A,  0x5720,  0x0063,  -0x00F0, 0x27A9,
    0x5896,  0x00AE,  -0x00E2, 0x25E0,  0x59FC,  0x0101,  -0x00D4, 0x241E,  0x5B53,  0x015B,  -0x00C6, 0x2264,  0x5C9A,
    0x01BE,  -0x00B8, 0x20B3,  0x5DD0,  0x022A,  -0x00AA, 0x1F0B,  0x5EF5,  0x029F,  -0x009C, 0x1D6C,  0x6007,  0x031C,
    -0x008F, 0x1BD7,  0x6106,  0x03A4,  -0x0082, 0x1A4C,  0x61F3,  0x0435,  -0x0076, 0x18CB,  0x62CB,  0x04D1,  -0x006A,
    0x1756,  0x638F,  0x0577,  -0x005F, 0x15EB,  0x643F,  0x0628,  -0x0054, 0x148C,  0x64D9,  0x06E4,  -0x004A, 0x1338,
    0x655E,  0x07AB,  -0x0041, 0x11F0,  0x65CD,  0x087D,  -0x0038, 0x10B4,  0x6626,  0x095A,  -0x0030, 0x0F83,  0x6669,
    0x0A44,  -0x0028, 0x0E5F,  0x6696,  0x0B39,  -0x0021, 0x0D46,  0x66AD,  0x0C39};

// One step of a volume ramp's value, the integer and the fraction: the value times a rate of 16.16 bits.
void StepVolume(int16_t& whole, uint16_t& fraction, int16_t rate_whole, uint16_t rate_fraction)
{
    int64_t accumulator = (int64_t{fraction} * rate_fraction) >> 16;
    accumulator = Wrap48(accumulator + int64_t{whole} * rate_fraction);
    accumulator = Wrap48(accumulator + int64_t{fraction} * rate_whole);
    accumulator = Wrap48(accumulator + int64_t{whole} * rate_whole * 65536);
    whole = Clamp16(accumulator >> 16);
    fraction = SaturateLow(accumulator);
}

// Holds a volume at its target. A rate above 1 stops at the target from below, and another rate from above.
int16_t LimitVolume(int16_t whole, int16_t target, int16_t rate_whole)
{
    if (rate_whole > 0)
    {
        return static_cast<uint16_t>(whole) >= static_cast<uint16_t>(target) ? target : whole;
    }

    return whole >= target ? whole : target;
}

// One step of a ramp's value in each lane. The linear microcode adds the rate. The standard one multiplies by it.
void StepLanes(bool linear, Vector& whole, Vector& fraction, int16_t rate, uint16_t rate_fraction)
{
    if (linear)
    {
        LinearStepLanes(whole, fraction, rate, rate_fraction);

        return;
    }

    for (std::size_t i = 0; i < 8; i++)
    {
        auto lane_fraction = static_cast<uint16_t>(fraction[i]);
        StepVolume(whole[i], lane_fraction, rate, rate_fraction);
        fraction[i] = static_cast<int16_t>(lane_fraction);
    }
}

// Keeps a ramp's value in each lane at its target.
void HoldLanes(bool linear, Vector& values, int16_t target, int16_t rate)
{
    if (linear)
    {
        LinearLimitLanes(values, target, rate);

        return;
    }

    for (int16_t& value : values)
    {
        value = LimitVolume(value, target, rate);
    }
}

// The values of a ramp in the 8 lanes of the first group, from `volume`.
void FirstRamp(bool linear, int16_t volume, int16_t rate, uint16_t rate_fraction, Vector& whole, Vector& fraction)
{
    if (linear)
    {
        LinearFirstRamp(volume, rate, rate_fraction, whole, fraction);

        return;
    }

    // The value after this group, and the steps to it across the group's 8 lanes.
    int64_t accumulator = int64_t{volume} * rate_fraction;
    accumulator = Wrap48(accumulator + int64_t{volume} * rate * 65536);
    const int16_t next = Clamp16(accumulator >> 16);
    const uint16_t next_fraction = SaturateLow(accumulator);
    const int16_t difference = Clamp16(int32_t{next} - volume);
    for (std::size_t i = 0; i < 8; i++)
    {
        int64_t step = (int64_t{kRampSteps[i]} * next_fraction) >> 16;
        step = Wrap48(step + int64_t{kRampSteps[i]} * difference);
        step = Wrap48(step + int64_t{volume} * 65536);
        whole[i] = Clamp16(step >> 16);
        fraction[i] = static_cast<int16_t>(SaturateLow(step));
    }
}

// Mixes a group of 8 samples into the dry and wet buffers of one side, at the volume of that side in each lane.
void MixSide(const Vector& samples, const Vector& volume, int16_t dry, int16_t wet, Vector& dry_samples,
             Vector& wet_samples)
{
    for (std::size_t i = 0; i < 8; i++)
    {
        const int16_t dry_gain = MultiplyFraction(volume[i], dry);
        const int16_t wet_gain = MultiplyFraction(volume[i], wet);
        dry_samples[i] = MixSample(dry_samples[i], samples[i], dry_gain);
        wet_samples[i] = MixSample(wet_samples[i], samples[i], wet_gain);
    }
}

// The residuals of an ADPCM frame, in two groups of 8 samples.
using Residuals = std::array<std::array<int32_t, 8>, 2>;

// The residuals of the frame's two groups of 8 samples: each value scaled by the header.
Residuals FrameResiduals(const std::array<uint16_t, 4>& words, int32_t shift, bool small)
{
    Residuals residuals;
    for (std::size_t group = 0; group < 2; group++)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            uint16_t top = 0;
            if (small)
            {
                top = static_cast<uint16_t>((words[group] >> (14 - i * 2)) << 14);
            }
            else
            {
                const uint16_t word = words[group * 2 + i / 4];
                top = static_cast<uint16_t>((word >> (12 - (i % 4) * 4)) << 12);
            }

            auto value = int32_t{static_cast<int16_t>(top)};
            if (shift > 0)
            {
                const uint32_t scale = (uint32_t{0x8000} >> ((static_cast<uint32_t>(shift) - 1) & 31)) & 0xFFFF;
                value = int32_t{static_cast<int16_t>((int64_t{value} * scale) >> 16)};
            }

            residuals[group][i] = value;
        }
    }

    return residuals;
}

// Decodes the two groups of 8 samples of a frame. The rows of the book predict each sample from the last two samples
// before its group and from the earlier residuals in the group. The residual of the sample is added to the prediction.
std::array<Vector, 2> PredictGroups(const Residuals& residuals, const Vector& last, const Vector& row1,
                                    const Vector& row2)
{
    std::array<Vector, 2> groups;
    for (std::size_t group = 0; group < 2; group++)
    {
        const int32_t older = group == 0 ? last[6] : groups[0][6];
        const int32_t newer = group == 0 ? last[7] : groups[0][7];
        for (std::size_t i = 0; i < 8; i++)
        {
            auto sum = static_cast<uint32_t>(row1[i] * older) + static_cast<uint32_t>(row2[i] * newer);
            for (std::size_t j = 0; j < i; j++)
            {
                sum += static_cast<uint32_t>(residuals[group][j] * row2[i - j - 1]);
            }

            sum += static_cast<uint32_t>(residuals[group][i] * 0x800);
            groups[group][i] = Clamp16((int64_t{static_cast<int32_t>(sum)} * 32) >> 16);
        }
    }

    return groups;
}

} // namespace

AudioRsp::AudioRsp(AudioMicrocode microcode, AudioAddressing addressing)
    : microcode_(microcode), addressing_(addressing)
{
    switch (microcode)
    {
    case AudioMicrocode::kStandard:
    case AudioMicrocode::kStandardLinear:
        layout_ = {.buffers = 0x5C0, .book = 0x4C0, .fields = 0x360, .scratch = 0xF90, .loop = 16};
        break;
    case AudioMicrocode::kCompact:
        layout_ = {.buffers = 0, .book = 0x3F0, .fields = 0xFA0, .scratch = 0xFA0, .loop = 0};
        break;
    case AudioMicrocode::kEadFirst:
        layout_ = {.buffers = 0x450, .book = 0x3D0, .fields = 0x330, .scratch = 0xFA0, .loop = 16};
        break;
    case AudioMicrocode::kEadSecond:
    case AudioMicrocode::kEadThird:
        layout_ = {.buffers = 0, .book = 0x330, .fields = 0x2E0, .scratch = 0xFB0, .loop = 8};
        break;
    }
}

const std::array<int16_t, 256>& AudioResampleTable()
{
    return kResampleTable;
}

void AudioRsp::RecordOps(AudioOps* ops)
{
    ops_ = ops;
    resampled_ = false;
}

void AudioRsp::RecordMark(AudioOpKind kind, int track)
{
    if (ops_ != nullptr)
    {
        AudioOp op;
        op.kind = kind;
        op.track = track;
        ops_->ops.push_back(op);
        resampled_ = false;
    }
}

void AudioRsp::RecordOp(AudioOpKind kind, uint32_t in, uint32_t out, uint32_t bytes)
{
    AudioOp op;
    op.kind = kind;
    op.in = in & kDmemMask;
    op.out = out & kDmemMask;
    op.bytes = bytes;
    ops_->ops.push_back(op);
}

void AudioRsp::RecordResample(AudioResampleOp op)
{
    // The place after the last output's group, in the input's samples.
    const uint32_t groups = std::max<uint32_t>((op.bytes + 15) / 16, 1);
    const uint32_t end = (uint32_t{op.first_fraction} + (groups * 16 * uint32_t{op.pitch})) >> 16;
    op.bytes = groups * 16;
    op.inputs = static_cast<uint32_t>(ops_->inputs.size());
    op.input_count = end + 4;
    for (uint32_t i = 0; i < op.input_count; i++)
    {
        ops_->inputs.push_back(static_cast<float>(static_cast<int16_t>(Get16(op.start + i * 2))));
    }

    AudioOp record;
    record.kind = AudioOpKind::kResample;
    record.extra = static_cast<uint32_t>(ops_->resamples.size());
    ops_->resamples.push_back(op);
    ops_->ops.push_back(record);
    resampled_ = true;
}

void AudioRsp::RecordEnv(AudioEnvOp op)
{
    if (!resampled_)
    {
        op.inputs = static_cast<uint32_t>(ops_->inputs.size());
        op.input_count = op.bytes / 2;
        for (uint32_t i = 0; i < op.input_count; i++)
        {
            ops_->inputs.push_back(static_cast<float>(static_cast<int16_t>(Get16(op.in + i * 2))));
        }
    }

    AudioOp record;
    record.kind = AudioOpKind::kEnvMixer;
    record.extra = static_cast<uint32_t>(ops_->envs.size());
    ops_->envs.push_back(op);
    ops_->ops.push_back(record);
    resampled_ = false;
}

void AudioRsp::StartTask()
{
    if (microcode_ == AudioMicrocode::kStandard || microcode_ == AudioMicrocode::kStandardLinear ||
        microcode_ == AudioMicrocode::kEadFirst)
    {
        Put32(kSegments, 0);
    }
}

uint32_t AudioRsp::Field(uint32_t offset) const
{
    return Get16(layout_.fields + offset);
}

void AudioRsp::SetField(uint32_t offset, uint16_t value)
{
    Put16(layout_.fields + offset, value);
}

std::array<uint8_t, AudioRsp::kDmemSize>& AudioRsp::Dmem()
{
    return dmem_;
}

const std::array<uint8_t, AudioRsp::kDmemSize>& AudioRsp::Dmem() const
{
    return dmem_;
}

uint16_t AudioRsp::Get16(uint32_t address) const
{
    return static_cast<uint16_t>((dmem_[address & kDmemMask] << 8) | dmem_[(address + 1) & kDmemMask]);
}

void AudioRsp::Put16(uint32_t address, uint16_t value)
{
    dmem_[address & kDmemMask] = static_cast<uint8_t>(value >> 8);
    dmem_[(address + 1) & kDmemMask] = static_cast<uint8_t>(value);
}

uint32_t AudioRsp::Get32(uint32_t address) const
{
    return (static_cast<uint32_t>(Get16(address)) << 16) | Get16(address + 2);
}

void AudioRsp::Put32(uint32_t address, uint32_t value)
{
    Put16(address, static_cast<uint16_t>(value >> 16));
    Put16(address + 2, static_cast<uint16_t>(value));
}

void AudioRsp::LoadQuad(Vector& vector, uint32_t address) const
{
    // A load stops at the next multiple of 16 bytes.
    const uint32_t count = 16 - (address & 15);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t byte = dmem_[(address + i) & kDmemMask];
        auto value = static_cast<uint16_t>(vector[i / 2]);
        value = (i & 1) == 0 ? static_cast<uint16_t>((value & 0x00FF) | (byte << 8))
                             : static_cast<uint16_t>((value & 0xFF00) | byte);
        vector[i / 2] = static_cast<int16_t>(value);
    }
}

void AudioRsp::StoreQuad(const Vector& vector, uint32_t address)
{
    const uint32_t count = 16 - (address & 15);
    for (uint32_t i = 0; i < count; i++)
    {
        const auto value = static_cast<uint16_t>(vector[i / 2]);
        dmem_[(address + i) & kDmemMask] =
            (i & 1) == 0 ? static_cast<uint8_t>(value >> 8) : static_cast<uint8_t>(value);
    }
}

void AudioRsp::LoadDouble(Vector& vector, int first, uint32_t address) const
{
    for (int i = 0; i < 4; i++)
    {
        vector[static_cast<std::size_t>(first + i)] =
            static_cast<int16_t>(Get16(address + static_cast<uint32_t>(i) * 2));
    }
}

void AudioRsp::StoreDouble(const Vector& vector, int first, uint32_t address)
{
    for (int i = 0; i < 4; i++)
    {
        Put16(address + static_cast<uint32_t>(i) * 2,
              static_cast<uint16_t>(vector[static_cast<std::size_t>(first + i)]));
    }
}

uint32_t AudioRsp::Address(uint32_t word) const
{
    if (addressing_ == AudioAddressing::kFlat)
    {
        return word;
    }

    if (microcode_ == AudioMicrocode::kStandard || microcode_ == AudioMicrocode::kStandardLinear ||
        microcode_ == AudioMicrocode::kEadFirst)
    {
        return (word & 0xFFFFFF) + Get32(kSegments + (word >> 24) * 4);
    }

    return word & 0xFFFFFF;
}

void AudioRsp::DmaRead(uint32_t dmem, uint32_t address, uint32_t length_minus_one, const AudioBus& bus)
{
    const uint32_t length = (length_minus_one | 7) + 1;
    std::array<uint8_t, kDmemSize> bytes;
    const uint32_t size = std::min<uint32_t>(length, kDmemSize);
    bus.Read(address & ~uint32_t{7}, bytes.data(), size);
    const uint32_t start = dmem & ~uint32_t{7};
    for (uint32_t i = 0; i < size; i++)
    {
        dmem_[(start + i) & kDmemMask] = bytes[i];
    }
}

void AudioRsp::DmaWrite(uint32_t dmem, uint32_t address, uint32_t length_minus_one, AudioBus& bus) const
{
    const uint32_t length = (length_minus_one | 7) + 1;
    std::array<uint8_t, kDmemSize> bytes;
    const uint32_t size = std::min<uint32_t>(length, kDmemSize);
    const uint32_t start = dmem & ~uint32_t{7};
    for (uint32_t i = 0; i < size; i++)
    {
        bytes[i] = dmem_[(start + i) & kDmemMask];
    }

    bus.Write(address & ~uint32_t{7}, bytes.data(), size);
}

void AudioRsp::Run(std::span<const AudioCommand> commands, AudioBus& bus)
{
    for (const AudioCommand& command : commands)
    {
        switch (microcode_)
        {
        case AudioMicrocode::kStandard:
        case AudioMicrocode::kStandardLinear:
            RunStandard(command, bus);
            break;
        case AudioMicrocode::kCompact:
            RunCompact(command, bus);
            break;
        case AudioMicrocode::kEadFirst:
            RunEadFirst(command, bus);
            break;
        case AudioMicrocode::kEadSecond:
        case AudioMicrocode::kEadThird:
            RunEadLate(command, bus);
            break;
        }
    }
}

void AudioRsp::RunStandard(const AudioCommand& command, AudioBus& bus)
{
    switch (command.Opcode() & 0x7F)
    {
    case kAudioAdpcm:
        Adpcm(command, bus);
        break;
    case kAudioClearBuffer:
        ClearBuffer(command);
        break;
    case kAudioEnvMixer:
        EnvMixer(command, bus);
        break;
    case kAudioLoadBuffer:
        LoadBuffer(command, bus);
        break;
    case kAudioResample:
        Resample(command, bus);
        break;
    case kAudioSaveBuffer:
        SaveBuffer(command, bus);
        break;
    case kAudioSegment:
        Segment(command);
        break;
    case kAudioSetBuffer:
        SetBuffer(command);
        break;
    case kAudioSetVolume:
        SetVolume(command);
        break;
    case kAudioDmemMove:
        DmemMove(command);
        break;
    case kAudioLoadAdpcm:
        LoadAdpcm(command, bus);
        break;
    case kAudioMixer:
        Mixer(command);
        break;
    case kAudioInterleave:
        Interleave(command);
        break;
    case kAudioPoleFilter:
        PoleFilter(command, bus);
        break;
    case kAudioSetLoop:
        SetLoop(command);
        break;
    default:
        break;
    }
}

void AudioRsp::Segment(const AudioCommand& command)
{
    Put32(kSegments + (command.w1 >> 24) * 4, command.w1 & 0xFFFFFF);
}

void AudioRsp::SetBuffer(const AudioCommand& command)
{
    const auto first = static_cast<uint16_t>(command.w0 + layout_.buffers);
    const auto second = static_cast<uint16_t>((command.w1 >> 16) + layout_.buffers);
    if (((command.w0 >> 16) & kFlagAux) != 0)
    {
        SetField(kWetRight, static_cast<uint16_t>(command.w1 + layout_.buffers));
        SetField(kDryRight, first);
        SetField(kWetLeft, second);
    }
    else
    {
        SetField(kIn, first);
        SetField(kOut, second);
        SetField(kCount, static_cast<uint16_t>(command.w1));
    }
}

void AudioRsp::SetVolume(const AudioCommand& command)
{
    const uint32_t flags = command.w0 >> 16;
    const auto low = static_cast<uint16_t>(command.w0);
    const auto rate_high = static_cast<uint16_t>(command.w1 >> 16);
    const auto rate_low = static_cast<uint16_t>(command.w1);
    if ((flags & kFlagAux) != 0)
    {
        SetField(kAuxMix, low);
        SetField(kAuxMix + 2, rate_low);
    }
    else if ((flags & kFlagVolume) != 0)
    {
        SetField((flags & kFlagLeft) != 0 ? kVolumeLeft : kVolumeRight, low);
    }
    else
    {
        const uint32_t ramp = (flags & kFlagLeft) != 0 ? kRamps : kRamps + 6;
        SetField(ramp, low);
        SetField(ramp + 2, rate_high);
        SetField(ramp + 4, rate_low);
    }
}

void AudioRsp::SetLoop(const AudioCommand& command)
{
    Put32(layout_.fields + layout_.loop, Address(command.w1));
}

void AudioRsp::ClearBuffer(const AudioCommand& command)
{
    int32_t count = static_cast<uint16_t>(command.w1);
    if (count == 0)
    {
        return;
    }

    uint32_t place = (command.w0 & 0xFFFF) + layout_.buffers;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kClear, 0, place, RoundedUp(count, 16));
    }

    while (count > 0)
    {
        for (uint32_t i = 0; i < 16; i++)
        {
            dmem_[(place + i) & kDmemMask] = 0;
        }

        place += 16;
        count -= 16;
    }
}

void AudioRsp::LoadBuffer(const AudioCommand& command, const AudioBus& bus)
{
    const uint32_t count = Field(kCount);
    if (count != 0)
    {
        DmaRead(Field(kIn), Address(command.w1), count - 1, bus);
        RecordDma(ops_, AudioOpKind::kLoad, Field(kIn), Address(command.w1), count);
    }
}

void AudioRsp::SaveBuffer(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t count = Field(kCount);
    if (count != 0)
    {
        DmaWrite(Field(kOut), Address(command.w1), count - 1, bus);
        RecordDma(ops_, AudioOpKind::kSave, Field(kOut), Address(command.w1), count);
    }
}

void AudioRsp::LoadAdpcm(const AudioCommand& command, const AudioBus& bus)
{
    const uint32_t count = command.w0 & 0xFFFF;
    DmaRead(layout_.book, Address(command.w1), count == 0 ? kDmemSize - 1 : count - 1, bus);
}

void AudioRsp::DmemMove(const AudioCommand& command)
{
    int32_t count = static_cast<uint16_t>(command.w1);
    if (count == 0)
    {
        return;
    }

    uint32_t from = (command.w0 & 0xFFFF) + layout_.buffers;
    uint32_t to = (command.w1 >> 16) + layout_.buffers;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kMove, from, to, RoundedUp(count, 16));
        ops_->ops.back().group = 16;
    }

    std::array<uint8_t, 16> bytes;
    while (count > 0)
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
    }
}

void AudioRsp::Interleave(const AudioCommand& command)
{
    int32_t count = Field(kCount);
    if (count == 0)
    {
        return;
    }

    uint32_t out = Field(kOut);
    uint32_t left = (command.w1 >> 16) + layout_.buffers;
    uint32_t right = (command.w1 & 0xFFFF) + layout_.buffers;
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kInterleave, left, out, RoundedUp(count, 16));
        ops_->ops.back().extra = right & kDmemMask;
    }

    Vector left_samples = {};
    Vector right_samples = {};
    while (count > 0)
    {
        LoadQuad(left_samples, left);
        LoadQuad(right_samples, right);
        for (uint32_t i = 0; i < 8; i++)
        {
            Put16(out + i * 4, static_cast<uint16_t>(left_samples[i]));
            Put16(out + i * 4 + 2, static_cast<uint16_t>(right_samples[i]));
        }

        count -= 16;
        left += 16;
        right += 16;
        out += 32;
    }
}

void AudioRsp::Mixer(const AudioCommand& command)
{
    int32_t count = Field(kCount);
    if (count == 0)
    {
        return;
    }

    uint32_t out = (command.w1 & 0xFFFF) + layout_.buffers;
    uint32_t in = (command.w1 >> 16) + layout_.buffers;
    const auto gain = static_cast<int16_t>(command.w0);
    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kMix, in, out, RoundedUp(count, 32));
        ops_->ops.back().gain = gain;
    }

    Vector out_samples = {};
    Vector in_samples = {};
    while (count > 0)
    {
        for (uint32_t half = 0; half < 2; half++)
        {
            LoadQuad(out_samples, out + half * 16);
            LoadQuad(in_samples, in + half * 16);
            for (std::size_t i = 0; i < 8; i++)
            {
                out_samples[i] = MixSample(out_samples[i], in_samples[i], gain);
            }

            StoreQuad(out_samples, out + half * 16);
        }

        count -= 32;
        in += 32;
        out += 32;
    }
}

void AudioRsp::Adpcm(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t flags = command.w0 >> 16;
    const bool small = microcode_ != AudioMicrocode::kStandard && microcode_ != AudioMicrocode::kStandardLinear &&
                       microcode_ != AudioMicrocode::kEadFirst && (flags & kFlagSmall) != 0;
    DecodeAdpcm(Field(kIn), Field(kOut), static_cast<int32_t>(Field(kCount)), flags, Address(command.w1),
                Get32(layout_.fields + layout_.loop), small, bus);
}

void AudioRsp::DecodeAdpcm(uint32_t in, uint32_t out, int32_t count, uint32_t flags, uint32_t state,
                           uint32_t loop_address, bool small, AudioBus& bus)
{
    const Vector zero = {};
    StoreQuad(zero, out);
    StoreQuad(zero, out + 16);
    if ((flags & kFlagInit) == 0)
    {
        DmaRead(out, (flags & kFlagLoop) != 0 ? loop_address : state, 31, bus);
    }

    Vector last = {};
    LoadQuad(last, out + 16);
    out += 32;

    const int32_t shift_base = small ? 14 : 12;
    const uint32_t frame_size = small ? 5 : 9;
    const auto read_frame =
        [this, shift_base](uint32_t place, int32_t& shift, std::array<uint16_t, 4>& words, Vector& row1, Vector& row2)
    {
        const uint8_t header = dmem_[place & kDmemMask];
        shift = shift_base - (header >> 4);
        for (uint32_t i = 0; i < 4; i++)
        {
            words[i] = Get16(place + 1 + i * 2);
        }

        const uint32_t row = layout_.book + (header & 0xFU) * 32;
        for (uint32_t i = 0; i < 8; i++)
        {
            row1[i] = static_cast<int16_t>(Get16(row + i * 2));
            row2[i] = static_cast<int16_t>(Get16(row + 16 + i * 2));
        }
    };

    int32_t shift = 0;
    std::array<uint16_t, 4> words = {};
    Vector row1 = {};
    Vector row2 = {};
    read_frame(in, shift, words, row1, row2);

    while (count > 0)
    {
        in += frame_size;
        const Residuals residuals = FrameResiduals(words, shift, small);
        const Vector current_row1 = row1;
        const Vector current_row2 = row2;
        read_frame(in, shift, words, row1, row2);

        const std::array<Vector, 2> groups = PredictGroups(residuals, last, current_row1, current_row2);
        last = groups[1];
        count -= 32;
        StoreDouble(groups[0], 0, out);
        StoreDouble(groups[0], 4, out + 8);
        StoreDouble(groups[1], 0, out + 16);
        StoreDouble(groups[1], 4, out + 24);
        out += 32;
    }

    DmaWrite(out - 32, state, 31, bus);
}

void AudioRsp::PoleFilter(const AudioCommand& command, AudioBus& bus)
{
    uint32_t in = Field(kIn);
    uint32_t out = Field(kOut);
    int32_t count = Field(kCount);
    if (count == 0)
    {
        return;
    }

    const auto gain = static_cast<int16_t>(command.w0);
    const auto scale = static_cast<uint16_t>(static_cast<uint32_t>(command.w0 & 0xFFFF) << 2);
    const uint32_t state = Address(command.w1);
    Put32(layout_.scratch, 0);
    if (((command.w0 >> 16) & kFlagInit) == 0)
    {
        DmaRead(layout_.scratch, state, 7, bus);
    }

    Vector row1 = {};
    Vector row2 = {};
    LoadQuad(row1, layout_.book);
    LoadQuad(row2, layout_.book + 16);

    // The feed-forward row is the second row scaled. It stays in DMEM in that form.
    Vector scaled = {};
    for (std::size_t i = 0; i < 8; i++)
    {
        scaled[i] = static_cast<int16_t>((int64_t{row2[i]} * scale) >> 16);
    }

    if (ops_ != nullptr)
    {
        RecordOp(AudioOpKind::kPole, in, out, RoundedUp(count, 16));
        AudioOp& op = ops_->ops.back();
        op.init = ((command.w0 >> 16) & kFlagInit) != 0;
        op.address = state;
        op.gain = gain;
        op.extra = static_cast<uint32_t>(ops_->tables.size());
        ops_->tables.push_back(static_cast<int16_t>(scale));
        ops_->tables.insert(ops_->tables.end(), row1.begin(), row1.end());
        ops_->tables.insert(ops_->tables.end(), row2.begin(), row2.end());
    }

    StoreQuad(scaled, layout_.book + 16);

    Vector last = {};
    LoadDouble(last, 4, layout_.scratch);
    Vector samples = {};
    LoadDouble(samples, 0, in);
    LoadDouble(samples, 4, in + 8);
    while (count > 0)
    {
        in += 16;
        count -= 16;
        Vector results = {};
        for (std::size_t i = 0; i < 8; i++)
        {
            auto sum = static_cast<uint32_t>(row1[i] * last[6]) + static_cast<uint32_t>(row2[i] * last[7]);
            for (std::size_t j = 0; j < i; j++)
            {
                sum += static_cast<uint32_t>(samples[j] * scaled[i - j - 1]);
            }

            sum += static_cast<uint32_t>(samples[i] * gain);
            results[i] = Clamp16((int64_t{static_cast<int32_t>(sum)} * 4) >> 16);
        }

        LoadDouble(samples, 0, in);
        LoadDouble(samples, 4, in + 8);
        StoreDouble(results, 0, out);
        StoreDouble(results, 4, out + 8);
        last = results;
        out += 16;
    }

    DmaWrite(out - 8, state, 7, bus);
}

AudioRsp::ResampleEnd AudioRsp::ResampleLoop(int32_t start, int32_t out, int32_t count, uint16_t first_fraction,
                                             uint16_t pitch, uint32_t table)
{
    // The place of each output sample, in the input's samples: a 16.16 value. Each group of 8 goes on from the group
    // before it by its last sample's place.
    std::array<int16_t, 8> whole = {};
    std::array<uint16_t, 8> fraction = {};
    for (int i = 0; i < 8; i++)
    {
        const int64_t accumulator = int64_t{first_fraction} + int64_t{2 * i} * pitch;
        whole[static_cast<std::size_t>(i)] = Clamp16(accumulator >> 16);
        fraction[static_cast<std::size_t>(i)] = SaturateLow(accumulator);
    }

    const auto sample_place = [start](int16_t place)
    {
        const int64_t accumulator = int64_t{start} + int64_t{static_cast<uint16_t>(place)} * 2;

        return static_cast<int32_t>(static_cast<int16_t>(SaturateLow(accumulator)));
    };

    while (true)
    {
        std::array<std::array<int16_t, 4>, 8> samples;
        for (std::size_t lane = 0; lane < 8; lane++)
        {
            const int32_t place = sample_place(whole[lane]);
            for (uint32_t tap = 0; tap < 4; tap++)
            {
                samples[lane][tap] = static_cast<int16_t>(Get16(static_cast<uint32_t>(place) + tap * 2));
            }
        }

        const std::array<uint16_t, 8> current_fraction = fraction;
        const int16_t last_whole = whole[7];
        const uint16_t last_fraction = fraction[7];
        for (int i = 0; i < 8; i++)
        {
            const int64_t accumulator =
                int64_t{last_fraction} + int64_t{last_whole} * 65536 + int64_t{2 * (i + 1)} * pitch;
            whole[static_cast<std::size_t>(i)] = Clamp16(accumulator >> 16);
            fraction[static_cast<std::size_t>(i)] = SaturateLow(accumulator);
        }

        Vector results = {};
        for (std::size_t lane = 0; lane < 8; lane++)
        {
            const std::size_t phase = current_fraction[lane] >> 10;
            const int16_t* taps = &kResampleTable[phase * 4];
            const int16_t a = MultiplyFraction(samples[lane][0], taps[0]);
            const int16_t b = MultiplyFraction(samples[lane][1], taps[1]);
            const int16_t c = MultiplyFraction(samples[lane][2], taps[2]);
            const int16_t d = MultiplyFraction(samples[lane][3], taps[3]);
            results[lane] = Clamp16(int32_t{Clamp16(int32_t{a} + b)} + Clamp16(int32_t{c} + d));
        }

        count -= 16;
        StoreQuad(results, static_cast<uint32_t>(out));
        if (count <= 0)
        {
            break;
        }

        out += 16;
    }

    // The microcode keeps the next group's places in DMEM as it reads them. A filter that follows may read those bytes.
    const uint32_t places = layout_.scratch + (microcode_ == AudioMicrocode::kCompact ? 16U : 32U);
    for (uint32_t lane = 0; lane < 8; lane++)
    {
        Put16(places + lane * 2, static_cast<uint16_t>(sample_place(whole[lane])));
        Put16(places + 16 + lane * 2, static_cast<uint16_t>((fraction[lane] >> 10) * 8 + table));
    }

    return {sample_place(whole[0]), fraction[0]};
}

void AudioRsp::Resample(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t flags = command.w0 >> 16;
    const auto pitch = static_cast<uint16_t>(command.w0);
    int32_t in = static_cast<int16_t>(Field(kIn));
    const int32_t out = static_cast<int16_t>(Field(kOut));
    const int32_t count = static_cast<int16_t>(Field(kCount));
    const uint32_t state = Address(command.w1);
    const int32_t first_in = in;
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

    if ((flags & kFlagLoop) != 0)
    {
        const auto back = static_cast<int16_t>(Get16(layout_.scratch + 10));
        for (uint32_t i = 0; i < 16; i++)
        {
            dmem_[(static_cast<uint32_t>(in) - 16 + i) & kDmemMask] = dmem_[(layout_.scratch + 16 + i) & kDmemMask];
        }

        in -= back;
    }

    in -= 8;
    const uint16_t first_fraction = Get16(layout_.scratch + 8);
    for (uint32_t i = 0; i < 8; i++)
    {
        dmem_[(static_cast<uint32_t>(in) + i) & kDmemMask] = dmem_[(layout_.scratch + i) & kDmemMask];
    }

    if (ops_ != nullptr)
    {
        AudioResampleOp op;
        op.start = static_cast<uint32_t>(in) & kDmemMask;
        op.out = static_cast<uint32_t>(out) & kDmemMask;
        op.bytes = static_cast<uint32_t>(std::max(count, 0));
        op.state = state;
        op.first_fraction = first_fraction;
        op.pitch = pitch;
        op.init = (flags & kFlagInit) != 0;
        RecordResample(op);
    }

    const ResampleEnd end =
        ResampleLoop(in, out, count, first_fraction, pitch, microcode_ == AudioMicrocode::kEadFirst ? 256U : 192U);

    SaveResampleState(end, first_in, state, bus);
}

// The state: the next 4 samples, the next fraction, and the input's 16 bytes around the place after them.
void AudioRsp::SaveResampleState(const ResampleEnd& end, int32_t first_in, uint32_t state, AudioBus& bus)
{
    Put16(layout_.scratch + 8, end.fraction);
    int32_t place = end.place;
    for (uint32_t i = 0; i < 8; i++)
    {
        dmem_[(layout_.scratch + i) & kDmemMask] = dmem_[(static_cast<uint32_t>(place) + i) & kDmemMask];
    }

    place += 8;
    int32_t misaligned = (place - first_in) & 0xF;
    place -= misaligned;
    if (misaligned != 0)
    {
        misaligned = 16 - misaligned;
    }

    Put16(layout_.scratch + 10, static_cast<uint16_t>(misaligned));
    std::array<uint8_t, 16> bytes;
    for (uint32_t i = 0; i < 16; i++)
    {
        bytes[i] = dmem_[(static_cast<uint32_t>(place) + i) & kDmemMask];
    }

    for (uint32_t i = 0; i < 16; i++)
    {
        dmem_[(layout_.scratch + 16 + i) & kDmemMask] = bytes[i];
    }

    DmaWrite(layout_.scratch, state, 31, bus);
}

void AudioRsp::LoadEnvRamps(EnvRamps& ramps, uint32_t place) const
{
    LoadQuad(ramps.left, place);
    LoadQuad(ramps.left_fraction, place + 16);
    LoadQuad(ramps.right, place + 32);
    LoadQuad(ramps.right_fraction, place + 48);
    LoadQuad(ramps.settings, place + 64);
}

void AudioRsp::StoreEnvRamps(const EnvRamps& ramps, uint32_t place)
{
    StoreQuad(ramps.left, place);
    StoreQuad(ramps.left_fraction, place + 16);
    StoreQuad(ramps.right, place + 32);
    StoreQuad(ramps.right_fraction, place + 48);
    StoreQuad(ramps.settings, place + 64);
}

void AudioRsp::RecordPlaces(AudioEnvOp& env, const EnvPlaces& places)
{
    env.in = places.in & kDmemMask;
    env.dry_left = places.dry_left & kDmemMask;
    env.dry_right = places.dry_right & kDmemMask;
    env.wet_left = places.wet_left & kDmemMask;
    env.wet_right = places.wet_right & kDmemMask;
}

void AudioRsp::EnvMixer(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t flags = command.w0 >> 16;
    const uint32_t state = Address(command.w1);
    const bool init = (flags & kFlagInit) != 0;
    const bool aux = (flags & kFlagAux) != 0;

    EnvRamps ramps;
    LoadQuad(ramps.settings, layout_.fields + kRamps);
    if (!init)
    {
        DmaRead(layout_.scratch, state, 79, bus);
        LoadEnvRamps(ramps, layout_.scratch);
    }

    const EnvPlaces places = {.in = Field(kIn),
                              .dry_left = Field(kOut),
                              .dry_right = Field(kDryRight),
                              .wet_left = Field(kWetLeft),
                              .wet_right = Field(kWetRight)};
    const int32_t count = static_cast<int16_t>(Field(kCount));
    AudioEnvOp env;
    if (ops_ != nullptr)
    {
        constexpr float kFull = 1.0F / 32768.0F;
        RecordPlaces(env, places);
        env.bytes = RoundedUp(count, 16);
        env.state = state;
        env.init = init;
        env.has_wet = aux;
        env.from_left = static_cast<float>(init ? static_cast<int16_t>(Field(kVolumeLeft)) : ramps.left[7]) * kFull;
        env.from_right = static_cast<float>(init ? static_cast<int16_t>(Field(kVolumeRight)) : ramps.right[7]) * kFull;
        env.dry_factor_left = static_cast<float>(ramps.settings[6]) * kFull;
        env.dry_factor_right = env.dry_factor_left;
        env.wet_factor_left = static_cast<float>(ramps.settings[7]) * kFull;
        env.wet_factor_right = env.wet_factor_left;
    }

    MixEnvGroups(ramps, places, count, init, aux);
    if (ops_ != nullptr)
    {
        env.to_left = static_cast<float>(ramps.left[7]) / 32768.0F;
        env.to_right = static_cast<float>(ramps.right[7]) / 32768.0F;
        RecordEnv(env);
    }

    StoreEnvRamps(ramps, layout_.scratch);
    DmaWrite(layout_.scratch, state, 79, bus);
}

void AudioRsp::LoadEnvSide(bool aux, Vector& dry_samples, uint32_t dry_place, Vector& wet_samples,
                           uint32_t wet_place) const
{
    LoadQuad(dry_samples, dry_place);
    if (aux)
    {
        LoadQuad(wet_samples, wet_place);
    }
}

void AudioRsp::StoreEnvSide(bool aux, const Vector& dry_samples, uint32_t dry_place, const Vector& wet_samples,
                            uint32_t wet_place)
{
    StoreQuad(dry_samples, dry_place);
    if (aux)
    {
        StoreQuad(wet_samples, wet_place);
    }
}

void AudioRsp::MixEnvGroups(EnvRamps& ramps, EnvPlaces places, int32_t count, bool init, bool aux)
{
    const int16_t left_target = ramps.settings[0];
    const int16_t left_rate = ramps.settings[1];
    const auto left_rate_fraction = static_cast<uint16_t>(ramps.settings[2]);
    const int16_t right_target = ramps.settings[3];
    const int16_t right_rate = ramps.settings[4];
    const auto right_rate_fraction = static_cast<uint16_t>(ramps.settings[5]);
    const int16_t dry = ramps.settings[6];
    const int16_t wet = ramps.settings[7];

    Vector samples = {};
    Vector dry_left_samples = {};
    Vector dry_right_samples = {};
    Vector wet_left_samples = {};
    Vector wet_right_samples = {};
    const bool linear = microcode_ == AudioMicrocode::kStandardLinear;
    const uint32_t wet_step = aux ? 16 : 0;
    if (init)
    {
        LoadQuad(samples, places.in);
        LoadEnvSide(aux, dry_left_samples, places.dry_left, wet_left_samples, places.wet_left);
        FirstRamp(linear, static_cast<int16_t>(Field(kVolumeLeft)), left_rate, left_rate_fraction, ramps.left,
                  ramps.left_fraction);
        HoldLanes(linear, ramps.left, left_target, left_rate);
        MixSide(samples, ramps.left, dry, wet, dry_left_samples, wet_left_samples);
        StoreEnvSide(aux, dry_left_samples, places.dry_left, wet_left_samples, places.wet_left);

        LoadEnvSide(aux, dry_right_samples, places.dry_right, wet_right_samples, places.wet_right);
        FirstRamp(linear, static_cast<int16_t>(Field(kVolumeRight)), right_rate, right_rate_fraction, ramps.right,
                  ramps.right_fraction);
        HoldLanes(linear, ramps.right, right_target, right_rate);
        MixSide(samples, ramps.right, dry, wet, dry_right_samples, wet_right_samples);
        StoreEnvSide(aux, dry_right_samples, places.dry_right, wet_right_samples, places.wet_right);

        count -= 16;
        places.in += 16;
        places.dry_left += 16;
        places.dry_right += 16;
        places.wet_left += wet_step;
        places.wet_right += wet_step;
    }

    while (true)
    {
        StepLanes(linear, ramps.left, ramps.left_fraction, left_rate, left_rate_fraction);
        HoldLanes(linear, ramps.left, left_target, left_rate);
        LoadQuad(samples, places.in);
        StepLanes(linear, ramps.right, ramps.right_fraction, right_rate, right_rate_fraction);

        LoadEnvSide(aux, dry_left_samples, places.dry_left, wet_left_samples, places.wet_left);
        LoadEnvSide(aux, dry_right_samples, places.dry_right, wet_right_samples, places.wet_right);
        MixSide(samples, ramps.left, dry, wet, dry_left_samples, wet_left_samples);
        StoreEnvSide(aux, dry_left_samples, places.dry_left, wet_left_samples, places.wet_left);
        HoldLanes(linear, ramps.right, right_target, right_rate);
        MixSide(samples, ramps.right, dry, wet, dry_right_samples, wet_right_samples);
        StoreEnvSide(aux, dry_right_samples, places.dry_right, wet_right_samples, places.wet_right);

        count -= 16;
        places.in += 16;
        places.dry_left += 16;
        places.dry_right += 16;
        places.wet_left += wet_step;
        places.wet_right += wet_step;
        if (count <= 0)
        {
            break;
        }
    }
}

} // namespace ntdlib::n64
