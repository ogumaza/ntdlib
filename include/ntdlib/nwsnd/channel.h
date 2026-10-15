// SPDX-License-Identifier: MIT

// The sound library's channels: nw::snd's driver::Channel and driver::ChannelManager in NintendoWare for Cafe, and the
// same classes of nn::atk on the Switch. The 3DS's nw::snd and the Wii's NW4R SND have older versions of them (see
// console.h). A channel is a note on a voice. The channel has an envelope, the console's LFOs (four, or one on the 3DS
// and the Wii), a pitch sweep and the parameters that its sequence track sets. Every sound frame, the channel works
// these out into its voice's parameters. The model is a template on a policy. The policy gives the console's steps (see
// console.h) and the types around the channels: the engine, the waves that the channels play and the outputs. The
// engine holds the channels and their voices. The engine goes into each call that requires it. The channels then hold
// no pointers, and a copy of them keeps its links. The comment on a method that follows one of the game's routines
// gives the routine's address in Animal Crossing: amiibo Festival's executable. A step of nn::atk alone gives the
// address in WarioWare: Get It Together!'s main. A field comment gives the field's offset in the game's object.
//
// The policy supplies the types Engine, WaveInfo (a wave's format, sample_rate and loop), Outputs and Console (the
// console's policy, see console.h), and ForEachOutput(function, outputs...). ForEachOutput calls the function on each
// output device's parameters in turn. For each device, ForEachOutput takes that device's parameters from each of the
// outputs it's given. A device's parameters have the fields volume, mix_mode, mix_parameter, pan, span, main_send and
// fx_send.
//
// The engine supplies:
//
//     Channels()                              the ChannelManager
//     Voices()                                the voices. AllocVoice(engine, channels, priority, owner) gives the
//                                             place of a voice that calls back the channel at `owner`, or kNoVoice.
//                                             At(place) gives the voice at a place.
//     Rng()                                   the random numbers that the LFOs draw
//     ChannelEvent(player, track, channel, status)
//                                             passes a channel's callback on to the track that owns the channel
//
// A voice supports SetSampleFormat, SetSampleRate, SetInterpolationType, AppendWave(engine, wave), Start, Stop(engine),
// Free(engine), SetVolume, SetPitch, SetPanMode, SetPanCurve, SetLpfFreq, SetBiquadFilter, SetOutputLine, SetOutputs
// and SetPriority(engine, priority). The Wii U's voices are MultiVoices (see voice.h).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <optional>
#include <vector>

#include "ntdlib/nwsnd/envelope.h"

namespace ntdlib::nwsnd
{

// A channel's place among the engine's channels. The model links channels by their places: a track's newest channel,
// and each channel's next channel. A copy of the model's objects then keeps its links.
using ChannelId = int;
inline constexpr ChannelId kNoChannel = -1;

// A voice's place among the engine's voices.
using VoiceId = int;
inline constexpr VoiceId kNoVoice = -1;

// Why a voice calls its channel back (VoiceCallbackStatus): its wave has played to its end (0), it was cancelled (1),
// the voices dropped it for a voice of higher priority (2), or the renderer took one of its voices away (3).
enum class VoiceCallbackStatus : uint32_t
{
    kFinishWave = 0,
    kCancel = 1,
    kDropVoice = 2,
    kDropDsp = 3,
};

template <typename Policy>
class ChannelManager;

// A note on a voice (driver::Channel), with the note's envelope, LFOs and sweep. ChannelManager holds the channels.
template <typename Policy>
class Channel
{
public:
    using Engine = typename Policy::Engine;
    using WaveInfo = typename Policy::WaveInfo;
    using Outputs = typename Policy::Outputs;
    using Console = typename Policy::Console;

    // The channel's LFOs, as many as the console has and of its kind, and what each one moves.
    using Lfos = std::array<typename Console::Lfo, Console::kLfoCount>;
    using LfoTypes = std::array<uint8_t, Console::kLfoCount>;

    // Why a channel calls its owner back (ChannelCallbackStatus): it was stopped (0), its voice was dropped (1), its
    // wave played to its end (2), or it was cancelled (3).
    enum class CallbackStatus : uint32_t
    {
        kStopped = 0,
        kDrop = 1,
        kFinish = 2,
        kCancel = 3,
    };

    // nn::atk's type of an LFO that has none yet. Such an LFO doesn't step (see console.h's kUntypedLfos).
    static constexpr uint8_t kUntypedLfo = 0xFF;

