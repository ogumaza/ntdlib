// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/voice.h"
#include "ntdlib/rounding.h"
#include "nx/audio/arithmetic.h"

namespace ntdlib::nx::audio
{
namespace
{

// The precision of volumes and mix volumes from REV7 on. This client requests REV8.
constexpr int kGainBits = 23;

// The depop's decay a sample, in Q15, for a mix at 48 kHz and at any other rate.
constexpr int32_t kDepopDecay48k = 31529;
constexpr int32_t kDepopDecay = 30923;

// The enhanced mode's depop: its decay a sample at 48 kHz and at any other rate, and the value below which it ends.
constexpr float kFloatDepopDecay48k = kDepopDecay48k / 32768.0f;
constexpr float kFloatDepopDecay = kDepopDecay / 32768.0f;
constexpr float kFloatDepopFloor = 1.0f / 65536.0f;

// The step of a ramp across a frame: the difference of the gains, which wraps, over the sample count.
int32_t RampStep(int32_t from, int32_t to, int count)
{
    return WrapSub(to, from) / count;
}

// The enhanced mode's ramp across a frame: out[i] = in[i] * (from + i * step), with the step that reaches `to` after
// `count` samples. Returns the last product.
float Ramp(float* out, const float* in, float from, float to, int count)
{
    const float step = (to - from) / static_cast<float>(count);
    float last = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        last = in[i] * (from + static_cast<float>(i) * step);
        out[i] = last;
    }

    return last;
}

// The same ramp added to `out`. Returns the last product.
float MixRamp(float* out, const float* in, float from, float to, int count)
{
    const float step = (to - from) / static_cast<float>(count);
    float last = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        last = in[i] * (from + static_cast<float>(i) * step);
        out[i] += last;
    }

    return last;
}

// The largest magnitude of `count` samples, held to 32 bits.
int32_t PeakOf(const float* samples, std::size_t count)
{
    float peak = 0.0f;
    for (std::size_t t = 0; t < count; ++t)
    {
        peak = std::max(peak, std::fabs(samples[t]));
    }

    return static_cast<int32_t>(std::min(peak, 2147483520.0f));
}

int32_t PeakOf(const int32_t* samples, std::size_t count)
{
    int64_t peak = 0;
    for (std::size_t t = 0; t < count; ++t)
    {
        const int64_t x = samples[t];
        peak = std::max(peak, x < 0 ? -x : x);
    }

    return static_cast<int32_t>(std::min<int64_t>(peak, std::numeric_limits<int32_t>::max()));
}

} // namespace

void Renderer::RenderFrame()
{
    Update();
    Generate();
    if (observer_ != nullptr)
    {
        observer_->RenderFrame();
    }
}

int32_t* Renderer::Buffer(int index)
{
    return buffers_.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(parameter_.sample_count);
}

float* Renderer::FloatBuffer(int index)
{
    return float_buffers_.data() + static_cast<std::size_t>(index) * static_cast<std::size_t>(parameter_.sample_count);
}

// RequestUpdateAudioRenderer: the renderer takes the client's parameters, and the client takes the voices' statuses.
// The renderer copied the statuses at the last command generation, before that frame's processing.
void Renderer::Update()
{
    for (std::size_t slot = 0; slot < voices_.size(); ++slot)
    {
        Voice& voice = voices_[slot];
        voice.server.in_use = voice.in_use;
        if (voice.in_use)
        {
            UpdateVoice(voice, static_cast<int>(slot));
        }
    }

    mutes_at_once_ = false;
    UpdateSplitters();

    // The effects' settings.
    for (Aux& aux : auxes_)
    {
        aux.server_enabled = aux.enabled;
        aux.server_inputs = aux.inputs;
        aux.server_outputs = aux.outputs;
        aux.server_ring = aux.ring;
    }

    UpdateRendererEffects();
}

// A voice's update in slot `slot`: the renderer takes the client's parameters and buffers, and the client takes the
// voice's statuses.
void Renderer::UpdateVoice(Voice& voice, int slot)
{
    // A new voice starts again from nothing (VoiceInfo::Initialize).
    Voice::Server& server = voice.server;
    if (voice.is_new)
    {
        server = Voice::Server{};
        server.in_use = true;
        server.is_new = true;
    }

    // The play state that the renderer keeps (VoiceInfo::UpdateParameters, 0x6a300).
    server.previous = server.state;
    switch (voice.play_state)
    {
    case PlayState::kStarted:
        server.state = 0;
        break;
    case PlayState::kStopped:
        server.state = server.state == 1 ? 1 : 2;
        break;
    case PlayState::kPaused:
        server.state = 3;
        break;
    }

    server.format = voice.format;
    server.sample_rate = voice.sample_rate;
    server.priority = voice.priority;
    server.sort_order = static_cast<int32_t>(std::find(order_.begin(), order_.end(), slot) - order_.begin());
    server.channel_count = voice.channel_count;
    server.behavior = voice.behavior;
    server.pitch = voice.pitch;
    server.volume = voice.volume;
    server.muted = voice.muted;
    server.meter = voice.meter;
    server.quality = voice.quality;
    server.biquads = voice.biquads;
    server.mix = voice.mix;
    server.splitter = voice.splitter;
    server.buffer_count = voice.buffer_count;

    TakeWaveBuffers(voice);

    for (int c = 0; c < voice.channel_count; ++c)
    {
        ChannelResource& resource = Resource(voice, c);
        resource.mix = resource.client_mix;
    }

    // After a restore, the voice takes its mute at once. A muted voice's last samples then leave the depop nothing.
    if (mutes_at_once_)
    {
        MuteAtOnce(voice);
    }

    ReportStatuses(voice);
}

