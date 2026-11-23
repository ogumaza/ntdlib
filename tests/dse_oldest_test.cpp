// SPDX-License-Identifier: MIT

// Tests of DSE's files of version 0x402 (ntdlib/dse/oldest.h). Each test builds a file by hand in the layout that
// oldest.h describes. OldestFile has to give the file back byte for byte. Upgraded has to put each field where
// formats.h reads it in version 0x415, and keep a song's 16 bytes of settings at 0x20 of the new settings. OldestBank
// has to give a bank back byte for byte, and write each edited value at the place of version 0x402 that oldest.h gives
// for it. The cut and the join of a set (ntdlib/dse/layout.h) have to give a song back byte for byte, and to keep its
// 16 bytes of settings when its tracks grow.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/dse/files.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/layout.h"
#include "ntdlib/dse/oldest.h"
#include "ntdlib/dse/sets.h"
#include "ntdlib/testing/dse_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace dse_files = ntdlib::testing::dse_files;
namespace dse = ntdlib::dse;
using dse_files::Builder;
using dse_files::Bytes;
using dse_files::TrackChunk;

// The start of a header of version 0x402: the magic word, a size that the caller fills, the version, 8 zeros, a date
// and a name with the tools' bytes after its zero.
void OldHeaderStart(Builder& b, const char* magic)
{
    b.Text(magic).U32(0).U32(0).U16(0x0402).U16(0x0102).Fill(8, 0);
    b.Append({0xD6, 0x07, 0x09, 0x1A, 0x12, 0x11, 0x1A, 0x05});
    b.Text("B_TEST").Fill(1, 0).Fill(9, 0xAA);
}

// A bank's chunk header of version 0x402: the label, 8 bytes that give the version and an alignment of 16, the size.
void OldBankChunk(Builder& b, const char* label, uint32_t size)
{
    b.Text(label).Append({0x00, 0x00, 0x02, 0x04, 0x10, 0x00, 0x00, 0x00}).U32(size);
}

// A song's 16 bytes of settings: 48 ticks to a quarter note, 2 tracks, 2 channels, byte 6 of 1, the global volume's
// index 2 in the high bits of byte 7, and a priority of 8.
const Bytes kOldSettings = {0x01, 0x00, 0x30, 0x00, 0x02, 0x02, 0x01, 0x20,
                            0x08, 0x00, 0x7F, 0x40, 0x00, 0x00, 0x00, 0x0F};

