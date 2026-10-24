// SPDX-License-Identifier: MIT

// The audio work of the model of the DSP's audio firmware (firmware_model.h): what the firmware does to data memory
// for nn::snd. The firmware's state lives in data memory at the firmware's addresses, and each routine reads and writes
// it there. The comments give the addresses of the firmware's routines in Pokémon X's dspaudio.cdc. FirmwareModel
// decides when each routine runs.
//
// In the enhanced mixing mode, the sources' structures and state lie in host memory (HostSources), in the firmware's
// layouts. Each source in turn takes source 0's places in data memory while the firmware's routines take its
// parameters, step it through its buffers and report on it. The enhanced renderer (firmware_enhanced.cpp) then mixes
// the samples that the source decoded.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "xsflib/bytes.h"

#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/ctr/dsp/fcram.h"

namespace ntdlib::wave
{
class SincKernel;
} // namespace ntdlib::wave

namespace ntdlib::ctr::dsp::firmware
{

// The firmware runs with data memory in two banks of 0x10000 words. Words above 0x8000 follow the bank that the
// firmware selects: bank 0, apart from its copies to and from the shared structures of region 1. Word address `a` of
// bank 1 is a | kBank1 here.
constexpr uint32_t kBank1 = 0x10000;
constexpr uint32_t kDataWords = 2 * kBank1;

constexpr int kSources = dsp::kSources;
constexpr int kFrameSamples = 160;
constexpr int kEnhancedFrameSamples = static_cast<int>(dsp::kEnhancedFrameSamples);

// The shared structures, by their places in the list that the firmware sends through the audio pipe. Each has a copy in
// each region: region 1's lies in bank 1.
constexpr std::array<uint16_t, 15> kSharedStructures = {0xbfff, 0x9e92, 0x8680, 0xa792, 0x9430, 0x8400, 0x8540, 0x9492,
                                                        0x8710, 0x8410, 0xa912, 0xaa12, 0xaad2, 0xac52, 0xac5c};
constexpr uint16_t kFrameCounter = 0xbfff;
constexpr uint16_t kSourceParams = 0x9e92; // 24 entries of 0x60 words
constexpr uint16_t kSourceParamsSize = 0x60;
constexpr uint16_t kReports = 0x8680;    // 24 entries of 6 words
constexpr uint16_t kAdpcmCoefs = 0xa792; // 24 entries of 16 words
constexpr uint16_t kAdpcmCoefsSize = 16;
constexpr uint16_t kMixerParams = 0x9430; // 0x62 words
constexpr uint16_t kMixerParamsSize = 0x62;
constexpr uint16_t kStatus = 0x8400; // 16 words
constexpr uint16_t kStatusSize = 16;
constexpr uint16_t kFinalSamples = 0x8540; // 0x140 words
constexpr uint16_t kFinalSamplesSize = 0x140;
constexpr uint16_t kAuxShared = 0x9492; // two buses of 0x500 words
constexpr uint16_t kProfile = 0x8410;   // 0x130 words of timings
constexpr uint16_t kProfileSize = 0x130;
constexpr uint16_t kSurroundShared = 0xa912;                                 // the surround's tables
constexpr std::array<uint16_t, 2> kSurroundFiltersShared = {0xaa12, 0xaad2}; // the surround's filters
constexpr std::array<uint16_t, 2> kSurroundPositionsShared = {0xac52, 0xac5c};

// A source's parameters (SourceParams), by word. The ARM11 stores the dirty bits and the floats low word first, and its
// other 32-bit values high word first.
namespace params
{
constexpr uint16_t kDirty = 0x00;
constexpr uint16_t kGain = 0x02; // [bus][channel], two words each
constexpr uint16_t kRate = 0x1a;
constexpr uint16_t kInterpolation = 0x1c; // interpolation | kernel << 8
constexpr uint16_t kFilters = 0x1d;
constexpr uint16_t kOnePole = 0x1e; // b0, a1
constexpr uint16_t kBiquad = 0x20;  // a2, a1, b2, b1, b0
constexpr uint16_t kQueueDirty = 0x25;
constexpr uint16_t kQueue = 0x26;  // four buffers of kQueueEntrySize words
constexpr uint16_t kReport = 0x50; // the firmware's copy of the source's report, 6 words
constexpr uint16_t kEnable = 0x50;
constexpr uint16_t kSync = 0x51;
constexpr uint16_t kPlayPosition = 0x52;
constexpr uint16_t kAddress = 0x56;
constexpr uint16_t kLength = 0x58;
constexpr uint16_t kFormat = 0x5a;
constexpr uint16_t kAdpcm = 0x5b; // predictor and scale, two history samples
constexpr uint16_t kBufferFlags = 0x5e;
constexpr uint16_t kBufferId = 0x5f;
} // namespace params

// A queued buffer: address and length (high word first), the ADPCM predictor and scale and two history samples, the
// ADPCM flag with the loop flag in the high byte, the buffer's id and a spare word.
constexpr uint16_t kQueueEntrySize = 10;
namespace queue
{
constexpr uint16_t kAddress = 0;
constexpr uint16_t kLength = 2;
constexpr uint16_t kAdpcm = 4;
constexpr uint16_t kFlags = 7;
constexpr uint16_t kId = 8;
} // namespace queue

// The mixer's parameters (MixerParams), by word.
namespace mixer_params
{
constexpr uint16_t kDirty = 0x00;
constexpr uint16_t kMasterVolume = 0x02;
constexpr uint16_t kAuxReturn = 0x04; // two floats
constexpr uint16_t kOutputBuffers = 0x08;
constexpr uint16_t kStatusWord = 0x09;
constexpr uint16_t kOutputMode = 0x0b;
constexpr uint16_t kClippingMode = 0x0c;
constexpr uint16_t kHeadphones = 0x0d;
constexpr uint16_t kSurroundDepth = 0x0e;
constexpr uint16_t kSurroundParameter = 0x10;
constexpr uint16_t kRearRatio = 0x11;
constexpr uint16_t kAuxBypass = 0x12;
constexpr uint16_t kAuxEnable = 0x14;
constexpr uint16_t kSyncMode = 0x5e;
constexpr uint16_t kDirty2 = 0x60;
} // namespace mixer_params

// The firmware's internal state in bank 0.
namespace internal
{
// The output: the ring of final frames that the output's interrupt reads, and its variables.
constexpr uint16_t kOutputOn = 0x069a;
constexpr uint16_t kRingFrames = 0x069b; // 2 or 3
constexpr uint16_t kRingFramesChanged = 0x069c;
constexpr uint16_t kMute = 0x069d;
constexpr uint16_t kMuteNext = 0x069e;
constexpr uint16_t kApplyMute = 0x069f;
constexpr uint16_t kReadPosition = 0x084c;
constexpr uint16_t kNextReadPosition = 0x084d;
constexpr uint16_t kJump = 0x084e;
constexpr uint16_t kRing = 0x084f; // up to three frames of 0x140 words
constexpr uint16_t kRingFrameWords = 0x140;
constexpr uint16_t kRefills = 0x0c0f;
constexpr uint16_t kRefillPhase = 0x0c10;
constexpr uint16_t kPolling = 0x0c11;

// A source's state, its report and its other state.
constexpr uint16_t kSourceState = 0x11f0; // 24 entries of 0x4e words
constexpr uint16_t kSourceStateSize = 0x4e;
constexpr uint16_t kReport = 0x1940; // 24 entries of 6 words
constexpr uint16_t kReportSize = 6;
constexpr uint16_t kVoice = 0x4294; // 24 entries of 0x45 words
constexpr uint16_t kVoiceSize = 0x45;

// The mixer's state: a copy of the mixer's parameters in the firmware's form.
constexpr uint16_t kMasterVolume = 0x19d0;
constexpr uint16_t kAuxReturn = 0x19d2; // two, two words apart
constexpr uint16_t kDeclick = 0x19d7;
constexpr uint16_t kOutputMode = 0x19d9;
constexpr uint16_t kClippingMode = 0x19da;
constexpr uint16_t kHeadphones = 0x19db;
constexpr uint16_t kSurroundDepth = 0x19dc;
constexpr uint16_t kSurroundParameter = 0x19de;
constexpr uint16_t kRearRatio = 0x19df;
constexpr uint16_t kAuxBypass = 0x19e0; // two
constexpr uint16_t kAuxEnable = 0x19e2; // two
constexpr uint16_t kSyncMode = 0x1a2c;
constexpr uint16_t kWritePosition = 0x1a2e;
constexpr uint16_t kRingBase = 0x1a2f;
constexpr uint16_t kProfile = 0x1a32;      // the timings that go to the shared kProfile
constexpr uint16_t kDecodeStarts = 0x0672; // the resampler's first input of each channel

// The buses: 160 samples of each channel, two words each, high word first. The main bus has four channels (front left
// and right, rear left and right). The bypass bus and the two-channel bus have two each, and the aux buses four each.
constexpr uint16_t kMainBus = 0x288c;
constexpr uint16_t kBypassBus = 0x2d8c;
constexpr uint16_t kTwoChannelBus = 0x300c;
constexpr uint16_t kAuxBus = 0x328c; // two, 0x500 words apart
constexpr uint16_t kAuxBusSize = 0x500;
constexpr uint16_t kBusChannel = 2 * kFrameSamples;
constexpr uint16_t kChannelPointers = 0x3c8c; // the main bus's four channels, the output's two, the two-channel bus's
constexpr uint16_t kPolyphase = 0x3c94;       // three filters of 128 phases of 4 taps
constexpr uint16_t kPolyphaseWords = 0x600;

// The frame's status and the counters.
constexpr uint16_t kStatus = 0x490c; // 16 words
constexpr uint16_t kLate = 0x490d;
constexpr uint16_t kArmCounter = 0x491c;
constexpr uint16_t kCounter = 0x491d;
constexpr uint16_t kLastCounter = 0x491e;
constexpr uint16_t kState = 0x491f;
constexpr uint16_t kMixedOnce = 0x4921;
constexpr uint16_t kRefillPeriods = 0x4924;
constexpr uint16_t kLateFrames = 0x4925;

// A source's work during the mix: its resampled samples (two channels 0xa4 words apart, after four words of the
// biquad's history), the phase of each output sample and their sum, and the firmware's ADPCM scratch.
constexpr uint16_t kResampled = 0x4926;
constexpr uint16_t kResampledChannel = 0xa4;
constexpr uint16_t kPhases = 0x4a6e;
constexpr uint16_t kPhaseSum = 0x4bae;
constexpr uint16_t kAdpcmFrame = 0x4baf; // an ADPCM frame's words, by the positions of their nibbles

// The surround's filters (four of 0x30 taps, or of 0x60 for headphones), its tables and the speakers' positions.
constexpr uint16_t kSurroundFilters = 0x4c00;
constexpr uint16_t kSurroundTables = 0x4f00;
constexpr uint16_t kSurroundPositions = 0x5000;

// The output frame (left, then right, 160 samples of two words each), the last outputs of the sources that stopped
// playing, by bus, the ADPCM decoder's factors, the DMA's buffer, the declick's buffer, the two decode buffers and the
// ADPCM decoder's position and header.
constexpr uint16_t kOutput = 0xc000;
constexpr uint16_t kStoppedSums = 0xc280;   // three buses of four channels of two words
constexpr uint16_t kNibbleFactors = 0xc29a; // by nibble position
constexpr uint16_t kDmaBuffer = 0xc2b0;
constexpr uint16_t kDeclickBuffer = 0xc6b0;
constexpr uint16_t kDecode = 0xc930; // two channels of 0xe04 words, each after four samples of history
constexpr uint16_t kDecodeChannel = 0xe04;
constexpr uint16_t kAdpcmPosition = 0xe538;
constexpr uint16_t kAdpcmHeader = 0xe539;

// The surround's state: its work for headphones, the filters in use and their taps, the surround depth, the frame's
// samples, the surround's switch, and the limiter's constants.
constexpr uint16_t kHeadphoneWork = 0xe600;
constexpr uint16_t kHeadphonesOn = 0xef20;
constexpr uint16_t kHeadphoneFilters = 0xef21; // four
constexpr uint16_t kHeadphoneTaps = 0xef25;
constexpr uint16_t kSurroundDepthCopy = 0xef26;
constexpr uint16_t kSurroundSamples = 0xef27;
constexpr uint16_t kSurroundOn = 0xef28;
constexpr uint16_t kLimiter = 0xf033; // five
} // namespace internal

// A source's state (S), by word.
namespace state
{
constexpr uint16_t kReport = 0x00; // address of the source's report
constexpr uint16_t kVoice = 0x01;  // address of the source's other state
constexpr uint16_t kNibble = 0x02; // ADPCM nibble position, and in the high byte the loop's
constexpr uint16_t kAddress = 0x03;
constexpr uint16_t kEnd = 0x05; // length, taken again from kLength at each decode
constexpr uint16_t kLength = 0x07;
constexpr uint16_t kFlags = 0x09;
constexpr uint16_t kPhase = 0x0a; // 8.24
constexpr uint16_t kFormat = 0x0c;
constexpr uint16_t kQueueDirty = 0x0d;
constexpr uint16_t kQueue = 0x0e; // four buffers of kQueueEntrySize words
constexpr uint16_t kCoefs = 0x36;
constexpr uint16_t kAdpcm = 0x47;     // predictor and scale, two history samples
constexpr uint16_t kLoopAdpcm = 0x4a; // the same at the start of a loop

// kFlags: the index of the queued buffer to play next, the queued buffers that are waiting, looping, playing in the
// last frame, and no ADPCM state for the loop.
constexpr uint16_t kQueueIndexMask = 0x00ff;
constexpr unsigned kQueueValidShift = 8;
constexpr uint16_t kLooping = 0x2000;
constexpr uint16_t kPlayed = 0x4000;
constexpr uint16_t kNoLoopContext = 0x8000;
} // namespace state

// A source's report (X), by word.
namespace report
{
constexpr uint16_t kEnabled = 0; // enabled, and in the high byte a change of buffer
constexpr uint16_t kSync = 1;
constexpr uint16_t kPosition = 2;
constexpr uint16_t kCurrentBuffer = 4;
constexpr uint16_t kLastBuffer = 5;
} // namespace report

// A source's other state (Y), by word.
namespace voice
{
constexpr uint16_t kHistory = 0x00; // four samples of each channel
constexpr uint16_t kFilters = 0x08;
constexpr uint16_t kOnePole = 0x09;      // b0, a1
constexpr uint16_t kOnePoleState = 0x0b; // each channel's last output
constexpr uint16_t kBiquad = 0x0d;       // a2, a1, b2, b1, b0
constexpr uint16_t kBiquadInput = 0x12;  // each channel's last two inputs
constexpr uint16_t kBiquadState = 0x16;  // each channel's two 32-bit states
constexpr uint16_t kLastOutput = 0x1e;   // [bus][channel]
constexpr uint16_t kGain = 0x2a;         // [bus][channel]
constexpr uint16_t kPreviousGain = 0x36;
constexpr uint16_t kInterpolation = 0x42; // interpolation | kernel << 8
constexpr uint16_t kRate = 0x43;          // 8.24
} // namespace voice

// The address of word `address` of a shared structure in region `region`.
constexpr uint32_t Shared(uint32_t address, int region)
{
    return region ? address | kBank1 : address;
}

// A source's state in the enhanced mode beside the firmware's: its last decoded samples of each channel, and its
// filters' states.
struct EnhancedSource
{
    // The samples before a frame's first new sample that an output can take: the widest kernel's left side, from the
    // earliest place in the frame.
    static constexpr int kHistory = 136;

