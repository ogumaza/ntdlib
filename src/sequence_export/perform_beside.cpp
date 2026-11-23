// SPDX-License-Identifier: MIT

// The performance of a companion beside a sequence (PerformBeside in performer.h): the timing of the companion, and
// the merge of the two performances (see perform_pieces.cpp).

#include "perform_pieces_internal.h"

namespace ntdlib::sequence_export
{

namespace
{

// The changes among `changes` of the global variables that `read` marks (Performance::globals_read), from frame `from`
// on, each `shift` frames later.
std::vector<VariableChange> ReadChanges(const std::vector<VariableChange>& changes, uint16_t read, uint64_t from,
                                        int64_t shift)
{
    std::vector<VariableChange> taken;
    for (const VariableChange& change : changes)
    {
        const bool global = change.index >= 16 && change.index < 32;
        if (global && (read >> (change.index - 16) & 1) && change.frame >= from)
        {
            taken.push_back({.frame = static_cast<uint64_t>(static_cast<int64_t>(change.frame) + shift),
                             .index = change.index,
                             .value = change.value});
        }
    }

    return taken;
}

// The global variables in frame `frame` of the companion, after its ticks (PerformBeside): -1 at first, then the
// game's writes `writes` and the companion's changes `changes`, frame by frame. In each frame, the game's writes come
// before the ticks.
std::array<int16_t, 16> GlobalsAfter(uint64_t frame, const std::vector<VariableChange>& writes,
                                     const std::vector<VariableChange>& changes)
{
    std::vector<std::pair<std::pair<uint64_t, int>, VariableChange>> ordered;
    for (const VariableChange& w : writes)
    {
        ordered.push_back({{w.frame, 0}, w});
    }
    for (const VariableChange& c : changes)
    {
        ordered.push_back({{c.frame, 1}, c});
    }
    std::ranges::stable_sort(ordered, {}, &std::pair<std::pair<uint64_t, int>, VariableChange>::first);

    std::array<int16_t, 16> globals;
    globals.fill(-1);
    for (const auto& [when, change] : ordered)
    {
        if (when.first <= frame && change.index >= 16 && change.index < 32)
        {
            globals[change.index - 16] = change.value;
        }
    }

    return globals;
}

// How a piece and its companion play together (PerformBeside): the companion's frame in which the piece starts, and
// each sound's options with the other's changes.
struct BesideTiming
{
    uint64_t start = 0;
    PerformOptions piece;
    PerformOptions companion;
};

// The time a companion has to hand over to its piece (PerformBeside).
constexpr double kHandOverMs = 30000.0;

// The rounds in which the piece and its companion take each other's changes, at most (PerformBeside).
constexpr int kBesideRounds = 8;

// Works out how the piece `piece` and its companion `companion` play together (PerformBeside), with the piece's options
// `piece_options` and the companion's `companion_options`. Each sound plays with the other's changes from its last run,
// until a run of each changes nothing that the other reads. The piece plays one more pass of its loop than its options
// ask. Its changes then cover the companion's run past the piece's end (MergeBeside). Nothing when the companion
// doesn't hand over within kHandOverMs.
std::optional<BesideTiming> TimeBeside(const SarSequence& piece, Regions& piece_regions,
                                       const PerformOptions& piece_options, const SarSequence& companion,
                                       Regions& companion_regions, const PerformOptions& companion_options,
                                       const CompanionScript& script, const Settings& settings)
{
    std::optional<BesideTiming> settled;
    std::vector<VariableChange> piece_changes; // in the companion's frames
    uint16_t piece_reads = 0;
    uint16_t companion_reads = 0;
    double piece_ms = 0.0;
    for (int round = 0; round < kBesideRounds; round++)
    {
        // The companion takes the game's writes, and the piece's changes in the frame after them.
        BesideTiming timing;
        timing.companion = companion_options;
        timing.companion.changes = script.writes;
        const std::vector<VariableChange> from_piece = ReadChanges(piece_changes, companion_reads, 0, 1);
        timing.companion.changes.insert(timing.companion.changes.end(), from_piece.begin(), from_piece.end());
        const double stop =
            settled ? static_cast<double>(settled->start) * settings.frame_ms + piece_ms + kHoldSeconds * 1000.0
                    : kHandOverMs;
        const Performance played = PlayOnce(companion, companion_regions, timing.companion, settings, stop);
        const auto hands_over = [&script](const VariableChange& c)
        {
            return c.index == script.hand_over && c.value == script.hand_over_value;
        };
        const auto hand_over = std::ranges::find_if(played.global_changes, hands_over);
        if (hand_over == played.global_changes.end())
        {
            return std::nullopt;
        }

        // The piece starts in that frame with the global variables as the game and the companion have left them. It
        // takes the game's later writes and the companion's later changes in their frames.
        timing.start = hand_over->frame;
        timing.piece = piece_options;
        const std::array<int16_t, 16> globals = GlobalsAfter(timing.start, script.writes, played.global_changes);
        for (int i = 0; i < 16; i++)
        {
            timing.piece.variables.try_emplace(static_cast<uint8_t>(16 + i), globals[i]);
        }
        const auto shift = -static_cast<int64_t>(timing.start);
        const std::vector<VariableChange> game = ReadChanges(script.writes, piece_reads, timing.start + 1, shift);
        const std::vector<VariableChange> from_companion =
            ReadChanges(played.global_changes, piece_reads, timing.start + 1, shift);
        timing.piece.changes.insert(timing.piece.changes.end(), game.begin(), game.end());
        timing.piece.changes.insert(timing.piece.changes.end(), from_companion.begin(), from_companion.end());

        PerformOptions longer = timing.piece;
        longer.loops = std::max(piece_options.loops, 1) + 1;
        const Performance piece_played = PlayOnce(piece, piece_regions, longer, settings, std::nullopt);
        std::vector<VariableChange> changes = piece_played.global_changes;
        for (VariableChange& change : changes)
        {
            change.frame += timing.start;
        }

        const bool same = settled && settled->start == timing.start && changes == piece_changes &&
                          piece_played.globals_read == piece_reads && played.globals_read == companion_reads &&
                          settled->piece.changes == timing.piece.changes;
        piece_changes = std::move(changes);
        piece_reads = piece_played.globals_read;
        companion_reads = played.globals_read;
        piece_ms = TempoMs(piece_played, piece_played.end_tick);
        settled = std::move(timing);
        if (same)
        {
            break;
        }
    }

    // The companion's final options take the piece's last changes.
    settled->companion = companion_options;
    settled->companion.changes = script.writes;
    const std::vector<VariableChange> from_piece = ReadChanges(piece_changes, companion_reads, 0, 1);
    settled->companion.changes.insert(settled->companion.changes.end(), from_piece.begin(), from_piece.end());

    return settled;
}

// A merge of a piece's performance and its companion's (MergeBeside): the performance, and the source of each of its
// tracks. A source is 0 for the piece or 1 for the companion, and the sound's track. Tracks that the merge doesn't
// use have the source {-1, -1}.
struct BesideMerge
{
    Performance performance;
    std::array<std::pair<int, int>, kTrackCount> sources;
};

// The start of a merge of the piece's performance `lead`, labelled `lead_label`, and the companion's performance
// `companion`, labelled `companion_label` (MergeBeside): the piece's timing, loop and end, with the two sounds' level
// cut, random draws, approximations and warnings.
BesideMerge StartBesideMerge(const Performance& lead, const std::string& lead_label, const Performance& companion,
                             const std::string& companion_label)
{
    BesideMerge merge;
    merge.sources.fill({-1, -1});
    Performance& piece = merge.performance;
    piece.timebase = lead.timebase;
    piece.tempo = lead.tempo;
    piece.frame_ticks = lead.frame_ticks;
    piece.loop = lead.loop;
    piece.loops = lead.loops;
    piece.end_tick = lead.end_tick;
    piece.seconds = lead.seconds;
    piece.truncated = lead.truncated;
    piece.holds = lead.holds;
    piece.level_cut_db = std::max(lead.level_cut_db, companion.level_cut_db);
    piece.draws = lead.draws || companion.draws;
    for (const auto& [what, count] : lead.approximations)
    {
        piece.approximations[what] += count;
    }
    for (const auto& [what, count] : companion.approximations)
    {
        piece.approximations[what] += count;
    }
    for (const std::string& warning : lead.warnings)
    {
        piece.warnings.push_back(lead_label + ": " + warning);
    }
    for (const std::string& warning : companion.warnings)
    {
        piece.warnings.push_back(companion_label + ": " + warning);
    }

    return merge;
}

// A companion event at its place in the piece's ticks, with what the comparison of passes compares (MergeBeside).
struct PlacedEvent
{
    double place = 0.0;
    TrackEvent event;
};

// A track of the companion in the piece's ticks (MergeBeside): the settings before the piece's start, the last of each
// kind on each part, the notes left out, and the other events at their places.
struct PlacedTrack
{
    std::map<std::pair<uint8_t, EventKind>, TrackEvent> settings_at_start;
    std::set<uint32_t> left_out;
    std::vector<PlacedEvent> placed;
};

// Places the events of track `t` of the companion's performance `companion` in the ticks of the piece's performance
// `lead` (MergeBeside). The companion's notes take ids after `note_base`, and its presets go into `presets` with the
// companion's bank slots. The companion started `offset_ms` before the piece. An event within half a frame
// (`frame_ms`) of the start counts as in the start's frame.
PlacedTrack PlaceCompanionTrack(const Performance& companion, int t, const Performance& lead, uint32_t note_base,
                                double offset_ms, double frame_ms, std::vector<PresetKey>& presets)
{
    PlacedTrack track;
    auto& settings_at_start = track.settings_at_start;
    auto& left_out = track.left_out;
    auto& placed = track.placed;
    for (TrackEvent e : companion.tracks[t])
    {
        if (e.note != 0)
        {
            e.note += note_base;
        }
        if (e.kind == EventKind::kPreset)
        {
            PresetKey key = companion.presets[e.preset];
            key.bank_slot = static_cast<uint8_t>(key.bank_slot + BankSet::kCompanionSlot);
            e.preset = seqlib::midi::PresetIndex(presets, key);
        }

        const double ms = TempoMs(companion, EventPlace(companion, e)) - offset_ms;
        const bool note = e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff;
        if (ms < -0.5 * frame_ms)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                left_out.insert(e.note);
            }
            else if (!note && e.kind != EventKind::kSoundOff)
            {
                e.tick = 0;
                e.after = 0.0;
                settings_at_start[{e.part, e.kind}] = e;
            }
            continue;
        }
        if (e.kind == EventKind::kNoteOff && left_out.contains(e.note))
        {
            continue;
        }

