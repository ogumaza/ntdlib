// SPDX-License-Identifier: MIT

// A model of MP2K's per-frame routine: its tracks, sound channels, envelopes, the mixer's progress through each sample
// and the writes to the PSG's registers, frame by frame.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/agb/gba.h"
#include "ntdlib/agb/rom.h"
#include "ntdlib/commands/sequence.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"

namespace ntdlib::mp2k
{
using ntdlib::agb::Rom;

// The bits of a channel's status.
constexpr uint8_t kStatusStart = 0x80;   // a note that has just started
constexpr uint8_t kStatusStop = 0x40;    // the note's release
constexpr uint8_t kStatusSpecial = 0x20; // a reversed or compressed sample that the mixer has set up
constexpr uint8_t kStatusLoop = 0x10;    // a sample that loops
constexpr uint8_t kStatusEcho = 0x04;    // the echo after the release
constexpr uint8_t kStatusEnvelope = 0x03;
constexpr uint8_t kStatusOn = kStatusStart | kStatusStop | kStatusEcho | kStatusEnvelope;

// The envelope's phases, in the low 2 bits of the status.
constexpr uint8_t kPhaseAttack = 3;
constexpr uint8_t kPhaseDecay = 2;
constexpr uint8_t kPhaseSustain = 1;
constexpr uint8_t kPhaseRelease = 0;

// The settings that a PSG channel's registers take in the frame.
constexpr uint8_t kModifyVolume = 0x01; // the outputs, the envelope and the wave channel's volume
constexpr uint8_t kModifyPitch = 0x02;

// The number of the first PSG channel, after the DirectSound channels.
constexpr int kFirstPsgChannel = kMaxDirectChannels;

// The number of sound channels: the DirectSound channels and the four PSG channels.
constexpr int kChannelCount = kMaxDirectChannels + 4;

// The most sound channels of a sequencer that grows its channels (Sequencer::GrowChannels). The console has
// kChannelCount.
constexpr int kMostChannels = 1024;

// The driver's tick: each frame adds the tempo to a counter. Each 150 in the counter is a tick.
constexpr int kTickCounter = 150;

// The tempo before a song sets one, in the driver's units: quarter notes a minute at 60 frames a second.
constexpr int kDefaultTempo = 150;

// A sound channel: one of the DirectSound mixer's, or one of the four PSG channels.
struct Channel
{
    uint8_t status = 0;
    uint8_t type = 0; // the voice's type
    uint8_t right = 0;
    uint8_t left = 0;
    uint8_t attack = 0;
    uint8_t decay = 0;
    uint8_t sustain = 0;
    uint8_t release = 0;
    uint8_t key = 0; // the key it plays at, before the track's key shift and bend
    uint8_t envelope = 0;
    uint8_t echo_volume = 0;
    uint8_t echo_length = 0;
    uint8_t gate = 0; // the ticks left before the note's release, or 0 for a tied note
    uint8_t midi_key = 0;
    uint8_t velocity = 0;
    uint8_t priority = 0;
    int8_t rhythm_pan = 0;
    int8_t start_pan = 0;   // the track's pan when the note started, for the Metroid games' mixer
    uint32_t frequency = 0; // a DirectSound channel's rate in Hz, or the PSG channel's frequency setting
    uint32_t wave = 0;      // the sample, or the PSG channel's duty, wave pattern or noise type
    uint8_t part = 0;       // 0 for a DirectSound channel, or the PSG channel that it plays: 1 to 4
    uint16_t unit = 0;      // the PSG unit whose registers a PSG channel writes: 0, or a grown unit (GrowChannels)
    int track = -1;
    int prev = -1; // the track's list of channels, newest first
    int next = -1;

    // A DirectSound channel's progress through its sample. Camelot's mixer plays a sample of no length as a synth
    // voice, whose count holds the pulse wave's duty or the saw wave's filter, and whose fraction is the wave's phase.
    int32_t count = 0;     // the points left before the sample's end
    uint32_t fraction = 0; // 23 bits
    uint32_t position = 0; // the address of the point it plays, or the point's number in a compressed sample
    bool synth = false;

