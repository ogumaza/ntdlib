// SPDX-License-Identifier: MIT

// Tests of DSi mode (ntdlib/ntr/machine.h, ntdlib/ntr/arm7.h), on cartridge images that run only on a DSi, with small
// programs for both CPUs. The expected values follow GBATEK: the launcher's state ("DSi BIOS RAM", "DSi Control
// Registers (SCFG)", "DSi New Shared WRAM", "DSi Cartridge Header"), the NDMA ("DSi New DMA (NDMA)"), the I2C bus and
// the BPTWL chip ("DSi I2C Bus", "DSi I2C Device 4Ah") and the BIOS ("BIOS SHA1 Functions", "GetPitchTable"). The SHA-1
// digests are FIPS 180's test vectors. The digests of SHA1_Init_Update_Finish_Mess follow GBATEK's pseudo code on a
// SHA-1 that gives FIPS 180's vectors.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"
#include "ntdlib/arm/cpu.h"
#include "ntdlib/ntr/arm7.h"
#include "ntdlib/ntr/driver_player.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/tables.h"
#include "ntdlib/ntr/sound.h"
#include "ntr_dsi_test_util.h"

namespace
{

using namespace ntr_dsi_tests;
using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Condition;
using ntdlib::ntr::Arm7System;
using ntdlib::ntr::DriverPlayer;
using ntdlib::ntr::Machine;
using ntdlib::ntr::Rom;
using ntdlib::ntr::SoundOutput;

constexpr uint32_t kIf = 0x04000214;
constexpr uint32_t kScfgRom = 0x04004000;
constexpr uint32_t kScfgClock = 0x04004004;
constexpr uint32_t kScfgExtended = 0x04004008;
constexpr uint32_t kScfgCard = 0x04004010;
constexpr uint32_t kMbk1 = 0x04004040;
constexpr uint32_t kMbk2 = 0x04004044;
constexpr uint32_t kMbk6 = 0x04004054;
constexpr uint32_t kMbk7 = 0x04004058;
constexpr uint32_t kMbk9 = 0x04004060;
constexpr uint32_t kNdma0 = 0x04004104;
constexpr uint32_t kI2cData = 0x04004500;
constexpr uint32_t kI2cControl = 0x04004501;

// The setting of SCFG_EXT7 that the launcher leaves for the game is 0x93FFFB06, with bits 0, 1, 2, 10, 18 and 31 from
// the header (kHeaderExtended).
constexpr uint32_t kGameArm7Extended = 0x13FFFF06;

Bytes HaltingImage(uint8_t unit_code = 3)
{
    return DsiImage(Words(kHalts), Words(kHalts), Pattern(0x40, 0x90), Pattern(0x30, 0x70), unit_code);
}

Bytes MemoryBytes(Machine& machine, uint32_t address, std::size_t size)
{
    const std::span<uint8_t> bytes = MemoryAt(machine.MainMemory(), address, size);
    return {bytes.begin(), bytes.end()};
}

void Put(Machine& machine, uint32_t address, std::span<const uint8_t> bytes)
{
    std::ranges::copy(bytes, MemoryAt(machine.MainMemory(), address, bytes.size()).begin());
}

// A CPU's access to `bus`: in the page's host memory where the bus gives one, and through Read and Write elsewhere.
uint32_t Load(ntdlib::arm::Bus& bus, uint32_t address, int size)
{
    const uint8_t* page = bus.Page(address & ~0xFFFu);
    if (page == nullptr)
    {
        return bus.Read(address, size);
    }

    uint32_t value = 0;
    for (int i = 0; i < size; i++)
    {
        value |= uint32_t{page[(address & 0xFFF) + static_cast<uint32_t>(i)]} << (8 * i);
    }

    return value;
}

void Store(ntdlib::arm::Bus& bus, uint32_t address, int size, uint32_t value)
{
    uint8_t* page = bus.Page(address & ~0xFFFu);
    if (page == nullptr)
    {
        bus.Write(address, size, value);
        return;
    }

    for (int i = 0; i < size; i++)
    {
        page[(address & 0xFFF) + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
    }
}

Bytes Hex(const std::string& text)
{
    Bytes bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2)
    {
        bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }

    return bytes;
}

Bytes Text(const std::string& text)
{
    return {text.begin(), text.end()};
}

void TestModeChoice()
{
    // Only the unit code of a DSi-only game (3) gives DSi mode. A DSi-enhanced game (2) runs as on a DS.
    for (const uint8_t unit_code : {uint8_t{0}, uint8_t{2}, uint8_t{3}})
    {
        const Bytes image = HaltingImage(unit_code);
        const Rom rom = Rom::Parse(image);
        Machine machine(rom);
        const bool dsi = unit_code == 3;
        XSFLIB_CHECK(machine.DsiMode() == dsi);
        XSFLIB_CHECK(machine.Arm7().DsiMode() == dsi);
        XSFLIB_CHECK(machine.MainMemory().size() == (dsi ? 0x1000000u : 0x400000u));
        XSFLIB_CHECK(machine.Arm7().MainMemory().size() == machine.MainMemory().size());
    }
}

void TestBootState()
{
    const Bytes arm9i = Pattern(0x40, 0x90);
    const Bytes arm7i = Pattern(0x30, 0x70);
    const Bytes image = DsiImage(Words(kHalts), Words(kHalts), arm9i, arm7i);
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    Arm7System& arm7 = machine.Arm7();

    // The header's 0x1000 bytes twice, and its first 0x160 twice.
    const Bytes header(image.begin(), image.begin() + 0x1000);
    XSFLIB_CHECK(MemoryBytes(machine, 0x02FFC000, 0x1000) == header);
    XSFLIB_CHECK(MemoryBytes(machine, 0x02FFE000, 0x1000) == header);
    const Bytes short_header(image.begin(), image.begin() + 0x160);
    XSFLIB_CHECK(MemoryBytes(machine, 0x02FFFA80, 0x160) == short_header);
    XSFLIB_CHECK(MemoryBytes(machine, 0x02FFFE00, 0x160) == short_header);

    // The firmware's area at the end of 16 MiB: the boot indicator and the user's settings.
    XSFLIB_CHECK(Word(machine, 0x02FFFC40) == 1);
    const Bytes user = MemoryBytes(machine, 0x02FFFC80, 0x70);
    XSFLIB_CHECK(user[0] == 5); // the settings' version

    // The game's SCFG_EXT7 setting, and the system settings that the pointer at 0x02FFFDFC gives.
    XSFLIB_CHECK(Word(machine, 0x02FFFDF0) == kGameArm7Extended);
    XSFLIB_CHECK(Load(arm7, 0x0380FFC4, 4) == kGameArm7Extended);
    XSFLIB_CHECK(Word(machine, 0x02FFFDFC) == 0x02000400);
    XSFLIB_CHECK(Word(machine, 0x02000400) == 0x0000000F);
    XSFLIB_CHECK(MemoryBytes(machine, 0x02000406, 1)[0] == (user[0x64] & 7));
    XSFLIB_CHECK(MemoryBytes(machine, 0x02000430, 12) == Bytes(user.begin() + 0x58, user.begin() + 0x64));
    XSFLIB_CHECK(MemoryBytes(machine, 0x02000444, 1)[0] == user[0x02]);
    XSFLIB_CHECK(MemoryBytes(machine, 0x02000446, 2) == Bytes(user.begin() + 0x03, user.begin() + 0x05));
    XSFLIB_CHECK(MemoryBytes(machine, 0x02000448, 20) == Bytes(user.begin() + 0x06, user.begin() + 0x1A));
    XSFLIB_CHECK(MemoryBytes(machine, 0x0200045E, 52) == Bytes(user.begin() + 0x1C, user.begin() + 0x50));

    // The DSi binaries.
    XSFLIB_CHECK(MemoryBytes(machine, kArm9iAddress, arm9i.size()) == arm9i);
    XSFLIB_CHECK(MemoryBytes(machine, kArm7iAddress, arm7i.size()) == arm7i);

    // The registers as the launcher leaves them.
    XSFLIB_CHECK(Load(machine, kScfgRom, 2) == 0x0001);
    XSFLIB_CHECK(Load(machine, kScfgClock, 2) == 0x0084);
    XSFLIB_CHECK(Load(machine, kScfgExtended, 4) == 0x8307F100);
    XSFLIB_CHECK(Load(machine, kScfgCard, 2) == 0x0008);
    XSFLIB_CHECK(Load(arm7, kScfgRom, 2) == 0x0501);
    XSFLIB_CHECK(Load(arm7, kScfgExtended, 4) == 0x93FFFB06);
    XSFLIB_CHECK(Load(arm7, kScfgCard, 2) == 0x0008);
    for (std::size_t i = 0; i < 5; i++)
    {
        const auto address = static_cast<uint32_t>(kMbk1 + 4 * i);
        XSFLIB_CHECK(Load(machine, address, 4) == kWramSettings[i]);
        XSFLIB_CHECK(Load(arm7, address, 4) == kWramSettings[i]);
    }
    for (std::size_t i = 0; i < 3; i++)
    {
        const auto address = static_cast<uint32_t>(kMbk6 + 4 * i);
        XSFLIB_CHECK(Load(machine, address, 4) == kWramSettings[5 + i]);
        XSFLIB_CHECK(Load(arm7, address, 4) == kWramSettings[8 + i]);
    }
    XSFLIB_CHECK((Load(arm7, kMbk9, 4) & 0xFFFFFF) == 0x00000F);

    // An ARM9 binary that covers 0x02000400 replaces the system settings there.
    const Bytes arm9 = Pattern(0x800, 0x11);
    const Bytes covering_image = DsiImage(arm9, Words(kHalts), arm9i, arm7i);
    const Rom covering = Rom::Parse(covering_image);
    Machine covered(covering);
    XSFLIB_CHECK(MemoryBytes(covered, 0x02000400, 0x100) == Bytes(arm9.begin() + 0x400, arm9.begin() + 0x500));
    XSFLIB_CHECK(Word(covered, 0x02FFFDFC) == 0x02000400);
}

// Calls `routine`, assembled at 0x02100000, with `args`. Returns r0.
uint32_t CallRoutine(Machine& machine, const ArmAssembler& routine, std::span<const uint32_t> args)
{
    ArmAssembler copy = routine;
    Put(machine, 0x02100000, copy.Finish());
    std::vector<SoundOutput> out;
    return machine.Call(0x02100000, args, machine.Clock() + 4'000'000, out);
}

// A routine that makes BIOS call `number` and returns.
ArmAssembler BiosCall(uint32_t number)
{
    ArmAssembler a(0x02100000);
    a.Swi(number);
    a.Bx(ntdlib::arm::kLr);

    return a;
}

void TestScfg()
{
    const Bytes image = HaltingImage();
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    Arm7System& arm7 = machine.Arm7();

    // The ARM9's SCFG_A9ROM and SCFG_MC are read-only. The ARM7's SCFG_ROM bits can only be set.
    Store(machine, kScfgRom, 2, 0x0000);
    Store(machine, kScfgCard, 2, 0x0000);
    XSFLIB_CHECK(Load(machine, kScfgRom, 2) == 0x0001);
    XSFLIB_CHECK(Load(machine, kScfgCard, 2) == 0x0008);
    Store(arm7, kScfgRom, 2, 0x0000);
    XSFLIB_CHECK(Load(arm7, kScfgRom, 2) == 0x0501);
    Store(arm7, kScfgRom, 2, 0x0002);
    XSFLIB_CHECK(Load(arm7, kScfgRom, 2) == 0x0503);

    // WaitByLoop passes 4 of the ARM9's cycles a loop. At the DS clock, an ARM9 cycle is half a system cycle.
    // SCFG_CLK's bit 0 makes it a quarter.
    constexpr uint32_t kLoops = 0x10000;
    const std::array<uint32_t, 1> loops = {kLoops};
    uint64_t start = machine.Clock();
    CallRoutine(machine, BiosCall(0x03), loops);
    const uint64_t slow = machine.Clock() - start;
    Store(machine, kScfgClock, 2, 0x0085);
    XSFLIB_CHECK(Load(machine, kScfgClock, 2) == 0x0085);
    start = machine.Clock();
    CallRoutine(machine, BiosCall(0x03), loops);
    const uint64_t fast = machine.Clock() - start;
    XSFLIB_CHECK(slow >= kLoops * 4 / 2 && slow < kLoops * 4 / 2 + 64);
    XSFLIB_CHECK(fast >= kLoops * 4 / 4 && fast < kLoops * 4 / 4 + 64);

    // With SCFG_EXT9's bit 31 clear, the ARM9's SCFG and MBK registers read as 0 and ignore writes. The ARM7's stay
    // open, the new shared WRAM stays mapped, and the NDMA's registers stay open.
    Store(machine, 0x03740000, 4, 0x12345678);
    Store(machine, kScfgExtended, 4, 0x0307F100);
    XSFLIB_CHECK(Load(machine, kScfgExtended, 4) == 0);
    XSFLIB_CHECK(Load(machine, kScfgClock, 2) == 0);
    XSFLIB_CHECK(Load(machine, kMbk1, 4) == 0);
    Store(machine, kScfgExtended, 4, 0x8307F100);
    XSFLIB_CHECK(Load(machine, kScfgExtended, 4) == 0);
    XSFLIB_CHECK(Load(arm7, kMbk1, 4) == kWramSettings[0]);
    XSFLIB_CHECK(Load(machine, 0x03740000, 4) == 0x12345678);
    Store(machine, kNdma0, 4, 0x02200000);
    XSFLIB_CHECK(Load(machine, kNdma0, 4) == 0x02200000);

    // The ARM7's SCFG_EXT7 gates the ARM7's registers alike.
    Store(arm7, kScfgExtended, 4, 0x13FFFB06);
    XSFLIB_CHECK(Load(arm7, kScfgExtended, 4) == 0);
    XSFLIB_CHECK(Load(arm7, kMbk1, 4) == 0);
}

void TestNewWram()
{
    const Bytes image = HaltingImage();
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    Arm7System& arm7 = machine.Arm7();

    // WRAM-A at the ARM7's 0x037C0000, WRAM-B and WRAM-C at the ARM9's 0x03740000 and 0x03700000.
    Store(arm7, 0x037C0000, 4, 0x11223344);
    Store(arm7, 0x037FFFFC, 4, 0x99AABBCC);
    Store(machine, 0x03740000, 4, 0x55667788);
    Store(machine, 0x03700000, 4, 0x01020304);
    XSFLIB_CHECK(Load(arm7, 0x037C0000, 4) == 0x11223344);
    XSFLIB_CHECK(Load(arm7, 0x037FFFFC, 4) == 0x99AABBCC);
    XSFLIB_CHECK(Load(machine, 0x03740000, 4) == 0x55667788);
    XSFLIB_CHECK(Load(machine, 0x03700000, 4) == 0x01020304);

    // WRAM-B's slots repeat through the window of 256 KiB: slot 0 again at 0x03780000.
    XSFLIB_CHECK(Load(machine, 0x03780000, 4) == 0x55667788);

    // No window of the ARM9 covers 0x037C0000. A write there leaves WRAM-A alone.
    Store(machine, 0x037C0000, 4, 0xAABBCCDD);
    XSFLIB_CHECK(Load(arm7, 0x037C0000, 4) == 0x11223344);

    // MBK2 gives WRAM-B's slot 0 to the ARM7, whose window covers the same addresses. The ARM9 then has no bank there.
    // The ARM7 can't write MBK2: MBK9 leaves WRAM-B's slots to the ARM9.
    XSFLIB_CHECK(Load(arm7, 0x03740000, 4) == 0);
    Store(machine, kMbk2, 1, 0x81);
    XSFLIB_CHECK(Load(machine, kMbk2, 4) == 0x8C888481);
    XSFLIB_CHECK(Load(arm7, 0x03740000, 4) == 0x55667788);
    XSFLIB_CHECK(Load(machine, 0x03740000, 4) == 0);
    Store(arm7, kMbk2, 1, 0x80);
    XSFLIB_CHECK(Load(arm7, kMbk2, 4) == 0x8C888481);
    XSFLIB_CHECK(Load(arm7, 0x03740000, 4) == 0x55667788);

    // MBK9 makes MBK1 read-only. Once the ARM7 clears MBK9, the ARM9 can write MBK1. Disabling WRAM-A's slot 0 then
    // unmaps it.
    Store(machine, kMbk1, 1, 0x01);
    Store(arm7, kMbk1, 1, 0x01);
    XSFLIB_CHECK(Load(arm7, kMbk1, 4) == 0x8D898581);
    Store(machine, kMbk9, 4, 0);
    XSFLIB_CHECK((Load(arm7, kMbk9, 4) & 0xFFFFFF) == 0x00000F);
    Store(arm7, kMbk9, 4, 0);
    Store(machine, kMbk1, 1, 0x01);
    XSFLIB_CHECK(Load(arm7, kMbk1, 4) == 0x8D898501);
    XSFLIB_CHECK(Load(arm7, 0x037C0000, 4) == 0);
    Store(machine, kMbk1, 1, 0x81);
    XSFLIB_CHECK(Load(arm7, 0x037C0000, 4) == 0x11223344);

    // MBK7 moves the ARM9's window of WRAM-B to 0x03000000.
    Store(machine, kMbk2, 1, 0x80);
    Store(machine, kMbk7, 4, 0x00403000);
    XSFLIB_CHECK(Load(machine, 0x03000000, 4) == 0x55667788);
}

void TestNdma()
{
    const Bytes image = HaltingImage();
    const Rom rom = Rom::Parse(image);

    // An immediate transfer of a block of 4 words with the IRQ, and a fill of 3 words.
    {
        Machine machine(rom);
        const Bytes data = Pattern(0x20, 0x31);
        Put(machine, 0x02200000, data);
        Store(machine, kNdma0 + 0x00, 4, 0x02200000);
        Store(machine, kNdma0 + 0x04, 4, 0x02210000);
        Store(machine, kNdma0 + 0x0C, 4, 4);
        XSFLIB_CHECK((Load(machine, kIf, 4) & (1u << 28)) == 0);
        Store(machine, kNdma0 + 0x18, 4, 0xD0000000);
        XSFLIB_CHECK(MemoryBytes(machine, 0x02210000, 0x10) == Bytes(data.begin(), data.begin() + 0x10));
        XSFLIB_CHECK(Word(machine, 0x02210010) == 0);
        XSFLIB_CHECK((Load(machine, kNdma0 + 0x18, 4) & 0x80000000) == 0);
        XSFLIB_CHECK((Load(machine, kIf, 4) & (1u << 28)) != 0);

        // Channel 1's fill, from its fill data, with the destination decrementing.
        const uint32_t channel1 = kNdma0 + 0x1C;
        Store(machine, channel1 + 0x04, 4, 0x02220008);
        Store(machine, channel1 + 0x0C, 4, 3);
        Store(machine, channel1 + 0x14, 4, 0xCAFEBABE);
        Store(machine, channel1 + 0x18, 4, 0x90006400);
        XSFLIB_CHECK(Word(machine, 0x02220000) == 0xCAFEBABE);
        XSFLIB_CHECK(Word(machine, 0x02220004) == 0xCAFEBABE);
        XSFLIB_CHECK(Word(machine, 0x02220008) == 0xCAFEBABE);
        XSFLIB_CHECK(Word(machine, 0x0222000C) == 0);
        XSFLIB_CHECK((Load(machine, kIf, 4) & (1u << 29)) == 0);
    }

    // The ARM9 reads 0x200 bytes of the card through channel 0 in the card's mode, a word a block, and waits for the
    // channel to end. Channel 0 then requests its IRQ. The channel starts before the read, or once the card's first
    // word is ready.
    for (const bool late : {false, true})
    {
        ArmAssembler a(kMainMemory);
        a.LoadConstant(4, kIo);
        a.Mov(0, 0x80);
        a.Strb(0, 4, 0x1A1); // AUXSPICNT: the slot on, in ROM mode
        a.LoadConstant(7, kNdma0);
        a.LoadConstant(0, 0x04100010);
        a.Str(0, 7, 0x00); // the source: the card's data
        a.LoadConstant(0, 0x02200000);
        a.Str(0, 7, 0x04);
        a.Mov(0, 0x80);
        a.Str(0, 7, 0x08); // the total: 0x80 words
        a.Mov(0, 1);
        a.Str(0, 7, 0x0C);             // a word a block
        a.LoadConstant(5, 0xC4004000); // on, with the IRQ, in the card's mode, from a fixed source
        if (!late)
        {
            a.Str(5, 7, 0x18);
        }
        a.LoadConstant(0, 0x800000B7);
        a.Str(0, 4, 0x1A8); // the command: B7, then the address 0x8000
        a.Mov(0, 0);
        a.Str(0, 4, 0x1AC);
        a.LoadConstant(0, 0xA1000000);
        a.Str(0, 4, 0x1A4); // ROMCTRL: a block of 0x200 bytes
        if (late)
        {
            const int ready = a.NewLabel();
            a.Bind(ready);
            a.Ldr(0, 4, 0x1A4);
            a.Tst(0, 0x800000);
            a.B(ready, Condition::kEq);
            a.Str(5, 7, 0x18);
        }
        const int wait = a.NewLabel();
        a.Bind(wait);
        a.Ldr(0, 7, 0x18);
        a.Tst(0, 0x80000000);
        a.B(wait, Condition::kNe);
        const int stop = a.NewLabel();
        a.Bind(stop);
        a.B(stop);
        const uint32_t stop_address = a.Address(stop);

        const Bytes arm9i = Pattern(0x200, 0x55);
        const Bytes card_image = DsiImage(a.Finish(), Words(kHalts), arm9i, Pattern(0x10, 0));
        const Rom card_rom = Rom::Parse(card_image);
        Machine machine(card_rom);
        machine.Arm9().SetStopAddress(stop_address);
        std::vector<SoundOutput> out;
        XSFLIB_CHECK(machine.Run(0x80000, out));
        XSFLIB_CHECK(MemoryBytes(machine, 0x02200000, 0x200) == arm9i);
        XSFLIB_CHECK(Word(machine, 0x02200200) == 0);
        XSFLIB_CHECK(machine.Counts().card_reads == 1);
        XSFLIB_CHECK((Load(machine, kIf, 4) & (1u << 28)) != 0);
    }

    // A start of a channel in a mode without a model, and a start of the ARM7's NDMA, stop the machine.
    {
        Machine machine(rom);
        Store(machine, kNdma0 + 0x18, 4, 0x81000000);
        std::vector<SoundOutput> out;
        std::string error;
        try
        {
            machine.Run(machine.Clock() + 1000, out);
        }
        catch (const std::runtime_error& e)
        {
            error = e.what();
        }
        XSFLIB_CHECK(error.find("NDMA channel 0 starts in mode 0x01") != std::string::npos);
    }
    {
        Machine machine(rom);
        Store(machine.Arm7(), kNdma0 + 0x18, 4, 0x90000000);
        std::vector<SoundOutput> out;
        std::string error;
        try
        {
            machine.Run(machine.Clock() + 1000, out);
        }
        catch (const std::runtime_error& e)
        {
            error = e.what();
        }
        XSFLIB_CHECK(error.find("the ARM7 starts an NDMA channel") != std::string::npos);
    }
}

void TestI2c()
{
    const Bytes image = HaltingImage();
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);
    Arm7System& arm7 = machine.Arm7();

