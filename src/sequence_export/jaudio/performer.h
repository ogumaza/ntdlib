// SPDX-License-Identifier: MIT

// Sequence execution (see performance.h). The performer follows JAudio2's sequence player as Four Swords Adventures'
// main.dol links it. The parser's command table (built at 0x80491e54) is Twilight Princess's without 0xe7. The
// performer reads 0xe7's arguments as Twilight Princess's table gives them. Like Twilight Princess's player, the
// performer sets an oscillator's scale with them. The player's classes are Twilight Princess's: JASSeqParser,
// JASSeqCtrl, JASSeqReader, JASTrack, JASRegisterParam, JASTrackPort, JASChannel, JASOscillator, JASLfo and
// JASDSPChannel. Each sub-frame of 80 samples, the channels update first (JASDSPChannel::updateAll), then the sequence
// runs its ticks (JASTrack::seqMain).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/sequence_export/jaudio/instruments.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/passes.h"
#include "sequence_export/jaudio/performer_records.h"

namespace ntdlib::sequence_export::jaudio
{

using ntdlib::sequence_export::MainLoopPasses;

// Execute a sequence sub-frame by sub-frame, following JAudio2, and record its output.
class Performer
{
public:
    // What MarkSteadyPass chose: whether the performance stops, the pass of the main loop it marked, if any, and
    // whether the performance plays one more pass to mark it.
    using LoopChoice = ntdlib::sequence_export::LoopChoice;

    // Multiply CC11 by `level_scale` (LevelPeak). Give each MIDI track at most `part_limits` parts. Make the main
    // loop's choices in `loop_choices`, from an earlier run (LoopChoices), and then decide. The messages name the
    // program `program`.
    Performer(const SequenceSound& sound, BankSet& banks, const PerformOptions& options, const std::string& program,
              double level_scale, const std::array<uint8_t, 16>& part_limits,
              const std::vector<LoopChoice>& loop_choices, const std::map<uint32_t, uint8_t>& tie_velocities,
              const std::set<int>& swept_filters);

    Performance Run();

    // Highest CC11 value before `level_scale` and MIDI clamping.
    double LevelPeak() const
    {
        return recorder_.LevelPeak();
    }

    // The parts each MIDI track used: one more than the highest.
    const std::array<uint8_t, 16>& PartsUsed() const
    {
        return recorder_.PartsUsed();
    }

    // The choices MarkSteadyPass made for the main loop, for a later run to make again.
    const std::vector<LoopChoice>& LoopChoices() const
    {
        return main_loop_.Choices();
    }

    // The highest velocity that ties gave each MIDI note above its velocity, by MIDI note ID, for a later run to start
    // the note at.
    const std::map<uint32_t, uint8_t>& TieVelocities() const
    {
        return tie_velocities_;
    }

    // The tracks whose notes played under more than one entry of the IIR filter's table (ZoneFilter).
    const std::set<int>& SweptFilters() const
    {
        return swept_filters_;
    }

private:
    // How a pass of the main loop starts: each part's settings and the tempo after the commands of its first tick, and
    // the part each MIDI track's new notes go to.
    using PassStart = ntdlib::sequence_export::PassStart;

    // A pass of a loop that the sub-frame's end decides about: the track, and the ticks it started and ended in.
    struct PendingLoop
    {
        int track = 0;
        uint32_t start = 0;
        uint32_t end = 0;
    };

    // A command's arguments, as ReadArguments reads them.
    using Arguments = std::array<uint32_t, 8>;

    // --- Tracks ---
    // Track `t`, by its index in tracks_.
    Track& TrackAt(int t)
    {
        return tracks_[static_cast<std::size_t>(t)];
    }

    const Track& TrackAt(int t) const
    {
        return tracks_[static_cast<std::size_t>(t)];
    }

    int NewTrack(int parent, int number);

