// SPDX-License-Identifier: MIT

// The performance of one sequence (see perform_sequence.h), with Perform and StartingPresetKey of performer.h.

#include "sequence_export/perform_sequence.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "seqlib/midi/controllers.h"
#include "xsflib/binary.h"

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/instruments.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/sequence_export/rsar_instruments.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/performer_engine.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

namespace
{

// Tunes every part of every track of `performance` by `pitch`, a ratio of frequencies (PerformOptions::pitch): RPN 2
// takes the nearest whole number of semitones and RPN 1 the rest, in 8192ths of a semitone. The events come before the
// part's other events at tick 0, and end with the null RPN. A pitch of 1 adds nothing.
void TuneChannels(Performance& performance, float pitch)
{
    if (pitch == 1.0f || !(pitch > 0.0f))
    {
        return;
    }

    const double semitones = 12.0 * std::log2(static_cast<double>(pitch));
    const long coarse = std::clamp(std::lround(semitones), -64L, 63L);
    const long fine = std::clamp(8192 + std::lround((semitones - static_cast<double>(coarse)) * 8192.0), 0L, 16383L);
    constexpr uint8_t kCoarseTuning = 2;
    constexpr uint8_t kFineTuning = 1;
    constexpr uint8_t kNullRpn = 127;
    const std::array<std::pair<uint8_t, long>, 10> controls = {{
        {seqlib::midi::cc::kRpnMsb, 0},
        {seqlib::midi::cc::kRpnLsb, kCoarseTuning},
        {seqlib::midi::cc::kDataEntry, 64 + coarse},
        {seqlib::midi::cc::kDataEntryLsb, 0},
        {seqlib::midi::cc::kRpnMsb, 0},
        {seqlib::midi::cc::kRpnLsb, kFineTuning},
        {seqlib::midi::cc::kDataEntry, fine >> 7},
        {seqlib::midi::cc::kDataEntryLsb, fine & 0x7f},
        {seqlib::midi::cc::kRpnMsb, kNullRpn},
        {seqlib::midi::cc::kRpnLsb, kNullRpn},
    }};
    for (std::vector<TrackEvent>& events : performance.tracks)
    {
        std::set<uint8_t> parts;
        for (const TrackEvent& e : events)
        {
            parts.insert(e.part);
        }

        std::vector<TrackEvent> tuning;
        for (const uint8_t part : parts)
        {
            for (const auto& [controller, value] : controls)
            {
                tuning.push_back({.tick = 0,
                                  .kind = seqlib::midi::ControlKind(controller),
                                  .value = static_cast<uint8_t>(value),
                                  .part = part});
            }
        }
        events.insert(events.begin(), tuning.begin(), tuning.end());
    }
}

// What a pass that found `found` passes on to the next, after a pass that found `earlier`. After a change of filters,
// the note ids no longer match. The next pass then takes only the filters.
PassFindings NextFindings(PassFindings found, const PassFindings& earlier)
{
    if (found.filter_moves != earlier.filter_moves)
    {
        found = PassFindings{.filter_moves = found.filter_moves};
    }

    return found;
}

} // namespace

SarSequence ReadSarSequence(const SoundArchive& archive, uint32_t sound_index, const Settings& settings)
{
    const SoundInfo& sound = archive.Sounds()[sound_index];
    const auto file = archive.FileData(sound.file_id);
    if (file.empty() && !archive.FileMissing(sound.file_id))
    {
        // As in NoData (instruments.cpp), the 3DS's message doesn't mention group files.
        throw xsflib::FormatError(settings.archive.flavor == sar::Flavor::kCtr || archive.ExternalGroups().empty()
                                      ? "sequence file is not stored in the archive"
                                      : "sequence file is not stored in the archive or a group file beside it");
    }
    if (file.empty())
    {
        throw xsflib::FormatError(archive.Truncated() ? "sequence extends past the truncated archive"
                                                      : "sequence extends past the archive");
    }

    return {sound, Sequence::Parse(file, settings.archive)};
}

SarSequence ReadWiiSequence(const rsar::SoundArchive& archive, uint32_t sound_index)
{
    const rsar::SoundInfo& sound = archive.Sounds()[sound_index];
    const auto file = archive.FileData(sound.file_id);
    if (file.empty() && !archive.FileMissing(sound.file_id))
    {
        throw xsflib::FormatError("sequence file is not stored in the archive");
    }
    if (file.empty())
    {
        throw xsflib::FormatError(archive.Truncated() ? "sequence extends past the truncated archive"
                                                      : "sequence extends past the archive");
    }

    SarSequence wii;
    wii.seq.data = rsar::Sequence::Parse(file).data;
    wii.seq.big_endian = true;

    SoundInfo& info = wii.sound;
    info.name = sound.name;
    info.file_id = sound.file_id;
    info.volume = sound.volume;
    info.pan_mode = sound.pan_mode;
    info.pan_curve = sound.pan_curve;
    info.allocate_track_flags = sound.allocate_track_flags;
    info.start_offset = sound.start_offset;

    return wii;
}

