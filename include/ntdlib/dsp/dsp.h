// SPDX-License-Identifier: MIT

// The GameCube and Wii DSP: a 16-bit Macronix DSP with 4 KWords each of instruction and data RAM. It runs the game's
// DSP program, such as AX's microcode. This class covers the DSP and its DSP-side hardware: the mailboxes, DMA to and
// from main memory, the accelerator that streams and decodes samples, and the DSP-side bits of the control register
// (reset, halt, the CPU's interrupt request and the init path). The CPU-side registers around it (the rest of DSPCR,
// the ARAM DMA and the audio interface's DMA) belong to the system that contains the DSP. The template parameter is the
// console's memory: WiiMemory, GameCubeMemory or WiiUMemory (memory.h). The Wii U's DSP is the Wii's DSP with more RAM
// and two registers that extend main-memory addresses. WiiUMemory gives those.
//
// The DSP was written in a clean room from a behavioural specification of Dolphin's DSP interpreter. It gives the same
// results as Dolphin, instruction by instruction. The boot ROM isn't shipped. The routines of the boot ROM that games
// use are implemented here: the entry and command loop that load a microcode, the loader at 0x80B5, and the four mixing
// routines that an older AX microcode calls.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/dsp/accelerator.h"

namespace ntdlib::dsp
{

// DSPCR's DSP-side bits.
namespace control
{

constexpr uint16_t kReset = 1u << 0;
constexpr uint16_t kInterrupt = 1u << 1;
constexpr uint16_t kHalt = 1u << 2;
constexpr uint16_t kInitBusy = 1u << 10;
constexpr uint16_t kInit = 1u << 11;

// All of the DSP-side bits. The system around the DSP keeps the others.
constexpr uint16_t kDspSide = kReset | kInterrupt | kHalt | kInitBusy | kInit;

} // namespace control

// A hardware stack: a visible top and 32 hidden entries behind a 5-bit pointer.
struct Stack
{
    uint16_t top = 0;
    uint8_t pointer = 0;
    std::array<uint16_t, 32> hidden{};
};

struct Registers
{
    std::array<uint16_t, 4> ar{};
    std::array<uint16_t, 4> ix{};
    std::array<uint16_t, 4> wr{0xffff, 0xffff, 0xffff, 0xffff};
    std::array<Stack, 4> stacks{};
    std::array<int64_t, 2> ac{}; // 40-bit, sign-extended
    std::array<uint16_t, 2> ax_l{};
    std::array<uint16_t, 2> ax_h{};
    uint16_t prod_l = 0;
    uint16_t prod_m1 = 0;
    uint16_t prod_h = 0; // 8 bits
    uint16_t prod_m2 = 0;
    uint16_t sr = 0x0a00;
    uint8_t cr = 0;
    uint16_t pc = 0x8000;
};

// The DSP's members are template instances. A program exports them as weak symbols. On macOS every call to an exported
// weak symbol goes through a stub. The stubs make the step loop measurably slower. Hidden visibility keeps the calls
// direct.
#if defined(__APPLE__)
#define NTDLIB_DSP_HIDDEN __attribute__((visibility("hidden")))
#else
#define NTDLIB_DSP_HIDDEN
#endif

// A copy of a Dsp is a snapshot of the DSP's state. Assigning the copy back restores that state. The copy shares main
// memory with the original Dsp.
template <typename Memory>
class NTDLIB_DSP_HIDDEN Dsp
{
public:
    explicit Dsp(Memory& memory);

    // Runs one step: an exception if one is due, one instruction (or one step of a boot ROM routine), and the loop
    // check.
    void Step();

    // Runs up to `steps` steps. Stops early if the DSP halts. Returns the number of steps run.
    uint64_t Run(uint64_t steps);

    bool Halted() const
    {
        return (control_ & control::kHalt) != 0;
    }

    // Whether the next step starts a polling loop that waits on a mailbox or on DRAM word 0x0352 (as Dolphin's analysis
    // finds them), whatever the polled word holds, or one of the boot ROM's waits for mail. A batch of DSP cycles ends
    // early at these points.
    bool AtIdleLoop() const;

    // Starts `microcode` at `entry` without running the boot code. The DSP is left in the state that the boot ROM
    // leaves when the SDK's boot code loads the microcode from `address`. IRAM holds the microcode and DRAM is clear.
    void StartMicrocode(std::span<const uint16_t> microcode, uint32_t address, uint16_t entry);

    // Starts running at `pc` with the halt and init bits clear. This matches the hand-over from the boot ROM.
    void StartAt(uint16_t pc)
    {
        regs_.pc = pc;
        control_ &= ~(control::kHalt | control::kInit);
    }

