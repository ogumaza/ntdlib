// SPDX-License-Identifier: MIT

// A model of libultra's synthesis driver, for the Nintendo 64. The model builds the audio command list that the game's
// alAudioFrame builds, from the same state.
//
// The driver holds physical voices. Each is a chain of three filters: a decoder (VADPCM or raw 16-bit samples), a
// resampler and an envelope mixer. The mixers feed an auxiliary bus. An effect can read that bus. The main bus sums the
// effect's output with the dry output, and the last filter saves the interleaved output in memory. A list is built in
// chunks of at most 160 samples, pulling the filters from the output back.
//
// A client (a sequence player) changes a voice with updates that carry a time in samples. Each update goes in a queue
// on the voice's envelope mixer. The mixer runs up to each update's time, then applies it. The game's table of free
// updates has a fixed size, and an update without a free entry is dropped.
//
// The compact form of the driver builds lists for the compact audio microcode. The N64 Music Tools use that form. Its
// chunks hold 184 samples. Each voice has one envelope mixer in a chunk. An update therefore takes effect on a chunk's
// edge. The volumes ramp in straight lines.
//
// The model keeps its state in plain values. A copy of the object is a snapshot.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"

namespace ntdlib::n64
{

constexpr int kAlMaxChunkSamples = 160;
constexpr int kAlCompactChunkSamples = 184;

// Where the driver's filters keep their buffers in DMEM.
constexpr int kAlDecoderIn = 0;
constexpr int kAlResamplerOut = 0;
constexpr int kAlTemp0 = 0;
constexpr int kAlDecoderOut = 320;
constexpr int kAlTemp1 = 320;
constexpr int kAlTemp2 = 640;
constexpr int kAlMainLeft = 1088;
constexpr int kAlMainRight = 1408;
constexpr int kAlAuxLeft = 1728;
constexpr int kAlAuxRight = 2048;

// A wave for a voice, with addresses in the model's memory.
struct AlWave
{
    bool raw = false; // raw 16-bit samples in place of VADPCM
    uint32_t base = 0;
    uint32_t length = 0;
    bool has_loop = false;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    uint32_t loop_count = 0;
    uint32_t loop_state = 0; // the address of the loop's 16 samples
    uint32_t book = 0;       // the address of the book's coefficients
    int32_t book_size = 0;   // in bytes
};

// libultra's effect presets, and one that takes parameters from the config.
enum class AlEffect : uint8_t
{
    kNone,
    kSmallRoom,
    kBigRoom,
    kChorus,
    kFlange,
    kEcho,
    kCustom,
};

enum class AlSynthForm : uint8_t
{
    kStandard,
    kCompact,
};

struct AlSynthConfig
{
    AlSynthForm form = AlSynthForm::kStandard;
    int voices = 16;
    int updates = 128;
    int output_rate = 32000;
    AlEffect effect = AlEffect::kNone;
    std::vector<int32_t> custom_effect; // the parameters of kCustom, in alFxNew's order

    // Whether the game's sound thread runs with the FPU rounding toward zero.
    bool round_toward_zero = false;

    // Whether the compact form computes its volume ramps in single precision. Mario Tennis does.
    bool single_precision_ramps = false;

    // The later compact library, in Chameleon Twist 2. A falling ramp with no time holds its volume (0x800CF930). An
    // update's time rounds to a chunk before the time of the last update comes off (0x800CFD68). An update with no
    // samples since the last takes its target at once (0x800CFF38).
    bool later_compact = false;

    // Whether the effect's filters keep a gain and taps of zero. Puzzle League's library sets only their cutoffs.
    bool filters_stay_clear = false;

    // Whether the standard form's ramps are straight lines in single precision. GoldenEye 007's library works them out
    // that way (0x7001AAF4 and 0x7001B150).
    bool linear_ramps = false;
};

class AlSynth
{
public:
    static constexpr int kOutsideVoice = -1;

    // Allocates the driver's state in `memory`.
    AlSynth(const AlSynthConfig& config, AudioMemory& memory);

    // Adds virtual voices, one for each of a player's voice states, and returns the first one's number.
    int AddVoices(int count);

