// SPDX-License-Identifier: MIT

// The ARM interpreter's run loop, register banks and exceptions, and the Cpu class over them.

#include "ntdlib/arm/cpu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "xsflib/text.h"

#include "arm/cpu_impl.h"

namespace ntdlib::arm
{
namespace detail
{
namespace
{

// The CPSR bits that exist on each architecture: the flags, v5's Q flag and the control bits.
constexpr uint32_t kArmV4TPsrBits = 0xF00000FF;
constexpr uint32_t kArmV5TEPsrBits = 0xF80000FF;

// The reset state: supervisor mode with IRQs and FIQs disabled.
constexpr uint32_t kResetCpsr = static_cast<uint32_t>(Mode::kSupervisor) | psr::kI | psr::kF;

} // namespace

Core::Core(Cpu& cpu, Bus& bus, Architecture architecture)
    : cpu_(cpu),
      bus_(bus),
      architecture_(architecture),
      v5_(architecture == Architecture::kArmV5TE),
      arm_handlers_(ArmHandlers()),
      thumb_handlers_(ThumbHandlers()),
      cpsr_(kResetCpsr),
      psr_bits_(v5_ ? kArmV5TEPsrBits : kArmV4TPsrBits)
{
    if (v5_)
    {
        // GBATEK: bits 3 to 6 always read as set. The vectors start high.
        control_ = 0x78 | control::kHighVectors;
        load_interworks_ = true;
        itcm_.storage.assign(0x8000, 0);
        dtcm_.storage.assign(0x4000, 0);
        UpdateTcms();
    }
}

int Core::BankOf(uint32_t mode) const
{
    switch (static_cast<Mode>(mode))
    {
    case Mode::kFiq:
        return kFiqBank;
    case Mode::kIrq:
        return kIrqBank;
    case Mode::kSupervisor:
        return kSupervisorBank;
    case Mode::kAbort:
        return kAbortBank;
    case Mode::kUndefined:
        return kUndefinedBank;
    default:
        return kUserBank;
    }
}

void Core::SwitchBank(int from, int to)
{
    r13_r14_[from] = {regs_[13], regs_[14]};
    regs_[13] = r13_r14_[to][0];
    regs_[14] = r13_r14_[to][1];

    // Only FIQ mode banks r8 to r12.
    if ((from == kFiqBank) != (to == kFiqBank))
    {
        std::array<uint32_t, 5>& saved = from == kFiqBank ? fiq_r8_r12_ : user_r8_r12_;
        const std::array<uint32_t, 5>& loaded = to == kFiqBank ? fiq_r8_r12_ : user_r8_r12_;
        std::copy(regs_.begin() + 8, regs_.begin() + 13, saved.begin());
        std::copy(loaded.begin(), loaded.end(), regs_.begin() + 8);
    }
}

void Core::WriteCpsr(uint32_t value)
{
    const int from = BankOf(cpsr_ & psr::kModeMask);
    const int to = BankOf(value & psr::kModeMask);
    if (from != to)
    {
        SwitchBank(from, to);
    }

    cpsr_ = value;
    thumb_ = (value & psr::kT) != 0;
}

void Core::RestoreCpsr()
{
    const int bank = BankOf(cpsr_ & psr::kModeMask);
    if (bank != kUserBank)
    {
        WriteCpsr(spsr_[bank]);
    }
}

bool Core::Privileged() const
{
    return (cpsr_ & psr::kModeMask) != static_cast<uint32_t>(Mode::kUser);
}

uint32_t Core::Spsr() const
{
    const int bank = BankOf(cpsr_ & psr::kModeMask);

    return bank == kUserBank ? 0 : spsr_[bank];
}

void Core::WriteSpsr(uint32_t value, uint32_t mask)
{
    const int bank = BankOf(cpsr_ & psr::kModeMask);
    if (bank != kUserBank)
    {
        spsr_[bank] = (spsr_[bank] & ~mask) | (value & mask);
    }
}

uint32_t Core::UserRegister(int index) const
{
    const int bank = BankOf(cpsr_ & psr::kModeMask);
    if (index >= 8 && index <= 12 && bank == kFiqBank)
    {
        return user_r8_r12_[index - 8];
    }

    if (index >= 13 && index <= 14 && bank != kUserBank)
    {
        return r13_r14_[kUserBank][index - 13];
    }

    return regs_[index];
}

void Core::SetUserRegister(int index, uint32_t value)
{
    const int bank = BankOf(cpsr_ & psr::kModeMask);
    if (index >= 8 && index <= 12 && bank == kFiqBank)
    {
        user_r8_r12_[index - 8] = value;
    }
    else if (index >= 13 && index <= 14 && bank != kUserBank)
    {
        r13_r14_[kUserBank][index - 13] = value;
    }
    else
    {
        regs_[index] = value;
    }
}

void Core::BranchExchange(uint32_t target)
{
    thumb_ = (target & 1) != 0;
    cpsr_ = thumb_ ? cpsr_ | psr::kT : cpsr_ & ~psr::kT;
    next_pc_ = target & (thumb_ ? ~1u : ~3u);
}

void Core::LoadPc(uint32_t value)
{
    if (load_interworks_)
    {
        BranchExchange(value);
    }
    else
    {
        BranchInState(value);
    }
}

uint32_t Core::VectorBase() const
{
    return v5_ && (control_ & control::kHighVectors) != 0 ? 0xFFFF0000 : 0;
}

void Core::EnterException(Mode mode, uint32_t vector, uint32_t return_address)
{
    const uint32_t old_cpsr = cpsr_;
    WriteCpsr((old_cpsr & ~(psr::kModeMask | psr::kT)) | static_cast<uint32_t>(mode) | psr::kI);
    spsr_[BankOf(static_cast<uint32_t>(mode))] = old_cpsr;
    regs_[14] = return_address;
    next_pc_ = VectorBase() + vector;
}

void Core::Undefined()
{
    const uint32_t address = regs_[15] - (thumb_ ? 4 : 8);
    if (!vectors_handled_)
    {
        next_pc_ = address;
        undefined_stop_ = true;
        UpdateAttention();
        return;
    }

    EnterException(Mode::kUndefined, kUndefinedVector, address + (thumb_ ? 2 : 4));
}

void Core::SoftwareInterrupt(uint32_t comment)
{
    if (swi_handler_ && swi_handler_(cpu_, comment))
    {
        return;
    }

    const uint32_t address = regs_[15] - (thumb_ ? 4 : 8);
    if (!vectors_handled_)
    {
        next_pc_ = address;
        undefined_stop_ = true;
        UpdateAttention();
        return;
    }

    EnterException(Mode::kSupervisor, kSwiVector, address + (thumb_ ? 2 : 4));
}

void Core::Breakpoint()
{
    const uint32_t address = regs_[15] - (thumb_ ? 4 : 8);
    if (!vectors_handled_)
    {
        next_pc_ = address;
        undefined_stop_ = true;
        UpdateAttention();
        return;
    }

    // A prefetch abort. Its return address is the instruction's plus 4 in both states.
    EnterException(Mode::kAbort, kPrefetchAbortVector, address + 4);
}

void Core::TakeIrq()
{
    // SUBS PC, LR, #4 returns to the instruction that the IRQ came before.
    EnterException(Mode::kIrq, kIrqVector, next_pc_ + 4);
}

void Core::Halt()
{
    halted_ = true;
    UpdateAttention();
}

void Core::UpdateAttention()
{
    attention_ = irq_line_ || halted_ || stop_requested_ || undefined_stop_;
}

inline void Core::Step()
{
    const uint32_t pc = next_pc_;
    ++cycles_;
    if (thumb_)
    {
        const uint32_t op = Fetch16(pc);
        next_pc_ = pc + 2;
        regs_[15] = pc + 4;
        thumb_handlers_[op >> 6](*this, op);
        return;
    }

    const uint32_t op = Fetch32(pc);
    next_pc_ = pc + 4;
    regs_[15] = pc + 8;
    const uint32_t cond = op >> 28;
    if (cond == 0xE || ConditionPassed(cond))
    {
        arm_handlers_[((op >> 16) & 0xFF0) | ((op >> 4) & 0xF)](*this, op);
    }
    else if (cond == 0xF)
    {
        RunUnconditional(*this, op);
    }
}

StopReason Core::Run(uint64_t cycles)
{
    stop_requested_ = false;
    undefined_stop_ = false;
    UpdateAttention();

    const uint64_t end = cycles_ + cycles;
    for (;;)
    {
        if (attention_)
        {
            if (undefined_stop_)
            {
                undefined_stop_ = false;
                UpdateAttention();
                return StopReason::kUndefined;
            }

            if (stop_requested_)
            {
                stop_requested_ = false;
                UpdateAttention();
                return StopReason::kStopped;
            }

            if (halted_)
            {
                if (!irq_line_)
                {
                    return StopReason::kHalted;
                }

                halted_ = false;
                UpdateAttention();
            }

            if (irq_line_ && (cpsr_ & psr::kI) == 0)
            {
                TakeIrq();
            }
        }

        if (next_pc_ == stop_address_)
        {
            return StopReason::kStopAddress;
        }

        if (cycles_ >= end)
        {
            return StopReason::kCycles;
        }

        Step();
    }
}

} // namespace detail

namespace
{

const char* ReasonText(StopReason reason)
{
    switch (reason)
    {
    case StopReason::kHalted:
        return "the CPU halted";
    case StopReason::kStopped:
        return "a handler stopped the CPU";
    case StopReason::kUndefined:
        return "an undefined instruction or an unhandled SWI";
    default:
        return "the CPU reached its stop address";
    }
}

} // namespace

struct Cpu::Impl : detail::Core
{
    using Core::Core;
};

Cpu::Cpu(Bus& bus, Architecture architecture) : impl_(std::make_unique<Impl>(*this, bus, architecture))
{
}

Cpu::~Cpu() = default;

Architecture Cpu::GetArchitecture() const
{
    return impl_->architecture_;
}

void Cpu::Jump(uint32_t pc, Mode mode)
{
    const uint32_t state = (pc & 1) != 0 ? psr::kT : 0;
    impl_->WriteCpsr((impl_->cpsr_ & ~(psr::kModeMask | psr::kT)) | static_cast<uint32_t>(mode) | state);
    impl_->next_pc_ = pc & (state != 0 ? ~1u : ~3u);
    impl_->halted_ = false;
    impl_->UpdateAttention();
}

StopReason Cpu::Run(uint64_t cycles)
{
    return impl_->Run(cycles);
}

uint32_t Cpu::Call(uint32_t address, std::span<const uint32_t> args, uint64_t max_cycles)
{
    detail::Core& core = *impl_;
    const uint32_t saved_pc = Pc();
    const uint32_t saved_sp = core.regs_[13];
    const uint32_t saved_stop = core.stop_address_;
    const bool saved_halted = core.halted_;

    // r0 to r3, then the rest on the stack from SP upward, with SP kept 8-byte aligned.
    for (size_t i = 0; i < std::min<size_t>(args.size(), 4); ++i)
    {
        core.regs_[i] = args[i];
    }

    if (args.size() > 4)
    {
        const uint32_t stacked = static_cast<uint32_t>(args.size() - 4);
        core.regs_[13] = (saved_sp - stacked * 4) & ~7u;
        for (uint32_t i = 0; i < stacked; ++i)
        {
            core.Write32((core.regs_[13] + i * 4) & ~3u, args[4 + i]);
        }
    }

    core.regs_[14] = kCallReturn;
    core.thumb_ = (address & 1) != 0;
    core.cpsr_ = core.thumb_ ? core.cpsr_ | psr::kT : core.cpsr_ & ~psr::kT;
    core.next_pc_ = address & (core.thumb_ ? ~1u : ~3u);
    core.stop_address_ = kCallReturn;
    core.halted_ = false;
    core.UpdateAttention();

    const StopReason reason = core.Run(max_cycles);
    const bool returned = reason == StopReason::kStopAddress && core.next_pc_ == kCallReturn;

    // Go on from where the CPU was before the call.
    core.stop_address_ = saved_stop;
    core.regs_[13] = saved_sp;
    core.thumb_ = (saved_pc & 1) != 0;
    core.cpsr_ = core.thumb_ ? core.cpsr_ | psr::kT : core.cpsr_ & ~psr::kT;
    core.next_pc_ = saved_pc & ~1u;
    core.halted_ = saved_halted;
    core.UpdateAttention();

    if (reason == StopReason::kCycles)
    {
        throw std::runtime_error("the call to " + xsflib::Hex(address, 8, true) + " didn't return within " +
                                 std::to_string(max_cycles) + " cycles");
    }

    if (!returned)
    {
        throw std::runtime_error("the call to " + xsflib::Hex(address, 8, true) +
                                 " didn't return: " + ReasonText(reason));
    }

    return core.regs_[0];
}

uint32_t Cpu::Load(uint32_t address, int size)
{
    detail::Core& core = *impl_;
    switch (size)
    {
    case 4:
        return core.Read32(address & ~3u);
    case 2:
        return core.Read16(address & ~1u);
    default:
        return core.Read8(address);
    }
}

void Cpu::Store(uint32_t address, int size, uint32_t value)
{
    detail::Core& core = *impl_;
    switch (size)
    {
    case 4:
        core.Write32(address & ~3u, value);
        return;
    case 2:
        core.Write16(address & ~1u, value & 0xFFFF);
        return;
    default:
        core.Write8(address, value & 0xFF);
        return;
    }
}

uint64_t Cpu::Cycles() const
{
    return impl_->cycles_;
}

void Cpu::Stop()
{
    impl_->stop_requested_ = true;
    impl_->UpdateAttention();
}

void Cpu::SetIrq(bool asserted)
{
    impl_->irq_line_ = asserted;
    impl_->UpdateAttention();
}

void Cpu::Halt()
{
    impl_->Halt();
}

bool Cpu::Halted() const
{
    return impl_->halted_;
}

void Cpu::SetStopAddress(uint32_t address)
{
    impl_->stop_address_ = address;
}

uint32_t Cpu::StopAddress() const
{
    return impl_->stop_address_;
}

void Cpu::SetSwiHandler(std::function<bool(Cpu&, uint32_t)> handler)
{
    impl_->swi_handler_ = std::move(handler);
}

void Cpu::SetVectorsHandled(bool handled)
{
    impl_->vectors_handled_ = handled;
}

uint32_t Cpu::Reg(int index) const
{
    return index == 15 ? impl_->next_pc_ : impl_->regs_[index];
}

void Cpu::SetReg(int index, uint32_t value)
{
    if (index == 15)
    {
        impl_->BranchInState(value);
    }
    else
    {
        impl_->regs_[index] = value;
    }
}

uint32_t Cpu::Cpsr() const
{
    return impl_->cpsr_;
}

void Cpu::SetCpsr(uint32_t value)
{
    impl_->WriteCpsr(value);
}

uint32_t Cpu::Spsr() const
{
    return impl_->Spsr();
}

uint32_t Cpu::Pc() const
{
    return impl_->next_pc_ | (impl_->thumb_ ? 1 : 0);
}

uint32_t Cpu::Cp15Control() const
{
    return impl_->control_;
}

std::span<uint8_t> Cpu::Itcm()
{
    return impl_->itcm_.storage;
}

std::span<uint8_t> Cpu::Dtcm()
{
    return impl_->dtcm_.storage;
}

uint32_t Cpu::DtcmBase() const
{
    return impl_->dtcm_.setting & 0xFFFFF000;
}

void Cpu::InvalidatePages()
{
    impl_->InvalidatePages();
}

} // namespace ntdlib::arm
