// SPDX-License-Identifier: MIT

// A model of the sound engine of Paper Mario. The engine plays the files of an SBN archive (sbn.h). The engine runs its
// players and its voices for each chunk of 184 samples, and builds the chunk's commands for the compact audio
// microcode. Each player has its tick rate. A track's voices come from a fixed range for its phrase.
//
// The model leaves out the engine's sound effects and ambient sounds.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/sbn.h"

namespace ntdlib::n64
{

// Tables of the game's code. A game's profile reads them from its image.
struct SbnTables
{
    std::array<uint8_t, 8> ticks_per_beat = {};         // for each timing preset of a BGM file
    std::array<uint8_t, 8> default_envelope = {};       // the press list's 4 bytes, then the release list's
    std::array<uint8_t, 40> custom_envelope_times = {}; // for the commands that write a custom envelope
    std::vector<int32_t> envelope_times;                // in microseconds, for each interval of an envelope
    std::vector<float> tune_scaling;                    // 128 and 32 factors above unity, then 128 and 128 below
};

struct SbnPlayerConfig
{
    int output_rate = 32006;
    bool stereo = true;
    uint32_t archive_place = 0; // of the archive in the image
    SbnTables tables;
};

// A note that starts.
struct SbnNoteStart
{
    int player = 0;
    int track = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    int32_t length = 0; // in ticks
    bool drum = false;
    uint32_t offset = 0; // the place of the note's command in the file
    int voice = -1;
};

class SbnPlayer
{
public:
    static constexpr int kPlayers = 2;
    static constexpr int kTracks = 16;
    static constexpr int kVoices = 24;
    static constexpr int kBuses = 4;
    static constexpr int kChunkSamples = 184;

    // The archive must outlive the player and its copies. `memory` holds the image at its kImageBase.
    SbnPlayer(const SbnPlayerConfig& config, const SbnArchive& archive, AudioMemory& memory);

    // Loads a song's files, like the game's call to load one. Fails when `song` isn't in the archive's list.
    bool LoadSong(int player, int song);

    // Starts the loaded song. The times are in milliseconds and the volumes run up to 127.
    void StartSong(int player, int variation, int duration = 0, int start_volume = 127, int final_volume = 127);
    void Stop(int player);
    bool Playing(int player) const;

    // Whether a player plays or a voice still sounds.
    bool Busy() const;

    // What the game writes: the mix of a song's linked tracks.
    void SetProximityMix(int player, uint32_t mix);

    // Appends the commands of a frame of `samples` output samples, a multiple of 184, to `list`, saving them at
    // `output`. Runs the players and the voices for each chunk.
    void RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples);

    // Called for each note that starts.
    std::function<void(const SbnNoteStart&)> on_note;

private:
    // The tools that check the model against a game run read and set the state below through ModelAccess.
    friend struct ModelAccess;

    struct Wave
    {
        uint32_t data = 0;
        uint32_t length = 0;
        uint32_t book = 0;
        uint32_t book_size = 0;
        uint32_t loop_state = 0;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        uint32_t loop_count = 0;
        uint16_t key_base = 0;
        uint8_t type = 0;
        float pitch_ratio = 0.5F;
        std::span<const uint8_t> file;
        uint32_t envelopes = 0; // in the file
    };

    // A place in an envelope's commands: in a file, in the default envelope or in a player's custom one. An empty
    // span names the default envelope.
    struct EnvelopePlace
    {
        std::span<const uint8_t> bytes;
        int custom_player = -1;
        int custom = 0;
        uint32_t at = 0;
    };

    struct Envelope
    {
        EnvelopePlace press;
        EnvelopePlace release;
    };

    struct Voice
    {
        int host_track = -1; // what a host's player calls the voice's track
        // The engine's side.
        int wave = -1;
        float pitch_ratio = 0.0F;
        int16_t client_volume = 0;
        int16_t volume = -1;
        int32_t delta = 0;
        uint8_t pan = 0xFF;
        uint8_t reverb = 0xFF;
        uint8_t bus = 0;
        uint8_t sync_flags = 0;
        bool done_pending = false;
        uint8_t priority = 0;
        uint8_t client_priority = 0;
        Envelope envelope;
        bool has_commands = false;
        EnvelopePlace commands;
        EnvelopePlace loop_start;
        uint8_t loop_counter = 0;
        uint8_t envelope_flags = 0;
        int32_t scale = 0x80;
        int32_t relative_start = 0x80;
        bool relative_release = false;
        int32_t initial = 0;
        int32_t target = 0;
        int32_t duration = 0;
        int32_t time_left = 0;
        float envelope_delta = 0.0F;
        uint8_t interval = 0;
        int track = -1;

