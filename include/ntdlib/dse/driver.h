// SPDX-License-Identifier: MIT

// A model of the ARM9's part of Procyon Studio's sound driver for the DS (DSE). The driver's thread runs a tick every
// 10 ms. A tick runs the song's sequence and those of the sound effects, then each voice's envelope and LFOs, and sends
// the ARM7's SND driver a list of commands for the hardware channels. The model keeps the driver's state as the driver
// keeps it, and follows the driver's arithmetic, the widths of its fields included. The addresses in the comments are
// those in Pokémon Mystery Dungeon: Explorers of Sky's ARM9 binary (North America).
//
// A song and each sound effect play as a sequence. Each track of a sequence plays on a channel of the sequence's
// synth. A channel holds the bank, the program, the volume, the pan and the LFO settings that its tracks' events set.
// A note takes a voice for each split of the channel's program that holds the note's key and velocity. Voice n plays
// on hardware channel n, with its envelope and up to 4 LFOs. The sound effects play on a pool of sequences that the
// driver sets up at its start: kEffectSequences of them, each with kEffectTracks tracks and as many channels.
//
// The model plays songs and sound effects. It doesn't play streams or the driver's capture of its output.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/dse/formats.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::dse
{

// The most commands that a track runs in one tick. The driver has no such limit. A track that loops without waiting
// runs for ever there, and the console hangs. The model's tick stops and throws a Hang instead. A sequence that doesn't
// hang runs far fewer commands in a tick.
constexpr int kCommandBudget = 100000;

// What a tick throws when a track runs kCommandBudget commands in it: the sequence would hang the console. The driver's
// state is then that of the commands run so far. Restore a snapshot of it, or start again.
class Hang : public std::runtime_error
{
public:
    Hang(int track, uint32_t offset);

    // The track's number in its sequence.
    int TrackIndex() const;

    // The offset in the sequence's file of the command that the track would run next.
    uint32_t Offset() const;

private:
    int track_;
    uint32_t offset_;
};

// How a song or a sound effect starts (0x0206DB7C and 0x0206DD70). `flags` selects the values that the start sets:
// 1 the tempo, 2 the bend, 4 the volume and 8 the pan. Each value fades to its target over its time, in milliseconds.
// A time of 0 sets it at once. The sequence's synth keeps a value that isn't selected: a volume of 127 and a pan of
// 0x40 after the sequence's reset. The tempo scales the tempo of the sequence's events by 1/256. 0 counts as 0x100.
struct StartParameters
{
    uint8_t flags = 4;
    int16_t delay_ms = 0; // the time before the sequence's first tick. A negative time counts as 0.
    int16_t tempo = 0x100;
    uint16_t tempo_ms = 0;
    int16_t bend = 0;
    uint16_t bend_ms = 0;
    int16_t volume = 127;
    uint16_t volume_ms = 0;
    uint16_t pan = 0x40; // the low byte
    uint16_t pan_ms = 0;
};

// A note that a track of the followed sequence plays, for a performer (SequenceHooks). The driver has no such event.
struct NoteEvent
{
    uint32_t number = 0; // the driver's notes count from 1
    int track = 0;       // the track's slot in the sequence
    int channel = 0;     // the channel of the sequence's synth
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint16_t voices = 0; // the voices that the note started, a bit for each
    bool legato = false; // the note went on in the voices of the track's previous key (0x02073E10), and started none
    uint32_t offset = 0; // the note's code, in the file of the song or of the bank of sound effects
    uint32_t length = 0; // the note's length in ticks, after the track's multiplier
};

// An event that a track of the followed sequence runs, for a performer (SequenceHooks). The driver has no such event.
struct EventStart
{
    int track = 0;       // the track's slot in the sequence
    uint32_t offset = 0; // the event's code, in the file of the song or of the bank of sound effects
    uint32_t next = 0;   // the offset after the event's arguments, or after its code for a code that stops the track
    uint8_t code = 0;
    uint8_t repeats = 0; // the repeats (0x9C) that the track runs inside
};

// An LFO's settings: a program gives them to its channel, and a channel's events change them. `type` 0 is off. Its
// output is 1 the pitch, 2 the volume and 3 the pan. A cycle takes `phase_ms`.
struct LfoValues
{
    friend bool operator==(const LfoValues&, const LfoValues&) = default;

    uint8_t type = 0;
    uint8_t output = 0;
    uint8_t waveform = 0;
    int32_t amplitude = 0;
    uint16_t phase_ms = 0;
    uint16_t delay_ms = 0;
    uint16_t envelope_ms = 0;
};

// An envelope's values: a split's, or those that a channel's events set over them. The values above 0x7F of an
// override leave the split's.
struct EnvelopeValues
{
    friend bool operator==(const EnvelopeValues&, const EnvelopeValues&) = default;

    uint8_t multiplier = 0xFF;
    uint8_t attack_begin = 0xFF;
    uint8_t attack_time = 0xFF;
    uint8_t decay_time = 0xFF;
    uint8_t sustain_level = 0xFF;
    uint8_t hold_time = 0xFF;
    uint8_t sustain_time = 0xFF;
    uint8_t release_time = 0xFF;
};

// What a performer reads of a channel of the followed sequence's synth.
struct ChannelView
{
    friend bool operator==(const ChannelView&, const ChannelView&) = default;

    uint16_t bank = 0;
    uint16_t program = 0;     // the program's slot in the bank
    bool has_program = false; // the channel's bank holds its program
    uint8_t volume = 0;       // the value of the channel's volume fade (0xE0 and 0xE1)
    uint8_t expression = 0;   // 0xE3
    int8_t synth_volume = 0;  // the synth's volume with the global volume's
    int16_t pan = 0x40;       // the channel's pan with the synth's: a voice adds its split's pan, less 0x40
    uint16_t bend = 0;        // the tuning, the bend and the synth's bend, in 256ths of a semitone
    int16_t key_bend = 0;     // the bend of the key bend (0xD7), over its range
    uint8_t bend_range = 0;   // in semitones, or 0 for the split's
    bool damper = false;      // the channel holds its voices past their keys' release
    int16_t jitter = 0;       // the range of the notes' random changes of pitch (0xD6)
    bool random_keys = false; // the notes take random keys (0xD5)
    int8_t lfo_level = 0;
    std::array<LfoValues, 4> lfos = {};
    EnvelopeValues envelope = {}; // the channel's override
};

// What a performer reads of a voice.
struct VoiceView
{
    friend bool operator==(const VoiceView&, const VoiceView&) = default;

    bool allocated = false; // the voice plays for a channel
    bool followed = false;  // for a channel of the followed sequence's synth
    int channel = -1;       // that channel
    int track = -1;         // the track whose note started it, of the sequence that played the note
    uint32_t note = 0;      // the number of the note that started it (NoteEvent)
    uint8_t key = 0;
    bool started = false;   // the voice's envelope runs: the voice sounds or releases
    bool releasing = false; // the envelope releases
    uint8_t level = 0;      // the envelope's level, from 0 to 127
    int16_t key_bend = 0;   // the key bend (0xD7) over the voice's range, in 256ths of a semitone
    int16_t jitter = 0;     // the note's random change of pitch (0xD6), in 256ths of a semitone
    int8_t wave = 0;        // the wave's index in the bank
    // The ticks in which the envelope releases to 0: the rest of its release, or a whole release from `level`. The
    // level falls in a straight line. Empty for a release that never ends, and for a voice without an envelope.
    std::optional<int32_t> release_ticks;
    // The split's key group, and the voices of the channel that the group allows, or 0 for any number. A note of a
    // group that allows one voice takes the voice of the group's last note on the channel.
    uint8_t key_group = 0;
    uint8_t key_group_voices = 0;
};

// Hooks for a performer, of the sequence that it follows (Driver::Follow): each runs inside Driver::Tick.
struct SequenceHooks
{
    std::function<void()> on_tick;                   // the start of each of the sequence's ticks, before its notes end
    std::function<void(const EventStart&)> on_event; // a track of the sequence runs an event, before the event
    std::function<void(const NoteEvent&)> on_note;   // a note of the sequence
    std::function<void(int voice)> on_release;       // a voice of the sequence releases
    std::function<void(int track)> on_loop_start;    // a track of the sequence passes its loop's start (0x99)
    std::function<void(int track)> on_loop;          // a track of the sequence goes back to its loop's start (0x98)
};

// The builds of the driver. The earlier build keeps no list of the notes that its channels hold. Each voice counts the
// ticks of its note and releases itself. A new note releases no voice of its key, and a legato moves the key of one
// voice. The earlier build stops the voices of a channel as it frees them. Each freed voice stays in the stops of the
// lists that follow until the voice starts again. Each list ends with a start and a stop of channels, empty or not. Its
// sequences have no delay, and step their fades on every tick. A sequence that has faded out stops before its fades
// step. A change of a synth's volume, pan or bend only marks the synth. Its channels take the change when the voices
// update. The later build stops a freed voice once, with the list's stop. It sends the start and the stop only when
// they name a channel.
enum class Build
{
    kLater,
    kEarlier,
};

// The build of DSE in an ARM9 binary, as the image holds it. The earlier build's voices count down the ticks of their
// notes in a word at offset 0x14C (0x02062A00 in Explorers of Time's ARM9 binary, North America). A binary without
// that code, or with the code compressed, gives the later build.
Build BuildOf(std::span<const uint8_t> arm9);

class Driver
{
public:
    // The voices and the length of a tick, in microseconds, that the driver starts with.
    static constexpr int kVoices = 16;
    static constexpr int kTickMicroseconds = 10'000;

    // The pool of sequences of the sound effects, as the driver's default settings give it (0x0206C1C0).
    static constexpr int kEffectSequences = 4;
    static constexpr int kEffectTracks = 4;

    // The driver of `build` after its start-up (0x0206C3E0). A copy goes on from the same state, with the same hooks
    // and the same banks' files.
    explicit Driver(Build build = Build::kLater);
    ~Driver();
    Driver(const Driver& other);
    Driver& operator=(const Driver& other);

    // Adds `bank` to the driver's banks, as the driver loads a bank (0x0206D108), and returns the bank's id. The
    // driver refuses a bank whose id it holds already, and the function then returns nothing. `samples` gives the
    // address of the bank's samples in main memory, for a bank that holds them. The driver keeps a reference to `bank`.
    std::optional<uint16_t> AddBank(const WaveBank& bank, uint32_t samples);

    // Places the waves of bank `id` that its main bank holds, from `address` on, as the driver loads them (0x0206D3D8),
    // and returns their size in bytes. Each wave takes (its loop's start + its loop's length) * 4 bytes, in the order
    // of the bank's slots. `main_bank` is the bank that holds the waves.
    uint32_t PlaceWaves(uint16_t id, const WaveBank& main_bank, uint32_t address);

    // Starts `song` as a song: the driver loads it (0x0206D808), and resumes it from its start (0x0206D9BC). The driver
    // keeps a copy of the song's bytes. A song that plays already stops first.
    void PlaySong(const Song& song, const StartParameters& parameters = {});

    // Adds `effects` to the driver's banks of sound effects, as the driver loads such a bank (0x0206EA18), and returns
    // the bank's id. The driver refuses a bank whose id it holds already, and the function then returns nothing. The
    // driver keeps a copy of the bank's bytes.
    std::optional<uint16_t> AddEffects(const SoundEffects& effects);

    // Starts sound effect `index` of the bank of sound effects `bank` on a sequence of the pool (0x0206EDFC), and
    // returns whether it starts. The driver refuses an effect that the bank lacks, and one with more tracks than a
    // sequence of the pool. Byte 0x1C of an effect's settings gives the most instances of the effect that may play at
    // once, or of the effects of its group where byte 0x1A gives one. 0 allows any number. When that many play, the
    // start stops the oldest of them, or refuses the effect where byte 0x18 has bit 0 set. With flag 0x40 of
    // `parameters`, the start stops the effects of `handle` in place of that check, or refuses the effect while one
    // plays where bit 0 is set. The effect then takes a sequence that doesn't play, else the oldest of those whose
    // byte 0x1B of the settings is at least the effect's, and stops it. The driver refuses the effect when none is.
    bool PlayEffect(uint16_t bank, uint16_t index, const StartParameters& parameters = {}, uint16_t handle = 0);

    // Stops the song (0x0206DA9C). A stop of `ms` milliseconds other than 0 fades the song out over that time while its
    // volume isn't 0, and the song stops once the fade ends. The song's channels keep the notes that they hold.
    void StopSong(uint32_t ms = 0);

    // Stops each sound effect that plays effect `index` of the bank of sound effects `bank`, or only those of `handle`
    // where `handle` isn't 0 (0x0206F38C). The stops take `ms` as StopSong does, and a stop at once frees the notes
    // that the effects' channels hold.
    void StopEffect(uint16_t bank, uint16_t index, uint16_t handle = 0, uint32_t ms = 0);

    // Stops each sound effect of `handle`, as StopEffect does (0x0206F38C).
    void StopHandle(uint16_t handle, uint32_t ms = 0);

    // Stops each sound effect, as StopEffect does (0x0206F4E0).
    void StopEffects(uint32_t ms = 0);

    // Runs a tick of the driver's thread (0x02071014) and returns the commands that it sends. `channel_status` holds a
    // bit for each hardware channel that plays, as the ARM7's driver gives them in the shared work. Throws
    // std::runtime_error where the driver's code breaks its lists of notes: when a song releases the damper while it
    // holds a note that has ended. Throws a Hang when a track runs kCommandBudget commands in a tick.
    ntr::snd::CommandList Tick(uint16_t channel_status);

    // Whether the song's sequence still plays.
    bool Playing() const;

    // The number of the pool's sequences that play.
    int EffectsPlaying() const;

    // A host's release of the followed sequence's notes. Each note's length runs out in the sequence's next tick, and
    // the note then ends as any note does: a channel's damper keeps it. The driver has no such call. A host that plays
    // a note for as long as a key is down has no other way to let it go.
    void ReleaseFollowedNotes();

    // For a performer, of the sequence that it follows: the song (kSong), or sequence n of the pool of sound
    // effects, from 0. The driver has none of these. The performer follows the song until it chooses another. The
    // views of a sequence without a song or an effect are empty.
    static constexpr int kSong = -1;
    void Follow(int sequence);
    void SetHooks(SequenceHooks hooks);
    const SequenceHooks& GetHooks() const;
    bool SequencePlays() const;
    uint32_t SequenceTick() const;     // the sequence's ticks so far
    uint16_t SequenceTimebase() const; // its ticks in a quarter note: its settings' value, a multiple of 8
    int32_t SequenceTempo() const;     // the length of a quarter note, in microseconds
    int ChannelCount() const;
    ChannelView GetChannel(int channel) const;
    int TrackCount() const;
    bool TrackPlays(int track) const;
    VoiceView GetVoice(int voice) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// The driver's arithmetic, for a converter. EnvelopeTicks gives the ticks that a stage of an envelope takes: entry
// `index` of the table of times, `multiplier` times over, or of the table without a multiplier where `multiplier` is 0.
// An index of 0x7F takes for ever, and gives nothing (0x02074D58). VoiceTimer gives a voice's timer at `pitch`, in
// 256ths of a semitone, for a wave whose byte 0x13 is `format`: 3 and up for the PSG and the noise (0x02074194). The
// voice plays 0xFFB0FF / timer samples a second, or steps of the PSG and the noise.
std::optional<int32_t> EnvelopeTicks(uint8_t multiplier, uint8_t index);
int32_t VoiceTimer(uint16_t pitch, uint8_t format);

// The levels, from 0 to 127, that an envelope of `values` gives a voice in each of its first `ticks` ticks, as the
// driver steps it (0x02074E0C and 0x02074F54), for a converter. Every value applies, a value above 0x7F included. The
// envelope releases before the tick `release` where that is given.
std::vector<uint8_t> EnvelopeLevels(const EnvelopeValues& values, int ticks, std::optional<int> release = {});

// The LFOs that program `program` of `bank` gives a channel that selects it (0x02073BC8), for a converter. Each is off
// for a program that the bank lacks.
std::array<LfoValues, 4> ProgramLfos(const WaveBank& bank, uint16_t program);

} // namespace ntdlib::dse
