// SPDX-License-Identifier: MIT

// Tests of the readers of the DS's sound archives and their files, of the decoding of their waves, and of the
// archives' layout and of their loading into main memory (ntdlib/sdat/formats.h, ntdlib/sdat/decode.h,
// ntdlib/sdat/layout.h and ntdlib/sdat/heap.h), on files that testing/sdat_files.h builds. The expected samples of
// IMA-ADPCM follow GBATEK's description of the DS's decoder. The expected layout and addresses follow the rules in
// layout.h and heap.h, worked out by hand.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/testing/check.h"

#include "ntdlib/sdat/decode.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/heap.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace files = ntdlib::testing::sdat_files;
namespace sdat = ntdlib::sdat;
using files::Bytes;

void TestSequence()
{
    const Bytes bytecode = {0xFE, 0x01, 0x00, 0x3C, 0x7F, 0x30, 0xFF};
    const Bytes file = files::Sequence(bytecode);

    const sdat::Sequence sequence = sdat::Sequence::Parse(file);

    XSFLIB_CHECK(sequence.header.size == file.size());
    XSFLIB_CHECK(sequence.data_offset == 0x1C);
    XSFLIB_CHECK(Bytes(sequence.bytecode.begin(), sequence.bytecode.begin() + 7) == bytecode);
}

void TestSequenceArchive()
{
    const Bytes bytecode = {0x3C, 0x7F, 0x30, 0xFF, 0x40, 0x7F, 0x30, 0xFF};
    const Bytes file = files::SequenceArchive({{0, 3, 100, 64, 32, 1}, {4, 5, 90, 60, 30, 2}}, bytecode);

    const sdat::SequenceArchive archive = sdat::SequenceArchive::Parse(file);

    XSFLIB_CHECK(archive.sequences.size() == 2);
    XSFLIB_CHECK(archive.sequences[1].offset == 4 && archive.sequences[1].bank == 5);
    XSFLIB_CHECK(archive.sequences[1].volume == 90 && archive.sequences[1].player == 2);
    XSFLIB_CHECK(archive.bytecode[4] == 0x40);
}

void TestBank()
{
    files::Instrument pcm{1, {1, 7, 2, 65, 120, 100, 90, 80, 30}};
    files::Instrument psg{2, {2, 3, 0, 69}};
    files::Instrument drums{16};
    drums.min_key = 36;
    drums.regions = {{1, 1}, {3}, {2, 5}};
    files::Instrument split{17};
    split.split_keys = {40, 80, 127};
    split.regions = {{1, 4}, {1, 5, 1}, {2, 1}};
    const Bytes file = files::Bank({pcm, {}, psg, drums, split});

    const sdat::Bank bank = sdat::Bank::Parse(file);

    XSFLIB_CHECK(bank.instruments.size() == 5);
    XSFLIB_CHECK(bank.instruments[0].type == sdat::InstrumentType::kPcm);
    XSFLIB_CHECK(bank.instruments[0].region.wave == 7 && bank.instruments[0].region.wave_archive == 2);
    XSFLIB_CHECK(bank.instruments[0].region.root_key == 65 && bank.instruments[0].region.release == 80);
    XSFLIB_CHECK(bank.instruments[0].region.pan == 30);
    XSFLIB_CHECK(bank.instruments[1].type == sdat::InstrumentType::kNone);
    XSFLIB_CHECK(bank.instruments[2].type == sdat::InstrumentType::kPsg && bank.instruments[2].region.wave == 3);
    XSFLIB_CHECK(bank.instruments[3].min_key == 36 && bank.instruments[3].max_key == 38);
    XSFLIB_CHECK(bank.instruments[3].regions.size() == 3);
    XSFLIB_CHECK(bank.instruments[3].regions[1].type == sdat::InstrumentType::kNoise);
    XSFLIB_CHECK(bank.instruments[3].regions[2].wave == 5);
    XSFLIB_CHECK(bank.instruments[4].regions.size() == 3);
    XSFLIB_CHECK(bank.instruments[4].split_keys[1] == 80 && bank.instruments[4].split_keys[3] == 0);
    XSFLIB_CHECK(bank.instruments[4].regions[1].wave_archive == 1);
}

