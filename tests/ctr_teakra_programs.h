// SPDX-License-Identifier: MIT

// Random DSP programs and states for the Teakra tests (ctr_teakra_jit_test, ctr_teakra_state_test): programs of mostly
// ordinary instructions with a few jumps and block repeats, random register states, and the DSPs to run them on.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <teakra/disassembler.h>
#include <teakra/impl/register.h>
#include <teakra/teakra.h>

namespace ntdlib::ctr::teakra_test
{

constexpr std::size_t kMemoryBytes = 0x80000;
constexpr uint32_t kProgramWords = 0x20000;

// Jumps, calls, returns, repeats and the like, by the disassembler's name for them: programs keep few of them (and more
// of the block repeats, whose loops the JIT runs itself), so that the runs between them, which the JIT translates, are
// long.
enum class Kind : uint8_t
{
    kOrdinary,
    kFlow,
    kBlockRepeat,
    kThrows, // undefined, or the interpreter throws for it whatever the state (not something to test)
};

// Sets TEAKRA_JIT while a DSP is made, which is when its JIT reads it.
class JitSetting
{
public:
    explicit JitSetting(const char* value)
    {
        if (const char* old = std::getenv("TEAKRA_JIT"))
        {
            old_ = old;
            had_ = true;
        }

        Set(value);
    }

    ~JitSetting()
    {
        Set(had_ ? old_.c_str() : nullptr);
    }

private:
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("TEAKRA_JIT", value ? value : "");
#else
        if (value)
        {
            setenv("TEAKRA_JIT", value, 1);
        }
        else
        {
            unsetenv("TEAKRA_JIT");
        }
#endif
    }

    std::string old_;
    bool had_ = false;
};

// A DSP with the JIT or without. Each program gets new ones: Reset leaves some MMIO registers as they were.
inline std::unique_ptr<Teakra::Teakra> MakeDsp(bool with_jit)
{
    JitSetting setting(with_jit ? nullptr : "0");
    return std::make_unique<Teakra::Teakra>(Teakra::UserConfig{});
}

// Marks the opcodes the interpreter throws for from a reset state, which programs leave out.
inline void FindThrowing(Teakra::Teakra& dsp, std::vector<Kind>& kinds)
{
    for (uint32_t op = 0; op < 0x10000; ++op)
    {
        if (kinds[op] != Kind::kOrdinary)
        {
            continue;
        }

        dsp.Reset();
        uint8_t* memory = dsp.GetDspMemory();
        memory[0] = static_cast<uint8_t>(op);
        memory[1] = static_cast<uint8_t>(op >> 8);
        memory[2] = memory[3] = 0;

        try
        {
            dsp.Run(1);
        }
        catch (const std::exception&)
        {
            kinds[op] = Kind::kThrows;
        }
    }
}

// The kind of each opcode. The first call runs each ordinary opcode once on a DSP (FindThrowing). Teakra may report
// MMIO on stdout during those runs. The tests therefore make the first call after SilenceStdout.
inline const std::vector<Kind>& Kinds()
{
    static const std::vector<Kind> kKinds = []
    {
        static const char* const kFlow[] = {"br",   "brr",  "call",  "calla", "callr", "ret",
                                            "rets", "reti", "retd",  "retic", "retid", "retidc",
                                            "rep",  "trap", "break", "cntx",  "eint",  "dint"};

        std::vector<Kind> table(0x10000, Kind::kOrdinary);
        for (uint32_t op = 0; op < 0x10000; ++op)
        {
            const std::vector<std::string> tokens = Teakra::Disassembler::GetTokenList(static_cast<uint16_t>(op));
            if (tokens.empty() || tokens[0] == "[ERROR]")
            {
                table[op] = Kind::kThrows;
                continue;
            }

            if (tokens[0] == "bkrep")
            {
                table[op] = Kind::kBlockRepeat;
            }

            for (const char* name : kFlow)
            {
                if (tokens[0] == name)
                {
                    table[op] = Kind::kFlow;
                }
            }
        }

        FindThrowing(*MakeDsp(false), table);

        return table;
    }();
    return kKinds;
}

inline Kind KindOf(uint16_t opcode)
{
    return Kinds()[opcode];
}

// A random state: data, pointers, flags and the modes that decide how the JIT translates.
inline void Randomize(Teakra::RegisterState& r, std::mt19937_64& rng)
{
    const auto bits = [&](int n)
    {
        return static_cast<uint16_t>(rng() & ((1u << n) - 1));
    };

    const auto acc = [&]
    {
        const uint64_t v = rng() & 0xFF'FFFF'FFFFull;
        return (v & (1ull << 39)) ? v | 0xFFFF'FF00'0000'0000ull : v;
    };

    for (auto& v : r.a)
    {
        v = acc();
    }
    for (auto& v : r.b)
    {
        v = acc();
    }
    r.a1s = acc();
    r.b1s = acc();

    for (auto& v : r.x)
    {
        v = bits(16);
    }
    for (auto& v : r.y)
    {
        v = bits(16);
    }
    for (auto& v : r.p)
    {
        v = static_cast<uint32_t>(rng());
    }
    for (auto& v : r.pe)
    {
        v = bits(1);
    }
    for (auto& v : r.ps)
    {
        v = bits(2);
    }

    for (auto& v : r.r)
    {
        v = bits(16);
    }
    r.sp = bits(16);
    r.page = bits(8);
    r.mixp = bits(16);
    r.sv = (rng() & 1) ? bits(16) : static_cast<uint16_t>(static_cast<int>(rng() % 81) - 40);

    for (uint16_t* flag : {&r.fz, &r.fm, &r.fn, &r.fv, &r.fe, &r.fc0, &r.fc1, &r.flm, &r.fvl, &r.fr})
    {
        *flag = bits(1);
    }
    r.sat = bits(1);
    r.sata = bits(1);
    r.s = bits(1);
    r.hwm = bits(2);

    r.stepi = bits(7);
    r.stepj = bits(7);
    r.modi = bits(9);
    r.modj = bits(9);
    r.stepi0 = bits(16);
    r.stepj0 = bits(16);
    for (auto& v : r.m)
    {
        v = (rng() % 4) == 0;
    }
    for (auto& v : r.br)
    {
        v = (rng() % 8) == 0;
    }

    r.stp16 = bits(1);
    r.cmd = bits(1);
    r.epi = bits(1);
    r.epj = bits(1);
    for (auto* array : {&r.arstep, &r.arpstepi, &r.arpstepj})
    {
        for (auto& v : *array)
        {
            v = bits(3);
        }
    }
    for (auto* array : {&r.aroffset, &r.arpoffseti, &r.arpoffsetj})
    {
        for (auto& v : *array)
        {
            v = bits(2);
        }
    }
    for (auto& v : r.arrn)
    {
        v = bits(3);
    }

    // Two bits each: arprnj picks r4 to r7 with 0 to 3.
    for (auto* array : {&r.arprni, &r.arprnj})
    {
        for (auto& v : *array)
        {
            v = bits(2);
        }
    }

    r.cpc = bits(1);
    r.repc = bits(16);
}

