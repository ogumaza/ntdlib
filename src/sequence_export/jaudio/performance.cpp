// SPDX-License-Identifier: MIT

// Sequence execution (see performance.h). The Performer class (performer.h) plays a sequence. This file contains its
// constructor, the search for loops and ends, the start of a JAudio1 effect and the run of the sequence. Perform runs
// the performer again until the levels, the parts and the ties settle.

#include "ntdlib/sequence_export/jaudio/performance.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/sequence_export/passes.h"
#include "sequence_export/jaudio/performer.h"
#include "sequence_export/lengths.h"

namespace ntdlib::sequence_export::jaudio
{
namespace
{

// The DSP program's channels, each a voice (JASDSPChannel).
constexpr int kDspChannels = 64;

using ntdlib::sequence_export::FittedParts;
using ntdlib::sequence_export::LevelScale;
using ntdlib::sequence_export::SpareChannels;

// The rules of the main loop's passes (ntdlib/sequence_export/passes.h). A setting that RestoreAtLoopStart sends again
// goes after the presets at the loop's first tick.
const ntdlib::sequence_export::LoopRules kLoopRules{{}, true};

// How the recorder places events (seqlib/midi/recorder.h). A setting replaces an earlier value of its kind anywhere in
// its tick. A part switch sends the new part the old part's values. No tick's frame starts before tick 0.
constexpr seqlib::midi::RecorderOptions kRecorderOptions{
    .replace_anywhere_in_tick = true, .resend_on_switch = true, .frame_ticks_from_zero = true};

} // namespace

Performer::Performer(const SequenceSound& sound, BankSet& banks, const PerformOptions& options,
                     const std::string& program, double level_scale, const std::array<uint8_t, 16>& part_limits,
                     const std::vector<LoopChoice>& loop_choices, const std::map<uint32_t, uint8_t>& tie_velocities,
                     const std::set<int>& swept_filters)
    : sound_(sound),
      banks_(banks),
      options_(options),
      program_(program),
      rng_(sound.jaudio1 && !sound.timed_random, options.seed),
      recorder_(kRecorderOptions),
      tempo_rate_(sound.tempo_ratio),
      main_loop_(loop_choices),
      tie_velocities_in_(tie_velocities),
      swept_filters_in_(swept_filters)
{
    recorder_.SetLevelScale(level_scale);
    recorder_.SetPartLimits(part_limits);
    for (uint32_t i = 0; i < sound.draws; i++)
    {
        rng_.Next();
    }
    fade_ = sound.fade_in > 0 ? 0.0f : 1.0f;
    if (sound.conductor)
    {
        for (const ConductorTrack& track : sound.conductor->tracks)
        {
            conductor_tracks_.push_back(
                {.module = 0xff, .modules = std::vector<ConductorModuleState>(track.modules.size())});
        }
    }
}

void Performer::OnJumpBack(int t, uint32_t target)
{
    // An effect's passes count only while its slot plays it. The other slots and the slot's own polling don't count.
    if (sound_.effect && (!InEffect(t) || !EffectTaken() || EffectFree()))
    {
        return;
    }

    // A backward jump to commands the track hasn't run is not a loop; shared code may precede its callers. Also ignore
    // passes without waits, such as polling.
    Track& tr = TrackAt(t);
    const auto run = tr.visited.find(target);
    if (run == tr.visited.end() || !tr.last_wait || *tr.last_wait < run->second.first)
    {
        return;
    }

    const uint32_t start = run->second.first;
    const uint64_t first_command = run->second.second;

    // A pass that changed nothing is idle: the track only waited. Writing another track's port counts as a change.
    auto changed = [&](std::size_t i)
    {
        const Track& other = tracks_[i];
        return (other.last_change && *other.last_change >= start) ||
               (other.last_signal && *other.last_signal > first_command);
    };

    tr.idle = !changed(static_cast<std::size_t>(t));

    // Use the first track that can still advance, in the order the tracks opened. A pass counts only if no earlier
    // track contributed during it. A track whose last pass was idle doesn't count as advancing. An idle pass counts
    // only if no later track advances or contributed either. A later track that has been quiet for kHoldSeconds doesn't
    // count as advancing.
    auto plays_on = [&](std::size_t i)
    {
        return tracks_[i].open && !WaitsForever(static_cast<int>(i)) && !tracks_[i].idle &&
               (static_cast<int>(i) < t || !Quiet(static_cast<int>(i)));
    };

    for (std::size_t i = 0; i < tracks_.size(); i++)
    {
        if (static_cast<int>(i) != t && InEffect(static_cast<int>(i)) && (static_cast<int>(i) < t || tr.idle) &&
            (plays_on(i) || changed(i)))
        {
            return;
        }
    }

    // An idle pass in which nothing sounded means the sequence has ended: its tracks only wait now.
    if (tr.idle && !AnySounding() && start < tick_subframes_.size() && last_sounding_ < tick_subframes_[start])
    {
        silent_end_ = true;
        return;
    }

    // A pass that changed something, of another loop than the marked one, moves the markers to it.
    const std::pair<int, uint32_t> owner{t, target};
    if (!main_loop_.Owner() || (!tr.idle && *main_loop_.Owner() != owner))
    {
        main_loop_.Restart(out_, owner);
    }

    main_loop_.CountPass(out_, owner, start, tick_);
    if (out_.loops >= options_.loops && TickSeconds(tick_) >= kMinLoopSeconds && ConductorDone())
    {
        pending_loop_ = PendingLoop{t, start, tick_};
    }
}

bool Performer::TrackSounds(int t) const
{
    for (const int c : voices_)
    {
        if (c >= 0 && channels_[static_cast<std::size_t>(c)].track == t)
        {
            return true;
        }
    }

    return false;
}

bool Performer::AnySounding() const
{
    return std::ranges::any_of(voices_, [](int v) { return v >= 0; });
}

bool Performer::Steady() const
{
    if (sound_.effect && !EffectTaken())
    {
        return false;
    }

    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open || !InEffect(static_cast<int>(t)))
        {
            continue;
        }
        if (!WaitsForever(static_cast<int>(t)))
        {
            return false;
        }

