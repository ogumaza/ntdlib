// SPDX-License-Identifier: MIT

#include "ntdlib/sequence_export/krawall/convert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "converted_notes.h"
#include "ntdlib/agb/files.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/agb/song_plan.h"
#include "ntdlib/krawall/driver.h"
#include "ntdlib/krawall/player.h"
#include "ntdlib/sequence_export/agb/midi.h"
#include "ntdlib/sequence_export/agb/module_bars.h"
#include "ntdlib/sequence_export/agb/settings.h"
#include "ntdlib/sequence_export/agb/sf2.h"
#include "ntdlib/sequence_export/krawall/soundfont.h"

namespace ntdlib::sequence_export::krawall
{

using ntdlib::agb::LoopStart;
using ntdlib::agb::PassComparison;
using ntdlib::agb::WriteFile;
using ntdlib::krawall::Channel;
using ntdlib::krawall::kPlayerChannels;
using ntdlib::krawall::MixChannel;
using ntdlib::krawall::ModuleSong;
using ntdlib::krawall::Player;
using ntdlib::sequence_export::agb::BarStarts;
using ntdlib::sequence_export::agb::BarTicks;
using ntdlib::sequence_export::agb::BendRangeFor;
using ntdlib::sequence_export::agb::ChannelWriter;
using ntdlib::sequence_export::agb::ChooseMeter;
using ntdlib::sequence_export::agb::kSmallBend;
using ntdlib::sequence_export::agb::LevelsToControllers;
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
namespace
{

// The most rows that a beat, or an eighth note in 12/8, can have.
constexpr uint32_t kMaxBeatRows = 64;

// The cents by which a note's pitch can miss its key before its bend makes up the difference: more than the table of
// periods misses equal temperament by in the octaves that modules use.
constexpr double kTuningCents = 3;

// The longest conversion, in seconds.
constexpr uint64_t kMaxSeconds = 60 * 60;

// A channel's mixer channel after a tick.
struct Voice
{
    bool playing = false;
    uint32_t handle = 0;
    uint32_t moves = 0;
    uint32_t sample = 0;
    int sample_index = -1;
    uint32_t offset = 0; // the point it plays, from the sample's start
    double rate = 0;     // Hz
    double left = 0;     // the level of each side, where 128 is full scale
    double right = 0;
    uint32_t entry = 0; // the address of the byte of flags of the channel's last entry in a row
};

// What the conversion keeps of a tick.
struct TickData
{
    uint64_t time = 0;     // the mixer's samples from the module's first tick
    uint32_t length = 0;   // the samples to the next tick
    bool row = false;      // a row starts on the tick
    int order = 0;         // the row's order
    int pattern_row = 0;   // the row's number in its pattern
    int speed = 0;         // the ticks of its row, at least 1
    uint32_t row_data = 0; // the row's first byte, and the address after the 0 that ends it
    uint32_t row_end = 0;
    std::array<Voice, kPlayerChannels> voices;
    std::array<std::pair<int8_t, int8_t>, kPlayerChannels> levels; // each channel's volume and pan
};

// The stretch of a module that the conversion covers, in ticks.
struct Plan
{
    bool loops = false;
    size_t loop_start = 0; // the first row whose place in the module comes back
    size_t loop_end = 0;   // where it comes back
    size_t end = 0;
};

// The model's output for the stretch of a module that the plan covers.
struct Simulation
{
    std::vector<TickData> ticks; // up to and including the plan's end
    Plan plan;
    int channels = 0;
    int mix_rate = 0;
    bool truncated = false; // cut off without a loop or a stop
    std::vector<std::string> warnings;
};

// Returns a channel's mixer channel after a tick. `scale` is the output's level for a mixer level of 1, where 128 is
// full scale.
Voice VoiceOf(const Player& player, const DriverInfo& info, const Channel& ch, double scale)
{
    Voice v;
    v.entry = ch.entry;
    v.playing = player.Sounding(ch);
    if (!v.playing)
    {
        return v;
    }

    const MixChannel& m = player.MixerOf(ch);
    v.handle = ch.handle;
    v.moves = m.moves;
    v.sample = m.sample;
    v.sample_index = m.sample_index;
    v.offset = m.pos - m.start;
    v.rate = std::fabs(double(m.inc)) * info.mix_rate / 65536.0;
    v.left = m.left * scale;
    v.right = (info.stereo ? m.right : m.left) * scale;

    return v;
}

// Runs the model until a row revisits an earlier module position with the same speed, tempo and pattern-loop state, and
// the module has played the loop between them `loops` times in all. From LoopStart::kSecondPass, the loop starts at the
// module's second pass through it. The module then plays the first pass before it. A song of a module that separates
// songs with +++ plays in song mode. Song mode loops back to the song's start at the song's end.
Simulation Simulate(const Rom& rom, const DriverInfo& info, const ModuleSong& song, int loops, LoopStart start_pass)
{
    Simulation sim;
    Player player(rom, info, song);
    sim.channels = player.Module().channels;
    sim.mix_rate = info.mix_rate;
    if (!player.Valid())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    const double scale = player.OutputScale();
    std::map<std::tuple<int, int, uint32_t, int, int>, size_t> rows;
    uint64_t first = 0;
    size_t end = SIZE_MAX;
    const auto record = [&](uint64_t time)
    {
        if (sim.ticks.size() > end)
        {
            return;
        }

        if (sim.ticks.empty())
        {
            first = time;
        }
        TickData d;
        d.time = time - first;
        d.length = player.TickSamples();
        d.row = player.TickInRow() == 0;
        d.order = player.RowOrder();
        d.pattern_row = player.RowNumber();
        d.speed = std::max(player.Speed(), 1);
        d.row_data = player.RowData();
        d.row_end = player.RowEnd();
        for (int c = 0; c < sim.channels; c++)
        {
            const Channel& channel = player.Channels()[size_t(c)];
            d.voices[size_t(c)] = VoiceOf(player, info, channel, scale);
            d.levels[size_t(c)] = {channel.volume, channel.panning};
        }

        // The first row that comes back to an earlier row's place starts the loop's second pass.
        if (d.row && end == SIZE_MAX)
        {
            const auto key = std::make_tuple(player.RowOrder(), player.RowNumber(), player.LoopState(), player.Speed(),
                                             player.Tempo());
            const auto [it, added] = rows.emplace(key, sim.ticks.size());
            if (!added)
            {
                const size_t length = sim.ticks.size() - it->second;
                const size_t skip = start_pass == LoopStart::kSecondPass ? length : 0;
                sim.plan.loops = true;
                sim.plan.loop_start = it->second + skip;
                sim.plan.loop_end = sim.ticks.size() + skip;
                end = sim.plan.loop_start + size_t(std::max(loops, 1)) * length;
            }
        }

        sim.ticks.push_back(d);
    };
    player.OnTick(record);

    const uint64_t most = kMaxSeconds * uint64_t(info.mix_rate);
    while (player.Playing() && sim.ticks.size() <= end && player.Elapsed() < most)
    {
        player.Frame();
    }

    if (sim.ticks.empty())
    {
        sim.warnings = player.Warnings();
        return sim;
    }

    if (end == SIZE_MAX)
    {
        // A module that stops ends where it stops, and one that neither stops nor loops is cut off.
        end = sim.ticks.size() - 1;
        if (player.Playing())
        {
            sim.warnings.push_back("found no loop within an hour, so the module is cut off there");
            sim.truncated = true;
        }
    }

    sim.ticks.resize(std::min(sim.ticks.size(), end + 1));
    sim.plan.end = sim.ticks.size() - 1;
    sim.warnings.insert(sim.warnings.end(), player.Warnings().begin(), player.Warnings().end());

    return sim;
}

// A tick of a note: how far its pitch is from the note's key, in semitones, and its levels.
struct Sound
{
    size_t tick = 0;
    double bend = 0;
    double left = 0;
    double right = 0;
};

// A note: a sample that a channel's mixer channel plays from the tick a note or an effect starts it until it stops,
// another starts, or the module ends.
struct Note
{
    size_t on = 0;
    size_t off = 0;
    int key = 0;
    int program = 0;
    uint32_t entry = 0; // the address of the byte of flags of the entry that started it
    std::vector<Sound> sounds;
};

// Returns the notes of channel `c`, adding their instruments to `instruments`.
std::vector<Note> MakeNotes(const Rom& rom, const DriverInfo& info, const Simulation& sim, int c,
                            InstrumentSet& instruments)
{
    std::vector<Note> notes;
    bool open = false;
    double start_rate = 0, tuning = 0;
    for (size_t i = 0; i <= sim.plan.end; i++)
    {
        const Voice& v = sim.ticks[i].voices[size_t(c)];
        const Voice* last = i > 0 ? &sim.ticks[i - 1].voices[size_t(c)] : nullptr;
        const bool started =
            v.playing && (!last || !last->playing || v.handle != last->handle || v.moves != last->moves);
        if (open && (!v.playing || started || i == sim.plan.end))
        {
            notes.back().off = i;
            open = false;
        }
        if (i == sim.plan.end)
        {
            break;
        }

        // A note's key is the one nearest its pitch, and a pitch that misses it by more than the table of periods does
        // starts with a bend. A sample that starts at no pitch starts its note when it has one.
        if (v.playing && !open && v.rate > 0)
        {
            Note n;
            n.on = i;
            n.entry = v.entry;
            n.program = instruments.Program(v.sample, v.sample_index, v.offset);
            const double semitones = 12 * std::log2(v.rate / SampleRate(rom, info, v.sample));
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

        // Each tick of the note: its pitch, as a bend from its key, and its levels.
        if (open)
        {
            Sound s;
            s.tick = i;
            s.bend = notes.back().sounds.empty() ? tuning : notes.back().sounds.back().bend;
            if (v.rate > 0)
            {
                s.bend = tuning + 12 * std::log2(v.rate / start_rate);
            }
            s.left = v.left;
            s.right = v.right;
            notes.back().sounds.push_back(s);
        }
    }

    return notes;
}

// Returns true if the notes of the module's second pass through its loop start as those of its first do: on the same
// channels and ticks from the pass's start, with the same sample, start point, key, bend and levels, and ticks of the
// same length. The module's channels keep their pan and volume when it comes back to its loop, so the first pass can
// play unlike the later ones. `sim` has to hold two passes of the loop.
bool FirstPassRepeats(const Rom& rom, const DriverInfo& info, const Simulation& sim)
{
    const Plan& plan = sim.plan;
    PassComparison passes(plan.loop_start, plan.loop_end - plan.loop_start);
    InstrumentSet instruments;
    for (int c = 0; c < sim.channels; c++)
    {
        for (const Note& n : MakeNotes(rom, info, sim, c, instruments))
        {
            const Sound& s = n.sounds.front();
            passes.Add(n.on, c, n.key, {double(n.program), s.bend, s.left, s.right, double(sim.ticks[n.on].length)});
        }
    }

    return passes.Alike();
}

// Returns the key with the largest count, the first of those that tie.
template <typename K>
K Commonest(const std::map<K, size_t>& counts)
{
    K best{};
    size_t most = 0;
    for (const auto& [key, count] : counts)
    {
        if (count > most)
        {
            best = key;
            most = count;
        }
    }

    return best;
}

// Returns whether one speed is a power-of-two multiple of the other. These speeds share a beat grid: rows divide or
// combine evenly.
bool SameGrid(uint32_t speed, uint32_t main)
{
    const uint32_t high = std::max(speed, main), low = std::min(speed, main);
    return low > 0 && high % low == 0 && ((high / low) & (high / low - 1)) == 0;
}

// MIDI timing based on the most common speed. At that speed, and at power-of-two multiples or fractions of it, each
// player tick occupies the same number of MIDI ticks. This lets a module halve its speed for finer rows without
// changing the MIDI tempo. At other speeds, each row gets the usual number of MIDI ticks, and the tempo changes to
// preserve its playback duration, subject to the resolution limit below.
struct Timing
{
    Meter meter;                                       // in MIDI ticks
    std::vector<uint32_t> ticks;                       // the MIDI tick of each of the module's ticks, and of the end
    std::vector<std::pair<uint32_t, uint32_t>> tempos; // MIDI tick and microseconds a quarter note
    std::vector<uint32_t> bars;                        // the MIDI ticks that start a bar
};

Timing MakeTiming(const Simulation& sim)
{
    // The rows at each speed, and the ticks of each length.
    Timing t;
    std::map<int, size_t> speeds;
    std::map<uint32_t, size_t> lengths;
    for (const TickData& d : sim.ticks)
    {
        lengths[d.length]++;
        if (d.row)
        {
            speeds[d.speed]++;
        }
    }
    const uint32_t main = uint32_t(std::max(Commonest(speeds), 1));

    // Increase the MIDI resolution so rows at other speeds can be divided evenly into player ticks. The ticks per
    // quarter note must fit in 15 bits. If a speed would require a higher resolution, leave its player ticks on the
    // normal grid.
    uint32_t scale = 1;
    std::set<uint32_t> stretched;
    for (const auto& entry : speeds)
    {
        const uint32_t s = uint32_t(entry.first);
        const uint32_t need = std::lcm(scale, s / std::gcd(s, main));
        if (!SameGrid(s, main) && 2 * kMaxBeatRows * main * need <= 32767)
        {
            scale = need;
            stretched.insert(s);
        }
    }

    // Each tick's MIDI tick.
    uint32_t at = 0;
    for (const TickData& d : sim.ticks)
    {
        t.ticks.push_back(at);
        const uint32_t s = uint32_t(d.speed);
        at += stretched.count(s) ? main * scale / s : scale;
    }
    t.ticks.push_back(at);

    // The beat, from the commonest length of tick, and the bars. A row lasts its speed's count of ticks.
    std::vector<ModuleStep> steps;
    for (size_t i = 0; i < sim.ticks.size(); i++)
    {
        const TickData& d = sim.ticks[i];
        steps.push_back({d.row, d.order, d.pattern_row, (t.ticks[i + 1] - t.ticks[i]) * uint32_t(d.speed)});
    }
    const std::vector<OrderRun> runs = OrderRuns(steps);
    t.meter = ChooseMeter(t.ticks, runs, main * scale, double(Commonest(lengths)) / sim.mix_rate / scale);
    t.bars = BarStarts(steps, t.ticks, runs, BarTicks(t.meter));

    // A tempo that gives each tick its length.
    uint32_t tempo = 0;
    for (size_t i = 0; i < sim.ticks.size(); i++)
    {
        const uint32_t s = uint32_t(sim.ticks[i].speed);
        const double stretch = stretched.count(s) ? double(s) / main : 1;
        const double seconds = double(sim.ticks[i].length) / sim.mix_rate * t.meter.quarter_ticks / scale * stretch;
        const uint32_t micros = uint32_t(std::lround(seconds * 1e6));
        if (micros != tempo)
        {
            t.tempos.push_back({t.ticks[i], micros});
            tempo = micros;
        }
    }

    return t;
}

// The MIDI channels other than the drum channel.
constexpr size_t kMelodicChannels = 15;

// The steps of CC10 or CC11, and the semitones of bend, by which two notes' settings can differ on one MIDI channel.
constexpr int kSettingTolerance = 3;
constexpr double kBendTolerance = 0.05;

// What a track sets for a note at a tick: its key, its pan and expression, and its bend. A key of -1 means no note.
struct TickState
{
    int key = -1;
    int pan = 64;
    int expression = 0;
    double bend = 0;
};

// Returns the settings that the channel's notes set at each tick up to the module's end.
std::vector<TickState> TickStates(const Simulation& sim, const std::vector<Note>& notes)
{
    std::vector<TickState> states(sim.plan.end);
    for (const Note& n : notes)
    {
        for (const Sound& s : n.sounds)
        {
            TickState& state = states[s.tick];
            state.key = n.key;
            LevelsToControllers(s.left, s.right, state.pan, state.expression);
            state.bend = s.bend;
        }
    }

    return states;
}

// Returns whether two notes at a tick need settings that one MIDI channel can't give both: another pan, expression or
// bend, or the same key, whose note off would end both.
bool Clashes(const TickState& a, const TickState& b)
{
    return a.key == b.key || std::abs(a.pan - b.pan) > kSettingTolerance ||
           std::abs(a.expression - b.expression) > kSettingTolerance || std::fabs(a.bend - b.bend) > kBendTolerance;
}

// Returns the mixer's samples in which two channels' notes sound at once with settings that clash.
uint64_t Clash(const Simulation& sim, const std::vector<TickState>& a, const std::vector<TickState>& b)
{
    uint64_t total = 0;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (a[i].key >= 0 && b[i].key >= 0 && Clashes(a[i], b[i]))
        {
            total += sim.ticks[i + 1].time - sim.ticks[i].time;
        }
    }

    return total;
}

// Returns the notes without the silence at their ends: the ticks from which the mixer holds both of a note's levels at
// 0. The game's output has nothing of the note then. On a shared MIDI channel, another track's expression would make
// the note sound again. A note whose levels stay 0 throughout goes.
std::vector<Note> WithoutFinalSilence(const std::vector<Note>& notes)
{
    std::vector<Note> kept;
    for (Note n : notes)
    {
        size_t end = n.sounds.size();
        while (end > 0 && n.sounds[end - 1].left == 0 && n.sounds[end - 1].right == 0)
        {
            end--;
        }
        if (end == 0)
        {
            continue;
        }
        if (end < n.sounds.size())
        {
            n.off = n.sounds[end].tick;
            n.sounds.resize(end);
        }
        kept.push_back(std::move(n));
    }

    return kept;
}

// Returns each channel's MIDI channel, from the channels other than the drum channel. With up to 15 channels that play
// notes, each takes one. With more, channels share them. A MIDI channel's controllers and bend act on all its notes.
// Each channel starts in a group of its own. The two groups that clash least then merge until 15 are left. A tie goes
// to the lower channels. The groups take MIDI channels in the order of their lowest channels. `notes` holds the notes
// without their final silence.
std::vector<int> SharedMidiChannels(const Simulation& sim, const std::vector<std::vector<Note>>& notes)
{
    std::vector<size_t> playing;
    std::vector<std::vector<TickState>> states;
    for (size_t c = 0; c < notes.size(); c++)
    {
        if (!notes[c].empty())
        {
            playing.push_back(c);
            states.push_back(TickStates(sim, notes[c]));
        }
    }

    const size_t count = playing.size();
    std::vector<std::vector<uint64_t>> clash(count, std::vector<uint64_t>(count));
    for (size_t a = 0; a < count; a++)
    {
        for (size_t b = a + 1; b < count; b++)
        {
            clash[a][b] = clash[b][a] = Clash(sim, states[a], states[b]);
        }
    }

    std::vector<std::vector<size_t>> groups(count);
    for (size_t a = 0; a < count; a++)
    {
        groups[a] = {a};
    }
    while (groups.size() > kMelodicChannels)
    {
        size_t best_a = 0;
        size_t best_b = 1;
        uint64_t least = UINT64_MAX;
        for (size_t a = 0; a < groups.size(); a++)
        {
            for (size_t b = a + 1; b < groups.size(); b++)
            {
                uint64_t added = 0;
                for (size_t x : groups[a])
                {
                    for (size_t y : groups[b])
                    {
                        added += clash[x][y];
                    }
                }
                if (added < least)
                {
                    best_a = a;
                    best_b = b;
                    least = added;
                }
            }
        }
        groups[best_a].insert(groups[best_a].end(), groups[best_b].begin(), groups[best_b].end());
        groups.erase(groups.begin() + std::ptrdiff_t(best_b));
    }

    std::vector<int> midi(notes.size(), MelodicChannel(0));
    for (size_t g = 0; g < groups.size(); g++)
    {
        for (size_t x : groups[g])
        {
            midi[playing[x]] = MelodicChannel(int(g));
        }
    }

    return midi;
}

// Returns each channel's MIDI channel in a module where every channel that plays notes gets one. In a module with up to
// 15 channels, channel c takes the c-th. In a module with more, the 15 channels whose notes last longest take one each,
// in the channels' order.
std::vector<int> SeparateMidiChannels(const Simulation& sim, const std::vector<std::vector<Note>>& notes)
{
    std::vector<int> midi(notes.size());
    if (notes.size() <= kMelodicChannels)
    {
        for (size_t c = 0; c < notes.size(); c++)
        {
            midi[c] = MelodicChannel(int(c));
        }
        return midi;
    }

    std::vector<uint64_t> length(notes.size());
    for (size_t c = 0; c < notes.size(); c++)
    {
        for (const Note& n : notes[c])
        {
            length[c] += sim.ticks[n.off].time - sim.ticks[n.on].time;
        }
    }

    std::vector<size_t> order(notes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return length[a] > length[b]; });

