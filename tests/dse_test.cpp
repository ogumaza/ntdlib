// SPDX-License-Identifier: MIT

// Tests of the readers and writers of DSE's files (ntdlib/dse/formats.h). Each test builds a file by hand in the layout
// that formats.h describes. The reader has to find its parts, and the writer has to give the file back byte for byte.
// The bytes of the edited files were worked out by hand from the same layout.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/dse/formats.h"
#include "ntdlib/testing/dse_files.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace dse_files = ntdlib::testing::dse_files;
namespace dse = ntdlib::dse;
using dse_files::BankChunk;
using dse_files::Builder;
using dse_files::Bytes;
using dse_files::HeaderStart;
using dse_files::SequenceInfoBytes;
using dse_files::TrackChunk;

// A song with two tracks. Track 0 sets the tempo, plays a note with a length of one byte, pauses, ends, and holds two
// bytes after its end. Track 1 sets its volume, and its chunk's size leaves out its end, which lies in the padding.
Bytes SongFile()
{
    Builder b;
    HeaderStart(b, "smdl", 0x7901);
    b.U32(1).U32(1).U32(0xFFFFFFFF).U32(0xFFFFFFFF);
    b.Text("song").Append({0x00, 0x00, 0x00, 0x01, 0x10, 0xFF, 0x00, 0x00, 0xB0, 0xFF, 0xFF, 0xFF});
    b.Append(SequenceInfoBytes(2, 2));

    TrackChunk(b, "trk ", 13).Append({0, 0, 0, 0, 0xA4, 0x78, 0x64, 0x60, 0x30, 0x80, 0x98, 0x8D, 0x92}).Fill(3, 0x98);
    TrackChunk(b, "trk ", 6).Append({1, 1, 0, 0, 0xE0, 0x7F}).Fill(2, 0x98);
    TrackChunk(b, "eoc ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    return b.Take();
}

void TestSong()
{
    const Bytes file = SongFile();
    const dse::Song song = dse::Song::Parse(file);
    XSFLIB_CHECK(song.info.TicksPerQuarter() == 48);
    XSFLIB_CHECK(song.info.TrackCount() == 2 && song.info.ChannelCount() == 2);
    XSFLIB_CHECK(song.tracks.size() == 2);
    if (song.tracks.size() != 2)
    {
        return;
    }

    const dse::Track& first = song.tracks[0];
    XSFLIB_CHECK(first.number == 0 && first.channel == 0);
    XSFLIB_CHECK(
        (first.events == std::vector<dse::Event>{{0xA4, {0x78}}, {0x64, {0x60, 0x30}}, {0x80, {}}, {0x98, {}}}));
    XSFLIB_CHECK((first.unread == Bytes{0x8D, 0x92}));
    XSFLIB_CHECK(!first.end_in_padding);

    const dse::Track& second = song.tracks[1];
    XSFLIB_CHECK(second.number == 1 && second.channel == 1);
    XSFLIB_CHECK((second.events == std::vector<dse::Event>{{0xE0, {0x7F}}, {0x98, {}}}));
    XSFLIB_CHECK(second.unread.empty() && second.end_in_padding);

    XSFLIB_CHECK(song.Write() == file);

    // Without its volume, track 1's end would fall on a multiple of 4. The size then counts the end: 5 bytes, padded
    // with 3.
    dse::Song edited = song;
    edited.tracks[1].events.erase(edited.tracks[1].events.begin());
    const Bytes written = edited.Write();
    XSFLIB_CHECK(xsflib::LoadLe32(written.data() + 0xA0 + 12) == 5);
    XSFLIB_CHECK((Bytes(written.begin() + 0xB0, written.begin() + 0xB8) == Bytes{1, 1, 0, 0, 0x98, 0x98, 0x98, 0x98}));
    XSFLIB_CHECK(xsflib::LoadLe32(written.data() + 0x08) == written.size());
    const dse::Song again = dse::Song::Parse(written);
    XSFLIB_CHECK(again.tracks.size() == 2 && !again.tracks[1].end_in_padding);
    XSFLIB_CHECK(again.tracks.size() == 2 && again.tracks[1].events == edited.tracks[1].events);
}

void TestEvents()
{
    // A note's length takes 0 to 3 bytes, by the top two bits of the byte after the code.
    XSFLIB_CHECK(dse::ArgumentBytes(0x7F, 0x00) == 1);
    XSFLIB_CHECK(dse::ArgumentBytes(0x01, 0xC0) == 4);
    XSFLIB_CHECK(dse::ArgumentBytes(0x8F, 0xFF) == 0);
    XSFLIB_CHECK(dse::ArgumentBytes(0x94, 0x00) == 3); // a wait of 24 bits
    XSFLIB_CHECK(dse::ArgumentBytes(0xAB, 0x00) == 1); // a byte that the driver skips
    XSFLIB_CHECK(dse::ArgumentBytes(0xC0, 0x00) == 0); // a flag without arguments
    XSFLIB_CHECK(dse::ArgumentBytes(0xCB, 0x00) == 2); // two bytes that the driver skips
    XSFLIB_CHECK(dse::ArgumentBytes(0xF0, 0x00) == 5); // an LFO's settings
    XSFLIB_CHECK(dse::ArgumentBytes(0xF8, 0x00) == 2);
    XSFLIB_CHECK(dse::ArgumentBytes(0x96, 0x00) == -1); // invalid
    XSFLIB_CHECK(dse::ArgumentBytes(0xFF, 0x00) == -1);
}

void TestSongRefusals()
{
    Bytes file = SongFile();
    file[0] = 'x';
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::Song::Parse(file); }) == "not a DSE smdl file");

    // Track 1 sets its volume, then starts a wait of 16 bits in its last byte. Its 8 bytes are a multiple of 4, so it
    // has no padding, and the wait's second byte lies past the chunk.
    file = SongFile();
    Builder b;
    b.Append(Bytes(file.begin(), file.begin() + 0xA0));
    TrackChunk(b, "trk ", 8).Append({1, 1, 0, 0, 0xE0, 0x7F, 0x93, 0x10});
    TrackChunk(b, "eoc ", 0);
    const Bytes endless = b.Take();
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::Song::Parse(endless); }) == "a track has no end");
}

