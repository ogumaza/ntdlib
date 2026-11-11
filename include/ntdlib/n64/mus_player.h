// SPDX-License-Identifier: MIT

// The files and the player of the N64 Music Tools, a sound library for the Nintendo 64. The library runs on the compact
// form of the synthesis driver in al_synth.h.
//
// A pointer bank lists a game's waves. A song holds three lists for each of its channels: the commands and notes, the
// volumes and the pitch bends. The player runs once a frame at a fixed rate. Each channel has a clock in 1/256 ticks. A
// channel's notes play on voices from a shared pool. Every channel sends its voice's pitch, volume, pan and effect
// level to the driver when they change.
//
// The players' state is plain values. A copy of a player is a snapshot. The files must outlive every copy.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/n64/al_synth.h"
#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"

namespace ntdlib::n64
{

// A wave of a pointer bank. Each place in a bank is an offset from the bank's first byte.
struct MusWave
{
    friend bool operator==(const MusWave&, const MusWave&) = default;

    uint32_t base = 0; // an offset in the file of samples
    uint32_t length = 0;
    uint8_t type = 0; // 0 for VADPCM
    bool has_loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t loop_count = 0;
    uint32_t loop_state = 0; // the place of the loop's 16 samples
    uint32_t book = 0;       // the place of the book's coefficients
    int32_t book_size = 0;   // in bytes
};

constexpr uint8_t kMusWaveAdpcm = 0;

// A pointer bank. Its header starts with a text such as "N64 PtrTablesV2".
struct MusBank
{
    // The semitones from a note's number to the wave's pitch, one for each wave: the detune in cents and the base note.
    std::vector<float> pitches;
    std::vector<MusWave> waves;

    // Throws xsflib::FormatError on a file that doesn't hold a bank. The game's rounding mode changes a wave's pitch.
    static MusBank Parse(std::span<const uint8_t> bytes, bool round_toward_zero = false);
};

// The forms of a song's header.
enum class MusSongForm : uint8_t
{
    kEarly,
    kLate,
};

struct MusSong
{
    MusSongForm form = MusSongForm::kEarly;
    std::span<const uint8_t> bytes;
    std::vector<uint32_t> data_lists;   // for each channel, 0 for a channel that doesn't play
    std::vector<uint32_t> volume_lists; // for each channel, 0 for none
    std::vector<uint32_t> bend_lists;   // for each channel, 0 for none
    uint32_t envelopes = 0;
    uint32_t drums = 0;
    uint32_t wave_table = 0;  // the later form: the wave of each of the song's wave numbers
    uint32_t master_list = 0; // the later form: a list that plays no notes, with the tempos and the markers

    // The song's bytes must outlive the object. Throws xsflib::FormatError on bytes that don't hold a song.
    static MusSong Parse(std::span<const uint8_t> bytes, MusSongForm form);
};

// A bank for the player: where the model's memory holds the bank's bytes and the file of samples.
struct MusSoundBank
{
    const MusBank* bank = nullptr;
    uint32_t bank_address = 0;
    uint32_t sample_address = 0;
};

struct MusPlayerConfig
{
    int channels = 24;
    int voices = 24;
    int frames_per_second = 60;
    uint16_t song_master = 0x7FFF;
    uint16_t effect_master = 0x7FFF;
    bool mono = false;

    // The time for a new voice to reach its first volume. A game's setting chooses between no ramp and one frame.
    bool ramp_new_voices = false;
    uint32_t random_seed = 0x7FFFFFFF;

    // Whether the game's sound thread runs with the FPU rounding toward zero.
    bool round_toward_zero = false;
};

// A note that a channel starts.
struct MusNoteStart
{
    int channel = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    uint32_t offset = 0; // the place in the song of the note's first byte
    int wave = 0;
    uint64_t time = 0; // the channel's clock, in 1/256 ticks
};

class MusPlayer
{
public:
    MusPlayer(const MusPlayerConfig& config, AlSynth& synth);

    // Starts a song on the channels that are free. Like the library's call, this takes effect in the next frame.
    void StartSong(const MusSong& song, const MusSoundBank& bank);

    void SetSongMaster(uint16_t volume);
    void SetEffectMaster(uint16_t volume);

    // Stops every channel at once, with the voices' releases.
    void StopAll();

    // The driver's call for a frame. Returns the microseconds to the next one.
    int32_t RunFrame();

    // Whether any channel still runs its list, or any voice still sounds.
    bool Finished() const;

    // Moves the player to `synth`, a copy of the driver that it plays on.
    void UseSynth(AlSynth& synth);

    uint32_t RandomSeed() const;
    void SetRandomSeed(uint32_t seed);

