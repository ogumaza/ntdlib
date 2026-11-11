// SPDX-License-Identifier: MIT

// A model of the Nintendo 64's audio microcodes. A microcode runs a list of 64-bit commands on sample buffers in the
// RSP's 4 KB of data memory (DMEM), and moves bytes between DMEM and main memory by DMA. The model holds DMEM as bytes
// at the microcode's addresses. Its results were checked sample for sample against an exact RSP that runs the
// microcodes from the games' images.
//
// Each microcode's dialect of the commands differs: EAD's have no segments in their later forms, and the compact one
// has fixed buffers of 184 samples. Libultra's standard microcode counts buffer addresses from 0x5C0. Its fixed-point
// results follow these rules:
//
//     a sample that the mixer scales:  clamp(((a * 0x7FFF + b * g) * 2 + 0x8000) >> 16)
//     a multiply in the resampler:     clamp((a * b * 2 + 0x8000) >> 16)
//     an ADPCM sample:                 clamp(s >> 11), where s is a sum that wraps at 32 bits
//     a pole filter's sample:          clamp(s >> 14), the same
//
// A DMA runs on 8-byte multiples of both addresses and rounds its length up to a multiple of 8 bytes. A clear and a
// move run on 16 bytes at a time. A mixer runs on 32 bytes at a time, and an envelope mixer on 16.
//
// A vector register that an unaligned load fills only partly keeps its other values. The model keeps each command's
// registers from its start, with zeros. A game that mixes on unaligned buffers would differ.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ntdlib/n64/audio_list.h"
#include "ntdlib/n64/audio_memory.h"

namespace ntdlib::n64
{

enum class AudioMicrocode : uint8_t
{
    kStandard,  // libultra's: Super Mario 64, Wave Race 64 and most games with libultra's sequence players
    kCompact,   // for frames of 184 samples, with fixed buffers: Paper Mario, Mario Tennis, Mario Golf, Pokémon Stadium
    kEadFirst,  // EAD's first with five buses: Mario Kart 64
    kEadSecond, // Ocarina of Time
    kEadThird,  // Majora's Mask and Pokémon Stadium 2

    // The standard microcode's commands, with volume ramps that are straight lines rather than powers: GoldenEye 007.
    kStandardLinear,
};

enum class AudioAddressing : uint8_t
{
    kFlat,      // a command's address is the bus's address in AudioMemory's layout
    kSegmented, // the console's: 24 bits and a segment from the table that a segment command fills
};

// What a command does to samples, with its addresses worked out: the record of a run that the enhanced mixing mode runs
// again in floating point (EnhancedMix). A command that only sets up the microcode, or that decodes a wave, leaves no
// operation. Places are bytes in DMEM.
enum class AudioOpKind : uint8_t
{
    kClear,
    kMove,
    kLoad,
    kSave,
    kMix,
    kAdd,
    kGain,
    kMultiply,
    kHalve,
    kDuplicate,
    kInterleave,
    kPole,
    kFilter,
    kResample,
    kNearest,
    kEnvMixer,

    // The marks of a list.
    kVoice,
    kVoicesEnd,
    kChunkEnd,

    // A voice's resampler. EnhancedMix replaces it with an interpolator.
    kMainResample,
};

struct AudioOp
{
    AudioOpKind kind = AudioOpKind::kClear;
    bool init = false;
    uint32_t in = 0;
    uint32_t out = 0;
    uint32_t bytes = 0;
    uint32_t group = 0;   // kMove: the bytes that the microcode copies at once. kDuplicate: the times.
    uint32_t address = 0; // main memory, or a state
    int32_t gain = 0;     // kMix: 1.15. kGain: 16.16. kPole: its gain. kNearest: the step.
    uint32_t extra = 0;   // the operation's index in AudioOps's lists. kInterleave: the right side. kPole: its scale.
    int track = -1;       // kVoice
};

// A resampler. Its input's samples go from `start`, tap 0 of the first output's filter. `in_tap` is the tap of the
// first sample that isn't from the state.
struct AudioResampleOp
{
    uint32_t start = 0;
    uint32_t out = 0;
    uint32_t bytes = 0;
    uint32_t state = 0;
    uint16_t first_fraction = 0;
    uint16_t pitch = 0;
    uint8_t in_tap = 4;
    uint8_t mode = 0; // 0 for the state's 4 samples, 2 for 2 of them, and 4 for each of them twice
    bool init = false;
    uint32_t inputs = 0; // the place in AudioOps::inputs of taps from 0
    uint32_t input_count = 0;
};

// An envelope mixer. A dry side's gain is its ramp times its factor. A wet side's is the ramp of its side, times its
// factor, times the wet ramp.
struct AudioEnvOp
{
    uint32_t in = 0;
    uint32_t dry_left = 0;
    uint32_t dry_right = 0;
    uint32_t wet_left = 0;
    uint32_t wet_right = 0;
    uint32_t bytes = 0;
    uint32_t state = 0;
    bool init = false;
    bool has_wet = true;
    bool swap = false;
    float from_left = 0.0F;
    float to_left = 0.0F;
    float from_right = 0.0F;
    float to_right = 0.0F;
    float from_wet = 1.0F;
    float to_wet = 1.0F;
    float dry_factor_left = 1.0F;
    float dry_factor_right = 1.0F;
    float wet_factor_left = 1.0F;
    float wet_factor_right = 1.0F;
    uint32_t inputs = 0; // the place in AudioOps::inputs of the input, where no resampler came before in the voice
    uint32_t input_count = 0;
};

struct AudioOps
{
    void Clear()
    {
        ops.clear();
        resamples.clear();
        envs.clear();
        tables.clear();
        inputs.clear();
    }

