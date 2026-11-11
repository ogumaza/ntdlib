// SPDX-License-Identifier: MIT

// A model of Nintendo EAD's early sound engine for the Nintendo 64, from the American build of Super Mario 64. The
// engine plays the programs of ead_script.h. A frame of output is a few updates, 240 a second at the usual rates. In
// each update:
//
// - Each sequence player that's on adds its tempo to a counter. When the counter passes a limit, one tick runs: the
//   sequence's script, each channel's script and each layer's script. A layer's note takes a voice from the lists that
//   its channel's policy names.
// - The voices take their envelopes, vibratos and portamentos.
// - The engine appends the commands for that update's samples: each voice's ADPCM, its resampling and its mix.
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
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/seq_file.h"

namespace ntdlib::n64
{

// The early engine's files in an image.
struct EadEarlyFiles
{
    // Reads the files that FindEarlySoundData finds. An image without them gives nothing.
    static std::optional<EadEarlyFiles> Read(std::span<const uint8_t> image);

    SeqFile sequences;
    std::vector<EadBank> banks;

    // The offset in the image of each bank's samples. A sample's address counts from there.
    std::vector<uint32_t> sample_places;

    // Each sequence's banks. The last is its default.
    std::vector<std::vector<uint8_t>> bank_sets;
};

enum class EadSoundMode : uint8_t
{
    kStereo = 0,
    kHeadset = 1,
    kMono = 3,
};

// A session's settings. The game chooses them from a table for each level.
struct EadEarlyConfig
{
    int ai_frequency = 32006; // the console's output rate
    int voices = 16;
    int reverb_window = 0xC00; // in samples; 0 for no reverb
    uint16_t reverb_gain = 0x2FFF;
    int16_t volume = 0x7FFF;
    EadSoundMode sound_mode = EadSoundMode::kStereo;
};

// A note that starts.
struct EadNoteStart
{
    int player = 0;
    int channel = 0;
    int layer = 0;
    uint8_t key = 0; // after the transpositions: the note, or the drum
    bool drum = false;
    uint16_t bank = 0;
    int16_t program = 0;   // the channel's instrument or wave value
    float velocity = 0.0F; // the layer's velocity, squared
    float pan = 0.5F;      // the layer's
    int32_t length = 0;    // in ticks
    int32_t gate = 0;      // the ticks before the note's end that it decays: the length's share of its gate
    uint32_t offset = 0;   // the place of the note's command in the sequence
    uint64_t update = 0;   // the engine's count of updates
    int voice = -1;
};

class EadEarlyPlayer
{
public:
    static constexpr int kPlayers = 3;
    static constexpr int kChannelsPerPlayer = 16;
    static constexpr int kLayersPerChannel = 4;

    // The files must outlive the player and its copies. The model takes its buffers from `memory`. `memory` holds the
    // image at its kImageBase. The samples' addresses then keep their low bits, like those of the game's DMA.
    EadEarlyPlayer(const EadEarlyConfig& config, const EadEarlyFiles& files, AudioMemory& memory);

    // Starts a sequence on a player.
    void Start(int player, int sequence);
    void Stop(int player);
    bool Enabled(int player) const;

    // Whether a player is enabled or a voice still sounds.
    bool Busy() const;

    // What the game writes to a player's fade: the volume at once, or a fade over a number of updates.
    void SetFadeVolume(int player, float volume);
    void FadeTo(int player, float volume, int updates);

    // A script's input from the game. Each channel has 8 ports. A script reads -1 from a port that holds nothing new.
    void WritePort(int player, int channel, int port, int8_t value);

    // The sequence's value for its variation commands, from the game.
    void SetVariation(int player, int8_t value);

    // Appends the commands of a frame of `samples` output samples, a multiple of 16, to `list`, saving them at
    // `output`. Runs the updates of that frame.
    void RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples);

    // Runs one update's scripts and voices, without commands.
    void RunUpdate();

    int UpdatesPerFrame() const;
    int TempoLimit() const;
    uint64_t UpdateCount() const;

    // Called for each note that starts.
    std::function<void(const EadNoteStart&)> on_note;

private:
    struct Script
    {
        uint32_t pc = 0;
        std::array<uint32_t, 4> stack = {};
        std::array<uint8_t, 4> loops = {};
        uint8_t depth = 0;
    };

    // An envelope lies in a bank, or in a sequence's bytes.
    struct EnvelopeRef
    {
        const EadEnvelope* bank = nullptr;
        int player = -1;
        uint32_t offset = 0;
    };

    struct AdsrSettings
    {
        uint8_t release = 0;
        uint16_t sustain = 0;
        EnvelopeRef envelope;
    };

    struct Portamento
    {
        uint8_t mode = 0;
        float position = 0.0F;
        float speed = 0.0F;
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
        uint8_t action = 0;
        AdsrPhase phase = AdsrPhase::kDisabled;
        int16_t target = 0;
        int16_t current = 0;
        int16_t index = 0;
        int16_t delay = 0;
        int16_t sustain = 0;
        int16_t fade_step = 0;
        int32_t velocity = 0;
        int32_t current_fine = 0;
        EnvelopeRef envelope;
    };

