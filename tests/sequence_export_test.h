// SPDX-License-Identifier: MIT

// The helpers of sequence_export's tests: the consoles that the tests run on, sequence_export's functions with the
// current console's settings, the builders of synthetic archives in its byte order, a performance of bytecode on a
// bank, the events of a performance that the tests compare, and a view of the MIDI files.
//
// A test reads the current console through `console`. RunOn sets it for each list of tests. A test that holds on
// several consoles is listed for each of them, and a console's engine-only tests are listed for that console alone.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "ntdlib/testing/sar_files.h"
#include "seqlib/file_name.h"
#include "seqlib/loop.h"
#include "seqlib/midi/performance_writer.h"
#include "seqlib/midi/reader.h"
#include "seqlib/read_error.h"
#include "seqlib/sf2/reader.h"
#include "seqlib/sf2/units.h"
#include "seqlib/wav.h"
#include "xsflib/testing/check.h"

namespace ntdlib::sequence_export::tests
{

using sar::SoundArchive;
using seqlib::PreparedWave;
using seqlib::PrepareForSoundFont;
using seqlib::SafeFileName;
using seqlib::WavFile;
using seqlib::midi::MidiReport;
using seqlib::midi::WriteMidi;
using seqlib::sf2::SoundFontPan;
using seqlib::sf2::ToTimecents;

// A console that the tests run on: its name in the failure reports, its settings, and the program name for the
// conversions to record.
struct Console
{
    std::string name;
    Settings settings;
    std::string program;
};

// The Wii U, with its settings (consoles.h).
inline Console WiiU()
{
    return {"Wii U", CafeSettings(), "xyz2midi"};
}

// The Switch, with its settings.
inline Console Switch()
{
    return {"Switch", SwitchSettings(), "xyz2midi"};
}

// The 3DS, with its settings.
inline Console Ctr()
{
    return {"3DS", CtrSettings(), "xyz2midi"};
}

// The Wii, with its settings for the build of NW4R SND in `build`, by default one of 2007.
inline Console Wii(const SndBuild& build = {})
{
    return {"Wii", RevolutionSettings(build), "xyz2midi"};
}

// The console that the tests run on now. RunOn sets it.
inline const Console* console = nullptr;

// A figure that differs by console: the figure of the Wii U, the Switch or the 3DS, whichever the tests run on now.
template <typename T>
T ByConsole(T wii_u, T nx, T ctr)
{
    switch (console->settings.engine)
    {
    case Engine::kSwitch:
        return nx;
    case Engine::kCtr:
        return ctr;
    default:
        return wii_u;
    }
}

// A test: a function that checks with XSFLIB_CHECK.
using Test = void (*)();

// Runs `tests` on `on`, in order. Reports on stderr how many of their checks failed there.
inline void RunOn(const Console& on, std::span<const Test> tests)
{
    console = &on;
    const int before = xsflib::testing::failures;
    for (const Test test : tests)
    {
        test();
    }
    console = nullptr;

    if (xsflib::testing::failures > before)
    {
        std::fprintf(stderr, "%d check(s) failed on the %s\n", xsflib::testing::failures - before, on.name.c_str());
    }
}

// Reports the run's result. Returns main()'s exit status: 0 if every check passed.
inline int Report()
{
    if (xsflib::testing::failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", xsflib::testing::failures);
        return 1;
    }

    std::puts("all checks passed");

    return 0;
}

// PCM as sar decodes it.
using Pcm = sar::Pcm;

inline Pcm DecodeWave(std::span<const uint8_t> fwav)
{
    return sar::DecodeWave(fwav, console->settings.archive);
}

inline Conversion ConvertSequence(const SoundArchive& archive, uint32_t sound_index, const PerformOptions& options = {})
{
    return sequence_export::ConvertSequence(archive, sound_index, options, console->settings, console->program);
}

inline Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks,
                           const PerformOptions& options)
{
    return sequence_export::Perform(archive, sound_index, banks, options, console->settings);
}

inline SfzInstruments BuildSfz(const Performance& perf, const MidiReport& midi, BankSet& banks,
                               const std::string& title, const std::vector<std::string>& preset_names)
{
    return sequence_export::BuildSfz(perf, midi, banks, title, preset_names, console->settings, console->program);
}

inline float DecayRate(int value)
{
    return sequence_export::DecayRate(value, console->settings.engine);
}

