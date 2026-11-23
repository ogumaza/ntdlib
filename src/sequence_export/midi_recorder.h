// SPDX-License-Identifier: MIT

// The recording of a sequence's performance (MidiRecorder), with what a pass finds for the next (PassFindings) and the
// time of a tick (TickTime). The Performer in performer_engine.h records through it. midi_recorder.cpp contains the
// code.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/sar/formats.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "sequence_export/performer_records.h"

namespace ntdlib::sequence_export
{

class Performer;

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

    // The events of track `t` from `first` to the end, this tick's, with each note's preset with the LFO settings `lfo`
    // in front of it, in place of the preset events (GiveStruckNotesLfo). `sent` holds each part's preset before the
    // tick, and takes the presets sent.
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

} // namespace ntdlib::sequence_export
