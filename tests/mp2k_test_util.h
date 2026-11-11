// SPDX-License-Identifier: MIT

// What MP2K's tests share: the names they use, the detection of a test cartridge, and the runs of its sequencer, its
// mixer and its archive player.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include "seqlib/midi/reader.h"
#include "seqlib/testing/check.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/player/archive_source.h"

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/mp2k/archive_player.h"
#include "ntdlib/mp2k/archive_source.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/sequencer.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/agb/song_banks.h"
#include "ntdlib/sequence_export/mp2k/convert.h"
#include "ntdlib/sequence_export/mp2k/performance.h"
#include "ntdlib/sequence_export/mp2k/soundfont.h"
#include "ntdlib/testing/agb_cart.h"
#include "ntdlib/testing/agb_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/wave/blep.h"

namespace ntdlib::sequence_export::mp2k
{
using enum ntdlib::mp2k::Command;
using enum ntdlib::mp2k::Xcommand;
namespace cc = seqlib::midi::cc;
namespace sf2 = seqlib::sf2;
using ntdlib::agb::kFrameRate;
using ntdlib::agb::kRomBase;
using ntdlib::agb::PathFromUtf8;
using ntdlib::agb::TrackLayout;
using ntdlib::agb::Utf8;
using ntdlib::mp2k::Action;
using ntdlib::mp2k::ArchivePlayer;
using ntdlib::mp2k::ArchivePlayerOptions;
using ntdlib::mp2k::CamelotKeyToFrequency;
using ntdlib::mp2k::Channel;
using ntdlib::mp2k::ClockLength;
using ntdlib::mp2k::DecodeEvent;
using ntdlib::mp2k::DescribeEvent;
using ntdlib::mp2k::DetectDriver;
using ntdlib::mp2k::DriverOverrides;
using ntdlib::mp2k::DumpSong;
using ntdlib::mp2k::Event;
using ntdlib::mp2k::kChannelCount;
using ntdlib::mp2k::KeyToFrequency;
using ntdlib::mp2k::KeyToPsgFrequency;
using ntdlib::mp2k::kFifoLead;
using ntdlib::mp2k::kFirstPsgChannel;
using ntdlib::mp2k::kFullPlayerVolume;
using ntdlib::mp2k::kVoiceCompressed;
using ntdlib::mp2k::kVoiceDrumKit;
using ntdlib::mp2k::kVoiceFixed;
using ntdlib::mp2k::kVoiceKeySplit;
using ntdlib::mp2k::kVoiceReverse;
using ntdlib::mp2k::NoteArguments;
using ntdlib::mp2k::ReadSong;
using ntdlib::mp2k::RunningStatus;
using ntdlib::mp2k::ScanTrack;
using ntdlib::mp2k::Sequencer;
using ntdlib::mp2k::SongAddress;
using ntdlib::mp2k::SongChannels;
using ntdlib::mp2k::SongPlayer;
using ntdlib::mp2k::SongReverb;
using ntdlib::mp2k::SongVolume;
using ntdlib::mp2k::SynthStep;
using ntdlib::mp2k::Wave;
using ntdlib::sequence_export::agb::ChannelWriter;
using ntdlib::sequence_export::agb::kInstant;
using ntdlib::sequence_export::agb::MidiFile;
using ntdlib::sequence_export::agb::PresetInstrument;
using ntdlib::sequence_export::agb::Timecents;
using ntdlib::testing::agb_files::CartImage;
using ntdlib::testing::agb_files::HasGlobalZoneModulators;
using ntdlib::testing::agb_files::Le16;
using ntdlib::testing::agb_files::Le32;
using ntdlib::testing::agb_files::MidiEvents;
using ntdlib::testing::agb_files::ReadAll;
using ntdlib::testing::agb_files::ReadMidi;
using ntdlib::testing::agb_files::ReadSf2;
using ntdlib::testing::agb_files::Sf2Records;
using ntdlib::testing::agb_files::TempPath;
using ntdlib::testing::agb_files::TempSettings;

using ntdlib::testing::mp2k_cart::Cart;
using ntdlib::testing::mp2k_cart::kCamelotMode;
using ntdlib::testing::mp2k_cart::kCamelotPulse;
using ntdlib::testing::mp2k_cart::kData;
using ntdlib::testing::mp2k_cart::kInit;
using ntdlib::testing::mp2k_cart::kLaterCamelotMode;
using ntdlib::testing::mp2k_cart::kMode;
using ntdlib::testing::mp2k_cart::kMonoVolume;
using ntdlib::testing::mp2k_cart::kPlayerTable;
using ntdlib::testing::mp2k_cart::kSongStart;
using ntdlib::testing::mp2k_cart::kSongTable;
using ntdlib::testing::mp2k_cart::Saw;
using ntdlib::testing::mp2k_cart::Track;

// The MIDI file's ticks for each of the driver's, without frame timing: 960 to the quarter note.
inline constexpr uint32_t kMidiTicks = 40;

// A voice type: a DirectSound sample that plays at the mixer's rate.
inline constexpr uint8_t kFixed = kVoiceFixed;

// Returns the driver info of a cartridge, failing the test if detection fails.
inline DriverInfo Detect(const Rom& rom)
{
    DriverInfo info;
    std::string error;
    SEQLIB_CHECK(DetectDriver(rom, DriverOverrides(), info, error));

    return info;
}

// Returns true if `seconds` is the length of `frames` frames.
inline bool AtFrames(double seconds, double frames)
{
    return std::fabs(seconds - frames / kFrameRate) < 1e-9;
}

// Runs a sequencer for `frames` frames, and returns the actions of the kind given.
inline std::vector<Action> Actions(Sequencer& seq, int frames, Action::Kind kind)
{
    std::vector<Action> out;
    for (int f = 0; f < frames; f++)
    {
        for (const Action& a : seq.Step())
        {
            if (a.kind == kind)
            {
                out.push_back(a);
            }
        }
    }

    return out;
}

// Each frame's output of the model's mixer, on each side.
struct MixedFrames
{
    std::vector<std::vector<int8_t>> right;
    std::vector<std::vector<int8_t>> left;
};

// Runs a sequencer for `frames` frames with the model mixing, and keeps each frame's output.
inline MixedFrames Mix(Sequencer& seq, int frames)
{
    MixedFrames out;
    seq.StartMixing();
    for (int f = 0; f < frames; f++)
    {
        seq.Step();
        out.right.emplace_back(seq.Right().begin(), seq.Right().end());
        out.left.emplace_back(seq.Left().begin(), seq.Left().end());
    }

    return out;
}

// Returns the number of the first frame whose output isn't silent, or -1.
inline int FirstSound(const MixedFrames& m)
{
    const auto loud = [](int8_t point)
    {
        return point != 0;
    };
    for (size_t f = 0; f < m.right.size(); f++)
    {
        if (std::any_of(m.right[f].begin(), m.right[f].end(), loud) ||
            std::any_of(m.left[f].begin(), m.left[f].end(), loud))
        {
            return int(f);
        }
    }

    return -1;
}

// Returns what a point of `value` at `volume` adds to a byte of the output: the product over 256, rounded down.
inline uint8_t Added(int32_t value, int volume)
{
    return uint8_t((value * volume) >> 8);
}

// Returns a sample of `length` points that jump about over the whole range.
inline std::vector<int8_t> Jumpy(size_t length)
{
    std::vector<int8_t> pcm;
    for (size_t i = 0; i < length; i++)
    {
        pcm.push_back(int8_t(int(i * 37 % 256) - 128));
    }

    return pcm;
}

// Decodes a compressed sample's blocks of 33 bytes. A block's first byte is its first point. The low half of its
// second byte gives the second point's step from the first, and the two halves of each later byte the next two steps,
// the high half first.
inline std::vector<int8_t> Decompress(const std::vector<uint8_t>& blocks)
{
    static const int kSteps[16] = {0, 1, 4, 9, 16, 25, 36, 49, -64, -49, -36, -25, -16, -9, -4, -1};
    std::vector<int8_t> out;
    for (size_t b = 0; b + 33 <= blocks.size(); b += 33)
    {
        uint8_t point = blocks[b];
        out.push_back(int8_t(point));
        point = uint8_t(point + kSteps[blocks[b + 1] & 0xF]);
        out.push_back(int8_t(point));
        for (size_t k = 2; k < 33; k++)
        {
            for (int half : {blocks[b + k] >> 4, blocks[b + k] & 0xF})
            {
                point = uint8_t(point + kSteps[half]);
                out.push_back(int8_t(point));
            }
        }
    }

    return out;
}

// Runs every test. Returns the exit code.
// The player's tests. The test cartridges' driver mixes 224 points a frame at 13,379 Hz, and gives the sound bias
// register a resolution of 8 bits: an output of 65,536 samples a second, 256 cycles apart.
inline constexpr uint32_t kFrameCycles = uint32_t(ntdlib::agb::kFrameCycles);

inline constexpr uint32_t kOutputCycles = 256;

// Runs `frames` frames, and returns each frame's samples.
inline std::vector<std::vector<int16_t>> PlayFrames(ArchivePlayer& player, int frames)
{
    std::vector<std::vector<int16_t>> out(static_cast<size_t>(frames));
    for (std::vector<int16_t>& frame : out)
    {
        player.RunFrame(frame);
    }

    return out;
}

// Runs `frames` frames, and returns their samples.
inline std::vector<int16_t> Play(ArchivePlayer& player, int frames)
{
    std::vector<int16_t> out;
    for (int f = 0; f < frames; f++)
    {
        player.RunFrame(out);
    }

    return out;
}

// Returns the largest magnitude of the samples of side `side` (0 for the left).
inline int32_t Peak(const std::vector<int16_t>& samples, size_t side)
{
    int32_t peak = 0;
    for (size_t i = side; i < samples.size(); i += 2)
    {
        peak = std::max(peak, std::abs(int32_t(samples[i])));
    }

    return peak;
}

// Returns whether `f` throws an exception of type E.
template <typename E, typename F>

inline bool Throws(F f)
{
    try
    {
        f();
    }
    catch (const E&)
    {
        return true;
    }

    return false;
}

// A song of two tracks: a saw that loops on a DirectSound channel, and a square. A rest takes the place of either.
struct TwoTracks
{
    Rom rom;
    DriverInfo info;
};

inline TwoTracks TwoTrackSongs(uint8_t reverb = 0)
{
    Cart cart;
    const uint32_t voices =
        cart.VoiceGroup({{0, Cart::Voice(0, cart.Wave(Saw(), 13379, 0))}, {1, Cart::Voice(1, 2, 60, {0, 0, 15, 0})}});
    const Track saw = Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Cmd(kCmdVol, 100).Note(48, 60, 127).Wait(48).Fine();
    const Track square =
        Track().Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 1).Cmd(kCmdVol, 100).Wait(6).Note(48, 64, 127).Wait(48).Fine();
    const Track rest = Track().Cmd(kCmdTempo, 75).Wait(96).Fine();
    cart.Song({saw, square}, voices, 0, 0, reverb);
    cart.Song({rest, square}, voices, 0, 0, reverb);
    cart.Song({saw, rest}, voices, 0, 0, reverb);
    TwoTracks songs{cart.ToRom(), {}};
    songs.info = Detect(songs.rom);

    return songs;
}

} // namespace ntdlib::sequence_export::mp2k
