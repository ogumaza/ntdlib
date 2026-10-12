// SPDX-License-Identifier: MIT

// DSP tests: programs whose every step must match Dolphin's DSP interpreter (commit 1ab733d). The DSP matches that
// emulator. A program is a scenario: memory and registers to start from, and steps to run. Its report is hashed and
// compared with the hash of Dolphin's report of the same program (`dsp_test_hashes.h`). The report holds the registers
// after every step, the exceptions taken, the halts, and the final registers, stacks, hardware registers, accelerator,
// IRAM and DRAM. The programs are three written by hand (loops that end at one address, extended instructions, and the
// accelerator decoding ADPCM from main memory) and 1,000 random ones. A random program has random IRAM, DRAM and
// registers, and runs for 1,000 steps. Some random programs have no hash and are skipped. Dolphin can't run them to the
// end (it stops on a DMA longer than 0x4000 bytes), or they run or read boot ROM code. The DSP doesn't have the boot
// ROM.
//
// Every program runs with each console's memory. The programs are written for a Wii. Its accelerator reads main memory.
// With the GameCube's memory, a program's bytes in main memory also go to ARAM at the same address, for the
// accelerator. The Wii U's memory has checks of its own, of what it changes: the sizes of IRAM and DRAM, the
// coefficient ROM's place, the registers that give the top bits of DMA's and the accelerator's addresses, and the halt
// where a program leaves IRAM.
//
// usage: dsp_test [--scenarios <folder>]
//   --scenarios  write the random programs to <folder> as scenario files (r00000.scn, ...) for remaking the hashes

#include "ntdlib/dsp/dsp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dsp_test_hashes.h"
#include "ntdlib/dsp/coef_rom.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/types.h"
#include "xsflib/testing/check.h"

namespace
{

namespace fs = std::filesystem;
using namespace xsflib;
using namespace ntdlib;
using xsflib::testing::Check;

constexpr int kRandomPrograms = 1000;
constexpr int kRandomSteps = 1000;

// The hand-written programs.
constexpr std::string_view kLoops =
    R"(# Nested BLOOPIs that end at the same address: the inner loop runs 3 times, the outer one doesn't repeat, and
# its entries stay on the stacks.
start pc 0
iramw 0 0x1102 0x0005 0x1103 0x0005 0x7600 0x7700 0x0000 0x0021
trace on
until halt 100
)";

constexpr std::string_view kExtensions =
    R"(# Extended loads and stores alongside main instructions that use the same registers.
start pc 0x0010
dramw 0x0100 0xabcd
dramw 0x0200 0x1111
dramw 0x0300 0x3333
iramw 0x0010 0x8f00 0x009e 0x8000 0x0091 0x0012 0x009f 0x3456 0x0091 0x0012 0x1c5f 0x8e00 0x1c7f 0x0080 0x0100
iramw 0x001e 0x7660 0x0080 0x0200 0x0083 0x0300 0x80c0 0x0021
trace on
until halt 50
)";

constexpr std::string_view kAdpcm = R"(# The accelerator decoding ADPCM from main memory, read through ACDAT.
start pc 0x0010
memb 0x1000 0x12 0x12 0x34 0x56 0x78 0x9a 0xbc 0xde
memb 0x1008 0x23 0xf0 0x1e 0x2d 0x3c 0x4b 0x5a 0x69
memb 0x1010 0x45 0x77 0x88 0x99 0xaa 0xbb 0xcc 0xdd
iramw 0x0006 0x02ff 0x0000 0x02ff 0x0000 0x02ff
iramw 0x0010 0x0092 0x00ff 0x16d1 0x0000 0x16d4 0x0000 0x16d5 0x2002 0x16d6 0x0000 0x16d7 0x201f 0x16d8 0x0000
iramw 0x001e 0x16d9 0x2002 0x16da 0x0012 0x16db 0x0000 0x16dc 0x0000 0x16a2 0x0400 0x16a3 0xfe00 0x0080 0x0100
iramw 0x002c 0x1128 0x0031 0x20dd 0x1b18 0x20d9 0x1b18 0x0021
until halt 2000
)";