void Renderer::MuteAtOnce(Voice& voice)
{
    Voice::Server& server = voice.server;
    server.previous_muted = server.muted;
    for (int c = 0; server.muted && c < voice.channel_count; ++c)
    {
        Resource(voice, c).dsp.last_samples = {};
        if (parameter_.enhanced)
        {
            enhanced_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])].last = {};
        }
    }
}

void Renderer::ReportStatuses(Voice& voice)
{
    // The statuses: none for a voice the renderer hasn't started yet, otherwise channel 0's statuses.
    const Voice::Server& server = voice.server;
    if (server.is_new)
    {
        voice.played = 0;
        voice.consumed = 0;
    }
    else
    {
        const VoiceState& reported = Resource(voice, 0).reported;
        voice.played = reported.played;
        voice.consumed = reported.consumed;
    }
    voice.dropped = false;
    voice.is_new = false;
    for (ClientWaveBuffer& buffer : voice.buffers)
    {
        buffer.sent = true;
    }
}

// The buffers the client appended since the last update, if their ranges fit (VoiceInfo::UpdateWaveBuffer).
void Renderer::TakeWaveBuffers(Voice& voice)
{
    Voice::Server& server = voice.server;
    for (std::size_t i = 0; i < voice.buffers.size(); ++i)
    {
        const WaveBufferInfo& info = voice.buffers[i].info;
        if (voice.buffers[i].sent || info.start < 0 || info.end < 0)
        {
            continue;
        }

        const uint64_t size = info.buffer.size();
        const uint64_t bytes = voice.format == SampleFormat::kPcmFloat ? 4 : 2;
        const bool fits =
            voice.format == SampleFormat::kAdpcm
                ? static_cast<uint64_t>(AdpcmBytes(info.start)) <= size &&
                      static_cast<uint64_t>(AdpcmBytes(info.end)) <= size
                : static_cast<uint64_t>(info.start) * bytes <= size && static_cast<uint64_t>(info.end) * bytes <= size;
        if (!fits)
        {
            continue;
        }

        WaveBuffer& buffer = server.buffers[i];
        buffer.buffer = info.buffer;
        buffer.start = info.start;
        buffer.end = info.end;
        buffer.loop = info.loop;
        buffer.end_of_stream = info.end_of_stream;
        buffer.loop_start = info.loop_fields ? info.loop_start : info.start;
        buffer.loop_end = info.loop_fields ? info.loop_end : info.end;
        buffer.loop_count = info.loop_fields ? info.loop_count : info.loop ? -1 : 0;
        buffer.context = nullptr;
        if (info.context != nullptr)
        {
            server.contexts[i] = *info.context;
            buffer.context = &server.contexts[i];
        }
        server.sent[i] = false;
    }
}

// The splitters' update (SplitterContext::Update, 0x67aec): the splitters that changed, in the order of their ids,
// then the destinations that changed. A splitter's list comes apart, and its destinations link up in their new order
// (SplitterContext::UpdateInfo, 0x67c20). The last one keeps the link it had. A destination takes its mix and mix
// volumes. One that comes into use takes them as its previous mix volumes too (SplitterDestinationData::Update,
// 0x673e4). The next frame then doesn't ramp.
void Renderer::UpdateSplitters()
{
    for (Splitter& splitter : splitters_)
    {
        if (!splitter.changed)
        {
            continue;
        }

        for (int d = splitter.server_first; d >= 0;)
        {
            SplitterDestination& data = splitter_destinations_[static_cast<std::size_t>(d)];
            d = data.next;
            data.next = -1;
        }
        splitter.server_first = splitter.destinations.empty() ? -1 : splitter.destinations.front();
        for (std::size_t k = 1; k < splitter.destinations.size(); ++k)
        {
            splitter_destinations_[static_cast<std::size_t>(splitter.destinations[k - 1])].next =
                splitter.destinations[k];
        }
        splitter.changed = false;
    }

    for (SplitterDestination& data : splitter_destinations_)
    {
        if (!data.changed)
        {
            continue;
        }

        data.server_mix = data.mix;
        data.server_volumes = data.volumes;
        if (!data.server_in_use && data.in_use)
        {
            data.previous_volumes = data.volumes;
            data.mixed = false;
        }
        data.server_in_use = data.in_use;
        data.changed = false;
    }
}

