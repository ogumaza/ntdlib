// SPDX-License-Identifier: MIT

// A NintendoWare sequence's performance: PerformanceBase, with what the NintendoWare performer adds to it. Each
// distinct PresetKey becomes a SoundFont preset.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "ntdlib/sequence_export/envelope.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/performance_base.h"
#include "ntdlib/sequence_export/settings.h"
#include "seqlib/midi/performance.h"

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

// Returns the frequency of `key`'s LFO in Hz, in SoundFont and SFZ time. On the 3DS, that time runs env_time_scale
// times slower than the game's envelope time.
double LfoHz(const PresetKey& key, const Settings& settings);

// Returns the delay of `key`'s LFO in milliseconds, in SoundFont and SFZ time.
double LfoDelayMs(const PresetKey& key, const Settings& settings);

// A recorded sequence: PerformanceBase, with its presets and what the NintendoWare performer adds to it.
struct Performance : PerformanceBase
{
    std::vector<PresetKey> presets;

    // Biquad type per track, or 0 for none. SFZ uses the first type each track enables.
    std::array<uint8_t, 16> biquad_types{};

    // On the Wii: played as a build of NW4R SND from 2008 on (SndBuild::Since2008). The regions of those builds have a
    // hold and may ignore note-offs.
    bool since_2008 = false;

    // On the Wii: stereo waves play in balance mode (SoundInfo::pan_mode). The builds of 2006 have only dual mode.
    bool balance = false;
};

// The most voices that --voices takes. Without --voices, a track can start this many new notes in one tick.
constexpr uint32_t kMaxVoiceLimit = 1024;

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

    // The voices for the notes that the game drops when no voice is free, from --voices: a count, or 0 to keep every
    // note. Without --voices, every note is kept.
    int voice_limit = 0;
};

} // namespace ntdlib::sequence_export
