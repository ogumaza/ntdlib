// SPDX-License-Identifier: MIT

#include "ntdlib/cafe/snd/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>

#include "xsflib/rip/length.h"

namespace ntdlib::cafe::snd
{
namespace
{

// How the engine reads the archive's files: the Wii U's flavour, by the players' rules. Those stop at the first
// problem. The readers take either byte order, by each file's byte order mark.
const sar::Options kSarOptions{sar::Flavor::kCafe, {}, false, sar::Rules::kStopAtFirstProblem};

// A sequence player's tracks, each with a meter (Engine::MeterTracks).
constexpr int kTrackMeters = 16;

} // namespace

Engine::Engine(ax::System& system, ax::Memory& memory, const sar::SoundArchive& archive, EngineOptions options)
    : system_(&system),
      memory_(&memory),
      archive_(&archive),
      options_(options),
      ax_voices_(system, memory),
      renderer_(ax_voices_, options_.hardware, options.voice_count),
      voices_(options.voice_count),
      channels_(options.voice_count),
      files_(archive, kSarOptions, [this](ntdlib::nwsnd::LoadedWaveArchive& loaded) { MapWaveArchive(loaded); }),
      effects_(system, options_.effects, options_.hardware.output_mode)
{
    global_vars_.fill(-1);

    // HwCallbackProc counts the frame, updates the AX voices, and wakes the sound thread. RunFrame stands in for the
    // sound thread.
    system_->RegisterFrameCallback([this] { ax_voices_.UpdateAllVoices(options_.hardware.output_mode); });
}

Engine::~Engine()
{
    system_->RegisterFrameCallback({});
}

void Engine::MapWaveArchive(ntdlib::nwsnd::LoadedWaveArchive& loaded)
{
    loaded.address = next_address_;
    memory_->Map(loaded.address, loaded.file);
    next_address_ += (static_cast<uint32_t>(loaded.file.size()) + 0xFF) & ~0xFFu;
}

Channel* Engine::NoteOn(const std::vector<uint32_t>& banks, int slot, const NoteOnInfo& info)
{
    const std::optional<Files::Note> note = files_.FindNote(banks, slot, info.prg_no, info.key, info.velocity);
    if (!note)
    {
        return nullptr;
    }

    const sar::VelocityRegion* region = note->region;
    Channel* channel = channels_.AllocChannel(*this, std::min(note->wave.channel_count, 2), info.priority,
                                              info.owner_player, info.owner_track);
    if (!channel)
    {
        return nullptr;
    }

    channel->key = static_cast<uint8_t>(info.key);
    channel->original_key = region->original_key;
    channel->velocity_volume = SequencePolicy::VelocityVolume(info.velocity);
    channel->init_volume = static_cast<float>(region->volume) * (1.0f / 127.0f);
    channel->tune = region->pitch;
    channel->envelope.SetAttack(region->adshr.attack);
    channel->envelope.SetHold(region->adshr.hold);
    channel->envelope.SetDecay(region->adshr.decay);
    channel->envelope.SetSustain(region->adshr.sustain);
    channel->envelope.SetRelease(region->adshr.release);
    channel->init_pan = static_cast<float>(info.init_pan + static_cast<int>(region->pan) - 0x40) / 63.0f;
    channel->key_group = region->key_group;
    channel->ignore_note_off = region->ignore_note_off;
    channel->interpolation_type = region->interpolation;
    channel->Start(*this, note->wave, info.length);

    return channel;
}

void Engine::ChannelEvent(int player, int track, Channel& channel, Channel::CallbackStatus)
{
    players_[static_cast<std::size_t>(player)]->ChannelEvent(*this, track, channel);
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
    const int id = static_cast<int>(players_.size());
    players_.push_back(std::make_unique<SequenceSoundPlayer>(id, info, sequence.data));
    pending_.push_back({id, 1});

    return players_.back().get();
}

void Engine::FrameProcess()
{
    voices_.UpdateAllVoiceStatus(*this);

    // The sound thread's commands. They start the sounds that are due.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (it->frames > 0)
        {
            --it->frames;
            ++it;
            continue;
        }

        players_[static_cast<std::size_t>(it->player)]->Start();
        it = pending_.erase(it);
    }

    // The players' frames (OnUpdateFrameSoundThread), the channels and the voices.
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        player->Update(*this);
    }
    channels_.UpdateAllChannel(*this);
    voices_.UpdateAllVoices(*this);
    ax_voices_.UpdatePriorityList();

    // HardwareManager::Update (0x0229EC08) has no fades to move here. The random numbers step once a frame.
    random_.Next();
}

void Engine::RunFrame()
{
    // The host's mutes and meters reach AX's voices first. AX renders the frame with them.
    SetHostMixes();
    system_->RenderFrame();
    FrameProcess();

    if (effects_.Any())
    {
        ringing_frames_ = xsflib::rip::Audible(system_->Output()) ? kRingingFrames : std::max(ringing_frames_ - 1, 0);
    }
}

