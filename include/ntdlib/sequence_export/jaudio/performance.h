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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "seqlib/midi/performance.h"

#include "ntdlib/sequence_export/jaudio/conductor.h"
#include "ntdlib/sequence_export/jaudio/envelope.h"
#include "ntdlib/sequence_export/jaudio/sound_data.h"
#include "ntdlib/sequence_export/jaudio/track_filter.h"
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

    // How a JAudio1 note's pan takes the bank's pan and the instrument effects' share of it (TChannel::calcPan). 0
    // leaves a value out and 1 adds it. A weight between comes from the pan powers. Pikmin 2's build counts the
    // effects' share as part of the bank's pan (0x80099464 in its main.dol). No note of Pikmin 2 on the GameCube
    // weights the two apart.
    float bank_pan_weight = 1.0f;
    float effect_pan_weight = 1.0f;

    // The entry of the track's IIR filter at note-on, where the zones play it (SequenceSound::zone_filters).
    uint8_t track_filter = kOpenTrackFilter;
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

    // The zones that a region becomes under each PresetKey::track_filter of `presets`, in the SoundFont and in SFZ
    // (TrackFilterLayers).
    std::map<uint8_t, std::vector<FilterLayer>> filter_layers;
    std::map<uint8_t, std::vector<FilterLayer>> sfz_filter_layers;

    // A bit for each port of the root track that the sequence read while the port held its first value: the sound's
    // (SequenceSound::ports) or the options' (PerformOptions::ports). A read by another track of the root's port
    // counts. Once the sequence writes a port, a read of it doesn't.
    uint16_t ports_read = 0;
};

// Options for playing a sequence.
struct PerformOptions
{
    // Main loop repetitions, at least 1. The default plays the intro and one loop, with MIDI loop markers.
    int loops = 1;

    // The state the game's random generator starts in (JMath::TRandom_fast_, which JAudio2's register command seeds
    // with 0). A JAudio1 sequence draws from JAudio1's generator instead. That one starts in the game's state at
    // start-up, and the seed doesn't change it (see SequenceSound::draws). A sound with SequenceSound::timed_random
    // draws from JAudio2's generator.
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
//
// Pikmin's sound library makes another request (EffectRequest::kPort). Its track for the effect's kind is the first
// track's child `slot`. The library writes the effect's number to the track's port `port`. The track's interrupt for
// the write reads the number and opens a track below it that plays the effect. The request counts as taken once the
// track has read the port. The effect has ended when no track below the track is open. The library can write other
// ports of the track first (SequenceSound::ports).
enum class EffectRequest : uint8_t
{
    kSlot,
    kPort,
};

struct EffectStart
{
    uint8_t slot = 0;
    uint16_t number = 0; // the effect's offset number (JAIBasic::getSoundOffsetNumberFromID)
    EffectRequest request = EffectRequest::kSlot;
    uint8_t port = 0; // for kPort
};

struct SequenceSound;

// What a companion does to one of its sounds in a game frame (Companion::Frame).
struct CompanionSound
{
    enum class Action : uint8_t
    {
        kStart, // stops the voice's sound, then starts `sound` at `level` with `ports` on its first track
        kStop,  // stops the voice's sound
    };

    Action action = Action::kStart;
    std::size_t voice = 0;
    const SequenceSound* sound = nullptr;
    float level = 1.0f;
    std::map<uint8_t, uint16_t> ports;
};

// The game's code that starts sounds beside a sequence as it plays, from what the sequence does: Donkey Kong Jungle
// Beat's chord effects (ntdlib/jaudio/chord_layer.h). The performer passes it the notes with a voice above 0 of the
// root's child 0 and of that track's child 0, muted or not. Once each game frame, after the sub-frames of the frame,
// it asks for the frame's sounds. They start in the next sub-frame. Their tracks record after the sequence's, and their
// loops and ends don't count for the sequence's.
class Companion
{
public:
    virtual ~Companion() = default;

    // A copy in the same state, for a probe that plays on from a copy of the performance.
    virtual std::unique_ptr<Companion> Clone() const = 0;

    virtual void Note(uint8_t key, uint8_t velocity) = 0;

    // `port(child, p)` reads port p of child `child` of the root's child 0, or nothing while that track isn't open.
    virtual std::vector<CompanionSound> Frame(const std::function<std::optional<uint16_t>(int, int)>& port) = 0;
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
    float mix_level = 1.0f;                      // GameRules::mix_level

    // The sequence is JAudio1's: its first command set, with the registers, interrupts and tracks of JAudio1's player
    // (JASystem::TSeqParser and TTrack in The Wind Waker's main.dol).
    bool jaudio1 = false;

