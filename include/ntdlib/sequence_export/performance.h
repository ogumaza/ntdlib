// SPDX-License-Identifier: MIT

// A NintendoWare sequence's performance: PerformanceBase, with what the NintendoWare performer adds to it. Each
// distinct PresetKey becomes a SoundFont preset.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "ntdlib/sequence_export/settings.h"

namespace ntdlib::sequence_export
{

using EventKind = seqlib::midi::EventKind;
using TrackEvent = seqlib::midi::TrackEvent;

// Note-on settings that a MIDI channel cannot represent. Each distinct PresetKey produces a SoundFont preset.
struct PresetKey
{
    bool operator==(const PresetKey&) const = default;

    uint8_t bank_slot = 0; // index into the sound's bank list (0-3; higher slots have no bank). It's 0 on the Wii.
    uint32_t program = 0;

    // Track envelope overrides (0xff: none, the bank region's value applies).
    uint8_t attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff, hold = 0xff;

    // The track's first LFO. CC1 holds the depth. Pan LFO is supported by SFZ only. Other types have no effect in the
    // engine. A negative delay keeps the LFO from starting. Such LFOs are omitted too. The speed and the delay are in
    // the engine's units: Hz and milliseconds on the Wii U and the Switch, and the bytecode's speed (Hz = speed * 100
    // / 256) and 5 ms steps on the 3DS and the Wii. LfoHz() and LfoDelayMs() convert them.
    uint8_t lfo_type = 0; // 0 pitch, 1 volume, 2 pan, 3 none
    uint8_t lfo_range = 1;
    float lfo_speed = 6.25f;
    uint32_t lfo_delay = 0;

    int8_t init_pan = 0; // -64..63, added to the region pan

    // Track filter (see filter.h). Filters that change during notes use kFilterController instead of a fixed cutoff.
    // This loses the biquad low-pass's slight resonance.
    SoundFontFilter filter;
    bool filter_controller = false;

    // The attack, in timecents, of a note that the game releases during its attack. The SoundFont and SFZ attacks rise
    // linearly in amplitude. The game's attack rises exponentially in dB. This attack reaches the game's level when the
    // note is released, and the release starts from that level. Unset for other notes.
    std::optional<int16_t> attack_timecents;

    // On the Wii: the centibels by which the zones play louder than their regions' volumes, up to full level. The Wii's
    // voice limits a channel's volume to full level. MIDI's velocity and track controllers stop at full level. The
    // zones take the rest of a note's volume (MidiRecorder::ZoneLift).
    int16_t lift = 0;

    // On the Wii: a note whose volume LFO the voice holds at full level. The SoundFont and SFZ LFOs can't stop there.
    // They swing `lfo_swing` 16ths of the LFO's depth instead. The zones play `lfo_cut` centibels quieter. The values
    // fit the note's level to the game's (MidiRecorder::VolumeLfoFits).
    uint8_t lfo_swing = 16;
    int16_t lfo_cut = 0;
};

// Applies the track envelope overrides in `key` to `env`.
EnvelopeValues OverrideEnvelope(EnvelopeValues env, const PresetKey& key);

// Whether the preset keys of `engine` hold the LFO in the bytecode's units: the speed, and the delay in 5 ms steps. The
// 3DS's and the Wii's keys do (PresetKey).
bool LfoInBytecodeUnits(Engine engine);

// Returns the frequency of `key`'s LFO in Hz, in SoundFont and SFZ time. On the 3DS, that time runs env_time_scale
// times slower than the game's envelope time.
double LfoHz(const PresetKey& key, const Settings& settings);

// Returns the delay of `key`'s LFO in milliseconds, in SoundFont and SFZ time.
double LfoDelayMs(const PresetKey& key, const Settings& settings);

// How a stretch of a track's commands began (Stretch::entry).
enum class Entry : uint8_t
{
    kOpen,   // the track opened
    kJump,   // a jump
    kCall,   // a call
    kReturn, // a return from a call
    kLoop,   // a loop's end, back to the loop's start
};

// A stretch of a track's commands that ran one after another, for an editor. A stretch begins where its track opens,
// jumps, calls, returns or loops back. It ends at the next such command, or where the track ends. Its commands run at
// the ticks that its waits give, from `tick` on.
struct Stretch
{
    bool operator==(const Stretch&) const = default;

    uint32_t tick = 0;  // when its first command ran
    uint32_t first = 0; // its first command's offset in the sequence's bytecode
    uint32_t last = 0;  // its last command's offset
    Entry entry = Entry::kOpen;
    uint8_t depth = 0;      // the calls and loops on the track's stack
    bool note_wait = false; // at its start, a note made the track wait for the note's length (MML notewait)
};

// The note command that started a MIDI note, for an editor.
struct NoteCommand
{
    bool operator==(const NoteCommand&) const = default;

