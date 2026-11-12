// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/logikstate/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/logikstate/driver.h"
#include "ntdlib/logikstate/mod1_player.h"
#include "ntdlib/logikstate/mod30_player.h"
#include "ntdlib/logikstate/player.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/module_bars.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/logikstate/soundfont.h"

namespace ntdlib::sequence_export::logikstate
{

using ntdlib::agb::LoopStart;
using ntdlib::agb::PassComparison;
using ntdlib::agb::WriteFile;
using ntdlib::logikstate::Build;
using ntdlib::logikstate::Channel;
using ntdlib::logikstate::Format;
using ntdlib::logikstate::kMixChannels;
using ntdlib::logikstate::kMod30Records;
using ntdlib::logikstate::Mod1Player;
using ntdlib::logikstate::Mod30Channel;
using ntdlib::logikstate::Mod30Player;
using ntdlib::logikstate::Mod30Settings;
using ntdlib::logikstate::Player;
using ntdlib::logikstate::Song;
using ntdlib::sequence_export::agb::BarStarts;
using ntdlib::sequence_export::agb::BarTicks;
using ntdlib::sequence_export::agb::BendRangeFor;
using ntdlib::sequence_export::agb::ChannelWriter;
using ntdlib::sequence_export::agb::ChooseMeter;
using ntdlib::sequence_export::agb::kSmallBend;
using ntdlib::sequence_export::agb::MelodicChannel;
using ntdlib::sequence_export::agb::Meter;
using ntdlib::sequence_export::agb::MidiFile;
using ntdlib::sequence_export::agb::MidiTrack;
using ntdlib::sequence_export::agb::ModuleStep;
using ntdlib::sequence_export::agb::NumberedPreset;
using ntdlib::sequence_export::agb::OrderRun;
using ntdlib::sequence_export::agb::OrderRuns;
using ntdlib::sequence_export::agb::SongNumber;
using ntdlib::sequence_export::agb::SongStem;
using ntdlib::sequence_export::agb::WriteBarSignatures;

namespace
{

// The MIDI ticks of a row. The ticks of the player's effects don't keep step with the rows. Each one therefore falls on
// the MIDI tick nearest its place in its row.
constexpr uint32_t kRowTicks = 120;

// The cents by which a note's pitch can miss its key before its bend makes up the difference: more than the table of
// periods misses equal temperament by in the octaves that modules use.
constexpr double kTuningCents = 3;

// The level of a channel at which a SoundFont sample plays at full scale: two FIFOs play each channel's points times
// its volume over 256. One FIFO plays them at half that level.
constexpr double kFullLevel = 128;
constexpr int kFullFifos = 2;

// The share of that level that a game plays once it has switched the sound off: both FIFOs at half volume.
constexpr double kHalfVolume = 0.5;

// The longest conversion, in seconds.
constexpr uint64_t kMaxSeconds = 60 * 60;

// The most channels of a module of any format.
constexpr size_t kMaxVoices = std::max(size_t(kMixChannels), size_t(kMod30Records));

// A channel of the mixer after a row or a tick.
struct Voice
{
    bool playing = false;
    uint32_t starts = 0; // the channel's count of notes and changes of sample
    PlayedSample sample; // with the point that the channel plays as its offset
    int number = 0;      // the sample's number in the module, from 1
    double rate = 0;     // Hz
    double level = 0;    // where kFullLevel is full scale
};

// What the conversion keeps of a row or of a tick of effects.
struct StepData
{
    uint64_t time = 0; // the mixer's samples from the song's start
    size_t row = 0;    // the row that it belongs to: a place in the rows
    std::array<Voice, kMaxVoices> voices;
};

// A row that the player played.
struct RowData
{
    size_t step = 0; // its place in the steps
    int order = 0;
    int pattern_row = 0;  // its number in its pattern
    uint32_t samples = 0; // its length
};

// The stretch of a song that the conversion covers, in rows.
struct Plan
{
    bool loops = false;
    size_t loop_start = 0; // the first row whose place in the module comes back
    size_t loop_end = 0;   // where it comes back
    size_t end = 0;
};

// The model's output for the stretch of a song that the plan covers.
struct Simulation
{
    std::vector<StepData> steps; // up to and including the plan's last row
    std::vector<RowData> rows;
    Plan plan;
    uint32_t channels = 0;
    uint32_t rate = 0;
    bool truncated = false; // cut off without a loop
    std::vector<std::string> warnings;
};

// A song, the build of the driver that plays it, the music's volume that the game plays it at, the FIFOs that play
// the mix, and whether the game plays its FIFOs at half volume. A GBAMOD30 module plays with the game's settings of
// the driver.
struct Play
{
    Song song;
    Build build = Build::kShortRecords;
    int32_t music_volume = 0;
    int fifos = kFullFifos;
    bool half_volume = false;
    Mod30Settings mod30;
};

// Returns the share of the full level at which the FIFOs play.
double FifoScale(const Play& play)
{
    return (play.half_volume ? kHalfVolume : 1.0) * play.fifos / kFullFifos;
}

// Returns a player of the song's format.
template <typename P>
P MakePlayer(const Rom& rom, const Play& play)
{
    if constexpr (std::is_same_v<P, Mod1Player>)
    {
        return P(rom, play.song, play.music_volume);
    }
    else if constexpr (std::is_same_v<P, Mod30Player>)
    {
        Mod30Settings settings = play.mod30;
        settings.music = play.music_volume;
        settings.loop = play.song.loop_flag;
        return P(rom, play.song.module, settings);
    }
    else
    {
        return P(rom, play.song, play.music_volume, play.build);
    }
}

// Returns the channels of a player's module, and the mixer's rate in hertz.
template <typename P>
uint32_t ChannelCount(const P& player)
{
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        return uint32_t(player.Channels());
    }
    else
    {
        return player.Header().channels;
    }
}

template <typename P>
uint32_t MixerRate(const P& player)
{
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        return player.Rate();
    }
    else
    {
        return player.Header().rate;
    }
}