    // Sends a byte, or reads one when `control` has bit 5, and gives I2C_CNT after the transfer.
    const auto transfer = [&arm7](uint8_t data, uint8_t control)
    {
        Store(arm7, kI2cData, 1, data);
        Store(arm7, kI2cControl, 1, control);
        return Load(arm7, kI2cControl, 1);
    };
    // Reads the BPTWL's register `index`.
    const auto read = [&](uint8_t index)
    {
        XSFLIB_CHECK(transfer(0x4A, 0x82) == 0x12);
        XSFLIB_CHECK(transfer(index, 0x80) == 0x10);
        XSFLIB_CHECK(transfer(0x4B, 0x82) == 0x12);
        transfer(0, 0xA1);
        return Load(arm7, kI2cData, 1);
    };

    XSFLIB_CHECK(read(0x00) == 0x33); // the version
    XSFLIB_CHECK(read(0x20) == 0x0F); // the battery: full, not charging
    XSFLIB_CHECK(read(0x40) == 0x1F); // the volume
    XSFLIB_CHECK(read(0x41) == 0x04); // the backlight
    XSFLIB_CHECK(read(0x10) == 0x00); // no button down
    XSFLIB_CHECK(read(0x70) == 0x00); // the warm boot's flag

    // Reads go on through the registers.
    read(0x40);
    transfer(0, 0xA1);
    XSFLIB_CHECK(Load(arm7, kI2cData, 1) == 0x04);

