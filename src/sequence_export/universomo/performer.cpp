// SPDX-License-Identifier: MIT

// The performer of Universomo's songs (see convert.h): the model's sound commands as MIDI events.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "seqlib/midi/performance.h"
#include "seqlib/midi/recorder.h"
#include "xsflib/binary.h"

#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/universomo/convert.h"
#include "ntdlib/universomo/formats.h"
#include "ntdlib/universomo/player.h"
#include "ntdlib/universomo/walk.h"

namespace ntdlib::sequence_export::universomo
{
namespace
{

namespace uv = ntdlib::universomo;
using seqlib::midi::EventKind;
using seqlib::midi::TrackOutput;

// The samples that a channel plays in a tick, times its timer's period: half the system clock's cycles in a frame.
constexpr double kTimerCyclesPerTick = uv::kFrameCycles / 2.0;

// The longest a song may play before its first loop or its end: an hour. The performer then gives up on it. The passes
// after the first don't count: the comparison and the loops require them.
constexpr uint64_t kMaxTicks = static_cast<uint64_t>(3600 * uv::kTicksPerSecond);

// The MIDI file's quarter note: 4 rows of the song's first speed.
constexpr uint32_t kRowsPerQuarter = 4;

const LoopRules kLoopRules{{}, false};

// The rules that compare two passes without their bends. A vibrato's position continues from pass to pass and never
// resets at a note. Where a pass's vibratos don't add up to whole turns, every pass bends otherwise than the one
// before, and no pass plays as the next one does.
const LoopRules kWithoutBends{{}, false, {EventKind::kPitch}};

// The note that a track's channel plays.
struct Voice
{
    uint32_t id = 0;
    uint8_t key = 60;
    bool once = false;   // the sample plays once, and ends after `length` samples
    double played = 0.0; // samples played so far
    double length = 0.0; // samples to play
    uint16_t timer = 0;  // the channel's timer
};

// The pitch in semitones of a channel at timer period `timer`, against the period of pitch 0 (key 60).
double SemitonesOf(uint16_t timer)
{
    return 12.0 * std::log2(static_cast<double>(uv::kBaseTimer) / timer);
}

// CC11 for a channel at hardware `volume` and `divider` with `pan`: the level and the pan's gain, as a square.
uint8_t ExpressionOf(uint8_t volume, uint8_t divider, uint8_t pan)
{
    const double level = uv::LevelOf(volume, divider) * PanGain(pan);
    return static_cast<uint8_t>(std::clamp<long>(std::lround(127.0 * std::sqrt(level)), 0, 127));
}

class Performer
{
public:
    Performer(const uv::Song& song, const uv::SampleBank& bank, const PerformOptions& options)
        : bank_(bank), player_(song, bank, options.loop)
    {
        out_.timebase = std::max<uint32_t>(song.speed, 1) * kRowsPerQuarter;
        recorder_.SetTempo(out_, 0, 60.0 * uv::kTicksPerSecond / out_.timebase);
        outputs_.resize(song.track_count);
        voices_.resize(song.track_count);
        for (std::size_t t = 0; t < outputs_.size(); t++)
        {
            outputs_[t].midi_track = static_cast<uint8_t>(t);
            pans_[t] = (t & 1) != 0 ? uv::kOddPan : uv::kEvenPan;
        }
    }

    // Plays ticks until the song stops, or until `passes` passes of its loop have ended. A pass ends where the update
    // reads the first row of the restart order: the update after the one that reads the last row, and wraps, reads it.
    void PlayUntil(uint32_t passes)
    {
        while (player_.Playing() && wrap_ticks_.size() < passes)
        {
            if (wrap_ticks_.empty() && tick_ >= kMaxTicks)
            {
                throw xsflib::FormatError("the song plays for more than an hour without looping");
            }

            const uint32_t wraps = player_.Wraps();
            const std::vector<uv::Command>& commands = player_.Update();
            if (player_.Tick() == 1 && wraps > wrap_ticks_.size())
            {
                wrap_ticks_.push_back(tick_);
            }
            for (const uv::Command& c : commands)
            {
                Record(c);
            }
            EndSamples();
            tick_++;
        }
    }

    // The ticks played so far.
    uint32_t Ticks() const
    {
        return tick_;
    }

    // The ticks at which each pass of the loop after the first starts: the ticks that read its first row.
    const std::vector<uint32_t>& WrapTicks() const
    {
        return wrap_ticks_;
    }

