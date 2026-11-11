// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

#include "cafe/ax/enhanced.h"
#include "ntdlib/cafe/ax/pb.h"
#include "ntdlib/cafe/ax/render.h"
#include "ntdlib/cafe/ax/system.h"
#include "ntdlib/fused.h"
#include "ntdlib/rounding.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::cafe::ax
{
namespace
{

// A float on the scale of 16-bit samples, rounded as RoundToInt32 (ntdlib/rounding.h) rounds it, and held to 16 bits.
int16_t RoundToInt16(float x)
{
    return static_cast<int16_t>(std::clamp(std::round(static_cast<double>(x)), -32768.0, 32767.0));
}

bool IsSilent(float sample)
{
    return sample == 0.0f;
}

// A peak of floats as a meter gives it: the magnitude, truncated and held to 32 bits.
int32_t PeakOf(float magnitude)
{
    return static_cast<int32_t>(std::min(static_cast<double>(magnitude), 2147483647.0));
}

// The enhanced mode's aux buses (see System): the 48 kHz frame, the 32 kHz frame, the sends and the returns that each
// channel keeps, the place of the input's first 32 kHz sample in the sends, and the place of the first return in the
// effect's output.
constexpr int kSendSamples = kSamplesPerFrame48k;
constexpr int kAuxSamples = kSamplesPerFrame32k;
constexpr std::size_t kSendHistory = 3 * kSendSamples;
constexpr std::size_t kReturnHistory = 2 * kAuxSamples;
constexpr int kFirstSend = kSendSamples + 48;
constexpr int kFirstReturn = kAuxSamples - 32;

// One channel's sends in an enhanced frame (System::ReturnAuxEnhanced). `sends` keeps the channel's last three 48 kHz
// frames. `input` takes the 32 kHz frame from `at`.
void SendEnhanced(float* sends, const FloatBuses::Channel& sent, std::vector<int32_t>& input, std::size_t at,
                  const wave::SincKernel& down)
{
    // The frame's sends, and the 32 kHz frame before this one for the callbacks. The interpolator's window lies inside
    // the sends that the bus keeps. When they are all 0, so is the frame.
    std::copy(sends + kSendSamples, sends + kSendHistory, sends);
    std::copy_n(sent.begin(), kSendSamples, sends + 2 * kSendSamples);
    if (std::all_of(sends, sends + kSendHistory, IsSilent))
    {
        std::fill_n(input.begin() + static_cast<std::ptrdiff_t>(at), kAuxSamples, 0);
        return;
    }

    for (int k = 0; k < kAuxSamples; ++k)
    {
        const int place = kFirstSend + 3 * k / 2;
        const uint32_t fraction = 3 * k % 2 == 0 ? 0 : 0x8000;
        const float sample = down.At(sends + place - down.HalfWidth() + 1, fraction);
        input[at + static_cast<std::size_t>(k)] = RoundToInt32(sample);
    }
}

// One channel's return in an enhanced frame (System::ReturnAuxEnhanced), from `output` at `at` into `main`. The return
// volume moves from `from` to `to`. `returns` keeps the channel's last two 32 kHz frames. Gives the peak of what it
// added.
float ReturnEnhanced(float* returns, const std::vector<int32_t>& output, std::size_t at, float from, float to,
                     const wave::SincKernel& up, FloatBuses::Channel& main)
{
    // The effect's output of the frame before, back at 48 kHz, at the return volume. Returns that are all 0 add
    // nothing.
    std::copy(returns + kAuxSamples, returns + kReturnHistory, returns);
    for (int k = 0; k < kAuxSamples; ++k)
    {
        returns[kAuxSamples + k] = static_cast<float>(output[at + static_cast<std::size_t>(k)]);
    }
    if (std::all_of(returns, returns + kReturnHistory, IsSilent))
    {
        return 0.0f;
    }

    float peak = 0.0f;
    for (int i = 0; i < kSendSamples; ++i)
    {
        const int place = kFirstReturn + 2 * i / 3;
        const float wet = up.At(returns + place - up.HalfWidth() + 1, TwoThirdsFraction(i));
        const float added = wet * FrameRampAt(from, to, i);
        main[static_cast<std::size_t>(i)] += added;
        peak = std::max(peak, std::fabs(added));
    }

    return peak;
}

// The FIR upsampler's taps, from the system library's data (sndcore2 0x100056E8 and snd_core 0x10004D68 in version
// 15702 of the OS, and the same in earlier versions). There are three phases of 12 taps. Every two inputs give three
// outputs, and each phase gives one of them. Phase p gives output 3k + p from inputs 2k + 1 down to 2k - 10. The first
// tap is for the latest input. Each phase's taps add up to 1 within 0.001.
// clang-format off
constexpr std::array<float, 36> kFirTaps = {
    0.0f,              -0x1.29521p-8f,    0x1.69a54p-7f,     -0x1.0f891p-5f,    0x1.4d715p-4f,     -0x1.863184p-3f,
    0x1.79054ep-1f,    0x1.02b344p-1f,    -0x1.34c8aep-3f,   0x1.f82c7ep-5f,    -0x1.79083cp-6f,   0x1.c9e4a4p-8f,
    0.0f,              0x1.3a63a8p-9f,    -0x1.3a883ep-7f,   0x1.ef034ap-6f,    -0x1.503674p-4f,   0x1.100f26p-2f,
    0x1.d676cp-1f,     -0x1.6c0b82p-3f,   0x1.43f1ap-4f,     -0x1.1d7ef4p-5f,   0x1.9fc89cp-7f,    -0x1.3afe62p-8f,
    -0x1.f5e68p-9f,    0x1.4bbefep-7f,    -0x1.8f5756p-6f,   0x1.8c31a4p-5f,    -0x1.8b981ep-4f,   0x1.050054p+0f,
    0x1.c8094ap-5f,    -0x1.2a2e28p-7f,   -0x1.54a516p-10f,  0x1.3dc1d2p-9f,    -0x1.715fe2p-10f,  0.0f,
};
// clang-format on

} // namespace

void System::MeasureMeters(int samples)
{
    for (std::size_t meter = 0; meter < meter_sums_.size(); ++meter)
    {
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            if (enhanced_ != nullptr)
            {
                float peak = 0.0f;
                for (const float sum : enhanced_->meter_sums[meter][channel])
                {
                    peak = std::max(peak, std::fabs(sum));
                }
                meter_peaks_[meter][channel] = PeakOf(peak);
                continue;
            }

            int64_t peak = 0;
            const Buses::Channel& sums = meter_sums_[meter][channel];
            for (int i = 0; i < samples; ++i)
            {
                peak = std::max(peak, std::abs(int64_t{sums[static_cast<std::size_t>(i)]}));
            }
            meter_peaks_[meter][channel] =
                static_cast<int32_t>(std::min<int64_t>(peak, std::numeric_limits<int32_t>::max()));
        }
    }
}

