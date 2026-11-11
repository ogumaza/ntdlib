// SPDX-License-Identifier: MIT

// Tests of the readers and writers of the Nintendo 64's sound files (ntdlib/n64). Each test builds a file by hand in
// the layout that the headers describe. The reader has to find its parts, and the writer has to give the file back byte
// for byte. The expected events and samples were worked out by hand from the same layouts.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/n64/bank_file.h"
#include "ntdlib/n64/midi.h"
#include "ntdlib/n64/seq_file.h"
#include "ntdlib/n64/vadpcm.h"

using xsflib::testing::ErrorOf;

namespace
{

namespace n64 = ntdlib::n64;
using Bytes = std::vector<uint8_t>;

void Put16(Bytes& bytes, uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void Put32(Bytes& bytes, uint32_t value)
{
    Put16(bytes, static_cast<uint16_t>(value >> 16));
    Put16(bytes, static_cast<uint16_t>(value));
}

void TestSeqFile()
{
    // Two sequences with stale bytes between them, and bytes after the last one that belong to something else.
    Bytes file = {'S', '1', 0, 2};
    Put32(file, 20);
    Put32(file, 2);
    Put32(file, 24);
    Put32(file, 3);
    file.insert(file.end(), {0xA1, 0xA2, 0xEE, 0xEF, 0xB1, 0xB2, 0xB3});
    Bytes data = file;
    data.push_back(0x55);

    XSFLIB_CHECK(n64::SeqFile::Matches(data));
    const n64::SeqFile bank = n64::SeqFile::Parse(data);
    XSFLIB_CHECK(bank.Entries().size() == 2);
    XSFLIB_CHECK(bank.Entries()[1] == (n64::SeqFile::Entry{24, 3}));
    const auto second = bank.Sequence(1);
    XSFLIB_CHECK((Bytes(second.begin(), second.end()) == Bytes{0xB1, 0xB2, 0xB3}));
    XSFLIB_CHECK(bank.Write() == file);

    // A longer first sequence moves the second to the next multiple of 4.
    n64::SeqFile edited = bank;
    const Bytes longer = {1, 2, 3, 4, 5};
    edited.SetSequence(0, longer);
    const Bytes written = edited.Write();
    XSFLIB_CHECK(edited.Entries()[0] == (n64::SeqFile::Entry{20, 5}));
    XSFLIB_CHECK(edited.Entries()[1] == (n64::SeqFile::Entry{28, 3}));
    XSFLIB_CHECK((Bytes(written.begin() + 20, written.end()) == Bytes{1, 2, 3, 4, 5, 0, 0, 0, 0xB1, 0xB2, 0xB3}));

    Bytes bad = file;
    bad[0] = 'X';
    XSFLIB_CHECK(!n64::SeqFile::Matches(bad));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::SeqFile::Parse(bad); }) ==
                 "not a sequence bank of revision S1");

    Bytes cut = file;
    cut.resize(file.size() - 1);
    XSFLIB_CHECK(!n64::SeqFile::Matches(cut));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::SeqFile::Parse(cut); }) ==
                 "sequence 1 of a sequence bank leaves the data");
}

// Hudson's revision S2: S1's table, then a record for each sequence, then 0xFFFFFFFF.
void TestHudsonRecords()
{
    Bytes file = {'S', '2', 0, 2};
    Put32(file, 0x38);
    Put32(file, 2);
    Put32(file, 0x3C);
    Put32(file, 3);

    for (const uint8_t bank : {uint8_t{0x00}, uint8_t{0x15}})
    {
        file.push_back(bank);
        file.push_back(static_cast<uint8_t>(0x7D - bank));
        Put16(file, 0xFFFF);
        Put32(file, 0x40);
        Put32(file, 0x1234);
        Put32(file, 0x5678);
    }

    Put32(file, 0xFFFFFFFF);
    file.insert(file.end(), {0xA1, 0xA2, 0xEE, 0xEF, 0xB1, 0xB2, 0xB3});

    const std::vector<n64::HudsonSongRecord> records = n64::ReadHudsonRecords(file);

    XSFLIB_CHECK(records.size() == 2);
    XSFLIB_CHECK(records[0] == (n64::HudsonSongRecord{0x00, 0x7D, 0x40}));
    XSFLIB_CHECK(records[1] == (n64::HudsonSongRecord{0x15, 0x68, 0x40}));

    // The sequences read as in S1.
    const n64::SeqFile bank = n64::SeqFile::Parse(file, n64::kHudsonSeqFileRevision);

    const auto second = bank.Sequence(1);
    XSFLIB_CHECK((Bytes(second.begin(), second.end()) == Bytes{0xB1, 0xB2, 0xB3}));

    XSFLIB_CHECK(
        ErrorOf<xsflib::FormatError>([&] { n64::ReadHudsonRecords(Bytes(file.begin(), file.begin() + 40)); }) ==
        "the records of a sequence bank of revision S2 leave the data");

    Bytes other = file;
    other[1] = '1';
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::ReadHudsonRecords(other); }) ==
                 "not a sequence bank of revision S2");
}