// A song with two tracks of 7 bytes, each padded with one byte. Track 0 sets the tempo and track 1 its volume.
Bytes OldSongFile()
{
    Builder b;
    OldHeaderStart(b, "smdl");
    b.U32(1).U32(1).U32(0xFFFFFFFF).U32(0xFFFFFFFF);
    b.Text("song").Append({0x00, 0x00, 0x00, 0x01, 0x10, 0xFF, 0x00, 0x00, 0xB0, 0xFF, 0xFF, 0xFF});
    b.Append(kOldSettings);
    TrackChunk(b, "trk ", 7).Append({0, 0, 0, 0, 0xA4, 0x78, 0x98}).Fill(1, 0x98);
    TrackChunk(b, "trk ", 7).Append({1, 1, 0, 0, 0xE0, 0x7F, 0x98}).Fill(1, 0x98);
    TrackChunk(b, "eoc ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));

    return b.Take();
}

void TestOldestSong()
{
    const Bytes file = OldSongFile();
    XSFLIB_CHECK(dse::IsOldestVersion(file));
    const dse::OldestFile parsed = dse::OldestFile::Parse(file);
    XSFLIB_CHECK(parsed.chunks.size() == 4);
    XSFLIB_CHECK(parsed.Write() == file);

    // A header that gives 0 for the size keeps it.
    Bytes no_size = file;
    xsflib::StoreLe32(no_size.data() + 0x08, 0);
    XSFLIB_CHECK(dse::OldestFile::Parse(no_size).Write() == no_size);

    const Bytes upgraded = dse::Upgraded(file);
    XSFLIB_CHECK(!dse::IsOldestVersion(upgraded));
    XSFLIB_CHECK(xsflib::LoadLe16(upgraded.data() + 0x0C) == dse::kVersion);
    XSFLIB_CHECK(xsflib::LoadLe32(upgraded.data() + 0x08) == upgraded.size());

    const dse::Song song = dse::Song::Parse(file);
    XSFLIB_CHECK(song == dse::Song::Parse(upgraded));
    XSFLIB_CHECK(song.info.TicksPerQuarter() == 48);
    XSFLIB_CHECK(song.info.TrackCount() == 2 && song.info.ChannelCount() == 2);
    const auto& info = song.info.bytes;
    XSFLIB_CHECK(info[0] == 0x01 && info[4] == 0x01 && info[0x19] == 2 && info[0x1B] == 8);
    XSFLIB_CHECK(std::equal(kOldSettings.begin(), kOldSettings.end(), info.begin() + 0x20));
    XSFLIB_CHECK(song.tracks.size() == 2);
    if (song.tracks.size() == 2)
    {
        XSFLIB_CHECK((song.tracks[0].events == std::vector<dse::Event>{{0xA4, {0x78}}, {0x98, {}}}));
        XSFLIB_CHECK(song.tracks[1].number == 1 && song.tracks[1].channel == 1);
        XSFLIB_CHECK((song.tracks[1].events == std::vector<dse::Event>{{0xE0, {0x7F}}, {0x98, {}}}));
    }
}

// A wave's entry of version 0x402 with `id`: IMA-ADPCM that loops, at 32,728 Hz. A short entry stops before the
// envelope at 0x30.
Bytes OldWave(uint8_t id, bool is_short)
{
    Builder b;
    b.Append({0x01, id, 0x00, 0x00, 0x3C, 0x00, 0x7F, 0x40, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    b.Append({0x02, 0x01}).U16(32728).U32(0x10 * id).U32(0xBE).U32(0x26B).Fill(16, 0);
    if (!is_short)
    {
        b.Append({0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x7F, 0x28, 0xFF});
    }

    return b.Take();
}

const Bytes kSplitEnvelope = {0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF,
                              0x00, 0x00, 0x00, 0x7F, 0x45, 0x5F, 0x37, 0xFF};

// A split of version 0x402 with `index`: keys from 32 * index, wave 2 * index, a fine tuning of -5, the root key 60, a
// coarse tuning of -2, a volume of 127 less the index, the centre pan and key group 1.
Bytes OldSplit(uint8_t index)
{
    const auto low = static_cast<uint8_t>(32 * index);
    const auto high = static_cast<uint8_t>(32 * index + 31);
    Builder b;
    b.Append({0x00, index, 0x02, 0x01, low, high, low, high, 0x00, 0x7F, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00});
    b.Append({0x00, static_cast<uint8_t>(2 * index)}).U16(0xFFFB);
    b.Append({60, 0xFE, static_cast<uint8_t>(0x7F - index), 0x40, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    b.Append(kSplitEnvelope);

    return b.Take();
}

// A wave bank with three wave slots (wave 1 left out), two program slots (program 0 left out), one key group and 5
// bytes of samples. Program 1 has 4 LFOs and two splits. The waves' entries take 48 bytes when `short_waves` is set.
Bytes OldBankFile(bool short_waves)
{
    const std::size_t entry = short_waves ? 0x30 : 0x40;
    Builder b;
    OldHeaderStart(b, "swdl");
    b.Append({0x00, 0xFF, 0xFF, 0xFF}).Fill(8, 0).U32(0x10);
    b.U32(0).U16(0).Append({3, 2, 1}).Fill(7, 0xFF);

    OldBankChunk(b, "wavi", static_cast<uint32_t>(16 + 2 * entry));
    b.U16(16).U16(0).U16(static_cast<uint32_t>(16 + entry)).Fill(10, 0);
    b.Append(OldWave(0, short_waves)).Append(OldWave(2, short_waves));

    OldBankChunk(b, "prgi", 16 + 0x10 + 0x80 + 0x40 + 0x60);
    b.U16(0).U16(16).Fill(12, 0);
    b.Append({0x01, 0x02, 0x00, 0x00, 0x7F, 0x40, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00});
    for (int i = 0; i < 16; i++)
    {
        b.Append({0x00, 0x00, 0xFF, 0x08, 0x00, 0x0F, 0x00, 0x00});
    }
    for (uint8_t lfo = 0; lfo < 4; lfo++)
    {
        b.Fill(16, static_cast<uint8_t>(0x10 + lfo));
    }
    b.Append(OldSplit(0)).Append(OldSplit(1));

    OldBankChunk(b, "kgrp", 8);
    b.Append({0x00, 0x00, 0xFF, 0x08, 0x00, 0xFF, 0x00, 0x00});
    b.Append({0xC1, 0xC7, 0x40, 0x00, 0x38, 0x30, 0xEC, 0x03});

    OldBankChunk(b, "pcmd", 5);
    b.Append({1, 2, 3, 4, 5}).Fill(11, 0);
    OldBankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));

    return b.Take();
}

void CheckWave(const std::optional<dse::WaveInfo>& wave, uint16_t id, bool is_short)
{
    XSFLIB_CHECK(wave.has_value());
    if (!wave)
    {
        return;
    }

    XSFLIB_CHECK(wave->Id() == id && wave->Format() == 0x200 && wave->Loops());
    XSFLIB_CHECK(wave->SampleRate() == 32728 && wave->SamplePosition() == 0x10U * id);
    XSFLIB_CHECK(wave->LoopStart() == 0xBE && wave->LoopLength() == 0x26B);
    XSFLIB_CHECK(wave->bytes[0] == 0x01 && wave->bytes[6] == 0x3C && wave->bytes[8] == 0x7F && wave->bytes[9] == 0x40);
    const std::span<const uint8_t> envelope = std::span(wave->bytes).subspan(0x30);
    if (is_short)
    {
        XSFLIB_CHECK(std::ranges::all_of(envelope, [](uint8_t byte) { return byte == 0; }));
    }
    else
    {
        XSFLIB_CHECK(envelope[0] == 0x01 && envelope[11] == 0x7F && envelope[14] == 0x28);
    }
}

void TestOldestBank(bool short_waves)
{
    const Bytes file = OldBankFile(short_waves);
    XSFLIB_CHECK(dse::IsOldestVersion(file));
    XSFLIB_CHECK(dse::OldestFile::Parse(file).Write() == file);

    const Bytes upgraded = dse::Upgraded(file);
    XSFLIB_CHECK(xsflib::LoadLe16(upgraded.data() + 0x0C) == dse::kVersion);
    XSFLIB_CHECK(xsflib::LoadLe16(upgraded.data() + 0x50 + 6) == dse::kVersion);

    const dse::WaveBank bank = dse::WaveBank::Parse(file);
    XSFLIB_CHECK(bank == dse::WaveBank::Parse(upgraded));
    XSFLIB_CHECK(bank.waves.size() == 3);
    if (bank.waves.size() == 3)
    {
        CheckWave(bank.waves[0], 0, short_waves);
        XSFLIB_CHECK(!bank.waves[1]);
        CheckWave(bank.waves[2], 2, short_waves);
    }

    XSFLIB_CHECK(bank.instruments.has_value());
    if (!bank.instruments || bank.instruments->programs.size() != 2 || !bank.instruments->programs[1])
    {
        XSFLIB_CHECK(false);
        return;
    }

    const dse::Instruments& instruments = *bank.instruments;
    XSFLIB_CHECK(!instruments.programs[0]);
    const dse::Program& program = *instruments.programs[1];
    XSFLIB_CHECK(program.Id() == 1 && program.header[4] == 0x7F && program.header[5] == 0x40);
    XSFLIB_CHECK(program.lfos.size() == 4 && program.splits.size() == 2);
    for (std::size_t lfo = 0; lfo < program.lfos.size(); lfo++)
    {
        XSFLIB_CHECK(std::ranges::all_of(program.lfos[lfo], [&](uint8_t byte) { return byte == 0x10 + lfo; }));
    }
    XSFLIB_CHECK(std::ranges::all_of(program.filler, [](uint8_t byte) { return byte == 0; }));
    if (program.splits.size() == 2)
    {
        const dse::SplitView split(std::span<const uint8_t, 0x30>(program.splits[1]));
        XSFLIB_CHECK(split.Index() == 1 && split.BendRange() == 2 && split.LowKey() == 32 && split.HighKey() == 63);
        XSFLIB_CHECK(split.LowVelocity() == 0 && split.HighVelocity() == 0x7F);
        XSFLIB_CHECK(split.Wave() == 2 && split.Tuning() == -5 && split.RootKey() == 60 && split.CoarseTuning() == -2);
        XSFLIB_CHECK(split.Volume() == 0x7E && split.Pan() == 0x40 && split.KeyGroup() == 1);
        XSFLIB_CHECK(std::equal(kSplitEnvelope.begin(), kSplitEnvelope.end(),
                                split.Bytes().begin() + dse::SplitView::kEnvelope));
    }
    XSFLIB_CHECK(instruments.key_groups.size() == 1);
    XSFLIB_CHECK((instruments.after_key_groups == Bytes{0xC1, 0xC7, 0x40, 0x00, 0x38, 0x30, 0xEC, 0x03}));
    XSFLIB_CHECK((bank.samples == Bytes{1, 2, 3, 4, 5}));

    // A header that gives 0 for the size, as some of the game's banks do, still reads.
    Bytes no_size = file;
    xsflib::StoreLe32(no_size.data() + 0x08, 0);
    XSFLIB_CHECK(dse::OldestFile::Parse(no_size).Write() == no_size);
    XSFLIB_CHECK(dse::WaveBank::Parse(no_size) == bank);
}

void TestOldestRefusals()
{
    const Bytes song = OldSongFile();

    // Without its eoc chunk, and with bytes after it.
    Bytes cut(song.begin(), song.end() - 16);
    xsflib::StoreLe32(cut.data() + 0x08, static_cast<uint32_t>(cut.size()));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::OldestFile::Parse(cut); }) ==
                 "the file doesn't end with its end chunk");

    Bytes longer = song;
    longer.insert(longer.end(), 16, 0);
    xsflib::StoreLe32(longer.data() + 0x08, static_cast<uint32_t>(longer.size()));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::OldestFile::Parse(longer); }) ==
                 "the file holds bytes after its end chunk");

    // A file of version 0x415 isn't one of version 0x402.
    Bytes newer = song;
    xsflib::StoreLe16(newer.data() + 0x0C, dse::kVersion);
    XSFLIB_CHECK(!dse::IsOldestVersion(newer));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::OldestFile::Parse(newer); }) ==
                 "not a DSE file of version 0x402");

    // The wave table's slot 2 names an entry past the wavi chunk.
    Bytes bank = OldBankFile(false);
    xsflib::StoreLe16(bank.data() + 0x60 + 4, 0x100);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::Upgraded(bank); }) == "a table names an entry past its chunk");

    // A bank of sound effects reads byte for byte, but has no layout of version 0x415.
    Builder b;
    OldHeaderStart(b, "sedl");
    b.Fill(16, 0xFF);
    TrackChunk(b, "eoc ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    const Bytes effects = b.Take();
    XSFLIB_CHECK(dse::OldestFile::Parse(effects).Write() == effects);
    XSFLIB_CHECK(!ErrorOf<dse::UnsupportedLayout>([&] { dse::Upgraded(effects); }).empty());
}

