// SPDX-License-Identifier: MIT

// Tests of the model of the Wii's banks (ntdlib/rsar/bank_file.h). The offsets that the tests expect follow from the
// layout that the header describes. The reader of formats.h reads the written banks back.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/wave_file.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace rsar = ntdlib::rsar;
using rsar::BankFile;
using rsar::WaveEncoding;
using rsar::WaveFile;
using Instruments = std::vector<std::optional<BankFile::Instrument>>;

rsar::ReadOptions PlaybackOptions()
{
    rsar::ReadOptions options;
    options.playback_fields = true;

    return options;
}

// A region whose values follow from `n`. It plays wave n % 2.
rsar::Region Values(int n)
{
    rsar::Region region;
    region.wave_index = n % 2;
    region.attack = static_cast<uint8_t>(100 + n);
    region.decay = 90;
    region.sustain = 80;
    region.release = 70;
    region.hold = static_cast<uint8_t>(n);
    region.original_key = static_cast<uint8_t>(60 + n);
    region.volume = 120;
    region.pan = static_cast<uint8_t>(64 - n);
    region.tune = 1.0f + static_cast<float>(n) / 8.0f;

    return region;
}

// A key region over `first` to `last` that plays one region over every velocity.
BankFile::KeyRegion Keys(uint8_t first, uint8_t last, int n)
{
    return {first, last, {{0, 127, Values(n)}}};
}

// Five programs: one region over every key; nothing; three key regions with a gap; two velocity regions; and ten
// single keys with a run of three after a gap, which take an index table.
Instruments FiveInstruments()
{
    Instruments instruments(5);
    instruments[0] = BankFile::Instrument{{Keys(0, 127, 0)}};
    instruments[2] = BankFile::Instrument{{Keys(0, 35, 1), Keys(48, 59, 2), Keys(60, 127, 3)}};
    instruments[3] = BankFile::Instrument{{{0, 127, {{0, 63, Values(4)}, {64, 127, Values(5)}}}}};
    BankFile::Instrument& drums = instruments[4].emplace();
    for (int k = 0; k < 10; ++k)
    {
        drums.key_regions.push_back(Keys(static_cast<uint8_t>(36 + k), static_cast<uint8_t>(36 + k), 6 + k));
    }
    drums.key_regions.push_back(Keys(50, 52, 16));

    return instruments;
}

// A mono DSP-ADPCM wave of 30 samples and a stereo PCM16 wave of 10.
std::vector<WaveFile> TwoWaves()
{
    std::vector<WaveFile> waves(2);
    waves[0].loop_end = 30;
    waves[0].channels.push_back({std::vector<uint8_t>(24, 0x11), rsar::DspAdpcmInfo{}});
    waves[1].encoding = WaveEncoding::kPcm16;
    waves[1].loop = true;
    waves[1].loop_start = 4;
    waves[1].loop_end = 10;
    waves[1].channels.push_back({std::vector<uint8_t>(20, 0x22), std::nullopt});
    waves[1].channels.push_back({std::vector<uint8_t>(20, 0x33), std::nullopt});

    return waves;
}

// Whether the reference at `at` of `r` names `offset` with data type `type`.
bool Names(const xsflib::Reader& r, std::size_t at, uint8_t type, uint32_t offset)
{
    return r.U8(at) == 1 && r.U8(at + 1) == type && r.U16(at + 2) == 0 && r.U32(at + 4) == offset;
}

bool Nothing(const xsflib::Reader& r, std::size_t at)
{
    return r.U32(at) == 0 && r.U32(at + 4) == 0;
}

