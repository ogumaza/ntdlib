// SPDX-License-Identifier: MIT

// The DSP's instruction set: decoding, the main instructions, the parallel load/store extensions, and the static
// analysis of IRAM for loop ends and polling loops.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>

#include "dsp/internal.h"
#include "ntdlib/dsp/dsp.h"

namespace ntdlib::dsp
{
namespace
{

enum class Op : uint8_t
{
    kUnknown,
    kNop,
    kDar,
    kIar,
    kSubarn,
    kAddarn,
    kHalt,
    kLoop,
    kBloop,
    kLri,
    kLr,
    kSr,
    kAddi,
    kIlrr,
    kIlrrd,
    kIlrri,
    kIlrrn,
    kXori,
    kAndi,
    kOri,
    kIf,
    kCmpi,
    kJmp,
    kAndf,
    kCall,
    kAndcf,
    kLsrn,
    kAsrn,
    kRet,
    kRti,
    kAddis,
    kCmpis,
    kLris,
    kLoopi,
    kBloopi,
    kSbclr,
    kSbset,
    kLsl,
    kLsr,
    kAsl,
    kAsr,
    kSi,
    kJr,
    kCallr,
    kLrr,
    kLrrd,
    kLrri,
    kLrrn,
    kSrr,
    kSrrd,
    kSrri,
    kSrrn,
    kMrr,
    kLrs,
    kSrsh,
    kSrs,
    kXorr,
    kXorc,
    kNot,
    kAndr,
    kLsrnrx,
    kOrr,
    kAsrnrx,
    kAndc,
    kLsrnr,
    kOrc,
    kAsrnr,
    kAddr,
    kAddax,
    kAdd,
    kAddp,
    kSubr,
    kSubax,
    kSub,
    kSubp,
    kMovr,
    kMovax,
    kMov,
    kMovp,
    kAddaxl,
    kIncm,
    kInc,
    kDecm,
    kDec,
    kNeg,
    kMovnp,
    kNx,
    kClr,
    kCmp,
    kMulaxh,
    kClrp,
    kTstprod,
    kTstaxh,
    kM2,
    kM0,
    kClr15,
    kSet15,
    kSet16,
    kSet40,
    kMul,
    kAsr16,
    kMulmvz,
    kMulac,
    kMulmv,
    kMulx,
    kAbs,
    kTst,
    kMulxmvz,
    kMulxac,
    kMulxmv,
    kMulc,
    kCmpaxh,
    kMulcmvz,
    kMulcac,
    kMulcmv,
    kMaddx,
    kMsubx,
    kMaddc,
    kMsubc,
    kLsl16,
    kMadd,
    kLsr16,
    kMsub,
    kAddpaxz,
    kClrl,
    kMovpz,
};

struct Pattern
{
    uint16_t mask;
    uint16_t value;
    Op op;
};

// The opcode map: the bits that must match, and their values.
constexpr Pattern kPatterns[] = {
    {0xfffc, 0x0000, Op::kNop},     {0xfffc, 0x0004, Op::kDar},     {0xfffc, 0x0008, Op::kIar},
    {0xfffc, 0x000c, Op::kSubarn},  {0xfff0, 0x0010, Op::kAddarn},  {0xffff, 0x0021, Op::kHalt},
    {0xffe0, 0x0040, Op::kLoop},    {0xffe0, 0x0060, Op::kBloop},   {0xffe0, 0x0080, Op::kLri},
    {0xffe0, 0x00c0, Op::kLr},      {0xffe0, 0x00e0, Op::kSr},      {0xfeff, 0x0200, Op::kAddi},
    {0xfefc, 0x0210, Op::kIlrr},    {0xfefc, 0x0214, Op::kIlrrd},   {0xfefc, 0x0218, Op::kIlrri},
    {0xfefc, 0x021c, Op::kIlrrn},   {0xfeff, 0x0220, Op::kXori},    {0xfeff, 0x0240, Op::kAndi},
    {0xfeff, 0x0260, Op::kOri},     {0xfff0, 0x0270, Op::kIf},      {0xfeff, 0x0280, Op::kCmpi},
    {0xfff0, 0x0290, Op::kJmp},     {0xfeff, 0x02a0, Op::kAndf},    {0xfff0, 0x02b0, Op::kCall},
    {0xfeff, 0x02c0, Op::kAndcf},   {0xffff, 0x02ca, Op::kLsrn},    {0xffff, 0x02cb, Op::kAsrn},
    {0xfff0, 0x02d0, Op::kRet},     {0xfff0, 0x02f0, Op::kRti},     {0xfe00, 0x0400, Op::kAddis},
    {0xfe00, 0x0600, Op::kCmpis},   {0xf800, 0x0800, Op::kLris},    {0xff00, 0x1000, Op::kLoopi},
    {0xff00, 0x1100, Op::kBloopi},  {0xff00, 0x1200, Op::kSbclr},   {0xff00, 0x1300, Op::kSbset},
    {0xfec0, 0x1400, Op::kLsl},     {0xfec0, 0x1440, Op::kLsr},     {0xfec0, 0x1480, Op::kAsl},
    {0xfec0, 0x14c0, Op::kAsr},     {0xff00, 0x1600, Op::kSi},      {0xff10, 0x1700, Op::kJr},
    {0xff10, 0x1710, Op::kCallr},   {0xff80, 0x1800, Op::kLrr},     {0xff80, 0x1880, Op::kLrrd},
    {0xff80, 0x1900, Op::kLrri},    {0xff80, 0x1980, Op::kLrrn},    {0xff80, 0x1a00, Op::kSrr},
    {0xff80, 0x1a80, Op::kSrrd},    {0xff80, 0x1b00, Op::kSrri},    {0xff80, 0x1b80, Op::kSrrn},
    {0xfc00, 0x1c00, Op::kMrr},     {0xf800, 0x2000, Op::kLrs},     {0xfe00, 0x2800, Op::kSrsh},
    {0xfc00, 0x2c00, Op::kSrs},     {0xfc80, 0x3000, Op::kXorr},    {0xfe80, 0x3080, Op::kXorc},
    {0xfe80, 0x3280, Op::kNot},     {0xfc80, 0x3400, Op::kAndr},    {0xfc80, 0x3480, Op::kLsrnrx},
    {0xfc80, 0x3800, Op::kOrr},     {0xfc80, 0x3880, Op::kAsrnrx},  {0xfe80, 0x3c00, Op::kAndc},
    {0xfe80, 0x3c80, Op::kLsrnr},   {0xfe80, 0x3e00, Op::kOrc},     {0xfe80, 0x3e80, Op::kAsrnr},
    {0xf800, 0x4000, Op::kAddr},    {0xfc00, 0x4800, Op::kAddax},   {0xfe00, 0x4c00, Op::kAdd},
    {0xfe00, 0x4e00, Op::kAddp},    {0xf800, 0x5000, Op::kSubr},    {0xfc00, 0x5800, Op::kSubax},
    {0xfe00, 0x5c00, Op::kSub},     {0xfe00, 0x5e00, Op::kSubp},    {0xf800, 0x6000, Op::kMovr},
    {0xfc00, 0x6800, Op::kMovax},   {0xfe00, 0x6c00, Op::kMov},     {0xfe00, 0x6e00, Op::kMovp},
    {0xfc00, 0x7000, Op::kAddaxl},  {0xfe00, 0x7400, Op::kIncm},    {0xfe00, 0x7600, Op::kInc},
    {0xfe00, 0x7800, Op::kDecm},    {0xfe00, 0x7a00, Op::kDec},     {0xfe00, 0x7c00, Op::kNeg},
    {0xfe00, 0x7e00, Op::kMovnp},   {0xf700, 0x8000, Op::kNx},      {0xf700, 0x8100, Op::kClr},
    {0xff00, 0x8200, Op::kCmp},     {0xff00, 0x8300, Op::kMulaxh},  {0xff00, 0x8400, Op::kClrp},
    {0xff00, 0x8500, Op::kTstprod}, {0xfe00, 0x8600, Op::kTstaxh},  {0xff00, 0x8a00, Op::kM2},
    {0xff00, 0x8b00, Op::kM0},      {0xff00, 0x8c00, Op::kClr15},   {0xff00, 0x8d00, Op::kSet15},
    {0xff00, 0x8e00, Op::kSet16},   {0xff00, 0x8f00, Op::kSet40},   {0xf700, 0x9000, Op::kMul},
    {0xf700, 0x9100, Op::kAsr16},   {0xf600, 0x9200, Op::kMulmvz},  {0xf600, 0x9400, Op::kMulac},
    {0xf600, 0x9600, Op::kMulmv},   {0xe700, 0xa000, Op::kMulx},    {0xf700, 0xa100, Op::kAbs},
    {0xf700, 0xb100, Op::kTst},     {0xe600, 0xa200, Op::kMulxmvz}, {0xe600, 0xa400, Op::kMulxac},
    {0xe600, 0xa600, Op::kMulxmv},  {0xe700, 0xc000, Op::kMulc},    {0xe700, 0xc100, Op::kCmpaxh},
    {0xe600, 0xc200, Op::kMulcmvz}, {0xe600, 0xc400, Op::kMulcac},  {0xe600, 0xc600, Op::kMulcmv},
    {0xfc00, 0xe000, Op::kMaddx},   {0xfc00, 0xe400, Op::kMsubx},   {0xfc00, 0xe800, Op::kMaddc},
    {0xfc00, 0xec00, Op::kMsubc},   {0xfe00, 0xf000, Op::kLsl16},   {0xfe00, 0xf200, Op::kMadd},
    {0xfe00, 0xf400, Op::kLsr16},   {0xfe00, 0xf600, Op::kMsub},    {0xfc00, 0xf800, Op::kAddpaxz},
    {0xfe00, 0xfc00, Op::kClrl},    {0xfe00, 0xfe00, Op::kMovpz},
};

const std::array<Op, 0x10000>& DecodeTable()
{
    static const std::array<Op, 0x10000> kTable = []
    {
        std::array<Op, 0x10000> built{};
        for (uint32_t op = 0; op < 0x10000; op++)
        {
            for (const Pattern& pattern : kPatterns)
            {
                if ((op & pattern.mask) == pattern.value)
                {
                    built[op] = pattern.op;
                    break;
                }
            }
        }

        return built;
    }();

    return kTable;
}

Op Decode(uint16_t op)
{
    return DecodeTable()[op];
}

// The decoding as a constant expression, for the dedicated handlers of single instruction words.
constexpr Op DecodeConstant(uint16_t op)
{
    for (const Pattern& pattern : kPatterns)
    {
        if ((op & pattern.mask) == pattern.value)
        {
            return pattern.op;
        }
    }

    return Op::kUnknown;
}

// The instruction words that AX's microcodes run most while mixing. Each of these gets a dedicated handler. Any other
// word goes through Execute.
constexpr uint16_t kFrequentWords[] = {
    0x001f, 0x005a, 0x0078, 0x0080, 0x0081, 0x0082, 0x0083, 0x009a, 0x009b, 0x009f, 0x00c0, 0x00dc, 0x00de,
    0x00df, 0x00e0, 0x00fe, 0x00ff, 0x0213, 0x0277, 0x0291, 0x0292, 0x0294, 0x0295, 0x029c, 0x029f, 0x02a0,
    0x02bf, 0x02c0, 0x02df, 0x0313, 0x0601, 0x0a0e, 0x1112, 0x115f, 0x147e, 0x14f6, 0x14fb, 0x1512, 0x1577,
    0x16c9, 0x16cb, 0x16cd, 0x16fb, 0x16fc, 0x16fd, 0x176f, 0x185a, 0x191b, 0x191c, 0x191e, 0x193c, 0x193e,
    0x195c, 0x195d, 0x195e, 0x1adf, 0x1b1a, 0x1b1e, 0x1b3e, 0x1b3f, 0x1b5e, 0x1c7e, 0x1c7f, 0x1f1d, 0x1fa1,
    0x1fbc, 0x1ffe, 0x26c9, 0x26fc, 0x26fe, 0x27ff, 0x2ccf, 0x2dcd, 0x2ece, 0x2fcf, 0x3400, 0x3500, 0x4700,
    0x4a28, 0x4ac3, 0x4b23, 0x4c00, 0x4e5a, 0x4e5b, 0x4f00, 0x5000, 0x6838, 0x6994, 0x8090, 0x80c9, 0x80f1,
    0x8100, 0x8271, 0x8900, 0x8912, 0x8970, 0x8a00, 0x8b00, 0x8c00, 0x8d00, 0x8e00, 0x8e60, 0x8f00, 0x90c3,
    0x9100, 0x9140, 0x917a, 0x9500, 0x95c3, 0x97c3, 0x9972, 0xa031, 0xa100, 0xac38, 0xad30, 0xae00, 0xb100,
    0xb179, 0xb42b, 0xb523, 0xb651, 0xc100, 0xd400, 0xd431, 0xdc31, 0xf000, 0xf04a, 0xf04b, 0xf0a1, 0xf1a0,
};

bool TwoWords(Op op)
{
    switch (op)
    {
    case Op::kLri:
    case Op::kLr:
    case Op::kSr:
    case Op::kSi:
    case Op::kAddi:
    case Op::kXori:
    case Op::kAndi:
    case Op::kOri:
    case Op::kCmpi:
    case Op::kAndf:
    case Op::kAndcf:
    case Op::kJmp:
    case Op::kCall:
    case Op::kBloop:
    case Op::kBloopi:
        return true;
    default:
        return false;
    }
}

// round(v): to the nearest multiple of 0x10000, ties to even.
int64_t Round(int64_t value)
{
    const int64_t biased = (value & 0x10000) ? value + 0x8000 : value + 0x7fff;

    return static_cast<int64_t>(static_cast<uint64_t>(biased) & ~uint64_t{0xffff});
}

// A shift of a 40-bit value: left by a positive amount, right by a negative one, logically (as unsigned) or
// arithmetically.
int64_t Shift(int64_t value, int amount, bool arithmetic)
{
    if (amount >= 0)
    {
        return Wrap40(static_cast<int64_t>(amount >= 64 ? 0 : static_cast<uint64_t>(value) << amount));
    }

    const int right = -amount;
    if (arithmetic)
    {
        return right >= 64 ? (value < 0 ? -1 : 0) : value >> right;
    }

    return right >= 64 ? 0 : static_cast<int64_t>(Unsigned40(value) >> right);
}

// The amount of a shift by a register: 6 bits, negative when bit 6 is set. A value of 0 gives 0 whatever bit 6 holds.
int ShiftAmount(uint16_t value)
{
    const int k = value & 0x3f;
    if (k == 0)
    {
        return 0;
    }

    return (value & 0x40) ? k - 64 : k;
}

} // namespace

template <typename Memory>
void Dsp<Memory>::BuildDecodeTable()
{
    DecodeTable();
}

template <typename Memory>
int Dsp<Memory>::SizeOf(uint16_t op) const
{
    return TwoWords(Decode(op)) ? 2 : 1;
}

template <typename Memory>
uint16_t Dsp<Memory>::FetchSecondWord()
{
    const uint16_t word = ReadImem(regs_.pc);
    regs_.pc++;

    return word;
}

template <typename Memory>
void Dsp<Memory>::SkipNextInstruction()
{
    regs_.pc = static_cast<uint16_t>(regs_.pc + SizeOf(ReadImem(regs_.pc)));
}

template <typename Memory>
void Dsp<Memory>::BuildLoopMap()
{
    // A linear walk of IRAM that decodes each instruction from the start, data included. LOOP and LOOPI end their loop
    // at the next address. BLOOP and BLOOPI end theirs at the address in their second word. That address may lie
    // outside IRAM. The polling loops are recognised by their words.
    std::fill(loop_end_.begin(), loop_end_.end(), uint8_t{0});
    std::fill(idle_.begin(), idle_.end(), uint8_t{0});

    const auto word = [this](uint32_t address) -> uint16_t
    {
        return address < Memory::kIramSize ? iram_[address] : 0;
    };

    for (uint32_t address = 0; address < Memory::kIramSize;)
    {
        const uint16_t op = iram_[address];
        const Op decoded = Decode(op);
        if (decoded == Op::kLoop || decoded == Op::kLoopi)
        {
            loop_end_[address + 1] = 1;
        }
        else if (decoded == Op::kBloop || decoded == Op::kBloopi)
        {
            loop_end_[word(address + 1)] = 1;
        }

        const uint16_t w1 = word(address + 1);
        const uint16_t w2 = word(address + 2);
        const uint16_t w3 = word(address + 3);
        const bool lrs_poll = (op == 0x26fc || op == 0x27fc || op == 0x26fe || op == 0x27fe) && w2 == 0x8000 &&
                              ((op & 0xff) == 0xfc ? (w1 == (op == 0x26fc ? 0x02c0 : 0x03c0) && w3 == 0x029d) ||
                                                         (w1 == (op == 0x26fc ? 0x02a0 : 0x03a0) && w3 == 0x029c)
                                                   : w1 == (op == 0x26fe ? 0x02c0 : 0x03c0) && w3 == 0x029c);
        const bool lr_poll =
            op == 0x00de && w1 == 0xfffe && w2 == 0x02c0 && w3 == 0x8000 && word(address + 4) == 0x029c;
        const bool dram_poll = op == 0x00da && w1 == 0x0352 && w2 == 0x8600 && w3 == 0x0295;
        if (lrs_poll || lr_poll || dram_poll)
        {
            idle_[address] = 1;
        }

        address += TwoWords(decoded) ? 2 : 1;
    }
}

template <typename Memory>
void Dsp<Memory>::Hold(HeldWrites& held, uint8_t reg, uint16_t value)
{
    held.writes[held.count++] = {PendingWrite::kRegister, reg, value};
}

template <typename Memory>
std::span<const uint16_t> Dsp<Memory>::FrequentWords()
{
    return kFrequentWords;
}

template <typename Memory>
void Dsp<Memory>::UseGenericHandlers()
{
    handlers_ = GenericHandlers();
}

template <typename Memory>
const typename Dsp<Memory>::Handler* Dsp<Memory>::GenericHandlers()
{
    static const std::array<Handler, 0x10000> kTable = []
    {
        std::array<Handler, 0x10000> built;
        built.fill([](Dsp& dsp, uint16_t op) { dsp.Execute(op); });

        return built;
    }();

    return kTable.data();
}

template <typename Memory>
const typename Dsp<Memory>::Handler* Dsp<Memory>::Handlers()
{
    static const std::array<Handler, 0x10000> kTable = []
    {
        std::array<Handler, 0x10000> built;
        std::copy_n(GenericHandlers(), built.size(), built.begin());

        [&built]<std::size_t... i>(std::index_sequence<i...>)
        {
            ((built[kFrequentWords[i]] = &ExecuteWord<kFrequentWords[i]>), ...);
        }(std::make_index_sequence<std::size(kFrequentWords)>());

        return built;
    }();

    return kTable.data();
}

template <typename Memory>
template <uint16_t word>
void Dsp<Memory>::ExecuteWord(Dsp& dsp, uint16_t op)
{
    static_cast<void>(op);
    constexpr auto kKind = static_cast<uint8_t>(DecodeConstant(word));
    dsp.ExecuteOp(word, kKind);
}

template <typename Memory>
void Dsp<Memory>::Execute(uint16_t op)
{
    ExecuteOp(op, static_cast<uint8_t>(Decode(op)));
}

template <typename Memory>
NTDLIB_DSP_FORCE_INLINE void Dsp<Memory>::ExecuteOp(uint16_t op, uint8_t kind)
{
    Registers& r = regs_;
    HeldWrites held;

    // The extension's memory accesses come first. Its register results wait until after the main instruction.
    if (op >= 0x3000)
    {
        Extension(static_cast<uint8_t>(op >= 0x4000 ? op & 0xff : op & 0x7f), held);
    }

    const auto mul = [this](uint16_t a, uint16_t b, bool a_signed, bool b_signed)
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
    const auto mulx = [&](int s, int t)
    {
        const uint16_t a = s ? r.ax_h[0] : r.ax_l[0];
        const uint16_t b = t ? r.ax_h[1] : r.ax_l[1];
        const bool su = (r.sr & kSrSu) != 0;
        const bool a_signed = !su || s == 1;
        const bool b_signed = !su || t == 1;

        return mul(a, b, su && s == 0 && t == 0 ? false : a_signed, su && s == 0 && t == 0 ? false : b_signed);
    };

    const auto add = [this](int d, int64_t b)
    {
        const int64_t a = regs_.ac[d];
        const int64_t result = Wrap40(a + b);
        regs_.ac[d] = result;
        SetFlagsAdd(a, b, result);
    };

    const auto sub = [this](int d, int64_t b)
    {
        const int64_t a = regs_.ac[d];
        const int64_t result = Wrap40(a - b);
        regs_.ac[d] = result;
        SetFlagsSub(a, b, result);
    };

    const auto compare = [this](int64_t a, int64_t b)
    {
        SetFlagsSub(a, b, Wrap40(a - b));
    };

    const auto logic = [this](int d, uint16_t m)
    {
        SetAcM(d, m);
        const int64_t value = regs_.ac[d];
        SetFlags16(m, value < kS32Min || value > kS32Max);
    };

    const auto set_acc = [this](int d, int64_t value)
    {
        SetAc(d, value);
        SetFlags40(regs_.ac[d], false, false);
    };

    const int bit8 = op >> 8 & 1;
    switch (static_cast<Op>(kind))
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
            const uint16_t count = Decode(op) == Op::kLoop ? ReadReg(op & 0x1f) : static_cast<uint16_t>(op & 0xff);
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
            const uint16_t count = Decode(op) == Op::kBloop ? ReadReg(op & 0x1f) : static_cast<uint16_t>(op & 0xff);
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
            switch (Decode(op))
            {
            case Op::kIlrrd:
                r.ar[s] = Decrement(r.ar[s], r.wr[s]);
                break;
            case Op::kIlrri:
                r.ar[s] = Increment(r.ar[s], r.wr[s]);
                break;
            case Op::kIlrrn:
                r.ar[s] = AddIndex(r.ar[s], r.ix[s], r.wr[s]);
                break;
            default:
                break;
            }
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
        set_acc(0, Shift(r.ac[0], -ShiftAmount(AcM(1)), Decode(op) == Op::kAsrn));
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
            set_acc(bit8, Shift(r.ac[bit8], -k, Decode(op) == Op::kAsr));
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
            switch (Decode(op))
            {
            case Op::kLrrd:
                r.ar[s] = Decrement(r.ar[s], r.wr[s]);
                break;
            case Op::kLrri:
                r.ar[s] = Increment(r.ar[s], r.wr[s]);
                break;
            case Op::kLrrn:
                r.ar[s] = AddIndex(r.ar[s], r.ix[s], r.wr[s]);
                break;
            default:
                break;
            }
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
            switch (Decode(op))
            {
            case Op::kSrrd:
                r.ar[d] = Decrement(r.ar[d], r.wr[d]);
                break;
            case Op::kSrri:
                r.ar[d] = Increment(r.ar[d], r.wr[d]);
                break;
            case Op::kSrrn:
                r.ar[d] = AddIndex(r.ar[d], r.ix[d], r.wr[d]);
                break;
            default:
                break;
            }
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
        set_acc(bit8, Shift(r.ac[bit8], ShiftAmount(r.ax_h[op >> 9 & 1]), Decode(op) == Op::kAsrnrx));
        break;

    case Op::kLsrnr:
    case Op::kAsrnr:
        set_acc(bit8, Shift(r.ac[bit8], ShiftAmount(AcM(1 - bit8)), Decode(op) == Op::kAsrnr));
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
            const int64_t value = Decode(op) == Op::kMovp ? p : Decode(op) == Op::kMovnp ? -p : Round(p);
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
            const Op decoded = Decode(op);
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
            SetProduct(Decode(op) == Op::kMaddx ? Product() + product : Product() - product);
            break;
        }

