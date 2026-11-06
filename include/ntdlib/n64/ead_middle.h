// SPDX-License-Identifier: MIT

// A model of the form of Nintendo EAD's sound engine between the early and later ones, from Mario Kart 64. The engine
// reads the early engine's files and plays their scripts with the commands of ead_script.h's kMiddle form. It writes
// its voices' settings for each update to a table, like the later engine. It builds a whole frame's commands after it
// has run the frame's updates. It uses the first of the EAD microcodes.
//
// The model holds its state in plain values and indices. The sound files stay outside it. A copy of the object is a
// snapshot while the files live. A script may write to its sequence. Each player therefore holds a copy of the
// sequence's bytes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/ead_bank.h"
#include "ntdlib/n64/ead_early.h"

namespace ntdlib::n64
{

// A reverb's settings in a session's table.
struct EadMiddleReverb
{
    uint8_t downsample = 1;
    uint16_t window = 0x20; // in units of 64 samples
    uint16_t gain = 0;
    bool in_use = true; // the standard microcode's form skips a reverb that isn't in use
};

// A session's settings. The game chooses them from a table.
struct EadMiddleConfig
{
    int frequency = 32000;
    int voices = 28;
    int frames_per_list = 1; // the session's count of video frames for each list of commands
    int refresh_rate = 60;
    float tempo_divisor = 16.713F; // the engine's value for a console with NTSC video
    std::vector<EadMiddleReverb> reverbs = {};
    EadSoundMode sound_mode = EadSoundMode::kStereo;

    // Wave Race 64 builds this engine's lists for the standard microcode, with tables of volume ramps. Mario Kart 64
    // builds them for its microcode.
    bool standard_microcode = false;
    uint16_t dry_gain = 0x7FFF; // the standard microcode's envelope mixer takes it for each voice's output
};

class EadMiddlePlayer
{
public:
    static constexpr int kPlayers = 4;
    static constexpr int kChannelsPerPlayer = 16;
    static constexpr int kChannels = 48;
    static constexpr int kLayersPerChannel = 4;
    static constexpr int kLayers = 64;
    static constexpr int kPorts = 8;

    // The files must outlive the player and its copies. The model takes its buffers from `memory`. `memory` holds the
    // image at its kImageBase. The samples' addresses then keep their low bits, like those of the game's DMA.
    EadMiddlePlayer(const EadMiddleConfig& config, const EadEarlyFiles& files, AudioMemory& memory);

    // Starts a sequence on a player. `fade_in_updates` of 0 starts at full volume.
    void Start(int player, int sequence, int fade_in_updates = 0);
    void Stop(int player);
    bool Enabled(int player) const;

    // Whether a player is enabled or a voice still sounds.
    bool Busy() const;

    // What the game writes to a player: a fade to silence over a number of updates, the scale of its volume, its tempo
    // and its transposition.
    void FadeOut(int player, int updates);
    void SetVolumeScale(int player, float scale);
    void SetTempo(int player, int beats_per_minute);
    void SetTranspose(int player, int8_t transpose);

    // What the game writes to a channel.
    void SetChannelVolumeScale(int player, int channel, float scale);
    void SetChannelVolume(int player, int channel, float volume);
    void SetChannelPan(int player, int channel, uint8_t pan);
    void SetChannelFrequency(int player, int channel, float scale);
    void SetChannelReverb(int player, int channel, uint8_t reverb);
    void SetChannelStopNotes(int player, int channel, bool stop);

    // A script's input from the game. A script reads -1 from a port that holds nothing new.
    void WritePort(int player, int channel, int port, int8_t value);

    // The sequence's value for its variation commands, from the game.
    void SetVariation(int player, int8_t value);

    // Mutes or restores every player. The game's pause does this.
    void SetMuted(bool muted);

    void SetSoundMode(EadSoundMode mode);

    // Marks a bank as loaded. A script can change only to a bank that the engine holds. Start marks a sequence's banks.
    void MarkBankLoaded(int bank);

    // Appends the commands of a frame of `samples` output samples, a multiple of 16, to `list`, saving them at
    // `output`. Runs the updates of that frame.
    void RunFrame(AudioList& list, AudioMemory& memory, uint32_t output, int samples);