    // Plays a wave on the channel's voice from the wave's start, for `length` ticks, or for ever when `length` is
    // negative (Start, 0x022B9948). Notes always start at the wave's start. The model therefore leaves out the start
    // offset that the game can give.
    void Start(Engine& engine, const WaveInfo& wave, int32_t length);

    // Each sound frame: the volume, pitch and output parameters, from the note, the track, the envelope and the LFOs
    // (the nameless 0x022C42C8). With `periodic`, the envelope, LFOs and sweep advance by a frame. On nn::atk, a
    // note-off that waits for the channel's first update takes effect after they advance. A policy whose voices use
    // these parameters in another shape gives the whole frame instead: UpdateChannel(channel, engine, periodic).
    void Update(Engine& engine, bool periodic);

    // The sweep's pitch now, in semitones (GetSweepValue, 0x022C4204).
    float GetSweepValue() const;

    void Stop(Engine& engine);    // 0x022C4278
    void Release(Engine& engine); // 0x022B6028

    // Ends the note (0x022B6E2C) if the note doesn't ignore note-off. nn::atk's note-off waits for the next periodic
    // update when the channel hasn't had an update yet (see console.h's kDeferredNoteOff).
    void NoteOff(Engine& engine);

    // Calls the owner back, once (CallChannelCallback, 0x022B8BA0). The engine passes the call on to the owner.
    void CallChannelCallback(Engine& engine, CallbackStatus status);

    // Leaves the channel without an owner (DetachChannel, 0x022B6104).
    void Detach()
    {
        owner_player = -1;
        owner_track = -1;
    }

    // The voice's call when its wave ends or the voice is dropped (VoiceCallbackFunc, 0x022B8BF0).
    void OnVoiceEvent(Engine& engine, VoiceCallbackStatus status);

    // Whether the wave loops.
    bool WaveLoops() const
    {
        return loop_;
    }

    // A pitch sweep from `pitch` semitones away to the note's key over `ticks` (SetSweepParam, 0x022C487C).
    void SetSweepParam(float pitch, int32_t ticks, bool auto_update)
    {
        sweep_pitch = pitch;
        sweep_length = ticks;
        sweep_counter = 0;
        auto_sweep = auto_update;
    }

    // 0x022B6E68.
    void UpdateSweep(int32_t count)
    {
        sweep_counter += count;
        if (sweep_counter > sweep_length)
        {
            sweep_counter = sweep_length;
        }
    }

    // The channel's voice. The engine holds the voices, and the channel reaches its voice only through this call.
    auto& Voice(Engine& engine) const
    {
        return engine.Voices().At(voice);
    }

    CurveAdshr<Console> envelope;      // +0x10
    Lfos lfos;                         // +0x2C
    LfoTypes lfo_types{};              // +0xAC: 0 pitch, 1 volume, 2 pan, and on nn::atk kUntypedLfo
    bool active = false;               // +0xB1
    bool allocated = false;            // +0xB2
    bool auto_sweep = true;            // +0xB3
    bool release_priority_fix = false; // +0xB4
    bool ignore_note_off = false;      // +0xB5
    uint8_t biquad_type = 0;           // +0xB6
    float track_volume = 1.0f;         // +0xB8
    float user_pitch = 1.0f;           // +0xBC
    float lpf = 0.0f;                  // +0xC0
    float biquad_value = 0.0f;         // +0xC4
    uint32_t output_line = 1;          // +0xC8
    Outputs outputs;                   // +0xCC: each device's output parameters
    float bend = 0.0f;                 // +0x1EC: semitones
    float sweep_pitch = 0.0f;          // +0x1F0
    int32_t sweep_counter = 0;         // +0x1F4
    int32_t sweep_length = 0;          // +0x1F8
    float init_pan = 0.0f;             // +0x1FC
    float init_span = 0.0f;            // +0x200
    float tune = 1.0f;                 // +0x204
    uint8_t silence_from = 255;        // +0x208: a ramp of the volume, in 255ths, that silences a track
    uint8_t silence_to = 255;          // +0x209
    uint16_t silence_duration = 0;     // +0x20A: frames
    uint16_t silence_counter = 0;      // +0x20C
    float cached_pitch = 0.0f;         // +0x210
    float cached_ratio = 1.0f;         // +0x214
    int32_t length = 0;                // +0x218: ticks, or negative for none
    uint32_t pan_mode = 0;             // +0x21C
    uint32_t pan_curve = 0;            // +0x220
    uint8_t key = 60;                  // +0x224
    uint8_t original_key = 60;         // +0x225
    uint8_t key_group = 0;             // +0x226
    uint8_t interpolation_type = 0;    // +0x227
    float init_volume = 1.0f;          // +0x228
    float velocity_volume = 1.0f;      // +0x22C

