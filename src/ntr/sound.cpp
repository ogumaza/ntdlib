// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/sound.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/blep.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::ntr
{

namespace
{

constexpr int16_t kAdpcmMinimum = -0x7FFF;
constexpr uint8_t kAdpcmLastIndex = 88;
constexpr uint32_t kMainMemory = 0x02000000;
constexpr uint32_t kMainMemoryEnd = 0x03000000;
constexpr uint32_t kTimerOverflow = 0x10000;
constexpr int16_t kPsgHigh = 0x7FFF;
constexpr int16_t kPsgLow = -0x7FFF;
constexpr uint16_t kNoiseStart = 0x7FFF;
constexpr uint16_t kNoiseTaps = 0x6000;

// The channels' timer overflows before their first sample: the delays that GBATEK gives for a start.
constexpr int kPcmDelay = 3;
constexpr int kAdpcmDelay = 11;
constexpr int kPsgDelay = 1;

// A sound shorter than 4 words hangs: it stays busy and plays nothing.
constexpr uint32_t kMinimumWords = 4;

// The offsets of the registers past the channels', from kSoundRegisters. Capture 1's registers follow capture 0's: its
// SNDCAPxCNT 1 byte on, and its SNDCAPxDAD and SNDCAPxLEN 8 bytes on.
constexpr uint32_t kSoundControl = 0x100;
constexpr uint32_t kSoundBias = 0x104;
constexpr uint32_t kCaptureControl = 0x108;
constexpr uint32_t kCaptureAddress = 0x110;
constexpr uint32_t kCaptureLength = 0x114;
constexpr uint32_t kCaptureBytes = 8;
constexpr uint32_t kChannelBytes = 0x10;
constexpr uint32_t kChannels = 16;
constexpr uint32_t kCaptures = 2;
constexpr uint32_t kMostGrownChannels = snd::kMostChannels - kChannels;

// The enhanced mode's delay, and the samples of the zeros before a wave and of the samples kept behind its place.
constexpr std::size_t kDelay = wave::BandLimitedSteps::kHalfWidth;
constexpr int64_t kZeros = 2 * wave::SincKernel::kMaxHalfWidth;
constexpr int64_t kKept = wave::SincKernel::kMaxHalfWidth;
constexpr int64_t kMostKept = 8192;

// The mixer's sum, on the scale of a 16-bit sample times 256, times the master volume, on the output's scale of 1.
constexpr float kOutputScale = 1.0f / (1 << 30);

// SOUNDxCNT's fields.
constexpr uint32_t kBusy = 1u << 31;
constexpr uint32_t kHold = 1u << 15;

// SOUNDCNT's fields.
constexpr uint32_t kMasterEnable = 1u << 15;
constexpr uint32_t kChannel1NotToMixer = 1u << 12;
constexpr uint32_t kChannel3NotToMixer = 1u << 13;

// SNDCAPxCNT's fields.
constexpr uint8_t kCaptureBusy = 0x80;
constexpr uint8_t kCapturePcm8 = 0x08;
constexpr uint8_t kCaptureOneShot = 0x04;

// The limits of a capture's input: 16.8 bits.
constexpr int32_t kCaptureMinimum = -0x800000;
constexpr int32_t kCaptureMaximum = 0x7FFFFF;

// Which bits of each register's bytes a write sets, and which a read gives.
struct Masks
{
    std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters> write{};
    std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters> read{};
};

constexpr void SetMask(std::array<uint8_t, kSoundRegistersEnd - kSoundRegisters>& masks, uint32_t offset, uint32_t mask,
                       int size)
{
    for (int i = 0; i < size; i++)
    {
        masks[offset + static_cast<uint32_t>(i)] = static_cast<uint8_t>(mask >> (8 * i));
    }
}

// The masks of GBATEK's registers. The reserved bits read as 0, and so do the write-only registers.
constexpr Masks MakeMasks()
{
    Masks masks;
    for (uint32_t channel = 0; channel < kChannels; channel++)
    {
        const uint32_t base = channel * kChannelBytes;
        SetMask(masks.write, base, 0xFF7F837F, 4); // SOUNDxCNT
        SetMask(masks.read, base, 0xFF7F837F, 4);
        SetMask(masks.write, base + 0x4, 0x07FFFFFC, 4); // SOUNDxSAD
        SetMask(masks.write, base + 0x8, 0xFFFF, 2);     // SOUNDxTMR
        SetMask(masks.write, base + 0xA, 0xFFFF, 2);     // SOUNDxPNT
        SetMask(masks.write, base + 0xC, 0x003FFFFF, 4); // SOUNDxLEN
    }
    for (const auto [offset, write, read, size] : {std::array<uint32_t, 4>{kSoundControl, 0xBF7F, 0xBF7F, 4},
                                                   {kSoundBias, 0x3FF, 0x3FF, 4},
                                                   {0x108, 0x8F, 0x8F, 1},             // SNDCAP0CNT
                                                   {0x109, 0x8F, 0x8F, 1},             // SNDCAP1CNT
                                                   {0x110, 0x07FFFFFC, 0x07FFFFFC, 4}, // SNDCAP0DAD
                                                   {0x114, 0xFFFF, 0, 4},              // SNDCAP0LEN
                                                   {0x118, 0x07FFFFFC, 0x07FFFFFC, 4}, // SNDCAP1DAD
                                                   {0x11C, 0xFFFF, 0, 4}})             // SNDCAP1LEN
    {
        SetMask(masks.write, offset, write, static_cast<int>(size));
        SetMask(masks.read, offset, read, static_cast<int>(size));
    }

    return masks;
}

constexpr Masks kMasks = MakeMasks();

// A volume or pan of 127 scales by 128/128.
int32_t Factor(uint32_t value)
{
    return value == 127 ? 128 : static_cast<int32_t>(value);
}

// The divider's shifts by SOUNDxCNT's bits 8 and 9: 1, 2, 4 and 16.
constexpr std::array<int, 4> kDivisorShifts = {0, 1, 2, 4};

ChannelFormat FormatOf(uint32_t control)
{
    return static_cast<ChannelFormat>((control >> 29) & 3);
}

uint32_t RepeatOf(uint32_t control)
{
    return (control >> 27) & 3;
}

// The samples that a wave's word holds.
uint32_t SamplesPerWord(ChannelFormat format)
{
    switch (format)
    {
    case ChannelFormat::kPcm8:
        return 4;
    case ChannelFormat::kPcm16:
        return 2;
    case ChannelFormat::kImaAdpcm:
        return 8;
    case ChannelFormat::kPsg:
        break;
    }

    return 1;
}

// A captured value from the mixer's 16.8 bits, with `shift` bits of fraction: 8 for PCM16, and 16 for PCM8, whose
// value is the 16.8 bits over 0x100. GBATEK: a fraction whose top bit is set rounds toward zero. Then the fraction
// goes. A negative value with that bit therefore rounds up, and every other value rounds down.
int32_t CapturedValue(int32_t value, int shift)
{
    const int32_t whole = value >> shift;
    return value < 0 && ((value >> (shift - 1)) & 1) != 0 ? whole + 1 : whole;
}

// The samples in `words` words of a wave, and the sample at the start of word `words`.
uint32_t SamplesIn(ChannelFormat format, uint32_t words)
{
    switch (format)
    {
    case ChannelFormat::kPcm8:
        return words * 4;
    case ChannelFormat::kPcm16:
        return words * 2;
    case ChannelFormat::kImaAdpcm:
        return words == 0 ? 0 : (words - 1) * 8; // the first word holds the decoder's state
    case ChannelFormat::kPsg:
        break;
    }

    return 0;
}

} // namespace

wave::ImaAdpcmState AdpcmStep(wave::ImaAdpcmState state, uint8_t nibble)
{
    if (state.index > kAdpcmLastIndex)
    {
        // The table of steps holds 0 past index 88.
        const int16_t sample = (nibble & 8) ? std::max(state.sample, kAdpcmMinimum) : state.sample;
        return {sample, kAdpcmLastIndex};
    }

    wave::ImaAdpcmState next = wave::ImaAdpcmNext(state, nibble);
    if (nibble & 8)
    {
        next.sample = std::max(next.sample, kAdpcmMinimum);
    }

    return next;
}

wave::ImaAdpcmState AdpcmHeader(uint32_t word)
{
    return {static_cast<int16_t>(word & 0xFFFF), static_cast<uint8_t>((word >> 16) & 0x7F)};
}

SoundUnit::SoundUnit(std::span<uint8_t> main_memory) : main_memory_(main_memory)
{
}

uint32_t SoundUnit::Read(uint32_t address, int size) const
{
    if (address >= kGrownChannelRegisters)
    {
        const uint32_t index = (address - kGrownChannelRegisters) / kGrownChannelBytes;
        const uint32_t offset = (address - kGrownChannelRegisters) % kGrownChannelBytes;
        if (index >= grown_registers_.size())
        {
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < size; i++)
        {
            const uint32_t at = offset + static_cast<uint32_t>(i);
            const uint8_t mask = at < kChannelBytes ? kMasks.read[at] : at < kGrownChannelTwin + 4 ? 0xFF : 0;
            value |= static_cast<uint32_t>(grown_registers_[index][at] & mask) << (8 * i);
        }
        return value;
    }

    const uint32_t offset = address - kSoundRegisters;
    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        const uint32_t at = offset + static_cast<uint32_t>(i);
        value |= static_cast<uint32_t>(registers_[at] & kMasks.read[at]) << (8 * i);
    }

    return value;
}

