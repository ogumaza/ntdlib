// SPDX-License-Identifier: MIT

// The instructions of primary opcode 31 (see cpu.h): the integer arithmetic, logic, shifts, compares and traps on
// registers, the indexed integer, floating-point and paired-single loads and stores, the string and reservation forms,
// the cache and synchronisation instructions, and the moves to and from the CR, the MSR and the special and segment
// registers.

#include <bit>
#include <cstdint>

#include "xsflib/bytes.h"

#include "common/force_inline.h"
#include "ntdlib/ppc/cpu.h"
#include "ppc/cpu_internal.h"

namespace ntdlib::ppc
{

bool Cpu::Op31(uint32_t op)
{
    State& s = state_;
    const int d = Rd(op);
    const int a = Ra(op);
    const int b = Rb(op);
    const uint32_t ra0 = a == 0 ? 0 : s.gpr[a];
    const uint32_t xo = op >> 1 & 0x3ff;
    const bool oe = (op & 0x400) != 0;

    // The forms with OE set record an overflow in XER.
    const auto set_overflow = [&](bool overflow) NTDLIB_INLINE_LAMBDA
    {
        if (oe)
        {
            SetOverflow(s.xer, overflow);
        }
    };

    // The arithmetic forms: the sum of x, y and a carry in, with the carry and overflow out.
    const auto arith = [&](uint32_t x, uint32_t y, uint32_t carry_in, bool set_carry) NTDLIB_INLINE_LAMBDA
    {
        const uint64_t sum = uint64_t{x} + y + carry_in;
        const auto result = static_cast<uint32_t>(sum);
        if (set_carry)
        {
            SetCarry(s.xer, (sum >> 32) != 0);
        }

        set_overflow(((x ^ result) & (y ^ result)) >> 31);
        s.gpr[d] = result;
        if (Rc(op))
        {
            SetCr0(result);
        }

        return true;
    };

    const auto logical = [&](uint32_t result) NTDLIB_INLINE_LAMBDA
    {
        s.gpr[a] = result;
        if (Rc(op))
        {
            SetCr0(result);
        }

        return true;
    };

    const uint32_t carry = (s.xer & kXerCa) ? 1 : 0;
    switch (xo)
    {
    case 0: // cmp
        SetCrField(d >> 2, CompareField(static_cast<int32_t>(s.gpr[a]), static_cast<int32_t>(s.gpr[b]), s.xer));
        return true;

    case 32: // cmpl
        SetCrField(d >> 2, CompareField(s.gpr[a], s.gpr[b], s.xer));
        return true;

    case 4: // tw
        if (Traps(static_cast<uint32_t>(d), static_cast<int32_t>(s.gpr[a]), static_cast<int32_t>(s.gpr[b])))
        {
            TakeException(Exception::kProgram, stop_pc_, kProgramTrap);
        }

        return true;

    case 8: // subfc
    case 520:
        return arith(~s.gpr[a], s.gpr[b], 1, true);

    case 10: // addc
    case 522:
        return arith(s.gpr[a], s.gpr[b], 0, true);

    case 40: // subf
    case 552:
        return arith(~s.gpr[a], s.gpr[b], 1, false);

    case 104: // neg
    case 616:
        return arith(~s.gpr[a], 0, 1, false);

    case 136: // subfe
    case 648:
        return arith(~s.gpr[a], s.gpr[b], carry, true);

    case 138: // adde
    case 650:
        return arith(s.gpr[a], s.gpr[b], carry, true);

    case 200: // subfze
    case 712:
        return arith(~s.gpr[a], 0, carry, true);

    case 202: // addze
    case 714:
        return arith(s.gpr[a], 0, carry, true);

    case 232: // subfme
    case 744:
        return arith(~s.gpr[a], 0xffffffff, carry, true);

    case 234: // addme
    case 746:
        return arith(s.gpr[a], 0xffffffff, carry, true);

    case 266: // add
    case 778:
        return arith(s.gpr[a], s.gpr[b], 0, false);

    case 11: // mulhwu
        s.gpr[d] = static_cast<uint32_t>((uint64_t{s.gpr[a]} * s.gpr[b]) >> 32);
        if (Rc(op))
        {
            SetCr0(s.gpr[d]);
        }

        return true;

    case 75: // mulhw
        s.gpr[d] = static_cast<uint32_t>(
            static_cast<uint64_t>(int64_t{static_cast<int32_t>(s.gpr[a])} * static_cast<int32_t>(s.gpr[b])) >> 32);
        if (Rc(op))
        {
            SetCr0(s.gpr[d]);
        }

        return true;

    case 235: // mullw
    case 747:
        {
            const int64_t product = int64_t{static_cast<int32_t>(s.gpr[a])} * static_cast<int32_t>(s.gpr[b]);
            s.gpr[d] = static_cast<uint32_t>(product);
            set_overflow(product != static_cast<int32_t>(product));
            if (Rc(op))
            {
                SetCr0(s.gpr[d]);
            }

            return true;
        }

    case 491: // divw
    case 1003:
        {
            // The manual leaves a division by 0, and 0x80000000 / -1, undefined. Here the result is rA's sign.
            const auto x = static_cast<int32_t>(s.gpr[a]);
            const auto y = static_cast<int32_t>(s.gpr[b]);
            const bool overflow = y == 0 || (x == INT32_MIN && y == -1);
            s.gpr[d] = overflow ? (x < 0 ? 0xffffffff : 0) : static_cast<uint32_t>(x / y);
            set_overflow(overflow);
            if (Rc(op))
            {
                SetCr0(s.gpr[d]);
            }

            return true;
        }

    case 459: // divwu
    case 971:
        s.gpr[d] = s.gpr[b] == 0 ? 0 : s.gpr[a] / s.gpr[b];
        set_overflow(s.gpr[b] == 0);
        if (Rc(op))
        {
            SetCr0(s.gpr[d]);
        }

        return true;

    case 19: // mfcr
        s.gpr[d] = s.cr;
        return true;

    case 144: // mtcrf
        {
            const uint32_t fields = op >> 12 & 0xff;
            uint32_t mask = 0;
            for (int i = 0; i < 8; i++)
            {
                if (fields & (0x80 >> i))
                {
                    mask |= 0xf0000000u >> (i * 4);
                }
            }

            s.cr = (s.cr & ~mask) | (s.gpr[d] & mask);
            return true;
        }

    case 512: // mcrxr
        SetCrField(d >> 2, s.xer >> 28);
        s.xer &= ~(kXerSo | kXerOv | kXerCa);
        return true;

    case 83: // mfmsr
        s.gpr[d] = s.msr;
        return true;

    case 146: // mtmsr
        SetMsr(s.gpr[d]);
        return true;

    case 339: // mfspr
    case 371: // mftb
        s.gpr[d] = MfSpr(static_cast<int>((op >> 16 & 31) | (op >> 6 & 0x3e0)));
        return true;

    case 467: // mtspr
        MtSpr(static_cast<int>((op >> 16 & 31) | (op >> 6 & 0x3e0)), s.gpr[d]);
        return true;

    case 595: // mfsr
        s.gpr[d] = s.sr[a & 15];
        return true;

    case 659: // mfsrin
        s.gpr[d] = s.sr[s.gpr[b] >> 28];
        return true;

    case 210: // mtsr
        s.sr[a & 15] = s.gpr[d];
        return true;

    case 242: // mtsrin
        s.sr[s.gpr[b] >> 28] = s.gpr[d];
        return true;

    case 24: // slw
        {
            const uint32_t amount = s.gpr[b] & 0x3f;
            return logical(amount >= 32 ? 0 : s.gpr[d] << amount);
        }

    case 536: // srw
        {
            const uint32_t amount = s.gpr[b] & 0x3f;
            return logical(amount >= 32 ? 0 : s.gpr[d] >> amount);
        }

    case 792: // sraw
    case 824: // srawi
        {
            // The carry is set when a negative value loses 1 bits.
            const uint32_t amount = xo == 824 ? static_cast<uint32_t>(b) : s.gpr[b] & 0x3f;
            const auto value = static_cast<int32_t>(s.gpr[d]);
            uint32_t result = 0;
            bool carry_out = false;
            if (amount >= 32)
            {
                result = value < 0 ? 0xffffffff : 0;
                carry_out = value < 0;
            }
            else
            {
                result = static_cast<uint32_t>(value >> amount);
                carry_out = value < 0 && amount > 0 && (static_cast<uint32_t>(value) << (32 - amount)) != 0;
            }

            SetCarry(s.xer, carry_out);
            return logical(result);
        }

    case 26: // cntlzw
        return logical(static_cast<uint32_t>(std::countl_zero(s.gpr[d])));

    case 28: // and
        return logical(s.gpr[d] & s.gpr[b]);

    case 60: // andc
        return logical(s.gpr[d] & ~s.gpr[b]);

    case 124: // nor
        return logical(~(s.gpr[d] | s.gpr[b]));

    case 284: // eqv
        return logical(~(s.gpr[d] ^ s.gpr[b]));

    case 316: // xor
        return logical(s.gpr[d] ^ s.gpr[b]);

    case 412: // orc
        return logical(s.gpr[d] | ~s.gpr[b]);

    case 444: // or
        return logical(s.gpr[d] | s.gpr[b]);

    case 476: // nand
        return logical(~(s.gpr[d] & s.gpr[b]));

    case 922: // extsh
        return logical(static_cast<uint32_t>(static_cast<int16_t>(s.gpr[d])));

    case 954: // extsb
        return logical(static_cast<uint32_t>(static_cast<int8_t>(s.gpr[d])));

    case 20: // lwarx
        {
            const uint32_t address = ra0 + s.gpr[b];
            const uint32_t value = Read32(address);
            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            reservation_ = true;
            reservation_address_ = address;
            return true;
        }

    case 150: // stwcx.
        {
            const uint32_t address = ra0 + s.gpr[b];
            const bool stored = reservation_ && reservation_address_ == address;
            if (stored)
            {
                Write32(address, s.gpr[d]);
            }

            reservation_ = false;
            SetCrField(0, (stored ? 2 : 0) | ((s.xer & kXerSo) ? 1 : 0));
            return !stopped_;
        }

    case 23:  // lwzx
    case 55:  // lwzux
    case 87:  // lbzx
    case 119: // lbzux
    case 279: // lhzx
    case 311: // lhzux
    case 343: // lhax
    case 375: // lhaux
    case 534: // lwbrx
    case 790: // lhbrx
        {
            const bool update = xo == 55 || xo == 119 || xo == 311 || xo == 375;
            const uint32_t address = (update ? s.gpr[a] : ra0) + s.gpr[b];
            uint32_t value = 0;
            switch (xo)
            {
            case 23:
            case 55:
                value = Read32(address);
                break;
            case 87:
            case 119:
                value = Read8(address);
                break;
            case 279:
            case 311:
                value = Read16(address);
                break;
            case 343:
            case 375:
                value = static_cast<uint32_t>(static_cast<int16_t>(Read16(address)));
                break;
            case 534:
                value = xsflib::ByteSwap32(Read32(address));
                break;
            default:
                value = xsflib::ByteSwap16(Read16(address));
                break;
            }

            if (stopped_)
            {
                return false;
            }

            s.gpr[d] = value;
            if (update)
            {
                s.gpr[a] = address;
            }

            return true;
        }

    case 151: // stwx
    case 183: // stwux
    case 215: // stbx
    case 247: // stbux
    case 407: // sthx
    case 439: // sthux
    case 662: // stwbrx
    case 918: // sthbrx
        {
            const bool update = xo == 183 || xo == 247 || xo == 439;
            const uint32_t address = (update ? s.gpr[a] : ra0) + s.gpr[b];
            switch (xo)
            {
            case 151:
            case 183:
                Write32(address, s.gpr[d]);
                break;
            case 215:
            case 247:
                Write8(address, static_cast<uint8_t>(s.gpr[d]));
                break;
            case 407:
            case 439:
                Write16(address, static_cast<uint16_t>(s.gpr[d]));
                break;
            case 662:
                Write32(address, xsflib::ByteSwap32(s.gpr[d]));
                break;
            default:
                Write16(address, xsflib::ByteSwap16(static_cast<uint16_t>(s.gpr[d])));
                break;
            }

            if (update && !stopped_)
            {
                s.gpr[a] = address;
            }

            return !stopped_;
        }

    case 533: // lswx
    case 597: // lswi
        {
            // Bytes go into rD and the following registers, four to a register, most significant first. The registers
            // wrap from r31 to r0.
            uint32_t address = xo == 597 ? ra0 : ra0 + s.gpr[b];
            uint32_t count = xo == 597 ? (b == 0 ? 32 : static_cast<uint32_t>(b)) : s.xer & 0x7f;
            int r = d - 1;
            for (uint32_t i = 0; i < count && !stopped_; i++, address++)
            {
                if (i % 4 == 0)
                {
                    r = (r + 1) % 32;
                    s.gpr[r] = 0;
                }

                s.gpr[r] |= uint32_t{Read8(address)} << (24 - (i % 4) * 8);
            }

            return !stopped_;
        }

    case 661: // stswx
    case 725: // stswi
        {
            uint32_t address = xo == 725 ? ra0 : ra0 + s.gpr[b];
            uint32_t count = xo == 725 ? (b == 0 ? 32 : static_cast<uint32_t>(b)) : s.xer & 0x7f;
            int r = d - 1;
            for (uint32_t i = 0; i < count && !stopped_; i++, address++)
            {
                if (i % 4 == 0)
                {
                    r = (r + 1) % 32;
                }

                Write8(address, static_cast<uint8_t>(s.gpr[r] >> (24 - (i % 4) * 8)));
            }

            return !stopped_;
        }

    case 1014: // dcbz
        {
            const uint32_t address = (ra0 + s.gpr[b]) & ~31u;
            for (uint32_t i = 0; i < 32 && !stopped_; i += 4)
            {
                Write32(address + i, 0);
            }

            return !stopped_;
        }

    case 54:  // dcbst
    case 86:  // dcbf
    case 246: // dcbtst
    case 278: // dcbt
    case 306: // tlbie
    case 470: // dcbi
    case 566: // tlbsync
    case 598: // sync
    case 854: // eieio
    case 982: // icbi
        return true;

    case 535: // lfsx
    case 567: // lfsux
    case 599: // lfdx
    case 631: // lfdux
    case 663: // stfsx
    case 695: // stfsux
    case 727: // stfdx
    case 759: // stfdux
    case 983: // stfiwx
        return LoadStoreFloat(op);

    case 6:  // psq_lx
    case 7:  // psq_stx
    case 38: // psq_lux
    case 39: // psq_stux
        return PairedLoadStore(op);

    default:
        // The paired-single indexed forms decode on 6 bits.
        switch (op >> 1 & 0x3f)
        {
        case 6:
        case 7:
        case 38:
        case 39:
            return PairedLoadStore(op);
        default:
            return Illegal(op);
        }
    }
}

bool Cpu::Illegal(uint32_t op)
{
    stop_opcode_ = op;
    stop_reason_ = StopReason::kIllegal;
    state_.pc = stop_pc_;

    return false;
}

} // namespace ntdlib::ppc