    struct Vibrato
    {
        bool active = false;
        int channel = -1;
        uint32_t time = 0;
        uint16_t rate = 0;
        uint16_t extent = 0;
        uint16_t rate_timer = 0;
        uint16_t extent_timer = 0;
        uint16_t delay = 0;
    };

    struct Player
    {
        bool enabled = false;
        bool muted = false;
        int8_t variation = -1;
        uint8_t state = 0;
        uint8_t policy = 0;
        uint8_t mute_behavior = 0;
        uint8_t sequence = 0;
        uint8_t default_bank = 0;
        uint16_t tempo = 0;
        uint16_t tempo_count = 0;
        uint16_t fade_updates = 0;
        int16_t transpose = 0;
        uint16_t delay = 0;
        float fade_volume = 1.0F;
        float fade_step = 0.0F;
        float volume = 0.0F;
        float mute_scale = 0.5F;
        std::array<int, kChannelsPerPlayer> channels = {};
        Script script;
        int32_t short_velocities = -1; // the place in the sequence, or -1 for the engine's table
        int32_t short_gates = -1;
        std::vector<uint8_t> bytes;
    };

    struct Channel
    {
        bool enabled = false;
        bool finished = false;
        bool script_stopped = false;
        bool has_instrument = false;
        bool stereo_effects = false;
        bool large_notes = false;
        uint8_t policy = 0;
        uint8_t mute_behavior = 0;
        uint8_t reverb = 0;
        uint8_t priority = 0;
        uint8_t bank = 0;
        uint16_t vibrato_rate_start = 0;
        uint16_t vibrato_extent_start = 0;
        uint16_t vibrato_rate_target = 0;
        uint16_t vibrato_extent_target = 0;
        uint16_t vibrato_rate_delay = 0;
        uint16_t vibrato_extent_delay = 0;
        uint16_t vibrato_delay = 0;
        uint16_t delay = 0;
        int16_t program = 0;
        int16_t transpose = 0;
        float volume_scale = 1.0F;
        float volume = 1.0F;
        float pan = 0.5F;
        float pan_weight = 1.0F;
        float frequency_scale = 1.0F;
        uint32_t dynamic_table = 0;
        const EadInstrument* instrument = nullptr;
        int player = -1;
        std::array<int, kLayersPerChannel> layers = {-1, -1, -1, -1};
        std::array<int8_t, 8> ports = {};
        Script script;
        AdsrSettings adsr;
    };

    struct Layer
    {
        bool enabled = false;
        bool finished = false;
        bool silent = false; // no sound from this command on
        bool continuous = false;
        bool loaded = false;
        uint8_t gate = 0x80;
        uint8_t portamento_target = 0;
        Portamento portamento;
        AdsrSettings adsr;
        uint16_t portamento_time = 0;
        int16_t transpose = 0;
        float frequency_scale = 0.0F;
        float velocity_square = 0.0F;
        float pan = 0.5F;
        float note_velocity = 0.0F;
        float note_pan = 0.0F;
        float note_frequency = 0.0F;
        int16_t default_length = 0;
        int16_t last_length = 0;
        int16_t delay = 0;
        int16_t duration = 0;
        int voice = -1;
        const EadInstrument* instrument = nullptr;
        const EadTunedSample* sound = nullptr;
        int channel = -1;
        Script script;
    };

    // The buffers of a voice in the model's memory.
    struct VoiceBuffers
    {
        uint32_t adpcm_state = 0;
        uint32_t final_resample = 0;
        uint32_t mix_state = 0;
        uint32_t pan_resample = 0;
        uint32_t pan_samples = 0;
        uint32_t dummy_resample = 0;
        uint32_t samples = 0;
    };

    struct Voice
    {
        int track = -1; // what a host's player calls the voice's track
        bool enabled = false;
        bool needs_init = false;
        bool restart = false;
        bool finished = false;
        bool mixer_needs_init = false;
        bool strong_right = false;
        bool strong_left = false;
        bool stereo_effects = false;
        bool headset_effects = false;
        uint8_t priority = 0;
        uint8_t sample_count = 0;
        int16_t program = 0;
        uint8_t bank = 0;
        int16_t adsr_volume = 0;
        uint16_t headset_right = 0;
        uint16_t headset_left = 0;
        uint16_t previous_headset_right = 0;
        uint16_t previous_headset_left = 0;
        int32_t position = 0;
        uint16_t position_fraction = 0;
        float portamento_scale = 1.0F;
        float vibrato_scale = 1.0F;
        const EadTunedSample* sound = nullptr;
        int previous_layer = -1;
        int layer = -1;
        int wanted_layer = -1;
        float frequency = 0.0F;
        uint16_t target_left = 0;
        uint16_t target_right = 0;
        uint8_t reverb = 0;
        uint8_t saved_reverb = 0;
        float saved_frequency = 0.0F;
        float saved_velocity = 0.0F;
        float saved_pan = 0.0F;
        Adsr adsr;
        Portamento portamento;
        Vibrato vibrato;
        int16_t current_left = 0;
        int16_t current_right = 0;
        int16_t reverb_shifted = 0;
        int pool = 0;
        VoiceBuffers buffers;
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
    int PopLowerPriority(int head, int limit);