    // A hardware register's stored value. For registers that are plain storage, this is what a read returns.
    uint16_t HardwareRegister(uint8_t reg) const
    {
        return hardware_[reg];
    }

    const Accelerator& AcceleratorState() const
    {
        return accelerator_;
    }

    // The pending exceptions as a bit mask indexed by exception number.
    uint8_t PendingExceptions() const
    {
        return pending_exceptions_;
    }

    // The exception that the last step took before its instruction, or -1.
    int TakenException() const
    {
        return taken_exception_;
    }

    // Loads words into IRAM or DRAM like a DMA. An IRAM load rebuilds the loop-end map.
    void LoadIram(uint16_t address, std::span<const uint16_t> words);
    void LoadDram(uint16_t address, std::span<const uint16_t> words);

    std::span<const uint16_t> Iram() const
    {
        return iram_;
    }

    std::span<const uint16_t> Dram() const
    {
        return dram_;
    }

    // The CPU side of the mailboxes: the high and low halves that the CPU reads and writes.
    uint16_t CpuReadMailboxHigh(bool to_dsp) const;
    uint16_t CpuReadMailboxLow(bool to_dsp);
    void CpuWriteMailboxHigh(uint16_t value);
    void CpuWriteMailboxLow(uint16_t value);

    // The 32 bits of each mailbox. Bit 31 is the full flag.
    uint32_t MailboxToDsp() const
    {
        return mailbox_to_dsp_;
    }

    uint32_t MailboxToCpu() const
    {
        return mailbox_to_cpu_;
    }

    // DSPCR's DSP-side bits, and a CPU write of DSPCR. `time_base` is the CPU's time base. The init path's busy bit
    // uses it.
    uint16_t Control(uint64_t time_base);
    void WriteControl(uint16_t value, uint64_t time_base);

    // Whether the DSP has requested a CPU interrupt (a DIRQ write with bit 0 set) since the last call.
    bool TakeInterruptRequest()
    {
        const bool requested = interrupt_request_;
        interrupt_request_ = false;

        return requested;
    }

    // Steps run since construction.
    uint64_t Steps() const
    {
        return steps_;
    }

    // The instruction words with dedicated handlers, and a switch that sends every word through the generic decoder
    // instead. Tests use these to compare the two paths.
    static std::span<const uint16_t> FrequentWords();
    void UseGenericHandlers();

    Registers regs_;

private:
    // The boot ROM model's state between steps.
    enum class Rom : uint8_t
    {
        kNone,          // in no routine: a step in the ROM starts the routine at the PC
        kCommandStart,  // the command loop clears the accumulators for the next mail
        kWaitCommand,   // the command loop waits for a mail
        kWaitArgument,  // the command loop waits for a command's argument
        kWaitMailTaken, // the command loop waits for the CPU to read its reply
        kLoading,       // the loader has loaded the microcode and runs out its steps before it jumps to it
        kMixing,        // a mixing routine has mixed and runs out its steps before it returns
    };

    // A register write that an extension defers until the main instruction has run.
    struct PendingWrite
    {
        enum Kind : uint8_t
        {
            kRegister,     // a write by number
            kLoadExtended, // a 'L or 'LN load into $acX.m in 40-bit mode
        };

        Kind kind = kRegister;
        uint8_t reg = 0;
        uint16_t value = 0;
    };

    // The writes that an instruction's extension defers: at most four (two registers and two address registers).
    struct HeldWrites
    {
        std::array<PendingWrite, 4> writes{};
        int count = 0;
    };

    // An instruction's handler (instructions.cpp). A table maps each instruction word to its handler. Most words go to
    // Execute. The words that AX's microcodes run most have dedicated handlers. In those handlers the compiler folds
    // away every test of the instruction's kind.
    using Handler = void (*)(Dsp& dsp, uint16_t op);

    static const Handler* Handlers();
    static const Handler* GenericHandlers();

    template <uint16_t word>
    static void ExecuteWord(Dsp& dsp, uint16_t op);

    // Instruction execution (instructions.cpp). ExecuteOp holds the body of every instruction. `kind` is the form that
    // the decoder gives the instruction. ExecuteOp and Extension are inlined into each handler.
    void Execute(uint16_t op);
    inline void ExecuteOp(uint16_t op, uint8_t kind);
    inline void Extension(uint8_t field, HeldWrites& held);
    static void Hold(HeldWrites& held, uint8_t reg, uint16_t value,
                     typename PendingWrite::Kind kind = PendingWrite::kRegister);
    uint16_t FetchSecondWord();
    int SizeOf(uint16_t op) const;
    void SkipNextInstruction();
    void BuildLoopMap();