    // A write to a register, then a read of it.
    XSFLIB_CHECK(transfer(0x4A, 0x82) == 0x12);
    XSFLIB_CHECK(transfer(0x40, 0x80) == 0x10);
    XSFLIB_CHECK(transfer(0x08, 0x81) == 0x11);
    XSFLIB_CHECK(read(0x40) == 0x08);

    // No other device answers.
    XSFLIB_CHECK(transfer(0xA0, 0x82) == 0x02);
    XSFLIB_CHECK(transfer(0xA1, 0x82) == 0x02);
    transfer(0, 0xA1);
    XSFLIB_CHECK(Load(arm7, kI2cData, 1) == 0xFF);
}

constexpr uint32_t kContext = 0x02180000;
constexpr uint32_t kDigest = 0x02190000;
constexpr uint32_t kSource = 0x02200000;

// SHA1_Init, then SHA1_Update on `message` in pieces of the sizes `pieces` (the rest in the last), then SHA1_Finish.
Bytes Sha1InPieces(Machine& machine, const Bytes& message, const std::vector<uint32_t>& pieces)
{
    Put(machine, kSource, message);
    xsflib::StoreLe32(MemoryAt(machine.MainMemory(), kContext + 0x60, 4).data(), 0);
    const std::array<uint32_t, 1> init = {kContext};
    CallRoutine(machine, BiosCall(0x24), init);

    uint32_t at = 0;
    for (std::size_t i = 0; i <= pieces.size(); i++)
    {
        const auto left = static_cast<uint32_t>(message.size()) - at;
        const uint32_t size = i < pieces.size() ? std::min(pieces[i], left) : left;
        const std::array<uint32_t, 3> update = {kContext, kSource + at, size};
        CallRoutine(machine, BiosCall(0x25), update);
        at += size;
    }

    const std::array<uint32_t, 2> finish = {kDigest, kContext};
    CallRoutine(machine, BiosCall(0x26), finish);

    return MemoryBytes(machine, kDigest, 20);
}

