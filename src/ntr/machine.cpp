// SPDX-License-Identifier: MIT

// The DS machine: the boot, the runs and calls, the memory map, the BIOS calls, the events and the display.
// machine_units.cpp contains the DMAs, the FIFO and the maths unit, machine_card.cpp the card and the SPI bus, and
// machine_io.cpp the registers.

#include "ntdlib/ntr/machine.h"

#include "machine_internal.h"

namespace ntdlib::ntr
{

Machine::Machine(const Rom& rom, const BackupMemory& backup)
    : image_(rom.Image()),
      past_image_(rom.Partial() ? 0 : 0xFF),
      arm7_(std::make_unique<Arm7System>(rom, static_cast<Arm9Link&>(*this))),
      arm9_(std::make_unique<arm::Cpu>(*this, arm::Architecture::kArmV5TE)),
      bios_(kBiosSize),
      palette_(kSmallMemory),
      oam_(kSmallMemory)
{
    if (backup.address_bytes != 0)
    {
        const std::size_t largest = backup.address_bytes == 1 ? 512 : backup.address_bytes == 2 ? 0x10000 : 0x1000000;
        if (backup.address_bytes < 1 || backup.address_bytes > 3 || !std::has_single_bit(backup.size) ||
            backup.size > largest || (backup.address_bytes == 1 && backup.size != largest))
        {
            throw std::invalid_argument("a backup memory with " + std::to_string(backup.address_bytes) +
                                        " address bytes can't hold " + std::to_string(backup.size) + " bytes");
        }
        backup_ = std::make_unique<SpiMemory>(backup.address_bytes, std::vector<uint8_t>(backup.size, backup.blank));
    }

    card_.infrared = backup.infrared;

    for (std::size_t i = 0; i < vram_.size(); i++)
    {
        vram_[i].assign(kVramSizes[i], 0);
    }

    for (std::size_t i = 0; i < kBios.size(); i++)
    {
        xsflib::StoreLe32(&bios_[4 * i], kBios[i]);
    }

    if (arm7_->DsiMode())
    {
        ndma_ = std::make_unique<Ndma>(
            *this, [this](uint32_t irq) { Request(irq); }, [this](std::string message) { Fail(std::move(message)); });
        ndma_->on_card_start = [this]
        {
            RunCardDmas();
        };
        dsi_io_ = std::make_unique<DsiIo>(false, kDsiArm9Extended, arm7_->DsiWram(), ndma_.get());
        dsi_io_->on_unknown = [this](uint32_t address, int size, bool write)
        {
            Unknown(address, size, write);
        };
        dsi_io_->on_fail = [this](std::string message)
        {
            Fail(std::move(message));
        };
        arm7_->DsiWram().on_map_change = [this]
        {
            arm7_->Cpu().InvalidatePages();
            arm9_->InvalidatePages();
        };
    }

    Boot(rom);
}

Machine::~Machine() = default;

std::span<uint8_t> Machine::Backup()
{
    return backup_ == nullptr ? std::span<uint8_t>() : backup_->Data();
}

arm::Cpu& Machine::Arm9()
{
    return *arm9_;
}

Arm7System& Machine::Arm7()
{
    return *arm7_;
}

std::span<uint8_t> Machine::MainMemory()
{
    return arm7_->MainMemory();
}

const MachineCounts& Machine::Counts() const
{
    return counts_;
}

uint64_t Machine::Clock() const
{
    return arm7_->Clock();
}

bool Machine::DsiMode() const
{
    return dsi_io_ != nullptr;
}

uint32_t Machine::FromEnd(uint32_t from_end)
{
    return kMainMemory + static_cast<uint32_t>(MainMemory().size()) - from_end;
}

void Machine::Boot(const Rom& rom)
{
    // The launcher's state comes first. A binary that covers part of it then replaces that part.
    if (dsi_io_ != nullptr)
    {
        BootDsi(rom);
    }
    LoadArm9(rom);

    const std::span<uint8_t> main = MainMemory();
    const auto put = [&main](uint32_t address, int size, uint32_t value)
    {
        for (int i = 0; i < size; i++)
        {
            main[(address - kMainMemory + static_cast<uint32_t>(i)) & (main.size() - 1)] =
                static_cast<uint8_t>(value >> (8 * i));
        }
    };

    chip_id_ = ChipId(image_.size());

    const auto header16 = [this](std::size_t at)
    {
        return at + 2 <= image_.size() ? static_cast<uint32_t>(xsflib::LoadLe16(&image_[at])) : 0u;
    };

    for (const uint32_t area : {FromEnd(kBootArea), FromEnd(kBootAreaCopy)})
    {
        put(area, 4, chip_id_);
        put(area + 4, 4, chip_id_);
        put(area + 8, 2, header16(0x15E));
        put(area + 0xA, 2, header16(0x6C));
    }

    put(FromEnd(kBootArea) + 0x50, 2, kArm7BiosCrc);
    put(FromEnd(kBootAreaCopy) + 0x10, 2, kArm7BiosCrc);
    put(FromEnd(kBootIndicator), 2, 1);

    const std::array<uint8_t, 0x70> settings = FirmwareUserSettings();
    for (std::size_t i = 0; i < settings.size(); i++)
    {
        put(FromEnd(kUserSettings) + static_cast<uint32_t>(i), 1, settings[i]);
    }

    io_[kPostFlag] = 1;
    arm9_->SetSwiHandler([this](arm::Cpu& cpu, uint32_t comment) { return Swi(cpu, comment); });
    arm9_->Jump(rom.Header().arm9.entry, arm::Mode::kSupervisor);
    ScheduleDisplay();
}

void Machine::BootDsi(const Rom& rom)
{
    const std::span<uint8_t> main = MainMemory();
    const auto put_bytes = [&main](uint32_t address, std::span<const uint8_t> bytes)
    {
        std::copy(bytes.begin(), bytes.end(), main.begin() + (address - kMainMemory));
    };

    // The headers.
    const std::span<const uint8_t> header = image_.first(std::min<std::size_t>(image_.size(), kDsiHeaderSize));
    put_bytes(FromEnd(kDsiCardHeader), header);
    put_bytes(FromEnd(kDsiHeader), header);
    put_bytes(FromEnd(kDsiShortHeader), header.first(std::min<std::size_t>(header.size(), kDsiShortHeaderSize)));

    // The game's SCFG_EXT7 setting, and the system settings (GBATEK's "DSi SD/MMC Firmware System Settings Data
    // Files"): the flags that the launcher sets, the language, and the touch screen's calibration, the favourite
    // colour, the birthday, the nickname and the message from the firmware's user settings.
    const uint32_t extended = (kDsiArm7Extended & ~kDsiExtendedFromHeader) |
                              (xsflib::LoadLe32(&image_[kHeaderExtended]) & kDsiExtendedFromHeader);
    xsflib::StoreLe32(&main[FromEnd(kDsiExtendedCopy) - kMainMemory], extended);
    xsflib::StoreLe32(&main[FromEnd(kDsiSettingsPointer) - kMainMemory], kDsiSettings);
    const std::array<uint8_t, 0x70> user = FirmwareUserSettings();
    uint8_t* settings = &main[kDsiSettings - kMainMemory];
    xsflib::StoreLe32(settings, 0x0000000F);
    settings[0x06] = user[0x64] & 7;
    std::copy(user.begin() + 0x58, user.begin() + 0x64, settings + 0x30);
    settings[0x44] = user[0x02];
    std::copy(user.begin() + 0x03, user.begin() + 0x05, settings + 0x46);
    std::copy(user.begin() + 0x06, user.begin() + 0x1A, settings + 0x48);
    std::copy(user.begin() + 0x1C, user.begin() + 0x50, settings + 0x5E);

    // The ARM9i and ARM7i binaries, through each CPU's bus.
    const auto load = [](arm::Bus& bus, const std::optional<Binary>& binary, std::span<const uint8_t> bytes)
    {
        for (uint32_t i = 0; binary && i < bytes.size(); i++)
        {
            StoreToBus(bus, binary->ram_address + i, 1, bytes[i]);
        }
    };
    load(*this, rom.Header().arm9i, rom.Arm9i());
    load(*arm7_, rom.Header().arm7i, rom.Arm7i());
}

void Machine::LoadArm9(const Rom& rom)
{
    const Binary& arm9 = rom.Header().arm9;
    const std::span<const uint8_t> binary = rom.Arm9();
    if (arm9.ram_address < kMainMemory ||
        arm9.ram_address - kMainMemory + uint64_t{binary.size()} > MainMemory().size())
    {
        throw std::runtime_error("the ARM9 binary doesn't fit in main memory at " +
                                 xsflib::Hex(arm9.ram_address, 8, true));
    }

    const std::span<uint8_t> main = MainMemory();
    uint8_t* at = &main[arm9.ram_address - kMainMemory];
    std::memcpy(at, binary.data(), binary.size());

    // The firmware decrypts the secure area and marks its start.
    if (arm9.rom_offset == kSecureArea && binary.size() >= kSecureAreaId.size() &&
        std::equal(kSecureAreaId.begin(), kSecureAreaId.end(), at))
    {
        xsflib::StoreLe32(at, kSecureAreaMark);
        xsflib::StoreLe32(at + 4, kSecureAreaMark);
    }
}

bool Machine::Run(uint64_t until, std::vector<SoundOutput>& out)
{
    bool stopped = false;
    while (Clock() < until && error_.empty())
    {
        if (arm9_->Halted() && !irq_line_)
        {
            // The ARM9 waits for an interrupt: the system runs to the ARM9's next event, or until the ARM7 wakes it.
            arm7_->Run(std::min(until, NextEvent()), out);
            Events();
            continue;
        }

        if (dsi_io_ != nullptr && NextEvent() <= Clock())
        {
            Events();
            continue;
        }

        polls_ = 0;
        arm9_running_ = true;
        const uint64_t burst = BurstCycles();
        burst_end_ = Clock() + burst / Arm9Cycles();
        const uint64_t start = arm9_->Cycles();
        const arm::StopReason reason = arm9_->Run(burst);
        arm9_running_ = false;
        if (reason == arm::StopReason::kStopAddress)
        {
            stopped = true;
            break;
        }

        // A burst that ran for a slice or polled passes its time, until the ARM7 makes a change that the ARM9 can
        // see. The ARM9 can't see one in WaitByLoop.
        uint64_t cycles = 0;
        if (reason == arm::StopReason::kCycles)
        {
            counts_.slices++;
            cycles = arm9_->Cycles() - start;
        }
        if (polled_)
        {
            polled_ = false;
            counts_.polls++;
            cycles = arm9_->Cycles() - start;
        }

        if (reason == arm::StopReason::kHalted && irq_line_)
        {
            // The IRQ line ends the halt at once while the CPSR masks the IRQ: the ARM9 spins, and its loops take the
            // time of their instructions.
            spin_cycles_ += arm9_->Cycles() - start;
            if (spin_cycles_ >= kSpinCycles)
            {
                cycles = spin_cycles_;
                spin_cycles_ = 0;
            }
        }

        const uint64_t ratio = Arm9Cycles();
        if (cycles > 0 && error_.empty())
        {
            Pass(Clock() + (cycles + ratio - 1) / ratio, true, out);
        }
        if (wait_cycles_ > 0 && error_.empty())
        {
            Pass(Clock() + (wait_cycles_ + ratio - 1) / ratio, false, out);
        }
        wait_cycles_ = 0;
    }

    if (!error_.empty())
    {
        throw std::runtime_error(error_);
    }

    return stopped;
}

uint32_t Machine::Call(uint32_t address, std::span<const uint32_t> args, uint64_t until, std::vector<SoundOutput>& out)
{
    arm::Cpu& cpu = *arm9_;
    const uint32_t saved_pc = cpu.Pc();
    const uint32_t saved_sp = cpu.Reg(13);
    const uint32_t saved_stop = cpu.StopAddress();
    const bool saved_halted = cpu.Halted();

    // r0 to r3, then the rest on the stack from SP upward, with SP kept 8-byte aligned.
    for (std::size_t i = 0; i < std::min<std::size_t>(args.size(), 4); i++)
    {
        cpu.SetReg(static_cast<int>(i), args[i]);
    }
    if (args.size() > 4)
    {
        const auto stacked = static_cast<uint32_t>(args.size() - 4);
        const uint32_t sp = (saved_sp - stacked * 4) & ~7u;
        for (uint32_t i = 0; i < stacked; i++)
        {
            cpu.Store(sp + i * 4, 4, args[4 + i]);
        }
        cpu.SetReg(13, sp);
    }

    cpu.SetReg(14, arm::Cpu::kCallReturn);
    cpu.SetStopAddress(arm::Cpu::kCallReturn);
    cpu.Jump(address, static_cast<arm::Mode>(cpu.Cpsr() & arm::psr::kModeMask));

    const bool returned = Run(until, out);
    const uint32_t result = cpu.Reg(0);

    // Go on from where the CPU was before the call.
    cpu.SetStopAddress(saved_stop);
    cpu.SetReg(13, saved_sp);
    cpu.Jump(saved_pc, static_cast<arm::Mode>(cpu.Cpsr() & arm::psr::kModeMask));
    if (saved_halted)
    {
        cpu.Halt();
    }

    if (!returned)
    {
        throw std::runtime_error("the call to " + xsflib::Hex(address, 8, true) + " didn't return by the clock " +
                                 std::to_string(until));
    }

    return result;
}

uint8_t* Machine::Page(uint32_t page)
{
    switch (page >> 24)
    {
    case 0x02:
        return MainMemory().data() + (page & (MainMemory().size() - 1));
    case 0x03:
        {
            if (dsi_io_ != nullptr)
            {
                if (const std::optional<uint8_t*> mapped = arm7_->DsiWram().Page(false, page))
                {
                    return *mapped;
                }
            }

            // WRAMCNT gives the ARM9 all of the shared work memory (0), its second half (1), its first half (2) or
            // none (3).
            const std::span<uint8_t> shared = arm7_->SharedWram();
            const auto half = static_cast<uint32_t>(shared.size() / 2);
            switch (wram_control_)
            {
            case 0:
                return shared.data() + (page & (shared.size() - 1));
            case 1:
                return shared.data() + half + (page & (half - 1));
            case 2:
                return shared.data() + (page & (half - 1));
            default:
                return nullptr;
            }
        }
    case 0x05:
        return palette_.data();
    case 0x06:
        {
            const uint32_t area = (page >> 21) & 7;
            if (area < 4)
            {
                return vram_[area].data() + (page & (vram_[area].size() - 1));
            }

            const uint32_t offset = page & 0x1FFFFF;
            return area == 4 && offset < vram_[4].size() ? vram_[4].data() + offset : nullptr;
        }
    case 0x07:
        return oam_.data();
    case 0x0C:
        // DSi mode mirrors main memory here (GBATEK's "DSi Memory Map").
        return dsi_io_ != nullptr ? MainMemory().data() + (page & (MainMemory().size() - 1)) : nullptr;
    case 0xFF:
        return page >= kBiosBase && page < kBiosBase + kBiosSize ? bios_.data() + (page - kBiosBase) : nullptr;
    default:
        return nullptr;
    }
}

uint32_t Machine::Read(uint32_t address, int size)
{
    switch (address >> 24)
    {
    case 0x04:
        return ReadIo(address, size);
    case 0x08:
    case 0x09:
    case 0x0A:
        // No GBA cartridge: the slot's lines read as high.
        return size == 4 ? 0xFFFFFFFF : size == 2 ? 0xFFFF : 0xFF;
    default:
        Unknown(address, size, false);
        return 0;
    }
}

void Machine::Write(uint32_t address, int size, uint32_t value)
{
    switch (address >> 24)
    {
    case 0x04:
        WriteIo(address, size, value);
        return;
    case 0x08:
    case 0x09:
    case 0x0A:
        return;
    default:
        Unknown(address, size, true);
        return;
    }
}

uint16_t Machine::SyncOutput() const
{
    return static_cast<uint16_t>((ipcsync_ & kSyncOutput) >> 8);
}

void Machine::OnSyncWrite(bool irq)
{
    arm7_changed_ = true;

    if (irq && (ipcsync_ & kSyncIrqEnable) != 0)
    {
        Request(irq::kIpcSync);
    }
}

void Machine::OnFifoToArm9(uint32_t word)
{
    arm7_changed_ = true;

    if (on_fifo_word)
    {
        on_fifo_word(false, word);
    }

    if (arm7_->WordsToArm9() == 1 && (fifo_control_ & kReceiveIrq) != 0)
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

void Machine::OnFifoToArm7Empty()
{
    arm7_changed_ = true;

    if ((fifo_control_ & kSendEmptyIrq) != 0)
    {
        Request(irq::kIpcSendEmpty);
    }
}

void Machine::Poll()
{
    if (arm9_running_ && ++polls_ == kPollReads)
    {
        polled_ = true;
        arm9_->Stop();
    }
}

bool Machine::Swi(arm::Cpu& cpu, uint32_t comment)
{
    if (on_arm9_swi && on_arm9_swi(cpu, comment))
    {
        return true;
    }

    const bool thumb = (cpu.Cpsr() & arm::psr::kT) != 0;
    if (!thumb && comment >> 16 == kFaultSwi && (comment & 0xFFFF) < kFaults.size())
    {
        // The vector's mode holds the address after the instruction that faulted in LR.
        Fail(std::string("the ARM9 took ") + kFaults[comment & 0xFFFF] + " before " +
             xsflib::Hex(cpu.Reg(14), 8, true));
        return true;
    }

    // In ARM state, the BIOS takes the number from bits 16 to 23 of the comment.
    const uint32_t number = thumb ? comment : (comment >> 16) & 0xFF;
    const auto load = [&cpu](uint32_t address, int size)
    {
        return cpu.Load(address, size);
    };
    const auto store = [&cpu](uint32_t address, int size, uint32_t value)
    {
        cpu.Store(address, size, value);
    };
    const auto fail = [this](std::string message)
    {
        Fail(std::move(message));
    };
    if (const std::optional<bool> done = bios::SharedCall(cpu, number, dsi_io_ != nullptr, load, store, fail))
    {
        return *done;
    }

    switch (number)
    {
    case bios::kWaitByLoop:
        {
            const auto loops = static_cast<int32_t>(cpu.Reg(0));
            if (loops > 0)
            {
                wait_cycles_ += static_cast<uint64_t>(loops) * kWaitLoopCycles;
                cpu.Stop();
            }

            return true;
        }
    case bios::kHalt:
        cpu.Halt();
        return true;
    case kBitUnPack:
        {
            // r2 points at the source's length in bytes (16 bits), the source's and the destination's widths in bits,
            // and an offset added to each unit, to zeros too when bit 31 is set.
            const uint32_t info = cpu.Reg(2);
            const uint32_t length = cpu.Load(info, 2);
            const uint32_t source_width = cpu.Load(info + 2, 1);
            const uint32_t destination_width = cpu.Load(info + 3, 1);
            const uint32_t offset_word = cpu.Load(info + 4, 4);
            if (source_width == 0 || source_width > 8 || destination_width == 0 || destination_width > 32)
            {
                Fail("BitUnPack with widths of " + std::to_string(source_width) + " and " +
                     std::to_string(destination_width) + " bits");
                return true;
            }

            uint32_t source = cpu.Reg(0);
            uint32_t destination = cpu.Reg(1);
            uint32_t word = 0;
            uint32_t filled = 0;
            for (uint32_t i = 0; i < length; i++)
            {
                const uint32_t byte = cpu.Load(source + i, 1);
                for (uint32_t bit = 0; bit < 8; bit += source_width)
                {
                    uint32_t unit = (byte >> bit) & ((1u << source_width) - 1);
                    if (unit != 0 || (offset_word & 0x80000000) != 0)
                    {
                        unit += offset_word & 0x7FFFFFFF;
                    }

                    word |= (destination_width == 32 ? unit : unit & ((1u << destination_width) - 1)) << filled;
                    filled += destination_width;
                    if (filled >= 32)
                    {
                        cpu.Store(destination, 4, word);
                        destination += 4;
                        word = 0;
                        filled = 0;
                    }
                }
            }

            return true;
        }
    case kLz77Uncompress:
    case kRlUncompress:
        {
            uint32_t source = cpu.Reg(0);
            uint32_t destination = cpu.Reg(1);
            const uint32_t size = cpu.Load(source, 4) >> 8;
            source += 4;

            const uint32_t end = destination + size;
            while (destination < end)
            {
                const uint32_t flags = cpu.Load(source++, 1);
                if (number == kRlUncompress)
                {
                    // Bit 7 set: the next byte, (flags & 0x7F) + 3 times. Clear: flags + 1 bytes as they are.
                    const bool run = (flags & 0x80) != 0;
                    const uint32_t count = run ? (flags & 0x7F) + 3 : flags + 1;
                    const uint32_t byte = run ? cpu.Load(source++, 1) : 0;
                    for (uint32_t i = 0; i < count && destination < end; i++)
                    {
                        cpu.Store(destination++, 1, run ? byte : cpu.Load(source++, 1));
                    }
                    continue;
                }

                // Each flag bit from bit 7: clear, a byte as it is; set, 3 to 18 bytes copied from 1 to 4096 bytes
                // back.
                for (int bit = 7; bit >= 0 && destination < end; bit--)
                {
                    if ((flags & (1u << bit)) == 0)
                    {
                        cpu.Store(destination++, 1, cpu.Load(source++, 1));
                        continue;
                    }

                    const uint32_t high = cpu.Load(source++, 1);
                    const uint32_t low = cpu.Load(source++, 1);
                    const uint32_t count = (high >> 4) + 3;
                    const uint32_t distance = ((high & 0xF) << 8 | low) + 1;
                    for (uint32_t i = 0; i < count && destination < end; i++)
                    {
                        cpu.Store(destination, 1, cpu.Load(destination - distance, 1));
                        destination++;
                    }
                }
            }

            return true;
        }
    case kDiff8bitUnFilter:
    case kDiff16bitUnFilter:
        {
            const int size = number == kDiff8bitUnFilter ? 1 : 2;
            uint32_t source = cpu.Reg(0);
            uint32_t destination = cpu.Reg(1);
            const uint32_t length = cpu.Load(source, 4) >> 8;
            source += 4;

            uint32_t sum = 0;
            for (uint32_t i = 0; i < length; i += static_cast<uint32_t>(size))
            {
                sum += cpu.Load(source + i, size);
                cpu.Store(destination + i, size, sum);
            }

            return true;
        }
    default:
        Fail("ARM9 SWI " + xsflib::Hex(number, 8, true) + ", which the BIOS doesn't model, at " +
             xsflib::Hex(cpu.Pc(), 8, true));
        return true;
    }
}

void Machine::Fail(std::string message)
{
    if (error_.empty())
    {
        error_ = std::move(message);
    }

    arm9_->Stop();
}

uint64_t Machine::Arm9Cycles() const
{
    return dsi_io_ != nullptr && dsi_io_->FastArm9() ? 4 : 2;
}

uint64_t Machine::BurstCycles() const
{
    if (dsi_io_ == nullptr)
    {
        return kSlice;
    }

    // The next event is after the clock: Run handles each one when it falls due.
    return std::min(kSlice, (NextEvent() - Clock()) * Arm9Cycles());
}

void Machine::EventScheduled()
{
    if (dsi_io_ != nullptr && arm9_running_ && NextEvent() < burst_end_)
    {
        arm9_->Stop();
    }
}

void Machine::Pass(uint64_t until, bool to_arm7_change, std::vector<SoundOutput>& out)
{
    arm7_changed_ = false;
    while (Clock() < until && error_.empty() && !(to_arm7_change && arm7_changed_))
    {
        arm7_->Run(std::min(until, NextEvent()), out);
        Events();
    }
}

uint64_t Machine::NextEvent() const
{
    uint64_t next = timers_.NextOverflow(next_display_);
    if (card_.busy && !card_.ready)
    {
        next = std::min(next, card_.ready_at);
    }

    return next;
}

void Machine::Events()
{
    const uint64_t now = Clock();
    for (;;)
    {
        const uint64_t next = NextEvent();
        if (next > now)
        {
            break;
        }

        if (next_display_ == next)
        {
            DisplayEvent(next);
        }

        // Timer n's IRQ is irq::kTimer0 << n.
        if (const uint32_t timers = timers_.Overflow(next); timers != 0)
        {
            Request(irq::kTimer0 * timers);
        }

        if (card_.busy && !card_.ready && card_.ready_at == next)
        {
            CardReady();
        }
    }
}

void Machine::Request(uint32_t irq)
{
    if_ |= irq;
    UpdateIrq();
}

void Machine::UpdateIrq()
{
    // The GX FIFO's IRQ follows its condition: the FIFO is always empty.
    if ((Stored(kGxStatus + 2) & 0xC000) != 0)
    {
        if_ |= kGxFifoIrq;
    }

    irq_line_ = (ime_ & 1) != 0 && (ie_ & if_) != 0;
    arm9_->SetIrq(irq_line_);
}

uint64_t Machine::Line() const
{
    return Clock() / kLineCycles % kFrameLines;
}

void Machine::ScheduleDisplay()
{
    ScheduleDisplayAfter(Clock());
}

void Machine::ScheduleDisplayAfter(uint64_t after)
{
    // The next start of a line, or of a line's horizontal blank, after `after`. The vertical blank's start is always an
    // event: it ends a display capture and starts DMA.
    const auto next = [after](uint64_t line, uint64_t cycle)
    {
        const uint64_t at = after / kFrameCycles * kFrameCycles + line * kLineCycles + cycle;
        return at > after ? at : at + kFrameCycles;
    };

    next_display_ = next(kVblankLine, 0);
    if ((dispstat_ & kVcountIrq) != 0)
    {
        const uint64_t setting = VcountSetting(dispstat_);
        if (setting < kFrameLines)
        {
            next_display_ = std::min(next_display_, next(setting, 0));
        }
    }

    const auto waits_for_hblank = [](const Dma& dma)
    {
        return (dma.control & kDmaEnable) != 0 && DmaTimingOf(dma.control) == kDmaHblank;
    };
    const bool hblank_dma = std::any_of(dmas_.begin(), dmas_.end(), waits_for_hblank);
    if ((dispstat_ & kHblankIrq) != 0 || hblank_dma)
    {
        const uint64_t at = after / kLineCycles * kLineCycles + kHblankCycle;
        next_display_ = std::min(next_display_, at > after ? at : at + kLineCycles);
    }
}

void Machine::DisplayEvent(uint64_t at)
{
    const uint64_t line = at / kLineCycles % kFrameLines;
    const uint64_t cycle = at % kLineCycles;
    if (cycle == 0 && line == kVblankLine)
    {
        if ((dispstat_ & kVblankIrq) != 0)
        {
            Request(irq::kVblank);
        }

        io_[kCaptureControl + 3] &= static_cast<uint8_t>(~kCaptureBusy);

        for (int channel = 0; channel < 4; channel++)
        {
            const uint32_t control = dmas_[static_cast<std::size_t>(channel)].control;
            if ((control & kDmaEnable) != 0 && DmaTimingOf(control) == kDmaVblank)
            {
                RunDma(channel);
            }
        }
    }

    if (cycle == 0 && (dispstat_ & kVcountIrq) != 0 && line == VcountSetting(dispstat_))
    {
        Request(irq::kVcount);
    }

    if (cycle == kHblankCycle)
    {
        if ((dispstat_ & kHblankIrq) != 0)
        {
            Request(irq::kHblank);
        }

        // DMA starts on the horizontal blanks of the lines that the screens show.
        for (int channel = 0; channel < 4 && line < kVblankLine; channel++)
        {
            const uint32_t control = dmas_[static_cast<std::size_t>(channel)].control;
            if ((control & kDmaEnable) != 0 && DmaTimingOf(control) == kDmaHblank)
            {
                RunDma(channel);
            }
        }
    }

    ScheduleDisplayAfter(at);
}

} // namespace ntdlib::ntr
