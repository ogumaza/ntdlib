// SPDX-License-Identifier: MIT

// Run BMS sequences as JAudio does and record note, controller and preset events for MIDI output.
//
// Jumps, calls, registers, ports and conditions require execution rather than static translation. The performer follows
// JAudio2's sequence player as Four Swords Adventures links it: the parser's command table, the tracks with their timed
// parameters, and the channels that its sub-frame updates move on. The parts that JAudio2 shares with Twilight
// Princess's build follow that, and the instruments follow The Wind Waker's JAudio. A JAudio1 sequence plays as The
// Wind Waker's player plays it: its first command set, its registers, its interrupts and its tracks. Its notes play on
// JAudio2's channels.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/performance_base.h"

namespace ntdlib::sequence_export::jaudio
{

class BankSet;

// The vibrato depth that CC1's full value gives, in cents. CC1 holds 128 times the depth over this. The performer
// records CC1 that way, and the SoundFont's and SFZ's modulators read it that way.
constexpr double kVibratoRangeCents = 100.0;

// Note-on settings that a MIDI channel can't represent. Each distinct PresetKey makes a SoundFont preset.
struct PresetKey
{
    bool operator==(const PresetKey&) const = default;

    uint32_t bank = 0;
    uint32_t program = 0;

    // The track's oscillators that replace the instrument's (envset and simpleadsr), as indexes into
    // Performance::oscillators, or -1 for the instrument's.
    int volume_oscillator = -1;
    int pitch_oscillator = -1;

    uint16_t direct_release = 0; // the track's direct release, 0 for none

    // The vibrato's speed and delay at note-on. Its depth goes by CC1.
    float vibrato_rate = 1.0f;  // in 72nds of a cycle per sub-frame
    uint16_t vibrato_delay = 0; // in sub-frames
};

// The events of a performance's tracks, from seqlib.
using EventKind = seqlib::midi::EventKind;
using TrackEvent = seqlib::midi::TrackEvent;

// Recorded sequence. ntdlib's PerformanceBase holds seqlib's part (the tempo, the tracks' events and names, each tick's
// sub-frame, the length and the main loop) and what the performance found. This part holds the presets.
struct Performance : ntdlib::sequence_export::PerformanceBase
{
    std::vector<PresetKey> presets;
    std::vector<Oscillator> oscillators; // the tracks' oscillators that presets name
    uint16_t default_release = 0;        // the sound's SequenceSound::default_release

    // The indexes in `presets` of the presets that play a key past the DSP's pitch limit (PresetZones's limit_pitch).
    std::set<uint32_t> pitch_limited_presets;
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;

    // The state the game's random generator starts in (JMath::TRandom_fast_, which JAudio2's register command seeds
    // with 0).
    uint32_t seed = 0;

    // Values for the sequence's ports (0-15), set before it starts. Games write ports to steer their music, and some
    // sequences read one when they start. An effect's values go to the ports of its slot (SequenceSound::effect).
    std::map<uint8_t, uint16_t> ports;

    // The voices that notes play on, from --voices. With a count, a note that finds no voice free takes the voice of an
    // older note of no higher priority, or is lost, as in the game with that many voices. The game has 64. With 0,
    // every note is kept.
    int voice_limit = 0;
};

// An echo that a game sets up in the DSP program at start-up (JASDsp::setFXLine). The line delays the voices' effect
// sends and passes them through a filter. The filter's output feeds back into the line and goes to the mix.
struct EchoLine
{
    uint32_t delay = 0;       // in sub-frames
    float feedback = 0.0f;    // the filter's gain
    float volume = 0.0f;      // the output's level in the mix
    float voice_level = 0.0f; // the game's output of a voice at a volume of 1, as a share of the voice's samples
};

// The tracks of a sound that play (SequenceSound::tracks). Element n holds a bit for each child of the root track's
// child n: bit m for child m.
using TrackMask = std::array<uint16_t, 16>;

// Every track plays.
inline constexpr TrackMask kEveryTrack = {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
                                          0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff};

// An effect that a JAudio1 game plays in its effects sequence (GameOptions::jaudio1_effects). The game's sound library
// plays the sequence from start-up. Each category of effects owns a run of the sequence's slots. Slot n is child n & 15
// of the first track's child n >> 4. The library starts an effect in the first slot of its category, as
// JAISeMgr::checkPlayingSe (0x80293c94 in The Wind Waker's main.dol) does. It writes four of the slot's ports: 3, a
// wait for the distance (0 without a camera), 6, the low half of the map's information (0 without a map), 4, the
// effect's number, and 0, the value 1. The slot's track then plays the effect. The slot is free again when its port 2
// holds 0 and its port 0 doesn't hold 1.
struct EffectStart
{
    uint8_t slot = 0;
    uint16_t number = 0; // the effect's offset number (JAIBasic::getSoundOffsetNumberFromID)
};

// A sequence to play: its data, the offset its first track starts at, the sound's volume from the sound table and its
// category, the release that the game's sound library gives an oscillator without one, the game's echo, the
// generation of JAudio that plays it, the tracks that the game lets play, and the effect it starts, if any.
struct SequenceSound
{
    std::string name;
    std::span<const uint8_t> data;
    uint32_t start = 0;
    float volume = 1.0f;
    uint16_t default_release = 0;                // GameRules::default_release
    std::optional<EchoLine> echo = std::nullopt; // GameRules::echo

    // The sequence is JAudio1's: its first command set, with the registers, interrupts and tracks of JAudio1's player
    // (JASystem::TSeqParser and TTrack in The Wind Waker's main.dol).
    bool jaudio1 = false;

    // The tracks that play (GameRules::tracks). JAISeq::getChildTrack numbers the same tracks: 16 * n + m for child m
    // of the root's child n. A game's code can mute such a track through the sound's handle after startSound, before
    // the sound's first tick (JASTrack::mute). The track keeps the mute when the sequence opens it. A muted track and
    // the tracks below it start no notes. When the sequence opens the track again, JASTrack::openChild clears the
    // mute.
    TrackMask tracks = kEveryTrack;

    // The effect that the sequence plays, for an effects sequence. The sequence then plays from its start. The effect
    // starts after the tick that opens its slot's track. That tick has to come in the first sub-frame that runs ticks.
    // Otherwise the effect plays nothing. Only the slot's track and the tracks below it count for the sequence's loops
    // and its end. The performance ends when the slot is free again. PerformOptions::ports go to the slot's track with
    // the library's writes, as the game's code passes values with the effect. A value for port 6 stands for the map's
    // information. The library's values for ports 0, 3 and 4 take the place of any others.
    std::optional<EffectStart> effect = std::nullopt;

    // A factor on the pitch of every note: a JAudio1 effect's pitch in the sound table (ntdlib::jaudio::SoundEntry's
    // pitch). The sound library gives it to the effect's track. It moves the notes' pitch as a bend does.
    float pitch = 1.0f;
};

// Play `sound`. `program` is the name that the messages give the program, such as "xyz2midi". Throw FormatError if
// the sequence can't be read.
Performance Perform(const SequenceSound& sound, BankSet& banks, const PerformOptions& options,
                    const std::string& program);

using seqlib::midi::NoteCount;

} // namespace ntdlib::sequence_export::jaudio