        for (const MoveParam& m : tr.move)
        {
            if (m.count != 0)
            {
                return false;
            }
        }
    }

    return true;
}

bool Performer::WaitsForever(int t) const
{
    // A track waiting for a note that never ends, or waiting kHeldTicks or more: minutes at a typical tempo. Games wait
    // that long for sounds they stop themselves.
    const Track& tr = TrackAt(t);
    if (!tr.open)
    {
        return false;
    }
    if (tr.wait_note_finish)
    {
        if (tr.slots[0] < 0)
        {
            return false;
        }

        const Channel& ch = channels_[static_cast<std::size_t>(tr.slots[0])];
        return ch.wave_loops && !ch.released && !ch.ended;
    }

    return tr.wait >= kHeldTicks;
}

bool Performer::Quiet(int t) const
{
    const Track& tr = TrackAt(t);
    if (TrackSounds(t))
    {
        return false;
    }

    const uint32_t since = std::max(tr.last_change.value_or(0), tr.opened);
    return TickSeconds(tick_) - TickSeconds(since) >= kHoldSeconds;
}

bool Performer::Stuck() const
{
    if (AnySounding() || (sound_.effect && !EffectTaken()))
    {
        return false;
    }

    for (std::size_t t = 0; t < tracks_.size(); t++)
    {
        if (tracks_[t].open && InEffect(static_cast<int>(t)) && !WaitsForever(static_cast<int>(t)))
        {
            return false;
        }
    }

    return true;
}