    // A PSG channel's envelope and output settings.
    uint8_t goal = 0;    // the envelope's full level, 0-31: at most 15 on each side
    uint8_t counter = 0; // counts before the envelope's next step
    uint8_t sustain_goal = 0;
    uint8_t pan = 0;
    uint8_t pan_mask = 0;

    // A PSG channel's register settings, as the driver keeps them for its writes.
    uint8_t modify = 0;   // the settings that the frame writes: kModifyVolume and kModifyPitch
    uint8_t length = 0;   // the voice's sound length, for NRx1
    uint8_t sweep = 0;    // channel 1's sweep, for NR10
    uint8_t n4 = 0;       // NRx4's high bits: the length's enable, and the wave channel's restart
    uint8_t step = 0;     // the envelope's step time and direction that the frame writes to NRx2's low 4 bits
    uint8_t lag = 0;      // the levels by which the hardware envelope trails the Metroid games' count of the level
    uint32_t pattern = 0; // the wave pattern in the wave channel's RAM

    // The conversion's view of the channel: its note has got louder than silence.
    bool audible = false;
};

// Something a track or a sound channel did, for the MIDI conversion.
struct Action
{
    enum Kind : uint8_t
    {
        kNoteOn,
        kNoteDropped, // a note that found no channel, or whose voice plays nothing
        kRelease,     // the release of a channel's note
        kNoteEnd,     // the end of a channel's note: its sound ended, or a later note took the channel
        kAudible,     // the first time a channel's note got louder than silence
        kVoice,       // value = the voice
        kLevels,      // left and right = the levels that the track's volume and pan give each side
        kPitch,       // value = the pitch offset in 256ths of a semitone
        kTempo,       // value = the tempo, in quarter notes a minute at 60 frames a second
        kCommand,     // value = the value of a command that a track ran, as the driver reads it (RecordCommands)
    };

    Kind kind = kNoteOn;
    uint8_t track = 0;
    uint8_t key = 0;       // kNoteOn, kNoteDropped: the key
    uint8_t velocity = 0;  // kNoteOn, kNoteDropped: the velocity
    uint8_t play_key = 0;  // kNoteOn: the key the note plays at, after a drum kit
    uint8_t gate = 0;      // kNoteOn, kNoteDropped: the note's length in ticks, or 0 for a tied note
    int32_t value = 0;     // kVoice, kPitch, kTempo and kCommand: the setting
    uint8_t left = 0;      // kLevels: the level on the left, 0-255
    uint8_t right = 0;     // kLevels: the level on the right
    int32_t old_tempo = 0; // kTempo: the tempo before it, which the rest of this tick's progress counted at
    int32_t counter = 0;   // kTempo: the progress towards the next tick, in the tempo's units, after this one
    uint64_t tick = 0;     // the player's tick count when it happened
    uint32_t frame = 0;    // the frame it happened on
    int channel = -1;      // kNoteOn, kRelease, kNoteEnd, kAudible: the channel
    int program = -1;      // kNoteOn: the track's voice
    uint32_t voice = 0;    // kNoteOn: the voice that plays the note, after a key split or drum kit
    uint32_t command = 0;  // kNoteOn, kNoteDropped, kCommand: the address of the command's first byte
    bool sounded = true;   // kNoteEnd: false if the note ended before the mixer started it
    bool taken = false;    // kNoteEnd: a later note took the channel
    bool modified = false; // kNoteOn: extended commands changed the track's voice, or start notes partway in
    uint32_t next = 0;     // kCommand: the address after the command
    commands::Kind command_kind = commands::Kind::kOther; // kCommand: what the command does (mp2k/commands.h)
};

// The most commands that a track runs in one tick. The driver has no such limit. A track that loops without waiting
// runs for ever there, and the game hangs. Step stops and throws a Hang instead. A song that doesn't hang runs far
// fewer commands in a tick.
constexpr int kCommandBudget = 100000;

// What Step throws when a track runs kCommandBudget commands in a tick: the song would hang the game. The sequencer's
// state is then that of the commands run so far. Restore a snapshot of it, or start again.
class Hang : public std::runtime_error
{
public:
    Hang(int track, uint32_t address);

