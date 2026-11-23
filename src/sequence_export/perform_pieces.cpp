// SPDX-License-Identifier: MIT

// The performance of several sequences as one piece (PerformTogether and PerformBeside in performer.h): the timing of
// the sequences, and the merge of their performances.
//
// This file contains the piece of several sequences that play together (PerformTogether). perform_beside.cpp contains
// the companion that plays beside a sequence (PerformBeside).

#include "perform_pieces_internal.h"

namespace ntdlib::sequence_export
{

namespace
{

// Whether `perf` places its ticks at the same times as `lead`: the same timebase and tempo, and the same frames,
// `frames`, for the ticks that both have.
bool SameTiming(const Performance& perf, const Performance& lead, const std::vector<double>& frames)
{
    const auto same_tempo = [](const Performance::Tempo& a, const Performance::Tempo& b)
    {
        return a.tick == b.tick && a.bpm == b.bpm;
    };
    const std::size_t common = std::min(perf.frame_ticks.size(), frames.size());

    return perf.timebase == lead.timebase && std::ranges::equal(perf.tempo, lead.tempo, same_tempo) &&
           std::equal(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(common), perf.frame_ticks.begin());
}

// The fractional tick in the ticks of `lead` at which `lead`'s MIDI tempo places fractional tick `tick` of `perf`.
double LeadTick(const Performance& perf, const Performance& lead, double tick)
{
    return TempoTick(lead, TempoMs(perf, tick));
}

// Works out the timing of a piece without its loop (TimePiece): the lead, the sequences with its timing, the frames of
// its ticks and the end of the piece. Returns the sequences with notes that loop.
std::vector<std::size_t> TimeSequences(const std::vector<PieceSequence>& sequences, PieceTiming& timing)
{
    const auto plays = [](const PieceSequence& s)
    {
        return PlaysNotes(s.played.performance);
    };
    const auto first = std::ranges::find_if(sequences, plays);
    timing.lead = first == sequences.end() ? 0 : static_cast<std::size_t>(first - sequences.begin());

    // The sequences with the lead's timing, the frames of their ticks, and the end of the piece without a loop.
    const Performance& lead = sequences[timing.lead].played.performance;
    timing.frame_ticks = lead.frame_ticks;
    std::vector<std::size_t> looping;
    double end = 0.0;
    for (const PieceSequence& s : sequences)
    {
        const Performance& perf = s.played.performance;
        const bool timed = SameTiming(perf, lead, timing.frame_ticks);
        timing.timed.push_back(timed);
        if (timed && perf.frame_ticks.size() > timing.frame_ticks.size())
        {
            timing.frame_ticks = perf.frame_ticks;
        }

        end = std::max(end, timed ? perf.end_tick : LeadTick(perf, lead, perf.end_tick));
        if (PlaysNotes(perf) && MarksLoop(perf))
        {
            looping.push_back(timing.timed.size() - 1);
        }
    }

    timing.end_tick = static_cast<uint32_t>(std::ceil(end));
    return looping;
}

// Whether the loops of the sequences `looping` have the lead's timing and one length, `length`, and start a whole
// number of passes apart (TimePiece). Sets `start` to the latest start of those loops, and `end` to the latest end of
// those sequences.
bool LoopsFit(const std::vector<PieceSequence>& sequences, const PieceTiming& timing,
              const std::vector<std::size_t>& looping, uint32_t length, uint32_t& start, uint32_t& end)
{
    bool fits = true;
    for (const std::size_t k : looping)
    {
        const Performance& perf = sequences[k].played.performance;
        fits = fits && timing.timed[k] && perf.loop->end - perf.loop->start == length;
        start = std::max(start, perf.loop->start);
        end = std::max(end, perf.end_tick);
    }
    for (const std::size_t k : looping)
    {
        const Performance& perf = sequences[k].played.performance;
        fits = fits && (start - perf.loop->start) % length == 0 && (end - perf.loop->start) % length == 0;
    }

    return fits;
}

// The first tick from `start` on, a whole number of passes of `length` ticks later, at or after the last event of each
// sequence with notes that doesn't loop (TimePiece).
uint32_t StartAfterOthers(const std::vector<PieceSequence>& sequences, const PieceTiming& timing, uint32_t start,
                          uint32_t length)
{
    const Performance& lead = sequences[timing.lead].played.performance;
    for (std::size_t k = 0; k < sequences.size(); k++)
    {
        const Performance& perf = sequences[k].played.performance;
        if (!PlaysNotes(perf) || MarksLoop(perf))
        {
            continue;
        }

        double last = 0.0;
        for (const std::vector<TrackEvent>& events : perf.tracks)
        {
            for (const TrackEvent& e : events)
            {
                last = std::max(last, timing.timed[k] ? e.tick + e.after : LeadTick(perf, lead, EventPlace(perf, e)));
            }
        }
        while (start < last)
        {
            start += length;
        }
    }

    return start;
}

// Works out the timing of the piece that `sequences` play, with a main loop that plays at least `loops` times.
//
// The piece takes the tempo of its first sequence with notes, the lead. A sequence with the lead's timing keeps its
// ticks. Any other sequence's events move to the places in the lead's ticks where the lead's tempo puts their times.
//
// The sequences with notes that loop must have the lead's timing and loops of one length. Each marks its loop on the
// first pass that the game's next pass repeats (ChooseSteadyPass). The piece marks the latest of those passes. Each
// sequence then plays one of its later passes there. The game's next pass repeats that pass too. The other sequences
// with notes have to end before the piece's loop. A sequence without notes doesn't count. The piece ends where its
// looping sequences end. Where the loops don't fit together, the piece has no loop, with a warning.
PieceTiming TimePiece(const std::vector<PieceSequence>& sequences, int loops)
{
    PieceTiming timing;
    const std::vector<std::size_t> looping = TimeSequences(sequences, timing);
    if (looping.empty())
    {
        return timing;
    }

    // The loops must have the lead's timing and one length, and start a whole number of passes apart.
    const uint32_t length =
        sequences[looping[0]].played.performance.loop->end - sequences[looping[0]].played.performance.loop->start;
    uint32_t start = 0;
    uint32_t loop_end = 0;
    if (!LoopsFit(sequences, timing, looping, length, start, loop_end))
    {
        timing.warnings.push_back("the sequences loop at different places: the piece has no loop markers");
        return timing;
    }

    // The sequences with notes that don't loop end before the piece's loop. Their events at the loop's tick are no
    // notes.
    start = StartAfterOthers(sequences, timing, start, length);
    loop_end = std::max(loop_end, start + static_cast<uint32_t>(std::max(loops, 1)) * length);
    timing.loop = Performance::Loop{start, start + length};
    timing.loops = static_cast<int>((loop_end - start) / length);
    timing.end_tick = loop_end;

    return timing;
}

// Merges the performances of `sequences` into the performance of their piece, with `timing`. The piece keeps each
// sequence's tracks with notes, in order, each named after its sequence's label and its track. Note ids and presets
// are numbered across the piece. Throws FormatError when the tracks with notes are more than MIDI's 16.
Performance MergePiece(const std::vector<PieceSequence>& sequences, const PieceTiming& timing)
{
    const Performance& lead = sequences[timing.lead].played.performance;
    Performance piece;
    piece.timebase = lead.timebase;
    piece.tempo = lead.tempo;
    piece.frame_ticks = timing.frame_ticks;
    piece.since_march_2008 = lead.since_march_2008;
    piece.balance = lead.balance;
    piece.loop = timing.loop;
    piece.loops = timing.loops;
    piece.end_tick = timing.end_tick;

    int next_track = 0;
    uint32_t note_base = 0;
    for (std::size_t k = 0; k < sequences.size(); k++)
    {
        const Performance& perf = sequences[k].played.performance;
        const std::string& label = sequences[k].source.sound.name;

        // Each track with notes, with the piece's note ids and presets, at the lead's ticks. The piece takes only the
        // presets that those tracks use.
        uint32_t last_note = 0;
        for (int t = 0; t < kTrackCount; t++)
        {
            if (!TrackPlays(perf.tracks[t]))
            {
                continue;
            }
            if (next_track == kTrackCount)
            {
                throw xsflib::FormatError("the sequences play notes on more tracks than MIDI's 16 channels");
            }

            std::vector<TrackEvent>& events = piece.tracks[next_track];
            for (TrackEvent e : perf.tracks[t])
            {
                if (e.note != 0)
                {
                    last_note = std::max(last_note, e.note);
                    e.note += note_base;
                }
                if (e.kind == EventKind::kPreset)
                {
                    e.preset = seqlib::midi::PresetIndex(piece.presets, perf.presets[e.preset]);
                }
                if (!timing.timed[k])
                {
                    std::tie(e.tick, e.after) = TickAndAfter(LeadTick(perf, lead, EventPlace(perf, e)));
                }
                events.push_back(e);
            }

            piece.track_names[next_track] = label + " Track " + std::to_string(t);
            piece.biquad_types[next_track] = perf.biquad_types[t];
            next_track++;
        }
        note_base += last_note;

        // What the sequence's performance found, and its length. The sequences share their levels' cut.
        piece.seconds = std::max(piece.seconds, perf.seconds);
        piece.truncated = piece.truncated || perf.truncated;
        piece.holds = piece.holds || perf.holds;
        piece.level_cut_db = std::max(piece.level_cut_db, perf.level_cut_db);
        for (const auto& [what, count] : perf.approximations)
        {
            piece.approximations[what] += count;
        }
        for (const std::string& warning : perf.warnings)
        {
            piece.warnings.push_back(label + ": " + warning);
        }
    }

    piece.warnings.insert(piece.warnings.end(), timing.warnings.begin(), timing.warnings.end());

    return piece;
}

std::vector<PieceRun> PieceRuns(const std::vector<PieceSequence>& sequences, const PieceTiming& timing,
                                const Performance& piece)
{
    std::vector<PieceRun> runs;
    double peak = 0.0;
    for (const PieceSequence& s : sequences)
    {
        runs.push_back(s.run);
        peak = std::max(peak, s.played.peak);
    }

    // The parts that each of the piece's tracks uses, and the parts that MIDI's channels leave it.
    std::array<uint8_t, kTrackCount> used{};
    for (int t = 0; t < kTrackCount; t++)
    {
        for (const TrackEvent& e : piece.tracks[t])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                used[t] = std::max<uint8_t>(used[t], static_cast<uint8_t>(e.part + 1));
            }
        }
    }
    const std::array<uint8_t, kTrackCount> fitted = FitParts(piece, used);