    // +0x230: the callback. The engine passes it on to the owner: the player's place among the engine's players, and
    // the track. Both are -1 for a channel without an owner.
    int owner_player = -1;
    int owner_track = -1;

    // The player and the track whose note the channel plays. Unlike the owner, they stay when the track lets the
    // channel go. The game has no such fields. A host uses them to find the track that a sound comes from.
    int source_player = -1;
    int source_track = -1;

    VoiceId voice = kNoVoice;             // +0x238
    ChannelId next_in_track = kNoChannel; // +0x23C: the next channel of the track
    ChannelId place = kNoChannel;         // the channel's place among the engine's channels

private:
    friend class ChannelManager<Policy>;

    void InitParam(int player, int track); // 0x022B8E28

    // The frame of Update for the policies without UpdateChannel.
    void RunFrame(Engine& engine, bool periodic);

    bool loop_ = false;              // +0x2E8
    bool release_requested_ = false; // nn::atk's +0x3D1: a note-off waits for the next periodic update
    bool updated_ = false;           // nn::atk's +0x3D2: the channel has had an update
};

// The channels: a pool of a fixed number, and a list of those in use, in the order they were allocated. They update in
// that order. nw::snd's pool lays its channels out one after another, and hands out the last freed first. Before any
// has been freed, the pool hands out its last channel first (PoolImpl's CreateImpl, AllocImpl and FreeImpl: 0x0228EADC,
// 0x022901CC and 0x022901F0). With no freed channel to hand out, the pool hands out the next channel down. The model's
// pool adds each place the first time it hands the place out. The model's pool then holds no more places than the most
// channels in use at once. A deque holds the places. A channel keeps its address while the pool grows.
template <typename Policy>
class ChannelManager
{
public:
    using Engine = typename Policy::Engine;

    // `count` is nw::snd's channel count, the same .data word as its voice count (0x101AA478).
    explicit ChannelManager(int count);

    // A channel with a voice for `channels` channels at `priority`, or nullptr (Channel::AllocChannel, 0x022B9000). The
    // channel calls back the track `track` of the player at `player` among the engine's players.
    Channel<Policy>* AllocChannel(Engine& engine, int channels, uint32_t priority, int player, int track);

    // Each sound frame: every channel's update, and those that have stopped are freed (0x022C97B4).
    void UpdateAllChannel(Engine& engine);

    int ActiveCount() const
    {
        return static_cast<int>(active_.size());
    }

    // The places of the channels in use, in the list's order.
    const std::list<ChannelId>& InUse() const
    {
        return active_;
    }

    // The channel at `place`. The place must be in use.
    Channel<Policy>& At(ChannelId place)
    {
        return *Entry(place);
    }

    const Channel<Policy>& At(ChannelId place) const
    {
        return *Entry(place);
    }

    // Whether any channel's envelope is in its attack, hold, decay or release, rather than settled at its sustain level
    // (for a rip tool's length analysis).
    bool AnyChanging() const;

    // Takes a channel out of the list and frees it (0x022B8D40). Channel::FreeChannel (0x022C41D0) does the same when a
    // track stops a note.
    void Free(Channel<Policy>& channel);

private:
    // Whether the pool holds `place`. The pool adds the places from the last down.
    bool Holds(ChannelId place) const
    {
        return place < count_ && place >= count_ - static_cast<int>(pool_.size());
    }

    // The pool's entry for `place`. The pool must hold the place.
    std::optional<Channel<Policy>>& Entry(ChannelId place)
    {
        return pool_[static_cast<std::size_t>(count_ - 1 - place)];
    }

    const std::optional<Channel<Policy>>& Entry(ChannelId place) const
    {
        return pool_[static_cast<std::size_t>(count_ - 1 - place)];
    }

    int count_;                                       // the places that the pool can hand out
    std::deque<std::optional<Channel<Policy>>> pool_; // each place's channel while it's in use, the last place first
    std::vector<ChannelId> free_;                     // the freed places, the next to hand out last
    std::list<ChannelId> active_;
};

} // namespace ntdlib::nwsnd