// A bank file with one bank, a percussion instrument and two programs, the second of them empty. The sound's wave has a
// loop and a book of one predictor. The padding after the key map holds bytes that the writer has to keep.
Bytes BankBytes()
{
    Bytes file = {'B', '1', 0, 1};
    Put32(file, 0x10);
    Put32(file, 0xCAFEF00D); // not in any object
    file.resize(0x10, 0);

    // 0x10: the bank. Offsets: percussion 0x28, program 0 at 0x28, program 1 empty.
    Put16(file, 2);
    file.push_back(0);
    file.push_back(0);
    Put32(file, 22050);
    Put32(file, 0x28);
    Put32(file, 0x28);
    Put32(file, 0);
    file.resize(0x28, 0);

    // 0x28: the instrument with one sound.
    file.insert(file.end(), {100, 64, 5, 0, 1, 2, 3, 4, 5, 6, 7, 8});
    Put16(file, 200);
    Put16(file, 1);
    Put32(file, 0x40);
    file.resize(0x40, 0);

    // 0x40: the sound.
    Put32(file, 0x50);
    Put32(file, 0x60);
    Put32(file, 0x68);
    file.insert(file.end(), {32, 96, 0, 0});

    // 0x50: the envelope.
    Put32(file, 1000);
    Put32(file, 2000);
    Put32(file, static_cast<uint32_t>(-1));
    file.insert(file.end(), {127, 80, 0, 0});

    // 0x60: the key map, then two bytes of padding that aren't zero.
    file.insert(file.end(), {0, 127, 21, 108, 60, static_cast<uint8_t>(-5), 0xAB, 0xCD});

    // 0x68: the wave. Base and length in the .tbl, a VADPCM type, a loop and a book.
    Put32(file, 0x1234);
    Put32(file, 900);
    file.insert(file.end(), {0, 0, 0, 0});
    Put32(file, 0x80);
    Put32(file, 0xB0);
    file.resize(0x80, 0);

    // 0x80: the loop with its state, then padding.
    Put32(file, 16);
    Put32(file, 1600);
    Put32(file, 0xFFFFFFFF);
    for (uint16_t i = 0; i < 16; i++)
    {
        Put16(file, static_cast<uint16_t>(i * 3));
    }
    file.resize(0xB0, 0);

    // 0xB0: the book. Order 2 and one predictor.
    Put32(file, 2);
    Put32(file, 1);
    for (uint16_t i = 0; i < 16; i++)
    {
        Put16(file, static_cast<uint16_t>(i + 1));
    }

    return file;
}