    // The track's number.
    int TrackIndex() const;

    // The address of the command that the track would run next.
    uint32_t Address() const;

private:
    int track_;
    uint32_t address_;
};

// Plays a song one frame at a time, as the driver's per-frame routine does.
class Sequencer
{
public:
    // Sets up `song` as the driver does when the game starts it on its music player after the driver's init. `rom` and
    // `info` have to outlive the sequencer.
    Sequencer(const Rom& rom, const DriverInfo& info, int song);

    // Returns true if the song's header was read, and its music player plays at least one of its tracks.
    bool Valid() const
    {
        return valid_;
    }

    // From the next note on, gives a note a channel past the console's when `on` and the driver would give it a channel
    // whose note is on and not in its release, or none: a grown channel of the note's part whose note is off, or a new
    // one, up to kMostChannels. A new note still takes a channel whose note is in its release, as on the console. No
    // note then cuts another before its release, and no priority leaves a note out (the maintainer, 2026-11-02). A
    // grown DirectSound channel mixes as the console's do. A grown PSG channel writes the registers of a grown PSG unit
    // (agb::kGrownPsgRegisters), as the driver writes the PSG's. The enhanced mixing mode's hosts grow the channels.
    // The console has kChannelCount.
    void GrowChannels(bool on);

    // The sound channels: the console's kChannelCount, and the channels grown since.
    int ChannelCount() const
    {
        return int(channels_.size());
    }

    // Runs one frame of the driver's per-frame routine. Returns what the tracks and channels did in it, in order.
    const std::vector<Action>& Step();

    // From the next frame on, mixes the DirectSound channels' samples into the mixer's buffer as the driver's mixer
    // does, point for point. The model follows the stereo mixer of the SDK's versions with and without reversed and
    // compressed samples, the mono mixer and Camelot's mixer. Call it before the first Step(): the buffer starts
    // silent. Throws std::invalid_argument when the buffer holds fewer than two frames' parts, or when the echo of
    // Camelot's mixer doesn't reach back within a frame.
    void StartMixing();

    // The mixer's output for the last frame: a signed 8-bit point for each of the frame's points, on each side, or
    // DriverInfo::camelot_output_points for each from Camelot's mixer. The mono mixer's left side is its right side.
    // Empty until the model mixes.
    std::span<const int8_t> Right() const;
    std::span<const int8_t> Left() const;

    // After StartMixing(), mixes the DirectSound channels in the enhanced mixing mode too, from the next frame on. Call
    // it before the first Step(). The console's mixer goes on as before: its buffer, its channels' progress through
    // their samples and the ends of their notes stay the console's. The enhanced mode keeps the console's samples,
    // steps, volumes, reverb and echo, and lifts the limits of the mixer's arithmetic:
    //
    // - The mixer puts out agb::kEnhancedFrameSamples samples a frame on each side, in floating point, on the scale of
    //   its 8-bit points. Nothing wraps, saturates or rounds a sum.
    // - A sample's position moves on by the console's step for each of the console's points. The enhanced mode's
    //   samples lie at even shares of the frame's move. The band-limited interpolator (wave::SincKernel) takes the
    //   sample's points around each position. A looped sample's points repeat its loop. Before a sample's start and
    //   past its end, the points are 0.
    // - Each side's volume follows the console's formula without its rounding. It ramps across the frame from its
    //   value in the last frame: sample i of n takes from + i * ((to - from) / n). A note that starts takes its volumes
    //   at once.
    // - The SDK's reverb takes the enhanced output of the frames whose parts the console's reverb reads. Both sides of
    //   each part add up, times the reverb over 512. The mono mixer's one side takes the reverb over 256.
    // - Camelot's echo keeps its delays and its levels. In Golden Sun: The Lost Age, a side takes 53/128 of its output
    //   from 4 frames before, less 8/128 of the other side's. The right side takes (1/4 + 1/512) of its output from a
    //   frame before, and the left side 1/4 of the right side's output from 1 frame and a third before. In Mario
    //   Tennis: Power Tour, the levels are 32/128 and -6/128 from 7 frames before, and the right side's share from a
    //   frame before is 1/4.
    // - A sample of 4-bit steps gives its decoded points. The interpolator takes them like a sample's points.
    // - Camelot's synth voices give the console's points at the mixer's rate. The band-limited interpolator takes
    //   them, with the next frame's first points as the voice would play them at the same pitch.
    // - A value of the reverb's or the echo's history below 2^-16 becomes 0.
    // - A note's samples start in the frame that starts the note, and end with its channel. The interpolator's span
    //   reaches no frame before or after them.
    //
    // Each operation takes its operands in a fixed order, and every platform gives the same samples.
    void StartEnhancedMixing();