Passes PerformPasses(const SoundInfo& sound, const Sequence& seq, Regions& banks, const PerformOptions& options,
                     const Settings& settings, const SharedPasses& shared,
                     const std::set<std::pair<int, uint32_t>>& endless_loops, std::optional<double> stop_ms)
{
    // Rerun sequences in these cases (PassFindings). When filters change during notes, the rerun moves those filters
    // from presets to controllers. When legatos raise MIDI notes above the velocities they start at, the rerun starts
    // those notes higher. When the game releases a MIDI note before its SoundFont attack ends, the rerun gives the note
    // an attack that reaches the game's level at the release. When rounded CC11 exceeds 127, the rerun scales every
    // value equally, and the tracks keep their balance. When a note's pitch LFO changes while it sounds, the rerun
    // gives the note a part of its own. The pitch bends of that part add the LFO. On the Wii, when the voice holds a
    // note's volume LFO at full level, the rerun gives the note a preset that fits its level.
    //
    // Only MIDI events and presets change between passes, but each change can move the others. With a controller-driven
    // filter, a legato may continue its MIDI note (Legato). That numbers the notes differently and changes the peak
    // CC11. A higher start lowers the peak. Note ids stay valid only between passes with the same filters. So run
    // passes until one leaves nothing to change, four at most. A pass whose parts don't fit MIDI's channels also
    // requires another pass. That pass keeps each track to the parts that fit (FitParts). Every later pass does the
    // same.
    PassFindings earlier;      // what the last pass found, for the next to play by
    double peak = shared.peak; // the CC11 peak the next pass scales for
    double applied = 0.0;      // the peak the last pass scaled for
    std::array<uint8_t, kTrackCount> part_limits;
    part_limits.fill(kPartCount);
    bool limited = false;
    if (shared.part_limits)
    {
        part_limits = *shared.part_limits;
        limited = true;
    }

    std::vector<Performer::LoopChoice> loop_choices;
    Performance performance;
    for (int pass = 0; pass < 4; pass++)
    {
        Performer performer(settings, sound, seq, banks, options, earlier, LevelScale(peak), part_limits, loop_choices);
        performer.SetEndlessLoops(endless_loops);
        if (stop_ms)
        {
            performer.SetStop(*stop_ms);
        }
        performance = performer.Run();

        // A sequence can repeat its music in a counted loop that the game plays for longer than the limit. When the
        // performance is cut at the limit inside counted loops, the sequence is performed again with each track's
        // outermost counted loop endless. Their passes then count as loops of the sequence.
        if (endless_loops.empty() && !performer.LimitLoops().empty())
        {
            return PerformPasses(sound, seq, banks, options, settings, shared, performer.LimitLoops(), stop_ms);
        }

        applied = peak;
        loop_choices = performer.LoopChoices();

        bool parts_fit = true;
        if (!limited)
        {
            if (const auto fitted = FittedParts(performance, performer.PartsUsed()))
            {
                parts_fit = false;
                part_limits = *fitted;
                limited = true;
            }
        }

        PassFindings found = NextFindings(performer.Findings(), earlier);
        const double level_peak = std::max(shared.peak, performer.LevelPeak());
        if (parts_fit && found == earlier && LevelScale(level_peak) == LevelScale(peak))
        {
            break;
        }

        earlier = std::move(found);
        peak = level_peak;
    }

    SetLevelCut(performance, applied);
    TuneChannels(performance, options.pitch);

    return {std::move(performance), applied};
}

Performance PlayOnce(const SarSequence& source, Regions& regions, const PerformOptions& options,
                     const Settings& settings, std::optional<double> stop_ms)
{
    std::array<uint8_t, kTrackCount> limits;
    limits.fill(kPartCount);
    Performer performer(settings, source.sound, source.seq, regions, options, {}, LevelScale(0.0), limits, {});
    if (stop_ms)
    {
        performer.SetStop(*stop_ms);
    }

    return performer.Run();
}

// Finds, reads and performs a CSAR or FSAR archive's sequence.
Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks, const PerformOptions& options,
                    const Settings& settings)
{
    const SarSequence source = ReadSarSequence(archive, sound_index, settings);
    SarRegions regions(banks);
    return PerformPasses(source.sound, source.seq, regions, options, settings).performance;
}

// Finds, reads and performs a Wii archive's sequence. The sound's fields and the sequence's bytecode go to the
// performer in sar's form (ReadWiiSequence).
Performance Perform(const rsar::SoundArchive& archive, uint32_t sound_index, RsarBankSet& banks,
                    const PerformOptions& options, const Settings& settings)
{
    const SarSequence wii = ReadWiiSequence(archive, sound_index);
    RsarRegions regions(banks, settings.wii_build.SinceMarch2008());
    return PerformPasses(wii.sound, wii.seq, regions, options, settings).performance;
}

PresetKey StartingPresetKey(uint32_t program, const Settings& settings)
{
    Track track;
    track.engine = settings.engine;
    track.Init();
    track.program = program;

    return TrackPresetKey(track);
}

} // namespace ntdlib::sequence_export
