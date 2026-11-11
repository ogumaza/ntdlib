// SPDX-License-Identifier: MIT

// The classes that perform a sequence (see performer.h). The core plays the sequence on nw::snd's model
// (SequenceCore). The Performer follows the core's hooks, and the MidiRecorder records the Performer's MIDI events.
// Addresses without another source come from the Wii U's nw::snd in Animal Crossing: amiibo Festival's executable.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/sequence.h"
#include "ntdlib/sequence_export/passes.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/recording_engine.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

class Performer;

// The core: a console's sequence player on nw::snd's model (nwsnd/). The core plays the sequence, and the performer
// follows it through the hooks below: each command, its arguments' random draws and variable reads, and each note
// command. The performer runs the core's tick of each track, and the core's frames move on with the frames of the
// performer's ticks. `VoiceLayer` gives the console's voices (see recording_engine.h).
template <typename Console, template <typename> class VoiceLayer = MultiVoiceLayer>
class SequenceCore : public RecordingEngine<Console, SequenceCore<Console, VoiceLayer>, Regions, VoiceLayer>
{
    friend class Performer;

public:
    using Base = RecordingEngine<Console, SequenceCore<Console, VoiceLayer>, Regions, VoiceLayer>;
    using Player = typename Base::Player;
    using Channel = typename Base::Channel;
    using Base::Base;

    // A track that runs the parse limit's commands in a tick without a wait reads on in the next tick. The track stops
    // once it has run on that way for kHoldSeconds. `now_ms_` is the tick's time. The performer records a track that
    // stops.
    bool OnParseLimit(Player& player, int track);

    // A track that the policy's parse limit ends, on the Wii. The performer records the stop in the same way as for
    // OnParseLimit.
    void OnParseLimitEnd(Player& player, int track);

    // The hooks that the performer follows (Performer::FollowCommandStart and the others).
    void OnCommandStart(Player& player, int track, uint32_t offset, uint32_t command, bool conditional);
    void OnCommand(Player& player, int track, uint32_t command, int32_t arg1, int32_t arg2);
    void OnExtendedCommand(Player& player, int track, uint32_t command, int32_t arg1, int32_t arg2);
    void OnRandomDraw(Player& player, int track);
    void OnVariableRead(Player& player, int track, int index);
    void OnNoteCommand(Player& player, int track, int key, int velocity, int32_t length);
    void OnNoteOn(Player& player, int track, Channel& channel);
    void OnLegato(Player& player, int track, Channel& channel);

    // A channel's release or note-off, and the end of the count of a track's lengths (Performer::FollowLengths).
    void OnRelease(Player& player, int track, Channel& channel);
    void OnLengthsCounted(Player& player, int track);

    // A note that finds no free channel, and a channel whose voice went to another note (RecordingEngine::NoteOn and
    // ChannelEvent).
    void OnNoChannel(int track);
    void OnChannelDropped(int track, Channel& channel);

    // The Wii's builds before September 2009: a frame in which a channel's volume LFO lifts the envelope's gain above
    // full level (RevolutionPolicy::UpdateChannel). The voice limits the gain to full level.
    void OnGainAboveFull(Channel& channel);

    // The Wii's builds before September 2009: a frame of a channel's volume LFO, with the envelope's level and the
    // LFO's in decibels (RevolutionPolicy::UpdateChannel).
    void OnVolumeLfo(Channel& channel, float envelope_db, float lfo_db);

    // A pass that stops the performance stops the track's tick at the jump. A copy of the performer then runs the rest
    // of the tick (Performer::PlayOn).
    bool StopsTick(Player& player, int track);

    // Whether the counted loop whose body starts at `address` loops for ever: the loops of Performer::SetEndlessLoops
    // do.
    bool LoopsForEver(const Player& player, int track, uint32_t address) const;

    // The build of the console's sound library that plays the sound. The Wii's channels depend on it
    // (RevolutionPolicy::UpdateChannel).
    const typename Console::Build& Build() const
    {
        return build_;
    }

private:
    double now_ms_ = 0.0;

    // The performer that the hooks report to. The performer sets it before each run of the core's tracks. A copy of the
    // performer holds a copy of the core, and the copy sets the pointer to itself.
    Performer* performer_ = nullptr;

    typename Console::Build build_{};

    // Each track's last tick that hit the parse limit, and the time that its run of such ticks began.
    std::array<int64_t, kTrackCount> limit_ticks_ = []
    {
        std::array<int64_t, kTrackCount> ticks;
        ticks.fill(-2);

        return ticks;
    }();
    std::array<double, kTrackCount> run_start_ms_{};
};

// The cores of the Wii U, the Switch, the 3DS and the Wii.
using CafeCore = SequenceCore<nwsnd::CafePolicy>;
using NxCore = SequenceCore<nwsnd::NxPolicy>;
using CtrCore = SequenceCore<nwsnd::CtrPolicy, CtrVoiceLayer>;
using RevolutionCore = SequenceCore<nwsnd::RevolutionPolicy>;

// When a tick comes: the sound frame it's processed in, and its time. The Wii's build of October 2006 counts time in
// 416ths of a tick (NextCoreTick). There `ticks_left` is the number of the frame's ticks after this one, and
// `nominal_ms` the tick's time at MIDI's tempo.
struct TickTime
{
    uint64_t frame = 0;
    double ms = 0;
    int ticks_left = 0;
    double nominal_ms = 0;
};

// What a pass of a sequence's performance found, for the next pass to play by (PerformPasses).
struct PassFindings
{
    bool operator==(const PassFindings&) const = default;

    // Tracks whose filters changed during a sounding note. Their filters require MIDI controllers because presets
    // cannot update an existing note.
    std::array<bool, kTrackCount> filter_moves{};

    // The MIDI notes that tie or mono legatos raise above the velocity they started at, by note id, with the highest
    // velocity each reaches. A later pass starts them there (MidiRecorder::MidiNoteOn).
    std::map<uint32_t, int> rises = {};

    // The MIDI notes the game releases before their SoundFont attack ends, more than 1 dB quieter than the attack has
    // reached then, by note id. Each value is the attack in timecents that reaches the game's level at the release. A
    // later pass gives the notes those attacks (MidiRecorder::MidiNoteOn).
    std::map<uint32_t, int16_t> early_releases = {};

    // On the Wii: the fits of the MIDI notes whose volume LFO the voice holds at full level, by note id
    // (MidiRecorder::VolumeLfoFits). A later pass gives the notes presets with those fits (MidiRecorder::MidiNoteOn).
    std::map<uint32_t, VolumeLfoFit> lfo_fits = {};

