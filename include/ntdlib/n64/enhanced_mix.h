// SPDX-License-Identifier: MIT

// The enhanced mixing mode of the Nintendo 64's audio. The mode keeps the console's voices, steps, gains and effects,
// and lifts the limits of its arithmetic. FramePlayer turns it on. Every model builds the same lists as on the console,
// and the exact microcode model runs them. The mode then runs each list again in floating point, from a record of what
// its commands did (AudioOps):
//
// - The mode runs a list some frames after the microcode model has run it. A voice's interpolator takes its later
//   inputs from the frames in between. An input that those frames don't reach yet counts as the last input that they
//   do. A voice that stops gives zeros after its end.
// - A voice's resampler takes its inputs as the microcode read them, in 16 bits. A wave's decoder, its loops and
//   whatever else comes before the resampler therefore stay as on the console. Each output comes from the band-limited
//   interpolator (wave::SincKernel) at the console's place: between the second and third of the 4 taps. A voice without
//   a resampler steps one sample at a time.
// - A voice's commands after its resampler, and every command outside a voice, work in single-precision floating point
//   on the scale of 16-bit samples. Nothing holds a sample to 16 bits, and a mix keeps all of what it adds to.
// - The envelope mixer's gains ramp across the list's length by the console's steps, with no wrap at either end of the
//   gain's range: sample i takes from + (i + 1) * ((to - from) / count). A voice that starts ramps from the list's
//   first volume. The console does the same.
// - A host's mute ramps across the frame in equal steps.
//
// With an output rate above the game's, the mode works at a whole multiple k of the game's rate: the smallest that
// reaches the output rate. Each list runs in two parts, and their outputs add up:
//
// - A voice's commands, and all the other commands on what the voices send to the dry buses, run with every place and
//   count multiplied by k. A delay in a voice's commands keeps its time. A filter there keeps its taps at the game's
//   spacing of k samples. Its response then repeats above the game's Nyquist frequency.
// - What the voices send to the wet buses goes down to the game's rate through the interpolator. The other commands run
//   on it at the game's rate. The effects then stay as on the console. Their output comes back up through the
//   interpolator. The dry part waits for the delay of those two filters.
//
// A last interpolator takes the sum to the output rate. The first output sample is at the time of the console's first
// sample. Every platform gives the same samples.
//
// The object's state is plain values. A copy of the object is a snapshot.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "ntdlib/n64/audio_memory.h"
#include "ntdlib/n64/audio_rsp.h"

namespace ntdlib::n64
{

// A place in DMEM that an engine's voices add their output to.
struct VoiceArea
{
    uint32_t place = 0;
    uint32_t bytes = 0;
    int side = 0;     // 0 for left, 1 for right
    bool wet = false; // whether a game's effects take it as their input
};

class EnhancedMix
{
public:
    static constexpr int kTracks = 32;
    static constexpr int kLookAheadFrames = 2;

    using Peaks = std::array<std::array<int32_t, 2>, kTracks>;

    // `output` is the place in main memory where each frame's list saves its output. An `output_rate` of 0 is the
    // game's.
    EnhancedMix(int game_rate, std::vector<VoiceArea> areas, uint32_t output, int output_rate);

    int OutputRate() const;

    // The frames that the microcode model has run and the mode hasn't.
    std::size_t Queued() const;

    // The record of the frame that the microcode model runs next. EndRecord takes it in.
    AudioOps& Recording();
    void EndRecord(int samples, bool busy);

    // Whether the model was still busy when it built the oldest frame.
    bool OldestBusy() const;

    // Mixes the oldest frame, and appends what it completes of the output to `out`: the left and the right side in
    // turn, on the scale of 16-bit samples. A frame's meters go in `peaks`.
    void MixFrame(uint32_t muted, bool metered, const AudioBus& bus, Peaks& peaks, std::vector<float>& out);

    // Leaves out the oldest frame, for frames without mixing. Returns its samples at the game's rate.
    int DropFrame();

    // Forgets the voices' inputs. The next inputs start from silence.
    void ForgetVoices();

private:
    // The inputs of a voice's resampler, from its start.
    struct Stream
    {
        std::vector<float> samples;
        int64_t base = 0; // the index of samples[0]
        bool closed = false;
        bool holds = false; // after a close, the last input stays rather than zeros
        uint64_t last_frame = 0;
        uint32_t uses = 0;

        int64_t End() const
        {
            return base + static_cast<int64_t>(samples.size());
        }
    };