void TestBankFile()
{
    const Bytes file = BankBytes();
    Bytes data = file;
    data.push_back(0x77);

    XSFLIB_CHECK(n64::BankFile::Matches(data));
    const n64::BankFile bank = n64::BankFile::Parse(data);
    XSFLIB_CHECK(bank.banks.size() == 1);
    XSFLIB_CHECK(bank.instruments.size() == 1 && bank.sounds.size() == 1 && bank.waves.size() == 1);
    if (bank.banks.size() != 1 || bank.instruments.size() != 1 || bank.sounds.size() != 1 || bank.waves.size() != 1)
    {
        return;
    }

    const n64::Bank& first = bank.banks[0];
    XSFLIB_CHECK(first.sample_rate == 22050 && first.percussion == 0);
    XSFLIB_CHECK((first.instruments == std::vector<int32_t>{0, n64::kNoObject}));

    const n64::BankInstrument& instrument = bank.instruments[0];
    XSFLIB_CHECK(instrument.volume == 100 && instrument.priority == 5 && instrument.vibrato_delay == 8);
    XSFLIB_CHECK(instrument.bend_range == 200 && instrument.sounds == std::vector<int32_t>{0});

    const n64::BankSound& sound = bank.sounds[0];
    XSFLIB_CHECK(sound.pan == 32 && sound.volume == 96);
    XSFLIB_CHECK(bank.envelopes[0].release_time == -1 && bank.envelopes[0].decay_volume == 80);
    XSFLIB_CHECK(bank.key_maps[0].key_base == 60 && bank.key_maps[0].detune == -5);

    const n64::BankWave& wave = bank.waves[0];
    XSFLIB_CHECK(wave.base == 0x1234 && wave.length == 900 && wave.type == n64::kWaveVadpcm);
    XSFLIB_CHECK(bank.loops[0].start == 16 && bank.loops[0].count == 0xFFFFFFFF && bank.loops[0].state[15] == 45);
    XSFLIB_CHECK(bank.books[0].book.predictor_count == 1 && bank.books[0].book.Coefficient(0, 1, 7) == 16);
    XSFLIB_CHECK(bank.Write() == file);

    // A changed field lands at its place. A new key map goes at the end on a multiple of 8 bytes.
    n64::BankFile edited = bank;
    edited.sounds[0].pan = 90;
    edited.key_maps.push_back({n64::kNoOffset, 1, 2, 3, 4, 5, 6});
    edited.sounds[0].key_map = 1;
    const Bytes written = edited.Write();
    XSFLIB_CHECK(written[0x4C] == 90);
    XSFLIB_CHECK(xsflib::LoadBe32(written.data() + 0x44) == 0xD8);
    XSFLIB_CHECK((Bytes(written.begin() + 0xD8, written.end()) == Bytes{1, 2, 3, 4, 5, 6}));
    const n64::BankFile again = n64::BankFile::Parse(written);
    XSFLIB_CHECK(again.key_maps.size() == 1 && again.key_maps[0].key_base == 5);

    // A list that outgrows its place moves to the end, and the bank points to the new place.
    n64::BankFile grown = bank;
    grown.instruments[0].sounds.push_back(0);
    const Bytes grown_bytes = grown.Write();
    XSFLIB_CHECK(xsflib::LoadBe32(grown_bytes.data() + 0x18) == 0xD8);
    XSFLIB_CHECK(xsflib::LoadBe16(grown_bytes.data() + 0xD8 + 14) == 2);
    const n64::BankFile grown_again = n64::BankFile::Parse(grown_bytes);
    XSFLIB_CHECK(grown_again.instruments.size() == 1 && grown_again.instruments[0].sounds.size() == 2);

    // A bad revision and a damaged wave.
    Bytes bad = file;
    bad[1] = '2';
    XSFLIB_CHECK(!n64::BankFile::Matches(bad));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::BankFile::Parse(bad); }) == "not a bank file of revision B1");

    Bytes bad_type = file;
    bad_type[0x68 + 8] = 7;
    XSFLIB_CHECK(!n64::BankFile::Matches(bad_type));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::BankFile::Parse(bad_type); }) ==
                 "a wave of a bank file has the unknown type 7");
}

// A compressed file with one track: a tempo, a loop start, two notes, a block that repeats 8 bytes of them, a delta
// with a doubled block code, a third note, a loop end and an end of track.
Bytes CompressedBytes()
{
    Bytes file;
    Put32(file, 68); // track 0
    for (int i = 1; i < 16; i++)
    {
        Put32(file, 0);
    }
    Put32(file, 48);

    const Bytes track = {
        0x00, 0xFF, 0x51, 0x07, 0xA1, 0x20, // 68: a tempo of 500,000 microseconds
        0x00, 0xFF, 0x2E, 0x00, 0xFF,       // 74: a loop start
        0x00, 0x90, 0x40, 0x7F, 0x18,       // 79: note 0x40, velocity 0x7F, duration 24
        0x18, 0x41, 0x7F, 0x18,             // 84: a running status: note 0x41 after 24 ticks
        0xFE, 0x00, 0x09, 0x08,             // 88: a block of the 8 bytes at 79, which ends with note 0x41's velocity
        0x18,                               // 92: that note's duration
        0xFE, 0xFE, 0x01,                   // 93: a delta of 0xFE 0x01
        0x42, 0x60, 0x0C,                   // 96: a running status: note 0x42, duration 12
        0x00, 0xFF, 0x2D, 0xFF, 0xFF,       // 99: a loop end, with an offset of 108 - 79 = 29
        0x00, 0x00, 0x00, 0x1D,             //
        0x00, 0xFF, 0x2F,                   // 108: an end of track
    };
    file.insert(file.end(), track.begin(), track.end());

    return file;
}