// A pack of a game's making: a header of 16 bytes, a song and a bank whose headers give a size of 0, and 32 bytes after
// them. OldestFileSize ends each file with its end chunk, and FindFiles gives the two files byte for byte.
void TestOldestInPack()
{
    Bytes song = OldSongFile();
    Bytes bank = OldBankFile(false);
    xsflib::StoreLe32(song.data() + 0x08, 0);
    xsflib::StoreLe32(bank.data() + 0x08, 0);
    Bytes pack(16, 0x5A);
    pack.insert(pack.end(), song.begin(), song.end());
    pack.insert(pack.end(), bank.begin(), bank.end());
    pack.insert(pack.end(), 32, 0xFF);

    const std::span<const uint8_t> bytes(pack);
    XSFLIB_CHECK(dse::OldestFileSize(bytes.subspan(16)) == song.size());
    XSFLIB_CHECK(dse::OldestFileSize(bytes.subspan(16 + song.size())) == bank.size());

    // A file cut before its end chunk, and bytes of another kind, give no size.
    XSFLIB_CHECK(!dse::OldestFileSize(bytes.subspan(16, song.size() - 1)));
    XSFLIB_CHECK(!dse::OldestFileSize(bytes));

    const std::vector<dse::NamedFile> files = {{.path = "data/Snd.iear", .bytes = bytes}};
    const std::vector<dse::NamedFile> found = dse::FindFiles(files);
    XSFLIB_CHECK(found.size() == 2);
    if (found.size() != 2)
    {
        return;
    }

    XSFLIB_CHECK(found[0].path == "data/Snd.iear/B_TEST.smd" && std::ranges::equal(found[0].bytes, song));
    XSFLIB_CHECK(found[1].path == "data/Snd.iear/B_TEST.swd" && std::ranges::equal(found[1].bytes, bank));
    XSFLIB_CHECK(dse::OldestFile::Parse(found[0].bytes).Write() == song);
    XSFLIB_CHECK(dse::OldestFile::Parse(found[1].bytes).Write() == bank);
}

