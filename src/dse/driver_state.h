// SPDX-License-Identifier: MIT

// The state of the DSE driver model (Driver::State), and the types that the state uses. driver_voices.cpp defines the
// work on voices, notes, channels and synths, and driver_lfos.cpp the work on LFOs. driver_sequences.cpp defines the
// work on sequences, effects, fades and commands. driver.cpp defines the steps of a tick and the interface of the
// driver.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/text.h"

#include "driver_units.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/dse/formats.h"
#include "ntdlib/dse/tables.h"
#include "ntdlib/ntr/snd/driver.h"

namespace ntdlib::dse
{

constexpr int kNone = -1;

struct Bank;

struct Synth;

struct Voice
{
    uint8_t Format() const
    {
        return sample[3];
    }

    uint32_t Address() const
    {
        return xsflib::LoadLe32(&sample[0x14]);
    }

    uint32_t sequence = 0;
    uint16_t note_off = 0;
    uint16_t flags = 0;
    uint16_t index = 0;
    uint16_t bit = 0;
    uint8_t key = 0;
    int8_t wave = 0;
    uint8_t bend_range = 0;
    int16_t key_bend = 0;
    int16_t note_key = 0;
    int16_t key_base = 0;
    uint8_t note_volume = 0;
    int8_t wave_pan = 0x40;
    uint16_t channel_key_group = 0;
    std::array<uint8_t, 0x20> sample = {};
    Envelope envelope;
    LfoBank lfos;
    int32_t timer = 0;
    int32_t volume = 0;
    int32_t pan = 0;
    // The envelope's level in bits 0 to 7, bit 8 while the voice sounds, and the voice's priority from bit 16 on.
    uint32_t state = 0;
    // A build without a list of held notes: the ticks until the voice's note ends.
    int32_t duration = 0;
    int next = kNone;
    struct Channel* channel = nullptr;
    uint32_t note = 0;        // the number of the note that started the voice, for a performer
    int track = -1;           // the track whose note started the voice, for a performer
    int16_t jitter = 0;       // the note's random change of pitch, for a performer
    uint8_t group_voices = 0; // the voices of a channel that the key group allows, or 0 for any, for a performer
};

// A node of the driver's list of the notes that the channels hold.
struct Note
{
    uint8_t on = 0;
    uint8_t held = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    int32_t duration = 0;
    int next = kNone;
};

struct Channel
{
    uint32_t sequence = 0;
    uint8_t flags = 0;
    uint16_t update = 0; // all but the later build: the parameters that the channel's voices update (UpdateVoices)
    uint8_t index = 0;
    uint8_t unknown6 = 0;
    uint8_t allocation = 0;
    uint8_t unknown8 = 0;
    int8_t voices = 0;
    int16_t jitter = 0;
    uint8_t random_low = 0;
    uint8_t random_high = 0;
    uint16_t bank_id = 0;
    uint16_t program = 0;
    uint16_t bend = 0;
    uint16_t volume = 0;
    int16_t pan = 0;
    Fade bend_fade;
    Fade volume_fade;
    Fade pan_fade;
    uint8_t expression = 0;
    int8_t lfo_level = 0;
    int16_t key_bend = 0;
    int16_t tuning = 0;
    int16_t unknown56 = 0;
    uint8_t optional_volume = 0;
    uint8_t bend_range = 0;
    std::array<uint8_t, 10> unknown5a = {}; // byte 7 selects the LFO that events 0xF0 to 0xF3 set
    EnvelopeParameters envelope;
    std::array<LfoSettings, 4> lfos = {};
    int voice_list = kNone;
    int held = kNone;
    Bank* bank = nullptr;
    const std::vector<uint8_t>* instrument = nullptr;
    Synth* synth = nullptr;
};

struct Synth
{
    uint8_t channel_count = 0;
    int8_t global_volume = 0;
    int8_t global_volume_index = 0;
    int16_t bend = 0;
    int8_t volume = 0;
    int8_t pan = 0;
    int8_t combined_volume = 0;
    uint8_t paused = 0;
    uint8_t unknown_a = 0;
    uint8_t voices = 0;
    uint16_t update = 0; // all but the later build: the parameters that the synth's voices update (UpdateVoices)
    std::vector<Channel> channels;
};

// A bank as the driver keeps it.
struct Bank
{
    uint16_t id = 0;
    uint16_t main_bank = 0;
    const WaveBank* file = nullptr;
    int16_t wave_count = 0;
    int16_t program_count = 0;
    uint8_t key_group_count = 0;
    uint32_t samples = 0;
    // Each wave's entry. The driver changes the entry as it places the wave.
    std::vector<std::optional<std::array<uint8_t, WaveInfo::kSize>>> waves;
    // Each program's bytes, as memory stores them: the header, the LFOs, 16 other bytes and the splits.
    std::vector<std::optional<std::vector<uint8_t>>> programs;
};

struct SubLoop
{
    uint32_t start = 0;
    uint32_t end = 0;
    uint8_t count = 0;
    uint8_t octave = 0;
};

struct TrackState
{
    uint16_t preamble = 0;
    int8_t playing = 0;
    uint8_t channel_number = 0;
    uint8_t octave = 0;
    uint8_t multiplier = 0;
    uint8_t loop_index = 0;
    uint8_t previous_key = 0;
    int8_t ignore_notes = 0;
    uint16_t loops = 0;
    uint32_t wait = 0;
    uint32_t previous_wait = 0;
    uint32_t previous_length = 0;
    uint32_t data = 0;
    uint32_t position = 0;
    uint32_t loop_start = 0;
    std::array<SubLoop, 4> sub_loops = {};
    Channel* channel = nullptr;
};

struct Sequence
{
    int8_t initialized = 0;
    int8_t stopped = 0;
    uint8_t looped = 0;
    std::array<int8_t, 4> status = {}; // playing, active, paused and how it ends at a volume of 0
    int8_t muted = 0;
    int8_t unknown9 = 0;
    int8_t unknown_a = 0;
    int8_t unknown_b = 0;
    uint8_t fades = 0;
    uint8_t tracks_playing = 0;
    int8_t track_count = 0;
    int8_t track_slots = 0;
    int8_t channel_count = 0;
    uint8_t unknown11 = 0;
    uint8_t priority = 0;
    uint8_t global_volume_index = 0;
    uint8_t group = 0;
    uint8_t ticks_per_eighth = 0;
    uint8_t signal = 0;
    uint8_t bpm = 0;
    int16_t delay = 0;
    uint32_t id = 0;
    uint16_t handle = 0;
    int32_t reset_ticks = 0;
    int32_t driver_ticks = 0;
    int32_t sequencer_ticks = 0;
    int32_t accumulator = 0;
    int32_t step = 0;
    int32_t microseconds_per_beat = 0;
    Fade tempo;
    Fade bend;
    Fade volume;
    Fade pan;
    int8_t volume_target = 0;
    std::vector<TrackState> tracks;
    Synth synth;
    std::shared_ptr<const std::vector<uint8_t>> file; // the song's or the bank of sound effects' bytes
};

// A sequence that the driver's state owns. A copy of the state copies the sequence (Driver's copy).
class OwnedSequence
{
public:
    OwnedSequence() = default;
    explicit OwnedSequence(std::unique_ptr<Sequence> sequence) : sequence_(std::move(sequence))
    {
    }
    OwnedSequence(const OwnedSequence& other)
        : sequence_(other.sequence_ ? std::make_unique<Sequence>(*other.sequence_) : nullptr)
    {
    }
    OwnedSequence& operator=(const OwnedSequence& other)
    {
        sequence_ = other.sequence_ ? std::make_unique<Sequence>(*other.sequence_) : nullptr;
        return *this;
    }
    OwnedSequence(OwnedSequence&&) = default;
    OwnedSequence& operator=(OwnedSequence&&) = default;
    ~OwnedSequence() = default;