void TestCompressedMidi()
{
    const Bytes file = CompressedBytes();
    XSFLIB_CHECK(n64::CompressedMidi::Matches(file));
    const n64::CompressedMidi midi = n64::CompressedMidi::Parse(file);
    XSFLIB_CHECK(midi.Division() == 48);
    XSFLIB_CHECK(midi.TrackOffset(0) == 68 && midi.TrackOffset(1) == 0);
    XSFLIB_CHECK((Bytes(midi.Bytes().begin(), midi.Bytes().end()) == file));

    const std::vector<n64::MidiEvent> events = midi.DecodeTrack(0);
    XSFLIB_CHECK(events.size() == 9);
    if (events.size() != 9)
    {
        return;
    }

    XSFLIB_CHECK(events[0].TempoMicroseconds() == 500000 && events[0].file_offset == 68);
    XSFLIB_CHECK(events[1].IsMeta(n64::kCompressedLoopStart));
    XSFLIB_CHECK(events[2].IsNoteOn() && events[2].data1 == 0x40 && events[2].data2 == 0x7F);
    XSFLIB_CHECK(events[2].duration == 24 && !events[2].running_status);
    XSFLIB_CHECK(events[3].delta == 0x18 && events[3].running_status && events[3].data1 == 0x41);

    // The events in the block report the block's source as their places.
    XSFLIB_CHECK(events[4].IsNoteOn() && events[4].delta == 0 && events[4].data1 == 0x40);
    XSFLIB_CHECK(events[4].file_offset == 79);
    XSFLIB_CHECK(events[5].running_status && events[5].file_offset == 84 && events[5].duration == 24);

    XSFLIB_CHECK(events[6].delta == ((0x7EU << 7) | 1) && events[6].data1 == 0x42 && events[6].duration == 12);
    XSFLIB_CHECK(events[6].file_offset == 93);
    XSFLIB_CHECK(events[7].IsMeta(n64::kCompressedLoopEnd) && events[7].loop_start == 1);
    XSFLIB_CHECK(events[8].IsMeta(n64::kMidiMetaEndOfTrack));

    // The expansion holds the block's bytes and one byte of 0xFE.
    const Bytes expanded = midi.ExpandTrack(0);
    XSFLIB_CHECK(expanded.size() == file.size() - 68 + 8 - 4 - 1);
    XSFLIB_CHECK(expanded[29] == 0xFE && expanded[30] == 0x01);

    Bytes unknown_meta = file;
    unknown_meta[68 + 2] = 0x01;
    XSFLIB_CHECK(!n64::CompressedMidi::Matches(unknown_meta));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::CompressedMidi::Parse(unknown_meta); }) ==
                 "a compressed MIDI track holds the unknown meta event 1");

    Bytes cut = file;
    cut.resize(file.size() - 2);
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::CompressedMidi::Parse(cut); }) ==
                 "a compressed MIDI track runs past the end of the file");
}

void TestCompressedBuild()
{
    // A track that repeats a long stretch: Build has to use a block, and the events have to come back.
    std::vector<n64::MidiEvent> track;
    n64::MidiEvent tempo;
    tempo.status = n64::kMidiMeta;
    tempo.type = n64::kMidiMetaTempo;
    tempo.payload = {0x07, 0xA1, 0x20};
    track.push_back(tempo);

    n64::MidiEvent start;
    start.status = n64::kMidiMeta;
    start.type = n64::kCompressedLoopStart;
    start.payload = {0, 0xFF};
    track.push_back(start);
    for (int repeat = 0; repeat < 4; repeat++)
    {
        for (uint8_t key = 60; key < 64; key++)
        {
            n64::MidiEvent note;
            note.delta = 24;
            note.status = 0x90;
            note.data1 = key;
            note.data2 = 100;
            note.duration = 12;
            note.running_status = track.back().status == 0x90;
            track.push_back(note);
        }
    }

    n64::MidiEvent end;
    end.status = n64::kMidiMeta;
    end.type = n64::kCompressedLoopEnd;
    end.payload = {0xFF, 0xFF, 0, 0, 0, 0};
    end.loop_start = 1;
    track.push_back(end);

    n64::MidiEvent finish;
    finish.status = n64::kMidiMeta;
    finish.type = n64::kMidiMetaEndOfTrack;
    track.push_back(finish);

    const std::vector<std::vector<n64::MidiEvent>> tracks = {{}, track};
    const n64::CompressedMidi built = n64::CompressedMidi::Build(tracks, 96);
    XSFLIB_CHECK(built.Division() == 96);
    XSFLIB_CHECK(built.TrackOffset(0) == 0 && built.TrackOffset(1) == 68);

    // 16 notes of 4 or 5 bytes take 65 bytes without blocks.
    XSFLIB_CHECK(built.Bytes().size() < 68 + 65);

    const n64::CompressedMidi parsed = n64::CompressedMidi::Parse(built.Bytes());
    std::vector<n64::MidiEvent> decoded = parsed.DecodeTrack(1);
    XSFLIB_CHECK(decoded.size() == track.size());
    if (decoded.size() != track.size())
    {
        return;
    }

    // The loop end's offset leads back from the end of the event to the bytes after the loop start.
    n64::MidiEvent& loop_end = decoded[decoded.size() - 2];
    XSFLIB_CHECK(loop_end.loop_start == 1);
    XSFLIB_CHECK(xsflib::LoadBe32(loop_end.payload.data() + 2) != 0);
    loop_end.payload = end.payload;
    XSFLIB_CHECK(decoded == track);
}

