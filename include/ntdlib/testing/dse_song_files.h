// SPDX-License-Identifier: MIT

// The song, its bank, the main bank that holds the bank's samples and a builder of banks of sound effects, which the
// tests of DSE's driver and of what plays on it share. dse_driver_test.cpp's data hold the commands that the driver's
// code in Pokémon Mystery Dungeon: Explorers of Sky's ARM9 binary (North America) sent for these files.

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/testing/dse_files.h"

namespace ntdlib::testing::dse_song_files
{

using dse_files::BankChunk;
using dse_files::Builder;
using dse_files::Bytes;
using dse_files::HeaderStart;
using dse_files::SequenceInfoBytes;
using dse_files::TrackChunk;

// The song bank's id, which the tracks select with events 0xA9 and 0xAA.
inline constexpr uint16_t kBankId = 0x7F00;

inline Bytes Join(std::initializer_list<Bytes> parts)
{
    Bytes bytes;
    for (const Bytes& part : parts)
    {
        bytes.insert(bytes.end(), part.begin(), part.end());
    }

    return bytes;
}

// A note of `velocity` on `key` (0 to 11) after a change of `octave` (-2 to 1), with a length of `length_bytes` bytes,
// the high byte first. A note without length bytes takes the track's previous length.
inline Bytes Note(uint8_t velocity, int key, int octave, uint32_t length = 0, int length_bytes = 0)
{
    Bytes bytes = {velocity, static_cast<uint8_t>(length_bytes << 6 | (octave + 2) << 4 | key)};
    for (int i = length_bytes - 1; i >= 0; i--)
    {
        bytes.push_back(static_cast<uint8_t>(length >> (8 * i)));
    }

    return bytes;
}

// A wave's parameters: `format` 0 to 4 (PCM8, PCM16, IMA-ADPCM, PSG, noise), with its loop and its place in the main
// bank's samples, in 32-bit words.
inline Bytes WaveEntry(uint16_t id, uint8_t format, bool loops, uint32_t rate, uint32_t position, uint32_t loop_start,
                       uint32_t loop_length, uint8_t duty = 0)
{
    Builder b;
    b.Append({0x01, 0xAA}).U16(id).Append({0x30, 0xF9, 0x3C, 0x00, 0x7F, 0x40, 0x00, 0x02, 0x00, 0x00, 0xAA, 0xAA});
    b.U16(0x0415)
        .U16(static_cast<uint16_t>(format << 8))
        .Append({0x09, static_cast<uint8_t>(loops ? 1 : 0), 0x01, 0x08});
    b.Append({0x00, duty, 0x01, 0x01}).U32(0).U32(rate).U32(position).U32(loop_start).U32(loop_length);
    b.Append({0x01, 0x01, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x7F, 0x28, 0xFF});

    return b.Take();
}

// An envelope's 16 bytes: the multiplier, the attack's start level, the attack, the decay, the sustain level, the
// hold, the sustain's time and the release.
struct Envelope
{
    uint8_t multiplier = 1;
    uint8_t attack_begin = 0;
    uint8_t attack = 0;
    uint8_t decay = 0;
    uint8_t sustain = 0x7F;
    uint8_t hold = 0;
    uint8_t sustain_time = 0x7F;
    uint8_t release = 0x28;
};

// A split of a program: the keys and velocities that it holds, the wave, the tuning, the root key's change of
// octave, the volume, the pan, the key group and the envelope.
struct Split
{
    uint8_t low_key = 0;
    uint8_t high_key = 0x7F;
    uint8_t low_velocity = 0;
    uint8_t high_velocity = 0x7F;
    uint16_t wave = 0;
    int16_t tuning = 0;
    int8_t coarse = 0;
    uint8_t volume = 0x7F;
    uint8_t pan = 0x40;
    uint8_t key_group = 0;
    uint8_t bend_range = 2;
    Envelope envelope = {};
};

// An LFO of a program: its type, its output, its waveform, its amplitude, and its phase, delay and envelope in ms.
struct Lfo
{
    uint8_t type = 0;
    uint8_t output = 0;
    uint8_t waveform = 1;
    int32_t amplitude = 0;
    uint16_t phase = 0;
    uint16_t delay = 0;
    uint16_t envelope = 0;
};

inline Bytes Program(uint16_t id, uint8_t volume, std::initializer_list<Lfo> lfos, std::initializer_list<Split> splits)
{
    Builder b;
    b.U16(id).U16(static_cast<uint16_t>(splits.size())).Append({volume, 0x40, 0x00, 0x0F, 0x00, 0x02, 0x00, 0x04});
    b.Fill(4, 0);
    for (std::size_t i = 0; i < 4; i++)
    {
        const Lfo lfo = i < lfos.size() ? lfos.begin()[i] : Lfo{};
        b.Append({0x00, lfo.type, lfo.output, lfo.waveform}).U32(static_cast<uint32_t>(lfo.amplitude)).U16(lfo.phase);
        b.U16(lfo.delay).U16(lfo.envelope).Fill(2, 0);
    }
    b.Fill(16, 0);
    uint8_t index = 0;
    for (const Split& s : splits)
    {
        b.Append({0x00, index++, s.bend_range, 0x01, s.low_key, s.high_key, s.low_key, s.high_key, s.low_velocity,
                  s.high_velocity, s.low_velocity, s.high_velocity});
        b.Fill(6, 0).U16(s.wave).U16(static_cast<uint16_t>(s.tuning)).Append({0x3C, static_cast<uint8_t>(s.coarse)});
        b.Append({s.volume, s.pan, s.key_group, 0x02}).Fill(4, 0);
        const Envelope& e = s.envelope;
        b.Append({0x01, e.multiplier, 0x01, 0x03, 0x03, 0xFF, 0xFF, 0xFF, e.attack_begin, e.attack, e.decay, e.sustain,
                  e.hold, e.sustain_time, e.release, 0xFF});
    }

    return b.Take();
}

// A table of 16-bit offsets with `slots` slots, padded to `size` bytes with `pad`, then the entries that it names in
// their order. A slot without an entry is 0.
inline Bytes OffsetTable(std::size_t slots, std::size_t size, const std::vector<std::pair<std::size_t, Bytes>>& entries,
                         uint8_t pad)
{
    Builder b;
    std::vector<uint16_t> offsets(slots, 0);
    std::size_t at = size;
    for (const auto& [slot, entry] : entries)
    {
        offsets[slot] = static_cast<uint16_t>(at);
        at += entry.size();
    }
    for (const uint16_t offset : offsets)
    {
        b.U16(offset);
    }
    b.Fill(size - slots * 2, pad);
    for (const auto& entry : entries)
    {
        b.Append(entry.second);
    }

    return b.Take();
}

// A table of 16-bit offsets with `slots` slots, padded to 16 bytes with `pad`, then the entries that it names in their
// order. The tools pad a table of waves with 0xAA, and a table of programs with zeros.
inline Bytes Table(std::size_t slots, const std::vector<std::pair<std::size_t, Bytes>>& entries, uint8_t pad)
{
    return OffsetTable(slots, AlignUp(slots * 2, 16), entries, pad);
}

// A bank's header from 0x30 on: the size of its samples, its slots of waves and programs, its key groups, its kind
// (1 for a main bank, 2 for a bank whose waves lie in the main bank) and the size of its wavi chunk.
inline void BankHeader(Builder& b, uint32_t samples, uint16_t waves, uint8_t key_groups, uint8_t kind, uint32_t wavi)
{
    b.Append({0x00, 0xAA, 0xAA, 0xAA}).U32(0).U32(0).U32(0x10).U32(samples).U16(0).U16(waves).U16(0x80);
    b.Append({key_groups, kind}).U32(wavi);
}

// The main bank's waves: wave 0 is PCM16 and loops, wave 1 is PCM8 without a loop, and wave 2 is IMA-ADPCM and loops.
// Each wave's samples follow the previous wave's.
inline const std::vector<std::pair<std::size_t, Bytes>>& MainWaves()
{
    static const std::vector<std::pair<std::size_t, Bytes>> kWaves = {
        {0, WaveEntry(0, 1, true, 32000, 0x000, 4, 60)},
        {1, WaveEntry(1, 0, false, 16000, 0x100, 0, 32)},
        {2, WaveEntry(2, 2, true, 22050, 0x180, 9, 23)},
    };

    return kWaves;
}

inline Bytes MainBankFile()
{
    const Bytes wavi = Table(4, MainWaves(), 0xAA);
    Builder samples;
    for (int i = 0; i < 0x200; i++)
    {
        samples.Append({static_cast<uint8_t>(i * 7), static_cast<uint8_t>(i * 13 + 5)});
    }
    const Bytes pcmd = samples.Take();

    Builder b;
    HeaderStart(b, "swdl", 0x0000);
    BankHeader(b, static_cast<uint32_t>(pcmd.size()), 4, 5, 1, static_cast<uint32_t>(wavi.size()));
    BankChunk(b, "wavi", static_cast<uint32_t>(wavi.size())).Append(wavi);
    BankChunk(b, "pcmd", static_cast<uint32_t>(pcmd.size())).Append(pcmd);
    BankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));

    return b.Take();
}