inline float HeldLevel(const EnvelopeValues& v, int64_t frames)
{
    return sequence_export::HeldLevel(v, frames, console->settings);
}

inline float ReleasedLevel(float level, float rate, int64_t frames)
{
    return sequence_export::ReleasedLevel(level, rate, frames, console->settings);
}

inline int64_t ReleaseFrames(float level, float rate)
{
    return sequence_export::ReleaseFrames(level, rate, console->settings);
}

inline int64_t FramesToLevel(const EnvelopeValues& v, float level)
{
    return sequence_export::FramesToLevel(v, level, console->settings);
}

inline Sf2Envelope ConvertEnvelope(const EnvelopeValues& v)
{
    return sequence_export::ConvertEnvelope(v, console->settings);
}

inline double Sf2AttackMs(const EnvelopeValues& v)
{
    return sequence_export::Sf2AttackMs(v, console->settings);
}

inline SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& v)
{
    return sequence_export::ConvertEnvelopeForSfz(v, console->settings);
}

inline GameFilters TrackFilters(int low_pass, int biquad_type, int biquad_value)
{
    return sequence_export::TrackFilters(low_pass, biquad_type, biquad_value, console->settings);
}

inline int BiquadShape(int biquad_type)
{
    return sequence_export::BiquadShape(biquad_type, console->settings);
}

inline SoundFontFilter ToSoundFont(const GameFilters& filters)
{
    return sequence_export::ToSoundFont(filters, console->settings);
}

inline uint8_t SfzBiquadValue(int biquad_type, int biquad_value)
{
    return sequence_export::SfzBiquadValue(biquad_type, biquad_value, console->settings);
}

inline int SfzLowPassCutoff(int index)
{
    return sequence_export::SfzLowPassCutoff(index, console->settings);
}

inline SfzBiquad SfzBiquadStep(int type, int step)
{
    return sequence_export::SfzBiquadStep(type, step, console->settings);
}

// A tick at the default tempo and timebase, 120 beats a minute of 48 ticks, in milliseconds.
constexpr double kTickMs = 60000.0 / (120.0 * 48.0);

// Returns the first problem that seqlib's SoundFont check finds, or an empty string.
inline std::string Sf2Problem(const std::vector<uint8_t>& d)
{
    const std::vector<std::string> problems = seqlib::sf2::Check(d);
    return problems.empty() ? "" : problems.front();
}

// A MIDI note: track, channel, key, velocity and start/end ticks.
using MidiNote = seqlib::midi::Note;

// MIDI track names, end ticks, notes, bends and markers from seqlib's reader. `ok` is false for what a file that seqlib
// writes never has: malformed data, deltas over four bytes, running status or overlapping same-key notes on one
// channel, among others.
struct MidiView
{
    bool ok = false;
    uint16_t division = 0;
    std::vector<std::string> track_names;
    std::vector<uint64_t> track_ends;
    std::vector<MidiNote> notes;
    std::vector<std::pair<uint32_t, int>> bends; // tick and 14-bit value
    std::vector<std::pair<uint32_t, std::string>> markers;
};

// Reads generated MIDI with seqlib's reader.
inline MidiView ParseMidi(const std::vector<uint8_t>& d)
{
    MidiView v;
    seqlib::midi::File file;
    try
    {
        file = seqlib::midi::Read(d);
    }
    catch (const seqlib::ReadError&)
    {
        return v;
    }

    v.ok = seqlib::midi::Problems(d).empty();
    v.division = file.division;
    v.track_names = seqlib::midi::TrackNames(file);
    for (const seqlib::midi::Track& t : file.tracks)
    {
        v.track_ends.push_back(t.end_tick);
    }
    v.notes = seqlib::midi::Notes(file);
    for (const seqlib::midi::Bend& bend : seqlib::midi::Bends(file))
    {
        v.bends.emplace_back(static_cast<uint32_t>(bend.tick), bend.value);
    }
    for (const seqlib::midi::Text& marker : seqlib::midi::Markers(file))
    {
        v.markers.emplace_back(static_cast<uint32_t>(marker.tick), marker.text);
    }

    return v;
}

// A piece of a sequence's bytecode: a byte, or a command's 16- or 24-bit argument from U16 or U24.
struct CodePiece
{
    // Implicit: a list of bytecode can then mix bytes and arguments. A byte out of range is a mistake in the test.
    CodePiece(int byte) : bytes{static_cast<uint8_t>(byte)}
    {
        if (byte < 0 || byte > 0xff)
        {
            throw std::logic_error("a byte of bytecode out of range: " + std::to_string(byte));
        }
    }