System::AuxBus* System::FindAuxBus(Device device, uint32_t index, int bus)
{
    if (bus < 0 || bus > 2)
    {
        return nullptr;
    }
    if (device == Device::kTv && index == 0)
    {
        return &aux_[0][bus];
    }
    if (device == Device::kDrc && index < kDrcCount)
    {
        return &aux_[1 + index][bus];
    }

    return nullptr;
}

void System::ReturnAux(int frame)
{
    // The DSP writes each aux bus that has a callback into one of three buffers. At the bus's return volume, the DSP
    // adds back into the main bus the buffer that the callbacks processed a frame ago. That buffer holds what the
    // voices sent two frames ago. The volume moves across the frame from the one the DSP used last.
    const int n = samples_per_frame_;
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (int bus = 0; bus < 3; ++bus)
        {
            AuxBus& aux = aux_[device][bus];
            std::array<int32_t, 2>& peaks = aux_peaks_[device][static_cast<std::size_t>(bus)];
            peaks = {0, 0};
            if (!aux.callback)
            {
                continue;
            }

            std::vector<int32_t>& out = aux.buffers[frame % 3];
            const std::vector<int32_t>& back = aux.buffers[(frame + 1) % 3];
            const auto samples = static_cast<std::size_t>(n);
            for (int channel = 0; channel < channels; ++channel)
            {
                const Buses::Channel& sent =
                    device == 0 ? buses_.tv[1 + bus][channel] : buses_.drc[device - 1][1 + bus][channel];
                Buses::Channel& main = device == 0 ? main_.tv[0][channel] : main_.drc[device - 1][0][channel];
                const std::size_t at = static_cast<std::size_t>(channel) * kMaxSamplesPerFrame;
                std::copy_n(sent.begin(), samples, out.begin() + static_cast<std::ptrdiff_t>(at));
                const int32_t peak = ntdlib::cafe::ax::ReturnAux({back.data() + at, samples}, aux.dsp_return_volume,
                                                                 aux.return_volume, {main.data(), samples});
                if (channel < 2)
                {
                    peaks[static_cast<std::size_t>(channel)] = peak;
                }
            }
            aux.dsp_return_volume = aux.return_volume;
        }
    }
}