    // Whether the frames from the next on mix in the enhanced mode. A frame without moves the channels on as before.
    // Its enhanced output is silent, and the reverb and the echo take it as silence. A host runs such frames to reach a
    // place in a song quickly.
    void SetEnhancedFrames(bool on)
    {
        enhanced_frames_ = on;
    }

    // The enhanced mode's output for the last frame, on each side. The mono mixer's left side is its right side. Empty
    // until the model mixes in the enhanced mode.
    std::span<const float> EnhancedRight() const;
    std::span<const float> EnhancedLeft() const;

    // The stores that the last frame made to the sound registers before the FIFOs, in order, as the driver makes them:
    // the PSG's registers, the sound control registers and the wave RAM.
    const std::vector<agb::RegisterWrite>& SoundWrites() const
    {
        return sound_writes_;
    }

    // Sets the sound registers from agb::kRegSound1CntL up to agb::kRegFifoA that the driver's writes start from, as
    // the game left them. The driver reads some of them back when it writes them. The model starts from the registers
    // that the SDK's init leaves. Throws std::invalid_argument unless `registers` holds agb::kSoundRegisterBytes bytes.
    void SetSoundRegisters(std::span<const uint8_t> registers);

    // From tick `tick` on, has each track report its levels where it first sets its volume or pan. The track reports
    // its pitch where it first sets that. A command sets them, and so does clearing the LFO's swing. The reports come
    // even where the settings stay the same. A player that jumps back to a loop's start keeps the settings that the
    // loop's end left. The game does the same until the loop sets them again.
    void RestateFrom(uint64_t tick)
    {
        restate_from_ = tick;
    }

    // From the next frame on, reports each command that a track runs (Action::kCommand), with its address and the
    // address after it. `command` holds the address. An extended wait reports itself once: the ticks in which it runs
    // again add no report.
    void RecordCommands()
    {
        record_commands_ = true;
    }

    // Sets the PSG envelopes' frame counter, 0 to 14. The driver counts the counter down once a frame, to 14 after 0.
    // The frame in which the counter reaches 0 steps the envelopes twice. The SDK's init leaves the counter at 0. At a
    // song's start, the counter depends on the frames that the driver ran since the init. Throws
    // std::invalid_argument for a value outside 0 to 14.
    void SetEnvelopeCounter(int counter);

    // The sound registers from agb::kRegSound1CntL up to agb::kRegFifoA: the values that the writes left.
    std::span<const uint8_t> SoundRegisters() const
    {
        return sound_registers_;
    }

    // From the next frame on, leaves the DirectSound channels of the tracks in `tracks` (bit t for track t) out of the
    // mixer's sums. A channel belongs to the track whose note last started it (ChannelTrack()). A muted channel moves
    // on through its sample as before. The PSG's channels aren't the mixer's: the hardware mutes them.
    void SetMutedTracks(uint16_t tracks)
    {
        muted_tracks_ = tracks;
    }

    // Returns the track whose note last started channel `channel`, or -1 before any note has.
    int ChannelTrack(int channel) const
    {
        return owners_[size_t(channel)];
    }