// Build looks for blocks in the tracks only. The header's track offsets change after the first track goes out, so a
// block that copied them would come back wrong. The first track holds runs of zeros, as the header does.
void TestCompressedBuildSkipsHeader()
{
    std::vector<n64::MidiEvent> track;
    for (int i = 0; i < 20; i++)
    {
        n64::MidiEvent controller;
        controller.status = 0xB0;
        controller.running_status = i != 0;
        track.push_back(controller);
    }

    n64::MidiEvent finish;
    finish.status = n64::kMidiMeta;
    finish.type = n64::kMidiMetaEndOfTrack;
    track.push_back(finish);

    // The other tracks hold only their ends, so that every offset in the header changes.
    std::vector<std::vector<n64::MidiEvent>> tracks(n64::CompressedMidi::kTrackCount, {finish});
    tracks[0] = track;
    const n64::CompressedMidi built = n64::CompressedMidi::Build(tracks, 48);
    XSFLIB_CHECK(n64::CompressedMidi::Parse(built.Bytes()).DecodeTrack(0) == track);
}

// A loop end's bytes go out as they stand, 0xFE included. Every low byte of the offset comes up as the loop grows.
void TestCompressedLoopOffsets()
{
    for (int notes = 1; notes < 300; notes++)
    {
        std::vector<n64::MidiEvent> track;
        n64::MidiEvent start;
        start.status = n64::kMidiMeta;
        start.type = n64::kCompressedLoopStart;
        start.payload = {1, 0xFF};
        track.push_back(start);
        for (int i = 0; i < notes; i++)
        {
            // Values that don't repeat in the same order, which keeps blocks out.
            n64::MidiEvent event;
            event.delta = 1;
            event.status = 0xB0;
            event.data1 = static_cast<uint8_t>(i % 120);
            event.data2 = static_cast<uint8_t>((i * i) % 127);
            track.push_back(event);
        }

        n64::MidiEvent end;
        end.delta = 2;
        end.status = n64::kMidiMeta;
        end.type = n64::kCompressedLoopEnd;
        end.payload = {0xFE, 0xFE, 0, 0, 0, 0};
        end.loop_start = 0;
        track.push_back(end);
        n64::MidiEvent last;
        last.status = n64::kMidiMeta;
        last.type = n64::kMidiMetaEndOfTrack;
        track.push_back(last);

        std::vector<std::vector<n64::MidiEvent>> tracks(n64::CompressedMidi::kTrackCount);
        tracks[0] = track;
        const n64::CompressedMidi built = n64::CompressedMidi::Build(tracks, 48);
        const n64::CompressedMidi parsed = n64::CompressedMidi::Parse(built.Bytes());
        const std::vector<n64::MidiEvent> again = parsed.DecodeTrack(0);
        XSFLIB_CHECK(again.size() == track.size());
        const n64::MidiEvent& loop_end = again[again.size() - 2];
        XSFLIB_CHECK(loop_end.IsMeta(n64::kCompressedLoopEnd));
        XSFLIB_CHECK(loop_end.delta == 2 && loop_end.loop_start == 0);
        XSFLIB_CHECK(loop_end.payload[0] == 0xFE && loop_end.payload[1] == 0xFE);
    }
}

