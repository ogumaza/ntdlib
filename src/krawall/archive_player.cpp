// SPDX-License-Identifier: MIT

#include "ntdlib/krawall/archive_player.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/agb/apu.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/output_peaks.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"

namespace ntdlib::krawall
{
namespace
{

// The sound registers as kragInit() leaves them: both FIFOs at full volume on timer 0, and in stereo, FIFO A on the
// left and FIFO B on the right. In mono, FIFO A plays on both sides. The sound bias register keeps the BIOS's 0x200,
// with 9 bits at 32,768 Hz.
std::vector<uint8_t> SoundRegisters(bool stereo)
{
    std::vector<uint8_t> registers(agb::kSoundRegisterBytes);
    const auto set = [&registers](uint32_t address, uint8_t value)
    {
        registers[address - agb::kRegSound1CntL] = value;
    };

    set(0x04000082, 0x0C);
    set(0x04000083, stereo ? 0x12 : 0x03);
    set(0x04000084, 0x80);
    set(0x04000089, 0x02);

    return registers;
}

// The mixes that the FIFOs hold at most before a frame mixes no more, and the mixes after the player and the mixer
// channels have stopped that the piece still sounds for.
constexpr size_t kQueuedMixes = 2;
constexpr int kTailMixes = 3;

} // namespace

ArchivePlayer::ArchivePlayer(const Rom& rom, const DriverInfo& info) : rom_(rom), info_(info)
{
}

Player ArchivePlayer::Load(int song) const
{
    if (song < 0 || size_t(song) >= info_.songs.size())
    {
        throw std::invalid_argument("piece " + std::to_string(song) + " doesn't exist");
    }

    Player player(info_.unpacked ? *info_.unpacked : rom_, info_, info_.songs[size_t(song)]);
    if (!player.Valid())
    {
        throw std::invalid_argument("piece " + std::to_string(song) + " can't be played");
    }

    return player;
}

void ArchivePlayer::Start(int song, const ArchivePlayerOptions& options)
{
    Begin(Load(song), options);
}

void ArchivePlayer::StartNote(int song, uint16_t instrument, uint8_t note, const ArchivePlayerOptions& options)
{
    Player player = Load(song);
    player.Audition(instrument, note);
    Begin(std::move(player), options);
}

void ArchivePlayer::ReleaseNote()
{
    if (!player_)
    {
        throw std::logic_error("no piece has started");
    }

    player_->ReleaseAudition();
}

void ArchivePlayer::Begin(Player player, const ArchivePlayerOptions& options)
{
    if (info_.frame_samples <= 0 || info_.mix_rate <= 0)
    {
        throw std::invalid_argument("the mixer mixes no samples");
    }

    player.MixOutput(true);
    player.MixEnhanced(options.enhanced);

    // Timer 0 runs at the mixer's rate. The FIFOs start with a mix of silence.
    const auto samples = size_t(info_.frame_samples);
    const auto period = uint32_t((1u << 24) / uint32_t(info_.mix_rate));
    agb::Apu apu(SoundRegisters(info_.stereo));
    apu.SetTimer(0, period, period);
    apu.Queue(0, std::vector<int8_t>(samples, 0));
    apu.Queue(1, std::vector<int8_t>(samples, 0));
    if (options.enhanced)
    {
        apu.StartEnhanced();
        const auto silence = size_t(std::ceil(double(samples) * agb::kEnhancedRate / info_.mix_rate));
        apu.QueueEnhanced(0, std::vector<float>(silence, 0.0f));
        apu.QueueEnhanced(1, std::vector<float>(silence, 0.0f));
    }

    player_.reset();
    player_.emplace(std::move(player));
    apu_ = std::move(apu);
    frames_ = 0;
    quiet_mixes_ = 0;
    enhanced_ = options.enhanced;
    peaks_ = {};
    output_peaks_ = {};
}

const Player& ArchivePlayer::Driver() const
{
    if (!player_)
    {
        throw std::logic_error("no piece has started");
    }

    return *player_;
}

uint64_t ArchivePlayer::Tick() const
{
    return player_ ? player_->Ticks() : 0;
}

bool ArchivePlayer::Busy() const
{
    if (!player_)
    {
        return false;
    }

    return (player_->Playing() && !player_->Auditioning()) || quiet_mixes_ < kTailMixes;
}

void ArchivePlayer::RunFrame(std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the piece plays in the enhanced mode");
    }