void TestSha1()
{
    const Bytes image = HaltingImage();
    const Rom rom = Rom::Parse(image);
    Machine machine(rom);

    const Bytes abc = Text("abc");
    const Bytes abc_digest = Hex("a9993e364706816aba3e25717850c26c9cd0d89d");
    XSFLIB_CHECK(Sha1InPieces(machine, abc, {}) == abc_digest);

    // The context after Finish: the length in bits as before the padding, no incomplete block, and a callback.
    XSFLIB_CHECK(Word(machine, kContext + 0x14) == 24);
    XSFLIB_CHECK(Word(machine, kContext + 0x18) == 0);
    XSFLIB_CHECK(Word(machine, kContext + 0x5C) == 0);
    XSFLIB_CHECK(Word(machine, kContext + 0x60) != 0);

    // Init's state.
    const std::array<uint32_t, 1> init = {kContext};
    CallRoutine(machine, BiosCall(0x24), init);
    XSFLIB_CHECK(Word(machine, kContext + 0x00) == 0x67452301);
    XSFLIB_CHECK(Word(machine, kContext + 0x04) == 0xEFCDAB89);
    XSFLIB_CHECK(Word(machine, kContext + 0x08) == 0x98BADCFE);
    XSFLIB_CHECK(Word(machine, kContext + 0x0C) == 0x10325476);
    XSFLIB_CHECK(Word(machine, kContext + 0x10) == 0xC3D2E1F0);
    XSFLIB_CHECK(Word(machine, kContext + 0x14) == 0);
    XSFLIB_CHECK(Word(machine, kContext + 0x5C) == 0);

    const Bytes two_blocks = Text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    const Bytes two_blocks_digest = Hex("84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    XSFLIB_CHECK(Sha1InPieces(machine, two_blocks, {}) == two_blocks_digest);
    XSFLIB_CHECK(Sha1InPieces(machine, two_blocks, {1, 2, 3, 50}) == two_blocks_digest);

    const Bytes million(1'000'000, 'a');
    const Bytes million_digest = Hex("34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    XSFLIB_CHECK(Sha1InPieces(machine, million, {}) == million_digest);
    XSFLIB_CHECK(Sha1InPieces(machine, million, {1, 63, 64, 65, 127, 4096, 3}) == million_digest);

    // SHA1_Init_Update_Finish.
    Put(machine, kSource, abc);
    const std::array<uint32_t, 3> whole = {kDigest + 0x40, kSource, 3};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x27), whole) == 1);
    XSFLIB_CHECK(MemoryBytes(machine, kDigest + 0x40, 20) == abc_digest);

    // SHA1_Compare_20_Bytes: 1 for a match, 0 for a mismatch or a null address.
    const std::array<uint32_t, 2> same = {kDigest, kDigest + 0x40};
    Put(machine, kDigest, abc_digest);
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x28), same) == 1);
    Put(machine, kDigest + 19, Bytes{0});
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x28), same) == 0);
    const std::array<uint32_t, 2> null = {0, kDigest + 0x40};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x28), null) == 0);
    const std::array<uint32_t, 2> both_null = {0, 0};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x28), both_null) == 0);

    // SHA1_Init_Update_Finish_Mess on "abc", 25 bytes: the second digest and 5 bytes of the next. The digest of
    // "carry 310" ends in 0x00, and its increment carries into the byte before.
    Put(machine, kDigest, Bytes(0x40, 0xEE));
    const std::array<uint32_t, 4> mess = {kDigest, 25, kSource, 3};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x29), mess) == 1);
    XSFLIB_CHECK(MemoryBytes(machine, kDigest, 26) == Hex("61fe4c10c48a8312881b3a8b88ed811060e27e978d331ae802ee"));
    const Bytes carry = Text("carry 310");
    Put(machine, kSource, carry);
    const std::array<uint32_t, 4> carry_mess = {kDigest, 20, kSource, static_cast<uint32_t>(carry.size())};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x29), carry_mess) == 1);
    XSFLIB_CHECK(MemoryBytes(machine, kDigest, 20) == Hex("de86dc00bb44cdcde998869139a87fb476d7689c"));
    const std::array<uint32_t, 4> no_destination = {0, 20, kSource, 3};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x29), no_destination) == 1);
    const std::array<uint32_t, 4> no_source = {kDigest, 20, 0, 3};
    XSFLIB_CHECK(CallRoutine(machine, BiosCall(0x29), no_source) == 0);

    // A context with a callback in the game's code stops the machine.
    Machine other(rom);
    xsflib::StoreLe32(MemoryAt(other.MainMemory(), kContext + 0x60, 4).data(), 0x02001000);
    CallRoutine(other, BiosCall(0x24), init);
    XSFLIB_CHECK(Word(other, kContext + 0x60) == 0x02001000);
    std::string error;
    try
    {
        const std::array<uint32_t, 3> update = {kContext, kSource, 3};
        CallRoutine(other, BiosCall(0x25), update);
    }
    catch (const std::runtime_error& e)
    {
        error = e.what();
    }
    XSFLIB_CHECK(error.find("callback 0x02001000") != std::string::npos);

    // A DS has no SHA-1 calls.
    const Bytes ds_image = HaltingImage(0);
    const Rom ds_rom = Rom::Parse(ds_image);
    Machine ds(ds_rom);
    error.clear();
    try
    {
        CallRoutine(ds, BiosCall(0x27), whole);
    }
    catch (const std::runtime_error& e)
    {
        error = e.what();
    }
    XSFLIB_CHECK(!error.empty());
}

