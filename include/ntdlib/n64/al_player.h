// SPDX-License-Identifier: MIT

// A model of libultra's two sequence players on the synthesis driver of al_synth.h. ALSeqPlayer plays standard MIDI
// files of format 0. ALCSPlayer plays compressed MIDI. The two differ in how a sequence's notes end. A
// compressed note holds its duration, and the player queues its note off. A tempo change then rescales the queued note
// offs. ALSeqPlayer reads note offs from the file, and loops between two markers that the game gives it.
//
// A player keeps a queue of events in microseconds, each with its distance from the one before. The driver calls the
// player when the next event comes due, and the player runs every event of that time. A periodic event keeps the queue
// from running empty. The player tells the driver the time to its next call. A compressed sequence's loop ends count
// down in the file's bytes. The player therefore holds a copy of them.
//
// The player's state is plain values and copies of bytes. A copy of the object is a snapshot.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/bank_file.h"

namespace ntdlib::n64
{

enum class AlSequenceForm : uint8_t
{
    kStandard,
    kCompressed,
};

enum class AlPlayerState : uint8_t
{
    kStopped,
    kPlaying,
    kStopping,
};

// A bank of a bank file for a player. `file_address` is the place of the file's first byte in the model's
// memory, for the books and the loop states. `sample_address` is the place of the first byte of the file of samples.
// The file must outlive the player.
struct AlSoundBank
{
    const BankFile* file = nullptr;
    std::size_t bank = 0;
    uint32_t file_address = 0;
    uint32_t sample_address = 0;
};

// The form of the library that a game links.
enum class AlPlayerForm : uint8_t
{
    kLibrary,

    // The compressed player of Pokémon Snap. CC 21 sets a scale for the whole player. The scale takes the place of the
    // sound's volume in a voice's level. CC 8 sets a scale for a channel. A change of level during a decay takes 1 ms,
    // and the decay then restarts from the gain that it had reached.
    kPlayerScale,
};

struct AlPlayerConfig
{
    int voices = 64;
    int events = 64;
    int channels = 16;
    AlPlayerForm form = AlPlayerForm::kLibrary;

    // Whether the game gives the player the oscillators of Nintendo's example code. An instrument's tremolo and vibrato
    // sound only with them.
    bool oscillators = false;
};

// 2^(cents / 1200), by squares of a single-precision step, as libultra's alCents2Ratio.
float CentsToRatio(int32_t cents);

// The wave of a sound, with its addresses in the model's memory.
AlWave WaveOfSound(const AlSoundBank& bank, const BankSound& sound);

// A note that starts. `offset` is the place in the sequence's bytes of the event's first byte. In a compressed MIDI
// file's block, that's the place in the block's source.
struct AlNoteStart
{
    int channel = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint32_t duration = 0;
    uint32_t offset = 0;
    int32_t time = 0; // in microseconds, the player's time
};

class AlPlayer
{
public:
    AlPlayer(const AlPlayerConfig& config, AlSynth& synth);

    // Each of these posts an event, like the library's function of that name. It takes effect in the next call.
    void SetBank(const AlSoundBank& bank);
    void SetSequence(AlSequenceForm form, std::span<const uint8_t> bytes);
    void SetVolume(int16_t volume);
    void Play();
    void Stop();

    // alSeqpLoop, with markers from ticks. Only ALSeqPlayer has it. A count of -1 loops for ever.
    void SetLoop(int32_t start_ticks, int32_t end_ticks, int32_t count);

    // Sends a MIDI message at once, like the library's functions for channels.
    void SendMidi(uint8_t status, uint8_t byte1, uint8_t byte2);

    // An amount added to each channel's effect send. Only AlPlayerForm::kPlayerScale has it.
    void SetFxOffset(int8_t offset);

    // The driver's call. Returns the microseconds until the next call.
    int32_t Handle(AlSynth& synth);

    AlPlayerState State() const;
    int32_t Time() const;
    int64_t TickPosition() const;

    // The driver's clock for this player: the samples at which it runs next.
    int32_t samples_left = 0;

