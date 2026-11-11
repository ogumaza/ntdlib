// SPDX-License-Identifier: MIT

// The records of the JAudio performer (performer.h): tracks, channels, LFOs, random numbers and the state of a
// conductor. The constants and helpers that more than one of the performer's files use are here too.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "seqlib/midi/recorder.h"

#include "ntdlib/jaudio/calc.h"
#include "ntdlib/jaudio/sequence_commands.h"
#include "ntdlib/sequence_export/jaudio/envelope.h"
#include "ntdlib/sequence_export/jaudio/instruments.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/jaudio/track_filter.h"
#include "ntdlib/sequence_export/jaudio/wave.h"

namespace ntdlib::sequence_export::jaudio
{

// A track's channel slots (TChannelMgr::CHANNEL_MAX), its children, and the depth of its call stack.
inline constexpr int kNoteIds = 8;
inline constexpr int kChildren = 16;
inline constexpr int kStackSize = 8;

// A sequence that opens more tracks than this is damaged.
inline constexpr std::size_t kMaxTracks = 512;

// A note quieter than this, in dB, can't be heard. A released note that quiet no longer counts as sounding when a track
// looks for a free MIDI channel. A tie on a note whose envelope has fallen that far raises nothing in MIDI.
inline constexpr double kInaudibleDb = -40.0;

// The level of a channel on the DSP's automatic mixer, against a channel whose gains updateMixer sets. A JAudio1 track
// that connects its mix line 0 to 0xffff (TTrack::connectBus) puts the channels that it starts on that mixer.
// TChannel::updateAutoMixer passes the DSP program the channel's volume at a level of 0x7fff (Driver::getAutoLevel),
// with its pan, effect send and surround. The DSP program sets the outputs' gains. Such a channel plays at about half
// the level. The half is an approximation: the performer doesn't model the DSP program's law.
inline constexpr float kAutoMixerLevel = 0.5f;

// A command's entry in a table of commands (see sequence_commands.h). JAudio2's table is jaudio::kSequenceCommands.
using CommandInfo = ntdlib::jaudio::SequenceCommand;

// The registers of a JAudio1 track: The Wind Waker's 14, and Pikmin's 32.
inline constexpr std::size_t kRegisters1 = 14;
inline constexpr std::size_t kRegistersJac = 32;

// The interrupts of a JAudio1 track (JASystem::TIntrMgr's requests): a write to port 0 or 1, its timer, a tick.
inline constexpr int kIntr1Port0 = 3;
inline constexpr int kIntr1Port1 = 4;
inline constexpr int kIntr1Timer = 6;
inline constexpr int kIntr1Tick = 7;

// cmdPanSwSet's 27 rules for a note's pan or effect send (JASystem::Driver::calc_sw_table).
using ntdlib::jaudio::kCalcRules;

// The interrupts a track can take (JASSeqCtrl::IntrType): pause, unpause, a write to port 0 or 1, its timer, a tick.
inline constexpr int kIntrPort0 = 2;
inline constexpr int kIntrPort1 = 3;
inline constexpr int kIntrTimer = 5;
inline constexpr int kIntrTick = 6;

// The sound library's random numbers. JAudio2 draws them from the game's fast generator (JMath::TRandom_fast_). That
// generator starts in `seed`. JAudio1 has another generator, with a state of two words (ntdlib::jaudio::PlayerRandom).
// Luigi's Mansion's earlier JAudio1 mixes the time base into its numbers. Its sounds draw from JAudio2's generator
// (SequenceSound::timed_random).
class Random
{
public:
    Random(bool jaudio1, uint32_t seed) : jaudio1_(jaudio1), fast_(seed)
    {
    }

    uint32_t Next()
    {
        if (!jaudio1_)
        {
            fast_ = ntdlib::jaudio::NextFastRandom(fast_);

            return fast_;
        }

        return player_.Next();
    }

private:
    bool jaudio1_;
    uint32_t fast_;
    ntdlib::jaudio::PlayerRandom player_;
};

// A track parameter that moves linearly to a target over a number of ticks (JASTrack::MoveParam_).
struct MoveParam
{
    float value = 0.0f;
    float target = 0.0f;
    uint32_t count = 0;
};

// The settings a track gives its channels (JASChannelParams): volume and pitch multiply down the tracks, and pan,
// effect send and the bend, in octaves, add.
struct ChannelParams
{
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.5f;
    float fx_mix = 0.0f;
    float bend = 0.0f;
};

// A channel's vibrato or tremolo (JASLfo): a sine of `depth`, `pitch` 72nds of a cycle per sub-frame, after a delay in
// sub-frames.
class Lfo
{
public:
    void Set(float depth, float pitch)
    {
        depth_ = depth;
        pitch_ = pitch;
    }