    // The MIDI notes whose pitch LFO changes while they sound, by note id. A later pass gives each of them a part of
    // its own. The pitch bends of that part add the LFO (MidiRecorder::MidiNoteOn).
    std::set<uint32_t> lfo_changes = {};

    // The MIDI notes past one of the console's pitch limits whose part bends while they sound, by note id: a sweep, an
    // LFO in the part's bends or a bend. The other notes past a limit hold their pitch. A later pass gives those notes
    // a preset that plays them at the limit (PresetKey::at_pitch_limits).
    std::set<uint32_t> pitch_moves = {};
};

// The recording of a sequence's performance: the MIDI events, and the parts of them that the passes use. The recorder's
// methods take the engine that it records, the Performer.
class MidiRecorder
{
    friend class Performer;

public:
    MidiRecorder();

    // Channel `n` of track `t` enters its release in the current frame.
    void OnRelease(Performer& engine, int t, Note& n);

    // Channel `n` of track `t` enters its release a fraction `after` of a tick past the current tick: a note-off that
    // waited for the channel's first update (Settings::note_off_waits_for_update).
    void OnDeferredRelease(Performer& engine, int t, Note& n, double after);

    // Channel `n` of track `t` stops at once (Channel::Stop).
    void OnStop(Performer& engine, int t, Note& n);

    // Track `t` closes. OnTrackClosing comes before its channels are released, and OnTrackClosed after.
    void OnTrackClosing(Performer& engine, int t);
    void OnTrackClosed(Performer& engine, int t);

    // The frames between the current tick and `next` pass.
    void OnBetweenTicks(Performer& engine, const TickTime& next);

    // Channel `n` of track `t` ends: its one-shot wave or its release has finished.
    void OnEnd(Performer& engine, int t, Note& n);

    // Channel `n` of track `t` has counted down its length for the tick.
    void OnLengthUpdated(Performer& engine, int t, Note& n);

    // Track `t` has run command `cmd` with its first argument `arg1`.
    void OnCommand(Performer& engine, int t, uint32_t cmd, int32_t arg1);

    // The timebase has changed.
    void OnTimebase(Performer& engine);

    // Track `t` stops after running without waits (the policy's kParseLimit).
    void OnRunaway(Performer& engine, int t);

    // Track `t`'s commands for the tick have run, and its settings reach its channels.
    void OnTickEnd(Performer& engine, int t);

    // Track `t` releases all its channels (SequenceTrack::ReleaseAllChannel). OnReleaseAll comes before the channels
    // are released, and OnReleasedAll after.
    void OnReleaseAll(Performer& engine, int t);
    void OnReleasedAll(Performer& engine, int t);

    // Track `t` stops all its channels at once, before they're freed (SequenceTrack::SetMute's mode 3).
    void OnStopAll(Performer& engine, int t);

    // Track `t` plays a note: the first hook of each note, before tie, mono or a new channel.
    void OnNoteOnStart(Performer& engine, int t);

    // Channel `n` of track `t` starts, at velocity `velocity` (the track's velocity range applied), on `region`.
    void OnNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region, bool stereo);

    // A tie or mono note continues channel `n` of track `t` at `key` and `velocity` (Legato). `heard` is false when the
    // channel was below hearing. Returns whether the note plays.
    bool OnLegato(Performer& engine, int t, Note& n, int key, int velocity, bool past_cap, bool heard);

    // Records the note-off of channel `n`'s MIDI note at the current tick, if the note is on.
    void MidiNoteOff(Performer& engine, int t, Note& n);

    // Adds a frame of MIDI note `id`'s volume LFO to the note's sums. The envelope stood at `envelope_db` and the LFO
    // at `lfo_db`. The SoundFont and SFZ play the LFO `scale` times as deep (ModulationValue).
    void OnVolumeLfo(uint32_t id, float envelope_db, float lfo_db, double scale);

    // The fits of the MIDI notes whose volume LFO the voice holds at full level, by note id. Unlisted notes keep the
    // LFO's full swing.
    std::map<uint32_t, VolumeLfoFit> VolumeLfoFits() const;

    // Emits changed volume, pan, pitch and other controllers for track `t`.
    void UpdateOutputs(Performer& engine, int t);

    // Sends part `part` of track `t` its pitch in sound frame `frame` if that has changed: at the current tick, or for
    // a frame between ticks, `after` it by a fraction of a tick (TrackEvent::after). Returns true if it was sent.
    bool SendPitch(Performer& engine, int t, uint8_t part, uint64_t frame, double after = 0);

    // Sends the pitches that timed sweeps reach in the sound frames between the current tick's frame and `next`'s
    // frame. A part whose pitch bends add a note's pitch LFO gets its pitch in the current tick's frame too.
    void SweepBetweenTicks(Performer& engine, const TickTime& next);

    // Gives the notes that track `t` struck in this tick presets with the track's LFO settings. This tick's preset
    // events are sent again in front of its notes.
    void GiveStruckNotesLfo(Performer& engine, int t);

    // Records the current tempo. At tempo or timebase 0, the sequence stops advancing (Run). MIDI keeps its previous
    // tempo. The Wii's build of October 2006 runs tempo / 416 ticks a frame (NextTick).
    void OnTempo(Performer& engine);

    // Counts an approximation required by MIDI or SoundFont.
    void Approximate(const char* what);

    // Records `now_ms_` for the new `tick_`, and its frame start as a fractional tick (Performance::frame_ticks). The
    // Wii's build of October 2006 runs a tick at the start of its frame. MIDI's tempo puts the tick at `nominal_ms_`,
    // up to a frame earlier.
    void RecordTickTime(Performer& engine);

