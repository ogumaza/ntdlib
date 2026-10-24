// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/ax/render.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "ntdlib/cafe/ax/cemu_departures.h"
#include "ntdlib/cafe/ax/memory.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/resample.h"
#include "ntdlib/dsp/accelerator.h"
#include "ntdlib/dsp/accumulator.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::cafe::ax
{
namespace
{

// The microcode's results wrap to the 40 bits of the DSP's accumulators (Wrap40), and round as the DSP rounds them
// (Round). In 40-bit mode, the microcode reads an accumulator's middle word saturated to 16 bits (MiddleWord).
using ntdlib::dsp::MiddleWord;
using ntdlib::dsp::Round;
using ntdlib::dsp::Wrap40;

// An accumulator stored as a 32-bit sample: the middle word as MiddleWord reads it, then the low word.
int32_t Store32(int64_t accumulator)
{
    const auto high = static_cast<uint16_t>(MiddleWord(accumulator));
    return static_cast<int32_t>(uint32_t{high} << 16 | static_cast<uint16_t>(accumulator));
}

// How the microcode uses the DSP's accelerator for a voice (ntdlib's model of the accelerator). The accelerator reads
// the voice's samples in order and decodes them: ADPCM, or PCM scaled by the gain. At the end address, the accelerator
// goes back to the loop address. The microcode loads the accelerator's registers from the voice's block at the start of
// the frame (0x0739). At the end, the microcode stores the position and the decoder's state back in the block. With
// `loop_keeps_history`, a loop keeps the decoder's history as Cemu's does (cemu_departures.h).
class Accelerator
{
public:
    Accelerator(Pb& pb, const Memory& memory, bool loop_keeps_history)
        : pb_(pb), memory_(memory), loop_keeps_history_(loop_keeps_history)
    {
        accelerator_.format = static_cast<uint16_t>(pb.format);
        accelerator_.start = pb.loop_address & ntdlib::dsp::kAcceleratorAddressMask;
        accelerator_.end = pb.end_address & ntdlib::dsp::kAcceleratorAddressMask;
        accelerator_.current = pb.current_address & ntdlib::dsp::kAcceleratorCurrentMask;
        accelerator_.gain = pb.adpcm.gain;
        accelerator_.pred_scale = pb.adpcm.predictor_scale & ntdlib::dsp::kAcceleratorPredScaleMask;
        accelerator_.yn1 = static_cast<uint16_t>(pb.adpcm.yn1);
        accelerator_.yn2 = static_cast<uint16_t>(pb.adpcm.yn2);
    }

    // The next sample. Reaching the end address raises an exception, and its handler runs (AtEnd). The formats that
    // read the accelerator's input register read 0.
    int16_t Next()
    {
        return static_cast<int16_t>(ntdlib::dsp::AcceleratorNext(
            accelerator_, pb_.adpcm.coefs, [this](uint32_t address) { return Byte(address); }, [this] { AtEnd(); }));
    }

    // Stores the position and the decoder's state in the block. The block keeps 7 bits of an ADPCM frame's header.
    void Store()
    {
        pb_.current_address = accelerator_.current;
        pb_.adpcm.predictor_scale = accelerator_.pred_scale & 0x7F;
        pb_.adpcm.yn1 = static_cast<int16_t>(accelerator_.yn1);
        pb_.adpcm.yn2 = static_cast<int16_t>(accelerator_.yn2);
    }

private:
    // A byte of sample data. The extension is the address's top three bits.
    uint8_t Byte(uint32_t address) const
    {
        return memory_.Read8(static_cast<uint32_t>(pb_.extension & 7) << 29 | (address & 0x1FFFFFFF));
    }

    // The exception's handler (0x087E). It stops a voice that doesn't loop. The rest of the frame's reads then give 0.
    // For a voice that loops, the handler loads the decoder's state at the loop start. A stream's history continues
    // instead. The stream's buffer has been refilled.
    void AtEnd()
    {
        if (pb_.loop_flag != 1)
        {
            pb_.state = VoiceState::kStop;
            accelerator_.reads_stopped = true;
        }
        else
        {
            accelerator_.pred_scale = pb_.adpcm_loop.predictor_scale & ntdlib::dsp::kAcceleratorPredScaleMask;
            if (pb_.type != VoiceType::kStream && !loop_keeps_history_)
            {
                accelerator_.yn1 = static_cast<uint16_t>(pb_.adpcm_loop.yn1);
                accelerator_.yn2 = static_cast<uint16_t>(pb_.adpcm_loop.yn2);
            }
        }
    }

    Pb& pb_;
    const Memory& memory_;
    bool loop_keeps_history_;
    ntdlib::dsp::Accelerator accelerator_;
};

// The frame's samples at the renderer's rate. The ratio advances a 16.16 position. Each output sample comes from the
// last four samples (see resample.h). The block keeps them oldest first. `cemu` can make the interpolation Cemu's
// (cemu_departures.h). Without `out`, the voice reads the same samples and keeps the same last four, without outputs.
void Fetch(Pb& pb, Accelerator& accelerator, const CemuDepartures& cemu, std::size_t samples, int16_t* out)
{
    if (pb.src_mode == SrcMode::kNone)
    {
        std::array<int16_t, 4> last = pb.src.last;
        for (std::size_t i = 0; i < samples; ++i)
        {
            last[i & 3] = accelerator.Next();
            if (out != nullptr)
            {
                out[i] = last[i & 3];
            }
        }
        for (std::size_t i = 0; i < 4; ++i)
        {
            pb.src.last[i] = last[(samples + i) & 3];
        }

        return;
    }

    const SrcMode mode = cemu.linear_four_tap && pb.src_mode == SrcMode::kFourTap ? SrcMode::kLinear : pb.src_mode;
    const bool newest = cemu.newest_samples && pb.format != SampleFormat::kPcm16;
    std::array<int16_t, 4> last = pb.src.last;
    std::size_t oldest = 0;
    uint32_t position = pb.src.frac;
    for (std::size_t i = 0; i < samples; ++i)
    {
        position += pb.src.ratio;
        for (uint32_t n = position >> 16; n > 0; --n)
        {
            last[oldest] = accelerator.Next();
            oldest = (oldest + 1) & 3;
        }
        position &= 0xFFFF;

        if (out != nullptr)
        {
            const std::array<int16_t, 4> ordered = {last[oldest], last[(oldest + 1) & 3], last[(oldest + 2) & 3],
                                                    last[(oldest + 3) & 3]};
            out[i] = Resample(mode, pb.four_tap_filter, ordered, position, newest);
        }
    }

    pb.src.frac = static_cast<uint16_t>(position);
    for (std::size_t i = 0; i < 4; ++i)
    {
        pb.src.last[i] = last[(oldest + i) & 3];
    }
}

// The volume envelope (0x0269): a 1.15 gain that starts at the block's volume with the first sample and moves by the
// delta each sample. The microcode steps each pair of samples by twice the delta. That step is a 16-bit signed value.
// The product saturates to 16 bits.
void ApplyEnvelope(VolumeRamp& ve, std::span<int16_t> samples)
{
    const auto delta = static_cast<uint16_t>(ve.delta);
    const auto step = static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const uint32_t start = i % 2 == 0 ? ve.volume : ve.volume + uint32_t{delta};
        const auto gain = static_cast<uint16_t>(start + static_cast<uint32_t>(static_cast<int32_t>(i / 2) * step));
        samples[i] = MiddleWord(2 * int64_t{samples[i]} * gain);
    }

    ve.volume =
        static_cast<uint16_t>(ve.volume + static_cast<uint32_t>(static_cast<int32_t>(samples.size() / 2) * step));
}

// The one-pole low-pass filter (0x05AF), y = a0 x + b0 y1, with a0 taken as unsigned. Each output saturates to 16 bits
// and feeds back that way.
void ApplyLpf(Lpf& lpf, std::span<int16_t> samples)
{
    if (lpf.on == 0)
    {
        return;
    }

    const auto a0 = static_cast<uint16_t>(lpf.a0);
    int16_t y = lpf.yn1;
    for (int16_t& x : samples)
    {
        y = MiddleWord(2 * int64_t{x} * a0 + 2 * int64_t{y} * lpf.b0);
        x = y;
    }

    lpf.yn1 = y;
}

// The biquad filter (0x05CC), with coefficients in 2.14. The sum is shifted left two places, rounded to a whole sample
// (ties to even) and saturated to 16 bits. Each output feeds back that way.
void ApplyBiquad(Biquad& biquad, std::span<int16_t> samples)
{
    if (biquad.on == 0)
    {
        return;
    }

    for (int16_t& x : samples)
    {
        const int64_t sum = int64_t{biquad.b0} * x + int64_t{biquad.b1} * biquad.xn1 + int64_t{biquad.b2} * biquad.xn2 +
                            int64_t{biquad.a1} * biquad.yn1 + int64_t{biquad.a2} * biquad.yn2;
        const int64_t shifted = Wrap40(sum * 4);
        const int16_t y = MiddleWord(Wrap40(Round(shifted)));

        biquad.xn2 = biquad.xn1;
        biquad.xn1 = x;
        biquad.yn2 = biquad.yn1;
        biquad.yn1 = y;
        x = y;
    }
}

// A host's gain for each sample of a frame (HostMix): from `from` to `to` in 1.15, by equal steps. The last sample gets
// `to`.
using HostGains = std::array<uint16_t, kMaxSamplesPerFrame>;

HostGains HostRamp(uint16_t from, uint16_t to, std::size_t samples)
{
    HostGains gains{};
    const int32_t change = int32_t{to} - int32_t{from};
    for (std::size_t i = 0; i < samples; ++i)
    {
        const auto step = static_cast<int32_t>(i + 1);
        gains[i] = static_cast<uint16_t>(from + change * step / static_cast<int32_t>(samples));
    }

    return gains;
}

// Adds the samples into one bus channel (0x0796 and 0x07B1). Each product is rounded down to a whole sample. The volume
// is constant, or it starts at the block's volume with the first sample and moves by the delta each sample. A host's
// gains, if any, scale each product in 1.15, rounded down again. A meter's channel, if any, takes the same products.
void Mix(std::span<const int16_t> samples, VolumeRamp& mix, bool ramp, Buses::Channel& bus,
         const HostGains* host = nullptr, Buses::Channel* meter = nullptr)
{
    const auto delta = static_cast<uint16_t>(mix.delta);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto gain = ramp ? static_cast<uint16_t>(mix.volume + i * delta) : mix.volume;
        auto product = static_cast<int32_t>((2 * int64_t{samples[i]} * gain) >> 16);
        if (host != nullptr)
        {
            product = static_cast<int32_t>((int64_t{product} * (*host)[i]) >> 15);
        }
        bus[i] += product;
        if (meter != nullptr)
        {
            (*meter)[i] += product;
        }
    }

    if (ramp)
    {
        mix.volume = static_cast<uint16_t>(mix.volume + samples.size() * delta);
    }
}