    case Op::kMaddc:
    case Op::kMsubc:
        {
            const int64_t product = mul(AcM(op >> 9 & 1), r.ax_h[op >> 8 & 1], true, true);
            SetProduct(Decode(op) == Op::kMaddc ? Product() + product : Product() - product);
            break;
        }

    case Op::kMadd:
    case Op::kMsub:
        {
            const int64_t product = mul(r.ax_l[bit8], r.ax_h[bit8], true, true);
            SetProduct(Decode(op) == Op::kMadd ? Product() + product : Product() - product);
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
NTDLIB_DSP_FORCE_INLINE void Dsp<Memory>::Extension(uint8_t field, HeldWrites& held)
{
    // Stores happen before loads. All memory accesses happen before the main instruction. The extension reads the
    // registers as they were before the step.
    Registers& r = regs_;

    const auto increment = [this, &held](int i)
    {
        Hold(held, static_cast<uint8_t>(i), Increment(regs_.ar[i], regs_.wr[i]));
    };

    const auto add_index = [this, &held](int i)
    {
        Hold(held, static_cast<uint8_t>(i), AddIndex(regs_.ar[i], regs_.ix[i], regs_.wr[i]));
    };

    if (field < 0x04)
    {
        return;
    }
    if (field < 0x10)
    {
        const int i = field & 3;
        switch (field >> 2 & 3)
        {
        case 1: // 'DR
            Hold(held, static_cast<uint8_t>(i), Decrement(r.ar[i], r.wr[i]));
            break;
        case 2: // 'IR
            increment(i);
            break;
        default: // 'NR
            add_index(i);
            break;
        }

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
        if (field & 4)
        {
            add_index(d);
        }
        else
        {
            increment(d);
        }

        return;
    }
    if (field < 0x80)
    {
        // 'L and 'LN: a load into $acX.m in 40-bit mode also sets h and l.
        const int s = field & 3;
        const auto dest = static_cast<uint8_t>(0x18 + (field >> 3 & 7));
        const uint16_t value = ReadDmem(r.ar[s]);
        if ((dest == 0x1e || dest == 0x1f) && (r.sr & kSrSxm))
        {
            held.writes[held.count++] = {PendingWrite::kLoadExtended, dest, value};
        }
        else
        {
            Hold(held, dest, value);
        }

        if (field & 4)
        {
            add_index(s);
        }
        else
        {
            increment(s);
        }

        return;
    }
    if (field < 0xc0)
    {
        // 'LS, 'SL and their N and M forms: one store and one load through $ar0 and $ar3.
        const auto dest = static_cast<uint8_t>(0x18 + (field >> 4 & 3));
        const uint16_t stored = ReadReg(0x1e + (field & 1));
        const bool load_first_is_store_at_ar3 = (field & 2) == 0; // 'LS: store at $ar3, load from $ar0
        uint16_t value = 0;
        if (load_first_is_store_at_ar3)
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
        if (variant & 1)
        {
            add_index(0);
        }
        else
        {
            increment(0);
        }

        if (variant & 2)
        {
            add_index(3);
        }
        else
        {
            increment(3);
        }

        return;
    }

    // 'LD and 'LDAX: two loads. The second load reads from $ar3. If both addresses lie in one 1024-word block, it reads
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
    if (variant & 1)
    {
        add_index(s);
    }
    else
    {
        increment(s);
    }

    if (variant & 2)
    {
        add_index(3);
    }
    else
    {
        increment(3);
    }
}

} // namespace ntdlib::dsp