void SoundUnit::Write(uint32_t address, int size, uint32_t value)
{
    if (address >= kGrownChannelRegisters)
    {
        WriteGrown(address, size, value);
        return;
    }

    const uint32_t offset = address - kSoundRegisters;
    for (int i = 0; i < size; i++)
    {
        const uint32_t at = offset + static_cast<uint32_t>(i);
        const uint8_t mask = kMasks.write[at];
        registers_[at] = static_cast<uint8_t>((registers_[at] & ~mask) | ((value >> (8 * i)) & mask));
    }

    // A write that reaches a capture's SNDCAPxCNT starts or stops the capture. The capture's timer starts from channel
    // 1's or channel 3's SOUNDxTMR.
    const uint32_t last = offset + static_cast<uint32_t>(size) - 1;
    for (uint32_t index = 0; index < kCaptures; index++)
    {
        const uint32_t at = kCaptureControl + index;
        if (at < offset || at > last)
        {
            continue;
        }

        Capture& capture = captures_[index];
        const bool busy = (registers_[at] & kCaptureBusy) != 0;
        if (busy && !capture.busy)
        {
            capture = Capture{.busy = true, .counter = ChannelWord(static_cast<int>(index * 2 + 1), 0x8) % 0x10000};
        }
        else if (!busy)
        {
            capture.busy = false;
        }
    }

    // A write that reaches a channel's SOUNDxCNT's high byte starts or stops the channel.
    if (last >= kChannels * kChannelBytes || last % kChannelBytes != 3)
    {
        return;
    }

    const int index = static_cast<int>(last / kChannelBytes);
    const bool busy = (Control(index) & kBusy) != 0;
    if (busy && !channels_[static_cast<std::size_t>(index)].busy)
    {
        Start(index);
    }
    else if (!busy && channels_[static_cast<std::size_t>(index)].busy)
    {
        Stop(index);
    }
}

void SoundUnit::WriteGrown(uint32_t address, int size, uint32_t value)
{
    const uint32_t index = (address - kGrownChannelRegisters) / kGrownChannelBytes;
    const uint32_t offset = (address - kGrownChannelRegisters) % kGrownChannelBytes;
    if (index >= static_cast<uint32_t>(kMostGrownChannels))
    {
        return;
    }
    Grow(static_cast<int>(kChannels + index + 1));

    std::array<uint8_t, kGrownChannelBytes>& bytes = grown_registers_[index];
    for (int i = 0; i < size; i++)
    {
        const uint32_t at = offset + static_cast<uint32_t>(i);
        const uint8_t mask = at < kChannelBytes ? kMasks.write[at] : at < kGrownChannelTwin + 4 ? 0xFF : 0;
        bytes[at] = static_cast<uint8_t>((bytes[at] & ~mask) | ((value >> (8 * i)) & mask));
    }

    // A write that reaches SOUNDxCNT's high byte starts or stops the channel. The console's channels do the same.
    if (offset > 3 || offset + static_cast<uint32_t>(size) <= 3)
    {
        return;
    }
    const int channel = static_cast<int>(kChannels + index);
    const bool busy = (Control(channel) & kBusy) != 0;
    if (busy && !channels_[static_cast<std::size_t>(channel)].busy)
    {
        Start(channel);
    }
    else if (!busy && channels_[static_cast<std::size_t>(channel)].busy)
    {
        Stop(channel);
    }
}

