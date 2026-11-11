// SPDX-License-Identifier: MIT

// A model of the DS's sound driver: SND, the SDK's library on the ARM7 that plays sequences, and the commands that the
// ARM9 sends it. The driver runs a frame each time its alarm fires, every 2,728 ticks of the ARM7's OS timer, and when
// the ARM9 wakes it. A frame starts the channels that the last frame set up, runs the commands, steps the sequences
// when the alarm woke it, steps the channels' envelopes, sweeps and LFOs, and writes their volumes, pans and timers.
//
// The model holds the driver's state as the driver does, with the same integer types, and reads the sequences, banks
// and wave archives from the main memory where the ARM9 put them. It writes the sound unit's registers in the order
// and with the sizes that the driver uses. Its frames therefore leave the sound unit as the driver's do. The model
// follows the driver of the SDK's builds that the test images hold. Build gives what differs between them.
//
// The model leaves out what only another part of a game uses: the alarms that the ARM9 sets up for streams
// (SND_CMD_SETUP_ALARM, and the alarms of SND_CMD_START_TIMER and SND_CMD_STOP_TIMER) and the copy of the driver's
// state for the ARM9 (SND_CMD_READ_DRIVER_INFO). A sequence's volume, expression, velocity or sustain above 127 reads
// past the driver's table of levels. The model gives the tables that follow it in the driver's binary, and 0 past
// them. A track that loops without waiting hangs the driver and the console. The model throws a Hang instead
// (kCommandBudget).

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/held_notes.h"

namespace ntdlib::ntr::snd
{

constexpr int kChannelCount = 16;

// The most channels of a driver that grows its channels (Driver::GrowChannels). The console has kChannelCount.
constexpr int kMostChannels = 1024;
constexpr int kPlayerCount = 16;
constexpr int kTrackCount = 32;
constexpr int kTracksPerPlayer = 16;
constexpr int kCallDepth = 3;

// The driver's alarm: 2,728 ticks of the OS timer, 64 system clocks each.
constexpr uint32_t kFrameClocks = 2728 * 64;

// The commands, by their numbers in a command's id.
enum class CommandId : uint32_t
{
    kStartSequence = 0x00,
    kStopSequence = 0x01,
    kPrepareSequence = 0x02,
    kStartPreparedSequence = 0x03,
    kPauseSequence = 0x04,
    kSkipSequence = 0x05,
    kPlayerParameter = 0x06,
    kTrackParameter = 0x07,
    kMuteTrack = 0x08,
    kAllocatableChannels = 0x09,
    kPlayerLocalVariable = 0x0A,
    kGlobalVariable = 0x0B,
    kStartTimer = 0x0C,
    kStopTimer = 0x0D,
    kSetUpPcmChannel = 0x0E,
    kSetUpPsgChannel = 0x0F,
    kSetUpNoiseChannel = 0x10,
    kSetUpCapture = 0x11,
    kSetUpAlarm = 0x12,
    kChannelTimer = 0x13,
    kChannelVolume = 0x14,
    kChannelPan = 0x15,
    kSurroundDecay = 0x16,
    kMasterVolume = 0x17,
    kMasterPan = 0x18,
    kOutputSelector = 0x19,
    kLockChannels = 0x1A,
    kUnlockChannels = 0x1B,
    kStopUnlockedChannels = 0x1C,
    kSetSharedWork = 0x1D,
    kInvalidateSequence = 0x1E,
    kInvalidateBank = 0x1F,
    kInvalidateWave = 0x20,
    kReadDriverInfo = 0x21,
};

// A command with its four arguments. The ARM9 writes it in memory after the link to the next command.
struct Command
{
    friend bool operator==(const Command&, const Command&) = default;

