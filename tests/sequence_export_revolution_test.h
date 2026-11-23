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
#include <cstring>
#include <iterator>
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
using ntdlib::sequence_export::tests::DecayRate;
using ntdlib::sequence_export::tests::FirstNotePreset;
using ntdlib::sequence_export::tests::FramesToLevel;
using ntdlib::sequence_export::tests::GlidesEveryFrame;
using ntdlib::sequence_export::tests::HeldLevel;
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
using ntdlib::sequence_export::tests::ZoneCutoff;
using ntdlib::sequence_export::tests::ZoneCutoffs;
using PlayedRanges = ntdlib::sequence_export::RsarPlayedRanges;
using seqlib::PreparedWave;
using seqlib::PrepareForSfz;
using seqlib::midi::MidiReport;
using seqlib::midi::NoteCount;
using seqlib::sf2::ToTimecents;
using xsflib::testing::ErrorOf;

// A tick at the default tempo and timebase, 120 beats a minute of 48 ticks, in milliseconds.
inline constexpr double kTickMs = 60000.0 / (120.0 * 48.0);

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

// The settings for `archive` under `options`.
inline Settings SettingsFor(const SoundArchive& archive, const PerformOptions& options)
{
    return Wii(options.build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive.Version()))).settings;
}

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
    return ntdlib::sequence_export::ConvertSequence(archive, sound_index, options, SettingsFor(archive, options),
                                                    Wii().program);
}

inline Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks,
                           const PerformOptions& options)
{
    return ntdlib::sequence_export::Perform(archive, sound_index, banks, options, SettingsFor(archive, options));
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

// Big-endian writer with a growing buffer, for the Wii's formats.
struct Writer
{
    void U8(std::size_t at, uint32_t v)
    {
        bytes[at] = static_cast<uint8_t>(v);
    }

    void U16(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe16(bytes.data() + at, static_cast<uint16_t>(v));
    }

    void U32(std::size_t at, uint32_t v)
    {
        xsflib::StoreBe32(bytes.data() + at, v);
    }

    // Writes an offset reference: an offset (type 1) with its data type.
    void Ref(std::size_t at, uint8_t data_type, std::size_t offset)
    {
        U8(at, 1);
        U8(at + 1, data_type);
        U32(at + 4, static_cast<uint32_t>(offset));
    }

    // Appends `n` zero bytes and returns their offset.
    std::size_t Add(std::size_t n)
    {
        bytes.resize(bytes.size() + n);

        return bytes.size() - n;
    }

    // Appends `data` and returns its offset.
    std::size_t Add(std::span<const uint8_t> data)
    {
        const std::size_t at = Add(data.size());
        std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));

        return at;
    }

    std::vector<uint8_t> bytes;
};

// Builds a file header with `magic`, `version` and space for `blocks` block entries after it, at 0x10. The caller fills
// them and the file size.
inline Writer FileHeader(const char* magic, uint16_t version, int blocks)
{
    Writer w;
    w.Add(0x20);
    std::memcpy(w.bytes.data(), magic, 4);
    w.U16(0x04, 0xfeff);
    w.U16(0x06, version);
    w.U16(0x0c, 0x20);
    w.U16(0x0e, static_cast<uint32_t>(blocks));

    return w;
}

// Starts a block with `magic` and returns its offset. Block finishes it.
inline std::size_t StartBlock(Writer& w, const char* magic)
{
    const std::size_t at = w.Add(8);
    std::memcpy(w.bytes.data() + at, magic, 4);

    return at;
}

// Finishes the block at `at` as block `i` of the header's table, and the file's size.
inline void Block(Writer& w, int i, std::size_t at)
{
    w.U32(at + 4, static_cast<uint32_t>(w.bytes.size() - at));
    w.U32(0x10 + i * 8, static_cast<uint32_t>(at));
    w.U32(0x14 + i * 8, static_cast<uint32_t>(w.bytes.size() - at));
    w.U32(0x08, static_cast<uint32_t>(w.bytes.size()));
}