    struct MainResample
    {
        uint32_t stream = 0;
        int64_t origin = 0; // the index of the input at the place of a first fraction of 0
        uint32_t out = 0;
        uint32_t samples = 0;
        uint16_t first_fraction = 0;
        uint16_t pitch = 0;
    };

    struct Frame
    {
        AudioOps ops;
        std::vector<AudioOp> run;
        std::vector<MainResample> mains;
        int samples = 0;
        bool busy = false;
        uint64_t serial = 0;
    };

    struct ResamplerState
    {
        std::array<float, 4> history = {};
        uint16_t fraction = 0;
    };

    // The buffers at one rate: DMEM and what the lists keep in main memory, with `scale` samples for each of the
    // console's.
    struct World
    {
        int scale = 1;
        std::vector<float> dmem;
        std::map<uint64_t, std::vector<float>> pages;
        std::map<uint32_t, ResamplerState> resamplers;

        std::size_t Place(uint32_t byte) const;
        float& Dmem(std::size_t index);
        float ReadMemory(uint64_t index) const;
        void WriteMemory(uint64_t index, float value);
    };

    // An interpolator that takes a wet bus down to the game's rate.
    struct Down
    {
        std::vector<float> history;
        int idle = 0;
    };

    void Resolve(Frame& frame);
    void ResolveVoice(Frame& frame, std::size_t begin, std::size_t end);
    void AddMain(Frame& frame, const AudioResampleOp& op);
    void AddUnresampled(Frame& frame, const AudioEnvOp& env);
    uint32_t StreamOf(const AudioResampleOp& op);
    void AddInputs(uint32_t id, const float* inputs, uint32_t count, uint64_t serial);

    void RunOp(World& world, const Frame& frame, const AudioOp& op, const AudioBus& bus) const;
    static void RunMove(World& world, const AudioOp& op, std::size_t count);
    static void RunLoad(World& world, const AudioOp& op, const AudioBus& bus, std::size_t count);
    static void RunDuplicate(World& world, const AudioOp& op, std::size_t count);
    static void RunInterleave(World& world, const AudioOp& op, std::size_t count);
    static void RunNearest(World& world, const AudioOp& op, std::size_t count);
    void RunMain(const MainResample& main);
    static void RunResample(World& world, const AudioResampleOp& op);
    static void RunEnv(World& world, const AudioEnvOp& env);
    static void RunPole(World& world, const AudioOp& op, const int16_t* table);
    static void RunFilter(World& world, const AudioOp& op, const int16_t* table);

    void StartVoice(int track, uint32_t muted, bool metered);
    void EndVoice();
    void FlushSends(uint32_t samples);
    void EndChunk(uint32_t samples, Peaks& peaks);
    void Emit(int samples, std::vector<float>& out);
    void DelayDryPart(std::vector<float>& mixed);
    void AddWetPart(std::vector<float>& mixed, std::size_t game_samples);
    void RunFinalInterpolator(const std::vector<float>& mixed, std::size_t count, std::vector<float>& out);

    int factor_;
    int rate_; // the rate that the lists run at
    int output_rate_;
    std::vector<VoiceArea> areas_;
    uint32_t output_;

    AudioOps recording_;
    std::deque<Frame> queue_;
    uint64_t serial_ = 0;
    uint32_t next_stream_ = 0;
    std::map<uint32_t, uint32_t> streams_by_state_;
    std::map<uint32_t, Stream> streams_;

    World low_;
    World high_;
    std::vector<float> scratch_;

    // A voice under a host's mute or meter.
    int voice_track_ = -1;
    bool voice_followed_ = false;
    bool voice_metered_ = false;
    bool voice_dirty_ = false;
    std::vector<float> before_;
    std::vector<float> sums_;
    uint32_t summed_tracks_ = 0;
    std::array<float, kTracks> track_gains_ = {};
    bool mixed_a_frame_ = false;
    uint32_t frame_muted_ = 0;
    int frame_done_ = 0;
    int frame_total_ = 0;

    // The way from the wet buses to the output.
    std::map<uint64_t, Down> downs_;
    int flush_slot_ = 0;
    std::array<std::vector<float>, 2> ups_;
    std::vector<float> delay_;
    std::size_t lag_ = 0; // the wet path's delay, at the rate that the lists run at

    // The last interpolator: the samples of each side from final_base_ on, and the next output's number.
    std::array<std::vector<float>, 2> final_;
    int64_t final_base_ = 0;
    uint64_t final_next_ = 0;
    std::size_t skip_ = 0;
};

} // namespace ntdlib::n64