// A volume that goes from `from` to `to` across a frame: one gain for each sample (0x04C5). Each step adds 683 65536ths
// of the change, or 682 for every third step. The first sample already has one step. The last sample gets `to`. The
// 48 kHz microcode steps by 455 65536ths each time.
std::array<uint16_t, kMaxSamplesPerFrame> Ramp(uint16_t from, uint16_t to, std::size_t samples)
{
    std::array<uint16_t, kMaxSamplesPerFrame> gains{};
    const auto change = static_cast<int16_t>(static_cast<uint16_t>(to - from));
    int64_t sum = int64_t{from} * 0x10000;
    for (std::size_t i = 0; i + 1 < samples; ++i)
    {
        const int step = samples == kSamplesPerFrame48k ? 455 : i % 3 == 2 ? 682 : 683;
        sum += int64_t{change} * step;
        gains[i] = static_cast<uint16_t>(sum >> 16);
    }
    gains[samples - 1] = to;

    return gains;
}

// The enhanced mode's frame: 144 samples at 48 kHz.
constexpr int kEnhancedSamples = kSamplesPerFrame48k;

// The most samples that an enhanced frame reads: a ratio of 8 for each output.
constexpr int kEnhancedMostReads = 8 * kEnhancedSamples;

// Where each output of an enhanced frame lies among the voice's samples: after `reads[i]` of the frame's samples,
// `behind` samples behind the newest, and `fractions[i]` 65536ths of the way to the next. `step` is the samples that an
// output moves by, for the interpolator's kernel.
struct Places
{
    std::array<int32_t, kEnhancedSamples> reads{};
    std::array<uint32_t, kEnhancedSamples> fractions{};
    int behind = 0;
    double step = 0.0;
};

