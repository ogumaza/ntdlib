// SPDX-License-Identifier: MIT

// Tests the CPU's skip of a polling loop against an interrupt. A loop of three instructions loads a word, compares it
// with 0 and branches back while it is 0. The OS's scheduler waits in such a loop while no thread is ready. An external
// interrupt arrives before each of the three instructions in turn. Its handler sets the word. After the handler, a pass
// of the loop reads the new word and leaves. The CPU has to reach the code after the loop within the instructions that
// the handler and the loop take. That holds wherever the interrupt arrived. A last case checks that the skip still
// runs a loop's passes at once while the word stays the same.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ppc/cpu.h"
#include "ntdlib/types.h"

namespace
{

using xsflib::testing::Check;

// The places of the program's parts in memory, and the I/O address at which a write lowers the interrupt line. The
// word is below 0x8000. An instruction addresses it there without a base register. The second loop reads its word
// through r7.
constexpr uint32_t kHandler = 0x500;
constexpr uint32_t kLoop = 0x1000;
constexpr uint32_t kSteadyLoop = 0x1800;
constexpr uint32_t kWord = 0x2000;
constexpr uint32_t kSteadyWord = 0x2100;
constexpr uint32_t kAcknowledge = 0x0c003000;

// The handler sets the word and lowers the interrupt line. It's four instructions long.
constexpr std::array<uint32_t, 4> kHandlerCode = {
    0x38a00001,         // li r5, 1
    0x90a00000 | kWord, // stw r5, kWord(0)
    0x90a40000,         // stw r5, 0(r4): the acknowledgement
    0x4c000064,         // rfi
};

// The loop, then the code after it: r6 becomes 1, and the CPU spins in a branch to itself.
constexpr std::array<uint32_t, 5> kLoopCode = {
    0x80600000 | kWord, // lwz r3, kWord(0)
    0x2c030000,         // cmpwi r3, 0
    0x4182fff8,         // beq -8
    0x38c00001,         // li r6, 1
    0x48000000,         // b .
};

// A loop whose word holds 0x0102 and never changes: lwz r3, 0(r7), cmpwi r3, 0x102, beq -8.
constexpr std::array<uint32_t, 3> kSteadyLoopCode = {
    0x80670000, // lwz r3, 0(r7)
    0x2c030102, // cmpwi r3, 0x102
    0x4182fff8, // beq -8
};

// 64 KiB of RAM, and one I/O register. A write to the register lowers the CPU's interrupt line.
class Machine : public ntdlib::ppc::Bus
{
public:
    Machine()
    {
        for (std::size_t i = 0; i < kHandlerCode.size(); i++)
        {
            xsflib::StoreBe32(&ram_[kHandler + i * 4], kHandlerCode[i]);
        }

        for (std::size_t i = 0; i < kLoopCode.size(); i++)
        {
            xsflib::StoreBe32(&ram_[kLoop + i * 4], kLoopCode[i]);
        }

        for (std::size_t i = 0; i < kSteadyLoopCode.size(); i++)
        {
            xsflib::StoreBe32(&ram_[kSteadyLoop + i * 4], kSteadyLoopCode[i]);
        }

        xsflib::StoreBe32(&ram_[kSteadyWord], 0x0102);

        // The first BAT pair of each kind maps the low 256 MiB onto itself. The loop runs with translation and
        // interrupts on. r4 contains the acknowledge address for the handler, and r7 the second loop's word.
        ntdlib::ppc::State& s = cpu.state_;
        s.spr[ntdlib::ppc::spr::kIbat0u] = 0x00001fff;
        s.spr[ntdlib::ppc::spr::kIbat0u + 1] = 2;
        s.spr[ntdlib::ppc::spr::kIbat0u + 8] = 0x00001fff;
        s.spr[ntdlib::ppc::spr::kIbat0u + 9] = 2;
        s.msr = ntdlib::ppc::msr::kIr | ntdlib::ppc::msr::kDr | ntdlib::ppc::msr::kEe;
        s.gpr[4] = kAcknowledge;
        s.gpr[7] = kSteadyWord;
        s.pc = kLoop;
        cpu.InvalidateTranslation();
    }

    uint8_t* RamPage(ntdlib::PAddr page) override
    {
        return page < ram_.size() ? &ram_[page] : nullptr;
    }

    bool ReadIo(ntdlib::PAddr, int, uint32_t&) override
    {
        return false;
    }

    bool WriteIo(ntdlib::PAddr address, int, uint32_t) override
    {
        if (address != kAcknowledge)
        {
            return false;
        }

        cpu.SetExternalInterrupt(false);

        return true;
    }

private:
    std::array<uint8_t, 0x10000> ram_{};

public:
    ntdlib::ppc::Cpu cpu{*this};
};

// Runs the loop until the CPU is about to run its instruction `at` (0 to 2), raises the interrupt and runs 16 more
// cycles. The handler is four instructions long. The loop then takes at most five to leave: the rest of the
// interrupted pass and a whole pass. The CPU must have run the instruction after the loop.
void CheckInterruptAt(uint32_t at)
{
    const std::string name = "an interrupt before instruction " + std::to_string(at) + " of the loop";
    Machine machine;
    ntdlib::ppc::Cpu& cpu = machine.cpu;
    cpu.Run(3000);
    for (int i = 0; i < 3 && cpu.state_.pc != kLoop + at * 4; i++)
    {
        cpu.Run(1);
    }

    Check(cpu.state_.pc == kLoop + at * 4 && cpu.state_.gpr[6] == 0, name + ": the CPU waits in the loop");

    cpu.SetExternalInterrupt(true);
    cpu.Run(16);
    Check(cpu.state_.gpr[5] == 1, name + ": the handler ran");
    Check(cpu.state_.spr[ntdlib::ppc::spr::kSrr0] == kLoop + at * 4, name + ": the interrupt came before it");
    Check(cpu.state_.gpr[6] == 1 && cpu.state_.pc == kLoop + 16, name + ": the CPU left the loop");
}

// Runs a loop whose word never changes for 3,000 cycles, without an interrupt. The skip has to run its passes at once:
// the CPU starts only a few of them. Without the skip, it would start about 1,000.
void CheckSteadyLoop()
{
    Machine machine;
    ntdlib::ppc::Cpu& cpu = machine.cpu;
    cpu.state_.pc = kSteadyLoop;
    int passes = 0;
    cpu.Watch({kSteadyLoop}, [&passes](uint32_t) { passes++; });
    cpu.Run(3000);
    Check(cpu.state_.gpr[3] == 0x0102, "the steady loop read its word");
    Check(passes > 0 && passes <= 4, "the skip ran the steady loop's passes at once: " + std::to_string(passes) +
                                         " passes started");
}

} // namespace

int main()
{
    for (uint32_t at = 0; at < 3; at++)
    {
        CheckInterruptAt(at);
    }

    CheckSteadyLoop();

    return xsflib::testing::Summary("ppc idle");
}