private:
    // Records the changes of track `t`'s volume, pan and sends in the tick (Performance::mix).
    void RecordMix(const Performer& engine, int t);

    // The steps of OnLegato. WithFirstRegionVolume returns `gain` with the volume of the region that channel `n`
    // started on, where Settings::keep_first_region_volume keeps it, over the volume of the region that its MIDI note
    // plays. SamePreset returns whether the note's preset now is the one that its part last sent. ContinueMidiNote
    // continues the note's MIDI note at `key` and `velocity`. `carries_on` says whether the MIDI note can take the
    // legato as it is.
    double WithFirstRegionVolume(const Performer& engine, const Note& n, int velocity, double gain);
    bool SamePreset(const Performer& engine, int t, const Note& n) const;
    void ContinueMidiNote(Performer& engine, int t, Note& n, int key, int velocity, bool carries_on, bool heard);

    // Starts/ends channel `n`'s MIDI note. The note plays `region` (null if none plays its key and velocity). Track
    // CC11 is multiplied by `gain` while the note plays (see Legato).
    void MidiNoteOn(Performer& engine, int t, Note& n, int velocity, const VelocityRegion* region, double gain = 1.0);

    // The steps of MidiNoteOn. PlaceNote moves track `t` to another part where note `n` requires a pitch or level of
    // its own, and places the note on the track's part. RaisedVelocity returns the velocity that the MIDI note starts
    // at, from `capped`, the velocity up to 127. SendNotePreset sends the note's preset. StartMidiNote records the
    // note-on.
    void PlaceNote(Performer& engine, int t, Note& n, int velocity, double gain);
    int RaisedVelocity(const Performer& engine, const Note& n, int capped, const VelocityRegion* region) const;
    void SendNotePreset(Performer& engine, int t, Note& n, const VelocityRegion* region);
    void StartMidiNote(Performer& engine, int t, Note& n, int velocity, int start, const VelocityRegion* region,
                       double gain);

    // Reports the approximations that MIDI and the synths make of track `t`'s LFOs, sends and filters at the start of
    // channel `n`'s MIDI note (MidiNoteOn).
    void ReportNoteApproximations(Performer& engine, int t, const Note& n);

    // Ends `n`'s MIDI note on release, stop or replacement. Removes unheard notes entirely (see Unheard).
    void EndMidiNote(Performer& engine, int t, Note& n);

    // Records the attack for released channel `n` if the game releases its MIDI note before the SoundFont attack ends,
    // more than 1 dB quieter than that attack has reached (PassFindings::early_releases).
    void RecordEarlyRelease(Performer& engine, const Note& n);

    // Detaches track `t`'s channels. They keep sounding.
    void DetachAll(Performer& engine, int t);

    // After track `t` detaches its channels (DetachAll), moves the track to another part if they still sound on its
    // part. In the game they keep their settings. On a shared MIDI channel, the track's later changes would reach them.
    void LeaveTails(Performer& engine, int t);

    // Moves track `t`'s new notes and changes to a part on which nothing but `except` sounds, within its part limit.
    // The notes on the track's current part keep the settings they have. Returns false if every part sounds.
    bool Switch(Performer& engine, int t, const Note* except = nullptr);

    // The steps of UpdateOutputs. A closed track's channels take nothing more from it, but go on with their timed
    // sweeps (Channel::Update): SweepClosedTrack sends their pitches. NoteLevelChange updates Track::last_change for a
    // change of track `t`'s CC11 `level` or its filter's controller value `change_filter`. ReportBalancePan reports a
    // track pan `pan` that the track's stereo waves don't take. SetSends sets the effect sends of `c`, and
    // SetSfzFilters the SFZ filters. SendToLeftParts sends `c` to the parts that the track has left but whose notes it
    // still holds. NotePitchChange updates Track::last_change for a change of the track's pitch. SendPartPitches sends
    // each part its pitch. `tails` says whether detached notes sound on the part that the track plays on.
    void SweepClosedTrack(Performer& engine, int t);
    void NoteLevelChange(Performer& engine, int t, double level, int change_filter);
    void ReportBalancePan(const Performer& engine, int t, int pan);
    void SetSends(const Performer& engine, int t, PartControllers& c) const;
    void SetSfzFilters(const Performer& engine, int t, PartControllers& c);
    void SendToLeftParts(Performer& engine, int t, const PartControllers& c);
    void NotePitchChange(Performer& engine, int t);
    void SendPartPitches(Performer& engine, int t, bool tails);

    // Sends setting `kind` of track `t` at `value` to `part`, or to the part the track plays on, if that part's last
    // value is another (seqlib::midi::Recorder::SendSetting). Returns true if it was sent.
    bool SendSetting(Performer& engine, int t, EventKind kind, int value, int part = -1);

    // Updates Track::last_change for an event of `kind` that track `t` records. UpdateOutputs handles the three values
    // that can differ between passes: the expression, the filter and the pitch. CC91 counts only with
    // Settings::reverb_moves_loops.
    void NoteChange(Performer& engine, int t, EventKind kind);

    // The pitch MIDI gives part `part` of track `t` in sound frame `frame`, in semitones. On the part the track plays
    // on, that's the track's bend and legato key offset with the newest sweep of its notes there that can be heard. A
    // part that the track has left follows the newest note there that can be heard: that note's bend, the offset of its
    // key from the key of its MIDI note, and the newest sweep on the part. Nothing if the part has no such note. A note
    // with lfo_bends adds its pitch LFO.
    std::optional<float> PartPitch(const Performer& engine, int t, uint8_t part, uint64_t frame) const;

    // True if notes that track `t` has detached still sound on the part that the track plays on. That part takes the
    // track's later changes. No other part was free for the track (LeaveTails).
    bool TailsOnPart(const Performer& engine, int t) const;

    // The Wii: reports the notes of track `t` that play at another level than in the game. The game limits a channel's
    // volume to full level (TrackLevel, ZoneLift).
    void CheckFullLevel(Performer& engine, int t);

    // At the end of a tick, reports each of track `t`'s notes whose LFO plays with other settings than its preset has.
    // Keeps the notes whose pitch LFO changes for a later pass (PassFindings::lfo_changes).
    void CheckLfo(Performer& engine, int t);

    // The events of track `t` from `first` to the end, this tick's, with each note's preset with the LFO settings
    // `lfo` in front of it, in place of the preset events (GiveStruckNotesLfo). `sent` holds each part's preset before
    // the tick, and takes the presets sent.
    std::vector<TrackEvent> TickEventsWithLfo(const Performer& engine, int t,
                                              std::vector<TrackEvent>::const_iterator first, const PresetKey& lfo,
                                              std::map<uint8_t, int>& sent);

    // Returns track `t`'s CC11 before legato gain, the level scale and the 127 limit (UpdateOutputs).
    double TrackLevel(const Performer& engine, int t) const;

    // The Wii: the centibels by which the zones of a note at `velocity` on track `t` play louder than their regions'
    // volumes (PresetKey::lift).
    int16_t ZoneLift(const Performer& engine, int t, int velocity) const;

    // Records an event for track `t` at the current tick, on `part` or else the part the track plays on, and updates
    // Track::last_change.
    //
    // nw::snd applies track settings at frame end (SequenceTrack::UpdateChannelParam). A note therefore uses settings
    // from the end of its tick, including changes after its note command. Controller changes go before that tick's
    // notes/presets. A change replaces any earlier value for the same setting. SFZ depends on this order to select the
    // biquad filter at note-on.
    void Emit(Performer& engine, int t, TrackEvent e, int part = -1);

    // The SFZ files select a filtered or an unfiltered region at note-on, from the biquad's controller (sfz.cpp). The
    // game gives the notes that a sound frame starts the settings that the frame ends with. When a tick switches the
    // biquad on or off, each note that an earlier tick of its frame started on the part gets a copy of the controller
    // before it. The copies go before notes of earlier ticks only. A loop that starts in the frame therefore keeps its
    // events.
    void GiveTheFrameTheBiquad(Performer& engine, int t, const TrackEvent& e);

    Performance out_;

    // The note ids, the part switches, the parts used and their limits, the levels, and the approximations and
    // warnings.
    seqlib::midi::Recorder rec_;

    // What the pass before found, and what this pass finds (Performer::Findings). In place of the volume LFOs' fits,
    // this pass sums their frames.
    PassFindings earlier_;
    std::array<bool, kTrackCount> filter_moves_{};
    std::map<uint32_t, int> rises_;
    std::map<uint32_t, int16_t> early_releases_;
    std::map<uint32_t, VolumeLfoSums> lfo_sums_;
    std::set<uint32_t> lfo_changes_;
    std::set<uint32_t> pitch_moves_;
    std::array<TrackMidi, kTrackCount> tracks_{}; // what each track's MIDI channels have been sent
};

