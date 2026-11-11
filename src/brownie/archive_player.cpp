// SPDX-License-Identifier: MIT

#include "ntdlib/brownie/archive_player.h"

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

#include "ntdlib/agb/output_peaks.h"

namespace ntdlib::brownie
{
namespace
{

constexpr uint32_t kRegFifoVolumes = 0x04000082; // SOUNDCNT_H's low byte
constexpr uint32_t kRegSoundCntHigh = 0x04000083;

// The frames after the sound's end in which the FIFOs can still play what they hold.
constexpr int kTailFrames = 2;

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

    Sequencer sequencer(rom_, info_, song);
    if (!sequencer.Valid())
    {
        throw std::invalid_argument("sound " + std::to_string(song) + " can't be played");
    }

    sequencer.KeepFifoBytes(true);
    agb::Apu apu(sequencer.SoundRegisters());
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);

    const bool mixer = info_.revision == Revision::kSwordOfMana;
    if (mixer)
    {
        const std::vector<int8_t> silence(kFifoLeadPoints, 0);
        apu.Queue(0, silence);
        apu.Queue(1, silence);
    }

    if (options.enhanced)
    {
        sequencer.StartEnhancedMixing();
        apu.StartEnhanced();
        const uint64_t wait = mixer ? uint64_t(kMeanWaitPoints) * info_.point_cycles : 0;
        const uint64_t samples =
            ((options.store_cycle + wait) * agb::kEnhancedFrameSamples + agb::kFrameCycles) / agb::kFrameCycles;
        const std::vector<float> silence(size_t(samples), 0.0f);
        apu.QueueEnhanced(0, silence);
        apu.QueueEnhanced(1, silence);
    }

    sequencer_.emplace(std::move(sequencer));
    apu_ = std::move(apu);
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    tail_ = kTailFrames;
    enhanced_ = options.enhanced;
    changes_.clear();
    next_change_ = 0;
    mixed_cycles_ = 0;
    peaks_ = {};
    output_peaks_ = {};
}

const Sequencer& ArchivePlayer::Driver() const
{
    if (!sequencer_)
    {
        throw std::logic_error("no sound has started");
    }

    return *sequencer_;
}

const std::vector<int8_t>& ArchivePlayer::FifoBytes(int fifo) const
{
    return Driver().FifoBytes(fifo);
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the sound plays in the enhanced mode");
    }

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (sequencer_ && !enhanced_)
    {
        throw std::logic_error("the sound doesn't play in the enhanced mode");
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
    uint8_t channels = 0;
    for (int c = 0; c < 4; c++)
    {
        if (silent >> sequencer_->PsgChannelTrack(c) & 1)
        {
            channels = uint8_t(channels | 1u << c);
        }
    }

    apu_.SetMuted(channels);
}

void ArchivePlayer::RunHardware(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    const auto run = [&](uint64_t part)
    {
        if (out)
        {
            apu_.Run(part, *out);
        }
        else if (enhanced_out)
        {
            apu_.Run(part, *enhanced_out);
        }
        else
        {
            apu_.Run(part);
        }
    };

    // A timer's change comes at its cycle, once the overflows before it have taken their samples.
    while (next_change_ < changes_.size() && changes_[next_change_].cycle <= mixed_cycles_ + cycles)
    {
        const FifoTimerChange& change = changes_[next_change_++];
        const uint64_t part = change.cycle - mixed_cycles_;
        run(part);
        cycles -= part;
        mixed_cycles_ = change.cycle;
        apu_.SetTimer(change.timer, change.period, change.first);
    }

    run(cycles);
    mixed_cycles_ += cycles;
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!sequencer_)
    {
        throw std::logic_error("no sound has started");
    }

    const bool metering = metered_ && (out || enhanced_out);
    const size_t first_sample = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    apu_.MeterChannels(metering);
    apu_.ClearPeaks();
    sequencer_->MeterVoices(metering);
    sequencer_->SetMutedChannels(SilentTracks());
    ApplyPsgMutes();
    RunHardware(store_cycle_, out, enhanced_out);

    sequencer_->SetEnhancedFrames(enhanced_out != nullptr);
    for (const agb::RegisterWrite& write : sequencer_->Step())
    {
        apu_.Store(write.address, write.size, write.value);
    }

    // The stores can give a channel to another track.
    ApplyPsgMutes();
    apu_.Queue(0, sequencer_->FifoBytes(0));
    apu_.Queue(1, sequencer_->FifoBytes(1));
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, sequencer_->EnhancedFifo(0));
        apu_.QueueEnhanced(1, sequencer_->EnhancedFifo(1));
    }

    changes_ = sequencer_->TimerChanges();
    next_change_ = 0;
    mixed_cycles_ = 0;
    RunHardware(agb::kFrameCycles - store_cycle_, out, enhanced_out);
    frames_++;
    tail_ = sequencer_->Playing() ? kTailFrames : std::max(tail_ - 1, 0);

    peaks_ = {};
    output_peaks_ = {};
    if (!metering)
    {
        return;
    }

    TakePeaks();

    // The samples of the frame.
    agb::KeepOutputPeaks(output_peaks_, out, enhanced_out, first_sample);
}

void ArchivePlayer::TakePeaks()
{
    // A voice's byte counts 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50%. SOUNDCNT_H's high
    // byte puts each FIFO on its sides, the right in the low bit.
    const uint8_t volumes = apu_.Register(kRegFifoVolumes);
    const uint8_t sides = apu_.Register(kRegSoundCntHigh);
    for (size_t t = 0; t < peaks_.size(); t++)
    {
        const std::array<float, 2> parts = sequencer_->VoicePeaks(int(t));
        for (int f = 0; f < 2; f++)
        {
            const float part = parts[size_t(f)] * float(volumes >> (2 + f) & 1 ? 4 : 2) * 64.0f;
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

    for (int c = 0; c < 4; c++)
    {
        const std::array<int32_t, 2> part = apu_.ChannelPeaks(c);
        std::array<int32_t, 2>& peak = peaks_[size_t(sequencer_->PsgChannelTrack(c))];
        peak[0] = std::max(peak[0], part[0]);
        peak[1] = std::max(peak[1], part[1]);
    }
}

bool ArchivePlayer::Busy() const
{
    return sequencer_ && tail_ > 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kChannelCount)
    {
        throw std::out_of_range("a sound has at most " + std::to_string(kChannelCount) + " tracks");
    }

    muted_ = uint16_t(mute ? muted_ | 1u << track : muted_ & ~(1u << track));
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kChannelCount)
    {
        throw std::out_of_range("a sound has at most " + std::to_string(kChannelCount) + " tracks");
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
    }
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    if (!sequencer_)
    {
        throw std::logic_error("no sound has started");
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.sequencer_ = std::make_shared<const Sequencer>(*sequencer_);
    snapshot.apu_ = apu_;
    snapshot.store_cycle_ = store_cycle_;
    snapshot.frames_ = frames_;
    snapshot.tail_ = tail_;
    snapshot.enhanced_ = enhanced_;
    snapshot.changes_ = changes_;
    snapshot.next_change_ = next_change_;
    snapshot.mixed_cycles_ = mixed_cycles_;
    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this || !snapshot.sequencer_)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    sequencer_.emplace(*snapshot.sequencer_);
    apu_ = snapshot.apu_;
    store_cycle_ = snapshot.store_cycle_;
    frames_ = snapshot.frames_;
    tail_ = snapshot.tail_;
    enhanced_ = snapshot.enhanced_;
    changes_ = snapshot.changes_;
    next_change_ = snapshot.next_change_;
    mixed_cycles_ = snapshot.mixed_cycles_;
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::brownie