    int UpdatesPerFrame() const;
    int TempoLimit() const;
    uint64_t UpdateCount() const;

    // Called for each note that starts.
    std::function<void(const EadNoteStart&)> on_note;

private:
    static constexpr int kNoLayer = -1;
    static constexpr int kNoChannel = -1;
    static constexpr int kNoVoice = -1;

    struct Script
    {
        uint32_t pc = 0;
        std::array<uint32_t, 4> stack = {};
        std::array<uint8_t, 4> loops = {};
        uint8_t depth = 0;
    };

    // An envelope lies in a bank, in a sequence's bytes, or is the engine's default one.
    struct EnvelopeRef
    {
        const EadEnvelope* bank = nullptr;
        int player = -1;
        uint16_t offset = 0;
    };

    struct AdsrSettings
    {
        uint8_t release = 0;
        uint8_t sustain = 0;
        EnvelopeRef envelope;
    };

    struct Adsr
    {
        uint8_t action = 0;
        uint8_t state = 0;
        uint8_t index = 0;
        int16_t delay = 0;
        float sustain = 0.0F;
        float velocity = 0.0F;
        float fade_step = 0.0F;
        float current = 0.0F;
        float target = 0.0F;
        EnvelopeRef envelope;
    };

    struct Portamento
    {
        uint8_t mode = 0;
        float position = 0.0F;
        float speed = 0.0F;
        float extent = 0.0F;
    };

    struct Vibrato
    {
        bool active = false;
        int channel = kNoChannel;
        uint32_t time = 0;
        float depth = 0.0F;
        float rate = 0.0F;
        uint16_t rate_timer = 0;
        uint16_t depth_timer = 0;
        uint16_t delay = 0;
    };

    struct Player
    {
        bool enabled = false;
        bool finished = false;
        bool muted = false;
        bool recalculate_volume = false;
        uint8_t state = 0;
        uint8_t policy = 0;
        uint8_t mute_behavior = 0;
        int sequence = 0;
        uint8_t default_bank = 0;
        uint16_t tempo = 0;
        uint16_t tempo_count = 0;
        int16_t transpose = 0;
        uint16_t delay = 0;
        uint16_t fade_updates = 0;
        uint16_t fade_updates_next = 0;
        float fade_volume = 1.0F;
        float fade_step = 0.0F;
        float mute_scale = 0.5F;
        float fade_scale = 1.0F;
        float applied_fade = 0.0F;
        int8_t variation = -1;
        int32_t short_velocities = -1; // the place in the sequence, or -1 for the engine's table
        int32_t short_gates = -1;
        Script script;
        std::array<int, kChannelsPerPlayer> channels = {};
        std::vector<uint8_t> bytes;
    };

    struct Channel
    {
        bool enabled = false;
        bool finished = false;
        bool stop_script = false;
        bool stop_notes = false;
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
        uint8_t bank = 0;
        uint8_t reverb_index = 0;
        uint8_t book_offset = 0;
        uint8_t new_pan = 0;
        uint8_t pan_weight = 0;
        uint16_t vibrato_rate_start = 0;
        uint16_t vibrato_depth_start = 0;
        uint16_t vibrato_rate_target = 0;
        uint16_t vibrato_depth_target = 0;
        uint16_t vibrato_rate_delay = 0;
        uint16_t vibrato_depth_delay = 0;
        uint16_t vibrato_delay = 0;
        uint16_t delay = 0;
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
        std::array<int, kLayersPerChannel> layers = {kNoLayer, kNoLayer, kNoLayer, kNoLayer};
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
        bool properties_need_init = false;
        uint8_t program = 0xFF;
        uint8_t gate = 0x80;
        uint8_t portamento_target = 0;
        uint8_t pan = 0x40;
        uint8_t note_pan = 0;
        int16_t delay = 0;
        int16_t gate_delay = 0;
        uint16_t portamento_time = 0;
        int16_t transpose = 0;
        int16_t default_length = 0;
        int16_t last_length = 0;
        AdsrSettings adsr;
        Portamento portamento;
        int voice = kNoVoice;
        float frequency_scale = 1.0F;
        float velocity_square = 0.0F;
        float note_velocity = 0.0F;
        float note_frequency = 0.0F;
        const EadInstrument* instrument = nullptr;
        const EadTunedSample* sound = nullptr;
        uint8_t sound_bank = 0;
        int channel = kNoChannel;
        Script script;
    };