// Returns channel `number` of the mixer as the player has it after a row or a tick. `scale` is the FIFOs' share of
// their full volume.
template <typename P>
Voice VoiceOf(const Rom& rom, const P& player, int number, double scale)
{
    const Channel& ch = player.Channels()[size_t(number)];
    Voice v;
    v.starts = ch.starts;
    v.playing = player.Sounding(ch, number);
    if (!v.playing)
    {
        return v;
    }

    // The point that the channel plays. Past its sample's end, the channel plays the loop from its start at the mixer's
    // next sample, or stops there.
    v.sample = {ch.first_point,
                ch.whole_length,
                ch.whole_loop_start,
                ch.whole_loop_length,
                SampleRate(rom, player.Header(), ch.fine_tune),
                0,
                Loops(ch.whole_loop_length)};
    const bool in_loop = ch.points != ch.first_point || ch.length != ch.whole_length;
    const uint32_t point = ch.position >> 12;
    if (point >= ch.length && !Loops(ch.loop_length))
    {
        v.playing = false;
        return v;
    }
    if (point >= ch.length)
    {
        v.sample.offset = LoopOffset(v.sample);
    }
    else
    {
        v.sample.offset = in_loop ? LoopOffset(v.sample) + point : point;
    }

    v.number = ch.sample + 1;
    v.rate = double(ch.step) * player.Header().rate / 4096.0;
    v.level = std::min(player.MixVolume(ch, number) * scale, kFullLevel);

    // In the GBAMOD1 build, a slide down can bring the step to 0 or below it. The sample then stands still or plays
    // backwards. A SoundFont can't play either. The note plays at level 0 until the step is above 0 again or another
    // sample starts, and keeps its bend.
    if constexpr (std::is_same_v<P, Mod1Player>)
    {
        if (int32_t(ch.step) <= 0)
        {
            v.rate = 0;
            v.level = 0;
        }
    }

    return v;
}