void SoundUnit::Grow(int channels)
{
    if (channels <= ChannelCount())
    {
        return;
    }
    channels_.resize(static_cast<std::size_t>(channels));
    grown_registers_.resize(static_cast<std::size_t>(channels) - kChannels);
    muted_.resize(static_cast<std::size_t>(channels));
    meters_.resize(static_cast<std::size_t>(channels), -1);
    if (enhanced_)
    {
        enhanced_->voices.resize(static_cast<std::size_t>(channels));
    }
}

int SoundUnit::ChannelCount() const
{
    return static_cast<int>(channels_.size());
}

void SoundUnit::Stop(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (enhanced_)
    {
        const uint64_t now = enhanced_->ticks * kEnhancedFrameSamples;
        StopVoice(index, now, now);
    }
    channel = Channel{.sample = channel.sample};
    if (enhanced_)
    {
        StepLevel(index, enhanced_->ticks);
    }
}

void SoundUnit::Run(uint64_t ticks, std::vector<SoundOutput>& out)
{
    Advance(ticks, &out, nullptr);
}

void SoundUnit::Run(uint64_t ticks)
{
    Advance(ticks, nullptr, nullptr);
}

void SoundUnit::StartEnhanced()
{
    // The channels that play now join the mix at their next start.
    enhanced_.emplace();
    enhanced_->voices.resize(channels_.size());
}

bool SoundUnit::Enhanced() const
{
    return enhanced_.has_value();
}

void SoundUnit::Run(uint64_t ticks, std::vector<float>& out)
{
    if (!enhanced_)
    {
        throw std::logic_error("the sound unit isn't in the enhanced mixing mode");
    }
    Advance(ticks, nullptr, &out);
}

void SoundUnit::SetMutedChannels(uint16_t channels)
{
    std::ranges::fill(muted_, uint8_t{0});
    for (uint32_t channel = 0; channel < kChannels; channel++)
    {
        muted_[channel] = static_cast<uint8_t>((channels >> channel) & 1);
    }
}

void SoundUnit::SetMutedChannels(std::span<const uint8_t> channels)
{
    std::ranges::fill(muted_, uint8_t{0});
    for (std::size_t channel = 0; channel < std::min(channels.size(), muted_.size()); channel++)
    {
        muted_[channel] = channels[channel] != 0 ? 1 : 0;
    }
}

void SoundUnit::SetMeters(const std::array<int8_t, 16>& meters)
{
    SetMeters(std::span<const int8_t>(meters));
}

void SoundUnit::SetMeters(std::span<const int8_t> meters)
{
    std::ranges::fill(meters_, int8_t{-1});
    for (std::size_t channel = 0; channel < std::min(meters.size(), meters_.size()); channel++)
    {
        meters_[channel] = meters[channel];
    }
    metered_ = std::ranges::any_of(meters_, [](int8_t meter) { return meter >= 0; });
    peaks_ = {};
}

const std::array<std::array<int32_t, 2>, 16>& SoundUnit::MeterPeaks() const
{
    return peaks_;
}

void SoundUnit::Advance(uint64_t ticks, std::vector<SoundOutput>* out, std::vector<float>* enhanced_out)
{
    if (enhanced_)
    {
        if (enhanced_out)
        {
            BeginEnhanced();
        }
        else
        {
            enhanced_->stale = true;
        }
        MixDue(enhanced_out != nullptr);
    }

    while (ticks > 0)
    {
        // A step ends at the next output sample, at a capture's next sample, or at the enhanced mode's next sample.
        uint64_t limit = std::min<uint64_t>(ticks, kTicksPerSample - phase_);
        for (const Capture& capture : captures_)
        {
            if (capture.busy)
            {
                limit = std::min<uint64_t>(limit, kTimerOverflow - capture.counter);
            }
        }
        if (enhanced_)
        {
            // The first tick after the next sample's time.
            const uint64_t next = enhanced_->samples * kEnhancedFrameTicks / kEnhancedFrameSamples + 1;
            limit = std::min<uint64_t>(limit, next - enhanced_->ticks);
        }

        const uint32_t step = static_cast<uint32_t>(limit);
        const uint64_t end_tick = enhanced_ ? enhanced_->ticks + step : 0;
        for (int index = 0; index < ChannelCount(); index++)
        {
            Channel& channel = channels_[static_cast<std::size_t>(index)];
            if (!channel.busy && !channel.playing)
            {
                continue;
            }

            channel.counter += step;
            while (channel.counter >= kTimerOverflow && (channel.busy || channel.playing))
            {
                const uint32_t late = channel.counter - kTimerOverflow;
                channel.counter -= kTimerOverflow - ChannelWord(index, 0x8) % 0x10000;
                Step(index);
                if (enhanced_)
                {
                    Overflowed(index, end_tick, late);
                }
            }
        }

        for (uint32_t index = 0; index < kCaptures; index++)
        {
            Capture& capture = captures_[index];
            if (!capture.busy)
            {
                continue;
            }

            capture.counter += step;
            if (capture.counter >= kTimerOverflow)
            {
                capture.counter -= kTimerOverflow - ChannelWord(static_cast<int>(index * 2 + 1), 0x8) % 0x10000;
                StepCapture(static_cast<int>(index));
            }
        }

        phase_ += step;
        ticks -= step;
        if (phase_ == kTicksPerSample)
        {
            phase_ = 0;
            if (out)
            {
                const std::array<int32_t, 2> mixed = Mix();
                out->push_back({static_cast<uint16_t>(mixed[0]), static_cast<uint16_t>(mixed[1])});
            }
        }
        if (enhanced_)
        {
            enhanced_->ticks = end_tick;
            MixDue(enhanced_out != nullptr);
        }
    }

    if (enhanced_ && enhanced_out)
    {
        FinishEnhanced(*enhanced_out);
    }
}