    std::vector<AudioOp> ops;
    std::vector<AudioResampleOp> resamples;
    std::vector<AudioEnvOp> envs;
    std::vector<int16_t> tables;
    std::vector<float> inputs;
};

// The resampler's filters: 64 phases of 4 taps in 1.15 format.
const std::array<int16_t, 256>& AudioResampleTable();

class AudioRsp
{
public:
    static constexpr std::size_t kDmemSize = 0x1000;

    explicit AudioRsp(AudioMicrocode microcode = AudioMicrocode::kStandard,
                      AudioAddressing addressing = AudioAddressing::kFlat);

    // Does what a microcode does before a task's first command.
    void StartTask();

    // Runs the commands of `commands` in their order.
    void Run(std::span<const AudioCommand> commands, AudioBus& bus);

    std::array<uint8_t, kDmemSize>& Dmem();
    const std::array<uint8_t, kDmemSize>& Dmem() const;

    // Keeps a record of what the commands do in `ops`, or keeps none for nullptr. The commands' results stay the same.
    void RecordOps(AudioOps* ops);

    // Adds a list's mark to the record.
    void RecordMark(AudioOpKind kind, int track);

private:
    using Vector = std::array<int16_t, 8>;

    // The places of a microcode's buffers and fields in DMEM.
    struct Layout
    {
        uint32_t buffers = 0; // a command's buffer addresses count from here
        uint32_t book = 0;
        uint32_t fields = 0;
        uint32_t scratch = 0;
        uint32_t loop = 0; // the place of the loop's address, from the fields
    };

    // Where a resampler stopped: the place of the next output sample's input, and its fraction.
    struct ResampleEnd
    {
        int32_t place = 0;
        uint16_t fraction = 0;
    };

    // The places of an envelope mixer's input and its four outputs in DMEM.
    struct EnvPlaces
    {
        uint32_t in = 0;
        uint32_t dry_left = 0;
        uint32_t dry_right = 0;
        uint32_t wet_left = 0;
        uint32_t wet_right = 0;
    };

    // An envelope mixer's ramps, in the order of the mixer's state in DMEM.
    struct EnvRamps
    {
        // The ramps' values in each lane: the whole part and the fraction, on the left and the right.
        Vector left = {};
        Vector left_fraction = {};
        Vector right = {};
        Vector right_fraction = {};

        // The ramps' settings: the left's target and rate, the right's target and rate, then the two gains.
        Vector settings = {};
    };

    // The masks of an EAD envelope mixer's outputs, and whether it swaps the wet sides.
    struct EnvMasks
    {
        uint16_t left = 0;
        uint16_t right = 0;
        uint16_t wet_left = 0;
        uint16_t wet_right = 0;
        bool swap = false;
    };

    uint16_t Get16(uint32_t address) const;
    void Put16(uint32_t address, uint16_t value);
    uint32_t Get32(uint32_t address) const;
    void Put32(uint32_t address, uint32_t value);
    void LoadQuad(Vector& vector, uint32_t address) const;
    void StoreQuad(const Vector& vector, uint32_t address);
    void LoadDouble(Vector& vector, int first, uint32_t address) const;
    void StoreDouble(const Vector& vector, int first, uint32_t address);
    uint32_t Field(uint32_t offset) const;
    void SetField(uint32_t offset, uint16_t value);
    uint32_t Address(uint32_t word) const;
    void DmaRead(uint32_t dmem, uint32_t address, uint32_t length_minus_one, const AudioBus& bus);
    void DmaWrite(uint32_t dmem, uint32_t address, uint32_t length_minus_one, AudioBus& bus) const;