    // alSynAllocVoice. Returns whether the voice has a physical voice. A voice that loses its physical voice to another
    // goes on without one.
    bool AllocVoice(int voice, int16_t priority, bool unity_pitch);
    void FreeVoice(int voice);
    void SetPriority(int voice, int16_t priority);
    void StartVoiceParams(int voice, const AlWave& wave, float pitch, int16_t volume, uint8_t pan, uint8_t fx_mix,
                          int32_t microseconds);
    void StartVoice(int voice, const AlWave& wave);
    void StopVoice(int voice);
    void SetVolume(int voice, int16_t volume, int32_t microseconds);
    void SetPan(int voice, uint8_t pan);
    void SetPitch(int voice, float pitch);
    void SetFxMix(int voice, uint8_t fx_mix);

    // The samples for a time in microseconds, rounded to the driver's multiples of 16 samples.
    int32_t TimeToSamples(int32_t microseconds) const;
    int32_t TimeToSamplesNoRound(int32_t microseconds) const;

    // The time of the driver's clock for updates: the time of the client's callback, on a multiple of 16 samples.
    void SetParamSamples(int32_t samples);
    int32_t ParamSamples() const;
    int32_t CurrentSamples() const;

    // Appends the commands for `samples` output samples to `list`, in chunks, saving them from `output`. The caller has
    // run the clients up to the frame's end. It moves the driver's clock on and collects the voices that are done.
    void BuildCommands(AudioList& list, AudioMemory& memory, uint32_t output, int samples);

    // A voice's physical voice, or -1.
    int PhysicalVoice(int voice) const;

    // Whether a chunk of the compact form has found the voice's whole wave played. Hey You, Pikachu!'s library sets a
    // flag in the voice for this at 0x80050028, and its player ends the voice at the next call.
    bool VoiceFinished(int voice) const;

    int VoiceCount() const;
    int PhysicalVoiceCount() const;

    // What a meter or a mute names: the client's number for a physical voice, or -1. The player sets it when it starts
    // a note.
    void SetVoiceTag(int voice, int tag);
    int PhysicalVoiceTag(int physical) const;

    // Whether a physical voice still plays a wave.
    bool AnyVoicePlaying() const;

private:
    enum class ParamType : uint8_t
    {
        kFreeVoice,
        kSetPitch,
        kSetVolume,
        kSetPan,
        kSetFxMix,
        kStartVoiceAlt,
        kStartVoice,
        kStopVoice,
    };

    struct Param
    {
        ParamType type = ParamType::kSetVolume;
        int32_t delta = 0;
        int32_t data = 0;
        int32_t more = 0;
        float pitch = 0.0F;
        bool unity = false;
        int16_t volume = 0;
        uint8_t pan = 0;
        uint8_t fx_mix = 0;
        int32_t samples = 0;
        AlWave wave;
        int physical = -1;
    };

    struct Voice
    {
        int physical = -1;
        int16_t priority = 0;
        bool unity_pitch = false;
        bool has_wave = false;
        bool finished = false;
    };

    struct PhysicalVoiceState
    {
        int voice = -1;
        int offset = 0;
        int tag = -1;

        // The decoder.
        uint32_t decoder_state = 0;
        uint32_t decoder_loop_state = 0;
        bool has_wave = false;
        AlWave wave;
        int32_t bytes_book = 0;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        uint32_t loop_count = 0;
        int32_t sample = 0;
        int32_t last_sample = 0;
        bool decoder_first = true;
        int64_t memory_in = 0;

        // The resampler.
        uint32_t resampler_state = 0;
        float ratio = 1.0F;
        bool unity_pitch = false;
        float delta = 0.0F;
        bool resampler_first = true;

        // The envelope mixer.
        uint32_t mixer_state = 0;
        int16_t pan = 0;
        int16_t volume = 1;
        int16_t current_left = 1;
        int16_t current_right = 1;
        int16_t dry = 0;
        int16_t wet = 0;
        uint16_t rate_low_left = 0;
        int16_t rate_high_left = 1;
        int16_t target_left = 1;
        uint16_t rate_low_right = 0;
        int16_t rate_high_right = 1;
        int16_t target_right = 1;
        int32_t mixer_delta = 0;
        int32_t segment_end = 0;
        bool mixer_first = true;
        bool playing = false;
        std::deque<Param> updates;
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
        int16_t feedback = 0;
        int16_t gain = 0;
        float mod_increment = 0.0F;
        float mod_value = 0.0F;
        int32_t mod_delta = 0;
        float mod_gain = 0.0F;
        bool has_low_pass = false;
        LowPass low_pass;
        bool has_resampler = false;
        uint32_t resampler_state = 0;
        float resampler_delta = 0.0F;
        bool resampler_first = true;
    };

