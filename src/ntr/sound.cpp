// SPDX-License-Identifier: MIT

// The DS's sound unit: the registers, the channels, the captures and the mixer (see sound.h).

#include "sound_internal.h"

namespace ntdlib::ntr
{

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

    // A write that reaches the high byte of a channel's SOUNDxCNT starts or stops the channel.
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

SoundUnit::EnhancedState::EnhancedState() = default;

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

bool SoundUnit::CapturesRun() const
{
    return captures_[0].busy || captures_[1].busy;
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
    if (address < kMainMemory || address >= kMainMemoryAreaEnd || main_memory_.empty())
    {
        return 0;
    }

    return main_memory_[(address - kMainMemory) % main_memory_.size()];
}

uint32_t SoundUnit::MemoryWord(uint32_t address) const
{
    // A word that is whole in main memory, after the mirroring, takes one look-up.
    if (address >= kMainMemory && address <= kMainMemoryAreaEnd - 4 && !main_memory_.empty())
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
    if (address < kMainMemory || address >= kMainMemoryAreaEnd || main_memory_.empty())
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
    // IMA-ADPCM loop never returns to the first word: that word holds the decoder's state.
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

} // namespace ntdlib::ntr
