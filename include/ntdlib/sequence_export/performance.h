// SPDX-License-Identifier: MIT

// A NintendoWare sequence's performance: PerformanceBase, with what the NintendoWare performer adds to it. Each
// distinct PresetKey becomes a SoundFont preset.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
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

    // Index into the sound's bank list (0-3; higher slots have no bank). It's 0 on the Wii. 128 to 131 are the banks of
    // a companion (BankSet::SetCompanion).
    uint8_t bank_slot = 0;
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

    // On the Wii U and the Switch: the notes start past one of the console's pitch limits and hold their pitch. No
    // sweep, no LFO bend and no bend moves them (PassFindings::pitch_moves). The console plays them at the limit. Their
    // zones and regions play the keys past a limit there (ResamplingRegion::at_limits). Other notes play every key at
    // its pitch. A bend or a sweep then moves them as in MIDI, past the limits too.
    bool at_pitch_limits = false;
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

// A game's change of a variable while the sequence plays, through the sound's handle. Variable `index` takes `value` at
// the start of sound frame `frame`, before the frame's ticks. Frame 0 is the frame of the sequence's first tick.
// Indices 0 to 15 are the player's variables, and 16 to 31 the global ones. 32 to 47 set track variable `index` - 32
// of every track. Other indices change nothing.
struct VariableChange
{
    bool operator==(const VariableChange&) const = default;

    uint64_t frame = 0;
    uint8_t index = 0;
    int16_t value = -1;
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

    // Whether a track drew a random number before the performance stopped. A sound that draws can play otherwise when
    // the game starts it in another frame: the sound library steps its random numbers once a frame
    // (PerformOptions::seed).
    bool draws = false;

    // For an editor: each track's changes of its volume, pan and sends, in the order of their ticks. A ramp changes
    // its setting in each tick until it ends.
    std::array<std::vector<MixChange>, 16> mix;

    // The changes of the global variables (16 to 31) that the sequence's commands made, in order, each in the frame of
    // its tick (VariableChange). Another sound reads them in the same frame (PerformBeside). The changes of
    // PerformOptions' `changes` aren't among them.
    std::vector<VariableChange> global_changes;

    // The global variables that the sequence's commands read: bit i for variable 16 + i.
    uint16_t globals_read = 0;
};

// The most voices that --voices takes. Without --voices, a track can start this many new notes in one tick.
constexpr uint32_t kMaxVoiceLimit = 1024;

// How a game plays a companion: a sequence that it starts before a piece and plays beside it, and that talks with the
// piece through the global variables (PerformBeside). The game starts the companion, and writes `writes` from the
// companion's first frame: frame 0 is the frame of the companion's first tick. The piece plays from the frame in which
// the companion's commands set global variable `hand_over` to `hand_over_value`. `volume` is the companion's volume
// (PerformOptions::volume).
struct CompanionScript
{
    std::vector<VariableChange> writes;
    uint8_t hand_over = 16;
    int16_t hand_over_value = 0;
    float volume = 1.0f;
};

// A game's write of a variable inside a user procedure: a function of the game that the sequence calls with the command
// 0xF0E0 and a number (nn::atk's SequenceUserProcCallback). From the start of sound frame `frame`, the write waits for
// the sequence's next call of procedure `procedure`, on any track. Variable `index` then takes `value` inside the call,
// before the track's next command. Frame 0 is the frame of the sequence's first tick. Indices 0 to 15 are the player's
// variables, and 16 to 31 the global ones. Other indices change nothing.
struct ProcedureChange
{
    uint64_t frame = 0;
    uint16_t procedure = 0;
    uint8_t index = 0;
    int16_t value = -1;
};

// The tracks of a sequence: 16.
inline constexpr std::size_t kSequenceTracks = 16;

// A value that a game gives a track's variable through the sound's handle as the sequence starts. Variable `index`, 0
// to 15, of track `track` is the variable that the track's commands number 32 + `index`.
struct TrackVariable
{
    uint8_t track = 0;
    uint8_t index = 0;
    int16_t value = -1;
};

