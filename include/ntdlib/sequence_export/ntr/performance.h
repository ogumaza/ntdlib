// SPDX-License-Identifier: MIT

// The performance of a sequence of a DS sound archive (SDAT) for MIDI, a SoundFont and SFZ files. The model of the
// DS's sound driver (ntdlib::ntr::snd::Driver) plays the sequence on an ArchivePlayer. The performer records what the
// driver plays: each note with its channel, the settings of each track, and a preset for each program with the track's
// envelope and LFO. The driver's model follows the console's driver frame by frame. The notes that the driver drops
// for want of a channel are therefore the ones that the console drops, and so are the notes whose channels another note
// takes.
//
// The driver follows the sequence's loops (snd::Driver::FollowLengths). The performance plays the main loop the
// number of times that the options give, and marks the pass that a player looping on the markers repeats as the game
// plays the next pass (ntdlib/sequence_export/passes.h).

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sequence_export/linear_pan.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::ntr
{

// The events of a performance's tracks, from seqlib.
using EventKind = seqlib::midi::EventKind;
using TrackEvent = seqlib::midi::TrackEvent;

// The settings of a note that a MIDI channel can't hold. Each distinct key makes a SoundFont preset.
struct PresetKey
{
    bool operator==(const PresetKey&) const = default;

    uint16_t program = 0;

    // The track's envelope (0xD0 to 0xD3), or 0xFF where the region's value applies.
    uint8_t attack = 0xFF;
    uint8_t decay = 0xFF;
    uint8_t sustain = 0xFF;
    uint8_t release = 0xFF;

    // The track's LFO at the note's start: its target (0 pitch, 1 volume, 2 pan), its speed, its range and its delay in
    // the driver's frames. CC1 holds its depth.
    uint8_t lfo_target = 0;
    uint8_t lfo_speed = 16;
    uint8_t lfo_range = 1;
    uint16_t lfo_delay = 0;
};

// A note's place on the driver's channels, for an editor.
struct NoteChannel
{
    bool operator==(const NoteChannel&) const = default;

    int16_t channel = -1; // the driver's channel that plays it: 0 to 15, or one that the driver grew (GrowChannels)

    // The tick where a later note took the channel before the note's length ran out. A channel without a length plays
    // its note until the channel releases it or its wave ends. The driver gives no length to a tie's channel or to the
    // channel of an instrument whose release is 0xFF.
    std::optional<uint32_t> cut_tick;
};

// A note that the driver never started, for an editor: it found no channel, or its program plays nothing at its key.
struct UnstartedNote
{
    bool operator==(const UnstartedNote&) const = default;

    uint32_t tick = 0;
    uint32_t length = 0; // the note command's length in ticks, or 0 for a note that lasts until its track releases it
    uint8_t track = 0;
    uint8_t key = 0; // after the transpose
    uint8_t velocity = 0;
    uint32_t offset = 0; // the note command's offset, as NoteCommand's
};

// A recorded sequence: PerformanceBase, with its presets and the bank that they play.
//
// For an editor, it also holds each track's stretches of commands, in the order that they ran, and the note command of
// each MIDI note, at the note's ID less 1 (TrackEvent::note). A stretch begins where its track opens, and at each
// command that doesn't follow the one before it: after a jump, a call, a return or a loop's end. The command before it
// says how it began. Its depth counts the calls and loops on the track's stack. Offsets count from the start of the
// sequence's data, prefixes included (snd::NoteEvent::offset). ntdlib::ntr::snd::DecodeSequence decodes the bytes from
// there. A performance that stops at the end of a pass leaves out the stretches that begin in the end's tick: they
// belong to the next pass.
//
// Each MIDI note's place on the driver's channels lies at the note's ID less 1 too. A note that finds every channel
// that its track may take playing takes the channel of the lowest priority, when its priority is as high. The note
// there then ends, sooner than its length when its length hasn't run out. Otherwise it gets no channel, and the
// performance lists it among the notes that the driver never started, in the order of their ticks. So does a note whose
// program plays nothing at its key. The MIDI file holds none of those.
//
// Each track's changes of its volume and pan come in the order of their ticks, with each setting's value at the end of
// its tick. The volume runs from 0 to 127, and the pan from -64 for the left to 63 for the right. A track starts at
// volume 127 and the centre. The DS has no sends.
struct Performance : PerformanceBase
{
    std::vector<PresetKey> presets;
    uint16_t bank = 0; // the sequence's bank in the archive

    std::array<std::vector<Stretch>, 16> stretches;
    std::vector<NoteCommand> note_commands;
    std::vector<NoteChannel> note_channels;
    std::vector<UnstartedNote> unstarted;
    std::array<std::vector<MixChange>, 16> mix;
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;

    // The state of the driver's random numbers when the sequence starts. The driver starts with 0x12345678.
    uint32_t seed = 0x12345678;

    // Values for the player's variables (0-15) and the global variables (16-31), set before the sequence starts. Some
    // sequences play nothing until the game sets one of their variables.
    std::map<uint8_t, int16_t> variables = {};

    // A game's changes of those variables while the sequence plays. On the DS, a change's frame counts the driver's
    // frames from the sequence's start: frame 0 is the first frame after ArchivePlayer::Start. The driver takes each
    // change before the frame's ticks, as ArchivePlayer::Send does. The performance plays on past the last change, and
    // its loop starts after it.
    std::vector<VariableChange> changes = {};

    // The channels that the archive's players give their sequences' tracks, by the player's number, the channels of
    // some tracks, and the capture units' reverb, where a game's code sets them as the sequence starts
    // (ntdlib::ntr::GameSettings). With a reverb, each part's CC91 holds the reverb's volume. A synth's reverb stands
    // in for the DS's echo.
    std::map<uint32_t, uint16_t> player_channels = {};
    std::vector<ntdlib::ntr::TrackChannels> track_channels = {};
    std::optional<ntdlib::ntr::Reverb> reverb = {};

    // The driver's commands that a game's code sends once it has started the sequence, in their order: track volumes,
    // mutes, pitch and pan, or the tempo's ratio. The driver runs them in the sequence's first frame, after its start.
    // The sequence plays on the driver's player 0.
    ntdlib::ntr::snd::CommandList commands = {};

    // The build of the driver (snd::Build).
    ntdlib::ntr::snd::Build build = ntdlib::ntr::snd::kDefaultBuild;
};

// The pan of the DS's mixer is linear (ntdlib/sequence_export/linear_pan.h).
using ntdlib::sequence_export::PanGain;
using ntdlib::sequence_export::PanPosition;

// Plays `sequence` of `archive` and records it. Throws xsflib::FormatError when the archive lacks the sequence, its
// file or its bank, and snd::Hang when a track hangs the driver.
Performance Perform(const sdat::Archive& archive, const ntdlib::ntr::SequenceRef& sequence,
                    const PerformOptions& options = {});

using seqlib::midi::NoteCount;

} // namespace ntdlib::sequence_export::ntr