// The places of a frame. The 4-tap filters and linear interpolation step as the console steps: the ratio advances the
// block's position, and each whole sample is a read. The position moves on in the block. Without a conversion, an
// output moves two thirds of a sample from the newest, and a frame reads 96 samples.
Places PlacesOf(Pb& pb)
{
    Places places;
    if (pb.src_mode == SrcMode::kNone)
    {
        constexpr std::array<uint32_t, 3> kThirds = {0, 21845, 43691};
        for (int i = 0; i < kEnhancedSamples; ++i)
        {
            places.reads[static_cast<std::size_t>(i)] = 1 + 2 * i / 3;
            places.fractions[static_cast<std::size_t>(i)] = kThirds[static_cast<std::size_t>(2 * i % 3)];
        }
        places.step = 2.0 / 3.0;

        return places;
    }

    uint32_t position = pb.src.frac;
    int32_t reads = 0;
    for (std::size_t i = 0; i < places.reads.size(); ++i)
    {
        position += pb.src.ratio;
        reads += static_cast<int32_t>(position >> 16);
        position &= 0xFFFF;
        places.reads[i] = reads;
        places.fractions[i] = position;
    }
    pb.src.frac = static_cast<uint16_t>(position);
    places.behind = pb.src_mode == SrcMode::kLinear ? 3 : 2;
    places.step = static_cast<double>(pb.src.ratio) / 65536.0;

    return places;
}

