// SPDX-License-Identifier: MIT

#include "ntdlib/ppc/cpu.h"

#include <algorithm>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"

#include "common/force_inline.h"
#include "ntdlib/types.h"
#include "ppc/cpu_internal.h"

namespace ntdlib::ppc
{

Cpu::Cpu(Bus& bus) : bus_(bus), data_map_(1u << 20, nullptr), fetch_map_(1u << 20, nullptr)
{
    state_.spr[spr::kPvr] = kBroadwayPvr;
}

StopReason Cpu::Run(uint64_t cycles)
{
    const int host_rounding = std::fegetround();
    UpdateRounding();
    end_ = cycles_ + cycles;
    stopped_ = false;
    fetch_page_ = kNoFetchPage;
    stop_reason_ = StopReason::kBudget;
    while (cycles_ < end_ && !stopped_)
    {
        // The decrementer's exception becomes pending when it goes negative. It does so again 2^32 ticks later.
        if (cycles_ >= dec_event_)
        {
            dec_pending_ = true;
            dec_event_ += (uint64_t{1} << 32) * kCyclesPerTick;
        }

        CheckInterrupts();
        next_check_ = std::min(end_, dec_event_);

        // Whether anything is watched is checked once for the batch. A check before every instruction slowed the loop.
        // The filter rules out most instructions before the search of the list. A search before every instruction
        // slowed a length analysis too.
        const bool watching = !watch_pcs_.empty();
        while (cycles_ < next_check_)
        {
            if (watching && (watch_filter_ & WatchBit(state_.pc)) != 0 &&
                std::find(watch_pcs_.begin(), watch_pcs_.end(), state_.pc) != watch_pcs_.end()) [[unlikely]]
            {
                on_watch_(state_.pc);
            }

            uint32_t op = 0;
            if (!Fetch(state_.pc, op))
            {
                break;
            }

            stop_pc_ = state_.pc;
            state_.pc += 4;
            cycles_++;
            if (!Step(op))
            {
                stopped_ = true;
                break;
            }
        }
    }

    std::fesetround(host_rounding);

    return stopped_ ? stop_reason_ : StopReason::kBudget;
}

void Cpu::Watch(std::vector<uint32_t> pcs, std::function<void(uint32_t pc)> on_watch)
{
    watch_pcs_ = std::move(pcs);
    watch_filter_ = 0;
    for (const uint32_t pc : watch_pcs_)
    {
        watch_filter_ |= WatchBit(pc);
    }

    on_watch_ = std::move(on_watch);
}

Cpu::Snapshot Cpu::Save() const
{
    Snapshot snapshot;
    snapshot.state = state_;
    snapshot.cycles = cycles_;
    snapshot.external = external_;
    snapshot.reservation = reservation_;
    snapshot.reservation_address = reservation_address_;
    snapshot.tb_base = tb_base_;
    snapshot.tb_cycle = tb_cycle_;
    snapshot.dec_base = dec_base_;
    snapshot.dec_cycle = dec_cycle_;
    snapshot.dec_event = dec_event_;
    snapshot.dec_pending = dec_pending_;

    return snapshot;
}

void Cpu::Restore(const Snapshot& snapshot)
{
    state_ = snapshot.state;
    cycles_ = snapshot.cycles;
    external_ = snapshot.external;
    reservation_ = snapshot.reservation;
    reservation_address_ = snapshot.reservation_address;
    tb_base_ = snapshot.tb_base;
    tb_cycle_ = snapshot.tb_cycle;
    dec_base_ = snapshot.dec_base;
    dec_cycle_ = snapshot.dec_cycle;
    dec_event_ = snapshot.dec_event;
    dec_pending_ = snapshot.dec_pending;
    InvalidateTranslation();
}

void Cpu::SkipIdleLoop(int cr_field)
{
    // Some loops load a word, compare it and branch back while it's unchanged. The OS uses one to wait for a thread to
    // become ready. Such a loop only spins until an interrupt changes the word. Nothing else runs while the CPU runs.
    // The loop's whole iterations up to the next interrupt check can therefore run at once. The CPU ends up where those
    // iterations would have left it.
    //
    // An interrupt can arrive between the load and the branch. The branch then goes back on the value from before the
    // interrupt, and the next load reads the word that the handler changed. That pass may leave the loop. Nothing is
    // skipped while the register and the word differ. A skip there would keep the CPU in the loop until the next
    // device event. The skip reads the word only where it is whole in a page of RAM.
    uint32_t load = 0;
    uint32_t compare = 0;
    if (!Fetch(stop_pc_ - 8, load) || !Fetch(stop_pc_ - 4, compare))
    {
        return;
    }

    const int loaded = Rd(load);
    const bool is_load = load >> 26 == 32 && Ra(load) != loaded;
    const bool is_compare = (compare >> 26 == 10 || compare >> 26 == 11) && Ra(compare) == loaded &&
                            static_cast<int>(compare >> 23 & 7) == cr_field;
    const uint32_t address = (Ra(load) ? state_.gpr[Ra(load)] : 0) + static_cast<uint32_t>(Simm(load));
    const uint8_t* page = DataPage(address);
    if (!is_load || !is_compare || page == nullptr || (address & kPageMask) > 0xffc ||
        xsflib::LoadBe32(page + (address & kPageMask)) != state_.gpr[loaded])
    {
        return;
    }

    constexpr uint64_t kLoopLength = 3;
    if (next_check_ > cycles_)
    {
        cycles_ += (next_check_ - cycles_) / kLoopLength * kLoopLength;
    }
}

void Cpu::InvalidateTranslation()
{
    RebuildMap(false);
    RebuildMap(true);
}

bool Cpu::ReadVirtual(uint32_t address, void* out, std::size_t size)
{
    auto* bytes = static_cast<uint8_t*>(out);
    for (std::size_t i = 0; i < size; i++)
    {
        const uint8_t* byte = RamByte(address + static_cast<uint32_t>(i));
        if (byte == nullptr)
        {
            return false;
        }

        bytes[i] = *byte;
    }

    return true;
}

bool Cpu::WriteVirtual(uint32_t address, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < size; i++)
    {
        uint8_t* byte = RamByte(address + static_cast<uint32_t>(i));
        if (byte == nullptr)
        {
            return false;
        }

        *byte = bytes[i];
    }

