// SPDX-License-Identifier: MIT

#include "ntdlib/mp2k/archive_player.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ntdlib::mp2k
{

namespace
{

constexpr uint32_t kRegSoundCntH = 0x04000082;

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    if (options.store_cycle >= agb::kFrameCycles)
    {
        throw std::invalid_argument("the writes' cycle lies past the frame");
    }

    Sequencer sequencer(rom_, info_, song);
    if (!sequencer.Valid())
    {
        throw std::invalid_argument("song " + std::to_string(song) + " can't be played");
    }
    sequencer.StartMixing();
    if (options.enhanced)
    {
        sequencer.StartEnhancedMixing();
        sequencer.GrowChannels(true);
    }
    sequencer.SetEnvelopeCounter(options.envelope_counter);

    // Timer 0 overflows once for each of the frame's points in the buffer: the mixer's, or two for each of the mixer's
    // in Golden Sun: The Lost Age's driver. The first frame plays the silent part of the buffer that the init cleared.
    agb::Apu apu(sequencer.SoundRegisters());
    apu.SetSequencer(options.sequencer_step, options.sequencer_cycles);
    const uint32_t points = uint32_t(sequencer.Right().size());
    apu.SetTimer(0, uint32_t(agb::kFrameCycles) / points, 0);
    const std::vector<int8_t> silence(points + kFifoLead, 0);
    apu.Queue(0, silence);
    apu.Queue(1, silence);

    // The enhanced mode's FIFOs start with the same silence at the enhanced rate: a frame, and the lead in the time of
    // kFifoLead of the console's points.
    if (options.enhanced)
    {
        apu.StartEnhanced();
        const uint32_t lead = (2 * kFifoLead * agb::kEnhancedFrameSamples + points) / (2 * points);
        const std::vector<float> enhanced_silence(agb::kEnhancedFrameSamples + lead, 0.0f);
        apu.QueueEnhanced(0, enhanced_silence);
        apu.QueueEnhanced(1, enhanced_silence);
    }

    sequencer_.emplace(std::move(sequencer));
    apu_ = std::move(apu);
    store_cycle_ = options.store_cycle;
    frames_ = 0;
    tail_ = 2;
    enhanced_ = options.enhanced;
    writes_.clear();
    actions_.clear();
    mixed_ = {};
    peaks_ = {};
    ApplySettings();
}

void ArchivePlayer::ApplySettings()
{
    sequencer_->SetMutedTracks(muted_);
    sequencer_->MeterTracks(metered_);
}

void ArchivePlayer::WriteMemory(int index, uint8_t value)
{
    if (index < 0 || size_t(index) >= kMemAccSize)
    {
        throw std::out_of_range("memacc's memory holds " + std::to_string(kMemAccSize) + " bytes");
    }
    writes_.emplace_back(index, value);
}

const Sequencer& ArchivePlayer::Driver() const
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }
    return *sequencer_;
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
    if (sequencer_ && !enhanced_)
    {
        throw std::logic_error("the song doesn't play in the enhanced mode");
    }
    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }

    const bool output = out || enhanced_out;
    const bool metering = metered_ && output;
    apu_.MeterChannels(metering);
    apu_.ClearPeaks();
    const auto run = [&](uint64_t cycles)
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
    };

    // The PSG's channels of muted tracks stay out of the output, those of the grown units too. The hardware plays up
    // to the driver's routine, whose writes then take effect. A note there can give a channel to another track.
    std::vector<uint8_t> muted_units;
    const auto mute_psg = [&]()
    {
        uint8_t muted = 0;
        muted_units.assign(size_t(apu_.PsgUnits() - 1), 0);
        for (int c = kFirstPsgChannel; c < sequencer_->ChannelCount(); c++)
        {
            const Channel& channel = sequencer_->GetChannel(c);
            const int owner = sequencer_->ChannelTrack(c);
            if (!channel.part || owner < 0 || !(muted_ >> owner & 1))
            {
                continue;
            }
            if (channel.unit == 0)
            {
                muted = uint8_t(muted | 1u << (channel.part - 1));
            }
            else if (size_t(channel.unit) <= muted_units.size())
            {
                uint8_t& unit = muted_units[size_t(channel.unit) - 1];
                unit = uint8_t(unit | 1u << (channel.part - 1));
            }
        }
        apu_.SetMuted(muted);
        if (!muted_units.empty())
        {
            apu_.SetMutedUnits(muted_units);
        }
    };
    mute_psg();
    run(store_cycle_);
    for (const auto& [index, value] : writes_)
    {
        sequencer_->SetMemory(index, value);
    }
    writes_.clear();
    sequencer_->SetEnhancedFrames(enhanced_out != nullptr);
    actions_ = sequencer_->Step();
    for (const agb::RegisterWrite& write : sequencer_->SoundWrites())
    {
        apu_.Store(write.address, write.size, write.value);
    }
    mute_psg();
    apu_.Queue(0, sequencer_->Right());
    apu_.Queue(1, sequencer_->Left());
    if (enhanced_)
    {
        apu_.QueueEnhanced(0, sequencer_->EnhancedRight());
        apu_.QueueEnhanced(1, sequencer_->EnhancedLeft());
    }
    run(agb::kFrameCycles - store_cycle_);
    frames_++;

    // The FIFOs play the frame's points in the next frame, and the last of them in the frame after.
    tail_ = !sequencer_->Ended() || !sequencer_->MixerSilent() ? 2 : std::max(tail_ - 1, 0);

    // A point that the mixer adds counts 4 or 2 times in the output's 10 bits: the FIFO's volume at 100% or 50%. The
    // stereo mixer's right side goes to FIFO A, and its left to FIFO B. The mono mixer's goes to FIFO A.
    peaks_ = {};
    if (metering)
    {
        const uint8_t control = apu_.Register(kRegSoundCntH);
        const int right = (control & 4) ? 4 : 2;
        const int left = info_.mono ? right : (control & 8) ? 4 : 2;
        for (size_t t = 0; t < peaks_.size(); t++)
        {
            peaks_[t] = {mixed_[t][0] * left * 64, mixed_[t][1] * right * 64};
        }
        for (int c = kFirstPsgChannel; c < sequencer_->ChannelCount(); c++)
        {
            const Channel& channel = sequencer_->GetChannel(c);
            const int owner = sequencer_->ChannelTrack(c);
            if (channel.part && owner >= 0 && channel.unit < apu_.PsgUnits())
            {
                const std::array<int32_t, 2> part = channel.unit == 0 ? apu_.ChannelPeaks(channel.part - 1)
                                                                      : apu_.UnitPeaks(channel.unit, channel.part - 1);
                for (size_t side = 0; side < 2; side++)
                {
                    peaks_[size_t(owner)][side] = std::max(peaks_[size_t(owner)][side], part[side]);
                }
            }
        }
    }
    for (int t = 0; t < kMaxTracks; t++)
    {
        mixed_[size_t(t)] = metering ? sequencer_->MixerPeaks(t) : std::array<int32_t, 2>{};
    }

    if (on_note)
    {
        for (const Action& action : actions_)
        {
            if (action.kind == Action::kNoteOn)
            {
                on_note(action);
            }
        }
    }
}

bool ArchivePlayer::Busy() const
{
    return sequencer_ && tail_ > 0;
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kMaxTracks)
    {
        throw std::out_of_range("a song has at most " + std::to_string(kMaxTracks) + " tracks");
    }
    muted_ = uint16_t(mute ? muted_ | 1u << track : muted_ & ~(1u << track));
    if (sequencer_)
    {
        ApplySettings();
    }
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    if (!on)
    {
        mixed_ = {};
        peaks_ = {};
    }
    if (sequencer_)
    {
        ApplySettings();
    }
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    if (!sequencer_)
    {
        throw std::logic_error("no song has started");
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.sequencer_.emplace(*sequencer_);
    snapshot.apu_ = apu_;
    snapshot.store_cycle_ = store_cycle_;
    snapshot.frames_ = frames_;
    snapshot.tail_ = tail_;
    snapshot.enhanced_ = enhanced_;
    snapshot.writes_ = writes_;
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
    writes_ = snapshot.writes_;
    actions_.clear();
    mixed_ = {};
    peaks_ = {};
    ApplySettings();
}

} // namespace ntdlib::mp2k