// Returns music channel `number` of a GBAMOD30 module as the player has it after a row or a tick.
Voice VoiceOf(const Rom&, const Mod30Player& player, int number, double scale)
{
    const Mod30Channel& ch = player.Records()[size_t(number)];
    Voice v;
    v.starts = ch.starts;
    v.playing = player.Sounding(ch, number);
    if (!v.playing)
    {
        return v;
    }

    // The point that the channel plays. Past its sample's end, the channel plays the loop from its start at the mixer's
    // next sample, or stops there.
    v.sample = {ch.first_point,
                ch.whole_length,
                ch.whole_loop_start,
                ch.whole_loop_length,
                uint32_t(std::lround(player.RootRate(ch.sample))),
                0,
                ch.whole_loop_length != 0};
    const uint32_t point = ch.position >> 12;
    if (ch.position >= ch.length && ch.loop_length == 0)
    {
        v.playing = false;
        return v;
    }
    if (ch.position >= ch.length)
    {
        v.sample.offset = LoopOffset(v.sample);
    }
    else
    {
        v.sample.offset = ch.looped ? LoopOffset(v.sample) + point : point;
    }

    v.number = ch.sample + 1;
    v.rate = double(ch.step) * player.Rate() / 4096.0;
    v.level = std::min(player.MixVolume(ch) * scale, kFullLevel);

    return v;
}

// Returns whether a channel sounds in the rows from `first` up to `last`.
bool Sounds(const Simulation& sim, size_t first, size_t last)
{
    for (size_t i = sim.rows[first].step; i < sim.rows[last].step; i++)
    {
        for (uint32_t c = 0; c < sim.channels; c++)
        {
            if (sim.steps[i].voices[c].playing && sim.steps[i].voices[c].level > 0)
            {
                return true;
            }
        }
    }

    return false;
}