// Writes a PCM16 wave info at `info` for `length` samples at `sample_rate`, looping from 0 if `loops`, with `channels`
// channels whose samples start at `samples_at` from the wave data's start.
inline void WaveInfo(Writer& w, std::size_t info, uint32_t length, uint32_t channels, bool loops, uint32_t sample_rate,
                     uint32_t data_offset, uint32_t samples_at)
{
    w.U8(info, 1); // PCM16
    w.U8(info + 1, loops ? 1 : 0);
    w.U8(info + 2, channels);
    w.U8(info + 3, sample_rate >> 16);
    w.U16(info + 4, sample_rate & 0xffff);
    w.U32(info + 0xC, length - 1); // the last sample's address
    w.U32(info + 0x10, 0x1C);
    w.U32(info + 0x14, data_offset);
    for (uint32_t c = 0; c < channels; c++)
    {
        w.U32(info + 0x1C + c * 4, 0x1C + channels * 4 + c * 0x18);
        w.U32(info + 0x1C + channels * 4 + c * 0x18, samples_at);
    }
}

// The size of a wave info with `channels` channels.
inline std::size_t WaveInfoSize(uint32_t channels)
{
    return 0x1C + channels * (4 + 0x18);
}

// Appends `samples` as big-endian PCM16 and returns their offset.
inline std::size_t AddSamples(Writer& w, const std::vector<int16_t>& samples)
{
    const std::size_t at = w.Add(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        w.U16(at + i * 2, static_cast<uint16_t>(samples[i]));
    }

    return at;
}

// Builds a PCM16 RWAV declaring `length` samples at `sample_rate`. Each of `channels` channels uses `samples`. If
// `loops` is set, the entire wave loops.
inline std::vector<uint8_t> Rwav(uint32_t length, const std::vector<int16_t>& samples, uint32_t channels = 1,
                                 bool loops = false, uint32_t sample_rate = 32000)
{
    Writer w = FileHeader("RWAV", 0x0102, 2);
    const std::size_t info = StartBlock(w, "INFO");
    w.Add(WaveInfoSize(channels));
    WaveInfo(w, info + 8, length, channels, loops, sample_rate, 0, 0);
    Block(w, 0, info);

    const std::size_t data = StartBlock(w, "DATA");
    AddSamples(w, samples);
    Block(w, 1, data);

    return w.bytes;
}

// Builds an RWAR holding RWAV files `waves`.
inline std::vector<uint8_t> Rwar(const std::vector<std::vector<uint8_t>>& waves)
{
    Writer w = FileHeader("RWAR", 0x0100, 2);
    const std::size_t table = StartBlock(w, "TABL");
    w.Add(4 + waves.size() * 12);
    w.U32(table + 8, static_cast<uint32_t>(waves.size()));
    Block(w, 0, table);

    const std::size_t data = StartBlock(w, "DATA");
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        w.Ref(table + 0xC + i * 12, 0, w.Add(waves[i]) - data);
        w.U32(table + 0xC + i * 12 + 8, static_cast<uint32_t>(waves[i].size()));
    }
    Block(w, 1, data);

    return w.bytes;
}

// A test bank region.
struct TestRegion
{
    int32_t wave_index = 0;
    uint8_t attack = 127, decay = 127, sustain = 127, release = 127, hold = 0, note_off_type = 0, key_group = 0;
    uint8_t original_key = 60, volume = 127, pan = 64;
    float tune = 1.0f;
};

// A test instrument: a region for every key and velocity, or a range table of `upper_keys` or `upper_velocities`, one
// region for each.
struct TestInstrument
{
    std::vector<TestRegion> regions = {TestRegion{}};
    std::vector<uint8_t> upper_keys = {};
    std::vector<uint8_t> upper_velocities = {};
};