    // True if track `t` or a track above it is muted (JASTrack::isMute, 0x800385ac), or is at a volume of 0 through the
    // sound's handle (SequenceSound::child_volumes).
    bool Muted(int t) const;

    void CloseTrack(int t);
    int OpenChild(int t, uint32_t number, uint32_t start);
    void CloseChild(int t, uint32_t number);
    int TickProc(int t);
    int SeqTick(int t);

    // Make track `t` wait for the `length` of the gate note that it has started, or for the note's end with a length of
    // 0.
    Outcome WaitForGateNote(int t, uint32_t length);

    // True while track `t` waits for its gate note to end. The wait ends with the note.
    bool WaitsForGateNote(int t);

    // Run track `t`'s commands up to its next wait. Return -1 when the track finishes, or when it runs kParseLimit
    // commands without a wait: that would hang the game.
    int ParseToWait(int t);

    uint8_t ReadByte(int t);
    uint16_t Read16(int t);
    uint32_t Read24(int t);
    uint32_t ReadMidiValue(int t);
    void SkipPrint(int t);
    uint32_t Data8(uint32_t offset) const;
    uint32_t Data16(uint32_t offset) const;
    uint32_t Data24(uint32_t offset) const;
    uint32_t Data32(uint32_t offset) const;
    void Jump(int t, uint32_t target);
    bool Call(int t, uint32_t target);
    bool Return(int t);

    // Start a loop of `count` passes on track `t`. The loop's end returns to the command after this one.
    void LoopStart(int t, uint32_t count);

    // End a pass of track `t`'s innermost loop. Go back to its start while passes are left.
    void LoopEnd(int t);

    bool Condition(int t, uint32_t condition);
    uint32_t ReadReg(int t, uint32_t reg);
    void WriteReg(int t, uint32_t reg, uint32_t value);
    void WriteRegister(int t, uint32_t reg, uint32_t value);
    uint32_t ReadRegister(int t, uint32_t reg);
    void Interrupt(int t, int type);

    // The track whose port a command of track `t` reaches: its parent, or with `child` its child that the high four
    // bits of `port` give. The low four bits give the port. -1 for none.
    int PortTrack(int t, bool child, uint32_t port) const;

    // Write `value` to port `port` of track `other`, from track `t`. For ports 0 and 1, the write raises the other
    // track's port interrupt.
    void WritePortOf(int t, int other, uint32_t port, uint32_t value);

    // Write `value` to port `port` of track `t`, as the game's code does (JASystem::TTrack::writePortAppDirect). For
    // ports 0 and 1, the write raises the track's port interrupt.
    void WritePortApp(int t, uint32_t port, uint32_t value);

    // Record track `t`'s read and write of port `port`. The root track's read of a port that still holds its first
    // value counts in Performance::ports_read. The root track's write of the port ends that.
    void NotePortRead(int t, uint32_t port);
    void NotePortWrite(int t, uint32_t port);

    void CheckInterrupt(int t);
    void TimerProcess(int t);
    void SetParam(int t, uint32_t target, float value, uint32_t count);
    void UpdateTimedParams(int t);
    void UpdateSeq(int t, bool force, float pan_scale);
    void UpdateTrack(int t, float pan_scale);
    float TicksPerSubFrame() const;
    int TransposeTotal(int t) const;

    // --- JAudio2's commands ---
    Outcome Parse(int t);
    Outcome Command(int t, uint8_t cmd, uint16_t types);

    // JAudio2's commands by their high four bits, with the arguments `a` that Command has read: the notes, the latest
    // key and the moves of the track's parameters (0xa0 to 0xbf), the children, calls, jumps and loops (0xc0 to 0xcf),
    // the ports and registers (0xd0 to 0xdf), the track's settings (0xe0 to 0xef), and the waits, interrupts, timer,
    // callback, prints and the end (0xf0 to 0xff).
    Outcome NoteCommand(int t, uint8_t cmd, const Arguments& a);
    void FlowCommand(int t, uint8_t cmd, const Arguments& a);
    void PortAndRegisterCommand(int t, uint8_t cmd, const Arguments& a);
    void SettingCommand(int t, uint8_t cmd, const Arguments& a);
    Outcome ControlCommand(int t, uint8_t cmd, const Arguments& a);

