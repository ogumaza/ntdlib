// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"
#include "ntdlib/nx/audio/voice.h"
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
constexpr float kFloatDepopDecay48k = 31529.0f / 32768.0f;
constexpr float kFloatDepopDecay = 30923.0f / 32768.0f;
constexpr float kFloatDepopFloor = 1.0f / 65536.0f;

// The step of a ramp across a frame: the difference of the gains, which wraps, over the sample count.
int32_t RampStep(int32_t from, int32_t to, int count)
{
    return WrapSub(to, from) / count;
}

// The bytes a DSP-ADPCM range up to `sample` takes, frames of 8 bytes for 14 samples.
int64_t AdpcmBytes(int32_t sample)
{
    const int64_t frame = sample / 14;
    const int64_t in_frame = sample % 14;
    return 8 * frame + (in_frame == 0 ? 0 : in_frame / 2 + 1 + in_frame % 2);
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

// A sample of the enhanced mode for an aux's send ring: to the nearest integer, halves away from zero, held to 32 bits.
int32_t RingSample(float x)
{
    return static_cast<int32_t>(std::clamp(std::round(static_cast<double>(x)), -2147483648.0, 2147483647.0));
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

} // namespace

Renderer::Renderer(const RendererParameter& parameter)
    : parameter_(parameter),
      voices_(static_cast<std::size_t>(parameter.voice_count)),
      resources_(static_cast<std::size_t>(parameter.voice_count)),
      mixes_(static_cast<std::size_t>(parameter.sub_mix_count) + 1),
      buffers_(static_cast<std::size_t>(parameter.mix_buffer_count + kMaxChannels) *
               static_cast<std::size_t>(parameter.sample_count)),
      depop_(static_cast<std::size_t>(parameter.mix_buffer_count))
{
    mixes_[kFinalMix].sample_rate = parameter.sample_rate;
    if (parameter.enhanced)
    {
        enhanced_.resize(resources_.size());
        float_buffers_.resize(buffers_.size());
        float_depop_.resize(depop_.size());
    }
}

void Renderer::SetObserver(RendererObserver* observer)
{
    observer_ = observer;
    if (observer_ != nullptr)
    {
        observer_->OpenAudioRenderer(parameter_);
    }
}

Renderer::Voice& Renderer::At(int voice)
{
    return voices_.at(static_cast<std::size_t>(voice));
}

const Renderer::Voice& Renderer::At(int voice) const
{
    return voices_.at(static_cast<std::size_t>(voice));
}

// The client keeps its voices in use by priority, 0 first, a new voice before the others of its priority.
void Renderer::Insert(int voice)
{
    const int32_t priority = At(voice).priority;
    const auto at =
        std::find_if(order_.begin(), order_.end(), [&](int other) { return At(other).priority >= priority; });
    order_.insert(at, voice);
}

void Renderer::Remove(int voice)
{
    order_.erase(std::remove(order_.begin(), order_.end(), voice), order_.end());
}

Renderer::ChannelResource& Renderer::Resource(const Voice& voice, int channel)
{
    return resources_[static_cast<std::size_t>(voice.resources[static_cast<std::size_t>(channel)])];
}

const Renderer::Mix* Renderer::MixInUse(int id) const
{
    if (id < 0 || id >= static_cast<int>(mixes_.size()) || !mixes_[static_cast<std::size_t>(id)].in_use)
    {
        return nullptr;
    }

    return &mixes_[static_cast<std::size_t>(id)];
}

int Renderer::AcquireVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                               const AdpcmCoefficients* coefficients)
{
    const int voice = TakeVoiceSlot(sample_rate, channel_count, format, priority, coefficients);
    if (observer_ != nullptr)
    {
        observer_->AcquireVoiceSlot(sample_rate, channel_count, format, priority, coefficients, voice);
    }

    return voice;
}

int Renderer::TakeVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                            const AdpcmCoefficients* coefficients)
{
    // A free slot, and the first free channel resources, one for each channel.
    const auto slot = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.in_use; });
    std::vector<int> free;
    for (std::size_t r = 0; r < resources_.size() && static_cast<int>(free.size()) < channel_count; ++r)
    {
        if (!resources_[r].in_use)
        {
            free.push_back(static_cast<int>(r));
        }
    }
    if (slot == voices_.end() || channel_count < 1 || channel_count > kMaxChannels ||
        static_cast<int>(free.size()) < channel_count)
    {
        return -1;
    }

    Voice& voice = *slot;
    voice = Voice{};
    voice.in_use = true;
    voice.is_new = true;
    voice.format = format;
    voice.sample_rate = sample_rate;
    voice.priority = priority;
    voice.channel_count = channel_count;
    if (coefficients != nullptr)
    {
        voice.coefficients = *coefficients;
    }
    for (int c = 0; c < channel_count; ++c)
    {
        ChannelResource& resource = resources_[static_cast<std::size_t>(free[static_cast<std::size_t>(c)])];
        resource.in_use = true;
        resource.client_mix = {};
        voice.resources[static_cast<std::size_t>(c)] = free[static_cast<std::size_t>(c)];
        resource_mark_ = std::max(resource_mark_, free[static_cast<std::size_t>(c)] + 1);
    }

    const int index = static_cast<int>(slot - voices_.begin());
    voice_mark_ = std::max(voice_mark_, index + 1);
    Insert(index);

    return index;
}