    // Called when a channel starts a note. A copy of the player keeps the function.
    std::function<void(const MusNoteStart&)> on_note;

private:
    // The tools that check the model against a game run read and set the state below through ModelAccess.
    friend struct ModelAccess;

    static constexpr int kForDepth = 4;
    static constexpr int kNoVoice = -1;
    static constexpr int kNoWave = -1;

    struct OpenFor
    {
        uint32_t place = 0;
        uint32_t volume_place = 0;
        uint32_t bend_place = 0;
        uint16_t volume_repeat = 0;
        uint16_t bend_repeat = 0;
        uint8_t count = 0;
        uint8_t volume = 0;
        uint8_t bend = 0;
    };

    struct Channel
    {
        uint64_t time = 0;
        uint64_t volume_time = 0;
        uint64_t bend_time = 0;
        uint64_t next_event = 0;
        uint64_t note_start = 0;
        uint64_t note_off = 0;
        uint64_t sweep_time = 0;
        uint32_t pc = 0;
        uint32_t volume_list = 0;
        uint32_t bend_list = 0;
        uint32_t data_base = 0;
        uint32_t volume_base = 0;
        uint32_t bend_base = 0;
        uint32_t drums = 0;
        int wave = kNoWave;
        int voice = kNoVoice;
        const MusSong* song = nullptr;
        MusSoundBank bank;
        int32_t fade = -1;
        int32_t fade_total = 1;
        int32_t priority = 0;
        float vibrato_depth = 0.0F;
        float bend = 0.0F;
        float last_pitch = 99.9F;
        float note_pitch = 0.0F;
        float distort = 0.0F;
        float distort_amount = 0.0F;
        float portamento_start = 0.0F;
        float portamento_now = 0.0F;
        float attack_step = 1.0F;
        float decay_step = 0.0F;
        float vibrato = 0.0F;
        float bend_range = 1.0F / 32.0F;
        float bend_value = 0.0F;
        uint16_t tempo_scale = 128;
        uint16_t length = 1;
        uint16_t tempo = 0;
        uint16_t base_tempo = 0;
        int16_t velocity_scale = 128;
        int16_t pan_scale = 128;
        uint16_t last_volume = 0xFFFF;
        uint16_t volume_repeat = 1;
        uint16_t bend_repeat = 1;
        uint16_t handle = 0;
        uint16_t ticks = 0;
        uint16_t fixed_length = 0;
        uint16_t wave_number = 0;
        uint16_t cutoff = 0;
        uint16_t end_it = 0;
        uint8_t vibrato_delay = 0;
        uint8_t vibrato_speed = 0;
        uint8_t portamento = 0;
        int8_t transpose = 0;
        uint8_t velocity = 0;
        uint8_t volume = 127;
        uint8_t pan = 64;
        uint8_t last_pan = 255;
        uint8_t envelope_first = 0;
        uint8_t envelope_peak = 127;
        uint8_t envelope_sustain = 127;
        uint8_t envelope_phase = 0;
        uint8_t envelope_value = 0;
        uint8_t attack = 1;
        uint8_t decay = 255;
        uint8_t release = 1;
        uint8_t reverb = 0;
        uint8_t reverb_base = 0;
        uint8_t last_reverb = 255;
        uint8_t wobble_on = 0;
        uint8_t wobble_off = 0;
        uint8_t wobble_count = 0;
        int8_t wobble_value = 0;
        int8_t wobble_amount = 0;
        uint8_t default_velocity = 127;
        uint8_t sweep = 0;
        uint8_t sweep_step = 0;
        uint8_t sweep_down = 0;
        uint8_t for_depth = 0;
        bool ignore_length = false;
        bool ignore_transpose = false;
        bool velocity_on = false;
        bool playing = false;
        std::array<OpenFor, kForDepth> fors = {};
    };

    struct VoiceState
    {
        uint8_t phase = 0;
        uint8_t release = 0;
        uint32_t end_frame = 0;
        int synth_voice = 0;
    };

    struct PendingSong
    {
        const MusSong* song = nullptr;
        MusSoundBank bank;
    };

