// SPDX-License-Identifier: MIT

// Tests flipper's parts that run without a game. FileDisc on made-up files: the header's fields for the file system
// table, the table's entries, the files' data and the zeros in the rest of the last file's last 32-byte block, with the
// GameCube's offsets in bytes and the Wii's offsets in units of 4 bytes. The register file, the processor interface,
// and the audio interface's sample counter at each console's rates. MachineBase with a machine that runs no code: main
// memory's pages, the report of unknown registers, the DMA rate that the audio interface passes on, and the blocks of
// the hardware page that both consoles share. The boot: low memory, the file system table, the OS globals, the CPU's
// start and the boot's errors. The game-mode source: a disc made of FILE chunks, MEM chunks, the audio's samples, the
// driver's status word and the stop messages.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/header.h"
#include "ntdlib/dsp/memory.h"
#include "ntdlib/flipper/audio_interface.h"
#include "ntdlib/flipper/file_disc.h"
#include "ntdlib/flipper/game_source.h"
#include "ntdlib/flipper/machine.h"
#include "ntdlib/flipper/processor_interface.h"
#include "ntdlib/flipper/registers.h"
#include "ntdlib/ppc/cpu.h"
#include "ntdlib/types.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

namespace
{

using xsflib::testing::Check;
using xsflib::testing::ErrorOf;

// Reads `size` bytes of `disc` at `offset`, or nothing if the read fails.
std::vector<uint8_t> ReadDisc(ntdlib::flipper::FileDisc& disc, uint64_t offset, std::size_t size)
{
    std::vector<uint8_t> data(size);
    if (!disc.Read(offset, data.data(), size))
    {
        return {};
    }

    return data;
}

// Builds a disc of two files with offsets in units of `unit` bytes and checks what a game reads from it.
void CheckDisc(uint32_t unit)
{
    const std::string name = "offsets in units of " + std::to_string(unit);
    const std::vector<uint8_t> kSound(1000, 0x5a);
    const std::vector<uint8_t> kSequences = {1, 2, 3, 4, 5, 6, 7};
    const std::vector<std::pair<std::string, std::span<const uint8_t>>> files = {
        {"AudioRes/a.baa", kSound},
        {"AudioRes/Seqs/b.arc", kSequences},
    };
    ntdlib::flipper::FileDisc disc(std::vector<uint8_t>(0x440, 0), files, unit);

    // The header gives the table's offset and size. The table starts with the root directory's entry.
    const std::vector<uint8_t> header = ReadDisc(disc, 0, 0x440);
    Check(header.size() == 0x440, name + ": the header reads");
    if (header.size() != 0x440)
    {
        return;
    }

    const uint64_t fst_offset = uint64_t{xsflib::LoadBe32(&header[0x424])} * unit;
    const uint64_t fst_size = uint64_t{xsflib::LoadBe32(&header[0x428])} * unit;
    Check(fst_offset == 0x2440, name + ": the table follows the apploader's place");
    Check(xsflib::LoadBe32(&header[0x42c]) == xsflib::LoadBe32(&header[0x428]), name + ": the table's largest size");
    const std::vector<uint8_t> fst = ReadDisc(disc, fst_offset, static_cast<std::size_t>(fst_size));
    Check(fst.size() == fst_size && fst_size >= 12 && fst[0] == 1, name + ": the table reads");
    if (fst.size() < 12 * 5)
    {
        return;
    }

    // Root, AudioRes, a.baa, Seqs and b.arc: the files are entries 2 and 4.
    Check(xsflib::LoadBe32(&fst[8]) == 5, name + ": the table has five entries");
    for (const auto& [index, data] : {std::pair<int, const std::vector<uint8_t>*>{2, &kSound}, {4, &kSequences}})
    {
        const uint8_t* entry = &fst[index * 12];
        const uint64_t offset = uint64_t{xsflib::LoadBe32(entry + 4)} * unit;
        const uint32_t size = xsflib::LoadBe32(entry + 8);
        Check(entry[0] == 0 && size == data->size(), name + ": file " + std::to_string(index) + "'s entry");
        Check(ReadDisc(disc, offset, size) == *data, name + ": file " + std::to_string(index) + "'s data");
    }

    // The disc interface transfers whole 32-byte blocks. A read of the last file's block gives the file's 7 bytes, then
    // zeros. Nothing comes after that block.
    const uint64_t last = uint64_t{xsflib::LoadBe32(&fst[4 * 12 + 4])} * unit;
    std::vector<uint8_t> block = kSequences;
    block.resize(32, 0);
    Check(ReadDisc(disc, last, 32) == block, name + ": the last file's block reads, with zeros past the file's end");
    Check(ReadDisc(disc, last, 33).empty(), name + ": a read past the last file's block fails");
    Check(ReadDisc(disc, 0x100000, 16).empty(), name + ": a read past the end fails");
}

// The register file stores big-endian values, and drops the bytes past its end.
void CheckRegisterFile()
{
    ntdlib::flipper::RegisterFile<8> file;
    file.Write(0, 4, 0x11223344);
    file.Write(6, 4, 0xaabbccdd);

    Check(file.Read(0, 4) == 0x11223344 && file.Read(1, 2) == 0x2233, "register file: big-endian reads");
    Check(file.Read(6, 4) == 0xaabb0000, "register file: the bytes past the end read as 0");
}

// The processor interface reads the causes that it's given and keeps the mask. The interrupt line follows the enabled
// causes in the low 16 bits. Its other registers store what's written to them.
void CheckProcessorInterface()
{
    ntdlib::flipper::ProcessorInterface pi;

    Check(!pi.Write(0x04, 0x00010040), "processor interface: the mask is modelled");
    Check(pi.Read(0x00, 0x1234) == 0x1234 && pi.Read(0x04, 0) == 0x00010040, "processor interface: causes and mask");
    Check(pi.Asserts(0x40) && !pi.Asserts(0x10000) && !pi.Asserts(0x20), "processor interface: the interrupt line");
    Check(!pi.Write(0x00, 0xffffffff), "processor interface: a write to the causes changes nothing");
    Check(pi.Write(0x24, 0x5678) && pi.Read(0x24, 0) == 0x5678, "processor interface: other registers store writes");
}

// The audio interface's counter counts once a period from a period after PSTAT is set. A write to AICR gives the DMA's
// rate. SCRESET clears the counter. A change of AISFR starts the other rate.
void CheckAudioInterface(const std::string& name, const ntdlib::flipper::AudioInterfaceOptions& options,
                         double period_32k, double period_48k)
{
    ntdlib::flipper::AudioInterface ai(options);
    const auto rate = ai.Write(0x00, 0x41, 1000); // PSTAT and AIDFR

    Check(rate && *rate, name + ": AIDFR selects the 32 kHz DMA");
    Check(!ai.Write(0x04, 0x12, 1000) && ai.Read(0x04, 1000) == 0x12, name + ": the volume");
    Check(ai.Read(0x00, 1000) == 0x41, name + ": AICR reads back");
    Check(ai.Read(0x08, 1000 + static_cast<uint64_t>(std::ceil(period_32k * 3))) == 3,
          name + ": three counts at 32 kHz");
    Check(ai.Read(0x08, 1000 + static_cast<uint64_t>(period_32k * 10.5)) == 10, name + ": ten counts at 32 kHz");

    const auto cleared = ai.Write(0x00, 0x21, 1000 + static_cast<uint64_t>(period_32k * 10.5)); // PSTAT and SCRESET
    Check(cleared && !*cleared, name + ": AIDFR clear selects the 48 kHz DMA");
    Check(ai.Read(0x08, 1000 + static_cast<uint64_t>(period_32k * 10.5)) == 0, name + ": SCRESET clears the counter");
    Check(ai.Read(0x00, 0) == 0x01, name + ": SCRESET reads back 0");

    ai.Write(0x00, 0x03, 50000); // PSTAT and AISFR: the counter restarts at 48 kHz
    Check(ai.Read(0x08, 50000 + static_cast<uint64_t>(period_48k * 2)) == 2, name + ": two counts at 48 kHz");
}

// A memory of 8 KiB for the machine below.
struct TestMemory
{
    uint8_t* At(ntdlib::PAddr address, std::size_t size)
    {
        return address + size <= bytes.size() ? bytes.data() + address : nullptr;
    }