void TestWaves()
{
    // PCM8 plays each byte as the high byte of a sample. The loop's start and length count words.
    const files::Wave pcm8{0, 1, 22050, 1, 1, {0x01, 0x80, 0x7F, 0xFF, 0x10, 0x20, 0x30, 0x40}};
    const files::Wave pcm16{1, 0, 32768, 0, 2, {0x34, 0x12, 0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00}};
    // IMA-ADPCM: a header word with the sample 0x7FF0 and the index 10, then the nibbles 7, 7, 15 and 15. Index 10's
    // step is 19: nibble 7 adds 19/8 + 19/4 + 19/2 + 19 = 2 + 4 + 9 + 19 = 34, and the sum stops at 0x7FFF. The
    // index goes to 18 (step 41). The second 7 adds 76, the sum stays at 0x7FFF, and the index goes to 26 (step 88).
    // Nibble 15 then takes 11 + 22 + 44 + 88 = 165 away: 0x7F5A. The index goes to 34 (step 190), and the last nibble
    // takes 23 + 47 + 95 + 190 = 355 away: 0x7DF7.
    const files::Wave adpcm{2, 0, 32768, 1, 1, {0xF0, 0x7F, 10, 0, 0x77, 0xFF, 0x00, 0x00}};
    const Bytes swar = files::WaveArchive({pcm8, pcm16, adpcm});

    const Bytes swav = files::WaveFile(pcm16);

    const sdat::WaveArchive archive = sdat::WaveArchive::Parse(swar);
    const sdat::Wave single = sdat::Wave::ParseFile(swav);

    XSFLIB_CHECK(archive.waves.size() == 3);
    XSFLIB_CHECK(archive.waves[0].loop == 1 && archive.waves[0].sample_rate == 22050);
    XSFLIB_CHECK(archive.waves[0].timer == 16756991 / 22050);
    XSFLIB_CHECK(archive.waves[0].Samples() == 8 && archive.waves[0].LoopStartSample() == 4);
    XSFLIB_CHECK(sdat::DecodeWave(archive.waves[0]) ==
                 (std::vector<int16_t>{0x0100, -0x8000, 0x7F00, -0x0100, 0x1000, 0x2000, 0x3000, 0x4000}));
    XSFLIB_CHECK(sdat::DecodeWave(archive.waves[1]) == (std::vector<int16_t>{0x1234, -0x8000, 0x7FFF, 1}));
    XSFLIB_CHECK(sdat::DecodeWave(single) == sdat::DecodeWave(archive.waves[1]));
    XSFLIB_CHECK(archive.waves[2].Samples() == 8 && archive.waves[2].LoopStartSample() == 0);
    const std::vector<int16_t> decoded = sdat::DecodeWave(archive.waves[2]);
    XSFLIB_CHECK(decoded.size() == 8);
    XSFLIB_CHECK(decoded[0] == 0x7FFF && decoded[1] == 0x7FFF && decoded[2] == 0x7F5A && decoded[3] == 0x7DF7);
}

void TestStream()
{
    // Two channels of PCM16, in blocks of 4 bytes: the last block holds one sample of each channel.
    files::Stream spec;
    spec.channels = 2;
    spec.samples = 5;
    spec.blocks = 3;
    spec.block_bytes = 4;
    spec.block_samples = 2;
    spec.last_block_bytes = 2;
    spec.last_block_samples = 1;
    spec.data = {1, 0, 2, 0, 11, 0, 12, 0, 3, 0, 4, 0, 13, 0, 14, 0, 5, 0, 15, 0};

    const Bytes file = files::StreamFile(spec);

    const sdat::Stream stream = sdat::Stream::Parse(file);

    XSFLIB_CHECK(stream.channels == 2 && stream.blocks == 3 && stream.sample_rate == 32728 && stream.timer == 16);
    XSFLIB_CHECK(stream.Block(1, 2).size() == 2 && stream.Block(1, 2)[0] == 15);
    XSFLIB_CHECK(sdat::DecodeStream(stream, 0) == (std::vector<int16_t>{1, 2, 3, 4, 5}));
    XSFLIB_CHECK(sdat::DecodeStream(stream, 1) == (std::vector<int16_t>{11, 12, 13, 14, 15}));
}

