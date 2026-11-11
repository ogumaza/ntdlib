// SPDX-License-Identifier: MIT

#include "ntdlib/rare/archive_player.h"

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

namespace ntdlib::rare
{
namespace
{

constexpr uint32_t kRegFifoVolumes = 0x04000082; // SOUNDCNT_H's low byte
constexpr uint32_t kRegSoundCntHigh = 0x04000083;

// The frames that a frame's mix takes to play, after the frame that makes it.
constexpr int kTailFrames = 2;

// The sound registers as the driver's init and its first VBlank leave them.
std::vector<uint8_t> SoundRegisters()
{
    std::vector<uint8_t> registers(agb::kSoundRegisterBytes);
    const auto set = [&registers](uint32_t address, uint8_t value)
    {
        registers[address - agb::kRegSound1CntL] = value;
    };

    set(kRegFifoVolumes, 0x0C);  // both FIFOs at full volume
    set(kRegSoundCntHigh, 0x03); // FIFO A on both sides, on timer 0
    set(0x04000084, 0x80);       // the sound on: the first VBlank after the init turns it on
    set(0x04000089, 0x42);       // with the bias's low byte at 0: the bias at 0x200, and 8 bits at 65,536 Hz

    return registers;
}

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

void ArchivePlayer::Start(int tune, const ArchivePlayerOptions& options)
{
    if (tune < 0 || tune >= info_.tune_count)
    {
        throw std::invalid_argument("tune " + std::to_string(tune) + " doesn't exist");
    }

    Sequencer sequencer(rom_, info_, tune, SilencedChannels(info_, tune, options.under_water));
    if (!sequencer.Valid())
    {
        throw std::invalid_argument("tune " + std::to_string(tune) + " can't be played");
    }

    Begin(std::move(sequencer), options);
}

void ArchivePlayer::StartNote(int tune, uint8_t program, uint8_t key, uint8_t velocity,
                              const ArchivePlayerOptions& options)
{
    if (tune < 0 || tune >= info_.tune_count)
    {
        throw std::invalid_argument("tune " + std::to_string(tune) + " doesn't exist");
    }

    Sequencer sequencer(rom_, info_, tune, SilencedChannels(info_, tune, options.under_water));
    if (!sequencer.Valid())
    {
        throw std::invalid_argument("tune " + std::to_string(tune) + " can't be played");
    }

    sequencer.Audition(program, key, velocity);
    Begin(std::move(sequencer), options);
}

void ArchivePlayer::ReleaseNote()
{
    if (!sequencer_)
    {
        throw std::logic_error("no tune has started");
    }

    sequencer_->ReleaseAudition();
}

void ArchivePlayer::Begin(Sequencer sequencer, const ArchivePlayerOptions& options)
{
    if (info_.samples_per_frame <= 0)
    {
        throw std::invalid_argument("the driver mixes no samples");
    }

    sequencer.MixSamples(true);
    sequencer.MixEnhanced(options.enhanced);

    // Timer 0 takes the frame's samples in the frame's cycles: every rate that the driver's rate routine supports gives
    // a whole number of cycles. The FIFO starts with a frame of silence.
    const auto samples = uint32_t(info_.samples_per_frame);
    const auto period = uint32_t(agb::kFrameCycles / samples);
    agb::Apu apu(SoundRegisters());
    apu.SetTimer(0, period, period);
    apu.Queue(0, std::vector<int8_t>(samples, 0));
    if (options.enhanced)
    {
        apu.StartEnhanced();
        apu.QueueEnhanced(0, std::vector<float>(agb::kEnhancedFrameSamples, 0.0f));
    }

    sequencer_.reset();
    sequencer_.emplace(std::move(sequencer));
    apu_ = std::move(apu);
    frames_ = 0;
    tail_ = kTailFrames;
    enhanced_ = options.enhanced;
    mixed_.reset();
    peaks_ = {};
    output_peaks_ = {};
}

const Sequencer& ArchivePlayer::Driver() const
{
    if (!sequencer_)
    {
        throw std::logic_error("no tune has started");
    }

    return *sequencer_;
}

uint64_t ArchivePlayer::Tick() const
{
    uint64_t tick = 0;
    for (int t = 0; sequencer_ && t < sequencer_->TrackCount(); t++)
    {
        tick = std::max(tick, sequencer_->TrackTick(t));
    }

    return tick;
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the tune plays in the enhanced mode");
    }

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (sequencer_ && !enhanced_)
    {
        throw std::logic_error("the tune doesn't play in the enhanced mode");
    }

    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

uint32_t ArchivePlayer::SilentTracks() const
{
    return muted_ | (soloed_ ? ~soloed_ : 0);
}

bool ArchivePlayer::Sounding() const
{
    if (!sequencer_->Ended() && !sequencer_->Auditioning())
    {
        return true;
    }

    // The driver clears only part of its buffer at some rates. With no voice to mix, the rest of the last mix then
    // plays again in each frame.
    const std::vector<int8_t>& samples = sequencer_->Samples();
    if (std::any_of(samples.begin(), samples.end(), [](int8_t s) { return s != 0; }))
    {
        return true;
    }

    for (int i = 0; i < sequencer_->SlotCount(); i++)
    {
        const Slot& s = sequencer_->GetSlot(i);
        if (s.mix == MixResult::kMixed && s.level > 0)
        {
            return true;
        }
    }

    return false;
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!sequencer_)
    {
        throw std::logic_error("no tune has started");
    }

