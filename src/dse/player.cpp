// SPDX-License-Identifier: MIT

#include "ntdlib/dse/player.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/rounding.h"

namespace ntdlib::dse
{

namespace
{

namespace snd = ntr::snd;

constexpr uint32_t kSoundBias = 0x04000504;
constexpr uint32_t kSoundBiasMiddle = 0x200;

// The channels' status in the shared work (SNDSharedWork).
constexpr uint32_t kChannelStatus = 0x08;

// DSE's start-up commands (0x0206C3E0): it locks every channel against the driver's sequences, stops every channel,
// capture and alarm, and sets up and starts alarm 0, whose wake-ups run DSE's ticks.
const std::vector<snd::CommandList> kStartUp = {{
    {snd::CommandId::kLockChannels, {0xFFFF, 0, 0, 0}},
    {snd::CommandId::kStartTimer, {0, 0, 0, 0}},
    {snd::CommandId::kStopTimer, {0xFFFF, 0xFFFF, 0xFFFF, 0}},
    {snd::CommandId::kSetUpAlarm, {0, Player::kTickClocks / 64, Player::kTickClocks / 64, 2}},
    {snd::CommandId::kStartTimer, {0, 0, 1, 0}},
}};

} // namespace

Player::UnitRegisters::UnitRegisters(ntr::SoundUnit& unit) : unit_(unit)
{
}

uint32_t Player::UnitRegisters::Read(uint32_t address, int size)
{
    return unit_.Read(address, size);
}

void Player::UnitRegisters::Write(uint32_t address, int size, uint32_t value)
{
    unit_.Write(address, size, value);
}

Player::Player(const PlayerOptions& options)
    : memory_(kMainMemorySize),
      unit_(memory_),
      registers_(unit_),
      driver_(memory_, registers_),
      dse_(options.build),
      next_frame_(options.first_frame)
{
    if (options.first_frame == 0 || options.first_frame > snd::kFrameClocks)
    {
        throw std::invalid_argument("the first frame lies outside 1 to snd::kFrameClocks");
    }
    unit_.Write(kSoundBias, 2, kSoundBiasMiddle);
    driver_.Start();
    snd::InitSharedWork(std::span(memory_).subspan(kSharedWork - kMainMemory, snd::kSharedWorkSize));
    std::vector<snd::CommandList> lists = {{{snd::CommandId::kSetSharedWork, {kSharedWork, 0, 0, 0}}}};
    lists.insert(lists.end(), kStartUp.begin(), kStartUp.end());
    driver_.Frame(true, lists);
    channel_tracks_.fill(-1);
    if (options.enhanced)
    {
        unit_.StartEnhanced();
    }
}

std::optional<uint16_t> Player::AddBank(const WaveBank& bank)
{
    const uint32_t address = AlignUp(free_, 32);
    const std::size_t size = bank.samples ? bank.samples->size() : 0;
    if (size > kMainMemory + kMainMemorySize - address)
    {
        throw std::runtime_error("a bank's samples don't fit in main memory");
    }

    const std::optional<uint16_t> id = dse_.AddBank(bank, address);
    if (id)
    {
        banks_[*id] = &bank;
        if (bank.samples)
        {
            std::ranges::copy(*bank.samples, memory_.begin() + (address - kMainMemory));
            free_ = address + static_cast<uint32_t>(size);
        }
    }
    return id;
}

std::vector<uint16_t> Player::LoadBanks(const SequenceFiles& files)
{
    std::vector<uint16_t> refused;
    for (const WaveBank* bank : files.banks)
    {
        const std::optional<uint16_t> id = AddBank(*bank);
        if (!id)
        {
            refused.push_back(xsflib::LoadLe16(&(bank->header)[0x0E]));
        }
        else if (!bank->samples && files.main_bank != nullptr)
        {
            PlaceWaves(*id, *files.main_bank);
        }
    }
    return refused;
}

bool Player::Start(const SequenceFiles& files)
{
    if (files.effects == nullptr)
    {
        if (files.song == nullptr)
        {
            throw std::invalid_argument("the files hold no song or sound effect");
        }
        dse_.PlaySong(*files.song);
        dse_.Follow(dse::Driver::kSong);
        return true;
    }

    // A fresh driver gives a sound effect the first sequence of the pool.
    const std::optional<uint16_t> id = AddEffects(*files.effects);
    if (!id || !dse_.PlayEffect(*id, files.effect))
    {
        return false;
    }
    dse_.Follow(0);
    return true;
}

void Player::PlaceWaves(uint16_t id, const WaveBank& main_bank)
{
    const auto found = banks_.find(id);
    if (found == banks_.end())
    {
        throw std::runtime_error("no bank of that id is loaded");
    }
    const WaveBank& bank = *found->second;
    const uint32_t address = AlignUp(free_, 32);
    const std::span<uint8_t> memory = std::span(memory_).subspan(address - kMainMemory);
    const std::span<const uint8_t> samples =
        main_bank.samples ? std::span<const uint8_t>(*main_bank.samples) : std::span<const uint8_t>();

    // The waves in the order of the bank's slots, as Driver::PlaceWaves places them, with the bytes at each wave's
    // place in the main bank's samples.
    uint32_t place = 0;
    const int count = xsflib::LoadLe16(&bank.header[0x46]);
    for (std::size_t i = 0; i < static_cast<std::size_t>(std::max(count, 0)); i++)
    {
        if (i >= bank.waves.size() || !bank.waves[i] || i >= main_bank.waves.size() || !main_bank.waves[i])
        {
            continue;
        }
        const uint32_t size = (bank.waves[i]->LoopStart() + bank.waves[i]->LoopLength()) << 2;
        if (size > memory.size() - place)
        {
            throw std::runtime_error("a bank's waves don't fit in main memory");
        }
        const uint32_t from = main_bank.waves[i]->SamplePosition();
        const std::size_t copied = from < samples.size() ? std::min<std::size_t>(size, samples.size() - from) : 0;
        std::ranges::copy(samples.subspan(from < samples.size() ? from : 0, copied), memory.begin() + place);
        std::fill_n(memory.begin() + place + copied, size - copied, uint8_t{0});
        place += size;
    }

    dse_.PlaceWaves(id, main_bank, address);
    free_ = address + place;
}

std::optional<uint16_t> Player::AddEffects(const SoundEffects& effects)
{
    return dse_.AddEffects(effects);
}

dse::Driver& Player::Driver()
{
    return dse_;
}

const dse::Driver& Player::Driver() const
{
    return dse_;
}

void Player::RunTick(std::vector<ntr::SoundOutput>& out)
{
    if (unit_.Enhanced())
    {
        throw std::logic_error("the player mixes in the enhanced mode");
    }
    Play(next_tick_, &out, nullptr);
}

void Player::RunTick()
{
    Play(next_tick_, nullptr, nullptr);
}

void Player::Run(uint64_t clocks, std::vector<ntr::SoundOutput>& out)
{
    if (unit_.Enhanced())
    {
        throw std::logic_error("the player mixes in the enhanced mode");
    }
    Play(clock_ + clocks, &out, nullptr);
}

void Player::Run(uint64_t clocks, std::vector<float>& out)
{
    if (!unit_.Enhanced())
    {
        throw std::logic_error("the player doesn't mix in the enhanced mode");
    }
    Play(clock_ + clocks, nullptr, &out);
}

void Player::Run(uint64_t clocks)
{
    Play(clock_ + clocks, nullptr, nullptr);
}

void Player::FollowVoices()
{
    // A freed voice's channel goes on playing what the voice played until DSE starts another voice there.
    for (int v = 0; v < dse::Driver::kVoices && v < snd::kChannelCount; v++)
    {
        const VoiceView view = dse_.GetVoice(v);
        if (view.allocated)
        {
            const bool host_track = view.followed && view.track >= 0 && view.track < kHostTracks;
            channel_tracks_[static_cast<std::size_t>(v)] = static_cast<int8_t>(host_track ? view.track : -1);
        }
    }
}

void Player::Advance(uint64_t clock, std::vector<ntr::SoundOutput>* out, std::vector<float>* enhanced_out)
{
    // The host's mutes and meters, by the channels' tracks. The meters of the sound unit take the tracks that the
    // channels play for, in the order of the channels. Each track then adds up its channels.
    FollowVoices();
    uint16_t muted = 0;
    std::array<int8_t, snd::kChannelCount> meters;
    meters.fill(-1);
    std::array<int8_t, kHostTracks> meter_of_track;
    meter_of_track.fill(-1);
    int8_t next_meter = 0;
    for (std::size_t channel = 0; channel < channel_tracks_.size(); channel++)
    {
        const int track = channel_tracks_[channel];
        if (track < 0)
        {
            continue;
        }
        if (muted_ & (1u << track))
        {
            muted = static_cast<uint16_t>(muted | 1u << channel);
        }
        int8_t& meter = meter_of_track[static_cast<std::size_t>(track)];
        if (meter < 0)
        {
            meter = next_meter++;
        }
        meters[channel] = meter;
    }
    unit_.SetMutedChannels(muted);
    const bool metering = metered_ && (out != nullptr || enhanced_out != nullptr);
    if (metering)
    {
        unit_.SetMeters(meters);
    }

    // A channel's timer counts once each 2 system clocks.
    const uint64_t ticks = (clock - clock_) / 2;
    if (out)
    {
        unit_.Run(ticks, *out);
    }
    else if (enhanced_out)
    {
        unit_.Run(ticks, *enhanced_out);
    }
    else
    {
        unit_.Run(ticks);
    }
    clock_ = clock;

    if (metering)
    {
        const std::array<std::array<int32_t, 2>, 16>& unit_peaks = unit_.MeterPeaks();
        for (std::size_t track = 0; track < meter_of_track.size(); track++)
        {
            if (const int meter = meter_of_track[track]; meter >= 0)
            {
                for (std::size_t side = 0; side < 2; side++)
                {
                    peaks_[track][side] =
                        std::max(peaks_[track][side], unit_peaks[static_cast<std::size_t>(meter)][side]);
                }
            }
        }
    }
}

void Player::Tick()
{
    // DSE's thread sends the commands of a tick that has some.
    const std::size_t at = kSharedWork - kMainMemory + kChannelStatus;
    status_ = xsflib::LoadLe16(&memory_[at]);
    snd::CommandList list = dse_.Tick(status_);
    if (!list.empty())
    {
        pending_.push_back(std::move(list));
    }
    next_tick_ += kTickClocks;
}

void Player::Play(uint64_t end, std::vector<ntr::SoundOutput>* out, std::vector<float>* enhanced_out)
{
    peaks_ = {};
    while (std::min(next_frame_, next_tick_) <= end)
    {
        if (next_frame_ <= next_tick_)
        {
            Advance(next_frame_, out, enhanced_out);
            driver_.Frame(true, pending_);
            pending_.clear();
            next_frame_ += snd::kFrameClocks;
        }
        else
        {
            Advance(next_tick_, out, enhanced_out);
            Tick();
        }
    }
    Advance(end, out, enhanced_out);
}

uint64_t Player::Clock() const
{
    return clock_;
}

uint64_t Player::NextTick() const
{
    return next_tick_;
}

bool Player::Busy() const
{
    return !pending_.empty() || dse_.Playing() || dse_.EffectsPlaying() > 0 || BusyChannels() != 0;
}

void Player::MuteTrack(int track, bool mute)
{
    if (track < 0 || track >= kHostTracks)
    {
        throw std::out_of_range("a host mutes tracks 0 to " + std::to_string(kHostTracks - 1));
    }
    muted_ = mute ? muted_ | 1u << track : muted_ & ~(1u << track);
}

bool Player::IsTrackMuted(int track) const
{
    return track >= 0 && track < kHostTracks && (muted_ & (1u << track)) != 0;
}

void Player::MeterTracks(bool meter)
{
    metered_ = meter;
    peaks_ = {};
    if (!meter)
    {
        std::array<int8_t, snd::kChannelCount> none;
        none.fill(-1);
        unit_.SetMeters(none);
    }
}

std::array<int32_t, 2> Player::TrackPeaks(int track) const
{
    return track >= 0 && track < kHostTracks ? peaks_[static_cast<std::size_t>(track)] : std::array<int32_t, 2>{};
}

uint16_t Player::BusyChannels() const
{
    uint16_t busy = 0;
    for (int channel = 0; channel < snd::kChannelCount; channel++)
    {
        if (unit_.Busy(channel))
        {
            busy = static_cast<uint16_t>(busy | 1u << channel);
        }
    }
    return busy;
}

uint16_t Player::LastStatus() const
{
    return status_;
}

const ntr::snd::Driver& Player::SoundDriver() const
{
    return driver_;
}

const ntr::SoundUnit& Player::Unit() const
{
    return unit_;
}

std::span<const uint8_t> Player::MainMemory() const
{
    return memory_;
}

Player::Snapshot Player::Save() const
{
    // The samples change only when a bank loads. A snapshot shares them with the last while they haven't.
    const auto begin = memory_.begin() + (kSamples - kMainMemory);
    const auto end = memory_.begin() + (std::max(free_, kSamples) - kMainMemory);
    if (!saved_samples_ || !std::ranges::equal(*saved_samples_, std::span(begin, end)))
    {
        saved_samples_ = std::make_shared<const std::vector<uint8_t>>(begin, end);
    }

    Snapshot snapshot;
    snapshot.owner_ = this;
    snapshot.dse_.emplace(dse_);
    snapshot.driver_.emplace(driver_);
    snapshot.unit_.emplace(unit_);
    std::ranges::copy(std::span(memory_).subspan(kSharedWork - kMainMemory, snd::kSharedWorkSize),
                      snapshot.shared_work_.begin());
    snapshot.samples_ = saved_samples_;
    snapshot.free_ = free_;
    snapshot.banks_ = banks_;
    snapshot.clock_ = clock_;
    snapshot.next_frame_ = next_frame_;
    snapshot.next_tick_ = next_tick_;
    snapshot.status_ = status_;
    snapshot.pending_ = pending_;
    snapshot.channel_tracks_ = channel_tracks_;
    return snapshot;
}

void Player::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner_ != this)
    {
        throw std::invalid_argument("the snapshot is another player's");
    }

    SequenceHooks hooks = dse_.GetHooks();
    dse_ = *snapshot.dse_;
    dse_.SetHooks(std::move(hooks));
    driver_ = *snapshot.driver_;
    unit_ = *snapshot.unit_;
    std::ranges::copy(snapshot.shared_work_, memory_.begin() + (kSharedWork - kMainMemory));
    std::ranges::copy(*snapshot.samples_, memory_.begin() + (kSamples - kMainMemory));
    free_ = snapshot.free_;
    banks_ = snapshot.banks_;
    clock_ = snapshot.clock_;
    next_frame_ = snapshot.next_frame_;
    next_tick_ = snapshot.next_tick_;
    status_ = snapshot.status_;
    pending_ = snapshot.pending_;
    channel_tracks_ = snapshot.channel_tracks_;
}

} // namespace ntdlib::dse