// A gain that goes from `from` to `to` across an enhanced frame: sample i takes from + i * ((to - from) / 144).
float RampAt(float from, float step, int i)
{
    return from + static_cast<float>(i) * step;
}

float StepOf(float from, float to)
{
    return (to - from) / static_cast<float>(kEnhancedSamples);
}

// Adds an enhanced frame's samples into one bus channel at a gain that ramps from `from` to `to`, and at the host's
// gains, if any. A meter's channel, if any, takes the same products.
void MixEnhanced(std::span<const float> samples, float from, float to, FloatBuses::Channel& bus,
                 const std::array<float, kEnhancedSamples>* host, FloatBuses::Channel* meter)
{
    if (from == 0.0f && to == 0.0f)
    {
        return;
    }

    const float step = StepOf(from, to);
    for (int i = 0; i < kEnhancedSamples; ++i)
    {
        const auto at = static_cast<std::size_t>(i);
        float product = samples[at] * RampAt(from, step, i);
        if (host != nullptr)
        {
            product = product * (*host)[at];
        }
        bus[at] += product;
        if (meter != nullptr)
        {
            (*meter)[at] += product;
        }
    }
}

// A mix's gain at the end of an enhanced frame. A volume that moves steps as the console steps it (Mix). A channel
// that the mask leaves out has none.
float MixGainAfter(VolumeRamp& mix, int bits)
{
    if (bits == 3)
    {
        mix.volume =
            static_cast<uint16_t>(mix.volume + std::size_t{kEnhancedSamples} * static_cast<uint16_t>(mix.delta));
    }

    return bits != 0 ? static_cast<float>(mix.volume) / 32768.0f : 0.0f;
}

} // namespace

void RenderVoice(Pb& pb, const Memory& memory, int samples, Buses& buses, const CemuDepartures& cemu,
                 const HostMix& host)
{
    if (pb.state != VoiceState::kRun)
    {
        return;
    }

    std::array<int16_t, kMaxSamplesPerFrame> buffer;
    const std::span<int16_t> x(buffer.data(), static_cast<std::size_t>(samples));
    Accelerator accelerator(pb, memory, cemu.loop_keeps_history);
    Fetch(pb, accelerator, cemu, x.size(), x.data());
    accelerator.Store();
    ApplyEnvelope(pb.ve, x);
    ApplyLpf(pb.lpf, x);
    ApplyBiquad(pb.biquad, x);

    // The host's mute scales every product of the frame. Without a mute, the products stay as they are.
    HostGains host_gains{};
    const bool scaled = host.was_muted || host.muted;
    if (scaled)
    {
        host_gains = HostRamp(host.was_muted ? 0 : kUnityVolume, host.muted ? 0 : kUnityVolume, x.size());
    }
    const HostGains* gains = scaled ? &host_gains : nullptr;

    // Into each bus and channel the voice's masks name: two bits each, both set for a volume that moves. The TV's main
    // bus also feeds the host's meter, front left and right.
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        for (int channel = 0; channel < kTvChannels; ++channel)
        {
            const int bits = (pb.tv_mask[bus] >> (2 * channel)) & 3;
            if (bits != 0)
            {
                Buses::Channel* meter =
                    bus == 0 && channel < 2 && host.meter != nullptr ? &(*host.meter)[channel] : nullptr;
                Mix(x, pb.tv_mix[channel][bus], bits == 3, buses.tv[bus][channel], gains, meter);
            }
        }
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int bus = 0; bus < kBusCount; ++bus)
        {
            for (int channel = 0; channel < kDrcChannels; ++channel)
            {
                const int bits = (pb.drc_mask[drc][bus] >> (2 * channel)) & 3;
                if (bits != 0)
                {
                    Mix(x, pb.drc_mix[drc][channel][bus], bits == 3, buses.drc[drc][bus][channel], gains);
                }
            }
        }
    }
}