bool SoundUnit::Busy(int channel) const
{
    return channels_[static_cast<std::size_t>(channel)].busy;
}

uint32_t SoundUnit::Phase() const
{
    return phase_;
}

SoundUnit::ChannelPlace SoundUnit::PlaceOf(int index) const
{
    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t control = Control(index);
    if (FormatOf(control) != ChannelFormat::kPsg)
    {
        return {.playing = channel.busy, .loops = RepeatOf(control) == 1, .position = channel.position};
    }

    constexpr uint32_t kRectangleSteps = 8;
    constexpr uint32_t kNoiseSteps = 0x7FFF;
    return {.playing = channel.busy,
            .loops = true,
            .position = channel.position % (Twin(index) >= 14 ? kNoiseSteps : kRectangleSteps)};
}

uint32_t SoundUnit::Control(int channel) const
{
    return ChannelWord(channel, 0);
}

uint32_t SoundUnit::ChannelWord(int channel, uint32_t offset) const
{
    if (channel < static_cast<int>(kChannels))
    {
        return Word(static_cast<uint32_t>(channel) * kChannelBytes + offset);
    }
    return xsflib::LoadLe32(&grown_registers_[static_cast<std::size_t>(channel) - kChannels][offset]);
}

uint8_t* SoundUnit::ChannelBytes(int channel)
{
    if (channel < static_cast<int>(kChannels))
    {
        return &registers_[static_cast<std::size_t>(channel) * kChannelBytes];
    }
    return grown_registers_[static_cast<std::size_t>(channel) - kChannels].data();
}

int SoundUnit::Twin(int channel) const
{
    if (channel < static_cast<int>(kChannels))
    {
        return channel;
    }
    return static_cast<int>(ChannelWord(channel, kGrownChannelTwin) % kChannels);
}

bool SoundUnit::Muted(int channel) const
{
    return muted_[static_cast<std::size_t>(channel)] != 0;
}

uint32_t SoundUnit::Word(uint32_t offset) const
{
    return xsflib::LoadLe32(&registers_[offset]);
}

uint8_t SoundUnit::Byte(uint32_t address) const
{
    if (address < kMainMemory || address >= kMainMemoryEnd || main_memory_.empty())
    {
        return 0;
    }

    return main_memory_[(address - kMainMemory) % main_memory_.size()];
}

uint32_t SoundUnit::MemoryWord(uint32_t address) const
{
    // A word that lies whole in main memory, after the mirroring, takes one look-up.
    if (address >= kMainMemory && address <= kMainMemoryEnd - 4 && !main_memory_.empty())
    {
        const std::size_t at = (address - kMainMemory) % main_memory_.size();
        if (at + 4 <= main_memory_.size())
        {
            return xsflib::LoadLe32(&main_memory_[at]);
        }
    }

    return static_cast<uint32_t>(Byte(address)) | static_cast<uint32_t>(Byte(address + 1)) << 8 |
           static_cast<uint32_t>(Byte(address + 2)) << 16 | static_cast<uint32_t>(Byte(address + 3)) << 24;
}

void SoundUnit::WriteByte(uint32_t address, uint8_t value)
{
    if (address < kMainMemory || address >= kMainMemoryEnd || main_memory_.empty())
    {
        return;
    }

    main_memory_[(address - kMainMemory) % main_memory_.size()] = value;
}

void SoundUnit::Start(int index)
{
    const uint32_t control = Control(index);
    const ChannelFormat format = FormatOf(control);
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel = Channel{
        .busy = true,
        .counter = ChannelWord(index, 0x8) % 0x10000,
        .delay = format == ChannelFormat::kPsg        ? kPsgDelay
                 : format == ChannelFormat::kImaAdpcm ? kAdpcmDelay
                                                      : kPcmDelay,
        .noise = kNoiseStart,
    };
    if (format != ChannelFormat::kPsg)
    {
        for (uint32_t& word : channel.queue)
        {
            word = Fetch(index);
        }
    }
    if (enhanced_)
    {
        StartVoice(index);
        StepLevel(index, enhanced_->ticks);
    }
}

void SoundUnit::Step(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t control = Control(index);
    if (!channel.busy)
    {
        // A one-shot wave's last sample has ended. The hold flag keeps it in the output.
        channel.playing = false;
        return;
    }

    if (channel.delay > 0)
    {
        channel.delay--;
        if (channel.delay > 0)
        {
            return;
        }

        if (FormatOf(control) == ChannelFormat::kImaAdpcm)
        {
            // The first word holds the decoder's state. The samples start with the next.
            channel.adpcm = AdpcmHeader(channel.queue[channel.head]);
            NextWord(index);
        }
        channel.position = 0;
    }
    else
    {
        channel.position++;
    }
    const bool advanced = channel.position != 0;

    if (FormatOf(control) == ChannelFormat::kPsg)
    {
        channel.playing = true;
        const int twin = Twin(index);
        if (twin >= 14)
        {
            const bool carry = (channel.noise & 1) != 0;
            channel.noise = static_cast<uint16_t>(channel.noise >> 1);
            if (carry)
            {
                channel.noise ^= kNoiseTaps;
            }
            channel.sample = carry ? kPsgLow : kPsgHigh;
        }
        else if (twin >= 8)
        {
            // Each cycle of 8 samples starts with its low part.
            const uint32_t duty = (control >> 24) & 7;
            channel.sample = duty != 7 && channel.position % 8 >= 7 - duty ? kPsgHigh : kPsgLow;
        }
        else
        {
            channel.sample = 0;
        }
        return;
    }

    StepWave(index, control, advanced);
}

void SoundUnit::StepWave(int index, uint32_t control, bool advanced)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const ChannelFormat format = FormatOf(control);
    const uint32_t loop_words = ChannelWord(index, 0x8) >> 16;
    const uint32_t words = loop_words + ChannelWord(index, 0xC);
    if (words < kMinimumWords)
    {
        channel.playing = false;
        channel.sample = 0;
        return;
    }

    const uint32_t end = SamplesIn(format, words);
    const uint32_t repeat = RepeatOf(control);
    if (advanced && channel.position >= end && repeat == 1)
    {
        channel.position = SamplesIn(format, loop_words);
        channel.adpcm = channel.loop_adpcm;
        NextWord(index);
    }
    else if (advanced && (channel.position & (SamplesPerWord(format) - 1)) == 0) // a power of two in each format
    {
        NextWord(index);
    }

    channel.playing = true;
    channel.sample = WaveSample(index, control);
    if (repeat == 2 && channel.position + 1 >= end)
    {
        // A one-shot wave's busy bit clears as its last sample starts. The sample plays to the end of its period.
        channel.busy = false;
        ChannelBytes(index)[3] &= 0x7F;
    }
}