void Renderer::ReleaseVoiceSlot(int voice)
{
    Voice& v = At(voice);
    for (int c = 0; c < v.channel_count; ++c)
    {
        ChannelResource& resource = Resource(v, c);
        resource.in_use = false;
        resource.client_mix = {};
    }
    v.in_use = false;
    Remove(voice);
    if (observer_ != nullptr)
    {
        observer_->ReleaseVoiceSlot(voice);
    }
}

void Renderer::SetVoicePlayState(int voice, PlayState state)
{
    At(voice).play_state = state;
    if (observer_ != nullptr)
    {
        observer_->SetVoicePlayState(voice, state);
    }
}

PlayState Renderer::GetVoicePlayState(int voice) const
{
    const PlayState state = At(voice).play_state;
    if (observer_ != nullptr)
    {
        observer_->GetVoicePlayState(voice, state);
    }

    return state;
}

void Renderer::SetVoicePitch(int voice, float pitch)
{
    At(voice).pitch = pitch;
    if (observer_ != nullptr)
    {
        observer_->SetVoicePitch(voice, pitch);
    }
}

void Renderer::SetVoiceVolume(int voice, float volume)
{
    At(voice).volume = volume;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceVolume(voice, volume);
    }
}

void Renderer::SetVoicePriority(int voice, int32_t priority)
{
    At(voice).priority = priority;
    Remove(voice);
    Insert(voice);
    if (observer_ != nullptr)
    {
        observer_->SetVoicePriority(voice, priority);
    }
}

void Renderer::SetVoiceSrcQuality(int voice, SrcQuality quality)
{
    At(voice).quality = quality;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceSrcQuality(voice, quality);
    }
}

void Renderer::SetVoiceBiquadFilterParameter(int voice, int index, const BiquadFilterParameter& parameter)
{
    At(voice).biquads.at(static_cast<std::size_t>(index)) = parameter;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceBiquadFilterParameter(voice, index, parameter);
    }
}

void Renderer::SetVoiceDestination(int voice, int mix)
{
    At(voice).mix = mix;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceDestination(voice, mix);
    }
}

void Renderer::SetVoiceMixVolume(int voice, float volume, int channel, int buffer)
{
    const int resource = At(voice).resources.at(static_cast<std::size_t>(channel));
    resources_[static_cast<std::size_t>(resource)].client_mix.at(static_cast<std::size_t>(buffer)) = volume;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceMixVolume(voice, volume, channel, buffer);
    }
}

void Renderer::SetVoiceMuted(int voice, bool muted)
{
    At(voice).muted = muted;
}

void Renderer::SetMeters(int count, int mix, std::span<const int> channels)
{
    // A host may set the same meters every frame. They then stay as they are.
    count = std::max(count, 0);
    if (count == meter_count_ && mix == meter_mix_ &&
        std::equal(channels.begin(), channels.end(), meter_channels_.begin(), meter_channels_.end()))
    {
        return;
    }

    meter_count_ = count;
    meter_mix_ = mix;
    meter_channels_.assign(channels.begin(), channels.end());
    meter_channel_count_ = channels.empty() ? 0 : std::max(*std::max_element(channels.begin(), channels.end()) + 1, 0);
    const std::size_t sums = static_cast<std::size_t>(meter_count_) * static_cast<std::size_t>(meter_channel_count_);
    meter_sums_.assign(sums * static_cast<std::size_t>(parameter_.sample_count), 0);
    meter_peaks_.assign(sums, 0);
    if (parameter_.enhanced)
    {
        float_meter_sums_.assign(meter_sums_.size(), 0.0f);
    }
}

void Renderer::SetVoiceMeter(int voice, int meter)
{
    At(voice).meter = meter;
}

std::span<const int32_t> Renderer::MeterPeaks(int meter) const
{
    if (meter < 0 || meter >= meter_count_)
    {
        throw std::out_of_range("the renderer has no such meter");
    }

    const auto channels = static_cast<std::size_t>(meter_channel_count_);

    return std::span<const int32_t>(meter_peaks_).subspan(static_cast<std::size_t>(meter) * channels, channels);
}