    // JASSeqParser::cmdReg (0xd9 and 0xda): arithmetic on a register, with a value or another register's value.
    void RegisterArithmetic(int t, const Arguments& a);

    // 0xdc: a byte, 16 bits, 24 bits or 32 bits from a table in the sequence, at an index or a byte offset, into a
    // register.
    void LoadFromTable(int t, const Arguments& a);

    // JASTrack::setOscTable (0xe8) and setOscAdsr (0xe9): a table for one of the track's oscillators.
    void SetOscTable(int t, const Arguments& a);
    void SetOscAdsr(int t, const Arguments& a);

    Arguments ReadArguments(int t, const CommandInfo& info, uint16_t override_types,
                            uint32_t (Performer::*read_reg)(int, uint32_t));

    // --- JAudio1 ---
    void InitTrack1(int t);
    int OpenChild1(int t, uint32_t mode_and_number, uint32_t start);
    int SeqTick1(int t);
    Outcome Parse1(int t);

    // The steps of Parse1 for the end of a note (0xf9, and 0x8x but the waits 0x80 and 0x88) and for cmdSetParam
    // (0x9x), with the command in `b`.
    void NoteOff1(int t, uint8_t b);
    void SetParam1(int t, uint8_t b);

    Outcome Note1(int t, uint8_t key);
    Outcome Command1(int t, uint8_t cmd, uint16_t override_types);

    // cmdCall (0xc4) and cmdJmp (0xc8). They read their arguments themselves. A flag byte sets which ones follow.
    void JumpOrCall1(int t, uint8_t cmd);

    // JAudio1's other commands in groups, with the arguments `a` that Command1 has read. The groups are the children,
    // returns, loops, waits and the end; the ports; the oscillators; the interrupts; the track settings; and the calls
    // to the game.
    Outcome TrackCommand1(int t, uint8_t cmd, const Arguments& a);
    void PortCommand1(int t, uint8_t cmd, const Arguments& a);
    void OscillatorCommand1(int t, uint8_t cmd, const Arguments& a);
    void InterruptCommand1(int t, uint8_t cmd, const Arguments& a);
    void SettingCommand1(int t, uint8_t cmd, const Arguments& a);
    void GameCommand1(int t, uint8_t cmd, const Arguments& a);

    void RegisterCommand1(int t, uint8_t low);

    // The steps of RegisterCommand1: the value in the encoding that the command gives, a load from a table of `kind` 4
    // to 8, and the store of the result in register `reg`. Registers 0x20 to 0x2f stand for parts of other registers
    // and for the track's addresses.
    int16_t ReadRegisterValue1(int t, uint32_t encoding);
    uint32_t LoadTable1(uint32_t table, uint32_t kind, uint32_t index) const;
    void StoreResult1(int t, uint32_t reg, int16_t value, uint32_t loaded);

    uint32_t ReadReg1(int t, uint32_t reg);
    uint16_t ReadReg16(int t, uint32_t reg);
    uint32_t ReadReg32(int t, uint32_t reg);
    void WriteRegDirect1(int t, uint32_t reg, uint16_t value);
    void StoreRegister1(int t, uint32_t reg, uint16_t value, uint16_t flag);
    void CheckInterrupt1(int t);
    void UpdateTrack1(int t);

    // Whether the game squares the volume of track `tr`: in volume mode 0, where its JASystem squares a track's volume.
    // Pikmin's library squares none (UpdateTrack1).
    bool SquaresVolume(const Track& tr) const
    {
        return tr.volume_mode == 0 && sound_.squared_track_volume && !sound_.jac;
    }