    int piece_track = 0;
    for (std::size_t k = 0; k < sequences.size(); k++)
    {
        const Performance& perf = sequences[k].played.performance;
        PieceRun& run = runs[k];
        if (timing.loop && MarksLoop(perf) && PlaysNotes(perf))
        {
            const uint32_t length = perf.loop->end - perf.loop->start;
            const auto passes = static_cast<int>((timing.end_tick - perf.loop->start) / length);
            if (passes > perf.loops)
            {
                run.loops = std::max(run.loops, passes);
            }
        }
        if (LevelScale(peak) != LevelScale(sequences[k].played.peak))
        {
            run.shared.peak = peak;
        }

        // The piece's tracks hold this sequence's tracks with notes, in order.
        std::array<uint8_t, kTrackCount> limits;
        limits.fill(1);
        bool over = false;
        for (int t = 0; t < kTrackCount; t++)
        {
            if (piece_track < kTrackCount && TrackPlays(perf.tracks[t]))
            {
                limits[t] = fitted[piece_track];
                over = over || fitted[piece_track] < used[piece_track];
                piece_track++;
            }
        }
        if (over)
        {
            run.shared.part_limits = limits;
        }
    }

    return runs;
}

// Performs `sequences`, whose sources are read, as one piece with the banks of `regions` (PerformTogether). Each
// sequence plays alone first. Then each that has to play otherwise for the piece plays again (PieceRuns). A run can
// only add passes, lower the levels' scale or take parts away. The runs therefore settle in a round or two.
Performance PerformPiece(std::vector<PieceSequence>& sequences, Regions& regions, const PerformOptions& options,
                         const Settings& settings)
{
    for (PieceSequence& s : sequences)
    {
        s.run.loops = options.loops;
        s.played = PlayPieceSequence(s.source, s.run, regions, options, settings);
    }

    for (int round = 0; round < 3; round++)
    {
        const PieceTiming timing = TimePiece(sequences, options.loops);
        Performance piece = MergePiece(sequences, timing);
        const std::vector<PieceRun> runs = PieceRuns(sequences, timing, piece);
        bool again = false;
        for (std::size_t k = 0; k < sequences.size(); k++)
        {
            if (runs[k] != sequences[k].run)
            {
                sequences[k].run = runs[k];
                sequences[k].played = PlayPieceSequence(sequences[k].source, runs[k], regions, options, settings);
                again = true;
            }
        }
        if (!again)
        {
            return piece;
        }
    }

    return MergePiece(sequences, TimePiece(sequences, options.loops));
}

} // namespace