    CommandId id = CommandId::kStartSequence;
    std::array<uint32_t, 4> args = {};
};

// The commands that the ARM9 sends at once. The driver runs each list whole, then counts it in the shared work.
using CommandList = std::vector<Command>;

// What differs between the driver's builds. BuildOf reads it from the tables in an ARM7 binary.
struct Build
{
    friend bool operator==(const Build&, const Build&) = default;

    // The level that the table of the levels of the squares of i/127 (SND_CalcDecibelSquare's) gives a volume of 0, in
    // 0.1 dB: -32768 in later builds, -723 in earlier ones. A channel's level of -723 or less is silent.
    int16_t zero_level = -32768;

    // Whether the build holds the table of the levels of i/127 (SND_CalcDecibel's) after the table of their squares. A
    // volume, an expression, a velocity or a sustain above 127 reads past the table of the squares: into the levels of
    // i/127 in a build that holds them, and into the table of the volume's shifts in one that doesn't.
    bool decibel_table = false;
};

// The build for a sequence that comes without a game's driver: the later builds' level of volume 0, without the table
// of the levels of i/127. Most of the drivers known hold these values.
constexpr Build kDefaultBuild{};

// The build of the driver in an ARM7 binary, from its tables. Throws std::runtime_error when the binary doesn't hold
// the driver's tables once.
Build BuildOf(std::span<const uint8_t> arm7);

// The most commands that a track runs in one tick. The driver has no such limit. A track that loops without waiting
// runs for ever there, and the console hangs. The model's frame stops and throws a Hang instead. A sequence that
// doesn't hang runs far fewer commands in a tick.
constexpr int kCommandBudget = 100000;

// What a frame throws when a track runs kCommandBudget commands in a tick: the sequence would hang the console. The
// driver's state is then that of the commands run so far. Restore a snapshot of it, or start again.
class Hang : public std::runtime_error
{
public:
    Hang(int player, int track, uint32_t offset);

    // The player, and the track's number in it.
    int PlayerIndex() const;
    int TrackIndex() const;

    // The offset in the sequence's bytecode of the command that the track would run next.
    uint32_t Offset() const;

private:
    int player_;
    int track_;
    uint32_t offset_;
};

// The size of the shared work (SNDSharedWork). The ARM9 gives the driver the shared work for the players' and channels'
// states and the variables.
constexpr uint32_t kSharedWorkSize = 0x280;

// Sets up the shared work at the start of `work` as the ARM9's SND_Init does (SNDi_InitSharedWork): the statuses and
// tick counters at 0, and every player's variables and the global variables at -1.
void InitSharedWork(std::span<uint8_t> work);

// The sound unit's registers in the ARM7's address space.
class Registers
{
public:
    virtual ~Registers() = default;

    virtual uint32_t Read(uint32_t address, int size) = 0;
    virtual void Write(uint32_t address, int size, uint32_t value) = 0;
};

// An LFO's settings (SNDLfoParam).
struct LfoParameters
{
    friend bool operator==(const LfoParameters&, const LfoParameters&) = default;