// OldestBank gives each bank back byte for byte, with a header that gives a size of 0 too.
void TestOldestBankWritesBack(bool short_waves)
{
    const Bytes file = OldBankFile(short_waves);
    XSFLIB_CHECK(dse::OldestBank(dse::WaveBank::Parse(file), file) == file);

    Bytes no_size = file;
    xsflib::StoreLe32(no_size.data() + 0x08, 0);
    XSFLIB_CHECK(dse::OldestBank(dse::WaveBank::Parse(no_size), no_size) == no_size);
}

// The places of the values in OldBankFile: wave 2's entry, and program 1's second split.
constexpr std::size_t kWaviData = 0x60;
constexpr std::size_t kSecondWave = kWaviData + 16 + 0x40;
constexpr std::size_t kPrgiData = kWaviData + 16 + 2 * 0x40 + 16;
constexpr std::size_t kSecondSplit = kPrgiData + 16 + 0x10 + 0x80 + 0x40 + 0x30;

// Edits of a wave's values and of a split's values land at the places of version 0x402, and nothing else changes.
void TestOldestBankEdits()
{
    const Bytes file = OldBankFile(false);
    dse::WaveBank bank = dse::WaveBank::Parse(file);
    dse::WaveInfo& wave = *bank.waves[2];
    wave.SetSampleRate(22050);
    wave.SetLoopStart(0x40);
    wave.SetLoops(false);
    dse::Program& program = *bank.instruments->programs[1];
    const dse::MutableSplitView split(std::span<uint8_t, 0x30>(program.splits[1]));
    split.SetKeys(40, 50);
    split.SetWave(0);
    split.SetTuning(-300);
    split.SetCoarseTuning(3);
    split.Bytes()[dse::SplitView::kVolume] = 0x50;
    split.Bytes()[dse::SplitView::kPan] = 0x20;
    split.Bytes()[dse::SplitView::kAttackTime] = 0x33;

    const Bytes written = dse::OldestBank(bank, file);
    Bytes expected = file;
    expected[kSecondWave + 0x11] = 0;
    xsflib::StoreLe16(&expected[kSecondWave + 0x12], 22050);
    xsflib::StoreLe32(&expected[kSecondWave + 0x18], 0x40);
    expected[kSecondSplit + 0x04] = 40;
    expected[kSecondSplit + 0x05] = 50;
    expected[kSecondSplit + 0x06] = 40;
    expected[kSecondSplit + 0x07] = 50;
    expected[kSecondSplit + 0x11] = 0;
    xsflib::StoreLe16(&expected[kSecondSplit + 0x12], static_cast<uint16_t>(-300));
    expected[kSecondSplit + 0x15] = 3;
    expected[kSecondSplit + 0x16] = 0x50;
    expected[kSecondSplit + 0x17] = 0x20;
    expected[kSecondSplit + 0x29] = 0x33;
    XSFLIB_CHECK(written == expected);
    XSFLIB_CHECK(dse::WaveBank::Parse(written) == bank);

    // More samples make the pcmd chunk longer. The tables keep their bytes, and the size follows.
    bank.samples->insert(bank.samples->end(), 20, 0x77);
    const Bytes longer = dse::OldestBank(bank, file);
    XSFLIB_CHECK(longer.size() == file.size() + 16);
    XSFLIB_CHECK(xsflib::LoadLe32(&longer[0x08]) == longer.size());
    XSFLIB_CHECK(std::equal(expected.begin() + 0x0C, expected.begin() + kPrgiData + 0x140, longer.begin() + 0x0C));
    const dse::WaveBank read = dse::WaveBank::Parse(longer);
    XSFLIB_CHECK(read.waves == bank.waves && read.instruments == bank.instruments && read.samples == bank.samples);

    // With the same tables, the bytes after the key groups are the bank's too.
    dse::WaveBank padded = dse::WaveBank::Parse(file);
    padded.instruments->after_key_groups = {1, 2, 3, 4, 5, 6, 7, 8};
    XSFLIB_CHECK(dse::WaveBank::Parse(dse::OldestBank(padded, file)).instruments == padded.instruments);
}