int Performer::EffectSlotTrack() const
{
    const EffectStart& e = *sound_.effect;
    if (e.request == EffectRequest::kPort)
    {
        const int track = tracks_.empty() ? -1 : tracks_[0].children[static_cast<std::size_t>(e.slot & 15)];
        return track >= 0 && TrackAt(track).open ? track : -1;
    }

    const int owner = tracks_.empty() ? -1 : tracks_[0].children[static_cast<std::size_t>(e.slot >> 4)];
    if (owner < 0 || !TrackAt(owner).open)
    {
        return -1;
    }

    const int slot = TrackAt(owner).children[static_cast<std::size_t>(e.slot & 15)];
    return slot >= 0 && TrackAt(slot).open ? slot : -1;
}

bool Performer::InEffect(int t) const
{
    if (!sound_.effect)
    {
        return true;
    }
    if (effect_track_ < 0)
    {
        return false;
    }
    if (t == effect_track_)
    {
        return sound_.effect->request == EffectRequest::kSlot && TrackAt(t).depth > 0;
    }

    for (int u = TrackAt(t).parent; u >= 0; u = TrackAt(u).parent)
    {
        if (u == effect_track_)
        {
            return true;
        }
    }

    return false;
}

bool Performer::EffectTaken() const
{
    if (effect_track_ < 0)
    {
        return false;
    }

    const Track& tr = TrackAt(effect_track_);
    if (sound_.effect->request == EffectRequest::kPort)
    {
        return ((tr.imported >> (sound_.effect->port & 15)) & 1) == 0;
    }

    return tr.ports[0] != 1;
}

bool Performer::EffectFree() const
{
    if (!EffectTaken())
    {
        return false;
    }

    if (sound_.effect->request == EffectRequest::kPort)
    {
        for (std::size_t t = 0; t < tracks_.size(); t++)
        {
            if (static_cast<int>(t) != effect_track_ && tracks_[t].open && InEffect(static_cast<int>(t)))
            {
                return false;
            }
        }

        return true;
    }

    return TrackAt(effect_track_).ports[2] == 0;
}

void Performer::StartEffect()
{
    // JAISeMgr::checkPlayingSe: the values that the game's code passed with the effect (setTrackPortData), then the
    // wait for the distance, the map's information, the effect's number and the request.
    effect_track_ = EffectSlotTrack();
    if (effect_track_ < 0)
    {
        return;
    }

    // Pikmin's library writes the number alone. Values for the track's other ports go first: the sound's, with the
    // options' in their place.
    const EffectStart& e = *sound_.effect;
    if (e.request == EffectRequest::kPort)
    {
        std::map<uint8_t, uint16_t> ports = sound_.ports;
        for (const auto& [port, value] : options_.ports)
        {
            ports[port] = value;
        }
        for (const auto& [port, value] : ports)
        {
            if ((port & 15u) != (e.port & 15u))
            {
                WritePortApp(effect_track_, port & 15u, value);
            }
        }
        WritePortApp(effect_track_, e.port & 15u, e.number);
        return;
    }

    uint16_t map = 0;
    for (const auto& [port, value] : options_.ports)
    {
        const uint32_t p = port & 15u;
        if (p == 6)
        {
            map = value;
        }
        else if (p != 0 && p != 3 && p != 4)
        {
            WritePortApp(effect_track_, p, value);
        }
    }

    WritePortApp(effect_track_, 3, 0);
    WritePortApp(effect_track_, 6, map);
    WritePortApp(effect_track_, 4, sound_.effect->number);
    WritePortApp(effect_track_, 0, 1);
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    const double now = TickSeconds(tick_);
    if (now - TickSeconds(start) <= kHoldSeconds)
    {
        return std::nullopt;
    }

    // The last change: the last MIDI event or tempo change.
    const uint32_t last = out_.LastEventTick();
    if (now - TickSeconds(last) <= kHoldSeconds)
    {
        return std::nullopt;
    }

    // Every sounding note must be endless, and still held by its track.
    bool sounding = false;
    for (const int c : voices_)
    {
        if (c < 0)
        {
            continue;
        }

        const Channel& ch = channels_[static_cast<std::size_t>(c)];
        if (!ch.wave_loops || ch.released || ch.track < 0)
        {
            return std::nullopt;
        }

        sounding = true;
    }
    if (!sounding)
    {
        return std::nullopt;
    }

    return TickSeconds(last) + kHoldSeconds;
}

