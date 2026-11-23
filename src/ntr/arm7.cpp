// SPDX-License-Identifier: MIT

// The ARM7's side of a DS (see arm7.h): the system's set-up, its run loop, the IPC, the memory map and the answers
// that the system's ARM9 gives at start-up.

#include "arm7_internal.h"

namespace ntdlib::ntr
{

Arm7System::Arm7System(const Rom& rom) : Arm7System(rom, nullptr)
{
}

Arm7System::Arm7System(const Rom& rom, Arm9Link& link) : Arm7System(rom, &link)
{
}

Arm7System::Arm7System(const Rom& rom, Arm9Link* link)
    : bios_(kBiosSize),
      main_(RunsInDsiMode(rom.Header()) ? kDsiMainMemorySize : kMainMemorySize),
      main_mask_(static_cast<uint32_t>(main_.size() - 1)),
      shared_wram_(kSharedWramSize),
      wram_(kWramSize),
      vram_(kVramSize),
      cpu_(std::make_unique<arm::Cpu>(*this, arm::Architecture::kArmV4T)),
      sound_(main_),
      link_(link),
      spi_(link == nullptr ? nullptr : std::make_unique<SpiDevices>())
{
    for (std::size_t i = 0; i < kBios.size(); i++)
    {
        for (int byte = 0; byte < 4; byte++)
        {
            bios_[4 * i + static_cast<std::size_t>(byte)] = static_cast<uint8_t>(kBios[i] >> (8 * byte));
        }
    }

    const std::span<const uint8_t> image = rom.Image();
    if (RunsInDsiMode(rom.Header()))
    {
        // The launcher maps the new shared WRAM as the header asks, and leaves SCFG_EXT7 open to the game. The game's
        // setting, from the header's bits 0, 1, 2, 10, 18 and 31, waits at 0x0380FFC4 (GBATEK's "DSi BIOS RAM").
        dsi_wram_ = std::make_unique<NewWram>();
        dsi_wram_->on_map_change = [this]
        {
            cpu_->InvalidatePages();
        };
        dsi_wram_->SetFromHeader(&image[kHeaderWramSettings]);
        dsi_io_ = std::make_unique<DsiIo>(true, kDsiArm7Extended, *dsi_wram_, nullptr);
        dsi_io_->on_unknown = [this](uint32_t address, int size, bool write)
        {
            Unknown(address, size, write);
        };
        dsi_io_->on_fail = [this](std::string message)
        {
            Fail(std::move(message));
        };
    }

    const Binary& arm7 = rom.Header().arm7;
    const std::span<const uint8_t> binary = rom.Arm7();
    for (uint32_t i = 0; i < binary.size(); i++)
    {
        const uint32_t address = arm7.ram_address + i;
        if (address < arm7.ram_address || (Page(address & ~0xFFFu) == nullptr && address >> 24 != 0x02))
        {
            throw std::runtime_error("the ARM7 binary doesn't fit in the memory at " +
                                     xsflib::Hex(arm7.ram_address, 8, true));
        }
        StoreToBus(*this, address, 1, binary[i]);
    }

    const uint32_t header_size = dsi_io_ != nullptr ? kDsiHeaderCopySize : kHeaderCopySize;
    for (uint32_t i = 0; i < header_size && i < image.size(); i++)
    {
        StoreToBus(*this, kMainMemory + FromEnd(kHeaderCopy) + i, 1, image[i]);
    }
    if (link_ == nullptr)
    {
        StoreToBus(*this, kMainMemory + FromEnd(kInitLockOwner), 2, kArm9InitLockId);
        StoreToBus(*this, kMainMemory + FromEnd(kArm9ReadyTags), 4, 0xFFFFFFFF);
    }
    if (dsi_io_ != nullptr)
    {
        const uint32_t setting = (kDsiArm7Extended & ~kDsiExtendedFromHeader) |
                                 (xsflib::LoadLe32(&image[kHeaderExtended]) & kDsiExtendedFromHeader);
        StoreToBus(*this, kDsiExtendedCopy, 4, setting);
    }

    io_[kPostFlag] = 1;
    io_[kPowerControl] = 1;
    sound_.Write(kSoundBias, 2, kSoundBiasMiddle);

    cpu_->SetSwiHandler([this](arm::Cpu& cpu, uint32_t comment) { return Swi(cpu, comment); });
    cpu_->SetStopAddress(kUndefinedVector);
    cpu_->SetCpsr(static_cast<uint32_t>(arm::Mode::kIrq) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kIrqStack);
    cpu_->SetCpsr(static_cast<uint32_t>(arm::Mode::kSupervisor) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kSupervisorStack);
    cpu_->SetCpsr(static_cast<uint32_t>(arm::Mode::kSystem) | arm::psr::kI | arm::psr::kF);
    cpu_->SetReg(13, kSystemStack);
    cpu_->Jump(arm7.entry, arm::Mode::kSystem);

    ScheduleDisplay();
}

Arm7System::~Arm7System() = default;

arm::Cpu& Arm7System::Cpu()
{
    return *cpu_;
}

SoundUnit& Arm7System::Sound()
{
    return sound_;
}

std::span<uint8_t> Arm7System::MainMemory()
{
    return main_;
}

uint64_t Arm7System::Clock() const
{
    return clock_;
}

bool Arm7System::Halted() const
{
    return halted_;
}

void Arm7System::Run(uint64_t until, std::vector<SoundOutput>& out)
{
    out_ = &out;
    while (clock_ < until && error_.empty())
    {
        if (halted_)
        {
            // The ARM7 leaves a halt once an enabled interrupt is requested, whatever IME holds.
            if ((ie_ & if_) != 0)
            {
                halted_ = false;
                continue;
            }

            Advance(std::min(NextEvent(), until));
            continue;
        }

        // A burst lasts from the CPU's last halt. Beside an ARM9, a burst that a link event ends goes on in the next
        // run. The event can come in the instruction that ends the burst.
        const uint64_t burst = link_ == nullptr ? kLongBurst : kLinkedBurst;
        const uint64_t start = cpu_->Cycles();
        const arm::StopReason reason = cpu_->Run(burst - burst_cycles_);
        burst_cycles_ += cpu_->Cycles() - start;
        if (reason == arm::StopReason::kStopAddress)
        {
            // The undefined mode's LR holds the address after the instruction.
            Fail("an undefined instruction before " + xsflib::Hex(cpu_->Reg(14), 8, true));
        }
        else if (reason == arm::StopReason::kHalted)
        {
            halted_ = true;
        }
        if (halted_ && (ie_ & if_) != 0)
        {
            // A halt with an enabled interrupt requested ends at once, and the burst goes on: a loop of such halts
            // takes the time of its instructions.
            halted_ = false;
        }
        if (halted_)
        {
            burst_cycles_ = 0;
        }
        else if (burst_cycles_ >= burst)
        {
            Advance(clock_ + burst_cycles_);
            burst_cycles_ = 0;
        }

        if (link_event_)
        {
            link_event_ = false;
            break;
        }
    }

    SyncSound();
    out_ = nullptr;
    if (!error_.empty())
    {
        throw std::runtime_error(error_);
    }
}

void Arm7System::SendToArm7(uint32_t word)
{
    if (to_arm7_.size() == kFifoWords)
    {
        return;
    }

    const bool was_empty = to_arm7_.empty();
    to_arm7_.push_back(word);
    if (was_empty && (fifo_control_ & kReceiveIrq) != 0)
    {
        Request(irq::kIpcReceiveNotEmpty);
    }
}

std::vector<uint32_t> Arm7System::TakeFromArm7()
{
    std::vector<uint32_t> words(to_arm9_.begin(), to_arm9_.end());
    to_arm9_.clear();
    if (!words.empty() && (fifo_control_ & kSendEmptyIrq) != 0)
    {
        Request(irq::kIpcSendEmpty);
    }

    return words;
}

uint32_t Arm7System::ReceiveFromArm7(bool take)
{
    const uint32_t word = to_arm9_.front();
    if (take)
    {
        to_arm9_.pop_front();
        if (to_arm9_.empty() && (fifo_control_ & kSendEmptyIrq) != 0)
        {
            Request(irq::kIpcSendEmpty);
        }
    }

    return word;
}

std::size_t Arm7System::WordsToArm9() const
{
    return to_arm9_.size();
}

std::size_t Arm7System::WordsToArm7() const
{
    return to_arm7_.size();
}

void Arm7System::ClearToArm7()
{
    to_arm7_.clear();
}

void Arm7System::RequestIrq(uint32_t irq)
{
    Request(irq);
}

void Arm7System::RequestSyncIrq()
{
    if ((ipcsync_ & kSyncIrqEnable) != 0)
    {
        Request(irq::kIpcSync);
    }
}

uint16_t Arm7System::SyncOutput() const
{
    return static_cast<uint16_t>((ipcsync_ & kSyncOutput) >> 8);
}

void Arm7System::SetWramControl(uint8_t value)
{
    wram_control_ = value & 3;
    cpu_->InvalidatePages();
}

std::span<uint8_t> Arm7System::SharedWram()
{
    return shared_wram_;
}

bool Arm7System::DsiMode() const
{
    return dsi_io_ != nullptr;
}

NewWram& Arm7System::DsiWram()
{
    return *dsi_wram_;
}

uint32_t Arm7System::FromEnd(uint32_t from_end) const
{
    return static_cast<uint32_t>(main_.size()) - from_end;
}

void Arm7System::LinkEvent()
{
    link_event_ = true;
    cpu_->Stop();
}

uint8_t* Arm7System::Page(uint32_t page)
{
    switch (page >> 24)
    {
    case 0x00:
        return page < kBiosSize ? bios_.data() + page : nullptr;
    case 0x02:
        if ((page & main_mask_) == FromEnd(kBootArea) && link_ == nullptr)
        {
            return nullptr;
        }
        return main_.data() + (page & main_mask_);
    case 0x03:
        if (dsi_wram_ != nullptr)
        {
            if (const std::optional<uint8_t*> mapped = dsi_wram_->Page(true, page))
            {
                return *mapped;
            }
        }
        // The ARM7 reaches the shared work memory up to 0x037FFFFF, and its work memory from 0x03800000. WRAMCNT
        // gives it all of the shared memory (3), one half (1 the first, 2 the second) or none. With none, the ARM7's
        // work memory takes the place.
        if (page < 0x03800000 && wram_control_ != 0)
        {
            const uint32_t half = kSharedWramSize / 2;
            switch (wram_control_)
            {
            case 1:
                return shared_wram_.data() + (page & (half - 1));
            case 2:
                return shared_wram_.data() + half + (page & (half - 1));
            default:
                return shared_wram_.data() + (page & (kSharedWramSize - 1));
            }
        }
        return wram_.data() + (page & (kWramSize - 1));
    case 0x06:
        return vram_.data() + (page & (kVramSize - 1));
    case 0x0C:
        // DSi mode mirrors main memory here (GBATEK's "DSi Memory Map").
        return dsi_io_ != nullptr ? main_.data() + (page & main_mask_) : nullptr;
    default:
        return nullptr;
    }
}

uint32_t Arm7System::Read(uint32_t address, int size)
{
    if (address >> 24 == 0x02)
    {
        uint32_t value = 0;
        for (int i = 0; i < size; i++)
        {
            value |= static_cast<uint32_t>(main_[(address & main_mask_) + static_cast<uint32_t>(i)]) << (8 * i);
        }
        return value;
    }
    if (dsi_io_ != nullptr && DsiIo::Contains(address))
    {
        return dsi_io_->Read(address, size);
    }
    if (address >> 24 == 0x04)
    {
        return ReadIo(address, size);
    }

    Unknown(address, size, false);

    return 0;
}

void Arm7System::Write(uint32_t address, int size, uint32_t value)
{
    if (address >> 24 == 0x02)
    {
        const uint32_t offset = address & main_mask_;
        for (int i = 0; i < size; i++)
        {
            main_[offset + static_cast<uint32_t>(i)] = static_cast<uint8_t>(value >> (8 * i));
        }

        if (link_ == nullptr)
        {
            AnswerArm9(offset, size);
        }
        return;
    }
    if (dsi_io_ != nullptr && DsiIo::Contains(address))
    {
        dsi_io_->Write(address, size, value);
        return;
    }
    if (address >> 24 == 0x04)
    {
        WriteIo(address, size, value);
        return;
    }

    Unknown(address, size, true);
}

void Arm7System::AnswerArm9(uint32_t offset, int size)
{
    const uint32_t arm7_counter = FromEnd(kArm7StartCounter);
    if (offset < arm7_counter + 2 && arm7_counter < offset + static_cast<uint32_t>(size))
    {
        const auto count = xsflib::LoadLe16(&main_[arm7_counter]);
        if (count != arm7_start_counter_)
        {
            arm7_start_counter_ = count;
            const uint32_t arm9_counter = FromEnd(kArm9StartCounter);
            const auto answer = static_cast<uint16_t>(xsflib::LoadLe16(&main_[arm9_counter]) + 1);
            xsflib::StoreLe16(&main_[arm9_counter], answer);
        }
        return;
    }

    const uint32_t bytes = FromEnd(kHandshake);
    if (size != 1 || offset < bytes || offset > bytes + 2)
    {
        return;
    }
    uint8_t* handshake = &main_[bytes];
    switch (offset - bytes)
    {
    case 0:
        // The ARM7 writes: the ARM9 copies.
        if (handshake[0] != handshake[1] && handshake[2] == 0)
        {
            handshake[1] = handshake[0];
            arm9_handshake_sum_ += handshake[1];
            if (arm9_handshake_sum_ >= kHandshakeSum)
            {
                handshake[3] = 0;
                handshake[2] = 1;
            }
        }
        break;
    case 1:
        // The ARM7 copies: the ARM9 writes the next count.
        if (handshake[0] == handshake[1] && handshake[2] == 0)
        {
            handshake[0] = static_cast<uint8_t>(0x80 | (arm9_handshake_count_++ & 0xF));
        }
        break;
    default:
        // The ARM7's sum is complete.
        if (handshake[2] != 0)
        {
            handshake[3] = 1;
        }
        break;
    }
}

} // namespace ntdlib::ntr
