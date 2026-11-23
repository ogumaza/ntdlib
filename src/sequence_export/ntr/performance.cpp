// SPDX-License-Identifier: MIT

// The DS's performer (see performance.h). An ArchivePlayer plays the sequence on the driver's player 0. The performer
// takes the driver's notes through the player's hooks, and reads the tracks' settings and the channels' states from
// the driver. It records each tick's settings when the next tick starts, and when the frame ends. A note's channel
// ends its MIDI note when the channel releases or stops.
//
// The driver runs a player's ticks at the start of its frames. A player's tempo adds to a counter each frame, and a
// tick runs each time the counter passes 240. MIDI's tempo puts each tick at its nominal time, up to a frame before the
// frame that runs it. The events of a tick go at the start of that frame (Performance::frame_ticks). The changes that
// the end of a frame makes, such as a note's release or a sweep's pitch, go at the start of their frame too
// (TrackEvent::after).
//
// A part that plays one note at a time follows the note's channel more closely. Where a part holds one note and the
// note bends, the part's pitch is the one at which the channel's timer plays it (TimerBend). Where a part sounds one
// note and the channel's LFO moves its pan, CC10 and CC11 follow the pan frame by frame, and CC1 is 0. SoundFont has
// no LFO that moves the pan, and the SFZ files' LFO then stays still. A note in its release still sounds: its channel
// keeps an LFO of its own, and CC10 can follow only one channel. Notes that overlap on a part keep the track's pitch
// and pan.

#include "ntdlib/sequence_export/ntr/performance.h"

#include "performance_internal.h"