void Performer::Changed(int t)
{
    TrackAt(t).last_change = tick_;
}

double Performer::TickSeconds(uint32_t tick) const
{
    if (tick < tick_seconds_.size())
    {
        return tick_seconds_[tick];
    }

    const double per_tick = kSubFrameSeconds / static_cast<double>(std::max(TicksPerSubFrame(), 1e-6f));
    return (tick_seconds_.empty() ? 0.0 : tick_seconds_.back()) +
           (static_cast<double>(tick) - static_cast<double>(tick_seconds_.size()) + 1.0) * per_tick;
}

Performer::PassStart Performer::PassStartAt(uint32_t tick) const
{
    return ntdlib::sequence_export::PassStartAt(out_, recorder_.PartSwitches(), tick);
}

Performer Performer::PlayOn(uint32_t until) const
{
    // The copy's vector holds only the tracks open so far. The channels that the probe starts point into it (see Run).
    Performer probe = *this;
    probe.tracks_.reserve(kMaxTracks);
    probe.stop_ = false;
    probe.pending_loop_.reset();
    probe.options_.loops = std::numeric_limits<int>::max();
    while (probe.tick_ < until && probe.Advance())
    {
        probe.pending_loop_.reset();
        if (TickSeconds(probe.tick_) >= kMaxSeconds)
        {
            break;
        }
    }

    return probe;
}

bool Performer::RepeatsAsTheGame(uint32_t start, uint32_t end) const
{
    return ntdlib::sequence_export::RepeatsAsTheGame(out_, recorder_.PartSwitches(), start, end, kLoopRules);
}

bool Performer::MarkSteadyPass()
{
    // A copy of this performer plays on. Its passes are compared.
    auto play_on = [this](uint32_t until)
    {
        return [probe = PlayOn(until)](uint32_t start, uint32_t end)
        {
            return probe.RepeatsAsTheGame(start, end);
        };
    };
    return main_loop_.MarkSteadyPass(out_, last_draw_, options_.loops, play_on);
}

void Performer::DecideLoop()
{
    const PendingLoop pending = *pending_loop_;
    pending_loop_.reset();
    if (MarkSteadyPass())
    {
        stop_ = true;
        stop_tick_ = pending.end;
        held_sound_end_ = HeldSoundEnd(pending.start);

        // A conductor draws its last modules' phrases at random without end (ConductorDone). No pass plays as the next
        // one does, so the file has no loop.
        if (sound_.conductor)
        {
            out_.DropLoop();
        }
    }
}

uint32_t Performer::SecondsToTick(double seconds) const
{
    const auto it = std::lower_bound(tick_seconds_.begin(), tick_seconds_.end(), seconds);
    if (it != tick_seconds_.end())
    {
        return static_cast<uint32_t>(it - tick_seconds_.begin());
    }

    const double per_tick = kSubFrameSeconds / static_cast<double>(std::max(TicksPerSubFrame(), 1e-6f));
    return static_cast<uint32_t>(tick_seconds_.size() - 1 + std::ceil((seconds - tick_seconds_.back()) / per_tick));
}

