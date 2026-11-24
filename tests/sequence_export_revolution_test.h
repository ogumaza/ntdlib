// SPDX-License-Identifier: MIT

// The helpers of the tests on the Wii's engine (sequence_export_revolution*_test.cpp): the Wii's settings, the
// builders of synthetic RSAR archives and their files, the performance and conversion of a sound, and the events of a
// performance that the tests compare.

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/loop.h"
#include "seqlib/midi/performance.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/midi/reader.h"
#include "seqlib/pcm.h"
#include "seqlib/sf2/units.h"
#include "xsflib/binary.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/envelope.h"
#include "ntdlib/rounding.h"
#include "ntdlib/rsar/decode.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/sound_build.h"
#include "sequence_export/wii_pan.h"
#include "sequence_export_rsar_files.h"
#include "sequence_export_test.h"

namespace ntdlib::sequence_export::wii_tests
{

namespace sf2 = seqlib::sf2;

using ntdlib::SndBuild;
using ntdlib::nwsnd::DecibelSquare;
using ntdlib::nwsnd::HoldMs;
using ntdlib::rsar::Bank;
using ntdlib::rsar::Region;
using ntdlib::rsar::SoundArchive;
using ntdlib::rsar::SoundInfo;
using ntdlib::rsar::Wave;
using ntdlib::sequence_export::AttackMs;
using ntdlib::sequence_export::BankConversion;
using ntdlib::sequence_export::BankSoundFont;
using ntdlib::sequence_export::ChannelPan;
using ntdlib::sequence_export::Conversion;
using ntdlib::sequence_export::EnvelopeValues;
using ntdlib::sequence_export::EventKind;
using ntdlib::sequence_export::FilterControllerValue;
using ntdlib::sequence_export::GameFilters;
using ntdlib::sequence_export::kFadedAttenuation;
using ntdlib::sequence_export::kFilterOpen;
using ntdlib::sequence_export::kInaudibleSustain;
using ntdlib::sequence_export::kSfzBiquadEvent;
using ntdlib::sequence_export::kSfzLowPassEvent;
using ntdlib::sequence_export::LowPassHz;
using ntdlib::sequence_export::LowPassIndex;
using ntdlib::sequence_export::Performance;
using ntdlib::sequence_export::PresetKey;
using ntdlib::sequence_export::RegionEnvelope;
using ntdlib::sequence_export::RegionRanges;
using ntdlib::sequence_export::RsarBankSet;
using ntdlib::sequence_export::RsarPlayedRanges;
using ntdlib::sequence_export::Settings;
using ntdlib::sequence_export::SfzFile;
using ntdlib::sequence_export::SfzInstruments;
using ntdlib::sequence_export::SfzLowPassValue;
using ntdlib::sequence_export::SoundFontFilter;
using ntdlib::sequence_export::TrackEvent;
using ntdlib::sequence_export::TrackFilters;
using ntdlib::sequence_export::WaveChannelPan;
using ntdlib::sequence_export::tests::CheckBankSoundFontHoldsEachInstrument;
using ntdlib::sequence_export::tests::DecayRate;
using ntdlib::sequence_export::tests::FirstNotePreset;
using ntdlib::sequence_export::tests::FramesToLevel;
using ntdlib::sequence_export::tests::GlidesEveryFrame;
using ntdlib::sequence_export::tests::HeldLevel;
using ntdlib::sequence_export::tests::kTickMs;
using ntdlib::sequence_export::tests::NoteOnsAndRestarts;
using ntdlib::sequence_export::tests::NoteParts;
using ntdlib::sequence_export::tests::PartControls;
using ntdlib::sequence_export::tests::PitchPlaces;
using ntdlib::sequence_export::tests::RegionLowPass;
using ntdlib::sequence_export::tests::ReleaseFrames;
using ntdlib::sequence_export::tests::RunOn;
using ntdlib::sequence_export::tests::Sf2AttackMs;
using ntdlib::sequence_export::tests::Sf2Problem;
using ntdlib::sequence_export::tests::SfzBiquadValue;
using ntdlib::sequence_export::tests::SfzLowPassCutoff;
using ntdlib::sequence_export::tests::Test;
using ntdlib::sequence_export::tests::Wii;
using ntdlib::sequence_export::tests::WiiBankTestArchive;
using ntdlib::sequence_export::tests::WiiConvertBank;
using ntdlib::sequence_export::tests::WiiConvertPiece;
using ntdlib::sequence_export::tests::WiiConvertSound;
using ntdlib::sequence_export::tests::WiiPerform;
using ntdlib::sequence_export::tests::WiiPerformPiece;
using ntdlib::sequence_export::tests::WiiPerformSound;
using ntdlib::sequence_export::tests::ZoneCutoff;
using ntdlib::sequence_export::tests::ZoneCutoffs;
using PlayedRanges = ntdlib::sequence_export::RsarPlayedRanges;
using seqlib::PreparedWave;
using seqlib::PrepareForSfz;
using seqlib::midi::MidiReport;
using seqlib::midi::NoteCount;
using seqlib::sf2::ToTimecents;
using xsflib::testing::ErrorOf;

// The Wii's settings for the builds of 2007. An archive older than RSAR 1.4 plays with them when there's no main.dol.
inline const Settings kSettings = Wii().settings;

// The Wii's sound frame, in milliseconds.
inline constexpr double kFrameMs = 3.0;

// The options for the Wii: sequence_export's PerformOptions, with the build that plays the sequence. Without a build,
// the archive's version gives it.
struct PerformOptions : ntdlib::sequence_export::PerformOptions
{
    std::optional<SndBuild> build;
};

// A sound's bank, read with the tests' settings.
class BankSet : public RsarBankSet
{
public:
    BankSet(const SoundArchive& archive, const SoundInfo& sound) : RsarBankSet(archive, sound, kSettings)
    {
    }
};

// Decoded PCM, with seqlib's loop and the wave's sample rate.
struct Pcm : seqlib::Pcm
{
    uint32_t sample_rate = 32000;
};

inline Pcm ToPcm(ntdlib::rsar::Pcm decoded)
{
    Pcm pcm;
    pcm.sample_rate = decoded.sample_rate;
    pcm.loop = decoded.loop;
    pcm.loop_start = decoded.loop_start;
    pcm.loop_end = decoded.loop_end;
    pcm.channels = std::move(decoded.channels);

    return pcm;
}

inline Conversion ConvertSequence(const SoundArchive& archive, uint32_t sound_index, const PerformOptions& options = {})
{
    return WiiConvertSound(archive, sound_index, options, options.build);
}

inline Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks,
                           const PerformOptions& options)
{
    return WiiPerform(archive, sound_index, banks, options, options.build);
}