// A bank of another layout than `like`'s takes the tables of Upgraded, and reads back as the same bank.
void TestOldestBankNewLayout()
{
    const Bytes file = OldBankFile(true);
    dse::WaveBank bank = dse::WaveBank::Parse(file);
    dse::Program& program = *bank.instruments->programs[1];
    program.splits.push_back(program.splits[0]);
    program.splits.back()[dse::SplitView::kIndex] = 2;
    program.header[0x02] = 3;
    // The bytes after the key groups are the bank's, not those of `like`.
    bank.instruments->after_key_groups = {1, 2, 3, 4, 5, 6, 7, 8};

    const Bytes written = dse::OldestBank(bank, file);
    XSFLIB_CHECK(dse::IsOldestVersion(written));
    XSFLIB_CHECK(std::equal(file.begin() + 0x0C, file.begin() + 0x46, written.begin() + 0x0C));
    XSFLIB_CHECK(xsflib::LoadLe32(&written[0x08]) == written.size());
    const dse::OldestFile parsed = dse::OldestFile::Parse(written);
    XSFLIB_CHECK(parsed.chunks.size() == 5 && parsed.chunks[0].data.size() == 16 + 2 * 0x40);
    const dse::WaveBank read = dse::WaveBank::Parse(written);
    XSFLIB_CHECK(read.waves == bank.waves);
    XSFLIB_CHECK(read.instruments == bank.instruments);
    XSFLIB_CHECK(read.samples == bank.samples);

    // Without a bank to keep the bytes of, the header is the bank's, with the version 0x402.
    const Bytes alone = dse::OldestBank(bank);
    XSFLIB_CHECK(dse::IsOldestVersion(alone));
    XSFLIB_CHECK(alone[0x46] == 3 && alone[0x47] == 2 && alone[0x48] == 1 && alone[0x49] == 0xFF);
    XSFLIB_CHECK(xsflib::LoadLe32(&alone[0x40]) == 0);
    const dse::WaveBank read_alone = dse::WaveBank::Parse(alone);
    XSFLIB_CHECK(read_alone.waves == bank.waves && read_alone.instruments == bank.instruments);
    XSFLIB_CHECK(read_alone.samples == bank.samples);

    // A bank of version 0x415 writes as one of version 0x402 with the same waves and splits.
    const dse::WaveBank newer = dse::WaveBank::Parse(dse::Upgraded(file));
    XSFLIB_CHECK(dse::WaveBank::Parse(dse::OldestBank(newer)).instruments == newer.instruments);
}