    uint8_t target = 0; // 0: pitch, 1: volume, 2: pan
    uint8_t speed = 0;
    uint8_t depth = 0;
    uint8_t range = 0;
    uint16_t delay = 0;
};

struct Lfo
{
    LfoParameters parameters;
    uint16_t delay_counter = 0;
    uint16_t counter = 0;
};

// A wave's settings (SNDWaveParam). A wave of a wave archive starts with them.
struct WaveParameters
{
    uint8_t format = 0;
    uint8_t loop = 0;
    uint16_t rate = 0;
    uint16_t timer = 0;
    uint16_t loop_start = 0;
    uint32_t loop_length = 0;
};

// A channel (SNDExChannel).
struct Channel
{
    uint16_t id = 0;
    uint16_t twin = 0; // the console's channel whose part it plays: itself, or another for a grown one (GrowChannels)
    uint8_t type = 0;  // 0: PCM, 1: PSG, 2: noise
    uint8_t envelope_stage = 0; // 0: attack, 1: decay, 2: sustain, 3: release
    bool active = false;
    bool start = false;
    bool auto_sweep = false;
    uint8_t sync = 0; // what the next frame writes: 1 start, 2 stop, 4 timer, 8 volume, 16 pan
    uint8_t pan_range = 0;
    uint8_t root_key = 0;
    int16_t user_decay2 = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    int8_t initial_pan = 0;
    int8_t user_pan = 0;
    int16_t user_decay = 0;
    int16_t user_pitch = 0;
    int32_t attenuation = 0;
    int32_t sweep_counter = 0;
    int32_t sweep_length = 0;
    uint8_t attack = 0;
    uint8_t sustain = 0;
    uint16_t decay = 0;
    uint16_t release = 0;
    uint8_t priority = 0;
    uint8_t pan = 0;
    uint16_t volume = 0; // SOUNDxCNT's volume in the low byte, the divider in the high byte
    uint16_t timer = 0;
    Lfo lfo;
    int16_t sweep_pitch = 0;
    int32_t length = 0;
    WaveParameters wave;
    uint32_t data = 0; // the wave's samples' address, or a PSG channel's duty
    int owner = -1;    // the track whose list holds the channel, or -1 for none
    int next = -1;     // the next channel of the owner's list
};

// A track (SNDTrack).
struct Track
{
    bool active = false;
    bool note_wait = false;
    bool muted = false;
    bool tie = false;
    bool note_finish_wait = false;
    bool portamento = false;
    bool compare = false;
    bool channel_mask_set = false;
    uint8_t pan_range = 0;
    uint16_t program = 0;
    uint8_t volume = 0;
    uint8_t expression = 0;
    int8_t pitch_bend = 0;
    uint8_t bend_range = 0;
    int8_t pan = 0;
    int8_t external_pan = 0;
    int16_t external_fader = 0;
    int16_t external_pitch = 0;
    uint8_t attack = 0;
    uint8_t decay = 0;
    uint8_t sustain = 0;
    uint8_t release = 0;
    uint8_t priority = 0;
    int8_t transpose = 0;
    uint8_t portamento_key = 0;
    uint8_t portamento_time = 0;
    int16_t sweep_pitch = 0;
    LfoParameters modulation;
    uint16_t channel_mask = 0;
    int32_t wait = 0;
    uint32_t base = 0;
    uint32_t position = 0; // 0 until a 0x93 opens the track
    std::array<uint32_t, kCallDepth> calls = {};
    std::array<uint8_t, kCallDepth> loop_counts = {};
    uint8_t call_depth = 0;
    int channels = -1; // the first channel of the track's list
};

// A player (SNDPlayer).
struct Player
{
    bool active = false;
    bool prepared = false;
    bool paused = false;
    uint8_t id = 0;
    uint8_t priority = 0;
    uint8_t volume = 0;
    int16_t external_fader = 0;
    std::array<uint8_t, kTracksPerPlayer> tracks = {}; // indices of the driver's tracks, 0xFF for none
    uint16_t tempo = 0;
    uint16_t tempo_ratio = 0;
    uint16_t tempo_counter = 0;
    uint32_t bank = 0;
};

// A note that a track's command plays (Driver::on_note).
struct NoteEvent
{
    friend bool operator==(const NoteEvent&, const NoteEvent&) = default;

    int player = 0;      // the player's number
    int track = 0;       // the track's number in the player, 0 to 15
    uint32_t offset = 0; // the note command's offset from the start of the sequence's data, its prefixes included
    uint8_t key = 0;     // after the transpose
    uint8_t velocity = 0;
    int32_t length = 0; // in ticks, or -1 for a note that lasts until its track releases it
    int channel = -1;   // the channel that plays the note, or -1 when the note gets none
    bool tie = false;   // whether the note goes on in its track's channel (a tie)
};

// A loop of a track of a player: the offset from the start of the sequence's data that the loop's end (0xFC) goes back
// to.
struct LoopPlace
{
    friend auto operator<=>(const LoopPlace&, const LoopPlace&) = default;