// A game's loop end whose offset ends in 0xFE, followed by an end of track. The player reads the offset as it stands.
void TestCompressedLoopEndBytes()
{
    Bytes file;
    Put32(file, 68);
    for (int i = 1; i < 16; i++)
    {
        Put32(file, 0);
    }
    Put32(file, 48);

    // 68: a loop start. 73: 60 controllers and a note, 245 bytes. 318: a loop end that leads back 0xFE bytes from 327.
    file.insert(file.end(), {0x00, 0xFF, 0x2E, 0x00, 0xFF});
    for (int i = 0; i < 60; i++)
    {
        file.insert(file.end(), {0x00, 0xB0, 0x07, static_cast<uint8_t>(i)});
    }
    file.insert(file.end(), {0x00, 0x90, 0x40, 0x7F, 0x18});
    file.insert(file.end(), {0x00, 0xFF, 0x2D, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFE, 0x00, 0xFF, 0x2F});

    const std::vector<n64::MidiEvent> events = n64::CompressedMidi::Parse(file).DecodeTrack(0);

    XSFLIB_CHECK(events.size() == 64);
    if (events.size() != 64)
    {
        return;
    }

    XSFLIB_CHECK(events[62].IsMeta(n64::kCompressedLoopEnd) && events[62].loop_start == 0);
    XSFLIB_CHECK(events[62].payload == (std::vector<uint8_t>{0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFE}));
    XSFLIB_CHECK(events[63].IsMeta(n64::kMidiMetaEndOfTrack));
}

// A loop end whose FF 2D comes out of a block, with a byte of the block left. The player reads the loop end's six bytes
// at the file's place after the block's code, and the byte left in the block after them.
void TestCompressedLoopEndInBlock()
{
    Bytes file;
    Put32(file, 68);
    for (int i = 1; i < 16; i++)
    {
        Put32(file, 0);
    }
    Put32(file, 48);

    const Bytes track = {
        0x00, 0xFF, 0x2E, 0x00, 0xFF,                         // 68: a loop start
        0x00, 0x90, 0x40, 0x7F, 0x18,                         // 73: note 0x40, velocity 0x7F, duration 24
        0x00, 0xFF, 0x2D, 0x01, 0x01, 0x00, 0x00, 0x00, 0x0E, // 78: a loop end back to 73
        0xFE, 0x00, 0x09, 0x04,                               // 87: a block of the 4 bytes at 78: 00 FF 2D 01
        0x02, 0x03, 0x00, 0x00, 0x00, 0x05,                   // 91: the second loop end's six bytes
        0xFF, 0x2F,                                           // 97: an end of track after the block's last byte
    };
    file.insert(file.end(), track.begin(), track.end());

    const std::vector<n64::MidiEvent> events = n64::CompressedMidi::Parse(file).DecodeTrack(0);

    XSFLIB_CHECK(events.size() == 5);
    if (events.size() != 5)
    {
        return;
    }

    XSFLIB_CHECK(events[2].IsMeta(n64::kCompressedLoopEnd) && events[2].loop_start == 0);
    XSFLIB_CHECK(events[3].IsMeta(n64::kCompressedLoopEnd) && events[3].file_offset == 78);
    XSFLIB_CHECK(events[3].payload == (std::vector<uint8_t>{0x02, 0x03, 0x00, 0x00, 0x00, 0x05}));
    XSFLIB_CHECK(events[4].IsMeta(n64::kMidiMetaEndOfTrack) && events[4].delta == 1);
}