    std::array<uint8_t, 0x2000> bytes{};
};

// What the machine below passes to its DSP interface: AIDFR's DMA rate, and the 16-bit registers.
struct TestDspInterface
{
    void SetDmaRate32k(bool rate_32k)
    {
        dma_rate_32k = rate_32k;
    }

    uint16_t Read(uint32_t reg) const
    {
        return regs[reg / 2];
    }

    void Write(uint32_t reg, uint16_t value)
    {
        regs[reg / 2] = value;
    }

    bool dma_rate_32k = false;
    std::array<uint16_t, 0x100> regs{};
};

// The machine's external interface: 32-bit registers that store what's written to them.
struct TestExi
{
    uint32_t Read(uint32_t offset) const
    {
        return regs[offset / 4];
    }

    void Write(uint32_t offset, uint32_t value)
    {
        regs[offset / 4] = value;
    }

    std::array<uint32_t, 0x10> regs{};
};

// A machine with the shared blocks of the hardware page at 0x0c000000, and no other device. It runs no code.
class TestMachine final : public ntdlib::flipper::MachineBase<TestMachine>, private ntdlib::flipper::SharedRegisters
{
public:
    TestMachine() : SharedRegisters(ntdlib::flipper::kGameCubeAudioInterface)
    {
    }

    ntdlib::ppc::Cpu& Cpu()
    {
        return cpu_;
    }