    explicit CodePiece(std::vector<uint8_t> argument) : bytes(std::move(argument))
    {
    }

    std::vector<uint8_t> bytes;
};

// Whether the console's files are big-endian. The Wii U's files are. The Switch's and the 3DS's files are
// little-endian.
inline bool BigEndian()
{
    return console->settings.archive.flavor == sar::Flavor::kCafe;
}

// A command's argument of `size` bytes, in the order in which the console's engine reads it. The Switch's engine
// follows the file's byte order, little-endian. The others read the arguments big-endian.
inline CodePiece Argument(uint32_t value, int size)
{
    if (value >> (8 * size) != 0)
    {
        throw std::logic_error("an argument too wide for " + std::to_string(size) + " bytes: " + std::to_string(value));
    }

    const bool little_endian = console->settings.arguments_follow_byte_order && !BigEndian();
    std::vector<uint8_t> bytes;
    for (int k = 0; k < size; k++)
    {
        const int byte = little_endian ? k : size - 1 - k;
        bytes.push_back(static_cast<uint8_t>(value >> (8 * byte)));
    }

    return CodePiece(std::move(bytes));
}

inline CodePiece U16(uint32_t value)
{
    return Argument(value, 2);
}

inline CodePiece U24(uint32_t value)
{
    return Argument(value, 3);
}

// Appends the bytecode of `pieces` to `code`.
inline void Append(std::vector<uint8_t>& code, std::initializer_list<CodePiece> pieces)
{
    for (const CodePiece& piece : pieces)
    {
        code.insert(code.end(), piece.bytes.begin(), piece.bytes.end());
    }
}

// The bytecode of `pieces`.
inline std::vector<uint8_t> Code(std::initializer_list<CodePiece> pieces)
{
    std::vector<uint8_t> code;
    Append(code, pieces);

    return code;
}

// The archive builders of these tests: thin helpers on ntdlib::testing::sar_files, in the console's formats. Their
// names follow the Wii U's files (FSAR, FWAV and so on). On the 3DS they build the C files (CSAR, CWAV and so on).
namespace files = ntdlib::testing::sar_files;

// Builds a PCM16 FWAV declaring `length` samples at `sample_rate`, in the format of `flavor`, or in the console's
// format without one. Each of `channels` channels uses `samples`. If `loops` is set, the entire wave loops.
inline std::vector<uint8_t> Pcm16Fwav(uint32_t length, const std::vector<int16_t>& samples, uint32_t channels = 1,
                                      bool loops = false, uint32_t sample_rate = 32000,
                                      sar::Flavor flavor = console->settings.archive.flavor)
{
    const bool big_endian = flavor == sar::Flavor::kCafe;
    std::vector<uint8_t> data;
    for (const int16_t s : samples)
    {
        const auto v = static_cast<uint16_t>(s);
        data.push_back(static_cast<uint8_t>(big_endian ? v >> 8 : v));
        data.push_back(static_cast<uint8_t>(big_endian ? v : v >> 8));
    }

    files::WaveSpec spec;
    spec.encoding = 1; // PCM16
    spec.loop = loops;
    spec.sample_rate = sample_rate;
    spec.loop_end = length;
    spec.channels.assign(channels, data);

    return files::Wave(flavor, spec);
}

// Builds a one-instrument FBNK with one region covering all keys and velocities. It plays wave table entry 0 at
// `volume` and `pan`, ignores note-off if `ignore_note_off`, and selects `interpolation` on AX. `waves` supplies (wave
// archive item ID, wave index) pairs.
inline std::vector<uint8_t> OneRegionFbnk(const std::vector<std::pair<uint32_t, uint32_t>>& waves, uint8_t volume = 127,
                                          uint8_t pan = 64, bool ignore_note_off = false, uint8_t interpolation = 0)
{
    files::Region region;
    region.volume = volume;
    region.pan = pan;
    const uint32_t flags = (ignore_note_off ? 1u : 0u) | uint32_t{interpolation} << 16;
    if (flags != 0)
    {
        region.flags = flags;
    }

    const files::VelocityTable velocities{files::VelocityTable::kDirect, 0, {{127, region}}};
    const files::KeyTable keys{files::KeyTable::kDirect, 0, {{127, velocities}}};

    return files::Bank(console->settings.archive.flavor, {waves, {keys}});
}