    // From the next frame on, keeps each track's peaks in the mixer when `on`: the largest magnitude, in a frame, of
    // the sum of the points that the track's DirectSound channels add to each side. The mixer's reverb and a muted
    // track add nothing. Without meters, the peaks are 0.
    void MeterTracks(bool on)
    {
        metering_ = on;
    }

    // Returns track `track`'s peaks of the last frame, left then right, in the mixer's 8-bit points.
    std::array<int32_t, 2> MixerPeaks(int track) const
    {
        return peaks_[size_t(track)];
    }

    // The memory that the song's memacc commands read and write: kMemAccSize bytes, 0 at the start. A game's code can
    // write it too.
    uint8_t Memory(int index) const
    {
        return memory_[size_t(index)];
    }
    void SetMemory(int index, uint8_t value)
    {
        memory_[size_t(index)] = value;
    }

    // Returns true when the mixer adds nothing more: the last frame's part of its buffer holds only 0, and with a
    // reverb, the whole buffer does. Without mixing, returns true.
    bool MixerSilent() const;

    // Returns true once every track has ended.
    bool TracksEnded() const
    {
        return (status_ & kPlayerStopped) != 0;
    }

    // Returns true once every track has ended and every channel is silent.
    bool Ended() const;

    // Returns the number of ticks the player has played.
    uint64_t Tick() const
    {
        return clock_;
    }

    int TrackCount() const
    {
        return int(tracks_.size());
    }

    // Returns the number of DirectSound channels the mixer uses for this song.
    int DirectChannels() const
    {
        return direct_channels_;
    }

    const Channel& GetChannel(int index) const
    {
        return channels_[size_t(index)];
    }

    // Returns problems found in the song's data.
    const std::vector<std::string>& Warnings() const
    {
        return warnings_;
    }

private:
    // The state of a track: the driver's MusicPlayerTrack.
    struct Track
    {
        uint8_t flags = 0;
        uint8_t wait = 0;
        uint8_t pattern_level = 0;
        uint8_t repeats = 0;
        uint8_t gate = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint8_t running = 0;
        int8_t key_m = 0;
        uint8_t pit_m = 0;
        int8_t key_shift = 0;
        int8_t tune = 0;
        int8_t bend = 0;
        uint8_t bend_range = 0;
        uint8_t vol_mr = 0;
        uint8_t vol_ml = 0;
        uint8_t vol = 0;
        uint8_t vol_x = 0;
        int8_t pan = 0;
        int8_t mod_m = 0;
        uint8_t mod = 0;
        uint8_t mod_t = 0;
        uint8_t lfo_speed = 0;
        uint8_t lfo_speed_c = 0;
        uint8_t lfo_delay = 0;
        uint8_t lfo_delay_c = 0;
        uint8_t priority = 0;
        uint8_t echo_volume = 0;
        uint8_t echo_length = 0;
        int chan = -1; // the newest of its channels
        Voice tone;    // its copy of its voice
        uint16_t timer = 0;
        uint32_t offset = 0; // the point that its notes start from in their samples
        uint32_t position = 0;
        std::array<uint32_t, 3> stack = {};

        // The conversion's view of the track.
        int voice = -1;
        bool modified = false; // an extended command changed its copy of its voice
        int last_left = -1, last_right = -1, last_pitch = 0x7FFFFFFF;
        uint8_t sets = 0;    // the settings it set in the tick, changed or not
        uint8_t restate = 0; // the settings it reports where it next sets them
    };

    // A track's levels on each side.
    struct SideLevels
    {
        uint8_t left = 0;
        uint8_t right = 0;
    };

    // The player's status once every track has ended.
    static constexpr uint32_t kPlayerStopped = 0x80000000u;

    // Runs a tick of track `t`: counts down its notes, runs its commands that are due and moves its LFO on.
    void RunTrack(int t);

    // Moves a track's LFO on for a tick of its wait in the Metroid games' way (0x08001696 in Metroid Fusion).
    void RunMetroidLfo(Track& track);