// A model of a game's user procedures (PerformOptions::user_procedures). It takes a procedure's number and the
// variables that the procedure sees: the player's 16, the 16 global ones and the 16 of the track that calls it.
using UserProcedureModel = std::function<bool(uint16_t procedure, std::span<int16_t, 16> local,
                                              std::span<int16_t, 16> global, std::span<int16_t, 16> track)>;

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
    // play nothing until the game sets one of their variables. 32 to 47 set track variable N - 32 of every track. The
    // performer sets them after the player allocates the tracks and before the first tick.
    std::map<uint8_t, int16_t> variables = {};

    // Values for the tracks' variables, set after the start and before the first tick. A track that the sequence's
    // start doesn't allocate takes none (nw::snd's SequenceSoundPlayer::SetTrackVariable).
    std::vector<TrackVariable> track_variables = {};

    // The game's changes of player, global and track variables while the sequence plays. Changes in one frame come in
    // this order. A loop's passes before a change can play otherwise than the passes after it. The main loop is
    // therefore a pass that starts after the last change. The performance plays on to find it.
    std::vector<VariableChange> changes = {};

    // The game's writes of variables inside user procedures. The writes that wait for the same call take effect in this
    // order. As with `changes`, the main loop is a pass that starts after the last write that takes effect, and the
    // performance plays on until every write's frame has come.
    std::vector<ProcedureChange> procedure_changes = {};

    // A model of the game's user procedures, or none. The model changes the variables as the game's procedure does,
    // inside the call, before the writes of `procedure_changes`. It returns whether the call changed state that the
    // variables don't show, such as a random generator's. The search for loops then counts the call as a random draw.
    // Each pass of the performance starts from a copy of the model. A model that holds its state by value therefore
    // starts each pass from the same state. Without a model, a call changes only what `procedure_changes` writes.
    UserProcedureModel user_procedures = {};

    // The voices for the notes that the game drops when no voice is free, from --voices: a count, or 0 to keep every
    // note. Without --voices, every note is kept.
    int voice_limit = 0;

    // The tracks that play, bit 0 for track 0. A game's code can silence a track through the sound's handle: the
    // player's SetTrackVolume, with volume 0. Once the sound has started, the performance gives the other tracks
    // volume 0 that way. Their notes stay in the file, at CC11 0. The search for loops and for the end still counts
    // those notes.
    uint16_t tracks = 0xffff;

    // The volumes that a game's code gives the tracks through the sound's handle (the player's SetTrackVolume), track 0
    // first, as factors of amplitude. Once the sound has started, the performance sets each volume other than 1 that
    // way, on the tracks that `tracks` keeps. The player multiplies a track's volume into its channels' volume, and
    // CC11 takes its square root.
    std::array<float, kSequenceTracks> track_volumes = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    // The sound's effect sends on buses A, B and C, from 0 to 1. A game's code sets them through the sound's handle
    // (BasicSound::SetFxSend). Each adds to every track's send on its bus. A voice holds the sum between 0 and 1. CC91
    // takes the sum on bus A, and on the Wii, CC93 takes the sum on bus B. On the Switch, the sends on buses B and C
    // add to the dry level (Sends::kReverbDry). By default, the 3DS's conversion leaves the sends out (Sends::kNone).
    std::array<float, 3> fx_sends = {};

    // The tempo ratio that the game sets through the sound's handle (nw::snd's SetTempoRatio). The core's ticks come at
    // the sequence's tempo times the ratio, and so do the MIDI file's tempo events.
    float tempo_ratio = 1.0f;

    // The pitch that the game sets through the sound's handle (nw::snd's SetPitch), a ratio of frequencies. The core's
    // voices play at their pitch times the ratio. Another pitch tunes every MIDI channel at tick 0, by RPN 2 (coarse
    // tuning) and RPN 1 (fine tuning), both in the MIDI specification.
    float pitch = 1.0f;

    // The volume that the game sets through the sound's handle (nw::snd's SoundHandle::SetVolume), a factor of
    // amplitude. It multiplies the sound's volume from the archive, and the MIDI file's levels take it in the same way.
    float volume = 1.0f;

    // The banks that the game gives the sequence's bank slots in place of the archive's, as item IDs (nw::snd's
    // SeqSoundInfo, which a StartInfo with flag 0x10 holds). The four replace all of the archive's slots, and
    // 0xFFFFFFFF leaves a slot empty. Without a value, the archive's banks hold. The 3DS's conversion reads them. The
    // other consoles' conversions ignore them.
    std::optional<std::array<uint32_t, 4>> banks = std::nullopt;
    // The reverbs that a game's code puts on aux buses A, B and C while the sound plays: each one's dry gain, the share
    // of its input that it passes on unchanged, from 0 to 1. None for a bus without a reverb. On the Switch
    // (Sends::kReverbDry), a send to bus B or C without a reverb adds to the dry level in full. With a reverb, it adds
    // at the reverb's dry gain, and CC91 takes it with the send on bus A. Bus A's entry isn't used: CC91 always stands
    // in for bus A's effect, unless bus_a_delay says that the effect is a delay.
    std::array<std::optional<float>, 3> bus_reverbs = {};

    // Whether the game keeps a delay on aux bus A (sar::ArchiveRules::bus_a_delay). CC91 stands in for one kind of
    // effect. Where a reverb on bus B or C takes CC91, the send on bus A then leaves it.
    bool bus_a_delay = false;
};

} // namespace ntdlib::sequence_export