// FromInstruments lays each instrument out in pre-order after the references: a region for one region over every key
// and velocity, a range table for up to 11 entries with entries for the gaps, an index table for more, and a range
// table of the velocities.
void TestLayout()
{
    const BankFile bank = BankFile::FromInstruments(0x0102, FiveInstruments(), TwoWaves());
    const BankFile::Files files = bank.Write();
    const xsflib::Reader r(files.file, true);
    const std::size_t base = 0x28;

    XSFLIB_CHECK(files.file.size() == 0x460 && r.Magic(0, "RBNK") && r.U16(4) == 0xFEFF && r.U16(6) == 0x0102);
    XSFLIB_CHECK(r.U32(8) == 0x460 && r.U16(0xC) == 0x20 && r.U16(0xE) == 1 && r.U32(0x10) == 0x20);
    XSFLIB_CHECK(r.U32(0x14) == 0x440 && r.U32(0x18) == 0 && r.Magic(0x20, "DATA") && r.U32(0x24) == 0x440);
    XSFLIB_CHECK(r.U32(base) == 5 && Names(r, base + 4, 1, 0x2C) && Nothing(r, base + 0xC));
    XSFLIB_CHECK(Names(r, base + 0x14, 2, 0x5C) && Names(r, base + 0x1C, 2, 0x114) && Names(r, base + 0x24, 3, 0x194));

    // The region of program 0.
    XSFLIB_CHECK(r.S32(base + 0x2C) == 0 && r.U8(base + 0x30) == 100 && r.U8(base + 0x31) == 90);
    XSFLIB_CHECK(r.U8(base + 0x38) == 60 && r.U8(base + 0x39) == 120 && r.U8(base + 0x3A) == 64);
    XSFLIB_CHECK(r.U32(base + 0x3C) == 0x3F800000 && r.U32(base + 0x40) == 0 && r.U32(base + 0x58) == 0);

    // Program 2's range table: an entry for the gap from 36 to 47.
    XSFLIB_CHECK(r.U8(base + 0x5C) == 4 && r.U8(base + 0x5D) == 35 && r.U8(base + 0x5E) == 47);
    XSFLIB_CHECK(r.U8(base + 0x5F) == 59 && r.U8(base + 0x60) == 127 && Names(r, base + 0x64, 1, 0x84));
    XSFLIB_CHECK(Nothing(r, base + 0x6C) && Names(r, base + 0x74, 1, 0xB4) && Names(r, base + 0x7C, 1, 0xE4));
    XSFLIB_CHECK(r.U8(base + 0xB4 + 0xC) == 62 && r.U8(base + 0xE4 + 4) == 103);

    // Program 3's key table of one entry, and its velocity table.
    XSFLIB_CHECK(r.U8(base + 0x114) == 1 && r.U8(base + 0x115) == 127 && Names(r, base + 0x118, 2, 0x120));
    XSFLIB_CHECK(r.U8(base + 0x120) == 2 && r.U8(base + 0x121) == 63 && r.U8(base + 0x122) == 127);
    XSFLIB_CHECK(Names(r, base + 0x124, 1, 0x134) && Names(r, base + 0x12C, 1, 0x164));
    XSFLIB_CHECK(r.S32(base + 0x164) == 1 && r.U32(base + 0x164 + 0x10) == 0x3FD00000);

    // Program 4's index table from 36 to 52. The run of 50 to 52 names one region.
    XSFLIB_CHECK(r.U8(base + 0x194) == 36 && r.U8(base + 0x195) == 52 && r.U16(base + 0x196) == 0);
    XSFLIB_CHECK(Names(r, base + 0x198, 1, 0x220) && Names(r, base + 0x198 + 9 * 8, 1, 0x3D0));
    XSFLIB_CHECK(Nothing(r, base + 0x198 + 10 * 8) && Nothing(r, base + 0x198 + 13 * 8));
    XSFLIB_CHECK(Names(r, base + 0x198 + 14 * 8, 1, 0x400) && Names(r, base + 0x198 + 16 * 8, 1, 0x400));
    XSFLIB_CHECK(r.U8(base + 0x400 + 4) == 116 && r.U32(base + 0x400 + 0x10) == 0x40400000);
    XSFLIB_CHECK(r.U32(0x458) == 0 && r.U32(0x45C) == 0);

    // The waves go into an RWAR.
    const rsar::WaveArchiveFile archive = rsar::WaveArchiveFile::Read(files.wave_data);
    XSFLIB_CHECK(archive.waves.size() == 2 && WaveFile::Read(archive.waves[1]) == TwoWaves()[1]);

    // NW4R SND's choice of region, and the model read back.
    const rsar::Bank parsed = rsar::Bank::Parse(files.file, files.wave_data, PlaybackOptions());
    XSFLIB_CHECK(parsed.InstrumentCount() == 5 && parsed.Waves().size() == 2 && !parsed.HasWaveBlock());
    XSFLIB_CHECK(parsed.Find(0, 0, 1) && *parsed.Find(0, 0, 1) == Values(0) && !parsed.Find(1, 60, 60));
    XSFLIB_CHECK(*parsed.Find(2, 35, 1) == Values(1) && !parsed.Find(2, 36, 1) && !parsed.Find(2, 47, 127));
    XSFLIB_CHECK(*parsed.Find(2, 48, 1) == Values(2) && *parsed.Find(2, 127, 127) == Values(3));
    XSFLIB_CHECK(*parsed.Find(3, 10, 63) == Values(4) && *parsed.Find(3, 10, 64) == Values(5));
    XSFLIB_CHECK(!parsed.Find(4, 35, 64) && *parsed.Find(4, 45, 64) == Values(15) && !parsed.Find(4, 49, 64));
    XSFLIB_CHECK(*parsed.Find(4, 51, 64) == Values(16) && !parsed.Find(4, 53, 64));
    const BankFile read = BankFile::Read(files.file, files.wave_data);
    XSFLIB_CHECK(read == bank && read.Instruments() == FiveInstruments());
}