    std::vector<size_t> first(order.begin(), order.begin() + kMelodicChannels);
    std::sort(first.begin(), first.end());
    for (size_t k = 0; k < first.size(); k++)
    {
        midi[first[k]] = MelodicChannel(int(k));
    }

    return midi;
}

// The MIDI file's bytes: a conductor track with the tempo map, then a track for each channel that plays notes, on the
// MIDI channel that `channels` gives it.
std::vector<uint8_t> MidiBytes(const std::string& title, const std::string& about, const Simulation& sim,
                               const Timing& timing, const std::vector<std::vector<Note>>& notes,
                               const std::vector<int>& channels)
{
    const Plan& plan = sim.plan;
    const uint32_t end = timing.ticks[plan.end];
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
        conductor.Meta(timing.ticks[plan.loop_start], 0x06, "loopStart");
        conductor.Meta(timing.ticks[plan.loop_end], 0x06, "loopEnd");
    }
    conductor.SetEnd(end);

    // The loop's start, from which each channel writes its settings again, unless the loop starts with the module.
    const uint32_t loop_start = plan.loops ? timing.ticks[plan.loop_start] : 0;

    for (size_t c = 0; c < notes.size(); c++)
    {
        const std::vector<Note>& list = notes[c];
        if (list.empty())
        {
            continue;
        }

        MidiTrack& mt = midi.AddTrack();
        const int ch = channels[c];
        mt.Name("Channel " + std::to_string(c + 1));
        mt.SetEnd(end);

        // The pitch bend range covers the largest bend of the channels on the MIDI channel. The ticks at which another
        // channel's note ends are those from which this channel's note sets its settings again.
        double widest = 0;
        bool shared = false;
        std::vector<char> other_ends(plan.end + 1, 0);
        for (size_t other = 0; other < notes.size(); other++)
        {
            if (channels[other] != ch || notes[other].empty())
            {
                continue;
            }

            shared = shared || other != c;
            for (const Note& n : notes[other])
            {
                other_ends[n.off] = other_ends[n.off] || other != c;
                for (const Sound& s : n.sounds)
                {
                    widest = std::max(widest, std::fabs(s.bend));
                }
            }
        }
        const int range = BendRangeFor(widest, kSmallBend);

        ChannelWriter writer(mt, ch, list[0].program, NumberedPreset(list[0].program), range, loop_start, shared);
        for (const Note& n : list)
        {
            const uint32_t on = timing.ticks[n.on];
            const uint32_t off = timing.ticks[n.off];
            if (on >= off)
            {
                continue;
            }

            writer.SetProgram(on, n.program, NumberedPreset(n.program));

            // Each tick's levels and bend.
            for (const Sound& s : n.sounds)
            {
                const uint32_t at = timing.ticks[s.tick];
                if (other_ends[s.tick])
                {
                    writer.SetSettingsAgain();
                }
                writer.SetLevels(at, s.left, s.right);
                writer.SetBend(at, s.bend);
            }

            writer.Note(on, off, n.key);
        }
    }

