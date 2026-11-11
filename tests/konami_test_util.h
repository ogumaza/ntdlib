// SPDX-License-Identifier: MIT

// What Konami's tests share: the names they use, the detection of a test cartridge with its tables given by hand, the
// readers of the MIDI and SoundFont files that a conversion writes, and the driver's code that the detection reads.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/konami/driver.h"
#include "ntdlib/konami/seqformat.h"
#include "ntdlib/konami/sequencer.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/konami/convert.h"
#include "ntdlib/sequence_export/konami/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/konami_cart.h"

namespace fs = std::filesystem;

namespace ntdlib::sequence_export::konami
{

namespace cc = seqlib::midi::cc;
using ntdlib::agb::FrameSeconds;
using ntdlib::agb::kRomBase;
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::Utf8;
using ntdlib::konami::Command;
using ntdlib::konami::DriverOverrides;
using ntdlib::konami::DungeonDiceSequencer;
using ntdlib::konami::kLiveStageSetting;
using ntdlib::konami::kMaxFrames;
using ntdlib::konami::kOutEternalDuelistPitch;
using ntdlib::konami::kOutNoteOn;
using ntdlib::konami::kOutPitch;
using ntdlib::konami::kOutPsgNote;
using ntdlib::konami::kOutStop;
using ntdlib::konami::kSettingAfterRequest;
using ntdlib::konami::kTracks;
using ntdlib::konami::Op;
using ntdlib::konami::Request;
using ntdlib::konami::Revision;
using ntdlib::konami::SampleInfo;
using ntdlib::konami::TrackOutput;
using ntdlib::konami::UltimateMastersSequencer;
using ntdlib::konami::Wct2004Sequencer;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::TempPath;
using namespace ntdlib::testing::konami_cart;

// Returns the settings of a driver whose song table is at `song_table`, for a Sequencer to play. `bend_kept` is
// DriverInfo::bend_kept.
inline DriverInfo SongsAt(uint32_t song_table, bool bend_kept = true)
{
    DriverInfo info;
    info.song_table = song_table;
    info.bend_kept = bend_kept;

    return info;
}

// Detects the driver in `rom` with its tables given by hand: `song_count` songs in the song table at `song_table`, the
// sample table at kSampleTable and a mixer rate of 16000 Hz. Fills in `info`, and returns false if the detection fails.
inline bool DetectGiven(const Rom& rom, uint32_t song_table, int song_count, DriverInfo& info)
{
    DriverOverrides ov;
    ov.song_table = song_table;
    ov.song_count = song_count;
    ov.sample_table = kSampleTable;
    ov.mix_rate = 16000;
    std::string err;

    return DetectDriver(rom, ov, info, err);
}

// A channel event of a MIDI file, the name of the track it's on, and the frame it's on.
struct MidiEvent
{
    std::string track;
    uint32_t frame;
    uint8_t status, data1, data2;
};

// Returns every channel event of every track of the MIDI file at `path`, at the frame its tick stands for under the
// file's tempo map.
inline std::vector<MidiEvent> ReadEvents(const std::string& path)
{
    const ntdlib::testing::agb_files::MidiEvents midi = ntdlib::testing::agb_files::ReadMidi(path);
    const std::vector<std::string> names = seqlib::midi::TrackNames(midi);
    std::vector<MidiEvent> events;
    std::vector<uint64_t> ticks;
    std::vector<std::pair<uint64_t, uint32_t>> tempos = {{0, 500000}}; // tick, microseconds a quarter note
    for (size_t t = 0; t < midi.tracks.size(); t++)
    {
        for (const auto& [tick, bytes] : midi.tracks[t].events)
        {
            if (bytes.size() == 6 && bytes[0] == 0xFF && bytes[1] == 0x51)
            {
                tempos.emplace_back(tick, uint32_t(bytes[3] << 16 | bytes[4] << 8 | bytes[5]));
            }
            if (bytes[0] < 0xF0)
            {
                events.push_back({names[t], 0, bytes[0], bytes[1], uint8_t(bytes.size() > 2 ? bytes[2] : 0)});
                ticks.push_back(tick);
            }
        }
    }

    // Each event's tick in seconds, then in frames.
    const double division = midi.division;
    for (size_t e = 0; e < events.size(); e++)
    {
        double seconds = 0;
        uint64_t at = 0;
        uint32_t tempo = 500000;
        for (const auto& [when, value] : tempos)
        {
            if (when > ticks[e])
            {
                break;
            }

            seconds += double(when - at) * tempo / division / 1e6;
            at = when;
            tempo = value;
        }
        seconds += double(ticks[e] - at) * tempo / division / 1e6;
        events[e].frame = uint32_t(std::lround(seconds / FrameSeconds(1)));
    }

    return events;
}

// A preset header of a SoundFont.
struct PresetHeader
{
    std::string name;
    int bank, program;
};

// Returns the preset headers of the SoundFont at `path`, in the file's order.
inline std::vector<PresetHeader> ReadPresets(const std::string& path)
{
    const std::vector<uint8_t> phdr = ReadSf2(path).chunks["phdr"];
    std::vector<PresetHeader> presets;
    for (size_t h = 0; h + 2 * 38 <= phdr.size(); h += 38) // the last header only ends the list
    {
        presets.push_back({reinterpret_cast<const char*>(&phdr[h]), phdr[h + 22] | (phdr[h + 23] << 8),
                           phdr[h + 20] | (phdr[h + 21] << 8)});
    }

    return presets;
}

// Returns the key range and root key, {lo, hi, root}, of each instrument zone of the SoundFont at `path`, in the file's
// order. Each zone starts with its key range.
inline std::vector<std::array<int, 3>> ReadZoneKeys(const std::string& path)
{
    const std::vector<uint8_t> igen = ReadSf2(path).chunks["igen"];
    std::vector<std::array<int, 3>> zones;
    for (size_t g = 0; g + 4 <= igen.size(); g += 4)
    {
        const int op = igen[g] | (igen[g + 1] << 8);
        if (op == sf2::kKeyRange)
        {
            zones.push_back({igen[g + 2], igen[g + 3], -1});
        }
        else if (op == sf2::kOverridingRootKey && !zones.empty())
        {
            zones.back()[2] = int16_t(igen[g + 2] | (igen[g + 3] << 8));
        }
    }

    return zones;
}

// Returns the start of a command reader in Thumb code: ldrb r5,[r3] / cmp r5,#0xFC / ble / cmp r5,#0xFF / bne, then
// `third` (a cmp) / bne / cmp r5,#0xEF / bgt, then `fifth` (a cmp) / bne.
inline std::vector<uint16_t> CommandReader(uint16_t third, uint16_t fifth)
{
    return {0x781D, 0x2DFC, 0xDD1E, 0x2DFF, 0xD107, third, 0xD100, 0x2DEF, 0xDC00, fifth, 0xD110};
}

// Writes Thumb code `code` at `at`, a multiple of 4, and after it the literals that its ldr instructions load: for each
// (index, value) of `literals`, the instruction at that index loads the value.
inline void CodeWithLiterals(Image& m, uint32_t at, std::vector<uint16_t> code,
                             const std::vector<std::pair<int, uint32_t>>& literals)
{
    uint32_t literal = (at + 2 * uint32_t(code.size()) + 3) & ~3u;
    for (const auto& [index, value] : literals)
    {
        const uint32_t pc = (at + 2 * uint32_t(index) + 4) & ~3u;
        code[size_t(index)] = uint16_t((code[size_t(index)] & 0xFF00) | ((literal - pc) / 4));
        m.Put32(literal, value);
        literal += 4;
    }

    m.PutHalfwords(at, code);
}

} // namespace ntdlib::sequence_export::konami