void System::ReturnAuxEnhanced(int frame)
{
    // Each aux bus that has a callback takes the frame's sends. The 32 kHz frame before this one goes to the buffer
    // that the callbacks process in this frame. The buffer that they processed a frame ago holds the effect's output of
    // the 32 kHz frame before that. That output goes back to 48 kHz into the main bus. The sends keep the frames F - 2
    // to F at 48 kHz, and the returns the 32 kHz frames F - 3 and F - 2.
    const wave::SincKernel& down = wave::SincKernel::ForStep(1.5);
    const wave::SincKernel& up = wave::SincKernel::ForStep(2.0 / 3.0);
    EnhancedState& enhanced = *enhanced_;
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (int bus = 0; bus < 3; ++bus)
        {
            AuxBus& aux = aux_[device][static_cast<std::size_t>(bus)];
            std::array<int32_t, 2>& peaks = aux_peaks_[device][static_cast<std::size_t>(bus)];
            peaks = {0, 0};
            if (!aux.callback)
            {
                continue;
            }

            const auto count = static_cast<std::size_t>(channels);
            if (aux.sends.empty())
            {
                aux.sends.assign(count * kSendHistory, 0.0f);
                aux.returns.assign(count * kReturnHistory, 0.0f);
            }
            std::vector<int32_t>& input = aux.buffers[static_cast<std::size_t>((frame + 2) % 3)];
            const std::vector<int32_t>& output = aux.buffers[static_cast<std::size_t>((frame + 1) % 3)];
            const float from = static_cast<float>(aux.dsp_return_volume) / 32768.0f;
            const float to = static_cast<float>(aux.return_volume) / 32768.0f;
            for (int channel = 0; channel < channels; ++channel)
            {
                const auto c = static_cast<std::size_t>(channel);
                const FloatBuses::Channel& sent = device == 0 ? enhanced.buses.tv[1 + bus][channel]
                                                              : enhanced.buses.drc[device - 1][1 + bus][channel];
                FloatBuses::Channel& main =
                    device == 0 ? enhanced.main.tv[0][channel] : enhanced.main.drc[device - 1][0][channel];
                const std::size_t at = c * kMaxSamplesPerFrame;
                SendEnhanced(aux.sends.data() + c * kSendHistory, sent, input, at, down);

                // A meter of a bus whose returns are all 0 reads 0.
                const float peak =
                    ReturnEnhanced(aux.returns.data() + c * kReturnHistory, output, at, from, to, up, main);
                if (channel < 2)
                {
                    peaks[c] = PeakOf(peak);
                }
            }
            aux.dsp_return_volume = aux.return_volume;
        }
    }
}