    // The tracks that play (GameRules::start_states). JAISeq::getChildTrack numbers the same tracks: 16 * n + m for
    // child m of the root's child n. A game's code can mute such a track through the sound's handle after startSound,
    // before the sound's first tick (JASTrack::mute). The track keeps the mute when the sequence opens it. A muted
    // track and the tracks below it start no notes. When the sequence opens the track again, JASTrack::openChild clears
    // the mute.
    TrackMask tracks = kEveryTrack;

    // The volumes that the game's code gives tracks through the sound's handle (GameRules::start_states). A track that
    // the map leaves out plays at 1. Index n is the root track's child n in a JAudio1 sequence
    // (JAISound::setTrackVolume, 0x802999f4 in The Wind Waker's main.dol). In a JAudio2 sequence it is the track that
    // JAISeq::getChildTrack(n) finds: child n % 16 of the root's child n / 16 (Z2SeqMgr::setChildTrackVolume,
    // 0x802b3fec in Twilight Princess's main.dol). The volume belongs to the sound, not to the track. It holds each
    // time the sequence opens the track. A track at 0 stays silent, and so do the tracks below it. In JAudio2, another
    // volume multiplies the track's squared volume (JASTrack::updateTrack reads it from the track's external buffer).
    // In JAudio1, a volume between 0 and 1 plays as 1.
    //
    // In a sequence of Pikmin's library (SequenceSound::jac), index n is child n of the first track. Pikmin's code
    // gives each child an external buffer that holds its volume (Jam_AssignExtBufferP and Jam_SetExtParamP). The
    // child's volume is multiplied by the buffer's (Jam_UpdateTrack, 0x80010e20 in the USA release's main.dol). The
    // product then reaches the tracks below the child. A child at 0 starts no notes. A child without a value keeps its
    // volume. GameRules::child_volumes sets these.
    std::map<uint8_t, float> child_volumes = {};

    // The tempo ratio that the game's code gives the sound (JAISound::setTempoProportion, 0x80298acc). The ticks run
    // that much faster.
    float tempo_ratio = 1.0f;

    // The ports that the game's code writes to the root track once the sequence runs (JAISound::setPortData,
    // 0x80298b74). A game writes them while its music plays, such as when a boss fight moves on. The performance
    // writes them after the first tick, the soonest a game's write can reach a sequence that clears the port in its
    // first commands. PerformOptions::ports are set before the first tick.
    std::map<uint8_t, uint16_t> game_ports = {};

    // The statuses that the game's code writes to port 9 of the root track in each of its frames, by the value of port
    // 10. A sequence may give its section on port 10. Z2SeqMgr::fieldBgmFramework (0x802b594c) works this way. A game
    // that starts a sound writes in the same frame, so the first tick reads a status for port 10's first value, 0.
    std::map<uint16_t, uint16_t> status_by_section = {};

    // The game's frames over which the sound fades in from silence as its sequence starts, or 0 for none. A start can
    // give a sound a fade time. JAudio reads it once the sequence's data is in (JAInter::SequenceMgr::checkReadSeq,
    // 0x8029706c). The Wind Waker's subBgmStart and bgmNowBattle give some of its music 60 frames, 2 seconds.
    uint16_t fade_in = 0;

    // Whether the game's JASystem squares a JAudio1 track's volume in volume mode 0 (GameRules::squared_track_volume).
    bool squared_track_volume = true;

    // Whether a JAudio1 track's routes give its notes its oscillators (GameRules::track_oscillator_routes).
    bool track_oscillator_routes = true;

    // The answers of the game's sequence callback to JAudio2's command 0xf9, by the command's argument
    // (GameRules::callback). The callback answers 0 to an argument that isn't here. Without a callback, the command's
    // handler writes 0xffff.
    std::optional<std::map<uint16_t, uint16_t>> callback = std::nullopt;

    // The JAudio1 sequence is Pikmin's, for its earlier sound library. Addresses are in the USA release's main.dol. The
    // library's table of commands (0x80225124) gives 0xd0 two 16-bit arguments and 0xd3 two registers. It gives 0xe5,
    // 0xe6, 0xf3 and 0xf4 none. 0xd0 and 0xe5 give the track a name that the game's code finds it by, and 0xe6 takes
    // the name away (0x80010500 and 0x800105c0). The other three do nothing. A track has 32 registers of 16 bits
    // (0x800100a0). Each of the address registers 0x28 to 0x2b holds two of them (0x80010280): 0x28 holds 16 in its
    // high half and 17 in its low half, 0x29 holds 18 and 19, and so on.
    bool jac = false;