    explicit operator bool() const
    {
        return sequence_ != nullptr;
    }
    Sequence& operator*() const
    {
        return *sequence_;
    }
    Sequence* operator->() const
    {
        return sequence_.get();
    }
    Sequence* get() const
    {
        return sequence_.get();
    }

private:
    std::unique_ptr<Sequence> sequence_;
};

// A bank of sound effects as the driver keeps it.
struct EffectBank
{
    uint16_t id = 0;
    std::shared_ptr<const std::vector<uint8_t>> file;
    int16_t count = 0;
    std::size_t sequences = 0; // where the seq chunk's data starts, or 0 for a bank whose byte 0x35 is 0
    int8_t unknown18 = 1;
};

struct Driver::State
{
    // 0x0206C8F4
    int32_t Random();

    Bank* BankById(uint16_t id);

    Voice& VoiceAt(int index);

    Note& NoteAt(int index);

    void FreeNote(int node);

    template <typename Function>
    void ForEachVoice(Channel& channel, const Function& function)
    {
        for (int v = channel.voice_list; v != kNone;)
        {
            Voice& voice = VoiceAt(v);
            const int next = voice.next;
            function(voice);
            v = next;
        }
    }

    // All but the later build mark the channel instead. Its voices take the marks when they update.
    void FlagVoices(Channel& channel, uint16_t flags);

