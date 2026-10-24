// SPDX-License-Identifier: MIT

// Sequence playback in nw::snd, the sound library of NintendoWare for Cafe (driver::SequenceTrack with its MmlParser,
// and driver::SequenceSoundPlayer), and in nn::atk, the same code on the Switch. The 3DS's nw::snd and the Wii's NW4R
// SND have older versions of the same code (see console.h). The model is a template on a policy. The policy gives the
// console's steps (see console.h), and the types that the model's user plays the notes with: the channels, the engine
// that makes them, the note's settings, the sound's info and the outputs. The comment on a method that follows one of
// the game's routines gives the routine's address in Animal Crossing: amiibo Festival's executable. A field comment
// gives the field's offset in the game's object.
//
// The engine goes into each call that requires it. The player and its tracks then hold no pointers, and a copy of them
// plays on apart from the original. An engine can report the sequence's loops to a rip tool's length analysis. Such an
// engine gives FollowsLoops(), whether the analysis follows the loops now, and OnLoop(player, track, target) (see
// SequenceSoundPlayer's comment on loops). The model keeps the analysis's records of the commands only while
// FollowsLoops() is true. Each pass of a loop updates a track's IsIdle, IsStill and IsQuiet from those records either
// way.
// The engine may also give LoopsForEver(player, track, address). While that returns true, the counted loop whose body
// starts at `address` loops for ever like a loop of count 0. The loop's passes count as loops. The game's counted loops
// end. An analysis can take one as endless when the game plays it for longer than a rip lasts.
// The engine may also give OnTrackLoop(player, track, target). While FollowsLoops() is true, it gets every track's
// passes of a loop, idle ones included. The track's IsIdle() then says whether the pass changed the sound.
//
// An engine can also follow the sequence as it plays. The model then calls these, in the game's order, if the engine
// has them. They report what happened. Only OnParseLimit and StopsTick also change what the model does.
//
//     OnTickStart(player, units_left)     a tick starts. `units_left` are the frame's units after the tick's time
//                                         (Policy::kFrameUnits for a tick at the frame's start).
//     OnTickEnd(player)                   every track has run the tick
//     OnCommandStart(player, track, offset, command, conditional)
//                                         the track starts the command at `offset`. `command` is the command after
//                                         its prefixes, and `conditional` whether an if prefix came first. The
//                                         command's arguments follow.
//     OnCommand(player, track, command, arg1, arg2)
//                                         the track has run a command (CommandProc), not the extended ones
//     OnExtendedCommand(player, track, command, arg1, arg2)
//                                         the track has run an extended command (0xF000 plus its second byte)
//     OnRandomDraw(player, track)         an argument or the random command (0x86) has drawn a random number
//     OnVariableRead(player, track, index)
//                                         an argument has read variable `index`
//     OnNoteCommand(player, track, key, velocity, length)
//                                         a note command runs: the key after the transpose, and the length that the
//                                         command gives. The track's mute and tie apply after the call.
//     StopsTick(player, track)            the track's tick stops before its next command if the call returns true.
//                                         The track reads on from that command at its next tick, or on a copy of the
//                                         player through ContinueTrackTick. The game has no such stop.
//     OnParseLimit(player, track)         the track ran Policy::kParseLimit commands without a wait. It stops if the
//                                         call returns true. Without the call, it resumes in the next tick, as in the
//                                         game. A policy whose limit ends the track (kParseLimitEndsTrack) doesn't call
//                                         it.
//     OnParseLimitEnd(player, track)      the track ran Policy::kParseLimit commands without a wait, on a policy whose
//                                         limit ends the track (kParseLimitEndsTrack). The track ends after the call.
//     OnNoteStart(player, track)          a note command, before the track ties, holds or starts a channel
//     OnNoteOn(player, track, channel)    the note started `channel`, with its settings in place
//     OnLegato(player, track, channel)    a tie or a monophonic note continued `channel` at a new key and velocity
//     OnRelease(player, track, channel)   Release released the channel, or NoteOff ran on it
//     OnStop(player, track, channel)      the channel stopped at once, before it's freed
//     OnReleaseAll(player, track)         ReleaseAllChannel starts: the track's parameters, then each release
//     OnDetachAll(player, track)          FreeAllChannel starts: the track's channels lose their owner
//     OnStopAll(player, track)            a mute that stops the track's channels at once starts
//     OnTrackClosing(player, track)       the track starts to close
//     OnTrackClosed(player, track)        the track has closed
//     OnLengthUpdated(player, track, channel)
//                                         the tick counted the channel's length down
//     OnLengthsCounted(player, track)     the tick has counted down the lengths of all the track's channels

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/nwsnd/channel.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/envelope.h"

namespace ntdlib::nwsnd
{

template <typename Policy>
class SequenceSoundPlayer;

// A player's tracks: 0 to 15.
inline constexpr int kTrackCount = 16;

// MoveValue<T, s16>: a linear ramp to a target over a number of sequence ticks.
template <typename T>
struct MoveValue
{
    void Init(T value)
    {
        start = value;
        target = value;
        duration = 0;
        counter = 0;
    }