int16_t SoundUnit::WaveSample(int index, uint32_t control)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t word = channel.queue[channel.head];
    switch (FormatOf(control))
    {
    case ChannelFormat::kPcm8:
        return static_cast<int16_t>(static_cast<int8_t>(word >> (8 * (channel.position % 4))) * 256);
    case ChannelFormat::kPcm16:
        return static_cast<int16_t>(word >> (16 * (channel.position % 2)));
    case ChannelFormat::kImaAdpcm:
        {
            // The channel keeps the decoder's state at the loop's start for each return to it.
            const uint32_t loop_words = ChannelWord(index, 0x8) >> 16;
            if (channel.position == SamplesIn(ChannelFormat::kImaAdpcm, loop_words))
            {
                channel.loop_adpcm = channel.adpcm;
            }
            const auto nibble = static_cast<uint8_t>((word >> (4 * (channel.position % 8))) & 0x0F);
            channel.adpcm = AdpcmStep(channel.adpcm, nibble);
            return channel.adpcm.sample;
        }
    case ChannelFormat::kPsg:
        break;
    }

    return 0;
}

void SoundUnit::NextWord(int index)
{
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    channel.queue[channel.head] = Fetch(index);
    channel.head = (channel.head + 1) % kQueueWords;
}

uint32_t SoundUnit::Fetch(int index)
{
    // A looping wave's words return to the loop's start after the last. The other waves' words run on in memory. An
    // IMA-ADPCM loop never returns to the first word, which holds the decoder's state.
    Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t control = Control(index);
    const uint32_t word = MemoryWord(ChannelWord(index, 0x4) + channel.fetch * 4);
    const uint32_t loop_words = ChannelWord(index, 0x8) >> 16;
    channel.fetch++;
    if (RepeatOf(control) == 1 && channel.fetch >= loop_words + ChannelWord(index, 0xC))
    {
        channel.fetch = FormatOf(control) == ChannelFormat::kImaAdpcm ? std::max<uint32_t>(loop_words, 1) : loop_words;
    }

    return word;
}

void SoundUnit::StepCapture(int index)
{
    Capture& capture = captures_[static_cast<std::size_t>(index)];
    const uint32_t at = static_cast<uint32_t>(index);
    const uint8_t control = registers_[kCaptureControl + at];
    const uint32_t address = Word(kCaptureAddress + at * kCaptureBytes);
    const uint32_t length = std::max<uint32_t>(Word(kCaptureLength + at * kCaptureBytes), 1) * 4;

    // Capture 0 takes the left side of the mixer, and capture 1 the right.
    std::array<int32_t, 2> mixer{};
    const uint32_t sound_control = Word(kSoundControl);
    for (int channel = 0; channel < ChannelCount(); channel++)
    {
        std::array<int32_t, 2> sides{};
        if ((channel == 1 && (sound_control & kChannel1NotToMixer)) ||
            (channel == 3 && (sound_control & kChannel3NotToMixer)) || !Sides(channel, sides))
        {
            continue;
        }
        mixer[0] += sides[0];
        mixer[1] += sides[1];
    }

    const int32_t value = std::clamp(mixer[static_cast<std::size_t>(index)], kCaptureMinimum, kCaptureMaximum);
    if (control & kCapturePcm8)
    {
        WriteByte(address + capture.written, static_cast<uint8_t>(CapturedValue(value, 16)));
        capture.written++;
    }
    else
    {
        const auto sample = static_cast<uint16_t>(CapturedValue(value, 8));
        WriteByte(address + capture.written, static_cast<uint8_t>(sample));
        WriteByte(address + capture.written + 1, static_cast<uint8_t>(sample >> 8));
        capture.written += 2;
    }

    if (capture.written >= length)
    {
        capture.written = 0;
        if (control & kCaptureOneShot)
        {
            capture.busy = false;
            registers_[kCaptureControl + at] &= static_cast<uint8_t>(~kCaptureBusy);
        }
    }
}

bool SoundUnit::Sides(int index, std::array<int32_t, 2>& sides) const
{
    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t control = Control(index);
    const bool held = !channel.playing && !channel.busy && (control & kHold) != 0;
    if ((!channel.playing && !held) || Muted(index))
    {
        return false;
    }

    // GBATEK's steps: the divider (16.4), the volume (16.11), the pan (16.18), and the cut to 16.8.
    const int32_t divided = (static_cast<int32_t>(channel.sample) * 16) >> kDivisorShifts[(control >> 8) & 3];
    const int32_t scaled = divided * Factor(control & 0x7F);
    const int32_t pan = Factor((control >> 16) & 0x7F);
    sides = {static_cast<int32_t>((int64_t{scaled} * (128 - pan)) >> 10),
             static_cast<int32_t>((int64_t{scaled} * pan) >> 10)};
    return true;
}

