// SPDX-License-Identifier: MIT

// The audio work of the model of the DSP's audio firmware (see firmware_audio.h).

#include "ctr/dsp/firmware_audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ctr/dsp/fcram.h"
#include "ntdlib/dsp/coef_rom.h"

namespace ntdlib::ctr::dsp::firmware
{
namespace
{

using internal::kBusChannel;

constexpr uint64_t kMask40 = (uint64_t{1} << 40) - 1;

// The value of a 40-bit accumulator that holds the low 40 bits of `value`.
int64_t Wrap40(int64_t value)
{
    return static_cast<int64_t>(static_cast<uint64_t>(value) << 24) >> 24;
}

int64_t Saturate32(int64_t value)
{
    return std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
}

int16_t Saturate16(int64_t value)
{
    return static_cast<int16_t>(
        std::clamp<int64_t>(value, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

// The high word that a move with saturation stores from an accumulator.
uint16_t High16(int64_t value)
{
    return static_cast<uint16_t>(static_cast<uint64_t>(Saturate32(value)) >> 16);
}

int32_t SignExtend32(uint32_t value)
{
    return static_cast<int32_t>(value);
}

// A logical shift of a 40-bit value, left for a positive amount.
uint64_t ShiftLogical40(uint64_t value, int amount)
{
    if (amount >= 0)
    {
        return amount >= 40 ? 0 : (value << amount) & kMask40;
    }

    return -amount >= 40 ? 0 : value >> -amount;
}

// The firmware's conversion of an IEEE float to a fixed-point number (0x35c1 and 0x35a6): the mantissa with its leading
// 1, shifted left by `left` and then by the exponent less `bias`, in a 40-bit accumulator. A negative float's sign bit
// joins the exponent. The shift then moves the mantissa out.
uint32_t FloatToFixed(uint32_t bits, int left, int bias)
{
    uint64_t value = static_cast<uint64_t>(static_cast<int64_t>(SignExtend32(bits))) & kMask40;
    value = (value & ~(uint64_t{0x1ff} << 23)) | (uint64_t{1} << 23);
    value = (value << left) & kMask40;
    const int exponent = static_cast<int>((bits >> 23) & 0x1ff);

    return static_cast<uint32_t>(ShiftLogical40(value, exponent - bias) >> 8);
}

// A gain: 1.0 gives 0x8000, and anything above 0xffff gives 0xffff.
uint16_t ToGain(uint32_t bits)
{
    return static_cast<uint16_t>(std::min<uint32_t>(FloatToFixed(bits, 8, 135), 0xffff));
}

// A volume: a gain that keeps below 0x8000.
uint16_t ToVolume(uint32_t bits)
{
    const uint16_t gain = ToGain(bits);

    return gain & 0x8000 ? 0x7fff : gain;
}

// A rate in 8.24: 0 for 0, and at least 0.1 otherwise.
uint32_t ToRate(uint32_t bits)
{
    return bits == 0 ? 0 : std::max<uint32_t>(FloatToFixed(bits, 16, 134), 0x19999);
}

// The nibbles that `samples` ADPCM samples take, with the headers of their frames (0x2958).
uint64_t Nibbles(uint32_t samples)
{
    const uint32_t rest = samples % 14;

    return uint64_t{samples / 14} * 16 + (rest ? rest + 2 : 0);
}

// The 16-bit word that holds ADPCM sample `samples`.
uint32_t AdpcmWord(uint32_t samples)
{
    return static_cast<uint32_t>(Nibbles(samples) >> 2) & 0x3fffffff;
}

// A buffer's size in 16-bit words for the check of its address (0x43e8). The firmware compares it with addresses in
// bytes.
uint32_t BufferWords(uint32_t length, uint16_t format)
{
    const int shift = static_cast<int>(format & 3) - 1;
    const auto shifted = [&](int32_t value)
    {
        return shift >= 0 ? static_cast<int32_t>(static_cast<uint32_t>(value) << shift) : value >> -shift;
    };

    switch ((format >> 2) & 3)
    {
    case 0:
        return static_cast<uint32_t>(shifted(SignExtend32(length)) >> 1) & 0x7fffffff;
    case 1:
        return static_cast<uint32_t>(shifted(SignExtend32(length)));
    case 2:
        return AdpcmWord(length);
    default:
        return 0;
    }
}

// The firmware's check of a buffer: it has to lie in the first 0x07000000 bytes of FCRAM's addresses.
bool BufferValid(uint32_t address, uint32_t words)
{
    const uint32_t end = address + words;

    return address - kFcramBase <= 0x06ffffff && end > 0x1fffffff && end <= 0x26ffffff;
}

// The bits of a source's flags that its first buffer resets: the queued buffers that wait, and the loop.
constexpr uint16_t kQueueAndLoopBits = 0x3f00;

// A source's samples after the resampler: the first channel's start, and the second channel's.
constexpr uint16_t kSamples = internal::kResampled + 4;

// The number of output samples of a frame times two, the entries of the phase table in words.
constexpr uint16_t kPhaseWords = 2 * kFrameSamples;

// The step of a gain's ramp across a frame, and of a declick's fade, in 1/65536 of the difference.
constexpr int32_t kRampStep = 409;
constexpr int32_t kDeclickStep = 412;

} // namespace

Audio::Audio(uint8_t* data, Fcram& fcram, bool enhanced)
    : data_(data), fcram_(&fcram), enhanced_(enhanced), source_meters_(kSources, -1)
{
}

void Audio::SetMeters(int count)
{
    meter_sums_.assign(static_cast<std::size_t>(std::max(count, 0)), MeterSums{});
    enhanced_meter_sums_.assign(enhanced_ ? meter_sums_.size() : 0, EnhancedMeterSums{});
    meter_peaks_.assign(meter_sums_.size(), {0, 0});
    std::fill(source_meters_.begin(), source_meters_.end(), -1);
}

void Audio::SetSourceMeter(int source, int meter)
{
    if (source >= 0 && static_cast<std::size_t>(source) < source_meters_.size())
    {
        source_meters_[static_cast<std::size_t>(source)] =
            meter >= 0 && static_cast<std::size_t>(meter) < meter_sums_.size() ? meter : -1;
    }
}

std::array<int32_t, 2> Audio::MeterPeaks(int meter) const
{
    if (meter < 0 || static_cast<std::size_t>(meter) >= meter_peaks_.size())
    {
        return {0, 0};
    }

    return meter_peaks_[static_cast<std::size_t>(meter)];
}

std::span<const int16_t> Audio::MixedFrame() const
{
    // The ring has no frame until the first mix.
    const std::span<int16_t> mixed(mixed_.data(),
                                   2 * static_cast<std::size_t>(enhanced_ ? kEnhancedFrameSamples : kFrameSamples));
    if (Word(internal::kMixedOnce) == 0)
    {
        std::fill(mixed.begin(), mixed.end(), 0);
        return mixed;
    }

    if (enhanced_)
    {
        for (std::size_t i = 0; i < mixed.size(); i++)
        {
            mixed[i] = Saturate16(std::lround(host_.frame[i] * 32768.0f));
        }

        return mixed;
    }

    const auto ring = static_cast<uint16_t>(RingFrames() * internal::kRingFrameWords);
    const uint16_t write = Word(internal::kWritePosition);
    const auto last =
        static_cast<uint16_t>(write >= internal::kRingFrameWords ? write - internal::kRingFrameWords
                                                                 : write + ring - internal::kRingFrameWords);
    for (std::size_t i = 0; i < mixed.size(); i++)
    {
        mixed[i] = Signed(internal::kRing + last + static_cast<uint32_t>(i));
    }

    return mixed;
}

uint32_t Audio::Long(uint32_t address) const
{
    return (uint32_t{Word(address)} << 16) | Word(address + 1);
}

void Audio::SetLong(uint32_t address, uint32_t value)
{
    SetWord(address, static_cast<uint16_t>(value >> 16));
    SetWord(address + 1, static_cast<uint16_t>(value));
}

uint32_t Audio::ArmLong(uint32_t address) const
{
    return Word(address) | (uint32_t{Word(address + 1)} << 16);
}

void Audio::Clear(uint32_t address, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
    {
        SetWord(address + i, 0);
    }
}

void Audio::Copy(uint32_t destination, uint32_t source, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
    {
        SetWord(destination + i, Word(source + i));
    }
}

void Audio::Load()
{
    for (int i = 0; i < kSources; i++)
    {
        const uint32_t s = SourceState(i);
        SetWord(s + state::kReport, static_cast<uint16_t>(internal::kReport + i * internal::kReportSize));
        SetWord(s + state::kVoice, static_cast<uint16_t>(internal::kVoice + i * internal::kVoiceSize));
    }

    constexpr std::array<uint16_t, 8> kPointers = {internal::kMainBus,
                                                   internal::kMainBus + kBusChannel,
                                                   internal::kMainBus + 2 * kBusChannel,
                                                   internal::kMainBus + 3 * kBusChannel,
                                                   internal::kOutput,
                                                   internal::kOutput + kBusChannel,
                                                   internal::kTwoChannelBus,
                                                   internal::kTwoChannelBus + kBusChannel};
    for (uint32_t i = 0; i < kPointers.size(); i++)
    {
        SetWord(internal::kChannelPointers + i, kPointers[i]);
    }

    // The three filters of the polyphase resampler are the first 0x600 words of the coefficient ROM of the Wii's DSP.
    for (uint32_t i = 0; i < internal::kPolyphaseWords; i++)
    {
        SetWord(internal::kPolyphase + i, ntdlib::dsp::kCoefficientRom[i]);
    }

    // The ADPCM decoder moves nibble k of a frame to the top of its word with a product.
    constexpr std::array<uint16_t, 4> kNibbleFactors = {0x0100, 0x1000, 0x0001, 0x0010};
    for (uint32_t i = 0; i < 16; i++)
    {
        SetWord(internal::kNibbleFactors + i, kNibbleFactors[i % 4]);
    }

    SetWord(internal::kDecodeStarts, internal::kDecode + 1);
    SetWord(internal::kDecodeStarts + 1, internal::kDecode + internal::kDecodeChannel + 1);
    SetWord(internal::kRingFrames, 2);
    SetWord(internal::kMute, 1);
    SetWord(internal::kMuteNext, 1);
    SetWord(internal::kRingBase, internal::kRing);
    SetWord(internal::kSurroundDepthCopy, 0x7fff);
    SetWord(internal::kSurroundSamples, kFrameSamples);
    SetWord(internal::kSurroundOn, 1);

    constexpr std::array<uint16_t, 5> kLimiter = {0xda14, 0x4000, 0x6000, 0x5555, kFrameSamples};
    for (uint32_t i = 0; i < kLimiter.size(); i++)
    {
        SetWord(internal::kLimiter + i, kLimiter[i]);
    }

    if (enhanced_)
    {
        host_ = HostSources{};
        source_meters_.assign(kSources, -1);
        GrowSources(kSources);
    }
}

void Audio::StartThread()
{
    SetWord(internal::kState, 1);
    SetWord(internal::kState + 1, 0);
    SetWord(internal::kState + 3, 0x10);
    SetWord(internal::kMixedOnce, 0);
    SetWord(internal::kMuteNext, 1);
    SetWord(internal::kApplyMute, 1);

    // The output's start (0x3373).
    SetWord(internal::kJump, 0);
    SetWord(internal::kRefills, 0);
    SetWord(internal::kRefillPhase, 0);
    SetWord(internal::kOutputOn, 1);
    SetWord(internal::kPolling, 0);

    // The structures' start (0x29db).
    SetWord(internal::kArmCounter, 3);
    SetWord(internal::kCounter, 3);
    SetWord(internal::kLastCounter, 3);
    for (int region = 0; region < 2; region++)
    {
        Clear(Shared(kMixerParams, region), kMixerParamsSize);
        Clear(Shared(kStatus, region), kStatusSize);
        Clear(Shared(kSourceParams, region), kSources * kSourceParamsSize);
        Clear(Shared(kReports, region), kSources * internal::kReportSize);
        Clear(Shared(kAdpcmCoefs, region), kSources * kAdpcmCoefsSize);
        Clear(Shared(kFinalSamples, region), kFinalSamplesSize);
        Clear(Shared(kAuxShared, region), 2 * internal::kAuxBusSize);
        Clear(Shared(kProfile, region), kProfileSize);
    }

    Clear(internal::kVoice, kSources * internal::kVoiceSize);
    SetWord(internal::kOutputMode, 1);
    SetHeadphones(false);
    SetWord(internal::kHeadphones, 0);
    if (enhanced_)
    {
        for (int region = 0; region < 2; region++)
        {
            std::fill(host_.params[region].begin(), host_.params[region].end(), 0);
            std::fill(host_.reports[region].begin(), host_.reports[region].end(), 0);
            std::fill(host_.coefs[region].begin(), host_.coefs[region].end(), 0);
        }

        for (int i = 0; i < SourceCount(); i++)
        {
            uint8_t* const voice = Block(i) + 2 * (internal::kSourceStateSize + internal::kReportSize);
            std::fill(voice, voice + 2 * internal::kVoiceSize, 0);
            host_.sources[static_cast<std::size_t>(i)] = EnhancedSource{};
        }
    }
}

void Audio::FinishStartUp()
{
    SetWord(internal::kLateFrames, 0xffff);
    SetWord(internal::kSyncMode, 1);
}

void Audio::TakeArmCounter()
{
    const int region = (Word(internal::kArmCounter) + 1) & 1;
    SetWord(internal::kArmCounter, Word(Shared(kFrameCounter, region)));
}

Audio::Refill Audio::RefillOutput(std::array<int16_t, 16>& words)
{
    Refill refill;
    if (Word(internal::kOutputOn) == 0)
    {
        return refill;
    }

    if (Word(internal::kRefillPhase) + 1 >= 20)
    {
        SetWord(internal::kRefillPeriods, Word(internal::kRefillPeriods) + 1);
        SetWord(internal::kRefillPhase, 0);
    }
    else
    {
        SetWord(internal::kRefillPhase, Word(internal::kRefillPhase) + 1);
    }

    const uint16_t read = Word(internal::kReadPosition);
    const bool muted = Word(internal::kMute) != 0;
    for (uint32_t i = 0; i < words.size(); i++)
    {
        const uint32_t address = internal::kRing + static_cast<uint16_t>(read + i);
        words[i] = muted ? 0 : static_cast<int16_t>(Word(address));
        if (!muted)
        {
            SetWord(address, 0);
        }
    }

    refill.pushed = true;
    const int16_t ring = static_cast<int16_t>(RingFrames() * internal::kRingFrameWords);
    int16_t next = static_cast<int16_t>(read + words.size());
    if (next >= ring)
    {
        next = static_cast<int16_t>(next - ring);
    }

    SetWord(internal::kReadPosition, static_cast<uint16_t>(next));
    const int16_t refills = std::min<int16_t>(static_cast<int16_t>(Signed(internal::kRefills) + 1),
                                              static_cast<int16_t>(RingFrames() * 20));
    SetWord(internal::kRefills, static_cast<uint16_t>(refills));
    if (refills > 0x13 && Word(internal::kPolling) == 0)
    {
        SetWord(internal::kRefills, static_cast<uint16_t>(refills - 20));
        if (Word(internal::kJump) != 0)
        {
            SetWord(internal::kReadPosition, Word(internal::kNextReadPosition));
            SetWord(internal::kRefills, 0);
            SetWord(internal::kRefillPhase, 0);
            SetWord(internal::kJump, 0);
        }

        if (Word(internal::kApplyMute) != 0)
        {
            SetWord(internal::kMute, Word(internal::kMuteNext));
        }

        refill.wake = true;
    }

    return refill;
}

void Audio::UpdateLate()
{
    const int16_t late_frames = Signed(internal::kLateFrames);
    SetWord(internal::kLate, static_cast<uint16_t>(std::max<int16_t>(late_frames, 0)));
    const int16_t read = Signed(internal::kReadPosition);
    const int16_t write = Signed(internal::kWritePosition);
    if (late_frames >= 0 && Word(internal::kLate) == 0 && read >= write && read - write <= 0x13f)
    {
        SetWord(internal::kLate, Word(internal::kLate) + 1);
    }
}

void Audio::TakeParams(int region)
{
    TakeMixerParams(region);
    const uint32_t mixer = Shared(kMixerParams, region);
    SetWord(mixer + mixer_params::kDirty, 0);
    SetWord(mixer + mixer_params::kDirty + 1, 0);
    SetWord(mixer + mixer_params::kDirty2, 0);
    SetWord(mixer + mixer_params::kDirty2 + 1, 0);
    if (enhanced_)
    {
        TakeHostParams(region);
        return;
    }

    for (int i = 0; i < kSources; i++)
    {
        const uint32_t params = Shared(kSourceParams + i * kSourceParamsSize, region);
        if (Word(params + params::kDirty) != 0 || Word(params + params::kDirty + 1) != 0)
        {
            TakeSourceParams(i, region);
            SetWord(params + params::kDirty, 0);
            SetWord(params + params::kDirty + 1, 0);
        }
    }
}

void Audio::FinishFrame(int region)
{
    // The firmware reaches only one region at a time above word 0x8000. Each copy to or from a region goes through a
    // buffer below it.
    Copy(internal::kMainBus, internal::kOutput, kFinalSamplesSize);
    Copy(Shared(kFinalSamples, region), internal::kMainBus, kFinalSamplesSize);
    for (int bus = 0; bus < 2; bus++)
    {
        const uint32_t own = internal::kAuxBus + bus * internal::kAuxBusSize;
        const uint32_t shared = Shared(kAuxShared + bus * internal::kAuxBusSize, region);
        for (uint32_t i = 0; i < internal::kAuxBusSize; i += 2)
        {
            SetWord(shared + i, Word(own + i + 1));
            SetWord(shared + i + 1, Word(own + i));
        }
    }

    for (int bus = 0; bus < 2; bus++)
    {
        const uint32_t own = internal::kAuxBus + bus * internal::kAuxBusSize;
        const uint32_t shared = Shared(kAuxShared + bus * internal::kAuxBusSize, 1 - region);
        for (uint32_t i = 0; i < internal::kAuxBusSize; i += 2)
        {
            SetWord(own + i, Word(shared + i + 1));
            SetWord(own + i + 1, Word(shared + i));
        }
    }

    Copy(Shared(kStatus, region), internal::kStatus, kStatusSize);
    Copy(Shared(kProfile, region), internal::kProfile, kProfileSize);
    if (enhanced_)
    {
        FinishHostFrame(region);
        return;
    }

    Copy(Shared(kReports, region), internal::kReport, kSources * internal::kReportSize);
    for (int i = 0; i < kSources; i++)
    {
        const uint32_t report = internal::kReport + i * internal::kReportSize;
        SetWord(report + report::kEnabled, Word(report + report::kEnabled) & 0x00ff);
    }
}

void Audio::StoreCounter(uint16_t counter)
{
    SetWord(Shared(kFrameCounter, counter & 1), counter);
}

void Audio::MixFrame()
{
    Clear(internal::kMainBus, 4 * kBusChannel);
    Clear(internal::kBypassBus, 2 * kBusChannel);
    Clear(internal::kTwoChannelBus, 2 * kBusChannel);
    Clear(internal::kStoppedSums, 3 * 8);
    for (int bus = 0; bus < 2; bus++)
    {
        if (Word(internal::kAuxEnable + bus) != 0)
        {
            MixAux(bus);
        }

        Clear(internal::kAuxBus + bus * internal::kAuxBusSize, internal::kAuxBusSize);
    }

    for (MeterSums& sums : meter_sums_)
    {
        for (auto& channel : sums)
        {
            channel.fill(0);
        }
    }

    // The enhanced mode mixes the sources apart from the firmware's buses. They then stay silent.
    for (int i = 0; i < (enhanced_ ? 0 : kSources); i++)
    {
        const uint32_t s = SourceState(i);
        const uint32_t report = Word(s + state::kReport);
        const uint32_t voice = Word(s + state::kVoice);
        const bool enabled = (Word(report + report::kEnabled) & 0xff) == 1;
        if (enabled)
        {
            metering_ = source_meters_[static_cast<std::size_t>(i)];
            ProcessSource(i);
            metering_ = -1;
        }
        else if (Word(s + state::kFlags) & state::kPlayed)
        {
            AddStoppedOutputs(voice);
        }

        const uint16_t flags = Word(s + state::kFlags);
        SetWord(s + state::kFlags, enabled ? flags | state::kPlayed : flags & ~state::kPlayed);
    }

    switch (Word(internal::kOutputMode))
    {
    case 0:
        Add(internal::kMainBus, internal::kBypassBus, 2 * kFrameSamples);
        MixRear();
        for (uint32_t n = 0; n < kFrameSamples; n++)
        {
            const uint32_t left = internal::kOutput + 2 * n;
            const uint32_t right = left + kBusChannel;
            const int32_t sum = static_cast<int32_t>(Long(left) + Long(right));
            SetLong(left, static_cast<uint32_t>(Scaled(sum, 0x4000)));
            SetLong(right, Long(left));
        }
        break;

    case 1:
        Add(internal::kMainBus, internal::kBypassBus, 2 * kFrameSamples);
        MixRear();
        break;

    case 2:
        MixSurround();
        Add(internal::kOutput, internal::kBypassBus, 2 * kFrameSamples);
        break;

    default:
        break;
    }

    const uint16_t declick = Word(internal::kDeclick);
    for (int bus = 0; bus < 3; bus++)
    {
        if (declick & (2 << bus))
        {
            for (uint32_t pair = 0; pair < 4; pair++)
            {
                Declick(internal::kStoppedSums + 8 * bus + 4 * pair);
            }
        }
    }

    const uint32_t ring = internal::kRing + Word(internal::kWritePosition);
    Scale(internal::kOutput, 2 * kFrameSamples, Word(internal::kMasterVolume));
    if (Word(internal::kClippingMode) == 0)
    {
        Clip(ring);
    }
    else
    {
        Limit(ring);
    }

    Copy(internal::kOutput, ring, kFinalSamplesSize);
    if (Word(internal::kMixedOnce) != 0)
    {
        const int16_t late = Signed(internal::kLateFrames);
        const int16_t periods = static_cast<int16_t>(Signed(internal::kRefillPeriods) + 1 - RingFrames());
        SetWord(internal::kLateFrames, late < 0 ? 0 : static_cast<uint16_t>(std::max<int16_t>(periods, 0)));
        SetWord(internal::kRefillPeriods, 0);
    }

    SetWord(internal::kMixedOnce, 1);
    SetWord(internal::kWritePosition, NextRingPosition(Word(internal::kWritePosition)));
    if (enhanced_)
    {
        MixEnhanced();
        return;
    }

    // The meters' peaks: the rear joins the front as MixRear joins them.
    const uint16_t ratio = Word(internal::kRearRatio);
    for (std::size_t m = 0; m < meter_sums_.size(); m++)
    {
        const MeterSums& sums = meter_sums_[m];
        std::array<int64_t, 2> peaks{};
        for (uint32_t n = 0; n < kFrameSamples; n++)
        {
            for (std::size_t c = 0; c < 2; c++)
            {
                const int64_t rear = sums[2 + c][n];
                const int64_t sum = sums[c][n] + (ratio == 0x8000 ? rear : (rear * static_cast<int16_t>(ratio)) >> 15);
                peaks[c] = std::max(peaks[c], sum < 0 ? -sum : sum);
            }
        }

        for (std::size_t c = 0; c < 2; c++)
        {
            meter_peaks_[m][c] = static_cast<int32_t>(std::min<int64_t>(peaks[c], std::numeric_limits<int32_t>::max()));
        }
    }
}

void Audio::AfterMix(bool force, bool& first)
{
    const bool changed = Word(internal::kRingFramesChanged) != 0;
    SetWord(internal::kRingFramesChanged, 0);
    if (changed || Word(internal::kLate) != 0 || first || (force && Word(internal::kSyncMode) == 1))
    {
        SetWord(internal::kNextReadPosition, NextRingPosition(Word(internal::kWritePosition)));
        SetWord(internal::kJump, 1);
        if (first)
        {
            SetWord(internal::kMuteNext, 0);
            SetWord(internal::kApplyMute, 1);
            first = false;
        }
    }
}

uint16_t Audio::RingFrames() const
{
    return Word(internal::kRingFrames);
}

uint16_t Audio::NextRingPosition(uint16_t position) const
{
    const uint16_t ring = static_cast<uint16_t>(RingFrames() * internal::kRingFrameWords);
    uint16_t next = static_cast<uint16_t>(position + internal::kRingFrameWords);
    if (static_cast<int16_t>(next) >= static_cast<int16_t>(ring))
    {
        next = static_cast<uint16_t>(next - ring);
    }

    return next;
}

void Audio::SetRingFrames(int16_t frames)
{
    const uint16_t count = static_cast<uint16_t>(std::clamp<int16_t>(frames, 2, 3));
    if (count != RingFrames())
    {
        Clear(internal::kRing, 3 * internal::kRingFrameWords);
        SetWord(internal::kRingFrames, count);
        SetWord(internal::kRingFramesChanged, 1);
    }
}

void Audio::SetHeadphones(bool on)
{
    SetWord(internal::kHeadphonesOn, on);
    Clear(internal::kHeadphoneWork, 0x400);
    Clear(internal::kHeadphoneWork + 0x400, 0x200);
    Clear(internal::kHeadphoneWork + 0x600, 0x20);
    SetWord(internal::kHeadphoneTaps, on ? 0x60 : 0x30);
    for (uint32_t i = 0; i < 4; i++)
    {
        SetWord(internal::kHeadphoneFilters + i,
                static_cast<uint16_t>(on ? internal::kSurroundFilters + 0x100 + 0x80 * i
                                         : internal::kSurroundFilters + 0x40 * i));
    }
}

void Audio::TakeMixerParams(int region)
{
    const uint32_t p = Shared(kMixerParams, region);
    const uint32_t dirty = ArmLong(p + mixer_params::kDirty);
    const auto bit = [dirty](int n)
    {
        return (dirty >> n) & 1;
    };

    if (dirty != 0)
    {
        if (bit(16))
        {
            SetWord(internal::kMasterVolume, ToVolume(ArmLong(p + mixer_params::kMasterVolume)));
        }

        if (bit(24))
        {
            SetWord(internal::kAuxReturn, ToVolume(ArmLong(p + mixer_params::kAuxReturn)));
        }

        if (bit(25))
        {
            SetWord(internal::kAuxReturn + 2, ToVolume(ArmLong(p + mixer_params::kAuxReturn + 2)));
        }

        for (int i = 0; i < 4; i++)
        {
            if (bit(6 + i))
            {
                SetWord(internal::kAuxBypass + i, Word(p + mixer_params::kAuxBypass + i));
            }
        }

        if (bit(17))
        {
            SetWord(internal::kDeclick, Word(p + mixer_params::kStatusWord));
            SetWord(internal::kStatus, Word(p + mixer_params::kStatusWord));
        }

        if (bit(26))
        {
            SetWord(internal::kOutputMode, Word(p + mixer_params::kOutputMode));
        }

        if (bit(27))
        {
            SetWord(internal::kClippingMode, Word(p + mixer_params::kClippingMode));
        }

        if (bit(28) && Word(p + mixer_params::kHeadphones) != Word(internal::kHeadphones))
        {
            SetWord(internal::kHeadphones, Word(p + mixer_params::kHeadphones));
            SetHeadphones(Word(internal::kHeadphones) == 1);
        }

        if (bit(31))
        {
            SetWord(internal::kRearRatio, Word(p + mixer_params::kRearRatio));
        }

        if (bit(29))
        {
            SetWord(internal::kSurroundDepth, Word(p + mixer_params::kSurroundDepth));
            SetWord(internal::kSurroundDepthCopy, Word(internal::kSurroundDepth));
        }

        if (bit(0))
        {
            const uint32_t source = Shared(kSurroundShared, region);
            Copy(internal::kSurroundTables, source, 0x7c);
            Copy(internal::kSurroundTables + 0x80, source + 0x7c, 0x7c);
        }

        for (int i = 0; i < 2; i++)
        {
            if (bit(1 + i))
            {
                CopySurroundFilters(Shared(kSurroundFiltersShared[i], region), i == 1);
            }
        }

        for (int i = 0; i < 2; i++)
        {
            if (bit(3 + i))
            {
                Copy(internal::kSurroundPositions + 0x10 * i, Shared(kSurroundPositionsShared[i], region), 10);
            }
        }

        if (bit(5))
        {
            SetWord(internal::kSurroundParameter, Word(p + mixer_params::kSurroundParameter));
            SetWord(internal::kSurroundOn, Word(internal::kSurroundParameter) != 0);
        }

        if (bit(15))
        {
            SetRingFrames(Signed(p + mixer_params::kOutputBuffers));
        }
    }

    if (ArmLong(p + mixer_params::kDirty2) & (1u << 16))
    {
        SetWord(internal::kSyncMode, Word(p + mixer_params::kSyncMode));
    }
}

void Audio::CopySurroundFilters(uint32_t source, bool long_filters)
{
    const uint32_t taps = long_filters ? 0x60 : 0x30;
    const uint32_t destination = internal::kSurroundFilters + (long_filters ? 0x100 : 0);
    for (uint32_t i = 0; i < 4; i++)
    {
        Copy(destination + (long_filters ? 0x80 : 0x40) * i, source + taps * i, taps);
    }
}

void Audio::TakeSourceParams(int source, int region)
{
    const uint32_t s = SourceState(source);
    const uint32_t x = Word(s + state::kReport);
    const uint32_t y = Word(s + state::kVoice);
    const uint32_t p = Shared(kSourceParams + source * kSourceParamsSize, region);
    const uint32_t dirty = ArmLong(p + params::kDirty);
    const auto bit = [dirty](int n)
    {
        return (dirty >> n) & 1;
    };

    const auto set_bits = [this](uint32_t address, uint16_t mask, uint16_t value)
    {
        SetWord(address, static_cast<uint16_t>((Word(address) & ~mask) | (value & mask)));
    };

    if (bit(29))
    {
        SetWord(s + state::kQueueDirty, 0);
        for (uint32_t k = 0; k < 4; k++)
        {
            SetLong(s + state::kQueue + kQueueEntrySize * k, 0);
        }

        SetWord(s + state::kFlags, Word(s + state::kFlags) & 0xa000);
        SetWord(x + report::kEnabled, 0);
        SetWord(x + report::kCurrentBuffer, 0);
        SetWord(x + report::kLastBuffer, 0);
        Clear(y + voice::kHistory, 8);
        Clear(y + voice::kOnePoleState, 2);
        Clear(y + voice::kBiquadInput, 12);
        history_cleared_ = true;
        filters_cleared_ = true;
    }

    if (bit(4))
    {
        SetWord(s + state::kFlags, Word(s + state::kFlags) & 0xe000);
    }

    if (bit(28))
    {
        SetWord(x + report::kSync, Word(p + params::kSync));
    }

    if (bit(0))
    {
        set_bits(s + state::kFormat, 0x000c, Word(p + params::kFormat));
    }

    if (bit(1))
    {
        set_bits(s + state::kFormat, 0x0003, Word(p + params::kFormat));
    }

    if (bit(30))
    {
        TakeFirstBuffer(s, p);
    }

    if (bit(2))
    {
        Copy(s + state::kCoefs, Shared(kAdpcmCoefs + source * kAdpcmCoefsSize, region), kAdpcmCoefsSize);
    }

    if (bit(21))
    {
        SetLong(s + state::kPhase, 0x01000000);
        SetLong(x + report::kPosition, Long(p + params::kPlayPosition));
        if (((Word(s + state::kFormat) >> 2) & 3) == 2)
        {
            const uint16_t nibble = static_cast<uint16_t>(Nibbles(Long(x + report::kPosition)) & 0xf);
            set_bits(s + state::kNibble, 0x00ff, nibble ? nibble : 2);
        }

        Clear(y + voice::kHistory, 8);
        history_cleared_ = true;
    }

    if (bit(16))
    {
        set_bits(x + report::kEnabled, 0x00ff, Word(p + params::kEnable));
    }

    for (int bus = 0; bus < 3; bus++)
    {
        if (bit(25 + bus))
        {
            for (int channel = 0; channel < 4; channel++)
            {
                SetWord(y + voice::kGain + 4 * bus + channel,
                        ToGain(ArmLong(p + params::kGain + 8 * bus + 2 * channel)));
            }
        }
    }

    if (bit(22))
    {
        SetWord(y + voice::kFilters, Word(p + params::kFilters));
    }

    if (bit(23))
    {
        Copy(y + voice::kOnePole, p + params::kOnePole, 2);
    }

    if (bit(24))
    {
        Copy(y + voice::kBiquad, p + params::kBiquad, 5);
    }

    if (bit(18))
    {
        SetLong(y + voice::kRate, ToRate(ArmLong(p + params::kRate)));
    }

    if (bit(19))
    {
        SetWord(s + state::kQueueDirty, Word(p + params::kQueueDirty));
        for (uint32_t k = 0; k < 4; k++)
        {
            const uint32_t entry = p + params::kQueue + kQueueEntrySize * k;
            if (!((Word(s + state::kQueueDirty) >> k) & 1) ||
                !BufferValid(Long(entry + queue::kAddress),
                             BufferWords(Long(entry + queue::kLength), Word(s + state::kFormat))))
            {
                continue;
            }

            Copy(s + state::kQueue + kQueueEntrySize * k, entry, kQueueEntrySize);
            const uint16_t valid = static_cast<uint16_t>(((Word(s + state::kFlags) >> 8) | (1u << k)) & 0x1f);
            set_bits(s + state::kFlags, 0x1f00, static_cast<uint16_t>(valid << 8));
        }

        SetWord(p + params::kQueueDirty, 0);
    }

    if (bit(3) && Word(x + report::kCurrentBuffer) == Word(p + params::kBufferId))
    {
        set_bits(s + state::kFlags, state::kLooping, static_cast<uint16_t>(Word(p + params::kBufferFlags) << 12));
        SetLong(s + state::kLength, Long(p + params::kLength));
    }

    if (bit(17))
    {
        SetWord(y + voice::kInterpolation, Word(p + params::kInterpolation));
    }

    Copy(p + params::kReport, x, internal::kReportSize);
}

void Audio::TakeFirstBuffer(uint32_t s, uint32_t p)
{
    const uint32_t address = Long(p + params::kAddress);
    const uint16_t format = Word(p + params::kFormat);
    if (!BufferValid(address, BufferWords(Long(p + params::kLength), format)))
    {
        return;
    }

    const uint32_t x = Word(s + state::kReport);
    const uint32_t y = Word(s + state::kVoice);
    SetWord(s + state::kFormat, format);
    Clear(y + voice::kHistory, 8);
    history_cleared_ = true;
    SetLong(s + state::kEnd, Long(p + params::kLength));
    SetLong(s + state::kLength, Long(p + params::kLength));
    if (((format >> 2) & 3) == 2 && (Word(p + params::kBufferFlags) & 1))
    {
        for (uint32_t i = 0; i < 3; i++)
        {
            SetWord(s + state::kLoopAdpcm + i, Word(p + params::kAdpcm + i));
            SetWord(s + state::kAdpcm + i, Word(p + params::kAdpcm + i));
        }

        SetWord(s + state::kFlags, Word(s + state::kFlags) & ~state::kNoLoopContext);
    }

    SetLong(s + state::kAddress, address);
    for (uint32_t k = 0; k < 4; k++)
    {
        SetLong(s + state::kQueue + kQueueEntrySize * k, 0);
    }

    SetWord(s + state::kFlags, Word(s + state::kFlags) & ~kQueueAndLoopBits);
    SetWord(x + report::kCurrentBuffer, Word(p + params::kBufferId));
    SetWord(s + state::kFlags, static_cast<uint16_t>(Word(s + state::kFlags) |
                                                     ((Word(p + params::kBufferFlags) << 12) & state::kLooping)));
}

void Audio::ProcessSource(int source)
{
    const uint32_t s = SourceState(source);
    std::array<uint16_t, 3> counts = {FillPhases(s), 0, 0};
    if (counts[0] == 0)
    {
        return;
    }

    const auto resample = [&](uint16_t /*decoded*/)
    {
        if (counts[1] != 0)
        {
            Resample(s, kSamples + counts[2] / 2, internal::kPhases + counts[2], counts[1] / 2);
        }
    };
    if (!DecodeFrame(s, counts, resample))
    {
        return;
    }

    const uint32_t samples = Filter(s);
    for (int bus = 0; bus < 3; bus++)
    {
        Mix(s, samples, bus);
    }
}

bool Audio::DecodeFrame(uint32_t s, std::array<uint16_t, 3>& counts, const std::function<void(uint16_t)>& chunk)
{
    const uint32_t x = Word(s + state::kReport);
    const uint32_t y = Word(s + state::kVoice);
    while (counts[0] != 0 && (Word(x + report::kEnabled) & 0xff) != 0)
    {
        Copy(internal::kDecode, y + voice::kHistory, 4);
        Copy(internal::kDecode + internal::kDecodeChannel, y + voice::kHistory + 4, 4);
        if (((Word(s + state::kFormat) >> 2) & 3) == 3)
        {
            // The firmware never comes back from a buffer of the fourth encoding. The model skips the source.
            return false;
        }

        const uint16_t decoded = Decode(s, counts);
        EndIfDone(s);
        chunk(decoded);
        counts[2] = static_cast<uint16_t>(counts[2] + counts[1]);
        Copy(y + voice::kHistory, internal::kDecode + decoded, 4);
        Copy(y + voice::kHistory + 4, internal::kDecode + internal::kDecodeChannel + decoded, 4);
    }

    return true;
}

void Audio::EndIfDone(uint32_t s)
{
    const uint32_t x = Word(s + state::kReport);
    const uint16_t index = Word(s + state::kFlags) & state::kQueueIndexMask;
    if (!(Word(s + state::kFlags) & state::kLooping) &&
        !((Word(s + state::kFlags) >> (state::kQueueValidShift + index)) & 1) &&
        Long(x + report::kPosition) >= Long(s + state::kEnd))
    {
        SetWord(x + report::kEnabled, 0);
        SetWord(x + report::kLastBuffer, Word(x + report::kCurrentBuffer));
        SetWord(x + report::kCurrentBuffer, 0);
        SetWord(x + report::kEnabled, 0x100);
    }
}

uint16_t Audio::FillPhases(uint32_t s)
{
    const uint32_t y = Word(s + state::kVoice);
    const int64_t rate = Long(y + voice::kRate);
    int64_t phase = SignExtend32(Long(s + state::kPhase));
    int64_t sum = 0;
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        const int64_t entry = Wrap40(phase) >> 8;
        SetLong(internal::kPhases + 2 * n, static_cast<uint32_t>(entry));
        phase = Wrap40((Wrap40(phase) & 0xffffff) + rate);
        sum += static_cast<int16_t>(static_cast<uint16_t>(entry >> 16));
    }

    SetWord(internal::kPhaseSum, static_cast<uint16_t>(sum));
    SetLong(s + state::kPhase, static_cast<uint32_t>(phase));

    return Word(internal::kPhaseSum);
}

uint16_t Audio::Chunk(uint16_t limit, std::array<uint16_t, 3>& counts) const
{
    uint16_t& left = counts[0];
    uint16_t& outputs = counts[1];
    const uint16_t offset = counts[2];
    uint16_t take;
    if (limit > left)
    {
        outputs = static_cast<uint16_t>(kPhaseWords - offset);
        take = left;
    }
    else
    {
        uint32_t kept = static_cast<uint16_t>(kPhaseWords - offset) / 2;
        int64_t sum = 0;
        for (uint32_t j = 0; j < kept; j++)
        {
            sum += Signed(internal::kPhases + offset + 2 * j);
        }

        bool popped = false;
        while (static_cast<uint16_t>(sum) > limit)
        {
            kept--;
            sum -= Signed(internal::kPhases + offset + 2 * kept);
            popped = true;
        }

        outputs = static_cast<uint16_t>(2 * kept);
        take = popped ? static_cast<uint16_t>(sum) : limit;
        take = std::min(take, left);
    }

    take = std::max<uint16_t>(take, 1);
    left = static_cast<uint16_t>(left - take);

    return take;
}

uint16_t Audio::Decode(uint32_t s, std::array<uint16_t, 3>& counts)
{
    const uint16_t format = Word(s + state::kFormat);
    const bool stereo = (format & 3) == 2;
    switch ((format >> 2) & 3)
    {
    case 0:
        {
            const uint16_t take = Chunk(stereo ? 0x400 : 0x800, counts);
            DecodePcm(s, take, stereo, true);
            return take;
        }

    case 1:
        {
            const uint16_t take = Chunk(stereo ? 0x200 : 0x400, counts);
            DecodePcm(s, take, stereo, false);
            return take;
        }

    case 2:
        {
            const uint16_t take = Chunk(0xe00, counts);
            DecodeAdpcm(s, take, stereo);
            return take;
        }

    default:
        return 0;
    }
}

void Audio::DecodePcm(uint32_t s, uint16_t count, bool stereo, bool pcm8)
{
    const uint32_t x = Word(s + state::kReport);
    uint32_t out0 = internal::kDecode + 4;
    uint32_t out1 = internal::kDecode + internal::kDecodeChannel + 4;
    uint32_t left = count;
    do
    {
        SetLong(s + state::kEnd, Long(s + state::kLength));
        const uint32_t position = Long(x + report::kPosition);
        const uint32_t end = Long(s + state::kEnd);
        if (position < end)
        {
            const uint32_t n = std::min(end - position, left);
            const uint32_t address = Long(s + state::kAddress);
            if (pcm8)
            {
                const uint32_t m = position & (stereo ? 0x1f : 0x3f);
                Dma(address + ((position - m) << (stereo ? 1 : 0)),
                    (((((m + n + 1) & ~1u) >> (stereo ? 0 : 1)) + 15) & ~15u));
                if (stereo)
                {
                    for (uint32_t i = 0; i < n; i++)
                    {
                        const uint16_t word = Word(internal::kDmaBuffer + m + i);
                        SetWord(out0 + i, static_cast<uint16_t>(word << 8));
                        SetWord(out1 + i, word & 0xff00);
                    }
                }
                else
                {
                    uint32_t input = internal::kDmaBuffer + m / 2;
                    uint32_t i = 0;
                    if (position & 1)
                    {
                        SetWord(out0 + i++, Word(input++) & 0xff00);
                    }

                    for (; i + 1 < n; i += 2)
                    {
                        const uint16_t word = Word(input++);
                        SetWord(out0 + i, static_cast<uint16_t>(word << 8));
                        SetWord(out0 + i + 1, word & 0xff00);
                    }

                    if (i < n)
                    {
                        SetWord(out0 + i, static_cast<uint16_t>(Word(input) << 8));
                    }
                }
            }
            else
            {
                const uint32_t m = position & (stereo ? 0x7 : 0xf);
                Dma(address + ((position - m) << (stereo ? 2 : 1)), ((((m + n) << (stereo ? 1 : 0)) + 15) & ~15u));
                if (stereo)
                {
                    for (uint32_t i = 0; i < n; i++)
                    {
                        SetWord(out0 + i, Word(internal::kDmaBuffer + 2 * (m + i)));
                        SetWord(out1 + i, Word(internal::kDmaBuffer + 2 * (m + i) + 1));
                    }
                }
                else
                {
                    Copy(out0, internal::kDmaBuffer + m, n);
                }
            }

            left -= n;
            SetLong(x + report::kPosition, position + n);
            out0 += n;
            out1 += n;
        }

        // The buffer ends as soon as the position reaches its end.
        if (Long(x + report::kPosition) < end)
        {
            continue;
        }

        if (Word(s + state::kFlags) & state::kLooping)
        {
            SetLong(x + report::kPosition, 0);
        }
        else if (!NextBuffer(s, false))
        {
            Clear(out0, left);
            Clear(out1, left);
            break;
        }
    } while (left != 0);
}

void Audio::DecodeAdpcm(uint32_t s, uint16_t count, bool stereo)
{
    const uint32_t x = Word(s + state::kReport);
    uint32_t out0 = internal::kDecode + 4;
    uint16_t nibble = Word(s + state::kNibble) & 0xff;
    uint32_t left = count;
    do
    {
        SetLong(s + state::kEnd, Long(s + state::kLength));
        const uint32_t position = Long(x + report::kPosition);
        const uint32_t end = Long(s + state::kEnd);
        if (position < end)
        {
            const uint32_t word = AdpcmWord(position);
            const uint32_t n = std::min(end - position, left);
            Dma(Long(s + state::kAddress) + 2 * (word & ~0xfu),
                ((static_cast<uint32_t>(Nibbles(n) >> 2) + 0x2f) & ~0xfu));
            if (!stereo)
            {
                SetWord(out0 - 2, Word(s + state::kAdpcm + 2));
                SetWord(out0 - 1, Word(s + state::kAdpcm + 1));
                SetWord(internal::kAdpcmPosition, nibble);
                SetWord(internal::kAdpcmHeader, Word(s + state::kAdpcm));
                DecodeAdpcmFrames(s, internal::kDmaBuffer + (word & 0xc), out0, static_cast<uint16_t>(n));
                SetWord(s + state::kAdpcm, Word(internal::kAdpcmHeader));
                SetWord(s + state::kAdpcm + 2, Word(out0 + n - 2));
                SetWord(s + state::kAdpcm + 1, Word(out0 + n - 1));
                nibble = Word(internal::kAdpcmPosition);
                out0 += n;
            }

            SetLong(x + report::kPosition, position + n);
            left -= n;
        }

        // The buffer ends as soon as the position reaches its end.
        if (Long(x + report::kPosition) < end)
        {
            continue;
        }

        if (Word(s + state::kFlags) & state::kLooping)
        {
            SetLong(x + report::kPosition, 0);
            nibble = std::max<uint16_t>(Word(s + state::kNibble) >> 8, 2);
            if (!(Word(s + state::kFlags) & state::kNoLoopContext))
            {
                Copy(s + state::kAdpcm, s + state::kLoopAdpcm, 3);
            }
        }
        else if (NextBuffer(s, true))
        {
            nibble = 2;
        }
        else
        {
            Clear(out0, left);
            Clear(internal::kDecode + internal::kDecodeChannel + 4, left);
            break;
        }
    } while (left != 0);

    SetWord(s + state::kNibble, static_cast<uint16_t>((Word(s + state::kNibble) & 0xff00) | (nibble & 0xff)));
}

void Audio::DecodeAdpcmFrames(uint32_t s, uint32_t input, uint32_t output, uint16_t count)
{
    uint16_t nibble = Word(internal::kAdpcmPosition);
    uint32_t remaining = count;
    while (remaining != 0)
    {
        const uint16_t header = Word(input) & 0xff;
        const int predictor = header >> 4;
        const int scale = header & 0xf;
        for (uint32_t i = 0; i < 4; i++)
        {
            // The frame's words go to the frame's copy by nibble position: the header's word holds nibbles 2 and 3.
            const uint32_t first = i == 0 ? 2 : 4 * i;
            for (uint32_t p = first; p < 4 * i + 4; p++)
            {
                SetWord(internal::kAdpcmFrame + p, Word(input + i));
            }
        }

        input += 4;
        const uint32_t steps = std::min<uint32_t>(remaining, 16u - nibble);
        const int16_t c0 = Signed(s + state::kCoefs + 2 * predictor);
        const int16_t c1 = Signed(s + state::kCoefs + 2 * predictor + 1);
        for (uint32_t k = 0; k < steps; k++)
        {
            const uint32_t p = nibble + k;
            const uint16_t top =
                static_cast<uint16_t>(Word(internal::kNibbleFactors + p) * Word(internal::kAdpcmFrame + p)) & 0xf000;
            const int64_t value = static_cast<int16_t>(top);
            const int64_t scaled = scale == 0 ? value >> 1 : value * (int64_t{1} << (scale - 1));
            const int64_t sum = scaled + c1 * int64_t{Signed(output - 2)} + c0 * int64_t{Signed(output - 1)};
            SetWord(output, High16(Wrap40(Wrap40(sum * 32) + 0x8000)));
            output++;
        }

        remaining -= steps;
        nibble = static_cast<uint16_t>(nibble + steps);
        if (nibble >= 16)
        {
            nibble = 2;
            SetWord(internal::kAdpcmHeader, Word(input) & 0xff);
        }
    }

    SetWord(internal::kAdpcmPosition, nibble);
}

bool Audio::NextBuffer(uint32_t s, bool adpcm)
{
    const uint32_t x = Word(s + state::kReport);
    const uint16_t flags = Word(s + state::kFlags);
    const uint16_t index = flags & state::kQueueIndexMask;
    if (!((flags >> (state::kQueueValidShift + index)) & 1))
    {
        return false;
    }

    const uint32_t slot = s + state::kQueue + kQueueEntrySize * index;
    SetLong(s + state::kAddress, Long(slot + queue::kAddress));
    SetWord(x + report::kCurrentBuffer, Word(slot + queue::kId));
    SetLong(x + report::kPosition, 0);
    SetLong(s + state::kEnd, Long(slot + queue::kLength));
    SetLong(s + state::kLength, Long(slot + queue::kLength));
    uint16_t next = static_cast<uint16_t>((Word(s + state::kFlags) & ~state::kLooping) |
                                          ((Word(slot + queue::kFlags) << 5) & state::kLooping));
    next = static_cast<uint16_t>(next & ~(1u << (state::kQueueValidShift + index)));
    SetWord(s + state::kFlags, next);
    SetWord(x + report::kEnabled, static_cast<uint16_t>((Word(x + report::kEnabled) & 0xff) | 0x100));
    SetWord(s + state::kFlags,
            static_cast<uint16_t>((Word(s + state::kFlags) & ~state::kQueueIndexMask) | ((index + 1) & 3)));
    if (adpcm)
    {
        if (Word(slot + queue::kFlags) & 0xff)
        {
            SetWord(s + state::kAdpcm + 1, Word(slot + queue::kAdpcm + 1));
            SetWord(s + state::kLoopAdpcm + 1, Word(slot + queue::kAdpcm + 1));
            SetWord(s + state::kAdpcm + 2, Word(slot + queue::kAdpcm + 2));
            SetWord(s + state::kLoopAdpcm + 2, Word(slot + queue::kAdpcm + 2));
            SetWord(s + state::kAdpcm, Word(slot + queue::kAdpcm));
            SetWord(s + state::kLoopAdpcm, Word(slot + queue::kAdpcm));
            SetWord(s + state::kFlags, Word(s + state::kFlags) & ~state::kNoLoopContext);
        }
        else
        {
            SetWord(s + state::kFlags, Word(s + state::kFlags) | state::kNoLoopContext);
        }
    }

    return true;
}

void Audio::Dma(uint32_t source, uint32_t words)
{
    // The DMA reads FCRAM in 32-bit units from the 4-byte boundary below the address. A unit outside FCRAM reads 0.
    const uint32_t base = source & ~3u;
    for (uint32_t k = 0; k < words; k++)
    {
        const uint8_t* unit = fcram_->At(base + 4 * (k / 2), 4);
        SetWord(internal::kDmaBuffer + k, unit ? xsflib::LoadLe16(unit + 2 * (k % 2)) : 0);
    }
}

void Audio::Resample(uint32_t s, uint32_t output, uint32_t phases, uint16_t count)
{
    const uint32_t y = Word(s + state::kVoice);
    const uint16_t interpolation = Word(y + voice::kInterpolation);
    const bool stereo = (Word(s + state::kFormat) & 3) == 2;
    const int channels = stereo ? 2 : 1;
    for (int c = 0; c < channels; c++)
    {
        const uint32_t base = internal::kDecode + c * internal::kDecodeChannel;
        const uint32_t out = output + c * internal::kResampledChannel;
        int32_t position = 0;
        for (uint32_t n = 0; n < count; n++)
        {
            position += Signed(phases + 2 * n);
            const uint16_t fraction = Word(phases + 2 * n + 1);
            const auto sample = [&](int32_t i)
            {
                return int64_t{Signed(static_cast<uint16_t>(base + position + i))};
            };

            uint16_t value;
            switch (interpolation & 0xff)
            {
            case 0:
                {
                    const uint32_t table =
                        static_cast<uint16_t>(internal::kPolyphase + ((interpolation << 1) & 0xfe00));
                    const uint32_t row = table + (fraction >> 9) * 4;
                    int64_t sum = 0;
                    for (int t = 0; t < 4; t++)
                    {
                        sum += 2 * int64_t{Signed(static_cast<uint16_t>(row + t))} * sample(t);
                    }

                    value = High16(Wrap40(sum));
                    break;
                }

            case 1:
                {
                    const int64_t s0 = sample(1);
                    const int64_t difference = Saturate16(sample(2) - s0);
                    value = High16(Wrap40(s0 * 65536 + difference * fraction));
                    break;
                }

            default:
                value = static_cast<uint16_t>(sample(1));
                break;
            }

            SetWord(out + n, value);
            if (!stereo)
            {
                SetWord(out + n + internal::kResampledChannel, value);
            }
        }
    }
}

uint32_t Audio::Filter(uint32_t s)
{
    const uint32_t y = Word(s + state::kVoice);
    const uint16_t filters = Word(y + voice::kFilters);
    const bool stereo = (Word(s + state::kFormat) & 3) == 2;
    uint32_t samples = kSamples;
    if (filters & 2)
    {
        Biquad(y, 0, internal::kResampled);
        if (stereo)
        {
            Biquad(y, 1, internal::kResampled + internal::kResampledChannel);
        }
        else
        {
            Copy(internal::kResampled + 2 + internal::kResampledChannel, internal::kResampled + 2, kFrameSamples);
        }

        samples = internal::kResampled + 2;
    }

    if (filters & 1)
    {
        OnePole(y, 0, samples);
        if (stereo)
        {
            OnePole(y, 1, samples + internal::kResampledChannel);
        }
        else
        {
            Copy(samples + internal::kResampledChannel, samples, kFrameSamples);
        }
    }

    if (filters == 0 && !stereo)
    {
        Copy(samples + internal::kResampledChannel, samples, kFrameSamples);
    }

    return samples;
}

void Audio::Biquad(uint32_t y, int channel, uint32_t buffer)
{
    // The samples start at buffer + 4, after the last two inputs of the previous frame. Each output goes two words
    // back, in place of the input two samples before it.
    const uint32_t input = y + voice::kBiquadInput + 2 * channel;
    const uint32_t state = y + voice::kBiquadState + 4 * channel;
    SetWord(buffer + 2, Word(input));
    SetWord(buffer + 3, Word(input + 1));
    std::array<int64_t, 5> c;
    for (uint32_t i = 0; i < c.size(); i++)
    {
        c[i] = Signed(y + voice::kBiquad + i);
    }

    int64_t s0 = SignExtend32(Long(state));
    int64_t s1 = SignExtend32(Long(state + 2));
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        const uint32_t x = buffer + 4 + n;
        const int64_t sum = c[0] * s0 + c[1] * s1 + c[2] * Signed(x - 2) + c[3] * Signed(x - 1) + c[4] * Signed(x);
        const int64_t acc = Wrap40(sum * 4);
        SetWord(x - 2, High16(acc));
        s0 = s1;
        s1 = Saturate32(acc >> 2) >> 14;
    }

    SetLong(state, static_cast<uint32_t>(s0));
    SetLong(state + 2, static_cast<uint32_t>(s1));
    SetWord(input, Word(buffer + 4 + kFrameSamples - 2));
    SetWord(input + 1, Word(buffer + 4 + kFrameSamples - 1));
}

void Audio::OnePole(uint32_t y, int channel, uint32_t samples)
{
    const int64_t b0 = Signed(y + voice::kOnePole);
    const int64_t a1 = Signed(y + voice::kOnePole + 1);
    int64_t last = Signed(y + voice::kOnePoleState + channel);
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        const uint16_t value = High16(Wrap40(2 * (a1 * last + b0 * Signed(samples + n))));
        SetWord(samples + n, value);
        last = static_cast<int16_t>(value);
    }