    void SetDelay(uint16_t delay)
    {
        delay_ = delay;
    }

    // Start again from the start of the cycle, after the delay (resetCounter).
    void Reset()
    {
        wait_ = delay_;
        phase_ = 0.0f;
    }

    void Advance(float step)
    {
        if (wait_ > 0)
        {
            wait_--;
            return;
        }

        phase_ += step * ((1.0f / 72.0f) * pitch_);
        if (phase_ >= 1.0f)
        {
            phase_ -= 1.0f;
        }
    }

    float Value() const
    {
        if (wait_ > 0 || depth_ == 0.0f)
        {
            return 0.0f;
        }

        return depth_ * std::sin(2.0f * 3.14159265f * phase_);
    }

private:
    float depth_ = 0.0f;
    float pitch_ = 1.0f;
    uint16_t delay_ = 0;
    uint16_t wait_ = 0;
    float phase_ = 0.0f;
};

// A JASChannel: a voice playing one note, and the MIDI note that stands for it.
struct Channel
{
    // The game's state.
    int track = -1;   // the track that gives it settings, or -1 once the track has detached it
    int note_id = -1; // its slot in the track
    NoteSound sound;
    uint8_t key = 0; // the key the track played, transposed
    uint8_t velocity = 0;
    int base_key = 60;       // the key that plays the wave at its rate: the wave's base key, or a drum's key
    float key_offset = 0.0f; // semitones from the base key (mKey)
    float key_sweep = 0.0f;  // how far a sweep has moved, in semitones
    float sweep_target = 0.0f;
    uint32_t sweep_count = 0; // sub-frames left in the sweep
    float init_pitch = 1.0f;  // the instrument's pitch times the wave's rate over the output rate
    std::array<const Oscillator*, 2> osc_data{};
    std::array<const Oscillator*, 2> track_osc{}; // the track's oscillators that play in place of the instrument's
    std::array<OscillatorState, 2> osc{};
    Lfo vibrato, tremolo;
    uint32_t update_timer = 0; // sub-frames until a gate note's release
    uint16_t direct_release = 0;
    bool started = false; // has had its first sub-frame
    bool released = false;
    bool ended = false;
    bool pitch_moved = false; // an oscillator has moved its pitch
    ChannelParams params;
    uint8_t priority = 0x40;
    uint8_t release_priority = 0;
    uint32_t age = 0; // sub-frames since it started (JASDSPChannel's update counter)
    double position = 0.0;
    const Pcm* pcm = nullptr; // the decoded wave
    uint32_t wave_length = 0; // for a wave that doesn't loop: its samples
    bool wave_loops = true;
    float level = 0.0f;          // its volume in the last sub-frame
    bool auto_mixer = false;     // it plays on the DSP's automatic mixer (kAutoMixerLevel)
    float envelope = 0.0f;       // its oscillators' volume in the last sub-frame
    bool envelope_heard = false; // that volume has been above kInaudibleDb

