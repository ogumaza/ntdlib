// SPDX-License-Identifier: MIT

// A model of Nintendo EAD's later sound engine for the Nintendo 64. Ocarina of Time and Majora's Mask use it. The two
// games differ in a few commands, a voice's start and end, and how a reverb thins its samples. The engine plays the
// programs of ead_script.h. A frame of output is a few updates, 180 a second at the usual rates. The engine runs a
// frame's updates first:
//
// - Each sequence player that's on adds its tempo to a counter. When the counter passes a limit, one tick runs: the
//   sequence's script, each channel's script and each layer's script. A layer's note takes a voice from the lists that
//   its channel's policy names.
// - Each voice takes its envelope, vibrato and portamento. It then records its settings for that update.
//
// The engine then builds the commands of each update, from those records, for the update's share of the frame.
//
// The model holds its state in plain values and indices. The sound files stay outside it. A copy of the object is a
// snapshot while the files live. A script may write to its sequence. Each player therefore holds a copy of the
// sequence's bytes.
//
// A note's start reaches `on_note`, with the place of its command in the sequence.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/ead_early.h"

namespace ntdlib::n64
{

// The later engine's files in an image.
struct EadLaterFiles
{
    // Reads the files that FindLaterSoundData finds. An image without them gives nothing.
    static std::optional<EadLaterFiles> Read(std::span<const uint8_t> image);

    // A table's entry with no size names the entry that holds its data. These lists hold the data under both numbers.
    std::vector<std::vector<uint8_t>> sequences;
    std::vector<EadBank> banks;

    // The offset in the image of each bank's two tables of samples. A sample's address counts from the one that its
    // medium names.
    std::vector<std::array<uint32_t, 2>> sample_places;

    // Each sequence's banks. The last is its default.
    std::vector<std::vector<uint8_t>> bank_sets;
};

// A reverb's settings in a session's table.
struct EadLaterReverb
{
    uint8_t downsample = 1;
    uint16_t window = 0x20; // in units of 64 samples
    uint16_t decay = 0;
    uint16_t second_delay = 0; // in units of 64 samples: a second read of the buffer, later in it
    uint16_t second_gain = 0;
    uint16_t volume = 0x7FFF;
    uint16_t leak_right_to_left = 0;
    uint16_t leak_left_to_right = 0;
    int8_t send_to = -1; // another reverb that this one adds to, or -1
    uint16_t send_gain = 0;
    int16_t low_pass_left = 0; // a cutoff, or 0 for no filter
    int16_t low_pass_right = 0;
};

enum class EadLaterOutput : uint8_t
{
    kStereo = 0,
    kHeadset = 1,
    kSurround = 2,
    kMono = 3,
    kSurroundEffects = 4, // Majora's Mask's form and later: halves each voice's targets, and adds a surround effect
};

enum class EadLaterVersion : uint8_t
{
    kOcarina,
    kMajora,
    kStadium2, // Majora's Mask's form, with its differences: a layer's flags reach its sound, and the delay
               // kEnvelopeSkip
};

// A session's settings. The game chooses them from a table.
struct EadLaterConfig
{
    EadLaterVersion version = EadLaterVersion::kOcarina;
    int sampling_frequency = 32000;
    int voices = 28;
    int players = 4;
    std::vector<EadLaterReverb> reverbs = {};
    EadLaterOutput output = EadLaterOutput::kStereo;
};

class EadLaterPlayer
{
public:
    static constexpr int kPlayers = 5;
    static constexpr int kChannelsPerPlayer = 16;
    // A channel's layers. Pokémon Stadium 2's build holds 6. The others hold 4.
    static constexpr int kLayersPerChannel = 6;
    static constexpr int kPorts = 8;

    // The files must outlive the player and its copies. The model takes its buffers from `memory`. `memory` holds the
    // image at its kImageBase. The samples' addresses then keep their low bits, like those of the game's DMA.
    EadLaterPlayer(const EadLaterConfig& config, const EadLaterFiles& files, AudioMemory& memory);