    SetWord(y + voice::kOnePoleState + channel, static_cast<uint16_t>(last));
}

void Audio::Mix(uint32_t s, uint32_t samples, int bus)
{
    const uint32_t y = Word(s + state::kVoice);
    const uint16_t format = Word(s + state::kFormat);
    const bool two_channel = format & 0x40;
    if (two_channel && bus != 0)
    {
        return;
    }

    const bool ramp = (Word(s + state::kFlags) & state::kPlayed) || (format & 0x20);
    const uint32_t gains = y + voice::kGain + 4 * bus;
    const uint32_t previous = y + voice::kPreviousGain + 4 * bus;
    const uint32_t last = y + voice::kLastOutput + 4 * bus;
    const uint32_t bus_start = bus == 0 ? internal::kMainBus : internal::kAuxBus + (bus - 1) * internal::kAuxBusSize;
    for (int pair = 0; pair < 2; pair++)
    {
        if (two_channel && pair == 1)
        {
            SetWord(last + 2, 0);
            SetWord(last + 3, 0);
            break;
        }

        const uint32_t g = gains + 2 * pair;
        const uint32_t pg = previous + 2 * pair;
        if (Word(g) == 0 && Word(g + 1) == 0 && Word(pg) == 0 && Word(pg + 1) == 0)
        {
            SetWord(last + 2 * pair, 0);
            SetWord(last + 2 * pair + 1, 0);
            continue;
        }

        uint32_t destination = bus_start + 2 * kBusChannel * pair;
        if (bus == 0 && pair == 0 && (format & 0x10))
        {
            destination = internal::kBypassBus;
        }
        else if (two_channel)
        {
            destination = internal::kTwoChannelBus;
        }

        const bool ramped = ramp && (Word(g) != Word(pg) || Word(g + 1) != Word(pg + 1));
        for (int c = 0; c < 2; c++)
        {
            const uint32_t source = samples + c * internal::kResampledChannel;
            const uint32_t out = destination + c * kBusChannel;
            const uint16_t gain = Word(g + c);
            const int64_t start = int64_t{Word(pg + c)} * 65536;
            const int64_t step = kRampStep * int64_t{static_cast<int16_t>(gain - Word(pg + c))};
            int64_t contribution = 0;
            std::array<int64_t, kFrameSamples>* meter =
                bus == 0 && metering_ >= 0
                    ? &meter_sums_[static_cast<std::size_t>(metering_)][static_cast<std::size_t>(2 * pair + c)]
                    : nullptr;
            for (uint32_t n = 0; n < kFrameSamples; n++)
            {
                const uint16_t factor = ramped ? static_cast<uint16_t>((start + n * step) >> 16) : gain;
                contribution = (int64_t{Signed(source + n)} * factor) >> 15;
                SetLong(out + 2 * n, static_cast<uint32_t>(SignExtend32(Long(out + 2 * n)) + contribution));
                if (meter != nullptr)
                {
                    (*meter)[n] += contribution;
                }
            }

            SetWord(last + 2 * pair + c, static_cast<uint16_t>(contribution));
        }
    }

    Copy(previous, gains, 4);
}

void Audio::AddStoppedOutputs(uint32_t y)
{
    for (uint32_t bus = 0; bus < 3; bus++)
    {
        for (uint32_t c = 0; c < 4; c++)
        {
            const uint32_t sum = internal::kStoppedSums + 8 * bus + 2 * c;
            SetLong(sum, static_cast<uint32_t>(SignExtend32(Long(sum)) + Signed(y + voice::kLastOutput + 4 * bus + c)));
        }
    }
}

void Audio::MixAux(int bus)
{
    const int16_t volume = Signed(internal::kAuxReturn + 2 * bus);
    if (volume <= 0)
    {
        return;
    }

    const uint32_t aux = internal::kAuxBus + bus * internal::kAuxBusSize;
    Scale(aux, 4 * kFrameSamples, static_cast<uint16_t>(volume));
    if (Word(internal::kAuxBypass + bus) != 0)
    {
        Add(internal::kBypassBus, aux, 2 * kFrameSamples);
        Add(internal::kMainBus + 2 * kBusChannel, aux + 2 * kBusChannel, 2 * kFrameSamples);
    }
    else
    {
        Add(internal::kMainBus, aux, 4 * kFrameSamples);
    }
}

void Audio::Declick(uint32_t sums)
{
    for (int c = 0; c < 2; c++)
    {
        const uint32_t sum = sums + 2 * c;
        const int32_t start = SignExtend32(Long(sum));
        const int32_t step = kDeclickStep * static_cast<int16_t>(start >> 16) +
                             static_cast<int32_t>((uint32_t{static_cast<uint16_t>(start)} * kDeclickStep) >> 16);
        const uint32_t buffer = internal::kDeclickBuffer + c * kBusChannel;
        int32_t value = start;
        for (uint32_t n = 0; n < kFrameSamples; n++)
        {
            SetLong(buffer + 2 * n, static_cast<uint32_t>(value));
            value = static_cast<int32_t>(static_cast<uint32_t>(value) - static_cast<uint32_t>(step));
        }
    }

    Add(internal::kOutput, internal::kDeclickBuffer, 2 * kFrameSamples);
    SetLong(sums, Long(internal::kDeclickBuffer + 2 * (kFrameSamples - 1)));
    SetLong(sums + 2, Long(internal::kDeclickBuffer + kBusChannel + 2 * (kFrameSamples - 1)));
}

int32_t Audio::Scaled(int32_t value, uint16_t factor)
{
    return static_cast<int32_t>((int64_t{static_cast<int16_t>(factor)} * value) >> 15);
}

void Audio::Scale(uint32_t address, uint32_t dwords, uint16_t factor)
{
    for (uint32_t i = 0; i < dwords; i++)
    {
        SetLong(address + 2 * i, static_cast<uint32_t>(Scaled(SignExtend32(Long(address + 2 * i)), factor)));
    }
}

void Audio::Add(uint32_t destination, uint32_t source, uint32_t dwords)
{
    for (uint32_t i = 0; i < dwords; i++)
    {
        SetLong(destination + 2 * i, Long(destination + 2 * i) + Long(source + 2 * i));
    }
}

void Audio::MixRear()
{
    const uint16_t ratio = Word(internal::kRearRatio);
    const uint32_t front = Word(internal::kChannelPointers);
    const uint32_t rear = Word(internal::kChannelPointers + 2);
    if (ratio != 0x8000)
    {
        Scale(rear, 2 * kFrameSamples, ratio);
    }

    const uint32_t output = Word(internal::kChannelPointers + 4);
    for (uint32_t i = 0; i < 2 * kFrameSamples; i++)
    {
        SetLong(output + 2 * i, Long(front + 2 * i) + Long(rear + 2 * i));
    }
}

void Audio::MixSurround()
{
    // The surround mix (0x545f) isn't modelled: the model mixes the output as in stereo.
    MixRear();
}

void Audio::Clip(uint32_t ring)
{
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        for (uint32_t c = 0; c < 2; c++)
        {
            const uint32_t value = Long(internal::kOutput + c * kBusChannel + 2 * n);
            const int32_t low24 = static_cast<int32_t>(value << 8) >> 8;
            SetWord(ring + 2 * n + c, static_cast<uint16_t>(Saturate16(low24)));
        }
    }
}