    // Called for each note that starts.
    std::function<void(const AlNoteStart&)> on_note;

private:
    enum class EventType : uint8_t
    {
        kPeriodic,
        kSequence,
        kNoteEnd,
        kEnvelope,
        kMidi,
        kNoteOff,
        kPlayerVolume,
        kPlay,
        kStop,
        kStopping,
        kSetSequence,
        kSetBank,
        kSetLoop,
        kTremolo,
        kVibrato,
    };

    struct Event
    {
        EventType type = EventType::kPeriodic;
        int voice = -1;
        uint8_t volume = 0;
        int32_t delta = 0;
        uint8_t status = 0;
        uint8_t byte1 = 0;
        uint8_t byte2 = 0;
        uint32_t duration = 0;
        uint32_t offset = 0;
        int16_t player_volume = 0;
        int32_t loop_start = 0;
        int32_t loop_end = 0;
        int32_t loop_count = 0;
        int oscillator = -1;
    };

    struct QueueItem
    {
        Event event;
        int32_t delta = 0;
    };

    enum class EnvelopePhase : uint8_t
    {
        kAttack,
        kDecay,
        kRelease,
    };

    enum class NotePhase : uint8_t
    {
        kNoteOn,
        kSustain,
        kRelease,
        kSustainRelease,
    };

    struct VoiceState
    {
        int synth_voice = -1;
        const BankSound* sound = nullptr;
        int32_t envelope_end = 0;
        float pitch = 1.0F;
        float vibrato = 1.0F;
        uint8_t envelope_gain = 0;
        uint8_t channel = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        EnvelopePhase envelope_phase = EnvelopePhase::kAttack;
        NotePhase phase = NotePhase::kNoteOn;
        uint8_t tremolo = 0;
        uint8_t oscillators = 0; // 1 for a tremolo, 2 for a vibrato
    };

    // One of the example code's oscillators. A type uses only some of the fields.
    struct Oscillator
    {
        uint8_t type = 0;
        bool second_half = false;
        uint16_t period = 0;
        uint16_t count = 0;
        uint8_t level = 0;
        uint8_t depth = 0;
        uint8_t low_level = 0;
        float cents = 0.0F;
        float low_ratio = 0.0F;
        float high_ratio = 0.0F;
        int32_t start_cents = 0;
        int32_t cents_range = 0;
    };

    struct ChannelState
    {
        const BankInstrument* instrument = nullptr;
        int16_t bend_range = 200;
        uint8_t pan = 64;
        uint8_t priority = 5;
        uint8_t volume = 127;
        uint8_t fx_mix = 0;
        uint8_t sustain = 0;
        int8_t scale = -1; // no scale while it's negative
        float pitch_bend = 1.0F;
    };

    // What a sequence gives next.
    enum class SequenceEventType : uint8_t
    {
        kMidi,
        kTempo,
        kEnd,
        kTrackEnd,
        kLoopStart,
        kLoopEnd,
    };

    struct SequenceEvent
    {
        SequenceEventType type = SequenceEventType::kMidi;
        uint8_t status = 0;
        uint8_t byte1 = 0;
        uint8_t byte2 = 0;
        uint8_t byte3 = 0;
        uint32_t duration = 0;
        uint32_t offset = 0;
    };

    // A compressed MIDI file's track.
    struct Track
    {
        uint32_t location = 0;
        uint32_t block_place = 0;
        uint32_t block_length = 0;
        uint8_t last_status = 0;
        uint32_t delta_ticks = 0;
    };

    // A place in a standard MIDI file.
    struct StandardMarker
    {
        uint32_t place = 0;
        uint8_t last_status = 0;
        int32_t last_ticks = 0;
        int32_t current_ticks = 0;
    };

    void Post(const Event& event, int32_t delta);
    int32_t NextEvent(Event& event);
    void FlushType(EventType type);
    void RemoveEnvelopeEvents(int voice);
    void StopOscillators(int voice);
    int32_t StartOscillator(int& state, float& value, uint8_t type, uint8_t rate, uint8_t depth, uint8_t delay);
    bool NeedsNoteKill(int voice, int32_t kill_time);