// Runs the model until a row revisits an earlier place in the module with the same speed, tempo and pattern-loop
// state, and the module has played the loop between them `loops` times in all. A loop in which no channel sounds is
// the wait after a jingle: the song then has no loop and ends where that loop starts. From LoopStart::kSecondPass, the
// loop starts at the module's second pass through it. The module then plays the first pass before it.
//
// A song of the GBAMOD1 build also ends at the first row whose order is past the module's last. The driver then clears
// the channels at each frame, and reads rows from the bytes after the module's orders. A song that plays once ends
// where its first row with a jump starts: the game's jingle routine goes back to the piece before at that jump, and
// clears the channels. Neither has a loop.
template <typename P>
Simulation Simulate(const Rom& rom, const Play& play, int loops, LoopStart start_pass)
{
    Simulation sim;
    P player = MakePlayer<P>(rom, play);
    if (!player.Valid())
    {
        sim.warnings = player.Warnings();
        return sim;
    }
    sim.channels = ChannelCount(player);
    sim.rate = MixerRate(player);

    // Two orders that play on alike count as one place. In a GBAMOD30 module, orders RepeatOrders() apart play on
    // alike.
    uint32_t repeat_orders = UINT32_MAX;
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        repeat_orders = player.RepeatOrders();
    }
    std::map<std::tuple<int, int, std::vector<uint32_t>>, size_t> places;
    size_t end = SIZE_MAX;
    const double scale = FifoScale(play);
    const auto record = [&](bool row)
    {
        if (!sim.rows.empty() && sim.rows.size() > end)
        {
            return;
        }

        // A row past the module's last order ends the song.
        if (row && end == SIZE_MAX && uint32_t(player.RowOrder()) >= player.Header().order_count)
        {
            end = sim.rows.size();
        }

        // The first row that comes back to an earlier row's place starts the loop's second pass.
        if (row && end == SIZE_MAX)
        {
            const int order = int(uint32_t(player.RowOrder()) % repeat_orders);
            const auto key = std::make_tuple(order, player.RowNumber(), player.RepeatState());
            const auto [it, added] = places.emplace(key, sim.rows.size());
            if (!added)
            {
                const size_t length = sim.rows.size() - it->second;
                const size_t skip = start_pass == LoopStart::kSecondPass ? length : 0;
                sim.plan.loops = true;
                sim.plan.loop_start = it->second + skip;
                sim.plan.loop_end = sim.rows.size() + skip;
                end = sim.plan.loop_start + size_t(std::max(loops, 1)) * length;
            }
        }
        if (row)
        {
            sim.rows.push_back({sim.steps.size(), player.RowOrder(), player.RowNumber(), player.RowSamples()});
        }
        if constexpr (std::is_same_v<P, Mod1Player>)
        {
            if (row && end == SIZE_MAX && play.song.plays_once && player.RowJumped())
            {
                end = sim.rows.size() - 1;
            }
        }

        StepData d;
        d.time = player.Elapsed();
        d.row = sim.rows.size() - 1;
        for (uint32_t c = 0; c < sim.channels; c++)
        {
            d.voices[c] = VoiceOf(rom, player, int(c), scale);
        }
        sim.steps.push_back(d);
    };
    player.OnStep(record);

    const uint64_t most = kMaxSeconds * uint64_t(sim.rate);
    while (player.Playing() && (sim.rows.empty() || sim.rows.size() <= end) && player.Elapsed() < most)
    {
        player.Frame();
    }

    sim.warnings = player.Warnings();
    if (sim.rows.empty())
    {
        return sim;
    }

    // A GBAMOD30 module without the loop flag stops after its last order's last row. The mixer plays on what the
    // channels hold, and the song ends where that row ends.
    if constexpr (std::is_same_v<P, Mod30Player>)
    {
        if (end == SIZE_MAX && !player.Playing() && player.Valid())
        {
            const RowData last = sim.rows.back();
            StepData d;
            d.time = sim.steps[last.step].time + last.samples;
            d.row = sim.rows.size();
            sim.rows.push_back({sim.steps.size(), last.order, last.pattern_row + 1, last.samples});
            sim.steps.push_back(d);
        }
    }

    // A loop whose first pass doesn't end within the limit doesn't count. The passes after one that does end there.
    if (end != SIZE_MAX && sim.rows.size() <= end)
    {
        if (sim.plan.loops && sim.plan.loop_end >= sim.rows.size())
        {
            sim.plan = {};
            end = SIZE_MAX;
        }
        else
        {
            end = sim.rows.size() - 1;
        }
    }

    // A module that the player can't play on ends where it stops, and one that doesn't loop within the limit is cut
    // off.
    if (end == SIZE_MAX)
    {
        end = sim.rows.size() - 1;
        if (player.Playing())
        {
            sim.warnings.push_back("found no loop within an hour, so the module is cut off there");
            sim.truncated = true;
        }
    }

    sim.rows.resize(std::min(sim.rows.size(), end + 1));
    if (sim.plan.loops && !Sounds(sim, sim.plan.loop_start, std::min(sim.plan.loop_end, sim.rows.size() - 1)))
    {
        sim.plan.loops = false;
        sim.rows.resize(sim.plan.loop_start + 1);
    }
    sim.plan.end = sim.rows.size() - 1;
    sim.steps.resize(sim.rows.back().step + 1);

    return sim;
}

// A row or a tick of a note: how far its pitch is from the note's key, in semitones, and its level.
struct Sound
{
    size_t step = 0;
    double bend = 0;
    double level = 0;
};

// A note: a sample that a channel plays from the row that starts it, or from the row or tick at which its volume rises
// from 0, until it stops, another starts, its volume falls to 0, or the song ends. The mixer of the build with short
// records leaves a channel at volume 0 alone, and the sample waits at its point. In the build with long records, the
// sample moves on in silence. A note then lasts through its rows at volume 0, at level 0.
struct Note
{
    size_t on = 0; // in steps
    size_t off = 0;
    int key = 0;
    int program = 0;
    std::vector<Sound> sounds;
};

