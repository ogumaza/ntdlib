// SPDX-License-Identifier: MIT

#include "ntdlib/brownie/game_driver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/arm/cpu.h"

namespace ntdlib::brownie
{
namespace
{

constexpr uint32_t kDma1Control = 0x040000C6;
constexpr uint32_t kDma2Control = 0x040000D2;
constexpr uint32_t kTimer0Control = 0x04000102;
constexpr uint32_t kTimerStride = 4;
constexpr uint32_t kInterruptEnable = 0x04000200;
constexpr uint32_t kFifoA = 0x040000A0;
constexpr uint16_t kTimerOn = 0x0080;
constexpr uint16_t kDmaOn = 0x8000;

// The BIOS's value of the sound bias register.
constexpr uint32_t kBiosBias = 0x0200;

// SOUNDCNT_H's bits that empty FIFO A and FIFO B.
constexpr uint16_t kFifoResets[2] = {0x0800, 0x8000};

// The points of each FIFO that a run of the mixer makes.
constexpr uint32_t kPoints = 16;

// A FIFO's limit, and the count at which it asks for more points.
constexpr size_t kFifoLimit = 32;
constexpr size_t kFifoAsks = 16;

// The bit of the first FIFO's interrupt in IE.
constexpr uint32_t kFirstInterrupt = 0x200;

} // namespace

GameDriver::GameDriver(const Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    system_.Store(agb::kRegSoundBias, 2, kBiosBias);
    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (address >= agb::kRegSound1CntL && address < kFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }

        if (address == agb::kRegSoundCntH && size == 2)
        {
            for (size_t f = 0; f < fifos_.size(); f++)
            {
                if (value & kFifoResets[f])
                {
                    fifos_[f].points.clear();
                }
            }
        }

        if (address >= kFifoA && address < kFifoA + 8)
        {
            OnFifoStore(int((address - kFifoA) / 4), size, value);
        }

        if (size == 2 && address >= kTimer0Control && address <= kTimer0Control + kTimerStride &&
            (address - kTimer0Control) % kTimerStride == 0)
        {
            OnTimerStore(int((address - kTimer0Control) / kTimerStride), value);
        }
    };

    Call(routines_.init);
    for (const GameRoutines::Copy& copy : routines_.copies)
    {
        for (uint32_t i = 0; i < copy.size; i++)
        {
            system_.Store(copy.to + i, 1, system_.Load(copy.from + i, 1));
        }
    }

    sound_writes_.clear();
    timer_changes_.clear();
}

void GameDriver::Call(uint32_t address, std::span<const uint32_t> args)
{
    arm::Cpu& cpu = system_.Cpu();
    for (int r = 0; r <= 12; r++)
    {
        cpu.SetReg(r, 0);
    }

    system_.Call(address, args);
}

void GameDriver::Play(int sound)
{
    const uint32_t args[] = {uint32_t(sound)};
    Call(routines_.request, args);
}

