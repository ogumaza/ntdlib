// SPDX-License-Identifier: MIT

// The settings of the NintendoWare performer. A program fills one Settings for the console it reads. Where the
// consoles' sound engines differ in mechanism, Settings::engine selects the engine's version of the code. Where they
// differ only in a value, a field holds the value.
//
// Each field's comment gives each console's value: W for the Wii U, S for the Switch, 3 for the 3DS and R for the Wii.

#pragma once

#include <cstdint>
#include <optional>

#include "ntdlib/ax/lpf.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"

namespace ntdlib::sequence_export
{

struct FilterTables;
struct ResamplingRule;

// The sound engine that the performer models. The engine selects the core that plays the sequence, with the console's
// policy (nwsnd/console.h). The core has the console's parse, clock, call stack, channels, envelopes, LFOs, sweeps and
// one-shot ends, in the console's arithmetic. Outside the core, the engine selects these:
// - the frames that a channel outlasts its one-shot wave: none (W), 2 (S) or 1 (3, R);
// - the units of the performer's frame: 1/65536 ms (W, S) or thousandths of the ARM's cycles (3), and the October
//   2006 build's tick units (R);
// - the policy of the predictions of the notes' levels and ends, and of the filters' SoundFont and SFZ forms;
// - the units of the LFO's speed and delay in the preset keys: the bytecode's units (3, R) or the core's units (W, S);
// - the interpolation messages: two on W and on 3, none on S and R;
// - the Wii's rules around the core: the track allocation, the global variables' start, the volume limits, the pan
//   curve, the key groups' release, the loop analysis and the sequence's end (R).
enum class Engine : uint8_t
{
    kRevolution, // NW4R SND and AX on the Wii
    kCtr,        // nw::snd and the DSP on the 3DS
    kCafe,       // nw::snd and AX on the Wii U
    kSwitch,     // nn::atk and the audio renderer on the Switch
};

// Where a track's effect sends go.
enum class Sends : uint8_t
{
    kNone,         // CC91 and CC93 stay at 0; sends A and B are approximated as omitted (3)
    kReverb,       // A as CC91; B, C and a main send other than 127 are approximated as omitted (W)
    kReverbChorus, // A as CC91 and B as CC93 (R)
    kReverbDry,    // A as CC91; the main send, B and C reach the mix dry, as CC11 (S)
};

// The settings of one console. consoles.h gives each console's Settings.
struct Settings
{
    Engine engine = Engine::kCafe;

    // How the sar readers read the archive and its files: the flavour, the reader's messages and how a group's file
    // table is read. Each console's function in consoles.h sets them. The Wii's RSAR archives have another reader.
    sar::Options archive;

    // Timing. One sound frame lasts frame_ms: 3 (W, R), 5 (S), or 160 samples of the DSP's clock (3:
    // 160 * 8192 * 1000 / 268111856). The envelopes and LFOs step env_step_ms at a time: 3 (W, R) or 5 (S, 3). On 3
    // the SoundFont and SFZ times scale by env_time_scale, frame_ms / env_step_ms. It's 1 on the others.
    double frame_ms = 3.0;
    int env_step_ms = 3;
    double env_time_scale = 1.0;

    // The new notes or legatos that a track can start in one tick in the game: 96 (W, S, R), 24 (3), 64 (R's build
    // of October 2006). A game can set another count. WarioWare: Get It Together! sets 72. With --voices, the performer
    // omits a track's new notes past this count in one tick. Without it, the cap is kMaxVoiceLimit. Legatos past this
    // count in one tick continue their MIDI note either way. The messages give the count in force.
    int voices = 96;

    // The voices that the game has, for the notes that the game drops when no voice is free. A console sets its count
    // once the performer drops those notes on its model. The console's settings then note how closely the model's drops
    // follow the game's drops. A conversion keeps every note by default. The hardware's drops aren't known exactly on
    // any console yet. With --voices N, the conversion drops notes as the model does with N voices (PerformOptions).
    // The help gives this count as the game's default. 0: no --voices option.
    int voice_limit = 0;

    // Steps of the random generator before the sound starts: 0 (W, R), 1 (S), 17 (3). The 3DS and the Wii take theirs
    // from their policies (kRandomStepsBeforeStart).
    int random_steps_before_start = 0;

    // The bytecode's 16- and 24-bit arguments and the FE track mask follow the sequence file's byte order (S). The
    // others read them big-endian.
    bool arguments_follow_byte_order = false;

    // A note whose length runs out in its note-on frame releases at the channel's first update (S).
    bool note_off_waits_for_update = false;

    Sends sends = Sends::kReverb;

    // Surround pan (0xd7) moves (R). The others approximate it as omitted.
    bool surround_pan = false;

    // A legato keeps the volume of the region that its channel started on (W, S). 3 and R leave the region's volume
    // out. The Wii U's SequenceTrack::NoteOn keeps it.
    bool keep_first_region_volume = true;

    // The console's SoundFont and SFZ forms of the track filters (filter.h). The script
    // tools/sequence_export/filter_tables.py fits them to the renderer's coefficients: 32 kHz with 23 low-pass steps
    // (W), 32 kHz with 58 steps (R), 32728 Hz (3), or 48 kHz with biquad types 6 to 10 (S). R's builds from November
    // 2008 give the 58 steps W's fits (RevolutionFilters).
    const FilterTables* filters = nullptr;

    // The low-passes that stand in for the console's resampling filters in the SoundFonts and the SFZ files
    // (resampling.h): for AX's sets (W, R), the renderer's sets (S) or the DSP's kernels (3). nullptr: none.
    const ResamplingRule* resampling = nullptr;

    // AX's one-pole low-pass, to which each SoundFont zone fits its cutoff for the track low-pass (zone_low_pass.h in
    // the sources): AXComputeLpfCoefs (W) or AXGetLpfCoefs (R). Unset: the zones take the table's cutoff (S, 3).
    std::optional<ax::LpfSettings> zone_low_pass;

    // NW4R SND's build (R only).
    SndBuild wii_build;

    // On W and S, UpdateTick truncates each tick's length to 1/65536 ms. A pass that ends at 10 s by the tempo then
    // ends just short of it. A pass that ends this many milliseconds before 10 s counts as reaching it: 1.0 (W, S),
    // 0.0 (3, R).
    double early_ticks_ms = 1.0;
};

} // namespace ntdlib::sequence_export