    Frame(&out, nullptr);
}

void ArchivePlayer::RunFrame(std::vector<float>& out)
{
    if (player_ && !enhanced_)
    {
        throw std::logic_error("the piece doesn't play in the enhanced mode");
    }

    Frame(nullptr, &out);
}

void ArchivePlayer::RunFrame()
{
    Frame(nullptr, nullptr);
}

uint32_t ArchivePlayer::SilentTracks() const
{
    return soloed_ ? (muted_ | ~soloed_) : muted_;
}

void ArchivePlayer::Frame(std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    if (!player_)
    {
        throw std::logic_error("no piece has started");
    }

    // The game mixes when the DMA has caught up with its mixes.
    const bool metering = metered_ && (out || enhanced_out);
    const size_t first_sample = out ? out->size() : enhanced_out ? enhanced_out->size() : 0;
    player_->MuteChannels(SilentTracks());
    player_->MeterChannels(metering);
    if (apu_.Queued(0) < kQueuedMixes * size_t(info_.frame_samples))
    {
        player_->Frame();
        apu_.Queue(0, player_->LeftOutput());
        apu_.Queue(1, player_->RightOutput());
        if (enhanced_)
        {
            apu_.QueueEnhanced(0, player_->EnhancedOutput(0));
            apu_.QueueEnhanced(1, player_->EnhancedOutput(1));
        }

        const bool sounding = (player_->Playing() && !player_->Auditioning()) || player_->MixerPlays();
        quiet_mixes_ = sounding ? 0 : std::min(quiet_mixes_ + 1, kTailMixes);
        peaks_ = {};
        if (metering)
        {
            for (int t = 0; t < kPlayerTracks; t++)
            {
                peaks_[size_t(t)] = player_->ChannelPeaks()[size_t(t)];
            }
        }
    }
    else
    {
        player_->SkipFrame();
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

    output_peaks_ = {};
    if (!metering)
    {
        peaks_ = {};
        return;
    }

    agb::KeepOutputPeaks(output_peaks_, out, enhanced_out, first_sample);
}

void ArchivePlayer::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("track " + std::to_string(track) + " doesn't exist");
    }

    muted_ = mute ? muted_ | (1u << track) : muted_ & ~(1u << track);
}

void ArchivePlayer::SoloTrack(int track, bool solo)
{
    if (track < 0 || track >= kPlayerTracks)
    {
        throw std::out_of_range("track " + std::to_string(track) + " doesn't exist");
    }

    soloed_ = solo ? soloed_ | (1u << track) : soloed_ & ~(1u << track);
}

void ArchivePlayer::MeterTracks(bool on)
{
    metered_ = on;
    peaks_ = {};
    output_peaks_ = {};
}

std::array<int32_t, 2> ArchivePlayer::TrackPeaks(int track) const
{
    if (track < 0 || track >= kPlayerTracks || (SilentTracks() >> track & 1))
    {
        return {};
    }

    return peaks_[size_t(track)];
}

ArchivePlayer::Snapshot ArchivePlayer::Save(const Snapshot*) const
{
    if (!player_)
    {
        throw std::logic_error("no piece has started");
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.player_ = std::make_shared<const Player>(*player_);
    snapshot.apu_ = apu_;
    snapshot.frames_ = frames_;
    snapshot.quiet_mixes_ = quiet_mixes_;
    snapshot.enhanced_ = enhanced_;
    return snapshot;
}

void ArchivePlayer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this || !snapshot.player_)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    player_.reset();
    player_.emplace(*snapshot.player_);
    apu_ = snapshot.apu_;
    frames_ = snapshot.frames_;
    quiet_mixes_ = snapshot.quiet_mixes_;
    enhanced_ = snapshot.enhanced_;
    peaks_ = {};
    output_peaks_ = {};
}

} // namespace ntdlib::krawall