void Renderer::MeterMix(int mix, bool on)
{
    if (mix < 0 || mix >= static_cast<int>(mixes_.size()))
    {
        throw std::out_of_range("the renderer has no such mix");
    }

    mix_peaks_.resize(mixes_.size());
    std::vector<int32_t>& peaks = mix_peaks_[static_cast<std::size_t>(mix)];
    if (on)
    {
        peaks.assign(static_cast<std::size_t>(kMaxMixBuffers), 0);
    }
    else
    {
        peaks.clear();
    }
}

std::span<const int32_t> Renderer::MixPeaks(int mix) const
{
    if (mix < 0 || static_cast<std::size_t>(mix) >= mix_peaks_.size())
    {
        return {};
    }

    const std::vector<int32_t>& peaks = mix_peaks_[static_cast<std::size_t>(mix)];
    const auto buffers = static_cast<std::size_t>(std::max(mixes_[static_cast<std::size_t>(mix)].buffer_count, 0));

    return std::span<const int32_t>(peaks).first(std::min(peaks.size(), buffers));
}

bool Renderer::AppendWaveBuffer(int voice, const WaveBufferInfo& buffer)
{
    const bool appended = QueueWaveBuffer(voice, buffer);
    if (observer_ != nullptr)
    {
        observer_->AppendWaveBuffer(voice, buffer, appended);
    }

    return appended;
}

bool Renderer::QueueWaveBuffer(int voice, const WaveBufferInfo& buffer)
{
    Voice& v = At(voice);
    if (v.buffer_count >= kWaveBufferCount)
    {
        return false;
    }

    ClientWaveBuffer& slot = v.buffers[static_cast<std::size_t>((v.buffer_index + v.buffer_count) % kWaveBufferCount)];
    slot.info = buffer;
    slot.sent = false;
    ++v.buffer_count;

    return true;
}

const void* Renderer::GetReleasedWaveBuffer(int voice)
{
    const void* tag = TakeReleasedWaveBuffer(voice);
    if (observer_ != nullptr)
    {
        observer_->GetReleasedWaveBuffer(voice, tag);
    }

    return tag;
}

const void* Renderer::TakeReleasedWaveBuffer(int voice)
{
    Voice& v = At(voice);
    if (v.consumed == v.released || v.buffer_count == 0)
    {
        return nullptr;
    }

    const void* tag = v.buffers[static_cast<std::size_t>(v.buffer_index)].info.tag;
    v.buffer_index = (v.buffer_index + 1) % kWaveBufferCount;
    --v.buffer_count;
    ++v.released;

    return tag;
}

uint64_t Renderer::GetVoicePlayedSampleCount(int voice) const
{
    const uint64_t count = At(voice).played;
    if (observer_ != nullptr)
    {
        observer_->GetVoicePlayedSampleCount(voice, count);
    }

    return count;
}

bool Renderer::IsVoiceDroppedFlagOn(int voice) const
{
    const bool dropped = At(voice).dropped;
    if (observer_ != nullptr)
    {
        observer_->IsVoiceDroppedFlagOn(voice, dropped);
    }

    return dropped;
}

bool Renderer::AcquireFinalMix(int buffer_count)
{
    const bool acquired = TakeFinalMix(buffer_count);
    if (observer_ != nullptr)
    {
        observer_->AcquireFinalMix(buffer_count, acquired);
    }

    return acquired;
}

bool Renderer::TakeFinalMix(int buffer_count)
{
    Mix& mix = mixes_[kFinalMix];
    if (mix.in_use || buffer_count < 1 || buffer_count > kMaxMixBuffers)
    {
        return false;
    }

    mix.in_use = true;
    mix.buffer_count = buffer_count;
    mix.mix_volumes.assign(static_cast<std::size_t>(buffer_count), {});
    mixes_changed_ = true;

    return true;
}

int Renderer::AcquireSubMix(int32_t sample_rate, int buffer_count)
{
    const int sub_mix = TakeSubMix(sample_rate, buffer_count);
    if (observer_ != nullptr)
    {
        observer_->AcquireSubMix(sample_rate, buffer_count, sub_mix);
    }

    return sub_mix;
}

int Renderer::TakeSubMix(int32_t sample_rate, int buffer_count)
{
    for (std::size_t id = 1; id < mixes_.size(); ++id)
    {
        Mix& mix = mixes_[id];
        if (mix.in_use)
        {
            continue;
        }
        if (buffer_count < 1 || buffer_count > kMaxMixBuffers)
        {
            return kNoMix;
        }

        mix = Mix{};
        mix.in_use = true;
        mix.sample_rate = sample_rate;
        mix.buffer_count = buffer_count;
        mix.mix_volumes.assign(static_cast<std::size_t>(buffer_count), {});
        mixes_changed_ = true;

        return static_cast<int>(id);
    }

    return kNoMix;
}