void AdvanceVoice(Pb& pb, const Memory& memory, int samples, const CemuDepartures& cemu)
{
    if (pb.state != VoiceState::kRun)
    {
        return;
    }

    // A filter in fixed point never quite forgets what it took in: two runs whose states differ can keep a difference
    // of a few steps for good. A voice with a filter on therefore runs its samples through its envelope and its
    // filters. RenderVoice does the same, and mixes them too.
    Accelerator accelerator(pb, memory, cemu.loop_keeps_history);
    if (pb.lpf.on != 0 || pb.biquad.on != 0)
    {
        std::array<int16_t, kMaxSamplesPerFrame> buffer;
        const std::span<int16_t> x(buffer.data(), static_cast<std::size_t>(samples));
        Fetch(pb, accelerator, cemu, x.size(), x.data());
        accelerator.Store();
        ApplyEnvelope(pb.ve, x);
        ApplyLpf(pb.lpf, x);
        ApplyBiquad(pb.biquad, x);
    }
    else
    {
        Fetch(pb, accelerator, cemu, static_cast<std::size_t>(samples), nullptr);
        accelerator.Store();

        // The envelope's volume after the frame: the volume that ApplyEnvelope leaves.
        const auto delta = static_cast<uint16_t>(pb.ve.delta);
        const auto step = static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
        pb.ve.volume =
            static_cast<uint16_t>(pb.ve.volume + static_cast<uint32_t>(static_cast<int32_t>(samples / 2) * step));
    }

    // The mixes' volumes after the frame: the volumes that Mix leaves.
    const auto after = [samples](VolumeRamp& mix, int bits)
    {
        if (bits == 3)
        {
            mix.volume = static_cast<uint16_t>(mix.volume +
                                               static_cast<std::size_t>(samples) * static_cast<uint16_t>(mix.delta));
        }
    };
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        for (int channel = 0; channel < kTvChannels; ++channel)
        {
            after(pb.tv_mix[channel][bus], (pb.tv_mask[bus] >> (2 * channel)) & 3);
        }
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int bus = 0; bus < kBusCount; ++bus)
        {
            for (int channel = 0; channel < kDrcChannels; ++channel)
            {
                after(pb.drc_mix[drc][channel][bus], (pb.drc_mask[drc][bus] >> (2 * channel)) & 3);
            }
        }
    }
}

int32_t ReturnAux(std::span<const int32_t> returned, uint16_t from, uint16_t to, std::span<int32_t> main)
{
    const auto gains = Ramp(from, to, main.size());
    int64_t peak = 0;
    for (std::size_t i = 0; i < main.size(); ++i)
    {
        const int64_t added = (int64_t{returned[i]} * gains[i]) >> 15;
        main[i] = Store32(int64_t{main[i]} + added);
        peak = std::max(peak, added < 0 ? -added : added);
    }

    return static_cast<int32_t>(std::min<int64_t>(peak, std::numeric_limits<int32_t>::max()));
}

void OutputBus(std::span<const int32_t> bus, uint16_t from, uint16_t to, std::span<int32_t> out)
{
    const auto gains = Ramp(from, to, out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = Store32(Wrap40(((int64_t{bus[i]} * gains[i]) >> 15) * 256));
    }
}