std::array<int32_t, 2> SoundUnit::Mix()
{
    std::array<int32_t, 2> mixer{};
    std::array<int32_t, 2> channel1{};
    std::array<int32_t, 2> channel3{};
    std::array<std::array<int32_t, 2>, 16> sums{};
    const uint32_t sound_control = Word(kSoundControl);
    for (int index = 0; index < ChannelCount(); index++)
    {
        std::array<int32_t, 2> sides{};
        if (!Sides(index, sides))
        {
            continue;
        }

        if (index == 1)
        {
            channel1 = sides;
            if (sound_control & kChannel1NotToMixer)
            {
                continue;
            }
        }
        else if (index == 3)
        {
            channel3 = sides;
            if (sound_control & kChannel3NotToMixer)
            {
                continue;
            }
        }
        mixer[0] += sides[0];
        mixer[1] += sides[1];
        if (const int meter = meters_[static_cast<std::size_t>(index)]; metered_ && meter >= 0)
        {
            sums[static_cast<std::size_t>(meter)][0] += sides[0];
            sums[static_cast<std::size_t>(meter)][1] += sides[1];
        }
    }

    // The meters take the master volume with 6 more bits than the output.
    if (metered_ && (sound_control & kMasterEnable))
    {
        const int32_t factor = Factor(sound_control & 0x7F);
        for (std::size_t meter = 0; meter < sums.size(); meter++)
        {
            for (std::size_t side = 0; side < 2; side++)
            {
                const int64_t level = std::abs((static_cast<int64_t>(sums[meter][side]) * factor) >> 15);
                peaks_[meter][side] = std::max(peaks_[meter][side], static_cast<int32_t>(level));
            }
        }
    }

    const int32_t bias = static_cast<int32_t>(Word(kSoundBias) & 0x3FF);
    std::array<int32_t, 2> out{};
    for (int side = 0; side < 2; side++)
    {
        int32_t value = mixer[side];
        switch ((sound_control >> (8 + 2 * side)) & 3)
        {
        case 1:
            value = channel1[side];
            break;
        case 2:
            value = channel3[side];
            break;
        case 3:
            value = channel1[side] + channel3[side];
            break;
        default:
            break;
        }

        // The master volume (14.21), and the cut to the integer.
        const int64_t master =
            (sound_control & kMasterEnable) ? (static_cast<int64_t>(value) * Factor(sound_control & 0x7F)) >> 21 : 0;
        out[static_cast<std::size_t>(side)] = std::clamp(static_cast<int32_t>(master) + bias, 0, 0x3FF);
    }

    return out;
}

void SoundUnit::StartVoice(int index)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    Voice fresh;
    fresh.steps = std::move(voice.steps);
    fresh.level = voice.level;
    fresh.idle = voice.idle;
    fresh.settled = voice.settled;
    fresh.run_gains = std::move(voice.run_gains);
    voice = std::move(fresh);

    const ChannelFormat format = FormatOf(Control(index));
    voice.psg = format == ChannelFormat::kPsg;
    voice.sounding = !voice.psg;
    voice.fresh = true;
    voice.delay = voice.psg ? kPsgDelay : format == ChannelFormat::kImaAdpcm ? kAdpcmDelay : kPcmDelay;
    if (!voice.psg)
    {
        voice.input.assign(static_cast<std::size_t>(kZeros), 0.0f);
        voice.first = -kZeros;
    }
}

void SoundUnit::StopVoice(int index, uint64_t at, uint64_t now)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    if (!voice.sounding || voice.stopped)
    {
        return;
    }

    // The samples after the one that plays go silent, or hold it with the hold flag. The voice rings out from there.
    voice.place = VoicePlace(index, at, now);
    voice.period = Period(index);
    voice.stopped = true;
    voice.stopped_at = at;
    voice.end = std::min(voice.end, static_cast<int64_t>(std::floor(voice.place)) + 1);
    voice.holds = (Control(index) & kHold) != 0;
    voice.held = channels_[static_cast<std::size_t>(index)].sample;
    for (int64_t sample = std::max(voice.end, voice.first); sample < voice.decoded; sample++)
    {
        voice.input[static_cast<std::size_t>(sample - voice.first)] = voice.holds ? voice.held : 0.0f;
    }
}

void SoundUnit::StepLevel(int index, uint64_t tick)
{
    // A rectangular wave's or noise's level: the channel's sample while it plays, or held.
    EnhancedState& enhanced = *enhanced_;
    Voice& voice = enhanced.voices[static_cast<std::size_t>(index)];
    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    const bool held = !channel.playing && !channel.busy && (Control(index) & kHold) != 0;
    const float level = voice.psg && (channel.playing || held) ? static_cast<float>(channel.sample) : 0.0f;
    if (level == voice.level)
    {
        return;
    }

    if (!enhanced.stale)
    {
        // The steps start with the run's first sample, at the level before the change.
        if (!voice.steps)
        {
            voice.steps.emplace(kEnhancedFrameTicks, kEnhancedFrameSamples);
            voice.steps->Restart(enhanced.run_first, voice.level);
            voice.run_gains.assign(enhanced.run_waves.size(), {});
        }
        else if (voice.idle)
        {
            voice.steps->Restart(enhanced.run_first, voice.level);
            voice.idle = false;
        }
        voice.steps->Step(tick, level - voice.level);
        voice.settled = tick * kEnhancedFrameSamples / kEnhancedFrameTicks + 1 + 2 * kDelay;
    }
    voice.level = level;
}

void SoundUnit::Overflowed(int index, uint64_t tick, uint32_t late)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    voice.overflows++;
    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.busy && !channel.playing)
    {
        // A one-shot wave's last sample has ended.
        StopVoice(index, (tick - late) * kEnhancedFrameSamples, tick * kEnhancedFrameSamples);
    }
    StepLevel(index, tick - late);
}

uint32_t SoundUnit::Period(int index) const
{
    return kTimerOverflow - ChannelWord(index, 0x8) % 0x10000;
}

double SoundUnit::VoicePlace(int index, uint64_t at, uint64_t now) const
{
    // The sample that plays, and the share of the timer's period since it started. The channel's timer gives the
    // place at `now`.
    const Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    if (voice.stopped)
    {
        return voice.place +
               (static_cast<double>(at) - static_cast<double>(voice.stopped_at)) / kEnhancedFrameSamples / voice.period;
    }

    const Channel& channel = channels_[static_cast<std::size_t>(index)];
    const uint32_t period = Period(index);
    const double since = static_cast<double>(channel.counter) - static_cast<double>(kTimerOverflow - period) -
                         (static_cast<double>(now) - static_cast<double>(at)) / kEnhancedFrameSamples;
    return static_cast<double>(static_cast<int64_t>(voice.overflows) - voice.delay) + since / period;
}