inline SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks,
                               const std::string& title, const std::vector<std::string>& preset_names, bool balance)
{
    return ntdlib::sequence_export::BuildSfz(perf, midi, banks, title, preset_names, balance, kSettings, Wii().program);
}

inline SoundFontFilter ToSoundFont(int low_pass, int biquad_type, int biquad_value)
{
    return ntdlib::sequence_export::ToSoundFont(TrackFilters(low_pass, biquad_type, biquad_value, kSettings),
                                                kSettings);
}

// The step of the biquad's preset table for 0xb4 `biquad_type` and 0xb5 `biquad_value`, or -1 for off.
inline int BiquadStep(int biquad_type, int biquad_value)
{
    const GameFilters f = TrackFilters(0x40, biquad_type, biquad_value, kSettings);
    return f.biquad_type != 0 ? f.biquad_step : -1;
}

// Converts a string to bytes.
inline std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Converts bytes to a string for comparison.
inline std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

// The files of a test archive whose one sequence, file 0, plays `commands` on a bank, file 1, with one region that
// plays a wave, of an RWAR in its wave data, of 64 samples of 1000 that loops.
inline std::vector<ArchiveFile> LoopingWaveFiles(const std::vector<uint8_t>& commands, TestRegion region = {})
{
    std::vector<ArchiveFile> files(2);
    files[0].data = MakeSequence(commands, {});
    files[1].data = Rbnk({TestInstrument{{region}}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)});

    return files;
}

// Performs the sound `index` of `archive`. If `build` is given, plays it as that build of NW4R SND does
// (PerformOptions).
inline Performance PerformSound(const SoundArchive& archive, uint32_t index,
                                std::optional<SndBuild> build = std::nullopt)
{
    return WiiPerformSound(archive, index, {}, build);
}

// Options for SequenceArchive.
struct TestSounds
{
    uint16_t version = 0x0104;      // the archive's version: 1.4 plays as a build of NW4R SND from 2008
    uint32_t track_flags = 1;       // the tracks each sound allocates
    TestRegion region = {};         // the bank's one region
    std::vector<uint8_t> wave = {}; // the RWAV the region plays: 64 samples of 1000 that loop if empty

