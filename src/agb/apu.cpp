// SPDX-License-Identifier: MIT

#include "ntdlib/agb/apu.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "xsflib/bytes.h"

namespace ntdlib::agb
{
namespace
{
// The registers, as offsets from kRegSound1CntL.
constexpr uint32_t kNr10 = 0x00;
constexpr uint32_t kNr11 = 0x02;
constexpr uint32_t kNr12 = 0x03;
constexpr uint32_t kNr13 = 0x04;
constexpr uint32_t kNr14 = 0x05;
constexpr uint32_t kNr21 = 0x08;
constexpr uint32_t kNr22 = 0x09;
constexpr uint32_t kNr23 = 0x0C;
constexpr uint32_t kNr24 = 0x0D;
constexpr uint32_t kNr30 = 0x10;
constexpr uint32_t kNr31 = 0x12;
constexpr uint32_t kNr32 = 0x13;
constexpr uint32_t kNr33 = 0x14;
constexpr uint32_t kNr34 = 0x15;
constexpr uint32_t kNr41 = 0x18;
constexpr uint32_t kNr42 = 0x19;
constexpr uint32_t kNr43 = 0x1C;
constexpr uint32_t kNr44 = 0x1D;
constexpr uint32_t kNr50 = 0x20;
constexpr uint32_t kNr51 = 0x21;
constexpr uint32_t kSoundCntH = 0x22;
constexpr uint32_t kSoundCntX = 0x24;
constexpr uint32_t kSoundBias = 0x28;
constexpr uint32_t kWaveRam = 0x30;

// The registers that turning the sound off clears: NR10 to NR51. The Game Boy clears the same registers.
constexpr uint32_t kPsgRegisters = 0x22;

// The bits of the frequency registers' high bytes.
constexpr uint8_t kLengthEnable = 0x40;
constexpr uint8_t kRestart = 0x80;

// SOUNDCNT_X's bit that turns the sound on.
constexpr uint8_t kMasterEnable = 0x80;

// The squares' duties, the first step in bit 7: 12.5%, 25%, 50% and 75%.
constexpr uint8_t kDuties[4] = {0x01, 0x81, 0x87, 0x7E};

// The cycles of a step of the squares' and the wave's timers, for each step of 2048 less the frequency.
constexpr uint32_t kSquareStepCycles = 16;
constexpr uint32_t kWaveStepCycles = 8;

// The noise's clock shifts from 14 on stop its counter.
constexpr int kNoiseShifts = 14;

uint32_t Offset(uint32_t address)
{
    return address - kRegSound1CntL;
}

uint32_t SquarePeriod(uint16_t frequency)
{
    return (2048u - frequency) * kSquareStepCycles;
}

uint32_t WavePeriod(uint16_t frequency)
{
    return (2048u - frequency) * kWaveStepCycles;
}

// The noise's frequency is 524,288 Hz / r / 2^(s+1), with r = 0.5 for a ratio of 0.
uint32_t NoisePeriod(uint8_t nr43)
{
    const uint32_t ratio = nr43 & 7u;
    const uint32_t shift = nr43 >> 4;
    return ratio ? ratio << (shift + 6) : 1u << (shift + 5);
}

// Moves a timer of `period` cycles on by `cycles`. Returns the steps it took.
uint32_t Count(uint32_t& countdown, uint32_t period, uint32_t cycles)
{
    if (cycles < countdown)
    {
        countdown -= cycles;
        return 0;
    }
    cycles -= countdown;
    countdown = period - cycles % period;
    return 1 + cycles / period;
}

// Moves a timer of `period` cycles on by `cycles` as Count() does, and calls `step` with the cycles into the run at
// each of its steps.
template <typename Step>
void CountEach(uint32_t& countdown, uint32_t period, uint32_t cycles, Step step)
{
    uint32_t at = 0;
    while (countdown <= cycles - at)
    {
        at += countdown;
        countdown = period;
        step(at);
    }
    countdown -= cycles - at;
}

// The samples of the enhanced mode whose times lie before `cycle`.
uint64_t EnhancedSamplesBefore(uint64_t cycle)
{
    return (cycle * kEnhancedFrameSamples + kFrameCycles - 1) / kFrameCycles;
}
} // namespace

Apu::EnhancedState::EnhancedState()
    : steps{wave::BandLimitedSteps(kFrameCycles, kEnhancedFrameSamples),
            wave::BandLimitedSteps(kFrameCycles, kEnhancedFrameSamples)}
{
}

Apu::Apu()
{
    Registers()[kSoundBias] = 0x00;
    Registers()[kSoundBias + 1] = 0x02;
}

Apu::Apu(std::span<const uint8_t> registers)
{
    if (registers.size() != kSoundRegisterBytes)
    {
        throw std::invalid_argument("the sound registers take " + std::to_string(kSoundRegisterBytes) + " bytes");
    }
    Psg& psg = psgs_[0];
    std::copy(registers.begin(), registers.end(), psg.registers.begin());
    psg.registers[kSoundCntX] &= kMasterEnable;
    psg.registers[kSoundCntH + 1] &= 0x77; // the FIFOs' resets
    psg.squares[0].duty = uint8_t(psg.registers[kNr11] >> 6);
    psg.squares[1].duty = uint8_t(psg.registers[kNr21] >> 6);
    psg.squares[0].frequency = uint16_t(psg.registers[kNr13] | (psg.registers[kNr14] & 7) << 8);
    psg.squares[1].frequency = uint16_t(psg.registers[kNr23] | (psg.registers[kNr24] & 7) << 8);
    psg.wave.frequency = uint16_t(psg.registers[kNr33] | (psg.registers[kNr34] & 7) << 8);
    psg.squares[0].length = uint16_t(64 - (psg.registers[kNr11] & 63));
    psg.squares[1].length = uint16_t(64 - (psg.registers[kNr21] & 63));
    psg.wave.length = uint16_t(256 - psg.registers[kNr31]);
    psg.noise.length = uint16_t(64 - (psg.registers[kNr41] & 63));
    // The CPU reaches the bank that doesn't play.
    const size_t bank = (psg.registers[kNr30] >> 6 & 1) ^ 1;
    std::copy(registers.begin() + kWaveRam, registers.end(), psg.wave.banks[bank].begin());
}

int Apu::Rate() const
{
    const int resolution = Registers()[kSoundBias + 1] >> 6;
    return std::min(kBaseOutputRate << resolution, kMaxOutputRate);
}

uint8_t Apu::Register(uint32_t address) const
{
    size_t unit = 0;
    uint32_t offset = Offset(address);
    if (address >= kGrownPsgRegisters)
    {
        unit = 1 + (address - kGrownPsgRegisters) / kGrownPsgBytes;
        offset = (address - kGrownPsgRegisters) % kGrownPsgBytes;
        if (unit >= psgs_.size())
        {
            return 0;
        }
    }
    if (offset >= kSoundRegisterBytes)
    {
        return 0;
    }
    const Psg& psg = psgs_[unit];
    if (offset >= kWaveRam)
    {
        const size_t bank = (psg.registers[kNr30] >> 6 & 1) ^ 1;
        return psg.wave.banks[bank][offset - kWaveRam];
    }
    if (offset == kSoundCntX)
    {
        return uint8_t((psg.registers[kSoundCntX] & kMasterEnable) | (psg.squares[0].on ? 1 : 0) |
                       (psg.squares[1].on ? 2 : 0) | (psg.wave.on ? 4 : 0) | (psg.noise.on ? 8 : 0));
    }
    return psg.registers[offset];
}

void Apu::Store(uint32_t address, int size, uint32_t value)
{
    for (int i = 0; i < size; ++i)
    {
        const uint32_t byte_address = address + uint32_t(i);
        if (byte_address >= kRegSound1CntL && byte_address < kRegFifoA)
        {
            StoreByte(psgs_[0], true, Offset(byte_address), uint8_t(value >> (8 * i)));
        }
        else if (byte_address >= kGrownPsgRegisters &&
                 byte_address - kGrownPsgRegisters < uint32_t(kMostPsgUnits - 1) * kGrownPsgBytes)
        {
            const size_t unit = 1 + (byte_address - kGrownPsgRegisters) / kGrownPsgBytes;
            GrowUnits(unit);
            StoreByte(psgs_[unit], false, (byte_address - kGrownPsgRegisters) % kGrownPsgBytes,
                      uint8_t(value >> (8 * i)));
        }
    }
    SettleEnhanced();
}

void Apu::SetMuted(uint8_t mask)
{
    muted_ = mask;
    SettleEnhanced();
}

void Apu::SetMutedUnits(std::span<const uint8_t> units)
{
    for (size_t unit = 1; unit < psgs_.size(); ++unit)
    {
        psgs_[unit].muted = unit - 1 < units.size() ? uint8_t(units[unit - 1] & 15) : 0;
    }
    SettleEnhanced();
}

uint8_t Apu::UnitMuted(size_t unit) const
{
    return unit == 0 ? uint8_t(muted_ & 15) : psgs_[unit].muted;
}

int32_t& Apu::Peak(size_t unit, int channel, size_t side)
{
    return unit == 0 ? peaks_[size_t(channel)][side] : psgs_[unit].peaks[size_t(channel)][side];
}

void Apu::GrowUnits(size_t unit)
{
    if (unit < psgs_.size())
    {
        return;
    }
    psgs_.resize(unit + 1);
    if (enhanced_)
    {
        enhanced_->levels.resize(psgs_.size());
        enhanced_->gains.resize(psgs_.size());
    }
}

void Apu::PowerOff()
{
    for (Psg& psg : psgs_)
    {
        std::fill(psg.registers.begin(), psg.registers.begin() + kPsgRegisters, uint8_t(0));
        for (auto& square : psg.squares)
        {
            square = Square{};
        }
        const auto banks = psg.wave.banks;
        psg.wave = Wave{};
        psg.wave.banks = banks;
        psg.noise = Noise{};
        psg.sweep = Sweep{};
    }
}

void Apu::StoreByte(Psg& psg, bool console, uint32_t offset, uint8_t value)
{
    const bool powered = (Registers()[kSoundCntX] & kMasterEnable) != 0;
    if (offset >= kWaveRam)
    {
        const size_t bank = (psg.registers[kNr30] >> 6 & 1) ^ 1;
        psg.wave.banks[bank][offset - kWaveRam] = value;
        return;
    }
    if (!console && offset >= kPsgRegisters)
    {
        // A grown unit's sound control registers do nothing.
        psg.registers[offset] = value;
        return;
    }
    if (offset == kSoundCntX)
    {
        const bool on = (value & kMasterEnable) != 0;
        if (powered && !on)
        {
            PowerOff();
        }
        else if (!powered && on)
        {
            // Pan Docs: the frame sequencer starts again from step 0, and the squares from the first step of their
            // duties.
            step_ = 0;
            sequencer_ = kSequencerCycles;
            for (Psg& unit : psgs_)
            {
                for (Square& square : unit.squares)
                {
                    square.position = 0;
                }
            }
        }
        psg.registers[kSoundCntX] = value & kMasterEnable;
        return;
    }
    if (offset == kSoundCntH + 1)
    {
        // Bits 11 and 15 empty the FIFOs and read back as 0.
        for (int fifo = 0; fifo < 2; ++fifo)
        {
            if (value & (0x08 << (4 * fifo)))
            {
                fifos_[size_t(fifo)].samples.clear();
                fifos_[size_t(fifo)].output = 0;
            }
        }
        psg.registers[offset] = value & 0x77;
        return;
    }
    if (offset < kPsgRegisters && !powered)
    {
        return;
    }
    psg.registers[offset] = value;
    switch (offset)
    {
    case kNr11:
    case kNr21:
        {
            Square& square = psg.squares[offset == kNr11 ? 0 : 1];
            square.duty = uint8_t(value >> 6);
            square.length = uint16_t(64 - (value & 63));
            break;
        }
    case kNr12:
    case kNr22:
        if ((value & 0xF8) == 0)
        {
            psg.squares[offset == kNr12 ? 0 : 1].on = false;
        }
        break;
    case kNr13:
    case kNr23:
        {
            Square& square = psg.squares[offset == kNr13 ? 0 : 1];
            square.frequency = uint16_t((square.frequency & 0x700) | value);
            break;
        }
    case kNr14:
    case kNr24:
        {
            const int n = offset == kNr14 ? 0 : 1;
            psg.squares[size_t(n)].frequency = uint16_t((psg.squares[size_t(n)].frequency & 0xFF) | (value & 7) << 8);
            if (value & kRestart)
            {
                StartSquare(psg, n);
            }
            break;
        }
    case kNr30:
        if ((value & 0x80) == 0)
        {
            StopWave(psg);
        }
        break;
    case kNr31:
        psg.wave.length = uint16_t(256 - value);
        break;
    case kNr33:
        psg.wave.frequency = uint16_t((psg.wave.frequency & 0x700) | value);
        break;
    case kNr34:
        psg.wave.frequency = uint16_t((psg.wave.frequency & 0xFF) | (value & 7) << 8);
        if (value & kRestart)
        {
            StartWave(psg);
        }
        break;
    case kNr41:
        psg.noise.length = uint16_t(64 - (value & 63));
        break;
    case kNr42:
        if ((value & 0xF8) == 0)
        {
            psg.noise.on = false;
        }
        break;
    case kNr44:
        if (value & kRestart)
        {
            StartNoise(psg);
        }
        break;
    default:
        break;
    }
}

void Apu::StartSquare(Psg& psg, int n)
{
    Square& square = psg.squares[size_t(n)];
    const uint8_t envelope = psg.registers[n == 0 ? kNr12 : kNr22];
    square.on = (envelope & 0xF8) != 0;
    if (square.length == 0)
    {
        square.length = 64;
    }
    square.countdown = SquarePeriod(square.frequency);
    square.envelope = Envelope{uint8_t(envelope >> 4), uint8_t(envelope & 7), (envelope & 8) != 0, 0};
    if (n == 0)
    {
        // Pan Docs: the sweep copies the frequency, and runs while its time or its shift isn't 0. With a shift, it
        // works out the next frequency at once, and a frequency past 2047 turns the channel off.
        const uint8_t sweep = psg.registers[kNr10];
        psg.sweep.shadow = square.frequency;
        psg.sweep.ticks = 0;
        psg.sweep.on = (sweep & 0x77) != 0;
        if ((sweep & 7) != 0 && SweepTarget(psg) > 0x7FF)
        {
            square.on = false;
        }
    }
}

void Apu::StartWave(Psg& psg)
{
    psg.wave.on = (psg.registers[kNr30] & 0x80) != 0;
    if (psg.wave.length == 0)
    {
        psg.wave.length = 256;
    }
    // GBATEK: the wave RAM is a shift register, with no pointer to the digit that plays. A restart leaves the digit
    // that plays as it was, and the next digit comes a whole step later. mGBA 0.11.0 does the same.
    psg.wave.countdown = WavePeriod(psg.wave.frequency);
}

void Apu::StopWave(Psg& psg)
{
    // mGBA 0.11.0 holds the wave channel's last level until the channel plays again.
    if (psg.wave.on)
    {
        psg.wave.held = uint8_t(Level(psg, 2));
        psg.wave.on = false;
    }
}

void Apu::StartNoise(Psg& psg)
{
    const uint8_t envelope = psg.registers[kNr42];
    psg.noise.on = (envelope & 0xF8) != 0;
    if (psg.noise.length == 0)
    {
        psg.noise.length = 64;
    }
    // GBATEK: the counter starts at 0x4000 with 15 bits and 0x40 with 7. GBATEK doesn't give the output before the
    // counter's first step. mGBA 0.11.0 stays silent until that step. The channel therefore starts with a carry.
    psg.noise.lfsr = (psg.registers[kNr43] & 8) ? 0x40 : 0x4000;
    psg.noise.carry = true;
    psg.noise.countdown = NoisePeriod(psg.registers[kNr43]);
    psg.noise.envelope = Envelope{uint8_t(envelope >> 4), uint8_t(envelope & 7), (envelope & 8) != 0, 0};
}

uint16_t Apu::SweepTarget(const Psg& psg)
{
    const uint8_t sweep = psg.registers[kNr10];
    const uint16_t change = uint16_t(psg.sweep.shadow >> (sweep & 7));
    return (sweep & 8) ? uint16_t(psg.sweep.shadow - change) : uint16_t(psg.sweep.shadow + change);
}

void Apu::StepSequencer()
{
    for (Psg& psg : psgs_)
    {
        StepUnit(psg);
    }
    step_ = uint8_t((step_ + 1) & 7);
}

void Apu::StepUnit(Psg& psg)
{
    if (step_ % 2 == 0)
    {
        // The length counters, at 256 Hz.
        const auto count = [&](bool& on, uint16_t& length, uint32_t control)
        {
            if ((psg.registers[control] & kLengthEnable) && length > 0 && --length == 0)
            {
                on = false;
            }
        };
        count(psg.squares[0].on, psg.squares[0].length, kNr14);
        count(psg.squares[1].on, psg.squares[1].length, kNr24);
        if ((psg.registers[kNr34] & kLengthEnable) && psg.wave.length > 0 && --psg.wave.length == 0)
        {
            StopWave(psg);
        }
        count(psg.noise.on, psg.noise.length, kNr44);
    }
    if (step_ == 2 || step_ == 6)
    {
        // The sweep, at 128 Hz: X(t) = X(t-1) +/- X(t-1) / 2^n every time units of 7.8 ms.
        const uint8_t sweep = psg.registers[kNr10];
        const uint8_t time = (sweep >> 4) & 7;
        if (psg.sweep.on && time != 0 && ++psg.sweep.ticks >= time)
        {
            psg.sweep.ticks = 0;
            const uint16_t target = SweepTarget(psg);
            if (target > 0x7FF)
            {
                psg.squares[0].on = false;
            }
            else if (sweep & 7)
            {
                psg.sweep.shadow = target;
                psg.squares[0].frequency = target;
                psg.registers[kNr13] = uint8_t(target);
                psg.registers[kNr14] = uint8_t((psg.registers[kNr14] & 0xF8) | target >> 8);
                if (SweepTarget(psg) > 0x7FF)
                {
                    psg.squares[0].on = false;
                }
            }
        }
    }
    if (step_ == 7)
    {
        // The envelopes, at 64 Hz: a step every n / 64 s.
        for (Envelope* envelope : {&psg.squares[0].envelope, &psg.squares[1].envelope, &psg.noise.envelope})
        {
            if (envelope->period != 0 && ++envelope->ticks >= envelope->period)
            {
                envelope->ticks = 0;
                if (envelope->up && envelope->level < 15)
                {
                    ++envelope->level;
                }
                else if (!envelope->up && envelope->level > 0)
                {
                    --envelope->level;
                }
            }
        }
    }
}

void Apu::SetSequencer(int step, uint32_t cycles)
{
    if (step < 0 || step > 7 || cycles == 0 || cycles > kSequencerCycles)
    {
        throw std::invalid_argument("the frame sequencer takes a step of 0 to 7, from 1 to " +
                                    std::to_string(kSequencerCycles) + " cycles away");
    }
    step_ = uint8_t(step);
    sequencer_ = cycles;
}

void Apu::SetTimer(int timer, uint32_t period, uint32_t first)
{
    timers_[size_t(timer)] = Timer{period, first};
}

void Apu::Queue(int fifo, std::span<const int8_t> samples)
{
    fifos_[size_t(fifo)].samples.insert(fifos_[size_t(fifo)].samples.end(), samples.begin(), samples.end());
}

void Apu::StartEnhanced()
{
    // The FIFOs start with silence for the band-limited steps' delay. The steps start at the channels' present levels.
    enhanced_.emplace();
    enhanced_->samples = EnhancedSamplesBefore(cycle_);
    enhanced_->levels.resize(psgs_.size());
    enhanced_->gains.resize(psgs_.size());
    for (std::deque<float>& fifo : enhanced_->fifos)
    {
        fifo.assign(size_t(wave::BandLimitedSteps::kHalfWidth), 0.0f);
    }
    enhanced_->stale = true;
}

void Apu::QueueEnhanced(int fifo, std::span<const float> samples)
{
    if (!enhanced_)
    {
        throw std::logic_error("the model isn't in the enhanced mode");
    }
    std::deque<float>& queue = enhanced_->fifos[size_t(fifo)];
    queue.insert(queue.end(), samples.begin(), samples.end());
}

void Apu::SetEnhancedGains()
{
    // A side's gain for each step of a channel's level: 2 * (master + 1) >> share of the 10-bit range, times 64. A
    // unit's NR51 sends its channels to the sides. The console's registers give the rest.
    const std::array<uint8_t, kSoundRegisterBytes>& registers = Registers();
    const bool powered = (registers[kSoundCntX] & kMasterEnable) != 0;
    const uint16_t control = xsflib::LoadLe16(&registers[kSoundCntH]);
    constexpr int kShares[4] = {2, 1, 0, 0};
    for (size_t unit = 0; unit < psgs_.size(); ++unit)
    {
        const uint8_t nr51 = psgs_[unit].registers[kNr51];
        const uint8_t muted = UnitMuted(unit);
        for (int side = 0; side < 2; ++side)
        {
            const uint8_t enables = uint8_t(side == 0 ? nr51 >> 4 : nr51 & 15);
            const int master = (side == 0 ? registers[kNr50] >> 4 : registers[kNr50]) & 7;
            for (int c = 0; c < 4; ++c)
            {
                const bool on = powered && (enables >> c & 1) && !(muted >> c & 1);
                enhanced_->gains[unit][size_t(side)][size_t(c)] = on ? (128 * (master + 1)) >> kShares[control & 3] : 0;
            }
        }
    }
}

void Apu::SettleEnhanced()
{
    // After a store, a step of the frame sequencer or a change of the mutes: each side's steps go to the sum of the
    // channels' levels at their gains, at the present cycle.
    if (!enhanced_ || enhanced_->stale)
    {
        return;
    }
    SetEnhancedGains();
    for (size_t unit = 0; unit < psgs_.size(); ++unit)
    {
        for (int c = 0; c < 4; ++c)
        {
            enhanced_->levels[unit][size_t(c)] = Level(psgs_[unit], c);
        }
    }
    for (size_t side = 0; side < 2; ++side)
    {
        int64_t target = 0;
        for (size_t unit = 0; unit < psgs_.size(); ++unit)
        {
            for (int c = 0; c < 4; ++c)
            {
                const int32_t part = enhanced_->gains[unit][side][size_t(c)] * enhanced_->levels[unit][size_t(c)];
                target += part;
                if (metering_)
                {
                    int32_t& peak = Peak(unit, c, side);
                    peak = std::max(peak, part);
                }
            }
        }
        wave::BandLimitedSteps& steps = enhanced_->steps[side];
        const double change = double(target) - steps.Level();
        if (change != 0.0)
        {
            steps.Step(cycle_, float(change));
        }
    }
}

void Apu::ChangeLevel(size_t unit, int channel, uint64_t cycle)
{
    const int level = Level(psgs_[unit], channel);
    int& held = enhanced_->levels[unit][size_t(channel)];
    if (level == held)
    {
        return;
    }
    for (size_t side = 0; side < 2; ++side)
    {
        const int32_t gain = enhanced_->gains[unit][side][size_t(channel)];
        if (gain != 0)
        {
            enhanced_->steps[side].Step(cycle, float(gain * (level - held)));
            if (metering_)
            {
                int32_t& peak = Peak(unit, channel, side);
                peak = std::max(peak, gain * level);
            }
        }
    }
    held = level;
}

void Apu::TakeEnhancedSamples(std::vector<float>* out)
{
    // The samples whose times now lie before the present cycle. Each takes one sample from each FIFO.
    EnhancedState& enhanced = *enhanced_;
    const uint64_t end = EnhancedSamplesBefore(cycle_);
    const size_t count = size_t(end - enhanced.samples);
    enhanced.samples = end;
    const auto take = [](std::deque<float>& fifo)
    {
        if (fifo.empty())
        {
            return 0.0f;
        }
        const float sample = fifo.front();
        fifo.pop_front();
        return sample;
    };
    if (!out)
    {
        for (size_t i = 0; i < count; ++i)
        {
            take(enhanced.fifos[0]);
            take(enhanced.fifos[1]);
        }
        return;
    }

    std::array<std::vector<float>, 2> psg;
    for (size_t side = 0; side < 2; ++side)
    {
        psg[side].resize(count);
        enhanced.steps[side].Read(psg[side].data(), count);
    }

    // Side 0 is the left. The left takes SOUNDCNT_H's odd enables.
    const std::array<uint8_t, kSoundRegisterBytes>& registers = Registers();
    const bool powered = (registers[kSoundCntX] & kMasterEnable) != 0;
    const uint16_t control = xsflib::LoadLe16(&registers[kSoundCntH]);
    std::array<std::array<float, 2>, 2> volumes{}; // each FIFO's volume on each side, in steps of a 16-bit sample
    for (size_t f = 0; f < 2; ++f)
    {
        for (size_t side = 0; side < 2; ++side)
        {
            const int enable = 8 + 4 * int(f) + (side == 0 ? 1 : 0);
            const bool on = powered && (control >> enable & 1) && !(muted_ >> (4 + f) & 1);
            volumes[f][side] = on ? float(((control >> (2 + f) & 1) ? 4 : 2) * 64) : 0.0f;
        }
    }
    for (size_t i = 0; i < count; ++i)
    {
        const std::array<float, 2> samples = {take(enhanced.fifos[0]), take(enhanced.fifos[1])};
        for (size_t side = 0; side < 2; ++side)
        {
            float sum = psg[side][i];
            for (size_t f = 0; f < 2; ++f)
            {
                const float part = samples[f] * volumes[f][side];
                sum += part;
                if (metering_)
                {
                    peaks_[4 + f][side] = std::max(peaks_[4 + f][side], int32_t(std::fabs(part)));
                }
            }
            out->push_back(sum * (1.0f / 32768.0f));
        }
    }
}

void Apu::OverflowTimers()
{
    const uint16_t control = xsflib::LoadLe16(&Registers()[kSoundCntH]);
    for (int t = 0; t < 2; ++t)
    {
        Timer& timer = timers_[size_t(t)];
        if (timer.period == 0 || timer.countdown != 0)
        {
            continue;
        }
        timer.countdown = timer.period;
        for (int f = 0; f < 2; ++f)
        {
            Fifo& fifo = fifos_[size_t(f)];
            if (((control >> (10 + 4 * f)) & 1) == uint32_t(t) && !fifo.samples.empty())
            {
                fifo.output = fifo.samples.front();
                fifo.samples.pop_front();
            }
        }
    }
}

void Apu::AdvanceEnhanced(size_t unit, uint32_t cycles)
{
    // As Advance(), with each change of a channel's level at the cycle of the step that makes it.
    Psg& psg = psgs_[unit];
    for (int n = 0; n < 2; ++n)
    {
        Square& square = psg.squares[size_t(n)];
        if (square.on)
        {
            CountEach(square.countdown, SquarePeriod(square.frequency), cycles,
                      [&](uint32_t at)
                      {
                          square.position = uint8_t((square.position + 1) & 7);
                          ChangeLevel(unit, n, cycle_ + at);
                      });
        }
    }
    if (psg.wave.on)
    {
        const uint32_t digits = (psg.registers[kNr30] & 0x20) ? 64 : 32;
        CountEach(psg.wave.countdown, WavePeriod(psg.wave.frequency), cycles,
                  [&](uint32_t at)
                  {
                      psg.wave.position = uint8_t((psg.wave.position + 1) % digits);
                      ChangeLevel(unit, 2, cycle_ + at);
                  });
    }
    const uint8_t nr43 = psg.registers[kNr43];
    if (psg.noise.on && (nr43 >> 4) < kNoiseShifts)
    {
        const uint16_t taps = (nr43 & 8) ? 0x60 : 0x6000;
        CountEach(psg.noise.countdown, NoisePeriod(nr43), cycles,
                  [&](uint32_t at)
                  {
                      psg.noise.carry = (psg.noise.lfsr & 1) != 0;
                      psg.noise.lfsr = uint16_t(psg.noise.lfsr >> 1);
                      if (psg.noise.carry)
                      {
                          psg.noise.lfsr ^= taps;
                      }
                      ChangeLevel(unit, 3, cycle_ + at);
                  });
    }
}

void Apu::Advance(Psg& psg, uint32_t cycles)
{
    for (Square& square : psg.squares)
    {
        if (square.on)
        {
            const uint32_t steps = Count(square.countdown, SquarePeriod(square.frequency), cycles);
            square.position = uint8_t((square.position + steps) & 7);
        }
    }
    if (psg.wave.on)
    {
        const uint32_t digits = (psg.registers[kNr30] & 0x20) ? 64 : 32;
        const uint32_t steps = Count(psg.wave.countdown, WavePeriod(psg.wave.frequency), cycles);
        psg.wave.position = uint8_t((psg.wave.position + steps) % digits);
    }
    const uint8_t nr43 = psg.registers[kNr43];
    if (psg.noise.on && (nr43 >> 4) < kNoiseShifts)
    {
        const uint32_t steps = Count(psg.noise.countdown, NoisePeriod(nr43), cycles);
        const uint16_t taps = (nr43 & 8) ? 0x60 : 0x6000;
        for (uint32_t k = 0; k < steps; ++k)
        {
            psg.noise.carry = (psg.noise.lfsr & 1) != 0;
            psg.noise.lfsr = uint16_t(psg.noise.lfsr >> 1);
            if (psg.noise.carry)
            {
                psg.noise.lfsr ^= taps;
            }
        }
    }
}

bool Apu::PsgSilent() const
{
    const auto silent = [](bool on, const Envelope& envelope)
    {
        return !on || (envelope.level == 0 && !(envelope.up && envelope.period != 0));
    };

    for (const Psg& psg : psgs_)
    {
        constexpr uint8_t kWaveVolumeBits = 0xE0;
        if (!silent(psg.squares[0].on, psg.squares[0].envelope) ||
            !silent(psg.squares[1].on, psg.squares[1].envelope) || !silent(psg.noise.on, psg.noise.envelope) ||
            (psg.wave.on && (psg.registers[kNr32] & kWaveVolumeBits) != 0))
        {
            return false;
        }
    }

    return true;
}

int Apu::Level(const Psg& psg, int channel)
{
    switch (channel)
    {
    case 0:
    case 1:
        {
            const Square& square = psg.squares[size_t(channel)];
            return square.on && (kDuties[square.duty] >> (7 - square.position) & 1) ? square.envelope.level : 0;
        }
    case 2:
        {
            if (!psg.wave.on)
            {
                return psg.wave.held;
            }
            const uint8_t nr30 = psg.registers[kNr30];
            const uint32_t digits = (nr30 & 0x20) ? 64 : 32;
            const uint32_t played = (psg.wave.position + digits - 1) % digits;
            const size_t bank = ((nr30 >> 6) ^ (played >> 5)) & 1;
            const uint8_t byte = psg.wave.banks[bank][(played & 31) >> 1];
            const int digit = (played & 1) ? byte & 15 : byte >> 4;
            const uint8_t nr32 = psg.registers[kNr32];
            if (nr32 & 0x80)
            {
                return digit * 3 >> 2;
            }
            constexpr int kShifts[4] = {4, 0, 1, 2};
            return digit >> kShifts[(nr32 >> 5) & 3];
        }
    default:
        return psg.noise.on && !psg.noise.carry ? psg.noise.envelope.level : 0;
    }
}

int16_t Apu::Output(int side)
{
    const std::array<uint8_t, kSoundRegisterBytes>& registers = Registers();
    const int bias = xsflib::LoadLe16(&registers[kSoundBias]) & 0x3FE;
    if ((registers[kSoundCntX] & kMasterEnable) == 0)
    {
        return 0;
    }
    // Side 0 is the left. The left takes the high bits of NR50 and NR51 and SOUNDCNT_H's odd enables. A unit's NR51
    // sends its channels to the sides.
    const int master = (side == 0 ? registers[kNr50] >> 4 : registers[kNr50]) & 7;
    const uint16_t control = xsflib::LoadLe16(&registers[kSoundCntH]);
    constexpr int kShares[4] = {2, 1, 0, 0}; // 25%, 50%, 100%, and 100% for the prohibited setting
    int levels = 0;
    for (size_t unit = 0; unit < psgs_.size(); ++unit)
    {
        const uint8_t nr51 = psgs_[unit].registers[kNr51];
        const uint8_t enables = uint8_t(side == 0 ? nr51 >> 4 : nr51 & 15);
        const uint8_t muted = UnitMuted(unit);
        for (int c = 0; c < 4; ++c)
        {
            if ((enables >> c & 1) && !(muted >> c & 1))
            {
                const int level = Level(psgs_[unit], c);
                levels += level;
                if (metering_)
                {
                    int32_t& peak = Peak(unit, c, size_t(side));
                    peak = std::max(peak, ((level * 2 * (master + 1)) >> kShares[control & 3]) * 64);
                }
            }
        }
    }
    int sum = (levels * 2 * (master + 1)) >> kShares[control & 3];
    for (int f = 0; f < 2; ++f)
    {
        const int enable = 8 + 4 * f + (side == 0 ? 1 : 0);
        if ((control >> enable & 1) && !(muted_ >> (4 + f) & 1))
        {
            const int part = fifos_[size_t(f)].output * ((control >> (2 + f) & 1) ? 4 : 2);
            sum += part;
            if (metering_)
            {
                int32_t& peak = peaks_[size_t(4 + f)][size_t(side)];
                peak = std::max(peak, std::abs(part) * 64);
            }
        }
    }
    const int resolution = registers[kSoundBias + 1] >> 6;
    const int value = std::clamp(bias + sum, 0, 0x3FF) & ~((2 << resolution) - 1);
    return int16_t(std::clamp((value - bias) * 64, -32768, 32767));
}

void Apu::Run(uint64_t cycles, std::vector<int16_t>& out)
{
    if (enhanced_)
    {
        throw std::logic_error("the model is in the enhanced mode");
    }
    RunCycles(cycles, &out, nullptr);
}

void Apu::Run(uint64_t cycles, std::vector<float>& out)
{
    if (!enhanced_)
    {
        throw std::logic_error("the model isn't in the enhanced mode");
    }
    RunCycles(cycles, nullptr, &out);
}

void Apu::Run(uint64_t cycles)
{
    RunCycles(cycles, nullptr, nullptr);
}

void Apu::RunCycles(uint64_t cycles, std::vector<int16_t>* out, std::vector<float>* enhanced_out)
{
    // In the enhanced mode, a run with output after one without starts the steps again at the channels' levels. A run
    // without output leaves them behind.
    const bool stepping = enhanced_ && enhanced_out;
    if (stepping && enhanced_->stale)
    {
        SetEnhancedGains();
        for (size_t side = 0; side < 2; ++side)
        {
            int64_t target = 0;
            for (size_t unit = 0; unit < psgs_.size(); ++unit)
            {
                for (int c = 0; c < 4; ++c)
                {
                    enhanced_->levels[unit][size_t(c)] = Level(psgs_[unit], c);
                    target += enhanced_->gains[unit][side][size_t(c)] * enhanced_->levels[unit][size_t(c)];
                }
            }
            enhanced_->steps[side].Restart(enhanced_->samples, double(target));
        }
        enhanced_->stale = false;
    }
    if (enhanced_ && !enhanced_out)
    {
        enhanced_->stale = true;
    }
    if (stepping && metering_)
    {
        for (size_t side = 0; side < 2; ++side)
        {
            for (size_t unit = 0; unit < psgs_.size(); ++unit)
            {
                for (int c = 0; c < 4; ++c)
                {
                    int32_t& peak = Peak(unit, c, side);
                    peak = std::max(peak, enhanced_->gains[unit][side][size_t(c)] * enhanced_->levels[unit][size_t(c)]);
                }
            }
        }
    }

    while (true)
    {
        // The events of the present cycle: the timers' overflows and the frame sequencer, then the stores that come
        // between runs, then the output. The output of the cycle where a run ends comes in the next run.
        OverflowTimers();
        if (sequencer_ == 0)
        {
            StepSequencer();
            sequencer_ = kSequencerCycles;
            SettleEnhanced();
        }
        if (cycles == 0)
        {
            break;
        }
        if (until_output_ == 0)
        {
            if (out)
            {
                out->push_back(Output(0));
                out->push_back(Output(1));
            }
            until_output_ = uint32_t(kSecondCycles / uint64_t(Rate()));
        }
        uint64_t step = std::min<uint64_t>({cycles, sequencer_, until_output_});
        for (Timer& timer : timers_)
        {
            if (timer.period != 0)
            {
                step = std::min<uint64_t>(step, timer.countdown);
            }
        }
        const uint32_t dt = uint32_t(step);
        for (size_t unit = 0; unit < psgs_.size(); ++unit)
        {
            if (stepping)
            {
                AdvanceEnhanced(unit, dt);
            }
            else
            {
                Advance(psgs_[unit], dt);
            }
        }
        for (Timer& timer : timers_)
        {
            if (timer.period != 0)
            {
                timer.countdown -= dt;
            }
        }
        sequencer_ -= dt;
        until_output_ -= dt;
        cycle_ += dt;
        cycles -= dt;
    }
    if (enhanced_)
    {
        TakeEnhancedSamples(enhanced_out);
    }
}

} // namespace ntdlib::agb