files::Archive ArchiveSpec()
{
    files::Archive spec;
    spec.items[0] = {files::SequenceItem(0, 1, "SEQ_TITLE", 2), files::AbsentItem(), files::SequenceItem(1, 1, "")};
    spec.items[1] = {files::SequenceArchiveItem(2, "SSAR_SE", {"SE_A", "SE_B"})};
    spec.items[2] = {files::AbsentItem(), files::BankItem(3, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK_TITLE")};
    spec.items[3] = {files::FileItem(4 | 0x01000000, "WAVE_TITLE")};
    spec.items[4] = {files::PlayerItem(1, 0xFFF0, 0x8000, "PLAYER_BGM")};
    spec.files = {files::Sequence({0xFF}), files::Sequence({0xFE, 0x01, 0x00, 0xFF}),
                  files::SequenceArchive({{0}}, {0xFF}), files::Bank({}), files::WaveArchive({})};

    return spec;
}

void TestArchive()
{
    const Bytes file = files::SoundArchive(ArchiveSpec());

    const sdat::Archive archive = sdat::Archive::Parse(file);

    XSFLIB_CHECK(archive.has_symbols);
    XSFLIB_CHECK(archive.sequences.size() == 3 && archive.sequences[0] && !archive.sequences[1]);
    XSFLIB_CHECK(archive.sequences[0]->name == "SEQ_TITLE" && archive.sequences[0]->bank == 1);
    XSFLIB_CHECK(archive.sequences[0]->player == 2 && archive.sequences[0]->volume == 100);
    XSFLIB_CHECK(archive.sequences[2]->file_id == 1 && archive.sequences[2]->name.empty());
    XSFLIB_CHECK(archive.sequence_archives[0]->name == "SSAR_SE");
    XSFLIB_CHECK(archive.sequence_archives[0]->sequence_names == (std::vector<std::string>{"SE_A", "SE_B"}));
    XSFLIB_CHECK(!archive.banks[0] && archive.banks[1]->wave_archives[1] == 0xFFFF);
    XSFLIB_CHECK(archive.banks[1]->name == "BANK_TITLE");
    XSFLIB_CHECK(archive.wave_archives[0]->file_id == 0x01000004 && archive.wave_archives[0]->name == "WAVE_TITLE");
    XSFLIB_CHECK(archive.players[0]->channels == 0xFFF0 && archive.players[0]->heap_size == 0x8000);
    XSFLIB_CHECK(archive.groups.empty() && archive.streams.empty());
    XSFLIB_CHECK(archive.files.size() == 5 && archive.files[0].offset % 32 == 0);
    XSFLIB_CHECK(sdat::Sequence::Parse(archive.File(1)).bytecode[0] == 0xFE);
    XSFLIB_CHECK(sdat::Bank::Parse(archive.File(3)).instruments.empty());
}

void TestArchiveWithoutNames()
{
    files::Archive spec = ArchiveSpec();
    spec.symbols = false;

    const Bytes file = files::SoundArchive(spec);

    const sdat::Archive archive = sdat::Archive::Parse(file);

    XSFLIB_CHECK(!archive.has_symbols);
    XSFLIB_CHECK(archive.sequences[0]->name.empty() && archive.sequences[0]->player == 2);
    XSFLIB_CHECK(archive.sequence_archives[0]->sequence_names.empty());
}

// FindArchives finds each archive that reads, at the file's start or at a multiple of 4 bytes inside it. A mark that
// starts no archive, or that lies off a multiple of 4, finds nothing.
void TestFindArchives()
{
    const Bytes archive = files::SoundArchive(ArchiveSpec());
    Bytes file = {'S', 'D', 'A', 'T', 0, 0, 0, 0};
    file.insert(file.end(), archive.begin(), archive.end());
    files::Align(file, 4);
    file.insert(file.end(), {'x', 'S', 'D', 'A', 'T', 0, 0, 0});
    const std::size_t second = file.size();
    file.insert(file.end(), archive.begin(), archive.end());

    const std::vector<sdat::Archive> found = sdat::FindArchives(file);

    XSFLIB_CHECK(found.size() == 2);
    XSFLIB_CHECK(found.size() == 2 && found[0].bytes.data() == file.data() + 8 &&
                 found[1].bytes.data() == file.data() + second);
    XSFLIB_CHECK(found.size() == 2 && found[1].sequences[0]->name == "SEQ_TITLE");
    XSFLIB_CHECK(sdat::FindArchives(archive).size() == 1 && sdat::FindArchives(Bytes{}).empty());
}

void TestRefusals()
{
    Bytes bad_mark = files::Sequence({0xFF});
    bad_mark[4] = 0xFE;
    Bytes cut = files::SoundArchive(ArchiveSpec());
    cut.resize(cut.size() - 1);
    Bytes no_info = files::SoundArchive(ArchiveSpec());
    no_info[xsflib::LoadLe32(&no_info[0x18])] = 'X';
    Bytes far_file = files::SoundArchive(ArchiveSpec());
    xsflib::StoreLe32(&far_file[xsflib::LoadLe32(&far_file[0x20]) + 12 + 4], 0x100000);

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sdat::Bank::Parse(files::Sequence({0xFF})); }) ==
                 "not an SBNK file");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sdat::Sequence::Parse(bad_mark); }) ==
                 "the SSEQ file's byte order mark isn't 0xFEFF");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sdat::Archive::Parse(cut); }) == "the SDAT file is cut short");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sdat::Archive::Parse(no_info); }) ==
                 "the SDAT file has no INFO or FAT block");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sdat::Archive::Parse(far_file); }) ==
                 "a file lies past the end of the file");
}