float SoundUnit::WaveAt(int index, double place)
{
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    const uint32_t period = voice.stopped ? voice.period : Period(index);
    if (voice.kernel == nullptr || period != voice.kernel_period)
    {
        voice.kernel =
            &wave::SincKernel::ForStep(static_cast<double>(kEnhancedFrameTicks) / kEnhancedFrameSamples / period);
        voice.kernel_period = period;
    }
    const wave::SincKernel& kernel = *voice.kernel;
    const int64_t sample = static_cast<int64_t>(std::floor(place));
    const auto fraction = static_cast<uint32_t>(std::min(65535.0, (place - static_cast<double>(sample)) * 65536.0));
    const int64_t half = kernel.HalfWidth();

    // A place far past the samples decoded, after a run without output, skips the samples between.
    const int64_t keep = sample - kKept;
    if (voice.decoded < keep)
    {
        while (voice.decoded < keep)
        {
            DecodeNext(index);
        }
        voice.input.clear();
        voice.first = voice.decoded;
    }
    while (voice.decoded <= sample + half)
    {
        voice.input.push_back(DecodeNext(index));
    }
    if (keep - voice.first > kMostKept)
    {
        voice.input.erase(voice.input.begin(), voice.input.begin() + static_cast<std::ptrdiff_t>(keep - voice.first));
        voice.first = keep;
    }

    const int64_t from = sample - half + 1;
    if (from < voice.first)
    {
        return 0.0f;
    }
    return kernel.At(&voice.input[static_cast<std::size_t>(from - voice.first)], fraction);
}

float SoundUnit::DecodeNext(int index)
{
    // The wave's samples in the order the channel plays them. StepWave and WaveSample give the same samples.
    Voice& voice = enhanced_->voices[static_cast<std::size_t>(index)];
    const int64_t sample = voice.decoded++;
    if (sample < 0)
    {
        return 0.0f;
    }
    if (sample >= voice.end)
    {
        return voice.holds ? voice.held : 0.0f;
    }

    const uint32_t control = Control(index);
    const ChannelFormat format = FormatOf(control);
    const uint32_t loop_words = ChannelWord(index, 0x8) >> 16;
    const uint32_t words = loop_words + ChannelWord(index, 0xC);
    if (format == ChannelFormat::kPsg || words < kMinimumWords)
    {
        return 0.0f;
    }

    const uint32_t end = SamplesIn(format, words);
    const uint32_t loop_start = SamplesIn(format, loop_words);
    const uint32_t repeat = RepeatOf(control);
    uint32_t at = voice.wave_sample;
    if (at >= end && repeat == 1)
    {
        at = loop_start;
        voice.adpcm = voice.loop_adpcm;
    }
    else if (at >= end && repeat == 2)
    {
        return (control & kHold) ? voice.last : 0.0f;
    }

    const uint32_t address = ChannelWord(index, 0x4);
    float value = 0.0f;
    switch (format)
    {
    case ChannelFormat::kPcm8:
        value = static_cast<float>(static_cast<int8_t>(Byte(address + at))) * 256.0f;
        break;
    case ChannelFormat::kPcm16:
        value = static_cast<float>(
            static_cast<int16_t>(Byte(address + 2 * at) | static_cast<uint32_t>(Byte(address + 2 * at + 1)) << 8));
        break;
    case ChannelFormat::kImaAdpcm:
        {
            if (at == 0)
            {
                voice.adpcm = AdpcmHeader(MemoryWord(address));
            }
            if (at == loop_start)
            {
                voice.loop_adpcm = voice.adpcm;
            }
            const uint32_t word = MemoryWord(address + 4 + (at / 8) * 4);
            voice.adpcm = AdpcmStep(voice.adpcm, static_cast<uint8_t>((word >> (4 * (at % 8))) & 0x0F));
            value = voice.adpcm.sample;
            break;
        }
    case ChannelFormat::kPsg:
        break;
    }
    voice.wave_sample = at + 1;
    voice.last = value;
    return value;
}

std::array<float, 2> SoundUnit::Gains(int index) const
{
    // The volume, the divider and the pan of Sides, on the scale of the mixer's 16.8 bits.
    const uint32_t control = Control(index);
    const float scale =
        static_cast<float>(Factor(control & 0x7F)) / static_cast<float>(64 << kDivisorShifts[(control >> 8) & 3]);
    const int32_t pan = Factor((control >> 16) & 0x7F);
    return {scale * static_cast<float>(128 - pan), scale * static_cast<float>(pan)};
}

void SoundUnit::BeginEnhanced()
{
    EnhancedState& enhanced = *enhanced_;
    enhanced.run_first = enhanced.samples;
    enhanced.run_waves.clear();
    enhanced.run_master.clear();
    enhanced.run_meters.clear();
    for (Voice& voice : enhanced.voices)
    {
        voice.run_gains.clear();
    }
    if (!enhanced.stale)
    {
        return;
    }

    // After a run without output, the steps start again at the levels, the delay starts empty, and the gains take
    // their values at once. A voice that the run gave a level gets steps then: a level can hold without a change.
    for (Voice& voice : enhanced.voices)
    {
        if (!voice.steps && voice.level != 0.0f)
        {
            voice.steps.emplace(kEnhancedFrameTicks, kEnhancedFrameSamples);
        }
        if (voice.steps)
        {
            voice.steps->Restart(enhanced.samples, voice.level);
            voice.idle = false;
            voice.settled = enhanced.samples;
        }
        voice.fresh = true;
    }
    enhanced.master_fresh = true;
    for (std::vector<float>& line : enhanced.delayed)
    {
        line.assign(kDelay, 0.0f);
    }
    enhanced.stale = false;
}

void SoundUnit::MixDue(bool output)
{
    // The samples whose times lie before the present tick. A run's samples are those whose times lie in it.
    EnhancedState& enhanced = *enhanced_;
    while (enhanced.samples * kEnhancedFrameTicks < enhanced.ticks * kEnhancedFrameSamples)
    {
        if (output)
        {
            MixEnhanced();
        }
        enhanced.samples++;
    }
}