    void RunChannel(Channel& channel, int index);
    void EndFade(Channel& channel);
    void SendVoice(Channel& channel, float ratio, uint32_t volume, uint32_t pan, uint32_t reverb);
    void StartVoice(Channel& channel, float ratio, uint32_t volume, uint32_t pan, uint32_t reverb);
    void InitChannel(Channel& channel) const;
    void StartPending(const PendingSong& pending);
    void Step(Channel& channel, int index);
    uint32_t RunCommands(Channel& channel);
    void EndNote(Channel& channel);
    uint32_t RunCommand(Channel& channel, uint8_t command, uint32_t place);
    void SetEnvelope(Channel& channel, uint32_t place);
    void SetTempo(Channel& channel, uint8_t value);
    void NoteTimes(Channel& channel) const;
    void RunEnvelope(Channel& channel);
    void ReadVolume(Channel& channel) const;
    void ReadBend(Channel& channel) const;
    void RunSweep(Channel& channel) const;
    float Vibrato(Channel& channel) const;
    static float Wobble(Channel& channel);
    float Pitch(Channel& channel, float offset) const;
    uint32_t VoiceVolume(const Channel& channel) const;
    int Random(int range);
    void ExpireVoices();
    void ReleaseChannelVoice(Channel& channel);
    AlWave WaveOf(const Channel& channel) const;
    uint8_t ByteAt(const Channel& channel, uint32_t place) const;
    uint32_t Sized(const Channel& channel, uint32_t& place) const;

    MusPlayerConfig config_;
    AlSynth* synth_ = nullptr;
    int32_t frame_microseconds_ = 0;
    uint16_t default_tempo_ = 0;
    uint32_t frame_ = 0;
    uint32_t random_ = 0;
    std::vector<Channel> channels_;
    std::vector<VoiceState> voices_;
    std::deque<int> free_voices_;
    std::vector<int> release_voices_;
    std::vector<PendingSong> pending_;
};

// The later library. Pokémon Puzzle League runs it on the standard driver. Each of a song's channels keeps one voice
// for good. The first 4 channels have no voice. They run the master lists of songs.
struct MusLatePlayerConfig
{
    int channels = 32; // the game's count, without the 4 for master lists
    int frames_per_second = 60;
    uint16_t song_master = 0x7FFF;
    uint16_t effect_master = 0x7FFF;
    bool stereo = true;
    int16_t voice_priority = 0;
    uint32_t random_seed = 0x87654321;
    bool round_toward_zero = false;  // the game's sound thread rounds its floating point toward zero
    bool end_after_note_off = false; // a note ends in the first frame after its time, not at it. Mario Tennis does this

    // A rest's velocity byte leaves the channel's velocity as it was. Mario Tennis does this.
    bool rest_keeps_velocity = false;

    // A channel's frame sends its pitch before its volume. A pitch too high for the synthesizer then silences its note
    // in the same frame. Mario Tennis does this.
    bool pitch_before_volume = false;

    bool single_precision = false; // the build of the player does all of its arithmetic in floats. Mario Tennis does
};

class MusLatePlayer
{
public:
    // Takes a physical voice for each channel, as the game's player does. With fewer physical voices than channels, the
    // later channels take the voices of the earlier ones.
    MusLatePlayer(const MusLatePlayerConfig& config, AlSynth& synth);

    // Starts a song. Its channels first run in the next frame.
    void StartSong(const MusSong& song, const MusSoundBank& bank);

    void SetSongMaster(uint16_t volume);
    void SetEffectMaster(uint16_t volume);

    // The driver's call for a frame. Returns the microseconds to the next one.
    int32_t RunFrame();

    // Whether any channel still runs its list.
    bool Finished() const;

    // Moves the player to `synth`, a copy of the driver that it plays on.
    void UseSynth(AlSynth& synth);

    uint32_t RandomSeed() const;
    void SetRandomSeed(uint32_t seed);

    // Called when a channel starts a note. A copy of the player keeps the function.
    std::function<void(const MusNoteStart&)> on_note;

private:
    // The tools that check the model against a game run read and set the state below through ModelAccess.
    friend struct ModelAccess;

    static constexpr int kMasterChannels = 4;
    static constexpr int kForDepth = 4;
    static constexpr int kNoWave = -1;

    struct OpenFor
    {
        uint32_t place = 0;
        uint32_t volume_place = 0;
        uint32_t bend_place = 0;
        uint16_t volume_repeat = 0;
        uint16_t bend_repeat = 0;
        uint8_t count = 0;
        uint8_t volume = 0;
        uint8_t bend = 0;
    };