// Builds a one-instrument FBNK whose regions split the velocities. Region k takes those up to `upper_velocities[k]` at
// pan `pans[k]` and volume `volumes[k]`, or 127 without `volumes`. Every region plays wave table entry 0, of `waves`.
inline std::vector<uint8_t> VelocitySplitFbnk(const std::vector<std::pair<uint32_t, uint32_t>>& waves,
                                              const std::vector<uint8_t>& upper_velocities,
                                              const std::vector<uint8_t>& pans,
                                              const std::vector<uint8_t>& volumes = {})
{
    files::VelocityTable velocities{files::VelocityTable::kRange, 0, {}};
    for (std::size_t k = 0; k < upper_velocities.size(); k++)
    {
        files::Region region;
        region.volume = volumes.empty() ? 127 : volumes[k];
        region.pan = pans[k];
        velocities.entries.push_back({upper_velocities[k], region});
    }

    const files::KeyTable keys{files::KeyTable::kDirect, 0, {{127, velocities}}};

    return files::Bank(console->settings.archive.flavor, {waves, {keys}});
}

// Builds a FWAR containing FWAV files `waves`.
inline std::vector<uint8_t> Fwar(const std::vector<std::vector<uint8_t>>& waves)
{
    return files::WaveArchive(console->settings.archive.flavor, waves);
}

// A file that the test archive holds.
struct ArchiveFile
{
    std::vector<uint8_t> data;
};

// Test sequence metadata: FSEQ file, first-track offset and bank slot item IDs.
struct SequenceSound
{
    uint32_t file_id = 0;
    uint32_t start_offset = 0;
    std::vector<uint32_t> banks = {};
    uint8_t pan_mode = 0, pan_curve = 0;
    uint32_t track_flags = 1;                                // the tracks the sound allocates
    std::optional<uint32_t> channel_priority = std::nullopt; // the channels' priority | release priority fix << 8
};

// Test archive tables: sequences, and the files of the bank and wave archive items. Defaults allow partial designated
// initializers without warnings.
struct ArchiveLists
{
    std::vector<SequenceSound> sequences = {};
    std::vector<uint32_t> banks = {}, wave_archives = {};
};

// Builds a sound archive with `archive_files` in order and metadata from `lists`. Each sequence plays at volume 127,
// with its pan mode and curve, its start offset, its banks and the channel priority that it sets, if any. The archive
// has no names.
inline std::vector<uint8_t> Fsar(const std::vector<ArchiveFile>& archive_files, const ArchiveLists& lists = {})
{
    files::ArchiveSpec spec;
    spec.names = false;
    for (const SequenceSound& s : lists.sequences)
    {
        files::SoundSpec sound;
        sound.file = s.file_id;
        sound.volume = 127;
        sound.pan = s.pan_mode | uint32_t{s.pan_curve} << 8;
        sound.allocate_track_flags = s.track_flags;
        sound.channel_priority = s.channel_priority;
        sound.start_offset = s.start_offset;
        for (const uint32_t id : s.banks)
        {
            // sar_files writes bank slots as indices into the banks. An empty slot stays 0xFFFFFFFF.
            if (id != 0xffffffff && id >> 24 != 0x03)
            {
                throw std::logic_error("Fsar takes bank item IDs or 0xFFFFFFFF in a sequence's bank slots");
            }
            sound.banks.push_back(id == 0xffffffff ? id : id & 0x00ffffff);
        }
        spec.sounds.push_back(sound);
    }
    for (const uint32_t file : lists.banks)
    {
        spec.banks.push_back({"", file});
    }
    for (const uint32_t file : lists.wave_archives)
    {
        spec.wave_archives.push_back({"", file});
    }
    for (const ArchiveFile& f : archive_files)
    {
        files::FileSpec file;
        file.bytes = f.data;
        spec.files.push_back(file);
    }

    return files::Archive(console->settings.archive.flavor, spec);
}

// Builds a FSEQ containing `commands` in DATA.
inline std::vector<uint8_t> Fseq(const std::vector<uint8_t>& commands)
{
    return files::Sequence(console->settings.archive.flavor, commands, {});
}