    std::size_t Registers1() const;

    // The share of its parent's tempo that Pikmin's child track `t` runs at, at most 1 (Track::tempo).
    float TempoShare(int t) const;

    // --- Pikmin 2's conductors ---
    int ChildNumber(int t) const;
    uint32_t ConductorRandom(uint32_t n);
    std::optional<uint16_t> ConductorCall(int t, uint16_t id);

    // The conductor calls of ConductorCall: the tables and the number of tracks (0x300), the restart of a track (0x600)
    // or its next module (0x800), the rounds of a module (0x900 to 0xd00) and the beat (0x2e00).
    uint16_t ConductorTables(int t);
    uint16_t ConductorTrackCall(int t, uint16_t id);
    uint16_t NextConductorModule(int t, int number, const ConductorTrack* followed);
    void FindConductorLists(const ConductorModule& mod, ConductorModuleState& m);
    uint16_t ConductorModuleCall(int t, uint16_t id);
    uint16_t ConductorBeat();

    uint16_t ConductorLength(int t, std::size_t number, bool in_order);
    void ConductorTip(int t, std::size_t number);
    uint16_t ConductorPlay(int t, std::size_t number);
    bool ConductorDone() const;

    // --- Sub-frames ---
    // The steps of Run. StartSequence sets up the voices, the echo lines and the first track with its ports, and
    // returns the first track. PlaySequence runs sub-frames until the sequence ends, loops, holds or reaches the time
    // limit. It returns true if the sequence finished.
    int StartSequence();
    bool PlaySequence(int root);

    // Run one sub-frame: the channels, then the ticks that are due. Return false when the first track finishes.
    bool Advance();

    // --- Channels ---
    std::optional<std::size_t> StartChannel(int t, int key, int velocity, uint32_t timer, float sweep = 0.0f,
                                            uint32_t sweep_subframes = 0, bool bends = false);

    // The steps of StartChannel. FindProgramSound finds what the program of track `tr` plays at `key` and `velocity`.
    // StartOscillators starts the oscillators of channel `ch` with the tables of `found`, or with the track tables in
    // their place.
    std::optional<NoteSound> FindProgramSound(const Track& tr, int key, int velocity);
    void StartOscillators(Track& tr, const NoteSound& found, Channel& ch);

    void RouteOscillators1(Track& tr, Channel& ch);
    void NoteOn(int t, uint32_t note_id, int key, int velocity);
    void GateOn(int t, int key, int velocity, float time, uint32_t flags);
    void NoteOff(int t, uint32_t note_id, uint16_t release);
    void ReleaseChannel(std::size_t c, uint16_t direct_release);
    void Detach(std::size_t c);
    void EndChannel(std::size_t c);
    void UpdateChannels();
    void UpdateChannel(Channel& ch);

    // The step of UpdateChannel after the first sub-frame: the LFOs, the gate timer and the oscillators. Returns
    // false when the volume oscillator stops and the channel ends.
    bool AdvanceChannel(Channel& ch);

    std::optional<std::size_t> AllocateVoice(uint8_t priority);
    float ChannelVolume(const Channel& ch, float osc_volume) const;
    float ChannelRatio(const Channel& ch, float osc_pitch) const;

    // --- The echo ---
    // Add what `ch` sends to the echo in the sub-frame that it played from position `from`.
    void SendToEcho(const Channel& ch, double from);

    // Move the echo on by sub-frame `subframe`, with the sends of the sub-frame.
    void StepEcho(uint64_t subframe);

    // True if the echo has something at kEchoAudible or above still to play.
    bool EchoSounds() const;

    // --- Loops and ends ---
    // Handle track `t` jumping back to `target`: a pass of a loop when it's a jump back to commands that ran before a
    // wait. Detect the main loop, and ask the sub-frame's end to decide whether to stop.
    void OnJumpBack(int t, uint32_t target);