        // The synthesis driver's side.
        uint32_t decoder_state = 0;
        uint32_t decoder_loop_state = 0;
        uint32_t resampler_state = 0;
        uint32_t mixer_state = 0;
        int physical_wave = -1;
        uint32_t book_size = 0;
        int32_t last_sample = 0;
        bool decoder_first = true;
        int64_t memory_in = 0;
        int32_t sample = 0;
        uint32_t loop_start_sample = 0;
        uint32_t loop_end_sample = 0;
        int32_t loop_count = 0;
        float resampler_delta = 0.0F;
        bool resampler_first = true;
        float ratio = 1.0F;
        bool mixer_dirty = true;
        bool playing = false;
        int16_t mixer_volume = 1;
        int16_t target_left = 1;
        int16_t target_right = 1;
        int16_t current_left = 1;
        int16_t current_right = 1;
        int16_t dry = 0;
        int16_t wet = 0;
        int16_t rate_high_left = 1;
        uint16_t rate_low_left = 0;
        int16_t rate_high_right = 1;
        uint16_t rate_low_right = 0;
        int32_t mixer_delta = 0;
        int32_t segment_end = 0;
        uint8_t mixer_pan = 64;
        uint8_t physical_bus = 0;
    };

    struct Note
    {
        int wave = -1;
        float pitch_ratio = 2.0F;
        int32_t detune = 0;
        int16_t rand_detune = 0;
        int32_t volume = 0;
        int32_t velocity = 0;
        int32_t length = 0;
        bool pending_tick = false;
        uint8_t tremolo_depth = 0;
        uint8_t tremolo_phase = 0;
        uint8_t tremolo_delay = 0;
    };

    struct Track
    {
        uint32_t position = 0;
        uint32_t saved_position = 0;
        uint32_t previous_position = 0;
        int wave = -1;
        Envelope envelope;
        int32_t instrument_volume = 0;
        int32_t volume_step = 0;
        int32_t volume_target = 0;
        int32_t volume_ticks = 0;
        int32_t proximity_volume = 0;
        int32_t proximity_step = 0;
        int32_t proximity_target = 0;
        int32_t proximity_ticks = 0;
        int16_t coarse_detune = 0;
        int16_t fine_detune = 0;
        int16_t detune = 0;
        int32_t delay = 0;
        uint8_t pan = 0x40;
        uint8_t random_pan = 0;
        uint8_t reverb = 0;
        uint8_t patch = 0;
        uint8_t volume = 0x7F;
        uint8_t press_override = 0;
        uint8_t tremolo_depth = 0;
        uint8_t tremolo_delay = 0;
        uint8_t tremolo_rate = 0;
        uint8_t detour_length = 0;
        uint8_t proximity_1 = 0;
        uint8_t proximity_2 = 0;
        bool proximity_set_changed = false;
        bool proximity_value_changed = false;
        bool changed_volume = false;
        bool changed_pan = false;
        bool changed_reverb = false;
        bool changed_tune = false;
        bool drums = false;
        bool muted = false;
        uint8_t polyphony = 0;
        uint8_t voice_count = 0;
        uint8_t first_voice = 0;
        uint8_t last_voice = 0;
        uint8_t linked_track = 0;
        uint8_t bus = 0;
    };

    struct Fade
    {
        int32_t base_volume = 0;
        int32_t base_step = 0;
        int16_t base_target = 0;
        uint16_t base_ticks = 0;
        int32_t envelope_volume = 0;
        int32_t envelope_step = 0;
        int16_t envelope_target = 0;
        uint16_t envelope_ticks = 0;
    };