bool Engine::IsBusy() const
{
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        if (!player->IsFinished())
        {
            return true;
        }
    }

    return channels_.ActiveCount() > 0;
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
    system_->SetMeters(player >= 0 ? kTrackMeters : 0);
}

std::array<int32_t, 2> Engine::TrackPeaks(int track) const
{
    if (metered_player_ < 0 || track < 0 || track >= kTrackMeters)
    {
        return {0, 0};
    }

    return system_->MeterPeaks(track);
}

void Engine::SetHostMixes()
{
    // The AX voices of each channel, by their indices: the mute of the channel's track, and its meter if the metered
    // player plays it. A channel's MultiVoice plays each of its wave's channels on an AX voice.
    std::array<bool, ax::kVoiceCount> muted{};
    std::array<int, ax::kVoiceCount> meters{};
    meters.fill(-1);
    for (const auto place : channels_.InUse())
    {
        const Channel& channel = channels_.At(place);
        if (channel.voice == ntdlib::nwsnd::kNoVoice)
        {
            continue;
        }

        const bool mute = IsTrackMuted(channel.source_player, channel.source_track);
        const bool metered = metered_player_ >= 0 && channel.source_player == metered_player_ &&
                             channel.source_track >= 0 && channel.source_track < kTrackMeters;
        for (const MultiVoice::Voice& voice : voices_.At(channel.voice).Voices())
        {
            if (voice.renderer >= 0)
            {
                muted[static_cast<std::size_t>(voice.renderer)] = mute;
                meters[static_cast<std::size_t>(voice.renderer)] = metered ? channel.source_track : -1;
            }
        }
    }

    // Every AX voice, those of no channel included.
    const std::array<ax::Voice, ax::kVoiceCount>& all = system_->Voices();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        system_->SetVoiceMuted(all[i], muted[i]);
        system_->SetVoiceMeter(all[i], meters[i]);
    }
}

Engine::Snapshot Engine::Save() const
{
    Snapshot snapshot{this, ax_voices_.Save(), renderer_.Save(), voices_,        channels_, random_, global_vars_,
                      {},   pending_,          effects_.Save(),  ringing_frames_};
    for (const std::unique_ptr<SequenceSoundPlayer>& player : players_)
    {
        snapshot.players.push_back(*player);
    }

    return snapshot;
}

void Engine::Restore(const Snapshot& snapshot)
{
    if (snapshot.owner != this)
    {
        throw std::invalid_argument("a snapshot of another engine");
    }
    if (snapshot.players.size() > players_.size())
    {
        throw std::invalid_argument("a snapshot with players that the engine doesn't have");
    }

    // The players that started after the snapshot go. The others keep their places. The starts that wait reach the
    // players in their places.
    players_.erase(players_.begin() + static_cast<std::ptrdiff_t>(snapshot.players.size()), players_.end());
    for (std::size_t i = 0; i < players_.size(); ++i)
    {
        *players_[i] = snapshot.players[i];
    }
    pending_ = snapshot.pending;

    // The voices at each level, the channels and the rest go back into their places.
    ax_voices_.Restore(snapshot.ax_voices);
    renderer_.Restore(snapshot.buffers);
    voices_ = snapshot.voices;
    channels_ = snapshot.channels;
    random_ = snapshot.random;
    global_vars_ = snapshot.global_vars;
    effects_.Restore(snapshot.effects);
    ringing_frames_ = snapshot.ringing_frames;

    // The copy of the MultiVoices can put them at other addresses. Each AX voice's dispose callback then takes its
    // voice at the new address.
    for (int place = 0; place < voices_.AddedCount(); ++place)
    {
        for (AxRenderer::Voice& voice : voices_.At(place).Voices())
        {
            renderer_.Rebind(voice);
        }
    }
}

ArchiveModel::ArchiveModel(const sar::SoundArchive& archive, EngineOptions options)
{
    system.Init(ax::System::Library::kSndCore2, options.renderer_48k);
    system.SetDeviceUpsampleStage(ax::System::Device::kTv, options.upsample_after_final_mix);
    system.SetDeviceUpsampleStage(ax::System::Device::kDrc, options.upsample_after_final_mix);
    engine = std::make_unique<Engine>(system, memory, archive, options);
}

ArchiveModel::Snapshot ArchiveModel::Save() const
{
    return {system.Save(), engine->Save()};
}

void ArchiveModel::Restore(const Snapshot& snapshot)
{
    if (snapshot.system.owner != &system)
    {
        throw std::invalid_argument("a snapshot of another model's AX");
    }

    // The engine checks its part before it changes anything.
    engine->Restore(snapshot.engine);
    system.Restore(snapshot.system);
}

} // namespace ntdlib::cafe::snd