void Renderer::SetSubMixDestination(int sub_mix, int destination)
{
    mixes_.at(static_cast<std::size_t>(sub_mix)).destination = destination;
    mixes_changed_ = true;
    if (observer_ != nullptr)
    {
        observer_->SetSubMixDestination(sub_mix, destination);
    }
}

void Renderer::SetSubMixMixVolume(int sub_mix, float volume, int source, int destination)
{
    Mix& mix = mixes_.at(static_cast<std::size_t>(sub_mix));
    mix.mix_volumes.at(static_cast<std::size_t>(source)).at(static_cast<std::size_t>(destination)) = volume;
    if (observer_ != nullptr)
    {
        observer_->SetSubMixMixVolume(sub_mix, volume, source, destination);
    }
}

void Renderer::AddDeviceSink(std::span<const int> inputs)
{
    sinks_.assign(inputs.begin(), inputs.end());
    output_.assign(static_cast<std::size_t>(parameter_.sample_count) * sinks_.size(), 0);
    if (parameter_.enhanced)
    {
        float_output_.assign(output_.size(), 0.0f);
    }
    if (observer_ != nullptr)
    {
        observer_->AddDeviceSink(inputs);
    }
}

uint32_t Renderer::GetRequiredBufferSizeForAuxSendReturnBuffer(const RendererParameter& parameter, int frames,
                                                               int channels)
{
    const int64_t samples = static_cast<int64_t>(channels * frames * parameter.sample_count);
    return static_cast<uint32_t>((samples * 4 + 0xBF) & ~int64_t{0x3F});
}

int Renderer::AddAux(int mix, uint32_t buffer_size)
{
    // The rings' room, after their headers, in samples and in frames of one channel. They go round nowhere until the
    // effect has its channels.
    Aux aux;
    aux.mix = mix;
    aux.enabled = true;
    const uint32_t bytes = buffer_size - 0x80;
    aux.capacity = bytes / 4;
    aux.capacity_frames = bytes / (static_cast<uint32_t>(parameter_.sample_count) * 4);
    aux.send.assign(aux.capacity, 0);
    aux.receive.assign(aux.capacity, 0);
    auxes_.push_back(std::move(aux));

    const int id = static_cast<int>(auxes_.size()) - 1;
    mixes_.at(static_cast<std::size_t>(mix)).effects.push_back(id);
    if (observer_ != nullptr)
    {
        observer_->AddAux(mix, buffer_size, id);
    }

    return id;
}

void Renderer::SetAuxInputOutput(int aux, std::span<const int8_t> inputs, std::span<const int8_t> outputs)
{
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const int most = mixes_.at(static_cast<std::size_t>(a.mix)).buffer_count;
    const int count = std::min(most, static_cast<int>(std::min(inputs.size(), outputs.size())));
    a.inputs.assign(inputs.begin(), inputs.begin() + std::max(count, 0));
    a.outputs.assign(outputs.begin(), outputs.begin() + std::max(count, 0));
    a.ring = count < 1 ? 0
                       : a.capacity_frames / static_cast<uint32_t>(count) * static_cast<uint32_t>(count) *
                             static_cast<uint32_t>(parameter_.sample_count);
    if (observer_ != nullptr)
    {
        observer_->SetAuxInputOutput(aux, inputs, outputs);
    }
}

void Renderer::SetAuxEnabled(int aux, bool enabled)
{
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    a.enabled = enabled;
    a.send_read = 0;
    a.receive_write = 0;
    if (observer_ != nullptr)
    {
        observer_->SetAuxEnabled(aux, enabled);
    }
}

int32_t Renderer::GetAuxSampleCount(int aux) const
{
    const auto count = static_cast<int32_t>(auxes_.at(static_cast<std::size_t>(aux)).capacity);
    if (observer_ != nullptr)
    {
        observer_->GetAuxSampleCount(aux, count);
    }

    return count;
}

int32_t Renderer::GetAuxSampleRate(int aux) const
{
    const int32_t rate = mixes_.at(static_cast<std::size_t>(auxes_.at(static_cast<std::size_t>(aux)).mix)).sample_rate;
    if (observer_ != nullptr)
    {
        observer_->GetAuxSampleRate(aux, rate);
    }

    return rate;
}

int32_t Renderer::ReadAuxSendBuffer(int aux, std::span<int32_t> samples)
{
    const int32_t count = ReadAuxSend(aux, samples);
    if (observer_ != nullptr)
    {
        observer_->ReadAuxSendBuffer(aux, samples, count);
    }

    return count;
}