    struct Player
    {
        std::span<const uint8_t> file;
        std::shared_ptr<const BgmFile> parsed;
        int song = -1;
        bool active = false;
        uint8_t priority = 0;
        uint8_t bus = 0;
        std::array<int8_t, 4> effect_indices = {};
        std::array<uint8_t, 4> effect_values = {};
        int32_t update_step = 0;
        int32_t tick_interval = 0;
        int32_t update_counter = 0;
        uint32_t max_tempo = 0;
        int32_t bus_volume = 0x8000;
        int32_t master_tempo = 0;
        int32_t master_tempo_bpm = 0;
        int32_t tempo_ticks = 0;
        int32_t tempo_target = 0;
        int32_t tempo_step = 0;
        int32_t master_volume = 0;
        int32_t volume_ticks = 0;
        int32_t volume_target = 0;
        int32_t volume_step = 0;
        int16_t pitch_shift = 0;
        int16_t detune = 0;
        float playback_rate = 1.0F;
        bool paused = false;
        bool volume_changed = false;
        Fade fade;
        uint8_t state = 0;
        uint32_t composition = 0;
        uint32_t composition_start = 0;
        std::array<uint32_t, 32> loop_labels = {};
        std::array<uint32_t, 4> loop_ends = {};
        std::array<uint8_t, 4> loop_counters = {};
        uint32_t loop_depth = 0;
        uint32_t phrase = 0;
        uint32_t condition_flags = 0;
        int32_t proximity_id = 0;
        int32_t proximity_value = 0;
        uint8_t proximity_volume = 0;
        bool first_link_mute = true;
        uint32_t counter = 0;
        uint32_t frames = 0;
        uint32_t random_1 = 0;
        uint32_t random_2 = 0;
        std::array<uint8_t, 4> args = {};
        std::array<uint8_t, 8> polyphony_counts = {};
        std::array<std::array<uint16_t, 9>, 8> custom_envelopes = {};
        std::array<uint8_t, 8> custom_write = {};
        uint8_t custom_writing = 0;
        std::array<Track, kTracks> tracks;
        std::array<Note, kVoices> notes;
    };

    struct LowPass
    {
        int16_t cutoff = 0;
        int16_t gain = 0;
        std::array<int16_t, 16> coefficients = {};
        uint32_t coefficients_address = 0;
        uint32_t state = 0;
        bool first = true;
    };

    struct Delay
    {
        uint32_t input = 0;
        uint32_t output = 0;
        int16_t feed_forward = 0;
        int16_t feed_back = 0;
        int16_t gain = 0;
        float mod_increment = 0.0F;
        float mod_value = 0.0F;
        int32_t mod_delta = 0;
        float mod_gain = 0.0F;
        bool has_filter = false;
        LowPass filter;
        bool has_resampler = false;
        uint32_t resampler_state = 0;
        float resampler_delta = 0.0F;
        bool resampler_first = true;
    };

    struct Effect
    {
        uint32_t base = 0;
        int32_t input = 0;
        int32_t length = 0;
        int delay_count = 0;
        std::array<Delay, 4> delays;
    };

    struct Bus
    {
        uint16_t gain = 0x7FFF;
        uint8_t effect_type = 0;
        std::array<Effect, 2> effects;
    };

    struct EffectChange
    {
        uint8_t type = 0;
        bool changed = false;
    };