    int player = 0;
    int track = 0; // the track's number in the player
    uint32_t offset = 0;
};

// The hooks of a rip tool's length analysis (ntdlib/ntr/length_model.h), with the player's number and the track's
// number in the player. The targets are offsets from the start of the sequence's data. The driver has no such hooks.
struct LengthHooks
{
    // A loop of the sequence: a pass that changed the sound, of track 0, of the first track that plays notes, or of a
    // track that holds a note on its own (see Driver::FollowLengths).
    std::function<void(int player, int track, uint32_t target)> on_loop = {};

    // Every pass of every track.
    std::function<void(int player, int track, uint32_t target)> on_track_loop = {};

    // A track opens another track (0x93). The open keeps the opened track's wait.
    std::function<void(int player, int track, int opened)> on_open_track = {};

    // A track starts a command.
    std::function<void(int player, int track)> on_command_start = {};

    // A track's command takes a random number: the value of a random prefix (0xA0) or of a random variable (0xB6).
    std::function<void(int player, int track)> on_draw = {};
};

class Driver
{
public:
    // A driver whose ARM7 reads and writes `main_memory`: the memory at 0x02000000. The memory repeats every
    // `main_memory.size()` bytes up to 0x02FFFFFF. The memory and the registers must outlive the driver. A copy of the
    // driver goes on from the same state, with the same memory and registers.
    Driver(std::span<uint8_t> main_memory, Registers& registers, Build build = {});

    // The driver's thread's start: it clears the channels, players and tracks, turns the sound unit's output on and
    // sets the master volume to 127.
    void Start();

    // Sets the state of the driver's random numbers. The driver starts with 0x12345678. Each random number steps the
    // state by a linear congruential generator.
    void SetRandomState(uint32_t state);

    // A frame: the alarm's when `step` is set, otherwise the frame that a wake-up from the ARM9 runs. The frame runs
    // `lists` in order. Throws a Hang when a track runs kCommandBudget commands in a tick.
    void Frame(bool step, std::span<const CommandList> lists);

    const Channel& GetChannel(int channel) const;

    // For the enhanced mixing mode: when `on`, a note that would take a channel whose note plays and hasn't reached its
    // release, or would get no channel for its priority, takes another channel instead. That's a channel past the
    // console's 16 that plays no more, or a new one, up to kMostChannels. A note in its release gives way to a new
    // note, as on the console (the maintainer, 2026-11-02). The new channel takes the part of the channel that the
    // console's driver chose: a wave channel's, a PSG channel's or a noise channel's. It's the chosen channel's twin.
    // Its registers lie past the console's (ntr::kGrownChannelRegisters). Only sequences' notes take such channels. The
    // ARM9's commands reach the console's 16. When it's off, the driver shares out the console's 16 as the game's
    // driver does. A start of the driver leaves the console's 16.
    void GrowChannels(bool on);

    // The channels: the console's 16, then those that have grown.
    int ChannelCount() const
    {
        return static_cast<int>(channels_.size());
    }
    const Track& GetTrack(int track) const;
    const Player& GetPlayer(int player) const;

    // Called with each note that a track plays, once the note has its channel and settings. A track that the driver
    // mutes plays no notes, and neither does a skip (SND_CMD_SKIP_SEQ). The driver has no such call.
    std::function<void(const NoteEvent&)> on_note;

    // Called at the start of each tick of a player, before its tracks run. The driver has no such call.
    std::function<void(int player)> on_tick;