void TestStandardMidi()
{
    Bytes file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 0};
    const Bytes track = {
        0x00, 0xFF, 0x03, 0x02, 'h',  'i',                                // a name
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, 0x00, 0x90, 0x3C, 0x40, // a tempo, then a note on
        0x81, 0x40, 0x3C, 0x00,                                           // running status, after 192 ticks
        0x00, 0xB0, 0x66, 0x00,                                           // the loop start's controller
        0x00, 0xF0, 0x01, 0xF7,                                           // a system exclusive
        0x00, 0xC1, 0x05,                                                 // a program change
        0x00, 0xFF, 0x2F, 0x00,                                           // end of track
    };
    xsflib::StoreBe32(file.data() + 18, static_cast<uint32_t>(track.size()));
    file.insert(file.end(), track.begin(), track.end());
    Bytes data = file;
    data.insert(data.end(), {0x12, 0x34});

    XSFLIB_CHECK(n64::StandardMidi::Matches(data));
    const n64::StandardMidi midi = n64::StandardMidi::Parse(data);
    XSFLIB_CHECK(midi.Format() == 0 && midi.Division() == 480 && midi.TrackCount() == 1);
    XSFLIB_CHECK((Bytes(midi.Bytes().begin(), midi.Bytes().end()) == file));

    const std::vector<n64::MidiEvent> events = midi.DecodeTrack(0);
    XSFLIB_CHECK(events.size() == 8);
    if (events.size() != 8)
    {
        return;
    }

    XSFLIB_CHECK(events[1].TempoMicroseconds() == 500000);
    XSFLIB_CHECK(events[3].delta == 192 && events[3].running_status && events[3].data2 == 0);
    XSFLIB_CHECK(events[3].file_offset == 22 + 17);
    XSFLIB_CHECK(events[4].data1 == n64::kMidiLoopStartController);
    XSFLIB_CHECK(events[5].status == 0xF0 && events[5].payload == Bytes{0xF7});
    XSFLIB_CHECK(events[6].status == 0xC1 && events[6].data1 == 5);

    const std::vector<std::vector<n64::MidiEvent>> tracks = {events};
    const n64::StandardMidi built = n64::StandardMidi::Build(0, 480, tracks);
    XSFLIB_CHECK((Bytes(built.Bytes().begin(), built.Bytes().end()) == file));

    Bytes bad = file;
    bad[0] = 'X';
    XSFLIB_CHECK(!n64::StandardMidi::Matches(bad));

    // A header that runs past the data, in a file of no tracks.
    const Bytes long_header = {'M', 'T', 'h', 'd', 0, 0, 0, 0x10, 0, 0, 0, 0, 1, 0xE0};
    XSFLIB_CHECK(!n64::StandardMidi::Matches(long_header));
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::StandardMidi::Parse(bad); }) == "not a standard MIDI file");
}

// A book of two predictors. Predictor 0 predicts nothing. Predictor 1 uses a first row and a second row whose values
// are easy to follow by hand.
n64::VadpcmBook SimpleBook()
{
    n64::VadpcmBook book;
    book.predictor_count = 2;
    book.coefficients.assign(32, 0);

    // Predictor 1's sample 0 takes half the sample before the group's last and the whole of the last.
    book.coefficients[16 + 0] = 1024;
    book.coefficients[16 + 8] = 2048;

    // Predictor 1's sample 1 takes the last sample. It also adds the first residual, as the second row's first value.
    book.coefficients[16 + 9] = 2048;

    return book;
}

void TestVadpcmFrame()
{
    const std::array<uint8_t, n64::kVadpcmFrameSize> bytes = {0x31, 0x1F, 0x70, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    const n64::VadpcmFrame frame = n64::VadpcmFrame::Parse(bytes);
    XSFLIB_CHECK(frame.scale == 3 && frame.predictor == 1);
    XSFLIB_CHECK(frame.residuals[0] == 1 && frame.residuals[1] == -1 && frame.residuals[2] == 7);
    XSFLIB_CHECK(frame.residuals[3] == 0 && frame.residuals[4] == -8);
    XSFLIB_CHECK(frame.Write() == bytes);
}

void TestVadpcmDecode()
{
    // Scale 2, predictor 0: the samples are the residuals times 4.
    const n64::VadpcmBook book = SimpleBook();
    const Bytes plain = {0x20, 0x12, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    const std::vector<int16_t> samples = n64::DecodeVadpcm(plain, book);
    XSFLIB_CHECK(samples.size() == 16);
    XSFLIB_CHECK(samples[0] == 4 && samples[1] == 8 && samples[2] == 12 && samples[3] == -4 && samples[15] == 0);
    XSFLIB_CHECK(samples[14] == -32);

    // Predictor 1, scale 0, after a frame that ended in 0 and 100.
    n64::VadpcmState state = {};
    state[14] = 50;
    state[15] = 100;
    const Bytes predicted = {0x01, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const std::vector<int16_t> out = n64::DecodeVadpcm(predicted, book, state);

    // Sample 0: 50 / 2 + 100 + 1 = 126. Sample 1: 100 + 2 + the first residual 1 = 103. Sample 2 holds the two earlier
    // residuals.
    XSFLIB_CHECK(out[0] == 126 && out[1] == 103 && out[2] == 3);

    // A frame's predictor has to be in the book.
    const Bytes bad_predictor = {0x05, 0, 0, 0, 0, 0, 0, 0, 0};
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::DecodeVadpcm(bad_predictor, book); }) ==
                 "a VADPCM frame names predictor 5 of a book of 2");

    // The microcodes take an order of 2 alone.
    n64::VadpcmBook other = book;
    other.order = 3;
    XSFLIB_CHECK(!other.Usable());
}