    void Update()
    {
        if (counter < duration)
        {
            ++counter;
        }
    }

    T Get() const
    {
        if (counter >= duration)
        {
            return target;
        }

        return static_cast<T>(static_cast<int>(start) +
                              (static_cast<int>(target) - static_cast<int>(start)) * counter / duration);
    }

    void SetTarget(T value, int16_t ticks)
    {
        start = Get();
        duration = ticks;
        counter = 0;
        target = value;
    }

    bool Moving() const
    {
        return counter < duration;
    }

    T start{};
    T target{};
    int16_t duration = 0;
    int16_t counter = 0;
};

// One of a player's 16 tracks (driver::SequenceTrack, with MmlParser). A track runs its part of the sequence's
// bytecode, keeps the settings that the commands give, and plays its notes on channels (see channel.h).
template <typename Policy>
class SequenceTrack
{
public:
    using Channel = typename Policy::Channel;
    using Engine = typename Policy::Engine;
    using Player = SequenceSoundPlayer<Policy>;

    // A call or a loop (8 bytes at +0x10C): where it returns to, and the loop's count.
    struct CallStackEntry
    {
        bool is_loop = false;
        uint8_t loop_count = 0;
        uint32_t address = 0;
    };

    // Track `index`, 0 to 15, of a player. The player passes itself and the engine to the calls that require them.
    explicit SequenceTrack(int index);

    void SetSeqData(std::span<const uint8_t> data, uint32_t offset); // 0x022B69A8
    void Open();                                                     // 0x022B69B8
    void Close(Player& player, Engine& engine);                      // 0x022B6158

    bool IsOpen() const
    {
        return open_;
    }

    // The track's state, for an engine that reads it: the offset of the next command, the ticks that the track waits
    // before it reads on, whether it waits for its notes to end, and the calls and loops that it's in, the innermost
    // last.
    uint32_t Current() const
    {
        return current_;
    }

    int32_t Wait() const
    {
        return wait_;
    }

    bool NoteFinishWait() const
    {
        return note_finish_wait_;
    }

    // The offset after the arguments of the command that the track runs in CommandProc, or ran last. A jump or a call
    // has moved the track on from there by the time OnCommand reports it.
    uint32_t CommandEnd() const
    {
        return command_end_;
    }

    std::span<const CallStackEntry> CallStack() const
    {
        return {call_stack_.data(), call_depth_};
    }

    // The settings that the track's commands give: tie and mono, the instrument, the volumes, the pan and the pitch
    // bend with their ramps, the bend's range in semitones, the initial pan, the LFOs with their types, the envelope
    // (0xFF keeps the instrument's value), the sends, and the filters.
    bool Tie() const
    {
        return tie_;
    }

    bool Mono() const
    {
        return mono_;
    }

    uint8_t BankIndex() const
    {
        return bank_index_;
    }

    uint32_t ProgramNumber() const
    {
        return prg_no_;
    }

    const MoveValue<uint8_t>& Volume() const
    {
        return vol_;
    }

    const MoveValue<uint8_t>& Volume2() const
    {
        return volume2_;
    }

    const MoveValue<int8_t>& Pan() const
    {
        return pan_;
    }

    const MoveValue<int8_t>& Bend() const
    {
        return bend_;
    }

    uint8_t BendRange() const
    {
        return bend_range_;
    }

    int8_t InitPan() const
    {
        return init_pan_;
    }

    const std::array<CurveLfoParam, 4>& LfoParams() const
    {
        return lfo_params_;
    }

    const std::array<uint8_t, 4>& LfoTypes() const
    {
        return lfo_types_;
    }

    uint8_t Attack() const
    {
        return attack_;
    }

    uint8_t Decay() const
    {
        return decay_;
    }

    uint8_t Sustain() const
    {
        return sustain_;
    }

    uint8_t Release() const
    {
        return release_;
    }

    int16_t Hold() const
    {
        return hold_;
    }

    uint8_t MainSend() const
    {
        return main_send_;
    }

    const std::array<uint8_t, 3>& FxSends() const
    {
        return fx_send_;
    }

    // The volume that the game sets through the sound's handle (the player's SetTrackVolume). It starts at 1.
    float HandleVolume() const
    {
        return volume_;
    }

    float Lpf() const
    {
        return lpf_;
    }

    int8_t BiquadType() const
    {
        return biquad_type_;
    }

    float BiquadValue() const
    {
        return biquad_value_;
    }

    uint8_t VelocityRange() const
    {
        return velocity_range_;
    }

    int8_t Transpose() const
    {
        return transpose_;
    }

    bool Damper() const
    {
        return damper_;
    }

    bool IsMuted() const
    {
        return mute_;
    }

    // Whether a note makes the track wait for its length.
    bool NoteWait() const
    {
        return note_wait_;
    }

