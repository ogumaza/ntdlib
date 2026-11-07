// SPDX-License-Identifier: MIT

#include "ntdlib/ubimilan/archive_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ntdlib::ubimilan
{
namespace
{

// The frames that Busy() waits for after the last sound: the FIFOs' lead lasts less than a frame.
constexpr int kTailFrames = 2;

// A byte of the mixer's in the output's 10 bits, at the FIFOs' full volume that the game's start sets, and a step of
// the 10 bits in a 16-bit sample.
constexpr float kFifoGain = 4.0f;
constexpr float kOutputStep = 64.0f;

// The hardware's channels of each track that plays one.
constexpr int kWaveTrack = 2;
constexpr int kNoiseChannel = 3;

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    if (options.store_cycle >= agb::kFrameCycles)
    {
        throw std::invalid_argument("the stores' cycle lies past the frame");
    }

    Output output(rom_, info_, song);
    if (!output.Valid())
    {
        throw std::invalid_argument("song " + std::to_string(song) + " can't be played");
    }

    agb::Apu apu;
    for (const agb::RegisterWrite& write : output.Song().InitWrites())
    {
        apu.Store(write.address, write.size, write.value);
    }

    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);

    // Up to the first stores, the FIFOs play the silence that the game's start left.
    const uint32_t period = OutputCycles(info_);
    apu.SetTimer(0, period, period);
    const std::vector<int8_t> silence(size_t(options.store_cycle / period + kFifoLeadPoints), 0);
    apu.Queue(0, silence);
    apu.Queue(1, silence);
    if (options.enhanced)
    {
        output.StartEnhancedMixing();
        apu.StartEnhanced();
        const uint64_t lead_cycles = uint64_t(kFifoLeadPoints) * period;
        const uint64_t samples =
            ((options.store_cycle + lead_cycles) * agb::kEnhancedFrameSamples + agb::kFrameCycles) / agb::kFrameCycles;
        const std::vector<float> enhanced_silence(size_t(samples), 0.0f);
        apu.QueueEnhanced(0, enhanced_silence);
        apu.QueueEnhanced(1, enhanced_silence);
    }

    output_.emplace(std::move(output));
    apu_ = std::move(apu);
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    tail_ = kTailFrames;
    enhanced_ = options.enhanced;
    peaks_ = {};
    output_peaks_ = {};
    last_voices_peak_ = 0.0f;
}

const Output& ArchivePlayer::Driver() const
{
    if (!output_)
    {
        throw std::logic_error("no song has started");
    }

    return *output_;
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the song plays in the enhanced mode");
    }

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (output_ && !enhanced_)
    {
        throw std::logic_error("the song doesn't play in the enhanced mode");
    }

    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

uint16_t ArchivePlayer::SilentTracks() const
{
    return uint16_t(muted_ | (soloed_ ? ~soloed_ : 0));
}

void ArchivePlayer::RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (out)
    {
        apu_.Run(cycles, *out);
    }
    else if (enhanced_out)
    {
        apu_.Run(cycles, *enhanced_out);
    }
    else
    {
        apu_.Run(cycles);
    }
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!output_)
    {
        throw std::logic_error("no song has started");
    }

    const bool metering = metered_ && (out || enhanced_out);
    const size_t first_sample = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    const uint16_t silent = SilentTracks();
    uint8_t channels = 0;
    channels |= silent >> 0 & 1 ? agb::kApuSquare1 : 0;
    channels |= silent >> 1 & 1 ? agb::kApuSquare2 : 0;
    channels |= silent >> kWaveTrack & 1 ? agb::kApuWave : 0;
    channels |= silent >> kKitChannel & 1 ? agb::kApuNoise : 0;
    apu_.SetMuted(channels);
    apu_.MeterChannels(metering);
    apu_.ClearPeaks();
    output_->MeterVoices(metering);
    output_->MuteVoices(silent >> kKitChannel & 1);
    RunHardware(store_cycle_, out, enhanced_out);

    output_->SetEnhancedFrames(enhanced_out != nullptr);
    output_->Frame();
    for (const agb::RegisterWrite& write : output_->SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }

    apu_.Queue(0, output_->Fifo());
    apu_.Queue(1, output_->Fifo());
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, output_->EnhancedFifo());
        apu_.QueueEnhanced(1, output_->EnhancedFifo());
    }

    RunHardware(agb::kFrameCycles - store_cycle_, out, enhanced_out);
    frames_++;

    const bool ended = !output_->Song().TrackActive();
    tail_ = !ended || !output_->VoicesSilent() ? kTailFrames : std::max(tail_ - 1, 0);

    peaks_ = {};
    output_peaks_ = {};
    if (!metering)
    {
        last_voices_peak_ = 0.0f;
        return;
    }

    TakePeaks();
    const auto keep = [this](size_t i, int32_t value)
    {
        int32_t& peak = output_peaks_[i % 2];
        peak = std::max(peak, std::abs(value));
    };

    if (out)
    {
        for (size_t i = first_sample; i < out->size(); i++)
        {
            keep(i - first_sample, (*out)[i]);
        }
    }
    else
    {
        for (size_t i = first_sample; i < enhanced_out->size(); i++)
        {
            keep(i - first_sample, int32_t(std::lround(double((*enhanced_out)[i]) * 32768.0)));
        }
    }
}

void ArchivePlayer::TakePeaks()
{
    const float mixed = std::max(output_->VoicesPeak(), last_voices_peak_);
    last_voices_peak_ = output_->VoicesPeak();
    const int32_t voices = int32_t(std::lround(mixed * kFifoGain * kOutputStep));
    peaks_[size_t(kKitChannel)] = {voices, voices};
    for (int c = 0; c < 4; c++)
    {
        const std::array<int32_t, 2> part = apu_.ChannelPeaks(c);
        std::array<int32_t, 2>& peak = peaks_[size_t(c == kNoiseChannel ? kKitChannel : c)];
        peak[0] += part[0];
        peak[1] += part[1];
    }
}

bool ArchivePlayer::Busy() const
{
    return output_ && tail_ > 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kTracks) + " tracks");
    }

    muted_ = uint16_t(mute ? muted_ | 1u << track : muted_ & ~(1u << track));
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kTracks) + " tracks");
    }

    soloed_ = uint16_t(solo ? soloed_ | 1u << track : soloed_ & ~(1u << track));
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    if (!on)
    {
        peaks_ = {};
        output_peaks_ = {};
        last_voices_peak_ = 0.0f;
    }
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    if (!output_)
    {
        throw std::logic_error("no song has started");
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.output_.emplace(*output_);
    snapshot.apu_ = apu_;
    snapshot.store_cycle_ = store_cycle_;
    snapshot.frames_ = frames_;
    snapshot.tail_ = tail_;
    snapshot.enhanced_ = enhanced_;

    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this || !snapshot.output_)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    output_.emplace(*snapshot.output_);
    apu_ = snapshot.apu_;
    store_cycle_ = snapshot.store_cycle_;
    frames_ = snapshot.frames_;
    tail_ = snapshot.tail_;
    enhanced_ = snapshot.enhanced_;
    peaks_ = {};
    output_peaks_ = {};
    last_voices_peak_ = 0.0f;
}

} // namespace ntdlib::ubimilan
