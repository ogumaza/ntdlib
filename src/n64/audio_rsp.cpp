// SPDX-License-Identifier: MIT

#include "ntdlib/n64/audio_rsp.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ntdlib::n64
{

namespace
{

constexpr uint32_t kDmemMask = 0xFFF;

// The microcode's segment table, where it has one. Every microcode that has one holds it here.
constexpr uint32_t kSegments = 0x320;

// The offsets of each microcode's fields in DMEM from the start of those fields.
constexpr uint32_t kIn = 0;
constexpr uint32_t kOut = 2;
constexpr uint32_t kCount = 4;
constexpr uint32_t kVolumeLeft = 6;
constexpr uint32_t kVolumeRight = 8;
constexpr uint32_t kDryRight = 10;
constexpr uint32_t kWetLeft = 12;
constexpr uint32_t kWetRight = 14;
constexpr uint32_t kRamps = 16; // shares its first word with the loop's address
constexpr uint32_t kAuxMix = 28;

// The compact microcode's fixed buffers, counted from its base, and its length of a buffer in bytes.
constexpr uint32_t kCompactDryLeft = 2512;
constexpr uint32_t kCompactDryRight = 2880;
constexpr uint32_t kCompactWetLeft = 3248;
constexpr uint32_t kCompactWetRight = 3616;
constexpr uint32_t kCompactSecond = 1632;
constexpr uint32_t kCompactMain = 1264;
constexpr int32_t kCompactBytes = 368;
constexpr uint32_t kCompactLoopAddress = 14;

constexpr uint8_t kFlagInit = 0x01;
constexpr uint8_t kFlagLoop = 0x02;
constexpr uint8_t kFlagLeft = 0x02;
constexpr uint8_t kFlagVolume = 0x04;
constexpr uint8_t kFlagAux = 0x08;
constexpr uint8_t kFlagSmall = 0x04;

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

// A ramp's share of the change from one group of 8 samples to the next, at each of the group's samples.
constexpr std::array<uint16_t, 8> kRampSteps = {0x2000, 0x4000, 0x6000, 0x8000, 0xA000, 0xC000, 0xE000, 0xFFFF};

constexpr int16_t Clamp16(int64_t value)
{
    return static_cast<int16_t>(std::clamp<int64_t>(value, -32768, 32767));
}

// The accumulator's 48 bits, sign-extended.
constexpr int64_t Wrap48(int64_t value)
{
    return static_cast<int64_t>(static_cast<uint64_t>(value) << 16) >> 16;
}

// The low 16 bits of the accumulator, when its upper bits fit in 16 bits. Otherwise 0 or 0xFFFF.
constexpr uint16_t SaturateLow(int64_t accumulator)
{
    const int64_t upper = accumulator >> 16;
    if (upper < -32768)
    {
        return 0;
    }

    if (upper > 32767)
    {
        return 0xFFFF;
    }

    return static_cast<uint16_t>(accumulator);
}

// A multiply of two fractions, rounded.
constexpr int16_t MultiplyFraction(int16_t a, int16_t b)
{
    return Clamp16((int64_t{a} * b * 2 + 0x8000) >> 16);
}

// A sample times an unsigned 16-bit gain: the product's upper half.
constexpr int16_t MultiplyHigh(int16_t sample, uint16_t gain)
{
    return static_cast<int16_t>((int64_t{sample} * gain) >> 16);
}

// A sample with its gain, added to a buffer's sample that keeps nearly all its level.
constexpr int16_t MixSample(int16_t out, int16_t in, int16_t gain)
{
    return Clamp16((int64_t{out} * 0x7FFF * 2 + 0x8000 + int64_t{in} * gain * 2) >> 16);
}

// The bytes that a command runs on. The command runs on `group` bytes at a time and always runs once.
constexpr uint32_t RoundedUp(int32_t bytes, uint32_t group)
{
    return bytes <= 0 ? group : ((static_cast<uint32_t>(bytes) + group - 1) / group) * group;
}

// A DMA's record. A DMA runs on 8-byte multiples of both addresses.
void RecordDma(AudioOps* ops, AudioOpKind kind, uint32_t dmem, uint32_t address, uint32_t bytes)
{
    if (ops != nullptr)
    {
        AudioOp op;
        op.kind = kind;
        op.in = dmem & kDmemMask & ~uint32_t{7};
        op.address = address & ~uint32_t{7};
        op.bytes = std::min<uint32_t>(((bytes - 1) | 7) + 1, AudioRsp::kDmemSize);
        ops->ops.push_back(op);
    }
}

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

        // The residuals of the frame's two groups of 8 samples: each value scaled by the header.
        std::array<std::array<int32_t, 8>, 2> residuals;
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

        const Vector current_row1 = row1;
        const Vector current_row2 = row2;
        read_frame(in, shift, words, row1, row2);

        std::array<Vector, 2> groups;
        for (std::size_t group = 0; group < 2; group++)
        {
            const int32_t older = group == 0 ? last[6] : groups[0][6];
            const int32_t newer = group == 0 ? last[7] : groups[0][7];
            for (std::size_t i = 0; i < 8; i++)
            {
                auto sum =
                    static_cast<uint32_t>(current_row1[i] * older) + static_cast<uint32_t>(current_row2[i] * newer);
                for (std::size_t j = 0; j < i; j++)
                {
                    sum += static_cast<uint32_t>(residuals[group][j] * current_row2[i - j - 1]);
                }

                sum += static_cast<uint32_t>(residuals[group][i] * 0x800);
                groups[group][i] = Clamp16((int64_t{static_cast<int32_t>(sum)} * 32) >> 16);
            }
        }

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

    // The state: the next 4 samples, the next fraction, and the input's 16 bytes around the place after them.
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

void AudioRsp::EnvMixer(const AudioCommand& command, AudioBus& bus)
{
    const uint32_t flags = command.w0 >> 16;
    const uint32_t state = Address(command.w1);
    const bool init = (flags & kFlagInit) != 0;
    const bool aux = (flags & kFlagAux) != 0;

    // The ramps' values in each lane: the whole part and the fraction, on the left and the right.
    Vector left = {};
    Vector left_fraction = {};
    Vector right = {};
    Vector right_fraction = {};

    // The ramps' settings: the left's target and rate, the right's target and rate, then the two gains.
    Vector settings = {};
    LoadQuad(settings, layout_.fields + kRamps);
    if (!init)
    {
        DmaRead(layout_.scratch, state, 79, bus);
        LoadQuad(left, layout_.scratch);
        LoadQuad(left_fraction, layout_.scratch + 16);
        LoadQuad(right, layout_.scratch + 32);
        LoadQuad(right_fraction, layout_.scratch + 48);
        LoadQuad(settings, layout_.scratch + 64);
    }

    const int16_t left_target = settings[0];
    const int16_t left_rate = settings[1];
    const auto left_rate_fraction = static_cast<uint16_t>(settings[2]);
    const int16_t right_target = settings[3];
    const int16_t right_rate = settings[4];
    const auto right_rate_fraction = static_cast<uint16_t>(settings[5]);
    const int16_t dry = settings[6];
    const int16_t wet = settings[7];

    uint32_t in = Field(kIn);
    uint32_t dry_left = Field(kOut);
    uint32_t dry_right = Field(kDryRight);
    uint32_t wet_left = Field(kWetLeft);
    uint32_t wet_right = Field(kWetRight);
    int32_t count = static_cast<int16_t>(Field(kCount));

    Vector samples = {};
    Vector dry_left_samples = {};
    Vector dry_right_samples = {};
    Vector wet_left_samples = {};
    Vector wet_right_samples = {};
    AudioEnvOp env;
    if (ops_ != nullptr)
    {
        constexpr float kFull = 1.0F / 32768.0F;
        env.in = in & kDmemMask;
        env.dry_left = dry_left & kDmemMask;
        env.dry_right = dry_right & kDmemMask;
        env.wet_left = wet_left & kDmemMask;
        env.wet_right = wet_right & kDmemMask;
        env.bytes = RoundedUp(count, 16);
        env.state = state;
        env.init = init;
        env.has_wet = aux;
        env.from_left = static_cast<float>(init ? static_cast<int16_t>(Field(kVolumeLeft)) : left[7]) * kFull;
        env.from_right = static_cast<float>(init ? static_cast<int16_t>(Field(kVolumeRight)) : right[7]) * kFull;
        env.dry_factor_left = static_cast<float>(dry) * kFull;
        env.dry_factor_right = env.dry_factor_left;
        env.wet_factor_left = static_cast<float>(wet) * kFull;
        env.wet_factor_right = env.wet_factor_left;
    }

    const auto load_side = [this, aux](Vector& dry_samples, uint32_t dry_place, Vector& wet_samples, uint32_t wet_place)
    {
        LoadQuad(dry_samples, dry_place);
        if (aux)
        {
            LoadQuad(wet_samples, wet_place);
        }
    };

    const auto mix_side = [&samples, dry, wet](const Vector& volume, Vector& dry_samples, Vector& wet_samples)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            const int16_t dry_gain = MultiplyFraction(volume[i], dry);
            const int16_t wet_gain = MultiplyFraction(volume[i], wet);
            dry_samples[i] = MixSample(dry_samples[i], samples[i], dry_gain);
            wet_samples[i] = MixSample(wet_samples[i], samples[i], wet_gain);
        }
    };

    const auto store_side =
        [this, aux](const Vector& dry_samples, uint32_t dry_place, const Vector& wet_samples, uint32_t wet_place)
    {
        StoreQuad(dry_samples, dry_place);
        if (aux)
        {
            StoreQuad(wet_samples, wet_place);
        }
    };

    const bool linear = microcode_ == AudioMicrocode::kStandardLinear;
    const auto hold = [linear](int16_t whole, int16_t target, int16_t rate) -> int16_t
    {
        if (!linear)
        {
            return LimitVolume(whole, target, rate);
        }

        return rate >= 0 ? std::min(whole, target) : std::max(whole, target);
    };

    const auto step_volume = [linear](int16_t& whole, uint16_t& fraction, int16_t rate, uint16_t rate_fraction)
    {
        if (!linear)
        {
            StepVolume(whole, fraction, rate, rate_fraction);

            return;
        }

        const uint32_t sum = uint32_t{fraction} + rate_fraction;
        fraction = static_cast<uint16_t>(sum);
        whole = Clamp16(int32_t{whole} + rate + static_cast<int32_t>(sum >> 16));
    };

    const auto first_ramp =
        [linear](int16_t volume, int16_t rate, uint16_t rate_fraction, Vector& whole, Vector& fraction)
    {
        if (linear)
        {
            for (std::size_t i = 0; i < 8; i++)
            {
                int64_t accumulator = (int64_t{kRampSteps[i]} * rate_fraction) >> 16;
                accumulator = Wrap48(accumulator + int64_t{kRampSteps[i]} * rate);
                accumulator = Wrap48(accumulator + int64_t{volume} * 65536);
                whole[i] = Clamp16(accumulator >> 16);
                fraction[i] = static_cast<int16_t>(SaturateLow(accumulator));
            }

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
    };

    const uint32_t wet_step = aux ? 16 : 0;
    if (init)
    {
        LoadQuad(samples, in);
        load_side(dry_left_samples, dry_left, wet_left_samples, wet_left);
        first_ramp(static_cast<int16_t>(Field(kVolumeLeft)), left_rate, left_rate_fraction, left, left_fraction);
        for (std::size_t i = 0; i < 8; i++)
        {
            left[i] = hold(left[i], left_target, left_rate);
        }

        mix_side(left, dry_left_samples, wet_left_samples);
        store_side(dry_left_samples, dry_left, wet_left_samples, wet_left);

        load_side(dry_right_samples, dry_right, wet_right_samples, wet_right);
        first_ramp(static_cast<int16_t>(Field(kVolumeRight)), right_rate, right_rate_fraction, right, right_fraction);
        for (std::size_t i = 0; i < 8; i++)
        {
            right[i] = hold(right[i], right_target, right_rate);
        }

        mix_side(right, dry_right_samples, wet_right_samples);
        store_side(dry_right_samples, dry_right, wet_right_samples, wet_right);

        count -= 16;
        in += 16;
        dry_left += 16;
        dry_right += 16;
        wet_left += wet_step;
        wet_right += wet_step;
    }

    while (true)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            auto fraction = static_cast<uint16_t>(left_fraction[i]);
            step_volume(left[i], fraction, left_rate, left_rate_fraction);
            left_fraction[i] = static_cast<int16_t>(fraction);
            left[i] = hold(left[i], left_target, left_rate);
        }

        LoadQuad(samples, in);
        for (std::size_t i = 0; i < 8; i++)
        {
            auto fraction = static_cast<uint16_t>(right_fraction[i]);
            step_volume(right[i], fraction, right_rate, right_rate_fraction);
            right_fraction[i] = static_cast<int16_t>(fraction);
        }

        load_side(dry_left_samples, dry_left, wet_left_samples, wet_left);
        load_side(dry_right_samples, dry_right, wet_right_samples, wet_right);
        mix_side(left, dry_left_samples, wet_left_samples);
        store_side(dry_left_samples, dry_left, wet_left_samples, wet_left);
        for (std::size_t i = 0; i < 8; i++)
        {
            right[i] = hold(right[i], right_target, right_rate);
        }

        mix_side(right, dry_right_samples, wet_right_samples);
        store_side(dry_right_samples, dry_right, wet_right_samples, wet_right);

        count -= 16;
        in += 16;
        dry_left += 16;
        dry_right += 16;
        wet_left += wet_step;
        wet_right += wet_step;
        if (count <= 0)
        {
            break;
        }
    }

    if (ops_ != nullptr)
    {
        env.to_left = static_cast<float>(left[7]) / 32768.0F;
        env.to_right = static_cast<float>(right[7]) / 32768.0F;
        RecordEnv(env);
    }

    StoreQuad(left, layout_.scratch);
    StoreQuad(left_fraction, layout_.scratch + 16);
    StoreQuad(right, layout_.scratch + 32);
    StoreQuad(right_fraction, layout_.scratch + 48);
    StoreQuad(settings, layout_.scratch + 64);
    DmaWrite(layout_.scratch, state, 79, bus);
}

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
    uint32_t in = (command.w0 >> 12) & 0xFF0;
    uint32_t dry_left = (command.w1 >> 20) & 0xFF0;
    uint32_t dry_right = (command.w1 >> 12) & 0xFF0;
    uint32_t wet_left = (command.w1 >> 4) & 0xFF0;
    uint32_t wet_right = (command.w1 << 4) & 0xFF0;

    // The microcode masks with these values. The wet ones aren't a whole inversion.
    const uint16_t mask_left = (command.w0 & 2) != 0 ? 0xFFFF : 0;
    const uint16_t mask_right = (command.w0 & 1) != 0 ? 0xFFFF : 0;
    const uint16_t mask_wet_left = (command.w0 & 8) != 0 ? 0xFFFC : 0;
    const uint16_t mask_wet_right = (command.w0 & 4) != 0 ? 0xFFFE : 0;
    const bool swap = (command.w0 & 0x10) != 0;
    int32_t count = static_cast<int32_t>((command.w0 >> 8) & 0xFF);
    AudioEnvOp env;
    if (ops_ != nullptr)
    {
        constexpr float kFull = 1.0F / 65536.0F;
        env.in = in & kDmemMask;
        env.dry_left = dry_left & kDmemMask;
        env.dry_right = dry_right & kDmemMask;
        env.wet_left = wet_left & kDmemMask;
        env.wet_right = wet_right & kDmemMask;
        env.bytes = static_cast<uint32_t>(std::max(count, 16)) * 2;
        env.swap = swap;

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
        env.dry_factor_left = mask_left != 0 ? -1.0F : 1.0F;
        env.dry_factor_right = mask_right != 0 ? -1.0F : 1.0F;
        env.wet_factor_left = env.dry_factor_left * (mask_wet_left != 0 ? -1.0F : 1.0F);
        env.wet_factor_right = env.dry_factor_right * (mask_wet_right != 0 ? -1.0F : 1.0F);
        RecordEnv(env);
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
            LoadQuad(samples, in + step);
            LoadQuad(dry_l, dry_left + step);
            LoadQuad(dry_r, dry_right + step);
            LoadQuad(wet_l, wet_left + step);
            LoadQuad(wet_r, wet_right + step);
            for (std::size_t i = 0; i < 8; i++)
            {
                const auto left = static_cast<int16_t>(MultiplyHigh(samples[i], left_gain) ^ mask_left);
                const auto right = static_cast<int16_t>(MultiplyHigh(samples[i], right_gain) ^ mask_right);
                dry_l[i] = Clamp16(int32_t{dry_l[i]} + left);
                dry_r[i] = Clamp16(int32_t{dry_r[i]} + right);
                const auto wet_of_left = static_cast<int16_t>(MultiplyHigh(left, wet_gain) ^ mask_wet_left);
                const auto wet_of_right = static_cast<int16_t>(MultiplyHigh(right, wet_gain) ^ mask_wet_right);
                wet_l[i] = Clamp16(int32_t{wet_l[i]} + (swap ? wet_of_right : wet_of_left));
                wet_r[i] = Clamp16(int32_t{wet_r[i]} + (swap ? wet_of_left : wet_of_right));
            }

            StoreQuad(dry_l, dry_left + step);
            StoreQuad(dry_r, dry_right + step);
            StoreQuad(wet_l, wet_left + step);
            StoreQuad(wet_r, wet_right + step);
        }

        in += 32;
        dry_left += 32;
        dry_right += 32;
        wet_left += 32;
        wet_right += 32;
        count -= 16;
        for (std::size_t i = 0; i < steps.size(); i++)
        {
            gains_[i] = static_cast<int16_t>(static_cast<uint16_t>(gains_[i]) + steps[i]);
        }
    } while (count > 0);
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