    // Regions split by velocity, up to each of `upper_velocities`, in place of `region`.
    std::vector<TestRegion> velocity_regions = {};
    std::vector<uint8_t> upper_velocities = {};

    // The build of NW4R SND that a main.dol shows, in place of the one that `version` gives.
    std::optional<SndBuild> build = std::nullopt;

    // The sounds' names, in order. A sound past the list's end has none.
    std::vector<std::string> names = {};
};

// Builds an archive with a sound for each of `sequences`, all of them on one bank whose one region plays one wave.
inline SoundArchive SequenceArchive(const std::vector<std::vector<uint8_t>>& sequences, const TestSounds& options = {})
{
    std::vector<ArchiveFile> files;
    std::vector<SequenceSound> sounds;
    for (const std::vector<uint8_t>& commands : sequences)
    {
        sounds.push_back({.file_id = static_cast<uint32_t>(files.size()), .allocate_track_flags = options.track_flags});
        files.push_back({MakeSequence(commands, {})});
    }

    const std::vector<uint8_t> wave =
        options.wave.empty() ? Rwav(64, std::vector<int16_t>(64, 1000), 1, true) : options.wave;
    const TestInstrument instrument =
        options.upper_velocities.empty()
            ? TestInstrument{{options.region}}
            : TestInstrument{.regions = options.velocity_regions, .upper_velocities = options.upper_velocities};
    files.push_back({Rbnk({instrument}), Rwar({wave})});

    const uint32_t bank = static_cast<uint32_t>(sequences.size());
    return SoundArchive::Load(
        Rsar(files, {.version = options.version, .sequences = sounds, .banks = {bank}, .names = options.names}));
}

// Performs `commands` as the one sound of a SequenceArchive.
inline Performance PerformCommands(const std::vector<uint8_t>& commands, const TestSounds& options = {})
{
    return PerformSound(SequenceArchive({commands}, options), 0, options.build);
}

// Performs `commands` as the one sound of a SequenceArchive with `sounds`, under `options`.
inline Performance PerformCommandsWith(const std::vector<uint8_t>& commands, const PerformOptions& options,
                                       const TestSounds& sounds = {})
{
    const SoundArchive archive = SequenceArchive({commands}, sounds);
    BankSet banks(archive, archive.Sounds()[0]);

    return Perform(archive, 0, banks, options);
}

// Performs sounds `indices` of `archive` together, as one piece (PerformTogether), under `options`.
inline Performance PerformTogether(const SoundArchive& archive, const std::vector<uint32_t>& indices,
                                   const PerformOptions& options = {})
{
    return WiiPerformPiece(archive, indices, options, options.build);
}

// Converts sounds `indices` of `archive` together, as the piece `name` (ConvertTogether).
inline Conversion ConvertTogether(const SoundArchive& archive, const std::vector<uint32_t>& indices,
                                  const std::string& name)
{
    return WiiConvertPiece(archive, indices, name);
}

// The ticks and kinds of track `t`'s note events.
inline std::vector<std::pair<uint32_t, EventKind>> NoteEvents(const Performance& p, int t = 0)
{
    std::vector<std::pair<uint32_t, EventKind>> notes;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.tick, e.kind);
        }
    }

    return notes;
}

// The ticks, keys and velocities of track `t`'s note-ons.
inline std::vector<std::tuple<uint32_t, int, int>> NoteOns(const Performance& p, int t = 0)
{
    std::vector<std::tuple<uint32_t, int, int>> notes;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            notes.emplace_back(e.tick, e.key, e.value);
        }
    }

    return notes;
}

// The ticks and values of track `t`'s events of `kind`.
inline std::vector<std::pair<uint32_t, int>> Controls(const Performance& p, EventKind kind, int t = 0)
{
    std::vector<std::pair<uint32_t, int>> values;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == kind)
        {
            values.emplace_back(e.tick, e.value);
        }
    }

    return values;
}

// The ticks and semitones of track `t`'s pitch events.
inline std::vector<std::pair<uint32_t, float>> Pitches(const Performance& p, int t = 0)
{
    std::vector<std::pair<uint32_t, float>> pitches;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kPitch)
        {
            pitches.emplace_back(e.tick, e.semitones);
        }
    }

    return pitches;
}

} // namespace ntdlib::sequence_export::wii_tests