    std::array<std::array<int16_t, kHistory>, 2> history{}; // oldest first
    std::array<float, 2> one_pole{};                        // each channel's last output
    std::array<std::array<float, 4>, 2> biquad{};           // each channel's last two inputs, then its last two outputs
};

// The words of the enhanced mode's block of a source: its state, report and other state, one after another.
constexpr uint32_t kBlockWords = internal::kSourceStateSize + internal::kReportSize + internal::kVoiceSize;

// The enhanced mode's aux buses in floating point: each bus's front left, front right, rear left and rear right
// channels of a frame.
using EnhancedAuxBuses = std::array<std::array<std::array<float, kEnhancedFrameSamples>, 4>, 2>;

// The enhanced mode's sources in host memory: each source's parameters, report and ADPCM coefficients in each region,
// in the layouts of the shared structures, its state, report and other state (kBlockWords words), and its enhanced
// state. The words are little-endian. Data memory holds its words the same way. The last frame's output follows, and
// then the aux buses: the last frame's mix, the samples that each region holds for the ARM11, and the samples that came
// back from the ARM11 for the next frame. Each region holds bus A's four channels of kEnhancedFrameSamples 32-bit
// samples, and then bus B's.
struct HostSources
{
    std::array<std::vector<uint8_t>, 2> params;
    std::array<std::vector<uint8_t>, 2> reports;
    std::array<std::vector<uint8_t>, 2> coefs;
    std::vector<uint8_t> blocks;
    std::vector<EnhancedSource> sources;
    std::array<float, 2 * kEnhancedFrameSamples> frame{}; // left and right in turn, with full scale at 1
    EnhancedAuxBuses aux_mix{};
    std::array<std::array<int32_t, 2 * 4 * kEnhancedFrameSamples>, 2> aux{};
    EnhancedAuxBuses aux_returns{};
};

// The firmware's work on data memory. The firmware's audio thread and its interrupts do the work, and FirmwareModel
// runs each piece at its time.
class Audio
{
public:
    // What the output's interrupt did once the FIFO ran empty.
    struct Refill
    {
        bool pushed = false; // 16 words went to the FIFO
        bool wake = false;   // the audio thread wakes up for the next frame
    };