    // Runs one command of track `t`. Returns false if the track has ended.
    bool RunCommand(int t);

    void StartTrack(Track& track);
    // Plays a note of track `t` whose first byte lies at `command`, with the track's key, velocity and length.
    void Note(int t, uint32_t command);
    void EndTie(int t);
    void EndTrack(int t);
    void ExtendedCommand(int t, const commands::Command& command);
    void MemAcc(Track& track, const commands::Command& command);
    void ClearModulation(Track& track);

    // Returns the levels that a track's volume and pan give each side. The LFO's swing counts when it modulates them.
    // The driver's TrkVolPitSet computes the same, and `metroid` gives the Metroid games' sequencer's routine
    // (0x0800491C in Metroid Fusion).
    static SideLevels TrackLevels(const Track& track, bool metroid);

    // Returns the levels that the Metroid games' sequencer gives a track's volume at `pan` (0x0800491C in Metroid
    // Fusion).
    static SideLevels MetroidLevels(const Track& track, int pan);

    // Recalculates a track's volumes and pitch, as the driver's TrkVolPitSet does, if its flags ask for it.
    void UpdateTrack(Track& track);

    // Sets a channel's volumes from its velocity and its track's volumes.
    void SetChannelVolume(Channel& c, const Track& track) const;

    // Sets a channel's frequency from its key and its track's pitch.
    void SetChannelPitch(Channel& c, const Track& track) const;

    // Returns a square or wave channel's frequency setting for `key`, raised by `fine`/256 of a semitone, from the
    // Metroid games' table.
    uint32_t MetroidPsgFrequency(int key, int fine) const;

    // Updates the channels of tracks whose volume or pitch changed in the frame.
    void UpdateChannels();

    // Returns the channel a note of priority `priority` on track `t` takes, or -1 if there's none it can take.
    int FindChannel(int t, int psg, int priority) const;

    // Returns a grown channel of part `part` whose note is off, or a new one. Returns -1 at kMostChannels.
    int GrownChannel(int part);

    void LinkChannel(int index, int t);
    void UnlinkChannel(int index);

    // Returns the channel a DirectSound note takes in Camelot's version of the driver, or -1 if there's none it can
    // take.
    int FindCamelotChannel(int t, int priority) const;

    void RunPsg();
    void RunDirect();

    // Writes PSG channel `n`'s registers in the channel's unit as the driver does when the channel's note starts.
    void StartPsgRegisters(int n, Channel& c);

    // Writes PSG channel `n`'s pitch, volumes and envelope in the channel's unit, where the channel's modify flags are
    // set.
    void WritePsgRegisters(int n, Channel& c);

    // Turns PSG channel `n` of unit `unit` off as the driver does: an envelope of 0 restarts it, or the wave channel
    // stops.
    void StopPsg(int n, int unit);

    // The address of the register at the console's `address` in PSG unit `unit`.
    static uint32_t UnitAddress(int unit, uint32_t address);

    // Stores `value` to the sound register at `address` and records the store. The address can lie in a grown unit.
    void WriteSound(uint32_t address, int size, uint32_t value);

    // The byte in the sound register at `address`, as the writes left it. The address can lie in a grown unit.
    uint8_t SoundRegister(uint32_t address) const;

    // Starts the frame's part of the mixer's buffer: the reverb of the buffer's earlier output, or silence. Camelot's
    // output stage writes the whole part later.
    void StartPart();

    // The points in the buffer for each of the mixer's points: 2 for Camelot's mixer, and 1 for the others.
    int PointsPerMixed() const;

    // Moves a DirectSound channel on through its sample for a frame. When the model mixes, adds each point to the
    // frame's part at the channel's volumes on each side.
    // When `meter` names a track, adds the points to that track's sums too.
    void MixChannel(Channel& c, const Wave& wave, int right, int left, int meter);

    // Moves a synth voice of Camelot's mixer on by a frame.
    void MixSynth(Channel& c) const;