    // The JAudio1 sequence plays under the later JAudio1 of Pikmin 2 (JASTrack, in the USA release's main.dol). Its
    // tracks have 32 registers, and the address registers hold registers 16 to 23 as Pikmin's library's do
    // (SequenceSound::jac). JASTrack keeps its registers at +0x268 and reads address register 0x28 from +0x288, where
    // registers 16 and 17 are (JASTrack::writeRegDirect, 0x800a1a24, and readReg32, 0x800a1778). The cave music opens
    // a phrase at the offset that registers 16 and 17 hold. A track's tick also counts down the wait that a running
    // interrupt put aside (JASTrack::mainProc, 0x8009f544). The Wind Waker's library leaves that wait as it is. The
    // root tracks of Pikmin 2's music wait between their polls while an interrupt runs every tick. A tempo or a
    // timebase reaches the tracks' tick rates at once, and a child opens with its parent's rate (JASTrack::setTempo,
    // 0x800a259c, and openChild, 0x800a155c). The lengths of the notes follow those rates. GameRules::later_jaudio1
    // sets this.
    bool later_jaudio1 = false;

    // The game's sequence callback mutes child 15 of any track when the child calls it with ID 0 (command 0xe7). Super
    // Mario Sunshine's callback does so (0x80015bc0 in the USA release's main.dol). The child then starts no notes
    // (0x8031a83c). The tracks that the child opens later take its mute (0x8031cb44). When the sequence opens the
    // child again, the mute is cleared (0x8031a3b4). GameRules::mute_child_15 sets this.
    bool mute_child_15 = false;

    // The answers that the game's code gives the sequence's calls to the game (command 0xe7), by the call's ID. A call
    // that isn't listed gets 0, as from The Wind Waker's callback (JAIBasic::setParameterSeqSync). Pikmin 2's sound
    // layer answers through the object that it makes for each piece (PSGame::seqCpuSync, 0x803372e0 in the USA
    // release's main.dol). GameRules::sync_answers sets these.
    std::map<uint16_t, uint16_t> sync_answers{};

    // The children of the first track that the game's code mutes as they start, a bit for each child's number. A muted
    // child and the tracks below it start no notes. Pikmin 2's directors mute the tracks of the layers that follow the
    // game's action when each track first calls the game (TrackOnDirectorBase::onPlayInit, 0x8045732c in the USA
    // release's main.dol). The tracks stay muted until the game's action turns them on. GameRules::muted_children sets
    // these.
    uint16_t muted_children = 0;

    // The conductor of one of Pikmin 2's pieces of cave music (PSAutoBgm::AutoBgm), or nothing. The conductor answers
    // the calls of the piece's tracks before SequenceSound::sync_answers does. It gives the first track its tempo and
    // the time between its calls. For each child of the first track, it picks the phrases and rests that the child
    // plays, at random, and sets the child's bank, program, volume, pan and transpose. The game draws its random
    // numbers from PSSystem::oRandom (0x80515edc in the USA release's main.dol). Nothing else draws from it, and it
    // starts at 0. GameRules::conductors sets this.
    std::optional<Conductor> conductor = std::nullopt;

    // For a variant of a sound that differs only in some of `ports` (GameRules::port_variants): a bit for each of those
    // ports. The variant counts only if its sequence reads one of them (Performance::ports_read). Otherwise it plays as
    // the sound does. 0 for a sound that isn't such a variant.
    uint16_t variant_ports = 0;

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

    // The numbers that the sound library draws from its random generator before the sound starts. In the game, other
    // sounds draw numbers too, and the sound's first draw comes later in the generator's run.
    uint32_t draws = 0;

    // Whether the game's JAudio1 generator mixes the CPU's time base into each number (GameRules::timed_random). A
    // JAudio1 sound then draws from JAudio2's generator, from PerformOptions::seed.
    bool timed_random = false;

    // The bank that a JAudio1 sequence's root track starts with, in the high byte of register 6. The program keeps its
    // first value. The sound library starts the track with bank 0. Pikmin's code gives a piece another (GameRules).
    uint16_t bank = 0;

    // The values that the game's code writes to the root track's ports as it starts the sequence. PerformOptions::ports
    // take the place of these. For an effect of EffectRequest::kPort, the values go to the effect's track before the
    // effect's number.
    std::map<uint8_t, uint16_t> ports{};

    // Whether the zones play the tracks' IIR filters. Music does. A sound effect leaves them out and reports them.
    bool zone_filters = false;

    // The rate at which the game runs those filters, in Hz (GameRules::filter_rate).
    double filter_rate = kOutputRate;

    // Makes the companion of a performance, or nothing (see Companion). Each run of the performance makes another. The
    // sounds that the companion starts have to outlive the performance.
    std::function<std::unique_ptr<Companion>()> companion = {};

    // The length of the game's frame, in seconds, for the companion: a video retrace of 59.94 Hz.
    double frame_seconds = 1001.0 / 60000.0;
};

// Play `sound`. `program` is the name that the messages give the program, such as "xyz2midi". Throw FormatError if
// the sequence can't be read.
Performance Perform(const SequenceSound& sound, BankSet& banks, const PerformOptions& options,
                    const std::string& program);

using seqlib::midi::NoteCount;

} // namespace ntdlib::sequence_export::jaudio