    TestMemory& Memory()
    {
        return memory_;
    }

    bool ReadIo(ntdlib::PAddr address, int size, uint32_t& value) override
    {
        value = 0;
        const std::optional<bool> known =
            (address & 0xffff0000) == 0x0c000000 ? ReadShared(address & 0xffff, size, value) : std::nullopt;
        if (!known || !*known)
        {
            ReportUnknown(address, size, false, 0);
        }

        return known.has_value();
    }

    bool WriteIo(ntdlib::PAddr address, int size, uint32_t value) override
    {
        const std::optional<bool> known =
            (address & 0xffff0000) == 0x0c000000 ? WriteShared(address & 0xffff, size, value) : std::nullopt;
        if (!known)
        {
            ReportUnknown(address, size, true, value);
            return false;
        }

        FinishWrite(address, size, value, *known);

        return true;
    }

    TestDspInterface dsp_interface_;
    TestExi exi_;
    std::vector<std::string> reports;

private:
    friend class ntdlib::flipper::MachineBase<TestMachine>;

    uint64_t NextEvent() const
    {
        return UINT64_MAX;
    }

    void Advance()
    {
    }

    uint32_t InterruptCauses() const
    {
        return 0;
    }

    TestMemory memory_;
    ntdlib::ppc::Cpu cpu_{*this};
};

// MachineBase gives the CPU main memory's pages, reports each unknown register once, and passes AIDFR to the DSP
// interface.
void CheckMachineBase()
{
    TestMachine machine;
    machine.on_unknown_io = [&](const std::string& line)
    {
        machine.reports.push_back(line);
    };

    Check(machine.RamPage(0x1000) == machine.Memory().bytes.data() + 0x1000, "machine: the second page of memory");
    Check(machine.RamPage(0x2000) == nullptr, "machine: no page past the end of memory");
    machine.WriteIo(0x0c003024, 4, 0x5678);
    machine.WriteIo(0x0c003024, 4, 0x9abc);
    uint32_t value = 0;
    machine.ReadIo(0x0c008000, 2, value);
    Check(machine.reports ==
              std::vector<std::string>{"write32 0c003024 = 00005678 at pc 00000000", "read16 0c008000 at pc 00000000"},
          "machine: each unknown register is reported once");
    machine.WriteIo(0x0c006c00, 4, 0x40);
    Check(machine.dsp_interface_.dma_rate_32k, "machine: AIDFR reaches the DSP interface");
}

// The shared blocks: the DSP interface's 16-bit registers in 32-bit accesses, SI's instant transfers, the blocks that
// store what's written to them, and EXI. ReadShared and WriteShared leave the disc interface's block and the blocks
// after AI to the machine.
void CheckSharedBlocks()
{
    TestMachine machine;
    machine.on_unknown_io = [&](const std::string& line)
    {
        machine.reports.push_back(line);
    };
    uint32_t value = 0;

    machine.dsp_interface_.regs[0] = 0x1234;
    machine.dsp_interface_.regs[1] = 0x5678;
    machine.ReadIo(0x0c005000, 4, value);
    Check(value == 0x12345678, "shared blocks: a 32-bit read of the DSP interface takes the high half first");
    machine.ReadIo(0x0c005002, 2, value);
    Check(value == 0x5678, "shared blocks: a 16-bit read of the DSP interface");
    machine.WriteIo(0x0c005004, 4, 0xaabbccdd);
    Check(machine.dsp_interface_.regs[2] == 0xaabb && machine.dsp_interface_.regs[3] == 0xccdd,
          "shared blocks: a 32-bit write of the DSP interface writes both halves");

    machine.WriteIo(0x0c006434, 4, 0x00000005);
    machine.ReadIo(0x0c006434, 4, value);
    Check(value == 0x80000004, "shared blocks: a write of SICOMCSR's TSTART finishes the transfer and sets TCINT");
    machine.WriteIo(0x0c006434, 4, 0x00000002);
    machine.ReadIo(0x0c006434, 4, value);
    Check(value == 0x00000002, "shared blocks: SICOMCSR without TSTART stores what's written");

    for (const uint32_t block : {0x0000u, 0x1000u, 0x2000u, 0x4000u})
    {
        machine.WriteIo(0x0c000010 + block, 2, 0xbeef);
        machine.ReadIo(0x0c000010 + block, 2, value);
        Check(value == 0xbeef, "shared blocks: CP, PE, VI and MI store what's written to them");
    }

    machine.WriteIo(0x0c006808, 4, 0x01020304);
    machine.ReadIo(0x0c006808, 4, value);
    Check(machine.exi_.regs[2] == 0x01020304 && value == 0x01020304, "shared blocks: EXI's registers");

    machine.reports.clear();
    Check(!machine.ReadIo(0x0c006000, 4, value) && !machine.ReadIo(0x0c007000, 4, value),
          "shared blocks: the machine handles the disc interface and the blocks after AI");
    Check(
        machine.reports == std::vector<std::string>{"read32 0c006000 at pc 00000000", "read32 0c007000 at pc 00000000"},
        "shared blocks: the machine reports the blocks that it doesn't handle");
}

// A machine with GameCube memory and a disc, for the boot. It runs no code.
class BootMachine final : public ntdlib::flipper::MachineBase<BootMachine>
{
public:
    explicit BootMachine(ntdlib::flipper::Disc& disc) : disc_(disc)
    {
    }