int32_t Renderer::ReadAuxSend(int aux, std::span<int32_t> samples)
{
    // Everything the renderer has written since the last read, up to the span.
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const uint32_t ring = a.ring;
    if (ring == 0)
    {
        return 0;
    }

    const uint32_t written = (a.send_write < a.send_read ? ring : 0) - a.send_read + a.send_write;
    const uint32_t count = std::min(written, static_cast<uint32_t>(samples.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        samples[i] = a.send[(a.send_read + i) % ring];
    }
    a.send_read = (a.send_read + count) % ring;

    return static_cast<int32_t>(count);
}

int32_t Renderer::WriteAuxReturnBuffer(int aux, std::span<const int32_t> samples)
{
    const int32_t written = WriteAuxReturn(aux, samples);
    if (observer_ != nullptr)
    {
        observer_->WriteAuxReturnBuffer(aux, samples, written);
    }

    return written;
}

int32_t Renderer::WriteAuxReturn(int aux, std::span<const int32_t> samples)
{
    // As far as the renderer's next read, or a whole ring when the two are at the same place.
    Aux& a = auxes_.at(static_cast<std::size_t>(aux));
    const uint32_t ring = a.ring;
    if (ring == 0)
    {
        return 0;
    }

    const uint32_t room = (a.receive_read > a.receive_write ? 0 : ring) + a.receive_read - a.receive_write;
    const uint32_t count = std::min(room, static_cast<uint32_t>(samples.size()));
    for (uint32_t i = 0; i < count; ++i)
    {
        a.receive[(a.receive_write + i) % ring] = samples[i];
    }
    a.receive_write = (a.receive_write + count) % ring;

    return static_cast<int32_t>(count);
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
    for (int b = 0; b < mix.buffer_count && b < static_cast<int>(peaks.size()); ++b)
    {
        if (parameter_.enhanced)
        {
            peaks[static_cast<std::size_t>(b)] =
                PeakOf(FloatBuffer(mix.offset + b), static_cast<std::size_t>(parameter_.sample_count));
            continue;
        }

        const int32_t* samples = Buffer(mix.offset + b);
        int64_t peak = 0;
        for (int t = 0; t < parameter_.sample_count; ++t)
        {
            const int64_t x = samples[t];
            peak = std::max(peak, x < 0 ? -x : x);
        }
        peaks[static_cast<std::size_t>(b)] =
            static_cast<int32_t>(std::min<int64_t>(peak, std::numeric_limits<int32_t>::max()));
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

void Renderer::RenderFrame()
{
    Update();
    Generate();
    if (observer_ != nullptr)
    {
        observer_->RenderFrame();
    }
}

Renderer::Snapshot Renderer::Save() const
{
    Snapshot snapshot;
    snapshot.owner = this;
    snapshot.voices.assign(voices_.begin(), voices_.begin() + voice_mark_);
    snapshot.resources.assign(resources_.begin(), resources_.begin() + resource_mark_);
    snapshot.order = order_;
    snapshot.mixes = mixes_;
    snapshot.auxes = auxes_;
    snapshot.mixes_changed = mixes_changed_;
    snapshot.sinks = sinks_;
    snapshot.buffers = buffers_;
    snapshot.depop = depop_;
    snapshot.output = output_;
    if (parameter_.enhanced)
    {
        snapshot.enhanced.assign(enhanced_.begin(), enhanced_.begin() + resource_mark_);
        snapshot.float_depop = float_depop_;
        snapshot.float_output = float_output_;
    }

    return snapshot;
}

void Renderer::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this)
    {
        throw std::invalid_argument("a snapshot of another renderer");
    }

    // The voices keep their places. Their wave buffers' contexts point into them. Those past the snapshot's marks go
    // back to how they started.
    const auto voices = snapshot.voices.size();
    const auto resources = snapshot.resources.size();
    std::copy(snapshot.voices.begin(), snapshot.voices.end(), voices_.begin());
    for (std::size_t slot = voices; slot < static_cast<std::size_t>(voice_mark_); ++slot)
    {
        voices_[slot] = Voice{};
    }
    std::copy(snapshot.resources.begin(), snapshot.resources.end(), resources_.begin());
    for (std::size_t r = resources; r < static_cast<std::size_t>(resource_mark_); ++r)
    {
        resources_[r] = ChannelResource{};
    }
    if (parameter_.enhanced)
    {
        std::copy(snapshot.enhanced.begin(), snapshot.enhanced.end(), enhanced_.begin());
        for (std::size_t r = resources; r < static_cast<std::size_t>(resource_mark_); ++r)
        {
            enhanced_[r] = EnhancedChannel{};
        }
        float_depop_ = snapshot.float_depop;
        float_output_ = snapshot.float_output;
    }
    voice_mark_ = static_cast<int>(voices);
    resource_mark_ = static_cast<int>(resources);
    order_ = snapshot.order;
    mixes_ = snapshot.mixes;
    auxes_ = snapshot.auxes;
    mixes_changed_ = snapshot.mixes_changed;
    sinks_ = snapshot.sinks;
    buffers_ = snapshot.buffers;
    depop_ = snapshot.depop;
    output_ = snapshot.output;
    mutes_at_once_ = true;
}

// RequestUpdateAudioRenderer: the renderer takes the client's parameters, and the client takes the voices' statuses.
// The renderer copied the statuses at the last command generation, before that frame's processing.
void Renderer::Update()
{
    for (std::size_t slot = 0; slot < voices_.size(); ++slot)
    {
        Voice& voice = voices_[slot];
        Voice::Server& server = voice.server;
        server.in_use = voice.in_use;
        if (!voice.in_use)
        {
            continue;
        }

        // A new voice starts again from nothing (VoiceInfo::Initialize).
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
        server.sort_order =
            static_cast<int32_t>(std::find(order_.begin(), order_.end(), static_cast<int>(slot)) - order_.begin());
        server.channel_count = voice.channel_count;
        server.pitch = voice.pitch;
        server.volume = voice.volume;
        server.muted = voice.muted;
        server.meter = voice.meter;
        server.quality = voice.quality;
        server.biquads = voice.biquads;
        server.mix = voice.mix;
        server.buffer_count = voice.buffer_count;

        // The buffers the client appended since the last update, if their ranges fit (VoiceInfo::UpdateWaveBuffer).
        for (std::size_t i = 0; i < voice.buffers.size(); ++i)
        {
            const WaveBufferInfo& info = voice.buffers[i].info;
            if (voice.buffers[i].sent || info.start < 0 || info.end < 0)
            {
                continue;
            }

            const uint64_t size = info.buffer.size();
            const uint64_t bytes = voice.format == SampleFormat::kPcmFloat ? 4 : 2;
            const bool fits = voice.format == SampleFormat::kAdpcm
                                  ? static_cast<uint64_t>(AdpcmBytes(info.start)) <= size &&
                                        static_cast<uint64_t>(AdpcmBytes(info.end)) <= size
                                  : static_cast<uint64_t>(info.start) * bytes <= size &&
                                        static_cast<uint64_t>(info.end) * bytes <= size;
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
            buffer.loop_start = info.start;
            buffer.loop_end = info.end;
            buffer.loop_count = info.loop ? -1 : 0;
            buffer.context = nullptr;
            if (info.context != nullptr)
            {
                server.contexts[i] = *info.context;
                buffer.context = &server.contexts[i];
            }
            server.sent[i] = false;
        }

        for (int c = 0; c < voice.channel_count; ++c)
        {
            ChannelResource& resource = Resource(voice, c);
            resource.mix = resource.client_mix;
        }

        // After a restore, the voice takes its mute at once. A muted voice's last samples then leave the depop nothing.
        if (mutes_at_once_)
        {
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

        // The statuses: none for a voice the renderer hasn't started yet, otherwise channel 0's statuses.
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

    mutes_at_once_ = false;

    // The effects' settings.
    for (Aux& aux : auxes_)
    {
        aux.server_enabled = aux.enabled;
        aux.server_inputs = aux.inputs;
        aux.server_outputs = aux.outputs;
        aux.server_ring = aux.ring;
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

    for (const Mix& mix : mixes_)
    {
        if (mix.in_use)
        {
            Depop(mix);
        }
    }

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
        if (parameter_.enhanced)
        {
            meter_peaks_[i] = PeakOf(&float_meter_sums_[i * count], count);
            continue;
        }

        int64_t peak = 0;
        for (std::size_t t = 0; t < count; ++t)
        {
            const int64_t x = meter_sums_[i * count + t];
            peak = std::max(peak, x < 0 ? -x : x);
        }
        meter_peaks_[i] = static_cast<int32_t>(std::min<int64_t>(peak, std::numeric_limits<int32_t>::max()));
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
                output_[at] = static_cast<int16_t>(std::clamp(RingSample(x), -32768, 32767));
            }
            continue;
        }

        const int32_t* samples = Buffer(final_mix.offset + sinks_[k]);
        for (int t = 0; t < parameter_.sample_count; ++t)
        {
            const int32_t x = samples[t];
            output_[static_cast<std::size_t>(t) * channels + k] = static_cast<int16_t>(std::clamp(x, -32768, 32767));
        }
    }
}

void Renderer::RenderVoice(Voice& voice)
{
    Voice::Server& server = voice.server;
    const int channels = server.channel_count;
    const auto state = [&](int c) -> VoiceState&
    {
        return Resource(voice, c).dsp;
    };
    const auto report = [&]()
    {
        for (int c = 0; c < channels; ++c)
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

    // A new voice starts from cleared states, at its volumes (VoiceInfo::UpdateForCommandGeneration, 0x6b480).
    if (server.is_new)
    {
        for (int c = 0; c < channels; ++c)
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
    // and plays if it has any; a voice that stops gives back every buffer and starts again from nothing. A voice that
    // was playing and stops or pauses has a frame of depop.
    bool render = false;
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
        render = std::any_of(state(0).valid.begin(), state(0).valid.end(), [](bool v) { return v; });
    }
    else
    {
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
        render = server.stopping;
    }

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
        return;
    }

    if (parameter_.enhanced)
    {
        RenderVoiceEnhanced(voice, destination);
        return;
    }

    const int count = parameter_.sample_count;
    for (int c = 0; c < channels; ++c)
    {
        VoiceState& s = state(c);
        ChannelResource& resource = Resource(voice, c);
        int32_t* work = Buffer(parameter_.mix_buffer_count + c);

        // The depop preparation: a voice that stops leaves its last samples to fade out.
        if (server.stopping)
        {
            if (destination != nullptr)
            {
                for (int b = 0; b < destination->buffer_count; ++b)
                {
                    int32_t& last = s.last_samples[static_cast<std::size_t>(b)];
                    if (last != 0)
                    {
                        int32_t& depop = depop_[static_cast<std::size_t>(destination->offset + b)];
                        depop = WrapAdd(depop, last);
                        last = 0;
                    }
                }
            }
            server.previous_volume = 0.0f;
            continue;
        }

        // The data source.
        DecodeFromWaveBuffersArgs args;
        args.format = server.format;
        args.out = work;
        args.state = &s;
        args.buffers = &server.buffers;
        args.channel = c;
        args.channel_count = channels;
        args.quality = server.quality;
        args.pitch = ToFixed(server.pitch, 15);
        args.source_rate = server.sample_rate;
        args.target_rate = parameter_.sample_rate;
        args.count = count;
        args.coefficients = &voice.coefficients;
        DecodeFromWaveBuffers(args);
        if (destination == nullptr)
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
        server.previous_volume = server.volume;
        server.previous_muted = server.muted;

        // The mix volumes, from the last frame's volumes: one command for a destination of more than 6 buffers, which
        // gives a silent buffer a last sample of 0, else one for each buffer that isn't silent.
        const bool grouped = destination->buffer_count > 6;
        for (int b = 0; b < destination->buffer_count; ++b)
        {
            const float before = resource.previous_mix[static_cast<std::size_t>(b)];
            const float now = resource.mix[static_cast<std::size_t>(b)];
            int32_t& last = s.last_samples[static_cast<std::size_t>(b)];
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

            last = ApplyMixRamp(Buffer(destination->offset + b), work, start, RampStep(start, end, count), count,
                                kGainBits);
            AddToMeter(server, b, work, start, end);
        }
        resource.previous_mix = resource.mix;
        server.biquads_were_enabled = {server.biquads[0].enabled, server.biquads[1].enabled};
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

        DecodeFromWaveBuffersArgs args;
        args.format = server.format;
        args.state = &s;
        args.buffers = &server.buffers;
        args.channel = c;
        args.channel_count = server.channel_count;
        args.quality = server.quality;
        args.pitch = ToFixed(server.pitch, 15);
        args.source_rate = server.sample_rate;
        args.target_rate = parameter_.sample_rate;
        args.count = parameter_.sample_count;
        args.coefficients = &voice.coefficients;
        if (parameter_.enhanced)
        {
            DecodeFromWaveBuffersEnhanced(args, {enhanced_[r].history, &decoded_, &input_}, nullptr);
        }
        else
        {
            DecodeFromWaveBuffers(args);
        }
        if (destination == nullptr)
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

        // In the console's mode, the voice takes its values for the next frame with each channel.
        if (!parameter_.enhanced)
        {
            server.previous_volume = server.volume;
            server.previous_muted = server.muted;
            server.biquads_were_enabled = {server.biquads[0].enabled, server.biquads[1].enabled};
        }
    }

    if (server.stopping)
    {
        server.previous_volume = 0.0f;
    }
    else if (destination != nullptr && parameter_.enhanced)
    {
        server.previous_volume = server.volume;
        server.previous_muted = server.muted;
        server.biquads_were_enabled = {server.biquads[0].enabled, server.biquads[1].enabled};
    }
}

// A metered voice's output to buffer `buffer` of the metered mix goes to its meter's channel too, at the same gains.
void Renderer::AddToMeter(const Voice::Server& server, int buffer, const int32_t* work, int32_t start, int32_t end)
{
    if (server.meter < 0 || server.meter >= meter_count_ || server.mix != meter_mix_ ||
        buffer >= static_cast<int>(meter_channels_.size()) || meter_channels_[static_cast<std::size_t>(buffer)] < 0)
    {
        return;
    }

    const int count = parameter_.sample_count;
    const std::size_t sum = static_cast<std::size_t>(server.meter) * static_cast<std::size_t>(meter_channel_count_) +
                            static_cast<std::size_t>(meter_channels_[static_cast<std::size_t>(buffer)]);
    ApplyMixRamp(&meter_sums_[sum * static_cast<std::size_t>(count)], work, start, RampStep(start, end, count), count,
                 kGainBits);
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
        DecodeFromWaveBuffersArgs args;
        args.format = server.format;
        args.state = &s;
        args.buffers = &server.buffers;
        args.channel = c;
        args.channel_count = server.channel_count;
        args.quality = server.quality;
        args.pitch = ToFixed(server.pitch, 15);
        args.source_rate = server.sample_rate;
        args.target_rate = parameter_.sample_rate;
        args.count = count;
        args.coefficients = &voice.coefficients;
        DecodeFromWaveBuffersEnhanced(args, {channel.history, &decoded_, &input_}, work);
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
        server.previous_volume = server.volume;
        server.previous_muted = server.muted;
        server.biquads_were_enabled = {server.biquads[0].enabled, server.biquads[1].enabled};
    }
}

