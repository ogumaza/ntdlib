// SPDX-License-Identifier: MIT

// The renderer's commands in a frame (see renderer.h): each voice's decoding and mix into its destination, in the
// console's mode and in the enhanced mode, the depop, the sub-mixes and the final mix, and their meters.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/renderer.h"
#include "ntdlib/nx/audio/voice.h"
#include "nx/audio/arithmetic.h"
#include "nx/audio/renderer_frame_internal.h"

namespace ntdlib::nx::audio
{

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
