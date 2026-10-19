// SPDX-License-Identifier: MIT

// The boot ROM routines that games use, implemented here because the ROM isn't shipped. The entry at 0x8000 and its
// command loop take a microcode's addresses from the CPU's mails. The loader at 0x80B5 loads a microcode by DMA and
// jumps to it. An older AX microcode also calls four mixing routines. Each routine takes as many steps as the ROM's
// code. It does its work on the first step and returns on the last.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "dsp/internal.h"
#include "ntdlib/dsp/dsp.h"

namespace ntdlib::dsp
{
namespace
{

// The ROM's routines, after the entry at kRomEntry.
constexpr uint16_t kCommandLoop = 0x8040; // where the model keeps the PC while the command loop runs
constexpr uint16_t kLoader = 0x80b5;
constexpr uint16_t kMixTwoConstant = 0x80e7;
constexpr uint16_t kMixOneConstant = 0x81f9;
constexpr uint16_t kMixTwoRamped = 0x8282;
constexpr uint16_t kMixOneRamped = 0x845d;

// The ROM's mails to the CPU: the ready mail that the entry sends, and the high halves of its replies to a mail that
// isn't a command and to an unknown command.
constexpr uint32_t kReadyMail = 0x8071feed;
constexpr uint16_t kNotCommandReply = 0xfeee;
constexpr uint16_t kUnknownCommandReply = 0xfaaa;

// The CPU's commands: mails with kCommandMark in their high halves. Six of them take the next mail as the address, the
// length or the destination of the microcode's IRAM or DRAM image. kStart takes the entry point and starts the loader.
constexpr uint16_t kCommandMark = 0x80f3;
constexpr uint16_t kIramAddress = 0xa001;
constexpr uint16_t kIramLength = 0xa002;
constexpr uint16_t kIramDestination = 0xc002;
constexpr uint16_t kDramAddress = 0xb001;
constexpr uint16_t kDramLength = 0xb002;
constexpr uint16_t kDramDestination = 0xc001;
constexpr uint16_t kStart = 0xd001;

} // namespace

template <typename Memory>
void Dsp<Memory>::StartMicrocode(std::span<const uint16_t> microcode, uint32_t address, uint16_t entry)
{
    // The state that the boot ROM leaves when the SDK's boot code loads a microcode from `address` and starts it at
    // `entry`. By then the OS's audio initialisation has run the init program. That program clears DRAM.
    regs_ = Registers{};
    regs_.pc = entry;
    regs_.ar[0] = entry;
    regs_.ix = {static_cast<uint16_t>(address >> 16 | 0x8000), static_cast<uint16_t>(address), 0,
                static_cast<uint16_t>(microcode.size() * 2)};
    regs_.ac[0] = 0x20000;
    regs_.ac[1] = int64_t{regs_.ix[3]} << 16;
    regs_.sr = kSrAm | kSrLogicZero | kSrTopBits;
    regs_.cr = 0xff;

    std::fill(iram_.begin(), iram_.end(), uint16_t{0});
    std::copy(microcode.begin(), microcode.begin() + std::min<std::size_t>(microcode.size(), Memory::kIramSize),
              iram_.begin());
    std::fill(dram_.begin(), dram_.end(), uint16_t{0});

    hardware_ = {};
    hardware_[kDscr] = 0x0002;
    hardware_[kDsmah] = regs_.ix[0];
    hardware_[kDsmal] = regs_.ix[1];
    accelerator_ = Accelerator{};

    control_ = control::kInit;
    mailbox_to_dsp_ = entry;
    mailbox_to_cpu_ = kReadyMail & ~kMailboxFull; // the ready mail, taken by the CPU
    pending_exceptions_ = 0;
    interrupt_request_ = false;
    rom_state_ = Rom::kNone;

    BuildLoopMap();
}

template <typename Memory>
bool Dsp<Memory>::AtIdleLoop() const
{
    if (rom_state_ == Rom::kWaitCommand || rom_state_ == Rom::kWaitArgument || rom_state_ == Rom::kWaitMailTaken)
    {
        return true;
    }

    return regs_.pc < Memory::kIramSize && idle_[regs_.pc];
}

template <typename Memory>
void Dsp<Memory>::RomStep()
{
    switch (rom_state_)
    {
    case Rom::kNone:
        RomStartRoutine();
        return;

    case Rom::kCommandStart:
        SetAc(0, 0);
        SetAc(1, 0);
        SetFlags40(0, false, false);
        rom_state_ = Rom::kWaitCommand;
        return;

    case Rom::kWaitCommand:
        RomTakeCommand();
        return;

    case Rom::kWaitArgument:
        RomTakeArgument();
        return;

    case Rom::kWaitMailTaken:
        if (!(mailbox_to_cpu_ & kMailboxFull))
        {
            regs_.sr |= kSrLogicZero;
            rom_state_ = Rom::kCommandStart;
        }

        return;

    case Rom::kLoading:
    case Rom::kMixing:
        // The routine's remaining steps do nothing. The last one jumps to the microcode or returns.
        if (--rom_delay_ == 0)
        {
            regs_.pc = rom_state_ == Rom::kLoading ? regs_.ar[0] : Pop(0);
            rom_state_ = Rom::kNone;
        }

        return;
    }
}

template <typename Memory>
void Dsp<Memory>::RomStartRoutine()
{
    switch (regs_.pc)
    {
    case kRomEntry:
        RomEntry();
        return;

    case kLoader:
        RomLoader();
        return;

    case kMixTwoConstant:
    case kMixOneConstant:
    case kMixTwoRamped:
    case kMixOneRamped:
        RomMix(regs_.pc);
        return;

    default:
        // An address outside the modelled routines. Without the ROM's code, the DSP stops.
        control_ |= control::kHalt;
        return;
    }
}

template <typename Memory>
std::optional<uint16_t> Dsp<Memory>::RomWaitMail()
{
    // The wait at 0x8078 polls CMBH until the mailbox is full. It returns with CMBH, full flag included, in $ac0.m and
    // with LZ set.
    if (!(mailbox_to_dsp_ & kMailboxFull))
    {
        return std::nullopt;
    }

    const uint16_t high = ReadHardware(kCmbh);
    SetAcM(0, high);
    regs_.sr |= kSrLogicZero;

    return high;
}

template <typename Memory>
void Dsp<Memory>::RomTakeCommand()
{
    const std::optional<uint16_t> high = RomWaitMail();
    if (!high)
    {
        return;
    }

    if (*high != kCommandMark)
    {
        // Not a command. The ROM reads it and replies with CMBH.
        SetAcM(1, ReadHardware(kCmbl));
        WriteHardware(kDmbh, kNotCommandReply);
        WriteHardware(kDmbl, *high);
        rom_state_ = Rom::kWaitMailTaken;
        return;
    }

    // The ROM compares the command with each of these in turn. The last comparison leaves the flags.
    const uint16_t command = ReadHardware(kCmbl);
    SetAcM(0, command);
    constexpr std::array<uint16_t, 7> kCommands = {
        kIramAddress, kIramLength, kIramDestination, kDramAddress, kDramLength, kDramDestination, kStart};
    for (const uint16_t known : kCommands)
    {
        SetAcM(1, known);
        const int64_t difference = Wrap40(regs_.ac[0] - regs_.ac[1]);
        SetFlagsSub(regs_.ac[0], regs_.ac[1], difference);
        if (command == known)
        {
            rom_command_ = command;
            rom_state_ = Rom::kWaitArgument;
            return;
        }
    }

    // An unknown command. The ROM replies with the command.
    WriteHardware(kDmbh, kUnknownCommandReply);
    WriteHardware(kDmbl, command);
    rom_state_ = Rom::kWaitMailTaken;
}

template <typename Memory>
void Dsp<Memory>::RomTakeArgument()
{
    const std::optional<uint16_t> high = RomWaitMail();
    if (!high)
    {
        return;
    }

    const uint16_t low = ReadHardware(kCmbl);
    if (rom_command_ == kStart)
    {
        // The entry point goes to $ac0.m and $ar0. Then the loader starts the microcode.
        SetAc(0, 0);
        SetAcM(0, low);
        regs_.ar[0] = low;
        regs_.pc = kLoader;
        rom_state_ = Rom::kNone;
        return;
    }

    // Each of the other commands reads CMBL into $ac1.m and moves it.
    SetAcM(1, low);
    switch (rom_command_)
    {
    case kIramAddress:
        regs_.ix[0] = *high;
        regs_.ix[1] = low;
        break;

    case kIramLength:
        regs_.ix[3] = low;
        break;

    case kIramDestination:
        regs_.ix[2] = low;
        break;

    case kDramAddress:
        regs_.ax_h[0] = *high;
        regs_.ax_l[0] = low;
        break;

    case kDramLength:
        regs_.ax_l[1] = low;
        break;

    case kDramDestination:
        regs_.ax_h[1] = low;
        break;
    }

    rom_state_ = Rom::kCommandStart;
}

template <typename Memory>
void Dsp<Memory>::RomEntry()
{
    // Set $cr, clear $sr's bits 8 to 12 (IE and EIE among them) and set the modes. Then send the ready mail and enter
    // the command loop.
    regs_.cr = 0xff;
    regs_.sr &= static_cast<uint16_t>(~((1u << 8) | kSrInterruptEnable | (1u << 10) | kSrExternalEnable | (1u << 12)));
    regs_.sr &= ~kSrSxm;
    regs_.sr &= ~kSrSu;
    regs_.sr |= kSrAm;
    WriteHardware(kDmbh, static_cast<uint16_t>(kReadyMail >> 16));
    WriteHardware(kDmbl, static_cast<uint16_t>(kReadyMail));
    regs_.pc = kCommandLoop;
    rom_state_ = Rom::kCommandStart;
}

template <typename Memory>
void Dsp<Memory>::RomLoader()
{
    // 0x80B5: loads DRAM, then IRAM, by DMA when their lengths aren't 0. Then jumps to $ar0. The routine takes 11
    // steps, plus 9 for each load.
    regs_.sr &= ~kSrSxm;
    SetAc(0, 0);
    SetAc(1, 0);
    SetFlags40(0, false, false);
    uint32_t steps = 11;
    if (regs_.ax_l[1] != 0)
    {
        WriteHardware(kDsmah, regs_.ax_h[0]);
        WriteHardware(kDsmal, regs_.ax_l[0]);
        WriteHardware(kDscr, 0);
        WriteHardware(kDspa, regs_.ax_h[1]);
        WriteHardware(kDsbl, regs_.ax_l[1]);
        SetAcM(0, ReadHardware(kDscr));
        regs_.sr |= kSrLogicZero;
        steps += 9;
    }

    SetAc(1, 0);
    SetAcM(1, regs_.ix[3]);
    SetFlags40(regs_.ac[1], false, false);
    if (regs_.ix[3] != 0)
    {
        WriteHardware(kDsmah, regs_.ix[0]);
        WriteHardware(kDsmal, regs_.ix[1]);
        WriteHardware(kDscr, 2);
        SetAcM(0, 2);
        WriteHardware(kDspa, regs_.ix[2]);
        WriteHardware(kDsbl, regs_.ix[3]);
        SetAcM(0, ReadHardware(kDscr));
        regs_.sr |= kSrLogicZero;
        steps += 9;
    }

    // This step is the first of the routine's steps.
    rom_delay_ = steps - 1;
    rom_state_ = Rom::kLoading;
}

template <typename Memory>
void Dsp<Memory>::RomMix(uint16_t routine)
{
    // Mixes 32 samples of one or two inputs into 24-bit accumulation buffers at a constant or ramped volume.
    Registers& r = regs_;
    const bool sxm = (r.sr & kSrSxm) != 0;

    // The address `count` words on. The routines step their addresses by the DSP's increment. AX keeps every $wr at
    // 0xFFFF, and with that wrap each step is a plain +1. The model ignores $wr.
    const auto advance = [](uint16_t address, int count)
    {
        return static_cast<uint16_t>(address + count);
    };

    // mul(x, v): the MULX product of $ax0.h = x (signed) and $ax1.l = v (unsigned when SU is set). The product is
    // doubled when AM is clear.
    const auto mul = [&r](uint16_t x, uint16_t v)
    {
        const int64_t a = static_cast<int16_t>(x);
        const int64_t b = (r.sr & kSrSu) ? int64_t{v} : int64_t{static_cast<int16_t>(v)};

        return (r.sr & kSrAm) ? a * b : a * b * 2;
    };

    // The middle word of a product, saturated in 40-bit mode. The routines store it.
    const auto sat = [sxm](int64_t product)
    {
        return static_cast<uint16_t>(sxm ? MiddleWord(product) : product >> 16);
    };

    const auto ramp = [sxm](uint16_t volume, uint16_t delta, int j) -> uint16_t
    {
        const int32_t value = static_cast<int16_t>(volume) + j * static_cast<int16_t>(delta);
        if (sxm && (value < -32768 || value > 32767))
        {
            return value > 0 ? 0x7fff : 0x8000;
        }

        return static_cast<uint16_t>(value);
    };

    // One mix: x (33 samples) with volumes v into the accumulation buffer at `accumulation`. The results are written as
    // pairs at `output`. Returns the last sample's result.
    const auto mix = [&](uint16_t input, uint16_t accumulation, uint16_t output,
                         const std::array<uint16_t, 32>& volumes, std::array<uint16_t, 33>& x)
    {
        std::array<int64_t, 32> results{};
        for (int i = 0; i < 33; i++)
        {
            x[i] = ReadDmem(advance(input, i));
        }

        for (int i = 0; i < 32; i++)
        {
            const uint16_t hi = ReadDmem(advance(accumulation, i * 2));
            const uint16_t lo = ReadDmem(advance(accumulation, i * 2 + 1));
            int32_t a = static_cast<int32_t>(uint32_t{hi & 0xffu} << 16 | lo);
            if (a & 0x800000)
            {
                a -= 0x1000000;
            }

            results[i] = Wrap40(int64_t{a} * 0x10000 + mul(x[i], volumes[i])) >> 16;
        }

        for (int i = 0; i < 32; i++)
        {
            WriteDmem(advance(output, i * 2), static_cast<uint16_t>(results[i] >> 16));
            WriteDmem(advance(output, i * 2 + 1), static_cast<uint16_t>(results[i]));
        }

        return results[31];
    };

    uint32_t steps = 0;
    std::array<uint16_t, 33> x{};
    std::array<uint16_t, 33> y{};
    std::array<uint16_t, 32> volumes{};
    switch (routine)
    {
    case kMixOneConstant:
        {
            const uint16_t v = ReadDmem(r.ar[1]);
            volumes.fill(v);
            const int64_t last = mix(r.ar[0], r.ar[2], r.ar[3], volumes, x);
            r.ar[0] = advance(r.ar[0], 33);
            r.ar[1] = advance(r.ar[1], 2);
            r.ar[2] = advance(r.ar[2], 64);
            r.ar[3] = advance(r.ar[3], 64);
            r.ax_h[0] = x[32];
            r.ax_l[0] = sat(mul(x[31], v));
            r.ax_l[1] = v;
            r.ax_h[1] = r.ar[3];
            SetAc(1, last);
            SetProduct(mul(x[32], v));
            steps = 137;
            break;
        }

    case kMixTwoConstant:
        {
            const uint16_t v1 = ReadDmem(r.ar[1]);
            const uint16_t v2 = ReadDmem(advance(r.ar[1], 2));
            volumes.fill(v1);
            mix(r.ar[0], r.ar[2], r.ar[3], volumes, x);
            volumes.fill(v2);
            const int64_t last = mix(r.ix[0], r.ix[1], r.ix[1], volumes, y);
            r.ar[0] = advance(r.ix[0], 33);
            r.ar[1] = advance(r.ar[1], 4);
            r.ar[2] = advance(r.ix[1], 64);
            r.ar[3] = r.ar[2];
            r.ax_h[0] = y[32];
            r.ax_l[0] = sat(mul(x[31], v1));
            r.ax_l[1] = v2;
            r.ax_h[1] = sat(mul(y[31], v2));
            SetAc(1, last);
            SetProduct(mul(y[32], v2));
            steps = 274;
            break;
        }

    case kMixOneRamped:
        {
            // The ramp goes to the scratch buffer at $ar3. The new volume goes back to the volume block.
            const uint16_t scratch = r.ar[3];
            const uint16_t v = ReadDmem(r.ar[1]);
            const uint16_t d = ReadDmem(advance(r.ar[1], 1));
            for (int j = 0; j < 32; j++)
            {
                volumes[j] = ramp(v, d, j);
                WriteDmem(advance(scratch, j), volumes[j]);
            }

            WriteDmem(r.ar[1], ramp(v, d, 32));
            const int64_t last = mix(r.ar[0], r.ar[2], r.ar[2], volumes, x);
            r.ix[3] = scratch;
            r.ar[0] = advance(r.ar[0], 33);
            r.ar[1] = advance(scratch, 32);
            r.ar[2] = advance(r.ar[2], 64);
            r.ar[3] = r.ar[2];
            r.ax_h[0] = x[32];
            r.ax_l[0] = sat(mul(x[31], volumes[31]));
            r.ax_l[1] = volumes[31];
            SetAc(1, last);
            SetProduct(mul(x[32], volumes[31]));
            steps = 238;
            break;
        }

    case kMixTwoRamped:
        {
            const uint16_t scratch = r.ar[3];
            const uint16_t v1 = ReadDmem(r.ar[1]);
            const uint16_t d1 = ReadDmem(advance(r.ar[1], 1));
            const uint16_t v2 = ReadDmem(advance(r.ar[1], 2));
            const uint16_t d2 = ReadDmem(advance(r.ar[1], 3));
            std::array<uint16_t, 32> volumes2{};
            for (int j = 0; j < 32; j++)
            {
                volumes[j] = ramp(v1, d1, j);
                WriteDmem(advance(scratch, j), volumes[j]);
            }

            WriteDmem(r.ar[1], ramp(v1, d1, 32));
            for (int j = 0; j < 32; j++)
            {
                volumes2[j] = ramp(v2, d2, j);
                WriteDmem(advance(scratch, 32 + j), volumes2[j]);
            }

            WriteDmem(advance(r.ar[1], 2), ramp(v2, d2, 32));
            mix(r.ar[0], r.ar[2], r.ar[2], volumes, x);
            const int64_t last = mix(r.ix[0], r.ix[1], r.ix[1], volumes2, y);
            r.ix[3] = scratch;
            r.ar[0] = advance(r.ix[0], 33);
            r.ar[1] = advance(scratch, 64);
            r.ar[2] = advance(r.ix[1], 64);
            r.ar[3] = r.ar[2];
            r.ax_h[0] = y[32];
            r.ax_l[0] = sat(mul(x[31], volumes[31]));
            r.ax_l[1] = volumes2[31];
            r.ax_h[1] = sat(mul(y[31], volumes2[31]));
            SetAc(1, last);
            SetProduct(mul(y[32], volumes2[31]));
            steps = 475;
            break;
        }
    }

    // Every routine ends with a CLR $ac0.
    SetAc(0, 0);
    SetFlags40(0, false, false);

    // This step is the first of the routine's steps.
    rom_delay_ = steps - 1;
    rom_state_ = Rom::kMixing;
}

} // namespace ntdlib::dsp