    // For a rip tool's length analysis: while `hooks.on_loop` is set, the driver follows each command's place in the
    // order that a player's tracks run them, and whether the command changes the track's sound. It follows the rules of
    // nwsnd's sequence player (nwsnd/sequence.h, SequenceTrack::CountLoop) with the defaults of nwsnd::LengthRules. The
    // 3DS takes the same rules. A jump back to a command that the track has run is a loop, outside a subroutine. So is
    // each pass of a loop whose count is 0 (0xD4 and 0xFC). The counted loops in `endless_loops` loop for ever, and
    // their passes count too. A loop of the sequence is a pass that changed the sound, of track 0, of a track that
    // plays notes after tracks that pass idly or wait for ever, or of a track that holds a note while every other track
    // rests. The driver has no such following. A start of a sequence clears what the driver has followed of it.
    void FollowLengths(LengthHooks hooks, std::set<LoopPlace> endless_loops = {});

    // The analysis's tests of player `player`. They hold while the driver follows lengths. A player is settled when
    // every open track waits for its notes, waits for ever, or passes still with no note that ends by itself. It holds
    // when every open track waits for ever, or passes still with no note that ends by itself. It's stuck when every
    // open track waits for ever, or passes still and quiet. A tempo of 0 settles it, holds it and sticks it. So does a
    // player with no open track. ChangedAt gives when a track last changed the sound, counted in the commands that the
    // player's tracks have run.
    bool IsSettled(int player) const;
    bool IsHolding(int player) const;
    bool IsStuck(int player) const;
    uint64_t ChangedAt(int player) const;

    // The tests of track `track` of player `player`: whether it's open (its sequence hasn't ended it), whether its
    // last pass changed nothing and nothing has changed since, and whether it will never read on: it waits for a
    // negative number of ticks, waits out a held note (kHeldTicks), or waits for its notes to end while one of them
    // never will.
    bool IsTrackOpen(int player, int track) const;
    bool IsTrackIdle(int player, int track) const;
    bool WaitsForEver(int player, int track) const;

    // For each open track of player `player` whose outermost loop has a count: that loop. A measure at the analysis's
    // limit makes them endless.
    std::set<LoopPlace> OutermostCountedLoops(int player) const;

    // Whether a channel's envelope moves (any stage but the sustain), and the channels that play.
    bool AnyChannelChanging() const;
    int ActiveChannels() const;

    // A held note, for the length analysis: one of this many ticks or more. That's 200 beats, and minutes at any usual
    // tempo. A sound that plays until the game stops it gives a note this length.
    static constexpr int32_t kHeldTicks = ntdlib::kHeldTicks;

private:
    // What the length analysis follows of a track and of a player (FollowLengths).
    struct TrackFollow
    {
        std::unordered_map<uint32_t, uint64_t> run; // when the track last ran the command at each offset
        std::array<bool, kCallDepth> loops = {};    // whether each entry of the call stack is a loop's
        std::array<int16_t, 32> loop_variables = {};
        uint64_t changed_at = 0;
        uint64_t noted_at = 0;
        uint64_t drew_at = 0;
        uint64_t looped_at = 0;
        uint64_t conditional_at = 0;
        uint64_t set_at = 0; // when the track last changed a variable's value
        // When the track last ended an idle pass of a poll after its last loop, or 0 (CountPoll).
        uint64_t polled_at = 0;
        bool idle = false;
        bool still = false;
        bool quiet = false;
        bool holds = false;
        bool held_wait = false;
    };

    struct PlayerFollow
    {
        uint64_t commands_run = 0;
        uint64_t changed_at = 0;
        uint32_t variables_read = 0; // the variables that a command has read: the player's 0 to 15, the global ones
                                     // 16 to 31
    };

    // The track's settings that its notes sound with.
    struct SoundState
    {
        friend bool operator==(const SoundState&, const SoundState&) = default;