    struct Fx
    {
        bool present = false;
        uint32_t base = 0;
        int32_t input = 0; // in samples from the base
        uint32_t length = 0;
        std::vector<Delay> delays;
    };

    void InitFx(const AlSynthConfig& config, AudioMemory& memory);
    bool AllocParam();
    void AddUpdate(int physical, Param param);

    // A physical voice's filters. Each appends to `list` and returns the DMEM offset that the next filter reads.
    void PullFx(AudioList& list, AudioMemory& memory, int count, int32_t offset);
    void PullAux(AudioList& list, AudioMemory& memory, int count, int32_t offset);
    void PullMixer(AudioList& list, AudioMemory& memory, int physical, int count, int32_t offset);
    void PullSubFrame(AudioList& list, PhysicalVoiceState& voice, int16_t& in, int16_t& out, int count, int32_t offset);
    void PullResampler(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count, int32_t offset);
    void PullDecoder(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count);
    void PullAdpcm(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count);
    void PullRaw(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count);
    void DecodeChunk(AudioList& list, PhysicalVoiceState& voice, int tsam, int bytes, int16_t out, int16_t in,
                     uint8_t flags);

    void LoadFxBuffer(AudioList& list, int64_t position, int buffer, int count);
    void SaveFxBuffer(AudioList& list, int64_t position, int buffer, int count);
    void LoadFxOutput(AudioList& list, Delay& delay, int buffer, int count);
    void FilterBuffer(AudioList& list, LowPass& low_pass, int buffer, int count);

    // The compact form's filters. Each works on one chunk.
    void BuildCompactCommands(AudioList& list, AudioMemory& memory, uint32_t output, int samples);
    void PullCompactFx(AudioList& list, AudioMemory& memory, int32_t offset);
    void PullCompactAux(AudioList& list, AudioMemory& memory, int32_t offset);
    void PullCompactMixer(AudioList& list, AudioMemory& memory, int physical, int32_t offset);
    void PullCompactSubFrame(AudioList& list, PhysicalVoiceState& voice, int16_t& in, int count);
    void PullCompactResampler(AudioList& list, PhysicalVoiceState& voice, int16_t in);
    void PullCompactAdpcm(AudioList& list, PhysicalVoiceState& voice, int16_t& out, int count);
    void CompactDecodeChunk(AudioList& list, PhysicalVoiceState& voice, int tsam, int bytes, int16_t out,
                            uint32_t flags);
    void CompactLoadFx(AudioList& list, int64_t position, int buffer, int count);
    void CompactSaveFx(AudioList& list, int64_t position, int buffer);
    void CompactLoadFxOutput(AudioList& list, Delay& delay, int buffer);
    void CompactFilter(AudioList& list, LowPass& low_pass, int buffer);

    void ApplySetWave(AudioMemory& memory, PhysicalVoiceState& voice, const AlWave& wave);
    void ResetFilters(PhysicalVoiceState& voice);
    void MoveToLame(int physical);
    int AllocatePhysical(int16_t priority, bool& stolen);

    AlSynthForm form_ = AlSynthForm::kStandard;
    bool round_toward_zero_ = false;
    bool single_precision_ramps_ = false;
    bool later_compact_ = false;
    bool linear_ramps_ = false;
    int output_rate_ = 32000;
    int32_t current_samples_ = 0;
    int32_t param_samples_ = 0;
    int free_updates_ = 0;
    std::vector<Voice> voices_;
    std::vector<PhysicalVoiceState> physical_;
    std::vector<int> free_list_;
    std::vector<int> lame_list_;
    std::vector<int> alloc_list_;
    Fx fx_;
};

} // namespace ntdlib::n64