    // What a voice plays in an update.
    struct UpdateState
    {
        bool enabled = false;
        bool needs_init = false;
        bool mixer_needs_init = false;
        bool finished = false;
        bool strong_right = false;
        bool strong_left = false;
        bool stereo_effects = false;
        bool headset_effects = false;
        bool synthetic_wave = false;
        bool two_parts = false;
        uint8_t reverb_index = 0;
        uint8_t book_offset = 0;
        uint8_t delay_left = 0;
        uint8_t delay_right = 0;
        uint8_t reverb = 0;
        uint16_t target_left = 0;
        uint16_t target_right = 0;
        uint16_t pitch = 0;
        const EadTunedSample* sound = nullptr;
        uint8_t sound_bank = 0;
        uint32_t wave_address = 0;
    };

    struct Attributes
    {
        uint8_t reverb = 0;
        uint8_t pan = 0;
        float frequency_scale = 0.0F;
        float velocity = 0.0F;
    };

    struct Voice
    {
        int track = -1; // what a host's player calls the voice's track
        uint8_t priority = 0;
        uint8_t harmonic = 0;
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
        uint16_t position_fraction = 0;
        int32_t position = 0;
        uint16_t current_left = 0;
        uint16_t current_right = 0;
        UpdateState state;
        int pool = 0;
        uint32_t adpcm_state = 0;
        uint32_t resample_state = 0;
        uint32_t pan_resample_state = 0;
        uint32_t pan_samples = 0;
        uint32_t mixer_state = 0;
        uint32_t halving_state = 0;
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
        uint8_t frames_to_ignore = 2;
        uint8_t current_frame = 0;
        uint8_t downsample = 1;
        uint16_t gain = 0;
        uint16_t downsample_pitch = 0;
        int32_t next_position = 0;
        int32_t buffer_samples = 0;
        uint32_t left = 0;
        uint32_t right = 0;
        uint32_t resample_left = 0;
        uint32_t resample_right = 0;
        std::array<std::vector<ReverbItem>, 2> items;
    };

    enum ListKind : uint8_t
    {
        kDisabled = 0,
        kDecaying = 1,
        kReleasing = 2,
        kActive = 3,
    };

    // Lists of voices: a voice's index or a list's head. A voice that's in no list has no previous item.
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

    uint8_t PeekByte(const Player& player, uint32_t place) const;
    uint16_t Peek16(const Player& player, uint32_t place) const;
    uint8_t Read8(const Player& player, Script& script) const;
    uint16_t Read16(const Player& player, Script& script) const;
    uint16_t ReadVariable(const Player& player, Script& script) const;
    EadEnvelope::Point EnvelopePoint(const EnvelopeRef& envelope, int index) const;
    bool BankLoaded(int bank) const;
    const EadInstrument* Instrument(int bank, int program) const;
    const EadDrum* Drum(int bank, int note) const;
    const EadSample* SampleOf(int bank, const EadTunedSample* sound) const;
    EnvelopeRef BankEnvelope(int bank, int32_t envelope) const;

    void ResetPlayer(int player);
    void DisablePlayer(int player);
    void InitChannels(int player, uint32_t mask);
    void DisableChannels(int player, uint32_t mask);
    void InitChannel(int channel);
    void DisableChannel(int channel);
    void EnableChannel(int player, int slot, uint32_t script);
    bool SetLayer(int channel, int slot);
    void DisableLayer(int layer);
    void FreeLayer(int channel, int slot);
    uint8_t FindProgram(const Channel& channel, uint8_t program, const EadInstrument*& out, AdsrSettings& adsr) const;
    void SetProgram(int channel, uint8_t program);
    bool ChangeBank(Channel& channel, const Player& player, uint8_t from_back);
    Channel* ChannelOf(int player, int channel);