// A note command that waits for the core's verdict on its channel (Performer::FollowNoteCommand), and where the command
// lies.
struct PendingNote
{
    int t = 0;
    int key = 0;
    int velocity = 0;
    int32_t length = 0;
    NoteCommand command;
};

// Executes a sequence tick by tick, following nw::snd::SequenceSoundPlayer, and records its output.
class Performer
{
    template <typename Console, template <typename> class VoiceLayer>
    friend class SequenceCore;
    friend class MidiRecorder;

public:
    // What MarkSteadyPass selected (passes.h).
    using LoopChoice = sequence_export::LoopChoice;

    // `earlier` holds what an earlier pass found (Findings): the tracks whose filters take controllers, and the MIDI
    // notes to start higher, to give other attacks, to fit to their volume LFO or to give a part of their own. CC11 is
    // multiplied by `level_scale` (LevelPeak). Each track gets at most `part_limits` parts (Switch). The main loop's
    // choices in `loop_choices` come from an earlier pass (LoopChoices). ChooseSteadyPass selects after them.
    Performer(const Settings& settings, const SoundInfo& sound, const Sequence& seq, Regions& banks,
              const PerformOptions& options, const PassFindings& earlier, double level_scale,
              const std::array<uint8_t, kTrackCount>& part_limits, const std::vector<LoopChoice>& loop_choices);

    // Runs the sound and returns its recorded performance.
    Performance Run();

    // What this pass found, for the next pass to play by.
    PassFindings Findings() const
    {
        return {.filter_moves = midi_.filter_moves_,
                .rises = midi_.rises_,
                .early_releases = midi_.early_releases_,
                .lfo_fits = midi_.VolumeLfoFits(),
                .lfo_changes = midi_.lfo_changes_,
                .pitch_moves = midi_.pitch_moves_};
    }

    // Highest sounding CC11 value before `level_scale` and MIDI clamping. Game volumes can reach 255. This value may
    // therefore exceed 127.
    double LevelPeak() const
    {
        return midi_.rec_.LevelPeak();
    }

    // The parts each track used: one more than the highest.
    const std::array<uint8_t, kTrackCount>& PartsUsed() const
    {
        return midi_.rec_.PartsUsed();
    }

    // The choices MarkSteadyPass made for the main loop, for a later pass to make again.
    const std::vector<LoopChoice>& LoopChoices() const
    {
        return main_loop_.Choices();
    }

    // The counted loops that loop for ever in this performance, each a track and its loop's body. Like the passes of a
    // loop of count 0, their passes count as loops of the sequence.
    void SetEndlessLoops(const std::set<std::pair<int, uint32_t>>& loops)
    {
        endless_loops_ = loops;
    }

    // Stops the performance at `ms`, as at the time limit, without a main loop (PerformBeside). Until then, a sound
    // that holds doesn't end it.
    void SetStop(double ms)
    {
        stop_ms_ = ms;
    }

    // The loops that a performance cut at the time limit makes endless when it runs again: the outermost loop that each
    // open track was in, where that loop is a counted one, each a track and its loop's body. The set is empty when the
    // performance ended before the limit or counted as finished there.
    const std::set<std::pair<int, uint32_t>>& LimitLoops() const
    {
        return limit_loops_;
    }

private:
    // When a track last ran a command: the tick, and the number of commands all tracks had run before it. The number
    // orders the commands of a tick. `call` is the serial number of the innermost call that the command ran in
    // (Track::call_ids), or 0 outside calls.
    struct CommandRun
    {
        uint32_t tick = 0;
        uint64_t order = 0;
        uint64_t call = 0;
    };

    // How a pass of the main loop starts (passes.h).
    using PassStart = sequence_export::PassStart;

    // True for the Wii's engine.
    bool Wii() const
    {
        return settings_.engine == Engine::kRevolution;
    }

    // Whether the sound plays as the Wii's build of October 2006 (SndBuild::October2006).
    bool WiiOctober2006() const
    {
        return Wii() && settings_.wii_build.October2006();
    }

    // Sound frame containing `ms`.
    uint64_t FrameOf(double ms) const
    {
        return static_cast<uint64_t>(ms / settings_.frame_ms + 1e-9);
    }

    // When the next tick comes at the current tempo. Nothing when tempo or timebase is 0: either stops nw::snd sequence
    // timing. The core's clock moves on to the tick.
    std::optional<TickTime> NextTick();

    // Makes a core of type `Core` with `channels` channels and `voices` voices to play the sequence. The core's random
    // state starts from PerformOptions::seed. Each global variable starts at -1. On the Wii, each starts at 0
    // (SeqPlayer::SetGlobalVariable), and the core plays as the game's build of NW4R SND does.
    template <typename Core>
    void MakeCore(Regions& banks, int channels, int voices);

    // NextTick from the core's clock (SequenceSoundPlayer::NextTickTime on the Wii U, the Switch and the 3DS). The
    // Wii's clock counts in float milliseconds, or in 416ths of a tick in the build of October 2006.
    template <typename Core>
    std::optional<TickTime> NextCoreTick(Core& core);

    // Track `t`'s state and settings, taken from the core's track for the performer's analysis and recording. A track
    // that the core's player has freed keeps its state.
    template <typename Core>
    void TakeCoreTrack(Core& core, int t);

    // Track `t`'s volumes, pan and bend, with their moves, from the core's track.
    template <typename Core>
    void TakeCoreRamps(Core& core, int t);