    // Registers (dsp.cpp).
    inline uint16_t ReadReg(int reg);
    inline void WriteReg(int reg, uint16_t value);
    void WriteRegLoad(int reg, uint16_t value);
    inline uint16_t RawReg(int reg) const;
    void Push(int stack, uint16_t value);
    uint16_t Pop(int stack);
    int64_t Product() const;
    void SetProduct(int64_t value);
    uint16_t AcH(int i) const;
    uint16_t AcM(int i) const;
    uint16_t AcL(int i) const;
    void SetAcH(int i, uint16_t value);
    void SetAcM(int i, uint16_t value);
    void SetAcL(int i, uint16_t value);
    void SetAc(int i, int64_t value);
    int32_t Ax(int i) const;
    bool Condition(int cc) const;
    void SetFlags40(int64_t value, bool carry, bool overflow);
    void SetFlagsAdd(int64_t a, int64_t b, int64_t result);
    void SetFlagsSub(int64_t a, int64_t b, int64_t result);
    void SetFlags16(uint16_t value, bool above_s32);
    uint16_t Increment(uint16_t address, uint16_t wrap) const;
    uint16_t Decrement(uint16_t address, uint16_t wrap) const;
    uint16_t AddIndex(uint16_t address, uint16_t index, uint16_t wrap) const;
    uint16_t SubtractIndex(uint16_t address, uint16_t index, uint16_t wrap) const;

    // The steps of an address register after an access, by the two bits that encode them: none, a decrement, an
    // increment, or an add of its index register. SteppedAddress gives $arI after its step.
    enum AddressStep : int
    {
        kNoStep,
        kDecrementStep,
        kIncrementStep,
        kAddIndexStep,
    };

    uint16_t SteppedAddress(int i, int step) const;

    void RaiseException(int number);
    void CheckExceptions();
    void RaiseCpuInterrupt();
    void CheckLoop();

    // Memory and hardware (hardware.cpp).
    uint16_t ReadImem(uint16_t address) const
    {
        // Addresses outside IRAM read as 0. The boot ROM isn't shipped. Its routines run in boot_rom.cpp.
        return address < Memory::kIramSize ? iram_[address] : 0;
    }

    uint16_t ReadDmem(uint16_t address);
    void WriteDmem(uint16_t address, uint16_t value);
    uint16_t ReadHardware(uint8_t reg);
    void WriteHardware(uint8_t reg, uint16_t value);
    void Dma();
    uint8_t* MainMemoryRange(uint32_t address, uint32_t size);
    uint16_t AcceleratorReadRaw();
    uint16_t AcceleratorReadSample();
    void AcceleratorWrite(uint16_t value);
    uint8_t AcceleratorByte(uint32_t address) const;
    static uint16_t MailboxReadHigh(uint32_t mailbox);
    static uint16_t MailboxReadLow(uint32_t& mailbox);
    static void MailboxWriteHigh(uint32_t& mailbox, uint16_t value);
    static void MailboxWriteLow(uint32_t& mailbox, uint16_t value);
    void Reset();
    void InitPath();

    // The boot ROM (boot_rom.cpp). RomStep runs one step of the routine that rom_state_ gives, or starts the routine at
    // the PC. RomWaitMail is the routine at 0x8078 that waits for a mail. It returns CMBH, or nothing while the mailbox
    // is empty.
    void RomStep();
    void RomStartRoutine();
    std::optional<uint16_t> RomWaitMail();
    void RomTakeCommand();
    void RomTakeArgument();
    void RomEntry();
    void RomLoader();
    void RomMix(uint16_t routine);

    Memory* memory_;
    const Handler* handlers_ = Handlers();
    std::vector<uint16_t> iram_;
    std::vector<uint16_t> dram_;
    std::array<uint16_t, 256> hardware_{};
    Accelerator accelerator_;
    uint32_t mailbox_to_dsp_ = 0;
    uint32_t mailbox_to_cpu_ = 0;
    uint16_t control_ = control::kHalt | control::kInit;
    uint64_t init_busy_until_ = 0; // the time base at which DSPCR's init busy bit clears
    bool interrupt_request_ = false;
    uint8_t pending_exceptions_ = 0;
    int taken_exception_ = -1;
    uint64_t steps_ = 0;

    // The static analysis of IRAM. It records the loop ends that the loops in IRAM name, and the starts of the loops
    // that poll a mailbox or DRAM word 0x0352. A loop end can be anywhere in the 64K of IMEM addresses.
    std::vector<uint8_t> loop_end_;
    std::vector<uint8_t> idle_;

    // The boot ROM model's place between steps: its state, the command whose argument it waits for, and the steps the
    // current routine has left.
    Rom rom_state_ = Rom::kNone;
    uint16_t rom_command_ = 0;
    uint32_t rom_delay_ = 0;
};

} // namespace ntdlib::dsp