// A track can fill its chunk without an end. Track 1 sets its volume twice in 8 bytes, a multiple of 4, so it has no
// padding. The song reads, and comes back byte for byte.
void TestTrackFillsItsChunk()
{
    const Bytes file = SongFile();
    Builder b;
    b.Append(Bytes(file.begin(), file.begin() + 0xA0));
    TrackChunk(b, "trk ", 8).Append({1, 1, 0, 0, 0xE0, 0x7F, 0xE0, 0x7F});
    TrackChunk(b, "eoc ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    const Bytes filled = b.Take();
    const dse::Song song = dse::Song::Parse(filled);
    XSFLIB_CHECK(song.tracks.size() == 2);
    if (song.tracks.size() != 2)
    {
        return;
    }
    XSFLIB_CHECK((song.tracks[1].events == std::vector<dse::Event>{{0xE0, {0x7F}}, {0xE0, {0x7F}}}));
    XSFLIB_CHECK(song.tracks[1].unread.empty() && !song.tracks[1].end_in_padding);
    XSFLIB_CHECK(song.Write() == filled);
}

// A wave's parameters with `id` and a rate of 22,050 Hz.
Bytes WaveEntry(uint16_t id)
{
    Builder b;
    b.Append({0x01, 0xAA}).U16(id).Append({0x30, 0xF9, 0x3C, 0x00, 0x7F, 0x40, 0x00, 0x02, 0x00, 0x00, 0xAA, 0xAA});
    b.U16(0x0415).U16(0x0200).Append({0x09, 0x01, 0x01, 0x08, 0x00, 0x04, 0x01, 0x01}).U32(0);
    b.U32(22050).U32(0x10 * id).U32(0xBE).U32(0x26B);
    b.Append({0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x7F, 0x28, 0xFF});
    return b.Take();
}

// A wave bank with three wave slots (wave 1 left out), two program slots (program 0 left out), one key group and 5
// bytes of samples. The kgrp and pcmd chunks are padded with other bytes than zeros.
Bytes WaveBankFile()
{
    Builder b;
    HeaderStart(b, "swdl", 0x7F00);
    b.Append({0x00, 0xAA, 0xAA, 0xAA}).Fill(8, 0).U32(0x10);
    b.U32(5).U16(0).U16(3).U16(2).U16(0x0202).U32(16 + 0x80);

    BankChunk(b, "wavi", 16 + 0x80).U16(16).U16(0).U16(16 + 0x40).Fill(10, 0xAA);
    b.Append(WaveEntry(0)).Append(WaveEntry(2));

    // Program 1: a header, one LFO, the filler and two splits.
    BankChunk(b, "prgi", 16 + 0x20 + 0x10 + 0x60).U16(0).U16(16).Fill(12, 0);
    b.U16(1).U16(2).Append({0x7F, 0x40, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x01, 0xAA, 0x00, 0x00, 0x00});
    b.Append({0x01, 0x00, 0x01, 0x01, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    b.Fill(16, 0xAA);
    for (uint8_t split = 0; split < 2; split++)
    {
        b.Append({0x00, split, 0x02, 0x00, static_cast<uint8_t>(split * 64), static_cast<uint8_t>(split * 64 + 63)});
        b.Fill(0x2A, split);
    }

    BankChunk(b, "kgrp", 8).Append({0x00, 0x00, 0x08, 0x08, 0x00, 0x0F, 0x00, 0x00});
    b.Append({0x97, 0xCB, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00});
    BankChunk(b, "pcmd", 5).Append({1, 2, 3, 4, 5}).Append({0x68, 0x00, 0x68, 0, 0, 0, 0, 0, 0, 0, 0});
    BankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    return b.Take();
}

void TestWaveBank()
{
    const Bytes file = WaveBankFile();
    const dse::WaveBank bank = dse::WaveBank::Parse(file);
    XSFLIB_CHECK(bank.waves.size() == 3);
    XSFLIB_CHECK(bank.waves.size() == 3 && bank.waves[0] && !bank.waves[1] && bank.waves[2]);
    if (bank.waves.size() == 3 && bank.waves[2])
    {
        const dse::WaveInfo& wave = *bank.waves[2];
        XSFLIB_CHECK(wave.Id() == 2 && wave.Format() == 0x200 && wave.Loops());
        XSFLIB_CHECK(wave.SampleRate() == 22050 && wave.SamplePosition() == 0x20);
        XSFLIB_CHECK(wave.LoopStart() == 0xBE && wave.LoopLength() == 0x26B);
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
    XSFLIB_CHECK(program.Id() == 1 && program.lfos.size() == 1 && program.splits.size() == 2);
    XSFLIB_CHECK(program.splits.size() == 2 && program.splits[1][1] == 1 && program.splits[1][5] == 127);
    XSFLIB_CHECK(instruments.key_groups.size() == 1);
    XSFLIB_CHECK((instruments.after_key_groups == Bytes{0x97, 0xCB, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00}));
    XSFLIB_CHECK((bank.samples == Bytes{1, 2, 3, 4, 5}));
    XSFLIB_CHECK(bank.after_samples.size() == 11 && bank.after_samples[0] == 0x68);

    XSFLIB_CHECK(bank.Write() == file);

    // A third split: the program's header counts it, and the prgi chunk grows by 0x30. The kgrp chunk then starts 0x30
    // bytes later.
    dse::WaveBank edited = bank;
    edited.instruments->programs[1]->splits.push_back(edited.instruments->programs[1]->splits[0]);
    const Bytes written = edited.Write();
    XSFLIB_CHECK(written.size() == file.size() + 0x30);
    XSFLIB_CHECK(xsflib::LoadLe32(written.data() + 0xF0 + 12) == 0xA0 + 0x30);
    XSFLIB_CHECK(xsflib::LoadLe16(written.data() + 0x100 + 0x10 + 2) == 3);
    XSFLIB_CHECK(xsflib::LoadLe32(written.data() + 0x40) == 5);
    XSFLIB_CHECK(xsflib::LoadLe32(written.data() + 0x08) == written.size());

    // The headers then hold the new counts and sizes. The other parts read back as they were edited.
    const dse::WaveBank reread = dse::WaveBank::Parse(written);
    XSFLIB_CHECK(reread.Write() == written);
    XSFLIB_CHECK(reread.waves == edited.waves && reread.samples == edited.samples);
    XSFLIB_CHECK(reread.instruments && reread.instruments->programs.size() == 2 && reread.instruments->programs[1] &&
                 reread.instruments->programs[1]->splits == edited.instruments->programs[1]->splits);
}

void TestWaveBankRefusals()
{
    // Wave 2's entry gives the number 1.
    Bytes file = WaveBankFile();
    file[0x60 + 0x50 + 2] = 1;
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::WaveBank::Parse(file); }) == "a wave's slot isn't its number");

    file = WaveBankFile();
    file[0x60 + 2] = 0xFF;
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::WaveBank::Parse(file); }) ==
                 "a table names an entry outside its chunk");
}