// Performs `commands` as the one sequence of an archive with `bank`. The bank's wave table entry 0 is a looping wave.
// The sound allocates the tracks in `track_flags`.
inline Performance PerformOnBank(const std::vector<uint8_t>& commands, const std::vector<uint8_t>& bank,
                                 uint32_t track_flags = 1)
{
    std::vector<ArchiveFile> files(3);
    files[0].data = Fseq(commands);
    files[1].data = bank;
    files[2].data = Fwar({Pcm16Fwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    const SoundArchive archive = SoundArchive::Load(
        Fsar(files,
             {.sequences = {{.banks = {0x03000000}, .track_flags = track_flags}}, .banks = {1}, .wave_archives = {2}}),
        console->settings.archive);
    BankSet banks(archive, archive.Sounds()[0], console->settings);
    return Perform(archive, 0, banks, {});
}

// The keys of the notes `p` plays on track 0, in order.
inline std::vector<int> NoteKeys(const Performance& p)
{
    std::vector<int> keys;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            keys.push_back(e.key);
        }
    }

    return keys;
}

// The note-ons of `p`'s track 0, and whether a legato was played as a new note.
inline std::pair<std::ptrdiff_t, bool> NoteOnsAndRestarts(const Performance& p)
{
    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    return {note_ons, p.approximations.contains("legato (tie or mono) played as a new note")};
}

// The ticks and values of track `t`'s events of `kind` on `part`.
inline std::vector<std::pair<uint32_t, int>> PartControls(const Performance& p, EventKind kind, uint8_t part, int t = 0)
{
    std::vector<std::pair<uint32_t, int>> values;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == kind && e.part == part)
        {
            values.emplace_back(e.tick, e.value);
        }
    }

    return values;
}

// The parts of track `t`'s note-ons.
inline std::vector<int> NoteParts(const Performance& p, int t = 0)
{
    std::vector<int> parts;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            parts.push_back(e.part);
        }
    }

    return parts;
}

// Track 0's CC11 values, and its note-ons' ticks, keys and velocities.
struct LegatoOutput
{
    std::vector<std::pair<uint32_t, int>> expression;
    std::vector<std::tuple<uint32_t, int, int>> notes;
};

inline LegatoOutput ReadLegatoOutput(const Performance& p)
{
    LegatoOutput out;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kExpression)
        {
            out.expression.emplace_back(e.tick, e.value);
        }
        else if (e.kind == EventKind::kNoteOn)
        {
            out.notes.emplace_back(e.tick, e.key, e.value);
        }
    }

    return out;
}

// The places, in ticks, and the semitones of track `t`'s pitch events. An event at a tick has its place at the start of
// the tick's frame. Events between or after ticks have their places where their frames start.
inline std::vector<std::pair<double, float>> PitchPlaces(const Performance& p, int t = 0)
{
    std::vector<std::pair<double, float>> pitches;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kPitch)
        {
            pitches.emplace_back(e.after > 0 ? e.tick + e.after : p.frame_ticks.at(e.tick), e.semitones);
        }
    }

    return pitches;
}

// True if `pitches` glide up 12 semitones over 120 ms with a pitch for each sound frame. Each frame moves the sweep on
// by an envelope step: 40 frames on the Wii U, and 24 on the Switch and the 3DS. Each pitch must be where its frame
// starts at tempo 120. That tempo is 0.096 ticks a millisecond.
inline bool GlidesEveryFrame(const std::vector<std::pair<double, float>>& pitches)
{
    const int step_ms = console->settings.env_step_ms;
    if (pitches.size() != static_cast<std::size_t>(120 / step_ms) + 1)
    {
        return false;
    }

    for (std::size_t f = 0; f < pitches.size(); f++)
    {
        const float semitones = static_cast<float>(120 - step_ms * static_cast<int>(f)) * -12.0f / 120.0f;
        const double place = pitches[0].first + static_cast<double>(f) * console->settings.frame_ms * 0.096;
        if (std::fabs(pitches[f].first - place) > 1e-4 || pitches[f].second != semitones)
        {
            return false;
        }
    }

    return true;
}

// The preset event in front of the first note-on of `p`'s track 0.
inline const PresetKey* FirstNotePreset(const Performance& p)
{
    const PresetKey* preset = nullptr;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPreset)
        {
            preset = &p.presets[e.preset];
        }
        else if (e.kind == EventKind::kNoteOn)
        {
            return preset;
        }
    }

    return nullptr;
}

} // namespace ntdlib::sequence_export::tests