    return midi.Encode();
}

// Plays a module as the conversion does: the model's ticks, the notes of each channel that the track mask picks, with
// their instruments in `instruments`, and each channel's MIDI channel. Returns false where the model played no tick.
// The report takes the warnings and the count of tracks.
bool PlayModule(const Rom& rom, const DriverInfo& info, const ModuleSong& entry, const ConvertSettings& settings,
                InstrumentSet& instruments, Simulation& sim, std::vector<std::vector<Note>>& notes,
                std::vector<int>& midi, SongReport& report)
{
    // The marked loop starts at the module's second pass if its first plays unlike it, as when the channels come back
    // to the loop with the pan or volume that its end left them.
    sim = Simulate(rom, info, entry, settings.loops, LoopStart::kFirstPass);
    if (sim.plan.loops && !FirstPassRepeats(rom, info, Simulate(rom, info, entry, 2, LoopStart::kFirstPass)))
    {
        sim = Simulate(rom, info, entry, settings.loops, LoopStart::kSecondPass);
    }
    report.warnings = sim.warnings;
    if (sim.ticks.empty())
    {
        return false;
    }

    // The notes of each channel chosen.
    notes.assign(size_t(sim.channels), {});
    for (int c = 0; c < sim.channels; c++)
    {
        if ((settings.track_mask >> c) & 1)
        {
            notes[size_t(c)] = MakeNotes(rom, info, sim, c, instruments);
            report.tracks += notes[size_t(c)].empty() ? 0 : 1;
        }
    }

    // With more than 15 channels that play notes, channels share MIDI channels.
    const auto playing =
        size_t(std::count_if(notes.begin(), notes.end(), [](const auto& list) { return !list.empty(); }));
    if (playing <= kMelodicChannels)
    {
        midi = SeparateMidiChannels(sim, notes);
    }
    else
    {
        report.tracks = 0;
        for (std::vector<Note>& list : notes)
        {
            list = WithoutFinalSilence(list);
            report.tracks += list.empty() ? 0 : 1;
        }
        midi = SharedMidiChannels(sim, notes);
        std::vector<std::vector<TickState>> states;
        for (const std::vector<Note>& list : notes)
        {
            states.push_back(TickStates(sim, list));
        }
        for (size_t a = 0; a < notes.size(); a++)
        {
            for (size_t b = a + 1; b < notes.size(); b++)
            {
                const uint64_t clash = midi[a] == midi[b] ? Clash(sim, states[a], states[b]) : 0;
                if (clash > 0)
                {
                    char text[128];
                    std::snprintf(text, sizeof text,
                                  "channels %zu and %zu share a MIDI channel, and their notes clash for %.1f s", a + 1,
                                  b + 1, double(clash) / sim.mix_rate);
                    report.warnings.push_back(text);
                }
            }
        }
    }

    return true;
}