int Renderer::ServerDestination(int splitter, int index) const
{
    if (splitter < 0 || static_cast<std::size_t>(splitter) >= splitters_.size())
    {
        return -1;
    }

    int d = splitters_[static_cast<std::size_t>(splitter)].server_first;
    for (int k = 0; k < index && d >= 0; ++k)
    {
        d = splitter_destinations_[static_cast<std::size_t>(d)].next;
    }

    return d;
}

// The destinations from place `first` down the list, `step` places apart: a channel's step through the list by the
// voice's channel count, or every destination for the depop. A destination counts while it's in use with a mix.
std::vector<std::pair<Renderer::SplitterDestination*, const Renderer::Mix*>> Renderer::SplitterMixes(
    const Voice::Server& server, int first, int step)
{
    std::vector<std::pair<SplitterDestination*, const Mix*>> mixes;
    const int stride = std::max(step, 1);
    const auto limit = static_cast<int>(splitter_destinations_.size());
    for (int index = first; index < limit; index += stride)
    {
        const int d = ServerDestination(server.splitter, index);
        if (d < 0)
        {
            break;
        }

        SplitterDestination& data = splitter_destinations_[static_cast<std::size_t>(d)];
        const Mix* mix = data.server_in_use ? MixInUse(data.server_mix) : nullptr;
        if (mix != nullptr)
        {
            mixes.emplace_back(&data, mix);
        }
    }

    return mixes;
}

// After the voices' commands, each destination in a splitter's list that a voice mixed takes its mix volumes as its
// previous ones (SplitterContext::UpdateInternalState, 0x67ef8, after CommandGenerator::GenerateVoiceCommands).
void Renderer::FinishSplitters()
{
    for (const Splitter& splitter : splitters_)
    {
        std::size_t steps = 0;
        for (int d = splitter.server_first; d >= 0 && steps < splitter_destinations_.size(); ++steps)
        {
            SplitterDestination& data = splitter_destinations_[static_cast<std::size_t>(d)];
            if (data.server_in_use && data.mixed)
            {
                data.previous_volumes = data.server_volumes;
            }
            data.mixed = false;
            d = data.next;
        }
    }
}

// The renderer's mixes in the order that the renderer renders them: the furthest from the final mix first, and the
// final mix last. Each mix takes the next range of mix buffers, from the first buffer on.
void Renderer::LayOutMixes()
{
    const int count = static_cast<int>(mixes_.size());
    std::vector<int> distance(mixes_.size(), -1);
    for (int id = 0; id < count; ++id)
    {
        int at = id;
        int steps = 0;
        while (at != kFinalMix && steps <= count && MixInUse(at) != nullptr)
        {
            at = mixes_[static_cast<std::size_t>(at)].destination;
            ++steps;
        }
        distance[static_cast<std::size_t>(id)] = at == kFinalMix && steps <= count ? steps : -1;
    }

    std::vector<int> order(mixes_.size());
    std::iota(order.begin(), order.end(), 0);
    const auto farther = [&](int a, int b)
    {
        return distance[static_cast<std::size_t>(a)] > distance[static_cast<std::size_t>(b)];
    };
    std::stable_sort(order.begin(), order.end(), farther);
    int offset = 0;
    for (const int id : order)
    {
        Mix& mix = mixes_[static_cast<std::size_t>(id)];
        if (mix.in_use)
        {
            mix.offset = offset;
            offset += mix.buffer_count;
        }
    }
    mixes_changed_ = false;
}