    void RunStandard(const AudioCommand& command, AudioBus& bus);
    void RunCompact(const AudioCommand& command, AudioBus& bus);
    void RunEadFirst(const AudioCommand& command, AudioBus& bus);
    void RunEadLate(const AudioCommand& command, AudioBus& bus);
    void Adpcm(const AudioCommand& command, AudioBus& bus);
    void DecodeAdpcm(uint32_t in, uint32_t out, int32_t count, uint32_t flags, uint32_t state, uint32_t loop_address,
                     bool small, AudioBus& bus);
    ResampleEnd ResampleLoop(int32_t start, int32_t out, int32_t count, uint16_t first_fraction, uint16_t pitch,
                             uint32_t table);
    void RecordOp(AudioOpKind kind, uint32_t in, uint32_t out, uint32_t bytes);
    void RecordResample(AudioResampleOp op);
    void RecordEnv(AudioEnvOp op);
    void ClearBuffer(const AudioCommand& command);
    void EnvMixer(const AudioCommand& command, AudioBus& bus);
    void LoadBuffer(const AudioCommand& command, const AudioBus& bus);
    void Resample(const AudioCommand& command, AudioBus& bus);
    void SaveBuffer(const AudioCommand& command, AudioBus& bus);
    void Segment(const AudioCommand& command);
    void SetBuffer(const AudioCommand& command);
    void SetVolume(const AudioCommand& command);
    void DmemMove(const AudioCommand& command);
    void LoadAdpcm(const AudioCommand& command, const AudioBus& bus);
    void Mixer(const AudioCommand& command);
    void Interleave(const AudioCommand& command);
    void PoleFilter(const AudioCommand& command, AudioBus& bus);
    void SetLoop(const AudioCommand& command);
    void SaveResampleState(const ResampleEnd& end, int32_t first_in, uint32_t state, AudioBus& bus);
    void LoadEnvRamps(EnvRamps& ramps, uint32_t place) const;
    void StoreEnvRamps(const EnvRamps& ramps, uint32_t place);
    static void RecordPlaces(AudioEnvOp& env, const EnvPlaces& places);
    void LoadEnvSide(bool aux, Vector& dry_samples, uint32_t dry_place, Vector& wet_samples, uint32_t wet_place) const;
    void StoreEnvSide(bool aux, const Vector& dry_samples, uint32_t dry_place, const Vector& wet_samples,
                      uint32_t wet_place);
    void MixEnvGroups(EnvRamps& ramps, EnvPlaces places, int32_t count, bool init, bool aux);

    void EadLoad(const AudioCommand& command, const AudioBus& bus);
    void EadSave(const AudioCommand& command, AudioBus& bus);
    void EadMixer(const AudioCommand& command);
    void EadAddBuffer(const AudioCommand& command);
    void EadCopyRepeated(const AudioCommand& command);
    void EadDuplicate(const AudioCommand& command);
    void EadHalve(const AudioCommand& command);
    void EadFirstEnvMixer(const AudioCommand& command);
    void EadLateEnvMixer(const AudioCommand& command);
    void RecordEadLateEnv(const EnvPlaces& places, const EnvMasks& masks, int32_t count);
    void EadLateResample(const AudioCommand& command, AudioBus& bus);
    void EadZeroOrderResample(const AudioCommand& command);
    void EadFilter(const AudioCommand& command, AudioBus& bus);
    void EadGain(const AudioCommand& command);
    void EadInterleave(const AudioCommand& command);
    void EadDecodeBytes(const AudioCommand& command, AudioBus& bus);
    void EadThirdDmemMove(const AudioCommand& command);
    void EadMultiplyTable(const AudioCommand& command);

    void CompactClear(const AudioCommand& command);
    void CompactLoad(const AudioCommand& command, const AudioBus& bus);
    void CompactSave(const AudioCommand& command, AudioBus& bus);
    void CompactSetVolume(const AudioCommand& command);
    void CompactInterleave();
    void CompactMove(const AudioCommand& command);
    void CompactAdpcm(const AudioCommand& command, AudioBus& bus);
    void CompactResample(const AudioCommand& command, AudioBus& bus);
    void CompactEnvMixer(const AudioCommand& command, AudioBus& bus);
    void MixCompactEnvGroups(EnvRamps& ramps, bool init, int16_t right_volume);
    void MixEnvBuffer(uint32_t place, const Vector& samples, const Vector& volume, int16_t amount);
    void CompactMixer(const AudioCommand& command);

    AudioMicrocode microcode_;
    AudioAddressing addressing_;
    Layout layout_;

    // The microcodes' registers that stay from one command to the next.
    Vector gains_ = {};
    uint32_t ramp_left_ = 0;
    uint32_t ramp_right_ = 0;
    uint32_t ramp_wet_ = 0;
    int32_t filter_count_ = 0;
    std::array<uint8_t, kDmemSize> dmem_ = {};
    AudioOps* ops_ = nullptr;
    bool resampled_ = false; // the record holds a resampler since its last mark
};

} // namespace ntdlib::n64