// Whether the report gives a hardware register's stored value. The report leaves out the accelerator's registers
// (0xFFD1-0xFFDF) because the accelerator state has a separate line. The interrupt and mailbox registers
// (0xFFFB-0xFFFF) are left out too because the mailbox lines give them.
bool Reported(int reg)
{
    return !(reg >= 0xd1 && reg <= 0xdf) && reg < 0xfb;
}

void AppendHex(std::string& out, uint64_t value, int digits)
{
    constexpr char kDigits[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
    {
        out += kDigits[value >> shift & 0xf];
    }
}

std::string Hex(uint64_t value, int digits)
{
    std::string out;
    AppendHex(out, value, digits);

    return out;
}

// splitmix64, for random programs that are the same on every platform.
class Random
{
public:
    explicit Random(uint64_t seed) : state_(seed)
    {
    }

    uint64_t Next()
    {
        state_ += 0x9e3779b97f4a7c15;
        uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;

        return z ^ (z >> 31);
    }

    // A number below `limit`.
    uint64_t Below(uint64_t limit)
    {
        return Next() % limit;
    }

private:
    uint64_t state_;
};

// Random state `n`: every register, IRAM and DRAM random, to run from 0. Like real programs, the wrapping registers are
// mostly 0xFFFF (no wrapping) or one less than a power of two. Bit 8 of $sr is clear because a write of $sr clears it.
std::string RandomState(int n)
{
    Random random(static_cast<uint64_t>(n) + 1);
    const auto wrap = [&random]
    {
        const uint64_t choice = random.Below(10);
        if (choice < 5)
        {
            return uint64_t{0xffff};
        }

        return choice < 8 ? (uint64_t{1} << random.Below(16)) - 1 : random.Below(0x10000);
    };

    std::string text = "start pc 0x0000\n";
    for (int i = 0; i < 4; i++)
    {
        text += "reg ar" + std::to_string(i) + " 0x" + Hex(random.Below(0x10000), 4) + "\n";
        text += "reg ix" + std::to_string(i) + " 0x" + Hex(random.Below(0x10000), 4) + "\n";
        text += "reg wr" + std::to_string(i) + " 0x" + Hex(wrap(), 4) + "\n";
    }
    for (int i = 0; i < 2; i++)
    {
        text += "reg ac" + std::to_string(i) + " 0x" + Hex(random.Below(uint64_t{1} << 40), 10) + "\n";
        text += "reg ax" + std::to_string(i) + " 0x" + Hex(random.Below(uint64_t{1} << 32), 8) + "\n";
    }
    text += "reg prod.l 0x" + Hex(random.Below(0x10000), 4) + "\n";
    text += "reg prod.m1 0x" + Hex(random.Below(0x10000), 4) + "\n";
    text += "reg prod.h 0x" + Hex(random.Below(0x100), 2) + "\n";
    text += "reg prod.m2 0x" + Hex(random.Below(3) == 0 ? random.Below(0x10000) : 0, 4) + "\n";
    text += "reg sr 0x" + Hex(random.Below(0x10000) & ~uint64_t{0x0100}, 4) + "\n";
    text += "reg cr 0x" + Hex(random.Below(2) == 0 ? random.Below(0x100) : 0, 2) + "\n";

    for (const char* memory : {"dramw", "iramw"})
    {
        for (int row = 0; row < 0x1000; row += 32)
        {
            text += std::string(memory) + " 0x" + Hex(row, 4);
            for (int i = 0; i < 32; i++)
            {
                text += " 0x" + Hex(random.Below(0x10000), 4);
            }
            text += "\n";
        }
    }

    return text;
}

// Random program `n`: random state `n`, run with a trace.
std::string RandomProgram(int n)
{
    return RandomState(n) + "trace on\nstep " + std::to_string(kRandomSteps) + "\n";
}

// A random program in which every fourth word of IRAM is `word`. The word then runs often.
std::string WordProgram(uint16_t word)
{
    std::string text = RandomState(0x10000 + word);
    for (int address = 0; address < 0x1000; address += 4)
    {
        text += "iramw 0x" + Hex(address, 4) + " 0x" + Hex(word, 4) + "\n";
    }

    return text + "trace on\nstep " + std::to_string(kRandomSteps) + "\n";
}

// Runs a scenario on the DSP and hashes its report (FNV-1a over its lines). Dolphin's reports were hashed the same way.
template <typename Memory>
class Scenario
{
public:
    // With `replay_every` above 0, every step whose number is a multiple of `replay_every` runs twice. The test copies
    // the DSP, runs the step and puts the copy back. Then the step runs for real. The report must come out the same.
    // With `generic`, every instruction goes through the generic decoder.
    explicit Scenario(int replay_every = 0, bool generic = false)
        : dsp_(std::make_unique<dsp::Dsp<Memory>>(memory_)), replay_every_(replay_every)
    {
        if (generic)
        {
            dsp_->UseGenericHandlers();
        }
    }

    // Runs `text`, a scenario. Returns false, with the reason in `error`, if it has a directive that this runner
    // doesn't support.
    bool Run(std::string_view text, std::string& error);

    // The hash of the report, its final state included.
    uint64_t Finish();

private:
    void Line(std::string_view line);
    bool Directive(const std::vector<std::string>& words, std::string& error);
    void SetRegister(const std::string& name, uint64_t value);

    // One step, with its events and trace line. Returns false if the DSP is halted.
    bool Step();

    std::string Registers() const;

    Memory memory_;
    std::unique_ptr<dsp::Dsp<Memory>> dsp_;
    int replay_every_ = 0;
    bool trace_ = false;
    uint64_t hash_ = 0xcbf29ce484222325;
};

template <typename Memory>
void Scenario<Memory>::Line(std::string_view line)
{
    for (const char c : line)
    {
        hash_ = (hash_ ^ static_cast<uint8_t>(c)) * 0x100000001b3;
    }
    hash_ = (hash_ ^ '\n') * 0x100000001b3;
}

template <typename Memory>
bool Scenario<Memory>::Run(std::string_view text, std::string& error)
{
    std::istringstream lines{std::string(text)};
    for (std::string line; std::getline(lines, line);)
    {
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        std::vector<std::string> words;
        for (std::string word; in >> word;)
        {
            words.push_back(word);
        }

        if (!words.empty() && !Directive(words, error))
        {
            return false;
        }
    }

    return true;
}

template <typename Memory>
bool Scenario<Memory>::Directive(const std::vector<std::string>& words, std::string& error)
{
    const std::string& kind = words[0];

    const auto number = [&words](std::size_t i)
    {
        return std::stoull(words.at(i), nullptr, 0);
    };

    if (kind == "start" && words.at(1) == "pc")
    {
        dsp_->StartAt(static_cast<uint16_t>(number(2)));
    }
    else if (kind == "reg")
    {
        SetRegister(words.at(1), number(2));
    }
    else if (kind == "iramw" || kind == "dramw")
    {
        std::vector<uint16_t> data;
        for (std::size_t i = 2; i < words.size(); i++)
        {
            data.push_back(static_cast<uint16_t>(number(i)));
        }

        const auto address = static_cast<uint16_t>(number(1));
        if (kind == "iramw")
        {
            dsp_->LoadIram(address, data);
        }
        else
        {
            dsp_->LoadDram(address, data);
        }
    }
    else if (kind == "memb")
    {
        const PAddr address = dsp::ToPhysical(static_cast<uint32_t>(number(1)));
        for (std::size_t i = 2; i < words.size(); i++)
        {
            const auto value = static_cast<uint8_t>(number(i));
            const PAddr at = address + static_cast<uint32_t>(i - 2);
            *memory_.At(at, 1) = value;
            if constexpr (!Memory::kAcceleratorReadsMem1)
            {
                if (uint8_t* aram = memory_.Aram(at, 1))
                {
                    *aram = value;
                }
            }
        }
    }
    else if (kind == "trace")
    {
        trace_ = words.at(1) == "on";
    }
    else if (kind == "step")
    {
        uint64_t ran = 0;
        while (ran < number(1) && Step())
        {
            ran++;
        }

        Line("event " + std::to_string(dsp_->Steps()) + " step " + words[1] + " ran " + std::to_string(ran) +
             (dsp_->Halted() ? " halted" : ""));
    }
    else if (kind == "until" && words.at(1) == "halt")
    {
        uint64_t ran = 0;
        while (!dsp_->Halted() && ran < number(2) && Step())
        {
            ran++;
        }

        Line("event " + std::to_string(dsp_->Steps()) + " until halt " + words[2] + " ran " + std::to_string(ran) +
             (dsp_->Halted() ? " met halted" : " not_met"));
    }
    else
    {
        error = "unknown directive " + kind;

        return false;
    }

    return true;
}

template <typename Memory>
void Scenario<Memory>::SetRegister(const std::string& name, uint64_t value)
{
    dsp::Registers& r = dsp_->regs_;
    const int i = name.back() - '0';
    if (name.starts_with("ar"))
    {
        r.ar[i] = static_cast<uint16_t>(value);
    }
    else if (name.starts_with("ix"))
    {
        r.ix[i] = static_cast<uint16_t>(value);
    }
    else if (name.starts_with("wr"))
    {
        r.wr[i] = static_cast<uint16_t>(value);
    }
    else if (name == "ac0" || name == "ac1")
    {
        r.ac[i] = static_cast<int64_t>(value << 24) >> 24;
    }
    else if (name == "ax0" || name == "ax1")
    {
        r.ax_h[i] = static_cast<uint16_t>(value >> 16);
        r.ax_l[i] = static_cast<uint16_t>(value);
    }
    else if (name == "prod.l")
    {
        r.prod_l = static_cast<uint16_t>(value);
    }
    else if (name == "prod.m1")
    {
        r.prod_m1 = static_cast<uint16_t>(value);
    }
    else if (name == "prod.h")
    {
        r.prod_h = static_cast<uint16_t>(value & 0xff);
    }
    else if (name == "prod.m2")
    {
        r.prod_m2 = static_cast<uint16_t>(value);
    }
    else if (name == "sr")
    {
        r.sr = static_cast<uint16_t>(value);
    }
    else if (name == "cr")
    {
        r.cr = static_cast<uint8_t>(value & 0xff);
    }
}

template <typename Memory>
bool Scenario<Memory>::Step()
{
    if (dsp_->Halted())
    {
        return false;
    }

    const uint16_t pc_before = dsp_->regs_.pc;
    if (replay_every_ > 0 && dsp_->Steps() % static_cast<uint64_t>(replay_every_) == 0)
    {
        const dsp::Dsp<Memory> before = *dsp_;
        dsp_->Step();
        *dsp_ = before;
    }

    dsp_->Step();
    const std::string step = std::to_string(dsp_->Steps());
    const int exception = dsp_->TakenException();
    if (exception >= 0)
    {
        Line("event " + step + " exception " + std::to_string(exception) + " from " + Hex(pc_before, 4));
    }

    if (dsp_->Halted())
    {
        Line("event " + step + " halt at " + Hex(dsp_->regs_.pc, 4));
    }

    // The trace line: where the step started (an exception's vector, if it took one), where it goes next, and the
    // registers after it.
    if (trace_)
    {
        const uint16_t pc = exception >= 0 ? static_cast<uint16_t>(exception * 2) : pc_before;
        std::string line = "trace " + step + " pc " + Hex(pc, 4) + " next " + Hex(dsp_->regs_.pc, 4);
        if (exception >= 0)
        {
            line += " exception " + std::to_string(exception);
        }
        Line(line + " " + Registers());
    }

    dsp_->TakeInterruptRequest();

    return true;
}

template <typename Memory>
std::string Scenario<Memory>::Registers() const
{
    const dsp::Registers& r = dsp_->regs_;
    std::string out = "ar";
    for (const uint16_t v : r.ar)
    {
        out += ' ';
        AppendHex(out, v, 4);
    }
    out += " ix";
    for (const uint16_t v : r.ix)
    {
        out += ' ';
        AppendHex(out, v, 4);
    }
    out += " wr";
    for (const uint16_t v : r.wr)
    {
        out += ' ';
        AppendHex(out, v, 4);
    }
    out += " st";
    for (const dsp::Stack& stack : r.stacks)
    {
        out += ' ';
        AppendHex(out, stack.top, 4);
    }
    out += " sp";
    for (const dsp::Stack& stack : r.stacks)
    {
        out += ' ';
        AppendHex(out, stack.pointer, 2);
    }

    out += " ac0 ";
    AppendHex(out, static_cast<uint64_t>(r.ac[0]) & 0xffffffffff, 10);
    out += " ac1 ";
    AppendHex(out, static_cast<uint64_t>(r.ac[1]) & 0xffffffffff, 10);
    out += " ax0 ";
    AppendHex(out, uint32_t{r.ax_h[0]} << 16 | r.ax_l[0], 8);
    out += " ax1 ";
    AppendHex(out, uint32_t{r.ax_h[1]} << 16 | r.ax_l[1], 8);
    out += " prod " + Hex(r.prod_l, 4) + " " + Hex(r.prod_m1, 4) + " " + Hex(r.prod_h, 4) + " " + Hex(r.prod_m2, 4);
    out += " sr " + Hex(r.sr, 4) + " cr " + Hex(r.cr, 4);

    return out;
}

template <typename Memory>
uint64_t Scenario<Memory>::Finish()
{
    // The registers as reads by number return them, raw: the accumulators' high parts sign-extended and no stack
    // popped.
    const dsp::Registers& r = dsp_->regs_;
    Line("final steps " + std::to_string(dsp_->Steps()));
    Line("final pc " + Hex(r.pc, 4));
    Line(std::string("final halted ") + (dsp_->Halted() ? "1" : "0"));

    const auto part = [&r](int ac, int shift)
    {
        const auto bits = static_cast<uint64_t>(r.ac[ac]);

        return shift == 32 ? static_cast<uint16_t>(static_cast<int8_t>(bits >> 32))
                           : static_cast<uint16_t>(bits >> shift);
    };

    const std::array<std::pair<const char*, uint16_t>, 32> registers = {{
        {"ar0", r.ar[0]},
        {"ar1", r.ar[1]},
        {"ar2", r.ar[2]},
        {"ar3", r.ar[3]},
        {"ix0", r.ix[0]},
        {"ix1", r.ix[1]},
        {"ix2", r.ix[2]},
        {"ix3", r.ix[3]},
        {"wr0", r.wr[0]},
        {"wr1", r.wr[1]},
        {"wr2", r.wr[2]},
        {"wr3", r.wr[3]},
        {"st0", r.stacks[0].top},
        {"st1", r.stacks[1].top},
        {"st2", r.stacks[2].top},
        {"st3", r.stacks[3].top},
        {"ac0.h", part(0, 32)},
        {"ac1.h", part(1, 32)},
        {"cr", r.cr},
        {"sr", r.sr},
        {"prod.l", r.prod_l},
        {"prod.m1", r.prod_m1},
        {"prod.h", r.prod_h},
        {"prod.m2", r.prod_m2},
        {"ax0.l", r.ax_l[0]},
        {"ax1.l", r.ax_l[1]},
        {"ax0.h", r.ax_h[0]},
        {"ax1.h", r.ax_h[1]},
        {"ac0.l", part(0, 0)},
        {"ac1.l", part(1, 0)},
        {"ac0.m", part(0, 16)},
        {"ac1.m", part(1, 16)},
    }};
    for (const auto& [name, value] : registers)
    {
        Line(std::string("reg ") + name + " " + Hex(value, 4));
    }
    Line("acc ac0 " + Hex(static_cast<uint64_t>(r.ac[0]) & 0xffffffffff, 10));
    Line("acc ac1 " + Hex(static_cast<uint64_t>(r.ac[1]) & 0xffffffffff, 10));

    for (int n = 0; n < 4; n++)
    {
        const dsp::Stack& stack = r.stacks[n];
        std::string line =
            "stack " + std::to_string(n) + " ptr " + Hex(stack.pointer, 2) + " top " + Hex(stack.top, 4) + " entries";
        for (const uint16_t v : stack.hidden)
        {
            line += " " + Hex(v, 4);
        }
        Line(line);
    }

    // The DSP's side of DSPCR (its reset, interrupt, halt and init bits), the mailboxes and the hardware registers.
    Line("control dsp " + Hex(dsp_->Control(0) & 0x0c07, 4));
    Line("mailbox cpu_to_dsp " + Hex(dsp_->MailboxToDsp(), 8));
    Line("mailbox dsp_to_cpu " + Hex(dsp_->MailboxToCpu(), 8));
    Line("exceptions pending " + Hex(dsp_->PendingExceptions(), 2));
    for (int row = 0; row < 256; row += 16)
    {
        std::string line = "ifx ff" + Hex(row, 2) + ":";
        for (int i = 0; i < 16; i++)
        {
            line += Reported(row + i) ? " " + Hex(dsp_->HardwareRegister(static_cast<uint8_t>(row + i)), 4) : " ----";
        }
        Line(line);
    }

    const dsp::Accelerator& a = dsp_->AcceleratorState();
    Line("accel start " + Hex(a.start, 8) + " end " + Hex(a.end, 8) + " current " + Hex(a.current, 8) + " format " +
         Hex(a.format, 4) + " gain " + Hex(a.gain, 4) + " yn1 " + Hex(a.yn1, 4) + " yn2 " + Hex(a.yn2, 4) +
         " pred_scale " + Hex(a.pred_scale, 4) + " input " + Hex(a.input, 4) + " reads_stopped " +
         (a.reads_stopped ? "1" : "0"));

    for (const auto& [name, words] : {std::pair{"iram ", dsp_->Iram()}, std::pair{"dram ", dsp_->Dram()}})
    {
        for (std::size_t row = 0; row < words.size(); row += 16)
        {
            std::string line = name + Hex(row, 4) + ":";
            for (std::size_t i = 0; i < 16; i++)
            {
                line += ' ';
                AppendHex(line, words[row + i], 4);
            }
            Line(line);
        }
    }

    return hash_;
}

// Runs a scenario and checks its report's hash. Steps are replayed as Scenario's constructor specifies.
template <typename Memory>
void CheckScenario(const std::string& name, std::string_view text, uint64_t expected, int replay_every = 0)
{
    Scenario<Memory> scenario(replay_every);
    std::string error;
    if (!scenario.Run(text, error))
    {
        Check(false, name + ": " + error);

        return;
    }

    const uint64_t hash = scenario.Finish();
    Check(hash == expected, name + ": report hash " + Hex(hash, 16) + ", Dolphin's hash " + Hex(expected, 16));
}

// Runs a scenario with the dedicated handlers for frequent words and with the generic decoder. Checks that the reports
// are the same.
template <typename Memory>
void CheckHandlers(const std::string& name, std::string_view text)
{
    std::array<uint64_t, 2> hashes{};
    for (int generic = 0; generic < 2; generic++)
    {
        Scenario<Memory> scenario(0, generic == 1);
        std::string error;
        if (!scenario.Run(text, error))
        {
            Check(false, name + ": " + error);

            return;
        }

        hashes[generic] = scenario.Finish();
    }

    Check(hashes[0] == hashes[1], name + ": the handler made for it and the generic decoder differ");
}

void WriteScenarios(const fs::path& folder)
{
    fs::create_directories(folder);
    for (int n = 0; n < kRandomPrograms; n++)
    {
        std::string name = std::to_string(n);
        name = "r" + std::string(5 - name.size(), '0') + name + ".scn";
        std::ofstream(folder / name, std::ios::binary) << RandomProgram(n);
    }
}

// Runs every check with one console's memory. `console` names it in the failures.
template <typename Memory>
void CheckAll(const std::string& console)
{
    CheckScenario<Memory>(console + ": loops", kLoops, test::kLoopsHash);
    CheckScenario<Memory>(console + ": extensions", kExtensions, test::kExtensionsHash);
    CheckScenario<Memory>(console + ": adpcm", kAdpcm, test::kAdpcmHash);
    for (const auto& [n, hash] : test::kRandomHashes)
    {
        CheckScenario<Memory>(console + ": random program " + std::to_string(n), RandomProgram(n), hash);
    }

    // Each word with a dedicated handler does what the generic decoder does with it.
    for (const uint16_t word : dsp::Dsp<Memory>::FrequentWords())
    {
        CheckHandlers<Memory>(console + ": word 0x" + Hex(word, 4), WordProgram(word));
    }

    // A copy of the DSP is a snapshot. Steps run from a copy that's then put back leave no trace.
    CheckScenario<Memory>(console + ": loops, replaying steps", kLoops, test::kLoopsHash, 1);
    CheckScenario<Memory>(console + ": extensions, replaying steps", kExtensions, test::kExtensionsHash, 1);
    CheckScenario<Memory>(console + ": adpcm, replaying steps", kAdpcm, test::kAdpcmHash, 1);
    for (const auto& [n, hash] : test::kRandomHashes)
    {
        if (n % 8 == 0)
        {
            CheckScenario<Memory>(console + ": random program " + std::to_string(n) + ", replaying steps",
                                  RandomProgram(n), hash, 5);
        }
    }
}

// Runs `program` from IRAM address `start` on the Wii U's DSP, with `dram` at DRAM address `dram_address`, until it
// halts. Main memory holds 0xABCD 0x1234 at 0x1000.
dsp::Dsp<dsp::WiiUMemory> RunWiiU(dsp::WiiUMemory& memory, uint16_t start, std::span<const uint16_t> program,
                                  uint16_t dram_address = 0, std::span<const uint16_t> dram = {})
{
    const std::array<uint8_t, 4> kWords = {0xab, 0xcd, 0x12, 0x34};
    std::copy(kWords.begin(), kWords.end(), memory.At(0x1000, kWords.size()));

    dsp::Dsp<dsp::WiiUMemory> dsp(memory);
    dsp.LoadDram(dram_address, dram);
    dsp.LoadIram(start, program);
    dsp.StartAt(start);
    dsp.Run(1000);

    return dsp;
}

// The Wii U's DSP: what WiiUMemory changes.
void CheckWiiU()
{
    dsp::WiiUMemory memory(0x10000);

    // DRAM runs to 0x2FFF, and the coefficient ROM lies at 0x3000 to 0x37FF.
    const std::array<uint16_t, 9> kReads = {
        0x00d8, 0x2fff, // lr $ax0.l, @0x2fff
        0x00d9, 0x1000, // lr $ax1.l, @0x1000: DRAM (the Wii has the ROM there)
        0x00da, 0x3000, // lr $ax0.h, @0x3000
        0x00db, 0x37ff, // lr $ax1.h, @0x37ff
        0x0021,         // halt
    };
    std::array<uint16_t, 0x2000> dram{};
    dram[0x0000] = 0x1234;
    dram[0x1fff] = 0x5a5a;

    auto dsp = RunWiiU(memory, 0, kReads, 0x1000, dram);

    Check(dsp.Halted() && dsp.regs_.ax_l[0] == 0x5a5a && dsp.regs_.ax_l[1] == 0x1234 &&
              dsp.regs_.ax_h[0] == dsp::kCoefficientRom[0] && dsp.regs_.ax_h[1] == dsp::kCoefficientRom[0x7ff],
          "Wii U: DRAM to 0x2FFF and the coefficient ROM at 0x3000");

    // IRAM runs to 0x1FFF. A program that leaves it halts the DSP.
    const std::array<uint16_t, 2> kLast = {0x0080, 0x1234}; // lri $ar0, #0x1234

    dsp = RunWiiU(memory, 0x1ffe, kLast);

    Check(dsp.Halted() && dsp.regs_.ar[0] == 0x1234 && dsp.regs_.pc == 0x2000, "Wii U: IRAM to 0x1FFF, then a halt");

    // DMA takes its address's top three bits from 0xFFBA, not from DSMAH.
    const std::array<uint16_t, 17> kDma = {
        0x16ce, 0xe000, // si @DSMAH, #0xe000
        0x16cf, 0x1000, // si @DSMAL, #0x1000
        0x16c9, 0x0000, // si @DSCR, #0: from main memory to DRAM
        0x16cd, 0x0100, // si @DSPA, #0x0100
        0x16cb, 0x0004, // si @DSBL, #4: 0x00001000
        0x16ba, 0x0001, // si @0xffba, #1
        0x16cd, 0x0200, // si @DSPA, #0x0200
        0x16cb, 0x0004, // si @DSBL, #4: 0x20001000, outside the memory
        0x0021,         // halt
    };

    dsp = RunWiiU(memory, 0, kDma);

    Check(dsp.Halted() && dsp.Dram()[0x100] == 0xabcd && dsp.Dram()[0x101] == 0x1234 && dsp.Dram()[0x200] == 0 &&
              dsp.Dram()[0x201] == 0,
          "Wii U: DMA's address extension");

    // The accelerator takes its byte address's top three bits from 0xFFBC.
    const std::array<uint16_t, 31> kAccelerator = {
        0x16d1, 0x0002,                 // si @FORMAT, #2: 16-bit reads
        0x16d4, 0x0000, 0x16d5, 0x0000, // the start, 0
        0x16d6, 0x3fff, 0x16d7, 0xffff, // the end, 0x3fffffff
        0x16d8, 0x1000, 0x16d9, 0x0800, // the address, 0x10000800: byte 0x20001000
        0x00d8, 0xffd3,                 // lr $ax0.l, @ACDRAW: 0x00001000
        0x16bc, 0x0001,                 // si @0xffbc, #1
        0x16d8, 0x0000, 0x16d9, 0x0800, // the address, 0x800: byte 0x1000
        0x00d9, 0xffd3,                 // lr $ax1.l, @ACDRAW: 0x20001000, outside the memory
        0x0021,                         // halt
    };

    dsp = RunWiiU(memory, 0, kAccelerator);

    Check(dsp.Halted() && dsp.regs_.ax_l[0] == 0xabcd && dsp.regs_.ax_l[1] == 0,
          "Wii U: the accelerator's address extension");
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "--scenarios")
        {
            WriteScenarios(argv[2]);

            return 0;
        }

        CheckAll<dsp::WiiMemory>("Wii");
        CheckAll<dsp::GameCubeMemory>("GameCube");
        CheckWiiU();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "exception: %s\n", e.what());

        return 1;
    }

    return xsflib::testing::failures == 0 ? 0 : 1;
}