void Renderer::Generate()
{
    if (mixes_changed_)
    {
        LayOutMixes();
    }
    if (!mixing_)
    {
        GenerateWithoutMixing();
        return;
    }
    std::fill(buffers_.begin(), buffers_.end(), 0);
    std::fill(meter_sums_.begin(), meter_sums_.end(), 0);
    std::fill(float_buffers_.begin(), float_buffers_.end(), 0.0f);
    std::fill(float_meter_sums_.begin(), float_meter_sums_.end(), 0.0f);

    // The voices, the least important first, and among equals the oldest first.
    std::vector<int> sorted;
    for (std::size_t slot = 0; slot < voices_.size(); ++slot)
    {
        if (voices_[slot].server.in_use)
        {
            sorted.push_back(static_cast<int>(slot));
        }
    }
    const auto renders_first = [&](int a, int b)
    {
        const Voice::Server& x = At(a).server;
        const Voice::Server& y = At(b).server;
        if (x.priority != y.priority)
        {
            return x.priority > y.priority;
        }

        return x.sort_order > y.sort_order;
    };
    std::stable_sort(sorted.begin(), sorted.end(), renders_first);
    for (const int index : sorted)
    {
        RenderVoice(At(index));
    }
    FinishSplitters();

    MeasureMeters();

    // The sub-mixes in their order, then the final mix and the sink.
    std::vector<int> mix_order;
    for (std::size_t id = 1; id < mixes_.size(); ++id)
    {
        if (mixes_[id].in_use)
        {
            mix_order.push_back(static_cast<int>(id));
        }
    }
    const auto lower_offset = [&](int a, int b)
    {
        return mixes_[static_cast<std::size_t>(a)].offset < mixes_[static_cast<std::size_t>(b)].offset;
    };
    std::stable_sort(mix_order.begin(), mix_order.end(), lower_offset);
    for (const int id : mix_order)
    {
        RenderSubMix(id);
    }
    RenderFinalMix();
    Output();
    FinishRendererEffects();
}

// A frame without mixing (see the class's comment): each voice moves on, each mix's depop fades, and the meters and the
// output are silent. Nothing reads the mix buffers until a mixed frame clears them.
void Renderer::GenerateWithoutMixing()
{
    for (Voice& voice : voices_)
    {
        if (voice.server.in_use)
        {
            RenderVoice(voice);
        }
    }
    FinishSplitters();

    for (const Mix& mix : mixes_)
    {
        if (mix.in_use)
        {
            Depop(mix);
        }
    }

    StartRendererEffects();
    FinishRendererEffects();

    std::fill(meter_peaks_.begin(), meter_peaks_.end(), 0);
    for (std::vector<int32_t>& peaks : mix_peaks_)
    {
        std::fill(peaks.begin(), peaks.end(), 0);
    }
    std::fill(output_.begin(), output_.end(), 0);
    std::fill(float_output_.begin(), float_output_.end(), 0.0f);
}

// The peaks of the meters' sums.
void Renderer::MeasureMeters()
{
    const auto count = static_cast<std::size_t>(parameter_.sample_count);
    for (std::size_t i = 0; i < meter_peaks_.size(); ++i)
    {
        meter_peaks_[i] =
            parameter_.enhanced ? PeakOf(&float_meter_sums_[i * count], count) : PeakOf(&meter_sums_[i * count], count);
    }
}

// The device sink's channels, interleaved: held to 16 bits, and in the enhanced mode in floating point too.
void Renderer::Output()
{
    const Mix& final_mix = mixes_[kFinalMix];
    const std::size_t channels = sinks_.size();
    for (std::size_t k = 0; k < channels; ++k)
    {
        if (parameter_.enhanced)
        {
            const float* samples = FloatBuffer(final_mix.offset + sinks_[k]);
            for (int t = 0; t < parameter_.sample_count; ++t)
            {
                const float x = samples[t];
                const std::size_t at = static_cast<std::size_t>(t) * channels + k;
                float_output_[at] = x * (1.0f / 32768.0f);
                output_[at] = static_cast<int16_t>(std::clamp(RoundToInt32(x), -32768, 32767));
            }
            continue;
        }

        const int32_t* samples = Buffer(final_mix.offset + sinks_[k]);
        std::span<const int32_t> frame(samples, static_cast<std::size_t>(parameter_.sample_count));
        std::vector<int32_t> upsampled;
        if (!upsamplers_.empty())
        {
            upsampled.resize(frame.size() * 3 / 2);
            UpsampleFrame(upsamplers_[k], frame, upsampled);
            frame = upsampled;
        }
        for (std::size_t t = 0; t < frame.size(); ++t)
        {
            output_[t * channels + k] = static_cast<int16_t>(std::clamp(frame[t], -32768, 32767));
        }
    }
}

void Renderer::RenderVoice(Voice& voice)
{
    Voice::Server& server = voice.server;
    const auto report = [&]()
    {
        for (int c = 0; c < server.channel_count; ++c)
        {
            ChannelResource& resource = Resource(voice, c);
            resource.reported = resource.dsp;
        }
    };

    // A voice that isn't in use, has no buffers, or was dropped, is skipped (VoiceInfo::ShouldSkip).
    if (server.buffer_count == 0)
    {
        report();
        return;
    }

    if (server.is_new)
    {
        StartNewVoice(voice);
    }
    const bool render = UpdatePlayState(voice);

    // The renderer reports the states as the generation leaves them, before this frame's processing.
    report();
    if (!render)
    {
        return;
    }

    const Mix* destination = MixInUse(server.mix);
    if (!mixing_)
    {
        AdvanceVoice(voice, destination);
    }
    else if (parameter_.enhanced)
    {
        RenderVoiceEnhanced(voice, destination);
    }
    else
    {
        RenderVoiceConsole(voice, destination);
    }
}