// A bank of version 1.1 describes its waves in WAVE. Each wave's samples start on 32 bytes of the wave data.
void TestOldVersion()
{
    const Instruments instruments = {BankFile::Instrument{{Keys(0, 127, 0)}}};
    const BankFile bank = BankFile::FromInstruments(0x0101, instruments, TwoWaves());
    const BankFile::Files files = bank.Write();
    const xsflib::Reader r(files.file, true);

    XSFLIB_CHECK(files.file.size() == 0x180 && r.U16(6) == 0x0101 && r.U16(0xE) == 2 && r.U32(0x10) == 0x20);
    XSFLIB_CHECK(r.U32(0x14) == 0x60 && r.U32(0x18) == 0x80 && r.U32(0x1C) == 0x100 && Names(r, 0x2C, 1, 0xC));
    XSFLIB_CHECK(r.Magic(0x80, "WAVE") && r.U32(0x84) == 0x100 && r.U32(0x88) == 2);
    XSFLIB_CHECK(Names(r, 0x8C, 0, 0x14) && Names(r, 0x94, 0, 0x80) && r.U8(0x9C) == 2 && r.U8(0x108) == 1);
    XSFLIB_CHECK(r.U32(0x9C + 0x14) == 0 && r.U32(0x108 + 0x14) == 32 && r.U32(0x108 + 0x24) == 0);
    XSFLIB_CHECK(r.U32(0x108 + 0x40) == 20 && r.U32(0x160) == 0 && r.U32(0x17C) == 0);
    XSFLIB_CHECK(files.wave_data.size() == 96 && files.wave_data[23] == 0x11 && files.wave_data[24] == 0);
    XSFLIB_CHECK(files.wave_data[32] == 0x22 && files.wave_data[52] == 0x33 && files.wave_data[72] == 0);

    const rsar::Bank parsed = rsar::Bank::Parse(files.file, files.wave_data, PlaybackOptions());
    XSFLIB_CHECK(parsed.HasWaveBlock() && parsed.Waves().size() == 2 && parsed.Waves()[0].loop_end == 30);
    XSFLIB_CHECK(parsed.Waves()[1].channels[1].data.data() == files.wave_data.data() + 52);
    XSFLIB_CHECK(BankFile::Read(files.file, files.wave_data) == bank);
}

// A tree in which several references name a node, and references of data type 4 name nodes: NW4R SND plays nothing
// for them. A node that only such references reach has kind kNone, and takes 4 zeros.
BankFile SharedTree()
{
    BankFile bank;
    bank.instruments = {{BankFile::kRangeTable, 0}, {BankFile::kRegion, 1}, {BankFile::kOtherNode, 3}, {}};
    bank.nodes.resize(4);
    bank.nodes[0].kind = BankFile::kRangeTable;
    bank.nodes[0].keys = {63, 127};
    bank.nodes[0].entries = {{BankFile::kOtherNode, 1}, {BankFile::kRegion, 2}};
    bank.nodes[1].region = Values(1);
    bank.nodes[2].region = Values(2);
    bank.nodes[3].kind = BankFile::kNone;
    bank.waves = TwoWaves();

    return bank;
}

void TestSharedTree()
{
    const BankFile bank = SharedTree();
    const BankFile::Files files = bank.Write();
    const xsflib::Reader r(files.file, true);
    const rsar::Bank parsed = rsar::Bank::Parse(files.file, files.wave_data, PlaybackOptions());
    const Instruments instruments = bank.Instruments();

    XSFLIB_CHECK(files.file.size() == 0xE0 && r.U32(0x14) == 0xC0 && Names(r, 0x2C, 2, 0x24));
    XSFLIB_CHECK(Names(r, 0x34, 1, 0x38) && Names(r, 0x3C, 4, 0x98) && Nothing(r, 0x44));
    XSFLIB_CHECK(Names(r, 0x28 + 0x28, 4, 0x38) && Names(r, 0x28 + 0x30, 1, 0x68));
    XSFLIB_CHECK(r.U8(0x28 + 0x38 + 4) == 101 && r.U8(0x28 + 0x68 + 4) == 102 && r.U32(0x28 + 0x98) == 0);
    XSFLIB_CHECK(!parsed.Find(0, 63, 64) && *parsed.Find(0, 64, 64) == Values(2) && *parsed.Find(1, 0, 0) == Values(1));
    XSFLIB_CHECK(!parsed.Find(2, 60, 60) && !parsed.Find(3, 60, 60));
    XSFLIB_CHECK(instruments.size() == 4 && !instruments[2] && !instruments[3]);
    XSFLIB_CHECK(instruments[0] == BankFile::Instrument{{Keys(64, 127, 2)}});
    XSFLIB_CHECK(instruments[1] == BankFile::Instrument{{Keys(0, 127, 1)}});
    XSFLIB_CHECK(BankFile::Read(files.file, files.wave_data) == bank);
}