void System::RunAuxCallbacks(int frame)
{
    // Each callback processes what the voices sent its bus in the last frame: TV A, B, C, then each GamePad's buses.
    for (std::size_t device = 0; device < aux_.size(); ++device)
    {
        const int channels = device == 0 ? kTvChannels : kDrcChannels;
        for (AuxBus& aux : aux_[device])
        {
            if (!aux.callback)
            {
                continue;
            }

            std::vector<int32_t>& buffer = aux.buffers[(frame + 2) % 3];
            std::array<int32_t*, kTvChannels> channel_buffers{};
            for (int channel = 0; channel < channels; ++channel)
            {
                channel_buffers[channel] = buffer.data() + static_cast<std::size_t>(channel) * kMaxSamplesPerFrame;
            }
            aux.callback(std::span<int32_t* const>(channel_buffers.data(), static_cast<std::size_t>(channels)),
                         static_cast<uint32_t>(AuxSamplesPerFrame()));
        }
    }
}

void System::FinalMixStage()
{
    const int n = samples_per_frame_;
    const bool upsample = n == kSamplesPerFrame32k;
    for (std::size_t m = 0; m < mixers_.size(); ++m)
    {
        // The device's channels, all of each device in turn.
        Mixer& mixer = mixers_[m];
        std::array<Samples*, kTvChannels + kDrcCount * kDrcChannels> in{};
        std::array<Samples*, kTvChannels + kDrcCount * kDrcChannels> out{};
        const std::size_t count = static_cast<std::size_t>(mixer.channels) * mixer.devices;
        for (std::size_t c = 0; c < count; ++c)
        {
            in[c] = m == 0 ? &output_.tv[c] : &output_.drc[c / kDrcChannels][c % kDrcChannels];
            out[c] = m == 0 ? &output48_.tv[c] : &output48_.drc[c / kDrcChannels][c % kDrcChannels];
        }

        // To 16-bit scale and 48 kHz, before the final-mix callback or after it.
        const bool before = upsample && !mixer.upsample_after;
        std::array<Samples, kTvChannels + kDrcCount * kDrcChannels> scratch;
        std::array<int32_t*, kTvChannels + kDrcCount * kDrcChannels> pointers{};
        for (std::size_t c = 0; c < count; ++c)
        {
            if (before)
            {
                Upsample(mixer, c, std::span<const int32_t>(in[c]->data(), n), *out[c], true);
                pointers[c] = out[c]->data();
            }
            else
            {
                Samples& shifted = upsample ? scratch[c] : *out[c];
                for (int i = 0; i < n; ++i)
                {
                    shifted[i] = (*in[c])[i] >> 8;
                }
                pointers[c] = shifted.data();
            }
        }

        FinalMix final_mix{std::span<int32_t* const>(pointers.data(), count), static_cast<uint16_t>(mixer.channels),
                           static_cast<uint16_t>(before || !upsample ? kOutputSamplesPerFrame : n),
                           static_cast<uint16_t>(mixer.devices), static_cast<uint16_t>(mixer.channels)};
        if (mixer.callback)
        {
            mixer.callback(final_mix);
        }

        if (upsample && !before)
        {
            for (std::size_t c = 0; c < count; ++c)
            {
                Upsample(mixer, c, std::span<const int32_t>(scratch[c].data(), n), *out[c], false);
            }
        }

        for (int device = 0; device < mixer.devices; ++device)
        {
            Remix(mixer, final_mix.channels_out,
                  std::span<Samples* const>(out.data() + static_cast<std::size_t>(device) * mixer.channels,
                                            static_cast<std::size_t>(mixer.channels)));
        }
    }
}