        placed.push_back({TempoTick(lead, std::max(ms, 0.0)), e});
    }

    return track;
}

// Whether the companion's events `placed` repeat with the loop `loop` of the piece's performance `lead` (MergeBeside),
// in frames of `frame_ms`.
bool RepeatsWithLoop(const Performance& lead, const Performance::Loop& loop, const std::vector<PlacedEvent>& placed,
                     double frame_ms)
{
    const auto same_event = [](const TrackEvent& a, const TrackEvent& b)
    {
        return a.kind == b.kind && a.key == b.key && a.value == b.value && a.preset == b.preset &&
               a.semitones == b.semitones && a.part == b.part;
    };
    const auto frame_start = [&lead, frame_ms](double tick)
    {
        return std::floor((TempoMs(lead, tick) + 0.01) / frame_ms) * frame_ms;
    };
    const double first = frame_start(loop.start);
    const double second = frame_start(loop.end);
    const double third = frame_start(loop.end + (loop.end - loop.start));
    std::vector<std::pair<double, TrackEvent>> marked;
    std::vector<std::pair<double, TrackEvent>> next;
    for (const PlacedEvent& p : placed)
    {
        const double ms = TempoMs(lead, p.place) + 0.01;
        if (p.event.kind == EventKind::kNoteOff)
        {
            continue;
        }
        if (ms >= first && ms < second)
        {
            marked.push_back({ms - first, p.event});
        }
        else if (ms >= second && ms < third)
        {
            next.push_back({ms - second, p.event});
        }
    }
    const auto same =
        [&same_event, frame_ms](const std::pair<double, TrackEvent>& a, const std::pair<double, TrackEvent>& b)
    {
        return std::abs(a.first - b.first) < 1.5 * frame_ms && same_event(a.second, b.second);
    };
    return std::ranges::equal(marked, next, same);
}