    // Decide about the pending loop at the end of a sub-frame: mark its steady pass and stop, or play on.
    void DecideLoop();

    // True if open track `t` won't read on before the game stops the sound: it waits for a note that never ends, or
    // waits kHeldTicks or more.
    bool WaitsForever(int t) const;

    // True if track `t` sounds nothing, and kHoldSeconds have passed since its last change or since it opened.
    bool Quiet(int t) const;

    // True if no open track will read on before the game stops the sound and nothing sounds: the sequence has ended.
    bool Stuck() const;

    // True if every open track waits for ever and none of their settings moves.
    bool Steady() const;

    // The time to end at if the main loop, which started at tick `start`, holds a sound unchanged: its pass is longer
    // than kHoldSeconds, nothing has changed for longer than that, and every sounding note is an endless one that its
    // track hasn't released. The performance ends kHoldSeconds after the last change.
    std::optional<double> HeldSoundEnd(uint32_t start) const;

    // How a pass that starts at `tick` starts.
    PassStart PassStartAt(uint32_t tick) const;

    // A copy of this performer that plays on up to tick `until`, with no limit on the loops.
    Performer PlayOn(uint32_t until) const;

    // True if a player that loops on the pass from `start` to `end` plays each repeat's notes as the game plays the
    // notes of the next pass: the same notes at the same ticks, with the same settings and tempo.
    bool RepeatsAsTheGame(uint32_t start, uint32_t end) const;

    // Mark the first of the loop owner's passes that plays as the pass after it does. Return true if the performance
    // can stop.
    bool MarkSteadyPass();

    bool AnySounding() const;
    bool TrackSounds(int t) const;
    void Changed(int t);
    double TickSeconds(uint32_t tick) const;
    void EndPerformance(bool finished);

    // The steps of EndPerformance for a sequence that finished. PlayTails plays the tails of the notes and the echo
    // out. SetFinishedLength sets the length of the performance from the last sound and the last event.
    void PlayTails();
    void SetFinishedLength();

    uint32_t SecondsToTick(double seconds) const;

    // --- Effects (SequenceSound::effect) ---
    // The track of the effect's slot, or -1 while it hasn't opened.
    int EffectSlotTrack() const;

    // Whether track `t` plays the effect, for the loops and the end: a track below the slot's track, or the slot's
    // track while it runs the effect's routine. Without an effect, every track does.
    bool InEffect(int t) const;

    // Whether the slot has taken the effect: the library's writes are done, and port 0 no longer holds 1.
    bool EffectTaken() const;

    // Whether the slot is free again after the effect: port 2 holds 0, and port 0 doesn't hold 1.
    bool EffectFree() const;

    // Start the effect once its slot's track has opened: write the slot's ports as the game's sound library does.
    void StartEffect();

    // --- Recording ---
    int SlotOf(int t);
    seqlib::midi::TrackOutput& Output(int t);
    void EmitTempo();
    void RecordOutputs(int t);
    void SendSetting(int t, EventKind kind, int value);
    void SendPitch(int t, float semitones);
    float GatePitch(int t) const;
    bool GateBends(int t) const;
    bool HoldsOnPart(int t) const;
    void SweepBetweenTicks();
    void SwitchPart(int t);
    void MidiNoteOn(std::size_t c);

    // The steps of MidiNoteOn. NotePreset returns the index of the preset that channel `ch` plays. OscillatorIndex
    // returns the index of `osc` in the track oscillators of the performance, and adds it if it's new. PlanTieVelocity
    // starts the note at the velocity that its ties reach, where the instrument plays the same wave.
    uint32_t NotePreset(const Channel& ch);
    int OscillatorIndex(const Oscillator& osc);
    void PlanTieVelocity(Channel& ch);