    struct Channel
    {
        uint64_t time = 0;
        uint64_t volume_time = 0;
        uint64_t bend_time = 0;
        uint64_t next_event = 0;
        uint64_t note_start = 0;
        uint64_t note_off = 0;
        uint64_t sweep_time = 0;
        uint32_t pc = 0;
        uint32_t volume_list = 0;
        uint32_t bend_list = 0;
        uint32_t data_base = 0;
        uint32_t volume_base = 0;
        uint32_t bend_base = 0;
        uint32_t drums = 0;
        uint32_t handle = 0;
        uint32_t envelope_speed_factor = 1024;
        int pending_wave = kNoWave;
        const MusSong* song = nullptr;
        MusSoundBank bank;
        float vibrato_depth = 0.0F;
        float vibrato_step = 0.0F;
        float vibrato = 0.0F;
        float distort = 0.0F;
        float distort_amount = 0.0F;
        float bend = 0.0F;
        float bend_range = 1.0F / 32.0F;
        float bend_value = 0.0F;
        float last_pitch = 99.9F;
        float note_pitch = 0.0F;
        float portamento_start = 0.0F;
        float portamento_now = 0.0F;
        float attack_step = 1.0F;
        float decay_step = 1.0F / 255.0F;
        float release_step = 1.0F / 15.0F;
        uint16_t tempo_scale = 128;
        uint16_t length = 1;
        uint16_t tempo = 0;
        uint16_t base_tempo = 0;
        int16_t velocity_scale = 128;
        int16_t pan_scale = 128;
        uint16_t last_volume = 0xFFFF;
        uint16_t volume_repeat = 1;
        uint16_t bend_repeat = 1;
        uint16_t ticks = 0;
        uint16_t fixed_length = 0;
        uint16_t wave_number = 0;
        uint16_t cutoff = 0;
        uint16_t end_it = 0;
        uint8_t vibrato_delay = 0;
        uint8_t vibrato_speed = 0;
        uint8_t portamento = 0;
        int8_t transpose = 0;
        uint8_t velocity = 0;
        uint8_t volume = 127;
        uint8_t pan = 64;
        uint8_t last_pan = 255;
        uint8_t envelope_speed = 1;
        uint8_t envelope_first = 0;
        uint8_t envelope_peak = 127;
        uint8_t envelope_sustain = 127;
        uint8_t envelope_phase = 0;
        uint8_t envelope_value = 0;
        uint8_t envelope_count = 0;
        uint8_t release_from = 0;
        uint8_t attack = 1;
        uint8_t decay = 255;
        uint8_t release = 15;
        uint8_t reverb = 0;
        uint8_t reverb_base = 0;
        uint8_t last_reverb = 255;
        uint8_t wobble_on = 0;
        uint8_t wobble_off = 0;
        uint8_t wobble_count = 0;
        int8_t wobble_value = 0;
        int8_t wobble_amount = 0;
        uint8_t default_velocity = 127;
        uint8_t sweep = 0;
        uint8_t sweep_step = 0;
        uint8_t sweep_down = 0;
        uint8_t for_depth = 0;
        bool paused = false;
        bool ignore_length = false;
        bool ignore_transpose = false;
        bool velocity_on = false;
        bool keep_note_times = false;
        bool keep_wave = false;
        bool playing = false;
        std::array<OpenFor, kForDepth> fors = {};
    };

    void InitChannel(Channel& channel) const;
    int FindChannel(const MusSong& song, int number) const;
    uint8_t ByteAt(const Channel& channel, uint32_t place) const;
    uint32_t Sized(const Channel& channel, uint32_t& place) const;
    void SetEnvelope(Channel& channel, uint32_t place) const;
    void SetTempo(Channel& channel, uint8_t value);
    int Random(int range);
    uint32_t RunCommand(Channel& channel, uint8_t command, uint32_t place);
    void Step(Channel& channel, int index);
    uint32_t RunCommands(Channel& channel);
    static void ReleaseForRest(Channel& channel);
    void ReadVelocity(Channel& channel, uint8_t note) const;
    void ReadLength(Channel& channel) const;
    uint16_t WaveNumber(const Channel& channel, uint32_t number) const;
    uint8_t StartDrum(Channel& channel, uint8_t note) const;
    static void StartNoteTimes(Channel& channel);
    void ChangeWave(Channel& channel, int voice, int wave);
    void SendReverb(Channel& channel, int voice);
    void StartWave(Channel& channel, int voice);
    void RunChannel(Channel& channel, int index);
    void ReadVolume(Channel& channel) const;
    void ReadBend(Channel& channel) const;
    static int32_t EnvelopeTicks(const Channel& channel, uint64_t from, uint32_t less);
    void RunEnvelope(Channel& channel) const;
    static void RunSweep(Channel& channel);
    static float Wobble(Channel& channel);
    float Vibrato(Channel& channel) const;
    void SendVolumeAndPan(Channel& channel, int voice);
    void SendPitch(Channel& channel, int voice, float offset);

    MusLatePlayerConfig config_;
    AlSynth* synth_ = nullptr;
    int32_t frame_microseconds_ = 0;
    uint16_t default_tempo_ = 0;
    uint32_t next_handle_ = 1;
    uint32_t random_ = 0;
    int first_voice_ = 0;
    std::vector<Channel> channels_;
    std::vector<uint32_t> unpause_;
};

} // namespace ntdlib::n64