// A new voice starts from cleared states, at its volumes (VoiceInfo::UpdateForCommandGeneration, 0x6b480).
void Renderer::StartNewVoice(Voice& voice)
{
    Voice::Server& server = voice.server;
    for (int c = 0; c < server.channel_count; ++c)
    {
        ChannelResource& resource = Resource(voice, c);
        resource.dsp = VoiceState{};
        resource.previous_mix = resource.mix;
        if (parameter_.enhanced)
        {
            enhanced_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])] = EnhancedChannel{};
        }
    }
    server.previous_volume = server.volume;
    server.previous_muted = server.muted;
    server.is_new = false;
}

// The play state (VoiceInfo::UpdateParametersForCommandGeneration, 0x6ad00): a started voice takes its new buffers,
// and plays if it has any; a voice that stops gives back every buffer and starts again from nothing. A voice that was
// playing and stops or pauses has a frame of depop. Returns whether the voice renders in this frame.
bool Renderer::UpdatePlayState(Voice& voice)
{
    Voice::Server& server = voice.server;
    const int channels = server.channel_count;
    const auto state = [&](int c) -> VoiceState&
    {
        return Resource(voice, c).dsp;
    };

    if (server.state == 0)
    {
        for (std::size_t i = 0; i < kWaveBufferCount; ++i)
        {
            if (!server.sent[i])
            {
                for (int c = 0; c < channels; ++c)
                {
                    state(c).valid[i] = true;
                }
                server.sent[i] = true;
            }
        }
        server.stopping = false;

        return std::any_of(state(0).valid.begin(), state(0).valid.end(), [](bool v) { return v; });
    }

    if (server.state == 2)
    {
        for (std::size_t i = 0; i < kWaveBufferCount; ++i)
        {
            server.sent[i] = true;
            for (int c = 0; c < channels; ++c)
            {
                VoiceState& s = state(c);
                if (s.valid[i])
                {
                    s.index = (s.index + 1) % kWaveBufferCount;
                    ++s.consumed;
                    s.valid[i] = false;
                }
            }
        }
        for (int c = 0; c < channels; ++c)
        {
            VoiceState& s = state(c);
            s.offset = 0;
            s.played = 0;
            s.adpcm = {};
            s.fraction = 0;
            s.history = {};
            if (parameter_.enhanced)
            {
                enhanced_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)])].history = {};
            }
        }
        server.state = 1;
    }
    server.stopping = server.previous == 0;

    return server.stopping;
}

// The console's processing of a voice that renders: each channel's data source, biquads, volume and mix volumes, or its
// depop preparation when the voice stops.
void Renderer::RenderVoiceConsole(Voice& voice, const Mix* destination)
{
    Voice::Server& server = voice.server;
    const int count = parameter_.sample_count;
    for (int c = 0; c < server.channel_count; ++c)
    {
        ChannelResource& resource = Resource(voice, c);
        VoiceState& s = resource.dsp;
        int32_t* work = Buffer(parameter_.mix_buffer_count + c);

        // A voice that stops leaves its last samples to fade out.
        if (server.stopping)
        {
            LeaveToDepop(server, s, destination);
            server.previous_volume = 0.0f;
            continue;
        }

        // The data source.
        DecodeFromWaveBuffers(DecodeArgs(voice, s, c, work));
        if (destination == nullptr && server.splitter == kNoSplitter)
        {
            continue;
        }

        // The biquads, in floating point, a filter's state cleared when it comes on.
        for (std::size_t i = 0; i < 2; ++i)
        {
            const BiquadFilterParameter& filter = server.biquads[i];
            if (!filter.enabled)
            {
                continue;
            }

            if (!server.biquads_were_enabled[i])
            {
                s.biquads[i] = {};
            }
            std::vector<float> samples(static_cast<std::size_t>(count));
            ConvertToFloat(samples.data(), work, count);
            ApplyBiquadFilter(samples.data(), samples.data(), filter.b, filter.a, s.biquads[i], count);
            ConvertToInt(work, samples.data(), count);
        }

        // The volume, from the last frame's volume. The host's mute takes it to 0.
        const float last_volume = server.previous_muted ? 0.0f : server.previous_volume;
        const float volume = server.muted ? 0.0f : server.volume;
        const int32_t from = ToFixed(last_volume, kGainBits);
        const int32_t to = last_volume == volume ? from : ToFixed(volume, kGainBits);
        ApplyLinearEnvelopeGain(work, work, from, RampStep(from, to, count), count, kGainBits);

        // The mix volumes, from the last frame's volumes, into the destination, or into each of the splitter's
        // destinations in turn.
        if (destination != nullptr)
        {
            MixVoiceChannel(server, s, work, *destination, server.mix, resource.previous_mix, resource.mix);
            resource.previous_mix = resource.mix;
        }
        else
        {
            for (const auto& [data, mix] : SplitterMixes(server, c, server.channel_count))
            {
                MixVoiceChannel(server, s, work, *mix, data->server_mix, data->previous_volumes, data->server_volumes);
                data->mixed = true;
            }
        }

        // The voice takes its values for the next frame with each channel.
        KeepFrameValues(server);
    }
}

