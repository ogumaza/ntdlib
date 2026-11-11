// SPDX-License-Identifier: MIT

#include "ntdlib/nx/audio/renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

#include "ntdlib/nx/audio/dsp.h"

namespace ntdlib::nx::audio
{

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

    // The splitters and their destinations start free, in the order of their ids (SplitterInfoManager::Initialize,
    // 0x453c4).
    splitters_.resize(static_cast<std::size_t>(std::max(parameter.splitter_count, 0)));
    splitter_destinations_.resize(static_cast<std::size_t>(std::max(parameter.splitter_destination_count, 0)));
    free_splitters_.resize(splitters_.size());
    std::iota(free_splitters_.begin(), free_splitters_.end(), 0);
    free_splitter_destinations_.resize(splitter_destinations_.size());
    std::iota(free_splitter_destinations_.begin(), free_splitter_destinations_.end(), 0);
    if (parameter.enhanced)
    {
        enhanced_.resize(resources_.size());
        float_buffers_.resize(buffers_.size());
        float_depop_.resize(depop_.size());
        effect_samples_.resize(static_cast<std::size_t>(2 * kMaxEffectChannels) *
                               static_cast<std::size_t>(parameter.sample_count));
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
                               const AdpcmCoefficients* coefficients, const VoiceBehavior& behavior)
{
    const int voice = TakeVoiceSlot(sample_rate, channel_count, format, priority, coefficients, behavior);
    if (observer_ != nullptr)
    {
        observer_->AcquireVoiceSlot(sample_rate, channel_count, format, priority, coefficients, behavior, voice);
    }

    return voice;
}

int Renderer::TakeVoiceSlot(int32_t sample_rate, int channel_count, SampleFormat format, int32_t priority,
                            const AdpcmCoefficients* coefficients, const VoiceBehavior& behavior)
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
    voice.behavior = behavior;
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
    At(voice).splitter = kNoSplitter;
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

int Renderer::AcquireSplitter(int32_t sample_rate, int channel_count, int destination_count)
{
    // A splitter takes a destination for each destination and channel. The renderer has as many destinations as the
    // parameter gives, shared by every splitter.
    int splitter = kNoSplitter;
    const int64_t needed = int64_t{destination_count} * int64_t{channel_count};
    if (!free_splitters_.empty() && channel_count >= 1 && destination_count >= 0 &&
        needed <= static_cast<int64_t>(free_splitter_destinations_.size()))
    {
        splitter = free_splitters_.front();
        free_splitters_.erase(free_splitters_.begin());
        Splitter& taken = splitters_[static_cast<std::size_t>(splitter)];
        taken.in_use = true;
        taken.changed = true;
        taken.channel_count = channel_count;
        taken.sample_rate = sample_rate;
        taken.destinations.clear();
        for (int64_t k = 0; k < needed; ++k)
        {
            const int destination = free_splitter_destinations_.front();
            free_splitter_destinations_.erase(free_splitter_destinations_.begin());
            SplitterDestination& data = splitter_destinations_[static_cast<std::size_t>(destination)];
            data.mix = kNoMix;
            data.volumes = {};
            data.in_use = false;
            data.changed = true;
            taken.destinations.push_back(destination);
        }
    }
    if (observer_ != nullptr)
    {
        observer_->AcquireSplitter(sample_rate, channel_count, destination_count, splitter);
    }

    return splitter;
}

void Renderer::ReleaseSplitter(int splitter)
{
    if (splitter == kNoSplitter)
    {
        return;
    }

    // The destinations and the splitter go to the ends of the free lists.
    Splitter& released = splitters_.at(static_cast<std::size_t>(splitter));
    released.in_use = false;
    released.changed = true;
    for (const int destination : released.destinations)
    {
        SplitterDestination& data = splitter_destinations_[static_cast<std::size_t>(destination)];
        data.mix = kNoMix;
        data.volumes = {};
        data.in_use = false;
        data.changed = true;
        free_splitter_destinations_.push_back(destination);
    }
    released.destinations.clear();
    free_splitters_.push_back(splitter);
    if (observer_ != nullptr)
    {
        observer_->ReleaseSplitter(splitter);
    }
}

Renderer::SplitterDestination& Renderer::ClientDestination(int splitter, int destination, int channel)
{
    const Splitter& owner = splitters_.at(static_cast<std::size_t>(splitter));
    const int channels = std::max(owner.channel_count, 1);
    const int index = channels * destination + channel % channels;
    return splitter_destinations_[static_cast<std::size_t>(owner.destinations.at(static_cast<std::size_t>(index)))];
}

void Renderer::SetSplitterDestination(int splitter, int destination, int mix)
{
    Splitter& owner = splitters_.at(static_cast<std::size_t>(splitter));
    for (int c = 0; c < owner.channel_count; ++c)
    {
        SplitterDestination& data = ClientDestination(splitter, destination, c);
        data.mix = mix;
        data.in_use = mix != kNoMix;
        data.changed = true;
    }
    owner.changed = true;
    if (observer_ != nullptr)
    {
        observer_->SetSplitterDestination(splitter, destination, mix);
    }
}

void Renderer::SetSplitterMixVolume(int splitter, int destination, float volume, int channel, int buffer)
{
    SplitterDestination& data = ClientDestination(splitter, destination, channel);
    data.volumes.at(static_cast<std::size_t>(buffer)) = volume;
    data.changed = true;
    if (observer_ != nullptr)
    {
        observer_->SetSplitterMixVolume(splitter, destination, volume, channel, buffer);
    }
}

void Renderer::SetVoiceSplitter(int voice, int splitter)
{
    if (parameter_.enhanced)
    {
        throw std::logic_error("the enhanced mixing mode has no splitters");
    }

    At(voice).mix = kNoMix;
    At(voice).splitter = splitter;
    if (observer_ != nullptr)
    {
        observer_->SetVoiceSplitter(voice, splitter);
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

int Renderer::GetWaveBufferCount(int voice) const
{
    const int count = At(voice).buffer_count;
    if (observer_ != nullptr)
    {
        observer_->GetWaveBufferCount(voice, count);
    }

    return count;
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
    // At 32 kHz, the sink's upsampler gives 3 samples for each 2 of the final mix (Upsampler). The enhanced mode has
    // no upsampler.
    const bool upsamples = parameter_.sample_rate == 32000;
    if (upsamples && parameter_.enhanced)
    {
        throw std::logic_error("the enhanced mixing mode has no upsampler");
    }

    sinks_.assign(inputs.begin(), inputs.end());
    upsamplers_.assign(upsamples ? sinks_.size() : 0, Upsampler{});
    const auto samples = static_cast<std::size_t>(parameter_.sample_count) * (upsamples ? 3 : 2) / 2;
    output_.assign(samples * sinks_.size(), 0);
    if (parameter_.enhanced)
    {
        float_output_.assign(output_.size(), 0.0f);
    }
    if (observer_ != nullptr)
    {
        observer_->AddDeviceSink(inputs);
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
    snapshot.splitters = splitters_;
    snapshot.splitter_destinations = splitter_destinations_;
    snapshot.free_splitters = free_splitters_;
    snapshot.free_splitter_destinations = free_splitter_destinations_;
    snapshot.auxes = auxes_;
    snapshot.delays = delays_;
    snapshot.reverbs = reverbs_;
    snapshot.limiters = limiters_;
    snapshot.mixes_changed = mixes_changed_;
    snapshot.sinks = sinks_;
    snapshot.upsamplers = upsamplers_;
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
    splitters_ = snapshot.splitters;
    splitter_destinations_ = snapshot.splitter_destinations;
    free_splitters_ = snapshot.free_splitters;
    free_splitter_destinations_ = snapshot.free_splitter_destinations;
    auxes_ = snapshot.auxes;
    delays_ = snapshot.delays;
    reverbs_ = snapshot.reverbs;
    limiters_ = snapshot.limiters;
    mixes_changed_ = snapshot.mixes_changed;
    sinks_ = snapshot.sinks;
    upsamplers_ = snapshot.upsamplers;
    buffers_ = snapshot.buffers;
    depop_ = snapshot.depop;
    output_ = snapshot.output;
    mutes_at_once_ = true;
}

} // namespace ntdlib::nx::audio