    ntdlib::ppc::Cpu& Cpu()
    {
        return cpu_;
    }

    ntdlib::dsp::GameCubeMemory& Memory()
    {
        return memory_;
    }

    bool ReadIo(ntdlib::PAddr, int, uint32_t& value) override
    {
        value = 0;
        return false;
    }

    bool WriteIo(ntdlib::PAddr, int, uint32_t) override
    {
        return false;
    }

    uint32_t Word(uint32_t address)
    {
        return xsflib::LoadBe32(memory_.At(ntdlib::dsp::ToPhysical(address), 4));
    }

    using MachineBase::LoadBootInfo;
    using MachineBase::StartCpu;

private:
    friend class ntdlib::flipper::MachineBase<BootMachine>;

    ntdlib::flipper::Disc& disc_;
    ntdlib::dsp::GameCubeMemory memory_;
    ntdlib::ppc::Cpu cpu_{*this};
};

// A disc that can't be read.
class NoDisc final : public ntdlib::flipper::Disc
{
public:
    bool Read(uint64_t, void*, std::size_t) override
    {
        return false;
    }
};

// The boot's low memory and file system table from a FileDisc, in each console's units, its errors, and the CPU's
// start.
void CheckBoot(uint32_t unit)
{
    const std::string name = "boot (unit " + std::to_string(unit) + ")";
    std::vector<uint8_t> header(ntdlib::disc::kHeaderSize, 0);
    std::copy_n("GTST01", 6, header.begin());
    const std::vector<uint8_t> data(100, 0x5a);
    ntdlib::flipper::FileDisc disc(header, {{"Audio/a.bin", data}, {"b.bin", data}}, unit);
    BootMachine machine(disc);
    const ntdlib::flipper::BootInfo info{unit, 0x81700000, 3, 162000000, 486000000};
    const auto loaded = machine.LoadBootInfo(info);

    uint8_t read[ntdlib::disc::kHeaderSize];
    disc.Read(0, read, sizeof(read));
    Check(std::equal(loaded.begin(), loaded.end(), read), name + ": the header it returns");
    Check(std::equal(read, read + 0x20, machine.Memory().At(0, 0x20)), name + ": the header's first 32 bytes");
    const uint32_t fst_size = xsflib::LoadBe32(read + ntdlib::disc::kHeaderFstSize) * unit;
    const uint32_t fst_address = (0x81800000 - fst_size) & ~0x1fu;
    std::vector<uint8_t> fst(fst_size);
    disc.Read(uint64_t{xsflib::LoadBe32(read + ntdlib::disc::kHeaderFstOffset)} * unit, fst.data(), fst.size());
    Check(std::equal(fst.begin(), fst.end(), machine.Memory().At(ntdlib::dsp::ToPhysical(fst_address), fst_size)),
          name + ": the file system table at the top of MEM1");
    Check(machine.Word(0x80000020) == 0x0d15ea5e && machine.Word(0x80000024) == 1 &&
              machine.Word(0x80000028) == 0x01800000 && machine.Word(0x8000002c) == 3 &&
              machine.Word(0x80000030) == 0 && machine.Word(0x80000034) == 0x81700000 &&
              machine.Word(0x80000038) == fst_address && machine.Word(0x8000003c) == fst_size &&
              machine.Word(0x800000f0) == 0x01800000 && machine.Word(0x800000f8) == 162000000 &&
              machine.Word(0x800000fc) == 486000000,
          name + ": the OS globals");

    machine.StartCpu(0x80003100);
    const ntdlib::ppc::State& s = machine.Cpu().state_;
    Check(s.pc == 0x80003100 &&
              s.msr == (ntdlib::ppc::msr::kIr | ntdlib::ppc::msr::kDr | ntdlib::ppc::msr::kRi | ntdlib::ppc::msr::kFp),
          name + ": the CPU starts at the entry with translation and floating point on");
    Check(s.spr[ntdlib::ppc::spr::kIbat0u] == 0x80001fff && s.spr[ntdlib::ppc::spr::kIbat0u + 1] == 2 &&
              s.spr[ntdlib::ppc::spr::kIbat0u + 8] == 0x80001fff && s.spr[ntdlib::ppc::spr::kIbat0u + 9] == 2 &&
              s.spr[ntdlib::ppc::spr::kIbat0u + 10] == 0xc0001fff && s.spr[ntdlib::ppc::spr::kIbat0u + 11] == 0x2a,
          name + ": the BATs of MEM1, cached and uncached");

    BootMachine high(disc);
    const std::string too_large = ErrorOf([&] { high.LoadBootInfo({unit, 0x817f0000, 3, 0, 0}); });
    Check(too_large == "the disc's file system table is too large",
          name + ": a table that doesn't fit above the arena");
    NoDisc none;
    BootMachine unreadable(none);
    const std::string no_header = ErrorOf([&] { unreadable.LoadBootInfo(info); });
    Check(no_header == "the disc's header can't be read", name + ": a disc without a header");
}

// The game-mode source: a disc made of FILE chunks, MEM chunks in memory, the audio's samples, the driver's status and
// the stop messages.
void CheckGameSource()
{
    std::vector<uint8_t> header(ntdlib::disc::kHeaderSize, 0);
    std::copy_n("GTST01", 6, header.begin());
    const std::vector<xsflib::psf::FileChunk> files = {
        {"sys/boot.bin", header}, {"a.bin", {1, 2, 3}}, {"a.bin", {4, 5}}, {"b.bin", {6}}};
    const auto disc = ntdlib::flipper::DiscFromFiles(files, "sys/boot.bin", ntdlib::disc::kGameCubeMagicOffset,
                                                     ntdlib::disc::kGameCubeMagic, 1);
    uint8_t read[ntdlib::disc::kHeaderSize];
    Check(disc->Read(0, read, sizeof(read)) && std::equal(read, read + 0x420, header.begin()),
          "game source: the header from its chunk");
    const uint32_t fst_offset = xsflib::LoadBe32(read + ntdlib::disc::kHeaderFstOffset);
    std::vector<uint8_t> fst(xsflib::LoadBe32(read + ntdlib::disc::kHeaderFstSize));
    disc->Read(fst_offset, fst.data(), fst.size());
    Check(xsflib::LoadBe32(fst.data() + 8) == 3 && xsflib::LoadBe32(fst.data() + 12 + 8) == 2,
          "game source: two files, the later a.bin replacing the earlier");

    const auto blank = ntdlib::flipper::DiscFromFiles({{"b.bin", {6}}}, "sys/boot.bin", ntdlib::disc::kWiiMagicOffset,
                                                      ntdlib::disc::kWiiMagic, 4);
    Check(blank->Read(0, read, sizeof(read)) && xsflib::LoadBe32(read + 0x18) == 0x5d1c9ea3 &&
              std::all_of(read, read + 0x18, [](uint8_t b) { return b == 0; }),
          "game source: a blank header with the console's magic");

    ntdlib::dsp::GameCubeMemory memory;
    Check(ntdlib::flipper::LoadMemChunks(memory, {{0x80003100, {1, 2, 3, 4}}}) && memory.At(0x3100, 4)[3] == 4,
          "game source: a MEM chunk at its CPU address");
    Check(!ntdlib::flipper::LoadMemChunks(memory, {{0x817ffffe, {1, 2, 3, 4}}}), "game source: a MEM chunk past MEM1");

    std::array<uint8_t, 32> block{};
    for (std::size_t i = 0; i < block.size(); i++)
    {
        block[i] = static_cast<uint8_t>(i);
    }
    std::vector<int16_t> samples;
    ntdlib::flipper::AppendAudioBlock(samples, block);
    Check(samples.size() == 16 && samples[0] == 0x0203 && samples[1] == 0x0001 && samples[15] == 0x1c1d,
          "game source: each block's sample is right, then left");

    xsflib::StoreBe32(memory.At(0x01700018, 4), 2);
    Check(ntdlib::flipper::DriverStatus(memory, 0x81700018) == 2 && ntdlib::flipper::DriverStatus(memory, 0) == 1 &&
              ntdlib::flipper::DriverStatus(memory, 0x81fffffe) == 1,
          "game source: the driver's status word, and 1 without one");
    Check(ntdlib::flipper::StopMessage(ntdlib::ppc::StopReason::kFault) ==
                  "the emulated program accessed memory that isn't there" &&
              ntdlib::flipper::StopMessage(ntdlib::ppc::StopReason::kIllegal) ==
                  "the emulated program ran an instruction that the CPU doesn't support" &&
              !ntdlib::flipper::StopMessage(ntdlib::ppc::StopReason::kBudget),
          "game source: the stop messages");
}

} // namespace

int main()
{
    CheckDisc(1);
    CheckDisc(4);
    CheckRegisterFile();
    CheckProcessorInterface();
    CheckAudioInterface("GameCube", ntdlib::flipper::kGameCubeAudioInterface, 15174.0, 486e6 * 2248.0 / 108e6);
    CheckAudioInterface("Wii", ntdlib::flipper::kWiiAudioInterface, 22781.25, 15187.5);
    CheckMachineBase();
    CheckSharedBlocks();
    CheckBoot(1);
    CheckBoot(4);
    CheckGameSource();

    return xsflib::testing::Summary("flipper");
}