    // Ends every note that still sounds at the current tick, and gives the performance its end.
    Performance Finish(uint32_t end_tick)
    {
        for (std::size_t t = 0; t < voices_.size(); t++)
        {
            EndNote(t, tick_);
        }
        seqlib::midi::EndPassAt(out_, end_tick);
        out_.end_tick = end_tick;
        out_.seconds = end_tick / uv::kTicksPerSecond;
        for (const auto& [what, count] : recorder_.Approximations())
        {
            out_.approximations[what] += count;
        }
        out_.warnings.insert(out_.warnings.end(), recorder_.Warnings().begin(), recorder_.Warnings().end());

        return out_;
    }

    const Performance& Recorded() const
    {
        return out_;
    }

private:
    void EndNote(std::size_t track, uint32_t tick)
    {
        Voice& voice = voices_[track];
        if (voice.id == 0)
        {
            return;
        }

        recorder_.NoteOff(out_, outputs_[track].midi_track, tick, outputs_[track].part, voice.key, voice.id);
        voice = Voice{};
    }

    // A channel's set-up ends the track's note and starts the next one.
    void Setup(std::size_t track, const uv::Command& c)
    {
        EndNote(track, tick_);
        TrackOutput& output = outputs_[track];
        const uv::Sample& sample = bank_.samples.at(c.sample);
        const uint32_t bytes_per_sample = sample.bits == 16 ? 2 : 1;
        const double played_bytes = c.loop_mode == 1 ? 0.0 : c.length * 4.0;
        if (c.loop_mode != 1 && played_bytes == 0.0)
        {
            recorder_.Approximate("notes of an empty sample left out");
            return;
        }
        if (c.offset != 0)
        {
            recorder_.Approximate("sample offsets left out (the note starts at the sample's start)");
        }

        const double semitones = SemitonesOf(c.timer);
        const int key = std::clamp(60 + static_cast<int>(std::lround(semitones)), 0, 127);

        // The driver has no reverb send. CC91 is 0: a synth that follows General MIDI 2 gives a channel without CC91 a
        // send of 40.
        recorder_.SendSetting(out_, output, tick_, EventKind::kVolume, 127);
        recorder_.SendSetting(out_, output, tick_, EventKind::kPan, PanController(c.pan));
        recorder_.SendSetting(out_, output, tick_, EventKind::kExpression, ExpressionOf(c.volume, c.divider, c.pan));
        recorder_.SendSetting(out_, output, tick_, EventKind::kReverb, 0);
        recorder_.SendPitch(out_, output, tick_, static_cast<float>(semitones - (key - 60)));
        recorder_.SendPreset(out_, output, tick_, seqlib::midi::PresetIndex(out_.presets, c.sample));

        Voice& voice = voices_[track];
        voice.id = recorder_.NoteOn(out_, output, tick_, static_cast<uint8_t>(key), 127);
        voice.key = static_cast<uint8_t>(key);
        voice.once = c.loop_mode != 1;
        voice.length = played_bytes / bytes_per_sample;
        voice.timer = c.timer;
        pans_[track] = c.pan;
    }

    void Record(const uv::Command& c)
    {
        const std::size_t track = c.channel;
        if (track >= outputs_.size())
        {
            return;
        }

        TrackOutput& output = outputs_[track];
        switch (c.kind)
        {
        case uv::Command::Kind::kSetup:
            Setup(track, c);
            break;

        case uv::Command::Kind::kStart:
            break;

        case uv::Command::Kind::kVolume:
            recorder_.SendSetting(out_, output, tick_, EventKind::kExpression,
                                  ExpressionOf(c.volume, c.divider, pans_[track]));
            break;

        case uv::Command::Kind::kTimer:
            {
                Voice& voice = voices_[track];
                voice.timer = c.timer;
                if (voice.id != 0)
                {
                    recorder_.SendPitch(out_, output, tick_,
                                        static_cast<float>(SemitonesOf(c.timer) - (voice.key - 60)));
                }
                break;
            }
        }
    }

    // A sample that plays once ends on the tick after the one in which its last sample plays.
    void EndSamples()
    {
        for (std::size_t t = 0; t < voices_.size(); t++)
        {
            Voice& voice = voices_[t];
            if (voice.id == 0 || !voice.once)
            {
                continue;
            }

            voice.played += kTimerCyclesPerTick / voice.timer;
            if (voice.played >= voice.length)
            {
                EndNote(t, tick_ + 1);
            }
        }
    }