// A bank of sound effects with three slots: slot 0 left out, slot 1 with one track and slot 2 with none. Its mcrl
// table has four slots, the third left out. Its bnkl table has two, the first left out.
Bytes SoundEffectsFile()
{
    Builder b;
    HeaderStart(b, "sedl", 0x0024);
    b.U16(3).Append({0x01, 0x00, 0x00, 0x01, 0x00, 0x00}).Fill(8, 0xFF);

    const uint32_t second = 16 + 0x30 + 16 + 8 + 16;
    BankChunk(b, "seq ", second + 0x30 + 16).U16(0).U16(16).U16(static_cast<uint16_t>(second)).Fill(10, 0);
    b.Append(SequenceInfoBytes(1, 1));
    TrackChunk(b, "trk ", 5).Append({0, 0, 0, 0, 0x98}).Fill(3, 0x98);
    TrackChunk(b, "eoc ", 0);
    b.Append(SequenceInfoBytes(0, 0));
    TrackChunk(b, "eoc ", 0).Fill(8, 0);

    BankChunk(b, "mcrl", 16).U16(8).U16(10).U16(0).U16(12).Append({0, 0, 0, 0, 1, 0, 0x45, 0x64});
    BankChunk(b, "bnkl", 10).U16(0).U16(4).Append({1, 0, 0, 0, 0x45, 0x64}).Fill(6, 0);
    BankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));
    return b.Take();
}