// Adds the companion's track `track` to `events` (MergeBeside): its settings from before the piece's start at the
// start, and its events up to the piece's end, tick `end`. A note that sounds at the end stops there.
void AddCompanionEvents(PlacedTrack& track, double end, std::vector<TrackEvent>& events)
{
    auto& settings_at_start = track.settings_at_start;
    auto& left_out = track.left_out;
    for (const auto& [kind, e] : settings_at_start)
    {
        events.push_back(e);
    }
    for (PlacedEvent& p : track.placed)
    {
        TrackEvent& e = p.event;
        if (p.place >= end)
        {
            // A note that sounds at the piece's end stops there.
            if (e.kind != EventKind::kNoteOff || left_out.contains(e.note))
            {
                if (e.kind == EventKind::kNoteOn)
                {
                    left_out.insert(e.note);
                }
                continue;
            }
            p.place = end;
        }
        std::tie(e.tick, e.after) = TickAndAfter(p.place);
        events.push_back(e);
    }
    const auto earlier = [](const TrackEvent& a, const TrackEvent& b)
    {
        return a.tick + a.after < b.tick + b.after;
    };
    std::ranges::stable_sort(events, earlier);
}

// Merges the piece's performance `lead`, labelled `lead_label`, and the companion's performance `companion`, labelled
// `companion_label` (PerformBeside). The companion started `offset_ms` before the piece. The merge keeps the piece's
// timing, loop and end. It contains the piece's tracks with notes, then the companion's, each named after its sequence
// and its track. The companion's events go where the piece's tempo puts their times. The companion's notes before the
// piece's start, or from the piece's end, are left out. Its settings from before the start move to the start. An event
// of the companion within half a frame (`frame_ms`) of the start counts as in the start's frame. Rounding can put such
// an event a little before the start. A note that sounds at the end stops there. The companion's presets use the
// companion's slots in the piece's banks (BankSet::SetCompanion). Where the piece loops, the companion's events of the
// marked pass have to come again in the pass after it, to within one and a half frames. The companion plays on past the
// piece's end for that.
// Otherwise the piece has no loop, with a warning. A track of the companion without a note between the piece's start
// and its end is left out. Throws FormatError when the tracks with notes are more than MIDI's 16.
BesideMerge MergeBeside(const Performance& lead, const std::string& lead_label, const Performance& companion,
                        const std::string& companion_label, double offset_ms, double frame_ms)
{
    BesideMerge merge = StartBesideMerge(lead, lead_label, companion, companion_label);
    Performance& piece = merge.performance;

    int next_track = 0;
    uint32_t note_base = 0;
    const auto add_track = [&](const Performance& perf, const std::string& label, int source, int t)
    {
        if (next_track == kTrackCount)
        {
            throw xsflib::FormatError("the sequences play notes on more tracks than MIDI's 16 channels");
        }

        piece.track_names[next_track] = label + " Track " + std::to_string(t);
        piece.biquad_types[next_track] = perf.biquad_types[t];
        merge.sources[next_track] = {source, t};
        return &piece.tracks[next_track++];
    };

    for (int t = 0; t < kTrackCount; t++)
    {
        if (!TrackPlays(lead.tracks[t]))
        {
            continue;
        }

        std::vector<TrackEvent>& events = *add_track(lead, lead_label, 0, t);
        for (TrackEvent e : lead.tracks[t])
        {
            note_base = std::max(note_base, e.note);
            if (e.kind == EventKind::kPreset)
            {
                e.preset = seqlib::midi::PresetIndex(piece.presets, lead.presets[e.preset]);
            }
            events.push_back(e);
        }
    }

    const double end = piece.end_tick;
    bool repeats = true;
    for (int t = 0; t < kTrackCount; t++)
    {
        if (!TrackPlays(companion.tracks[t]))
        {
            continue;
        }

        PlacedTrack track = PlaceCompanionTrack(companion, t, lead, note_base, offset_ms, frame_ms, piece.presets);

        // Where the piece loops, the companion's events in the marked pass have to come again one pass later. The
        // companion's events are at the starts of their frames, and the piece's frames bound the passes: the frame of
        // the loop's start, of its end, and of the end of the pass after it. Rounding can put an event a little before
        // the start of its frame. Events compare by their times from their pass's start, to within one and a half
        // frames. The companion's ticks needn't keep step with the piece's frames. An event can then move by a frame
        // from one pass to the next. A note-off at the start of a pass can end a note of the pass before. The note-offs
        // therefore don't count.
        if (piece.loop && repeats)
        {
            repeats = RepeatsWithLoop(lead, *piece.loop, track.placed, frame_ms);
        }

        const auto sounds_by_end = [end](const PlacedEvent& p)
        {
            return p.event.kind == EventKind::kNoteOn && p.place < end;
        };
        if (!std::ranges::any_of(track.placed, sounds_by_end))
        {
            continue;
        }

        AddCompanionEvents(track, end, *add_track(companion, companion_label, 1, t));
    }

    if (piece.loop && !repeats)
    {
        piece.DropLoop();
        piece.warnings.push_back(companion_label + " doesn't repeat with the loop of " + lead_label +
                                 ": the piece has no loop markers");
    }

    return merge;
}