        uint16_t program = 0;
        int8_t transpose = 0;
        uint8_t bend_range = 0;
        uint8_t priority = 0;
        uint8_t portamento_key = 0;
        uint8_t portamento_time = 0;
        std::array<uint8_t, 4> envelope = {};
        int16_t sweep_pitch = 0;
        LfoParameters modulation;
        bool muted = false;
        bool tie = false;
        bool portamento = false;
        bool note_wait = false;
        uint8_t volume = 0;
        uint8_t expression = 0;
        int8_t pitch_bend = 0;
        int8_t pan = 0;
        uint8_t player_volume = 0;
        uint16_t tempo = 0;
        int channels = -1;
    };

    SoundState StateOf(const Track& track, const Player& player) const;
    int FollowCommand(int track, int number, Player& player, bool play_notes);
    void MarkChange(int track, const Player& player);
    void MarkRead(const Player& player, int variable);
    void CountLoop(int track, int number, const Player& player, uint32_t target);
    void CountPoll(int track, const Player& player, uint32_t target, uint32_t next);
    bool FollowsJumpAsLoop(int track, uint32_t target, uint32_t next, bool conditional) const;
    bool LoopsForEver(const Player& player, int number, uint32_t offset) const;
    const Track* TrackOf(int player, int number) const;
    bool IsIdle(int track) const;
    bool IsStill(int track) const;
    bool WaitsForEver(int track) const;
    bool WaitsForNotes(int track) const;
    bool HasEndingNote(int track) const;
    bool HoldsSustainedNote(int track) const;
    bool ChannelWaveLoops(const Channel& channel) const;
    bool IsMainTrack(const Player& player, int number) const;
    bool OthersRest(const Player& player, int number) const;
    template <typename Test>
    bool EveryOpenTrack(int player, const Test& test) const;

    // Main memory.
    uint8_t Read8(uint32_t address) const;
    uint16_t Read16(uint32_t address) const;
    uint32_t Read32(uint32_t address) const;
    void Write8(uint32_t address, uint8_t value);
    void Write16(uint32_t address, uint16_t value);
    void Write32(uint32_t address, uint32_t value);

    // The driver's tables, read as its code reads them, past their ends too.
    int16_t DecibelSquare(int index) const;
    uint8_t FastAttack(int index) const;

    // The sound unit (SND_channel).
    void SetUpPcmChannel(int channel, uint32_t data, int format, int loop, int loop_start, int loop_length, int volume,
                         int divider, int timer, int pan);
    void SetUpPsgChannel(int channel, int duty, int volume, int divider, int timer, int pan);
    void SetUpNoiseChannel(int channel, int volume, int divider, int timer, int pan);
    void StopChannel(int channel, int hold);
    void SetChannelVolume(int channel, int volume, int divider);
    void SetChannelTimer(int channel, int timer);
    void SetChannelPan(int channel, int pan);
    bool IsChannelActive(int channel);
    void SetMasterPan(int pan);
    void SetSurroundDecay(int decay);
    int SurroundDecay(int volume, int pan) const;
    void SetOutputSelector(int left, int right, int channel1, int channel3);
    void SetUpCapture(int capture, int format, uint32_t address, int size, bool loop, int source, int add);
    bool IsCaptureActive(int capture);

    // The channels (SND_exChannel).
    void UpdateChannels();
    void StepChannels(bool step);
    int32_t StepEnvelope(Channel& channel, bool step);
    int32_t StepSweep(Channel& channel, bool step);
    int32_t StepLfo(Channel& channel, bool step);
    void SetAttack(Channel& channel, int attack);
    void SetDecay(Channel& channel, int decay);
    void SetRelease(Channel& channel, int release);
    void StartChannel(Channel& channel, int32_t length);
    int Allocate(uint32_t mask, int priority, bool weak_locks_too, int owner);
    int GrownChannel(int chosen);
    bool Surrounded(int channel) const;
    void Free(Channel& channel);
    void Callback(Channel& channel, int status);
    void StopUnlockedChannels(uint32_t mask);
    void LockChannels(uint32_t mask, uint32_t weak);
    void InvalidateWave(uint32_t start, uint32_t end);