void TestOldestBankRefusals()
{
    const Bytes file = OldBankFile(false);
    const dse::WaveBank bank = dse::WaveBank::Parse(file);

    dse::WaveBank fast = bank;
    fast.waves[0]->SetSampleRate(65536);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(fast, file); }) ==
                 "a file of version 0x402 holds sample rates up to 65,535 Hz");

    dse::WaveBank wide = bank;
    dse::MutableSplitView(std::span<uint8_t, 0x30>(wide.instruments->programs[1]->splits[0])).SetWave(256);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(wide, file); }) ==
                 "a split of version 0x402 plays waves 0 to 255");

    dse::WaveBank many = bank;
    many.waves.resize(256);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(many, file); }) ==
                 "a file of version 0x402 holds at most 255 wave slots, programs and key groups");

    dse::WaveBank programs = bank;
    programs.instruments->programs.resize(256);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(programs, file); }) ==
                 "a file of version 0x402 holds at most 255 wave slots, programs and key groups");

    dse::WaveBank groups = bank;
    groups.instruments->key_groups.resize(256);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(groups, file); }) ==
                 "a file of version 0x402 holds at most 255 wave slots, programs and key groups");

    dse::WaveBank splits = bank;
    splits.instruments->programs[1]->splits.resize(256);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(splits, file); }) ==
                 "a file of version 0x402 holds at most 255 splits in a program");

    dse::WaveBank lfos = bank;
    lfos.instruments->programs[1]->lfos.resize(256);
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(lfos, file); }) ==
                 "a file of version 0x402 holds at most 255 LFOs in a program");

    // A program of 255 splits with 4 LFOs takes 12,448 bytes. Five, with the offsets, take 62,256 bytes. The sixth
    // starts there, and the seventh would start at 74,704, past the 64 KiB that a 16-bit offset reaches.
    dse::WaveBank large = bank;
    dse::Program full = *bank.instruments->programs[1];
    full.splits.resize(255, full.splits[0]);
    full.header[0x02] = 255;
    large.instruments->programs.assign(7, full);
    for (std::size_t slot = 0; slot < 7; slot++)
    {
        large.instruments->programs[slot]->header[0x00] = static_cast<uint8_t>(slot);
    }
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(large, file); }) ==
                 "a program has to start within the first 64 KiB of the programs");
    large.instruments->programs.resize(6);
    XSFLIB_CHECK(dse::WaveBank::Parse(dse::OldestBank(large, file)).instruments == large.instruments);

    dse::WaveBank no_samples = bank;
    no_samples.samples.reset();
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(no_samples); }) ==
                 "a file of version 0x402 holds no bank without samples");

    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { dse::OldestBank(bank, OldSongFile()); }) ==
                 "not a wave bank of version 0x402");
}