// Converts a module, writing its files if `write` is set. With `files`, the files' bytes go there instead of to files,
// and the report names no paths.
SongReport Run(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings, InstrumentSet* shared,
               bool write, SongFiles* files = nullptr)
{
    SongReport report;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        report.warnings.push_back("there's no module " + std::to_string(song));
        return report;
    }

    const ModuleSong& entry = info.songs[size_t(song)];
    const uint32_t module = entry.module;
    InstrumentSet own;
    InstrumentSet& instruments = shared ? *shared : own;
    Simulation sim;
    std::vector<std::vector<Note>> notes;
    std::vector<int> midi;
    if (!PlayModule(rom, info, entry, settings, instruments, sim, notes, midi, report))
    {
        return report;
    }

    const Timing timing = MakeTiming(sim);
    const double rate = sim.mix_rate;
    report.seconds = double(sim.ticks[sim.plan.end].time) / rate;
    if (sim.plan.loops)
    {
        report.loop_start = double(sim.ticks[sim.plan.loop_start].time) / rate;
        report.loop_end = double(sim.ticks[sim.plan.loop_end].time) / rate;
    }
    report.bpm = 60e6 / timing.tempos.front().second;

    report.silent = report.tracks == 0;
    if (report.silent || !write)
    {
        report.ok = true;
        return report;
    }

    // The files' names and titles, and the MIDI file's description. The description names the module by its place in
    // the list.
    const std::string number = SongNumber(song, int(info.songs.size()));
    const std::string title = rom.Title() + " #" + number;
    const int place = int(std::find(info.modules.begin(), info.modules.end(), module) - info.modules.begin());
    char about[192];
    if (entry.song < 0)
    {
        std::snprintf(about, sizeof about, "%s (%s) module %d at 0x%08X, converted by %s", rom.Title().c_str(),
                      rom.GameCode().c_str(), place, unsigned(module), settings.program.c_str());
    }
    else
    {
        std::snprintf(about, sizeof about, "%s (%s) song %d of module %d at 0x%08X, converted by %s",
                      rom.Title().c_str(), rom.GameCode().c_str(), entry.song, place, unsigned(module),
                      settings.program.c_str());
    }
    const std::string stem = SongStem(settings, number);

    std::string error;
    std::vector<uint8_t> bytes = MidiBytes(title, about, sim, timing, notes, midi);
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

    // The module's SoundFont, unless the instruments go in a shared one.
    if (!shared)
    {
        Sf2File file = own.Build(rom, info);
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

ConvertedModule ConvertedNotes(const Rom& rom, const DriverInfo& info, int song, const ConvertSettings& settings)
{
    ConvertedModule out;
    if (song < 0 || size_t(song) >= info.songs.size())
    {
        out.warnings.push_back("there's no module " + std::to_string(song));
        return out;
    }

    SongReport report;
    InstrumentSet instruments;
    Simulation sim;
    std::vector<std::vector<Note>> notes;
    std::vector<int> midi;
    const bool played =
        PlayModule(rom, info, info.songs[size_t(song)], settings, instruments, sim, notes, midi, report);
    out.warnings = report.warnings;
    if (!played)
    {
        return out;
    }

    const Timing timing = MakeTiming(sim);
    const Plan& plan = sim.plan;
    out.valid = true;
    out.truncated = sim.truncated;
    out.ticks_per_quarter = uint32_t(timing.meter.quarter_ticks);
    out.end = timing.ticks[plan.end];
    out.seconds = double(sim.ticks[plan.end].time) / sim.mix_rate;
    if (plan.loops)
    {
        out.loops = true;
        out.loop_start = timing.ticks[plan.loop_start];
        out.loop_end = timing.ticks[plan.loop_end];
    }
    for (const auto& [tick, micros] : timing.tempos)
    {
        out.tempos.emplace_back(tick, micros);
    }

    // The rows and the channels' levels up to the end.
    for (size_t i = 0; i < plan.end; i++)
    {
        const TickData& d = sim.ticks[i];
        if (d.row)
        {
            out.rows.push_back({timing.ticks[i], d.row_data, d.row_end, d.order, d.pattern_row});
        }
        for (int c = 0; c < sim.channels; c++)
        {
            const auto [volume, pan] = d.levels[size_t(c)];
            if (i == 0 || d.levels[size_t(c)] != sim.ticks[i - 1].levels[size_t(c)])
            {
                out.levels.push_back({c, timing.ticks[i], volume, pan});
            }
        }
    }

    // The notes that the MIDI file writes.
    for (size_t c = 0; c < notes.size(); c++)
    {
        for (const Note& n : notes[c])
        {
            const uint32_t on = timing.ticks[n.on];
            const uint32_t off = timing.ticks[n.off];
            if (on < off)
            {
                out.notes.push_back({int(c), n.entry, on, off, n.key, n.program});
            }
        }
    }

    return out;
}

} // namespace ntdlib::sequence_export::krawall
