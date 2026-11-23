// SPDX-License-Identifier: MIT

// Tests of the bursts in which the machine runs the ARM9 (ntdlib/ntr/machine.h), on cartridge images that run only on a
// DSi and on DS images, with small programs for the ARM9.

#include <cstdint>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/sound.h"
#include "ntr_dsi_test_util.h"

namespace
{

using namespace ntr_dsi_tests;
using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Condition;
using ntdlib::ntr::Machine;
using ntdlib::ntr::Rom;
using ntdlib::ntr::SoundOutput;

// A program for the ARM9, and where it stops.
struct Program
{
    Bytes code;
    uint32_t stop = 0; // the address of the branch to itself that ends the program
};

// What the burst test's program starts.
enum class Start
{
    kCard,
    kTimer,
    kVcount,
    kHblankDma,
};

// The ARM9's program for the burst tests, at 0x02000000. It starts an event. Then it stops.
// - kCard: a read of 0x200 bytes of the card at 0x8000, ready 2,600 cycles later.
// - kTimer: timer 0 with its IRQ, to overflow after 0x100 cycles.
// - kVcount: DISPSTAT's IRQ at line 2, 4,260 cycles from the boot.
// - kHblankDma: DMA 0 at the horizontal blank, 1,536 cycles from the boot.
// For the first three, it then counts down a loop of 20,000 rounds. The loop takes far longer than the event's wait.
// The program then reads the register that shows the event once: ROMCTRL, or IF. It stores the value at 0x02200000. A
// loop without I/O doesn't poll. The event's time therefore comes only where a burst ends. For the DMA, it waits for
// the DMA to end.
Program BurstProgram(Start start)
{
    ArmAssembler a(kMainMemory);
    a.LoadConstant(4, kIo);
    const int stop = a.NewLabel();
    const int loop = a.NewLabel();
    uint32_t shown = 0x214; // the register that shows the event
    switch (start)
    {
    case Start::kCard:
        a.Mov(0, 0x80);
        a.Strb(0, 4, 0x1A1);
        a.LoadConstant(0, 0x800000B7);
        a.Str(0, 4, 0x1A8);
        a.Mov(0, 0);
        a.Str(0, 4, 0x1AC);
        a.LoadConstant(0, 0xA1000000);
        a.Str(0, 4, 0x1A4); // ROMCTRL: a block of 0x200 bytes
        shown = 0x1A4;
        break;
    case Start::kTimer:
        a.LoadConstant(0, 0x00C0FF00);
        a.Str(0, 4, 0x100); // TM0CNT: the reload 0xFF00, on, with the IRQ
        break;
    case Start::kVcount:
        a.LoadConstant(0, 0x0220);
        a.Strh(0, 4, 0x004); // DISPSTAT: the IRQ at line 2
        break;
    case Start::kHblankDma:
        {
            a.LoadConstant(0, 0x02200000);
            a.Str(0, 4, 0x0B0);
            a.LoadConstant(0, 0x02200010);
            a.Str(0, 4, 0x0B4);
            a.LoadConstant(0, 0x94000001);
            a.Str(0, 4, 0x0B8); // DMA0CNT: on, at the horizontal blank, a word
            const int wait = a.NewLabel();
            a.Bind(wait);
            a.Ldr(0, 4, 0x0B8);
            a.Tst(0, 0x80000000);
            a.B(wait, Condition::kNe);
            a.B(stop);
            break;
        }
    }

    a.LoadConstant(1, 20'000);
    a.Bind(loop);
    a.Sub(1, 1, 1, true);
    a.B(loop, Condition::kNe);
    a.Ldr(0, 4, static_cast<int>(shown));
    a.LoadConstant(1, 0x02200000);
    a.Str(0, 1, 0);
    a.Bind(stop);
    a.B(stop);
    Program program;
    program.stop = a.Address(stop);
    program.code = a.Finish();

    return program;
}

// The ARM9's programs for the tests of a burst on a DS, at 0x02000000. Each counts down a loop of 20,000 rounds and
// starts the card's read of kCard.
// - With `loop_first`, the loop comes first. The program then waits for the card's data: a poll of ROMCTRL.
// - Otherwise the read comes first, and 100 reads of ROMCTRL follow. The poll ends the burst before the card's data are
//   ready. After the loop, the program reads ROMCTRL once and stores the value at 0x02200000.
Program DsBurstProgram(bool loop_first)
{
    ArmAssembler a(kMainMemory);
    a.LoadConstant(4, kIo);
    const int loop = a.NewLabel();
    const int poll = a.NewLabel();
    const int stop = a.NewLabel();
    const auto count_down = [&a, loop]
    {
        a.LoadConstant(1, 20'000);
        a.Bind(loop);
        a.Sub(1, 1, 1, true);
        a.B(loop, Condition::kNe);
    };
    const auto start_card = [&a]
    {
        a.Mov(0, 0x80);
        a.Strb(0, 4, 0x1A1);
        a.LoadConstant(0, 0x800000B7);
        a.Str(0, 4, 0x1A8);
        a.Mov(0, 0);
        a.Str(0, 4, 0x1AC);
        a.LoadConstant(0, 0xA1000000);
        a.Str(0, 4, 0x1A4);
    };

    if (loop_first)
    {
        count_down();
        start_card();
        a.Bind(poll);
        a.Ldr(0, 4, 0x1A4);
        a.Tst(0, 0x00800000);
        a.B(poll, Condition::kEq);
    }
    else
    {
        start_card();
        a.Mov(2, 100);
        a.Bind(poll);
        a.Ldr(0, 4, 0x1A4);
        a.Sub(2, 2, 1, true);
        a.B(poll, Condition::kNe);
        count_down();
        a.Ldr(0, 4, 0x1A4);
        a.LoadConstant(1, 0x02200000);
        a.Str(0, 1, 0);
    }

    a.Bind(stop);
    a.B(stop);
    Program program;
    program.stop = a.Address(stop);
    program.code = a.Finish();

    return program;
}

// The run of `program` on an image with `unit_code`: whether it stops by the first vertical blank, the clock at which
// it stops, and the value that it stored at 0x02200000.
struct BurstRun
{
    bool stopped = false;
    uint64_t end = 0;
    uint32_t shown = 0;
};

BurstRun RunProgram(const Program& program, uint8_t unit_code)
{
    const Bytes image = DsiImage(program.code, Words(kHalts), Pattern(0x200, 0x55), Pattern(0x10, 0), unit_code);
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    machine.Arm9().SetStopAddress(program.stop);
    std::vector<SoundOutput> out;
    constexpr uint64_t kFirstBlank = 192 * 2130;
    BurstRun run;
    if (machine.Run(kFirstBlank, out))
    {
        run.stopped = true;
        run.end = machine.Clock();
        run.shown = Word(machine, 0x02200000);
    }

    return run;
}

// The burst program's run, in DSi mode unless `unit_code` gives a DS image.
BurstRun RunBurst(Start start, uint8_t unit_code = 3)
{
    return RunProgram(BurstProgram(start), unit_code);
}

void TestBurstEnds()
{
    // In DSi mode, a burst that starts an event earlier than the next ends at once. The next burst ends at the event.
    // The program's loop therefore runs across the event, and its read sees it: the card's data ready (ROMCTRL's bit
    // 23), the timer's IRQ (IF's bit 3), and line 2's (IF's bit 2). The loop ends well before the first vertical
    // blank.
    const BurstRun card = RunBurst(Start::kCard);
    XSFLIB_CHECK(card.end != 0 && card.end < 0x20000);
    XSFLIB_CHECK((card.shown & 0x00800000) != 0);
    const BurstRun timer = RunBurst(Start::kTimer);
    XSFLIB_CHECK(timer.end != 0 && timer.end < 0x20000);
    XSFLIB_CHECK((timer.shown & 0x08) != 0);
    const BurstRun vcount = RunBurst(Start::kVcount);
    XSFLIB_CHECK(vcount.end != 0 && vcount.end < 0x20000);
    XSFLIB_CHECK((vcount.shown & 0x04) != 0);

    // The DMA at line 0's horizontal blank. The program waits for it.
    const BurstRun hblank = RunBurst(Start::kHblankDma);
    XSFLIB_CHECK(hblank.end >= 1536 && hblank.end < 1536 + 16);
}

void TestDsBursts()
{
    // On a DS, a burst runs for a slice, whatever events are due. The card's read starts in the first burst. A poll
    // ends that burst. The second burst runs the loop and the read of ROMCTRL before the machine handles the card's
    // event. The read therefore doesn't see the data ready.
    const BurstRun read_first = RunProgram(DsBurstProgram(false), 0);
    XSFLIB_CHECK(read_first.stopped && (read_first.shown & 0x00800000) == 0);

    // A burst that starts an event goes on, and its time passes when its poll ends it: the loop of 20,000 rounds before
    // the card's read counts, about 20,000 cycles of the system clock.
    const BurstRun loop_first = RunProgram(DsBurstProgram(true), 0);
    XSFLIB_CHECK(loop_first.stopped && loop_first.end >= 20'000);
}

} // namespace

int main()
{
    TestBurstEnds();
    TestDsBursts();

    return xsflib::testing::Summary("DSi bursts");
}