void RenderVoiceEnhanced(Pb& pb, EnhancedVoice& voice, const Memory& memory, FloatBuses& buses, const HostMix& host)
{
    if (pb.state != VoiceState::kRun)
    {
        return;
    }

    // The interpolator's inputs: the samples read before, the frame's samples, then the samples after them from a copy
    // of the accelerator. Each output's kernel takes `half` inputs on each side of its place.
    constexpr int kHistory = EnhancedVoice::kHistory;
    const Places places = PlacesOf(pb);
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(places.step);
    const int half = kernel.HalfWidth();
    const int32_t reads = places.reads.back();
    const int ahead = half - places.behind;
    std::array<float, kHistory + kEnhancedMostReads + wave::SincKernel::kMaxHalfWidth> input{};
    std::copy(voice.history.begin(), voice.history.end(), input.begin());
    Accelerator accelerator(pb, memory, false);
    for (int32_t k = 0; k < reads; ++k)
    {
        input[static_cast<std::size_t>(kHistory + k)] = accelerator.Next();
    }
    accelerator.Store();
    if (pb.state == VoiceState::kRun)
    {
        Pb copy = pb;
        Accelerator later(copy, memory, false);
        for (int k = 0; k < ahead; ++k)
        {
            input[static_cast<std::size_t>(kHistory + reads + k)] = later.Next();
        }
    }

    // The block keeps the last four samples read, and the voice the samples for the next frame.
    for (std::size_t i = 0; i < 4; ++i)
    {
        pb.src.last[i] = static_cast<int16_t>(input[static_cast<std::size_t>(kHistory + reads - 4) + i]);
    }
    for (std::size_t i = 0; i < voice.history.size(); ++i)
    {
        voice.history[i] = static_cast<int16_t>(input[static_cast<std::size_t>(reads) + i]);
    }

    // Each output at its place. The kernel's first input lies half - 1 samples before the output's left neighbour.
    std::array<float, kEnhancedSamples> x{};
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const int32_t first = kHistory + places.reads[i] - places.behind - half;
        x[i] = kernel.At(&input[static_cast<std::size_t>(first)], places.fractions[i]);
    }

    // The envelope, from the gain that the last frame reached to the block's volume after the frame.
    const auto delta = static_cast<uint16_t>(pb.ve.delta);
    const auto ve_step = static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
    pb.ve.volume = static_cast<uint16_t>(pb.ve.volume +
                                         static_cast<uint32_t>(static_cast<int32_t>(kEnhancedSamples / 2) * ve_step));
    const float ve_to = static_cast<float>(pb.ve.volume) / 32768.0f;
    const float ve_from = voice.starts ? ve_to : voice.ve;
    const float ve_by = StepOf(ve_from, ve_to);
    for (int i = 0; i < kEnhancedSamples; ++i)
    {
        x[static_cast<std::size_t>(i)] = x[static_cast<std::size_t>(i)] * RampAt(ve_from, ve_by, i);
    }
    voice.ve = ve_to;

    // The low-pass, then the biquad.
    if (pb.lpf.on != 0)
    {
        const float a0 = static_cast<float>(static_cast<uint16_t>(pb.lpf.a0)) / 32768.0f;
        const float b0 = static_cast<float>(pb.lpf.b0) / 32768.0f;
        float y = voice.lpf;
        for (float& sample : x)
        {
            y = a0 * sample + b0 * y;
            sample = y;
        }
        voice.lpf = y;
    }
    if (pb.biquad.on != 0)
    {
        const float b0 = static_cast<float>(pb.biquad.b0) / 16384.0f;
        const float b1 = static_cast<float>(pb.biquad.b1) / 16384.0f;
        const float b2 = static_cast<float>(pb.biquad.b2) / 16384.0f;
        const float a1 = static_cast<float>(pb.biquad.a1) / 16384.0f;
        const float a2 = static_cast<float>(pb.biquad.a2) / 16384.0f;
        auto& [x1, x2, y1, y2] = voice.biquad;
        for (float& sample : x)
        {
            const float y = b0 * sample + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2;
            x2 = x1;
            x1 = sample;
            y2 = y1;
            y1 = y;
            sample = y;
        }
    }

    // The host's mute: from its gain at the start of the frame to its gain at the end, by equal steps.
    std::array<float, kEnhancedSamples> host_gains{};
    const bool scaled = host.was_muted || host.muted;
    if (scaled)
    {
        const float from = host.was_muted ? 0.0f : 1.0f;
        const float to = host.muted ? 0.0f : 1.0f;
        for (int i = 0; i < kEnhancedSamples; ++i)
        {
            host_gains[static_cast<std::size_t>(i)] =
                from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(kEnhancedSamples);
        }
    }
    const std::array<float, kEnhancedSamples>* gains = scaled ? &host_gains : nullptr;

    // Into each bus and channel, from the gain that the last frame reached. The TV's main bus also feeds the host's
    // meter, front left and right.
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        for (int channel = 0; channel < kTvChannels; ++channel)
        {
            const int bits = (pb.tv_mask[bus] >> (2 * channel)) & 3;
            float& last = voice.tv[static_cast<std::size_t>(channel)][static_cast<std::size_t>(bus)];
            const float to = MixGainAfter(pb.tv_mix[channel][bus], bits);
            const float from = voice.starts ? to : last;
            last = to;
            FloatBuses::Channel* meter = bus == 0 && channel < 2 && host.float_meter != nullptr
                                             ? &(*host.float_meter)[static_cast<std::size_t>(channel)]
                                             : nullptr;
            MixEnhanced(x, from, to, buses.tv[bus][channel], gains, meter);
        }
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int bus = 0; bus < kBusCount; ++bus)
        {
            for (int channel = 0; channel < kDrcChannels; ++channel)
            {
                const int bits = (pb.drc_mask[drc][bus] >> (2 * channel)) & 3;
                float& last = voice.drc[static_cast<std::size_t>(drc)][static_cast<std::size_t>(channel)]
                                       [static_cast<std::size_t>(bus)];
                const float to = MixGainAfter(pb.drc_mix[drc][channel][bus], bits);
                const float from = voice.starts ? to : last;
                last = to;
                MixEnhanced(x, from, to, buses.drc[drc][bus][channel], gains, nullptr);
            }
        }
    }

    voice.starts = false;
}