    int AddWave(const BkFile& file, std::span<const uint8_t> bytes, uint32_t file_place,
                const BkInstrument& instrument);
    void LoadBank(uint16_t file, int bank, int bank_set);
    int WaveOf(int bank, int patch, Envelope& envelope) const;
    uint8_t EnvelopeByte(const EnvelopePlace& place) const;
    uint8_t ReadEnvelope(EnvelopePlace& place) const;
    float PitchRatio(int32_t tuning) const;
    void InitPlayer(Player& player, int priority, int bus, std::array<int8_t, 4> effects);
    void SetTickResolution(Player& player, int32_t step, uint32_t ticks_per_beat);
    void RunChunkClients();
    void BeginChunk();
    void UpdateFade(int index);
    void UpdateBusVolumes(Player& player);
    void SetBusVolume(int bus, uint16_t volume, int32_t bus_volume);
    void RunPlayer(int index);
    void Initialize(int index);
    void ReadComposition(Player& player);
    void EndCompositionLoop(Player& player, uint32_t command);
    static void RepeatCompositionLoop(Player& player, uint32_t& depth, uint32_t label);
    void LoadPhrase(Player& player, uint32_t command);
    void UpdateStop(int index);
    void UpdatePlaying(int index);
    static bool StepPlayerRamps(Player& player);
    static void StepTrackRamps(Track& track, bool changed_volume);
    bool RunTrackCommands(int index, int t, Track& track);
    void StartNote(int index, int t, Track& track, uint8_t opcode, uint32_t command_place, uint8_t& search_from);
    bool AcquireVoice(Player& player, const Track& track, uint8_t& search_from, uint8_t& voice_index);
    bool StealVoice(Player& player, const Track& track, uint8_t& voice_index);
    void SetUpDrumNote(const Player& player, const Track& track, int32_t key, int32_t level, Voice& voice, Note& note);
    void SetUpInstrumentNote(int index, const Track& track, int32_t key, int32_t level, Voice& voice, Note& note);
    void UpdateTrackNotes(Player& player, const Track& track);
    void UpdateNote(const Player& player, const Track& track, Voice& voice, Note& note);
    void UpdateDrumNote(const Player& player, const Track& track, int32_t level, Voice& voice, Note& note);
    void UpdateInstrumentNote(const Player& player, const Track& track, int32_t level, Voice& voice, Note& note);
    void SetNotePitch(Voice& voice, Note& note, int32_t tuning);
    uint8_t ReadTrackByte(Player& player, Track& track);
    void RunCommand(int index, Track& track, uint8_t opcode);
    void Special(int index, Track& track);
    void ResetVoice(int voice);
    void ResetAllVoices(const Player& player);
    void UpdateVoices();
    static int32_t EnvelopeLevel(const Voice& voice);
    static int32_t ApplyVolumeChange(Voice& voice);
    static void SetEnvelopeVolume(Voice& voice, int32_t current);
    static void SetSegmentVolume(Voice& voice);
    void ReleaseEnvelope(Voice& voice);
    void StepVoiceEnvelope(Voice& voice);
    void AfterVolumeChange(Voice& voice);
    void StartEnvelope(Voice& voice);
    uint8_t StepEnvelope(Voice& voice);
    int32_t IntervalTime(uint8_t interval) const;
    void SynthStopVoice(int voice);
    void SynthStartVoice(int voice);
    void RefreshMixer(Voice& voice);
    void SynthSetMixer(int voice, int16_t volume, int32_t delta, uint8_t pan, uint8_t reverb);
    void SynthSetPanAndMix(int voice, uint8_t pan, uint8_t reverb);
    void SynthSetVolumeDelta(int voice, int16_t volume, int32_t delta);
    void LoadEffect(Bus& bus, Effect& effect, uint8_t type);
    void InitFilter(LowPass& filter, AudioMemory& memory);
    void BuildChunk(AudioList& list, AudioMemory& memory, uint32_t place);
    void DelayBusChannel(AudioList& list) const;
    void AccumulateBus(AudioList& list, const Bus& bus, bool first_bus) const;
    void PullVoice(AudioList& list, int index, AudioMemory& memory);
    void DecodeAdpcmLoop(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out);
    void DecodeAdpcm(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out);
    void LoadPcmLoop(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out);
    void LoadPcm(AudioList& list, Voice& voice, const Wave& wave, int32_t count, int& out);
    void MixVoice(AudioList& list, Voice& voice);
    void DecodeChunk(AudioList& list, Voice& voice, const Wave& wave, int tsam, int bytes, int16_t out, uint32_t flags);
    void PullEffect(AudioList& list, Effect& effect, int16_t wet);
    void ResampleDelay(AudioList& list, const Effect& effect, Delay& delay, int output_buffer);
    void LoadDelayLine(AudioList& list, const Effect& effect, int64_t position, int buffer, int count);

    SbnPlayerConfig config_;
    const SbnArchive* archive_ = nullptr;
    std::vector<Wave> waves_;
    int default_wave_ = -1;
    std::array<std::array<std::array<int, 16>, 16>, 8> bank_sets_ = {};
    std::array<BgmDrum, 72> global_drums_ = {};
    std::array<BgmInstrument, 0x40> global_instruments_ = {};
    std::array<Voice, kVoices> voices_;
    std::array<Bus, kBuses> buses_;
    std::array<EffectChange, kBuses> effect_changes_;
    std::array<Player, kPlayers> players_;
    uint32_t dry_accumulator_ = 0;
    uint32_t wet_accumulator_ = 0;
    uint32_t delay_main_ = 0;
    uint32_t delay_aux_ = 0;
    int32_t delay_counter_ = 0;
    uint8_t delayed_bus_ = 0;
    uint8_t delayed_channel_ = 0;
    int32_t delay_count_ = 4;
    uint8_t request_delay_time_ = 0;
    uint8_t request_delay_side_ = 0;
    uint8_t request_delay_bus_ = 0;
    bool request_delay_pending_ = false;
    AudioMemory* build_memory_ = nullptr;
};

} // namespace ntdlib::n64
