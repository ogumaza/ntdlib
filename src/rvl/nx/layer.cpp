// SPDX-License-Identifier: MIT

#include "ntdlib/rvl/nx/layer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ntdlib/rvl/ax/pb.h"

namespace ntdlib::rvl::nx
{

namespace
{

constexpr int32_t kSampleRate = 32000;

// The renderer's settings (0xf2cc48), and the buffers of each sub-mix and aux effect (the table at 0x4ca0020).
constexpr int kFinalMixBuffers = 6;
constexpr int kSubMixBuffers = 4;
constexpr std::array<int8_t, kSubMixBuffers> kAuxBuffers{0, 1, 2, 3};
constexpr std::array<int, kFinalMixBuffers> kSinkInputs{0, 1, 4, 5, 2, 3};

// The splitter's destinations: the final mix, then the sub-mixes of aux buses A and B.
constexpr int kDestinationCount = 3;

constexpr float kVolumeScale = 1.0f / 32768.0f;
constexpr float kRatioScale = 1.0f / 65536.0f;

uint32_t Address(uint16_t hi, uint16_t lo)
{
    return ax::Join(hi, lo);
}

void SetAddress(uint16_t& hi, uint16_t& lo, uint32_t address)
{
    hi = static_cast<uint16_t>(address >> 16);
    lo = static_cast<uint16_t>(address);
}

// The sample index plus 2 of a DSP-ADPCM address in nibbles, from a base in nibbles. Each 16 nibbles hold 14 samples.
uint64_t AdpcmIndex(uint64_t address, uint64_t base)
{
    const uint64_t offset = address - base;
    return (offset >> 4) * ax::kAdpcmSamplesPerFrame + (offset & 0xf);
}

// The pitch for the PB's ratio, held to 1/16 to 16 (0x103ef14 and 0x103e600).
float Pitch(const ax::Pb& pb)
{
    const float ratio = static_cast<float>(Address(pb.src.ratio_hi, pb.src.ratio_lo)) * kRatioScale;
    if (ratio > 16.0f)
    {
        return 16.0f;
    }

    return ratio >= 1.0f / 16.0f ? ratio : 1.0f / 16.0f;
}

// Biquad 1: the PB's biquad, or none (0x103ef4c and 0x103e620).
audio::BiquadFilterParameter Biquad(const ax::Pb& pb)
{
    audio::BiquadFilterParameter parameter;
    if (pb.biquad.on != 0)
    {
        parameter.enabled = true;
        parameter.b = {static_cast<int16_t>(pb.biquad.b0), static_cast<int16_t>(pb.biquad.b1),
                       static_cast<int16_t>(pb.biquad.b2)};
        parameter.a = {static_cast<int16_t>(pb.biquad.a1), static_cast<int16_t>(pb.biquad.a2)};
    }

    return parameter;
}

// Biquad 0: the low-pass filter's a0 and b0 in Q15 become b0 and a1 in Q14, truncated (0x103e660).
audio::BiquadFilterParameter LowPass(const ax::Pb& pb)
{
    audio::BiquadFilterParameter parameter;
    if (pb.lpf.on != 0)
    {
        parameter.enabled = true;
        parameter.b[0] = static_cast<int16_t>(static_cast<float>(pb.lpf.a0) * kVolumeScale * 16384.0f);
        parameter.a[0] = static_cast<int16_t>(static_cast<float>(pb.lpf.b0) * kVolumeScale * 16384.0f);
    }

    return parameter;
}

ax::SampleFormat FormatOf(const ax::Pb& pb)
{
    return static_cast<ax::SampleFormat>(pb.addr.format);
}

} // namespace

int32_t Layer::RendererPriority(uint32_t priority)
{
    const float value = static_cast<float>(priority) / -31.0f * 255.0f + 255.0f;
    if (value < 0.0f)
    {
        return 0;
    }

    return value > 255.0f ? 255 : static_cast<int32_t>(value);
}

audio::RendererParameter Layer::Parameter()
{
    audio::RendererParameter parameter;
    parameter.sample_rate = kSampleRate;
    parameter.sample_count = 160;
    parameter.mix_buffer_count = 14;
    parameter.sub_mix_count = 2;
    parameter.voice_count = kVoiceCount;
    parameter.splitter_count = 64;
    parameter.splitter_destination_count = 192;

    return parameter;
}

Layer::Layer(ax::System& ax, const ntdlib::dsp::WiiMemory& memory, audio::Renderer& renderer)
    : ax_(&ax), memory_(&memory), renderer_(&renderer)
{
    ax.UseLayer(*this, kVoiceCount);
    for (uint32_t i = 0; i < kVoiceCount; i++)
    {
        SetState(ax.VoiceAt(i), records_[i], State::kFree);
    }

    // The final mix, the two sub-mixes at full volume into it, buffer to buffer, and the device sink. Then an aux
    // effect on each sub-mix (0xf2cdb4 to 0xf2d0c0).
    if (!renderer.AcquireFinalMix(kFinalMixBuffers))
    {
        throw std::logic_error("the renderer has no final mix for the layer");
    }

    for (std::size_t bus = 0; bus < sub_mixes_.size(); bus++)
    {
        const int sub_mix = renderer.AcquireSubMix(kSampleRate, kSubMixBuffers);
        if (sub_mix == audio::Renderer::kNoMix)
        {
            throw std::logic_error("the renderer has no sub-mix for the layer");
        }

        renderer.SetSubMixDestination(sub_mix, audio::Renderer::kFinalMix);
        for (const int8_t buffer : kAuxBuffers)
        {
            renderer.SetSubMixMixVolume(sub_mix, 1.0f, buffer, buffer);
        }

        sub_mixes_[bus] = sub_mix;
    }

    renderer.AddDeviceSink(kSinkInputs);
    const uint32_t size = audio::Renderer::GetRequiredBufferSizeForAuxSendReturnBuffer(renderer.Parameter(),
                                                                                       kSubMixBuffers, kSubMixBuffers);
    for (std::size_t bus = 0; bus < auxes_.size(); bus++)
    {
        auxes_[bus] = renderer.AddAux(sub_mixes_[bus], size);
        renderer.SetAuxInputOutput(auxes_[bus], kAuxBuffers, kAuxBuffers);
        aux_enabled_[bus] = true;
    }
}

Layer::Record& Layer::RecordOf(const ax::Voice& voice)
{
    return records_.at(voice.index);
}

const Layer::Record& Layer::RecordOf(const ax::Voice& voice) const
{
    return records_.at(voice.index);
}

void Layer::SetState(ax::Voice& voice, Record& record, State state)
{
    record.state = state;
    voice.pb.state = static_cast<uint16_t>(static_cast<int16_t>(state));
}

void Layer::ClearFields(ax::Voice& voice, Record& record)
{
    const int renderer_voice = record.voice;
    const int splitter = record.splitter;
    const uint64_t stamp = record.stamp;
    record = Record{};
    record.voice = renderer_voice;
    record.splitter = splitter;
    record.stamp = stamp;
    voice.callback = nullptr;
    voice.sync = 0;
    voice.depop = false;
    voice.pb = ax::Pb{};
    voice.loop_end = 0;
}

void Layer::Release(ax::Voice& voice, Record& record)
{
    if (record.voice >= 0)
    {
        renderer_->ReleaseVoiceSlot(record.voice);
    }

    if (record.splitter != audio::Renderer::kNoSplitter)
    {
        renderer_->ReleaseSplitter(record.splitter);
    }

    record.voice = -1;
    record.splitter = audio::Renderer::kNoSplitter;
    SetState(voice, record, State::kFree);
}

ax::Voice* Layer::AcquireVoice(uint32_t priority, ax::Voice::Callback callback)
{
    // The first free record by index (0x1027d3c).
    std::size_t taken = records_.size();
    for (std::size_t i = 0; i < records_.size(); i++)
    {
        if (records_[i].state == State::kFree)
        {
            taken = i;
            break;
        }
    }

    // Otherwise the lowest priority below the one asked for, and among equals the oldest stamp (0x1027d6c). The
    // priorities compare signed, the stamps unsigned.
    if (taken == records_.size())
    {
        int32_t best_priority = static_cast<int32_t>(priority) - 1;
        uint64_t best_stamp = std::numeric_limits<uint64_t>::max();
        for (std::size_t i = 0; i < records_.size(); i++)
        {
            const auto record_priority = static_cast<int32_t>(records_[i].priority);
            if (record_priority < best_priority || (record_priority == best_priority && records_[i].stamp < best_stamp))
            {
                taken = i;
                best_priority = record_priority;
                best_stamp = records_[i].stamp;
            }
        }

        if (taken == records_.size())
        {
            return nullptr;
        }

        // The owner's callback, then the victim's release (0x1027e80 to 0x1027fc0). An acquired record has no
        // renderer voice yet. A record that plays or is paused stops first. The model leaves out the extra
        // RequestUpdateAudioRenderer that the game makes before the release: no frame renders between the two.
        ax::Voice& victim = ax_->VoiceAt(static_cast<uint32_t>(taken));
        Record& record = records_[taken];
        if (victim.callback)
        {
            const ax::Voice::Callback owner = victim.callback;
            owner(&victim);
        }

        if (record.state != State::kReleasing && record.state != State::kAcquired)
        {
            renderer_->SetVoicePlayState(record.voice, audio::PlayState::kStopped);
        }

        ClearFields(victim, record);
        Release(victim, record);
    }

    // The record's set-up (0x1027fc4): its state, callback, stamp and priority.
    ax::Voice& voice = ax_->VoiceAt(static_cast<uint32_t>(taken));
    Record& record = records_[taken];
    SetState(voice, record, State::kAcquired);
    voice.callback = std::move(callback);
    record.stamp = next_stamp_++;
    record.priority = priority;

    return &voice;
}

void Layer::FreeVoice(ax::Voice& voice)
{
    Record& record = RecordOf(voice);
    if (record.state == State::kFree || record.state == State::kReleasing)
    {
        throw std::logic_error("AXFreeVoice on a free voice of the layer");
    }

    if (record.state == State::kAcquired)
    {
        ClearFields(voice, record);
        SetState(voice, record, State::kFree);

        return;
    }

    renderer_->SetVoicePlayState(record.voice, audio::PlayState::kStopped);
    ClearFields(voice, record);
    SetState(voice, record, State::kReleasing);
}

void Layer::SetVoicePriority(ax::Voice& voice, uint32_t priority)
{
    Record& record = RecordOf(voice);
    record.priority = priority;
    if (record.state == State::kPlaying || record.state == State::kPaused)
    {
        renderer_->SetVoicePriority(record.voice, RendererPriority(priority));
    }
}

uint32_t Layer::VoicePriority(const ax::Voice& voice) const
{
    return RecordOf(voice).priority;
}

void Layer::SetVoiceState(ax::Voice& voice, ax::VoiceState state)
{
    Record& record = RecordOf(voice);
    if (record.state == State::kFree || record.state == State::kReleasing)
    {
        throw std::logic_error("AXSetVoiceState on a free voice of the layer");
    }

    if (state == ax::VoiceState::kStop)
    {
        if (record.state == State::kPlaying)
        {
            renderer_->SetVoicePlayState(record.voice, audio::PlayState::kPaused);
            SetState(voice, record, State::kPaused);
        }

        return;
    }

    if (record.state == State::kAcquired && !SetUp(voice, record))
    {
        return;
    }

    renderer_->SetVoicePlayState(record.voice, audio::PlayState::kStarted);
    SetState(voice, record, State::kPlaying);
}

bool Layer::SetUp(ax::Voice& voice, Record& record)
{
    const ax::Pb& pb = voice.pb;
    if (pb.type == static_cast<uint16_t>(ax::VoiceType::kStream))
    {
        throw std::logic_error("the layer's model plays no stream voices");
    }

    // The option that skips the pitch and the sample-rate conversion (src_select 2) isn't modelled.
    if (pb.src_select == static_cast<uint16_t>(ax::SrcMode::kNone))
    {
        throw std::logic_error("the layer's model has no voices without sample-rate conversion");
    }

    const ax::SampleFormat format = FormatOf(pb);
    audio::SampleFormat renderer_format = audio::SampleFormat::kInvalid;
    const audio::AdpcmCoefficients* coefficients = nullptr;
    switch (format)
    {
    case ax::SampleFormat::kAdpcm:
        renderer_format = audio::SampleFormat::kAdpcm;
        for (std::size_t i = 0; i < record.coefficients.size(); i++)
        {
            record.coefficients[i] = static_cast<int16_t>(pb.adpcm.a[i]);
        }
        coefficients = &record.coefficients;
        break;
    case ax::SampleFormat::kPcm16:
        renderer_format = audio::SampleFormat::kPcmInt16;
        break;
    default:
        throw std::logic_error("the layer's model plays only DSP-ADPCM and PCM16");
    }

    const int renderer_voice =
        renderer_->AcquireVoiceSlot(kSampleRate, 1, renderer_format, RendererPriority(record.priority), coefficients,
                                    audio::VoiceBehavior{.reset_played_at_loop = true});
    if (renderer_voice < 0)
    {
        return false;
    }

    const int splitter = renderer_->AcquireSplitter(kSampleRate, 1, kDestinationCount);
    if (splitter == audio::Renderer::kNoSplitter)
    {
        renderer_->ReleaseVoiceSlot(renderer_voice);

        return false;
    }

    record.voice = renderer_voice;
    record.splitter = splitter;
    renderer_->SetSplitterDestination(splitter, 0, audio::Renderer::kFinalMix);
    renderer_->SetSplitterDestination(splitter, 1, sub_mixes_[0]);
    renderer_->SetSplitterDestination(splitter, 2, sub_mixes_[1]);
    renderer_->SetVoiceSplitter(renderer_voice, splitter);
    renderer_->SetVoiceSrcQuality(renderer_voice, pb.src_select == static_cast<uint16_t>(ax::SrcMode::kFourTap)
                                                      ? audio::SrcQuality::kHigh
                                                      : audio::SrcQuality::kMedium);
    renderer_->SetVoicePitch(renderer_voice, Pitch(pb));
    renderer_->SetVoiceBiquadFilterParameter(renderer_voice, 1, Biquad(pb));
    renderer_->SetVoiceVolume(renderer_voice, static_cast<float>(pb.ve.current_volume) * kVolumeScale);
    SetMix(voice, record, ~uint32_t{0});

    // The base and the first and last samples from it (0x103f080). The base is 64 bytes aligned. ADPCM's addresses
    // count nibbles, PCM16's samples.
    const uint64_t current = Address(pb.addr.current_address_hi, pb.addr.current_address_lo);
    const uint64_t end = Address(pb.addr.end_address_hi, pb.addr.end_address_lo);
    if (format == ax::SampleFormat::kAdpcm)
    {
        record.base = (current >> 1) & ~uint64_t{63};
        record.start = AdpcmIndex(current, record.base << 1) - 2;
        record.end = AdpcmIndex(end, record.base << 1) - 2;
    }
    else
    {
        record.base = (current << 1) & ~uint64_t{63};
        record.start = current - (record.base >> 1);
        record.end = end - (record.base >> 1);
    }

    record.slot = 0;
    AppendWaveBuffer(voice, record, 0);

    return true;
}

void Layer::AppendWaveBuffer(const ax::Voice& voice, Record& record, std::size_t slot)
{
    // The wave buffer's end and loop, as samples from the base. Each end is one past the last sample. The size runs to
    // the byte after the one that holds the last sample.
    const ax::Pb& pb = voice.pb;
    const ax::SampleFormat format = FormatOf(pb);
    const uint64_t loop = Address(pb.addr.loop_address_hi, pb.addr.loop_address_lo);
    const uint64_t end = Address(pb.addr.end_address_hi, pb.addr.end_address_lo);
    const uint64_t loop_end = voice.loop_end;
    uint64_t end_index = 0;
    uint64_t loop_start_index = 0;
    uint64_t loop_end_index = 0;
    uint64_t size = 0;
    if (format == ax::SampleFormat::kAdpcm)
    {
        const uint64_t base = record.base << 1;
        end_index = AdpcmIndex(end, base) - 1;
        loop_end_index = AdpcmIndex(loop_end, base) - 1;
        loop_start_index = AdpcmIndex(loop, base) - 2;
        const uint64_t last = base + 2 + end_index / ax::kAdpcmSamplesPerFrame * ax::kAdpcmNibblesPerFrame +
                              end_index % ax::kAdpcmSamplesPerFrame;
        size = (last >> 1) - record.base + 1;
    }
    else
    {
        const uint64_t base = record.base >> 1;
        loop_start_index = loop - base;
        end_index = end - base + 1;
        loop_end_index = loop_end - base + 1;
        size = ((record.base + 2 * end_index) & ~uint64_t{1}) - record.base + 1;
    }

    // A one-shot voice that has started gets no more buffers (0x103e9a8).
    const bool loops = pb.addr.loop_flag != 0;
    if (renderer_->GetVoicePlayState(record.voice) == audio::PlayState::kStarted && !loops)
    {
        return;
    }

    audio::WaveBufferInfo& buffer = record.buffers[slot];
    buffer = audio::WaveBufferInfo{};
    buffer.buffer = Samples(static_cast<uint32_t>(record.base), static_cast<std::size_t>(size), format);
    buffer.start = static_cast<int32_t>(record.start);
    buffer.end = static_cast<int32_t>(end_index);
    buffer.loop = loops;
    buffer.end_of_stream = true;
    buffer.tag = &buffer;
    if (loops)
    {
        if (format == ax::SampleFormat::kAdpcm)
        {
            record.loop_context = {pb.adpcm_loop.loop_pred_scale, static_cast<int16_t>(pb.adpcm_loop.loop_yn1),
                                   static_cast<int16_t>(pb.adpcm_loop.loop_yn2)};
            buffer.context = &record.loop_context;
        }

        buffer.loop_fields = true;
        buffer.loop_start = static_cast<int32_t>(loop_start_index);
        buffer.loop_end = static_cast<int32_t>(loop_end_index);
        buffer.loop_count = -1;
    }

    renderer_->AppendWaveBuffer(record.voice, buffer);
}

void Layer::SetMix(const ax::Voice& voice, const Record& record, uint32_t mixer)
{
    // Each bit sets one volume of a destination. The game's code gives the R bits of aux buses A and B the L volumes.
    struct Send
    {
        uint32_t bit;
        uint16_t ax::PbMix::* volume;
        int destination;
        int buffer;
    };

    static constexpr std::array<Send, 12> kSends{{
        {ax::mixer::kL, &ax::PbMix::l, 0, 0},
        {ax::mixer::kR, &ax::PbMix::r, 0, 1},
        {ax::mixer::kS, &ax::PbMix::s, 0, 2},
        {ax::mixer::kAuxCL, &ax::PbMix::aux_c_l, 0, 3},
        {ax::mixer::kAuxAL, &ax::PbMix::aux_a_l, 1, 0},
        {ax::mixer::kAuxAR, &ax::PbMix::aux_a_l, 1, 1},
        {ax::mixer::kAuxAS, &ax::PbMix::aux_a_s, 1, 2},
        {ax::mixer::kAuxCR, &ax::PbMix::aux_c_r, 1, 3},
        {ax::mixer::kAuxBL, &ax::PbMix::aux_b_l, 2, 0},
        {ax::mixer::kAuxBR, &ax::PbMix::aux_b_l, 2, 1},
        {ax::mixer::kAuxBS, &ax::PbMix::aux_b_s, 2, 2},
        {ax::mixer::kAuxCS, &ax::PbMix::aux_c_s, 2, 3},
    }};

    for (const Send& send : kSends)
    {
        if ((mixer & send.bit) != 0)
        {
            renderer_->SetSplitterMixVolume(record.splitter, send.destination,
                                            static_cast<float>(voice.pb.mix.*send.volume) * kVolumeScale, 0,
                                            send.buffer);
        }
    }
}

void Layer::ServiceWaveBuffers(ax::Voice& voice, Record& record)
{
    // A released buffer turns to the other slot. The current address goes back into the PB: the loop address once no
    // buffer is left.
    if (renderer_->GetReleasedWaveBuffer(record.voice) != nullptr)
    {
        record.slot ^= 1;
    }

    const int count = renderer_->GetWaveBufferCount(record.voice);
    const uint64_t played = renderer_->GetVoicePlayedSampleCount(record.voice);
    const audio::WaveBufferInfo& buffer = record.buffers[record.slot];
    ax::PbAddr& address = voice.pb.addr;
    uint32_t current = 0;
    if (count == 0)
    {
        current = Address(address.loop_address_hi, address.loop_address_lo);
    }
    else if (FormatOf(voice.pb) == ax::SampleFormat::kPcm16)
    {
        current = static_cast<uint32_t>((record.base >> 1) + static_cast<uint64_t>(buffer.start) + played);
    }
    else
    {
        const uint64_t sample = static_cast<uint64_t>(buffer.start) + played;
        current =
            static_cast<uint32_t>(2 * record.base + sample / ax::kAdpcmSamplesPerFrame * ax::kAdpcmNibblesPerFrame +
                                  sample % ax::kAdpcmSamplesPerFrame + 2);
    }

    SetAddress(address.current_address_hi, address.current_address_lo, current);
    const bool wants = buffer.loop ? count == 1 && buffer.loop_count != -1 : count == 1;
    if (wants)
    {
        AppendWaveBuffer(voice, record, record.slot ^ 1u);
    }
}

void Layer::ServiceAux(int bus)
{
    // The sub-mix reaches the final mix at the bus's return volume, up to 1.
    const auto index = static_cast<std::size_t>(bus);
    const float volume = std::min(static_cast<float>(ax_->AuxReturnVolume(bus)) * kVolumeScale, 1.0f);
    for (const int8_t buffer : kAuxBuffers)
    {
        renderer_->SetSubMixMixVolume(sub_mixes_[index], volume, buffer, buffer);
    }

    if (ax_->HasAuxCallback(bus))
    {
        throw std::logic_error("the layer's model has no callbacks on AX's aux buffers");
    }

    // A full frame goes through the callback. A bus without one, or a part of a frame, returns what it read.
    const bool callback = static_cast<bool>(aux_callbacks_[index]);
    std::array<int32_t, kAuxSamples> samples{};
    for (;;)
    {
        const int32_t count = renderer_->ReadAuxSendBuffer(auxes_[index], samples);
        if (count <= 0)
        {
            break;
        }

        if (callback && static_cast<std::size_t>(count) == kAuxSamples)
        {
            aux_callbacks_[index](samples);
        }
        renderer_->WriteAuxReturnBuffer(auxes_[index],
                                        std::span<const int32_t>(samples).first(static_cast<std::size_t>(count)));
    }

    if (aux_enabled_[index] != callback)
    {
        renderer_->SetAuxEnabled(auxes_[index], callback);
        aux_enabled_[index] = callback;
    }
}

void Layer::SetAuxCallback(int bus, AuxCallback callback)
{
    aux_callbacks_.at(static_cast<std::size_t>(bus)) = std::move(callback);
}

void Layer::Convert(ax::Voice& voice, const Record& record)
{
    // The parts of the PB that NW4R SND changed since the last frame (0x103e590 to 0x103e6ec). The envelope's ramp and
    // the mix's ramps are left out.
    const ax::Pb& pb = voice.pb;
    const uint32_t sync = voice.sync;
    if ((sync & ax::sync::kVe) != 0)
    {
        renderer_->SetVoiceVolume(record.voice, static_cast<float>(pb.ve.current_volume) * kVolumeScale);
    }

    if ((sync & ax::sync::kMix) != 0)
    {
        SetMix(voice, record, pb.MixerCtrl());
    }

    if ((sync & ax::sync::kSrcRatio) != 0)
    {
        renderer_->SetVoicePitch(record.voice, Pitch(pb));
    }

    if ((sync & (ax::sync::kBiquad | ax::sync::kBiquadCoefs)) != 0)
    {
        renderer_->SetVoiceBiquadFilterParameter(record.voice, 1, Biquad(pb));
    }

    if ((sync & (ax::sync::kLpf | ax::sync::kLpfCoefs)) != 0)
    {
        renderer_->SetVoiceBiquadFilterParameter(record.voice, 0, LowPass(pb));
    }

    voice.sync = 0;
}

void Layer::RenderFrame()
{
    // The final mix's volume follows AX's master volume. NW4R SND keeps it at 0x8000, the final mix's 1.0.
    if (ax_->MasterVolume() != ax::kMaxVolume)
    {
        throw std::logic_error("the layer's model keeps the master volume at 0x8000");
    }

    for (uint32_t i = 0; i < kVoiceCount; i++)
    {
        if (records_[i].state == State::kPlaying)
        {
            ServiceWaveBuffers(ax_->VoiceAt(i), records_[i]);
        }
    }

    ServiceAux(0);
    ServiceAux(1);
    for (uint32_t i = 0; i < kVoiceCount; i++)
    {
        if (records_[i].state == State::kPlaying || records_[i].state == State::kPaused)
        {
            Convert(ax_->VoiceAt(i), records_[i]);
        }
    }

    renderer_->RenderFrame();
    for (uint32_t i = 0; i < kVoiceCount; i++)
    {
        if (records_[i].state == State::kReleasing)
        {
            Release(ax_->VoiceAt(i), records_[i]);
        }
    }
}

std::span<const uint8_t> Layer::Samples(uint32_t address, std::size_t size, ax::SampleFormat format)
{
    const uint8_t* bytes = memory_->At(address, size);
    if (bytes == nullptr)
    {
        throw std::logic_error("a wave of the layer is outside the model's memory");
    }

    const auto key = std::make_pair(address, size);
    const auto found = copy_index_.find(key);
    if (found != copy_index_.end() && std::memcmp(found->second->original.data(), bytes, size) == 0)
    {
        return found->second->samples;
    }

    WaveCopy& copy = copies_.emplace_back();
    copy.original.assign(bytes, bytes + size);
    copy.samples = copy.original;
    if (format == ax::SampleFormat::kPcm16)
    {
        for (std::size_t i = 0; i + 1 < size; i += 2)
        {
            std::swap(copy.samples[i], copy.samples[i + 1]);
        }
    }

    copy_index_[key] = &copy;

    return copy.samples;
}

Layer::Snapshot Layer::Save() const
{
    return Snapshot{this, records_, next_stamp_, aux_enabled_};
}

void Layer::Restore(const Snapshot& snapshot)
{
    if (snapshot.layer != this)
    {
        throw std::invalid_argument("a snapshot of another layer");
    }

    records_ = snapshot.records;
    next_stamp_ = snapshot.next_stamp;
    aux_enabled_ = snapshot.aux_enabled;
}

} // namespace ntdlib::rvl::nx
