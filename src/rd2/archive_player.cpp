// SPDX-License-Identifier: MIT

#include "ntdlib/rd2/archive_player.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ntdlib::rd2
{
namespace
{

constexpr uint32_t kRegFifoVolumes = 0x04000082; // SOUNDCNT_H's low byte
constexpr uint32_t kRegSoundCntHigh = 0x04000083;

// The frames that a frame's mix takes to play, after the frame that makes it: the FIFOs play its first bytes in the
// next frame, and its last 17 in the frame after.
constexpr int kTailFrames = 2;

// The frames between a voice's mixing and its playing.
constexpr size_t kMeterFrames = size_t(uint64_t(kFifoLead) * kFifoCycles / agb::kFrameCycles);

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

    if (song < 0 || song >= SongCount(info_))
    {
        throw std::invalid_argument("song " + std::to_string(song) + " doesn't exist");
    }

    if (const Effect* effect = SongEffect(info_, song); effect && !effect->address)
    {
        throw std::invalid_argument("the entry of song " + std::to_string(song) + " gives no track");
    }

    Output output(rom_, info_);
    output.Play(song, options.version);
    if (!output.Song()->Valid())
    {
        throw std::invalid_argument("song " + std::to_string(song) + " can't be played");
    }

    if (options.echo_goal)
    {
        output.SetEchoGoal(*options.echo_goal);
    }

    agb::Apu apu(output.SoundRegisters());
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);
    apu.SetTimer(0, kFifoCycles, kFifoCycles);
    const std::vector<int8_t> silence(size_t(kFifoLead), 0);
    apu.Queue(0, silence);
    apu.Queue(1, silence);
    if (options.enhanced)
    {
        // The first byte of the first mix plays from the timer's overflow that takes it.
        output.StartEnhancedMixing();
        apu.StartEnhanced();
        const uint64_t samples =
            (uint64_t(kFifoLead + 1) * kFifoCycles * agb::kEnhancedFrameSamples + agb::kFrameCycles / 2) /
            agb::kFrameCycles;
        const std::vector<float> enhanced_silence(size_t(samples), 0.0f);
        apu.QueueEnhanced(0, enhanced_silence);
        apu.QueueEnhanced(1, enhanced_silence);
    }

    output_.emplace(output);
    apu_ = std::move(apu);
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    tail_ = kTailFrames;
    enhanced_ = options.enhanced;
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

const Output& ArchivePlayer::Driver() const
{
    if (!output_)
    {
        throw std::logic_error("no song has started");
    }

    return *output_;
}

void ArchivePlayer::SetEchoGoal(uint8_t level)
{
    if (!output_)
    {
        throw std::logic_error("no song has started");
    }

    output_->SetEchoGoal(level);
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

void ArchivePlayer::ApplyPsgMutes()
{
    const uint16_t silent = SilentTracks();
    uint8_t console = 0;
    std::vector<uint8_t> grown;
    for (int unit = 0; unit < output_->PsgUnits(); unit++)
    {
        uint8_t channels = 0;
        for (int c = 0; c < 4; c++)
        {
            const int track = output_->PsgChannelTrack(unit, c);
            if (track >= 0 && (silent >> track & 1))
            {
                channels = uint8_t(channels | 1u << c);
            }
        }

        if (unit == 0)
        {
            console = channels;
        }
        else
        {
            grown.push_back(channels);
        }
    }

    apu_.SetMuted(console);
    if (!grown.empty())
    {
        apu_.SetMutedUnits(grown);
    }
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
    apu_.MeterChannels(metering);
    apu_.ClearPeaks();
    output_->MeterVoices(metering);
    output_->SetMutedTracks(SilentTracks());
    ApplyPsgMutes();
    RunHardware(store_cycle_, out, enhanced_out);

    output_->SetEnhancedFrames(enhanced_out != nullptr);
    output_->Frame();
    for (const agb::RegisterWrite& write : output_->SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }

    // The stores can give a channel to another track.
    ApplyPsgMutes();
    apu_.Queue(0, output_->FifoA());
    apu_.Queue(1, output_->FifoB());
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, output_->EnhancedFifoA());
        apu_.QueueEnhanced(1, output_->EnhancedFifoB());
    }

    RunHardware(agb::kFrameCycles - store_cycle_, out, enhanced_out);
    frames_++;

    const bool silent = output_->Song()->Ended() && output_->MixerSilent() && apu_.PsgSilent();
    tail_ = silent ? std::max(tail_ - 1, 0) : kTailFrames;

    peaks_ = {};
    output_peaks_ = {};
    if (!metering)
    {
        mixed_.clear();
        return;
    }

    TrackParts parts;
    for (size_t t = 0; t < parts.size(); t++)
    {
        parts[t] = output_->VoicePeaks(int(t));
    }

    mixed_.push_back(parts);
    if (mixed_.size() > kMeterFrames)
    {
        TakePeaks(mixed_.front());
        mixed_.erase(mixed_.begin());
    }

    // The samples of the frame.
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

void ArchivePlayer::TakePeaks(const TrackParts& parts)
{
    // A voice's byte counts 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50%. SOUNDCNT_H's high
    // byte puts each FIFO on its sides, the right in the low bit.
    const uint8_t volumes = apu_.Register(kRegFifoVolumes);
    const uint8_t sides = apu_.Register(kRegSoundCntHigh);
    for (size_t t = 0; t < peaks_.size(); t++)
    {
        for (int f = 0; f < 2; f++)
        {
            const float part = parts[t][size_t(f)] * float(volumes >> (2 + f) & 1 ? 4 : 2) * 64.0f;
            const int32_t peak = int32_t(std::lround(part));
            if (sides >> (4 * f) & 1)
            {
                peaks_[t][1] = std::max(peaks_[t][1], peak);
            }
            if (sides >> (4 * f + 1) & 1)
            {
                peaks_[t][0] = std::max(peaks_[t][0], peak);
            }
        }
    }

    for (int unit = 0; unit < output_->PsgUnits() && unit < apu_.PsgUnits(); unit++)
    {
        for (int c = 0; c < 4; c++)
        {
            // A frame's stores can give a channel to a muted track. The channel has played up to then. A muted track's
            // meter stays at 0.
            const int track = output_->PsgChannelTrack(unit, c);
            if (track < 0 || (SilentTracks() >> track & 1))
            {
                continue;
            }

            const std::array<int32_t, 2> part = unit == 0 ? apu_.ChannelPeaks(c) : apu_.UnitPeaks(unit, c);
            std::array<int32_t, 2>& peak = peaks_[size_t(track)];
            peak[0] = std::max(peak[0], part[0]);
            peak[1] = std::max(peak[1], part[1]);
        }
    }
}

bool ArchivePlayer::Busy() const
{
    return output_ && tail_ > 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kPlayerTracks) + " tracks");
    }

    muted_ = uint16_t(mute ? muted_ | 1u << track : muted_ & ~(1u << track));
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kPlayerTracks) + " tracks");
    }

    soloed_ = uint16_t(solo ? soloed_ | 1u << track : soloed_ & ~(1u << track));
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    if (!on)
    {
        mixed_.clear();
        peaks_ = {};
        output_peaks_ = {};
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
    snapshot.output_ = std::make_shared<const Output>(*output_);
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
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::rd2