// Writes `r` at `at`.
inline void WriteRegion(Writer& w, std::size_t at, const TestRegion& r)
{
    w.U32(at, static_cast<uint32_t>(r.wave_index));
    const uint8_t fields[] = {r.attack,        r.decay,     r.sustain,      r.release, r.hold, 0,
                              r.note_off_type, r.key_group, r.original_key, r.volume,  r.pan,  0};
    for (std::size_t i = 0; i < std::size(fields); i++)
    {
        w.U8(at + 4 + i, fields[i]);
    }

    uint32_t tune;
    std::memcpy(&tune, &r.tune, 4);
    w.U32(at + 0x10, tune);
}

// Builds an RBNK of `instruments`. A version 1.1 bank also gets a WAVE block describing `waves` PCM16 waves of `length`
// samples each. Their samples follow one another in the bank's wave data. A later bank's waves are an RWAR.
inline std::vector<uint8_t> Rbnk(const std::vector<TestInstrument>& instruments, uint16_t version = 0x0102,
                                 uint32_t waves = 0, uint32_t length = 0)
{
    Writer w = FileHeader("RBNK", version, waves ? 2 : 1);
    const std::size_t data = StartBlock(w, "DATA"), base = data + 8;
    const std::size_t table = w.Add(4 + instruments.size() * 8);
    w.U32(table, static_cast<uint32_t>(instruments.size()));
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        const TestInstrument& inst = instruments[i];
        if (inst.upper_keys.empty() && inst.upper_velocities.empty())
        {
            const std::size_t region = w.Add(0x14);
            WriteRegion(w, region, inst.regions[0]);
            w.Ref(table + 4 + i * 8, 1, region - base);
            continue;
        }

        // A range table: a count and each entry's upper key or velocity, then a reference to each entry's region.
        auto range_table = [&](const std::vector<uint8_t>& uppers)
        {
            const std::size_t n = uppers.size(), refs = ntdlib::AlignUp(n + 1, 4);
            const std::size_t range = w.Add(refs + n * 8);
            w.U8(range, static_cast<uint32_t>(n));
            for (std::size_t k = 0; k < n; k++)
            {
                w.U8(range + 1 + k, uppers[k]);
                const std::size_t region = w.Add(0x14);
                WriteRegion(w, region, inst.regions[k]);
                w.Ref(range + refs + k * 8, 1, region - base);
            }

            return range;
        };

        if (inst.upper_velocities.empty())
        {
            w.Ref(table + 4 + i * 8, 2, range_table(inst.upper_keys) - base);
            continue;
        }

        // Velocities split under a key table of one entry, for every key.
        const std::size_t keys = w.Add(4 + 8);
        w.U8(keys, 1);
        w.U8(keys + 1, 127);
        w.Ref(keys + 4, 2, range_table(inst.upper_velocities) - base);
        w.Ref(table + 4 + i * 8, 2, keys - base);
    }
    Block(w, 0, data);

    if (waves)
    {
        const std::size_t wave = StartBlock(w, "WAVE"), wave_base = wave + 8;
        const std::size_t refs = w.Add(4 + waves * 8);
        w.U32(refs, waves);
        for (uint32_t i = 0; i < waves; i++)
        {
            const std::size_t info = w.Add(WaveInfoSize(1));
            WaveInfo(w, info, length, 1, false, 32000, i * length * 2, 0);
            w.Ref(refs + 4 + i * 8, 0, info - wave_base);
        }
        Block(w, 1, wave);
    }

    return w.bytes;
}

// Builds an RSEQ with `commands` in its DATA block.
inline std::vector<uint8_t> Rseq(const std::vector<uint8_t>& commands)
{
    Writer w = FileHeader("RSEQ", 0x0100, 1);
    const std::size_t data = StartBlock(w, "DATA");
    w.Add(4);
    w.U32(data + 8, 0xC);
    w.Add(commands);
    Block(w, 0, data);

    return w.bytes;
}

// A test file and the wave data that goes with it.
struct ArchiveFile
{
    std::vector<uint8_t> data;
    std::vector<uint8_t> wave_data = {};
};