    void RunPlayer(int player);
    void RunChannel(int channel);
    void RunLayer(int layer);
    void PlayerSound(int player);
    void ChannelSound(int channel, bool recalculate_volume);

    void ProcessVoices();
    float UpdateAdsr(Adsr& adsr);
    void UpdateVibrato(Voice& voice);
    float VibratoScale(Vibrato& vibrato);
    void InitVibrato(int voice);
    void DecayOrRelease(int layer, bool release);
    void InitVoice(Voice& voice);
    void DisableVoice(Voice& voice);
    void InitVoiceForLayer(int voice, int layer);
    void SetVolumes(Voice& voice, float velocity, uint8_t pan, uint8_t reverb);
    void SetPitch(Voice& voice, float frequency);
    int BuildWave(Voice& voice, Layer& layer, int wave);
    void TakeAfterRelease(int voice, int layer);
    int AllocFromDisabled(int pool, int layer);
    int AllocFromDecaying(int pool, int layer);
    int AllocFromActive(int pool, int layer);
    int AllocVoice(int layer);

    void EnsureBank(AudioMemory& memory, int bank);
    void StartReverbItem(AudioMemory& memory, Reverb& reverb, int length, int update);
    void LoadReverbSamples(AudioList& list, const Reverb& reverb, int length, int update);
    void SaveReverbSamples(AudioList& list, Reverb& reverb, int update);
    void BuildUpdate(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update);
    void BuildVoice(AudioList& list, AudioMemory& memory, int voice, UpdateState& state, int length, int update);
    void BuildEnvelope(AudioList& list, const UpdateState& state, Voice& voice, int length, int side);
    void BuildDelay(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags, int side);

    // Wave Race 64 keeps a velocity's square as it is. Mario Kart 64 scales it to 1.
    float VelocityDivisor() const
    {
        return config_.standard_microcode ? 1.0F : 16129.0F;
    }

    // The lists for the standard microcode.
    void BuildRampTables();
    void LoadReverbStandard(AudioList& list, const Reverb& reverb, int length, int update) const;
    void SaveReverbStandard(AudioList& list, Reverb& reverb, int update) const;
    void BuildUpdateStandard(AudioList& list, AudioMemory& memory, uint32_t output, int length, int update);
    void BuildVoiceStandard(AudioList& list, AudioMemory& memory, int voice, UpdateState& state, int length);
    void BuildEnvelopeStandard(AudioList& list, const UpdateState& state, Voice& voice, int length, int side,
                               bool use_reverb);
    static void BuildDelayStandard(AudioList& list, const UpdateState& state, Voice& voice, int size, uint32_t flags,
                                   int side);

    EadMiddleConfig config_;
    const EadEarlyFiles* files_;
    int updates_per_frame_ = 0;
    int samples_per_update_ = 0;
    int samples_per_update_max_ = 0;
    int samples_per_update_min_ = 0;
    float updates_inverse_ = 0.0F;
    float updates_inverse_scaled_ = 0.0F;
    int16_t max_tempo_ = 0;
    uint64_t updates_ = 0;
    std::array<Player, kPlayers> players_;
    std::vector<Channel> channels_;
    std::vector<Layer> layers_;
    std::vector<int> free_layers_;
    std::vector<uint8_t> layer_is_free_;
    std::vector<Voice> voices_;
    std::vector<int> next_;
    std::vector<int> previous_;
    std::vector<UpdateState> update_states_;

    // For each of the update's three sizes: 1024 steps of a volume, for the ramps' two halves.
    std::array<std::vector<float>, 3> ramp_up_;
    std::array<std::vector<float>, 3> ramp_down_;
    int ramp_table_ = 1;
    bool standard_reverb_ = false;
    std::vector<Reverb> reverbs_;
    std::vector<uint8_t> bank_loaded_;
    std::vector<std::vector<uint32_t>> book_addresses_;
    std::vector<std::vector<uint32_t>> loop_addresses_;
    uint32_t waves_address_ = 0;
};

} // namespace ntdlib::n64