    // All but the later build mark the synth instead. The voices of all its channels take the marks when they update.
    void FlagSynth(Synth& synth, uint16_t flags);

    // Voices

    // 0x020748A0
    void FreeVoice(Voice& voice);

    // 0x020749A4
    void MarkStarted(Voice& voice);

    // 0x020749F4
    void MarkStopped(Voice& voice);

    // 0x02074A2C
    int CountActive(Channel& channel);

    // 0x02074618: a voice for a note of `channel` with key group `group` (its 8 bytes), owner `owner` and priority
    // `priority`.
    Voice* TakeVoice(Channel& channel, const std::array<uint8_t, 8>& group, uint16_t owner, uint8_t priority);

    // 0x02074780
    void StartVoice(Channel& channel, Voice& voice, uint8_t priority);

    // 0x0207485C
    void ReleaseVoice(Channel& channel, Voice& voice);

    // 0x02074150: releases the voices that play `key`.
    void ReleaseVoices(Channel& channel, uint8_t key);

    // 0x020750B4
    void StartLfos(LfoBank& bank, const std::array<LfoSettings, 4>& settings, int8_t level);

    // 0x020752AC: gives each LFO of type 3, whose level is constant, the level of event 0xBE.
    static void SetConstantLevels(LfoBank& bank, int8_t level);

    // The waveforms (0x020753AC to 0x020755B8).
    int32_t Waveform(Lfo& lfo);

    // 0x020752F8: returns the flags of the parameters that the LFOs change.
    uint16_t StepLfos(LfoBank& bank);

    // Notes of a channel

    // A random key in the channel's region for `note`, while the channel has a region.
    void RandomKey(const Channel& channel, Note& note);

    // 0x02073CD8: takes a node for `note`, and gives it a random key in the channel's region.
    int HoldNote(Channel& channel, Note& note);

    // 0x02073D68: releases the voices of the first held note with `node`'s key, and frees that note.
    void ReleaseHeldNote(Channel& channel, int node);

    // 0x02073E10: a legato. The first held note with key `previous` takes `note`, and its voices take the new key.
    // The earlier and middle builds (0x02064C1C in Explorers of Time's ARM9 binary) give the key and the note's
    // duration to the channel's first voice of key `previous` alone.
    void Legato(Channel& channel, const Note& note, uint8_t previous);

    // 0x02073EA0
    void ReleaseKey(Channel& channel, uint8_t key);

    // 0x020747DC: releases the held notes whose time is up. The driver's code frees such a note twice: in the release
    // of its key (0x02073D68), then again here. Its list of free notes then leads back to the note, the next notes
    // share it, and the code soon walks a list that never ends. The model stops there.
    void ReleaseEndedNotes(Channel& channel);

    // 0x0206530C in Explorers of Time's ARM9 binary: the earlier and middle builds release each voice that a release
    // has marked, those that release already among them.
    void ReleaseDampedVoices(Channel& channel);

    // A split of the channel's program: 48 bytes from `at`. Nothing where the program ends before them.
    static std::optional<SplitView> SplitAt(const std::vector<uint8_t>& program, std::size_t at);

    // 0x0206D5C8: the next split after the one at `previous` (0 for none) that holds `key` and `velocity`.
    static std::optional<std::size_t> NextSplit(const std::vector<uint8_t>& program,
                                                std::optional<std::size_t> previous, uint8_t key, int8_t velocity);

    // 0x02073EDC: starts a voice for each split that holds the note. The earlier and middle builds (0x0206496C in
    // Explorers of Time's ARM9 binary) first test the program, then draw the jitter, then the key in the channel's
    // region. Each of their voices takes the note's duration.
    uint16_t StartNote(Channel& channel, const Note& given);

    // 0x02074CF4
    static void SetEnvelopeOverride(EnvelopeParameters& parameters, const EnvelopeParameters& override);

    // Channels

    // 0x02073A90. The earlier build then stops the freed voices (0x02064770 in Explorers of Time's ARM9 binary, North
    // America).
    void FreeVoices(Channel& channel);

    // 0x02073BC8
    void SelectProgram(Channel& channel, uint16_t index);