// The song's bank. Its waves 0 to 2 lie in the main bank. Wave 3 is the PSG at duty 3, wave 4 the noise, and wave 5 an
// IMA-ADPCM wave that the main bank lacks. Program 0 has one split. Program 1 has two splits that overlap in keys 48 to
// 63, in key group 1, which holds two voices of a channel. Program 2 plays the PSG below key 64 and the noise from
// it, in key group 2, which takes voices 8 to 13 at a lower priority, with three LFOs. Program 3 plays wave 5 with a
// release past the table of times.
inline Bytes BankFile()
{
    std::vector<std::pair<std::size_t, Bytes>> waves = MainWaves();
    waves.insert(waves.end(), {
                                  {3, WaveEntry(3, 3, true, 8000, 0, 0, 0, 3)},
                                  {4, WaveEntry(4, 4, true, 8000, 0, 0, 0)},
                                  {5, WaveEntry(5, 2, false, 11025, 0x1A0, 0, 16)},
                              });
    const Bytes wavi = Table(6, waves, 0xAA);
    const Envelope slow = {.attack_begin = 0x20, .attack = 0x10, .decay = 0x20, .sustain = 0x60, .release = 0x28};
    const Envelope unscaled = {.multiplier = 0,
                               .attack = 0x08,
                               .decay = 0x0C,
                               .sustain = 0x50,
                               .hold = 0x04,
                               .sustain_time = 0x30,
                               .release = 0x20};
    const Envelope past_table = {.decay = 0x18, .sustain = 0x40, .release = 0x85};
    const Bytes prgi = Table(
        128,
        {
            {0, Program(0, 0x7F, {}, {{.wave = 0, .tuning = -1742, .coarse = -12, .volume = 0x6E, .envelope = slow}})},
            {1, Program(1, 0x70, {},
                        {{.high_key = 63, .wave = 1, .tuning = 100, .pan = 0x20, .key_group = 1, .envelope = unscaled},
                         {.low_key = 48, .wave = 2, .tuning = -300, .coarse = -1, .pan = 0x60, .key_group = 1}})},
            {2,
             Program(
                 2, 0x7F,
                 {{.type = 1, .output = 1, .waveform = 3, .amplitude = 0x80, .phase = 40, .delay = 20, .envelope = 30},
                  {.type = 3, .output = 2, .waveform = 6, .amplitude = 0x40, .phase = 20},
                  {.type = 1, .output = 3, .waveform = 5, .amplitude = 0x20, .phase = 100}},
                 {{.high_key = 63, .wave = 3, .key_group = 2, .bend_range = 12},
                  {.low_key = 64, .low_velocity = 0x20, .wave = 4, .key_group = 2}})},
            {3, Program(3, 0x60, {}, {{.wave = 5, .tuning = 50, .envelope = past_table}})},
        },
        0);
    const Bytes kgrp = Join({{0, 0, 0xFF, 8, 0, 0xFF, 0, 0}, {1, 0, 2, 8, 0, 15, 0, 0}, {2, 0, 0xFF, 4, 8, 13, 0, 0}});

    Builder b;
    HeaderStart(b, "swdl", kBankId);
    BankHeader(b, ntdlib::dse::WaveBank::kNoSamples, 6, 3, 2, static_cast<uint32_t>(wavi.size()));
    BankChunk(b, "wavi", static_cast<uint32_t>(wavi.size())).Append(wavi);
    BankChunk(b, "prgi", static_cast<uint32_t>(prgi.size())).Append(prgi);
    BankChunk(b, "kgrp", static_cast<uint32_t>(kgrp.size())).Append(kgrp).Align(16);
    BankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));

    return b.Take();
}

