// SPDX-License-Identifier: MIT

// Tests of the readers and writers of DSE's files (ntdlib/dse/formats.h). Each test builds a file by hand in the layout
// that formats.h describes. The reader has to find its parts, and the writer has to give the file back byte for byte.
// The bytes of the edited files were worked out by hand from the same layout.

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
using dse_files::Builder;
using dse_files::Bytes;
using dse_files::HeaderStart;
using dse_files::SequenceInfoBytes;

// A song with two tracks. Track 0 sets the tempo, plays a note with a length of one byte, pauses, ends, and holds two
// bytes after its end. Track 1 sets its volume, and its chunk's size leaves out its end, which lies in the padding.
Bytes SongFile()
{
    Builder b;
    HeaderStart(b, "smdl", 0x7901);
    b.U32(1).U32(1).U32(0xFFFFFFFF).U32(0xFFFFFFFF);
    b.Text("song").Raw({0x00, 0x00, 0x00, 0x01, 0x10, 0xFF, 0x00, 0x00, 0xB0, 0xFF, 0xFF, 0xFF});
    b.Raw(SequenceInfoBytes(2, 2));

    b.TrackChunk("trk ", 13).Raw({0, 0, 0, 0, 0xA4, 0x78, 0x64, 0x60, 0x30, 0x80, 0x98, 0x8D, 0x92}).Fill(3, 0x98);
    b.TrackChunk("trk ", 6).Raw({1, 1, 0, 0, 0xE0, 0x7F}).Fill(2, 0x98);
    b.TrackChunk("eoc ", 0);
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
    b.Raw(Bytes(file.begin(), file.begin() + 0xA0));
    b.TrackChunk("trk ", 8).Raw({1, 1, 0, 0, 0xE0, 0x7F, 0x93, 0x10}).TrackChunk("eoc ", 0);
    const Bytes endless = b.Take();
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { dse::Song::Parse(endless); }) == "a track has no end");
}

// A track can fill its chunk without an end. Track 1 sets its volume twice in 8 bytes, a multiple of 4, so it has no
// padding. The song reads, and comes back byte for byte.
void TestTrackFillsItsChunk()
{
    const Bytes file = SongFile();
    Builder b;
    b.Raw(Bytes(file.begin(), file.begin() + 0xA0));
    b.TrackChunk("trk ", 8).Raw({1, 1, 0, 0, 0xE0, 0x7F, 0xE0, 0x7F}).TrackChunk("eoc ", 0);
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
    b.Raw({0x01, 0xAA}).U16(id).Raw({0x30, 0xF9, 0x3C, 0x00, 0x7F, 0x40, 0x00, 0x02, 0x00, 0x00, 0xAA, 0xAA});
    b.U16(0x0415).U16(0x0200).Raw({0x09, 0x01, 0x01, 0x08, 0x00, 0x04, 0x01, 0x01}).U32(0);
    b.U32(22050).U32(0x10 * id).U32(0xBE).U32(0x26B);
    b.Raw({0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x7F, 0x28, 0xFF});
    return b.Take();
}

// A wave bank with three wave slots (wave 1 left out), two program slots (program 0 left out), one key group and 5
// bytes of samples. The kgrp and pcmd chunks are padded with other bytes than zeros.
Bytes WaveBankFile()
{
    Builder b;
    HeaderStart(b, "swdl", 0x7F00);
    b.Raw({0x00, 0xAA, 0xAA, 0xAA}).Fill(8, 0).U32(0x10);
    b.U32(5).U16(0).U16(3).U16(2).U16(0x0202).U32(16 + 0x80);

    b.BankChunk("wavi", 16 + 0x80).U16(16).U16(0).U16(16 + 0x40).Fill(10, 0xAA);
    b.Raw(WaveEntry(0)).Raw(WaveEntry(2));

    // Program 1: a header, one LFO, the filler and two splits.
    b.BankChunk("prgi", 16 + 0x20 + 0x10 + 0x60).U16(0).U16(16).Fill(12, 0);
    b.U16(1).U16(2).Raw({0x7F, 0x40, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x01, 0xAA, 0x00, 0x00, 0x00});
    b.Raw({0x01, 0x00, 0x01, 0x01, 0x10, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    b.Fill(16, 0xAA);
    for (uint8_t split = 0; split < 2; split++)
    {
        b.Raw({0x00, split, 0x02, 0x00, static_cast<uint8_t>(split * 64), static_cast<uint8_t>(split * 64 + 63)});
        b.Fill(0x2A, split);
    }

    b.BankChunk("kgrp", 8).Raw({0x00, 0x00, 0x08, 0x08, 0x00, 0x0F, 0x00, 0x00});
    b.Raw({0x97, 0xCB, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00});
    b.BankChunk("pcmd", 5).Raw({1, 2, 3, 4, 5}).Raw({0x68, 0x00, 0x68, 0, 0, 0, 0, 0, 0, 0, 0});
    b.BankChunk("eod ", 0);
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
    b.U16(3).Raw({0x01, 0x00, 0x00, 0x01, 0x00, 0x00}).Fill(8, 0xFF);

    const uint32_t second = 16 + 0x30 + 16 + 8 + 16;
    b.BankChunk("seq ", second + 0x30 + 16).U16(0).U16(16).U16(static_cast<uint16_t>(second)).Fill(10, 0);
    b.Raw(SequenceInfoBytes(1, 1)).TrackChunk("trk ", 5).Raw({0, 0, 0, 0, 0x98}).Fill(3, 0x98).TrackChunk("eoc ", 0);
    b.Raw(SequenceInfoBytes(0, 0)).TrackChunk("eoc ", 0).Fill(8, 0);

    b.BankChunk("mcrl", 16).U16(8).U16(10).U16(0).U16(12).Raw({0, 0, 0, 0, 1, 0, 0x45, 0x64});
    b.BankChunk("bnkl", 10).U16(0).U16(4).Raw({1, 0, 0, 0, 0x45, 0x64}).Fill(6, 0);
    b.BankChunk("eod ", 0);
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

    return xsflib::testing::Summary("DSE files");
}