    return true;
}

uint8_t* Cpu::RamByte(uint32_t address)
{
    PAddr physical = 0;
    if (!Translate(address, false, physical))
    {
        return nullptr;
    }

    uint8_t* page = bus_.RamPage(physical & ~kPageMask);

    return page == nullptr ? nullptr : page + (physical & kPageMask);
}

std::string Cpu::Describe() const
{
    char text[160];
    std::snprintf(text, sizeof(text), "pc %08x lr %08x msr %08x cr %08x r1 %08x r3 %08x r4 %08x r5 %08x", state_.pc,
                  state_.lr, state_.msr, state_.cr, state_.gpr[1], state_.gpr[3], state_.gpr[4], state_.gpr[5]);

    return text;
}

NTDLIB_FORCE_INLINE bool Cpu::Step(uint32_t op)
{
    State& s = state_;
    const int d = Rd(op);
    const int a = Ra(op);
    const uint32_t ra0 = a == 0 ? 0 : s.gpr[a]; // (rA|0)
    switch (op >> 26)
    {
    case 3: // twi
        if (Traps(static_cast<uint32_t>(d), static_cast<int32_t>(s.gpr[a]), Simm(op)))
        {
            TakeException(Exception::kProgram, stop_pc_, kProgramTrap);
        }

        return true;

    case 4:
        return Op4(op);

    case 7: // mulli
        s.gpr[d] = s.gpr[a] * static_cast<uint32_t>(Simm(op));
        return true;

    case 8: // subfic
        {
            const uint32_t imm = static_cast<uint32_t>(Simm(op));
            const uint32_t inverted = ~s.gpr[a];
            const uint64_t sum = uint64_t{inverted} + imm + 1;
            s.gpr[d] = static_cast<uint32_t>(sum);
            SetCarry(s.xer, (sum >> 32) != 0);
            return true;
        }

    case 10: // cmpli
        SetCrField(d >> 2, CompareField(s.gpr[a], Uimm(op), s.xer));
        return true;

    case 11: // cmpi
        SetCrField(d >> 2, CompareField(static_cast<int32_t>(s.gpr[a]), Simm(op), s.xer));
        return true;

    case 12: // addic
    case 13: // addic.
        {
            const uint64_t sum = uint64_t{s.gpr[a]} + static_cast<uint32_t>(Simm(op));
            s.gpr[d] = static_cast<uint32_t>(sum);
            SetCarry(s.xer, (sum >> 32) != 0);
            if (op >> 26 == 13)
            {
                SetCr0(s.gpr[d]);
            }

            return true;
        }

    case 14: // addi
        s.gpr[d] = ra0 + static_cast<uint32_t>(Simm(op));
        return true;

    case 15: // addis
        s.gpr[d] = ra0 + (Uimm(op) << 16);
        return true;

    case 16: // bc
        {
            const uint32_t bo = static_cast<uint32_t>(d);
            if (!(bo & 4))
            {
                s.ctr--;
            }

            const bool ctr_ok = (bo & 4) || ((s.ctr != 0) != ((bo & 2) != 0));
            const bool cond_ok = (bo & 16) || (CrBit(a) == ((bo & 8) != 0));
            if (op & 1)
            {
                s.lr = stop_pc_ + 4;
            }

            if (ctr_ok && cond_ok)
            {
                const auto offset = static_cast<uint32_t>(static_cast<int16_t>(op & 0xfffc));
                s.pc = (op & 2) ? offset : stop_pc_ + offset;
                if (offset == static_cast<uint32_t>(-8) && (op & 3) == 0 && (bo & 4))
                {
                    SkipIdleLoop(a >> 2);
                }
            }

            return true;
        }

    case 17: // sc
        TakeException(Exception::kSystemCall, s.pc);
        return true;

    case 18: // b
        {
            uint32_t offset = op & 0x03fffffc;
            if (offset & 0x02000000)
            {
                offset |= 0xfc000000;
            }

            if (op & 1)
            {
                s.lr = stop_pc_ + 4;
            }

            s.pc = (op & 2) ? offset : stop_pc_ + offset;
            return true;
        }

    case 19:
        return Op19(op);

    case 20: // rlwimi
        {
            const uint32_t mask = Mask(op >> 6 & 31, op >> 1 & 31);
            const uint32_t rotated = Rotate(s.gpr[d], Rb(op));
            s.gpr[a] = (rotated & mask) | (s.gpr[a] & ~mask);
            if (Rc(op))
            {
                SetCr0(s.gpr[a]);
            }

            return true;
        }

    case 21: // rlwinm
        s.gpr[a] = Rotate(s.gpr[d], Rb(op)) & Mask(op >> 6 & 31, op >> 1 & 31);
        if (Rc(op))
        {
            SetCr0(s.gpr[a]);
        }

        return true;

    case 23: // rlwnm
        s.gpr[a] = Rotate(s.gpr[d], static_cast<int>(s.gpr[Rb(op)] & 31)) & Mask(op >> 6 & 31, op >> 1 & 31);
        if (Rc(op))
        {
            SetCr0(s.gpr[a]);
        }

        return true;

    case 24: // ori
        s.gpr[a] = s.gpr[d] | Uimm(op);
        return true;

    case 25: // oris
        s.gpr[a] = s.gpr[d] | (Uimm(op) << 16);
        return true;

    case 26: // xori
        s.gpr[a] = s.gpr[d] ^ Uimm(op);
        return true;

    case 27: // xoris
        s.gpr[a] = s.gpr[d] ^ (Uimm(op) << 16);
        return true;

    case 28: // andi.
        s.gpr[a] = s.gpr[d] & Uimm(op);
        SetCr0(s.gpr[a]);
        return true;

    case 29: // andis.
        s.gpr[a] = s.gpr[d] & (Uimm(op) << 16);
        SetCr0(s.gpr[a]);
        return true;

    case 31:
        return Op31(op);

    case 32: // lwz
    case 33: // lwzu
        {
            const uint32_t address = (op >> 26 == 33 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint32_t value = Read32(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (op >> 26 == 33)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 34: // lbz
    case 35: // lbzu
        {
            const uint32_t address = (op >> 26 == 35 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint8_t value = Read8(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (op >> 26 == 35)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 36: // stw
    case 37: // stwu
        {
            const uint32_t address = (op >> 26 == 37 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            Write32(address, s.gpr[d]);
            if (op >> 26 == 37 && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 38: // stb
    case 39: // stbu
        {
            const uint32_t address = (op >> 26 == 39 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            Write8(address, static_cast<uint8_t>(s.gpr[d]));
            if (op >> 26 == 39 && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 40: // lhz
    case 41: // lhzu
        {
            const uint32_t address = (op >> 26 == 41 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint16_t value = Read16(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (op >> 26 == 41)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 42: // lha
    case 43: // lhau
        {
            const uint32_t address = (op >> 26 == 43 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            const uint16_t value = Read16(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = static_cast<uint32_t>(static_cast<int16_t>(value));
            if (op >> 26 == 43)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 44: // sth
    case 45: // sthu
        {
            const uint32_t address = (op >> 26 == 45 ? s.gpr[a] : ra0) + static_cast<uint32_t>(Simm(op));
            Write16(address, static_cast<uint16_t>(s.gpr[d]));
            if (op >> 26 == 45 && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 46: // lmw
        {
            uint32_t address = ra0 + static_cast<uint32_t>(Simm(op));
            for (int r = d; r < 32 && !stopped_; r++, address += 4)
            {
                const uint32_t value = Read32(address);
                if (!stopped_)
                {
                    s.gpr[r] = value;
                }
            }

            return !stopped_;
        }

    case 47: // stmw
        {
            uint32_t address = ra0 + static_cast<uint32_t>(Simm(op));
            for (int r = d; r < 32 && !stopped_; r++, address += 4)
            {
                Write32(address, s.gpr[r]);
            }

            return !stopped_;
        }

    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
    case 53:
    case 54:
    case 55:
        return LoadStoreFloat(op);

    case 56:
    case 57:
    case 60:
    case 61:
        return PairedLoadStore(op);

    case 59:
        return Op59(op);

    case 63:
        return Op63(op);

    default:
        return Illegal(op);
    }
}

bool Cpu::Op19(uint32_t op)
{
    State& s = state_;
    const int d = Rd(op);
    const int a = Ra(op);
    const int b = Rb(op);
    const auto cr_op = [&](auto combine) NTDLIB_INLINE_LAMBDA
    {
        const bool value = combine(CrBit(a), CrBit(b));
        const uint32_t bit = 1u << (31 - d);
        s.cr = value ? s.cr | bit : s.cr & ~bit;
    };

    switch (op >> 1 & 0x3ff)
    {
    case 0: // mcrf
        SetCrField(d >> 2, CrField(a >> 2));
        return true;

    case 16:  // bclr
    case 528: // bcctr
        {
            const bool to_ctr = (op >> 1 & 0x3ff) == 528;
            const uint32_t bo = static_cast<uint32_t>(d);
            if (!to_ctr && !(bo & 4))
            {
                s.ctr--;
            }

            const bool ctr_ok = to_ctr || (bo & 4) || ((s.ctr != 0) != ((bo & 2) != 0));
            const bool cond_ok = (bo & 16) || (CrBit(a) == ((bo & 8) != 0));
            const uint32_t target = (to_ctr ? s.ctr : s.lr) & ~3u;
            if (op & 1)
            {
                s.lr = stop_pc_ + 4;
            }

            if (ctr_ok && cond_ok)
            {
                s.pc = target;
            }

            return true;
        }

    case 33: // crnor
        cr_op([](bool x, bool y) { return !(x || y); });
        return true;

    case 50: // rfi
        SetMsr((s.msr & ~kSrr1Msr) | (s.spr[spr::kSrr1] & kSrr1Msr));
        s.pc = s.spr[spr::kSrr0] & ~3u;
        return true;

    case 129: // crandc
        cr_op([](bool x, bool y) { return x && !y; });
        return true;

    case 150: // isync
        return true;

    case 193: // crxor
        cr_op([](bool x, bool y) { return x != y; });
        return true;

    case 225: // crnand
        cr_op([](bool x, bool y) { return !(x && y); });
        return true;

    case 257: // crand
        cr_op([](bool x, bool y) { return x && y; });
        return true;

    case 289: // creqv
        cr_op([](bool x, bool y) { return x == y; });
        return true;

    case 417: // crorc
        cr_op([](bool x, bool y) { return x || !y; });
        return true;

    case 449: // cror
        cr_op([](bool x, bool y) { return x || y; });
        return true;

    default:
        return Illegal(op);
    }
}

} // namespace ntdlib::ppc