// Names the RegisterState field at byte `offset`, for a report.
inline std::string FieldAt(std::size_t offset)
{
    Teakra::RegisterState r;
    const auto* base = reinterpret_cast<const uint8_t*>(&r);

    struct Field
    {
        const char* name;
        const void* at;
        std::size_t size;
    };
#define NTDLIB_FIELD(f) (Field{#f, &r.f, sizeof(r.f)})
    const Field fields[] = {
        NTDLIB_FIELD(pc),         NTDLIB_FIELD(prpage),     NTDLIB_FIELD(cpc),      NTDLIB_FIELD(repc),
        NTDLIB_FIELD(rep),        NTDLIB_FIELD(bcn),        NTDLIB_FIELD(lp),       NTDLIB_FIELD(bkrep_stack),
        NTDLIB_FIELD(a),          NTDLIB_FIELD(b),          NTDLIB_FIELD(a1s),      NTDLIB_FIELD(b1s),
        NTDLIB_FIELD(sat),        NTDLIB_FIELD(sata),       NTDLIB_FIELD(s),        NTDLIB_FIELD(sv),
        NTDLIB_FIELD(fz),         NTDLIB_FIELD(fm),         NTDLIB_FIELD(fn),       NTDLIB_FIELD(fv),
        NTDLIB_FIELD(fe),         NTDLIB_FIELD(fc0),        NTDLIB_FIELD(fc1),      NTDLIB_FIELD(flm),
        NTDLIB_FIELD(fvl),        NTDLIB_FIELD(fr),         NTDLIB_FIELD(x),        NTDLIB_FIELD(y),
        NTDLIB_FIELD(hwm),        NTDLIB_FIELD(p),          NTDLIB_FIELD(pe),       NTDLIB_FIELD(ps),
        NTDLIB_FIELD(r),          NTDLIB_FIELD(mixp),       NTDLIB_FIELD(sp),       NTDLIB_FIELD(page),
        NTDLIB_FIELD(stepi),      NTDLIB_FIELD(stepj),      NTDLIB_FIELD(modi),     NTDLIB_FIELD(modj),
        NTDLIB_FIELD(stepi0),     NTDLIB_FIELD(stepj0),     NTDLIB_FIELD(m),        NTDLIB_FIELD(br),
        NTDLIB_FIELD(arstep),     NTDLIB_FIELD(arpstepi),   NTDLIB_FIELD(arpstepj), NTDLIB_FIELD(aroffset),
        NTDLIB_FIELD(arpoffseti), NTDLIB_FIELD(arpoffsetj), NTDLIB_FIELD(arrn),     NTDLIB_FIELD(arprni),
        NTDLIB_FIELD(arprnj),     NTDLIB_FIELD(ie),
    };
#undef NTDLIB_FIELD

    for (const Field& f : fields)
    {
        const std::size_t start = static_cast<const uint8_t*>(f.at) - base;
        if (offset >= start && offset < start + f.size)
        {
            return std::string(f.name) + "+" + std::to_string(offset - start);
        }
    }

    return "offset " + std::to_string(offset);
}

// Runs `dsp` for `cycles` cycles. Returns what the run threw, or an empty string.
inline std::string RunCatching(Teakra::Teakra& dsp, uint32_t cycles)
{
    try
    {
        dsp.Run(cycles);
    }
    catch (const std::exception& e)
    {
        return std::string("threw: ") + e.what();
    }

    return {};
}

// DSP memory for a program: few jumps (and some block repeats) in program memory, and random data.
inline std::vector<uint8_t> RandomMemory(std::mt19937_64& rng)
{
    std::vector<uint8_t> memory(kMemoryBytes);
    for (uint32_t word = 0; word < kMemoryBytes / 2; ++word)
    {
        uint16_t value = static_cast<uint16_t>(rng());
        while (word < kProgramWords)
        {
            const Kind kind = KindOf(value);
            if (kind == Kind::kOrdinary ||
                (kind != Kind::kThrows && (rng() % 100) < (kind == Kind::kBlockRepeat ? 30u : 2u)))
            {
                break;
            }

            value = static_cast<uint16_t>(rng());
        }
        memory[word * 2] = static_cast<uint8_t>(value);
        memory[word * 2 + 1] = static_cast<uint8_t>(value >> 8);
    }

    return memory;
}

} // namespace ntdlib::ctr::teakra_test