    // The MIDI note.
    int slot = -1; // its MIDI track
    bool midi_on = false;
    uint32_t midi_id = 0;
    uint8_t part = 0;
    float midi_key_offset = 0.0f; // key_offset when the MIDI note started
    uint8_t midi_velocity = 0;    // the MIDI note's velocity
    uint8_t cc11_velocity = 0;    // the velocity that CC11 follows (GateOn)
    bool bends = false;           // pitch bends and CC11 follow the note's sweeps and tied notes
};

// The ticks in a sub-frame at a tempo, timebase and tempo rate (JASTrack::updateTempo, 0x80037804). 80 samples a
// sub-frame make 4/3 of a 60th of a second's ticks per thousand samples (4/3 at 0x8053b274).
inline float TickRate(uint16_t tempo, uint16_t timebase, float tempo_rate)
{
    return tempo_rate * (static_cast<float>(timebase) * static_cast<float>(tempo) * (4.0f / 3.0f) / kDacRate);
}

// True if `volume`, an amplitude, is below kInaudibleDb.
inline bool Inaudible(float volume)
{
    return 20.0 * std::log10(std::max(static_cast<double>(volume), 1e-10)) <= kInaudibleDb;
}

// True if `ch`'s envelope has fallen below hearing. An attack that starts there hasn't. A tie changes the note's
// velocity but not its envelope.
inline bool BelowHearing(const Channel& ch)
{
    return ch.envelope_heard && Inaudible(ch.envelope);
}

// A byte value spread over 16 bits: below 0x80, its low bits repeat. A wider value from a register keeps the bits that
// the shifts leave in 16 bits.
inline int16_t SpreadByte(uint32_t byte)
{
    auto value = static_cast<int16_t>(byte << 8);
    if (!(byte & 0x80))
    {
        value = static_cast<int16_t>(value | static_cast<int16_t>(byte << 1));
    }

    return value;
}

// The argument types that a register prefix gives a command, in the command table's form. The top bits of `mask`, one
// for each of the first `count` arguments, mark those that come from registers (type 3).
inline uint16_t RegisterTypes(uint8_t mask, int count)
{
    uint16_t types = 0;
    uint16_t bits = 3;
    for (int i = 0; i < count; i++)
    {
        if (mask & 0x80)
        {
            types = static_cast<uint16_t>(types | bits);
        }
        mask = static_cast<uint8_t>(mask << 1);
        bits = static_cast<uint16_t>(bits << 2);
    }

    return types;
}

// A JAudio1 track's oscillator as TTrack::init sets it (the template at 0x80367568 in The Wind Waker's main.dol): a
// volume oscillator with no attack table, and a release table that falls to 0 over 10 units and stops.
inline const Oscillator kTrackOscillator1{0, 1.0f, {}, {{0, 10, 0}, {15, 1, 0}}, false, 1.0f, 0.0f};

// A JAudio1 track oscillator's route when it has none (TTrack::init).
inline constexpr uint8_t kNoRoute = 0xf;

// A JASTrack with its sequence control (JASSeqCtrl) and reader (JASSeqReader).
struct Track
{
    // Allocation.
    bool open = false;
    int parent = -1;
    std::array<int, kChildren> children{};
    int slot = -1; // the MIDI track it records into
    std::string name;
    bool muted = false;         // JASTrack::mute's flag (SequenceSound::tracks)
    bool silent = false;        // at a volume of 0 through the sound's handle (SequenceSound::child_volumes)
    float handle_volume = 1.0f; // its volume through the sound's handle, other than 0 (SequenceSound::child_volumes)
    int conductor_track = -1;   // the conductor's track whose module its last call 0x800 started

    // The reader.
    uint32_t pc = 0;
    std::array<uint32_t, kStackSize> return_to{};
    std::array<uint16_t, kStackSize> loop_count{};
    int depth = 0;
    int32_t wait = 0;
    bool wait_note_finish = false;

    // Interrupts.
    uint32_t intr_table = 0;
    uint16_t intr_mask = 0;
    uint16_t intr_pending = 0;
    std::optional<uint32_t> intr_return;
    uint8_t timer_repeats = 0;
    uint32_t timer = 0;
    uint32_t timer_reload = 0;

    // Registers and ports. JAudio1 tracks have kRegisters1 or kRegistersJac of them.
    std::array<uint16_t, kRegistersJac> regs{};
    std::array<uint16_t, 16> ports{};
    uint16_t imported = 0, exported = 0;

    // Settings.
    std::array<MoveParam, 6> move{};
    std::array<std::optional<Oscillator>, 2> osc{};
    std::array<float, 2> osc_scale{1.0f, 1.0f}; // the oscillators' scales (JASTrack's mOscParam[i].mScale)
    uint16_t bank = 0;
    uint16_t program = 0xf0;
    int8_t transpose = 0;
    uint8_t bend_sense = 12;
    uint8_t gate_rate = 100;
    uint8_t note_on_prio = 0x40;
    uint8_t release_prio = 0;
    uint32_t skip_samples = 0;
    uint16_t direct_release = 0;
    float pan_power = 1.0f;
    float vib_depth = 0.0f, vib_pitch = 1.0f, trem_depth = 0.0f, trem_pitch = 1.0f;
    uint16_t vib_delay = 0, trem_delay = 0;
    uint8_t latest_key = 60;
    float tick_rate = TickRate(120, 48, 1.0f); // the tick rate that gate notes use (JASTrack +0x1dc)
    bool tied = false;                         // the next gate note continues the last (flag4)
    bool dirty = true;                         // settings to recompute (flag5)
    bool iir = false;                          // an IIR filter other than the open one
    uint8_t filter_entry = kOpenTrackFilter;   // the IIR filter's entry of the table, or the open one's for others
    std::array<int, kNoteIds> slots{};
    ChannelParams params;