namespace ntdlib::sequence_export::ntr
{

namespace
{

Performer::Performer(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                     const PerformOptions& options, double level_scale, const std::array<uint8_t, 16>& part_limits,
                     const LoopReplay& replay)
    : sequence_(sequence),
      options_(options),
      player_(archive, {.build = options.build, .random_state = options.seed}),
      s_(replay, level_scale, part_limits)
{
    for (std::array<int16_t, 2>& mixed : s_.mixed)
    {
        mixed = {kStartVolume, 0};
    }
    s_.out.timebase = kTimebase;
    s_.out.bank = BankOf(archive, sequence);

    player_.on_tick = [this](int player)
    {
        if (player == 0)
        {
            OnTick();
        }
    };
    player_.on_note = [this](const snd::NoteEvent& e)
    {
        if (e.player == 0)
        {
            OnNote(e);
        }
    };
    snd::LengthHooks hooks;
    hooks.on_loop = [this](int player, int track, uint32_t target)
    {
        if (player == 0)
        {
            OnLoop(track, target);
        }
    };
    hooks.on_track_loop = [this](int player, int track, uint32_t target)
    {
        if (player == 0)
        {
            OnTrackLoop(track, target);
        }
    };
    hooks.on_command_start = [this](int player, int track)
    {
        if (player == 0)
        {
            OnCommandStart(track);
        }
    };
    hooks.on_draw = [this](int player, int)
    {
        if (player == 0)
        {
            s_.last_draw = s_.tick;
        }
    };
    player_.FollowLengths(std::move(hooks));
}

const snd::Track* Performer::TrackOf(int number) const
{
    const uint8_t index = Driver().GetPlayer(0).tracks[static_cast<std::size_t>(number)];
    return index == kNoTrack ? nullptr : &Driver().GetTrack(index);
}

// --- The driver's hooks ---

void Performer::OnTick()
{
    if (!s_.ticked)
    {
        s_.StartTicks(s_.out.frame_ticks);
        SyncTempo();
        return;
    }

    // The settings of the last tick, then the next tick at MIDI's tempo.
    SyncTracks();
    s_.NextTick(s_.out.frame_ticks);
}

void Performer::OnCommandStart(int number)
{
    if (const snd::Track* track = TrackOf(number))
    {
        s_.runs[static_cast<std::size_t>(number)][track->position - track->base] = s_.tick;
        RecordCommand(number, *track);
    }
}

void Performer::OnLoop(int number, uint32_t target)
{
    // The pass started when the track last ran `target`. A pass within one tick waited for nothing.
    const std::map<uint32_t, uint32_t>& runs = s_.runs[static_cast<std::size_t>(number)];
    const auto run = runs.find(target);
    if (run == runs.end() || run->second >= s_.tick)
    {
        return;
    }

    // The driver gives the passes that changed the sound, of the track that owns the sequence's loop. A pass of
    // another loop than the marked one moves the markers to it.
    const uint32_t start = run->second;
    if (start < s_.changed_at)
    {
        return;
    }

    const std::pair<int, uint32_t> owner{number, target};
    if (!s_.main_loop.Owner() || *s_.main_loop.Owner() != owner)
    {
        s_.main_loop.Restart(s_.out, owner);
    }

    s_.CountPass(s_.out, owner, number, start, options_.loops);
}

void Performer::OnTrackLoop(int number, uint32_t target)
{
    // The loop started when the track last ran `target`. OnLoop times a pass of the main loop the same way.
    const std::map<uint32_t, uint32_t>& runs = s_.runs[static_cast<std::size_t>(number)];
    if (const auto run = runs.find(target); run != runs.end() && run->second < s_.tick)
    {
        s_.loop_lengths[static_cast<std::size_t>(number)] = s_.tick - run->second;
    }
}

std::size_t Performer::WholeSpan() const
{
    std::vector<uint32_t> lengths;
    for (int number = 0; number < snd::kTracksPerPlayer; number++)
    {
        const snd::Track* track = TrackOf(number);
        if (track != nullptr && track->active)
        {
            lengths.push_back(s_.loop_lengths[static_cast<std::size_t>(number)]);
        }
    }

    return ntdlib::sequence_export::detail::LastPassSpan(s_, lengths);
}

void Performer::OnNote(const snd::NoteEvent& e)
{
    if (!s_.ticked)
    {
        return;
    }

    const snd::Track* track = TrackOf(e.track);
    if (track == nullptr)
    {
        return;
    }

    // A note that gets no channel plays nothing on the console. It gets no MIDI note. So does a note whose program
    // plays nothing at its key.
    if (e.channel < 0)
    {
        s_.strikes.unstarted.insert({e.track, s_.tick, e.key});
        s_.out.unstarted.push_back({.tick = s_.tick,
                                    .length = e.length < 0 ? 0 : static_cast<uint32_t>(e.length),
                                    .track = static_cast<uint8_t>(e.track),
                                    .key = e.key,
                                    .velocity = e.velocity,
                                    .offset = e.offset});
        return;
    }

    const auto c = static_cast<std::size_t>(e.channel);
    Voice& v = s_.voices[c];
    TrackOutput& output = s_.outputs[static_cast<std::size_t>(e.track)];

    // A tie goes on in the note's channel. A tie to another key moves the part's pitch (VoicePitch). The channel keeps
    // the velocity of the note that it started with.
    if (e.tie && v.sounding && v.held && v.track == e.track)
    {
        const snd::Channel& channel = Driver().GetChannel(e.channel);
        if (channel.key != v.key)
        {
            for (std::size_t other = 0; other < s_.voices.size(); other++)
            {
                const Voice& o = s_.voices[other];
                if (other != c && o.sounding && o.track == v.track && o.part == v.part)
                {
                    Approximate("tie to another key on a part with other notes (they follow its pitch)");
                    break;
                }
            }
        }
        if (e.velocity != v.velocity)
        {
            Approximate("tie at another velocity (keeping the first note's)");
        }

        // The tie starts the channel's sweep again, with its counter at 0.
        v.sweep_counter = channel.sweep_counter;
        return;
    }

    // The channel's last note stops at once on the console.
    if (v.sounding)
    {
        CutVoice(e.channel, 0.0, "note cut short by another note's channel (rings on in its release)");
    }

    // The settings that the tick's commands have changed so far, then a part for the note.
    SyncTrack(e.track, *track);
    const int32_t sweep = SweepApplied(e.channel);
    if (NeedsOtherPart(e.track, output.part, sweep != 0) && !SwitchPart(e.track, output))
    {
        Approximate("note on a part whose pitch another note moves (no MIDI channel free)");
    }

    PresetKey key;
    key.program = track->program;
    key.attack = track->attack;
    key.decay = track->decay;
    key.sustain = track->sustain;
    key.release = track->release;
    key.lfo_target = track->modulation.target;
    key.lfo_speed = track->modulation.speed;
    key.lfo_range = track->modulation.range;
    key.lfo_delay = track->modulation.delay;

    // A part that plays the note alone bends it at the rate of the channel's timer.
    const snd::Channel& channel = Driver().GetChannel(e.channel);
    const int32_t bend = BendUnits(*track) + sweep;
    const bool nominal = bend == 0 || PartHolds(e.track, output.part);
    const int timer_pitch = (channel.key - channel.root_key) * 64 + bend;
    const float pitch =
        nominal ? static_cast<float>(bend) / 64.0f
                : static_cast<float>(TimerBend(channel.type, channel.wave.timer, channel.root_key, e.key, timer_pitch));
    const uint32_t preset = seqlib::midi::PresetIndex(s_.out.presets, key);
    s_.out.preset_keys.resize(s_.out.presets.size());
    s_.out.preset_keys[preset].insert(e.key);
    s_.recorder.SendPreset(s_.out, output, s_.tick, preset);
    s_.recorder.SendPitch(s_.out, output, s_.tick, pitch);
    SetPartBend(e.track, output.part, static_cast<float>(bend) / 64.0f, 0.0,
                nominal ? 0.0 : TimerRounding(channel.type, channel.wave.timer, channel.root_key, e.key, timer_pitch));

    // A tie's channel and the channel of an instrument whose release is 0xFF have no length (Driver::NoteOn). The note
    // then plays past its command's length.
    const auto velocity = static_cast<uint8_t>(std::clamp<int>(e.velocity, 1, 127));
    const int32_t length = Driver().GetChannel(e.channel).length;
    v = Voice{.sounding = true,
              .held = true,
              .track = e.track,
              .part = output.part,
              .key = e.key,
              .velocity = e.velocity,
              .note = s_.recorder.NoteOn(s_.out, output, s_.tick, e.key, velocity),
              .ends = length < 0 ? UINT32_MAX : s_.tick + static_cast<uint32_t>(length),
              .sweep_counter = Driver().GetChannel(e.channel).sweep_counter,
              .preset = key,
              .struck = s_.tick,
              .strike_range = track->bend_range,
              .last_bend = BendUnits(*track)};
    s_.last_change_ms = s_.TickMs(s_.tick);

    // For the pass test: what the part stands for at the strike. A note of a track whose LFO moves the pan plays the
    // LFO that the pass writes for it. The LFO's depth therefore counts as sent at every strike.
    std::array<uint8_t, 3>& at_strike = s_.strikes.stands[{e.track, s_.tick}];
    for (std::size_t k = 0; const EventKind kind : {EventKind::kPan, EventKind::kExpression, EventKind::kModulation})
    {
        const auto stand = s_.stood.find({e.track, output.part, kind});
        at_strike[k++] = stand != s_.stood.end() ? stand->second : 0;
    }
    if (track->modulation.target == kPanTarget)
    {
        MarkStandSent(e.track, output.part, EventKind::kModulation, 0.0);
    }

    // CC10 can't follow a pan LFO on a part that sounds two notes (PanLfoChannel). The SFZ files hold the LFO.
    if (track->modulation.target == kPanTarget && track->modulation.depth != 0)
    {
        std::vector<Voice*> sounding;
        for (Voice& o : s_.voices)
        {
            if (o.sounding && o.track == e.track && o.part == v.part)
            {
                sounding.push_back(&o);
            }
        }
        for (Voice* o : sounding)
        {
            if (sounding.size() > 1 && !o->pan_lfo_reported)
            {
                o->pan_lfo_reported = true;
                Approximate("pan LFO on a note that overlaps another on its part (omitted from SoundFont)");
            }
        }
    }

    // The note command ran in the track's last stretch.
    std::vector<NoteCommand>& commands = s_.out.note_commands;
    if (commands.size() < v.note)
    {
        commands.resize(v.note);
        s_.out.note_channels.resize(v.note);
    }
    const std::vector<Stretch>& stretches = s_.out.stretches[static_cast<std::size_t>(e.track)];
    commands[v.note - 1] = {e.offset, stretches.empty() ? 0 : static_cast<uint32_t>(stretches.size() - 1)};
    s_.out.note_channels[v.note - 1] = {.channel = static_cast<int16_t>(e.channel)};
}

} // namespace

} // namespace ntdlib::sequence_export::ntr