    // Track `t`'s waits from the core's track: the ticks to wait, the wait for its notes to end, and whether its last
    // note is a held one that the track waits out.
    template <typename Core>
    void TakeCoreWaits(Core& core, int t);

    // The tempo, the timebase, the main volume and the variables from the core's player.
    template <typename Core>
    void TakeCorePlayer(Core& core);

    // What the core's run of command `cmd` on track `t` changed, for the performer's analysis and recording: the pass
    // that a jump back or the end of an endless loop closes, the values of the track and the player, the variables, and
    // the notes that a tie, mono or mute releases or stops.
    template <typename Core>
    void RecordCoreCommand(Core& core, int t, uint32_t cmd, int32_t arg1, int32_t arg2);

    // The steps of RecordCoreCommand. ClosePass handles a jump back of track `t`, or the end of an endless loop, that
    // command `cmd` with first argument `arg1` is about to run. RecordVariableCommand records a variable command's read
    // of variable `index` and, when `changed`, its change. `op` is the command's last byte.
    void ClosePass(int t, uint32_t cmd, int32_t arg1);
    void RecordVariableCommand(int t, uint32_t op, uint8_t index, bool changed);

    // ParseTracks on the core: the core runs each track's tick, and the performer follows the core's hooks.
    template <typename Core>
    bool ParseCoreTracks(Core& core, int first);

    // For a copy that PlayOn makes at a jump: the copy of the core runs the rest of track `t`'s tick.
    template <typename Core>
    void ContinueCoreTrack(Core& core, int t);

    // The performer's records after the core ran track `t`'s tick, or the rest of it: the close of a track that ended,
    // the start of a wait, and the track's state and settings from the core. `ran` says whether the track ran commands.
    // Returns whether the track is open after the tick.
    template <typename Core>
    bool FinishCoreTrackTick(Core& core, int t, bool was_open, bool open, bool ran);

    // Runs `f` on the core, if the performer has one.
    template <typename F>
    void WithCore(F&& f)
    {
        const auto run = [&](auto& core)
        {
            if constexpr (!std::is_same_v<std::decay_t<decltype(core)>, std::monostate>)
            {
                f(core);
            }
        };
        std::visit(run, core_);
    }

    // What the core's hooks report (SequenceCore). Each records the sequence's analysis: a command's start, a command
    // that has run with `next` the offset after its arguments, a random draw, an argument's variable read, and a note
    // command. They do nothing once a pass has stopped the performance.
    void FollowCommandStart(int t, uint32_t offset, uint32_t command, bool conditional);
    void FollowCommand(int t, uint32_t command, int32_t arg1, int32_t arg2, uint32_t next);
    void FollowRandomDraw();
    void FollowVariableRead(int t, int index);
    void FollowNoteCommand(int t, int key, int velocity, int32_t length);

    // The core started channel `channel` for track `t`'s note, on the voice with key `key`
    // (RecordingEngine::PositionKey). The note that NoteOn started for the same command plays there.
    void FollowNoteOn(int t, nwsnd::ChannelId channel, int key);

    // The core continued channel `channel` of track `t` for the note command: a tie or a mono legato.
    void FollowLegato(int t, nwsnd::ChannelId channel);

    // The core found no free channel for track `t`'s note. With voice drops (VoiceLimit), the note plays nothing, as in
    // the game. Without them, the performer keeps the note.
    void FollowNoChannel(int t);

    // The core gave the voice of track `t`'s channel `channel` to another note. With voice drops, the note on that
    // channel stops at once, as in the game.
    void FollowChannelDrop(int t, nwsnd::ChannelId channel);

    // On the Wii's builds before September 2009, a volume LFO lifted the envelope's gain above full level on channel
    // `channel` of track `t`. The game holds the gain at full level there. The LFOs of the SoundFont and the SFZ swing
    // above it. Each note is reported once.
    void FollowGainAboveFull(int t, nwsnd::ChannelId channel);

    // On the Wii's builds before September 2009, channel `channel` of track `t` played a frame of its volume LFO. The
    // envelope stood at `envelope_db` and the LFO at `lfo_db`. The frame goes into the sums of the note on the channel
    // (MidiRecorder::OnVolumeLfo).
    void FollowVolumeLfo(int t, nwsnd::ChannelId channel, float envelope_db, float lfo_db);

    // Track `t`'s newest note on channel `channel`, on the track or let go, or nullptr. An older note's channel has
    // ended, and a newer channel can take its place.
    Note* NewestNoteOn(int t, nwsnd::ChannelId channel);

    // The core released channel `channel`, or ran a note-off on it. While the core counts down a track's lengths
    // (SequenceTrack::UpdateChannelLength, 0x022B6E88), FollowRelease keeps the channels whose lengths ran out. After
    // the count, FollowLengths releases their notes, in the order the notes started. FollowLengths copies each note's
    // length from its channel, or counts the length of a note without one. The channels that have stopped are removed.
    void FollowRelease(nwsnd::ChannelId channel);
    void FollowLengths(int t);

    // Runs the note command that FollowNoteCommand keeps until the core's verdict: NoteOn and the track's record of it.
    // With `drop`, the core found no free channel, and NoteOn starts no channel. A command's start and the end of a
    // track's tick settle a note that the core gave no verdict on, such as one with no region.
    void SettleNote(bool drop);

    // The voices for the notes that the game drops: --voices's count. 0, the default, keeps every note. The drops come
    // from the core.
    int VoiceLimit() const
    {
        return options_.voice_limit;
    }

    // The new notes that a track can start in one tick: the game's count (Settings::voices) with --voices. Without it,
    // the cap is kMaxVoiceLimit, far above any console's count. A loop without waits then makes at most that many MIDI
    // notes in one tick.
    int TickNoteCap() const
    {
        return VoiceLimit() > 0 ? settings_.voices : static_cast<int>(kMaxVoiceLimit);
    }

    // The core's channel that plays note `n`, or nullptr once the core has freed it.
    template <typename Core>
    const typename Core::Channel* CoreChannelOf(Core& core, const Note& n);

    // Each note's frame in which its one-shot wave runs out, from the core's voice layer, after the core's frames.
    template <typename Core>
    void TakeCoreWaves(Core& core);

    // The value of the pitch LFO of each note with lfo_bends in sound frame `frame`, from the core's channel before the
    // frame's update (Note::lfo_pitch).
    template <typename Core>
    void TakeCoreLfos(Core& core, uint64_t frame);
    template <typename Core>
    void TakeNoteLfo(Core& core, Note& n, const typename Core::Channel& channel, uint64_t frame);