    // The track's variables, numbered 32 to 47 in the commands.
    const std::array<int16_t, 16>& Variables() const
    {
        return vars_;
    }

    // Whether the track's last note is a held one (see kHeldTicks), and whether the track waits it out.
    bool Holds() const
    {
        return holds_;
    }

    bool HeldWait() const
    {
        return held_wait_;
    }

    // The place of the newest channel that the track plays, or kNoChannel. Each channel gives the next one's place in
    // next_in_track.
    ChannelId ChannelList() const
    {
        return channel_list_;
    }

    // The channels that the track plays, the newest first.
    std::vector<const Channel*> Channels(const Engine& engine) const;

    // Whether the track won't read on while its notes sound. That's when the track waits for its notes to end (after a
    // note of length 0 with note wait on) and some still sound, or when the track waits for ever. A track whose notes
    // have all ended reads on at its next tick.
    bool WaitsForNotes(const Engine& engine) const
    {
        return (note_finish_wait_ && channel_list_ != kNoChannel) || WaitsForEver(engine);
    }

    // Whether the track will never read on, or not before the game stops the sound. The track waits for a negative
    // number of ticks, waits out a held note (see kHeldTicks), or waits for its notes to end while one never will. A
    // note or wait whose length comes from a variable left at its default of -1 makes a negative wait.
    bool WaitsForEver(const Engine& engine) const
    {
        return wait_ < 0 || held_wait_ || WaitsOnEndlessNote(engine);
    }

    // Whether the track waits for its notes to end while one of them has no length and plays a looping wave. That note
    // ends only when the game stops it.
    bool WaitsOnEndlessNote(const Engine& engine) const;

    // Whether the track's last pass of a loop changed nothing in the sound, and nothing has changed since (for a rip
    // tool's length analysis while the engine follows loops). A track that polls or counts a variable once a tick makes
    // such idle passes. See CountLoop.
    bool IsIdle() const
    {
        return idle_ && changed_at_ < looped_at_;
    }

    // Whether the track is idle, its last pass left the variables that tracks read as the pass before did, and it has
    // drawn no random number since that pass began. Such a track does the same on every pass until the game changes a
    // variable.
    bool IsStill() const
    {
        return IsIdle() && still_ && drew_at_ < looped_at_;
    }

    // Whether the track's last pass of a loop played no note. A track after track 0 whose passes play no notes doesn't
    // keep a later track from being the main track (see SequenceSoundPlayer::IsMainTrack). A still track whose passes
    // play notes can change the sound again. Once the track's notes end, its next pass starts a new note.
    bool IsQuiet() const
    {
        return quiet_;
    }

    // Whether a note of the track will end when its length runs out. A held note (see kHeldTicks) doesn't count. The
    // game stops it first. Under the policy's length rules (damper_holds_notes), nor does a note while the damper is
    // on.
    bool HasEndingNote(const Engine& engine) const;

    // Whether the track's volume, volume2, pan, surround pan or pitch bend is moving to a new value.
    bool IsMoving() const
    {
        return vol_.Moving() || volume2_.Moving() || pan_.Moving() || span_.Moving() || bend_.Moving();
    }

    // A tick: the ramps advance and the commands up to the next wait run. Returns -1 when the track reaches its end, 1
    // otherwise, or 0 if it's closed (0x022B6F14).
    int ParseNextTick(Player& player, Engine& engine);

    // The second half of a tick: the commands up to the next wait. Returns as ParseNextTick does, apart from a closed
    // track.
    int ParseCommands(Player& player, Engine& engine);

    void UpdateChannelLength(Player& player, Engine& engine); // 0x022B6E88
    void UpdateChannelParam(Player& player, Engine& engine);  // 0x022B55B4

private:
    friend class SequenceSoundPlayer<Policy>;

    // The settings of an LFO that commands change (see SetLfo).
    enum class LfoSetting
    {
        kCurve,
        kPhase,
        kDepth,
        kSpeed,
        kType,
        kRange,
        kDelay,
        kPeriod,
    };

    // What a command can change in the track's sound, for a rip tool's length analysis: the track's settings, its
    // ramps' places and targets, its newest channel, and the player's tempo and main volume.
    struct SoundState
    {
        bool operator==(const SoundState&) const = default;

        uint32_t prg_no;
        uint8_t bank_index;
        int8_t transpose;
        uint8_t velocity_range, bend_range, priority, porta_key, porta_time, main_send;
        int8_t init_pan, biquad_type;
        std::array<uint8_t, 4> envelope;
        int16_t hold;
        std::array<uint8_t, 3> fx_send;
        float lpf, biquad_value, sweep_pitch;
        int32_t output_line;
        std::array<CurveLfoParam, 4> lfo_params;
        std::array<uint8_t, 4> lfo_types;
        bool mute, silence, tie, mono, porta, damper, front_bypass, note_wait;
        std::array<int32_t, 18> ramps; // the value, target and ticks to go of the track's five ramps and main volume
        ChannelId newest;
        uint8_t timebase;
        uint16_t tempo;
    };