// The depop preparation of a voice channel that stops: its last samples go to the depop of its destination's buffers,
// to fade out. A splitter's destinations share the channel's last samples. The first destination that has a buffer
// takes its last sample.
void Renderer::LeaveToDepop(const Voice::Server& server, VoiceState& state, const Mix* destination)
{
    const auto prepare = [&](const Mix& mix)
    {
        for (int b = 0; b < mix.buffer_count; ++b)
        {
            int32_t& last = state.last_samples[static_cast<std::size_t>(b)];
            if (last != 0)
            {
                int32_t& depop = depop_[static_cast<std::size_t>(mix.offset + b)];
                depop = WrapAdd(depop, last);
                last = 0;
            }
        }
    };

    if (destination != nullptr)
    {
        prepare(*destination);
        return;
    }

    for (const auto& target : SplitterMixes(server, 0, 1))
    {
        prepare(*target.second);
    }
}

// The data source command's arguments for channel `channel` of a voice, with its state `state`, into `out`
// (GenerateDataSourceCommand, 0x5c0c0).
DecodeFromWaveBuffersArgs Renderer::DecodeArgs(const Voice& voice, VoiceState& state, int channel, int32_t* out) const
{
    const Voice::Server& server = voice.server;
    DecodeFromWaveBuffersArgs args;
    args.format = server.format;
    args.out = out;
    args.state = &state;
    args.buffers = &server.buffers;
    args.channel = channel;
    args.channel_count = server.channel_count;
    args.quality = server.quality;
    args.pitch = ToFixed(server.pitch, 15);
    args.source_rate = server.sample_rate;
    args.target_rate = parameter_.sample_rate;
    args.count = parameter_.sample_count;
    args.coefficients = &voice.coefficients;
    args.reset_played_at_loop = server.behavior.reset_played_at_loop;

    return args;
}

// One command for a destination of more than 6 buffers, else one for each buffer that isn't silent. The one command
// gives a silent buffer a last sample of 0. Each buffer's last sample goes to the channel's last samples at the
// buffer's place in the mix.
void Renderer::MixVoiceChannel(const Voice::Server& server, VoiceState& state, const int32_t* work, const Mix& mix,
                               int mix_id, const std::array<float, kMaxMixBuffers>& from,
                               const std::array<float, kMaxMixBuffers>& to)
{
    const int count = parameter_.sample_count;
    const bool grouped = mix.buffer_count > 6;
    for (int b = 0; b < mix.buffer_count; ++b)
    {
        const float before = from[static_cast<std::size_t>(b)];
        const float now = to[static_cast<std::size_t>(b)];
        int32_t& last = state.last_samples[static_cast<std::size_t>(b)];
        if (!grouped && before == 0.0f && now == 0.0f)
        {
            continue;
        }

        const int32_t start = ToFixed(before, kGainBits);
        const int32_t end = before == now ? start : ToFixed(now, kGainBits);
        if (grouped && start == 0 && end == 0)
        {
            last = 0;
            continue;
        }

        last = ApplyMixRamp(Buffer(mix.offset + b), work, start, RampStep(start, end, count), count, kGainBits);
        AddToMeter(server, mix_id, b, work, start, end);
    }
}

// A voice's frame without mixing (see the class's comment). Its channels move on through their buffers, and the
// voice's values for the next frame are those that a mixed frame leaves. A filter that comes on still starts from a
// cleared state. The voice's last products are 0, and a voice that stops leaves the depop nothing.
void Renderer::AdvanceVoice(Voice& voice, const Mix* destination)
{
    Voice::Server& server = voice.server;
    for (int c = 0; c < server.channel_count; ++c)
    {
        const auto r = static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)]);
        ChannelResource& resource = resources_[r];
        VoiceState& s = resource.dsp;
        s.last_samples = {};
        if (parameter_.enhanced)
        {
            enhanced_[r].last = {};
        }
        if (server.stopping)
        {
            continue;
        }

        const DecodeFromWaveBuffersArgs args = DecodeArgs(voice, s, c, nullptr);
        if (parameter_.enhanced)
        {
            DecodeFromWaveBuffersEnhanced(args, {enhanced_[r].history, &decoded_, &input_}, nullptr);
        }
        else
        {
            DecodeFromWaveBuffers(args);
        }
        if (destination == nullptr && server.splitter == kNoSplitter)
        {
            continue;
        }

        for (std::size_t i = 0; i < 2; ++i)
        {
            if (server.biquads[i].enabled && !server.biquads_were_enabled[i])
            {
                s.biquads[i] = {};
            }
        }
        resource.previous_mix = resource.mix;
        for (const auto& target : SplitterMixes(server, c, server.channel_count))
        {
            target.first->mixed = true;
        }

        // In the console's mode, the voice takes its values for the next frame with each channel.
        if (!parameter_.enhanced)
        {
            KeepFrameValues(server);
        }
    }

    if (server.stopping)
    {
        server.previous_volume = 0.0f;
    }
    else if (destination != nullptr && parameter_.enhanced)
    {
        KeepFrameValues(server);
    }
}