// An archive of every kind of record but players and streams, with a name missing and a sequence archive's names.
sdat::ArchiveModel LayoutModel()
{
    sdat::ArchiveModel model;
    sdat::Symbols symbols;
    symbols.names[0] = {"SEQ_A", std::nullopt};
    symbols.names[1] = {"ARC"};
    symbols.sequence_names = {std::vector<std::optional<std::string>>{"S0", "S1"}};
    symbols.names[2] = {"BANK"};
    model.symbols = symbols;
    model.sequences = {sdat::SequenceInfo{0, 0, 100, 64, 32, 0, 0, ""}, std::nullopt};
    model.sequence_archives = {sdat::SequenceArchiveInfo{1, "", {}}};
    model.banks = {sdat::BankInfo{2, {0, 0xFFFF, 0xFFFF, 0xFFFF}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{3, ""}};
    model.groups = {sdat::GroupInfo{{{0, 1, 0, 0}}, ""}};
    model.files = {Bytes(4, 1), Bytes(8, 2), Bytes(12, 3), Bytes(16, 4)};
    return model;
}

void TestLayout()
{
    const sdat::ArchiveModel model = LayoutModel();
    const Bytes bytes = sdat::BuildArchive(model);
    const xsflib::Reader r(bytes);

    // SYMB at 0x40: its lists from 0x40 in the block, then the names from 0x80: "SEQ_A", "ARC", "S0", "S1" and
    // "BANK", up to 0x95. The block's size counts the zeros up to 0x98. INFO follows at 0xD8, with its lists and
    // records up to 0xA4. FAT takes 12 + 4 * 16 bytes from 0x17C, and FILE starts at 0x1C8. Its files lie on
    // multiples of 32 from 0x1E0, and the archive ends at 0x260.
    XSFLIB_CHECK(bytes.size() == 0x260);
    XSFLIB_CHECK(r.U32(8) == 0x260 && r.U16(12) == 0x40 && r.U16(14) == 4);
    XSFLIB_CHECK(r.U32(0x10) == 0x40 && r.U32(0x14) == 0x95);
    XSFLIB_CHECK(r.U32(0x18) == 0xD8 && r.U32(0x1C) == 0xA4);
    XSFLIB_CHECK(r.U32(0x20) == 0x17C && r.U32(0x24) == 0x4C);
    XSFLIB_CHECK(r.U32(0x28) == 0x1C8 && r.U32(0x2C) == 0x98);
    XSFLIB_CHECK(r.Magic(0x40, "SYMB") && r.U32(0x44) == 0x98);

    // SYMB's lists: the sequences at 0x40, the sequence archives at 0x4C with the archive's list at 0x58, the banks
    // at 0x64, and the empty lists from 0x6C.
    const std::vector<uint32_t> symb_lists = {0x40, 0x4C, 0x64, 0x6C, 0x70, 0x74, 0x78, 0x7C};
    for (uint32_t kind = 0; kind < 8; kind++)
    {
        XSFLIB_CHECK(r.U32(0x48 + 4 * kind) == symb_lists[kind]);
    }
    XSFLIB_CHECK(r.U32(0x80) == 2 && r.U32(0x84) == 0x80 && r.U32(0x88) == 0);
    XSFLIB_CHECK(r.U32(0x8C) == 1 && r.U32(0x90) == 0x86 && r.U32(0x94) == 0x58);
    XSFLIB_CHECK(r.U32(0x98) == 2 && r.U32(0x9C) == 0x8A && r.U32(0xA0) == 0x8D);
    XSFLIB_CHECK(r.U32(0xA4) == 1 && r.U32(0xA8) == 0x90);
    XSFLIB_CHECK(r.CString(0x40 + 0x8D, 8) == "S1" && r.CString(0x40 + 0x90, 8) == "BANK");
    XSFLIB_CHECK(r.U8(0x40 + 0x95) == 0 && r.U8(0x40 + 0x97) == 0);

    // INFO's lists, each followed by its records.
    const std::vector<uint32_t> info_lists = {0x40, 0x58, 0x64, 0x78, 0x84, 0x88, 0x9C, 0xA0};
    for (uint32_t kind = 0; kind < 8; kind++)
    {
        XSFLIB_CHECK(r.U32(0xD8 + 8 + 4 * kind) == info_lists[kind]);
    }
    XSFLIB_CHECK(r.U32(0xD8 + 0x40) == 2 && r.U32(0xD8 + 0x44) == 0x4C && r.U32(0xD8 + 0x48) == 0);
    XSFLIB_CHECK(r.U8(0xD8 + 0x4C + 6) == 100 && r.U8(0xD8 + 0x4C + 7) == 64);
    XSFLIB_CHECK(r.U32(0xD8 + 0x5C) == 0x60 && r.U32(0xD8 + 0x60) == 1);
    XSFLIB_CHECK(r.U32(0xD8 + 0x68) == 0x6C && r.U16(0xD8 + 0x6C + 6) == 0xFFFF);
    XSFLIB_CHECK(r.U32(0xD8 + 0x8C) == 0x90 && r.U32(0xD8 + 0x90) == 1 && r.U8(0xD8 + 0x95) == 1);

    // FAT and FILE.
    XSFLIB_CHECK(r.U32(0x17C + 8) == 4);
    const std::vector<uint32_t> offsets = {0x1E0, 0x200, 0x220, 0x240};
    for (uint32_t id = 0; id < 4; id++)
    {
        XSFLIB_CHECK(r.U32(0x17C + 12 + 16 * id) == offsets[id] && r.U32(0x17C + 16 + 16 * id) == 4 * (id + 1));
        XSFLIB_CHECK(r.U8(offsets[id]) == id + 1);
    }
    XSFLIB_CHECK(r.Magic(0x1C8, "FILE") && r.U32(0x1CC) == 0x98 && r.U32(0x1D0) == 4);

    // The model reads back.
    const sdat::ArchiveModel read = sdat::ReadArchiveModel(bytes);
    XSFLIB_CHECK(read.symbols == model.symbols);
    XSFLIB_CHECK(read.files == model.files);
    XSFLIB_CHECK(read.sequences.size() == 2 && read.sequences[0]->volume == 100 && !read.sequences[1]);
    XSFLIB_CHECK(read.groups.size() == 1 && read.groups[0]->items == model.groups[0]->items);
}

void TestLayoutWithoutNames()
{
    sdat::ArchiveModel model = LayoutModel();
    model.symbols.reset();
    const Bytes bytes = sdat::BuildArchive(model);
    const xsflib::Reader r(bytes);

    // Without SYMB, the header gives 3 blocks and SYMB's offset and size are 0. INFO starts at 0x40.
    XSFLIB_CHECK(r.U16(14) == 3 && r.U32(0x10) == 0 && r.U32(0x14) == 0);
    XSFLIB_CHECK(r.U32(0x18) == 0x40 && r.U32(0x1C) == 0xA4);
    XSFLIB_CHECK(!sdat::ReadArchiveModel(bytes).symbols);

    // An archive laid out otherwise is refused: here, a file moved off its multiple of 32.
    Bytes moved = bytes;
    const uint32_t fat = r.U32(0x20);
    moved[fat + 12] = static_cast<uint8_t>(moved[fat + 12] + 4);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { sdat::ReadArchiveModel(moved); })
                     .find("isn't laid out as the SDK's tools lay it out") != std::string::npos);
}