    static constexpr int kCallStackDepth = Policy::kCallStackDepth;

    // A held note, for a rip tool's length analysis: one of this many ticks or more. That's 200 beats at the default
    // timebase, and minutes at any usual tempo. A sound that plays until the game stops it gives a note this length.
    // The game then stops the sound before the note ends. Music's notes are far shorter.
    static constexpr int32_t kHeldTicks = 9600;

    SoundState State(const Player& player) const;

    // Calls Parse. While the engine follows loops, also records what a rip tool's length analysis follows: each
    // command's place in the order the player's tracks run them, and whether the command changes the track's sound.
    int RunCommand(Player& player, Engine& engine);

    void InitParam(); // 0x022CABAC

    // Runs the command at the track's offset (MmlParser::Parse, 0x022CB7C8). The decoder's ReadPlayedCommand
    // (commands.h) reads it as the game's parser does.
    int Parse(Player& player, Engine& engine);

    // Parse's extended commands (0xF000 plus the byte after 0xF0). `condition` is false after an if (0xA2) whose
    // comparison failed.
    void RunExtended(Player& player, Engine& engine, const PlayedCommand& played, bool condition);

    // The value of a command's argument as the game's parser reads it (MmlParser::ReadArg, 0x022CB67C): the value
    // itself, a value that the player's generator draws, or the value of a variable. Parse takes the values in the
    // order of the command's bytes.
    int32_t ArgumentValue(Player& player, Engine& engine, const commands::Argument& argument);
    void CommandProc(Player& player, Engine& engine, uint32_t command, int32_t arg1,
                     int32_t arg2); // MmlParser::CommandProc, 0x022CBF1C

    // CommandProc's extended commands (0xF000 plus the second byte). VariableCommand runs those on a variable: the
    // arithmetic and the comparisons.
    void ExtendedCommandProc(Player& player, Engine& engine, uint32_t command, int32_t arg1, int32_t arg2);
    void VariableCommand(Player& player, Engine& engine, uint32_t op, int16_t& variable, int32_t value);

    // CommandProc's commands that move through the data and the call stack: 0x88 opens track `index` at `offset`, 0x89
    // jumps to `target`, 0x8A calls the subroutine at `target`, 0xD4 starts a loop of `count` passes (0 for ever), 0xFC
    // ends a pass of the loop on top of the call stack, and 0xFD returns from a call. A full call stack ignores a call
    // and a loop's start.
    void OpenTrack(Player& player, Engine& engine, int32_t index, uint32_t offset);
    void Jump(Player& player, Engine& engine, uint32_t target);
    void Call(uint32_t target);
    void LoopStart(uint8_t count);
    void LoopEnd(Player& player, Engine& engine);
    void Return();

    // A setting of LFO `lfo` from a command's argument. LFO 0 has main commands. LFOs 1 to 3 have extended ones.
    void SetLfo(int lfo, LfoSetting setting, int32_t arg);

    void ReleaseAllChannel(Player& player, Engine& engine, int release); // 0x022B608C
    void FreeAllChannel(Player& player, Engine& engine);                 // 0x022B6114
    void StopAllChannel(Player& player, Engine& engine);                 // 0x022CAAA4
    void SetMute(Player& player, Engine& engine, int mode);              // 0x022CB34C
    void OnChannelEvent(Engine& engine, Channel& channel);               // ChannelCallbackFunc, 0x022CB06C

    // A channel of the track stopped at once and freed, or released. Each calls the engine's hook, OnStop or OnRelease.
    void StopChannel(Player& player, Engine& engine, Channel& channel);
    void ReleaseChannel(Player& player, Engine& engine, Channel& channel);

    // The track's newest channel, and the one after `channel`, or nullptr.
    Channel* First(Engine& engine) const;
    const Channel* First(const Engine& engine) const;
    Channel* Next(Engine& engine, const Channel& channel) const;
    const Channel* Next(const Engine& engine, const Channel& channel) const;

    // 0x022C9A4C. Returns whether the note changes the sound (for a rip tool's length analysis). A note changes the
    // sound unless it continues the newest note at the same key and velocity, with no sweep. Only a tie or a monophonic
    // note continues the newest note.
    bool NoteOn(Player& player, Engine& engine, int key, int velocity, int32_t length, bool tie);

    // The track went back to `target`, the start of a loop. The pass that ended there began at the track's last run of
    // `target`. A pass that changed nothing in the sound is idle, such as a pass that only polls or counts a variable.
    // An idle pass is never a loop of the sequence (the engine's OnLoop). Any other pass of track 0 is one. So is a
    // pass of the main track (SequenceSoundPlayer::IsMainTrack) that played a note on the way round. A pass that played
    // no note is one too while the track holds a note that sounds until the game stops it (HoldsSustainedNote) and
    // every other open track rests (SequenceSoundPlayer::OthersRest). The pass then changes that note, such as its
    // volume or pitch. The engine's OnTrackLoop gets every pass.
    void CountLoop(Player& player, Engine& engine, uint32_t target);