// The enhanced mode's AddToMeter.
void Renderer::AddToMeterEnhanced(const Voice::Server& server, int buffer, const float* work, float start, float end)
{
    if (server.meter < 0 || server.meter >= meter_count_ || server.mix != meter_mix_ ||
        buffer >= static_cast<int>(meter_channels_.size()) || meter_channels_[static_cast<std::size_t>(buffer)] < 0)
    {
        return;
    }

    const int count = parameter_.sample_count;
    const std::size_t sum = static_cast<std::size_t>(server.meter) * static_cast<std::size_t>(meter_channel_count_) +
                            static_cast<std::size_t>(meter_channels_[static_cast<std::size_t>(buffer)]);
    MixRamp(&float_meter_sums_[sum * static_cast<std::size_t>(count)], work, start, end, count);
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

// A mix's effects in their order. An aux's command for each channel writes the input buffer into the send ring and
// reads the output buffer from the return ring, both from the renderer's offsets plus the channel's place in the frame.
// The last channel's command moves both offsets on by a frame of every channel (GenerateAuxCommand, 0x5f4a0, and
// WriteAuxBufferDsp and ReadAuxBufferDsp, 0x6d2b0 and 0x6d0f0). A disabled aux starts its rings again and passes its
// inputs through.
void Renderer::RenderEffects(const Mix& mix)
{
    const auto count = static_cast<uint32_t>(parameter_.sample_count);
    const bool enhanced = parameter_.enhanced;
    for (const int id : mix.effects)
    {
        Aux& aux = auxes_[static_cast<std::size_t>(id)];
        const std::size_t channels = aux.server_inputs.size();
        if (!aux.server_enabled)
        {
            aux.send_write = 0;
            aux.receive_read = 0;
            for (std::size_t k = 0; k < channels; ++k)
            {
                const int input = mix.offset + aux.server_inputs[k];
                const int output = mix.offset + aux.server_outputs[k];
                if (input != output && enhanced)
                {
                    std::copy(FloatBuffer(input), FloatBuffer(input) + count, FloatBuffer(output));
                }
                else if (input != output)
                {
                    std::copy(Buffer(input), Buffer(input) + count, Buffer(output));
                }
            }
            continue;
        }

        const uint32_t ring = aux.server_ring;
        for (std::size_t k = 0; k < channels && ring != 0; ++k)
        {
            const uint32_t offset = static_cast<uint32_t>(k) * count;
            const int input = mix.offset + aux.server_inputs[k];
            const int output = mix.offset + aux.server_outputs[k];
            if (aux.send_write + offset <= ring)
            {
                for (uint32_t t = 0; t < count; ++t)
                {
                    aux.send[(aux.send_write + offset + t) % ring] =
                        enhanced ? RingSample(FloatBuffer(input)[t]) : Buffer(input)[t];
                }
            }
            if (aux.receive_read + offset <= ring)
            {
                for (uint32_t t = 0; t < count; ++t)
                {
                    const int32_t x = aux.receive[(aux.receive_read + offset + t) % ring];
                    if (enhanced)
                    {
                        FloatBuffer(output)[t] = static_cast<float>(x);
                    }
                    else
                    {
                        Buffer(output)[t] = x;
                    }
                }
            }
        }
        if (ring != 0)
        {
            const auto frame = static_cast<uint32_t>(channels) * count;
            aux.send_write = (aux.send_write + frame) % ring;
            aux.receive_read = (aux.receive_read + frame) % ring;
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

} // namespace ntdlib::nx::audio