    // The core's frames without ticks, from the current frame up to frame `until`, and each note's one-shot playback
    // and pitch LFO from them. They continue the waves past the sound's last tick. The frames stop once no note's
    // one-shot wave plays on, and no pitch LFO goes on in a note with lfo_bends.
    template <typename Core>
    void RunCoreTail(Core& core, uint64_t until);

    // After sequence timing stops, continues sounding channels at the MIDI tempo (OnTempo). Releases and one-shot waves
    // finish normally; held notes last kHoldSeconds. No further sequence commands run.
    void PlayOutStopped();

    // Real milliseconds per tick at the current tempo and PerformOptions::tempo_ratio, or 0 at tempo 0 or timebase 0. A
    // tick of the Wii's build of October 2006 lasts 416 / tempo frames, whatever the timebase.
    double MsPerTick() const
    {
        if (WiiOctober2006())
        {
            return tempo_ > 0 ? settings_.frame_ms * kTickUnits / (tempo_ * double{options_.tempo_ratio}) : 0.0;
        }

        const double tpm = static_cast<double>(timebase_) * tempo_ * options_.tempo_ratio; // ticks per minute
        return tpm > 0 ? 60000.0 / tpm : 0.0;
    }

    // First tick at or after `ms`. Extrapolates beyond the last tick at the final tempo, or the MIDI tempo if sequence
    // timing stopped.
    uint32_t MsToTick(double ms) const;

    // Milliseconds per tick at MIDI's tempo (OnTempo).
    double MidiMsPerTick() const
    {
        return 60000.0 / (midi_.out_.tempo.back().bpm * midi_.out_.timebase);
    }

    // Includes a stopped note's end time in the sound's duration.
    void Ended(const Note& n)
    {
        sound_end_ms_ = std::max(sound_end_ms_, std::min(n.SoundEndMs(), now_ms_));
    }

    // True if `n` hasn't been released and its envelope has sunk below hearing (MidiNote::quiet_ms). Such a note then
    // counts as silence in the search for loops, held sounds and the silence at the time limit.
    bool BelowHearing(const Note& n) const
    {
        return !n.released && now_ms_ >= n.midi.quiet_ms;
    }

    // Allocates track 0, tracks requested by sound flags, and those in a leading alloctrack command.
    void AllocateTracks();

    // Starts the core's sound, with the tracks that AllocateTracks allocated, the game's variables and the tracks that
    // PerformOptions::tracks leaves out silenced. The performer takes the player's and the global variables from the
    // core.
    void StartCore();

    // Ends the performance at the time limit. Returns true where the sequence counts as finished there.
    bool EndAtTimeLimit();

    // Ends the performance where a pass stopped it (stop_), and sets `next_pass` to where the main loop's next pass
    // starts. Returns true where the sequence counts as finished there.
    bool EndAtStoppingPass(PassStart& next_pass);

    // Whether the performance ends after the current tick, and if so, whether the sequence counts as finished. A
    // sequence that holds counts from `steady_since`, the time it started holding, or -1.
    std::optional<bool> EndAfterTick(bool any_open, double& steady_since);

    // Ends all notes and records duration/end tick. `finished` means the sequence ended naturally.
    void EndPerformance(bool finished);

    // The steps of EndPerformance. EndNotes ends the MIDI notes of every track's channels. In a sequence that ended
    // naturally (`finished`), it also records the end of their sound and their tails' sweeps. SetFinishedEnd sets the
    // end of a sequence that ended naturally, and leaves out the events that can't be heard after its last sound.
    void EndNotes(bool finished);
    void SetFinishedEnd();

    // Starts track `t` at command offset `offset`.
    void OpenTrack(int t, uint32_t offset);

    // Releases track `t`'s channels and closes the track.
    void CloseTrack(int t);

    // Starts a tick: the core begins it and moves the main volume a tick on. The performer takes the main volume.
    void StartTick();

    // Records each global variable that the tick's commands changed from `before`, its value at the start of the tick
    // after the game's changes (Performance::global_changes).
    void RecordGlobalChanges(const std::array<int16_t, 16>& before);

    // Makes the game's changes of variables that come by the current frame (PerformOptions::changes), before the
    // frame's first tick. The search for the main loop then starts again. It also starts again at the frame of a write
    // in a user procedure (PerformOptions::procedure_changes). The write waits from then for a call.
    void ChangeVariables();

    // Makes the game's writes in user procedure `procedure` (PerformOptions::procedure_changes) that wait for a call by
    // the current frame and haven't taken effect, in their order. A call of the procedure by a track runs them. The
    // search for the main loop then starts again.
    void ChangeInProcedure(uint16_t procedure);

    // Runs the game's model of user procedure `procedure` (PerformOptions::user_procedures) for a call by track `t`.
    // The model's changes of variables count as the track's. A call that changed the model's state counts as a random
    // draw.
    void RunUserProcedure(int t, uint16_t procedure);

    // Whether the frame of every write in a user procedure has come.
    bool ProcedureChangesArmed() const
    {
        return armed_procedure_changes_ == options_.procedure_changes.size();
    }

    // Runs the open tracks from track `first` on for one tick, and closes those that finish. Returns true if one of
    // them is still open.
    bool ParseTracks(int first);

    // Ends a tick: sends each track's changes, and reports the notes whose LFOs play otherwise than their presets.
    void FinishTick();

    // Moves on to the tick at `next`. Sends the pitches that timed sweeps reach on the way.
    void AdvanceTo(const TickTime& next);

    // How a pass that starts at `tick` starts.
    PassStart PassStartAt(uint32_t tick) const;

    // A copy of this performer that has played the rest of the tick in which track `t` jumped back to `target` and
    // stopped the performance, with no limit on the loops (PlayOn and TurnsInner).
    Performer ResumeAfterJump(int t, uint32_t target) const;

    // Moves on to the next tick and plays it. Returns false when no tick comes or no track is open after it.
    bool PlayNextTick();

    // A copy of this performer that plays on from the tick in which track `t` jumped back to `target` and stopped the
    // performance, up to but not including tick `until`, with no limit on the loops.
    Performer PlayOn(int t, uint32_t target, uint32_t until) const;

    // How the next pass of the main loop starts when track `t` has jumped back to `target` in this tick.
    PassStart NextPassStart(int t, uint32_t target) const;

    // True if a copy of this performer that plays on for kMinLoopSeconds from track `t`'s jump back to `target` finds
    // the loop at `target` inner (CountedPassStart). A pass of another loop then ends with a jump back to `target` in
    // it.
    bool TurnsInner(int t, uint32_t target) const;