uint32_t Word(const Bytes& memory, uint32_t address)
{
    return xsflib::Reader(memory).U32(address - 0x02000000);
}

// A bank that plays waves of a wave archive that loads its waves one at a time (slot 0) and of one that loads whole
// (slot 1). Slot 3 names a wave archive that the archive lacks. The second bank plays waves 1 and 3 of the first
// archive. The third and fourth banks share the first and second banks' files, with the whole wave archive in slot 0.
files::Archive HeapSpec()
{
    files::Archive spec;
    const files::Region wave_2{.wave = 2};
    spec.files.push_back(files::Bank({
        {.type = 1, .region = wave_2},
        {.type = 16, .min_key = 36, .regions = {{.wave = 0}, {.type = 2, .wave = 1}}},
        {.type = 17, .split_keys = {60, 127}, .regions = {wave_2, {.wave = 0, .wave_archive = 1}}},
        {.type = 1, .region = {.wave = 5}},
    }));
    spec.files.push_back(files::WaveArchive(
        {{.data = Bytes(4, 0x10)}, {.data = Bytes(8, 0x11)}, {.data = Bytes(12, 0x12)}, {.data = Bytes(16, 0x13)}}));
    spec.files.push_back(files::WaveArchive({{.data = Bytes(4, 0x20)}}));
    spec.files.push_back(files::Bank({{.type = 1, .region = {.wave = 1}}, {.type = 1, .region = {.wave = 3}}}));
    spec.items[2] = {files::BankItem(0, {0, 1, 0xFFFF, 7}, "BANK_A"),
                     files::BankItem(3, {0, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK_B"),
                     files::BankItem(0, {1, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK_C"),
                     files::BankItem(3, {1, 0xFFFF, 0xFFFF, 0xFFFF}, "BANK_D")};
    spec.items[3] = {files::FileItem(0x01000001, "WAVE_SINGLE"), files::FileItem(2, "WAVE_WHOLE")};
    return spec;
}

void TestHeap()
{
    const Bytes file = files::SoundArchive(HeapSpec());
    const sdat::Archive archive = sdat::Archive::Parse(file);
    Bytes memory(0x10000, 0);
    sdat::SoundHeap heap(memory, 0x02000000, 0x02000100, 0x02010000);

    // The bank, then the single archive's header with its table twice, 0x3C + 2 * 16 bytes. The waves follow in the
    // order of the instruments: wave 2 (24 bytes) and the drum set's wave 0 (16 bytes). The drum set's PSG region, the
    // key split's wave of slot 1, the second use of wave 2 and wave 5, past the archive's 4, load nothing. The whole
    // wave archive comes last.
    const uint32_t bank = heap.LoadBank(archive, 0);
    XSFLIB_CHECK(bank == 0x02000100);
    const uint32_t table = (bank + static_cast<uint32_t>(archive.File(0).size()) + 31) & ~31u;
    const uint32_t whole = table + 0xA0;
    XSFLIB_CHECK(heap.End() == whole + ((archive.File(2).size() + 31) & ~std::size_t{31}));
    XSFLIB_CHECK(Word(memory, table + 0x38) == 4);
    XSFLIB_CHECK(Word(memory, table + 0x3C) == table + 0x80 && Word(memory, table + 0x40) == 0);
    XSFLIB_CHECK(Word(memory, table + 0x44) == table + 0x60 && Word(memory, table + 0x48) == 0);
    const xsflib::Reader single(archive.File(1));
    for (uint32_t wave = 0; wave < 4; wave++)
    {
        XSFLIB_CHECK(Word(memory, table + 0x4C + 4 * wave) == single.U32(0x3C + 4 * wave));
    }
    XSFLIB_CHECK(memory[table + 0x60 - 0x02000000 + 12] == 0x12 && memory[table + 0x60 - 0x02000000 + 23] == 0x12);
    XSFLIB_CHECK(memory[table + 0x80 - 0x02000000 + 12] == 0x10);
    XSFLIB_CHECK(Word(memory, whole + 0x3C) == 0x40 && memory[whole + 0x40 - 0x02000000 + 12] == 0x20);

    // The links: each wave archive's first link is the bank's. Slots 2 and 3 stay empty.
    XSFLIB_CHECK(Word(memory, bank + 0x18) == table && Word(memory, bank + 0x1C) == 0);
    XSFLIB_CHECK(Word(memory, table + 0x18) == bank + 0x18);
    XSFLIB_CHECK(Word(memory, bank + 0x20) == whole && Word(memory, whole + 0x18) == bank + 0x20);
    XSFLIB_CHECK(Word(memory, bank + 0x28) == 0 && Word(memory, bank + 0x30) == 0);

    // A bank loads once.
    const Bytes before = memory;
    XSFLIB_CHECK(heap.LoadBank(archive, 0) == bank && memory == before);

    // The second bank shares the table, takes the first place in its list of links, and loads waves 1 and 3. The last
    // wave runs to the end of its file.
    const uint32_t end = heap.End();
    const uint32_t second = heap.LoadBank(archive, 1);
    const uint32_t wave_1 = (second + static_cast<uint32_t>(archive.File(3).size()) + 31) & ~31u;
    XSFLIB_CHECK(second == end && Word(memory, second + 0x18) == table && Word(memory, second + 0x1C) == bank + 0x18);
    XSFLIB_CHECK(Word(memory, table + 0x18) == second + 0x18);
    XSFLIB_CHECK(Word(memory, table + 0x40) == wave_1 && Word(memory, table + 0x48) == wave_1 + 0x20);
    XSFLIB_CHECK(heap.End() == wave_1 + 0x40 && memory[wave_1 + 0x20 - 0x02000000 + 27] == 0x13);

    // A bank loaded again keeps its place in the lists.
    const Bytes linked = memory;
    XSFLIB_CHECK(heap.LoadBank(archive, 0) == bank && memory == linked);

    // The third bank moves the first bank's slot 0 from the end of the table's list to the start of the whole wave
    // archive's. Slot 1 keeps its link.
    XSFLIB_CHECK(heap.LoadBank(archive, 2) == bank && heap.End() == wave_1 + 0x40);
    XSFLIB_CHECK(Word(memory, table + 0x18) == second + 0x18 && Word(memory, second + 0x1C) == 0);
    XSFLIB_CHECK(Word(memory, bank + 0x18) == whole && Word(memory, bank + 0x1C) == bank + 0x20);
    XSFLIB_CHECK(Word(memory, whole + 0x18) == bank + 0x18 && Word(memory, bank + 0x20) == whole);

    // The fourth bank moves the second bank's slot 0 from the start of the table's list. The table's list is then
    // empty.
    XSFLIB_CHECK(heap.LoadBank(archive, 3) == second);
    XSFLIB_CHECK(Word(memory, table + 0x18) == 0 && Word(memory, second + 0x18) == whole);
    XSFLIB_CHECK(Word(memory, second + 0x1C) == bank + 0x18 && Word(memory, whole + 0x18) == second + 0x18);

    // The first bank again moves its slot 0 back to the table, from the middle of the whole wave archive's list.
    XSFLIB_CHECK(heap.LoadBank(archive, 0) == bank && heap.End() == wave_1 + 0x40);
    XSFLIB_CHECK(Word(memory, bank + 0x18) == table && Word(memory, bank + 0x1C) == 0);
    XSFLIB_CHECK(Word(memory, table + 0x18) == bank + 0x18 && Word(memory, second + 0x1C) == bank + 0x20);
    XSFLIB_CHECK(Word(memory, whole + 0x18) == second + 0x18);

    // Files that don't fit, and a bank that the archive lacks.
    Bytes small(0x1000, 0);
    sdat::SoundHeap tight(small, 0x02000000, 0x02000100, 0x02000200);
    XSFLIB_CHECK(ErrorOf<std::runtime_error>([&] { tight.LoadBank(archive, 0); }) ==
                 "the files don't fit in the sound heap");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { heap.LoadBank(archive, 4); }) == "the archive has no bank 4");
}

} // namespace

int main()
{
    TestSequence();
    TestSequenceArchive();
    TestBank();
    TestWaves();
    TestStream();
    TestArchive();
    TestArchiveWithoutNames();
    TestFindArchives();
    TestRefusals();
    TestLayout();
    TestLayoutWithoutNames();
    TestHeap();

    return xsflib::testing::Summary("DS sound archive");
}