    // Starts a sequence on a player.
    void Start(int player, int sequence);
    void Stop(int player);
    bool Enabled(int player) const;

    // Whether a player is enabled or a voice still sounds.
    bool Busy() const;

    // What the game writes to a player: the scale of its volume, a fade to silence over a number of updates, its mute.
    void SetVolumeScale(int player, float scale);
    void FadeOut(int player, int updates);
    void SetMuted(int player, bool muted);

    // What the game writes to a channel: the scale of its volume, its frequency and its pan.
    void SetChannelVolumeScale(int player, int channel, float scale);
    void SetChannelFrequency(int player, int channel, float scale);
    void SetChannelPan(int player, int channel, uint8_t pan);

    // A script's input from the game. A script reads -1 from a port that holds nothing new.
    void WritePlayerPort(int player, int port, int8_t value);
    void WriteChannelPort(int player, int channel, int port, int8_t value);

    // The value that a frame's scripts read as random. The engine takes it from the console's timer and output. A
    // comparison with a game has to feed the game's value. The model steps a simple generator without it.
    void SetRandom(uint32_t value);

    // Appends the commands of a frame of `samples` output samples to `list`, saving them at `output`. Runs the updates
    // of that frame.
    void RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples);

    int UpdatesPerFrame() const;
    int TempoLimit() const;
    uint64_t UpdateCount() const;

    // Called for each note that starts.
    std::function<void(const EadNoteStart&)> on_note;

private:
    static constexpr int kNoLayer = -1;

    struct Script
    {
        uint32_t pc = 0;
        std::array<uint32_t, 4> stack = {};
        std::array<uint8_t, 4> loops = {};
        uint8_t depth = 0;
        int8_t value = 0;
    };

    // An envelope lies in a bank, in a sequence's bytes, or in the engine.
    struct EnvelopeRef
    {
        const EadEnvelope* bank = nullptr;
        int player = -1;
        uint32_t offset = 0;
    };

    struct AdsrSettings
    {
        uint8_t release = 0;
        uint8_t sustain = 0;
        EnvelopeRef envelope;
    };

    struct Portamento
    {
        uint8_t mode = 0;
        uint16_t position = 0;
        uint16_t speed = 0;
        float extent = 0.0F;
    };

    enum class AdsrPhase : uint8_t
    {
        kDisabled,
        kInitial,
        kStartLoop,
        kLoop,
        kFade,
        kHang,
        kDecay,
        kRelease,
        kSustain,
    };

    struct Adsr
    {
        bool hang = false;
        bool decay = false;
        bool release = false;
        bool start_at_target = false; // Pokémon Stadium 2: the envelope has passed a skip point
        AdsrPhase phase = AdsrPhase::kDisabled;
        uint8_t index = 0;
        int16_t delay = 0;
        float sustain = 0.0F;
        float velocity = 0.0F;
        float fade_step = 0.0F;
        float current = 0.0F;
        float target = 0.0F;
        EnvelopeRef envelope;
    };

    struct Vibrato
    {
        bool active = false;
        int channel = -1; // the channel whose settings apply, or -1 for a layer's fixed settings
        uint32_t time = 0;
        float depth = 0.0F;
        float rate = 0.0F;
        uint16_t rate_timer = 0;
        uint16_t depth_timer = 0;
        uint16_t delay = 0;
    };

    // A filter's taps lie in a sequence's bytes or in a voice's saved copy.
    struct FilterRef
    {
        int player = -1;
        uint32_t offset = 0;
        bool saved = false;
    };