    // True if a MIDI player that loops on the pass from `start` to `end` plays each repeat's notes as the game plays
    // the notes of the next pass (passes.h). This performer has played that pass.
    bool RepeatsAsTheGame(uint32_t start, uint32_t end) const;

    // Marks the first of the loop owner's passes that plays as the pass after it does (RepeatsAsTheGame). A MIDI player
    // that loops on the markers plays each repeat as the marked pass. Track `t` has just jumped back to `target`.
    // Returns true if the performance can stop, false if it requires another pass first.
    bool MarkSteadyPass(int t, uint32_t target);

    // True if the bytecode's 16- and 24-bit arguments are big-endian (Settings::arguments_follow_byte_order).
    bool ArgumentsBigEndian() const
    {
        return !settings_.arguments_follow_byte_order || seq_.big_endian;
    }

    // Resolves player, global or track variable `index` for track `t`. Returns nullptr above 47.
    int16_t* Variable(int t, uint8_t index);

    // Sets track variable `index` - 32 (`index` from 32 to 47) of every allocated track, in the core and here.
    template <typename Core>
    void SetTrackVariables(Core& core, uint8_t index, int16_t value);

    // Records that track `t` reads player or global variable `index` (a comparison or an argument), or changes it. A
    // change sets Track::last_signal if another open track has read the variable and can still read on.
    void NoteVariableRead(int t, uint8_t index);
    void NoteVariableChange(int t, uint8_t index);

    // The notes of mute mode `mode` on track `t`: mode 2 releases them, and mode 3 stops them.
    void MuteNotes(int t, uint8_t mode);

    // True if track `t` is in a subroutine and the pass that began with command `first_command` (as commands_run_
    // counts) ran a conditional jump, call, return or fin and played no note. The track must also hold no note that
    // sounds until the game stops it (see OnLoop).
    bool Leavable(int t, uint64_t first_command) const;

    // Gives each entry that track `t`'s call stack gained since the track's last command a serial number
    // (Track::call_ids). A command either pushes entries or pops them. The numbered entries therefore follow the stack.
    void NumberCalls(int t);

    // The serial number of the innermost call on track `t`'s stack, or 0 outside calls.
    uint64_t InnermostCall(int t) const;

    // True if track `t` has returned from the call with serial number `call`. Commands outside calls (0) never are.
    bool ReturnedFrom(int t, uint64_t call) const;

    // Handles track `t` jumping back to `target`. `jump` is true for a jump command and false for the end of an endless
    // loop. Marks the pass that the jump ends if the pass counts.
    void OnLoop(int t, uint32_t target, bool jump);

    // The first tick of the pass that track `t`'s jump back to `target` ends, if the pass counts as a pass of the loop.
    // The check updates the track's records of its jumps back, its inner loops, its last pass and whether that pass was
    // idle.
    std::optional<uint32_t> CountedPassStart(int t, uint32_t target, bool jump);

    // Whether the pass of track `t` that its jump back to `target` ends is a later pass of an inner loop, and doesn't
    // count (CountedPassStart). `first_command` is the order of the pass's first command (CommandRun::order). Records
    // the jump back and the track's inner loops.
    bool InnerLoopPass(int t, uint32_t target, uint64_t first_command);

    // Whether track `t` changed something during a pass that started at tick `start` with the command of order
    // `first_command`.
    bool ChangedSince(int t, uint32_t start, uint64_t first_command) const;

    // Marks a counted pass of track `t`'s loop at `target`, from tick `start` to now, in the main loop's passes and its
    // markers. Stops the performance after the requested repetitions.
    void MarkPass(int t, uint32_t target, uint32_t start);

    // Plays a note on track `t`. Allocates a channel for it, or reuses the newest one for tie/mono. Returns false for a
    // legato on a channel below hearing. Such a legato plays nothing. With `drop`, the game found no free channel for a
    // new note: NoteOn ends after the tie and mono rules, and returns false.
    bool NoteOn(int t, int key, int velocity, int32_t length, bool drop = false);

    // The channel that a note of track `t` at `key` and velocity `vel` starts on `region` and its wave `pcm`, with the
    // note's `length` (NoteOn).
    Note NewNote(int t, int key, int vel, int32_t length, const VelocityRegion& region, const Pcm& pcm);