    // `data` is data memory: kDataWords words, little-endian, as Device::Ram() holds them after program memory. With
    // `enhanced`, the work runs in the enhanced mixing mode.
    Audio(uint8_t* data, Fcram& fcram, bool enhanced = false);

    // Fills data memory with the values that the firmware's image gives the words that the model uses.
    void Load();

    // The start of the audio thread (0x2bd1 up to the handshake in 0x29db): the output's mute and start, and the
    // clearing of the shared structures and of the sources' state.
    void StartThread();

    // The end of the start-up (0x29db), once the ARM11 has the structures' addresses.
    void FinishStartUp();

    // The semaphore's interrupt (0x333d): takes the ARM11's frame counter from the region after the last one.
    void TakeArmCounter();

    // The output's interrupt (0x3293), once the output's FIFO has run empty: fills `words` with the 16 words to send.
    Refill RefillOutput(std::array<int16_t, 16>& words);

    // The status's late flag at the start of a frame that the thread forces (0x2c3a).
    void UpdateLate();

    // Takes the parameters of region `region` (0x3076).
    void TakeParams(int region);

    // Copies the frame's results to region `region`, and takes the aux buses that the ARM11 returned in the other
    // region (0x30e9).
    void FinishFrame(int region);

    // Stores the frame counter in its region (0x30bf).
    void StoreCounter(uint16_t counter);