// The cut of a set with a song of version 0x402 makes its tracks a sequence. The join gives the files back byte for
// byte, and a longer track keeps the song's 16 bytes of settings and its size.
void TestOldestSongLayout(bool size_in_header)
{
    Bytes song = OldSongFile();
    if (!size_in_header)
    {
        xsflib::StoreLe32(song.data() + 0x08, 0);
    }

    const Bytes bank = OldBankFile(false);
    const std::vector<dse::NamedFile> files = {{"data/Snd.iear/B_TEST.smd", song}, {"data/Snd.iear/B_TEST.swd", bank}};

    // The driver's offsets count in the layout of version 0x415, and the file holds its tracks 32 bytes earlier.
    XSFLIB_CHECK(dse::TracksOffset(song, std::nullopt) == 0x80U);
    XSFLIB_CHECK(dse::TracksInFile(song, std::nullopt) == 0x60U);
    const dse::SettingsPlaces places = dse::SongSettingsPlaces(song);
    XSFLIB_CHECK(
        (places == dse::SettingsPlaces{.start = 0x50, .size = 16, .ticks = 0x52, .tracks = 0x54, .channels = 0x55}));

    dse::Layout layout = dse::Cut(files);
    XSFLIB_CHECK(layout.sequences.size() == 1U);
    if (layout.sequences.size() != 1)
    {
        return;
    }

    const Bytes set = dse::Join(layout.sequences, layout.pieces, layout.fields);
    const std::vector<dse::NamedFile> read = dse::ReadSet(set);
    XSFLIB_CHECK(read.size() == 2U && std::ranges::equal(read[0].bytes, song) &&
                 std::ranges::equal(read[1].bytes, bank));

    // A wait of 3 bytes after track 0's tempo takes the track from 7 bytes to 10, padded to 12.
    ntdlib::commands::Command wait;
    wait.kind = ntdlib::commands::Kind::kWait;
    wait.value.value = 300 * ntdlib::commands::kOne;
    ntdlib::commands::Sequence& sequence = layout.sequences[0];
    sequence.items.insert(sequence.items.begin() + 2, wait);
    const Bytes grown_set = dse::Join(layout.sequences, layout.pieces, layout.fields);
    const std::vector<dse::NamedFile> grown = dse::ReadSet(grown_set);
    const std::span<const uint8_t> bytes = grown[0].bytes;
    XSFLIB_CHECK(bytes.size() == song.size() + 4);
    XSFLIB_CHECK(xsflib::LoadLe32(&bytes[0x08]) == (size_in_header ? bytes.size() : 0U));
    XSFLIB_CHECK(std::equal(song.begin(), song.begin() + 0x08, bytes.begin()) &&
                 std::equal(song.begin() + 0x0C, song.begin() + 0x60, bytes.begin() + 0x0C));
    XSFLIB_CHECK(dse::IsOldestVersion(bytes) &&
                 dse::OldestFile::Parse(bytes).Write() == Bytes(bytes.begin(), bytes.end()));
    XSFLIB_CHECK(dse::DecodeTracks(dse::Song::Parse(bytes).tracks).items == sequence.items);
    XSFLIB_CHECK(std::ranges::equal(grown[1].bytes, bank));
}