    // Whether the engine takes the counted loop whose body starts at `address` as endless (its LoopsForEver).
    bool LoopsForEver(Player& player, Engine& engine, uint32_t address) const;

    // The command being run changed the track's sound.
    void MarkChange(Player& player);

    // A number from the engine's random generator, for a random argument or the random command (0x86). The track
    // records the draw. Then the engine's OnRandomDraw, if it has one, is called.
    uint32_t DrawRandom(Player& player, Engine& engine);

    // The track read variable `index` in a command's argument or in a comparison. Whether a track has read variable
    // `index` so far: any track for the player's and the global variables, this one for the track's variables.
    void MarkRead(Player& player, int index);
    bool IsRead(const Player& player, int index) const;

    // Whether the track holds a note that sounds until the game stops it on a looping wave, settled at its envelope's
    // sustain level. Such a note is a held note (see kHeldTicks) or a note of length 0.
    bool HoldsSustainedNote(const Engine& engine) const;

    // A variable by its command's index (MmlParser::GetVariablePtr, 0x022CB600): 0 to 15 the player's variables, 16 to
    // 31 the global ones, 32 to 47 the track's variables.
    int16_t* Variable(Player& player, Engine& engine, int index);

    int index_;
    bool open_ = false; // +0x01

    // SetTrackVolume, SetTrackPitch and the rest: 1 and the defaults. The game calls them. The player's SetTrackVolume
    // sets the volume, and the others keep their defaults.
    float volume_ = 1.0f;              // +0x04
    float pitch_ = 1.0f;               // +0x08
    float pan_range_ = 1.0f;           // +0x0C
    typename Policy::Outputs outputs_; // +0x10

    std::span<const uint8_t> data_; // +0x100
    uint32_t current_ = 0;          // +0x104
    uint32_t command_end_ = 0;      // see CommandEnd
    bool has_data_ = false;
    bool cmp_flag_ = true;                                                   // +0x108
    bool note_wait_ = true;                                                  // +0x109
    bool tie_ = false;                                                       // +0x10A
    bool mono_ = false;                                                      // +0x10B
    std::array<CallStackEntry, kCallStackDepth> call_stack_;                 // +0x10C
    uint8_t call_depth_ = 0;                                                 // +0x15C
    bool front_bypass_ = false;                                              // +0x15D (InitParam leaves it)
    bool mute_ = false;                                                      // +0x15E
    bool silence_ = false;                                                   // +0x15F
    int32_t wait_ = 0;                                                       // +0x160
    bool note_finish_wait_ = false;                                          // +0x164
    bool porta_ = false;                                                     // +0x165
    bool damper_ = false;                                                    // +0x166
    uint8_t bank_index_ = 0;                                                 // +0x167
    uint32_t prg_no_ = 0;                                                    // +0x168
    float sweep_pitch_ = 0.0f;                                               // +0x16C
    MoveValue<uint8_t> vol_;                                                 // +0x170
    MoveValue<uint8_t> volume2_;                                             // +0x176
    MoveValue<int8_t> pan_;                                                  // +0x17C
    MoveValue<int8_t> span_;                                                 // +0x182
    MoveValue<int8_t> bend_;                                                 // +0x188
    std::array<CurveLfoParam, 4> lfo_params_;                                // +0x190
    std::array<uint8_t, 4> lfo_types_{};                                     // +0x1D0
    uint8_t velocity_range_ = 127;                                           // +0x1D4
    uint8_t bend_range_ = 2;                                                 // +0x1D5
    int8_t init_pan_ = 0;                                                    // +0x1D6
    int8_t transpose_ = 0;                                                   // +0x1D8
    uint8_t priority_ = 64;                                                  // +0x1D9
    uint8_t porta_key_ = 60;                                                 // +0x1DA
    uint8_t porta_time_ = 0;                                                 // +0x1DB
    uint8_t attack_ = 0xFF, decay_ = 0xFF, sustain_ = 0xFF, release_ = 0xFF; // +0x1DC
    int16_t hold_ = 0xFF;                                                    // +0x1E0
    int8_t biquad_type_ = 0;                                                 // +0x1E2
    uint8_t main_send_ = 127;                                                // +0x1E3
    std::array<uint8_t, 3> fx_send_{};                                       // +0x1E4
    float lpf_ = 0.0f;                                                       // +0x1E8
    float biquad_value_ = 0.0f;                                              // +0x1EC
    int32_t output_line_ = -1;                                               // +0x1F0: -1 takes the player's line
    std::array<int16_t, 16> vars_{};                                         // +0x1F4
    ChannelId channel_list_ = kNoChannel;                                    // +0x218