// The song's tracks, each with its preamble.
inline std::vector<Bytes> Tracks()
{
    const Bytes select = {0xA9, 0x7F, 0xAA, 0x00};
    return {
        // Track 0 sets the tempo, changes it, and starts a fade of the song's volume that no tick runs.
        Join({{0, 0, 0, 0, 0xA4, 0x78, 0x99, 0x93, 0xC0, 0x00, 0xA5, 0x64, 0xAF, 0x00, 0x01, 0x50, 0x93, 0xC0, 0x00},
              {0xA4, 0x78, 0x98}}),
        // Track 1 plays notes with pauses, waits, a legato, the tuning's events and fades, the bend LFO, a sub-loop
        // with a break and the length's multiplier.
        Join({{1, 1, 0, 0},
              select,
              {0xAC, 0x00, 0xE0, 0x64, 0xE8, 0x40, 0xE3, 0x7F, 0xA0, 0x04, 0x99},
              Note(0x50, 0, 0, 24, 1),
              {0x85},
              Note(0x60, 4, 0),
              {0x92, 0x0C, 0x90, 0x91, 0xFA, 0xC0},
              Note(0x60, 7, 0, 0x30, 2),
              {0xD4, 0x30, 0x00, 0x02, 0x92, 0x18, 0xD0, 0xFE, 0xD2, 0x10, 0xD3, 0x00, 0x01, 0xD1, 0x01},
              {0xE2, 0x30, 0x00, 0x40, 0xEA, 0x30, 0x00, 0x10},
              Note(0x70, 0, 1, 0x20, 1),
              {0x92, 0x30, 0xDC, 0x40, 0x00, 0x32, 0x00, 0x03, 0xDD, 0x0A, 0x00, 0x14, 0x00},
              Note(0x60, 2, -1, 0x30, 1),
              {0x92, 0x30, 0xDF, 0x00, 0x9C, 0x03},
              Note(0x40, 0, 0, 0x0C, 1),
              {0x92, 0x0C, 0x9E, 0x92, 0x06, 0x9D, 0xE1, 0xF0, 0xE9, 0x08, 0xBC, 0x40},
              Note(0x60, 0, 0, 0x30, 1),
              {0x92, 0x30, 0xBC, 0x7F, 0x98}}),
        // Track 2 plays chords on program 1, holds a note past its end with the damper, strikes its key again, and
        // releases the damper while the new note plays. It then plays random keys, jitter and a key bend.
        Join({{2, 2, 0, 0},
              select,
              {0xAC, 0x01, 0xE0, 0x7F, 0xE8, 0x40, 0x99},
              Note(0x50, 0, 0, 0x60, 1),
              Note(0x50, 4, 0),
              Note(0x50, 7, 0),
              Note(0x50, 11, 0),
              {0x92, 0x60, 0xBF, 0x7F},
              Note(0x60, 0, 1, 0x0C, 1),
              {0x92, 0x30},
              Note(0x60, 0, 0, 0x30, 1),
              {0x92, 0x18, 0xBF, 0x00, 0x92, 0x18, 0xD5, 0x3C, 0x30},
              Note(0x60, 0, -1, 0x18, 1),
              {0x92, 0x18, 0xD5, 0x00, 0x00, 0xD6, 0x20, 0x00},
              Note(0x60, 0, 0),
              {0x92, 0x18, 0xD6, 0x00, 0x00, 0xD7, 0x10, 0x00, 0xDB, 0x0C},
              Note(0x60, 0, 0),
              {0x92, 0x18, 0xD7, 0x00, 0x00, 0x98}}),
        // Track 3 plays the PSG and the noise with the volume LFO, the general LFO's events, the envelope's
        // overrides, the LFO level and a wait for the voices to fade, then program 3.
        Join({{3, 3, 0, 0},
              select,
              {0xAC, 0x02, 0xE0, 0x60, 0xE8, 0x50, 0xA0, 0x04, 0x99},
              {0xE4, 0x20, 0x00, 0x40, 0x00, 0x00, 0xE5, 0x00, 0x00, 0x0A, 0x00},
              Note(0x60, 0, 0, 0x30, 1),
              {0x92, 0x30, 0xE7, 0x00, 0xF3, 0x03, 0x01, 0x03, 0xF0, 0x10, 0x00, 0x20, 0x00, 0x04},
              {0xF1, 0x05, 0x00, 0x05, 0x00, 0xF2, 0x05, 0x08, 0xF2, 0x06, 0x0A},
              Note(0x60, 0, 1),
              {0x92, 0x30},
              Note(0x60, 0, 1),
              {0x92, 0x30, 0xF3, 0x03, 0x00, 0x00},
              {0xB1, 0x40, 0xB2, 0x10, 0xB3, 0x05, 0xB4, 0x20, 0xFF, 0xB5, 0x30, 0xB6, 0x10},
              Note(0x60, 0, -2),
              {0x92, 0x30, 0xB0, 0xBE, 0x40, 0xA1, 0xFF},
              Note(0x60, 0, 0),
              {0x92, 0x30, 0x95, 0x0C, 0xAC, 0x03},
              Note(0x60, 0, 1, 0x18, 1),
              {0x92, 0x30, 0xAC, 0x02, 0xA0, 0x04, 0x98}}),
        // Track 4 plays three notes on program 0 and ends without a loop.
        Join({{4, 4, 0, 0},
              select,
              {0xAC, 0x00, 0xE0, 0x50, 0xE8, 0x60},
              Note(0x7F, 9, 0, 0x60, 1),
              {0x92, 0x10},
              Note(0x7F, 5, 0),
              {0x92, 0x10},
              Note(0x7F, 2, 0),
              {0x92, 0x60, 0x98}}),
    };
}