    struct Player
    {
        bool enabled = false;
        bool finished = false;
        bool muted = false;
        bool recalculate_volume = false;
        bool stop_script = false;
        bool apply_bend = false;
        uint8_t state = 0;
        uint8_t policy = 0;
        uint8_t mute_behavior = 0;
        uint8_t sequence = 0;
        uint8_t default_font = 0;
        uint16_t tempo = 0;
        uint16_t tempo_count = 0;
        uint16_t tempo_change = 0;
        int16_t transpose = 0;
        uint16_t delay = 0;
        uint16_t fade_updates = 0;
        uint16_t fade_updates_next = 0;
        float fade_volume = 1.0F;
        float fade_step = 0.0F;
        float volume = 0.0F;
        float mute_scale = 0.5F;
        float fade_scale = 1.0F;
        float applied_fade = 0.0F;
        float bend = 1.0F;
        Script script;
        int32_t short_velocities = -1; // the place in the sequence, or -1 for the engine's table
        int32_t short_gates = -1;
        int32_t skip_ticks = 0;
        uint32_t script_counter = 0;
        std::array<int8_t, kPorts> ports = {};
        std::vector<uint8_t> bytes;
    };

    struct Channel
    {
        bool enabled = false;
        bool finished = false;
        bool stop_script = false;
        bool muted = false;
        bool has_instrument = false;
        bool stereo_effects = false;
        bool large_notes = false;
        bool change_frequency = false;
        bool change_volume = false;
        bool change_pan = false;
        uint8_t policy = 0;
        uint8_t mute_behavior = 0;
        uint8_t reverb = 0;
        uint8_t priority = 0;
        uint8_t release_priority = 0;
        uint8_t font = 0;
        uint8_t reverb_index = 0;
        uint8_t book_offset = 0;
        uint8_t new_pan = 0;
        uint8_t pan_weight = 0;
        uint8_t gain = 0;
        uint8_t velocity_random = 0;
        uint8_t gate_random = 0;
        uint8_t comb_size = 0;
        uint8_t stereo = 0;
        uint8_t surround = 0xFF;
        uint16_t start_sample = 0;
        uint16_t vibrato_rate_start = 0;
        uint16_t vibrato_depth_start = 0;
        uint16_t vibrato_rate_target = 0;
        uint16_t vibrato_depth_target = 0;
        uint16_t vibrato_rate_delay = 0;
        uint16_t vibrato_depth_delay = 0;
        uint16_t vibrato_delay = 0;
        uint16_t delay = 0;
        uint16_t comb_gain = 0;
        uint16_t pointer = 0;
        int16_t program = 0;
        int16_t transpose = 0;
        float volume_scale = 1.0F;
        float volume = 1.0F;
        int32_t pan = 0;
        float applied_volume = 0.0F;
        float frequency_scale = 1.0F;
        uint32_t dynamic_table = 0;
        const EadInstrument* instrument = nullptr;
        int player = -1;
        int32_t filter = -1; // the place in the sequence, or -1
        std::array<int, kLayersPerChannel> layers = {kNoLayer, kNoLayer, kNoLayer, kNoLayer, kNoLayer, kNoLayer};
        std::array<int8_t, kPorts> ports = {};
        Script script;
        AdsrSettings adsr;
    };

    struct Layer
    {
        bool enabled = false;
        bool finished = false;
        bool muted = false;
        bool continuous = false;
        bool loaded = false;
        bool ignore_drum_pan = false;
        bool continuous_started = false;
        bool properties_need_init = false;
        uint8_t stereo = 0;
        uint8_t program = 0xFF;
        uint8_t gate = 0x80;
        uint8_t semitone = 0;
        uint8_t portamento_target = 0;
        uint8_t pan = 0x40;
        uint8_t note_pan = 0;
        uint8_t reverb = 0;
        uint8_t surround = 0x80;
        uint16_t flags = 0xFFFF;
        int16_t delay = 0;
        int16_t gate_delay = 0;
        int16_t delay_copy = 0;
        uint16_t portamento_time = 0;
        int16_t transpose = 0;
        int16_t default_length = 0;
        int16_t last_length = 0;
        AdsrSettings adsr;
        Portamento portamento;
        int voice = -1;
        float frequency_scale = 1.0F;
        float bend = 1.0F;
        float velocity_varied = 0.0F;
        float velocity_square = 0.0F;
        float note_velocity = 0.0F;
        float note_frequency = 0.0F;
        const EadInstrument* instrument = nullptr;
        const EadTunedSample* sound = nullptr;
        uint8_t sound_font = 0;
        int channel = -1;
        uint32_t command_place = 0;
        Script script;
    };