void AdvanceVoiceEnhanced(Pb& pb, EnhancedVoice& voice, const Memory& memory)
{
    if (pb.state != VoiceState::kRun)
    {
        return;
    }

    // The frame's reads. The block keeps the last four samples read, and the voice the samples for the next frame.
    constexpr int kHistory = EnhancedVoice::kHistory;
    const Places places = PlacesOf(pb);
    const int32_t reads = places.reads.back();
    std::array<int16_t, kHistory + kEnhancedMostReads> input; // the first kHistory + reads
    std::copy(voice.history.begin(), voice.history.end(), input.begin());
    Accelerator accelerator(pb, memory, false);
    for (int32_t k = 0; k < reads; ++k)
    {
        input[static_cast<std::size_t>(kHistory + k)] = accelerator.Next();
    }
    accelerator.Store();
    for (std::size_t i = 0; i < 4; ++i)
    {
        pb.src.last[i] = input[static_cast<std::size_t>(kHistory + reads - 4) + i];
    }
    std::copy_n(input.begin() + reads, voice.history.size(), voice.history.begin());

    // The gains that the envelope and the mixes reach.
    const auto delta = static_cast<uint16_t>(pb.ve.delta);
    const auto ve_step = static_cast<int16_t>(static_cast<uint16_t>(2 * delta));
    pb.ve.volume = static_cast<uint16_t>(pb.ve.volume +
                                         static_cast<uint32_t>(static_cast<int32_t>(kEnhancedSamples / 2) * ve_step));
    voice.ve = static_cast<float>(pb.ve.volume) / 32768.0f;
    for (int bus = 0; bus < kBusCount; ++bus)
    {
        for (int channel = 0; channel < kTvChannels; ++channel)
        {
            voice.tv[static_cast<std::size_t>(channel)][static_cast<std::size_t>(bus)] =
                MixGainAfter(pb.tv_mix[channel][bus], (pb.tv_mask[bus] >> (2 * channel)) & 3);
        }
    }
    for (int drc = 0; drc < kDrcCount; ++drc)
    {
        for (int bus = 0; bus < kBusCount; ++bus)
        {
            for (int channel = 0; channel < kDrcChannels; ++channel)
            {
                voice.drc[static_cast<std::size_t>(drc)][static_cast<std::size_t>(channel)]
                         [static_cast<std::size_t>(bus)] =
                    MixGainAfter(pb.drc_mix[drc][channel][bus], (pb.drc_mask[drc][bus] >> (2 * channel)) & 3);
            }
        }
    }
    voice.starts = false;
}

void ResetHistory(EnhancedVoice& voice, const std::array<int16_t, 4>& last)
{
    voice.history.fill(0);
    std::copy(last.begin(), last.end(), voice.history.end() - 4);
}

} // namespace ntdlib::cafe::ax