void TestVadpcmSmall()
{
    // Scale 3, predictor 0: the samples are the residuals times 8. The residuals take 2 bits each, the high bits first.
    const n64::VadpcmBook book = SimpleBook();
    const Bytes plain = {0x30, 0x1B, 0xE4, 0x00, 0xFF};
    const std::vector<int16_t> samples = n64::DecodeVadpcmSmall(plain, book);
    const std::vector<int16_t> expected = {0, 8, -16, -8, -8, -16, 8, 0, 0, 0, 0, 0, -8, -8, -8, -8};
    XSFLIB_CHECK(samples == expected);

    // Predictor 1, scale 0, after a frame that ended in 50 and 100, with the residuals 1, -2 and 0.
    n64::VadpcmState state = {};
    state[14] = 50;
    state[15] = 100;
    const Bytes predicted = {0x01, 0x60, 0x00, 0x00, 0x00};
    const std::vector<int16_t> out = n64::DecodeVadpcmSmall(predicted, book, state);

    // Sample 0: 50 / 2 + 100 + 1 = 126. Sample 1: 100 + 1 - 2 = 99. Sample 2: 1 - 2 + 0 = -1.
    XSFLIB_CHECK(out[0] == 126 && out[1] == 99 && out[2] == -1);

    // Bytes after the last whole frame stay out.
    const Bytes extra = {0x30, 0x1B, 0xE4, 0x00, 0xFF, 0x30};
    XSFLIB_CHECK(n64::DecodeVadpcmSmall(extra, book).size() == 16);

    // The scales stop at 14 for a small frame, and at 12 for a frame.
    const Bytes high_small = {0xF0, 0x40, 0x00, 0x00, 0x00};
    XSFLIB_CHECK(n64::DecodeVadpcmSmall(high_small, book)[0] == 16384);
    const Bytes high = {0xF0, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    XSFLIB_CHECK(n64::DecodeVadpcm(high, book)[0] == 4096);

    const Bytes bad_predictor = {0x05, 0, 0, 0, 0};
    XSFLIB_CHECK(ErrorOf<xsflib::FormatError>([&] { n64::DecodeVadpcmSmall(bad_predictor, book); }) ==
                 "a VADPCM frame names predictor 5 of a book of 2");
}

void TestVadpcmEncode()
{
    // Samples from the decoder encode to the same frames.
    const n64::VadpcmBook book = SimpleBook();
    const Bytes frames = {0x20, 0x12, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                          0x31, 0x12, 0x03, 0x70, 0x00, 0xF0, 0x00, 0x10, 0x00};
    const std::vector<int16_t> samples = n64::DecodeVadpcm(frames, book);
    XSFLIB_CHECK(n64::EncodeVadpcm(samples, book) == frames);

    // Samples that no frame gives exactly come out close. A step of 1000 fits scale 7 and residual 8, near enough.
    std::vector<int16_t> rough(16, 0);
    rough[0] = 1000;
    const std::vector<uint8_t> encoded = n64::EncodeVadpcm(rough, book);
    XSFLIB_CHECK(encoded.size() == n64::kVadpcmFrameSize);
    const std::vector<int16_t> decoded = n64::DecodeVadpcm(encoded, book);
    XSFLIB_CHECK(decoded.size() == 16 && std::abs(decoded[0] - 1000) < 256);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("n64_formats",
                                    []
                                    {
                                        TestSeqFile();
                                        TestHudsonRecords();
                                        TestBankFile();
                                        TestCompressedMidi();
                                        TestCompressedBuild();
                                        TestCompressedBuildSkipsHeader();
                                        TestCompressedLoopOffsets();
                                        TestCompressedLoopEndBytes();
                                        TestCompressedLoopEndInBlock();
                                        TestStandardMidi();
                                        TestVadpcmFrame();
                                        TestVadpcmDecode();
                                        TestVadpcmSmall();
                                        TestVadpcmEncode();
                                    });
}
