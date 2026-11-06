// SPDX-License-Identifier: MIT

#include "ntdlib/konami/archive_player.h"

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

namespace ntdlib::konami
{

namespace
{

constexpr uint32_t kSound3CntX = 0x04000074;
constexpr uint32_t kRegFifoVolumes = 0x04000082; // SOUNDCNT_H's low byte
constexpr uint32_t kRegSoundCntHigh = 0x04000083;

// The bytes that a game stores in a FIFO after its reset.
constexpr std::array<int8_t, 4> kRestartBytes = {};

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

    Request request;
    if (options.live && std::find(info_.live_songs.begin(), info_.live_songs.end(), song) != info_.live_songs.end())
    {
        request.setting = kLiveStageSetting;
        const auto table = info_.live_tables.find(song);
        if (table != info_.live_tables.end())
        {
            request.samples = table->second;
        }
    }

    Output output(rom_, info_);
    output.Play(song, request);
    if (!output.Song()->Valid())
    {
        throw std::invalid_argument("song " + std::to_string(song) + " can't be played");
    }

    agb::Apu apu(output.SoundRegisters());
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);

    // The init restarts the wave channel, and the registers alone start no channel. On most revisions, a note only
    // loads the wave RAM, flips the bank and sets the frequency. Without this store, the channel would never sound.
    apu.Store(kSound3CntX, 2, 0x8000);

    // Up to the first stores, the FIFOs play the silence that the init left. A revision without a mixer has its timers
    // stopped there.
    const uint32_t period = output.FifoCycles(0);
    const uint64_t lead_cycles = uint64_t(output.FifoLead()) * period;
    if (period)
    {
        apu.SetTimer(0, period, period);
        const std::vector<int8_t> silence(size_t(options.store_cycle / period + output.FifoLead()), 0);
        apu.Queue(0, silence);
        apu.Queue(1, silence);
    }

    if (options.enhanced)
    {
        output.StartEnhancedMixing();
        apu.StartEnhanced();
        const uint64_t samples =
            ((options.store_cycle + lead_cycles) * agb::kEnhancedFrameSamples + agb::kFrameCycles) / agb::kFrameCycles;
        const std::vector<float> silence(size_t(samples), 0.0f);
        apu.QueueEnhanced(0, silence);
        apu.QueueEnhanced(1, silence);
    }

    output_.emplace(std::move(output));
    apu_ = std::move(apu);
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    meter_frames_ = size_t(lead_cycles / agb::kFrameCycles);
    tail_frames_ = int(meter_frames_) + 2;
    tail_ = tail_frames_;
    enhanced_ = options.enhanced;
    changes_.clear();
    next_change_ = 0;
    mixed_cycles_ = 0;
    fifo_bytes_ = {};
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
    uint8_t channels = 0;
    for (int c = 0; c < 4; c++)
    {
        if (silent >> output_->PsgChannelTrack(c) & 1)
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

    // A timer's change comes at its overflow's cycle, once the overflow has taken its sample.
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
    const bool reached = output_->Frame();
    for (const agb::RegisterWrite& write : output_->SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }

    for (int f = 0; f < 2; f++)
    {
        if (output_->FifoRestarted(f))
        {
            apu_.Queue(f, kRestartBytes);
        }
    }

    // The stores can give a channel to another track.
    ApplyPsgMutes();
    output_->Mix(fifo_bytes_[0], fifo_bytes_[1]);
    apu_.Queue(0, fifo_bytes_[0]);
    apu_.Queue(1, fifo_bytes_[1]);
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, output_->EnhancedFifoA());
        apu_.QueueEnhanced(1, output_->EnhancedFifoB());
    }

    changes_ = output_->TimerChanges();
    next_change_ = 0;
    mixed_cycles_ = 0;
    RunHardware(agb::kFrameCycles - store_cycle_, out, enhanced_out);
    frames_++;

    const bool ended = !reached || !output_->Song()->AnyTrackActive();
    tail_ = !ended || !output_->MixerSilent() ? tail_frames_ : std::max(tail_ - 1, 0);

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
    if (mixed_.size() > meter_frames_)
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

    // The stores of a frame can give a channel to a muted track. The channel can sound before the mutes follow. A muted
    // track's meter stays at 0.
    for (int c = 0; c < 4; c++)
    {
        const int track = output_->PsgChannelTrack(c);
        if (SilentTracks() >> track & 1)
        {
            continue;
        }

        const std::array<int32_t, 2> part = apu_.ChannelPeaks(c);
        std::array<int32_t, 2>& peak = peaks_[size_t(track)];
        peak[0] = std::max(peak[0], part[0]);
        peak[1] = std::max(peak[1], part[1]);
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
    snapshot.output_.emplace(*output_);
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
    changes_ = snapshot.changes_;
    next_change_ = snapshot.next_change_;
    mixed_cycles_ = snapshot.mixed_cycles_;
    fifo_bytes_ = {};
    mixed_.clear();
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::konami