    const uv::SampleBank& bank_;
    uv::Player player_;
    Performance out_;
    seqlib::midi::Recorder recorder_;
    std::vector<TrackOutput> outputs_;
    std::vector<Voice> voices_;
    std::array<uint8_t, uv::kMaxTracks> pans_ = {};
    uint32_t tick_ = 0;
    std::vector<uint32_t> wrap_ticks_;
};

// The notes of `recorded` that start before `tick` and end after it, or never. The game plays a note at the loop's end
// on into the next pass. A player that loops on the markers cuts it at the jump.
uint32_t NotesAcross(const Performance& recorded, uint32_t tick)
{
    uint32_t count = 0;
    for (const std::vector<seqlib::midi::TrackEvent>& events : recorded.tracks)
    {
        std::map<uint32_t, uint32_t> starts; // the start of each note that hasn't ended, by its ID
        for (const seqlib::midi::TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                starts[e.note] = e.tick;
            }
            else if (e.kind == EventKind::kNoteOff)
            {
                const auto start = starts.find(e.note);
                if (start != starts.end())
                {
                    count += start->second < tick && e.tick > tick ? 1 : 0;
                    starts.erase(start);
                }
            }
        }
        count +=
            static_cast<uint32_t>(std::ranges::count_if(starts, [tick](const auto& s) { return s.second < tick; }));
    }

    return count;
}

} // namespace

Performance Perform(const uv::Song& song, const uv::SampleBank& bank, const PerformOptions& options)
{
    Performer performer(song, bank, options);
    if (!options.loop)
    {
        // The update that stops the song sets every channel's volume to 0: the song ends on its tick.
        performer.PlayUntil(1);

        return performer.Finish(std::max<uint32_t>(performer.Ticks(), 1) - 1);
    }

    // Play a pass more than the loops after the second pass. The second pass then has a pass after it to compare with.
    // The first pass that plays as the next one does is the marked one.
    const int loops = std::max(options.loops, 1);
    performer.PlayUntil(static_cast<uint32_t>(loops) + 2);
    const std::vector<uint32_t>& wraps = performer.WrapTicks();
    std::vector<std::pair<uint32_t, uint32_t>> passes = {{0, wraps.at(0)}};
    for (std::size_t i = 1; i < wraps.size(); i++)
    {
        passes.emplace_back(wraps[i - 1], wraps[i]);
    }

    // The first pass that plays as the next one does, then the first that does but for its vibratos' bends.
    const auto repeats = [&](std::size_t pass, const LoopRules& rules)
    {
        return RepeatsAsTheGame(performer.Recorded(), {}, passes[pass].first, passes[pass].second, rules);
    };
    std::size_t marked = 1;
    bool bends_differ = false;
    bool steady = true;
    if (repeats(0, kLoopRules))
    {
        marked = 0;
    }
    else if (!repeats(1, kLoopRules))
    {
        bends_differ = true;
        marked = repeats(0, kWithoutBends) ? 0 : 1;
        steady = marked == 0 || repeats(1, kWithoutBends);
    }

    const uint32_t loop_start = passes[marked].first;
    const uint32_t loop_end = passes[marked].second;
    const uint32_t end = passes[marked + static_cast<std::size_t>(loops) - 1].second;
    // Where the bends differ from pass to pass, the loop's start sends the marked pass's bends again. A MIDI player
    // that loops on the markers otherwise keeps the loop end's bend for a note that sends none.
    PassStart next_pass = PassStartAt(performer.Recorded(), {}, loop_end);
    if (bends_differ)
    {
        for (const auto& [key, event] : PassStartAt(performer.Recorded(), {}, loop_start).settings)
        {
            if (std::get<2>(key) == EventKind::kPitch)
            {
                next_pass.settings[key] = event;
            }
        }
    }
    const uint32_t across = NotesAcross(performer.Recorded(), loop_end);
    Performance out = performer.Finish(end);
    out.loop = seqlib::midi::Performance::Loop{loop_start, loop_end};
    out.loops = loops;
    RestoreAtLoopStart(out, {}, next_pass, kLoopRules);
    if (!steady)
    {
        out.warnings.push_back("the loop's second pass plays otherwise than the third; the markers loop the second");
    }
    else if (bends_differ)
    {
        out.approximations["vibratos whose phase moves from pass to pass (the loop repeats one pass's phase)"]++;
    }
    if (across > 0)
    {
        out.approximations["notes that sound across the loop's end (a player that loops on the markers cuts them)"] +=
            across;
    }

    return out;
}

} // namespace ntdlib::sequence_export::universomo