inline Bytes SongFile(const std::vector<Bytes>& tracks = Tracks())
{
    Builder b;
    HeaderStart(b, "smdl", kBankId);
    b.U32(1).U32(1).U32(0xFFFFFFFF).U32(0xFFFFFFFF);
    b.Text("song").Append({0x00, 0x00, 0x00, 0x01, 0x10, 0xFF, 0x00, 0x00, 0xB0, 0xFF, 0xFF, 0xFF});
    b.Append(SequenceInfoBytes(static_cast<uint8_t>(tracks.size()), 16));
    for (const Bytes& track : tracks)
    {
        TrackChunk(b, "trk ", static_cast<uint32_t>(track.size())).Append(track).Align(4, 0x98);
    }
    TrackChunk(b, "eoc ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));

    return b.Take();
}

// A sound effect: its tracks, each with its preamble, and its settings' bytes 0x18 (bit 0 refuses an effect in place
// of a stop of the oldest instance), 0x1A (the group), 0x1B (the priority) and 0x1C (the most instances). `slots` is
// the count of tracks that the settings give, or 0 for the count of `tracks`. `banks` gives the ids of its bank list.
struct Effect
{
    std::vector<Bytes> tracks;
    uint8_t refuse = 0;
    uint8_t group = 0;
    uint8_t priority = 13;
    uint8_t most = 0xFF;
    uint8_t slots = 0;
    std::vector<uint16_t> banks = {kBankId};
};

// A table of records with a slot for each record.
inline Bytes Records(const std::vector<Bytes>& records)
{
    std::vector<std::pair<std::size_t, Bytes>> entries;
    for (std::size_t slot = 0; slot < records.size(); slot++)
    {
        entries.emplace_back(slot, records[slot]);
    }

    return OffsetTable(records.size(), 2 * records.size(), entries, 0);
}

// A bank of sound effects of id `id`. Each effect has a record of zeros in mcrl, and its list of banks in bnkl. An
// empty slot has the list of the song's bank.
inline Bytes EffectsFile(const std::vector<std::optional<Effect>>& effects, uint16_t id)
{
    // The table's slots, padded to 16 bytes, then each effect's settings, tracks and eoc chunk.
    std::vector<std::pair<std::size_t, Bytes>> sequences;
    std::vector<Bytes> macros;
    std::vector<Bytes> bank_lists;
    for (std::size_t slot = 0; slot < effects.size(); slot++)
    {
        const std::optional<Effect>& effect = effects[slot];
        macros.push_back({0, 0});
        Builder list;
        const std::vector<uint16_t> banks = effect ? effect->banks : std::vector<uint16_t>{kBankId};
        list.U16(static_cast<uint16_t>(banks.size())).U16(0);
        for (const uint16_t bank : banks)
        {
            list.U16(bank);
        }
        bank_lists.push_back(list.Take());

        if (!effect)
        {
            continue;
        }

        const auto count = static_cast<uint8_t>(effect->slots != 0 ? effect->slots : effect->tracks.size());
        Bytes info = SequenceInfoBytes(count, count);
        info[0x18] = effect->refuse;
        info[0x19] = 4;
        info[0x1A] = effect->group;
        info[0x1B] = effect->priority;
        info[0x1C] = effect->most;

        Builder sequence;
        sequence.Append(info);
        for (const Bytes& track : effect->tracks)
        {
            TrackChunk(sequence, "trk ", static_cast<uint32_t>(track.size())).Append(track).Align(4, 0x98);
        }
        TrackChunk(sequence, "eoc ", 0);
        sequences.emplace_back(slot, sequence.Take());
    }

    const Bytes seq = Table(effects.size(), sequences, 0);
    const Bytes mcrl = Records(macros);
    const Bytes bnkl = Records(bank_lists);

    Builder b;
    HeaderStart(b, "sedl", id);
    b.U16(static_cast<uint16_t>(effects.size())).Append({0x01, 0x00, 0x00, 0x01, 0x00, 0x00}).Fill(8, 0xFF);
    BankChunk(b, "seq ", static_cast<uint32_t>(seq.size())).Append(seq).Align(16);
    BankChunk(b, "mcrl", static_cast<uint32_t>(mcrl.size())).Append(mcrl).Align(16);
    BankChunk(b, "bnkl", static_cast<uint32_t>(bnkl.size())).Append(bnkl).Align(16);
    BankChunk(b, "eod ", 0);
    b.Put32(0x08, static_cast<uint32_t>(b.Size()));

    return b.Take();
}

} // namespace ntdlib::testing::dse_song_files
