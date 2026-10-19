// SPDX-License-Identifier: MIT

#include "ntdlib/nx/atk/engine.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ntdlib::nx::atk
{
namespace
{

// How the engine reads the archive's files: the Switch's flavour, with the rules that stop at the first problem.
const sar::Options kSarOptions{sar::Flavor::kNx, {}, false, sar::Rules::kStopAtFirstProblem};

// The renderer nn::atk opens (HardwareManager::Initialize, 0xc05340): 5 ms frames at 48 kHz, one sub-mix, 30 mix
// buffers (24 for the sub-mix and 6 for the final mix), and a voice for each of the sound system's voices.
audio::RendererParameter RendererParameterFor(const EngineOptions& options)
{
    audio::RendererParameter parameter;
    parameter.sample_rate = ArchiveModel::kSampleRate;
    parameter.sample_count = ArchiveModel::kFrameSamples;
    parameter.mix_buffer_count = 30;
    parameter.sub_mix_count = 1;
    parameter.voice_count = options.voice_count;

    return parameter;
}

} // namespace

Engine::Engine(audio::Renderer& renderer, const sar::SoundArchive& archive, const OutputReceiver& receiver,
               EngineOptions options)
    : renderer_(&renderer),
      archive_(&archive),
      receiver_(&receiver),
      options_(options),
      allocator_(renderer, options_.hardware, options.voice_count),
      voice_renderer_(allocator_, options_.hardware, options.voice_count),
      voices_(options.voice_count),
      channels_(options.voice_count),
      files_(archive, kSarOptions)
{
    global_vars_.fill(-1);
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    const std::optional<Files::Note> note = files_.FindNote(banks, slot, info.prg_no, info.key, info.velocity);
    if (!note)
    {
        return nullptr;
    }

    Channel* channel = channels_.AllocChannel(*this, std::min(note->wave.channel_count, 2), info.priority,
                                              info.owner_player, info.owner_track);
    if (channel == nullptr)
    {
        return nullptr;
    }

    ntdlib::nwsnd::SetUpNoteChannel<SequencePolicy>(*channel, *note->region, info);
    if (channel->voice != ntdlib::nwsnd::kNoVoice)
    {
        channel->Voice(*this).SetOutputReceiver(info.receiver);
    }
    channel->Start(*this, note->wave, info.length);

    return channel;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus)
{
    players_[player].ChannelEvent(*this, track, channel);
}

SequenceSoundPlayer* Engine::StartSequence(uint32_t index)
{
    if (index >= archive_->Sounds().size())
    {
        return nullptr;
    }

    const sar::SoundInfo& info = archive_->Sounds()[index];
    if (info.type != sar::SoundType::kSequence)
    {
        return nullptr;
    }

    const std::span<const uint8_t> file = archive_->FileData(info.file_id);
    if (file.empty())
    {
        return nullptr;
    }

    const sar::Sequence sequence = sar::Sequence::Parse(file, kSarOptions);

    return &players_.Add(1, info, sequence.data, receiver_);
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus(*this);

    // The sound thread's commands. They start the sounds that are due.
    players_.StartDue();

    // The players' frames, the channels and the voices, a renderer frame each.
    players_.ForEach([this](SequenceSoundPlayer& player) { player.Update(*this, 1); });
    channels_.UpdateAllChannel(*this);
    voices_.UpdateAllVoices(*this);

    // HardwareManager::Update (0xc05d90) has no fades to move here. The random numbers step once a frame.
    random_.Next();
}

void Engine::RunFrame()
{
    // SoundThread::Run: the voices' statuses, the sound frame, then the voices' parameters and states reach the
    // renderer. The host's mutes and meters reach it last. The renderer renders a frame with them.
    allocator_.UpdateAllVoiceStatus();
    FrameProcess();
    allocator_.UpdateAllVoice(options_.hardware.output_mode);
    UpdateEffects();
    MuteVoices();
    MeterVoices();
    renderer_->RenderFrame();
}

void Engine::AppendEffect(int bus, std::unique_ptr<EffectAux> effect)
{
    // Buffers for the effect's ring, with whole frames of the effect's channels. The effect takes the bus's first
    // channels.
    const uint32_t size = audio::Renderer::GetRequiredBufferSizeForAuxSendReturnBuffer(
        renderer_->Parameter(), EffectAux::kFrameCount, EffectAux::kChannelCount);
    AttachedEffect attached;
    attached.effect = std::move(effect);
    attached.aux = renderer_->AddAux(receiver_->mix, size);
    std::vector<int8_t> buffers;
    for (int channel = 0; channel < EffectAux::kChannelCount; ++channel)
    {
        buffers.push_back(static_cast<int8_t>((bus + 1) * receiver_->channel_count + channel));
    }
    renderer_->SetAuxInputOutput(attached.aux, buffers, buffers);
    renderer_->SetAuxEnabled(attached.aux, true);
    attached.work.resize(static_cast<std::size_t>(renderer_->GetAuxSampleCount(attached.aux)));
    effects_.at(static_cast<std::size_t>(bus) + 1).push_back(std::move(attached));
}

void Engine::UpdateEffects()
{
    // Only a receiver whose voices reach every bus updates its effects.
    if (!options_.hardware.all_buses)
    {
        return;
    }

    for (std::vector<AttachedEffect>& bus : effects_)
    {
        for (AttachedEffect& attached : bus)
        {
            const int32_t capacity = renderer_->GetAuxSampleCount(attached.aux);
            EffectAux::BufferInfo info;
            info.sample_count_per_frame = capacity / (EffectAux::kFrameCount * EffectAux::kChannelCount);
            info.sample_rate = renderer_->GetAuxSampleRate(attached.aux);
            info.frame_count = EffectAux::kFrameCount;
            info.channel_count = EffectAux::kChannelCount;
            info.output_mode = options_.hardware.output_mode;
            info.read_sample_count = renderer_->ReadAuxSendBuffer(attached.aux, attached.work);
            const auto samples =
                std::span<int32_t>(attached.work).first(static_cast<std::size_t>(info.read_sample_count));
            attached.effect->UpdateBuffer(samples, info);
            renderer_->WriteAuxReturnBuffer(attached.aux, samples);
        }
    }
}

bool Engine::IsBusy() const
{
    return players_.AnyPlaying() || channels_.ActiveCount() > 0;
}

void Engine::MuteTrack(int player, int track, bool mute)
{
    if (mute)
    {
        muted_tracks_.insert({player, track});
    }
    else
    {
        muted_tracks_.erase({player, track});
    }
}

bool Engine::IsTrackMuted(int player, int track) const
{
    return muted_tracks_.contains({player, track});
}

void Engine::MeterTracks(int player)
{
    metered_player_ = player;
    SetRendererMeters();
}

std::array<int32_t, 2> Engine::TrackPeaks(int track) const
{
    if (metered_player_ < 0 || track < 0 || track >= nwsnd::kTrackCount)
    {
        return {0, 0};
    }

    // A meter has no channels while every bus has an effect.
    const std::span<const int32_t> peaks = renderer_->MeterPeaks(track);

    return {!peaks.empty() ? peaks[0] : 0, peaks.size() > 1 ? peaks[1] : 0};
}

void Engine::SetRendererMeters()
{
    if (metered_player_ < 0)
    {
        renderer_->SetMeters(0, audio::Renderer::kNoMix, {});
        return;
    }

    // The front left and right of each bus without an effect go to a meter's left and right. On a bus with an effect,
    // the effect's output replaces what the voices send.
    const int channels = receiver_->channel_count;
    std::vector<int> map(static_cast<std::size_t>(receiver_->bus_count * channels), -1);
    for (int bus = 0; bus < receiver_->bus_count && bus < static_cast<int>(effects_.size()) && channels >= 2; ++bus)
    {
        if (effects_[static_cast<std::size_t>(bus)].empty())
        {
            map[static_cast<std::size_t>(bus * channels)] = 0;
            map[static_cast<std::size_t>(bus * channels + 1)] = 1;
        }
    }
    renderer_->SetMeters(nwsnd::kTrackCount, receiver_->mix, map);
}

void Engine::MuteVoices()
{
    // The low-level voices of the channels whose notes come from a muted track, by their places.
    std::vector<bool> muted(allocator_.All().size(), false);
    for (const auto place : channels_.InUse())
    {
        const Channel& channel = channels_.At(place);
        if (channel.voice == ntdlib::nwsnd::kNoVoice || !IsTrackMuted(channel.source_player, channel.source_track))
        {
            continue;
        }

        for (const MultiVoice::Voice& voice : voices_.At(channel.voice).Voices())
        {
            if (voice.renderer >= 0)
            {
                muted[static_cast<std::size_t>(voice.renderer)] = true;
            }
        }
    }

    // Every voice of the renderer that a low-level voice holds. A restore can bring back a mute that no longer holds.
    const std::span<const LowLevelVoice> all = allocator_.All();
    for (std::size_t place = 0; place < all.size(); ++place)
    {
        if (all[place].Slot() >= 0)
        {
            renderer_->SetVoiceMuted(all[place].Slot(), muted[place]);
        }
    }
}

void Engine::MeterVoices()
{
    // An effect can come in any frame.
    SetRendererMeters();

    // The low-level voices of the channels whose notes come from the metered player, by their places, with the tracks'
    // meters.
    std::vector<int> meters(allocator_.All().size(), -1);
    for (const auto place : channels_.InUse())
    {
        const Channel& channel = channels_.At(place);
        if (metered_player_ < 0 || channel.voice == ntdlib::nwsnd::kNoVoice ||
            channel.source_player != metered_player_ || channel.source_track < 0 ||
            channel.source_track >= nwsnd::kTrackCount)
        {
            continue;
        }

        for (const MultiVoice::Voice& voice : voices_.At(channel.voice).Voices())
        {
            if (voice.renderer >= 0)
            {
                meters[static_cast<std::size_t>(voice.renderer)] = channel.source_track;
            }
        }
    }

    // Every voice of the renderer that a low-level voice holds. A restore can bring back a meter that no longer holds.
    const std::span<const LowLevelVoice> all = allocator_.All();
    for (std::size_t place = 0; place < all.size(); ++place)
    {
        if (all[place].Slot() >= 0)
        {
            renderer_->SetVoiceMeter(all[place].Slot(), meters[place]);
        }
    }
}

Engine::Snapshot Engine::Save() const
{
    Snapshot snapshot{this,    allocator_.Save(), voice_renderer_.Save(), voices_, channels_,
                      random_, global_vars_,      players_.Save(),        {}};
    for (std::size_t bus = 0; bus < effects_.size(); ++bus)
    {
        for (const AttachedEffect& attached : effects_[bus])
        {
            snapshot.effects[bus].push_back(attached.effect->Clone());
        }
    }

    return snapshot;
}

void Engine::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this)
    {
        throw std::invalid_argument("a snapshot of another engine");
    }
    players_.CheckRestore(snapshot.players);
    for (std::size_t bus = 0; bus < effects_.size(); ++bus)
    {
        if (snapshot.effects[bus].size() != effects_[bus].size())
        {
            throw std::invalid_argument("a snapshot with effects that the engine doesn't have");
        }
    }

    // The players and the starts that wait.
    players_.Restore(snapshot.players);

    // The voices, the channels and the rest. The restore replaces each effect with a copy of the snapshot's effect. The
    // snapshot keeps its copy for the next restore.
    allocator_.Restore(snapshot.low_level_voices);
    voice_renderer_.Restore(snapshot.buffers);
    voices_ = snapshot.voices;
    channels_ = snapshot.channels;
    random_ = snapshot.random;
    global_vars_ = snapshot.global_vars;
    for (std::size_t bus = 0; bus < effects_.size(); ++bus)
    {
        for (std::size_t i = 0; i < effects_[bus].size(); ++i)
        {
            effects_[bus][i].effect = snapshot.effects[bus][i]->Clone();
        }
    }
}