void Performer::EndPerformance(bool finished)
{
    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers.
    if (held_sound_end_)
    {
        out_.holds = true;
        out_.DropLoop();
    }

    // A performance that stops at the end of a pass leaves out what the next pass started in its last tick.
    const std::optional<uint32_t> end_tick =
        held_sound_end_ ? std::optional<uint32_t>(std::min(SecondsToTick(*held_sound_end_), tick_)) : stop_tick_;
    if (!finished && end_tick)
    {
        // A note that started before the end keeps its note-off. The cut drops a note-off that came later.
        seqlib::midi::ClampNoteOffs(out_, *end_tick);
        const std::set<uint32_t> late = seqlib::midi::CutAt(out_, *end_tick);
        for (Channel& ch : channels_)
        {
            if (late.contains(ch.midi_id))
            {
                ch.midi_on = false;
            }
        }
        tick_ = *end_tick;
    }

    // A finished sequence plays its tails out; otherwise every note stops at the end.
    if (finished)
    {
        PlayTails();
    }
    for (std::size_t c = 0; c < channels_.size(); c++)
    {
        MidiNoteOff(c);
    }

    if (finished)
    {
        SetFinishedLength();
    }
    else
    {
        out_.seconds = TickSeconds(tick_);
        out_.end_tick = tick_;
    }

    // The note-offs at the end come at the end.
    seqlib::midi::ClampNoteOffs(out_, out_.end_tick);
}

void Performer::PlayTails()
{
    const uint64_t limit = subframe_ + static_cast<uint64_t>(kMaxTailSeconds / kSubFrameSeconds);
    while (AnySounding() && subframe_ < limit)
    {
        subframe_++;
        UpdateChannels();
    }
    sound_end_ = std::max(sound_end_, static_cast<double>(subframe_) * kSubFrameSeconds);
    if (!AnySounding())
    {
        sound_end_ = std::min(sound_end_, static_cast<double>(last_sounding_ + 1) * kSubFrameSeconds);
    }

    // The game's echo sounds on after the last voice, until it falls silent.
    for (uint64_t s = subframe_ + 1; EchoSounds(); s++)
    {
        StepEcho(s);
    }
    if (echo_heard_)
    {
        sound_end_ = std::max(sound_end_, static_cast<double>(*echo_heard_ + 1) * kSubFrameSeconds);
    }
}

void Performer::SetFinishedLength()
{
    uint32_t last_event = 0;
    for (const std::vector<TrackEvent>& events : out_.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind != EventKind::kNoteOff)
            {
                last_event = std::max(last_event, e.tick);
            }
        }
    }

    // Changes that come more than kHoldSeconds after the last sound can't be heard (EndsAtLastSound). The performance
    // then ends with the sound, including the echo. A note below hearing that sounds on keeps its note-off, at the end.
    if (EndsAtLastSound(sound_end_, TickSeconds(last_event)))
    {
        const uint32_t end = SecondsToTick(sound_end_);
        seqlib::midi::ClampNoteOffs(out_, end);
        seqlib::midi::CutAt(out_, end);
        std::erase_if(out_.tempo, [end](const Performance::Tempo& t) { return t.tick > end; });
        last_event = std::min(last_event, end);
    }
    out_.seconds = std::max(sound_end_, TickSeconds(last_event));
    out_.end_tick = std::max(last_event, SecondsToTick(out_.seconds));
}

bool Performer::Advance()
{
    // The game's writes by section (SequenceSound::status_by_section). The root track is track 0.
    if (!sound_.status_by_section.empty())
    {
        const auto status = sound_.status_by_section.find(TrackAt(0).ports[10]);
        if (status != sound_.status_by_section.end())
        {
            WritePortApp(0, 9, status->second);
        }
    }

    MoveFade();
    UpdateChannels();
    if (accumulator_ < 1.0f)
    {
        SweepBetweenTicks();
    }

    // JASTrack::seqMain: the ticks that are due, then the settings for the next sub-frame.
    while (accumulator_ >= 1.0f)
    {
        accumulator_ -= 1.0f;
        if (ticked_)
        {
            tick_++;
        }
        RecordTickTime();
        if (!ticked_)
        {
            EmitTempo();
        }
        const int ret = TickProc(0);
        ticked_ = true;
        UpdateSeq(0, false, 1.0f);
        if (ret < 0)
        {
            CloseTrack(0);
            return false;
        }

        // The game starts an effect between two of the effects sequence's ticks, once its slot waits for it.
        if (sound_.effect && effect_track_ < 0)
        {
            StartEffect();
        }

        for (std::size_t t = 0; t < tracks_.size(); t++)
        {
            RecordOutputs(static_cast<int>(t));
        }
    }

    accumulator_ += TicksPerSubFrame();
    UpdateSeq(0, false, 1.0f);
    subframe_++;

    return true;
}