// A test sequence sound.
struct SequenceSound
{
    uint32_t file_id = 0;
    uint32_t start_offset = 0;
    uint32_t bank = 0;
    uint8_t pan_mode = 0, pan_curve = 0;
    uint8_t volume = 127;
    uint32_t allocate_track_flags = 1;
};

// Test archive tables: the archive's version, its sequences and banks (by file ID), its sounds' names, and the groups
// that hold its files (by file ID; by default one group holds them all). Defaults allow partial designated initializers
// without warnings.
struct ArchiveLists
{
    uint16_t version = 0x0104;
    std::vector<SequenceSound> sequences = {};
    std::vector<uint32_t> banks = {};
    std::vector<std::string> names = {};
    std::vector<std::vector<uint32_t>> groups = {};
};

// The groups of `lists`, or one group that holds each of the `files` files.
inline std::vector<std::vector<uint32_t>> ArchiveGroups(std::size_t files, const ArchiveLists& lists)
{
    std::vector<std::vector<uint32_t>> groups = lists.groups;
    if (groups.empty())
    {
        groups.emplace_back();
        for (uint32_t id = 0; id < files; id++)
        {
            groups[0].push_back(id);
        }
    }

    return groups;
}

// Adds the SYMB block: an offset, from the block's payload, to a table of offsets, from the payload too, to `names`.
inline void AddSymbols(Writer& w, const std::vector<std::string>& names)
{
    const std::size_t symb = StartBlock(w, "SYMB"), symb_base = symb + 8;
    w.Add(4);
    const std::size_t strings = w.Add(4 + names.size() * 4);
    w.U32(symb_base, static_cast<uint32_t>(strings - symb_base));
    w.U32(strings, static_cast<uint32_t>(names.size()));
    for (std::size_t i = 0; i < names.size(); i++)
    {
        const std::size_t at = w.Add(names[i].size() + 1);
        std::memcpy(w.bytes.data() + at, names[i].data(), names[i].size());
        w.U32(strings + 4 + i * 4, static_cast<uint32_t>(at - symb_base));
    }
    w.U32(symb + 4, static_cast<uint32_t>(w.bytes.size() - symb));
    w.U32(0x10, static_cast<uint32_t>(symb));
    w.U32(0x14, static_cast<uint32_t>(w.bytes.size() - symb));
}

// Adds table `i` of the INFO block whose payload starts at `base`, with `count` references, and returns its offset.
inline std::size_t InfoTable(Writer& w, std::size_t base, int i, std::size_t count)
{
    const std::size_t t = w.Add(4 + count * 8);
    w.U32(t, static_cast<uint32_t>(count));
    w.Ref(base + i * 8, 0, t - base);

    return t;
}

// Adds the INFO block's sound table: a sequence sound for each of `sequences`. Sound i takes symbol i as its name, for
// i below `names`.
inline void AddSounds(Writer& w, std::size_t base, const std::vector<SequenceSound>& sequences, std::size_t names)
{
    const std::size_t sounds = InfoTable(w, base, 0, sequences.size());
    for (std::size_t i = 0; i < sequences.size(); i++)
    {
        const SequenceSound& s = sequences[i];
        const std::size_t e = w.Add(0x2C), d = w.Add(0x10);
        w.Ref(sounds + 4 + i * 8, 0, e - base);
        w.U32(e, i < names ? static_cast<uint32_t>(i) : 0xffffffff);
        w.U32(e + 4, s.file_id);
        w.U8(e + 0x14, s.volume);
        w.U8(e + 0x16, 1); // a sequence
        w.Ref(e + 0x18, 1, d - base);
        w.U8(e + 0x28, s.pan_mode);
        w.U8(e + 0x29, s.pan_curve);
        w.U32(d, s.start_offset);
        w.U32(d + 4, s.bank);
        w.U32(d + 8, s.allocate_track_flags);
        w.U8(d + 0xC, 64);
    }
}