ArchiveModel::ArchiveModel(const sar::SoundArchive& archive, EngineOptions options, audio::RendererObserver* observer)
    : renderer(RendererParameterFor(options))
{
    renderer.SetObserver(observer);

    // The final mix, and sub-mix 0. Each of sub-mix 0's buses reaches the final mix's channels at 1.0.
    constexpr int kChannels = 6;
    constexpr int kBuses = 4;
    renderer.AcquireFinalMix(kChannels);
    const int sub = renderer.AcquireSubMix(kSampleRate, kChannels * kBuses);
    renderer.SetSubMixDestination(sub, audio::Renderer::kFinalMix);
    for (int bus = 0; bus < kBuses; ++bus)
    {
        for (int channel = 0; channel < kChannels; ++channel)
        {
            renderer.SetSubMixMixVolume(sub, 1.0f, bus * kChannels + channel, channel);
        }
    }

    // The console's output in stereo: the final mix's front left and right. Nothing reaches its other channels while
    // nn::atk mixes for stereo.
    constexpr std::array<int, 2> kSinkInputs = {0, 1};
    renderer.AddDeviceSink(kSinkInputs);

    sub_mix.mix = sub;
    sub_mix.channel_count = kChannels;
    sub_mix.bus_count = kBuses;
    engine = std::make_unique<Engine>(renderer, archive, sub_mix, options);

    // The scene's reverbs, in place before any sound starts.
    for (std::size_t bus = 0; bus < options.aux_reverbs.size(); ++bus)
    {
        if (options.aux_reverbs[bus])
        {
            engine->AppendEffect(static_cast<int>(bus), std::make_unique<I3dl2AuxEffect>(*options.aux_reverbs[bus]));
        }
    }
}

ArchiveModel::Snapshot ArchiveModel::Save() const
{
    return {renderer.Save(), engine->Save()};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    // Every check comes before the first change: the renderer's owner here, then the engine's checks.
    if (snapshot.renderer.owner != &renderer)
    {
        throw std::invalid_argument("a snapshot of another renderer");
    }

    engine->Restore(snapshot.engine);
    renderer.Restore(snapshot.renderer);
}

} // namespace ntdlib::nx::atk