void SoundUnit::MixEnhanced()
{
    EnhancedState& enhanced = *enhanced_;
    const uint64_t at = enhanced.samples * kEnhancedFrameTicks;
    const uint64_t now = enhanced.ticks * kEnhancedFrameSamples;
    const uint32_t sound_control = Word(kSoundControl);
    const auto ramp = [](std::array<float, 2>& gains, std::array<float, 2>& targets, std::array<float, 2>& slopes,
                         uint32_t& left, bool& fresh, const std::array<float, 2>& target)
    {
        if (fresh)
        {
            gains = target;
            targets = target;
            left = 0;
            fresh = false;
        }
        else if (target != targets)
        {
            targets = target;
            for (std::size_t side = 0; side < 2; side++)
            {
                slopes[side] = (target[side] - gains[side]) / kEnhancedFrameSamples;
            }
            left = kEnhancedFrameSamples;
        }
        if (left > 0 && --left == 0)
        {
            gains = targets;
        }
        else if (left > 0)
        {
            gains[0] += slopes[0];
            gains[1] += slopes[1];
        }
    };

    std::array<float, 6> waves{};
    std::array<float, 32> meters{};
    for (int index = 0; index < ChannelCount(); index++)
    {
        Voice& voice = enhanced.voices[static_cast<std::size_t>(index)];
        if (!voice.sounding && !voice.steps)
        {
            continue;
        }

        ramp(voice.gains, voice.targets, voice.slopes, voice.ramp, voice.fresh, Gains(index));
        const bool muted = Muted(index);
        if (voice.steps)
        {
            voice.run_gains.push_back(muted ? std::array<float, 2>{} : voice.gains);
        }
        if (!voice.sounding)
        {
            continue;
        }

        const double place = VoicePlace(index, at, now);
        if (voice.stopped && !voice.holds && place - static_cast<double>(kZeros) > static_cast<double>(voice.end))
        {
            // The voice has rung out.
            voice.sounding = false;
            voice.input = {};
            continue;
        }
        const float value = WaveAt(index, place);
        if (muted)
        {
            continue;
        }

        const std::array<float, 2> sides = {value * voice.gains[0], value * voice.gains[1]};
        bool mixed = true;
        if (index == 1)
        {
            waves[2] = sides[0];
            waves[3] = sides[1];
            mixed = (sound_control & kChannel1NotToMixer) == 0;
        }
        else if (index == 3)
        {
            waves[4] = sides[0];
            waves[5] = sides[1];
            mixed = (sound_control & kChannel3NotToMixer) == 0;
        }
        if (!mixed)
        {
            continue;
        }
        waves[0] += sides[0];
        waves[1] += sides[1];
        if (const int meter = meters_[static_cast<std::size_t>(index)]; metered_ && meter >= 0)
        {
            meters[static_cast<std::size_t>(meter) * 2] += sides[0];
            meters[static_cast<std::size_t>(meter) * 2 + 1] += sides[1];
        }
    }

    const float master_target =
        (sound_control & kMasterEnable) ? static_cast<float>(Factor(sound_control & 0x7F)) : 0.0f;
    std::array<float, 2> master = {enhanced.master, enhanced.master};
    std::array<float, 2> master_targets = {enhanced.master_target, enhanced.master_target};
    std::array<float, 2> master_slopes = {enhanced.master_slope, enhanced.master_slope};
    ramp(master, master_targets, master_slopes, enhanced.master_ramp, enhanced.master_fresh,
         {master_target, master_target});
    enhanced.master = master[0];
    enhanced.master_target = master_targets[0];
    enhanced.master_slope = master_slopes[0];

    enhanced.run_waves.push_back(waves);
    enhanced.run_master.push_back(enhanced.master);
    if (metered_)
    {
        enhanced.run_meters.push_back(meters);
    }
}

void SoundUnit::FinishEnhanced(std::vector<float>& out)
{
    EnhancedState& enhanced = *enhanced_;
    const std::size_t count = enhanced.run_waves.size();

    // The rectangular waves and the noise, kDelay samples late, at their gains.
    std::vector<float> psg(2 * count, 0.0f);
    std::vector<float> levels(count);
    for (int index = 0; index < ChannelCount(); index++)
    {
        Voice& voice = enhanced.voices[static_cast<std::size_t>(index)];
        if (!voice.steps || voice.idle)
        {
            continue;
        }
        voice.steps->Read(levels.data(), count);
        const int meter = meters_[static_cast<std::size_t>(index)];
        for (std::size_t i = 0; i < count; i++)
        {
            for (std::size_t side = 0; side < 2; side++)
            {
                const float part = levels[i] * voice.run_gains[i][side];
                psg[2 * i + side] += part;
                if (metered_ && meter >= 0)
                {
                    enhanced.run_meters[i][static_cast<std::size_t>(meter) * 2 + side] += part;
                }
            }
        }
        if (voice.level == 0.0f && voice.steps->Level() == 0.0 && enhanced.samples >= voice.settled)
        {
            voice.idle = true;
        }
    }

    // The waves through the delay.
    std::array<std::vector<float>, 6> delayed;
    for (std::size_t line = 0; line < 6; line++)
    {
        std::vector<float>& held = enhanced.delayed[line];
        for (const std::array<float, 6>& waves : enhanced.run_waves)
        {
            held.push_back(waves[line]);
        }
        delayed[line].assign(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(count));
        held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(count));
    }

    // The outputs that SOUNDCNT chooses, at the master volume.
    const uint32_t sound_control = Word(kSoundControl);
    out.reserve(out.size() + 2 * count);
    for (std::size_t i = 0; i < count; i++)
    {
        for (std::size_t side = 0; side < 2; side++)
        {
            float value = delayed[side][i] + psg[2 * i + side];
            const float channel1 = delayed[2 + side][i];
            const float channel3 = delayed[4 + side][i];
            switch ((sound_control >> (8 + 2 * side)) & 3)
            {
            case 1:
                value = channel1;
                break;
            case 2:
                value = channel3;
                break;
            case 3:
                value = channel1 + channel3;
                break;
            default:
                break;
            }
            out.push_back(value * enhanced.run_master[i] * kOutputScale);
        }
    }

    // The meters, on the scale of 16-bit samples.
    if (metered_)
    {
        for (std::size_t i = 0; i < count; i++)
        {
            for (std::size_t meter = 0; meter < peaks_.size(); meter++)
            {
                for (std::size_t side = 0; side < 2; side++)
                {
                    const float level =
                        std::abs(enhanced.run_meters[i][meter * 2 + side] * enhanced.run_master[i]) / 32768.0f;
                    peaks_[meter][side] = std::max(peaks_[meter][side], static_cast<int32_t>(std::lround(level)));
                }
            }
        }
    }
}

} // namespace ntdlib::ntr