void GameDriver::Frame()
{
    sound_writes_.clear();
    now_ = frames_ * agb::kFrameCycles;
    Call(routines_.frame);
    voices_before_mix_.clear();
    for (int v = 0; v < routines_.voice_count; v++)
    {
        voices_before_mix_.push_back(VoiceBytes(v));
    }
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

uint32_t GameDriver::Period(int fifo)
{
    return 0x10000 - system_.Load(agb::kRegTimer0Reload + kTimerStride * uint32_t(fifo), 2);
}

void GameDriver::OnFifoStore(int fifo, int size, uint32_t value)
{
    HardwareFifo& f = fifos_[size_t(fifo)];
    for (int i = 0; i < size; i++)
    {
        if (f.points.size() < kFifoLimit)
        {
            const int8_t point = int8_t(uint8_t(value >> (8 * i)));
            f.points.push_back(point);
            fifo_bytes_[size_t(fifo)].push_back(point);
        }
    }
}

void GameDriver::OnTimerStore(int timer, uint32_t value)
{
    HardwareFifo& f = fifos_[size_t(timer)];
    const bool on = (value & kTimerOn) != 0;
    if (on == f.running)
    {
        return;
    }

    f.running = on;
    const uint32_t period = on ? Period(timer) : 0;
    const uint64_t frame_start = frames_ * agb::kFrameCycles;
    if (on)
    {
        f.next = now_ + period;
    }

    timer_changes_.push_back({uint32_t(now_ - frame_start), timer, period, period});
}

void GameDriver::PlayFifo(int fifo, uint64_t end)
{
    HardwareFifo& f = fifos_[size_t(fifo)];
    const uint32_t dma = fifo == 0 ? kDma1Control : kDma2Control;
    while (f.running && f.next < end)
    {
        now_ = f.next;
        if (!f.points.empty())
        {
            f.points.pop_front();
        }

        if (f.points.size() <= kFifoAsks && (system_.Load(dma, 2) & kDmaOn) &&
            (system_.Load(kInterruptEnable, 2) & (kFirstInterrupt << fifo)))
        {
            Call(routines_.fifo_irqs[size_t(fifo)]);
        }

        f.next += Period(fifo);
    }
}

void GameDriver::Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    if (routines_.fifo_irqs[0])
    {
        const uint64_t end = (frames_ + 1) * agb::kFrameCycles;
        for (int f = 0; f < 2; f++)
        {
            PlayFifo(f, end);
        }
    }
    else if ((system_.Load(kDma1Control, 2) & kDmaOn) && fifos_[0].running)
    {
        if (mix_start_ < 0)
        {
            mix_start_ = int64_t(frames_);
            mixed_ = 0;
        }

        const uint64_t cycles = uint64_t(kPoints) * Period(0);
        const uint64_t due = (frames_ - uint64_t(mix_start_) + 1) * agb::kFrameCycles / cycles;
        for (; mixed_ < due; mixed_++)
        {
            Call(routines_.mixer);
            for (uint32_t i = 0; i < 2 * kPoints; i++)
            {
                fifo_bytes_[i / kPoints].push_back(int8_t(system_.Load(routines_.fifo_points + i, 1)));
            }
        }
    }
    else
    {
        mix_start_ = -1;
    }

    fifo_a = std::move(fifo_bytes_[0]);
    fifo_b = std::move(fifo_bytes_[1]);
    fifo_bytes_ = {};
    frame_changes_ = std::move(timer_changes_);
    timer_changes_.clear();
    std::stable_sort(frame_changes_.begin(), frame_changes_.end(),
                     [](const FifoTimerChange& a, const FifoTimerChange& b) { return a.cycle < b.cycle; });
    frames_++;
}

const std::vector<FifoTimerChange>& GameDriver::TimerChanges() const
{
    return frame_changes_;
}

bool GameDriver::Mixing() const
{
    return mix_start_ >= 0;
}

std::vector<uint8_t> GameDriver::ChannelBytes(int channel)
{
    std::vector<uint8_t> bytes(0x38);
    const uint32_t at = routines_.channels + uint32_t(channel) * 0x38;
    for (uint32_t i = 0; i < 0x38; i++)
    {
        bytes[i] = uint8_t(system_.Load(at + i, 1));
    }

    return bytes;
}

std::vector<uint8_t> GameDriver::VoiceBytes(int voice, bool before_mix)
{
    if (before_mix)
    {
        return voices_before_mix_[size_t(voice)];
    }

    std::vector<uint8_t> bytes(16);
    const uint32_t at = routines_.voices + uint32_t(voice) * 16;
    for (uint32_t i = 0; i < 16; i++)
    {
        bytes[i] = uint8_t(system_.Load(at + i, 1));
    }

    return bytes;
}

std::vector<uint8_t> GameDriver::VariableBytes()
{
    std::vector<uint8_t> bytes;
    for (const auto& [from, to] : routines_.variables)
    {
        for (uint32_t at = from; at < to; at++)
        {
            bytes.push_back(uint8_t(system_.Load(at, 1)));
        }
    }

    return bytes;
}

bool GameDriver::Playing()
{
    for (int c = 0; c < routines_.channel_count; c++)
    {
        if (system_.Load(routines_.channels + uint32_t(c) * 0x38, 1) & 1)
        {
            return true;
        }
    }

    return routines_.fifo_irqs[0] ? fifos_[0].running || fifos_[1].running : mix_start_ >= 0;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace ntdlib::brownie