    // Works out `count` points of a synth voice of Camelot's mixer at `step`, and moves the voice on by them. Returns
    // true for a saw wave. The mixer plays a saw at half the volumes.
    bool CamelotSynthPoints(Channel& c, uint32_t step, int32_t* points, int count) const;

    // The enhanced mode's part of a frame: its start from the reverb or the echo, a channel's samples at the volumes
    // from `from` to `to` (right then left), and the frame's end in the history.
    void StartEnhancedPart();
    void MixEnhanced(int index, const Channel& c, const Wave& wave, std::array<float, 2> to, bool starting);
    void EndEnhancedPart();

    // Whether Camelot's mixer plays the channel's fixed-pitch sample in 4-bit steps.
    bool PlaysCamelotSteps(const Channel& c) const;

    // The points of such a sample, from its first step, for the enhanced mode.
    const std::vector<float>& CamelotStepPoints(const Wave& wave);

    // Moves a DirectSound channel on through its sample for a frame as Camelot's mixer does, and adds each point to
    // the frame's sums at `volumes`: the right side's in the low half, and the left side's in the high half.
    void MixCamelot(Channel& c, const Wave& wave, uint32_t volumes, int meter);

    // Camelot's output stage: turns the frame's sums into two points on each side for each of them, and starts the
    // next frame's sums with the echo.
    void OutputCamelot();

    // Records the track's levels and pitch for the conversion if they changed, or if it set them for the first time
    // from RestateFrom()'s tick on.
    void ReportTrack(int t);

    // Returns a track's pitch in 256ths of a semitone: its tune, bend and key shift, and the LFO's swing when it
    // modulates the pitch. `metroid` gives the Metroid games' sequencer's routine (0x080048C0 in Metroid Fusion).
    static int32_t TrackPitch(const Track& track, bool metroid);

    void AddAction(Action action);

    // Releases the note that track `t` plays on `channel`.
    void ReleaseNote(int t, int channel);

    // Ends a channel's note. `taken` says that a later note takes the channel.
    void EndNote(int channel, bool sounded, bool taken = false);

    // Records that a channel's note is audible, the first time the level that the hardware plays it at is above 0.
    void CheckAudible(int channel);

    void Warn(const std::string& message);

    const Rom& rom_;
    const DriverInfo& info_;
    int direct_channels_ = 0; // the mixer's DirectSound channels for the song
    bool valid_ = false;
    SongHeader header_;
    std::vector<Track> tracks_;
    std::vector<Channel> channels_ = std::vector<Channel>(kChannelCount);
    bool growing_ = false;
    std::vector<std::array<uint8_t, agb::kGrownPsgBytes>> grown_registers_; // the grown PSG units' registers

    // The music player's state.
    uint32_t status_ = 0;
    uint8_t priority_ = 0;
    uint64_t clock_ = 0;
    uint16_t tempo_d_ = kDefaultTempo;
    uint16_t tempo_u_ = 0x100;
    uint16_t tempo_i_ = kDefaultTempo;
    uint16_t tempo_c_ = 0;
    std::array<uint8_t, kMemAccSize> memory_ = {};

    // The mixer's state: its buffer, the right side's and then the left side's, the frames left before the sound DMA
    // starts the buffer again, the frames that the buffer holds, the reverb and the frame's part of the buffer.
    bool mixing_ = false;
    std::vector<int8_t> pcm_;
    uint8_t dma_counter_ = 0;
    uint8_t dma_period_ = 0;
    uint8_t reverb_ = 0;
    // The tracks' volX at their start: a quarter of the volume that the game's code gives the player.
    uint8_t vol_x_ = 64;
    uint32_t part_ = 0;
    uint16_t muted_tracks_ = 0;
    bool metering_ = false;
    std::vector<int16_t> sums_; // each track's sums for the frame: its right side's points, then its left side's
    std::array<std::array<int32_t, 2>, kMaxTracks> peaks_ = {};
    std::vector<int8_t> owners_ = std::vector<int8_t>(kChannelCount, -1); // ChannelTrack()