// Adds the INFO block's bank table: a bank in each file of `banks`.
inline void AddBanks(Writer& w, std::size_t base, const std::vector<uint32_t>& banks)
{
    const std::size_t table = InfoTable(w, base, 1, banks.size());
    for (std::size_t i = 0; i < banks.size(); i++)
    {
        const std::size_t e = w.Add(0xC);
        w.Ref(table + 4 + i * 8, 0, e - base);
        w.U32(e, 0xffffffff);
        w.U32(e + 4, banks[i]);
    }
}

// Adds the INFO block's file table for `files` files: each file's positions, the groups that hold it, and its item in
// each.
inline void AddFilePositions(Writer& w, std::size_t base, std::size_t files,
                             const std::vector<std::vector<uint32_t>>& groups)
{
    const std::size_t file_table = InfoTable(w, base, 3, files);
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> positions(files);
    for (uint32_t g = 0; g < groups.size(); g++)
    {
        for (uint32_t k = 0; k < groups[g].size(); k++)
        {
            positions[groups[g][k]].emplace_back(g, k);
        }
    }
    for (std::size_t id = 0; id < files; id++)
    {
        const std::size_t e = w.Add(0x1C);
        w.Ref(file_table + 4 + id * 8, 0, e - base);
        const std::size_t places = w.Add(4 + positions[id].size() * 8);
        w.U32(places, static_cast<uint32_t>(positions[id].size()));
        w.Ref(e + 0x14, 0, places - base);
        for (std::size_t p = 0; p < positions[id].size(); p++)
        {
            const std::size_t position = w.Add(8);
            w.U32(position, positions[id][p].first);
            w.U32(position + 4, positions[id][p].second);
            w.Ref(places + 4 + p * 8, 0, position - base);
        }
    }
}

// The INFO block's entries of the groups and of their items, in order. The FILE block fills in where the files are.
struct GroupEntries
{
    std::vector<std::size_t> groups, items;
};

// Adds the INFO block's group table for `groups`.
inline GroupEntries AddGroups(Writer& w, std::size_t base, const std::vector<std::vector<uint32_t>>& groups)
{
    const std::size_t group_table = InfoTable(w, base, 4, groups.size());
    GroupEntries entries;
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const std::size_t e = w.Add(0x28);
        w.Ref(group_table + 4 + g * 8, 0, e - base);
        entries.groups.push_back(e);
        const std::size_t items = w.Add(4 + groups[g].size() * 8);
        w.U32(items, static_cast<uint32_t>(groups[g].size()));
        w.Ref(e + 0x20, 0, items - base);
        for (std::size_t k = 0; k < groups[g].size(); k++)
        {
            const std::size_t item = w.Add(0x18);
            w.U32(item, groups[g][k]);
            w.Ref(items + 4 + k * 8, 0, item - base);
            entries.items.push_back(item);
        }
    }

    return entries;
}

// Adds the INFO block: references to the sound, bank, player, file and group tables, for `files` files.
inline GroupEntries AddInfo(Writer& w, std::size_t files, const ArchiveLists& lists,
                            const std::vector<std::vector<uint32_t>>& groups)
{
    const std::size_t info = StartBlock(w, "INFO"), base = info + 8;
    w.Add(6 * 8);
    AddSounds(w, base, lists.sequences, lists.names.size());
    AddBanks(w, base, lists.banks);
    InfoTable(w, base, 2, 0);
    AddFilePositions(w, base, files, groups);
    GroupEntries entries = AddGroups(w, base, groups);
    InfoTable(w, base, 5, 0);
    w.U32(info + 4, static_cast<uint32_t>(w.bytes.size() - info));
    w.U32(0x18, static_cast<uint32_t>(info));
    w.U32(0x1c, static_cast<uint32_t>(w.bytes.size() - info));

    return entries;
}

