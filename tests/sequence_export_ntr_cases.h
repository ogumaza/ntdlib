// SPDX-License-Identifier: MIT

// The bank, the waves and the archives that the tests of the DS's conversion (ntdlib/sequence_export/ntr) share, and
// the helpers that read a performance's events.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"
#include "xsflib/testing/check.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/ntr/performance.h"
#include "ntdlib/testing/sdat_files.h"

namespace sequence_export_ntr_cases
{

namespace files = ntdlib::testing::sdat_files;
namespace sdat = ntdlib::sdat;
using namespace ntdlib::sequence_export::ntr;
using files::Bytes;
using seqlib::midi::EventKind;
using xsflib::testing::Check;

constexpr uint16_t kNone = 0xFFFF;

// The bank's instruments, by program:
//   0: wave 0, a PCM8 square that loops
//   1: the PSG at duty 3, at root key 69
//   2: the noise
//   3: a drum set from key 36: wave 0, the PSG at duty 1 and the noise
//   4: a key split up to keys 48, 40 and 127: wave 0, wave 1 and the PSG at duty 0. The second region never plays.
//   5: a wave at an address that the game's code sets
//   6: none
//   7: wave 1 with an attack of 100, a decay of 30, a sustain of 64, a release of 0xFF and a pan of 0
inline Bytes TestBank()
{
    return files::Bank({
        {.type = 1, .region = {.wave = 0}},
        {.type = 2, .region = {.wave = 3, .root_key = 69}},
        {.type = 3},
        {.type = 16, .min_key = 36, .regions = {{.wave = 0, .root_key = 36}, {.type = 2, .wave = 1}, {.type = 3}}},
        {.type = 17, .split_keys = {48, 40, 127}, .regions = {{.wave = 0}, {.wave = 1}, {.type = 2, .wave = 0}}},
        {.type = 4},
        {},
        {.type = 1, .region = {.wave = 1, .attack = 100, .decay = 30, .sustain = 64, .release = 0xFF, .pan = 0}},
    });
}

// The waves: 0, a PCM8 square at about 32,768 Hz that loops from its start, and 1, eight PCM16 samples at about
// 16,384 Hz that play once. A wave's length is its loop's start and length, in 32-bit words.
inline Bytes TestWaveArchive()
{
    const files::Wave square{
        .loop = 1,
        .loop_length = 4,
        .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    files::Wave ramp{.encoding = 1, .sample_rate = 16384, .loop_length = 4};
    for (int i = 0; i < 8; i++)
    {
        files::Append16(ramp.data, static_cast<uint32_t>(i * 0x1000));
    }

    return files::WaveArchive({square, ramp});
}

// An archive whose sequence 0, `name`, plays `bytecode` on `bank`, on player 0, at volume `volume`. The player may
// take the channels of `channels`, or any channel for 0.
inline Bytes TestArchiveBytes(const Bytes& bytecode, const std::string& name = "BGM_TEST", uint16_t channels = 0,
                              uint8_t volume = 127, const Bytes& bank = TestBank())
{
    sdat::ArchiveModel model;
    sdat::Symbols symbols;
    symbols.names[0] = {name};
    symbols.names[2] = {"BANK_TEST"};
    symbols.names[3] = {"WAVE_TEST"};
    symbols.names[4] = {"PLAYER_BGM"};
    model.symbols = symbols;
    model.sequences = {sdat::SequenceInfo{0, 0, volume, 64, 64, 0, 0, ""}};
    model.banks = {sdat::BankInfo{1, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{2, ""}};
    model.players = {sdat::PlayerInfo{1, 0, channels, 0, ""}};
    model.files = {files::Sequence(bytecode), bank, TestWaveArchive()};

    return sdat::BuildArchive(model);
}

struct TestArchive
{
    explicit TestArchive(const Bytes& bytecode, uint16_t channels = 0, uint8_t volume = 127)
        : bytes(TestArchiveBytes(bytecode, "BGM_TEST", channels, volume)), archive(sdat::Archive::Parse(bytes))
    {
    }

    Bytes bytes;
    sdat::Archive archive;
};

// Note 60 at velocity 127 for 48 ticks, a wait of 48 ticks, and the end.
inline const Bytes kOneNote = {0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

// Note 60 for 24 ticks and a wait of 24 ticks, in a loop: a jump to the start.
inline const Bytes kLoop = {0x3C, 0x7F, 0x18, 0x80, 0x18, 0x94, 0x00, 0x00, 0x00};

// A program change to the program in variable 0 (the prefix 0xA1 takes the argument from a variable), then note 60 for
// 48 ticks, a wait of 48 ticks and the end.
inline const Bytes kProgramFromVariable = {0xA1, 0x81, 0x00, 0x3C, 0x7F, 0x30, 0x80, 0x30, 0xFF};

// The driver's command that a game's code sends for NNS_SndPlayerSetTrackVolume: the tracks of `tracks` of player 0
// take the fader `level` (SND_CalcDecibel's level of the volume, in 0.1 dB). Pokémon Black's code at 0x02005F24 sends
// the command in that form: {0x02000000, tracks, 0x0A, 0xFFFF8000} for volume 0, and 0 in place of 0xFFFF8000 for
// 127.
inline ntdlib::ntr::snd::Command TrackVolume(uint32_t tracks, int16_t level)
{
    return {.id = ntdlib::ntr::snd::CommandId::kTrackParameter,
            .args = {0x02000000, tracks, 0x0A, static_cast<uint32_t>(static_cast<int32_t>(level))}};
}

// A track's events of `kind`.
inline std::vector<seqlib::midi::TrackEvent> EventsOf(const Performance& p, int track, EventKind kind)
{
    std::vector<seqlib::midi::TrackEvent> events;
    for (const seqlib::midi::TrackEvent& e : p.tracks[static_cast<std::size_t>(track)])
    {
        if (e.kind == kind)
        {
            events.push_back(e);
        }
    }

    return events;
}

} // namespace sequence_export_ntr_cases