void Renderer::KeepFrameValues(Voice::Server& server)
{
    server.previous_volume = server.volume;
    server.previous_muted = server.muted;
    server.biquads_were_enabled = {server.biquads[0].enabled, server.biquads[1].enabled};
}

std::optional<std::size_t> Renderer::MeterSum(const Voice::Server& server, int mix, int buffer) const
{
    if (server.meter < 0 || server.meter >= meter_count_ || mix != meter_mix_ ||
        buffer >= static_cast<int>(meter_channels_.size()) || meter_channels_[static_cast<std::size_t>(buffer)] < 0)
    {
        return std::nullopt;
    }

    const std::size_t sum = static_cast<std::size_t>(server.meter) * static_cast<std::size_t>(meter_channel_count_) +
                            static_cast<std::size_t>(meter_channels_[static_cast<std::size_t>(buffer)]);

    return sum * static_cast<std::size_t>(parameter_.sample_count);
}

// A metered voice's output to buffer `buffer` of the metered mix goes to its meter's channel too, at the same gains.
void Renderer::AddToMeter(const Voice::Server& server, int mix, int buffer, const int32_t* work, int32_t start,
                          int32_t end)
{
    const std::optional<std::size_t> sum = MeterSum(server, mix, buffer);
    if (!sum)
    {
        return;
    }

    const int count = parameter_.sample_count;
    ApplyMixRamp(&meter_sums_[*sum], work, start, RampStep(start, end, count), count, kGainBits);
}

// The enhanced mode's processing of a voice that renders (see the class's comment).
void Renderer::RenderVoiceEnhanced(Voice& voice, const Mix* destination)
{
    Voice::Server& server = voice.server;
    const int count = parameter_.sample_count;
    const float last_volume = server.previous_muted ? 0.0f : server.previous_volume;
    const float volume = server.muted ? 0.0f : server.volume;
    for (int c = 0; c < server.channel_count; ++c)
    {
        const auto r = static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(c)]);
        ChannelResource& resource = resources_[r];
        VoiceState& s = resource.dsp;
        EnhancedChannel& channel = enhanced_[r];
        float* work = FloatBuffer(parameter_.mix_buffer_count + c);

        // A voice that stops leaves its last products to fade out.
        if (server.stopping)
        {
            for (int b = 0; destination != nullptr && b < destination->buffer_count; ++b)
            {
                float& last = channel.last[static_cast<std::size_t>(b)];
                float_depop_[static_cast<std::size_t>(destination->offset + b)] += last;
                last = 0.0f;
            }
            continue;
        }

        // The data source.
        DecodeFromWaveBuffersEnhanced(DecodeArgs(voice, s, c, nullptr), {channel.history, &decoded_, &input_}, work);
        if (destination == nullptr)
        {
            continue;
        }

        // The biquads, a filter's state cleared when it comes on.
        for (std::size_t i = 0; i < 2; ++i)
        {
            const BiquadFilterParameter& filter = server.biquads[i];
            if (!filter.enabled)
            {
                continue;
            }

            if (!server.biquads_were_enabled[i])
            {
                s.biquads[i] = {};
            }
            ApplyBiquadFilter(work, work, filter.b, filter.a, s.biquads[i], count);
        }

        // The volume, then each mix volume, from the last frame's. A buffer that the voice doesn't reach keeps no last
        // product.
        Ramp(work, work, last_volume, volume, count);
        for (int b = 0; b < destination->buffer_count; ++b)
        {
            const float before = resource.previous_mix[static_cast<std::size_t>(b)];
            const float now = resource.mix[static_cast<std::size_t>(b)];
            float& last = channel.last[static_cast<std::size_t>(b)];
            if (before == 0.0f && now == 0.0f)
            {
                last = 0.0f;
                continue;
            }

            last = MixRamp(FloatBuffer(destination->offset + b), work, before, now, count);
            AddToMeterEnhanced(server, b, work, before, now);
        }
        resource.previous_mix = resource.mix;
    }

    // The voice's values for the next frame.
    if (server.stopping)
    {
        server.previous_volume = 0.0f;
    }
    else if (destination != nullptr)
    {
        KeepFrameValues(server);
    }
}