    // JAudio1's settings (JASystem::TTrack). The registers are JASystem::TRegisterParam's in `regs`: six general ones
    // (3 is the flag that conditions test), the bank and program (6), the bend sense (7), the five pan powers (8 to 12)
    // and the priority (13). The address registers are apart.
    std::array<uint32_t, 4> addresses{};
    uint8_t volume_mode = 0;                  // 0 squares the volume (JAudio2 always does)
    std::array<uint8_t, 3> parent_calc{};     // how the pan, the effect send and the surround take the parent's
    uint8_t open_mode = 0;                    // the opentrack command's mode (TTrack +0x37b)
    bool auto_mixer = false;                  // its mix line 0 connects to 0xffff: the DSP's automatic mixer
    std::array<uint32_t, 8> intr_addresses{}; // each interrupt's routine
    bool intr_enabled = true;                 // clri enables the interrupts, seti disables them
    int32_t intr_wait = 0;                    // the wait that a running interrupt put aside
    bool gate_ends = false;                   // a gate note's wait frees note slot 0 when it runs out (endProcess)
    bool time_relate = true;                  // a gate note's length is in ticks, not in sub-frames

    // The rules (kCalcRules) for its notes' pan, effect send and surround (TTrack +0x15a). TTrack::init gives 13.
    std::array<uint8_t, 3> channel_calc{13, 13, 13};

    // Its two oscillators (TTrack +0x2cc) and their routes to its notes' oscillators (TTrack +0x2fc).
    std::array<Oscillator, 2> osc1{kTrackOscillator1, kTrackOscillator1};
    std::array<uint8_t, 2> osc_routes{kNoRoute, kNoRoute};

    // Whether the performer follows each of those oscillators: false once a command or a parameter that it leaves out
    // has changed the oscillator.
    std::array<bool, 2> osc1_known{true, true};

    // Pikmin's tempo of the track (SequenceSound::jac). A child takes its parent's when it opens. A child that sets one
    // runs a tick for each whole step of its count. Each of its parent's ticks adds the share of the parent's tempo, at
    // most 1.
    uint16_t tempo = 120;
    bool own_tempo = false;
    float tempo_count = 0.0f;

    // The volume of the track's external buffer (SequenceSound::child_volumes).
    float ext_volume = 1.0f;

    // MIDI state.
    seqlib::midi::TrackOutput output; // the MIDI track and part it records into, and each part's last values
    std::optional<uint32_t> last_change;
    std::optional<uint64_t> last_signal; // when it last wrote another track's port, in commands run
    bool idle = false;                   // its last pass of a loop changed nothing
    uint32_t opened = 0;                 // the tick it last opened in
    std::optional<uint32_t> last_wait;
    std::map<uint32_t, std::pair<uint32_t, uint64_t>> visited; // each command's last tick, and the commands run then
};

// What the game's sound layer keeps for a module of a track of a conductor (PSAutoBgm::Module and its two rounds,
// PSAutoBgm::OnCycle and OffCycle). Offsets are the fields' in the module.
struct ConductorModuleState
{
    bool tables = false;               // the lists below are set (+0x2c1)
    uint32_t phrase_list = 0;          // the module's list of phrases' offsets, 3 bytes each (+0x2a8)
    uint8_t phrase_count = 0;          // (+0x2ac)
    uint32_t length_list = 0;          // its list of lengths, 2 bytes each (+0x2b0)
    uint8_t length_count = 0;          // (+0x2b4)
    uint16_t beats = 0;                // the beats since the module started (+0x2a4)
    uint8_t cycle = 0;                 // 0 in the round of phrases (OnCycle), 1 in the round of rests (+0x2c0)
    std::array<uint8_t, 2> steps{};    // each round's step (+0x8 of each round)
    std::array<uint8_t, 2> taken{};    // the lengths that each round has taken in order (+0x3c of each round)
    uint16_t length = 0;               // the last length (+0x2b6)
    uint16_t phrase = 0xffff;          // the last phrase (+0x2c2)
    std::array<uint16_t, 2> history{}; // the round of phrases' last two phrases, the older first
    uint8_t history_count = 0;
};

// What it keeps for a track of a conductor (PSAutoBgm::Track): the module that plays, or 0xff before the first.
struct ConductorTrackState
{
    uint8_t module = 0xff; // (+0x99)
    std::vector<ConductorModuleState> modules;
};

// A command run's outcome.
enum class Outcome
{
    kContinue,
    kWait,
    kFinish,
};

} // namespace ntdlib::sequence_export::jaudio