void System::FinalMixStageEnhanced()
{
    // Each device's main bus, without an upsampler. A final-mix callback takes the channels as 32-bit samples.
    EnhancedState& enhanced = *enhanced_;
    for (std::size_t m = 0; m < mixers_.size(); ++m)
    {
        Mixer& mixer = mixers_[m];
        std::array<FloatBuses::Channel*, kTvChannels + kDrcCount * kDrcChannels> channels{};
        const std::size_t count = static_cast<std::size_t>(mixer.channels) * mixer.devices;
        for (std::size_t c = 0; c < count; ++c)
        {
            channels[c] = m == 0 ? &enhanced.main.tv[0][c] : &enhanced.main.drc[c / kDrcChannels][0][c % kDrcChannels];
        }

        uint16_t channels_out = static_cast<uint16_t>(mixer.channels);
        if (mixer.callback)
        {
            std::array<Samples, kTvChannels + kDrcCount * kDrcChannels> samples{};
            std::array<int32_t*, kTvChannels + kDrcCount * kDrcChannels> pointers{};
            for (std::size_t c = 0; c < count; ++c)
            {
                for (std::size_t i = 0; i < samples[c].size(); ++i)
                {
                    samples[c][i] = RoundToInt32((*channels[c])[i]);
                }
                pointers[c] = samples[c].data();
            }
            FinalMix final_mix{std::span<int32_t* const>(pointers.data(), count), static_cast<uint16_t>(mixer.channels),
                               static_cast<uint16_t>(kOutputSamplesPerFrame), static_cast<uint16_t>(mixer.devices),
                               static_cast<uint16_t>(mixer.channels)};
            mixer.callback(final_mix);
            channels_out = final_mix.channels_out;
            for (std::size_t c = 0; c < count; ++c)
            {
                for (std::size_t i = 0; i < samples[c].size(); ++i)
                {
                    (*channels[c])[i] = static_cast<float>(samples[c][i]);
                }
            }
        }

        for (int device = 0; device < mixer.devices; ++device)
        {
            RemixEnhanced(mixer, channels_out,
                          std::span<FloatBuses::Channel* const>(
                              channels.data() + static_cast<std::size_t>(device) * mixer.channels,
                              static_cast<std::size_t>(mixer.channels)));
        }
    }

    // The TV's stereo output.
    for (std::size_t i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        const float left = enhanced.main.tv[0][0][i];
        const float right = enhanced.main.tv[0][1][i];
        enhanced.output[2 * i] = left / 32768.0f;
        enhanced.output[2 * i + 1] = right / 32768.0f;
        stereo_[2 * i] = RoundToInt16(left);
        stereo_[2 * i + 1] = RoundToInt16(right);
    }
}

void System::Upsample(Mixer& mixer, std::size_t channel, std::span<const int32_t> in, std::span<int32_t> out,
                      bool shift) const
{
    if (cemu_.linear_upsampler || mixer.linear[channel / static_cast<std::size_t>(mixer.channels)])
    {
        UpsampleLinear(in, out, mixer.history[channel], shift);
    }
    else
    {
        UpsampleFir(in, out, mixer.fir[channel], shift);
    }
}

void System::UpsampleLinear(std::span<const int32_t> in, std::span<int32_t> out, int32_t& history, bool shift)
{
    // Three outputs for every two inputs, spaced evenly and two thirds of an input behind (sndcore2 0x02007B2C in
    // version 15702 of the OS). The middle output is the first input. The other two weigh two inputs by 2/3 and 1/3 in
    // single precision.
    const float two_thirds = std::bit_cast<float>(0x3F2AAAABu);
    const float one_third = std::bit_cast<float>(0x3EAAAAAAu);
    const int bits = shift ? 8 : 0;

    // The product by 2/3 is fused with the sum.
    const auto weigh = [two_thirds](float x, float third)
    {
        return std::fma(x, two_thirds, third);
    };

    for (std::size_t k = 0; 2 * k + 1 < in.size(); ++k)
    {
        const float third_of_first = static_cast<float>(in[2 * k]) * one_third;
        out[3 * k] = ntdlib::PowerPcToInt32(weigh(static_cast<float>(history), third_of_first)) >> bits;
        out[3 * k + 1] = in[2 * k] >> bits;
        out[3 * k + 2] = ntdlib::PowerPcToInt32(weigh(static_cast<float>(in[2 * k + 1]), third_of_first)) >> bits;
        history = in[2 * k + 1];
    }
}