void AudioRsp::CompactPoleFilter(const AudioCommand& command, AudioBus& bus)
{
    uint32_t place = (command.w1 >> 24) == 0 ? kCompactMain : kCompactSecond;
    const auto gain = static_cast<int16_t>(command.w0);
    const auto scale = static_cast<uint16_t>(command.w0 << 2);
    const uint32_t state = command.w1 & 0xFFFFFF;

    Vector row = {};
    LoadQuad(row, layout_.book + 16);
    Vector scaled = {};
    for (std::size_t i = 0; i < 8; i++)
    {
        scaled[i] = MultiplyHigh(row[i], scale);
    }

    StoreQuad(scaled, layout_.book + 16);

    Vector last = {};
    if (((command.w0 >> 16) & 1) == 0)
    {
        DmaRead(layout_.scratch, state, 7, bus);
        LoadDouble(last, 4, layout_.scratch);
    }

    Vector samples = {};
    LoadQuad(samples, place);
    int32_t count = kCompactBytes;
    do
    {
        Vector results = {};
        for (std::size_t i = 0; i < 8; i++)
        {
            auto sum = static_cast<uint32_t>(row[i] * last[7]);
            for (std::size_t j = 0; j < i; j++)
            {
                sum += static_cast<uint32_t>(samples[j] * scaled[i - j - 1]);
            }

            sum += static_cast<uint32_t>(samples[i] * gain);
            results[i] = Clamp16((int64_t{static_cast<int32_t>(sum)} * 4) >> 16);
        }

        // The microcode filters in place. It writes each group's results while it reads the next group.
        StoreQuad(last, place - 16);
        count -= 16;
        LoadQuad(samples, place + 16);
        last = results;
        place += 16;
    } while (count > 0);

    StoreQuad(last, place - 16);
    DmaWrite(place - 8, state, 7, bus);
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
    if (!init)
    {
        DmaRead(layout_.fields, state, 79, bus);
    }

    Vector left = {};
    Vector left_fraction = {};
    Vector right = {};
    Vector right_fraction = {};
    Vector settings = {};
    if (!init)
    {
        LoadQuad(left, layout_.fields);
        LoadQuad(left_fraction, layout_.fields + 16);
        LoadQuad(right, layout_.fields + 32);
        LoadQuad(right_fraction, layout_.fields + 48);
    }

    LoadQuad(settings, layout_.fields + 64);
    const int16_t left_target = settings[0];
    const int16_t left_rate = settings[1];
    const auto left_rate_fraction = static_cast<uint16_t>(settings[2]);
    const int16_t right_target = settings[3];
    const int16_t right_rate = settings[4];
    const auto right_rate_fraction = static_cast<uint16_t>(settings[5]);
    const int16_t dry = settings[6];
    const int16_t wet = settings[7];

    uint32_t in = kCompactMain;
    uint32_t dry_left = kCompactDryLeft;
    uint32_t dry_right = kCompactDryRight;
    uint32_t wet_left = kCompactWetLeft;
    uint32_t wet_right = kCompactWetRight;

    const auto limit = [](Vector& values, int16_t target, int16_t rate)
    {
        for (int16_t& value : values)
        {
            value = rate >= 0 ? std::min(value, target) : std::max(value, target);
        }
    };

    const auto advance = [](Vector& whole, Vector& fraction, int16_t rate, uint16_t rate_fraction)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            const uint32_t sum = uint32_t{static_cast<uint16_t>(fraction[i])} + rate_fraction;
            fraction[i] = static_cast<int16_t>(sum);
            whole[i] = Clamp16(int32_t{whole[i]} + rate + static_cast<int32_t>(sum >> 16));
        }
    };

    const auto ramp = [](int16_t volume, int16_t rate, uint16_t rate_fraction, Vector& whole, Vector& fraction)
    {
        for (std::size_t i = 0; i < 8; i++)
        {
            int64_t accumulator = (int64_t{kRampSteps[i]} * rate_fraction) >> 16;
            accumulator = Wrap48(accumulator + int64_t{kRampSteps[i]} * rate);
            accumulator = Wrap48(accumulator + int64_t{volume} * 65536);
            whole[i] = Clamp16(accumulator >> 16);
            fraction[i] = static_cast<int16_t>(SaturateLow(accumulator));
        }
    };

    const auto mix_buffer = [this](uint32_t place, const Vector& samples, const Vector& volume, int16_t amount)
    {
        Vector buffer = {};
        LoadQuad(buffer, place);
        for (std::size_t i = 0; i < 8; i++)
        {
            buffer[i] = MixSample(buffer[i], samples[i], MultiplyFraction(volume[i], amount));
        }

        StoreQuad(buffer, place);
    };

    AudioEnvOp env;
    if (ops_ != nullptr)
    {
        constexpr float kFull = 1.0F / 32768.0F;
        env.in = in;
        env.dry_left = dry_left;
        env.dry_right = dry_right;
        env.wet_left = wet_left;
        env.wet_right = wet_right;
        env.bytes = kCompactBytes;
        env.state = state;
        env.init = init;
        env.from_left = static_cast<float>(init ? static_cast<int16_t>(Field(80)) : left[7]) * kFull;
        env.from_right = static_cast<float>(init ? static_cast<int16_t>(command.w0) : right[7]) * kFull;
        env.dry_factor_left = static_cast<float>(dry) * kFull;
        env.dry_factor_right = env.dry_factor_left;
        env.wet_factor_left = static_cast<float>(wet) * kFull;
        env.wet_factor_right = env.wet_factor_left;
    }

    Vector samples = {};
    int32_t count = kCompactBytes;
    if (init)
    {
        LoadQuad(samples, in);
        ramp(static_cast<int16_t>(Field(80)), left_rate, left_rate_fraction, left, left_fraction);
        limit(left, left_target, left_rate);
        ramp(static_cast<int16_t>(command.w0), right_rate, right_rate_fraction, right, right_fraction);
        limit(right, right_target, right_rate);
        mix_buffer(dry_left, samples, left, dry);
        mix_buffer(wet_left, samples, left, wet);
        mix_buffer(dry_right, samples, right, dry);
        mix_buffer(wet_right, samples, right, wet);
        count -= 16;
        in += 16;
        dry_left += 16;
        wet_left += 16;
        dry_right += 16;
        wet_right += 16;
    }

    advance(left, left_fraction, left_rate, left_rate_fraction);
    Vector saved_left = {};
    Vector saved_left_fraction = {};
    while (true)
    {
        advance(right, right_fraction, right_rate, right_rate_fraction);
        LoadQuad(samples, in);
        limit(left, left_target, left_rate);
        limit(right, right_target, right_rate);
        saved_left = left;
        saved_left_fraction = left_fraction;
        mix_buffer(dry_left, samples, left, dry);
        mix_buffer(wet_left, samples, left, wet);
        advance(left, left_fraction, left_rate, left_rate_fraction);
        mix_buffer(dry_right, samples, right, dry);
        mix_buffer(wet_right, samples, right, wet);
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

    if (ops_ != nullptr)
    {
        env.to_left = static_cast<float>(saved_left[7]) / 32768.0F;
        env.to_right = static_cast<float>(right[7]) / 32768.0F;
        RecordEnv(env);
    }

    StoreQuad(saved_left, layout_.fields);
    StoreQuad(saved_left_fraction, layout_.fields + 16);
    StoreQuad(right, layout_.fields + 32);
    StoreQuad(right_fraction, layout_.fields + 48);
    StoreQuad(settings, layout_.fields + 64);
    DmaWrite(layout_.fields, state, 79, bus);
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