Performance Performer::Run()
{
    const int root = StartSequence();
    const bool finished = PlaySequence(root);

    const PassStart next_pass = out_.loop && stop_ && !held_sound_end_ ? PassStartAt(out_.loop->end) : PassStart{};
    EndPerformance(finished);
    if (finished)
    {
        out_.DropLoop();
    }
    if (out_.loop)
    {
        ntdlib::sequence_export::RestoreAtLoopStart(out_, recorder_.PartSwitches(), next_pass, kLoopRules);
    }

    return out_;
}

int Performer::StartSequence()
{
    // Channels point at their tracks' oscillator tables. The tracks therefore never move: the vector holds room for
    // every track that a sequence may open.
    tracks_.reserve(kMaxTracks);

    // The voices: as many as --voices gives, or the DSP's 64 to start with when every note is kept.
    voices_.assign(static_cast<std::size_t>(options_.voice_limit > 0 ? options_.voice_limit : kDspChannels), -1);
    out_.default_release = sound_.default_release;
    if (sound_.echo)
    {
        for (std::vector<float>& line : echo_lines_)
        {
            line.assign(std::max<std::size_t>(sound_.echo->delay, 1), 0.0f);
        }
    }

    // JAISeq starts the sequence on a first track, with the ports the game has set. An effect's ports go to its slot
    // (StartEffect).
    slot_tracks_.fill(-1);
    const int root = NewTrack(-1, 0);
    if (sound_.jaudio1)
    {
        InitTrack1(root);
    }
    Track& tr = TrackAt(root);
    tr.pc = sound_.start;
    tr.open = true;
    if (sound_.jaudio1 && sound_.bank != 0)
    {
        tr.bank = sound_.bank;
        tr.regs[6] = static_cast<uint16_t>(sound_.bank << 8 | (tr.regs[6] & 0xff));
    }
    if (!sound_.effect)
    {
        // The game's values first, then the options'. An option takes the place of the game's value.
        std::map<uint8_t, uint16_t> ports;
        for (const auto& [port, value] : sound_.ports)
        {
            ports[port & 15] = value;
        }
        for (const auto& [port, value] : options_.ports)
        {
            ports[port & 15] = value;
        }
        for (const auto& [port, value] : ports)
        {
            tr.ports[port] = value;
            tr.imported = static_cast<uint16_t>(tr.imported | (1u << port));
            first_ports_ = static_cast<uint16_t>(first_ports_ | (1u << port));
        }
    }
    root_track_ = root;
    return root;
}