void TestArm7Bios()
{
    // The ARM7 asks GetPitchTable for entry 0x100 as the SDK does on a DSi, with the index less 0x46A, and takes the
    // digest of "abc" with SHA1_Init_Update_Finish. Then it halts.
    ArmAssembler a(0x02380000);
    a.LoadConstant(0, 0x100 - 0x46A);
    a.Swi(0x1B);
    a.LoadConstant(4, 0x02300000);
    a.Str(0, 4, 0);
    a.Add(0, 4, 0x10);
    const int abc = a.NewLabel();
    a.LoadAddress(1, abc);
    a.Mov(2, 3);
    a.Swi(0x27);
    a.Str(0, 4, 4);
    const int halt = a.NewLabel();
    a.Bind(halt);
    a.Swi(0x06);
    a.B(halt);
    a.Pool();
    a.Bind(abc);
    a.Word(0x00636261);

    const Bytes image = DsiImage(Words(kHalts), a.Finish(), Pattern(0x10, 0), Pattern(0x10, 0));
    const Rom rom = Rom::Parse(image);
    Arm7System arm7(rom);
    std::vector<SoundOutput> out;
    arm7.Run(100'000, out);
    const std::span<uint8_t> memory = arm7.MainMemory();
    XSFLIB_CHECK(xsflib::LoadLe32(MemoryAt(memory, 0x02300000, 4).data()) == ntdlib::ntr::snd::tables::kPitch[0x100]);
    XSFLIB_CHECK(xsflib::LoadLe32(MemoryAt(memory, 0x02300004, 4).data()) == 1);
    const std::span<uint8_t> digest = MemoryAt(memory, 0x02300010, 20);
    XSFLIB_CHECK(Bytes(digest.begin(), digest.end()) == Hex("a9993e364706816aba3e25717850c26c9cd0d89d"));
}

void TestDriverPlayerRefuses()
{
    // The driver player refuses a DSi-only image: its ARM7 binary doesn't start on Arm7System alone.
    const Bytes dsi = HaltingImage();
    const Rom dsi_rom = Rom::Parse(dsi);
    std::string error;

    try
    {
        DriverPlayer player(dsi_rom);
    }
    catch (const std::runtime_error& e)
    {
        error = e.what();
    }

    XSFLIB_CHECK(error == "the driver player can't run a game that runs only on a DSi");
}

} // namespace

int main()
{
    TestModeChoice();
    TestBootState();
    TestScfg();
    TestNewWram();
    TestNdma();
    TestI2c();
    TestSha1();
    TestArm7Bios();
    TestDriverPlayerRefuses();

    return xsflib::testing::Summary("DSi mode");
}