// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/nx/audio/voice.h"
#include "ntdlib/rounding.h"
#include "nx/audio/arithmetic.h"
#include "nx/audio/renderer_frame_internal.h"

namespace ntdlib::nx::audio
{

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

} // namespace ntdlib::nx::audio