    // Camelot's mixer: each point's sums of both sides in one word, the count of the channels that the frame mixed, and
    // the place of the last frame's part.
    std::vector<uint32_t> camelot_sums_;
    uint32_t camelot_channels_ = 0;
    uint32_t camelot_last_part_ = 0;

    // The enhanced mode: each DirectSound channel's position, from the sample's first point and through its loops in
    // 2^-23 points, its volumes of the last frame, and a synth voice's last points of the last frame.
    struct EnhancedChannel
    {
        uint64_t position = 0;
        std::array<float, 2> volumes = {};
        std::array<float, 32> history = {};
    };
    bool enhanced_ = false;
    bool enhanced_frames_ = true;
    std::vector<EnhancedChannel> enhanced_channels_ = std::vector<EnhancedChannel>(kChannelCount);
    std::vector<float> enhanced_out_;     // the frame's output: the right side, then the left
    std::vector<float> enhanced_history_; // the console's buffer of parts, in floats: the right side's, then the left's
    std::vector<float> enhanced_echo_;    // Camelot's echo for the next frame: the right side, then the left
    std::map<uint32_t, std::shared_ptr<const std::vector<float>>> camelot_step_points_; // by the sample's address

    uint8_t c15_ = 0; // the PSG envelopes' frame counter, 14 down to 0
    std::array<uint8_t, agb::kSoundRegisterBytes> sound_registers_ = {};
    std::vector<agb::RegisterWrite> sound_writes_;
    uint32_t frame_ = 0;
    uint64_t restate_from_ = ~uint64_t(0);
    bool record_commands_ = false;
    std::vector<Action> actions_;
    std::vector<std::string> warnings_;
};

// Returns the rate in Hz at which a sample whose header gives `frequency` (1024 times its rate for key 60) plays `key`,
// raised by `fine`/256 of a semitone, as the driver's MidiKeyToFreq works it out.
uint32_t KeyToFrequency(uint32_t frequency, int key, int fine);

// Returns the rate as KeyToFrequency() does, as Camelot's version of MidiKeyToFreq works it out. It shifts the step to
// the next semitone down by the octaves, where the driver shifts both semitones' rates and takes the difference, so the
// two can round differently.
uint32_t CamelotKeyToFrequency(uint32_t frequency, int key, int fine);

// Returns the rate as KeyToFrequency() does, or as CamelotKeyToFrequency() does in Camelot's version of the driver.
uint32_t SampleFrequency(const DriverInfo& info, uint32_t frequency, int key, int fine);

// Returns the frequency setting of PSG channel `channel` (1-4) for `key`, raised by `fine`/256 of a semitone, as the
// driver's MidiKeyToCgbFreq works it out: the 11-bit register value for channels 1-3, or a noise setting for 4.
uint32_t KeyToPsgFrequency(int channel, int key, int fine);

// Returns how far the phase of a synth voice of Camelot's mixer moves on for each point of the mixer's output, at
// `frequency` Hz. The phase moves 8 times as far as a sample's position would. A cycle of the wave therefore lasts 64
// of a sample's points. The step wraps around at 32 bits.
inline uint32_t SynthStep(uint32_t frequency, uint32_t step_scale)
{
    return (frequency * step_scale) << 3;
}

// Returns the point of a synth voice's saw wave at `phase`. The wave rises from -112 to 112 over a cycle. It rises 3
// for every 4 of the phase's 256 steps and jumps 32 halfway.
inline int32_t SawPoint(uint32_t phase)
{
    return int32_t(phase >> 24) - 0x70 - int32_t((phase >> 26) & 0x1F);
}

// Returns the saw wave's filter output for `point` after the output `last`: the point plus half the last output.
inline int32_t FilterSaw(int32_t point, int32_t last)
{
    return point + (last >> 1);
}

// Returns how far the triangle wave that sets a pulse wave's duty moves on each frame: the fourth byte of the synth
// voice's data at `data`, at the top of a word.
inline uint32_t DutyStep(const Rom& rom, uint32_t data)
{
    return uint32_t(rom.U8(data + 3)) << 24;
}

} // namespace ntdlib::mp2k