// The shared level scale and the part limits that the piece and its companion take for their merge `merge`
// (PerformBeside), as PieceRuns gives them to the sequences of a piece.
std::pair<SharedPasses, SharedPasses> BesideShares(const Passes& lead, const Passes& companion,
                                                   const BesideMerge& merge)
{
    const Performance& merged = merge.performance;
    std::pair<SharedPasses, SharedPasses> shares;
    const double peak = std::max(lead.peak, companion.peak);
    if (LevelScale(peak) != LevelScale(lead.peak))
    {
        shares.first.peak = peak;
    }
    if (LevelScale(peak) != LevelScale(companion.peak))
    {
        shares.second.peak = peak;
    }

    std::array<uint8_t, kTrackCount> used{};
    for (int t = 0; t < kTrackCount; t++)
    {
        for (const TrackEvent& e : merged.tracks[t])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                used[t] = std::max<uint8_t>(used[t], static_cast<uint8_t>(e.part + 1));
            }
        }
    }
    const std::array<uint8_t, kTrackCount> fitted = FitParts(merged, used);

    const auto limit = [&](int source, SharedPasses& share)
    {
        std::array<uint8_t, kTrackCount> limits;
        limits.fill(1);
        bool over = false;
        for (int m = 0; m < kTrackCount; m++)
        {
            if (merge.sources[m].first == source)
            {
                limits[merge.sources[m].second] = fitted[m];
                over = over || fitted[m] < used[m];
            }
        }
        if (over)
        {
            share.part_limits = limits;
        }
    };
    limit(0, shares.first);
    limit(1, shares.second);

    return shares;
}

} // namespace