    // What a rip tool's length analysis follows while the engine follows loops. Times are counts of the commands that
    // the player's tracks have run (SequenceSoundPlayer::commands_run_).
    std::vector<uint64_t> run_;                // when the track last ran each command, or 0 if it hasn't
    uint64_t changed_at_ = 0;                  // when the track last changed the sound
    uint64_t drew_at_ = 0;                     // when the track last drew a random number
    uint64_t conditional_at_ = 0;              // when the track last ran a conditional command
    uint64_t looped_at_ = 0;                   // when the track last went back to the start of a loop
    uint64_t noted_at_ = 0;                    // when the track last played a note
    uint16_t variables_read_ = 0;              // the track's variables it has read, a bit each
    std::array<int16_t, 48> loop_variables_{}; // the variables that tracks read, at the last loop (0 for the others)
    bool idle_ = false;                        // whether the track's last pass of a loop changed nothing in the sound
    bool still_ = false; // whether that pass was idle, drew no random number and left the variables as the one before
    bool quiet_ = false; // whether that pass played no note
    bool conditional_ = false; // whether the command being run is conditional
    bool holds_ = false;       // whether the track's last note is a held one (see kHeldTicks)
    bool held_wait_ = false;   // whether the track waits out a held note
};

// What a player gets from its sound: the sound's info in the archive, its sequence's bytecode, and its banks by slot.
// The banks are item IDs. The engine finds a note's bank and wave (its NoteOn).
//
// Loops: while the engine's FollowsLoops() is true, the engine's OnLoop(player, track, target) reports a loop of the
// sequence (for a rip tool's length analysis). A pass of track `track` that changed the sound ended with a jump back to
// `target`, a command the track has already run, or at the end of a loop that repeats for ever and starts at `target`.
// The track is track 0, or the main track if it played a note on the way round (see IsMainTrack). A later track also
// counts if it holds a note that sounds until the game stops it while every other open track rests (see OthersRest). A
// jump back into code that the track hasn't run isn't a loop. Sequences that share their start make such jumps.
// The engine's OnTrackLoop(player, track, target), if it has one, gets every track's passes. A rip tool can then wait
// for the cycles of the tracks other than the main track.
template <typename Policy>
class SequenceSoundPlayer
{
public:
    using Engine = typename Policy::Engine;
    using Receiver = typename Policy::Receiver;
    using TrackType = SequenceTrack<Policy>;

    // Player `id` of an engine. The player's channels output to `receiver` on a console whose policy has receivers.
    SequenceSoundPlayer(int id, const typename Policy::SoundInfo& info, std::span<const uint8_t> data,
                        const Receiver* receiver = nullptr);

    // The player's place among the engine's players. A note's channel gives it as its owner (Policy::NoteOnInfo's
    // owner_player), with the track as owner_track.
    int Id() const
    {
        return id_;
    }

    // A channel's callback. The engine passes the callback on to the channel's owner: track `track` of this player
    // (SequenceTrack::OnChannelEvent).
    void ChannelEvent(Engine& engine, int track, typename Policy::Channel& channel)
    {
        if (TrackType* owner = GetTrack(track))
        {
            owner->OnChannelEvent(engine, channel);
        }
    }

    // Opens the tracks that the sound's allocation flags name. The first track opens at the sound's start (Setup,
    // 0x022C8654, and Start, 0x022C8924).
    void Start();

    // Each sound frame, or `frames` of them: the ticks that come due, then each track's parameters reach its channels
    // (0x022B75FC).
    void Update(Engine& engine, int frames = 1);

    // A tick in steps, for an engine that runs the ticks itself, such as one that steps another model of each track
    // beside it. BeginTick moves the main volume a tick on. RunTrackTick then runs a tick of track `index`, for each
    // index from 0 to 15 in turn: the lengths of its notes, its commands, and its close when it ends. RunTrackTick
    // returns whether the track is open after the tick. EndTick ends the tick with whether any track is open: the
    // player finishes if none is, or else counts the tick. EndTick returns whether the player finished. A clock's tick
    // runs the same steps (0x022B70AC). UpdateTrackParams gives each track's parameters to its channels. Update does
    // that after a frame's ticks. Call the steps on a player that has started and hasn't finished.
    void BeginTick();
    bool RunTrackTick(Engine& engine, int index);
    bool EndTick(Engine& engine, bool any_open);
    void UpdateTrackParams(Engine& engine);

    // The rest of track `index`'s tick on a copy of the player, made in a hook while the track ran its commands: the
    // commands up to the next wait, and the track's close if it ends. Returns whether the track is open after them. An
    // engine that tests how the sequence plays on from a jump runs the copy on this way.
    bool ContinueTrackTick(Engine& engine, int index);

    // The clock in steps, for an engine that runs the ticks itself, on a console whose clock counts units of time (not
    // Policy::kFloatClock). StartClock begins `frames` frames of the clock. NextTickTime then moves the clock to the
    // next tick in them, and returns the units left in them after the tick's time (see OnTickStart). It returns nothing
    // once no tick falls in them, or while the clock gives no ticks (ClockStopped). Run each tick before the next call.
    // Update runs a frame's ticks in the same steps.
    void StartClock(int frames)
        requires(!Policy::kFloatClock);
    std::optional<uint64_t> NextTickTime()
        requires(!Policy::kFloatClock);

