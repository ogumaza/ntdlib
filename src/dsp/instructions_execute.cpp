// SPDX-License-Identifier: MIT

// The DSP's instructions (see dsp.h): each main instruction by its kind, and the parallel load/store extensions.

#include <cstdint>

#include "common/force_inline.h"
#include "dsp/instructions_internal.h"
#include "dsp/internal.h"
#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/dsp/dsp.h"

namespace ntdlib::dsp
{

template <typename Memory>
NTDLIB_FORCE_INLINE void Dsp<Memory>::ExecuteOp(uint16_t op, uint8_t kind)
{
    Registers& r = regs_;
    HeldWrites held;

    // The extension's memory accesses come first. Its register results wait until after the main instruction.
    if (op >= 0x3000)
    {
        Extension(static_cast<uint8_t>(op >= 0x4000 ? op & 0xff : op & 0x7f), held);
    }

    // The helpers are inlined into every case. Out of line, they took about a fifth of the time of a busy piece that
    // runs JAudio's DSP program.
    const auto mul = [this](uint16_t a, uint16_t b, bool a_signed, bool b_signed) NTDLIB_INLINE_LAMBDA
    {
        // The exact product, doubled when AM is clear.
        const int64_t x = a_signed ? Sext16(a) : int64_t{a};
        const int64_t y = b_signed ? Sext16(b) : int64_t{b};
        int64_t product = x * y;
        if (!(regs_.sr & kSrAm))
        {
            product *= 2;
        }

        return product;
    };

    // MULX's operands: unsigned where SU selects it.
    const auto mulx = [&](int s, int t) NTDLIB_INLINE_LAMBDA
    {
        const uint16_t a = s ? r.ax_h[0] : r.ax_l[0];
        const uint16_t b = t ? r.ax_h[1] : r.ax_l[1];
        const bool su = (r.sr & kSrSu) != 0;
        const bool a_signed = !su || s == 1;
        const bool b_signed = !su || t == 1;

        return mul(a, b, a_signed, b_signed);
    };

    const auto add = [this](int d, int64_t b) NTDLIB_INLINE_LAMBDA
    {
        const int64_t a = regs_.ac[d];
        const int64_t result = Wrap40(a + b);
        regs_.ac[d] = result;
        SetFlagsAdd(a, b, result);
    };

    const auto sub = [this](int d, int64_t b) NTDLIB_INLINE_LAMBDA
    {
        const int64_t a = regs_.ac[d];
        const int64_t result = Wrap40(a - b);
        regs_.ac[d] = result;
        SetFlagsSub(a, b, result);
    };

    const auto compare = [this](int64_t a, int64_t b) NTDLIB_INLINE_LAMBDA
    {
        SetFlagsSub(a, b, Wrap40(a - b));
    };

    const auto logic = [this](int d, uint16_t m) NTDLIB_INLINE_LAMBDA
    {
        SetAcM(d, m);
        const int64_t value = regs_.ac[d];
        SetFlags16(m, value < kS32Min || value > kS32Max);
    };

    const auto set_acc = [this](int d, int64_t value) NTDLIB_INLINE_LAMBDA
    {
        SetAc(d, value);
        SetFlags40(regs_.ac[d], false, false);
    };

    const int bit8 = op >> 8 & 1;
    const auto decoded = static_cast<Op>(kind);
    switch (decoded)
    {
    case Op::kUnknown:
    case Op::kNop:
    case Op::kNx:
        break;

    case Op::kDar:
        r.ar[op & 3] = Decrement(r.ar[op & 3], r.wr[op & 3]);
        break;

    case Op::kIar:
        r.ar[op & 3] = Increment(r.ar[op & 3], r.wr[op & 3]);
        break;

    case Op::kSubarn:
        r.ar[op & 3] = SubtractIndex(r.ar[op & 3], r.ix[op & 3], r.wr[op & 3]);
        break;

    case Op::kAddarn:
        r.ar[op & 3] = AddIndex(r.ar[op & 3], r.ix[op >> 2 & 3], r.wr[op & 3]);
        break;

    case Op::kHalt:
        control_ |= control::kHalt;
        r.pc--;
        break;

    case Op::kLoop:
    case Op::kLoopi:
        {
            const uint16_t count = decoded == Op::kLoop ? ReadReg(op & 0x1f) : static_cast<uint16_t>(op & 0xff);
            if (count == 0)
            {
                SkipNextInstruction();
                break;
            }

            Push(0, r.pc);
            Push(2, r.pc);
            Push(3, count);
            break;
        }

    case Op::kBloop:
    case Op::kBloopi:
        {
            const uint16_t end = FetchSecondWord();
            const uint16_t count = decoded == Op::kBloop ? ReadReg(op & 0x1f) : static_cast<uint16_t>(op & 0xff);
            if (count == 0)
            {
                r.pc = end;
                SkipNextInstruction();
                break;
            }

            Push(0, r.pc);
            Push(2, end);
            Push(3, count);
            break;
        }

    case Op::kLri:
        WriteRegLoad(op & 0x1f, FetchSecondWord());
        break;

    case Op::kLr:
        WriteRegLoad(op & 0x1f, ReadDmem(FetchSecondWord()));
        break;

    case Op::kSr:
        {
            const uint16_t address = FetchSecondWord();
            WriteDmem(address, ReadReg(op & 0x1f));
            break;
        }

    case Op::kAddi:
        add(bit8, Sext16(FetchSecondWord()) * 0x10000);
        break;

    case Op::kIlrr:
    case Op::kIlrrd:
    case Op::kIlrri:
    case Op::kIlrrn:
        {
            const int s = op & 3;
            WriteRegLoad(0x1e + bit8, ReadImem(r.ar[s]));
            r.ar[s] = SteppedAddress(s, op >> 2 & 3);
            break;
        }

    case Op::kXori:
        logic(bit8, AcM(bit8) ^ FetchSecondWord());
        break;

    case Op::kAndi:
        logic(bit8, AcM(bit8) & FetchSecondWord());
        break;

    case Op::kOri:
        logic(bit8, AcM(bit8) | FetchSecondWord());
        break;

    case Op::kIf:
        if (!Condition(op & 0xf))
        {
            SkipNextInstruction();
        }
        break;

    case Op::kCmpi:
        compare(r.ac[bit8], Sext16(FetchSecondWord()) * 0x10000);
        break;

    case Op::kJmp:
        {
            const uint16_t target = FetchSecondWord();
            if (Condition(op & 0xf))
            {
                r.pc = target;
            }
            break;
        }

    case Op::kCall:
        {
            const uint16_t target = FetchSecondWord();
            if (Condition(op & 0xf))
            {
                Push(0, r.pc);
                r.pc = target;
            }
            break;
        }

    case Op::kAndf:
        {
            const uint16_t imm = FetchSecondWord();
            r.sr = (AcM(bit8) & imm) == 0 ? r.sr | kSrLogicZero : r.sr & ~kSrLogicZero;
            break;
        }

    case Op::kAndcf:
        {
            const uint16_t imm = FetchSecondWord();
            r.sr = (AcM(bit8) & imm) == imm ? r.sr | kSrLogicZero : r.sr & ~kSrLogicZero;
            break;
        }

    case Op::kLsrn:
    case Op::kAsrn:
        // A positive amount from $ac1.m shifts $ac0 right.
        set_acc(0, Shift(r.ac[0], -ShiftAmount(AcM(1)), decoded == Op::kAsrn));
        break;

    case Op::kRet:
        if (Condition(op & 0xf))
        {
            r.pc = Pop(0);
        }
        break;

    case Op::kRti:
        if (Condition(op & 0xf))
        {
            r.sr = Pop(1);
            r.pc = Pop(0);
            RaiseCpuInterrupt();
        }
        break;

    case Op::kAddis:
        add(bit8, static_cast<int64_t>(static_cast<int8_t>(op & 0xff)) * 0x10000);
        break;

    case Op::kCmpis:
        compare(r.ac[bit8], static_cast<int64_t>(static_cast<int8_t>(op & 0xff)) * 0x10000);
        break;

    case Op::kLris:
        WriteRegLoad(0x18 + (op >> 8 & 7), static_cast<uint16_t>(static_cast<int8_t>(op & 0xff)));
        break;

    case Op::kSbclr:
        r.sr &= static_cast<uint16_t>(~(1u << ((op & 7) + 6)));
        break;

    case Op::kSbset:
        r.sr |= static_cast<uint16_t>(1u << ((op & 7) + 6));
        RaiseCpuInterrupt();
        break;

    case Op::kLsl:
    case Op::kAsl:
        set_acc(bit8, Shift(r.ac[bit8], op & 0x3f, false));
        break;

    case Op::kLsr:
    case Op::kAsr:
        {
            // The amount is encoded as a negative 6-bit number.
            const int i = op & 0x3f;
            const int k = i == 0 ? 0 : 64 - i;
            set_acc(bit8, Shift(r.ac[bit8], -k, decoded == Op::kAsr));
            break;
        }

    case Op::kSi:
        {
            const auto address = static_cast<uint16_t>(static_cast<int8_t>(op & 0xff));
            WriteDmem(address, FetchSecondWord());
            break;
        }

    case Op::kJr:
        if (Condition(op & 0xf))
        {
            r.pc = ReadReg(op >> 5 & 7);
        }
        break;

    case Op::kCallr:
        if (Condition(op & 0xf))
        {
            const uint16_t target = ReadReg(op >> 5 & 7);
            Push(0, r.pc);
            r.pc = target;
        }
        break;

    case Op::kLrr:
    case Op::kLrrd:
    case Op::kLrri:
    case Op::kLrrn:
        {
            // The load is written first. The address register then moves, based on the registers at that point.
            const int s = op >> 5 & 3;
            WriteRegLoad(op & 0x1f, ReadDmem(r.ar[s]));
            r.ar[s] = SteppedAddress(s, op >> 7 & 3);
            break;
        }

    case Op::kSrr:
    case Op::kSrrd:
    case Op::kSrri:
    case Op::kSrrn:
        {
            const int d = op >> 5 & 3;
            const uint16_t value = ReadReg(op & 0x1f);
            WriteDmem(r.ar[d], value);
            r.ar[d] = SteppedAddress(d, op >> 7 & 3);
            break;
        }

    case Op::kMrr:
        {
            const uint16_t value = ReadReg(op & 0x1f);
            WriteRegLoad(op >> 5 & 0x1f, value);
            break;
        }

    case Op::kLrs:
        WriteRegLoad(0x18 + (op >> 8 & 7), ReadDmem(static_cast<uint16_t>(r.cr << 8 | (op & 0xff))));
        break;

    case Op::kSrsh:
        WriteDmem(static_cast<uint16_t>(r.cr << 8 | (op & 0xff)), ReadReg(0x10 + bit8));
        break;

    case Op::kSrs:
        WriteDmem(static_cast<uint16_t>(r.cr << 8 | (op & 0xff)), ReadReg(0x1c + (op >> 8 & 3)));
        break;

    case Op::kXorr:
        logic(bit8, AcM(bit8) ^ r.ax_h[op >> 9 & 1]);
        break;

    case Op::kXorc:
        logic(bit8, AcM(bit8) ^ AcM(1 - bit8));
        break;

    case Op::kNot:
        logic(bit8, AcM(bit8) ^ 0xffff);
        break;

    case Op::kAndr:
        logic(bit8, AcM(bit8) & r.ax_h[op >> 9 & 1]);
        break;

    case Op::kOrr:
        logic(bit8, AcM(bit8) | r.ax_h[op >> 9 & 1]);
        break;

    case Op::kAndc:
        logic(bit8, AcM(bit8) & AcM(1 - bit8));
        break;

    case Op::kOrc:
        logic(bit8, AcM(bit8) | AcM(1 - bit8));
        break;

    case Op::kLsrnrx:
    case Op::kAsrnrx:
        // A positive amount shifts left.
        set_acc(bit8, Shift(r.ac[bit8], ShiftAmount(r.ax_h[op >> 9 & 1]), decoded == Op::kAsrnrx));
        break;

    case Op::kLsrnr:
    case Op::kAsrnr:
        set_acc(bit8, Shift(r.ac[bit8], ShiftAmount(AcM(1 - bit8)), decoded == Op::kAsrnr));
        break;

    case Op::kAddr:
        add(bit8, Sext16(RawReg(0x18 + (op >> 9 & 3))) * 0x10000);
        break;

    case Op::kAddax:
        add(bit8, Ax(op >> 9 & 1));
        break;

    case Op::kAdd:
        add(bit8, r.ac[1 - bit8]);
        break;

    case Op::kAddp:
        add(bit8, Product());
        break;

    case Op::kSubr:
        sub(bit8, Sext16(RawReg(0x18 + (op >> 9 & 3))) * 0x10000);
        break;

    case Op::kSubax:
        sub(bit8, Ax(op >> 9 & 1));
        break;

    case Op::kSub:
        sub(bit8, r.ac[1 - bit8]);
        break;

    case Op::kSubp:
        sub(bit8, Product());
        break;

    case Op::kMovr:
        set_acc(bit8, Sext16(RawReg(0x18 + (op >> 9 & 3))) * 0x10000);
        break;

    case Op::kMovax:
        set_acc(bit8, Ax(op >> 9 & 1));
        break;

    case Op::kMov:
        set_acc(bit8, r.ac[1 - bit8]);
        break;

    case Op::kMovp:
    case Op::kMovnp:
    case Op::kMovpz:
        {
            // The flags come from the exact value before it's wrapped to 40 bits.
            const int64_t p = Product();
            const int64_t value = decoded == Op::kMovp ? p : decoded == Op::kMovnp ? -p : Round(p);
            SetAc(bit8, value);
            SetFlags40(value, false, false);
            break;
        }

    case Op::kAddaxl:
        add(bit8, r.ax_l[op >> 9 & 1]);
        break;

    case Op::kIncm:
        add(bit8, 0x10000);
        break;

    case Op::kInc:
        add(bit8, 1);
        break;

    case Op::kDecm:
        sub(bit8, 0x10000);
        break;

    case Op::kDec:
        sub(bit8, 1);
        break;

    case Op::kNeg:
        {
            const int64_t a = r.ac[bit8];
            const int64_t result = Wrap40(0 - a);
            r.ac[bit8] = result;
            SetFlagsSub(0, a, result);
            break;
        }

    case Op::kClr:
        set_acc(op >> 11 & 1, 0);
        break;

    case Op::kCmp:
        compare(r.ac[0], r.ac[1]);
        break;

    case Op::kMulaxh:
        SetProduct(mul(r.ax_h[0], r.ax_h[0], true, true));
        break;

    case Op::kClrp:
        r.prod_l = 0x0000;
        r.prod_m1 = 0xfff0;
        r.prod_h = 0x00ff;
        r.prod_m2 = 0x0010;
        break;

    case Op::kTstprod:
        SetFlags40(Product(), false, false);
        break;

    case Op::kTstaxh:
        SetFlags16(r.ax_h[bit8], false);
        break;

    case Op::kM2:
        r.sr &= ~kSrAm;
        break;

    case Op::kM0:
        r.sr |= kSrAm;
        break;

    case Op::kClr15:
        r.sr &= ~kSrSu;
        break;

    case Op::kSet15:
        r.sr |= kSrSu;
        break;

    case Op::kSet16:
        r.sr &= ~kSrSxm;
        break;

    case Op::kSet40:
        r.sr |= kSrSxm;
        break;

    case Op::kMul:
        {
            const int s = op >> 11 & 1;
            SetProduct(mul(r.ax_l[s], r.ax_h[s], true, true));
            break;
        }

    case Op::kAsr16:
        set_acc(op >> 11 & 1, r.ac[op >> 11 & 1] >> 16);
        break;

    case Op::kMulmvz:
    case Op::kMulac:
    case Op::kMulmv:
    case Op::kMulxmvz:
    case Op::kMulxac:
    case Op::kMulxmv:
    case Op::kMulcmvz:
    case Op::kMulcac:
    case Op::kMulcmv:
        {
            // The accumulator takes the old product (rounded, added or moved). Then the new product is computed.
            const int dest = bit8;
            const int64_t old_product = Product();
            int64_t product = 0;
            if (decoded == Op::kMulmvz || decoded == Op::kMulac || decoded == Op::kMulmv)
            {
                const int s = op >> 11 & 1;
                product = mul(r.ax_l[s], r.ax_h[s], true, true);
            }
            else if (decoded == Op::kMulxmvz || decoded == Op::kMulxac || decoded == Op::kMulxmv)
            {
                product = mulx(op >> 12 & 1, op >> 11 & 1);
            }
            else
            {
                product = mul(AcM(op >> 12 & 1), r.ax_h[op >> 11 & 1], true, true);
            }

            int64_t value = 0;
            if (decoded == Op::kMulmvz || decoded == Op::kMulxmvz || decoded == Op::kMulcmvz)
            {
                value = Round(old_product);
            }
            else if (decoded == Op::kMulac || decoded == Op::kMulxac || decoded == Op::kMulcac)
            {
                value = r.ac[dest] + old_product;
            }
            else
            {
                value = old_product;
            }

            set_acc(dest, value);
            SetProduct(product);
            break;
        }

    case Op::kMulx:
        SetProduct(mulx(op >> 12 & 1, op >> 11 & 1));
        break;

    case Op::kAbs:
        {
            const int d = op >> 11 & 1;
            set_acc(d, r.ac[d] < 0 ? -r.ac[d] : r.ac[d]);
            break;
        }

    case Op::kTst:
        SetFlags40(r.ac[op >> 11 & 1], false, false);
        break;

    case Op::kMulc:
        SetProduct(mul(AcM(op >> 12 & 1), r.ax_h[op >> 11 & 1], true, true));
        break;

    case Op::kCmpaxh:
        compare(r.ac[op >> 11 & 1], Sext16(r.ax_h[op >> 12 & 1]) * 0x10000);
        break;

    case Op::kMaddx:
    case Op::kMsubx:
        {
            const uint16_t a = (op >> 9 & 1) ? r.ax_h[0] : r.ax_l[0];
            const uint16_t b = (op >> 8 & 1) ? r.ax_h[1] : r.ax_l[1];
            const int64_t product = mul(a, b, true, true);
            SetProduct(decoded == Op::kMaddx ? Product() + product : Product() - product);
            break;
        }

    case Op::kMaddc:
    case Op::kMsubc:
        {
            const int64_t product = mul(AcM(op >> 9 & 1), r.ax_h[op >> 8 & 1], true, true);
            SetProduct(decoded == Op::kMaddc ? Product() + product : Product() - product);
            break;
        }

    case Op::kMadd:
    case Op::kMsub:
        {
            const int64_t product = mul(r.ax_l[bit8], r.ax_h[bit8], true, true);
            SetProduct(decoded == Op::kMadd ? Product() + product : Product() - product);
            break;
        }

    case Op::kLsl16:
        set_acc(bit8, Shift(r.ac[bit8], 16, false));
        break;

    case Op::kLsr16:
        set_acc(bit8, Shift(r.ac[bit8], -16, false));
        break;

    case Op::kAddpaxz:
        {
            // The rounded product plus $axS.h in the middle. The carry compares the old product with the result.
            const int d = bit8;
            const int64_t old_product = Product();
            SetAc(d, Round(old_product) + Sext16(r.ax_h[op >> 9 & 1]) * 0x10000);
            const bool carry = static_cast<uint64_t>(old_product) > static_cast<uint64_t>(r.ac[d]);
            SetFlags40(r.ac[d], carry, false);
            break;
        }

    case Op::kClrl:
        {
            // Despite its name, it rounds to a multiple of 0x10000. The flags come from the value before it's wrapped.
            const int64_t rounded = Round(r.ac[bit8]);
            SetAc(bit8, rounded);
            SetFlags40(rounded, false, false);
            break;
        }
    }

    // The extension's deferred results, after the main instruction. They replace anything the main instruction wrote.
    for (int i = 0; i < held.count; i++)
    {
        const PendingWrite& write = held.writes[i];
        if (write.kind == PendingWrite::kLoadExtended)
        {
            r.ac[write.reg - 0x1e] = Sext16(write.value) * 0x10000;
        }
        else
        {
            WriteReg(write.reg, write.value);
        }
    }
}

template <typename Memory>
NTDLIB_FORCE_INLINE void Dsp<Memory>::Extension(uint8_t field, HeldWrites& held)
{
    // Stores happen before loads. All memory accesses happen before the main instruction. The extension reads the
    // registers as they were before the step.
    Registers& r = regs_;

    // The step of $arI after an access: an increment, or an add of $ixI.
    const auto step = [this, &held](int i, bool add_index) NTDLIB_INLINE_LAMBDA
    {
        Hold(held, static_cast<uint8_t>(i), SteppedAddress(i, add_index ? kAddIndexStep : kIncrementStep));
    };

    if (field < 0x04)
    {
        return;
    }
    if (field < 0x10)
    {
        // 'DR, 'IR and 'NR
        const int i = field & 3;
        Hold(held, static_cast<uint8_t>(i), SteppedAddress(i, field >> 2 & 3));
        return;
    }
    if (field < 0x20)
    {
        // 'MV
        Hold(held, static_cast<uint8_t>(0x18 + (field >> 2 & 3)), ReadReg(0x1c + (field & 3)));
        return;
    }
    if (field < 0x40)
    {
        // 'S and 'SN
        const int d = field & 3;
        WriteDmem(r.ar[d], ReadReg(0x1c + (field >> 3 & 3)));
        step(d, (field & 4) != 0);
        return;
    }
    if (field < 0x80)
    {
        // 'L and 'LN: a load into $acX.m in 40-bit mode also sets h and l.
        const int s = field & 3;
        const auto dest = static_cast<uint8_t>(0x18 + (field >> 3 & 7));
        const uint16_t value = ReadDmem(r.ar[s]);
        const bool extended = (dest == 0x1e || dest == 0x1f) && (r.sr & kSrSxm);
        Hold(held, dest, value, extended ? PendingWrite::kLoadExtended : PendingWrite::kRegister);
        step(s, (field & 4) != 0);
        return;
    }
    if (field < 0xc0)
    {
        // 'LS, 'SL and their N and M forms: one store and one load through $ar0 and $ar3.
        const auto dest = static_cast<uint8_t>(0x18 + (field >> 4 & 3));
        const uint16_t stored = ReadReg(0x1e + (field & 1));
        const bool store_at_ar3 = (field & 2) == 0; // 'LS stores at $ar3 and loads from $ar0. 'SL does the reverse.
        uint16_t value = 0;
        if (store_at_ar3)
        {
            WriteDmem(r.ar[3], stored);
            value = ReadDmem(r.ar[0]);
        }
        else
        {
            WriteDmem(r.ar[0], stored);
            value = ReadDmem(r.ar[3]);
        }

        Hold(held, dest, value);
        const int variant = field >> 2 & 3; // 0 plain, 1 N, 2 M, 3 NM
        step(0, (variant & 1) != 0);
        step(3, (variant & 2) != 0);
        return;
    }

    // 'LD and 'LDAX: two loads. The second load reads from $ar3. If both addresses are in one 1024-word block, it reads
    // the first address again instead.
    const bool ldax = (field & 3) == 3;
    const int s = ldax ? (field >> 5 & 1) : (field & 3);
    const uint16_t first = ReadDmem(r.ar[s]);
    const uint16_t second = (r.ar[s] >> 10) == (r.ar[3] >> 10) ? ReadDmem(r.ar[s]) : ReadDmem(r.ar[3]);
    if (ldax)
    {
        const int axr = field >> 4 & 1;
        Hold(held, static_cast<uint8_t>(0x1a + axr), first);
        Hold(held, static_cast<uint8_t>(0x18 + axr), second);
    }
    else
    {
        Hold(held, static_cast<uint8_t>((field >> 5 & 1) ? 0x1a : 0x18), first);
        Hold(held, static_cast<uint8_t>((field >> 4 & 1) ? 0x1b : 0x19), second);
    }

    const int variant = field >> 2 & 3; // 0 plain, 1 N (on $arS), 2 M (on $ar3), 3 both
    step(s, (variant & 1) != 0);
    step(3, (variant & 2) != 0);
}

} // namespace ntdlib::dsp