    // The banks (SND_bank).
    struct Instrument;
    bool ReadInstrument(uint32_t bank, int program, int key, Instrument& instrument) const;
    uint32_t WaveAddress(uint32_t bank, int wave_archive, int wave) const;
    bool NoteOn(Channel& channel, int key, int velocity, int32_t length, uint32_t bank, const Instrument& instrument);

    // The sequences (SND_seq).
    void StepPlayers(bool step);
    void PrepareSequence(int player, uint32_t sequence, uint32_t offset, uint32_t bank);
    void StopSequence(int player);
    void PauseSequence(int player, uint32_t pause);
    void SkipSequence(int player, uint32_t ticks);
    void SetTrackMute(int player, uint32_t tracks, int mode);
    void SetAllocatableChannels(int player, uint32_t tracks, uint32_t channels);
    void InvalidateSequence(uint32_t start, uint32_t end);
    void InvalidateBank(uint32_t start, uint32_t end);
    void SetPlayerParameter(int player, uint32_t offset, uint32_t value, int size);
    void SetTrackParameter(int player, uint32_t tracks, uint32_t offset, uint32_t value, int size);
    void InitPlayer(Player& player, uint32_t bank);
    void InitTrack(Track& track);
    int AllocateTrack();
    Track* PlayerTrack(Player& player, int index);
    void StepPlayer(Player& player);
    bool StepTicks(Player& player, bool play_notes);
    int StepTrack(int track, int number, Player& player, bool play_notes);
    int RunCommand(int track, int number, Player& player, bool play_notes);
    // Returns the channel that plays the note, or -1 for none.
    int PlayNote(int track, Player& player, int key, int velocity, int32_t length);
    void UpdateTrackChannels(Track& track, const Player& player, bool release);
    void ReleaseTrackChannels(Track& track, const Player& player, int release);
    void FreeTrackChannels(Track& track);
    void StopTrack(Player& player, int index);
    void StopPlayer(Player& player);
    void MuteTrack(Track& track, const Player& player, int mode);
    uint8_t ReadByte(Track& track);
    uint16_t ReadU16(Track& track);

    // The value of a command's argument: the value itself, a variable's or a random one.
    int32_t Resolve(const commands::Argument& argument, const Player& player);
    uint32_t VariableAddress(const Player& player, int variable) const;

    // The commands, and the shared work (SND_command, SND_work).
    void Run(const Command& command);
    void StartTimer(uint32_t channels, uint32_t captures);
    void StopTimer(uint32_t channels, uint32_t captures, int hold);
    void UpdateSharedWork();
    uint16_t Random();

    std::span<uint8_t> main_memory_;
    Registers* registers_;
    Build build_;
    std::vector<uint8_t> tables_; // the driver's tables from kDecibelSquare to kFastAttack, in its binary's layout
    std::size_t shifts_ = 0;      // kVolumeShift's offset in tables_
    std::size_t fast_attack_ = 0; // kFastAttack's offset in tables_

    std::vector<Channel> channels_ = std::vector<Channel>(kChannelCount);
    bool growing_ = false;
    std::array<Player, kPlayerCount> players_ = {};
    std::array<Track, kTrackCount> tracks_ = {};
    uint32_t locked_ = 0;
    uint32_t weakly_locked_ = 0;
    int master_pan_ = -1;
    std::vector<uint8_t> original_volumes_ = std::vector<uint8_t>(kChannelCount);
    std::vector<uint8_t> original_pans_ = std::vector<uint8_t>(kChannelCount);
    int surround_decay_ = 0;
    uint32_t shared_work_ = 0;
    uint32_t random_ = 0x12345678;

    bool following_ = false;
    LengthHooks hooks_;
    std::set<LoopPlace> endless_loops_;
    std::array<TrackFollow, kTrackCount> track_follows_ = {};
    std::array<PlayerFollow, kPlayerCount> player_follows_ = {};
};

} // namespace ntdlib::ntr::snd