    // 0x02073B90
    void SelectBank(Channel& channel, uint16_t id);

    // 0x02073C90
    void ApplyKeyBend(Channel& channel, int16_t value);

    // 0x020738DC: the channel's settings from the driver's defaults.
    void ResetChannel(Channel& channel, uint32_t id);

    // Synths

    void ChannelVolumes(Synth& synth);

    // 0x02073208
    void ResetSynth(Synth& synth);

    // 0x0207356C. All but the later build (0x02064518 in Explorers of Time's ARM9 binary) leave the channels' volumes
    // until the voices update with a mark of the volume.
    void SelectGlobalVolume(Synth& synth, uint8_t index, int flag);

    // 0x02073654, 0x020736E8 and 0x020737B8. All but the later build (0x02064568, 0x02064580 and 0x020645C0 in
    // Explorers of Time's ARM9 binary) only mark the synth. Its channels work out their values when the voices update.
    void SetSynthBend(Synth& synth, int16_t bend);

    void SetSynthVolume(Synth& synth, int8_t volume);

    void SetSynthPan(Synth& synth, uint8_t pan);

    // 0x0207384C
    void ResetChannels(Synth& synth, int bank, uint32_t id);

    // 0x020733BC
    void FreeHeldNotes(Synth& synth);

    // 0x02073480
    void FreeAllVoices(Synth& synth);

    // 0x020734AC
    void SilenceVoices(Synth& synth, uint8_t flag);

    // 0x02073504
    void ReplayHeldNotes(Synth& synth);

    // Sequences

    uint8_t Byte(Sequence& sequence, TrackState& track, uint32_t at);

    // 0x0206E26C
    void RestartSequence(Sequence& sequence);

    // The ticks of a fade of `ms` milliseconds: at least 1 for a time other than 0.
    uint16_t FadeTicks(uint32_t ms) const;

    // Sets `fade`'s target to `value` (16.16), and returns its ticks, or 0 where the fade sets the value at once.
    static uint16_t SetFade(Fade& fade, int32_t value, uint16_t ticks);

    // 0x0206DD70: the fades of the sequence's tempo, bend, volume and pan, as `parameters` selects them.
    void StartFades(Sequence& sequence, const StartParameters& parameters);

    // 0x0206DB7C: starts the sequence with `parameters`, for `handle` and in `group`.
    void StartSequence(Sequence& sequence, uint16_t handle, uint8_t group, const StartParameters& parameters);

    // 0x0206E3C4: stops the sequence, or fades it out over `ms` milliseconds while its volume isn't 0. A stop at once
    // frees the voices of the sequence's channels, and with `clear` 1 the notes that they hold.
    void StopSequence(Sequence& sequence, uint32_t ms, int clear);

    // 0x0206E85C
    static void ClearTracks(Sequence& sequence);

    // 0x0206E6A0: a sequence with `tracks` tracks and a synth of `channels` channels.
    std::unique_ptr<Sequence> MakeSequence(uint8_t tracks, uint8_t channels);

    // 0x0206E554: the sequence's settings, from the settings at `info` in its file, and its tracks, from the track
    // chunks that follow them. Returns false for a sequence with more tracks than `sequence` holds.
    bool LoadSequence(Sequence& sequence, std::size_t info);

    EffectBank* EffectBankById(uint16_t id);

    // 0x0206EFB0: where the settings of effect `index` of `bank` are in the bank's file, or nothing.
    static std::optional<std::size_t> EffectAt(const EffectBank& bank, int16_t index);

    // 0x0206EFF8 and 0x0206F100: whether the instances that `matches` selects among the pool's sequences that play
    // refuse a new one: `most` or more of them. With `stop` 1, the driver stops the oldest of them instead, until fewer
    // than `most` play, and refuses nothing.
    template <typename Match>
    bool TooManyInstances(const Match& matches, uint8_t most, int stop)
    {
        if (most == 0)
        {
            return false;
        }
        if (stop == 1)
        {
            Sequence* oldest = nullptr;
            int count = 0;
            do
            {
                count = 0;
                uint32_t lowest = 0x7FFFFFFF;
                for (const OwnedSequence& q : effects)
                {
                    if (q->status[0] == 1 && matches(*q))
                    {
                        count++;
                        if (lowest > static_cast<uint32_t>(q->reset_ticks))
                        {
                            lowest = static_cast<uint32_t>(q->reset_ticks);
                            oldest = q.get();
                        }
                    }
                }
                if (count >= most && oldest != nullptr)
                {
                    StopSequence(*oldest, 0, 1);
                }
            } while (count > most);
            return false;
        }
        int count = 0;
        for (const OwnedSequence& q : effects)
        {
            if (q->status[0] == 1 && matches(*q))
            {
                count++;
            }
        }
        return count >= most;
    }

