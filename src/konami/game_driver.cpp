// SPDX-License-Identifier: MIT

#include "ntdlib/konami/game_driver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/arm/cpu.h"

namespace ntdlib::konami
{
namespace
{

constexpr uint32_t kThumbReturn = 0x4770; // bx lr

// Timer 0's control lies after its reload, and DMA 1's registers after DMA 0's.
constexpr uint32_t kTimerStride = 4;
constexpr uint32_t kDmaStride = 12;
constexpr uint32_t kDmaControl = 10;
constexpr uint16_t kDmaEnable = 0x8000;

// The prescaler of a timer's control register.
constexpr uint32_t kTimerPrescale[4] = {1, 64, 256, 1024};
constexpr uint16_t kTimerOn = 0x0080;

constexpr uint32_t kSoundCntHHigh = agb::kRegSoundCntH + 1;
constexpr uint8_t kFifoAReset = 0x08;
constexpr uint8_t kFifoResets = 0x88; // both FIFOs' reset bits: they read back as 0

} // namespace

GameDriver::GameDriver(const Rom& rom, const GameRoutines& routines) : routines_(routines), system_(rom)
{
    // A ring mixer's per-frame entry ends by running the mixer. Mix() runs it instead. The other games' timer
    // interrupts run theirs at that point.
    if (routines_.mixer_call)
    {
        system_.Store(routines_.mixer_call & ~1u, 2, kThumbReturn);
    }

    system_.on_io_write = [this](uint32_t address, int size, uint32_t value)
    {
        if (!recording_)
        {
            return;
        }

        if (address >= agb::kRegSound1CntL && address < agb::kRegFifoA)
        {
            sound_writes_.push_back({address, uint8_t(size), value});
        }

        if ((address >= agb::kRegFifoA && address < agb::kRegFifoA + 8) || address == agb::kRegSoundCntH ||
            address == kSoundCntHHigh)
        {
            fifo_writes_.push_back({address, uint8_t(size), value});
        }

        const bool whole = address == agb::kRegSoundCntH && size == 2;
        if (whole || (address == kSoundCntHHigh && size == 1))
        {
            const uint8_t high = uint8_t(whole ? value >> 8 : value);
            if (high & kFifoResets)
            {
                recording_ = false;
                system_.Store(kSoundCntHHigh, 1, high & ~kFifoResets);
                recording_ = true;
            }
        }
    };

    Call(routines_.init | 1, routines_.init_args);
    for (const GameRoutines::Call& call : routines_.init_calls)
    {
        Call(call.address | 1, call.args);
    }
}

void GameDriver::Call(uint32_t address, std::span<const uint32_t> args)
{
    system_.Call(address, args);
}

void GameDriver::Play(int song, bool live)
{
    if (live)
    {
        if (!routines_.live_request)
        {
            throw std::runtime_error("the game has no live stage");
        }

        int entry = -1;
        for (int i = 0; i < routines_.live_entries && entry < 0; i++)
        {
            const uint32_t at = routines_.live_table + uint32_t(i * routines_.live_entry_size);
            if (system_.Load(at, routines_.live_entry_size) == uint32_t(song))
            {
                entry = i;
            }
        }
        if (entry < 0)
        {
            throw std::runtime_error("the live stage plays no song " + std::to_string(song));
        }

        std::vector<uint32_t> args{uint32_t(entry)};
        args.insert(args.end(), routines_.live_args.begin(), routines_.live_args.end());
        Call(routines_.live_request | 1, args);
    }
    else
    {
        std::vector<uint32_t> args{uint32_t(song)};
        args.insert(args.end(), routines_.request_args.begin(), routines_.request_args.end());
        Call(routines_.request_song | 1, args);
    }

    Call(routines_.start_song | 1);
}

bool GameDriver::Frame()
{
    sound_writes_.clear();
    fifo_writes_.clear();

    // The call of the per-frame entry stops at the output stage. The entry's return address is the output stage too,
    // and there the stack pointer has come back.
    arm::Cpu& cpu = system_.Cpu();
    const uint32_t stage = routines_.output_stage & ~1u;
    cpu.SetCpsr(uint32_t(arm::Mode::kSystem) | arm::psr::kI | arm::psr::kF);
    cpu.SetReg(13, agb::System::kStack);
    cpu.SetReg(14, stage | 1);
    cpu.Jump(routines_.frame | 1, arm::Mode::kSystem);
    const uint32_t saved_stop = cpu.StopAddress();
    bool reached = false;
    uint64_t left = agb::System::kCallCycles;
    for (;;)
    {
        cpu.SetStopAddress(stage);
        const uint64_t before = cpu.Cycles();
        const arm::StopReason reason = cpu.Run(left);
        const uint64_t ran = cpu.Cycles() - before;
        if (reason != arm::StopReason::kStopAddress || ran >= left)
        {
            cpu.SetStopAddress(saved_stop);
            throw std::runtime_error("the driver's per-frame entry didn't return");
        }

        left -= ran;
        const uint32_t sp = cpu.Reg(13);
        if (sp == agb::System::kStack)
        {
            break;
        }

        // The output stage comes once a frame. The records stay on the stack through it.
        reached = true;
        records_.assign(size_t(routines_.tracks), GameRecord());
        for (int t = 0; t < routines_.tracks; t++)
        {
            const uint32_t at = sp + uint32_t(routines_.record_offset + t * routines_.record_size);
            GameRecord& r = records_[size_t(t)];
            r.pitch = int16_t(system_.Load(at, 2));
            r.b2 = uint8_t(system_.Load(at + 2, 1));
            r.vol = uint8_t(system_.Load(at + 3, 1));
            r.trig = uint8_t(system_.Load(at + 4, 1));
            r.flags = uint8_t(system_.Load(at + 5, 1));
            r.key = uint16_t(system_.Load(at + 6, 2));
            if (routines_.record_pan >= 0)
            {
                r.pan_a = uint8_t(system_.Load(at + uint32_t(routines_.record_pan), 1));
                r.pan_b = uint8_t(system_.Load(at + uint32_t(routines_.record_pan) + 1, 1));
            }
        }

        // Step past the stop address.
        cpu.SetStopAddress(0xFFFFFFFF);
        const uint64_t step = cpu.Cycles();
        cpu.Run(1);
        left -= std::min(left, cpu.Cycles() - step);
    }

    cpu.SetStopAddress(saved_stop);
    return reached;
}

const std::vector<GameRecord>& GameDriver::Records() const
{
    return records_;
}

const std::vector<agb::RegisterWrite>& GameDriver::SoundWrites() const
{
    return sound_writes_;
}

uint32_t GameDriver::BlockCycles()
{
    const uint32_t reload = system_.Load(agb::kRegTimer0Reload, 2);
    const uint32_t control = system_.Load(agb::kRegTimer0Reload + 2, 2);
    return 16 * (0x10000 - reload) * kTimerPrescale[control & 3];
}

double GameDriver::Rate()
{
    return 16.0 * 16777216.0 / BlockCycles();
}

uint32_t GameDriver::FifoPeriod(int index)
{
    // SOUNDCNT_H chooses a FIFO's timer.
    const uint32_t timer = (system_.Load(agb::kRegSoundCntH, 2) >> (10 + 4 * index)) & 1;
    const uint32_t reload = system_.Load(agb::kRegTimer0Reload + kTimerStride * timer, 2);
    const uint32_t control = system_.Load(agb::kRegTimer0Reload + kTimerStride * timer + 2, 2);
    return control & kTimerOn ? (0x10000 - reload) * kTimerPrescale[control & 3] : 0;
}

void GameDriver::TickFifo(int index, std::vector<int8_t>& out)
{
    Fifo& fifo = fifos_[size_t(index)];
    if (!fifo.queue.empty())
    {
        fifo.queue.erase(fifo.queue.begin());
    }

    const uint32_t dma = agb::kRegDma0Source + kDmaStride * uint32_t(1 + index);
    if (fifo.queue.size() <= 16 && (system_.Load(dma + kDmaControl, 2) & kDmaEnable))
    {
        const uint32_t source = system_.Load(dma, 4);
        for (uint32_t i = 0; i < 16; i++)
        {
            const int8_t value = int8_t(system_.Load(source + i, 1));
            fifo.queue.push_back(value);
            out.push_back(value);
        }

        Call(routines_.fifo_irqs[size_t(index)] | 1);
    }
}

std::vector<int8_t> GameDriver::FifoStores(int fifo) const
{
    std::vector<int8_t> bytes;
    const uint32_t first = agb::kRegFifoA + 4 * uint32_t(fifo);
    for (const agb::RegisterWrite& w : fifo_writes_)
    {
        for (int i = 0; i < w.size; i++)
        {
            const uint32_t address = w.address + uint32_t(i);
            if (address >= first && address < first + 4)
            {
                bytes.push_back(int8_t(uint8_t(w.value >> (8 * i))));
            }
        }
    }

    return bytes;
}

const std::vector<FifoTimerChange>& GameDriver::TimerChanges() const
{
    return timer_changes_;
}

void GameDriver::Mix(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    fifo_a.clear();
    fifo_b.clear();
    if (routines_.fifo_irqs[0])
    {
        MixFifos(fifo_a, fifo_b);
        return;
    }

    const uint32_t block_cycles = BlockCycles();
    due_cycles_ += agb::kFrameCycles;
    const uint32_t blocks = uint32_t(due_cycles_ / block_cycles);
    due_cycles_ %= block_cycles;
    if (routines_.mixer_tick)
    {
        MixRing(blocks, fifo_a, fifo_b);
        return;
    }

    for (uint32_t b = 0; b < blocks; b++)
    {
        Call(routines_.mixer);
        for (uint32_t i = 0; i < 16; i++)
        {
            fifo_a.push_back(int8_t(system_.Load(routines_.fifo_buffer + i, 1)));
            fifo_b.push_back(int8_t(system_.Load(routines_.fifo_buffer + uint32_t(routines_.fifo_b) + i, 1)));
        }
    }
}

void GameDriver::MixFifos(std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    timer_changes_.clear();
    ApplyFifoWrites();
    RunFifo(0, fifo_a);
    RunFifo(1, fifo_b);

    std::stable_sort(timer_changes_.begin(), timer_changes_.end(),
                     [](const FifoTimerChange& a, const FifoTimerChange& b) { return a.cycle < b.cycle; });
}

void GameDriver::ApplyFifoWrites()
{
    // A reset of a FIFO empties it, and a store to a FIFO adds its bytes. The frame's stores take effect before its
    // cycles.
    for (const agb::RegisterWrite& w : fifo_writes_)
    {
        for (int i = 0; i < w.size; i++)
        {
            const uint32_t address = w.address + uint32_t(i);
            const uint8_t value = uint8_t(w.value >> (8 * i));
            if (address == kSoundCntHHigh)
            {
                for (int f = 0; f < 2; f++)
                {
                    if (value & (kFifoAReset << (4 * f)))
                    {
                        fifos_[size_t(f)].queue.clear();
                    }
                }
            }
            else if (address >= agb::kRegFifoA && address < agb::kRegFifoA + 8)
            {
                fifos_[(address - agb::kRegFifoA) / 4].queue.push_back(int8_t(value));
            }
        }
    }
}

void GameDriver::RunFifo(int f, std::vector<int8_t>& out)
{
    Fifo& fifo = fifos_[size_t(f)];
    uint32_t period = FifoPeriod(f);
    if (period && !fifo.ticking)
    {
        fifo.ticking = true;
        fifo.next_tick = period;
    }

    // An overflow at the frame's first cycle has already reached the hardware.
    const bool own_timer = ((system_.Load(agb::kRegSoundCntH, 2) >> (10 + 4 * f)) & 1) == uint32_t(f);
    if (own_timer && applied_periods_[size_t(f)] != period)
    {
        applied_periods_[size_t(f)] = period;
        timer_changes_.push_back({0, f, period,
                                  !period               ? 0
                                  : fifo.next_tick == 0 ? period
                                                        : uint32_t(fifo.next_tick)});
    }

    while (period && fifo.next_tick < int64_t(agb::kFrameCycles))
    {
        TickFifo(f, out);
        const uint32_t next_period = FifoPeriod(f);
        if (own_timer && next_period != period)
        {
            applied_periods_[size_t(f)] = next_period;
            timer_changes_.push_back({uint32_t(fifo.next_tick), f, next_period, next_period});
        }

        period = next_period;
        if (period)
        {
            fifo.next_tick += period;
        }
    }

    if (period)
    {
        fifo.next_tick -= int64_t(agb::kFrameCycles);
    }
    else
    {
        fifo.ticking = false;
    }
}

void GameDriver::MixRing(uint32_t blocks, std::vector<int8_t>& fifo_a, std::vector<int8_t>& fifo_b)
{
    // The DMA 1 interrupt moves a ring mixer's position on for each block, and the mixer fills the ring up to it.
    const uint32_t start = system_.Load(routines_.mixer_position, 2);
    if (blocks)
    {
        for (uint32_t b = 0; b < blocks; b++)
        {
            Call(routines_.mixer_tick | 1);
        }
        Call(routines_.mixer);
    }

    for (uint32_t i = 0; i < 16 * blocks; i++)
    {
        const uint32_t at = (start + i) % uint32_t(routines_.mixer_ring);
        fifo_a.push_back(int8_t(system_.Load(routines_.fifo_buffer + at, 1)));
        fifo_b.push_back(int8_t(system_.Load(routines_.fifo_buffer + uint32_t(routines_.fifo_b) + at, 1)));
    }
}

std::vector<uint8_t> GameDriver::VoiceBytes(int voice)
{
    std::vector<uint8_t> bytes(size_t(routines_.voice_size));
    const uint32_t at = routines_.voices + uint32_t(voice * routines_.voice_size);
    for (int i = 0; i < routines_.voice_size; i++)
    {
        bytes[size_t(i)] = uint8_t(system_.Load(at + uint32_t(i), 1));
    }

    return bytes;
}

std::vector<uint8_t> GameDriver::EchoBytes(int bus)
{
    std::vector<uint8_t> bytes(20);
    for (uint32_t i = 0; i < 20; i++)
    {
        bytes[i] = uint8_t(system_.Load(routines_.echo_buses + uint32_t(bus) * 20 + i, 1));
    }

    return bytes;
}

uint32_t GameDriver::EchoMask()
{
    return routines_.echo_mask ? system_.Load(routines_.echo_mask, 4) : 0;
}

agb::System& GameDriver::Gba()
{
    return system_;
}

} // namespace ntdlib::konami