void TestSoundEffects()
{
    const Bytes file = SoundEffectsFile();
    const dse::SoundEffects effects = dse::SoundEffects::Parse(file);
    XSFLIB_CHECK(effects.sequences.size() == 3);
    if (effects.sequences.size() != 3 || !effects.sequences[1] || !effects.sequences[2])
    {
        XSFLIB_CHECK(false);
        return;
    }
    XSFLIB_CHECK(!effects.sequences[0]);
    XSFLIB_CHECK(effects.sequences[1]->tracks.size() == 1 && effects.sequences[2]->tracks.empty());
    XSFLIB_CHECK(effects.sequences[1]->info.TrackCount() == 1);

    XSFLIB_CHECK((effects.macros.slots.size() == 4 && effects.macros.slots[0] == Bytes{0, 0} &&
                  effects.macros.slots[1] == Bytes{0, 0} && !effects.macros.slots[2] &&
                  effects.macros.slots[3] == Bytes{1, 0, 0x45, 0x64}));
    XSFLIB_CHECK((effects.bank_lists.slots.size() == 2 && !effects.bank_lists.slots[0] &&
                  effects.bank_lists.slots[1] == Bytes{1, 0, 0, 0, 0x45, 0x64}));

    XSFLIB_CHECK(effects.Write() == file);

    // A fourth slot with a copy of the first sound effect: the table then holds 8 bytes, padded to 16 as before.
    dse::SoundEffects edited = effects;
    edited.sequences.push_back(edited.sequences[1]);
    const Bytes written = edited.Write();
    XSFLIB_CHECK(xsflib::LoadLe16(written.data() + 0x30) == 4);
    XSFLIB_CHECK(xsflib::LoadLe16(written.data() + 0x50 + 6) == 16 + 0x30 + 16 + 8 + 16 + 0x30 + 16);

    // The header then holds the new count and size. The other parts read back as they were edited.
    const dse::SoundEffects reread = dse::SoundEffects::Parse(written);
    XSFLIB_CHECK(reread.Write() == written);
    XSFLIB_CHECK(reread.sequences == edited.sequences && reread.macros == edited.macros &&
                 reread.bank_lists == edited.bank_lists);
}

// Each setter of a sequence's settings writes its value where SequenceInfoBytes holds it, and leaves the other bytes.
void TestSequenceInfoSetters()
{
    const Bytes original = SequenceInfoBytes(2, 3);
    dse::SequenceInfo info;
    std::ranges::copy(original, info.bytes.begin());

    info.SetTicksPerQuarter(0x1E0);
    info.SetTrackCount(17);
    info.SetChannelCount(16);

    XSFLIB_CHECK(info.TicksPerQuarter() == 0x1E0 && info.TrackCount() == 17 && info.ChannelCount() == 16);
    Bytes expected = original;
    expected[2] = 0xE0;
    expected[3] = 0x01;
    expected[6] = 17;
    expected[7] = 16;
    XSFLIB_CHECK(Bytes(info.bytes.begin(), info.bytes.end()) == expected);
}