    // 0x0206F208: whether a sequence of the pool that plays has `handle`. With `stop` 1, the driver stops each such
    // sequence instead, and the function returns false.
    bool HandlePlays(uint16_t handle, int stop);

    // 0x0206F2C4: the first sequence of the pool that doesn't play, else the oldest of those whose priority is at
    // least `priority`, or none.
    Sequence* SequenceForEffect(uint8_t priority);

    // 0x02071784
    void StepFades(Sequence& sequence);

    // 0x02071160. The earlier and middle builds (0x02062A00 in Explorers of Time's ARM9 binary) count down each
    // voice's duration instead, and release the voice when the count reaches 0. A voice that starts with 0 never
    // releases.
    void StepNoteLengths(Sequence& sequence);

    void RunEvent(Sequence& sequence, TrackState& track, uint8_t code, uint32_t& at);

    // 0x02071224
    void RunTrack(Sequence& sequence, TrackState& track);

    // 0x020713E8. The earlier build (0x02062C14 in Explorers of Time's ARM9 binary) has no delay, and steps the fades
    // on every tick. It stops a sequence that fades out before the fades step, and runs the rest of the step. The
    // middle build (0x020743A4 in Luminous Arc 2's ARM9 binary) has the delay and steps the fades only while they
    // run, but stops a faded sequence as the earlier build does. The late middle build (0x02129D74 in Shiren DS 2's
    // ARM9 binary) stops a faded sequence as the later build does, but doesn't silence one.
    void StepSequence(Sequence& sequence);

    // The voices' steps of a tick

    // 0x0207448C
    void StepVoices();

    // 0x02064C74 in Explorers of Time's ARM9 binary: the earlier build works out a channel's pitch, volume and pan as
    // its voices update, for the parameters that the channel's and the synth's marks name. Each of the channel's voices
    // takes the marks.
    void UpdateChannels(Synth& synth);

    // 0x020764BC in Luminous Arc 2's ARM9 binary: the middle builds work out a channel's pitch, volume and pan for each
    // of its voices on every tick, for the parameters that the marks of the channel and of the synth name. The marks
    // stay set. Gives the marks for the voice.
    uint16_t UpdateChannel(Channel& channel);

    // 0x02074194
    void UpdateVoices();

    // 0x02074A58, and 0x0206553C in Explorers of Time's ARM9 binary for the earlier build
    void WriteCommands(ntr::snd::CommandList& list);

    // 0x0207457C
    void CheckEndedChannels(uint16_t channel_status);

    // After a copy of `from`: points the copy's pointers at the copy's channels, synths, banks and programs where they
    // point at from's.
    void Repoint(const State& from);

    Build build = Build::kLater;

    // Whether the build follows the rules of the earlier build. The oldest build follows them too, apart from the rules
    // that it changes.
    bool Earlier() const
    {
        return build == Build::kEarlier || build == Build::kOldest;
    }

    // Whether the build keeps a list of the notes that its channels hold, and counts their ticks there.
    bool ListsNotes() const
    {
        return build == Build::kLater || build == Build::kLateMiddle;
    }

    ntr::snd::CommandList commands; // the commands since the last list: the start of the next list
    int16_t tick = Driver::kTickMicroseconds;
    int32_t total_ticks = 0;
    uint32_t seed = 0xBC614E;
    uint8_t velocity_mode = 2;
    std::array<int8_t, 16> global_volumes = {};
    std::array<uint8_t, 128> defaults = {};
    std::array<Note, 128> notes = {};
    int free_notes = kNone;
    std::array<Voice, Driver::kVoices> voices = {};
    int voice_count = Driver::kVoices;
    uint16_t last_start_mask = 0;
    uint16_t start_mask = 0;
    uint16_t stop_mask = 0;
    uint16_t deallocated_mask = 0;
    std::list<Bank> banks;
    std::list<EffectBank> effect_banks;
    OwnedSequence song;
    std::vector<OwnedSequence> effects; // the pool, in the order of the driver's list

    // For a performer: the sequence that it follows, its hooks, and the notes that have started.
    int followed = kSong;
    SequenceHooks hooks;
    uint32_t notes_started = 0;

    Sequence* Followed() const;
};

} // namespace ntdlib::dse