Performance PerformBeside(const SoundArchive& archive, uint32_t sound_index, BankSet& banks,
                          const SoundArchive& companion_archive, uint32_t companion_index, BankSet& companion_banks,
                          const CompanionScript& script, const PerformOptions& options, const Settings& settings)
{
    const SarSequence piece = ReadSarSequence(archive, sound_index, settings);
    const SarSequence companion = ReadSarSequence(companion_archive, companion_index, settings);
    banks.SetCompanion(&companion_banks);
    SarRegions piece_regions(banks);
    SarRegions companion_regions(companion_banks);

    // The companion plays with the piece's options, but not with its variables, changes, tracks, banks and volume. It
    // plays as long as the piece.
    PerformOptions companion_options = options;
    companion_options.variables.clear();
    companion_options.track_variables.clear();
    companion_options.changes.clear();
    companion_options.procedure_changes.clear();
    companion_options.tracks = 0xffff;
    companion_options.banks.reset();
    companion_options.volume = script.volume;
    companion_options.loops = std::numeric_limits<int>::max();

    const std::optional<BesideTiming> timing =
        TimeBeside(piece, piece_regions, options, companion, companion_regions, companion_options, script, settings);
    if (!timing)
    {
        throw xsflib::FormatError(companion.sound.name + " doesn't hand over to " + piece.sound.name +
                                  " within 30 seconds");
    }

    // The companion plays on past the piece's end by a pass of the piece's loop, for MergeBeside's comparison.
    const double offset_ms = static_cast<double>(timing->start) * settings.frame_ms;
    const auto stop = [offset_ms](const Performance& lead)
    {
        double ms = offset_ms + TempoMs(lead, lead.end_tick) + kHoldSeconds * 1000.0;
        if (lead.loop)
        {
            ms += TempoMs(lead, lead.loop->end) - TempoMs(lead, lead.loop->start);
        }

        return ms;
    };

    SharedPasses lead_share;
    SharedPasses companion_share;
    Passes lead = PerformPasses(piece.sound, piece.seq, piece_regions, timing->piece, settings, lead_share);
    Passes played = PerformPasses(companion.sound, companion.seq, companion_regions, timing->companion, settings,
                                  companion_share, {}, stop(lead.performance));
    const auto same = [](const SharedPasses& a, const SharedPasses& b)
    {
        return a.peak == b.peak && a.part_limits == b.part_limits;
    };
    for (int round = 0; round < 3; round++)
    {
        BesideMerge merged = MergeBeside(lead.performance, piece.sound.name, played.performance, companion.sound.name,
                                         offset_ms, settings.frame_ms);
        const auto [lead_now, companion_now] = BesideShares(lead, played, merged);
        if (same(lead_now, lead_share) && same(companion_now, companion_share))
        {
            return std::move(merged.performance);
        }

        if (!same(lead_now, lead_share))
        {
            lead_share = lead_now;
            lead = PerformPasses(piece.sound, piece.seq, piece_regions, timing->piece, settings, lead_share);
        }
        companion_share = companion_now;
        played = PerformPasses(companion.sound, companion.seq, companion_regions, timing->companion, settings,
                               companion_share, {}, stop(lead.performance));
    }

    return MergeBeside(lead.performance, piece.sound.name, played.performance, companion.sound.name, offset_ms,
                       settings.frame_ms)
        .performance;
}

} // namespace ntdlib::sequence_export