// The enhanced mode's AddToMeter.
void Renderer::AddToMeterEnhanced(const Voice::Server& server, int buffer, const float* work, float start, float end)
{
    const std::optional<std::size_t> sum = MeterSum(server, server.mix, buffer);
    if (!sum)
    {
        return;
    }

    MixRamp(&float_meter_sums_[*sum], work, start, end, parameter_.sample_count);
}

// The depop of a mix's buffers: each buffer's step fades out across the frames.
void Renderer::Depop(const Mix& mix)
{
    if (parameter_.enhanced)
    {
        const float decay = mix.sample_rate == 48000 ? kFloatDepopDecay48k : kFloatDepopDecay;
        for (int b = 0; b < mix.buffer_count; ++b)
        {
            float& depop = float_depop_[static_cast<std::size_t>(mix.offset + b)];
            if (depop == 0.0f)
            {
                continue;
            }

            float* samples = FloatBuffer(mix.offset + b);
            for (int t = 0; t < parameter_.sample_count; ++t)
            {
                depop *= decay;
                samples[t] += depop;
            }
            if (std::fabs(depop) < kFloatDepopFloor)
            {
                depop = 0.0f;
            }
        }
        return;
    }

    const int32_t decay = mix.sample_rate == 48000 ? kDepopDecay48k : kDepopDecay;
    for (int b = 0; b < mix.buffer_count; ++b)
    {
        int32_t& depop = depop_[static_cast<std::size_t>(mix.offset + b)];
        if (depop != 0)
        {
            depop = ApplyDepopMix(Buffer(mix.offset + b), depop, decay, parameter_.sample_count);
        }
    }
}

void Renderer::RenderSubMix(int id)
{
    const Mix& mix = mixes_[static_cast<std::size_t>(id)];
    Depop(mix);
    RenderEffects(mix);
    MeasureMix(id);

    const Mix* destination = MixInUse(mix.destination);
    if (destination == nullptr)
    {
        return;
    }

    for (int i = 0; i < mix.buffer_count; ++i)
    {
        for (int j = 0; j < destination->buffer_count; ++j)
        {
            const float gain = mix.volume * mix.mix_volumes[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            if (gain != 0.0f && parameter_.enhanced)
            {
                float* out = FloatBuffer(destination->offset + j);
                const float* in = FloatBuffer(mix.offset + i);
                for (int t = 0; t < parameter_.sample_count; ++t)
                {
                    out[t] += in[t] * gain;
                }
            }
            else if (gain != 0.0f)
            {
                ApplyMix(Buffer(destination->offset + j), Buffer(mix.offset + i), ToFixed(gain, kGainBits),
                         parameter_.sample_count, kGainBits);
            }
        }
    }
}

void Renderer::RenderFinalMix()
{
    const Mix& mix = mixes_[kFinalMix];
    if (!mix.in_use)
    {
        return;
    }

    Depop(mix);
    RenderEffects(mix);
    MeasureMix(kFinalMix);
    if (parameter_.enhanced)
    {
        for (int b = 0; b < mix.buffer_count; ++b)
        {
            float* samples = FloatBuffer(mix.offset + b);
            for (int t = 0; t < parameter_.sample_count; ++t)
            {
                samples[t] *= mix.volume;
            }
        }
        return;
    }

    const int32_t gain = ToFixed(mix.volume, kGainBits);
    for (int b = 0; b < mix.buffer_count; ++b)
    {
        int32_t* samples = Buffer(mix.offset + b);
        ApplyUniformGain(samples, samples, gain, parameter_.sample_count, kGainBits);
    }
}

// The largest magnitude of each of the mix's buffers in the frame, for a host that has the mix's peaks on.
void Renderer::MeasureMix(int id)
{
    if (static_cast<std::size_t>(id) >= mix_peaks_.size() || mix_peaks_[static_cast<std::size_t>(id)].empty())
    {
        return;
    }

    const Mix& mix = mixes_[static_cast<std::size_t>(id)];
    std::vector<int32_t>& peaks = mix_peaks_[static_cast<std::size_t>(id)];
    const auto count = static_cast<std::size_t>(parameter_.sample_count);
    for (int b = 0; b < mix.buffer_count && b < static_cast<int>(peaks.size()); ++b)
    {
        const int buffer = mix.offset + b;
        peaks[static_cast<std::size_t>(b)] =
            parameter_.enhanced ? PeakOf(FloatBuffer(buffer), count) : PeakOf(Buffer(buffer), count);
    }
}

} // namespace ntdlib::nx::audio