    const bool metering = metered_ && (out || enhanced_out);
    const size_t first_sample = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    sequencer_->MuteTracks(SilentTracks());
    sequencer_->Step();
    apu_.Queue(0, sequencer_->Samples());
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, sequencer_->EnhancedSamples());
    }

    if (out)
    {
        apu_.Run(agb::kFrameCycles, *out);
    }
    else if (enhanced_out)
    {
        apu_.Run(agb::kFrameCycles, *enhanced_out);
    }
    else
    {
        apu_.Run(agb::kFrameCycles);
    }
    frames_++;
    tail_ = Sounding() ? kTailFrames : std::max(tail_ - 1, 0);

    // The frame plays the mix of the frame before.
    peaks_ = {};
    output_peaks_ = {};
    if (!metering)
    {
        mixed_.reset();
        return;
    }

    if (mixed_)
    {
        TakePeaks(*mixed_);
    }
    mixed_ = sequencer_->TrackPeaks();

    agb::KeepOutputPeaks(output_peaks_, out, enhanced_out, first_sample);
}

void ArchivePlayer::TakePeaks(const TrackParts& parts)
{
    // A byte of the mix counts 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50%. The model gives
    // the output's steps 64 times on the scale of 16-bit samples. SOUNDCNT_H's high byte puts FIFO A on its sides, the
    // right in the low bit.
    const uint8_t volumes = apu_.Register(kRegFifoVolumes);
    const uint8_t sides = apu_.Register(kRegSoundCntHigh);
    const int32_t scale = (volumes >> 2 & 1 ? 4 : 2) * 64;
    const uint32_t silent = SilentTracks();
    for (size_t t = 0; t < parts.size() && t < peaks_.size(); t++)
    {
        if (silent >> t & 1)
        {
            continue;
        }

        const int32_t peak = parts[t] * scale;
        if (sides & 1)
        {
            peaks_[t][1] = peak;
        }
        if (sides >> 1 & 1)
        {
            peaks_[t][0] = peak;
        }
    }
}

std::array<int32_t, 2> ArchivePlayer::TrackPeaks(int track) const
{
    if (track < 0 || track >= kPlayerTracks)
    {
        return {};
    }

    return peaks_[size_t(track)];
}

bool ArchivePlayer::Busy() const
{
    return sequencer_ && tail_ > 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("the player mutes at most " + std::to_string(kPlayerTracks) + " tracks");
    }

    muted_ = mute ? muted_ | 1u << track : muted_ & ~(1u << track);
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("the player solos at most " + std::to_string(kPlayerTracks) + " tracks");
    }

    soloed_ = solo ? soloed_ | 1u << track : soloed_ & ~(1u << track);
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    if (!on)
    {
        mixed_.reset();
        peaks_ = {};
        output_peaks_ = {};
    }
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    if (!sequencer_)
    {
        throw std::logic_error("no tune has started");
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.sequencer_ = std::make_shared<const Sequencer>(*sequencer_);
    snapshot.apu_ = apu_;
    snapshot.frames_ = frames_;
    snapshot.tail_ = tail_;
    snapshot.enhanced_ = enhanced_;
    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this || !snapshot.sequencer_)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    sequencer_.reset();
    sequencer_.emplace(*snapshot.sequencer_);
    apu_ = snapshot.apu_;
    frames_ = snapshot.frames_;
    tail_ = snapshot.tail_;
    enhanced_ = snapshot.enhanced_;
    mixed_.reset();
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::rare