    // The same steps on a console whose clock counts in float milliseconds (Policy::kFloatClock). StartClock begins one
    // frame, and `frames` must be 1. NextTickTime returns the units left in the frame after the tick's time, and
    // ClockRestMs the same time in the clock's float milliseconds. ClockTicksLeft gives the ticks that a clock in
    // 416ths of a tick (Policy::CountsTickParts) has left in the frame after the tick.
    void StartClock(int frames)
        requires Policy::kFloatClock;
    std::optional<uint64_t> NextTickTime()
        requires Policy::kFloatClock;

    float ClockRestMs() const
        requires Policy::kFloatClock
    {
        return clock_rest_ms_;
    }

    int ClockTicksLeft() const
        requires Policy::kFloatClock
    {
        return clock_ticks_due_;
    }

    // Whether the clock gives no ticks: the tempo, the timebase or the tempo ratio is 0. A clock in 416ths of a tick
    // (Policy::CountsTickParts) gives none once the tempo times the tempo ratio truncates to 0 and less than a whole
    // tick is counted.
    bool ClockStopped() const
    {
        if constexpr (Policy::kFloatClock)
        {
            if (Policy::CountsTickParts(build_))
            {
                return Policy::ToInt32(static_cast<float>(tempo_) * tempo_ratio_) == 0 && tick_parts_ < 416;
            }
        }

        return Policy::TicksPerMs(timebase_, tempo_, tempo_ratio_) == 0.0f;
    }

    // The library's build, on a console whose commands or clock depend on it (Policy::Build). The game's code gives it.
    // Set it before the sequence starts.
    void SetBuild(const typename Policy::Build& build)
    {
        build_ = build;
        grammar_ = GrammarOf<Policy>(build);
    }

    // Sets variable `index` to `value`. A game can do that through the sound's handle, before the sequence starts or
    // while it plays. 0 to 15 are the player's variables, and 16 to 31 the global ones. `engine` holds the global
    // variables. Other indices change nothing.
    void SetVariable(Engine& engine, int index, int16_t value)
    {
        if (int16_t* variable = Variable(engine, index))
        {
            *variable = value;
        }
    }

    // Sets the tempo ratio, by which the clock multiplies the tempo. It starts at 1. A game sets it through the sound's
    // handle (SeqPlayer::SetTempoRatio, 0x8003e818 in Wii Sports' main.dol). The 3DS's library has no tempo ratio.
    void SetTempoRatio(float ratio)
    {
        tempo_ratio_ = ratio;
    }

    // Sets the volume of each allocated track whose bit is set in `track_flags`, bit 0 for track 0. Policy::TrackVolume
    // multiplies it into the volume of the track's channels. It starts at 1, and opening a track keeps it. Set it after
    // Start, which allocates the tracks. A game sets it through the sound's handle (SeqPlayer::SetTrackVolume,
    // 0x8003e8b0 in Wii Sports' main.dol). The 3DS's policy leaves it out.
    void SetTrackVolume(uint32_t track_flags, float volume)
    {
        for (int i = 0; i < kTrackCount; ++i)
        {
            if ((track_flags & (1u << i)) != 0 && tracks_[i])
            {
                tracks_[i]->volume_ = volume;
            }
        }
    }

    bool IsFinished() const
    {
        return finished_;
    }

    // The ticks so far (+0x228).
    uint32_t TickCount() const
    {
        return tick_count_;
    }

    // The sequence's tempo and timebase, and the main volume with its ramp.
    uint16_t Tempo() const
    {
        return tempo_;
    }

    uint8_t Timebase() const
    {
        return timebase_;
    }

    const MoveValue<uint8_t>& MainVolume() const
    {
        return main_volume_;
    }

    // The player's variables, numbered 0 to 15 in the commands.
    const std::array<int16_t, 16>& LocalVariables() const
    {
        return local_vars_;
    }

    // Track `index`, 0 to 15, or nullptr if it isn't allocated.
    const TrackType* Track(int index) const
    {
        return index >= 0 && index < kTrackCount && tracks_[index] ? &*tracks_[index] : nullptr;
    }

    // Whether the sequence has started and hasn't ended.
    bool IsActive() const
    {
        return started_ && !finished_;
    }

    // Whether no open track will change the sound while the notes sound, or the tempo is 0, or the timebase under the
    // policy's length rules (for a rip tool's length analysis). Each open track waits for its notes to end, waits for
    // ever, or only makes still passes (SequenceTrack::IsStill) and has no note whose length will run out
    // (SequenceTrack::HasEndingNote). Then the sequence plays on unchanged while the notes sound.
    bool IsSettled(const Engine& engine) const;

    // Whether no open track will change the sound again, or the tempo is 0, or the timebase under the policy's length
    // rules. Each open track waits for ever, or only makes still passes that play no notes (SequenceTrack::IsQuiet).
    bool IsStuck(const Engine& engine) const;

