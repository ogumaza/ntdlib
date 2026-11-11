// SPDX-License-Identifier: MIT

// The performance of a song or a sound effect of DSE, Procyon Studio's sound driver for the DS, for MIDI, a SoundFont
// and SFZ files. The model of DSE's driver plays the sequence on dse::Player, with the model of the DS's sound driver
// and the sound unit. A sound effect plays on the first sequence of the driver's pool. The performer records what the
// driver plays: each note with its voices, the settings of each channel of the sequence, and a preset for each program
// with the channel's envelope and LFOs. The model follows the game's driver tick by tick. The notes whose voices the
// driver gives to other notes are therefore the ones that the console cuts, and a voice whose wave ends without a loop
// ends as on the console.
//
// Each channel of the sequence is a MIDI track: the channels hold the settings, and every track plays on one. The
// performance plays the main loop the number of times that the options give, and marks the pass that a player looping
// on the markers repeats as the game plays the next pass (ntdlib/sequence_export/passes.h).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/player.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::dse
{

// The events of a performance's tracks, from seqlib.
using EventKind = seqlib::midi::EventKind;
using TrackEvent = seqlib::midi::TrackEvent;

// What a song or a sound effect plays (ntdlib/dse/player.h). The driver refuses a bank whose id it holds already.
using ntdlib::dse::SequenceFiles;

// The settings of a note that a MIDI channel can't hold. Each distinct key makes a SoundFont preset.
struct PresetKey
{
    bool operator==(const PresetKey&) const = default;

    uint16_t bank = 0;
    uint16_t program = 0;
    ntdlib::dse::EnvelopeValues envelope = {}; // the channel's override of the splits' envelopes
    std::array<ntdlib::dse::LfoValues, 4> lfos = {};
    int8_t lfo_level = 0; // the level of the LFOs of type 3 (0xBE)
};

// A note that the driver never started, for an editor: it found no voice, or its program plays nothing at its key and
// velocity.
struct UnstartedNote
{
    bool operator==(const UnstartedNote&) const = default;

    uint32_t tick = 0;
    uint32_t length = 0; // the note's length in ticks, or 0 for a note that its channel holds
    uint8_t track = 0;   // the track's slot in the sequence
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint32_t offset = 0; // the note's event, as NoteCommand's
};

// What the driver did with a MIDI note's voices, for an editor.
struct NoteVoices
{
    bool operator==(const NoteVoices&) const = default;

    // the tick where a later note took the note's last voice while its channel held it
    std::optional<uint32_t> cut_tick;
};

// A recorded sequence: PerformanceBase, with its presets.
//
// For an editor, it also holds the stretches of each track's events, by the track's slot in the sequence, in the order
// that they ran. A stretch begins where its track starts, and at each event that doesn't follow the one before it:
// after the end of a pass of the track's loop (0x98), a repeat's end (0x9D) or a repeat's exit (0x9E). The event before
// it says how it began: a repeat's end begins a loop, and the others a jump. An event that waits for the channel's
// voices (0x95) runs again in later ticks, in the same stretch. A stretch's depth counts the repeats that the track
// runs inside. The performance also holds the event of each MIDI note and the slot of the track that ran it, at the
// note's ID less 1 (TrackEvent::note). Offsets count from the start of the file of the song or of the bank of sound
// effects. A performance that stops at the end of a pass leaves out the stretches that begin in the end's tick: they
// belong to the next pass.
//
// Each track's changes of its channel's volume and pan come in the order of their ticks, with each setting's value at
// the end of its tick. The volume runs from 0 to 127: the value of the channel's volume fade (0xE0 and 0xE1). The pan
// runs from -64 for the left to 63 for the right: the channel's pan, with the synth's. The changes count from a volume
// of 127 and the centre. Tracks that play on one channel take the same changes. DSE has no sends.
struct Performance : PerformanceBase
{
    std::vector<PresetKey> presets;

    std::vector<std::vector<Stretch>> stretches;
    std::vector<NoteCommand> note_commands;
    std::vector<uint8_t> note_tracks;
    std::vector<NoteVoices> note_channels; // at the note's ID less 1
    std::vector<std::vector<MixChange>> mix;
    std::vector<UnstartedNote> unstarted;
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;
    ntdlib::dse::Build build = ntdlib::dse::Build::kLater; // the build of DSE's driver (dse::BuildOf)
};

// Plays the song or the sound effect of `files` and records it. Throws std::invalid_argument for files with neither.
// A sound effect that the driver refuses records nothing, with a warning. Throws dse::Hang when a track hangs the
// driver.
Performance Perform(const SequenceFiles& files, const PerformOptions& options = {});

} // namespace ntdlib::sequence_export::dse