    // Mixes a frame into the output's ring (0x2e0d).
    void MixFrame();

    // The thread's step after a frame's mix (0x2cff): makes the output jump to the new frame when it has to. `force`
    // and `first` are the thread's flags. Clears `first`.
    void AfterMix(bool force, bool& first);

    // A host's meters (Device::SetMeters). The firmware has none. MixFrame adds up each metered source's mix into the
    // main bus, and the meters change nothing in data memory.
    void SetMeters(int count);
    void SetSourceMeter(int source, int meter);
    std::array<int32_t, 2> MeterPeaks(int meter) const;

    // The final samples of the frame before the ring's write position, left and right in turn (Device::MixedFrame).
    // In the enhanced mode, the last frame's output, rounded to 16 bits. The span holds them until the next call.
    std::span<const int16_t> MixedFrame() const;

    // The enhanced mode's sources (Device::SourceCount, Device::GrowSources and Device::SourceStructure).
    bool Enhanced() const
    {
        return enhanced_;
    }

    int SourceCount() const;
    void GrowSources(int count);
    uint8_t* SourceStructure(int structure, int region);

    // The enhanced mode's aux buses in region `region`, for the ARM11 (Device::AuxStructure), or nullptr in the
    // firmware's mixing.
    int32_t* AuxStructure(int region);