    // Moves channel `n` to `key` without another attack. Continues its MIDI note where possible, or once this tick has
    // reached the legato cap of Settings::voices (`past_cap`; see NoteOn). Otherwise starts a new one. Returns false if
    // the channel is below hearing (BelowHearing). A legato doesn't start the envelope again. The channel therefore
    // stays there.
    bool Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap);

    // Starts `n`'s sweep from the core's channel for the note command (settling_channel_), or no sweep without one.
    void StartSweep(Note& n);

    // True before `n` has been heard. Envelopes start at -90.4 dB and first advance at the end of the note-on frame.
    bool Unheard(const Note& n) const;

    // True if released channel `n` remains above the inaudibility threshold (kInaudibleSustain).
    bool StillAudible(const Note& n) const;

    // True if channel `n` can be heard: if it's released, above the inaudibility threshold (StillAudible).
    bool Heard(const Note& n) const;

    // True if no channel of track `t` but `n` can be heard on the part the track plays on, released ones included. CC11
    // acts on the whole MIDI channel. Only a note that's alone can therefore start higher and be brought down to its
    // velocity by CC11 (MidiNoteOn).
    bool Alone(int t, const Note& n) const;

    // True if a channel of track `t` on `part`, other than `except`, still sounds.
    bool PartSounds(int t, uint8_t part, const Note* except = nullptr) const;

    // Like tie, mono and mute mode 2, releases track `t`'s channels and detaches them. Gives them the track's settings
    // first.
    void ReleaseAndDetach(int t);

    // Releases channel `n`, at MML release rate `release_value` if one is given.
    void ReleaseNote(int t, Note& n, int release_value = -1);

    // Stops channel `n` at once.
    void StopNote(int t, Note& n);

    // Removes stopped channels from track `t`, including detached channels.
    void PruneNotes(int t);

    // Makes the note-offs pending in the current tick's frame (Note::release_pending) if `next` comes in a later frame.
    // nn::atk makes them in the frame's channel update, after the envelopes' first step. So each note sounds for that
    // frame and releases from there in the next. Its MIDI note-off comes where that frame starts.
    void ReleasePending(const TickTime& next);

    // True if a note of track `t` on `part` that sounds at `ms` has a timed sweep still moving after sound frame
    // `frame`, or has lfo_bends.
    bool SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const;

    // Sends the pitches that the timed sweeps of a finished sequence's release tails reach after its last tick. Each
    // pitch goes as far after that tick as its frame starts, at the sequence's last tempo.
    void SweepAfterEnd();

    // The Wii: returns the volume NW4R SND gives track `t`'s channels (SeqTrack::UpdateChannelParam): each volume
    // squared, the volume from the sound's handle, and the sound's volume, clamped to 1 (SeqPlayer's volume).
    float UserVolume(int t) const;

    // Track `t`'s effect send on `bus`, from 0 to 127 for CC91 and CC93. It's the track's send over 127 plus the
    // sound's send (PerformOptions::fx_sends). A voice holds the sum between 0 and 1.
    int EffectSend(int t, int bus) const;

    // Builds channel `n`'s current preset key. The key uses a controller for the filter if track `t` is in
    // `midi_.earlier_.filter_moves`.
    PresetKey NoteKey(int t, const Note& n) const;

    // Recalculates track `t`'s filter and records changes affecting sounding notes (PassFindings::filter_moves). As in
    // the game, detached channels keep their previous filter.
    void SetFilter(int t);

    // Returns true if one of track `t`'s channels is still sounding.
    bool TrackBusy(int t) const;

    // True if any channel of track `t` except `except` is sounding, including detached channels.
    bool OthersSounding(int t, const Note* except) const;

    // True if open track `t` won't read on before the game stops the sound: it waits a negative number of ticks, waits
    // out a note of kHeldTicks or more, or is blocked on held, looping notes that never end.
    bool WaitsForever(int t) const;

    // True if track `t` sounds nothing, and kHoldSeconds have passed since its last change or since it opened,
    // whichever is later.
    bool Quiet(int t) const;

    // The Wii: true if no open track will read on before the game stops the sound and no note sounds: the sequence has
    // ended.
    bool Stuck() const;

    // True if every open track waits for ever (WaitsForever) and no sweep, volume, pan or bend can change. The volumes
    // are the track's volume, volume2 and the main volume.
    bool Steady() const;

    // Returns the time to end the performance at if the main loop holds a sound unchanged. The loop's pass started at
    // tick `start`. Such a pass is longer than kHoldSeconds and has changed nothing for longer than that. The pass
    // sounds only endless notes, each the newest on an open track with tie or mono on. The next pass usually continues
    // them. The sound then holds forever. As with Steady, the performance ends kHoldSeconds after the last change.
    std::optional<double> HeldSoundEnd(uint32_t start) const;

    // The tick of the last change that HeldSoundEnd and EndPerformance count: the last MIDI event or tempo change.
    // Without Settings::reverb_moves_loops, a change of CC91 doesn't count.
    uint32_t LastChangeTick() const;

    // True if the marked main loop sounds nothing once the notes before it have ended. No note starts from the loop's
    // start on. No channel has a MIDI note any more. Each channel has ended or ends without the game. Release tails and
    // fades may still play. The game keeps such a sound in silence until it stops the sound, or until a poll leaves the
    // loop.
    bool SilentLoop() const;

    const Settings& settings_;
    const SoundInfo& sound_;
    const Sequence& seq_;
    Regions& banks_;
    PerformOptions options_;

    // The core, on the Wii U, the Switch, the 3DS and the Wii, and whether any of its tracks is open after the tracks
    // that have run this tick.
    std::variant<std::monostate, CafeCore, NxCore, CtrCore, RevolutionCore> core_;
    bool core_open_ = false;

    // The notes that NoteOn has started, and the track whose newest note waits for the core's channel, or -1
    // (FollowNoteOn).
    uint64_t notes_started_ = 0;
    int note_to_pair_ = -1;

    // The note command that waits for the core's verdict (FollowNoteCommand, SettleNote), and the core's channel for it
    // while FollowNoteOn and FollowLegato settle it. The MIDI notes that NoteOn starts while SettleNote settles a note
    // command take `settling_command_` (Performance::note_commands).
    std::optional<PendingNote> pending_note_;
    nwsnd::ChannelId settling_channel_ = nwsnd::kNoChannel;
    NoteCommand settling_command_;

    // The core counts down a track's lengths, and the channels that the count releases (FollowRelease).
    bool counting_lengths_ = false;
    std::vector<nwsnd::ChannelId> length_releases_;

    MidiRecorder midi_; // what the performer records
    std::array<Track, kTrackCount> tracks_{};
    std::array<int16_t, 16> player_vars_{};
    std::array<int16_t, 16> global_vars_{};
    uint32_t timebase_ = 48;
    int tempo_ = 120;
    nwsnd::MoveValue<uint8_t> main_volume_;
    uint32_t tick_ = 0;
    double now_ms_ = 0;
    uint64_t frame_ = 0; // the sound frame the current tick is processed in

    // The Wii's build of October 2006 (NextCoreTick): the ticks of the current frame after this one, and the tick's
    // time at MIDI's tempo.
    int frame_ticks_left_ = 0;
    double nominal_ms_ = 0;
    bool balance_ = false; // the Wii's stereo waves play in balance mode (Performance::balance)
    std::array<std::map<uint32_t, CommandRun>, kTrackCount> last_run_; // when each command offset last ran, per track
    uint64_t commands_run_ = 0;                                        // by all tracks
    uint64_t call_serial_ = 0;                                         // the last serial number of a call (NumberCalls)
    std::vector<double> tick_ms_;                                      // the time of each tick
    double sound_end_ms_ = 0;                                          // end time of the last completed note
    bool stop_ = false;
    int stopped_by_ = -1;                         // the track whose pass of the main loop stopped the performance
    std::optional<double> held_sound_end_ms_;     // set when the main loop holds a sound (HeldSoundEnd)
    bool silent_loop_ = false;                    // the main loop sounded nothing (SilentLoop)
    MainLoopPasses main_loop_;                    // the main loop's counted passes and marked pass (OnLoop)
    std::size_t next_change_ = 0;                 // the first of the game's changes still to come (ChangeVariables)
    std::vector<bool> procedure_taken_;           // for each write in a user procedure, whether it has taken effect
    std::size_t armed_procedure_changes_ = 0;     // the writes in user procedures whose frames have come
    uint32_t changed_at_ = 0;                     // the tick of the last change, or of a procedure write's frame
    std::optional<uint32_t> last_draw_;           // the last tick a track drew a random number in
    uint64_t last_draw_order_ = 0;                // commands_run_ at the last random draw, or 0 before the first
    std::array<uint16_t, 32> variable_readers_{}; // the tracks that have read each player or global variable
    std::set<std::pair<int, uint32_t>> endless_loops_;
    std::set<std::pair<int, uint32_t>> limit_loops_;
    std::optional<double> stop_ms_; // SetStop
};

} // namespace ntdlib::sequence_export