void System::UpsampleFir(std::span<const int32_t> in, std::span<int32_t> out, FirHistory& history, bool shift)
{
    // sndcore2 0x02007C84 and snd_core 0x02006140 in version 15702 of the OS, in paired singles. The inputs go in
    // pairs. Each pair holds the later input first. Each phase sums its taps on the pairs' later inputs apart from its
    // taps on the earlier inputs. Each sum starts from the latest pair and takes a product and five fused
    // multiply-adds. Then the phase adds the two sums.
    const int bits = shift ? 8 : 0;
    for (std::size_t k = 0; 2 * k + 1 < in.size(); ++k)
    {
        std::array<float, 12> inputs;
        inputs[0] = static_cast<float>(in[2 * k + 1]);
        inputs[1] = static_cast<float>(in[2 * k]);
        std::copy(history.begin(), history.end(), inputs.begin() + 2);

        for (std::size_t phase = 0; phase < 3; ++phase)
        {
            const float* taps = &kFirTaps[12 * phase];
            float later = taps[0] * inputs[0];
            float earlier = taps[1] * inputs[1];
            for (std::size_t j = 2; j < inputs.size(); j += 2)
            {
                later = std::fma(taps[j], inputs[j], later);
                earlier = std::fma(taps[j + 1], inputs[j + 1], earlier);
            }
            out[3 * k + phase] = ntdlib::PowerPcToInt32(later + earlier) >> bits;
        }

        std::copy(inputs.begin(), inputs.begin() + history.size(), history.begin());
    }
}

const System::RemixMatrix* System::RemixMatrixOf(const Mixer& mixer, uint16_t channels_in, std::size_t channels)
{
    constexpr uint32_t kDeviceChannels = 2;
    for (const RemixMatrix& candidate : mixer.matrices)
    {
        if (candidate.in == channels_in && candidate.out == kDeviceChannels && candidate.in <= channels)
        {
            return &candidate;
        }
    }

    return nullptr;
}

void System::Remix(const Mixer& mixer, uint16_t channels_in, std::span<Samples* const> channels)
{
    // The matrix for the final-mix callback's output channels and the device's two channels, applied in place. Each
    // output is the sum of the inputs' weighted values. Each weighted value is truncated to an integer (sndcore2
    // 0x02008B00 in version 15702 of the OS).
    const RemixMatrix* matrix = RemixMatrixOf(mixer, channels_in, channels.size());
    if (!matrix)
    {
        return;
    }

    for (int i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        std::array<float, kTvChannels> in{};
        for (uint32_t k = 0; k < matrix->in; ++k)
        {
            in[k] = static_cast<float>((*channels[k])[i]);
        }
        for (uint32_t j = 0; j < matrix->out; ++j)
        {
            uint32_t sum = 0;
            for (uint32_t k = 0; k < matrix->in; ++k)
            {
                sum += static_cast<uint32_t>(ntdlib::PowerPcToInt32(in[k] * matrix->weights[j * matrix->in + k]));
            }
            (*channels[j])[i] = static_cast<int32_t>(sum);
        }
    }
}

void System::RemixEnhanced(const Mixer& mixer, uint16_t channels_in, std::span<FloatBuses::Channel* const> channels)
{
    // Remix's matrix, in floating point: each output is the sum of the inputs' weighted values.
    const RemixMatrix* matrix = RemixMatrixOf(mixer, channels_in, channels.size());
    if (!matrix)
    {
        return;
    }

    for (std::size_t i = 0; i < kOutputSamplesPerFrame; ++i)
    {
        std::array<float, kTvChannels> in{};
        for (uint32_t k = 0; k < matrix->in; ++k)
        {
            in[k] = (*channels[k])[i];
        }
        for (uint32_t j = 0; j < matrix->out; ++j)
        {
            float sum = 0.0f;
            for (uint32_t k = 0; k < matrix->in; ++k)
            {
                sum += in[k] * matrix->weights[j * matrix->in + k];
            }
            (*channels[j])[i] = sum;
        }
    }
}

} // namespace ntdlib::cafe::ax