    // What a voice plays in an update.
    struct UpdateState
    {
        bool enabled = false;
        bool needs_init = false;
        bool finished = false;
        bool strong_right = false;
        bool strong_left = false;
        bool stereo_effects = false;
        bool headset_effects = false;
        uint8_t reverb_index = 0;
        uint8_t book_offset = 0;
        bool synthetic_wave = false;
        bool two_parts = false;
        bool use_delay = false;
        uint8_t gain = 0;
        uint8_t delay_left = 0;
        uint8_t delay_right = 0;
        uint8_t reverb = 0;
        uint8_t harmonics = 0;
        uint8_t comb_size = 0;
        uint8_t surround = 0xFF;
        uint16_t target_left = 0;
        uint16_t target_right = 0;
        uint16_t pitch = 0;
        uint16_t comb_gain = 0;
        const EadTunedSample* sound = nullptr;
        uint8_t sound_font = 0;
        uint32_t wave_address = 0;
        std::optional<FilterRef> filter;
    };

    struct Attributes
    {
        uint8_t reverb = 0;
        uint8_t gain = 0;
        uint8_t pan = 0;
        uint8_t stereo = 0;
        uint8_t comb_size = 0;
        uint8_t surround = 0xFF;
        uint16_t comb_gain = 0;
        float frequency_scale = 0.0F;
        float velocity = 0.0F;
        std::optional<FilterRef> filter;
        std::array<int16_t, 8> saved_filter = {};
    };

    // The buffers of a voice in the model's memory.
    struct VoiceBuffers
    {
        uint32_t adpcm_state = 0;
        uint32_t resample_state = 0;
        uint32_t filter_state = 0;
        uint32_t delay_state = 0;
        uint32_t comb_state = 0;
        uint32_t filter_taps = 0;
        uint32_t surround_state = 0;
    };

    struct Voice
    {
        int track = -1; // what a host's player calls the voice's track
        uint8_t priority = 0;
        uint8_t wave = 0;
        uint8_t harmonic = 0;
        uint8_t font = 0;
        uint8_t release_kind = 0; // 1 after a decay, 2 after a release
        bool stereo_effects = false;
        float portamento_scale = 1.0F;
        float vibrato_scale = 1.0F;
        int previous_layer = kNoLayer;
        int layer = kNoLayer;
        int wanted_layer = kNoLayer;
        Attributes attributes;
        Adsr adsr;
        Portamento portamento;
        Vibrato vibrato;
        bool restart = false;
        uint8_t previous_delay_left = 0;
        uint8_t previous_delay_right = 0;
        uint8_t reverb = 0;
        uint8_t parts = 0;
        uint16_t position_fraction = 0;
        int32_t position = 0;
        int16_t current_left = 0;
        int16_t current_right = 0;
        bool comb_needs_init = false;
        bool stop_loop = false;
        uint16_t surround_gain = 0;
        uint32_t start_position = 0;
        UpdateState state;
        int pool = 0;
        VoiceBuffers buffers;
    };

    struct ReverbItem
    {
        uint32_t downsample_left = 0;
        uint32_t downsample_right = 0;
        int32_t start = 0;
        int16_t length_a = 0;
        int16_t length_b = 0;
    };

    struct Reverb
    {
        uint8_t resample_flags = 1;
        bool enabled = false;
        uint8_t frames_to_ignore = 2;
        uint8_t current_frame = 0;
        uint8_t downsample = 1;
        int8_t send_to = -1;
        uint16_t window = 0;
        int16_t send_gain = 0;
        int16_t volume = 0;
        uint16_t decay = 0;
        uint16_t downsample_pitch = 0;
        int16_t leak_right_to_left = 0;
        int16_t leak_left_to_right = 0;
        uint16_t second_delay = 0;
        int16_t second_gain = 0;
        int32_t next_position = 0;
        int32_t buffer_samples = 0;
        uint32_t left = 0;
        uint32_t right = 0;
        std::array<uint32_t, 4> resample_states = {};
        std::array<std::array<ReverbItem, 5>, 2> items = {};
        std::array<std::array<ReverbItem, 5>, 2> second_items = {};
        uint32_t filter_left = 0;
        uint32_t filter_right = 0;
        uint32_t filter_left_state = 0;
        uint32_t filter_right_state = 0;
    };