    // The enhanced mode's output of the last frame that MixFrame mixed, left and right in turn, with full scale at 1.
    std::span<const float> EnhancedFrame() const
    {
        return host_.frame;
    }

    // The enhanced mode's state outside data memory, for a snapshot.
    const HostSources& Host() const
    {
        return host_;
    }

    void RestoreHost(const HostSources& host);

    uint16_t Word(uint32_t address) const
    {
        return xsflib::LoadLe16(data_ + 2 * address);
    }

    void SetWord(uint32_t address, uint16_t value)
    {
        xsflib::StoreLe16(data_ + 2 * address, value);
    }

    int16_t Signed(uint32_t address) const
    {
        return static_cast<int16_t>(Word(address));
    }

private:
    // A meter's sums of a frame's mix, sample by sample: front left, front right, rear left and rear right.
    using MeterSums = std::array<std::array<int64_t, kFrameSamples>, 4>;
    using EnhancedMeterSums = std::array<std::array<float, kEnhancedFrameSamples>, 4>;

    // A 32-bit value in the firmware's order: high word first.
    uint32_t Long(uint32_t address) const;
    void SetLong(uint32_t address, uint32_t value);

    // A 32-bit value in the ARM11's order: low word first.
    uint32_t ArmLong(uint32_t address) const;

