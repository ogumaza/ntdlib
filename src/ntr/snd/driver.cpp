// SPDX-License-Identifier: MIT

#include "ntdlib/ntr/snd/driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "driver_shared.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"

namespace ntdlib::ntr::snd
{

namespace
{

constexpr uint32_t kMainMemoryEnd = 0x03000000;

constexpr uint32_t kMasterVolumeRegister = 0x04000500;

// The places in the shared work (SNDSharedWork).
constexpr uint32_t kFinishedCommandTag = 0x0;

constexpr uint32_t kChannelStatus = 0x8;

constexpr uint32_t kCaptureStatus = 0xA;

constexpr uint32_t kVariableCount = 16;

} // namespace

Hang::Hang(int player, int track, uint32_t offset)
    : std::runtime_error("track " + std::to_string(track) + " of player " + std::to_string(player) + " runs " +
                         std::to_string(kCommandBudget) + " commands in a tick without waiting, up to offset " +
                         xsflib::Hex(offset, 0, true) + ": the sequence hangs"),
      player_(player),
      track_(track),
      offset_(offset)
{
}

int Hang::PlayerIndex() const
{
    return player_;
}

int Hang::TrackIndex() const
{
    return track_;
}

uint32_t Hang::Offset() const
{
    return offset_;
}

Build BuildOf(std::span<const uint8_t> arm7)
{
    // The tables start with the LFO's sine. The squares' levels follow on 4 bytes, then the levels of i/127 in the
    // builds that hold them, then the volume shifts.
    const auto sine = std::search(arm7.begin(), arm7.end(), std::begin(tables::kLfoSine), std::end(tables::kLfoSine),
                                  [](uint8_t byte, int8_t value) { return byte == static_cast<uint8_t>(value); });
    if (sine == arm7.end() ||
        std::search(sine + 1, arm7.end(), std::begin(tables::kLfoSine), std::end(tables::kLfoSine),
                    [](uint8_t byte, int8_t value) { return byte == static_cast<uint8_t>(value); }) != arm7.end())
    {
        throw std::runtime_error("the ARM7 binary doesn't hold the sound driver's tables once");
    }

    const auto at = static_cast<std::size_t>(sine - arm7.begin());
    const std::size_t squares = (at + std::size(tables::kLfoSine) + 3) & ~std::size_t{3};
    const std::size_t after = squares + 2 * std::size(tables::kDecibelSquare);
    if (after + std::size(tables::kVolumeShift) > arm7.size())
    {
        throw std::runtime_error("the ARM7 binary ends inside the sound driver's tables");
    }

    Build build;
    build.zero_level = static_cast<int16_t>(xsflib::LoadLe16(&arm7[squares]));
    build.decibel_table = !std::equal(std::begin(tables::kVolumeShift), std::end(tables::kVolumeShift),
                                      arm7.begin() + static_cast<std::ptrdiff_t>(after));
    return build;
}

void InitSharedWork(std::span<uint8_t> work)
{
    std::fill(work.begin(), work.begin() + kSharedWorkSize, uint8_t{0});
    for (uint32_t player = 0; player < kPlayerCount; player++)
    {
        std::fill_n(work.begin() + kPlayerWork + kPlayerWorkSize * player, 2 * kVariableCount, uint8_t{0xFF});
    }
    std::fill_n(work.begin() + kGlobalVariables, 2 * kVariableCount, uint8_t{0xFF});
}

Driver::Driver(std::span<uint8_t> main_memory, Registers& registers, Build build)
    : main_memory_(main_memory), registers_(&registers), build_(build)
{
    // The tables lie one after another in the driver's binary. Values past the end of one read the next.
    for (int i = 0; i < 128; i++)
    {
        const int16_t level = i == 0 ? build.zero_level : tables::kDecibelSquare[i];
        tables_.push_back(static_cast<uint8_t>(level));
        tables_.push_back(static_cast<uint8_t>(static_cast<uint16_t>(level) >> 8));
    }
    if (build.decibel_table)
    {
        for (const int16_t level : tables::kDecibel)
        {
            tables_.push_back(static_cast<uint8_t>(level));
            tables_.push_back(static_cast<uint8_t>(static_cast<uint16_t>(level) >> 8));
        }
    }
    shifts_ = tables_.size();
    tables_.insert(tables_.end(), std::begin(tables::kVolumeShift), std::end(tables::kVolumeShift));
    tables_.insert(tables_.end(), std::begin(tables::kChannelOrder), std::end(tables::kChannelOrder));
    fast_attack_ = tables_.size();
    tables_.insert(tables_.end(), std::begin(tables::kFastAttack), std::end(tables::kFastAttack));
}

void Driver::Start()
{
    channels_.resize(kChannelCount);
    original_volumes_.resize(kChannelCount);
    original_pans_.resize(kChannelCount);
    for (int i = 0; i < kChannelCount; i++)
    {
        Channel& channel = channels_[static_cast<std::size_t>(i)];
        channel.id = static_cast<uint16_t>(i);
        channel.twin = static_cast<uint16_t>(i);
        channel.sync = 0;
        channel.active = false;
    }
    locked_ = 0;
    weakly_locked_ = 0;
    for (int i = 0; i < kPlayerCount; i++)
    {
        players_[static_cast<std::size_t>(i)].active = false;
        players_[static_cast<std::size_t>(i)].id = static_cast<uint8_t>(i);
    }
    for (Track& track : tracks_)
    {
        track.active = false;
    }

    registers_->Write(kMixRegister, 1, registers_->Read(kMixRegister, 1) | 0x80);
    SetOutputSelector(0, 0, 0, 0);
    registers_->Write(kMasterVolumeRegister, 1, 0x7F);
}

void Driver::Frame(bool step, std::span<const CommandList> lists)
{
    UpdateChannels();
    for (const CommandList& list : lists)
    {
        for (const Command& command : list)
        {
            Run(command);
        }
        if (shared_work_ != 0)
        {
            Write32(shared_work_ + kFinishedCommandTag, Read32(shared_work_ + kFinishedCommandTag) + 1);
        }
    }
    StepPlayers(step);
    StepChannels(step);
    UpdateSharedWork();
    Random();
}

const Channel& Driver::GetChannel(int channel) const
{
    return channels_[static_cast<std::size_t>(channel)];
}

void Driver::GrowChannels(bool on)
{
    growing_ = on;
    if (on)
    {
        // The channels never move: a reference to one stays valid as others grow.
        channels_.reserve(kMostChannels);
    }
}

const Track& Driver::GetTrack(int track) const
{
    return tracks_[static_cast<std::size_t>(track)];
}

const Player& Driver::GetPlayer(int player) const
{
    return players_[static_cast<std::size_t>(player)];
}

uint8_t Driver::Read8(uint32_t address) const
{
    if (address < kMainMemory || address >= kMainMemoryEnd || main_memory_.empty())
    {
        return 0;
    }

    return main_memory_[(address - kMainMemory) % main_memory_.size()];
}

uint16_t Driver::Read16(uint32_t address) const
{
    return static_cast<uint16_t>(Read8(address) | Read8(address + 1) << 8);
}

uint32_t Driver::Read32(uint32_t address) const
{
    return Read16(address) | static_cast<uint32_t>(Read16(address + 2)) << 16;
}

void Driver::Write8(uint32_t address, uint8_t value)
{
    if (address >= kMainMemory && address < kMainMemoryEnd && !main_memory_.empty())
    {
        main_memory_[(address - kMainMemory) % main_memory_.size()] = value;
    }
}

void Driver::Write16(uint32_t address, uint16_t value)
{
    Write8(address, static_cast<uint8_t>(value));
    Write8(address + 1, static_cast<uint8_t>(value >> 8));
}

void Driver::Write32(uint32_t address, uint32_t value)
{
    Write16(address, static_cast<uint16_t>(value));
    Write16(address + 2, static_cast<uint16_t>(value >> 16));
}

uint16_t Driver::ReadU16(Track& track)
{
    const uint16_t low = ReadByte(track);
    return static_cast<uint16_t>(low | ReadByte(track) << 8);
}

void Driver::Run(const Command& command)
{
    const std::array<uint32_t, 4>& a = command.args;
    const auto player = [&a]
    {
        return static_cast<int>(a[0] % kPlayerCount);
    };
    switch (command.id)
    {
    case CommandId::kStartSequence:
        PrepareSequence(player(), a[1], a[2], a[3]);
        players_[static_cast<std::size_t>(player())].prepared = true;
        break;
    case CommandId::kStopSequence:
        StopSequence(player());
        break;
    case CommandId::kPrepareSequence:
        PrepareSequence(player(), a[1], a[2], a[3]);
        break;
    case CommandId::kStartPreparedSequence:
        players_[static_cast<std::size_t>(player())].prepared = true;
        break;
    case CommandId::kPauseSequence:
        PauseSequence(player(), a[1]);
        break;
    case CommandId::kSkipSequence:
        SkipSequence(player(), a[1]);
        break;
    case CommandId::kPlayerParameter:
        SetPlayerParameter(player(), a[1], a[2], static_cast<int>(a[3]));
        break;
    case CommandId::kTrackParameter:
        SetTrackParameter(static_cast<int>((a[0] & 0xFFFFFF) % kPlayerCount), a[1], a[2], a[3],
                          static_cast<uint8_t>(a[0] >> 24));
        break;
    case CommandId::kMuteTrack:
        SetTrackMute(player(), a[1], static_cast<int>(a[2]));
        break;
    case CommandId::kAllocatableChannels:
        SetAllocatableChannels(player(), a[1], a[2]);
        break;
    case CommandId::kPlayerLocalVariable:
        if (shared_work_ != 0)
        {
            Write16(shared_work_ + kPlayerWork + kPlayerWorkSize * a[0] + 2 * a[1], static_cast<uint16_t>(a[2]));
        }
        break;
    case CommandId::kGlobalVariable:
        if (shared_work_ != 0)
        {
            Write16(shared_work_ + kGlobalVariables + 2 * a[0], static_cast<uint16_t>(a[1]));
        }
        break;
    case CommandId::kStartTimer:
        StartTimer(a[0], a[1]);
        break;
    case CommandId::kStopTimer:
        StopTimer(a[0], a[1], static_cast<int>(a[3]));
        break;
    case CommandId::kSetUpPcmChannel:
        SetUpPcmChannel(static_cast<int>(a[0] & 0xF), a[1] & 0x7FFFFFF, static_cast<int>((a[3] >> 24) & 0x3),
                        static_cast<int>((a[3] >> 26) & 0x3), static_cast<int>(a[3] & 0xFFFF),
                        static_cast<int>(a[2] & 0x3FFFFF), static_cast<int>((a[2] >> 24) & 0x7F),
                        static_cast<int>((a[2] >> 22) & 0x3), static_cast<int>((a[0] >> 16) & 0xFFFF),
                        static_cast<int>((a[3] >> 16) & 0x7F));
        break;
    case CommandId::kSetUpPsgChannel:
        SetUpPsgChannel(static_cast<int>(a[0] & 0xF), static_cast<int>(a[3]), static_cast<int>(a[1] & 0x7F),
                        static_cast<int>((a[1] >> 8) & 0x3), static_cast<int>((a[2] >> 8) & 0xFFFF),
                        static_cast<int>(a[2] & 0x7F));
        break;
    case CommandId::kSetUpNoiseChannel:
        SetUpNoiseChannel(static_cast<int>(a[0] & 0xF), static_cast<int>(a[1] & 0x7F),
                          static_cast<int>((a[1] >> 8) & 0x3), static_cast<int>((a[2] >> 8) & 0xFFFF),
                          static_cast<int>(a[2] & 0x7F));
        break;
    case CommandId::kSetUpCapture:
        SetUpCapture(static_cast<int>((a[2] >> 31) & 1), static_cast<int>((a[2] >> 30) & 1), a[0],
                     static_cast<int>(a[1]), ((a[2] >> 29) & 1) != 0, static_cast<int>((a[2] >> 28) & 1),
                     static_cast<int>((a[2] >> 27) & 1));
        break;
    case CommandId::kChannelTimer:
        for (uint32_t i = 0, mask = a[0]; i < kChannelCount && mask != 0; i++, mask >>= 1)
        {
            if (mask & 1)
            {
                SetChannelTimer(static_cast<int>(i), static_cast<int>(a[1]));
            }
        }
        break;
    case CommandId::kChannelVolume:
        for (uint32_t i = 0, mask = a[0]; i < kChannelCount && mask != 0; i++, mask >>= 1)
        {
            if (mask & 1)
            {
                SetChannelVolume(static_cast<int>(i), static_cast<int>(a[1]), static_cast<int>(a[2]));
            }
        }
        break;
    case CommandId::kChannelPan:
        for (uint32_t i = 0, mask = a[0]; i < kChannelCount && mask != 0; i++, mask >>= 1)
        {
            if (mask & 1)
            {
                SetChannelPan(static_cast<int>(i), static_cast<int>(a[1]));
            }
        }
        break;
    case CommandId::kSurroundDecay:
        SetSurroundDecay(static_cast<int>(a[0]));
        break;
    case CommandId::kMasterVolume:
        registers_->Write(kMasterVolumeRegister, 1, static_cast<uint8_t>(a[0]));
        break;
    case CommandId::kMasterPan:
        SetMasterPan(static_cast<int>(a[0]));
        break;
    case CommandId::kOutputSelector:
        SetOutputSelector(static_cast<int>(a[0]), static_cast<int>(a[1]), static_cast<int>(a[2]),
                          static_cast<int>(a[3]));
        break;
    case CommandId::kLockChannels:
        LockChannels(a[0], a[1]);
        break;
    case CommandId::kUnlockChannels:
        if (a[1] & 1)
        {
            weakly_locked_ &= ~a[0];
        }
        else
        {
            locked_ &= ~a[0];
        }
        break;
    case CommandId::kStopUnlockedChannels:
        StopUnlockedChannels(a[0]);
        break;
    case CommandId::kSetSharedWork:
        shared_work_ = a[0];
        break;
    case CommandId::kInvalidateSequence:
        InvalidateSequence(a[0], a[1]);
        break;
    case CommandId::kInvalidateBank:
        InvalidateBank(a[0], a[1]);
        break;
    case CommandId::kInvalidateWave:
        InvalidateWave(a[0], a[1]);
        break;
    case CommandId::kSetUpAlarm:
    case CommandId::kReadDriverInfo:
        break;
    }
}

void Driver::StartTimer(uint32_t channels, uint32_t captures)
{
    for (int i = 0; i < kChannelCount && channels != 0; i++, channels >>= 1)
    {
        if (channels & 1)
        {
            registers_->Write(ChannelRegister(i, 0x3), 1, registers_->Read(ChannelRegister(i, 0x3), 1) | 0x80);
        }
    }
    if (captures & 1)
    {
        if (captures & 2)
        {
            registers_->Write(kCaptureControl, 2, registers_->Read(kCaptureControl, 2) | 0x8080);
        }
        else
        {
            registers_->Write(kCaptureControl, 1, registers_->Read(kCaptureControl, 1) | 0x80);
        }
    }
    else if (captures & 2)
    {
        registers_->Write(kCaptureControl + 1, 1, registers_->Read(kCaptureControl + 1, 1) | 0x80);
    }
    UpdateSharedWork();
}

void Driver::StopTimer(uint32_t channels, uint32_t captures, int hold)
{
    for (int i = 0; i < kChannelCount && channels != 0; i++, channels >>= 1)
    {
        if (channels & 1)
        {
            StopChannel(i, hold);
        }
    }
    if (captures & 1)
    {
        registers_->Write(kCaptureControl, 1, 0);
    }
    if (captures & 2)
    {
        registers_->Write(kCaptureControl + 1, 1, 0);
    }
    UpdateSharedWork();
}

void Driver::UpdateSharedWork()
{
    if (shared_work_ == 0)
    {
        return;
    }

    uint16_t active = 0;
    uint16_t capturing = 0;
    for (int i = 0; i < kChannelCount; i++)
    {
        if (IsChannelActive(i))
        {
            active = static_cast<uint16_t>(active | 1u << i);
        }
    }
    if (IsCaptureActive(0))
    {
        capturing |= 1;
    }
    if (IsCaptureActive(1))
    {
        capturing |= 2;
    }
    Write16(shared_work_ + kChannelStatus, active);
    Write16(shared_work_ + kCaptureStatus, capturing);
}

void Driver::SetRandomState(uint32_t state)
{
    random_ = state;
}

uint16_t Driver::Random()
{
    random_ = random_ * 1664525u + 1013904223u;
    return static_cast<uint16_t>(random_ >> 16);
}

} // namespace ntdlib::ntr::snd