    uint32_t offset = 0;  // the command's offset in the sequence's bytecode
    uint32_t stretch = 0; // the stretch that ran it, in Performance::stretches of the note's track
};

// A setting of a track's mix, for an editor. The values are those that the console's sound library holds. On the
// Switch, they're in the units of commands/units.h.
enum class Mix : uint8_t
{
    kVolume, // from 0 to 255, and 127 before a command sets it
    kPan,    // from -64 for the left to 63 for the right, and 0 before a command sets it
    kSendA,  // the send to aux bus A, from 0 to 255, and 0 before a command sets it
    kSendB,
    kSendC,
};

// The number of a track's mix settings.
inline constexpr int kMixSettingCount = 5;

// A setting of a track's mix that changed in a tick, for an editor, with its value at the end of the tick.
struct MixChange
{
    bool operator==(const MixChange&) const = default;

    uint32_t tick = 0;
    Mix setting = Mix::kVolume;
    int16_t value = 0;
};

// A recorded sequence: PerformanceBase, with its presets and what the NintendoWare performer adds to it.
struct Performance : PerformanceBase
{
    std::vector<PresetKey> presets;

    // Biquad type per track, or 0 for none. SFZ uses the first type each track enables.
    std::array<uint8_t, 16> biquad_types{};

    // On the Wii: played as a build of NW4R SND from March 2008 on (SndBuild::SinceMarch2008). The regions of those
    // builds have a hold and may ignore note-offs.
    bool since_march_2008 = false;

    // On the Wii: stereo waves play in balance mode (SoundInfo::pan_mode). The build of October 2006 has only dual
    // mode.
    bool balance = false;

    // For an editor: each track's stretches of commands, in the order that they ran, and the note command of each MIDI
    // note, at the note's ID less 1 (TrackEvent::note).
    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;

    // For an editor: each track's changes of its volume, pan and sends, in the order of their ticks. A ramp changes
    // its setting in each tick until it ends.
    std::array<std::vector<MixChange>, 16> mix;
};

// The most voices that --voices takes. Without --voices, a track can start this many new notes in one tick.
constexpr uint32_t kMaxVoiceLimit = 1024;

// A game's change of a variable while the sequence plays, through the sound's handle. Variable `index` takes `value` at
// the start of sound frame `frame`, before the frame's ticks. Frame 0 is the frame of the sequence's first tick.
// Indices 0 to 15 are the player's variables, and 16 to 31 the global ones. Other indices change nothing.
struct VariableChange
{
    uint64_t frame = 0;
    uint8_t index = 0;
    int16_t value = -1;
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;

    // The random generator's state when the game starts the sound. 0x12345678 is its value at startup. The game
    // advances it every sound frame. The state therefore depends on elapsed time. Settings::random_steps_before_start
    // gives the steps before the sequence starts.
    uint32_t seed = 0x12345678;

    // Values for player variables (0-15) and global variables (16-31), set before the sequence starts. Some sequences
    // play nothing until the game sets one of their variables.
    std::map<uint8_t, int16_t> variables = {};

    // The game's changes of player and global variables while the sequence plays. Changes in one frame come in this
    // order. A loop's passes before a change can play otherwise than the passes after it. The main loop is therefore
    // a pass that starts after the last change. The performance plays on to find it.
    std::vector<VariableChange> changes = {};

    // The voices for the notes that the game drops when no voice is free, from --voices: a count, or 0 to keep every
    // note. Without --voices, every note is kept.
    int voice_limit = 0;

    // The tracks that play, bit 0 for track 0. A game's code can silence a track through the sound's handle: the
    // player's SetTrackVolume, with volume 0. Once the sound has started, the performance gives the other tracks
    // volume 0 that way. Their notes stay in the file, at CC11 0. The search for loops and for the end still counts
    // those notes.
    uint16_t tracks = 0xffff;

    // The sound's effect sends on buses A, B and C, from 0 to 1. A game's code sets them through the sound's handle
    // (BasicSound::SetFxSend). Each adds to every track's send on its bus. A voice holds the sum between 0 and 1. CC91
    // takes the sum on bus A, and on the Wii, CC93 takes the sum on bus B. On the Switch, the sends on buses B and C
    // add to the dry level (Sends::kReverbDry). The 3DS's conversion leaves the sends out (Sends::kNone).
    std::array<float, 3> fx_sends = {};
};

} // namespace ntdlib::sequence_export