void TestSettingsPlaces()
{
    Builder b;
    dse_files::HeaderStart(b, "smdl", 0);
    b.Fill(16, 0);
    b.Text("song").Fill(12, 0);
    b.Append(dse_files::SequenceInfoBytes(3, 2));
    const Bytes newer = b.Take();

    XSFLIB_CHECK((dse::SongSettingsPlaces(newer) ==
                  dse::SettingsPlaces{.start = 0x50, .size = 0x30, .ticks = 0x52, .tracks = 0x56, .channels = 0x57}));
    XSFLIB_CHECK((dse::SequenceInfoPlaces(0x94) ==
                  dse::SettingsPlaces{.start = 0x94, .size = 0x30, .ticks = 0x96, .tracks = 0x9A, .channels = 0x9B}));
    XSFLIB_CHECK(dse::TracksOffset(newer, std::nullopt) == 0x80U);
    XSFLIB_CHECK(dse::TracksInFile(newer, std::nullopt) == 0x80U);

    Bytes other = newer;
    xsflib::StoreLe16(&other[0x0C], 0x0410);

    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::SongSettingsPlaces(other); }) ==
                 "a DSE song of another version than 0x415 or 0x402");
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::SongSettingsPlaces(OldBankFile(false)); }) ==
                 "not a DSE song");

    const Bytes cut(newer.begin(), newer.begin() + 0x70);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::SongSettingsPlaces(cut); }) ==
                 "the song ends before its settings");
}

} // namespace

int main()
{
    TestOldestSong();
    TestOldestBank(false);
    TestOldestBank(true);
    TestOldestRefusals();
    TestOldestInPack();
    TestOldestBankWritesBack(false);
    TestOldestBankWritesBack(true);
    TestOldestBankEdits();
    TestOldestBankNewLayout();
    TestOldestBankRefusals();
    TestOldestSongLayout(true);
    TestOldestSongLayout(false);
    TestSettingsPlaces();

    return xsflib::testing::Summary("DSE files of version 0x402");
}