    void HandleSequenceEvent(AlSynth& synth);
    void HandleMidi(AlSynth& synth, const Event& event);
    void HandleTempo(const SequenceEvent& event);
    void PostNextSequenceEvent();
    void ReleaseVoice(AlSynth& synth, int voice, int32_t time);
    void UnmapVoice(int voice);
    void InitFromBank();
    void SetInstrument(const BankInstrument& instrument, int channel);
    void ResetChannel(int channel);
    void SetTempo(float tempo);
    AlWave WaveOf(const BankSound& sound) const;

    int MapVoice(uint8_t key, uint8_t velocity, uint8_t channel);
    int LookupVoice(uint8_t key, uint8_t channel) const;
    const BankSound* LookupSound(uint8_t key, uint8_t velocity, uint8_t channel) const;
    int16_t VoiceVolume(const VoiceState& voice) const;
    uint8_t FxMix(const ChannelState& channel) const;
    void ChangeVoiceLevel(AlSynth& synth, int voice);
    bool RestartDecay(AlSynth& synth, int voice);
    int32_t VoiceDelta(const VoiceState& voice) const;
    uint8_t VoicePan(const VoiceState& voice) const;

    // The compressed reader.
    void NewCompressed();
    bool CompressedNextDelta(int32_t& ticks);
    SequenceEvent CompressedNextEvent();
    SequenceEvent CompressedTrackEvent(int track);
    uint8_t TrackByte(int track);
    uint32_t TrackVarLen(int track);
    uint8_t SequenceByte(uint32_t place) const;

    // The standard reader.
    void NewStandard();
    bool StandardNextDelta(int32_t& ticks);
    SequenceEvent StandardNextEvent();
    uint8_t StandardByte();
    int32_t StandardVarLen();
    StandardMarker MakeMarker(int32_t ticks);

    int channels_ = 16;
    int max_events_ = 64;
    const BankFile* file_ = nullptr;
    std::size_t bank_ = 0;
    uint32_t file_address_ = 0;
    uint32_t sample_address_ = 0;
    int32_t microseconds_per_tick_ = 488;
    AlPlayerState state_ = AlPlayerState::kStopped;
    int16_t volume_ = 0x7FFF;
    AlPlayerForm library_form_ = AlPlayerForm::kLibrary;
    int16_t player_scale_ = 120;
    int8_t fx_offset_ = 0;
    bool oscillators_ = false;
    std::vector<Oscillator> oscillator_states_;
    std::vector<int> free_oscillators_;
    int32_t time_ = 0;
    Event next_event_;
    std::vector<QueueItem> queue_;
    std::vector<ChannelState> channels_state_;
    std::vector<VoiceState> voices_;
    std::vector<int> free_voices_; // the head at the back
    std::vector<int> active_voices_;

    bool has_sequence_ = false;
    AlSequenceForm form_ = AlSequenceForm::kCompressed;
    std::vector<uint8_t> pending_bytes_;
    std::vector<uint8_t> bytes_;
    float ticks_per_quarter_inverse_ = 0.0F;

    std::array<Track, 16> tracks_ = {};
    uint32_t valid_tracks_ = 0;
    uint32_t last_delta_ticks_ = 0;
    int64_t last_ticks_ = 0;
    bool delta_flag_ = true;

    uint32_t place_ = 0;
    uint8_t last_status_ = 0;
    int32_t standard_ticks_ = 0;
    uint32_t track_start_ = 0;
    int32_t loop_count_ = 0;
    StandardMarker loop_start_;
    StandardMarker loop_end_;
};

// alAudioFrame with one client: runs the player up to the frame's end, and appends the frame's commands to `list`. The
// frame's samples go to `output` in the model's memory.
void BuildAlFrame(AlSynth& synth, AlPlayer& player, AudioList& list, AudioMemory& memory, uint32_t output, int samples);

} // namespace ntdlib::n64