template <class Error, class Call>
bool Throws(Call call, const std::string& part)
{
    const std::string message = ErrorOf<Error>(call);

    return !message.empty() && message.find(part) != std::string::npos;
}

// Write refuses a tree that Read wouldn't give back, and FromInstruments runs that overlap, don't follow their order,
// run backwards or pass 127. Read refuses other files and versions, and a bank that the tools didn't lay out.
void TestRefusals()
{
    std::vector<BankFile> banks(9, SharedTree());
    banks[0].version = 0x0103;
    banks[1].instruments[3] = {BankFile::kRegion, 9};
    banks[2].instruments[1] = {BankFile::kRangeTable, 1};
    std::swap(banks[3].nodes[1], banks[3].nodes[2]);
    banks[3].nodes[0].entries = {{BankFile::kOtherNode, 2}, {BankFile::kRegion, 1}};
    banks[3].instruments[1] = {BankFile::kRegion, 2};
    banks[4].nodes.push_back(banks[4].nodes[1]);
    banks[5].nodes[3].kind = BankFile::kRegion;
    banks[6].nodes[0].keys = {127};
    banks[7].nodes[1].entries = {{BankFile::kRegion, 2}};
    banks[8].nodes[0].kind = BankFile::kIndexTable;
    banks[8].nodes[0].keys = {64, 63};
    banks[8].instruments[0].type = BankFile::kIndexTable;
    const auto write = [&](std::size_t i)
    {
        return [&banks, i]
        {
            banks[i].Write();
        };
    };

    XSFLIB_CHECK(Throws<std::invalid_argument>(write(0), "a version other than 1.1 and 1.2"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(1), "to a node that the bank lacks"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(2), "doesn't match its node's kind"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(3), "don't follow the order"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(4), "a node that no reference names"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(5), "only references of data type 4 reach has a kind"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(6), "a range table's keys don't match its entries"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(7), "has keys or entries"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(write(8), "an index table's keys run backwards"));

    const auto from = [](BankFile::Instrument instrument)
    {
        return [instrument]
        {
            const Instruments instruments = {instrument};
            BankFile::FromInstruments(0x0102, instruments, {});
        };
    };
    BankFile::Instrument overlapping{{Keys(0, 63, 0), Keys(63, 127, 1)}};
    BankFile::Instrument unordered{{Keys(64, 127, 0), Keys(0, 63, 1)}};
    BankFile::Instrument backwards{{Keys(10, 5, 0)}};
    BankFile::Instrument past{{Keys(0, 200, 0)}};
    BankFile::Instrument velocities{{{0, 127, {{0, 64, Values(0)}, {64, 127, Values(1)}}}}};
    XSFLIB_CHECK(Throws<std::invalid_argument>(from(overlapping), "key regions overlap"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(from(unordered), "key regions overlap"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(from(backwards), "key regions overlap"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(from(past), "key regions overlap"));
    XSFLIB_CHECK(Throws<std::invalid_argument>(from(velocities), "velocity regions overlap"));

    const BankFile::Files files = SharedTree().Write();
    std::vector<uint8_t> newer = files.file;
    newer[7] = 3;
    std::vector<uint8_t> tail = files.file;
    tail[0x28 + 0x38 + 0x20] = 1;
    std::vector<uint8_t> padding = files.file;
    padding[0xDF] = 1;
    std::vector<uint8_t> type = files.file;
    type[0x34 + 1] = 5;
    const std::vector<uint8_t> wave = TwoWaves()[0].Write();
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { BankFile::Read(wave, files.wave_data); }, "not an RBNK file"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { BankFile::Read(newer, files.wave_data); }, "other than 1.1"));
    XSFLIB_CHECK(
        Throws<xsflib::FormatError>([&] { BankFile::Read(tail, files.wave_data); }, "other bytes of the file"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { BankFile::Read(padding, files.wave_data); }, "other bytes"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { BankFile::Read(type, files.wave_data); }, "of another kind"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { BankFile::Read(files.file, wave); }, "not an RWAR file"));
}

} // namespace

int main()
{
    TestLayout();
    TestOldVersion();
    TestSharedTree();
    TestRefusals();

    return xsflib::testing::Summary("Wii bank file");
}