bool Performer::PlaySequence(int root)
{
    bool finished = false;
    double steady_since = -1.0;
    bool game_ports_written = sound_.game_ports.empty();
    for (;;)
    {
        if (!Advance())
        {
            finished = true;
            break;
        }

        // The game's writes to the root track's ports come after the first tick (SequenceSound::game_ports).
        if (!game_ports_written && ticked_)
        {
            for (const auto& [port, value] : sound_.game_ports)
            {
                WritePortApp(root, port & 15u, value);
            }
            game_ports_written = true;
        }

        if (pending_loop_)
        {
            DecideLoop();
            if (stop_)
            {
                break;
            }
        }

        // A sequence whose tracks only wait, with nothing sounding, has ended. So has an effect whose slot is free
        // again, or one whose slot's track didn't open in the first sub-frame with ticks. Its notes' tails play out.
        const bool no_slot = sound_.effect && ticked_ && effect_track_ < 0;
        if (silent_end_ || Stuck() || (sound_.effect && EffectFree()) || no_slot)
        {
            if (no_slot)
            {
                out_.warnings.push_back("the effects sequence opens no track for slot " +
                                        std::to_string(sound_.effect->slot) + ": the effect plays nothing");
            }
            finished = true;
            out_.DropLoop();
            break;
        }

        const double now = TickSeconds(tick_);
        if (steady_since < 0 && Steady())
        {
            steady_since = now;
        }
        if (steady_since >= 0 && !Steady())
        {
            steady_since = -1.0;
        }
        if (steady_since >= 0 && now - steady_since >= kHoldSeconds)
        {
            out_.holds = true;
            break;
        }

        // Treat a sequence that has been silent for longer than kHoldSeconds at the time limit as finished. It ends
        // with its last sound.
        if (static_cast<double>(subframe_) * kSubFrameSeconds >= kMaxSeconds)
        {
            finished =
                !AnySounding() && static_cast<double>(subframe_ - last_sounding_) * kSubFrameSeconds > kHoldSeconds;
            out_.truncated = !finished;
            break;
        }
        if (TicksPerSubFrame() <= 0.0f)
        {
            out_.warnings.push_back(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                                   : "tempo 0: the sequence stops advancing");
            break;
        }
    }

    return finished;
}

Performance Perform(const SequenceSound& sound, BankSet& banks, const PerformOptions& options,
                    const std::string& program)
{
    if (sound.start >= sound.data.size())
    {
        throw FormatError("sequence starts past its data");
    }

    // Run again when ties take notes above their velocity, to start those notes higher. Run again when CC11 would pass
    // 127, scaling every value equally, or when the tracks' parts don't fit MIDI's channels. Four runs at most.
    std::array<uint8_t, 16> part_limits;
    part_limits.fill(16);
    double peak = 0.0;
    double applied = 0.0;
    bool limited = false;
    std::vector<Performer::LoopChoice> loop_choices;
    std::map<uint32_t, uint8_t> tie_velocities;
    bool ties_known = false;
    std::set<int> swept_filters;
    Performance performance;
    for (int pass = 0; pass < 4; pass++)
    {
        Performer performer(sound, banks, options, program, LevelScale(peak), part_limits, loop_choices, tie_velocities,
                            swept_filters);
        performance = performer.Run();

        // A run that finds swept filters has given their entries presets. The next run leaves them out. This run
        // doesn't count.
        if (swept_filters.empty() && !performer.SweptFilters().empty())
        {
            swept_filters = performer.SweptFilters();
            pass--;
            continue;
        }
        applied = peak;
        loop_choices = performer.LoopChoices();
        if (!ties_known)
        {
            ties_known = true;
            if (!performer.TieVelocities().empty())
            {
                tie_velocities = performer.TieVelocities();
                continue;
            }
        }

        bool parts_fit = true;
        if (!limited)
        {
            if (const auto fitted = FittedParts(performance, performer.PartsUsed(), SpareChannels::kForLayers))
            {
                part_limits = *fitted;
                limited = true;
                parts_fit = false;
            }
        }
        if (parts_fit && LevelScale(performer.LevelPeak()) == LevelScale(peak))
        {
            break;
        }

        peak = performer.LevelPeak();
    }

    ntdlib::sequence_export::SetLevelCut(performance, applied);

    for (const PresetKey& key : performance.presets)
    {
        if (!performance.filter_layers.contains(key.track_filter))
        {
            performance.filter_layers[key.track_filter] =
                TrackFilterLayers(key.track_filter, sound.filter_rate, FilterFormat::kSoundFont);
            performance.sfz_filter_layers[key.track_filter] =
                TrackFilterLayers(key.track_filter, sound.filter_rate, FilterFormat::kSfz);
        }
    }

    return performance;
}

} // namespace ntdlib::sequence_export::jaudio