    // A load that a script started and that the engine runs a part of in each frame. The target's port takes the
    // result.
    struct SlowLoad
    {
        bool active = false;
        int32_t bytes_left = 0;
        int player = -1;
        int channel = -1; // a channel's index, or -1 for the player's ports
        int port = 0;
        int32_t sequence = -1; // a sequence to copy into the player's bytes, or -1
        uint32_t copy_to = 0;
    };

    enum ListKind : uint8_t
    {
        kDisabled = 0,
        kDecaying = 1,
        kReleasing = 2,
        kActive = 3,
    };

    // Lists of voices: a voice's index or a list's head.
    int Head(int pool, ListKind kind) const;
    int PlayerPool(int player) const;
    int ChannelPool(int channel) const;
    void PushBack(int head, int voice);
    void PushFront(int head, int voice);
    int PopBack(int head);
    void Remove(int voice);
    void ClearPool(int pool);
    void FillPool(int pool, int count);
    int FindLowerPriority(int head, int limit) const;

    uint8_t Read8(const Player& player, Script& script) const;
    int16_t Read16(const Player& player, Script& script) const;
    uint16_t ReadVariable(const Player& player, Script& script) const;
    uint8_t PeekByte(const Player& player, uint32_t place) const;
    uint16_t Peek16(const Player& player, uint32_t place) const;
    EadEnvelope::Point EnvelopePoint(const EnvelopeRef& envelope, int index) const;
    std::array<int16_t, 8> FilterTaps(const Voice& voice, const FilterRef& filter) const;
    int RealFont(int font) const;

    const EadInstrument* Instrument(int font, int program) const;
    const EadDrum* Drum(int font, int note) const;
    const EadSoundEffect* SoundEffect(int font, int number) const;
    const EadSample* SampleOf(int font, const EadTunedSample* sound) const;

    int FlowCommand(Player& player, Script& script, uint8_t command, uint16_t argument);
    uint16_t FlowArgument(const Player& player, Script& script, uint8_t command) const;

    void ResetPlayer(int player);
    void DisablePlayer(int player);
    void DisableChannelLayers(int player);
    void InitChannel(int channel);
    void DisableChannel(int channel);
    void EnableChannel(int player, int slot, uint32_t script);
    bool SetLayer(int channel, int slot);
    void DisableLayer(int layer);
    void FreeLayer(int channel, int slot);
    void SetPriorities(Channel& channel, uint8_t priorities);
    void SetProgram(int channel, uint8_t program);
    uint8_t FindProgram(Channel& channel, uint8_t program, const EadInstrument*& out, AdsrSettings& adsr);
    void LoadFilter(int player, uint32_t place, int low_pass, int high_pass);
    void StartSlowLoad(SlowLoad load);
    void StartSampleLoad(int player, int channel, int port, int font, int number);
    void RunSlowLoads();
    uint8_t ChannelForm(uint8_t command) const;
    void ReleaseNotesOfFont(int font);

    void RunPlayer(int player);
    void RunChannel(int channel);
    void RunLayer(int layer);
    int LayerCommands(int layer);
    int LayerNote(int layer, int command);
    int LayerSound(int layer, int command);
    void LayerVoice(int layer, bool same_sound);
    void PlayerSound(int player);
    void ChannelSound(int channel, bool recalculate_volume, bool apply_bend);
    void ProcessVoices(int state_offset);
    void InitUpdateState(Voice& voice, UpdateState& state, float frequency, float velocity, uint8_t pan, uint8_t reverb,
                         uint8_t stereo, uint8_t gain, const std::optional<FilterRef>& filter, uint8_t comb_size,
                         uint16_t comb_gain);
    float UpdateAdsr(Adsr& adsr);
    void UpdateVibrato(Voice& voice);
    float VibratoScale(Vibrato& vibrato);
    void InitVibrato(int voice);
    void DecayOrRelease(int layer, AdsrPhase target);
    void InitVoice(Voice& voice);
    void DisableVoice(Voice& voice);
    void InitVoiceForLayer(int voice, int layer);
    int BuildWave(Voice& voice, Layer& layer, int wave);
    void InitWave(Voice& voice, Layer& layer);
    void TakeAfterRelease(int voice, int layer);
    int AllocFromDisabled(int pool, int layer);
    int AllocFromDecaying(int pool, int layer);
    int AllocFromActive(int pool, int layer);
    int AllocVoice(int layer);