    // Whether the main volume, or any open track's volume, pan or pitch bend, is moving to a new value.
    bool IsMoving() const;

    // When a track last changed the sound, counted in the commands that the tracks have run while the engine follows
    // loops (for a rip tool's length analysis). A track changes the sound when it plays a note, changes a setting or a
    // ramp, or opens a track. 0 if no track has.
    uint64_t ChangedAt() const
    {
        return changed_at_;
    }

private:
    friend class SequenceTrack<Policy>;

    // 0x022B5588.
    TrackType* GetTrack(int index)
    {
        return index >= 0 && index < kTrackCount && tracks_[index] ? &*tracks_[index] : nullptr;
    }

    int16_t* Variable(Engine& engine, int index); // 0x022C8A58

    // Whether track `index` is the main track: every track before it is closed, idle (SequenceTrack::IsIdle) or waiting
    // for ever (SequenceTrack::WaitsForEver), or comes after track 0 and plays no notes in its passes
    // (SequenceTrack::IsQuiet). The main track's loops are the sequence's loops. Track 0 keeps every later track from
    // the loops while its passes aren't idle.
    bool IsMainTrack(const Engine& engine, int index) const;

    // Whether every open track but track `index` is idle (SequenceTrack::IsIdle) or waits for ever
    // (SequenceTrack::WaitsForEver).
    bool OthersRest(const Engine& engine, int index) const;

    // Whether `test` holds for every open track. When none is open, the result comes from the policy's length rules
    // (no_track_passes).
    bool EveryOpenTrack(const std::function<bool(const TrackType& track)>& test) const;

    // A tick of every track, in the steps above. Returns whether the player finished.
    bool RunTick(Engine& engine);

    // The ticks in `frames` frames of the clock, in the clock's steps (0x022B7368). On a console whose clock counts in
    // float milliseconds (Policy::kFloatClock), `frames` must be 1.
    void UpdateTick(Engine& engine, int frames);

    void FinishPlayer(Engine& engine);          // 0x022B620C
    void CloseTrack(Engine& engine, int index); // 0x022B6198

    int id_;
    std::span<const uint8_t> data_;
    uint32_t allocate_track_flags_;
    uint32_t start_offset_;
    std::vector<uint32_t> bank_items_;                         // the bank in each slot, by item ID
    std::array<std::optional<TrackType>, kTrackCount> tracks_; // +0x1C8
    std::array<int16_t, 16> local_vars_;                       // +0x208
    bool started_ = false;                                     // +0x25
    bool finished_ = false;                                    // +0x27

    // What BasicSound gives the player from the sound's info and what the game sets (BasicSoundPlayer's parameters).
    float volume_ = 1.0f;              // +0x2C
    float pitch_ = 1.0f;               // +0x30
    float lpf_ = 0.0f;                 // +0x34
    float biquad_value_ = 0.0f;        // +0x38
    int8_t biquad_type_ = -1;          // +0x3C: replaces the tracks' type when 0 or more
    uint32_t pan_mode_ = 0;            // +0x40
    uint32_t pan_curve_ = 0;           // +0x44
    uint32_t output_line_ = 1;         // +0x48
    typename Policy::Outputs outputs_; // +0x50
    const Receiver* receiver_ = nullptr;

    bool release_priority_fix_ = false; // +0x190
    float pan_range_ = 1.0f;            // +0x194
    float tempo_ratio_ = 1.0f;          // +0x198
    float tick_fraction_ = 0.0f;        // +0x19C
    uint16_t tick_parts_ = 416;         // a clock in 416ths of a tick: the time counted, from a whole tick at the start
    uint64_t clock_left_ = 0;           // the units left in the clock's frames after the last tick (StartClock)
    bool clock_ticked_ = false;         // whether a tick has come in those frames
    float clock_rest_ms_ = 0.0f;        // a float clock: the milliseconds left in the frame after the last tick
    int clock_ticks_due_ = 0;           // a clock in 416ths: the whole ticks that the frame took out of the count
    uint8_t channel_priority_ = 64;     // +0x1AC
    uint8_t timebase_ = 48;             // +0x1AD
    uint16_t tempo_ = 120;              // +0x1AE
    MoveValue<uint8_t> main_volume_;    // +0x1B0
    typename Policy::Build build_{};    // the library's build if its commands depend on it
    Grammar grammar_ = GrammarOf<Policy>(); // the rules by which the tracks read the build's commands
    uint32_t tick_count_ = 0;               // +0x228

    // What a rip tool's length analysis follows while the engine follows loops: the commands that the tracks have run,
    // when a track last changed the sound (ChangedAt), and the player's and the global variables that a track has read,
    // a bit each.
    uint64_t commands_run_ = 0;
    uint64_t changed_at_ = 0;
    uint32_t variables_read_ = 0;
};

} // namespace ntdlib::nwsnd