// Each setter of a wave's parameters writes its value where WaveEntry holds it, and leaves the other bytes.
void TestWaveInfoSetters()
{
    const Bytes entry = WaveEntry(2);
    dse::WaveInfo wave;
    std::ranges::copy(entry, wave.bytes.begin());

    wave.SetId(7);
    wave.SetFormat(0x100);
    wave.SetLoops(false);
    wave.SetSampleRate(32000);
    wave.SetSamplePosition(0x1234);
    wave.SetLoopStart(3);
    wave.SetLoopLength(0x40);

    XSFLIB_CHECK(wave.Id() == 7 && wave.Format() == 0x100 && !wave.Loops());
    XSFLIB_CHECK(wave.SampleRate() == 32000 && wave.SamplePosition() == 0x1234);
    XSFLIB_CHECK(wave.LoopStart() == 3 && wave.LoopLength() == 0x40);
    Bytes expected = entry;
    xsflib::StoreLe16(&expected[0x02], 7);
    xsflib::StoreLe16(&expected[0x12], 0x100);
    expected[0x15] = 0;
    xsflib::StoreLe32(&expected[0x20], 32000);
    xsflib::StoreLe32(&expected[0x24], 0x1234);
    xsflib::StoreLe32(&expected[0x28], 3);
    xsflib::StoreLe32(&expected[0x2C], 0x40);
    XSFLIB_CHECK(Bytes(wave.bytes.begin(), wave.bytes.end()) == expected);

    wave.SetLoops(true);
    XSFLIB_CHECK(wave.Loops() && wave.bytes[0x15] == 1);
}

// A split's view reads each value where the driver reads it: the index at 1, the bend range at 2, the key range at 4
// and 5, the velocity range at 8 and 9, the wave at 0x12, the tuning at 0x14, the coarse tuning at 0x17, the volume,
// the pan and the key group from 0x18, and the envelope's 16 bytes from 0x20. Split 1 of WaveBankFile's program 1
// holds 1 in each byte after the key range.
void TestSplitView()
{
    const dse::WaveBank bank = dse::WaveBank::Parse(WaveBankFile());
    if (!bank.instruments || bank.instruments->programs.size() != 2 || !bank.instruments->programs[1] ||
        bank.instruments->programs[1]->splits.size() != 2)
    {
        XSFLIB_CHECK(false);
        return;
    }
    const std::array<uint8_t, 0x30>& held = bank.instruments->programs[1]->splits[1];

    const dse::SplitView split(held);

    XSFLIB_CHECK(split.Index() == 1 && split.BendRange() == 2);
    XSFLIB_CHECK(split.LowKey() == 64 && split.HighKey() == 127);
    XSFLIB_CHECK(split.LowVelocity() == 1 && split.HighVelocity() == 1);
    XSFLIB_CHECK(split.Wave() == 0x0101 && split.Tuning() == 0x0101 && split.RootKey() == 1);
    XSFLIB_CHECK(split.CoarseTuning() == 1 && split.Volume() == 1 && split.Pan() == 1 && split.KeyGroup() == 1);
    XSFLIB_CHECK(split.Envelope().data() == held.data() + 0x20);
    XSFLIB_CHECK(split.EnvelopeMultiplier() == 1 && split.AttackBegin() == 1 && split.AttackTime() == 1);
    XSFLIB_CHECK(split.DecayTime() == 1 && split.SustainLevel() == 1 && split.HoldTime() == 1);
    XSFLIB_CHECK(split.SustainTime() == 1 && split.ReleaseTime() == 1);
}

// The setters of a split's view write the ranges and their copies 2 bytes on, and the values of more than one byte
// little-endian. The other bytes stay.
void TestSplitViewSetters()
{
    std::array<uint8_t, 0x30> bytes = {};
    bytes.fill(0xAA);
    const dse::MutableSplitView split(bytes);

    split.SetKeys(10, 20);
    split.SetVelocities(30, 40);
    split.SetWave(0x0203);
    split.SetTuning(-2);
    split.SetCoarseTuning(-5);

    XSFLIB_CHECK(split.LowKey() == 10 && split.HighKey() == 20);
    XSFLIB_CHECK(split.LowVelocity() == 30 && split.HighVelocity() == 40);
    XSFLIB_CHECK(split.Wave() == 0x0203 && split.Tuning() == -2 && split.CoarseTuning() == -5);
    std::array<uint8_t, 0x30> expected = {};
    expected.fill(0xAA);
    expected[0x04] = 10;
    expected[0x05] = 20;
    expected[0x06] = 10;
    expected[0x07] = 20;
    expected[0x08] = 30;
    expected[0x09] = 40;
    expected[0x0A] = 30;
    expected[0x0B] = 40;
    expected[0x12] = 0x03;
    expected[0x13] = 0x02;
    expected[0x14] = 0xFE;
    expected[0x15] = 0xFF;
    expected[0x17] = 0xFB;
    XSFLIB_CHECK(bytes == expected);
}

} // namespace

int main()
{
    TestSong();
    TestEvents();
    TestSongRefusals();
    TestTrackFillsItsChunk();
    TestWaveBank();
    TestWaveBankRefusals();
    TestSoundEffects();
    TestSequenceInfoSetters();
    TestWaveInfoSetters();
    TestSplitView();
    TestSplitViewSetters();

    return xsflib::testing::Summary("DSE files");
}