// Adds the FILE block: each group's files, then their wave data. Fills in the groups' `entries`.
inline void AddFiles(Writer& w, const std::vector<ArchiveFile>& files, const std::vector<std::vector<uint32_t>>& groups,
                     const GroupEntries& entries)
{
    const std::size_t file_block = StartBlock(w, "FILE");
    std::size_t item_index = 0;
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const std::size_t e = entries.groups[g];
        const std::size_t start = w.bytes.size();
        std::vector<std::size_t> offsets;
        for (const uint32_t id : groups[g])
        {
            offsets.push_back(w.Add(files[id].data) - start);
        }

        const std::size_t wave_start = w.bytes.size();
        std::vector<std::size_t> wave_offsets;
        for (const uint32_t id : groups[g])
        {
            wave_offsets.push_back(w.Add(files[id].wave_data) - wave_start);
        }

        w.U32(e + 0x10, static_cast<uint32_t>(start));
        w.U32(e + 0x14, static_cast<uint32_t>(wave_start - start));
        w.U32(e + 0x18, static_cast<uint32_t>(wave_start));
        w.U32(e + 0x1C, static_cast<uint32_t>(w.bytes.size() - wave_start));
        for (std::size_t k = 0; k < groups[g].size(); k++)
        {
            const std::size_t item = entries.items[item_index++];
            const ArchiveFile& f = files[groups[g][k]];
            w.U32(item + 4, static_cast<uint32_t>(offsets[k]));
            w.U32(item + 8, static_cast<uint32_t>(f.data.size()));
            w.U32(item + 0xC, static_cast<uint32_t>(wave_offsets[k]));
            w.U32(item + 0x10, static_cast<uint32_t>(f.wave_data.size()));
        }
    }
    w.U32(file_block + 4, static_cast<uint32_t>(w.bytes.size() - file_block));
    w.U32(0x20, static_cast<uint32_t>(file_block));
    w.U32(0x24, static_cast<uint32_t>(w.bytes.size() - file_block));
}

// Builds a sound archive of `files`, with metadata from `lists`. Each group holds its files and then their wave data.
inline std::vector<uint8_t> Rsar(const std::vector<ArchiveFile>& files, const ArchiveLists& lists = {})
{
    Writer w;
    w.Add(0x40);
    std::memcpy(w.bytes.data(), "RSAR", 4);
    w.U16(0x04, 0xfeff);
    w.U16(0x06, lists.version);
    w.U16(0x0c, 0x40);
    w.U16(0x0e, 3);

    const std::vector<std::vector<uint32_t>> groups = ArchiveGroups(files.size(), lists);
    AddSymbols(w, lists.names);
    const GroupEntries entries = AddInfo(w, files.size(), lists, groups);
    AddFiles(w, files, groups, entries);
    w.U32(0x08, static_cast<uint32_t>(w.bytes.size()));

    return w.bytes;
}

// The files of a test archive whose one sequence, file 0, plays `commands` on a bank, file 1, with one region that
// plays a wave, of an RWAR in its wave data, of 64 samples of 1000 that loops.
inline std::vector<ArchiveFile> LoopingWaveFiles(const std::vector<uint8_t>& commands, TestRegion region = {})
{
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq(commands);
    files[1].data = Rbnk({TestInstrument{{region}}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)});

    return files;
}

// Performs the sound `index` of `archive`. If `build` is given, plays it as that build of NW4R SND does
// (PerformOptions).
inline Performance PerformSound(const SoundArchive& archive, uint32_t index,
                                std::optional<SndBuild> build = std::nullopt)
{
    BankSet banks(archive, archive.Sounds()[index]);
    PerformOptions options;
    options.build = build;

    return Perform(archive, index, banks, options);
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
        files.push_back({Rseq(commands)});
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
    BankSet banks(archive, archive.Sounds()[indices.at(0)]);

    return ntdlib::sequence_export::PerformTogether(archive, indices, banks, options, SettingsFor(archive, options));
}

// Converts sounds `indices` of `archive` together, as the piece `name` (ConvertTogether).
inline Conversion ConvertTogether(const SoundArchive& archive, const std::vector<uint32_t>& indices,
                                  const std::string& name)
{
    const PerformOptions kOptions;

    return ntdlib::sequence_export::ConvertTogether(archive, indices, name, kOptions, SettingsFor(archive, kOptions),
                                                    Wii().program);
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