void Audio::Limit(uint32_t ring)
{
    const int64_t bias = Signed(internal::kLimiter);
    const int64_t offset = Word(internal::kLimiter + 1);
    const int64_t gain = Signed(internal::kLimiter + 2);
    const int64_t input_gain = Signed(internal::kLimiter + 3);
    for (uint32_t n = 0; n < kFrameSamples; n++)
    {
        for (uint32_t c = 0; c < 2; c++)
        {
            const int64_t value = SignExtend32(Long(internal::kOutput + c * kBusChannel + 2 * n));
            const int64_t u = (value * input_gain) >> 15;
            const int64_t x = static_cast<int16_t>(static_cast<uint32_t>(Saturate32(u * 65536)) >> 16);
            const int64_t q = static_cast<int16_t>(static_cast<uint32_t>(2 * x * gain) >> 16);
            const int64_t r = static_cast<int16_t>(static_cast<uint32_t>(2 * q * q) >> 16);
            const int64_t t = static_cast<int16_t>(static_cast<uint32_t>(2 * r * bias) >> 16);
            const int64_t g = static_cast<uint16_t>(t + offset);
            SetWord(ring + 2 * n + c, static_cast<uint16_t>(static_cast<uint32_t>(Saturate32(4 * (2 * g * q))) >> 16));
        }
    }
}

uint32_t Audio::SourceState(int source)
{
    return internal::kSourceState + source * internal::kSourceStateSize;
}

} // namespace ntdlib::ctr::dsp::firmware