    uint8_t Read8(const Player& player, Script& script) const;
    uint16_t Read16(const Player& player, Script& script) const;
    uint16_t ReadVariable(const Player& player, Script& script) const;
    uint8_t PeekByte(const Player& player, uint32_t place) const;
    EadEnvelope::Point EnvelopePoint(const EnvelopeRef& envelope, int index) const;

    void InitVoices(AudioMemory& memory);
    void WriteBooksAndLoops(AudioMemory& memory);
    void ResetPlayer(int player);
    void DisablePlayer(int player);
    void InitChannels(int player, uint16_t mask);
    void DisableChannels(int player, uint16_t mask);
    void EnableChannel(int player, int slot, uint32_t script);
    void InitChannel(int channel);
    void DisableChannel(int channel);
    int AllocateChannel() const;
    int SetLayer(int channel, int slot);
    void DisableLayer(int layer);
    void FreeLayer(int channel, int slot);

    int FindInstrument(const Channel& channel, uint8_t program, const EadInstrument*& out, AdsrSettings& adsr) const;
    void SetInstrument(int channel, uint8_t program);

    void RunPlayer(int player);
    void RunChannel(int channel);
    void RunLayer(int layer);
    void PlayerSound(int player);
    void ChannelSound(int channel);
    void ReclaimVoices();
    void ProcessVoices();
    bool RetireVoice(int voice);
    void UpdateAdsr(Voice& voice);
    void ReadEnvelopePoint(Adsr& adsr) const;
    static void StepDecayOrRelease(Adsr& adsr);
    void UpdateVibrato(Voice& voice);
    float VibratoScale(Vibrato& vibrato);
    void InitVibrato(int voice);
    void DecayOrRelease(int layer, AdsrPhase target);
    void InitVoiceForLayer(int voice, int layer);
    void EnableVoice(Voice& voice);
    void DisableVoice(Voice& voice);
    void SetVelocityPanReverb(Voice& voice, float velocity, float pan, uint8_t reverb);
    void BuildWave(Voice& voice, Layer& layer, AudioMemory* memory);
    void InitWave(Voice& voice, Layer& layer);
    void ReleaseAndTake(int voice, int layer);
    int AllocFromDisabled(int pool, int layer);
    int AllocFromDecaying(int pool, int layer);
    int AllocFromActive(int pool, int layer);
    int AllocFromPools(int layer, std::initializer_list<int> pools);
    int AllocVoice(int layer);

    void Update(AudioList& list, AudioMemory& memory, uint32_t output, int samples);
    void BuildReverbStart(AudioList& list, int samples);
    void BuildReverbEnd(AudioList& list);
    void BuildVoices(AudioList& list, uint32_t output, int samples);
    void BuildVoice(AudioList& list, Voice& voice, int samples, const BankBook*& loaded_book);
    static uint32_t LoadWave(AudioList& list, Voice& voice, int32_t whole_samples);
    uint32_t DecodeSample(AudioList& list, Voice& voice, int parts, int32_t whole_samples, int flags,
                          const BankBook*& loaded_book);
    void DecodeAdpcm(AudioList& list, Voice& voice, const EadSample& sample, int wanted, int& flags, int& first_offset);
    void BuildEnvelope(AudioList& list, Voice& voice, int samples, int headset);
    void BuildHeadset(AudioList& list, Voice& voice, int length, int flags, int headset);

    EadEarlyConfig config_;
    const EadEarlyFiles* files_;
    int updates_per_frame_ = 4;
    int tempo_limit_ = 14360;
    uint64_t updates_ = 0;

    std::array<Player, kPlayers> players_;
    std::vector<Channel> channels_;
    std::vector<Layer> layers_;
    std::vector<int> free_layers_;
    std::vector<uint8_t> layer_is_free_;
    std::vector<Voice> voices_;
    std::vector<int> next_;
    std::vector<int> previous_;

    // The objects' addresses in the model's memory, for each bank.
    std::vector<std::vector<uint32_t>> book_addresses_;
    std::vector<std::vector<uint32_t>> loop_addresses_;

    // The pending wave writes of voices, for the frame's memory.
    AudioMemory* build_memory_ = nullptr;

    uint32_t reverb_left_ = 0;
    uint32_t reverb_right_ = 0;
    int reverb_position_ = 0;

    // The reverb's place in this update.
    int reverb_length_a_ = 0;
    int reverb_length_b_ = 0;
    int reverb_start_ = 0;
};

} // namespace ntdlib::n64