// Returns the notes of channel `c`, adding their instruments to `instruments`.
std::vector<Note> MakeNotes(const Simulation& sim, uint32_t c, InstrumentSet& instruments)
{
    std::vector<Note> notes;
    bool open = false;
    double start_rate = 0, tuning = 0;
    const size_t end = sim.steps.size() - 1;
    for (size_t i = 0; i <= end; i++)
    {
        const Voice& v = sim.steps[i].voices[c];
        const Voice* last = i > 0 ? &sim.steps[i - 1].voices[c] : nullptr;
        const bool started = v.playing && (!last || !last->playing || v.starts != last->starts);
        if (open && (!v.playing || started || i == end))
        {
            notes.back().off = i;
            open = false;
        }
        if (i == end)
        {
            break;
        }

        // A note's key is the one nearest its pitch, and a pitch that misses it by more than the table of periods does
        // starts with a bend.
        if (v.playing && !open && v.rate > 0)
        {
            Note n;
            n.on = i;
            n.program = instruments.Program(v.sample, v.number);
            const double semitones = 12 * std::log2(v.rate / v.sample.rate);
            n.key = std::clamp(int(std::lround(kRootKey + semitones)), 0, 127);
            tuning = semitones - (n.key - kRootKey);
            if (std::fabs(tuning) * 100 < kTuningCents)
            {
                tuning = 0;
            }
            start_rate = v.rate;
            notes.push_back(n);
            open = true;
        }

        // Each row and tick of the note: its pitch in a bend from its key, and its level.
        if (open)
        {
            Sound s;
            s.step = i;
            s.bend = notes.back().sounds.empty() ? tuning : notes.back().sounds.back().bend;
            if (v.rate > 0)
            {
                s.bend = tuning + 12 * std::log2(v.rate / start_rate);
            }
            s.level = v.level;
            notes.back().sounds.push_back(s);
        }
    }

    return notes;
}

// MIDI timing. Each row lasts kRowTicks MIDI ticks, and the tempo sets its length.
struct Timing
{
    Meter meter;                                       // in MIDI ticks
    std::vector<uint32_t> rows;                        // the MIDI tick of each row, and of the end
    std::vector<uint32_t> steps;                       // the MIDI tick of each step
    std::vector<std::pair<uint32_t, uint32_t>> tempos; // MIDI tick and microseconds a quarter note
    std::vector<uint32_t> bars;                        // the MIDI ticks that start a bar
};

Timing MakeTiming(const Simulation& sim)
{
    // The rows, and the row of the commonest length.
    Timing t;
    std::vector<ModuleStep> rows;
    std::map<uint32_t, size_t> lengths;
    for (size_t r = 0; r < sim.rows.size(); r++)
    {
        t.rows.push_back(uint32_t(r) * kRowTicks);
        rows.push_back({true, sim.rows[r].order, sim.rows[r].pattern_row, kRowTicks});
        lengths[sim.rows[r].samples]++;
    }
    t.rows.push_back(uint32_t(sim.rows.size()) * kRowTicks);

    const auto fewer = [](const auto& a, const auto& b)
    {
        return a.second < b.second;
    };
    const uint32_t usual = std::max_element(lengths.begin(), lengths.end(), fewer)->first;

    // The beat, from the commonest length of row, and the bars.
    const std::vector<OrderRun> runs = OrderRuns(rows);
    t.meter = ChooseMeter(t.rows, runs, kRowTicks, double(usual) / sim.rate / kRowTicks);
    t.bars = BarStarts(rows, t.rows, runs, BarTicks(t.meter));

    // A tempo that sets the length of each row.
    uint32_t tempo = 0;
    for (size_t r = 0; r < sim.rows.size(); r++)
    {
        const double seconds = double(sim.rows[r].samples) / sim.rate * t.meter.quarter_ticks / kRowTicks;
        const uint32_t micros = uint32_t(std::lround(seconds * 1e6));
        if (micros != tempo)
        {
            t.tempos.push_back({t.rows[r], micros});
            tempo = micros;
        }
    }

    // Each step's MIDI tick: its row's, plus its share of the row.
    for (const StepData& d : sim.steps)
    {
        const RowData& row = sim.rows[d.row];
        const uint64_t within = d.time - sim.steps[row.step].time;
        const uint64_t ticks = row.samples ? (within * kRowTicks + row.samples / 2) / row.samples : 0;
        t.steps.push_back(t.rows[d.row] + uint32_t(std::min<uint64_t>(ticks, kRowTicks)));
    }

    return t;
}

