// SPDX-License-Identifier: MIT

// The enhanced mixing mode of the model of the DSP's audio firmware (see firmware_model.h): the sources in host memory,
// and the renderer that mixes them at 48 kHz in floating point.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "xsflib/bytes.h"

#include "ctr/dsp/firmware_audio.h"
#include "ntdlib/ctr/dsp/dsp.h"
#include "ntdlib/wave/sinc.h"

namespace ntdlib::ctr::dsp::firmware
{
namespace
{

// The shared structures that the sources' entries make up, by their places in the list of addresses that the firmware
// sends.
constexpr int kParamsStructure = 1;
constexpr int kReportsStructure = 2;
constexpr int kCoefsStructure = 3;

// The words of a source's block before its report and before its other state.
constexpr uint32_t kBlockReport = internal::kSourceStateSize;
constexpr uint32_t kBlockVoice = internal::kSourceStateSize + internal::kReportSize;

// The bytes of an entry of each shared structure.
constexpr std::size_t kParamsBytes = 2 * kSourceParamsSize;
constexpr std::size_t kReportBytes = 2 * internal::kReportSize;
constexpr std::size_t kCoefsBytes = 2 * kAdpcmCoefsSize;
constexpr std::size_t kBlockBytes = 2 * kBlockWords;

// An enhanced frame's outputs fall on the console's frame: 47 outputs to each 32 of the console's samples.
constexpr int64_t kConsoleSteps = 32;
constexpr int64_t kOutputSteps = 47;
static_assert(kFrameSamples * kOutputSteps == kEnhancedFrameSamples * kConsoleSteps);

// The most samples that the firmware's decode of a chunk takes in each format (Audio::Decode).
uint32_t ChunkLimit(uint16_t format)
{
    const bool stereo = (format & 3) == 2;
    switch ((format >> 2) & 3)
    {
    case 0:
        return stereo ? 0x400 : 0x800;
    case 1:
        return stereo ? 0x200 : 0x400;
    default:
        return 0xe00;
    }
}

// The place of output `m` of a frame, in 8.24 samples after the place of the console's first output. That output
// interpolates between the third and second samples before the frame's first new sample, at the phase `phase`. Each
// console sample steps by `rate`.
int64_t Place(int64_t phase, uint32_t rate, int m)
{
    return (phase * kOutputSteps + int64_t{m} * kConsoleSteps * rate) / kOutputSteps;
}

// A sample of an aux bus as a 32-bit integer for the ARM11, on the scale of the firmware's sums: rounded to the nearest
// integer, halves away from zero, and held within 32 bits.
int32_t AuxSample(float sample)
{
    const double rounded = std::round(static_cast<double>(sample));
    return static_cast<int32_t>(std::clamp(rounded, static_cast<double>(std::numeric_limits<int32_t>::min()),
                                           static_cast<double>(std::numeric_limits<int32_t>::max())));
}

} // namespace

int Audio::SourceCount() const
{
    return enhanced_ ? static_cast<int>(host_.sources.size()) : kSources;
}

void Audio::GrowSources(int count)
{
    const int old = SourceCount();
    count = std::min(count, dsp::kEnhancedSources);
    if (!enhanced_ || count <= old)
    {
        return;
    }

    const auto n = static_cast<std::size_t>(count);
    for (int region = 0; region < 2; region++)
    {
        host_.params[region].resize(n * kParamsBytes, 0);
        host_.reports[region].resize(n * kReportBytes, 0);
        host_.coefs[region].resize(n * kCoefsBytes, 0);
    }

    host_.blocks.resize(n * kBlockBytes, 0);
    host_.sources.resize(n);
    source_meters_.resize(n, -1);
    for (int i = old; i < count; i++)
    {
        InitBlock(i);
    }
}

uint8_t* Audio::SourceStructure(int structure, int region)
{
    if (!enhanced_ || region < 0 || region > 1)
    {
        return nullptr;
    }

    switch (structure)
    {
    case kParamsStructure:
        return host_.params[region].data();
    case kReportsStructure:
        return host_.reports[region].data();
    case kCoefsStructure:
        return host_.coefs[region].data();
    default:
        return nullptr;
    }
}

int32_t* Audio::AuxStructure(int region)
{
    if (!enhanced_ || region < 0 || region > 1)
    {
        return nullptr;
    }

    return host_.aux[static_cast<std::size_t>(region)].data();
}

void Audio::RestoreHost(const HostSources& host)
{
    host_ = host;
    source_meters_.resize(host_.sources.size(), -1);
}

uint8_t* Audio::Block(int source)
{
    return host_.blocks.data() + static_cast<std::size_t>(source) * kBlockBytes;
}

// A source as the firmware's sources start: zeros, with the pointers of the state to source 0's report and other
// state, where each source does its work.
void Audio::InitBlock(int source)
{
    uint8_t* const block = Block(source);
    std::fill(block, block + kBlockBytes, 0);
    xsflib::StoreLe16(block + 2 * state::kReport, internal::kReport);
    xsflib::StoreLe16(block + 2 * state::kVoice, internal::kVoice);
    host_.sources[static_cast<std::size_t>(source)] = EnhancedSource{};
}

void Audio::SwapIn(int source)
{
    const uint8_t* const block = Block(source);
    std::memcpy(data_ + 2 * SourceState(0), block, 2 * internal::kSourceStateSize);
    std::memcpy(data_ + 2 * internal::kReport, block + 2 * kBlockReport, kReportBytes);
    std::memcpy(data_ + 2 * internal::kVoice, block + 2 * kBlockVoice, 2 * internal::kVoiceSize);
}

void Audio::SwapOut(int source)
{
    uint8_t* const block = Block(source);
    std::memcpy(block, data_ + 2 * SourceState(0), 2 * internal::kSourceStateSize);
    std::memcpy(block + 2 * kBlockReport, data_ + 2 * internal::kReport, kReportBytes);
    std::memcpy(block + 2 * kBlockVoice, data_ + 2 * internal::kVoice, 2 * internal::kVoiceSize);
}

// TakeParams's part for the sources: each source with dirty parameters takes them in source 0's places.
void Audio::TakeHostParams(int region)
{
    const uint32_t p = Shared(kSourceParams, region);
    const uint32_t coefs = Shared(kAdpcmCoefs, region);
    for (int i = 0; i < SourceCount(); i++)
    {
        uint8_t* const params = host_.params[region].data() + static_cast<std::size_t>(i) * kParamsBytes;
        if (xsflib::LoadLe16(params + 2 * params::kDirty) == 0 &&
            xsflib::LoadLe16(params + 2 * params::kDirty + 2) == 0)
        {
            continue;
        }

        SwapIn(i);
        std::memcpy(data_ + 2 * p, params, kParamsBytes);
        std::memcpy(data_ + 2 * coefs, host_.coefs[region].data() + static_cast<std::size_t>(i) * kCoefsBytes,
                    kCoefsBytes);
        history_cleared_ = false;
        filters_cleared_ = false;
        TakeSourceParams(0, region);
        SetWord(p + params::kDirty, 0);
        SetWord(p + params::kDirty + 1, 0);
        std::memcpy(params, data_ + 2 * p, kParamsBytes);
        SwapOut(i);

        // Where the firmware clears a source's last samples or its filters' states, the enhanced mode clears them too.
        EnhancedSource& source = host_.sources[static_cast<std::size_t>(i)];
        if (history_cleared_)
        {
            for (auto& channel : source.history)
            {
                channel.fill(0);
            }
        }

        if (filters_cleared_)
        {
            source.one_pole = {};
            source.biquad = {};
        }
    }
}

// FinishFrame's part for the sources, their reports, and for the aux buses. The buses that the last frame mixed go to
// region `region` for the ARM11. The buses that the ARM11 gave back in the other region wait for the next frame's mix,
// as the firmware takes them back.
void Audio::FinishHostFrame(int region)
{
    for (int i = 0; i < SourceCount(); i++)
    {
        uint8_t* const report = Block(i) + 2 * kBlockReport;
        std::memcpy(host_.reports[region].data() + static_cast<std::size_t>(i) * kReportBytes, report, kReportBytes);
        const uint16_t enabled = xsflib::LoadLe16(report + 2 * report::kEnabled);
        xsflib::StoreLe16(report + 2 * report::kEnabled, enabled & 0x00ff);
    }

    std::array<int32_t, 2 * 4 * kEnhancedFrameSamples>& out = host_.aux[static_cast<std::size_t>(region)];
    const std::array<int32_t, 2 * 4 * kEnhancedFrameSamples>& back = host_.aux[static_cast<std::size_t>(1 - region)];
    for (std::size_t bus = 0; bus < 2; bus++)
    {
        for (std::size_t c = 0; c < 4; c++)
        {
            const std::size_t at = (4 * bus + c) * kEnhancedFrameSamples;
            for (std::size_t n = 0; n < kEnhancedFrameSamples; n++)
            {
                out[at + n] = AuxSample(host_.aux_mix[bus][c][n]);
                host_.aux_returns[bus][c][n] = static_cast<float>(back[at + n]);
            }
        }
    }
}

// MixFrame's part in the enhanced mode: the aux buses that came back from the ARM11, each source that plays, and the
// output.
void Audio::MixEnhanced()
{
    for (auto& channel : main_)
    {
        channel.fill(0.0f);
    }

    // A bus that is on joins the main bus at its return volume, as MixAux adds it. Its bypass sends its front channels
    // past the surround mode, which the enhanced mode mixes as stereo: the sum stays the same.
    for (std::size_t bus = 0; bus < 2; bus++)
    {
        const int16_t volume = Signed(internal::kAuxReturn + 2 * static_cast<uint32_t>(bus));
        if (Word(internal::kAuxEnable + static_cast<uint32_t>(bus)) != 0 && volume > 0)
        {
            const float gain = static_cast<float>(volume) / 32768.0f;
            for (std::size_t c = 0; c < 4; c++)
            {
                for (std::size_t n = 0; n < kEnhancedFrameSamples; n++)
                {
                    main_[c][n] += host_.aux_returns[bus][c][n] * gain;
                }
            }
        }

        for (auto& channel : host_.aux_mix[bus])
        {
            channel.fill(0.0f);
        }
    }

    for (EnhancedMeterSums& sums : enhanced_meter_sums_)
    {
        for (auto& channel : sums)
        {
            channel.fill(0.0f);
        }
    }

    for (int i = 0; i < SourceCount(); i++)
    {
        const bool enabled = (xsflib::LoadLe16(Block(i) + 2 * (kBlockReport + report::kEnabled)) & 0xff) == 1;
        if (enabled)
        {
            metering_ = source_meters_[static_cast<std::size_t>(i)];
            MixEnhancedSource(i);
            metering_ = -1;
        }

        uint8_t* const flags = Block(i) + 2 * state::kFlags;
        const uint16_t value = xsflib::LoadLe16(flags);
        xsflib::StoreLe16(flags, static_cast<uint16_t>(enabled ? value | state::kPlayed : value & ~state::kPlayed));
    }

    OutputEnhanced();
}

// A source's frame. The firmware's routines take the source through its buffers in source 0's places, in
// ProcessSource's steps. The source's decoded samples join those of its last frames. Each output then comes from the
// band-limited interpolator at the place of the firmware's output. The filters and the gains apply in floating point.
void Audio::MixEnhancedSource(int index)
{
    SwapIn(index);
    const uint32_t s = SourceState(0);
    const uint32_t y = Word(s + state::kVoice);
    const int64_t phase = static_cast<int32_t>(Long(s + state::kPhase));
    const uint32_t rate = Long(y + voice::kRate);
    std::array<uint16_t, 3> counts = {FillPhases(s), 0, 0};
    const uint32_t advance = counts[0];
    if (advance == 0)
    {
        SwapOut(index);
        return;
    }

    const bool stereo = (Word(s + state::kFormat) & 3) == 2;
    const int channels = stereo ? 2 : 1;
    EnhancedSource& source = host_.sources[static_cast<std::size_t>(index)];
    for (int c = 0; c < channels; c++)
    {
        input_[c].assign(source.history[c].begin(), source.history[c].end());
    }

    const auto capture = [&](uint16_t decoded)
    {
        for (int c = 0; c < channels; c++)
        {
            const uint32_t samples = internal::kDecode + c * internal::kDecodeChannel + 4;
            for (uint32_t i = 0; i < decoded; i++)
            {
                input_[c].push_back(Signed(samples + i));
            }
        }
    };
    if (!DecodeFrame(s, counts, capture))
    {
        SwapOut(index);
        return;
    }

    // A source that stops during the frame gives zeros after its end.
    const std::size_t decoded = EnhancedSource::kHistory + advance;
    for (int c = 0; c < channels; c++)
    {
        input_[c].resize(decoded, 0.0f);
        std::transform(input_[c].end() - EnhancedSource::kHistory, input_[c].end(), source.history[c].begin(),
                       [](float sample) { return static_cast<int16_t>(sample); });
    }

    // The source's mix as it stands, before the gains move on as Mix moves them: the main bus's four gains, then each
    // aux bus's.
    const uint32_t x = Word(s + state::kReport);
    const uint16_t format = Word(s + state::kFormat);
    const bool played = (Word(s + state::kFlags) & state::kPlayed) != 0;
    std::array<uint16_t, 12> gains;
    std::array<uint16_t, 12> previous;
    for (uint32_t i = 0; i < 12; i++)
    {
        gains[i] = Word(y + voice::kGain + i);
        previous[i] = Word(y + voice::kPreviousGain + i);
    }

    for (int bus = 0; bus < 3; bus++)
    {
        if (bus == 0 || !(format & 0x40))
        {
            Copy(y + voice::kPreviousGain + 4 * bus, y + voice::kGain + 4 * bus, 4);
        }
    }

    const bool playing = (Word(x + report::kEnabled) & 0xff) != 0;
    SwapOut(index);

    // The interpolator's later inputs come from a copy of the source. Source 0's places still hold that copy.
    const double step = static_cast<double>(rate) * kConsoleSteps / (static_cast<double>(kOutputSteps) * 16777216.0);
    const wave::SincKernel& kernel = wave::SincKernel::ForStep(step);
    const int64_t last = EnhancedSource::kHistory - 3 + (Place(phase, rate, kEnhancedFrameSamples - 1) >> 24);
    const int64_t ahead = std::max<int64_t>(last + kernel.HalfWidth() + 1 - static_cast<int64_t>(decoded), 0);
    if (playing)
    {
        DecodeAhead(s, static_cast<uint32_t>(ahead), channels);
    }

    for (int c = 0; c < channels; c++)
    {
        input_[c].resize(decoded + static_cast<std::size_t>(ahead) + 2 * wave::SincKernel::kMaxHalfWidth, 0.0f);
    }

    RenderEnhanced(kernel, phase, rate, channels);
    FilterEnhanced(y, source, channels);
    if (!stereo)
    {
        rendered_[1] = rendered_[0];
    }

    // A two-channel source mixes into the two-channel bus. Only the surround output takes that bus (see
    // firmware_model.h).
    if (format & 0x40)
    {
        return;
    }

    // The main bus, then the aux buses. The meters take the main bus's mix.
    const bool ramp = played || (format & 0x20);
    for (std::size_t bus = 0; bus < 3; bus++)
    {
        for (int pair = 0; pair < 2; pair++)
        {
            const auto g = static_cast<std::size_t>(4 * bus + 2 * pair);
            if (gains[g] == 0 && gains[g + 1] == 0 && previous[g] == 0 && previous[g + 1] == 0)
            {
                continue;
            }

            const bool ramped = ramp && (gains[g] != previous[g] || gains[g + 1] != previous[g + 1]);
            for (std::size_t c = 0; c < 2; c++)
            {
                const float to = static_cast<float>(gains[g + c]) / 32768.0f;
                const float from = ramped ? static_cast<float>(previous[g + c]) / 32768.0f : to;
                const float delta = (to - from) / static_cast<float>(kEnhancedFrameSamples);
                const std::size_t channel = g % 4 + c;
                float* const out = bus == 0 ? main_[channel].data() : host_.aux_mix[bus - 1][channel].data();
                float* const meter = bus == 0 && metering_ >= 0
                                         ? enhanced_meter_sums_[static_cast<std::size_t>(metering_)][channel].data()
                                         : nullptr;
                for (int n = 0; n < kEnhancedFrameSamples; n++)
                {
                    const float contribution =
                        rendered_[c][static_cast<std::size_t>(n)] * (from + static_cast<float>(n) * delta);
                    out[n] += contribution;
                    if (meter != nullptr)
                    {
                        meter[n] += contribution;
                    }
                }
            }
        }
    }
}

// Decodes `count` more samples of the source in source 0's places for the interpolator's later inputs, and adds them to
// the input. Decode decodes them the same way. A source that stops gives zeros after its end.
void Audio::DecodeAhead(uint32_t s, uint32_t count, int channels)
{
    const uint16_t format = Word(s + state::kFormat);
    const bool stereo = (format & 3) == 2;
    const uint32_t limit = ChunkLimit(format);
    while (count != 0)
    {
        const auto n = static_cast<uint16_t>(std::min(count, limit));
        switch ((format >> 2) & 3)
        {
        case 0:
            DecodePcm(s, n, stereo, true);
            break;

        case 1:
            DecodePcm(s, n, stereo, false);
            break;

        case 2:
            DecodeAdpcm(s, n, stereo);
            break;

        default:
            Clear(internal::kDecode + 4, n);
            Clear(internal::kDecode + internal::kDecodeChannel + 4, n);
            break;
        }

        for (int c = 0; c < channels; c++)
        {
            const uint32_t samples = internal::kDecode + c * internal::kDecodeChannel + 4;
            for (uint32_t i = 0; i < n; i++)
            {
                input_[c].push_back(Signed(samples + i));
            }
        }

        count -= n;
    }
}

// The source's outputs of the frame at the places of the firmware's resampler, from the interpolator. The frame's
// first new sample follows the source's history in the input.
void Audio::RenderEnhanced(const wave::SincKernel& kernel, int64_t phase, uint32_t rate, int channels)
{
    const int64_t half = kernel.HalfWidth();
    const auto size = static_cast<int64_t>(input_[0].size());
    for (int m = 0; m < kEnhancedFrameSamples; m++)
    {
        const int64_t place = Place(phase, rate, m);
        const int64_t index =
            std::clamp<int64_t>(EnhancedSource::kHistory - 3 + (place >> 24), half - 1, size - half - 1);
        const auto fraction = static_cast<uint32_t>((place >> 8) & 0xffff);
        for (int c = 0; c < channels; c++)
        {
            rendered_[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] =
                kernel.At(input_[c].data() + (index - (half - 1)), fraction);
        }
    }
}

// The source's filters: the biquad and then the one-pole, on the scale of 16-bit samples. Filter applies them in the
// same order.
void Audio::FilterEnhanced(uint32_t y, EnhancedSource& source, int channels)
{
    const uint16_t filters = Word(y + voice::kFilters);
    if (filters & 2)
    {
        const auto a2 = static_cast<float>(Signed(y + voice::kBiquad));
        const auto a1 = static_cast<float>(Signed(y + voice::kBiquad + 1));
        const auto b2 = static_cast<float>(Signed(y + voice::kBiquad + 2));
        const auto b1 = static_cast<float>(Signed(y + voice::kBiquad + 3));
        const auto b0 = static_cast<float>(Signed(y + voice::kBiquad + 4));
        for (int c = 0; c < channels; c++)
        {
            std::array<float, 4>& state = source.biquad[static_cast<std::size_t>(c)];
            for (float& sample : rendered_[static_cast<std::size_t>(c)])
            {
                const float out =
                    (a2 * state[3] + a1 * state[2] + b2 * state[1] + b1 * state[0] + b0 * sample) / 16384.0f;
                state[1] = state[0];
                state[0] = sample;
                state[3] = state[2];
                state[2] = out;
                sample = out;
            }
        }
    }

    if (filters & 1)
    {
        const auto b0 = static_cast<float>(Signed(y + voice::kOnePole));
        const auto a1 = static_cast<float>(Signed(y + voice::kOnePole + 1));
        for (int c = 0; c < channels; c++)
        {
            float& last = source.one_pole[static_cast<std::size_t>(c)];
            for (float& sample : rendered_[static_cast<std::size_t>(c)])
            {
                last = (b0 * sample + a1 * last) / 32768.0f;
                sample = last;
            }
        }
    }
}

// The main bus's output as MixFrame makes it, without the limiter or the clipping, and the meters' peaks.
void Audio::OutputEnhanced()
{
    const uint16_t ratio = Word(internal::kRearRatio);
    const float rear = ratio == 0x8000 ? 1.0f : static_cast<float>(static_cast<int16_t>(ratio)) / 32768.0f;
    const float volume = static_cast<float>(static_cast<int16_t>(Word(internal::kMasterVolume))) / 32768.0f;
    const uint16_t mode = Word(internal::kOutputMode);
    for (std::size_t n = 0; n < kEnhancedFrameSamples; n++)
    {
        float left = main_[0][n] + main_[2][n] * rear;
        float right = main_[1][n] + main_[3][n] * rear;
        if (mode == 0)
        {
            left = (left + right) * 0.5f;
            right = left;
        }
        else if (mode > 2)
        {
            left = 0.0f;
            right = 0.0f;
        }

        host_.frame[2 * n] = left * volume / 32768.0f;
        host_.frame[2 * n + 1] = right * volume / 32768.0f;
    }

    for (std::size_t m = 0; m < enhanced_meter_sums_.size(); m++)
    {
        const EnhancedMeterSums& sums = enhanced_meter_sums_[m];
        std::array<float, 2> peaks{};
        for (std::size_t n = 0; n < kEnhancedFrameSamples; n++)
        {
            for (std::size_t c = 0; c < 2; c++)
            {
                peaks[c] = std::max(peaks[c], std::fabs(sums[c][n] + sums[2 + c][n] * rear));
            }
        }

        for (std::size_t c = 0; c < 2; c++)
        {
            meter_peaks_[m][c] = static_cast<int32_t>(
                std::min<double>(std::round(peaks[c]), static_cast<double>(std::numeric_limits<int32_t>::max())));
        }
    }
}

} // namespace ntdlib::ctr::dsp::firmware