    void MidiNoteOff(std::size_t c);
    bool Audible(const Channel& ch) const;
    bool TailsOnPart(int slot, uint8_t part) const;
    void Approximate(const std::string& what);
    void RecordTickTime();
    double TrackLevel(int t) const;
    float SoundVolume() const;
    void MoveFade();

    // The entry of the IIR filter that the zones play for track `t`'s notes, or the open one. A track whose notes play
    // under more than one entry gets the open one: a zone's filter can't move during a note. So does an entry that
    // nobody would hear.
    uint8_t ZoneFilter(int t) const;
    int ChannelsTrack(int t) const;
    std::array<float, 3> NoteWeights(int t, std::size_t value) const;

    const SequenceSound& sound_;
    BankSet& banks_;
    PerformOptions options_;
    std::string program_;
    Random rng_;
    Performance out_;
    seqlib::midi::Recorder recorder_;
    std::vector<Track> tracks_;
    std::vector<Channel> channels_;
    std::vector<int> voices_;  // channel index per voice, -1 free
    float accumulator_ = 1.0f; // ticks due (JASTrack's field_0x1d8)
    uint16_t tempo_ = 120;
    uint16_t timebase_ = 48;
    float tempo_rate_ = 1.0f; // the game's tempo ratio (SequenceSound::tempo_ratio)
    float fade_ = 1.0f;       // the level of the game's fade-in so far (SequenceSound::fade_in)
    uint64_t subframe_ = 0;
    uint32_t tick_ = 0;
    bool ticked_ = false; // the first tick has run
    std::vector<double> tick_seconds_;
    std::vector<uint64_t> tick_subframes_;
    double sound_end_ = 0.0;     // when the last channel stopped
    uint64_t last_sounding_ = 0; // the last sub-frame in which a channel sounded

    // The game's echo (SequenceSound::echo): the left and right lines, one entry for each sub-frame of the delay, the
    // entry that the next sub-frame plays, the sends of the sub-frame, and the last sub-frame in which it sounded.
    std::array<std::vector<float>, 2> echo_lines_;
    std::size_t echo_next_ = 0;
    std::array<float, 2> echo_sends_{};
    std::optional<uint64_t> echo_heard_;

    uint64_t commands_run_ = 0; // by all tracks
    bool stop_ = false;
    bool silent_end_ = false; // the main loop's pass was idle and silent: the sequence has ended
    std::optional<PendingLoop> pending_loop_;
    std::optional<uint32_t> stop_tick_;             // the end of the pass that stopped the performance
    MainLoopPasses main_loop_;                      // the main loop's counted passes and marked pass
    std::optional<double> held_sound_end_;          // set when the main loop holds a sound (HeldSoundEnd)
    std::optional<uint32_t> last_draw_;             // the last tick a track drew a random number in
    std::array<int, 16> slot_tracks_{};             // the track recorded in each MIDI track, or -1
    int effect_track_ = -1;                         // the track of the effect's slot, once it has opened
    int root_track_ = -1;                           // the root track, once Run has opened it
    uint16_t first_ports_ = 0;                      // the root's ports that hold their first values (ports_read)
    std::map<uint32_t, uint8_t> tie_velocities_in_; // an earlier run's TieVelocities
    std::map<uint32_t, uint8_t> tie_velocities_;

    // A conductor's state (SequenceSound::conductor): its tracks, the sequence's tables of lists of phrases and of
    // lengths, PSSystem::oRandom, and the first track's beat (SeqTrackRoot +0x3c).
    std::vector<ConductorTrackState> conductor_tracks_;
    uint32_t conductor_phrases_ = 0;
    uint32_t conductor_lengths_ = 0;
    uint32_t conductor_random_ = 0;
    bool beat_top_ = false;

    std::set<int> swept_filters_in_; // an earlier run's SweptFilters
    std::set<int> swept_filters_;
    std::map<int, uint8_t> filter_seen_; // the first entry that each track's notes played under
};

} // namespace ntdlib::sequence_export::jaudio
