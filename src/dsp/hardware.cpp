// SPDX-License-Identifier: MIT

// The DSP's memories and hardware registers: the memory maps, the mailboxes, DMA, the accelerator, and the DSP side of
// the control register.

#include <algorithm>
#include <cstdint>
#include <span>

#include "dsp/internal.h"
#include "ntdlib/dsp/coef_rom.h"
#include "ntdlib/dsp/dsp.h"
#include "ntdlib/types.h"
#include "xsflib/bytes.h"

namespace ntdlib::dsp
{
namespace
{

// The numbers of the accelerator and the other hardware registers, as the low 8 bits of their addresses. The numbers
// for DMA and the mailboxes are in internal.h.
constexpr uint8_t kFormat = 0xd1;
constexpr uint8_t kAcdraw = 0xd3;
constexpr uint8_t kAcsah = 0xd4;
constexpr uint8_t kAcsal = 0xd5;
constexpr uint8_t kAceah = 0xd6;
constexpr uint8_t kAceal = 0xd7;
constexpr uint8_t kAccah = 0xd8;
constexpr uint8_t kAccal = 0xd9;
constexpr uint8_t kPredScale = 0xda;
constexpr uint8_t kYn1 = 0xdb;
constexpr uint8_t kYn2 = 0xdc;
constexpr uint8_t kAcdsamp = 0xdd;
constexpr uint8_t kGain = 0xde;
constexpr uint8_t kAcin = 0xdf;
constexpr uint8_t kAmdm = 0xef;
constexpr uint8_t kDirq = 0xfb;

// The Wii U's registers that give the top three bits of a main-memory address: one for DMA and one for the accelerator.
constexpr uint8_t kDmaExtension = 0xba;
constexpr uint8_t kAcceleratorExtension = 0xbc;

// The init path copies its program from here.
constexpr PAddr kInitProgram = 0x01000000;
constexpr uint32_t kInitProgramSize = 0x1000;

// The init path's busy bit clears 130 time-base ticks after it starts.
constexpr uint64_t kInitBusyTicks = 130;

} // namespace

template <typename Memory>
uint16_t Dsp<Memory>::ReadDmem(uint16_t address)
{
    // The data memory map: DRAM, the coefficient ROM (twice on the GameCube and the Wii), and the hardware registers
    // through the whole 0xF000-0xFFFF range. The ROM's range starts at a multiple of its size.
    static_assert(Memory::kCoefficientRomBase % 0x800 == 0);
    if (address < Memory::kDramSize)
    {
        return dram_[address];
    }
    if (address >= Memory::kCoefficientRomBase && address < Memory::kCoefficientRomEnd)
    {
        return kCoefficientRom[address & 0x7ff];
    }
    if (address >= 0xf000)
    {
        return ReadHardware(static_cast<uint8_t>(address));
    }

    return 0;
}

template <typename Memory>
void Dsp<Memory>::WriteDmem(uint16_t address, uint16_t value)
{
    if (address < Memory::kDramSize)
    {
        dram_[address] = value;
    }
    else if (address >= 0xf000)
    {
        WriteHardware(static_cast<uint8_t>(address), value);
    }
}

template <typename Memory>
uint16_t Dsp<Memory>::ReadHardware(uint8_t reg)
{
    Accelerator& a = accelerator_;
    switch (reg)
    {
    case kFormat:
        return a.format;
    case kAcdraw:
        return AcceleratorRead(true);
    case kAcsah:
        return static_cast<uint16_t>(a.start >> 16);
    case kAcsal:
        return static_cast<uint16_t>(a.start);
    case kAceah:
        return static_cast<uint16_t>(a.end >> 16);
    case kAceal:
        return static_cast<uint16_t>(a.end);
    case kAccah:
        return static_cast<uint16_t>(a.current >> 16);
    case kAccal:
        return static_cast<uint16_t>(a.current);
    case kPredScale:
        return a.pred_scale;
    case kYn1:
        return a.yn1;
    case kYn2:
        return a.yn2;
    case kAcdsamp:
        return AcceleratorRead(false);
    case kGain:
        return a.gain;
    case kAcin:
        return a.input;
    case kDirq:
        return 0;
    case kDmbh:
        return MailboxReadHigh(mailbox_to_cpu_);
    case kDmbl:
        return MailboxReadLow(mailbox_to_cpu_);
    case kCmbh:
        return MailboxReadHigh(mailbox_to_dsp_);
    case kCmbl:
        return MailboxReadLow(mailbox_to_dsp_);
    default:
        return hardware_[reg];
    }
}

template <typename Memory>
void Dsp<Memory>::WriteHardware(uint8_t reg, uint16_t value)
{
    Accelerator& a = accelerator_;

    const auto high = [value](uint32_t old, uint32_t mask)
    {
        return ((uint32_t{value} << 16) | (old & 0xffff)) & mask;
    };

    const auto low = [value](uint32_t old, uint32_t mask)
    {
        return ((old & 0xffff0000) | value) & mask;
    };

    switch (reg)
    {
    case kDsbl:
        hardware_[kDsbl] = value;
        Dma();
        break;

    case kFormat:
        a.format = value;
        break;

    case kAcdraw:
        AcceleratorWrite(value);
        break;

    case kAcsah:
        a.start = high(a.start, 0x3fffffff);
        break;

    case kAcsal:
        a.start = low(a.start, 0x3fffffff);
        break;

    case kAceah:
        a.end = high(a.end, 0x3fffffff);
        break;

    case kAceal:
        a.end = low(a.end, 0x3fffffff);
        break;

    case kAccah:
        a.current = high(a.current, 0xbfffffff);
        break;

    case kAccal:
        a.current = low(a.current, 0xbfffffff);
        break;

    case kPredScale:
        a.pred_scale = value & 0x7f;
        break;

    case kYn1:
        a.yn1 = value;
        break;

    case kYn2:
        a.yn2 = value;
        a.reads_stopped = false;
        break;

    case kGain:
        a.gain = value;
        break;

    case kAcin:
        a.input = value;
        break;

    case kDirq:
        if (value & 1)
        {
            interrupt_request_ = true;
        }
        break;

    case kDmbh:
        MailboxWriteHigh(mailbox_to_cpu_, value);
        break;

    case kDmbl:
        MailboxWriteLow(mailbox_to_cpu_, value);
        break;

    case kCmbh:
        MailboxWriteHigh(mailbox_to_dsp_, value);
        break;

    case kCmbl:
        MailboxWriteLow(mailbox_to_dsp_, value);
        break;

    default:
        hardware_[reg] = value;
        break;
    }
}

template <typename Memory>
void Dsp<Memory>::Dma()
{
    // The whole transfer happens at once, between main memory and DRAM or IRAM. The busy bit never reads as set.
    const uint16_t control_bits = hardware_[kDscr];
    const uint32_t length = hardware_[kDsbl];
    hardware_[kDscr] = control_bits | 4;
    if (hardware_[kAmdm] == 0 && length <= 0x4000)
    {
        const uint32_t address = uint32_t{hardware_[kDsmah]} << 16 | hardware_[kDsmal];
        const uint32_t words = length / 2;
        const bool iram = (control_bits & 2) != 0;
        const bool to_main = (control_bits & 1) != 0;
        std::vector<uint16_t>& memory = iram ? iram_ : dram_;
        const uint32_t start = static_cast<uint16_t>(hardware_[kDspa] * 2) / 2u;
        uint8_t* host = nullptr;
        if (MainMemoryRange(address, words * 2, host))
        {
            if (!to_main)
            {
                for (uint32_t i = 0; i < words && start + i < memory.size(); i++)
                {
                    memory[start + i] = xsflib::LoadBe16(host + i * 2);
                }

                if (iram)
                {
                    BuildLoopMap();
                }
            }
            else if (!iram)
            {
                for (uint32_t i = 0; i < words && start + i < memory.size(); i++)
                {
                    xsflib::StoreBe16(host + i * 2, memory[start + i]);
                }
            }
        }
    }

    hardware_[kDscr] &= ~4;
    hardware_[kDsbl] = 0;
}

template <typename Memory>
bool Dsp<Memory>::MainMemoryRange(uint32_t address, uint32_t size, uint8_t*& host)
{
    // The address is masked to 30 bits. On the Wii U, a register gives its top three bits instead. The whole range must
    // lie in main memory: in MEM1, or in MEM2 on the Wii.
    if constexpr (Memory::kAddressExtensions)
    {
        host = memory_->At((uint32_t{hardware_[kDmaExtension]} & 7) << 29 | (address & 0x1fffffff),
                           std::max<uint32_t>(size, 1));
    }
    else
    {
        host = memory_->At(address & 0x3fffffff, std::max<uint32_t>(size, 1));
    }

    return host != nullptr;
}

template <typename Memory>
uint8_t Dsp<Memory>::AcceleratorByte(uint32_t address) const
{
    // The accelerator reads ARAM. On the Wii, ARAM is MEM2, and an address with bit 28 clear reads MEM1 instead. On the
    // Wii U, a register gives the address's top three bits, and the accelerator reads main memory. Bytes outside the
    // memory read as 0.
    const uint8_t* p = nullptr;
    if constexpr (Memory::kAddressExtensions)
    {
        p = memory_->At((uint32_t{hardware_[kAcceleratorExtension]} & 7) << 29 | (address & 0x1fffffff));
    }
    else
    {
        p = Memory::kAcceleratorReadsMem1 && !(address & 0x10000000) ? memory_->At(address & 0x01ffffff)
                                                                     : memory_->Aram(address & 0x03ffffff);
    }

    return p != nullptr ? *p : 0;
}

template <typename Memory>
uint16_t Dsp<Memory>::AcceleratorRead(bool raw)
{
    Accelerator& a = accelerator_;
    const int size = a.format & 3;
    const auto byte = [this](uint32_t address)
    {
        return AcceleratorByte(address);
    };

    if (raw)
    {
        // Returns the raw value, then advances the address. A read at the end wraps to the start and raises exception
        // 3.
        const uint16_t value = AcceleratorRaw(a.current, size, byte);
        a.current = size == 3 ? (a.current & ~3u) | ((a.current + 1) & 3) : a.current + 1;
        if (a.current - 1 == a.end)
        {
            a.current = a.start;
            RaiseException(3);
        }

        a.current &= 0xbfffffff;

        return value;
    }

    // Returns a decoded sample and advances the address past frame headers. Reaching the end stops the reads and raises
    // exception 5.
    const std::span<const uint16_t, 16> coefs(hardware_.data() + 0xa0, 16);
    const auto at_end = [this, &a]
    {
        a.reads_stopped = true;
        RaiseException(5);
    };
    return AcceleratorNext(a, coefs, byte, at_end);
}

template <typename Memory>
void Dsp<Memory>::AcceleratorWrite(uint16_t value)
{
    // Writes happen only when bit 31 of the current address is set. They always go to ARAM (MEM2 on the Wii) and raise
    // exception 4 each time.
    Accelerator& a = accelerator_;
    if (!(a.current & 0x80000000))
    {
        return;
    }

    if (uint8_t* p = memory_->Aram((a.current * 2) & 0x03ffffff, 2))
    {
        xsflib::StoreBe16(p, value);
    }

    a.current++;
    RaiseException(4);
}

template <typename Memory>
uint16_t Dsp<Memory>::MailboxReadHigh(uint32_t mailbox)
{
    return static_cast<uint16_t>(mailbox >> 16);
}

template <typename Memory>
uint16_t Dsp<Memory>::MailboxReadLow(uint32_t& mailbox)
{
    mailbox &= ~kMailboxFull;

    return static_cast<uint16_t>(mailbox);
}

template <typename Memory>
void Dsp<Memory>::MailboxWriteHigh(uint32_t& mailbox, uint16_t value)
{
    mailbox = ((uint32_t{value} << 16) | (mailbox & 0xffff)) & ~kMailboxFull;
}

template <typename Memory>
void Dsp<Memory>::MailboxWriteLow(uint32_t& mailbox, uint16_t value)
{
    mailbox = (mailbox & 0xffff0000) | value | kMailboxFull;
}

template <typename Memory>
uint16_t Dsp<Memory>::CpuReadMailboxHigh(bool to_dsp) const
{
    return MailboxReadHigh(to_dsp ? mailbox_to_dsp_ : mailbox_to_cpu_);
}

template <typename Memory>
uint16_t Dsp<Memory>::CpuReadMailboxLow(bool to_dsp)
{
    return MailboxReadLow(to_dsp ? mailbox_to_dsp_ : mailbox_to_cpu_);
}

template <typename Memory>
void Dsp<Memory>::CpuWriteMailboxHigh(uint16_t value)
{
    MailboxWriteHigh(mailbox_to_dsp_, value);
}

template <typename Memory>
void Dsp<Memory>::CpuWriteMailboxLow(uint16_t value)
{
    MailboxWriteLow(mailbox_to_dsp_, value);
}

template <typename Memory>
uint16_t Dsp<Memory>::Control(uint64_t time_base)
{
    // Only these bits are kept on the DSP side.
    if ((control_ & control::kInitBusy) && time_base >= init_busy_until_)
    {
        control_ &= ~control::kInitBusy;
    }

    return control_ & (control::kReset | control::kInterrupt | control::kHalt | control::kInitBusy | control::kInit);
}

template <typename Memory>
void Dsp<Memory>::WriteControl(uint16_t value, uint64_t time_base)
{
    // The DSP side, in order: reset, the init path, the new value and the CPU's interrupt request.
    if (value & control::kReset)
    {
        Reset();
        value &= ~control::kReset;
    }

    if ((control_ & control::kInit) && !(value & control::kInit))
    {
        InitPath();
        value = (value & ~control::kInit) | control::kInitBusy;
        init_busy_until_ = time_base + kInitBusyTicks;
    }

    control_ = value;
    if (value & control::kInterrupt)
    {
        if (regs_.sr & kSrExternalEnable)
        {
            RaiseException(7);
            control_ &= ~control::kInterrupt;
        }

        CheckExceptions();
    }
}

template <typename Memory>
void Dsp<Memory>::Reset()
{
    // Only the PC and the wrap registers change.
    regs_.pc = 0x8000;
    regs_.wr = {0xffff, 0xffff, 0xffff, 0xffff};
    BuildLoopMap();
}

template <typename Memory>
void Dsp<Memory>::InitPath()
{
    // Copy the 0x1000 bytes at 0x01000000 into IRAM's first 0x800 words and set the PC to 0.
    regs_.pc = 0;
    if (const uint8_t* source = memory_->At(kInitProgram, kInitProgramSize))
    {
        for (uint32_t i = 0; i < kInitProgramSize / 2; i++)
        {
            iram_[i] = xsflib::LoadBe16(source + i * 2);
        }
    }

    BuildLoopMap();
}

} // namespace ntdlib::dsp