Performance PerformTogether(const SoundArchive& archive, std::span<const uint32_t> sound_indices, BankSet& banks,
                            const PerformOptions& options, const Settings& settings)
{
    if (sound_indices.empty())
    {
        throw std::invalid_argument("no sequences to perform together");
    }

    // One SoundFont holds the piece's instruments. The sequences therefore share their banks, and the pan mode that
    // places their stereo waves.
    const SoundInfo& first = archive.Sounds()[sound_indices[0]];
    for (const uint32_t index : sound_indices)
    {
        const SoundInfo& sound = archive.Sounds()[index];
        if (sound.banks != first.banks || sound.pan_mode != first.pan_mode)
        {
            throw std::invalid_argument(first.name + " and " + sound.name + " differ in banks or pan mode");
        }
    }

    SarRegions regions(banks);
    std::vector<PieceSequence> sequences(sound_indices.size());
    for (std::size_t k = 0; k < sound_indices.size(); k++)
    {
        sequences[k].source = ReadSarSequence(archive, sound_indices[k], settings);
    }

    return PerformPiece(sequences, regions, options, settings);
}

Performance PerformTogether(const rsar::SoundArchive& archive, std::span<const uint32_t> sound_indices,
                            RsarBankSet& banks, const PerformOptions& options, const Settings& settings)
{
    if (sound_indices.empty())
    {
        throw std::invalid_argument("no sequences to perform together");
    }

    // One SoundFont holds the piece's instruments. The sequences therefore share their bank, and the pan mode that
    // places its stereo waves.
    const rsar::SoundInfo& first = archive.Sounds()[sound_indices[0]];
    for (const uint32_t index : sound_indices)
    {
        const rsar::SoundInfo& sound = archive.Sounds()[index];
        if (sound.bank != first.bank || sound.pan_mode != first.pan_mode)
        {
            throw std::invalid_argument(first.name + " and " + sound.name + " differ in bank or pan mode");
        }
    }

    RsarRegions regions(banks, settings.wii_build.SinceMarch2008());
    std::vector<PieceSequence> sequences(sound_indices.size());
    for (std::size_t k = 0; k < sound_indices.size(); k++)
    {
        sequences[k].source = ReadWiiSequence(archive, sound_indices[k]);
    }

    return PerformPiece(sequences, regions, options, settings);
}

} // namespace ntdlib::sequence_export