    void Clear(uint32_t address, uint32_t words);
    void Copy(uint32_t destination, uint32_t source, uint32_t words);

    // The address of a source's state.
    static uint32_t SourceState(int source);

    uint16_t RingFrames() const;
    uint16_t NextRingPosition(uint16_t position) const;
    void SetRingFrames(int16_t frames);
    void SetHeadphones(bool on);
    void TakeMixerParams(int region);
    void CopySurroundFilters(uint32_t source, bool long_filters);
    void TakeSourceParams(int source, int region);
    void TakeFirstBuffer(uint32_t source_state, uint32_t params);

    void ProcessSource(int source);
    bool DecodeFrame(uint32_t source_state, std::array<uint16_t, 3>& counts,
                     const std::function<void(uint16_t)>& chunk);
    void EndIfDone(uint32_t source_state);
    uint16_t FillPhases(uint32_t source_state);
    uint16_t Chunk(uint16_t limit, std::array<uint16_t, 3>& counts) const;
    uint16_t Decode(uint32_t source_state, std::array<uint16_t, 3>& counts);
    void DecodePcm(uint32_t source_state, uint16_t count, bool stereo, bool pcm8);
    void DecodeAdpcm(uint32_t source_state, uint16_t count, bool stereo);
    void DecodeAdpcmFrames(uint32_t source_state, uint32_t input, uint32_t output, uint16_t count);
    bool NextBuffer(uint32_t source_state, bool adpcm);
    void Dma(uint32_t source, uint32_t words);
    void Resample(uint32_t source_state, uint32_t output, uint32_t phases, uint16_t count);
    uint32_t Filter(uint32_t source_state);
    void Biquad(uint32_t voice, int channel, uint32_t buffer);
    void OnePole(uint32_t voice, int channel, uint32_t samples);
    void Mix(uint32_t source_state, uint32_t samples, int bus);
    void AddStoppedOutputs(uint32_t voice);
    void MixAux(int bus);
    void Declick(uint32_t sums);

    // A 32-bit value times a Q15 factor, rounded down (0x1bc73).
    static int32_t Scaled(int32_t value, uint16_t factor);

    void Scale(uint32_t address, uint32_t dwords, uint16_t factor);
    void Add(uint32_t destination, uint32_t source, uint32_t dwords);
    void MixRear();
    void MixSurround();
    void Clip(uint32_t ring);
    void Limit(uint32_t ring);

    // The enhanced mode (firmware_enhanced.cpp).
    void InitBlock(int source);
    void SwapIn(int source);
    void SwapOut(int source);
    void TakeHostParams(int region);
    void FinishHostFrame(int region);
    void MixEnhanced();
    void MixEnhancedSource(int source);
    void DecodeAhead(uint32_t source_state, uint32_t count, int channels);
    void RenderEnhanced(const wave::SincKernel& kernel, int64_t phase, uint32_t rate, int channels);
    void FilterEnhanced(uint32_t voice, EnhancedSource& source, int channels);
    void OutputEnhanced();
    uint8_t* Block(int source);

    uint8_t* data_;
    Fcram* fcram_;
    bool enhanced_;
    std::vector<int> source_meters_; // each source's meter, or -1
    std::vector<MeterSums> meter_sums_;
    std::vector<EnhancedMeterSums> enhanced_meter_sums_;
    std::vector<std::array<int32_t, 2>> meter_peaks_;
    int metering_ = -1;                                              // the meter of the source that MixFrame mixes
    mutable std::array<int16_t, 2 * kEnhancedFrameSamples> mixed_{}; // MixedFrame

    // The enhanced mode's sources, and its work: whether TakeSourceParams cleared the source's decoded samples or its
    // filters' states, each channel's input in a frame, a source's output, and the main bus.
    HostSources host_;
    bool history_cleared_ = false;
    bool filters_cleared_ = false;
    std::array<std::vector<float>, 2> input_;
    std::array<std::array<float, kEnhancedFrameSamples>, 2> rendered_{};
    std::array<std::array<float, kEnhancedFrameSamples>, 4> main_{};
};

} // namespace ntdlib::ctr::dsp::firmware