    void EnsureBank(AudioMemory& memory, int font);
    void StartReverbItems(AudioMemory& memory, int length, int update, int reverb);
    void BuildUpdate(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update);
    void BuildVoice(AudioList& list, AudioMemory& memory, int voice, UpdateState& state, int length, int update);
    void BuildEnvelope(AudioList& list, const UpdateState& state, Voice& voice, int length, int delay_side);
    void BuildDelay(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags, int delay_side);
    void BuildSurround(AudioList& list, UpdateState& state, Voice& voice, int length, uint32_t flags);
    void SaveAligned(AudioList& list, uint32_t dmem, uint32_t size, uint32_t address);
    void BuildWaveLoad(AudioList& list, const UpdateState& state, Voice& voice, int to_load);
    void LoadReverbSamples(AudioList& list, const Reverb& reverb, int length, int update);
    void SaveReverbSamples(AudioList& list, Reverb& reverb, int update);
    void SendReverb(AudioList& list, const Reverb& reverb, int update);

    // The places in the RSP's memory that the engine's commands name.
    struct DmemLayout
    {
        uint32_t temp;
        uint32_t decoded;
        uint32_t delay_temp;
        uint32_t comb_temp;
        uint32_t compressed_end;
        uint32_t left;
        uint32_t right;
        uint32_t wet_temp;
        uint32_t wet_scratch;
        uint32_t wet_left;
        uint32_t wet_right;
    };

    EadLaterConfig config_;
    const EadLaterFiles* files_;
    bool majora_ = false;
    bool stadium_2_ = false;
    DmemLayout dmem_ = {};

    int frame_target_ = 0;
    int updates_per_frame_ = 0;
    int samples_per_update_ = 0;
    int samples_per_update_max_ = 0;
    int samples_per_update_min_ = 0;
    float resample_rate_ = 1.0F;
    float updates_inverse_ = 0.0F;
    float updates_inverse_scaled_ = 0.0F;
    float updates_scaled_ = 0.0F;
    int ai_frequency_ = 0;
    int16_t max_tempo_ = 0;
    float note_length_scale_ = 0.0F;
    std::array<float, 256> decay_table_ = {};
    uint64_t updates_ = 0;
    int32_t frames_ = 0;
    uint32_t random_ = 0;
    bool random_set_ = false;

    std::array<Player, kPlayers> players_;
    std::vector<Channel> channels_;
    std::vector<Layer> layers_;
    std::vector<int> free_layers_;
    std::vector<uint8_t> layer_is_free_;
    std::vector<Voice> voices_;
    std::vector<int> next_;
    std::vector<int> previous_;
    std::vector<UpdateState> update_states_;
    std::vector<Reverb> reverbs_;
    std::vector<uint8_t> font_loaded_;
    std::array<SlowLoad, 2> slow_loads_ = {};
    int next_slow_load_ = 0;
    std::vector<const EadSample*> samples_in_memory_;
    std::vector<std::array<int8_t, 2>> pending_script_loads_; // a player's number and a port

    // The objects' addresses in the model's memory, for each bank. A bank's are empty until it's used.
    std::vector<std::vector<uint32_t>> book_addresses_;
    std::vector<std::vector<uint32_t>> loop_addresses_;
    uint32_t waves_address_ = 0;
    uint32_t note_book_address_ = 0;
    uint32_t noise_offset_ = 0;
    uint32_t current_book_ = 0;
};

} // namespace ntdlib::n64