// Returns true if the notes of the song's second pass through its loop start as those of its first do: on the same
// channels and MIDI ticks from the pass's start, with the same sample, start point, key, bend and level, in rows of the
// same length. A channel keeps its volume, its sample and its slide when the module comes back to its loop. The
// first pass can therefore play unlike the later ones. `sim` has to contain two passes of the loop.
bool FirstPassRepeats(const Simulation& sim)
{
    const Timing timing = MakeTiming(sim);
    const Plan& plan = sim.plan;
    PassComparison passes(timing.rows[plan.loop_start], timing.rows[plan.loop_end] - timing.rows[plan.loop_start]);
    InstrumentSet instruments;
    for (uint32_t c = 0; c < sim.channels; c++)
    {
        for (const Note& n : MakeNotes(sim, c, instruments))
        {
            const Sound& s = n.sounds.front();
            const double row_samples = sim.rows[sim.steps[n.on].row].samples;
            passes.Add(timing.steps[n.on], int(c), n.key, {double(n.program), s.bend, s.level, row_samples});
        }
    }

    return passes.Alike();
}

// The MIDI file's bytes: a conductor track with the tempo map, then a track for each channel that plays notes.
std::vector<uint8_t> MidiBytes(const std::string& title, const std::string& about, const Simulation& sim,
                               const Timing& timing, const std::vector<std::vector<Note>>& notes)
{
    const Plan& plan = sim.plan;
    const uint32_t end = timing.rows[plan.end];
    MidiFile midi(uint16_t(timing.meter.quarter_ticks));
    MidiTrack& conductor = midi.AddTrack();
    conductor.Name(title);
    conductor.Meta(0, 0x01, about);
    WriteBarSignatures(conductor, timing.bars, timing.meter);
    for (const auto& [tick, micros] : timing.tempos)
    {
        conductor.Tempo(tick, micros);
    }
    if (plan.loops)
    {
        conductor.Meta(timing.rows[plan.loop_start], 0x06, "loopStart");
        conductor.Meta(timing.rows[plan.loop_end], 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the song.
    const uint32_t loop_start = plan.loops ? timing.rows[plan.loop_start] : 0;
    for (size_t c = 0; c < notes.size(); c++)
    {
        const std::vector<Note>& list = notes[c];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        mt.Name("Channel " + std::to_string(c + 1));
        mt.SetEnd(end);

        // The pitch bend range covers the channel's largest bend.
        double widest = 0;
        for (const Note& n : list)
        {
            for (const Sound& s : n.sounds)
            {
                widest = std::max(widest, std::fabs(s.bend));
            }
        }
        const int range = BendRangeFor(widest, kSmallBend);

        ChannelWriter writer(mt, MelodicChannel(int(c)), list[0].program, NumberedPreset(list[0].program), range,
                             loop_start);
        for (const Note& n : list)
        {
            const uint32_t on = timing.steps[n.on];
            const uint32_t off = timing.steps[n.off];
            if (on >= off)
            {
                continue;
            }

            // Each row's and tick's level and bend. Both of the mixer's sides play the channel alike.
            writer.SetProgram(on, n.program, NumberedPreset(n.program));
            for (const Sound& s : n.sounds)
            {
                const uint32_t at = timing.steps[s.step];
                writer.SetLevels(at, s.level, s.level);
                writer.SetBend(at, s.bend);
            }
            writer.Note(on, off, n.key);
        }
    }

    return midi.Encode();
}

// Plays a song as the conversion does: the model's rows and ticks, and the notes of each channel that the track mask
// picks, with their instruments in `instruments`. Returns false where the model played no row. The function writes
// the warnings and the count of tracks to the report. `P` is the player of the song's format.
template <typename P>
bool PlaySong(const Rom& rom, const Play& entry, const ConvertSettings& settings, InstrumentSet& instruments,
              Simulation& sim, std::vector<std::vector<Note>>& notes, SongReport& report)
{
    // The marked loop starts at the song's second pass if its first plays unlike it.
    sim = Simulate<P>(rom, entry, settings.loops, LoopStart::kFirstPass);
    if (sim.plan.loops && !FirstPassRepeats(Simulate<P>(rom, entry, 2, LoopStart::kFirstPass)))
    {
        sim = Simulate<P>(rom, entry, settings.loops, LoopStart::kSecondPass);
    }
    report.warnings = sim.warnings;
    if (sim.rows.empty())
    {
        return false;
    }

    notes.assign(sim.channels, {});
    for (uint32_t c = 0; c < sim.channels; c++)
    {
        if ((settings.track_mask >> c) & 1)
        {
            notes[c] = MakeNotes(sim, c, instruments);
            report.tracks += notes[c].empty() ? 0 : 1;
        }
    }

    return true;
}

// Converts a song, writing its files if `write` is set. With `files`, the files' bytes go there instead of to files,
// and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write, SongFiles* files = nullptr)
{
    SongReport report;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        report.warnings.push_back("there's no song " + std::to_string(song));
        return report;
    }

    const Song& entry = info.songs[size_t(song)];
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    Simulation sim;
    std::vector<std::vector<Note>> notes;
    const Play play = {entry, info.build, info.music_volume, info.fifos, info.sound_off_call != 0, info.mod30};
    bool played = false;
    switch (info.format)
    {
    case Format::kGbaMod1:
        played = PlaySong<Mod1Player>(rom, play, settings, instruments, sim, notes, report);
        break;

    case Format::kGbaMod30:
        played = PlaySong<Mod30Player>(rom, play, settings, instruments, sim, notes, report);
        break;

    case Format::kGbaMod2:
        played = PlaySong<Player>(rom, play, settings, instruments, sim, notes, report);
        break;
    }
    if (!played)
    {
        return report;
    }

    const Timing timing = MakeTiming(sim);
    const double rate = sim.rate;
    const auto seconds = [&](size_t row)
    {
        return double(sim.steps[sim.rows[row].step].time) / rate;
    };
    report.seconds = seconds(sim.plan.end);
    if (sim.plan.loops)
    {
        report.loop_start = seconds(sim.plan.loop_start);
        report.loop_end = seconds(sim.plan.loop_end);
    }
    report.bpm = 60e6 / timing.tempos.front().second;

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description.
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number;
    char about[192];
    std::snprintf(about, sizeof about, "%s (%s) module at 0x%08X from order %d, converted by %s", rom.Title().c_str(),
                  rom.GameCode().c_str(), unsigned(entry.module), entry.order, settings.program.c_str());
    const std::string stem = SongStem(settings, number);

    std::string error;
    std::vector<uint8_t> bytes = MidiBytes(title, about, sim, timing, notes);
    if (files)
    {
        files->midi = std::move(bytes);
    }
    else
    {
        report.midi_path = stem + ".mid";
        if (!WriteFile(report.midi_path, bytes, error))
        {
            report.warnings.push_back(error);
            return report;
        }
    }

    // The song's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        Sf2File file = own.Build(rom);
        file.program = settings.program;
        file.SetTitle(title);
        if (!files)
        {
            report.sf2_path = stem + ".sf2";
        }
        if (files ? !file.Encode(files->sf2, error) : !file.Write(report.sf2_path, error))
        {
            report.warnings.push_back(error);
            return report;
        }
    }

    report.ok = true;

    return report;
}

} // namespace

SongReport InspectSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    return Run(rom, info, song, settings, nullptr, false);
}

SongReport ConvertSong(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings,
                       InstrumentSet* shared)
{
    return Run(rom, info, song, settings, shared, true);
}

SongFiles ConvertSongFiles(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    SongFiles files;
    files.report = Run(rom, info, song, settings, nullptr, true, &files);
    return files;
}

} // namespace ntdlib::sequence_export::logikstate
